#include "kmp_perf.h"
#include "kmp.h"
#include "kmp_debug.h"
#include "kmp_os.h"
#include "kmp_perf_objects.h"
#include "kmp_routine.h"
#include "kmp_schedule.h"

#include <asm/unistd_64.h>
#include <cstdint>
#include <limits>
#include <sched.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/ioctl.h>

#ifdef PERF_COUNTERS

namespace {
inline constexpr int perf_id(PerfEvents event) {
  return static_cast<int>(event);
}

inline double frac(uint64_t numerator, uint64_t denominator) {
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

template <PerfEvents ev> inline constexpr int perf_event() {
  if constexpr (ev == PerfEvents::CACHE_REFS)
    return PERF_COUNT_HW_CACHE_REFERENCES;
  if constexpr (ev == PerfEvents::BACK_STALL)
    return PERF_COUNT_HW_STALLED_CYCLES_BACKEND;
  if constexpr (ev == PerfEvents::LLC_MISSES)
    return PERF_COUNT_HW_CACHE_MISSES;
  if constexpr (ev == PerfEvents::TOT_CYCLES)
    return PERF_COUNT_HW_CPU_CYCLES;
  if constexpr (ev == PerfEvents::TOT_INSTRUCTIONS)
    return PERF_COUNT_HW_INSTRUCTIONS;
  if constexpr (ev == PerfEvents::PAGE_FAULTS)
    return PERF_COUNT_SW_PAGE_FAULTS;
}

const char *enumToString(PerfEvents event) {
  switch (event) {
  case PerfEvents::BACK_STALL:
    return "BACK_STALL";
  case PerfEvents::CACHE_REFS:
    return "CACHE_REFS";
  case PerfEvents::LLC_MISSES:
    return "LLC_MISSES";
  case PerfEvents::TOT_CYCLES:
    return "TOT_CYCLES";
  case PerfEvents::TOT_INSTRUCTIONS:
    return "TOT_INSTRUCTIONS";
  case PerfEvents::PAGE_FAULTS:
    return "PAGE_FAULTS";
  default:
    return "UNKNOWN";
  }
}

int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
                        int group_fd, unsigned long flags) {
  return static_cast<int32_t>(
      syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

void log_and_sum_events(kmp_info_t *thread, uint64_t *accum) {
  const auto gtid = __kmp_gtid_from_thread(thread);

  for (auto i = 0; i < NUM_PERF_EVENTS; i++) {
    accum[i] += thread->th.perf_accum[i];
  }

  KA_TRACE(5,
           ("     #Counters for T#%d:\n"
            "      - Tot cycles = %ld\n"
            "      - Tot ins = %ld\n"
            "      - Cache refs = %ld\n"
            "      - LLC misses = %ld\n"
            "      - Backend stalls = %ld\n"
            "      - Execution Time = %f\n"
            "     # Ratios:\n"
            "      - IPC = %lf\n"
            "      - Miss ratio = %lf\n"
            "      - Stalls ratio = %lf\n",
            gtid, thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)],
            thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
            thread->th.perf_accum[perf_id(PerfEvents::CACHE_REFS)],
            thread->th.perf_accum[perf_id(PerfEvents::LLC_MISSES)],
            thread->th.perf_accum[perf_id(PerfEvents::BACK_STALL)],
            thread->th.time_accum,
            frac(thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
                 thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)]),
            frac(thread->th.perf_accum[perf_id(PerfEvents::LLC_MISSES)],
                 thread->th.perf_accum[perf_id(PerfEvents::CACHE_REFS)]),
            frac(thread->th.perf_accum[perf_id(PerfEvents::BACK_STALL)],
                 thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)])));
}

template <PerfEvents ev>
void init_perf_event(kmp_info_t *thread, perf_event_attr *pe, int32_t cpu_id) {
  constexpr int type =
      ev != PerfEvents::PAGE_FAULTS ? PERF_TYPE_HARDWARE : PERF_TYPE_SOFTWARE;
  pe->type = type;
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

  if (fd < 3) {
    return;
  }

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

  if (fd < 3) {
    return 0;
  }

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

  init_perf_event<PerfEvents::CACHE_REFS>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::LLC_MISSES>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::TOT_CYCLES>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::BACK_STALL>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::PAGE_FAULTS>(thread, &pe, cpu_id);
}

