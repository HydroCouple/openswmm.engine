// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
#include "GroundHeat.hpp"
namespace openswmm { struct SimulationContext; }
namespace openswmm::twoD {
struct MeshData;struct SurfaceStateData;struct SolverOptions2D;
// Empty in compatibility mode. Bed energy survives surface drying.
struct MeshHeatState {
    SimulationContext* context=nullptr; // owning engine; never serialized
    bool enabled=false,surface=false,radiative=false,bed=false;
    bool surface_inherits=true,radiative_inherits=true;
    bool ground_column=false;
    GroundHeatState standalone_ground;
    GroundHeatState* aquifer_ground=nullptr; // borrowed physical owner
    std::vector<unsigned char> aquifer_owned;
    double ground_now=0;
    std::vector<double> bed_energy,initial_bed_energy,bed_capacity,reported_temperature;
    std::vector<double> atmospheric_j,radiative_j,sensible_j,ground_j,water_bed_j,water_j,latent_j,evaporated_m3;
    std::vector<int> failures;
    std::vector<unsigned char> hyporheic_eligible;
    GroundHeatState& groundOwner(int i) {return aquifer_owned[i]?*aquifer_ground:standalone_ground;}
    const GroundHeatState& groundOwner(int i) const {return aquifer_owned[i]?*aquifer_ground:standalone_ground;}
    void refreshBedView(int i);
    void clear(){*this=MeshHeatState{};}
};
std::string initializeMeshHeat(SimulationContext&,const MeshData&,SurfaceStateData&,const SolverOptions2D&,
    GroundHeatState* aquifer=nullptr,const std::vector<uint8_t>* coverage=nullptr);
void prepareMeshHeat(SimulationContext&,SurfaceStateData&,double seconds);
// Called before intensive source sinks: the accepted evaporating water must
// receive its thermal update before it leaves, including complete drying.
double applyMeshHeatCell(const MeshData&,const SolverOptions2D&,MeshHeatState&,int cell,double temperature_volume,
                         double available_m3,double evaporated_m3,double seconds) noexcept;
// Standalone storage has a fixed saturated pore volume: accepted infiltration
// displaces an equal volume at its bottom, with finite soil heat exchange.
void advectStandaloneGround(MeshHeatState&,int cell,double accepted_m3,double donor_celsius) noexcept;
} // namespace openswmm::twoD
