#include "kmp_routine.h"
#include "kmp.h"
#include "kmp_config.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_schedule.h"
#include "kmp_topo.h"
#include <algorithm>
#include <bitset>
#include <cfloat>
#include <climits>
#include <cmath>

namespace {
routine_config UNDEFINED_CONFIG = {-1, -1, 0, StealPolicy::NUMA};

kmp_int64 getDiff(kmp_int64 a, kmp_int64 b) { return a > b ? a - b : b - a; }

kmp_int64 getMin(kmp_int64 a, kmp_int64 b) { return a < b ? a : b; }

void printStatsArray(routine_stats_nodes arr) {
  auto i = 0;
  for (const auto &stat : arr) {
    KA_TRACE(1, ("Node[%d] ExecT:%f \n", i, stat.execution_time));
    i++;
  }
}

} // namespace

inline bool operator==(const routine_config &lhs, const routine_config &rhs) {
  return lhs.num_threads == rhs.num_threads && lhs.num_tasks == rhs.num_tasks &&
         lhs.node_mask == rhs.node_mask && lhs.steal_policy == rhs.steal_policy;
}

///
/// @brief This constructor creates a routine object storing information
/// about a taskloop with ID routine_id (memory address of taskloop i.e.
/// routine_entry).
///
Routine::Routine(kmp_int64 routine_id, kmp_uint32 nthreads, kmp_uint64 ub)
    : m_routine_id(routine_id), m_upper_bound(ub),
      m_current_config(getInitialConfig(nthreads)),
      m_1stfastest(UNDEFINED_CONFIG), m_2ndfastest(UNDEFINED_CONFIG),
      m_search_finished(false), m_iteration_count(0),
      MOLDABILITY_GRANULARITY(Topo::numa_topology.get_numa_size()) {}

///
/// @brief Get a read-only reference of the current config
/// for the routine.
///
const routine_config &Routine::getCurrentConfig() const {
  return m_current_config;
}

///
/// @brief Sets up and returns the initial config. This config
/// is used for the first iteration of the taskloop.
///
routine_config Routine::getInitialConfig(kmp_uint32 nthreads) {
  routine_config config;
  config.num_threads = nthreads;
  config.num_tasks = nthreads * 10;
  config.node_mask =
      static_cast<kmp_uint16>((1U << Topo::numa_topology.get_num_numa()) - 1);
  config.steal_policy = StealPolicy::NUMA;

  return config;
}

///
/// @brief Check if binary search is possible.
///
void Routine::initBinarySearch() {

  if (m_current_config.num_threads >= MOLDABILITY_GRANULARITY * 2) {

    m_current_config.num_threads = m_current_config.num_threads / 2;
    KA_TRACE(
        1, ("Routine::initBinarySearch(): Only one previous config."
            " Try half the number of threads (%d threads/2) for routine %p .\n",
            m_current_config.num_threads, m_routine_id));
    return;
  }

  KA_TRACE(1, ("Routine::initBinarySearch(): Binary search not possible: To "
               "few NUMA nodes\n"));
  m_search_finished = true;
  m_1stfastest = m_current_config;
}

///
/// @brief This function selects the number of threads used in the
/// next config, by using binary search.
///
void Routine::binarySearch() {

  kmp_real64 fastest1st_time = calcSlowestNUMAExec(m_1stfastest);
  kmp_real64 fastest2nd_time = calcSlowestNUMAExec(m_2ndfastest);

  KA_TRACE(3, ("\nRoutine::binarySearch():"
               " Comparing old configs for routine %p. \n"
               "Fastest config={%d, %d, %d} execT=%f, "
               " Second fastest={%d, %d, %d} execT=%f.\n",
               m_routine_id, m_1stfastest.num_threads, m_1stfastest.num_tasks,
               static_cast<int>(m_1stfastest.steal_policy), fastest1st_time,
               m_2ndfastest.num_threads, m_2ndfastest.num_tasks,
               static_cast<int>(m_2ndfastest.steal_policy), fastest2nd_time));

  const kmp_int64 diff_threads =
      getDiff(m_1stfastest.num_threads, m_2ndfastest.num_threads);
  const kmp_int64 next_num_threads =
      getMin(m_1stfastest.num_threads, m_2ndfastest.num_threads) +
      (((diff_threads / 2) / MOLDABILITY_GRANULARITY) *
       MOLDABILITY_GRANULARITY);

  // Check if the smallest config is fastest.
  // In this case, select the smallest number of threads
  /*   if (m_iteration_count == 3 &&
        m_1stfastest.num_threads < m_2ndfastest.num_threads) {
      if (m_current_config.num_threads == MOLDABILITY_GRANULARITY) {
        m_search_finished = true;
      }

      m_current_config.num_threads = MOLDABILITY_GRANULARITY;
    } */

  // Check if a local minima has been found.
  // In this case, select the fastest config.
  if (diff_threads <= MOLDABILITY_GRANULARITY) {

    m_search_finished = true;
    m_current_config = m_1stfastest;

    KA_TRACE(
        3,
        ("Routine::binarySearch(): Search finished. Select fastest config.\n"));
  }

  // Select the config inbetween the fastest and
  // second fastest config.
  else {
    if (m_current_config.num_threads == next_num_threads) {
      m_search_finished = true;
      m_current_config = m_1stfastest;
    } else {
      m_current_config.num_threads = next_num_threads;
    }

    KA_TRACE(3, ("Routine::binarySearch(): Selecting new config"
                 " based on thread diff: %d, new number of threads: %d.\n",
                 diff_threads, m_current_config.num_threads));
  }
}

