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

static inline constexpr int perf_id(PerfEvents event) {
  return static_cast<int>(event);
}

template <PerfEvents ev> static inline constexpr int perf_event() {
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
}

static int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid,
                               int cpu, int group_fd, unsigned long flags) {
  return static_cast<int32_t>(
      syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

template <PerfEvents ev>
static void init_perf_event(kmp_info_t *thread, perf_event_attr *pe,
                            int32_t cpu_id) {
  pe->type = PERF_TYPE_HARDWARE;
  pe->config = perf_event<ev>();
  int32_t fd = perf_event_open(pe, 0, cpu_id, -1, 0);
  KA_TRACE(
      1,
      ("%s:%d: __kmp_init_counter: Perf start CPU#%d. Fd = %d, Event = %d\n ",
       __FILE_NAME__, __LINE__, cpu_id, fd, perf_id(ev)));

  if (fd == -1) {
    KA_TRACE(1,
             ("%s:%d: __kmp_init_counter(ERROR): Perf event fail for CPU#%d\n ",
              __FILE_NAME__, __LINE__, cpu_id));
    perror("Reason: ");
    return;
  }

  ioctl(fd, PERF_EVENT_IOC_RESET);
  ioctl(fd, PERF_EVENT_IOC_ENABLE);

  __kmp_read_system_time(&thread->th.time);

  thread->th.perf_stats[perf_id(ev)] = fd;
}

template <PerfEvents ev>
static int64_t disable_perf_event(kmp_info_t *thread, int32_t cpu_id) {
  // Disable event and read
  const int fd = thread->th.perf_stats[perf_id(ev)];
  ioctl(fd, PERF_EVENT_IOC_DISABLE);

  int64_t counter = 0;
  if (read(fd, &counter, sizeof(int64_t)) == -1) {
    KA_TRACE(1, ("%s:%d: __kmp_stop_counter(ERROR): Reading counter for "
                 "CPU#%d. Read fail\n",
                 __FILE_NAME__, __LINE__, cpu_id));
    perror("Reason: ");
    close(fd);
    return -1;
  }
  return counter;
}

inline double frac(uint64_t numerator, uint64_t denominator) {
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

void Perf::__kmp_init_counter(kmp_info_t *thread, int32_t gtid) {
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

void Perf::__kmp_stop_counter(kmp_info_t *thread, int32_t gtid,
                              int32_t *routine, int32_t *task_id) {
  int32_t cpu_id = sched_getcpu();

  int64_t tot_cycles =
      disable_perf_event<PerfEvents::TOT_CYCLES>(thread, cpu_id);
  int64_t cache_refs =
      disable_perf_event<PerfEvents::CACHE_REFS>(thread, cpu_id);
  int64_t llc_misses =
      disable_perf_event<PerfEvents::LLC_MISSES>(thread, cpu_id);
  int64_t tot_ins =
      disable_perf_event<PerfEvents::TOT_INSTRUCTIONS>(thread, cpu_id);
  int64_t back_stall =
      disable_perf_event<PerfEvents::BACK_STALL>(thread, cpu_id);

  kmp_real64 current_time = 0;
  __kmp_read_system_time(&current_time);
  thread->th.time = current_time - thread->th.time;

  KA_TRACE(1,
           ("%s:%d: __kmp_stop_counter: Counters for Task %p executing Routine "
            "%p on CPU#%d (T#%d):\n"
            "      - Tot cycles = %lld\n"
            "      - Tot ins = %lld\n"
            "      - Cache refs = %lld\n"
            "      - LLC misses = %lld\n"
            "      - Backend stalls = %lld\n"
            "      - Execution time = %f\n",
            __FILE_NAME__, __LINE__, task_id, routine, cpu_id, gtid, tot_cycles,
            tot_ins, cache_refs, llc_misses, back_stall, thread->th.time));
}

static void log_and_sum_events(kmp_info_t *thread, uint64_t *accum) {
  const auto gtid = __kmp_gtid_from_thread(thread);
  int32_t cpu_id = sched_getcpu();
  for (auto i = 0; i < NUM_PERF_EVENTS; i++) {
    accum[i] += thread->th.perf_accum[i];
  }
  KA_TRACE(1, ("      #Counters for T#%d = CPU#%d:\n"
               "      - Tot cycles = %lld\n"
               "      - Tot ins = %lld\n"
               "      - Cache refs = %lld\n"
               "      - LLC misses = %lld\n"
               "      - Backend stalls = %lld\n",
               gtid, cpu_id,
               thread->th.perf_accum[perf_id(PerfEvents::TOT_CYCLES)],
               thread->th.perf_accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
               thread->th.perf_accum[perf_id(PerfEvents::CACHE_REFS)],
               thread->th.perf_accum[perf_id(PerfEvents::LLC_MISSES)],
               thread->th.perf_accum[perf_id(PerfEvents::BACK_STALL)]));
}

void Perf::__kmp_summarize_taskloop(kmp_team *team) {
  KA_TRACE(
      1, ("%s:%d: __kmp_summarize_taskloop: Summarizing counters for team %p\n",
          __FILE_NAME__, __LINE__, team));

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
      duration = thread->th.time;
    }
    thread->th.time = 0.0;
  }

  KA_TRACE(1, ("%s:%d: __kmp_summarize_taskloop: Taskloop execution time for "
               "team %p = %lf\n"
               "      - Tot cycles = %lld\n"
               "      - Tot ins = %lld\n"
               "      - Cache refs = %lld\n"
               "      - LLC misses = %lld\n"
               "      - Backend stalls = %lld\n",
               __FILE_NAME__, __LINE__, team, duration,
               accum[perf_id(PerfEvents::TOT_CYCLES)],
               accum[perf_id(PerfEvents::TOT_INSTRUCTIONS)],
               accum[perf_id(PerfEvents::CACHE_REFS)],
               accum[perf_id(PerfEvents::LLC_MISSES)],
               accum[perf_id(PerfEvents::BACK_STALL)]));
}
