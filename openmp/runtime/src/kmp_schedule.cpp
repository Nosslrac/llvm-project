#include "kmp_schedule.h"

#include "hwloc.h"
#include "kmp.h"
#include "kmp_debug.h"
#include <unordered_map>
#include "kmp_os.h"
#include "kmp_routine.h"

#include <immintrin.h>
#include <bitset>

namespace {

constexpr uint64_t ALL_PROCS = ~0ULL;

inline int bitCount(kmp_uint64 mask) {
#if __has_builtin(__builtin_popcountll)
  return __builtin_popcountll(mask);
#else
  return 0; // Impl needed
#endif
}

inline int bitScan(kmp_uint64 mask) {
#if __has_builtin(__builtin_ctzll)
  return __builtin_ctzll(mask);
#else
  return 0; // Impl needed
#endif
}

inline kmp_uint64 min(const kmp_uint64 a, const kmp_uint64 b) {
  if (a < b) {
    return a;
  }
  return b;
}

inline kmp_uint64 max(const kmp_uint64 a, const kmp_uint64 b) {
  if (a > b) {
    return a;
  }
  return b;
}

// Routine stuff
std::unordered_map<kmp_int64, Routine>
    routine_map; // Map containing all routines

kmp_real64 routine_timer;

// Routine stuff end

// Topology info
const NumaTopology numa_topology = Schedule::__kmp_read_topology();

void __kmp_alloc_task_deque(kmp_thread_data_t *thread_data, int32_t gtid) {

  __kmp_init_bootstrap_lock(&thread_data->td.td_deque_lock);
  KMP_DEBUG_ASSERT(thread_data->td.td_deque == NULL);

  // Initialize last stolen task field to "none"
  thread_data->td.td_deque_last_stolen = -1;

  KMP_DEBUG_ASSERT(TCR_4(thread_data->td.td_deque_ntasks) == 0);
  KMP_DEBUG_ASSERT(thread_data->td.td_deque_head == 0);
  KMP_DEBUG_ASSERT(thread_data->td.td_deque_tail == 0);

  KA_TRACE(2,
           ("%s:%d: __kmp_alloc_task_deque: Allocate task space for T#%d.\n ",
            __FILE_NAME__, __LINE__, gtid));
  // Allocate space for task deque, and zero the deque
  // Cannot use __kmp_thread_calloc() because threads not around for
  // kmp_reap_task_team( ).
  thread_data->td.td_deque = (kmp_taskdata_t **)__kmp_allocate(
      INITIAL_TASK_DEQUE_SIZE * sizeof(kmp_taskdata_t *));
  thread_data->td.td_deque_size = INITIAL_TASK_DEQUE_SIZE;
}

kmp_info_t *__kmp_select_thread(kmp_team *team, kmp_taskdata_t *taskdata,
                                kmp_uint32 nthreads) {
  KMP_DEBUG_ASSERT(taskdata);

  // Divide the threads equally accross NUMA nodes
  const auto numaSize =
      numa_topology.get_num_cores() / numa_topology.get_num_numa();
  const auto processor = taskdata->td_numa_place * numaSize;

  std::bitset<64> binaryRep(taskdata->td_affin_mask);
  KA_TRACE(3, ("%s:%d: __kmp_select_thread: Task#%p: "
               "Taskdata->td_affin_mask=%lu=0b%s scheduled on T#%d\n",
               __FILE_NAME__, __LINE__, taskdata, taskdata->td_affin_mask,
               binaryRep.to_string().c_str(), processor));
  KMP_DEBUG_ASSERT(processor < nthreads);
  return team->t.t_threads[static_cast<int>(processor)];
}

} // namespace

kmp_int32 Schedule::__kmp_get_victim(kmp_int32 tid, kmp_int32 prev_victim_tid) {
  const auto nNumaNodes = numa_topology.get_num_numa();
  const auto numaSize = numa_topology.get_num_cores() / nNumaNodes;

  if (tid / nNumaNodes == prev_victim_tid / nNumaNodes) {
    return prev_victim_tid;
  }
  return static_cast<kmp_int32>((tid / nNumaNodes) * numaSize);
}

kmp_thread_data_t *Schedule::__kmp_optimal_thread(kmp_info *master_thread,
                                                  kmp_task_team *task_team,
                                                  kmp_taskdata_t *taskdata) {

  KMP_DEBUG_ASSERT(taskdata);

  const auto nthreads = task_team->tt.tt_nproc;
  kmp_team *team = master_thread->th.th_team;
  kmp_info_t *base_numa = __kmp_select_thread(
      team, taskdata, nthreads); // Get primary thread from NUMA node

  const auto new_gtid = __kmp_gtid_from_thread(base_numa);
  const auto tid = __kmp_tid_from_gtid(new_gtid);
  kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[tid];

  if (UNLIKELY(thread_data->td.td_deque == NULL)) {
    __kmp_alloc_task_deque(thread_data, new_gtid);
  }

  KA_TRACE(
      3, ("%s:%d: __kmp_optimal_thread: Base NUMA thread based on affinity T#%d"
          "(tid=%d).\n ",
          __FILE_NAME__, __LINE__, new_gtid, tid));

  return thread_data;
}