void Routine::predictiveEstimation() {

  kmp_real64 IPC_max = 0.0;
  kmp_real64 IPC_min = DBL_MAX;

  const auto &stats = m_execution_history.at(m_current_config);
  for (const auto &stat : stats) {
    if (stat.IPC == 0.0 || std::isnan(stat.IPC)) {
      continue;
    }
    IPC_max = std::max(IPC_max, stat.IPC);
    IPC_min = std::min(IPC_min, stat.IPC);
  }

  KMP_DEBUG_ASSERT(IPC_max > 0.0);
  KMP_DEBUG_ASSERT(IPC_min < DBL_MAX);

  // Kernel of the prediction modell
  kmp_real64 IPC_diff = IPC_max / IPC_min;
  kmp_uint32 num_nodes =
      (((Topo::numa_topology.get_num_numa() / IPC_diff) * 2) /
       Topo::numa_topology.get_num_numa()) *
      Topo::numa_topology.get_num_numa();

  num_nodes = std::clamp(num_nodes, 1U, Topo::numa_topology.get_num_numa() - 1);

  m_current_config.num_threads =
      num_nodes * Topo::numa_topology.get_numa_size();

  KA_TRACE(
      1, ("Routine::predictiveEstimation(): Routine %p: IPC max: %f, IPC min: "
          "%f, IPC diff: %f, num_nodes: %d.\n",
          m_routine_id, IPC_max, IPC_min, IPC_diff, num_nodes));
}

// Try the surronding configs of the fastest
void Routine::predictiveModel() {

  auto ncores = m_1stfastest.num_threads + MOLDABILITY_GRANULARITY;
  auto elem = std::find_if(
      m_execution_history.begin(), m_execution_history.end(),
      [ncores](const auto &kv) { return kv.first.num_threads == ncores; });

  // Try a larger config if it has not been tried before
  if (elem == m_execution_history.end() &&
      ncores <= Topo::numa_topology.get_num_cores()) {
    m_current_config = m_1stfastest;
    m_current_config.num_threads = ncores;
    KA_TRACE(1, ("Routine::predictiveModel(): Routine %p trying a larger "
                 "config! Ncores: %d.\n",
                 m_routine_id, ncores));
    return;
  }

  ncores = m_1stfastest.num_threads - MOLDABILITY_GRANULARITY;
  elem = std::find_if(
      m_execution_history.begin(), m_execution_history.end(),
      [ncores](const auto &kv) { return kv.first.num_threads == ncores; });

  // Try a smaller config if it has not been tried before
  if (elem == m_execution_history.end() && ncores >= MOLDABILITY_GRANULARITY) {
    m_current_config = m_1stfastest;
    m_current_config.num_threads = ncores;
    KA_TRACE(1, ("Routine::predictiveModel(): Routine %p trying a smaller "
                 "config! Ncores: %d.\n",
                 m_routine_id, ncores));
    return;
  }

  // Otherwise, search finished
  m_search_finished = true;
  m_current_config = m_1stfastest;
  KA_TRACE(
      1,
      ("Routine::predictiveModel(): Routine %p finished search! Select fastest "
       "Ncores: %d.\n",
       m_routine_id, m_current_config.num_threads));
}

