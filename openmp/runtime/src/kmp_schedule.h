#pragma once

#include "kmp.h"
#include "kmp_os.h"
#include "kmp_routine.h"

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;
union kmp_task_team;
union kmp_thread_data;
struct kmp_taskdata;
struct kmp_affinity_t;

class NumaTopology {
public:
  explicit NumaTopology(uint32_t num_numa, uint32_t nthreads)
      : m_num_numa(num_numa), m_nthreads(nthreads) {};
  uint32_t get_num_numa() const { return m_num_numa; }
  uint32_t get_nthreads() const { return m_nthreads; }

private:
  uint32_t m_num_numa;
  uint32_t m_nthreads;
};

namespace Schedule {

// Scheduling decisions
kmp_int32 __kmp_get_optimal_grainsize(kmp_info *thread);
kmp_thread_data *__kmp_optimal_thread(kmp_info *master_thread,
                                      kmp_task_team *task_team,
                                      kmp_taskdata *taskdata);
void __kmp_set_task_affinity(kmp_info *thread, kmp_taskdata *taskdata,
                             kmp_int64 routine_id, kmp_uint64 lb, kmp_uint64 ub,
                             kmp_uint64 glob_ub);
void __kmp_set_any_affinity(kmp_taskdata *taskdata);

// Affinity part
void __kmp_set_numa_affinity(kmp_affinity_t *affinity, int32_t ncpus);
void __kmp_show_affinity(kmp_info *thread);
void __kmp_set_per_thread_affinity(kmp_info *thread, int32_t gtid, int place);

// Routine part
void __kmp_store_routine_stats(kmp_int64 routine_id, routine_stats stats);
routine_config __kmp_select_config(kmp_info *thread, kmp_uint64 num_tasks);
void __kmp_start_routine_timer();
kmp_real64 __kmp_get_routine_timer();

// Topology part
NumaTopology __kmp_read_topology();

} // namespace Schedule
