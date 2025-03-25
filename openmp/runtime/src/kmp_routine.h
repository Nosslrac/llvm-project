#pragma once

#include "kmp_os.h"
#include <unordered_map>
#include "kmp_debug.h"
#include <cfloat>
#include <climits>

union kmp_info;
constexpr int MOLDABILITY_GRANULARITY = 8;

enum class AffinityPolicy : int {
  DEFAULT = 0,
  NUMA_STRICT = 1,
  NUMA_LOOSE = 2,
  NONE = 3,
};

struct routine_config {
  kmp_int64 num_threads = -1;
  kmp_int64 num_tasks = -1;
  AffinityPolicy task_affinity = AffinityPolicy::NONE;
};

struct routine_stats {
  kmp_real64 execution_time = -1;
  kmp_real64 stall_ratio = -1; // Total cycle/stall cycle

  // add some more stats of course
  // maybe taskloop idleness ratio, stall ratio etc
};

struct routine_config_hash {
  std::size_t operator()(const routine_config &rc) const {
    std::size_t h1 = std::hash<kmp_int64>{}(rc.num_threads);
    std::size_t h2 = std::hash<kmp_int64>{}(rc.num_tasks);
    std::size_t h3 = std::hash<int>{}(static_cast<int>(rc.task_affinity));
    return h1 ^ (h2 << 1) ^ (h3 << 2);
  }
};

class Routine {

private:
  kmp_int64 routine_id;
  std::unordered_map<routine_config, routine_stats, routine_config_hash>
      execution_history;
  routine_config current_config;
  bool minima_found;

  routine_config getFastestConfig(int val);

public:
  Routine(kmp_int64);
  routine_config getCurrentConfig();
  routine_config getDefaultConfig(kmp_info *, kmp_int64);
  routine_config getNextConfig();
  void storeExecution(routine_stats);
};
