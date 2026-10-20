// SPDX-License-Identifier: Apache-2.0
#ifndef OPENSWMM_GW_BOUNDARY_FORCING_HPP
#define OPENSWMM_GW_BOUNDARY_FORCING_HPP
#include "GwSourceForcing.hpp"
#include <cstdint>
namespace openswmm { struct SimulationContext; }
namespace openswmm::twoD {
struct MeshData; struct SubsurfaceConfig; struct SubsurfaceState;
struct SubsurfaceTransportState; struct GwTransportData;
struct GwBoundaryTerm {
    int row=-1;
    bool flux=false;
    double scale=1;
    GwSourceSignal signal;
};
struct GwResolvedBoundary {
    int cell=-1,edge=-1;
    bool head=false;
    double length=0,distance=0;
    GwSourceSignal signal;
    std::vector<GwBoundaryTerm> terms;
    double requested_in=0,requested_out=0,applied_in=0,applied_out=0;
    std::vector<double> mass_in,mass_out,flux_in,flux_out;
};
struct GwBoundaryPulse {
    std::size_t boundary=0;
    double in=0,out=0;
    std::vector<double> mass,flux_in,flux_out;
};
std::vector<GwResolvedBoundary> resolveGwBoundaries(SimulationContext&,const MeshData&,
    const SubsurfaceConfig&,const GwTransportData&,const SubsurfaceState&,
    const SubsurfaceTransportState&,const std::vector<uint8_t>& coverage,double heat_capacity);
void appendGwBoundaryCuts(const GwResolvedBoundary&,double,double,double,double,double,
                          std::vector<double>&);
GwBoundaryPulse integrateGwBoundary(const GwResolvedBoundary&,double,double,
    double head,double bottom,double thickness,double conductivity,int species);
}
#endif
