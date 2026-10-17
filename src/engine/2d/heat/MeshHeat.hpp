// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
namespace openswmm { struct SimulationContext; }
namespace openswmm::twoD {
struct MeshData;struct SurfaceStateData;struct SolverOptions2D;
// Empty in compatibility mode. Bed energy survives surface drying.
struct MeshHeatState {
    SimulationContext* context=nullptr; // owning engine; never serialized
    bool enabled=false,surface=false,radiative=false,bed=false;
    double ground_now=0;
    std::vector<double> bed_energy,initial_bed_energy,bed_capacity,reported_temperature;
    std::vector<double> atmospheric_j,radiative_j,sensible_j,ground_j,water_bed_j,water_j,latent_j,evaporated_m3;
    std::vector<int> failures;
    std::vector<unsigned char> hyporheic_eligible;
    void clear(){*this=MeshHeatState{};}
};
std::string initializeMeshHeat(SimulationContext&,const MeshData&,SurfaceStateData&,const SolverOptions2D&);
void prepareMeshHeat(SimulationContext&,SurfaceStateData&,double seconds);
// Called before intensive source sinks: the accepted evaporating water must
// receive its thermal update before it leaves, including complete drying.
double applyMeshHeatCell(const MeshData&,const SolverOptions2D&,MeshHeatState&,int cell,double temperature_volume,
                         double available_m3,double evaporated_m3,double seconds) noexcept;
} // namespace openswmm::twoD
