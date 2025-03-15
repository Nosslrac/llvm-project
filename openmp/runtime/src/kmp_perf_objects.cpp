#include "kmp_perf_objects.h"
#include "kmp.h"
#include "kmp_debug.h"
// #include "kmp_debug.h"
// #include "kmp_os.h"

#include <asm/unistd_64.h>
#include <linux/perf_event.h>
#include <unistd.h>
#include <sys/ioctl.h>



namespace {
inline void init_attr(perf_event_attr *pe) {
  memset(pe, 0, sizeof(perf_event_attr));
  pe->size = sizeof(perf_event_attr);
  pe->disabled = 1;
  pe->exclude_kernel = 1;
  pe->exclude_hv = 1;
  pe->inherit = 0;
}

inline double frac(uint64_t numerator, uint64_t denominator)
{
  if(denominator == 0)
  {
    return 0.0;
  }
  return static_cast<double>(numerator) / static_cast<double>(denominator);
}

int32_t perf_event_open(struct perf_event_attr *hw_event, pid_t pid, int cpu,
                        int group_fd, unsigned long flags) {
  return static_cast<int32_t>(
      syscall(__NR_perf_event_open, hw_event, pid, cpu, group_fd, flags));
}

const char* enumToString(EventCodes event)
{
  switch (event) {
    case EventCodes::TOT_DISPATCH_SLOTS:
      return "TotDisp";
    case EventCodes::BACKEND_BOUND:
      return "BackendBound";
    case EventCodes::BACKEND_MEM_NUMERATOR:
      return "BackendNumerator";
    case EventCodes::BACKEND_MEM_DENOMINATOR:
      return "BackendDenominator";
    default:
      return "UNKNOWN";
  }
}

} // namespace

template<EventCodes E>
AMDRawEvent<E>::AMDRawEvent(int32_t cpu_id) : m_fd(-1), m_cpu(cpu_id) {}

template<EventCodes E>
void AMDRawEvent<E>::initCounter() {
  KA_TRACE(5, ("%s::initCounter: CPU#%d\n", enumToString(E), m_cpu));
  perf_event_attr pe;
  init_attr(&pe);
  pe.type = PERF_TYPE_RAW;
  pe.config = E;
  m_fd = perf_event_open(&pe, 0, m_cpu, -1, 0);
  KMP_DEBUG_ASSERT2(m_fd > 2, "Open perf event failed");
}

template<EventCodes E>
void AMDRawEvent<E>::startCounter() const {
  KA_TRACE(5, ("%s::startCounter: fd=%d\n", enumToString(E), m_fd));
  KMP_DEBUG_ASSERT2(m_fd > 2, "Invalid filedescriptor");
  ioctl(m_fd, PERF_EVENT_IOC_RESET);
  ioctl(m_fd, PERF_EVENT_IOC_ENABLE);
}



//////////////////////////////////////////////////////////////////////
/// @brief Disabling and reading varies a bit between the counters ///
//////////////////////////////////////////////////////////////////////
template<EventCodes E>
uint64_t AMDRawEvent<E>::stopAndRead() const {
  KA_TRACE(5, ("%s::stopAndRead: fd=%d\n", enumToString(E), m_fd));
  KMP_DEBUG_ASSERT2(m_fd > 2, "Invalid filedescriptor");

  ioctl(m_fd, PERF_EVENT_IOC_DISABLE);

  uint64_t counter = 0;
  if (read(m_fd, &counter, sizeof(uint64_t)) == -1) {
    KA_TRACE(1, ("%s:%d: __kmp_stop_counter(ERROR): Reading counter for "
                 "CPU#%d. Read fail\n",
                 __FILE_NAME__, __LINE__, m_cpu));
    perror("Reason: ");
    close(m_fd);
    return 0;
  }
  if constexpr (E == EventCodes::TOT_DISPATCH_SLOTS)
  {
    return counter * 6;
  }
  return counter;
}


template<EventCodes E>
void AMDRawEvent<E>::disableCounter() {
  KMP_DEBUG_ASSERT2(m_fd > 2, "Invalid file descriptor");
  ioctl(m_fd, PERF_EVENT_IOC_DISABLE);
  close(m_fd);
  m_fd = -1;
}


///////////////////////////////////////////////////////////////////
/// @brief RawAMDPerfContainer wraps all raw counters           ///
///////////////////////////////////////////////////////////////////
RawAMDPerfContainer::RawAMDPerfContainer(int32_t cpu_id, int32_t gtid)
    : m_cpu(cpu_id), m_gtid(gtid), m_totDisp(cpu_id), m_backendBound(cpu_id),
    m_backendMemNumerator(cpu_id), m_backendMemDenominator(cpu_id) {
}

RawAMDPerfContainer &
RawAMDPerfContainer::operator=(RawAMDPerfContainer &&other) {
  m_cpu = other.m_cpu;
  m_gtid = other.m_gtid;
  m_totDisp = other.m_totDisp;
  m_backendBound = other.m_backendBound;
  m_backendMemNumerator = other.m_backendMemNumerator;
  m_backendMemDenominator = other.m_backendMemDenominator;
  KA_TRACE(5, ("RawContainer assign for CPU#%d\n", m_cpu));
  return *this;
}

void RawAMDPerfContainer::initAll() {
  KA_TRACE(5, ("RawAMDPerfContainer::initAll: Init AMD counters for %d\n", m_cpu));
  m_totDisp.initCounter();
  m_backendBound.initCounter();
  m_backendMemNumerator.initCounter();
  m_backendMemDenominator.initCounter();
  // TODO: Add the rest
}

void RawAMDPerfContainer::startAll() const {
  KA_TRACE(5, ("RawAMDPerfContainer::startAll: Starting all AMD counters for CPU#%d\n", m_cpu));
  m_totDisp.startCounter();
  m_backendBound.startCounter();
  m_backendMemNumerator.startCounter();
  m_backendMemDenominator.startCounter();
  // TODO: Add the rest
}

AMDRawResults RawAMDPerfContainer::stopAndReadAll() const {
  KA_TRACE(5, ("RawAMDPerfContainer::stopAndReadAll: Stop and read AMD counters for CPU#%d\n", m_cpu));
  const auto totDisp = m_totDisp.stopAndRead();

  const auto backendBound = m_backendBound.stopAndRead();
  const auto backendMemNumer = m_backendMemNumerator.stopAndRead();
  const auto backendMemDenom = m_backendMemDenominator.stopAndRead();

  const auto backendBoundFrac = frac(backendBound, totDisp);
  const auto backendBoundMemFrac = backendBoundFrac * frac(backendMemNumer, backendMemDenom);
  const auto backendBoundCPU = backendBoundFrac * (1 - frac(backendMemNumer, backendMemDenom));

  // TODO: Add the rest
  return AMDRawResults(totDisp, backendBoundFrac, backendBoundMemFrac, backendBoundCPU);
}

void RawAMDPerfContainer::disableAll() {
  KA_TRACE(5, ("RawAMDPerfContainer::disableAll: Disabling AMD counters for CPU#%d\n", m_cpu));
  m_totDisp.disableCounter();
  m_backendBound.disableCounter();
  m_backendMemNumerator.disableCounter();
  m_backendMemDenominator.disableCounter();
  // TODO: Add the rest
}