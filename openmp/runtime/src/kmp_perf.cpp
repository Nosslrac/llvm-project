#include "kmp_perf.h"
#include "kmp.h"
#include "kmp_debug.h"
#include "kmp_os.h"

#include <asm/unistd_64.h>
#include <cstdint>
#include <sched.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/ioctl.h>

namespace {
  inline constexpr int perf_id(PerfEvents event)
  {
    return static_cast<int>(event);
  }

  inline double frac(uint64_t numerator, uint64_t denominator)
  {
    return static_cast<double>(numerator) / static_cast<double>(denominator);
  }

  template<PerfEvents ev>
  inline constexpr int perf_event()
  {
    if constexpr (ev == PerfEvents::CACHE_REFS) return PERF_COUNT_HW_CACHE_REFERENCES;
    if constexpr (ev == PerfEvents::BACK_STALL) return PERF_COUNT_HW_STALLED_CYCLES_BACKEND;
    if constexpr (ev == PerfEvents::LLC_MISSES) return PERF_COUNT_HW_CACHE_MISSES;
    if constexpr (ev == PerfEvents::TOT_CYCLES) return PERF_COUNT_HW_CPU_CYCLES;
    if constexpr (ev == PerfEvents::TOT_INSTRUCTIONS) return PERF_COUNT_HW_INSTRUCTIONS;
    if constexpr (ev == PerfEvents::PAGE_FAULTS) return PERF_COUNT_SW_PAGE_FAULTS;
  }

  const char* enumToString(PerfEvents event)
  {
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


  int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                              int cpu, int group_fd, unsigned long flags) {
    return static_cast<int32_t>(syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
  }


  void log_and_sum_events(kmp_info_t *thread, uint64_t* accum)
  {
    const auto gtid = __kmp_gtid_from_thread(thread);
    int32_t cpu_id = sched_getcpu();
    for(auto i = 0; i < NUM_PERF_EVENTS; i++)
    {
      accum[i] += thread->th.perf_accum[i];    
    }
    KA_TRACE(1, ("     #Counters for T#%d = CPU#%d:\n"
                "      - Tot cycles = %ld\n"
                "      - Tot ins = %ld\n"
                "      - Cache refs = %ld\n"
                "      - LLC misses = %ld\n"
                "      - Backend stalls = %ld\n"
                "     # Ratios:\n"
                "      - IPC = %lf\n"
                "      - Miss ratio = %lf\n"
                "      - Stalls ratio = %lf\n",
                  gtid, cpu_id, 
                  thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)],
                  thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)], 
                  thread->th.perf_accum[perf_id(PerfEvents::CACHE_REFS)], 
                  thread->th.perf_accum[perf_id(PerfEvents::LLC_MISSES)], 
                  thread->th.perf_accum[perf_id(PerfEvents::BACK_STALL)],
                  frac(accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)], accum[perf_id(PerfEvents::TOT_CYCLES)]),
                  frac(accum[perf_id(PerfEvents::LLC_MISSES)], accum[perf_id(PerfEvents::CACHE_REFS)]),
                  frac(accum[perf_id(PerfEvents::BACK_STALL)], accum[perf_id(PerfEvents::TOT_CYCLES)])));
  }

  template<PerfEvents ev>
  void init_perf_event(kmp_info_t *thread, perf_event_attr* pe, int32_t cpu_id)
  {
    constexpr int type = ev != PerfEvents::PAGE_FAULTS ? PERF_TYPE_HARDWARE : PERF_TYPE_SOFTWARE;
    pe->type = type;
    pe->config = perf_event<ev>();
    int32_t fd = perf_event_open(pe, 0, cpu_id, -1, 0);
    thread->th.perf_stats[perf_id(ev)] = fd;

    if (fd == -1) {
      KA_TRACE(5, ("%s:%d: __kmp_start_counter(ERROR): #T%d = CPU#%d: Cannot open %s\n", 
      __FILE_NAME__, __LINE__, __kmp_gtid_from_thread(thread), cpu_id, enumToString(ev)));
      perror("Reason: ");
      return;
    }
    
    ioctl(fd, PERF_EVENT_IOC_RESET);
    ioctl(fd, PERF_EVENT_IOC_ENABLE);
  }

  template<PerfEvents ev>
  uint64_t disable_perf_event(kmp_info_t *thread, int32_t cpu_id)
  {
    // Disable event and read
    const int fd = thread->th.perf_stats[perf_id(ev)];
    if(fd == -1) // Counter was unavailable
    {
      return 0;
    }

    ioctl(fd, PERF_EVENT_IOC_DISABLE);

    uint64_t counter = 0;
    if(read(fd, &counter, sizeof(uint64_t)) == -1)
    {
      KA_TRACE(1, ("%s:%d: __kmp_stop_counter(ERROR): Reading counter for CPU#%d. Read fail\n", __FILE_NAME__, __LINE__, cpu_id));
      perror("Reason: ");
      close(fd);
      return 0;
    }

    thread->th.perf_stats[perf_id(ev)] = 0; // reset fd
    thread->th.perf_accum[perf_id(ev)] += counter;
    close(fd);
    return counter;
  }
} // namespace

