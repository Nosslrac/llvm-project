#include "kmp.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_schedule.h"

namespace {

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

kmp_info_t *__kmp_select_thread(kmp_info *master_thread, kmp_team *team,
                                int32_t nthreads) {
  // Round robin scheduling
  master_thread->th.next_thread =
      (master_thread->th.next_thread + 1) % nthreads;
  return team->t.t_threads[static_cast<int>(master_thread->th.next_thread)];
}

} // namespace

kmp_thread_data_t *Schedule::__kmp_optimal_thread(kmp_info *master_thread,
                                                  kmp_task_team *task_team) {
  int32_t nthreads = task_team->tt.tt_nproc;
  kmp_team *team = master_thread->th.th_team;
  kmp_info_t *rand_thread =
      __kmp_select_thread(master_thread, team, nthreads); // Get random thread

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

void Schedule::__kmp_init_affinity(kmp_team *task_team) {
  // Default proc bind to true
  task_team->t.t_proc_bind = proc_bind_true;
}

kmp_int32 Schedule::__kmp_get_optimal_grainsize(kmp_info *thread) {

  kmp_int32 grainsize = thread->th.th_team_nproc * 10;

  int gtid = __kmp_gtid_from_thread(thread);

  KA_TRACE(1, ("__kmp_schedule:get_optimal_grainsize: T#%d, grain %d.\n", gtid,
               grainsize));

  return grainsize;
}
