// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PorousHeat.hpp"
#include "GroundPorewater.hpp"
#include <cstdint>
namespace openswmm {struct SimulationContext;}
namespace openswmm::surface {class MeshSnow;}
namespace openswmm::twoD {
struct MeshData;struct GwTransportData;struct SubsurfaceTransportState;
struct GroundThermalCell {
    PorousHeatColumn column;
    GroundPorewaterColumn porewater;
    std::vector<double> saturated_water;
    // Water already accepted out of this column but not yet applied to its
    // hydraulic moisture profile. Its sensible energy is debited at the
    // producing face; reserve its heat capacity from intervening contacts.
    std::vector<double> reserved_out_water;
    double initial_j=0,advective_j=0,source_j=0,surface_j=0,deep_j=0,latent_j=0,lateral_j=0,water_contact_j=0,channel_contact_j=0,lid_contact_j=0;
    double vertical_absolute_j=0,geo_flux=.065;
    bool surface_owned=false;
    int failed=0;
    std::string error;
};
struct GroundLidThermalOwner {int node=-1,row=-1;double area=0;};
struct GroundHeatState {
    SimulationContext* context=nullptr;
    surface::MeshSnow* snow_owner=nullptr; // borrowed mesh snow state for reciprocal UEB contact
    const GwTransportData* config=nullptr;
    std::vector<GroundThermalCell> cells;
    // Resolved Mesh Richards LID footprints by soil cell. Derived from the
    // hydraulic bed bindings at initialization; energy itself lives in the
    // persistent LidNodeState and is checkpointed with the 1D owner.
    std::vector<std::vector<GroundLidThermalOwner>> lid_owners;
    double top_now=0,deep_now=0;
    double density=1000,water_capacity=4184000;
    int top_series=-1,deep_series=-1;
    bool active() const noexcept {return !cells.empty();}
    void clear(){*this={};}
    double activeCapacity(int cell,std::size_t layer) const noexcept;
    double activeTemperature(int cell,std::size_t layer) const noexcept;
    bool snowContactEnabled(int cell) const noexcept;
    double snowContactFraction(int cell) const noexcept;
    bool appendSnowContact(int cell,double area,std::vector<ThermalBoundary>&) const;
    std::string applySnowContactEnergy(int cell,double soil_j);
    std::string initialize(SimulationContext&,const MeshData&,const SubsurfaceState&,
                          const SubsurfaceTransportState&,const GwTransportData&,const std::vector<uint8_t>& coverage);
    std::string initializeStandalone(SimulationContext&,const MeshData&,const GwTransportData&,const std::vector<unsigned char>& owned);
    void prepare();
    void refreshView(int cell,SubsurfaceTransportState&);
    double zoneTemperature(int cell,bool saturated) const;
    // Append physical top/deep boundaries, returning the number of top
    // records. Exposed fraction excludes the finite water contact footprint.
    std::size_t boundaries(int cell,double exposed_fraction,std::vector<ThermalBoundary>&) const;
    // Called after accepted water advection. A surface-owned column advances
    // its conduction with its finite water contact instead, never twice.
    std::string conduct(int cell,double seconds,bool midpoint=false);
};
} // namespace openswmm::twoD