///
/// @brief This function select the next config used for executing a taskloop.
/// This includes the moldability algorithm, load balance calculations and
/// setting the node_mask.
///
/// For a certain taskloop, the next config will be selected accordingly:
/// 1. First & second iteration, execute on maxium number of nodes.
/// 2. Third iteration, check if the execution time is below the threshold.
///    In this case, execute on only one node. Otherwise, use binary search to
///    find optimal number of threads.
/// 3. The binary search will continue until the fastest config is found. This
///    config will be used for the rest of the execution.
/// 4. Check if load balancing is required for the fastest config, to
///    determine if the stealing policy needs to be changed.
const routine_config &Routine::getNextConfig(kmp_uint64 ub) {
  m_iteration_count++;
  bool current_search_state = m_search_finished;

#ifndef MOLDABILITY
#ifdef LOADBALANCE
  m_current_config.steal_policy = checkLoadBalance();
#endif
  return m_current_config;
#endif
  if (m_current_config.num_threads == 1) {
    m_search_finished = true;
  } else if (ub != m_upper_bound) {
    // Taskloop size is not constant, abort moldability

    m_current_config.num_threads = Topo::numa_topology.get_num_cores();
    m_current_config.num_tasks = m_current_config.num_threads * 10;
    m_current_config.node_mask =
        static_cast<kmp_uint16>((1U << Topo::numa_topology.get_num_numa()) - 1);
    m_current_config.steal_policy = StealPolicy::NUMA;

    if (m_upper_bound != 0) {
      KA_TRACE(1, ("Routine:getNextConfig(): Taskloop size is not constant, "
                   "moldability aborted!\n"));
    }

    m_upper_bound = 0;
    m_search_finished = true;
    return m_current_config;
  } else if (m_iteration_count == 1) {
    // Do nothing, just keep executing the current config
  } else if (m_search_finished) {
    // Keep running the fastest config
    m_current_config = m_1stfastest;
  } else if (m_iteration_count == 2) {
    if (calcFastestNUMAExec(m_current_config) >=
        TASKLOOP_PERF_COUNTERS_VALID_TIME) {
      predictiveEstimation();
    } else {
      KA_TRACE(1, ("Routine::getNextConfig(): Routine %p is a tiny taskloop! "
                   "Select minimal config!\n",
                   m_routine_id));
      m_current_config.num_threads = MOLDABILITY_GRANULARITY;
      m_search_finished = true;
    }
  }

  // If two or more previous configs, try a config inbetween the two fastest
  // configs
  else {
    predictiveModel();
    // binarySearch();
  }

  // For now, always set numer of task according to default OpenMP heuristic
  m_current_config.num_tasks = m_current_config.num_threads * 10;

  // Determine NUMA node placement
  m_current_config.node_mask = getNUMAMask();

  // Check if load balancing is required.
#ifdef LOADBALANCE
  if (m_search_finished == true && current_search_state == false &&
      (m_current_config.num_threads != MOLDABILITY_GRANULARITY)) {
    m_current_config.steal_policy = checkLoadBalance();
    KA_TRACE(1, ("Routine::getNextConfig(): Iteration counts for Routine %p to "
                 "finish search: %d.\n",
                 m_routine_id, m_iteration_count));
  }
#endif

  KA_TRACE(1, ("Routine::getNextConfig(): Routine %p was given config: "
               "{nthreads=%d, ntasks=%d, mask=%s, steal_policy=%d} .\n",
               m_routine_id, m_current_config.num_threads,
               m_current_config.num_tasks,
               std::bitset<16>(m_current_config.node_mask).to_string().c_str(),
               m_current_config.steal_policy));

  // Update current config
  return m_current_config;
}

// This method stores the latest taskloop execution
//
// NOTE: The method relies on the fact that the config used
// for the execution is stored in the current_config variable
void Routine::storeExecution(routine_stats_nodes stats) {
  if (m_iteration_count == 0) {
    KA_TRACE(1, ("Routine::storeExecution(): First execution discarded "
                 "(routine %p)\n",
                 m_routine_id));
    return;
  }

  kmp_uint16 mask = m_current_config.node_mask;

  // Make sure only active NUMA nodes have reported stats, remove all other
  // stats.
  auto i = 0;
  for (const auto &stat : stats) {
    if (((1U << i) & mask) == 0 && (stat.execution_time != 0)) {
      auto tmp = std::bitset<16>(m_current_config.node_mask);
      KA_TRACE(1, ("Routine::storeExecution(): Node mask = 0b%s. Stat non zero "
                   "for node [%d] "
                   "(routine %p)\n",
                   tmp.to_string().c_str(), i, m_routine_id));
      KMP_DEBUG_ASSERT(false);
    }
    i++;
  }

  // If config doesnt exists, just add the config and stats
  if (m_execution_history.find(m_current_config) == m_execution_history.end()) {
    m_execution_history.emplace(m_current_config, stats);

    KA_TRACE(1, ("Routine:storeExecution: routine %p inserted new config={%d, "
                 "%d, %d}\n",
                 m_routine_id, m_current_config.num_threads,
                 m_current_config.num_tasks,
                 static_cast<int>(m_current_config.steal_policy)));

  } else {

    KA_TRACE(1, ("Routine:storeExecution: routine %p has new stats for "
                 "config={%d, %d, %d}.\n",
                 m_routine_id, m_current_config.num_threads,
                 m_current_config.num_tasks,
                 static_cast<int>(m_current_config.steal_policy)));
    // For now, we just overwrite the stats with latest run
    m_execution_history.at(m_current_config) = stats;
    KMP_DEBUG_ASSERT(m_search_finished);
  }
  printStatsArray(stats);

  if (m_1stfastest.num_threads == -1 ||
      isXFasterThanY(m_current_config, m_1stfastest)) {
    KMP_DEBUG_ASSERT(!(m_current_config == m_1stfastest));
    KA_TRACE(1, ("Routine::storeExecution: Updating fastest configs\n"));
    m_2ndfastest = m_1stfastest;
    m_1stfastest = m_current_config;
  } else if (m_2ndfastest.num_threads == -1 ||
             isXFasterThanY(m_current_config, m_2ndfastest)) {
    KMP_DEBUG_ASSERT(!(m_current_config == m_2ndfastest));
    KA_TRACE(1, ("Routine::storeExecution: Updating 2nd fastest config\n"));
    m_2ndfastest = m_current_config;
  }
}
///
/// @brief Calculates the slowest execution time among all NUMA nodes for a
/// certain config
///
kmp_real64 Routine::calcSlowestNUMAExec(const routine_config &config) {

  kmp_real64 slowest = 0.0;
  const auto &stats = m_execution_history.at(config);
  for (const auto &stat : stats) {
    slowest = std::max(slowest, stat.execution_time);
  }

  KMP_DEBUG_ASSERT(slowest > 0.0);
  return slowest;
}