void Perf::__kmp_init_counters(kmp_info_t *thread, int32_t gtid)
{
  // Init perf event
  perf_event_attr pe;
  memset(&pe, 0, sizeof(perf_event_attr));
  pe.size = sizeof(perf_event_attr);
  pe.disabled = 1;
  pe.exclude_kernel = 1;
  pe.inherit = 0;
  pe.exclude_hv = 1;

  int32_t cpu_id = sched_getcpu();

  init_perf_event<PerfEvents::CACHE_REFS>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::LLC_MISSES>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::TOT_CYCLES>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::BACK_STALL>(thread, &pe, cpu_id);
  init_perf_event<PerfEvents::PAGE_FAULTS>(thread, &pe, cpu_id);

  KA_TRACE(2, ("%s:%d: __kmp_init_counter: Perf start T#%d = CPU#%d.\n ", 
              __FILE_NAME__, __LINE__, gtid, cpu_id));
}


void Perf::__kmp_stop_counters(kmp_info_t *thread, int32_t gtid, kmp_int32 *routine, kmp_int32 *task_id)
{
  int32_t cpu_id = sched_getcpu();

  uint64_t tot_cycles = disable_perf_event<PerfEvents::TOT_CYCLES>(thread, cpu_id);
  uint64_t cache_refs = disable_perf_event<PerfEvents::CACHE_REFS>(thread, cpu_id);
  uint64_t llc_misses = disable_perf_event<PerfEvents::LLC_MISSES>(thread, cpu_id);
  uint64_t tot_ins = disable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, cpu_id);
  uint64_t back_stall = disable_perf_event<PerfEvents::BACK_STALL>(thread, cpu_id);
  uint64_t page_fault = disable_perf_event<PerfEvents::PAGE_FAULTS>(thread, cpu_id);

  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);
  thread->th.time = current_time - thread->th.time;

  KA_TRACE(2, ("%s:%d: __kmp_stop_counter: Counters for Task %p executing routine %p on CPU#%d (T#%d):\n"
               "      - Tot cycles = %ld\n"
               "      - Tot ins = %ld\n"
               "      - Cache refs = %ld\n"
               "      - LLC misses = %ld\n"
               "      - Backend stalls = %ld\n"
               "      - Page faults = %ld\n"
               "      - Execution time = %f\n",
               __FILE_NAME__, __LINE__, task_id, routine, cpu_id, gtid, tot_cycles, tot_ins, 
               cache_refs, llc_misses, back_stall, page_fault, thread->th.time));
}


void Perf::__kmp_summarize_taskloop(kmp_team *team)
{
  uint64_t accum[NUM_PERF_EVENTS] = {};
  int32_t nthreads = team->t.t_nproc;
  kmp_real64 duration = 0.0;
  for (int i = 0; i < nthreads; ++i) {
    kmp_info_t *thread = team->t.t_threads[i];
    log_and_sum_events(thread, accum);
    if (thread->th.time > 0.1) {
      if (duration > 0.1) {
        KA_TRACE(1, ("%s:%d: __kmp_summarize_taskloop(ERROR): Duplicate time "
                     "measurement\n",
                     __FILE_NAME__, __LINE__, team));
      }
      thread->th.time = 0.0; // Reset for next taskloop
    }
  }

  KA_TRACE(1, ("%s:%d: __kmp_summarize_taskloop: Taskloop execution time for team %p = %lf\n"
               "      - Tot cycles = %ld\n"
               "      - Tot ins = %ld\n"
               "      - Cache refs = %ld\n"
               "      - LLC misses = %ld\n"
               "      - Backend stalls = %ld\n"
               "      - Page faults = %ld\n"
               "  # Ratios:\n"
               "      - IPC = %lf\n"
               "      - Miss ratio = %lf\n"
               "      - Stalls ratio = %lf\n",
                 __FILE_NAME__, __LINE__, team, duration,
                 accum[perf_id(PerfEvents::TOT_CYCLES)], 
                 accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)], 
                 accum[perf_id(PerfEvents::CACHE_REFS)], 
                 accum[perf_id(PerfEvents::LLC_MISSES)], 
                 accum[perf_id(PerfEvents::BACK_STALL)],
                 accum[perf_id(PerfEvents::PAGE_FAULTS)],
                 frac(accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)], accum[perf_id(PerfEvents::TOT_CYCLES)]),
                 frac(accum[perf_id(PerfEvents::LLC_MISSES)], accum[perf_id(PerfEvents::CACHE_REFS)]),
                 frac(accum[perf_id(PerfEvents::BACK_STALL)], accum[perf_id(PerfEvents::TOT_CYCLES)])));
}
