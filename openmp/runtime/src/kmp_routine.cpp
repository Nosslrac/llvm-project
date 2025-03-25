#include "kmp_routine.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_schedule.h"
#include <cfloat>
#include <climits>

namespace {
routine_config UNDEFINED_CONFIG = {-1, -1, AffinityPolicy::NONE};

kmp_int64 getDiff(kmp_int64 a, kmp_int64 b) { return a > b ? a - b : b - a; }

kmp_int64 getMin(kmp_int64 a, kmp_int64 b) { return a < b ? a : b; }

} // namespace

inline bool operator==(const routine_config &lhs, const routine_config &rhs) {
  return lhs.num_threads == rhs.num_threads && lhs.num_tasks == rhs.num_tasks &&
         lhs.task_affinity == rhs.task_affinity;
}

// Class constructor
Routine::Routine(kmp_int64 id)
    : routine_id(id), current_config(UNDEFINED_CONFIG), minima_found(false) {}

routine_config Routine::getCurrentConfig() { return current_config; }

routine_config Routine::getDefaultConfig(kmp_info *thread,
                                         kmp_int64 num_tasks) {
  routine_config config;
  config.num_threads = thread->th.th_team->t.t_nproc;
  config.num_tasks = num_tasks;
  config.task_affinity = AffinityPolicy::NUMA_STRICT;

  // Update current config
  current_config = config;

  return config;
}

// Exploration of possible configs using binary search (kind of)
// based on execution history. When a local minima is found,
// always return the fastest config.
//
// This method only considers moldability as of now...
// TODO: Add analyzis of metrics for changing the config affinity.
routine_config Routine::getNextConfig() {
  routine_config next_config = current_config;

  // If minima found, return the fastest config
  if (minima_found) {
    next_config = getFastestConfig(0);
  }

  // If only one previous config, try half the number of threads
  else if (execution_history.size() < 2) {

    next_config.num_threads = current_config.num_threads / 2;

    // For now, let the num tasks scale with the number of threads
    next_config.num_tasks = current_config.num_tasks / 2;

    KA_TRACE(
        1, ("Routine::getNextConfig(): Only one previous config."
            " Try half the number of threads (%d threads/2) for routine %p .\n",
            current_config.num_threads, routine_id));

  }

  // If two or more previous configs, try a config inbetween the two fastest
  // configs
  else {

    // Find the two fastest configs
    routine_config fastest = current_config, second_fastest = current_config,
                   smallest = current_config;
    kmp_real64 fastest_time = DBL_MAX, second_fastest_time = DBL_MAX;

    for (const auto &entry : execution_history) {

      // Find fastest
      if (entry.second.execution_time < fastest_time) {

        second_fastest = fastest;
        second_fastest_time = fastest_time;
        fastest = entry.first;
        fastest_time = entry.second.execution_time;

      } else if (entry.second.execution_time < second_fastest_time) {

        second_fastest = entry.first;
        second_fastest_time = entry.second.execution_time;
      }

      // Find smallest
      if (entry.first.num_threads < smallest.num_threads)
        smallest = entry.first;
    }
    KMP_DEBUG_ASSERT(fastest_time < DBL_MAX);
    KMP_DEBUG_ASSERT(second_fastest_time < DBL_MAX);

    KA_TRACE(1, ("\nRoutine::getNextConfig():"
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
      // For now, select num_tasks = threads*10 as default, change later ofc
      // maybe???
      next_config.num_tasks = next_config.num_threads * 10;
    }

    // Check if a local minima has been found.
    // In this case, select the fastest config.
    else if (diff_threads <= MOLDABILITY_GRANULARITY ||
             current_config.num_threads == next_num_threads) {

      minima_found = true;
      next_config = fastest;
      KA_TRACE(1, ("Routine::getNextConfig(): Minima found!"
                   " Fastest config selected.\n"))
    }

    // Select the config inbetween the fastest and
    // second fastest config.
    else {

      next_config.num_threads = next_num_threads;
      // For now, select num_tasks = threads*10 as default, change later ofc
      // maybe???
      next_config.num_tasks = next_config.num_threads * 10;

      KA_TRACE(1, ("Routine::getNextConfig(): Selecting new config"
                   " based on thread diff: %d, new number of threads: %d "
                   "(min:%d + diff/2:%d).\n",
                   diff_threads, next_config.num_threads,
                   getMin(fastest.num_threads, second_fastest.num_threads),
                   diff_threads / 2));
    }
  }

  // Update current config
  current_config = next_config;

  return next_config;
}

// This method stores the latest taskloop execution
//
// NOTE: The method relies on the fact that the config used
// for the execution is stored in the current_config variable
void Routine::storeExecution(routine_stats stats) {

  // If config doesnt exists, just add the config and stats
  if (execution_history.find(current_config) == execution_history.end()) {
    execution_history.emplace(current_config, stats);

    KA_TRACE(
        1,
        ("\nRoutine:storeExecution: routine %p inserted new config={%d, %d, %d}"
         "\nwith the stats={ExecT=%f, StallRatio=%f}\n",
         routine_id, current_config.num_threads, current_config.num_tasks,
         static_cast<int>(current_config.task_affinity), stats.execution_time,
         stats.stall_ratio));
    return;
  }

  KA_TRACE(1, ("Routine:storeExecution: routine %p has new stats for "
               "config={%d, %d, %d}.\n"
               "Old stats={ExecT=%f, StallRatio=%f}, New stats={ExecT=%f, "
               "StallRatio=%f}\n",
               routine_id, current_config.num_threads, current_config.num_tasks,
               static_cast<int>(current_config.task_affinity),
               execution_history.at(current_config).execution_time,
               execution_history.at(current_config).stall_ratio,
               stats.execution_time, stats.stall_ratio));

  // For now, we select the fastest stats
  // for the config. In future, we might
  // do cumulative average etc.

  if (execution_history.at(current_config).execution_time >
      stats.execution_time)
    execution_history.at(current_config) = stats;

  /*
  // Cumulative average is used to combine stats
  // from multiple executions.
  // N = 0.5 used.
  kmp_real64 N = 0.5;
  kmp_real64 exec_time = execution_history.at(current_config).execution_time;
  kmp_real64 stall_ratio = execution_history.at(current_config).stall_ratio;

  stats.execution_time = (1-N)*exec_time + N*stats.execution_time;
  stats.stall_ratio = (1-N)*stall_ratio + N*stats.stall_ratio;

  execution_history.at(current_config) = stats; */
}

routine_config Routine::getFastestConfig(int val) {
  routine_config best_config;

  switch (val) {
  case 0: // Look for lowest execution time
  {
    kmp_real64 best_time = DBL_MAX;
    for (auto const &entry : execution_history) {
      if (entry.second.execution_time < best_time) {
        best_time = entry.second.execution_time;
        best_config = entry.first;
      }
    }

    KMP_DEBUG_ASSERT(best_time < DBL_MAX);
    break;
  }

  default:
    break;
  }

  return best_config;
}