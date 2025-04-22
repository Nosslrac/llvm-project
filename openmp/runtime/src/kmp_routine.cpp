#include "kmp_routine.h"
#include "kmp.h"
#include "kmp_config.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_schedule.h"
#include <cfloat>
#include <climits>

namespace {
routine_config UNDEFINED_CONFIG = {-1, -1, 0, StealPolicy::NUMA};

kmp_int64 getDiff(kmp_int64 a, kmp_int64 b) { return a > b ? a - b : b - a; }

kmp_int64 getMin(kmp_int64 a, kmp_int64 b) { return a < b ? a : b; }

kmp_uint16 calcMask(kmp_uint8 offset, kmp_uint8 num, int NUM_NUMA) {

  KA_TRACE(15, ("Routine::calcMask: offest: %d, num: %d, NUMA: %d\n", offset,
                num, NUM_NUMA));

  kmp_uint64 mask = (1ULL << num);
  mask = mask - 1U;
  if (offset + num <= NUM_NUMA) {
    mask = mask << offset;
  } else {
    mask = mask << (NUM_NUMA - num);
  }
  return mask;
}

void printStatsArray(routine_stats_nodes arr, int NUM_NUMA) {

  for (int i = 0; i < NUM_NUMA; i++) {
    KA_TRACE(1, ("Node[%d] ExecT:%f \n", i, arr[i].execution_time));
  }
}

} // namespace

inline bool operator==(const routine_config &lhs, const routine_config &rhs) {
  return lhs.num_threads == rhs.num_threads && lhs.num_tasks == rhs.num_tasks &&
         lhs.node_mask == rhs.node_mask &&
         lhs.task_affinity == rhs.task_affinity;
}

// Class constructor
Routine::Routine(kmp_int64 id)
    : routine_id(id), current_config(UNDEFINED_CONFIG), minima_found(false),
      initial_iteration(false) {

  NUM_NUMANODES = Topo::numa_topology.get_num_numa();
  KMP_DEBUG_ASSERT(NUM_NUMANODES);
  NUMANODE_SIZE = Topo::numa_topology.get_num_cores() / NUM_NUMANODES;
  KMP_DEBUG_ASSERT(NUMANODE_SIZE);
  NUM_SOCKETS = Topo::numa_topology.get_num_socket();
  KMP_DEBUG_ASSERT(NUM_SOCKETS);
  SOCKET_SIZE =
      NUM_NUMANODES / NUM_SOCKETS; // Number of NUMA nodes on each socket
  KMP_DEBUG_ASSERT(SOCKET_SIZE);
  MOLDABILITY_GRANULARITY = NUMANODE_SIZE;
}

routine_config Routine::getCurrentConfig() { return current_config; }

routine_config Routine::getDefaultConfig(kmp_info *thread,
                                         kmp_int64 num_tasks) {
  routine_config config;
  config.num_threads = thread->th.th_team->t.t_nproc;
  config.num_tasks = num_tasks;
  config.node_mask = calcMask(0, NUM_NUMANODES, NUM_NUMANODES);
  config.task_affinity = StealPolicy::NUMA;

  // Update current config
  current_config = config;

  return config;
}

