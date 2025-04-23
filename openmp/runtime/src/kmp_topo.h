#pragma once

#include "kmp_os.h"

////////////////////////////////
///   Forward declarations   ///
////////////////////////////////
union kmp_info;
union kmp_team;
union kmp_task_team;
union kmp_thread_data;
struct kmp_taskdata;
struct kmp_affinity_t;

class NumaTopology {
public:
  explicit NumaTopology(kmp_uint32 num_numa, kmp_uint32 num_cores,
                        kmp_uint32 num_sockets)
      : m_num_numa(num_numa), m_num_cores(num_cores),
        m_num_sockets(num_sockets), m_numa_size(num_cores / num_numa) {};
  kmp_uint32 get_num_numa() const { return m_num_numa; }
  kmp_uint32 get_num_cores() const { return m_num_cores; }
  kmp_uint32 get_num_socket() const { return m_num_sockets; }
  kmp_uint32 get_numa_size() const { return m_numa_size; }

private:
  kmp_uint32 m_num_numa;
  kmp_uint32 m_num_cores;
  kmp_uint32 m_num_sockets;
  kmp_uint32 m_numa_size;
};

namespace Topo {

// Topology part
NumaTopology __kmp_read_topology();
extern const NumaTopology numa_topology;
} // namespace Topo