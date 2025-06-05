#include "kmp_schedule.h"

#include "kmp.h"
#include "kmp_debug.h"
#include <bitset>
#include <unordered_map>
#include "kmp_os.h"
#include "kmp_perf.h"
#include "kmp_routine.h"
#include "kmp_topo.h"

#include <immintrin.h>

namespace {

constexpr kmp_uint32 NUM_LOAD_STRICT_TASK = 3;

#ifdef MOLDABILITY

inline kmp_uint8 bitCount(kmp_uint64 mask) {
#if __has_builtin(__builtin_popcountll)
  return static_cast<kmp_uint8>(__builtin_popcountll(mask));
#else
  return 0; // Impl needed
#endif
}

inline kmp_uint8 bitScan(kmp_uint64 mask) {
#if __has_builtin(__builtin_ctzll)
  return static_cast<kmp_uint8>(__builtin_ctzll(mask));
#else
  return 0; // Impl needed
#endif
}

inline kmp_uint8 findBit(kmp_uint16 nodeMask, const kmp_uint8 count) {
  for (auto i = count; i > 0; i--) {
    nodeMask &= nodeMask - 1;
  }
  KMP_DEBUG_ASSERT(nodeMask > 0);
  return bitScan(nodeMask);
}

#endif

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

// Shared globals: used by summarizing thread
std::unordered_map<kmp_int64, Routine>
    routine_map; // Map containing all routines

kmp_real64 routine_timer;

} // namespace

///
/// @brief This function returns the base node of the NUMA node
/// containing the processor corresponding to tid.
///
kmp_int32 Schedule::__kmp_get_numa_base(kmp_int32 tid) {
  const auto numaCores = Topo::numa_topology.get_num_cores();
  const auto numNuma = Topo::numa_topology.get_num_numa();
  KMP_DEBUG_ASSERT(numaCores);
  KMP_DEBUG_ASSERT(numNuma);

  const auto numaSize = numaCores / numNuma;
  KMP_DEBUG_ASSERT(numaSize);
  return static_cast<kmp_int32>((tid / numaSize) * numaSize);
}

///
/// @brief This function selects the thread_data queue to put the
/// taskdata on based on td_task_place_tid (calculated in
/// __kmp_set_task_affinity). It might update the affinity mask of the task to
/// enable load balancing.
///
kmp_thread_data_t *
Schedule::__kmp_select_thread_data_queue(kmp_task_team *task_team,
                                         kmp_taskdata_t *taskdata) {

  const auto nthreads = task_team->tt.tt_nproc;
  KMP_DEBUG_ASSERT(taskdata);
  KMP_DEBUG_ASSERT(taskdata->td_task_place_tid < nthreads);

  kmp_thread_data_t *thread_data =
      &task_team->tt.tt_threads_data[taskdata->td_task_place_tid];
  kmp_info_t *base_numa_thread = thread_data->td.td_thr;
  if (taskdata->td_affin_mask !=
      static_cast<kmp_uint16>(StealPolicy::TASK_GENERATION)) {
#ifdef MOLDABILITY
    const kmp_uint16 available_steal = taskdata->td_available_steal;
#else
    const kmp_uint16 available_steal =
        static_cast<kmp_uint16>(StealPolicy::FULL);
#endif
    taskdata->td_affin_mask |= Schedule::__kmp_get_load_balance_mask(
        base_numa_thread, thread_data, available_steal);
  }

  KA_TRACE(3, ("%s:%d: __kmp_optimal_thread: Base NUMA thread tid=%d\n ",
               __FILE_NAME__, __LINE__, taskdata->td_task_place_tid));
  return thread_data;
}

