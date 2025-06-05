#pragma once

#include "kmp.h"
#include "kmp_os.h"
#include "kmp_routine.h"
#include "kmp_topo.h"

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;
union kmp_task_team;
union kmp_thread_data;
struct kmp_taskdata;
struct kmp_affinity_t;

namespace Schedule {
struct PolicyInfo {
  explicit PolicyInfo(kmp_uint16 node_mask, kmp_uint16 available_steal)
      : node_mask(node_mask), available_steal_mask(available_steal) {};
  kmp_uint16 node_mask;
  kmp_uint16 available_steal_mask;
};

// Scheduling decisions
kmp_int32 __kmp_get_numa_base(kmp_int32 tid);
kmp_thread_data *__kmp_select_thread_data_queue(kmp_task_team *task_team,
                                                kmp_taskdata *taskdata);
void __kmp_set_task_affinity(kmp_info *thread, kmp_taskdata *taskdata,
                             kmp_int64 routine_id, const PolicyInfo &policyInfo,
                             kmp_uint64 lb, kmp_uint64 ub, kmp_uint64 glob_ub);

// Set the queue index for local numa node that has strict stealing
void __kmp_set_head_all(kmp_task_team *task_team);
void __kmp_set_start_head(kmp_task_team *task_team, kmp_info *thread,
                          kmp_int32 tid);

kmp_uint16 __kmp_get_load_balance_mask(kmp_info *thread,
                                       kmp_thread_data *thread_data,
                                       kmp_uint16 available_steal);

// Affinity part
void __kmp_set_numa_affinity(kmp_affinity_t *affinity, int32_t ncpus);
void __kmp_show_affinity(kmp_info *thread);
void __kmp_set_per_thread_affinity(kmp_info *thread, int32_t gtid);

// Routine part
void __kmp_store_routine_stats(kmp_team *team, kmp_int64 routine_id);
routine_config __kmp_select_config(kmp_info *thread);
void __kmp_start_routine_timer();
kmp_real64 __kmp_get_routine_timer();
PolicyInfo __kmp_get_policy_info(kmp_info *thread, kmp_int64 routine_id);

} // namespace Schedule
