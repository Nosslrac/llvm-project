#include "kmp_schedule.h"

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

inline int min(const int a, const int b)
{
  if(a < b)
  {
    return a;
  }
  return b;
}

// Routine stuff
std::unordered_map<kmp_int64, Routine> routine_map; // Map containing all routines

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

kmp_info_t *__kmp_select_thread(kmp_team *team, kmp_taskdata_t *taskdata, int32_t nthreads) {
  // Divide the threads equally accross NUMA nodes
  const auto nNumaNodes = nthreads / 8; // TODO: use topology to decide this
  const auto node = (bitScan(taskdata->td_affin_mask) / nNumaNodes) * nNumaNodes;
  std::bitset<64> binaryRep(taskdata->td_affin_mask);
  KA_TRACE(3, ("%s:%d: __kmp_select_thread: Task#%p: Taskdata->td_affin_mask=%lu=0b%s scheduled on T#%d\n", 
  __FILE_NAME__, __LINE__, taskdata, taskdata->td_affin_mask, binaryRep.to_string().c_str(), node));
  KMP_DEBUG_ASSERT(node < nthreads);
  return team->t.t_threads[static_cast<int>(node)];
}

} // namespace

kmp_thread_data_t *Schedule::__kmp_optimal_thread(kmp_info *master_thread,
                                                  kmp_task_team *task_team,
                                                  kmp_taskdata_t *taskdata) {
  int32_t nthreads = task_team->tt.tt_nproc;
  kmp_team *team = master_thread->th.th_team;
  kmp_info_t *base_numa =
      __kmp_select_thread(team, taskdata, nthreads); // Get primary thread from NUMA node

  const auto new_gtid = __kmp_gtid_from_thread(base_numa);
  const auto tid = __kmp_tid_from_gtid(new_gtid);
  kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[tid];

  if (UNLIKELY(thread_data->td.td_deque == NULL)) {
    __kmp_alloc_task_deque(thread_data, new_gtid);
  }

  KA_TRACE(3, ("%s:%d: __kmp_optimal_thread: Base NUMA thread based on affinity T#%d"
               "(tid=%d).\n ",
               __FILE_NAME__, __LINE__, new_gtid, tid));

  return thread_data;
}

//
void Schedule::__kmp_set_any_affinity(kmp_taskdata_t* taskdata)
{
  taskdata->td_affin_mask = ALL_PROCS;
  KA_TRACE(3, ("__kmp_set_any_affinity: Setting any affinity child task %p of parent %p\n",
              taskdata, taskdata->td_parent));
}


void Schedule::__kmp_set_task_affinity(kmp_info *thread, kmp_taskdata_t* taskdata, 
  kmp_int64 routine_id, kmp_uint64 lb, kmp_uint64 ub, kmp_uint64 glob_ub)
{
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;

  // Moldability: the number of threads used
  // are determined by the selected config.
  routine_config config = routine_map.at(routine_id).getCurrentConfig();
  nthreads = config.num_threads;

  // TODO: use topology to decide this.
  // TODO: enable the use of "half" numa nodes, right 
  //       now only whole numa nodes can be used.
  const auto numaNodeSize = 8;
  const auto nNumaNodes = nthreads / numaNodeSize; 

  const auto midRange = (lb + ub) / 2;
  const auto bucketSize = glob_ub / nNumaNodes;
  auto numaId = midRange / bucketSize;
  numaId = min(numaId, nNumaNodes - 1); // Round down for last iterations
  const auto discreteProc = numaId * numaNodeSize; // 0 | 8 | 16 | 24 | ...

  // Set the correct Numa node bits
  taskdata->td_affin_mask = 0b11111111ULL << discreteProc;
  KA_TRACE(3, ("%s:%d: __kmp_set_task_affinity: Nnuma=%d, numaid=%d, Proc=%d MidIter#%d => Affin_mask=%lu.\n",
      __FILE_NAME__, __LINE__, nNumaNodes, numaId, discreteProc, midRange, taskdata->td_affin_mask));
  
  KMP_DEBUG_ASSERT(numaId < nNumaNodes);
  KMP_DEBUG_ASSERT(discreteProc < nthreads);
}


void Schedule::__kmp_show_affinity(kmp_info *thread)
{
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;
  for (int i = 0; i < nthreads; ++i) {
      kmp_info_t *thread = team->t.t_threads[i];
      char buf[KMP_AFFIN_MASK_PRINT_LEN];
      __kmp_affinity_print_mask(buf, KMP_AFFIN_MASK_PRINT_LEN,
                                thread->th.th_affin_mask);
      KA_TRACE(3, ("T#%d has affinity: %s\n", __kmp_gtid_from_thread(thread), buf));
    
  }
  
}

void Schedule::__kmp_set_per_thread_affinity(kmp_info *thread, int32_t gtid, int place)
{
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;
  int tid = __kmp_tid_from_gtid(gtid);
  place = tid; // TODO: Use topology to map threads to specific cores
  if(thread->th.th_affin_mask == NULL)
  {
    KA_TRACE(5, ("Alloc: T#%d\n", gtid));
    KMP_CPU_ALLOC(thread->th.th_affin_mask);
  }
  else
  {
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
  KA_TRACE(5, ("Setting thread affinity: Tid=%d T#%d, Affinity: %s\n", tid, gtid, buf));
}

// Initialize global affinity object for the main thread and prepare for workers
void Schedule::__kmp_set_numa_affinity(kmp_affinity_t* global_affin, int32_t ncpus)
{
  // To make bind_place do something
  global_affin->type=affinity_explicit;
  global_affin->num_masks = ncpus;

  KMP_CPU_ALLOC_ARRAY(global_affin->masks, ncpus);

  kmp_affin_mask_t* mask;
  for (int i = 0; i < ncpus; i++) {
    mask = KMP_CPU_INDEX(global_affin->masks, i);
    KMP_CPU_ZERO(mask);
  }

  KA_TRACE(1, ("Affinity:\n"
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


void Schedule::__kmp_store_routine_stats(kmp_int64 routine_id, routine_stats stats) {

  kmp_real64 end_time;
  __kmp_read_system_time(&end_time);
  kmp_real64 tot_exec_time = end_time - routine_timer;
  stats.execution_time = tot_exec_time;

  // Verify that the routine exists in the map
  KMP_DEBUG_ASSERT(routine_map.find(routine_id) != routine_map.end())

  KA_TRACE(1, ("__kmp_store_routine_stats: New stat store for routine %p:" 
    " tot_exec_time:%f, stall_ratio:%f\n",
    routine_id, stats.execution_time, stats.stall_ratio));

  // Store the execution stats
  routine_map.at(routine_id).storeExecution(stats);
}


routine_config Schedule::__kmp_select_config(kmp_info* thread, kmp_uint64 num_tasks) {
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
    }
#else
      ret_config = routine_map.at(routine_id).getDefaultConfig(thread, num_tasks);
    }
#endif


  KA_TRACE(1, ("__kmp_select_config: routine %p was given new config={%d, %d, %d}.\n",
    routine_id, ret_config.num_threads, ret_config.num_tasks, 
    static_cast<int>(ret_config.task_affinity)));

  return ret_config;
}

void Schedule::__kmp_start_routine_timer(){
  __kmp_read_system_time(&routine_timer);
}