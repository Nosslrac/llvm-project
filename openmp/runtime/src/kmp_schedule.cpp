#include "kmp_schedule.h"

#include "hwloc.h"
#include "kmp.h"
#include "kmp_debug.h"
#include <unordered_map>
#include "kmp_os.h"
#include "kmp_perf.h"
#include "kmp_routine.h"
#include "kmp_topo.h"

#include <immintrin.h>

namespace {

constexpr kmp_uint32 LOAD_STRICT = 3;

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

kmp_int32 __kmp_get_distribution_tid(kmp_taskdata_t *taskdata,
                                     kmp_uint32 nthreads) {
  KMP_DEBUG_ASSERT(taskdata);

  // Divide the threads equally accross NUMA nodes
  const auto numaCores = Topo::numa_topology.get_num_cores();
  const auto numNuma = Topo::numa_topology.get_num_numa();
  KMP_DEBUG_ASSERT(numaCores);
  KMP_DEBUG_ASSERT(numNuma);

  const auto numaSize = numaCores / numNuma;
  KMP_DEBUG_ASSERT(numaSize);
  const auto processor = taskdata->td_numa_place * numaSize;

  KMP_DEBUG_ASSERT(processor < nthreads);
  return static_cast<kmp_int32>(processor);
}

} // namespace

kmp_int32 Schedule::__kmp_get_numa_base(kmp_int32 victim_tid) {
  const auto numaCores = Topo::numa_topology.get_num_cores();
  const auto numNuma = Topo::numa_topology.get_num_numa();
  KMP_DEBUG_ASSERT(numaCores);
  KMP_DEBUG_ASSERT(numNuma);

  const auto numaSize = numaCores / numNuma;
  KMP_DEBUG_ASSERT(numaSize);
  return static_cast<kmp_int32>((victim_tid / numaSize) * numaSize);
}

kmp_thread_data_t *Schedule::__kmp_optimal_thread(kmp_info *master_thread,
                                                  kmp_task_team *task_team,
                                                  kmp_taskdata_t *taskdata,
                                                  kmp_int64 routine_id) {

  KMP_DEBUG_ASSERT(taskdata);

  const auto nthreads = task_team->tt.tt_nproc;

  kmp_int32 base_numa_tid = __kmp_get_distribution_tid(
      taskdata, nthreads); // Get primary thread from NUMA node

  kmp_thread_data_t *thread_data =
      &task_team->tt.tt_threads_data[base_numa_tid];
  kmp_info_t *base_numa_thread = thread_data->td.td_thr;
  taskdata->td_affin_mask |= Schedule::__kmp_get_load_balance_mask(
      base_numa_thread, thread_data, routine_id);

  KA_TRACE(3, ("%s:%d: __kmp_optimal_thread: Base NUMA thread tid=%d\n ",
               __FILE_NAME__, __LINE__, base_numa_tid));
  return thread_data;
}

//
void Schedule::__kmp_set_any_affinity(kmp_taskdata_t *taskdata) {
  KMP_DEBUG_ASSERT(taskdata);
  taskdata->td_affin_mask = static_cast<kmp_uint16>(StealPolicy::FULL);
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

  // Info based on topology
  const auto numaCores = Topo::numa_topology.get_num_cores();
  KMP_DEBUG_ASSERT(numaCores);
#ifdef MOLDABILITY
  KMP_DEBUG_ASSERT(routine_map.find(routine_id) != routine_map.end());
  routine_config config = routine_map.at(routine_id).getCurrentConfig();
  const auto numNuma = bitCount(config.node_mask);
  const auto firstNode = bitScan(config.node_mask);
#else
  const auto numNuma = Topo::numa_topology.get_num_numa();
  const auto firstNode = 0;
#endif
  KMP_DEBUG_ASSERT(numNuma);

  const auto numaNodeSize = numaCores / numNuma;
  KMP_DEBUG_ASSERT(numaNodeSize);

  // When single thread is executing
  if (nthreads == 1) {
    taskdata->td_numa_place = static_cast<kmp_uint8>(__kmp_tid_from_gtid(
                                  __kmp_gtid_from_thread(thread))) /
                              numaNodeSize;
    taskdata->td_affin_mask = static_cast<kmp_uint16>(StealPolicy::FULL);
    return;
  }

  const auto midRange = (lb + ub) / 2;
  const auto bucketSize = max(glob_ub / numNuma, numaNodeSize);
  const auto numaId = static_cast<kmp_uint8>(
      min((midRange / bucketSize) + firstNode, numNuma - 1));
  KMP_DEBUG_ASSERT(numaId < numNuma);

  taskdata->td_affin_mask = static_cast<kmp_uint16>(1U << numaId);
  taskdata->td_numa_place = numaId;

  KA_TRACE(
      3,
      ("%s:%d: __kmp_set_task_affinity: for routine %p: Nthreads=%d, Nnuma=%d, "
       "numaid=%d,"
       "bucketSize=%lu "
       "MidIter#%d => Affin_mask=%lu.\n",
       __FILE_NAME__, __LINE__, routine_id, nthreads, numNuma, numaId,
       bucketSize, midRange, taskdata->td_affin_mask));
}