//
void Schedule::__kmp_set_any_affinity(kmp_taskdata_t *taskdata) {
  KMP_DEBUG_ASSERT(taskdata);
  taskdata->td_affin_mask = ALL_PROCS;
  KA_TRACE(3, ("__kmp_set_any_affinity: Setting any affinity child task %p of "
               "parent %p\n",
               taskdata, taskdata->td_parent));
}

void Schedule::__kmp_set_task_affinity(kmp_info *thread,
                                       kmp_taskdata_t *taskdata,
                                       kmp_int64 routine_id, kmp_uint64 lb,
                                       kmp_uint64 ub, kmp_uint64 glob_ub) {
  KMP_DEBUG_ASSERT(taskdata);

  kmp_team_t *team = thread->th.th_team;
  auto nthreads = static_cast<uint32_t>(team->t.t_nproc);

  if (nthreads == 1) { // When single task is executed
    taskdata->td_numa_place = 0;
    taskdata->td_affin_mask = ALL_PROCS;
    return;
  }

  KA_TRACE(3, (" __kmp_set_task_affinity (enter): nproc:%d,"
               " lb:%lld, ub:%lld, glob_ub:%lld\n",
               thread->th.th_team_nproc, lb, ub, glob_ub));
  // TODO: Use moldability / config to select stealing and distribution
  /* if (has_config(routine_id)) {
    Do moldability
    config = Routine::get_config(routine_id, lb, ub, glob_ub);
    taskdata->td_numa_place = config.place;
    taskdata->td_affin_mask = config.steal;
    return;
  }*/

  // Fallback based on topology
  const auto nNumaNodes = numa_topology.get_num_numa();
  const auto numaNodeSize = numa_topology.get_num_cores() / nNumaNodes;

  const auto midRange = (lb + ub) / 2;
  const auto bucketSize = max(glob_ub / nNumaNodes, numaNodeSize);
  // Get numa node id, cannot be larger than the last one
  const auto numaId =
      static_cast<kmp_uint8>(min(midRange / bucketSize, numaNodeSize - 1));
  const auto discreteProc = numaId * numaNodeSize; // 0 | 8 | 16 | 24 | ...

  // Set the correct Numa node bits
  // taskdata->td_affin_mask = numa_topology.get_base_steal_bits() <<
  // discreteProc;
  taskdata->td_affin_mask = ALL_PROCS;
  taskdata->td_numa_place = numaId;

  KA_TRACE(3, ("%s:%d: __kmp_set_task_affinity: Nthreads=%d, Nnuma=%d, "
               "numaid=%d, Proc=%d, "
               "bucketSize=%lu "
               "MidIter#%d => Affin_mask=%lu.\n",
               __FILE_NAME__, __LINE__, nthreads, nNumaNodes, numaId,
               discreteProc, bucketSize, midRange, taskdata->td_affin_mask));

  KMP_DEBUG_ASSERT(numaId < nNumaNodes);
  KMP_DEBUG_ASSERT(discreteProc < nthreads);
}

void Schedule::__kmp_show_affinity(kmp_info *thread) {
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;
  for (int i = 0; i < nthreads; ++i) {
    kmp_info_t *thread = team->t.t_threads[i];
    char buf[KMP_AFFIN_MASK_PRINT_LEN];
    __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
                              thread->th.th_affin_mask);
    KA_TRACE(3,
             ("T#%d has affinity: %s\n", __kmp_gtid_from_thread(thread), buf));
  }
}

void Schedule::__kmp_set_per_thread_affinity(kmp_info *thread, int32_t gtid,
                                             int place) {
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;
  int tid = __kmp_tid_from_gtid(gtid);
  place = tid; // TODO: Use topology to map threads to specific cores
  if (thread->th.th_affin_mask == NULL) {
    KA_TRACE(5, ("Alloc: T#%d\n", gtid));
    KMP_CPU_ALLOC(thread->th.th_affin_mask);
  } else {
    KA_TRACE(5, ("Setting zero: T#%d\n", gtid));
    KMP_CPU_ZERO(thread->th.th_affin_mask);
  }
  KMP_CPU_SET(place, thread->th.th_affin_mask);
  thread->th.th_current_place = place;
  thread->th.th_new_place = place;
  thread->th.th_last_place = nthreads - 1;
  thread->th.force_affin = 1;
  __kmp_set_system_affinity(thread->th.th_affin_mask, TRUE);
  char buf[KMP_AFFIN_MASK_PRINT_LEN];
  __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
                            thread->th.th_affin_mask);
  KA_TRACE(5, ("Setting thread affinity: Tid=%d T#%d, Affinity: %s\n", tid,
               gtid, buf));
}