void Perf::__kmp_start_counters(kmp_info_t *thread) {
  int32_t gtid = __kmp_get_gtid();
  int32_t cpu_id = sched_getcpu();

  KA_TRACE(3, ("%s:%d: __kmp_start_counter(entered): T#%d = CPU#%d.\n ",
               __FILE_NAME__, __LINE__, gtid, cpu_id));

  // Start perf counters and execution time
  __kmp_read_system_time(&thread->th.time);
  enable_perf_event<PerfEvents::CACHE_REFS>(thread);
  enable_perf_event<PerfEvents::LLC_MISSES>(thread);
  enable_perf_event<PerfEvents::TOT_CYCLES>(thread);
  enable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread);
  enable_perf_event<PerfEvents::BACK_STALL>(thread);
  enable_perf_event<PerfEvents::PAGE_FAULTS>(thread);

#ifdef AMD_PERF
  thread->th.perf_container.startAll();
#endif
}

void Perf::__kmp_stop_counters(kmp_info_t *thread, int32_t gtid,
                               kmp_int32 task_id) {
  int32_t cpu_id = sched_getcpu();

  uint64_t tot_cycles = stop_perf_event<PerfEvents::TOT_CYCLES>(thread, cpu_id);
  uint64_t cache_refs = stop_perf_event<PerfEvents::CACHE_REFS>(thread, cpu_id);
  uint64_t llc_misses = stop_perf_event<PerfEvents::LLC_MISSES>(thread, cpu_id);
  uint64_t tot_ins =
      stop_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, cpu_id);
  uint64_t back_stall = stop_perf_event<PerfEvents::BACK_STALL>(thread, cpu_id);
  uint64_t page_fault =
      stop_perf_event<PerfEvents::PAGE_FAULTS>(thread, cpu_id);

#ifdef AMD_PERF
  AMDRawResults results = thread->th.perf_container.stopAndReadAll();
