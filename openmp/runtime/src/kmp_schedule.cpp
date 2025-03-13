#include "kmp_schedule.h"

#include "kmp.h"
#include "kmp_debug.h"
#include "kmp_affinity.h"

#include <immintrin.h>
#include <bitset>

namespace {

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
  KA_TRACE(3, ("__kmp_select_thread: Task#%d: Taskdata->td_affin_mask=%lu=0b%s scheduled on T#%d\n", 
  taskdata->td_task_id, taskdata->td_affin_mask, binaryRep.to_string().c_str(), node));
  KMP_ASSERT(node < nthreads);
  return team->t.t_threads[static_cast<int>(node)];
}

} // namespace

kmp_thread_data_t *Schedule::__kmp_optimal_thread(kmp_info *master_thread,
                                                  kmp_task_team *task_team,
                                                  kmp_taskdata_t *taskdata) {
  int32_t nthreads = task_team->tt.tt_nproc;
  kmp_team *team = master_thread->th.th_team;
  kmp_info_t *rand_thread =
      __kmp_select_thread(team, taskdata, nthreads); // Get primary thread from NUMA node

  const auto new_gtid = __kmp_gtid_from_thread(rand_thread);
  const auto tid = __kmp_tid_from_gtid(new_gtid);
  kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[tid];

  if (UNLIKELY(thread_data->td.td_deque == NULL)) {
    __kmp_alloc_task_deque(thread_data, new_gtid);
  }

  KA_TRACE(1, ("%s:%d: __kmp_get_random_deque: Finding random deque to add "
               "task T#%d.\n ",
               __FILE_NAME__, __LINE__, new_gtid));

  return thread_data;
}


void Schedule::__kmp_set_task_affinity(kmp_info *thread, kmp_taskdata_t* taskdata, int32_t taskid, int32_t ntasks)
{
  kmp_team_t *team = thread->th.th_team;
  int32_t nthreads = team->t.t_nproc;
  if(taskid == -1)
  {
    taskid = 0;
    KA_TRACE(2, ("%s:%d: __kmp_set_task_affinity: Task is from taskloop recur => run on NUMA %d\n", __FILE_NAME__, __LINE__, taskid));
  }
  const auto nNumaNodes = nthreads / 8; // TODO: use topology to decide this
  const auto numaNodeSize = nthreads / nNumaNodes;

  const auto bucketSize = ntasks / nNumaNodes;
  const auto numaId = taskid / bucketSize;

  const auto discreteProc = numaId * numaNodeSize; // 0 | 8 | 16 | 24 | ...
  // Set the correct Numa node bits
  taskdata->td_affin_mask = 0b11111111ULL << discreteProc;
  KA_TRACE(2, ("__kmp_set_task_affinity: Nnuma=%d, numaid=%d, Proc=%d Task#%d => Affin_mask=%lu. ntasks=%d\n",
      nNumaNodes, numaId, discreteProc, taskid, taskdata->td_affin_mask, ntasks));
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
      KA_TRACE(1, ("T#%d has affinity: %s\n", __kmp_gtid_from_thread(thread), buf));
    
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

kmp_int32 Schedule::__kmp_get_optimal_grainsize(kmp_info *thread) {

  kmp_int32 grainsize = thread->th.th_team_nproc * 10;

  int gtid = __kmp_gtid_from_thread(thread);

  KA_TRACE(1, ("__kmp_schedule:get_optimal_grainsize: T#%d, grain %d.\n", gtid,
               grainsize));

  return grainsize;
}

