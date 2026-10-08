// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_GW_SOURCE_RESOLVER_HPP
#define OPENSWMM_GW_SOURCE_RESOLVER_HPP
#include "GwSourceForcing.hpp"
namespace openswmm {struct SimulationContext;}
namespace openswmm::twoD {
struct MeshData;struct GwTransportData;struct SubsurfaceState;struct SubsurfaceTransportState;
std::vector<GwResolvedSource> resolveGwSources(SimulationContext&,const MeshData&,
    const GwTransportData&,const SubsurfaceState&,const SubsurfaceTransportState&);
}
#endif
