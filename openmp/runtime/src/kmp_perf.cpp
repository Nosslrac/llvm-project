#include "kmp_perf.h"
#include "kmp.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_perf_objects.h"
#include "kmp_topo.h"

#include <asm/unistd_64.h>
#include <cstdint>
#include <limits>
#include <sched.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <vector>

namespace {
inline constexpr int perf_id(PerfEvents event) {
  return static_cast<int>(event);
}

inline kmp_real64 frac(uint64_t numerator, uint64_t denominator) {
  return static_cast<kmp_real64>(numerator) /
         static_cast<kmp_real64>(denominator);
}

template <PerfEvents ev> inline constexpr int perf_event() {
  if constexpr (ev == PerfEvents::TOT_CYCLES)
    return PERF_COUNT_HW_CPU_CYCLES;
  if constexpr (ev == PerfEvents::TOT_INSTRUCTIONS)
    return PERF_COUNT_HW_INSTRUCTIONS;
}

const char *enumToString(PerfEvents event) {
  switch (event) {
  case PerfEvents::TOT_CYCLES:
    return "TOT_CYCLES";
  case PerfEvents::TOT_INSTRUCTIONS:
    return "TOT_INSTRUCTIONS";
  default:
    return "UNKNOWN";
  }
}

int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
                        int group_fd, unsigned long flags) {
  return static_cast<int32_t>(
      syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}
#ifdef PERF_COUNTERS
template <PerfEvents ev>
void init_perf_event(kmp_info_t *thread, perf_event_attr *pe, int32_t cpu_id) {
  pe->type = PERF_TYPE_HARDWARE;
  pe->config = perf_event<ev>();
  int32_t fd = perf_event_open(pe, 0, cpu_id, -1, 0);
  thread->th.perf_stats[perf_id(ev)] = fd;

  if (fd == -1) {
    KA_TRACE(
        1,
        ("%s:%d: __kmp_init_perf_event(ERROR): #T%d = CPU#%d: Cannot open %s\n",
         __FILE_NAME__, __LINE__, __kmp_gtid_from_thread(thread), cpu_id,
         enumToString(ev)));
    perror("Reason: ");
    return;
  }

  KA_TRACE(5, ("%s:%d: __kmp_init_perf_event: (E#%d, FD#%d, T#%d, CPU#%d).\n",
               __FILE_NAME__, __LINE__, ev, fd, __kmp_gtid_from_thread(thread),
               cpu_id));
}

template <PerfEvents ev> void enable_perf_event(kmp_info_t *thread) {
  int32_t cpu_id = sched_getcpu();
  kmp_int64 gtid = __kmp_gtid_from_thread(thread);

  int32_t fd = thread->th.perf_stats[perf_id(ev)];

  KMP_DEBUG_ASSERT(fd > 2);

  ioctl(fd, PERF_EVENT_IOC_RESET);

  uint64_t counter = 0;
  if (read(fd, &counter, sizeof(uint64_t)) == -1) {
    KA_TRACE(1, ("%s:%d: __kmp_enable_perf_event(ERROR): Reading counter for "
                 "T#%d. Read fail\n",
                 __FILE_NAME__, __LINE__, gtid));
    perror("Reason: ");
    close(fd);
    return;
  }

  KA_TRACE(5, ("%s:%d: __kmp_enable_perf_event: (E#%d, FD#%d, T#%d, CPU#%d)\n",
               __FILE_NAME__, __LINE__, ev, fd, gtid, cpu_id));

  ioctl(fd, PERF_EVENT_IOC_ENABLE);
}

template <PerfEvents ev>
uint64_t stop_perf_event(kmp_info_t *thread, int32_t cpu_id) {
  // Stop event and read
  const int fd = thread->th.perf_stats[perf_id(ev)];

  KMP_DEBUG_ASSERT(fd > 2);

  ioctl(fd, PERF_EVENT_IOC_DISABLE);

  uint64_t counter = 0;
  if (read(fd, &counter, sizeof(uint64_t)) == -1) {
    KA_TRACE(1, ("%s:%d: __kmp_stop_perf_event(ERROR): Reading counter for "
                 "CPU#%d. Read fail\n",
                 __FILE_NAME__, __LINE__, cpu_id));
    perror("Reason: ");
    close(fd);
    thread->th.perf_stats[perf_id(ev)] = -1;
    return 0;
  }

  KA_TRACE(
      5, ("%s:%d: __kmp_stop_perf_event: (E#%d, FD#%d, T#%d, CPU#%d, Val=%d)\n",
          __FILE_NAME__, __LINE__, ev, fd, __kmp_gtid_from_thread(thread),
          cpu_id, counter));

  thread->th.perf_accum[perf_id(ev)] += counter;
  return counter;
}

template <PerfEvents ev> void disable_perf_event(kmp_info_t *thread) {
  int32_t cpu_id = sched_getcpu();

  // Disable event
  const int fd = thread->th.perf_stats[perf_id(ev)];
  if (fd <= 2) // Counter was unavailable
  {
    return;
  }

  KA_TRACE(5, ("%s:%d: __kmp_disable_perf_event: (E#%d, FD#%d, T#%d, CPU#%d)\n",
               __FILE_NAME__, __LINE__, ev, fd, __kmp_gtid_from_thread(thread),
               cpu_id));

  thread->th.perf_stats[perf_id(ev)] = -1; // reset fd
  ioctl(fd, PERF_EVENT_IOC_DISABLE);
  close(fd);
}
} // namespace

