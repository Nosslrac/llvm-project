#pragma once

#include "kmp_os.h"

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
  PAGE_FAULTS = 5,
};

constexpr int32_t NUM_PERF_EVENTS = 6;

namespace Perf {
  void __kmp_init_counters(kmp_info *thread, int32_t gtid);
  void __kmp_stop_counters(kmp_info *thread, int32_t gtid, kmp_int32 *routine, kmp_int32 *task_id);

  void __kmp_summarize_taskloop(kmp_team *team);
} // namespace Perf