void Schedule::__kmp_reset_head_all(kmp_task_team *task_team) {
  for (auto i = 0; i < task_team->tt.tt_nproc; ++i) {
    kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[i];
    Schedule::__kmp_set_start_head(task_team, thread_data->td.td_thr, i);
  }
}

void Schedule::__kmp_set_start_head(kmp_task_team_t *task_team,
                                    kmp_info_t *thread, kmp_int32 tid) {
  const auto numa_base_tid = Schedule::__kmp_get_numa_base(tid);

  kmp_thread_data_t *threads_data =
      &task_team->tt.tt_threads_data[numa_base_tid];
  KMP_DEBUG_ASSERT(threads_data != NULL);
  thread->th.numa_head_start = threads_data->td.td_deque_head;
  KA_TRACE(3, ("__kmp_set_start_head: Thread tid=%d set head to %d\n", tid,
               thread->th.numa_head_start));
}

kmp_uint16 Schedule::__kmp_get_load_balance_mask(kmp_info_t *thread,
                                                 kmp_thread_data_t *thread_data,
                                                 kmp_int64 routine_id) {

  const auto coresPerNuma =
      Topo::numa_topology.get_num_cores() / Topo::numa_topology.get_num_numa();
  const auto numStrictTasks = coresPerNuma * LOAD_STRICT;
  // Distance of the current spot in queue in comparison to where queue started
  // this taskloop
  const auto distance =
      (thread_data->td.td_deque_tail - thread->th.numa_head_start +
       thread_data->td.td_deque_size) &
      TASK_DEQUE_MASK(thread_data->td);

  // If in strict range then disable load balancing for this task
  if (distance <= numStrictTasks) {
    return static_cast<kmp_uint16>(StealPolicy::NUMA);
  }
#ifdef MOLDABILITY
  KMP_DEBUG_ASSERT(routine_map.find(routine_id) != routine_map.end())
  routine_config config = routine_map.at(routine_id).getCurrentConfig();
  return static_cast<kmp_uint16>(config.task_affinity);
#endif
  return static_cast<kmp_uint16>(StealPolicy::FULL);
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

///
/// @brief This function performs the logical thread pinning to physical
/// cores and sets up NUMA specific variables for the thread.
/// @note This is required for the performance montoring in kmp_perf.cpp
///
void Schedule::__kmp_set_per_thread_affinity(kmp_info *thread, int32_t gtid) {
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;
  int place = __kmp_tid_from_gtid(gtid);

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

  const auto numaId =
      static_cast<kmp_uint8>(place) / (Topo::numa_topology.get_num_cores() /
                                       Topo::numa_topology.get_num_numa());
  // This thread is allowed to steal tasks with matching mask
  thread->th.steal_mask =
      (1U << numaId) |
      static_cast<kmp_uint16>(StealPolicy::FULL); // All threads can steal tasks
                                                  // with load balance bit
}

///
/// @brief This function initializes the global affinity object in
/// OpenMP runtime based on the hardware topology.
/// @note This does not perform the thread pinning. For that
/// refer to Schedule::__kmp_set_per_thread_affinity().
///
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

void Schedule::__kmp_store_routine_stats(kmp_team *team, kmp_int64 routine_id) {
  routine_stats_nodes stats(Topo::numa_topology.get_num_numa());

  const kmp_real64 taskloop_start_time = Schedule::__kmp_get_routine_timer();
  Perf::__kmp_get_taskloop_stats(team, stats, taskloop_start_time);

  // Verify that the routine exists in the map
  KMP_DEBUG_ASSERT(routine_map.find(routine_id) != routine_map.end());

  KA_TRACE(3, ("__kmp_store_routine_stats: New stat store for routine %p\n",
               routine_id));

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
    ret_config = routine_map.at(routine_id).getNextConfig();
  }

  KA_TRACE(1,
           ("__kmp_select_config: routine %p was given new config={%d, %d, %d, "
            "%d}.\n",
            routine_id, ret_config.num_threads, ret_config.num_tasks,
            ret_config.node_mask, static_cast<int>(ret_config.task_affinity)));

  return ret_config;
}

void Schedule::__kmp_start_routine_timer() {
  __kmp_read_system_time(&routine_timer);
}

kmp_real64 Schedule::__kmp_get_routine_timer() { return routine_timer; }
