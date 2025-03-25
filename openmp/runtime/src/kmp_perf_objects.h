#pragma once
#include <cstdint>

enum EventCodes : uint64_t {
  TOT_DISPATCH_SLOTS = 0x430076,
  BACKEND_BOUND = 0x100431EA0,
  BACKEND_MEM_NUMERATOR = 0x43A2D6,
  BACKEND_MEM_DENOMINATOR = 0x4302D6,
};

struct AMDRawResults {
  explicit AMDRawResults()
      : totDisp(0), backend(0), backendMem(0), backendCPU(0) {};
  explicit AMDRawResults(uint64_t disp, double bound, double boundMem,
                         double boundCPU)
      : totDisp(disp), backend(bound), backendMem(boundMem),
        backendCPU(boundCPU) {};
  AMDRawResults &operator+=(const AMDRawResults &other);
  AMDRawResults avg(int32_t nthreads);

  uint64_t totDisp;
  double backend;
  double backendMem;
  double backendCPU;
};

///////////////////////////////
/// @brief Raw event slots  ///
///////////////////////////////
template <EventCodes code> class AMDRawEvent {
public:
  explicit AMDRawEvent(int32_t cpu_id);

  void initCounter();
  void startCounter() const;
  uint64_t stopAndRead();
  void disableCounter();
  void resetAccumulatedCounter();
  uint64_t accumulatedCounter();

private:
  int32_t m_fd;
  int32_t m_cpu;
  uint64_t m_accumCounter;
};

///////////////////////////////////////////////////
/// @brief Container for all AMD perf counters  ///
///////////////////////////////////////////////////
class RawAMDPerfContainer {
public:
  explicit RawAMDPerfContainer(int32_t cpu_id, int32_t gtid);
  RawAMDPerfContainer &operator=(RawAMDPerfContainer &&other);

  void initAll();
  void startAll() const;
  AMDRawResults stopAndReadAll();
  AMDRawResults summarizeCounters();
  void disableAll();

private:
  int32_t m_cpu;
  int32_t m_gtid;
  AMDRawEvent<EventCodes::TOT_DISPATCH_SLOTS> m_totDisp;
  AMDRawEvent<EventCodes::BACKEND_BOUND> m_backendBound;
  AMDRawEvent<EventCodes::BACKEND_MEM_NUMERATOR> m_backendMemNumerator;
  AMDRawEvent<EventCodes::BACKEND_MEM_DENOMINATOR> m_backendMemDenominator;
};
