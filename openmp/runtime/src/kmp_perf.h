#pragma once

#include "kmp_os.h"

#include <string>

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;


enum class PerfEvents : int
{
  BACK_STALL = 0,
  CACHE_REFS = 1,
  LLC_MISSES = 2,
  TOT_CYCLES = 3,
};

constexpr int32_t NUM_PERF_EVENTS = 4;

namespace Perf {
  void __kmp_init_counter(kmp_info *thread, int32_t gtid);
  void __kmp_stop_counter(kmp_info *thread, int32_t gtid);


} // namespace Perf