// Exploration of possible configs using binary search (kind of) based on
// execution history. When a local minima is found, always return the fastest
// config and enable load balancing.
//
// For a certen taskloop, the moldability algorithm will work accordingly:
// 1. First & second iteration, execute on maxium number of nodes.
// 2. Third iteration, check if the execution time is below the threshold.
//    In this case, execute on only one node. Otherwise, use binary search to
//    find optimal number of threads.
// 3. The binary search will continue until a local minima is found. This condig
//    will be used for the rest of the execution.
// 4. Check if load balancing is required for the local minima config, to
//    determine if the stealing policy needs to be changed.
routine_config Routine::getNextConfig() {
  routine_config next_config = current_config;

#ifndef MOLDABILITY
#ifdef LOADBALANCE
  next_config.task_affinity = checkLoadBalance(next_config);
#endif
  return next_config;
#endif

  // If minima found, return the fastest config
  if (minima_found) {
    // Do nothing, just keep executing the current config
  }

  // Don't save the first iteration due to empty caches etc.
  else if (!initial_iteration) {
    initial_iteration = true;
  }

  // If only one previous config, try half the number of threads
  // unless the execution time is below threshold, then run on 1 node.
  else if (execution_history.size() < 2) {

    if (calcSlowestNUMAExec(current_config) < TINY_TASKLOOP_THRESHOLD) {

      next_config.num_threads = MOLDABILITY_GRANULARITY;
      KA_TRACE(1, ("Routine::getNextConfig(): Very short taskloop!!"
                   " Only use 1 NUMA node (%d threads) for routine %p .\n",
                   next_config.num_threads, routine_id));
      minima_found = true;

    } else {

      next_config.num_threads = current_config.num_threads / 2;
      KA_TRACE(
          3,
          ("Routine::getNextConfig(): Only one previous config."
           " Try half the number of threads (%d threads/2) for routine %p .\n",
           current_config.num_threads, routine_id));
    }
  }

  // If two or more previous configs, try a config inbetween the two fastest
  // configs
  else {

    routine_config fastest = current_config, second_fastest = current_config,
                   smallest = current_config;
    kmp_real64 fastest_time = DBL_MAX, second_fastest_time = DBL_MAX;

    for (const auto &entry : execution_history) {
      // Select the slowest time among all NUMA nodes in a certain config
      // MAYBE CHANGE THIS TO THE AVERAGE EXEC TIME OF ALL NODES?
      kmp_real64 exec_time = calcSlowestNUMAExec(entry.first);

      // Find fastest time among all configs
      if (exec_time < fastest_time) {

        second_fastest = fastest;
        second_fastest_time = fastest_time;
        fastest = entry.first;
        fastest_time = exec_time;

      } else if (exec_time < second_fastest_time) {

        second_fastest = entry.first;
        second_fastest_time = exec_time;
      }

      // Find smallest
      if (entry.first.num_threads < smallest.num_threads)
        smallest = entry.first;
    }
    KMP_DEBUG_ASSERT(fastest_time < DBL_MAX);
    KMP_DEBUG_ASSERT(second_fastest_time < DBL_MAX);

    KA_TRACE(3, ("\nRoutine::getNextConfig():"
                 " Comparing old configs for routine %p. \n"
                 "Fastest config={%d, %d, %d} execT=%f, "
                 " Second fastest={%d, %d, %d} execT=%f.\n",
                 routine_id, fastest.num_threads, fastest.num_tasks,
                 static_cast<int>(fastest.task_affinity), fastest_time,
                 second_fastest.num_threads, second_fastest.num_tasks,
                 static_cast<int>(second_fastest.task_affinity),
                 second_fastest_time));

    kmp_int64 diff_threads =
        getDiff(fastest.num_threads, second_fastest.num_threads);
    kmp_int64 next_num_threads =
        getMin(fastest.num_threads, second_fastest.num_threads) +
        diff_threads / 2;

    // Check if the smallest config is fastest.
    // In this case, schedule an even smaller config if possible.
    if (smallest.num_threads == fastest.num_threads &&
        fastest.num_threads > MOLDABILITY_GRANULARITY) {

      next_config.num_threads = fastest.num_threads - MOLDABILITY_GRANULARITY;
    }

    // Check if a local minima has been found.
    // In this case, select the fastest config.
    else if (diff_threads <= MOLDABILITY_GRANULARITY ||
             current_config.num_threads == next_num_threads) {

      minima_found = true;
      next_config = fastest;

      KA_TRACE(3, ("Routine::getNextConfig(): Minima found!"
                   " Fastest config selected.\n"))

// Check if load balancing is required.
#ifdef LOADBALANCE
      next_config.task_affinity = checkLoadBalance(next_config);
#endif
    }

    // Select the config inbetween the fastest and
    // second fastest config.
    else {

      next_config.num_threads = next_num_threads;

      KA_TRACE(3, ("Routine::getNextConfig(): Selecting new config"
                   " based on thread diff: %d, new number of threads: %d "
                   "(min:%d + diff/2:%d).\n",
                   diff_threads, next_config.num_threads,
                   getMin(fastest.num_threads, second_fastest.num_threads),
                   diff_threads / 2));
    }
  }

  // For now, always set numer of task according to default heuristic
  next_config.num_tasks = next_config.num_threads * 10;

  // Determine NUMA node placement
  next_config.node_mask =
      getNUMAMask(next_config.num_threads / MOLDABILITY_GRANULARITY);

  KA_TRACE(1,
           ("Routine::getNextConfig(): Routine %p was given node mask: %d .\n",
            routine_id, next_config.node_mask));

  // Update current config
  current_config = next_config;

  return next_config;
}

// This method stores the latest taskloop execution
//
// NOTE: The method relies on the fact that the config used
// for the execution is stored in the current_config variable
void Routine::storeExecution(routine_stats_nodes stats) {
  kmp_uint16 mask = current_config.node_mask;

  // Make sure only active NUMA nodes have reported stats, remove all other
  // stats.
  /*   for (int i = 0; i < NUM_NUMANODES; i++) {
      if (!((mask >> i) & 1U) && (stats[i].execution_time != 0)) {
        stats[i] = {0, 0};
        KA_TRACE(1, ("Routine::storeExecution(): Stats removed for node[%d] "
                     "(routine %p)\n",
                     i, routine_id));
      }
    } */

  // If config doesnt exists, just add the config and stats
  if (execution_history.find(current_config) == execution_history.end()) {
    execution_history.emplace(current_config, stats);

    KA_TRACE(1,
             ("Routine:storeExecution: routine %p inserted new config={%d, "
              "%d, %d}\n",
              routine_id, current_config.num_threads, current_config.num_tasks,
              static_cast<int>(current_config.task_affinity)));

    printStatsArray(stats, NUM_NUMANODES);
    return;
  }

  KA_TRACE(1, ("Routine:storeExecution: routine %p has new stats for "
               "config={%d, %d, %d}.\n",
               routine_id, current_config.num_threads, current_config.num_tasks,
               static_cast<int>(current_config.task_affinity)));

  printStatsArray(stats, NUM_NUMANODES);

  // For now, we just overwrite the stats with latest run
  for (int i = 0; i < NUM_NUMANODES; i++) {
    execution_history.at(current_config)[i] = stats[i];
  }
}

