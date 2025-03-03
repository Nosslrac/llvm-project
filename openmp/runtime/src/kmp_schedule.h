#pragma once

#include "kmp_os.h"

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;
union kmp_task_team;
union kmp_thread_data;

namespace Schedule {
  kmp_thread_data* __kmp_optimal_thread(kmp_info *master_thread, kmp_task_team *task_team);
  void __kmp_init_affinity(kmp_team *task_team);

} // namespace Schedule