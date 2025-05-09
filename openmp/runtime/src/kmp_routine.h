#pragma once

#include "kmp_os.h"
#include <unordered_map>
#include <cfloat>
#include <climits>
#include <vector>

union kmp_info;
constexpr kmp_real64 LOAD_BALANCE_REQUIRED_FACTOR = 1.0;
constexpr kmp_real64 TASKLOOP_PERF_COUNTERS_VALID_TIME = 0.001;

enum class StealPolicy : kmp_uint16 {
  NUMA = 0,
  SOCKET = 1,
  FULL = (1U << 15),
  TASK_GENERATION = (1U << 16) - 1
};

struct routine_config {
  kmp_int64 num_threads;
  kmp_int64 num_tasks;
  kmp_uint16 node_mask;
  StealPolicy steal_policy;
};

struct routine_stats {
  kmp_real64 execution_time;
  kmp_real64 IPC;
};

using routine_stats_nodes = std::vector<routine_stats>;

struct routine_config_hash {
  std::size_t operator()(const routine_config &rc) const {
    std::size_t h1 = std::hash<kmp_int64>{}(rc.num_threads);
    std::size_t h2 = std::hash<kmp_int64>{}(rc.num_tasks);
    std::size_t h3 = std::hash<kmp_uint16>{}(rc.node_mask);
    std::size_t h4 = std::hash<int>{}(static_cast<int>(rc.steal_policy));
    return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
  }
};

class Routine {

private:
  kmp_int64 m_routine_id;
  std::unordered_map<routine_config, routine_stats_nodes, routine_config_hash>
      m_execution_history;
  kmp_uint64 m_upper_bound;
  routine_config m_current_config;
  routine_config m_1stfastest;
  routine_config m_2ndfastest;
  bool m_search_finished;
  kmp_uint32 m_iteration_count;

  kmp_uint32 MOLDABILITY_GRANULARITY;

  routine_config getInitialConfig(kmp_uint32 nthreads);
  void initBinarySearch();
  void binarySearch();
  void predictiveModel();
  void predictiveEstimation();

  kmp_real64 calcSlowestNUMAExec(const routine_config &config);
  kmp_real64 calcFastestNUMAExec(const routine_config &config);
  kmp_uint16 getNUMAMask() const;
  StealPolicy checkLoadBalance();

  inline bool isXFasterThanY(const routine_config &X, const routine_config &Y) {
    return calcSlowestNUMAExec(X) < calcSlowestNUMAExec(Y);
  }

public:
  explicit Routine(kmp_int64 routine_id, kmp_uint32 nthreads, kmp_uint64 ub);
  const routine_config &getCurrentConfig() const;
  const routine_config &getNextConfig(kmp_uint64 ub);
  void storeExecution(routine_stats_nodes stats);
};