routine_config Routine::getFastestConfig() {
  routine_config best_config;

  kmp_real64 best_time = DBL_MAX;
  for (auto const &entry : execution_history) {
    kmp_real64 exec_time = calcSlowestNUMAExec(entry.first);
    if (exec_time < best_time) {
      best_time = exec_time;
      best_config = entry.first;
    }
  }

  KMP_DEBUG_ASSERT(best_time < DBL_MAX);

  return best_config;
}

routine_config Routine::getInterTaskloopConfig(kmp_int8 num_threads) {
  routine_config config = current_config;
  config.num_threads = num_threads;
  config.num_tasks = num_threads * 10;
  return config;
}

// Calculates the slowest execution time among all NUMA nodes for a certain
// config
kmp_real64 Routine::calcSlowestNUMAExec(routine_config config) {

  kmp_real64 slowest = 0;

  for (int i = 0; i < NUM_NUMANODES; i++) {

    kmp_real64 exec_time = execution_history.at(config)[i].execution_time;

    // Find slowest
    if (exec_time > slowest) {
      slowest = exec_time;
    }
  }

  KMP_DEBUG_ASSERT(slowest > 0);
  return slowest;
}

// Calculates the fastest execution time among all NUMA nodes for a certain
// config
kmp_real64 Routine::calcFastestNUMAExec(routine_config config) {

  kmp_real64 fastest = DBL_MAX;

  for (int i = 0; i < NUM_NUMANODES; i++) {

    kmp_real64 exec_time = execution_history.at(config)[i].execution_time;

    // Find fastest
    if (exec_time < fastest && exec_time != 0) {
      fastest = exec_time;
    }
  }

  KMP_DEBUG_ASSERT(fastest < DBL_MAX);
  return fastest;
}

// Calculates the average execution time among all NUMA nodes for a certain
// config
kmp_real64 Routine::calcAverageNUMAExec(routine_config config) {

  kmp_real64 avrg_time = 0.0;
  int nodes = config.num_threads / NUMANODE_SIZE;
  KMP_DEBUG_ASSERT(nodes);

  for (int i = 0; i < NUM_NUMANODES; i++) {

    avrg_time += execution_history.at(config)[i].execution_time;
  }

  avrg_time = avrg_time / nodes;

  return avrg_time;
}

// This function will return the NUMA node maskbased on fastest execution time.
kmp_uint16 Routine::getNUMAMask(kmp_uint64 num_nodes) {

  routine_config config = getFastestConfig();

  kmp_uint16 mask = 0;
  int max_node_id = NUM_NUMANODES - 1;

  // Find the fastest node
  kmp_real64 fastest_time = DBL_MAX;
  kmp_uint16 fastest_node = 0;
  for (int i = 0; i < max_node_id; i++) {

    kmp_real64 exec_time = execution_history.at(config)[i].execution_time;

    if (exec_time < fastest_time && exec_time != 0) {
      fastest_time = exec_time;
      fastest_node = i;
    }
  }

  KMP_DEBUG_ASSERT(fastest_time < DBL_MAX);
  KMP_DEBUG_ASSERT(fastest_node <= max_node_id);

  kmp_uint8 node_offset = (fastest_node / SOCKET_SIZE) * SOCKET_SIZE;
  mask = calcMask(node_offset, num_nodes, NUM_NUMANODES);

  kmp_uint16 max_val = (1ULL << NUM_NUMANODES) - 1;
  KMP_DEBUG_ASSERT(mask <= max_val);

  return mask;
}

StealPolicy Routine::checkLoadBalance(routine_config config) {
  StealPolicy policy = StealPolicy::NUMA;

  kmp_real64 slowest = 0.0;
  kmp_real64 fastest = DBL_MAX;

  for (int i = 0; i < NUM_NUMANODES; i++) {

    kmp_real64 exec_time = execution_history.at(config)[i].execution_time;

    // Find slowest
    if (exec_time > slowest) {
      slowest = exec_time;
    }
    // Find fastest
    if (exec_time < fastest && exec_time != 0) {
      fastest = exec_time;
    }
  }

  kmp_real64 diff = slowest / fastest;

  if (diff >= LOAD_BALANCE_REQUIRED_FACTOR)
    policy = StealPolicy::FULL;

  KA_TRACE(2, ("Routine::checkLoadbalance(): Policy %d selected for routine "
               "%p. Fastest:%f, "
               "Slowest:%f, diff:%f\n",
               policy, routine_id, fastest, slowest, diff))

  return policy;
}