///
/// @brief This function initialized the perf conters for a specific thread and
/// CPU core, by opening all perf event file descriptors. This function should
/// only be called once per thread.
//
void Perf::__kmp_init_counters(kmp_info_t *thread, int32_t gtid) {
  int32_t cpu_id = sched_getcpu();

#ifdef AMD_PERF
  thread->th.perf_container = RawAMDPerfContainer(cpu_id, gtid);
  thread->th.perf_container.initAll();
#endif

  KA_TRACE(3, ("%s:%d: __kmp_init_counter(entered): T#%d = CPU#%d.\n ",
               __FILE_NAME__, __LINE__, gtid, cpu_id));

  // Init perf event
  perf_event_attr pe;
  memset(&pe, 0, sizeof(perf_event_attr));
  pe.size = sizeof(perf_event_attr);
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.inherit = 0;
  pe.exclude_hv = 1;

  init_perf_event<PerfEvents::TOT_CYCLES>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, &pe, cpu_id);
}

///
/// @brief This function enables all perf events for a specific thread.
///
void Perf::__kmp_start_counters(kmp_info_t *thread) {
  int32_t gtid = __kmp_gtid_from_thread(thread);
  int32_t cpu_id = sched_getcpu();

  KA_TRACE(3, ("%s:%d: __kmp_start_counter(entered): T#%d = CPU#%d.\n ",
               __FILE_NAME__, __LINE__, gtid, cpu_id));

  // Start perf counters and execution time
  __kmp_read_system_time(&thread->th.time);
  enable_perf_event<PerfEvents::TOT_CYCLES>(thread);
  enable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread);

#ifdef AMD_PERF
  thread->th.perf_container.startAll();
#endif
}

///
/// @brief This function stops and resets all perf events for a specific
/// thread.
///
void Perf::__kmp_stop_counters(kmp_info_t *thread, int32_t gtid,
                               kmp_int32 task_id) {
  int32_t cpu_id = sched_getcpu();

  uint64_t tot_cycles = stop_perf_event<PerfEvents::TOT_CYCLES>(thread, cpu_id);
  uint64_t tot_ins =
      stop_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, cpu_id);

#ifdef AMD_PERF
  AMDRawResults results = thread->th.perf_container.stopAndReadAll();
#endif

  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);
  kmp_real64 elapsed_time = current_time - thread->th.time;

  KA_TRACE(4, ("%s:%d: __kmp_stop_counters: Counters for Task %p executing "
               "routine %p on CPU#%d (T#%d):\n"
               "      - Tot cycles = %ld\n"
               "      - Tot ins = %ld\n"
#ifdef AMD_PERF
               "  # AMD raw ratios:\n"
               "      - TotDisp = %lu\n"
               "      - L1 Fills All = %lu\n"
               "      - L1 Fills Different NUMA = %lu\n"
               "      - L1 Fills same CXX = %lu\n"
               "      - L1 Fills another CXX = %lu\n"
               "      - L3 Misses = %lu\n"
               "      - Retiring fraction = %lf\n"
               "      - Backend bound = %lf\n"
               "      - Backend bound Memory = %lf\n"
               "      - Backend bound CPU = %lf\n"
#endif
               "      - Execution time = %f\n",
               __FILE_NAME__, __LINE__, task_id, thread->th.routine_id, cpu_id,
               gtid, tot_cycles, tot_ins
#ifdef AMD_PERF
               ,
               results.m_totDisp, results.m_l1All, results.m_l1DiffNuma,
               results.m_l1SameCXX, results.m_l1AnotherCXX, results.m_l3Miss,
               results.m_retiring, results.m_backend, results.m_backendMem,
               results.m_backendCPU
#endif
               ,
               elapsed_time));
}

///
/// @brief This function disables all perf events for a specific thread. This
/// function should only be called once per thread.
///
void Perf::__kmp_disable_counters(kmp_info_t *thread) {

  disable_perf_event<PerfEvents::TOT_CYCLES>(thread);
  disable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread);

