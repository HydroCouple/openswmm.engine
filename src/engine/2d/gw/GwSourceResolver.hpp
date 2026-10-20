// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_GW_SOURCE_RESOLVER_HPP
#define OPENSWMM_GW_SOURCE_RESOLVER_HPP
#include "GwSourceForcing.hpp"
#include <unordered_map>
namespace openswmm {struct SimulationContext;}
namespace openswmm::twoD {
struct MeshData;struct GwTransportData;struct SubsurfaceState;struct SubsurfaceTransportState;
GwSourceSignal resolveGwSignal(SimulationContext&,double,const std::string&,
    std::unordered_map<std::string,GwSourceSignal>&,const char* label);
std::vector<GwResolvedSource> resolveGwSources(SimulationContext&,const MeshData&,
    const GwTransportData&,const SubsurfaceState&,const SubsurfaceTransportState&);
}
#endif
