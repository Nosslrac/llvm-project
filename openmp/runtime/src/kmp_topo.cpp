#include "kmp_topo.h"

#include "hwloc.h"
#include "kmp.h"
#include "kmp_debug.h"

///////////////////////////////////////////////
///               Topology section          ///
///////////////////////////////////////////////

const NumaTopology Topo::numa_topology = Topo::__kmp_read_topology();

NumaTopology Topo::__kmp_read_topology() {
  hwloc_topology_t topology = nullptr;
  // Load topology
  if (hwloc_topology_init(&topology) == -1) {
    KMP_FATAL(MsgExiting, "Hardware topology not read");
    return NumaTopology(0, 0, 0);
  }
  if (hwloc_topology_load(topology) == -1) {
    KMP_FATAL(MsgExiting, "Hardware topology not read");
    return NumaTopology(0, 0, 0);
  }

  // Get relevant intro
  const auto nNumaNodes =
      hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_NUMANODE);
  const auto ncores = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
  const auto nsockets = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_PACKAGE);

  KMP_ASSERT(nNumaNodes > 0);
  KMP_ASSERT(ncores > 0);
  KMP_ASSERT(nsockets > 0);

  return NumaTopology(static_cast<kmp_uint32>(nNumaNodes),
                      static_cast<kmp_uint32>(ncores),
                      static_cast<kmp_uint32>(nsockets));
}

void NumaTopology::showTopo() const {
  KA_TRACE(1, ("NumaTopology::showTopo: System properties:\n"
               "    - Number of numa nodes: %u\n"
               "    - Numa size: %u\n"
               "    - Number of cores: %u\n"
               "    - Number of sockets: %u\n",
               m_num_numa, m_numa_size, m_num_cores, m_num_sockets));
}