///
/// @brief This function sets up the affinity of taskloop tasks.
/// The affinity is based on the iteration range of the tasks and
/// decides which NUMA node this task is put on.
/// @note The affinity mask of the task (td_affin_mask) will be updated
/// in __kmp_select_thread_data_queue() and might be tagged for load balancing.
void Schedule::__kmp_set_task_affinity(kmp_info *thread,
                                       kmp_taskdata_t *taskdata,
                                       const kmp_int64 routine_id,
                                       const PolicyInfo &policyInfo,
                                       const kmp_uint64 lb, const kmp_uint64 ub,
                                       const kmp_uint64 glob_ub) {
  KMP_DEBUG_ASSERT(taskdata);

  kmp_team_t *team = thread->th.th_team;
  const auto nthreads = static_cast<uint32_t>(team->t.t_nproc);

  // Info based on topology
#ifdef MOLDABILITY
  // Load balance mask is the same as node_mask when moldability is active
  const auto numNuma = bitCount(policyInfo.node_mask);
#else
  const auto numNuma = Topo::numa_topology.get_num_numa();
#endif
  KMP_DEBUG_ASSERT(numNuma);
  const kmp_uint8 numaNodeSize = Topo::numa_topology.get_numa_size();
  KMP_DEBUG_ASSERT(numaNodeSize);

  // When single thread is executing
  if (nthreads == 1) {
    taskdata->td_task_place_tid = (static_cast<kmp_uint8>(__kmp_tid_from_gtid(
                                       __kmp_gtid_from_thread(thread))) /
                                   numaNodeSize) *
                                  numaNodeSize;
    taskdata->td_affin_mask = static_cast<kmp_uint16>(StealPolicy::FULL);
    taskdata->td_available_steal = static_cast<kmp_uint16>(StealPolicy::FULL);
    return;
  }

  const auto midRange = (lb + ub) / 2;
  const auto bucketSize = max(glob_ub / numNuma, numaNodeSize);
  const auto bucketId =
      static_cast<kmp_uint8>(min((midRange / bucketSize), numNuma - 1));
  KMP_DEBUG_ASSERT(bucketId < numNuma);
#ifdef MOLDABILITY
  const auto numaId = findBit(policyInfo.node_mask, bucketId);
#else
  const auto numaId = bucketId;
#endif

  taskdata->td_affin_mask = static_cast<kmp_uint16>(1U << numaId);
  taskdata->td_task_place_tid = numaId * numaNodeSize;
  taskdata->td_available_steal = policyInfo.available_steal_mask;

  KA_TRACE(3, ("%s:%d: __kmp_set_task_affinity: for routine %p: Nthreads=%d, "
               "Nnuma=%d, "
               "numaid=%d,"
               "bucketSize=%lu "
               "MidIter#%d => Affin_mask=0b%s.\n"
               "Task_place=%d\n",
               __FILE_NAME__, __LINE__, routine_id, nthreads, numNuma, numaId,
               bucketSize, midRange,
               std::bitset<16>(taskdata->td_affin_mask).to_string().c_str(),
               taskdata->td_task_place_tid));
}

///
/// @brief Update all thread's numa_head_start to their corresponding NUMA
/// base's thread_data deque head.
///
void Schedule::__kmp_set_head_all(kmp_task_team *task_team) {
  for (auto i = 0; i < task_team->tt.tt_nproc; ++i) {
    kmp_thread_data_t *thread_data = &task_team->tt.tt_threads_data[i];
    Schedule::__kmp_set_start_head(task_team, thread_data->td.td_thr, i);
  }
}

///
/// @brief Sets the numa_head_start of the thread. This is used to decide
/// whether a task will be marked for load balancing later.
///
void Schedule::__kmp_set_start_head(kmp_task_team_t *task_team,
                                    kmp_info_t *thread, kmp_int32 tid) {
  const auto numa_base_tid = Schedule::__kmp_get_numa_base(tid);

  kmp_thread_data_t *threads_data =
      &task_team->tt.tt_threads_data[numa_base_tid];
  KMP_DEBUG_ASSERT(threads_data != NULL);
  thread->th.has_execed_on_self = 0;
  thread->th.numa_head_start = threads_data->td.td_deque_head;
  KA_TRACE(3, ("__kmp_set_start_head: Thread tid=%d set head to %d\n", tid,
               thread->th.numa_head_start));
}

