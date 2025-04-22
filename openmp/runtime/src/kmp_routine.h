#pragma once

#include "kmp_os.h"
#include <unordered_map>
#include "kmp_debug.h"
#include "kmp_topo.h"
#include <cfloat>
#include <climits>
#include <vector>

union kmp_info;
constexpr kmp_real64 TINY_TASKLOOP_THRESHOLD = 0.005;
constexpr kmp_real64 LOAD_BALANCE_REQUIRED_FACTOR = 2.0;

enum class StealPolicy : kmp_uint16 {
  NUMA = 0,
  SOCKET = 1,
  FULL = (1U << 15),
};

struct routine_config {
  kmp_int64 num_threads = -1;
  kmp_int64 num_tasks = -1;
  kmp_uint16 node_mask = 0;
  StealPolicy task_affinity = StealPolicy::NUMA;
};

struct routine_stats {
  kmp_real64 execution_time = -1;
  kmp_real64 IPC = -1;
};

using routine_stats_nodes = std::vector<routine_stats>;

struct routine_config_hash {
  std::size_t operator()(const routine_config &rc) const {
    std::size_t h1 = std::hash<kmp_int64>{}(rc.num_threads);
    std::size_t h2 = std::hash<kmp_int64>{}(rc.num_tasks);
    std::size_t h3 = std::hash<kmp_uint16>{}(rc.node_mask);
    std::size_t h4 = std::hash<int>{}(static_cast<int>(rc.task_affinity));
    return h1 ^ (h2 << 1) ^ (h3 << 2) ^ (h4 << 3);
  }
};

class Routine {

private:
  kmp_int64 routine_id;
  std::unordered_map<routine_config, routine_stats_nodes, routine_config_hash>
      execution_history;
  routine_config current_config;
  bool minima_found;
  bool initial_iteration;

  int NUM_NUMANODES;
  int NUMANODE_SIZE;
  int NUM_SOCKETS;
  int SOCKET_SIZE; // Number of NUMA nodes
  int MOLDABILITY_GRANULARITY;

  routine_config getFastestConfig();
  kmp_real64 calcSlowestNUMAExec(routine_config);
  kmp_real64 calcFastestNUMAExec(routine_config);
  kmp_real64 calcAverageNUMAExec(routine_config);
  kmp_uint16 getNUMAMask(kmp_uint64);
  StealPolicy checkLoadBalance(routine_config);

public:
  Routine(kmp_int64);
  routine_config getCurrentConfig() const;
  routine_config getDefaultConfig(kmp_info *, kmp_int64);
  routine_config getInterTaskloopConfig(kmp_int8);
  routine_config getNextConfig();
  void storeExecution(routine_stats_nodes);
};