#ifdef AMD_PERF
  thread->th.perf_container.disableAll();
#endif
}
#else
} // namespace
#endif

/////////////////////////////////////////////////////////////////////////////
///                 Always used by Routine class                          ///
/////////////////////////////////////////////////////////////////////////////

///
/// @brief This function summarizes the counter stats for each thread
/// and aggregates them on NUMA node granularity.
///
void __kmp_summarize_taskloop_stats(kmp_team *team,
                                    routine_stats_nodes &numaSummary,
                                    const kmp_uint32 nthreads,
                                    const kmp_uint32 numaSize,
                                    const kmp_real64 taskloop_start_time) {
  kmp_real64 stop_time = taskloop_start_time;
  kmp_real64 IPC = 0.0;
  int node_loss = 0;
  auto tasks = 0;
  auto tasks_gen = 0;

#ifdef AMD_PERF
  AMDRawResults results;
#endif

  for (kmp_uint32 i = 0; i < nthreads; i++) {
    kmp_info_t *thread = team->t.t_threads[i];
#ifdef PERF_COUNTERS
    const auto tot_ins =
        thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)];
    const auto tot_cyc = thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)];
    if (tot_cyc != 0) {
      IPC += frac(tot_ins, tot_cyc);
    } else {
      node_loss++;
    }

    tasks += thread->th.num_tasks_exec;
    tasks_gen += thread->th.num_task_gen_exec;

    // Reset thread stats
    thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)] = 0;
    thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)] = 0;
    thread->th.num_tasks_exec = 0;
    thread->th.num_task_gen_exec = 0;

#endif
    if (stop_time < thread->th.task_finish_time) {
      stop_time = thread->th.task_finish_time;
    }

#ifdef AMD_PERF
    results += thread->th.perf_container.summarizeCounters();
#endif

    if ((i + 1) % numaSize == 0 || (i + 1) == nthreads) {
      if (numaSize - node_loss == 0) {
        KA_TRACE(1,
                 ("__kmp_summarize_taskloop_stats: Node loss = numaSize!\n"));
        node_loss = 0;
      }
      numaSummary[i / numaSize].execution_time =
          stop_time - taskloop_start_time;
      numaSummary[i / numaSize].IPC = IPC / (numaSize - node_loss);

#ifdef AMD_PERF
      results.avg(numaSize - node_loss);
#endif

      KA_TRACE(1,
               ("__kmp_summarize_taskloop_stats: NUMA node %d\n"
                "      - IPC: %lf\n"
                "      - Exec time: %lf\n"
                "      - Tasks (not averaged): %d\n"
                "      - Gen Tasks (not averaged): %d\n"

#ifdef AMD_PERF
                "  # AMD raw ratios:\n"
                "      - TotDisp = %lu\n"
                "      - L1 Fills All = %lu\n"
                "      - L1 Fills Different NUMA = %lu\n"
                "      - L1 Fills same CXX = %lu\n"
                "      - L1 Fills another CXX = %lu\n"
                "      - Retiring fraction = %lf\n"
                "      - Backend bound = %lf\n"
                "      - Backend bound Memory = %lf\n"
                "      - Backend bound CPU = %lf\n"
#endif
                ,
                i / numaSize, IPC / (numaSize - node_loss),
                stop_time - taskloop_start_time, tasks, tasks_gen
#ifdef AMD_PERF
                ,
                results.m_totDisp, results.m_l1All, results.m_l1DiffNuma,
                results.m_l1SameCXX, results.m_l1AnotherCXX, results.m_retiring,
                results.m_backend, results.m_backendMem, results.m_backendCPU
#endif
                ));
      IPC = 0.0;
      stop_time = taskloop_start_time;
      tasks = 0;
      tasks_gen = 0;
      node_loss = 0;
    }
  }
}

///
/// @brief This function returns the perf metrics from
/// the aggregated perf counters.
///
void Perf::__kmp_get_taskloop_stats(kmp_team *team,
                                    routine_stats_nodes &ret_stats,
                                    const kmp_real64 taskloop_start_time) {
  uint32_t nthreads = std::min(static_cast<kmp_uint32>(team->t.t_nproc),
                               Topo::numa_topology.get_num_cores());
  const auto numaCores = Topo::numa_topology.get_num_cores();
  const auto numNuma = Topo::numa_topology.get_num_numa();
  KMP_DEBUG_ASSERT(numaCores);
  KMP_DEBUG_ASSERT(numNuma);

  const auto numaNodeSize = numaCores / numNuma;
  KMP_DEBUG_ASSERT(numaNodeSize);

  __kmp_summarize_taskloop_stats(team, ret_stats, nthreads, numaNodeSize,
                                 taskloop_start_time);
}