///
/// @brief Decide if task should be marked for load balancing.
///
kmp_uint16
Schedule::__kmp_get_load_balance_mask(kmp_info_t *thread,
                                      kmp_thread_data_t *thread_data,
                                      const kmp_uint16 available_steal) {

  const auto coresPerNuma =
      Topo::numa_topology.get_num_cores() / Topo::numa_topology.get_num_numa();
  const auto numStrictTasks = coresPerNuma * NUM_LOAD_STRICT_TASK;
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
  return available_steal;
}

///
/// @brief Print the current affinity of all threads.
///
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

///
/// @brief Summarize and store the stats for the executed taskloop
///
void Schedule::__kmp_store_routine_stats(kmp_team *team, kmp_int64 routine_id) {
  if (team->t.t_nproc == 1) {
    KA_TRACE(
        1,
        ("__kmp_store_routine_stats: Only 1 thread, do not store stats. %p\n",
         routine_id));
    return;
  }
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

///
/// @brief Get the next config for the taskloop routine that is
/// about to be executed.
///
routine_config Schedule::__kmp_select_config(kmp_info *thread) {

  if (thread->th.th_team_nproc == 1) {
    KA_TRACE(1, ("__kmp_store_routine_stats: Select default "
                 "config={1,10,255,TASK_GEN}\n"));
    return routine_config{1, 10,
                          static_cast<kmp_uint16>(StealPolicy::TASK_GENERATION),
                          StealPolicy::TASK_GENERATION};
  }

  routine_config ret_config;
  kmp_int64 routine_id = thread->th.routine_id;

  // Check if routine has executed before
  // If not, add new routine to map and return default config
  if (routine_map.find(routine_id) == routine_map.end()) {
    routine_map.emplace(routine_id,
                        Routine(routine_id, thread->th.th_team_nproc));
    ret_config = routine_map.at(routine_id).getCurrentConfig();

  } else {
    ret_config = routine_map.at(routine_id).getNextConfig();
  }

  KA_TRACE(1,
           ("__kmp_select_config: routine %p was given new config={%d, %d, %d, "
            "%d}.\n",
            routine_id, ret_config.num_threads, ret_config.num_tasks,
            ret_config.node_mask, static_cast<int>(ret_config.steal_policy)));

  return ret_config;
}

///
/// @brief Returns the info about the policy selected for the current routine.
///
Schedule::PolicyInfo Schedule::__kmp_get_policy_info(kmp_info *thread,
                                                     kmp_int64 routine_id) {
  if (thread->th.th_team_nproc == 1) {
    return PolicyInfo(static_cast<kmp_uint16>(StealPolicy::TASK_GENERATION),
                      static_cast<kmp_uint16>(StealPolicy::FULL));
  }

#ifdef MOLDABILITY
  KMP_DEBUG_ASSERT(routine_map.find(routine_id) != routine_map.end());
  const auto &config = routine_map.at(routine_id).getCurrentConfig();
  const kmp_uint16 node_mask = config.node_mask;
  const kmp_uint16 available_steal =
      config.steal_policy == StealPolicy::FULL
          ? node_mask
          : static_cast<kmp_uint16>(StealPolicy::NUMA);
#else
  const kmp_uint16 node_mask = 0;
  const kmp_uint16 available_steal = static_cast<kmp_uint16>(StealPolicy::FULL);
#endif

  return PolicyInfo(node_mask, available_steal);
}

///
/// @brief Start the timer for the current routine.
///
void Schedule::__kmp_start_routine_timer() {
  __kmp_read_system_time(&routine_timer);
}

///
/// @brief Gets the current value of the routine timer.
///
kmp_real64 Schedule::__kmp_get_routine_timer() { return routine_timer; }
