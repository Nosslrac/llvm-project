#pragma once

#include "kmp_os.h"
#include <string>

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;

enum class PerfEvents : int {
  TOT_CYCLES = 0,
  TOT_INSTRUCTIONS = 1,
  CACHE_REFS = 2,
  LLC_MISSES = 3,
  BACK_STALL = 4,
};

constexpr int32_t NUM_PERF_EVENTS = 5;

namespace Perf {
void __kmp_init_counter(kmp_info *thread, int32_t gtid);
void __kmp_stop_counter(kmp_info *thread, int32_t gtid, int32_t *routine,
                        int32_t *task_id);

void __kmp_summarize_taskloop(kmp_team *team);

} // namespace Perf