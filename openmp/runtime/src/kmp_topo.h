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
                        kmp_uint32 num_sockets, kmp_uint64 base_steal_bits)
      : m_num_numa(num_numa), m_num_cores(num_cores),
        m_num_sockets(num_sockets), m_base_steal_bits(base_steal_bits) {};
  kmp_uint32 get_num_numa() const { return m_num_numa; }
  kmp_uint32 get_num_cores() const { return m_num_cores; }
  kmp_uint32 get_num_socket() const { return m_num_sockets; }
  kmp_uint64 get_base_steal_bits() const { return m_base_steal_bits; }

private:
  kmp_uint32 m_num_numa;
  kmp_uint32 m_num_cores;
  kmp_uint32 m_num_sockets;
  kmp_uint64 m_base_steal_bits;
};

namespace Topo {

// Topology part
NumaTopology __kmp_read_topology();
extern const NumaTopology numa_topology;
} // namespace Topo