#endif

  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);
  kmp_real64 elapsed_time = current_time - thread->th.time;
  thread->th.time_accum += elapsed_time;

  KA_TRACE(4, ("%s:%d: __kmp_stop_counters: Counters for Task %p executing "
               "routine %p on CPU#%d (T#%d):\n"
               "      - Tot cycles = %ld\n"
               "      - Tot ins = %ld\n"
               "      - Cache refs = %ld\n"
               "      - LLC misses = %ld\n"
               "      - Backend stalls = %ld\n"
               "      - Page faults = %ld\n"
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
               gtid, tot_cycles, tot_ins, cache_refs, llc_misses, back_stall,
               page_fault
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

void Perf::__kmp_disable_counters(kmp_info_t *thread) {

  disable_perf_event<PerfEvents::CACHE_REFS>(thread);
  disable_perf_event<PerfEvents::LLC_MISSES>(thread);
  disable_perf_event<PerfEvents::TOT_CYCLES>(thread);
  disable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread);
  disable_perf_event<PerfEvents::BACK_STALL>(thread);
  disable_perf_event<PerfEvents::PAGE_FAULTS>(thread);

#ifdef AMD_PERF
  thread->th.perf_container.disableAll();
#endif
}

void Perf::__kmp_summarize_taskloop(kmp_team *team) {
#ifdef AMD_PERF
  AMDRawResults accumAMD;
#endif
  uint64_t accum[NUM_PERF_EVENTS] = {};
  uint32_t nthreads = team->t.t_nproc;

  KA_TRACE(1, ("\n%s:%d: __kmp_summarize_taskloop: Summarizing"
               " stats for routine %p \n",
               __FILE_NAME__, __LINE__, team->t.t_threads[0]->th.routine_id));

  for (int i = 0; i < nthreads; ++i) {
    kmp_info_t *thread = team->t.t_threads[i];
    log_and_sum_events(thread, accum);
#ifdef AMD_PERF
    accumAMD += thread->th.perf_container.summarizeCounters();
#endif
  }

#ifdef AMD_PERF
  accumAMD = accumAMD.avg(nthreads);
#endif

  KA_TRACE(1, ("%s:%d: __kmp_summarize_taskloop: Taskloop team %p\n"
               "      - Tot cycles = %ld\n"
               "      - Tot ins = %ld\n"
               "      - Cache refs = %ld\n"
               "      - LLC misses = %ld\n"
               "      - Backend stalls = %ld\n"
               "      - Page faults = %ld\n"
               "  # Ratios:\n"
               "      - IPC = %lf\n"
               "      - Miss ratio = %lf\n"
               "      - Stalls ratio = %lf\n"
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
               ,
               __FILE_NAME__, __LINE__, team,
               accum[perf_id(PerfEvents::TOT_CYCLES)],
               accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
               accum[perf_id(PerfEvents::CACHE_REFS)],
               accum[perf_id(PerfEvents::LLC_MISSES)],
               accum[perf_id(PerfEvents::BACK_STALL)],
               accum[perf_id(PerfEvents::PAGE_FAULTS)],
               frac(accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
                    accum[perf_id(PerfEvents::TOT_CYCLES)]),
               frac(accum[perf_id(PerfEvents::LLC_MISSES)],
                    accum[perf_id(PerfEvents::CACHE_REFS)]),
               frac(accum[perf_id(PerfEvents::BACK_STALL)],
                    accum[perf_id(PerfEvents::TOT_CYCLES)])
#ifdef AMD_PERF
                   ,
               accumAMD.m_totDisp, accumAMD.m_l1All, accumAMD.m_l1DiffNuma,
               accumAMD.m_l1SameCXX, accumAMD.m_l1AnotherCXX, accumAMD.m_l3Miss,
               accumAMD.m_retiring, accumAMD.m_backend, accumAMD.m_backendMem,
               accumAMD.m_backendCPU
#endif
               ));
}

void Perf::__kmp_summarize_taskloop_numa(kmp_team *team,
                                         const kmp_real64 taskloop_start_time) {
  uint32_t nthreads = std::min(static_cast<kmp_uint32>(team->t.t_nproc),
                               Schedule::numa_topology.get_num_cores());

  KA_TRACE(3, ("\n%s:%d: __kmp_summarize_taskloop_numa: Summarizing"
               " stats for routine %p \n",
               __FILE_NAME__, __LINE__, team->t.t_threads[0]->th.routine_id));

  const auto numaNodeSize = Schedule::numa_topology.get_num_cores() /
                            Schedule::numa_topology.get_num_numa();
  for (kmp_uint32 i = 0U; i < nthreads / numaNodeSize; i++) {
#ifdef AMD_PERF
    AMDRawResults accumAMD;
#endif
    uint64_t accum[NUM_PERF_EVENTS] = {};
    kmp_real64 min_time = std::numeric_limits<kmp_real64>::max();
    kmp_real64 max_time = 0.0;
    for (kmp_uint32 j = 0;
         j < numaNodeSize && (i * numaNodeSize) + j < nthreads; ++j) {
      kmp_info_t *thread = team->t.t_threads[(i * 8) + j];
      min_time = std::min(thread->th.task_finish_time, min_time);
      max_time = std::max(thread->th.task_finish_time, max_time);
      log_and_sum_events(thread, accum);
#ifdef AMD_PERF
      accumAMD += thread->th.perf_container.summarizeCounters();
#endif
    }
    auto threadPerNuma = nthreads / numaNodeSize;
    if (threadPerNuma < 1) {
      threadPerNuma = 1;
    }

#ifdef AMD_PERF
    accumAMD = accumAMD.avg(threadPerNuma);
#endif

    KA_TRACE(3, ("%s:%d: __kmp_summarize_taskloop_numa: Taskloop team=%p: NUMA "
                 "node=%d\nMin ExecT=%lf -- Max ExecT=%lf\n"
                 "      - Tot cycles = %ld\n"
                 "      - Tot ins = %ld\n"
                 "      - Cache refs = %ld\n"
                 "      - LLC misses = %ld\n"
                 "      - Backend stalls = %ld\n"
                 "      - Page faults = %ld\n"
                 "  # Ratios:\n"
                 "      - IPC = %lf\n"
                 "      - Miss ratio = %lf\n"
                 "      - Stalls ratio = %lf\n"
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
                 ,
                 __FILE_NAME__, __LINE__, team, i,
                 min_time - taskloop_start_time, max_time - taskloop_start_time,
                 accum[perf_id(PerfEvents::TOT_CYCLES)],
                 accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
                 accum[perf_id(PerfEvents::CACHE_REFS)],
                 accum[perf_id(PerfEvents::LLC_MISSES)],
                 accum[perf_id(PerfEvents::BACK_STALL)],
                 accum[perf_id(PerfEvents::PAGE_FAULTS)],
                 frac(accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
                      accum[perf_id(PerfEvents::TOT_CYCLES)]),
                 frac(accum[perf_id(PerfEvents::LLC_MISSES)],
                      accum[perf_id(PerfEvents::CACHE_REFS)]),
                 frac(accum[perf_id(PerfEvents::BACK_STALL)],
                      accum[perf_id(PerfEvents::TOT_CYCLES)])
#ifdef AMD_PERF
                     ,
                 accumAMD.m_totDisp, accumAMD.m_l1All, accumAMD.m_l1DiffNuma,
                 accumAMD.m_l1SameCXX, accumAMD.m_l1AnotherCXX,
                 accumAMD.m_l3Miss, accumAMD.m_retiring, accumAMD.m_backend,
                 accumAMD.m_backendMem, accumAMD.m_backendCPU
#endif
                 ));
  }
}

// This method calculates the perf metrics from
// the accumulated perf counters.
//
// NOTE: We could do all kinds of analysis here to return
// other types of metrics.
void Perf::__kmp_get_taskloop_stats(kmp_team *team,
                                    routine_stats_nodes *ret_stats,
                                    const kmp_real64 taskloop_start_time) {
  int32_t nthreads = team->t.t_nproc;
  uint64_t tot_cycles = 0;
  uint64_t tot_instr = 0;
  kmp_real64 exec_time = 0.0;

  /* Loop over all threads to calculate the metrics */
  for (int i = 0; i < nthreads; ++i) {
    kmp_info_t *thread = team->t.t_threads[i];

    /* Calculate execution_time stat */
    if (exec_time < thread->th.task_finish_time)
      exec_time = thread->th.task_finish_time;

    /* Calculate counters used for stall_ratio */
    tot_instr += thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)];
    tot_cycles += thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)];

    // Combine the stats for each NUMA node
    if ((i + 1) % 8 == 0) {

      (*ret_stats)[i / 8].execution_time =
          std::max((exec_time - taskloop_start_time), 0.0);
      exec_time = 0.0;

      (*ret_stats)[i / 8].IPC = frac(tot_instr, tot_cycles);
      tot_instr = 0.0;
      tot_cycles = 0.0;

      KA_TRACE(4,
               ("Perf::__kmp_get_taskloop_stats: for routine %p node[%d]: "
                "execT:%f, IPC:%f\n",
                team->t.t_threads[0]->th.routine_id, (i / 8),
                (*ret_stats)[i / 8].execution_time, (*ret_stats)[i / 8].IPC));
    }
  }
}

void Perf::__kmp_reset_taskloop_stats(kmp_team *team) {

  int32_t nthreads = team->t.t_nproc;

  for (int i = 0; i < nthreads; ++i) {
    kmp_info_t *thread = team->t.t_threads[i];

    // Reset all accums
    for (auto i = 0; i < NUM_PERF_EVENTS; i++) {
      thread->th.perf_accum[i] = 0;
    }

    // Reset thread taskloop timer
    thread->th.time_accum = 0.0;
  }
}

#endif