///
/// @brief Calculates the fastest execution time among all NUMA nodes for a
/// certain config
///
kmp_real64 Routine::calcFastestNUMAExec(const routine_config &config) {

  kmp_real64 fastest = DBL_MAX;
  const auto &stats = m_execution_history.at(config);
  for (const auto &stat : stats) {
    if (stat.execution_time == 0.0) {
      continue;
    }
    fastest = std::min(fastest, stat.execution_time);
  }

  KMP_DEBUG_ASSERT(fastest < DBL_MAX);
  KMP_DEBUG_ASSERT(fastest > 0.0);

  return fastest;
}

inline kmp_uint32 pext(kmp_uint64 BB, kmp_uint64 mask) {
  return static_cast<kmp_uint32>(__builtin_ia32_pext_di(BB, mask));
}

///
/// @brief This function will return the NUMA node mask for the current config.
/// @note This function assumes that m_current_config does not
/// change before next taskloop execution.
///
kmp_uint16 Routine::getNUMAMask() const {
  if (m_iteration_count == 1 || m_current_config.num_threads == 1) {
    return m_current_config.node_mask;
  }
  const auto NODE_PER_SOCKET =
      Topo::numa_topology.get_num_numa() / Topo::numa_topology.get_num_socket();
  const auto NUMA_SIZE = Topo::numa_topology.get_numa_size();
  const auto ncores = Topo::numa_topology.get_num_cores();
  auto elem = std::find_if(
      m_execution_history.begin(), m_execution_history.end(),
      [ncores](const auto &kv) { return kv.first.num_threads == ncores; });
  KMP_DEBUG_ASSERT(elem != m_execution_history.end());

  const auto &stats = elem->second;
  auto min_iter =
      std::min_element(stats.begin(), stats.end(),
                       [](const routine_stats &lhs, const routine_stats &rhs) {
                         return lhs.execution_time < rhs.execution_time;
                       });
  auto index = std::distance(stats.begin(), min_iter);
  KA_TRACE(1, ("Routine::getNUMAMask(): Fastest index = %d\n", index));

  auto numaCount = (m_current_config.num_threads / NUMA_SIZE) - 1;
  kmp_uint16 mask = static_cast<kmp_uint16>(1U << index);
  index = (index / NODE_PER_SOCKET) * NODE_PER_SOCKET;
  while (numaCount > 0) {
    while (((1U << index) & mask) != 0) {
      index = (index + 1) % Topo::numa_topology.get_num_numa();
    }
    mask |= 1U << index;
    numaCount--;
  }

  return mask;
}

///
/// @brief Decides whether load balancing is required based
/// on ratio between fastest and slowest NUMA node.
///
StealPolicy Routine::checkLoadBalance() {
  StealPolicy policy = StealPolicy::NUMA;

  kmp_real64 slowest = 0.0;
  kmp_real64 fastest = DBL_MAX;

  for (const auto &stat : m_execution_history.at(m_current_config)) {
    kmp_real64 exec_time = stat.execution_time;
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

  KA_TRACE(1, ("Routine::checkLoadbalance(): Policy %d selected for routine "
               "%p. Fastest:%f, "
               "Slowest:%f, diff:%f\n",
               policy, m_routine_id, fastest, slowest, diff))

  return policy;
}