// Initialize global affinity object for the main thread and prepare for workers
void Schedule::__kmp_set_numa_affinity(kmp_affinity_t *global_affin,
                                       int32_t ncpus) {
  // To make bind_place do something
  global_affin->type = affinity_explicit;
  global_affin->num_masks = ncpus;

  KMP_CPU_ALLOC_ARRAY(global_affin->masks, ncpus);

  kmp_affin_mask_t *mask;
  for (int i = 0; i < ncpus; i++) {
    mask = KMP_CPU_INDEX(global_affin->masks, i);
    KMP_CPU_ZERO(mask);
  }

  KA_TRACE(5,
           ("Affinity:\n"
            "Proclist: %p\n"
            "Affinity type: %d\n"
            "Num masks: %d\n"
            "Num os id: %d\n"
            "Compact: %d\n"
            "",
            // "Affinity mask: %s\n",
            global_affin->proclist, global_affin->type, global_affin->num_masks,
            global_affin->num_os_id_masks, global_affin->compact));
}

void Schedule::__kmp_store_routine_stats(kmp_int64 routine_id,
                                         routine_stats stats) {

  kmp_real64 end_time;
  __kmp_read_system_time(&end_time);
  kmp_real64 tot_exec_time = end_time - routine_timer;
  stats.execution_time = tot_exec_time;

  // Verify that the routine exists in the map
  KMP_DEBUG_ASSERT(routine_map.find(routine_id) != routine_map.end())

  KA_TRACE(2, ("__kmp_store_routine_stats: New stat store for routine %p:"
               " tot_exec_time:%f, stall_ratio:%f\n",
               routine_id, stats.execution_time, stats.stall_ratio));

  // Store the execution stats
  routine_map.at(routine_id).storeExecution(stats);
}

routine_config Schedule::__kmp_select_config(kmp_info *thread,
                                             kmp_uint64 num_tasks) {
  routine_config ret_config;
  kmp_int64 routine_id = thread->th.routine_id;

  // Check if routine has executed before
  // If not, add new routine to map and return default config
  if (routine_map.find(routine_id) == routine_map.end()) {
    routine_map.emplace(routine_id, Routine(routine_id));
    ret_config = routine_map.at(routine_id).getDefaultConfig(thread, num_tasks);

  } else {
#ifdef MOLDABILITY
    ret_config = routine_map.at(routine_id).getNextConfig();
#else
    ret_config = routine_map.at(routine_id).getDefaultConfig(thread, num_tasks);
#endif
  }

  KA_TRACE(
      1,
      ("__kmp_select_config: routine %p was given new config={%d, %d, %d}.\n",
       routine_id, ret_config.num_threads, ret_config.num_tasks,
       static_cast<int>(ret_config.task_affinity)));

  return ret_config;
}

void Schedule::__kmp_start_routine_timer() {
  __kmp_read_system_time(&routine_timer);
}

kmp_real64 Schedule::__kmp_get_routine_timer() { return routine_timer; }

///////////////////////////////////////////////
///               Topology section          ///
///////////////////////////////////////////////

NumaTopology Schedule::__kmp_read_topology() {
  hwloc_topology_t topology = nullptr;
  // Load topology
  if (hwloc_topology_init(&topology) == -1) {
    KMP_FATAL(MsgExiting, "Hardware topology not read");
    return NumaTopology(0, 0, 0, 0);
  }
  if (hwloc_topology_load(topology) == -1) {
    KMP_FATAL(MsgExiting, "Hardware topology not read");
    return NumaTopology(0, 0, 0, 0);
  }

  // Get relevant intro
  const auto nNumaNodes =
      hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NUMANODE);
  const auto ncores = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
  const auto nsockets = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PACKAGE);
  const auto base_steal_bits = (1ULL << nNumaNodes) - 1;

  KA_TRACE(1, ("__kmp_read_topology: Number of NUMA nodes detected to %u, "
               "total cores = %u, sockets = %u, base_steal_bits = %lu\n",
               nNumaNodes, ncores, nsockets, base_steal_bits));

  // Todo: generate stealmasks based on policy

  return NumaTopology(static_cast<kmp_uint32>(nNumaNodes),
                      static_cast<kmp_uint32>(ncores),
                      static_cast<kmp_uint32>(nsockets), base_steal_bits);
}