// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <limits>
#include <string>
#include <vector>
#include "GroundHeat.hpp"
#include "LidNodeHeat.hpp"
namespace openswmm { struct SimulationContext; }
namespace openswmm::twoD {
struct MeshData;struct SurfaceStateData;struct SolverOptions2D;struct GwNodeBed;
// Empty in compatibility mode. Bed energy survives surface drying.
struct MeshHeatState {
    SimulationContext* context=nullptr; // owning engine; never serialized
    bool enabled=false,surface=false,radiative=false,bed=false;
    bool surface_inherits=true,radiative_inherits=true;
    bool ground_column=false;
    GroundHeatState standalone_ground;
    GroundHeatState* aquifer_ground=nullptr; // borrowed physical owner
    std::vector<unsigned char> aquifer_owned;
    std::vector<double> channel_footprint; // empty by default; SI area owned by routed channels
    double ground_now=0;
    std::vector<double> bed_energy,initial_bed_energy,bed_capacity,reported_temperature;
    std::vector<double> atmospheric_j,radiative_j,sensible_j,ground_j,water_bed_j,water_j,latent_j,evaporated_m3;
    std::vector<int> failures;
    // Seconds already advanced during the current router interval. Surface
    // temperature rows update stores inside the marcher; dry columns receive
    // only the remaining interval after routing completes.
    std::vector<double> ground_column_seconds;
    std::vector<unsigned char> hyporheic_eligible;
    // LOCAL_STORAGE porewater: fixed saturated pore volume, independent of
    // surface wetness. Temperature remains solely in bed_energy.
    int bed_species=0,bed_age_row=-1,bed_temp_row=-1;
    std::vector<double> bed_pore_volume,bed_carried_mass,bed_initial_mass,bed_exchange_mass,bed_generated_age;
    GroundHeatState& groundOwner(int i) {return aquifer_owned[i]?*aquifer_ground:standalone_ground;}
    const GroundHeatState& groundOwner(int i) const {return aquifer_owned[i]?*aquifer_ground:standalone_ground;}
    void refreshBedView(int i);
    void clear(){*this=MeshHeatState{};}
};
struct MeshAtmosphericFlux {
    double radiation=0,sensible=0,latent=0;
    double total() const noexcept {return radiation+sensible+latent;}
};
// A column's portion of one shared finite thermal system. Index ranges let
// a multi-owner solve reuse exactly the same mesh physics and receipts.
struct GroundColumnAssembly {
    struct LidEnergy {
        int row=-1,first_store=0;
        double area=0;
        LidNodeThermalGraph graph;
        std::vector<double> physical_energy;
        std::vector<double> physical_ground_contact_j;
        std::vector<std::size_t> vertical_contacts;
        std::size_t ground_contact=std::numeric_limits<std::size_t>::max();
    };
    GroundHeatState* owner=nullptr;
    MeshHeatState* mesh_heat=nullptr;
    int cell=-1,first_store=0,soil_count=0,water=-1;
    std::size_t first_contact=0,vertical_count=0,contact_count=0,first_boundary=0,top_count=0,ground_count=0,atmosphere=0;
    std::vector<LidEnergy> lids;
    int snow_boundary=-1;
    double mass=0,volume=0,evaporated=0,seconds=0,wet=0,temperature=0,slope=0,water_initial_j=0;
    MeshAtmosphericFlux flux0,flux1;
};
GroundColumnAssembly appendGroundColumnSystem(const MeshData&,const SolverOptions2D&,GroundHeatState&,int cell,
    MeshHeatState*,double temperature_volume,double volume,double evaporated,double seconds,double channel_area,
    std::vector<ThermalStore>&,std::vector<ThermalContact>&,std::vector<ThermalBoundary>&);
double commitGroundColumnSystem(GroundColumnAssembly&,const ThermalSolution&) noexcept;
std::string initializeMeshHeat(SimulationContext&,const MeshData&,SurfaceStateData&,const SolverOptions2D&,
    GroundHeatState* aquifer=nullptr,const std::vector<uint8_t>* coverage=nullptr);
std::string configureGroundLidOwners(SimulationContext&,const MeshData&,GroundHeatState&,
    const std::vector<GwNodeBed>&);
void prepareMeshHeat(SimulationContext&,SurfaceStateData&,double seconds);
// Complete ground-column conduction for dry cells that did not reach a
// temperature source row during the hydraulic march.
void advanceDryGroundColumns(const MeshData&,const SolverOptions2D&,SurfaceStateData&,double seconds) noexcept;
// Called before intensive source sinks: the accepted evaporating water must
// receive its thermal update before it leaves, including complete drying.
double applyMeshHeatCell(const MeshData&,const SolverOptions2D&,MeshHeatState&,int cell,double temperature_volume,
                         double available_m3,double evaporated_m3,double seconds) noexcept;
// Standalone storage has a fixed saturated pore volume: accepted infiltration
// displaces an equal volume at its bottom, with finite soil heat exchange.
void advectStandaloneGround(MeshHeatState&,int cell,double accepted_m3,double donor_celsius) noexcept;
double applyLocalBedSpecies(const MeshData&,MeshHeatState&,int cell,int species,
                           double surface_mass,double available_m3,double seconds) noexcept;
bool ageLocalBedPorewater(MeshHeatState&,double seconds) noexcept;
} // namespace openswmm::twoD
