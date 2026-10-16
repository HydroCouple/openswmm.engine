// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "MeshHeat.hpp"
#include <cstdint>
#include <utility>
namespace openswmm::detail {class CompletedSourceArchive;}
namespace openswmm::twoD {
struct ChannelGroundContact {
    int link=-1,cell=-1,layer=-1;
    double begin=0,end=0,share=1,invert=0;
};
struct SurfaceThermalSample {double mass=0,volume=0,evaporated=0;};
// All routed water owners sharing a soil column join one finite solve. The
// router owns this state; the CPU marcher invokes it on its source clock.
class ChannelGroundState {
public:
    void clear(){*this={};}
    bool active() const noexcept {return !contacts_.empty();}
    bool ownsCell(int i) const noexcept {return i>=0&&std::size_t(i)<cell_owned_.size()&&cell_owned_[i];}
    bool ownsLink(int i) const noexcept {return i>=0&&std::size_t(i)<link_owned_.size()&&link_owned_[i];}
    const std::vector<int>& cells() const noexcept {return cells_;}
    const std::vector<ChannelGroundContact>& contacts() const noexcept {return contacts_;}
    std::size_t contactCount(int link) const noexcept {return ownsLink(link)?contact_ranges_[link].second-contact_ranges_[link].first:0;}
    // Capacity-weighted temperature of the actual soil beneath this reach.
    // This is a view of existing soil energy, including while the reach is dry.
    double bedTemperature(int link,double begin=0,double end=1) const noexcept;
    std::string initialize(SimulationContext&,const MeshData&,GroundHeatState&,SubsurfaceTransportState&,SurfaceStateData&,double coordinate_to_si);
    // Samples are already mixed, before intensive sinks. Returned surface
    // temperature-volume values are published by the caller at that boundary.
    std::string advance(const SolverOptions2D&,const std::vector<SurfaceThermalSample>&,
                        double seconds,std::vector<double>& surface_mass);
    const std::vector<double>& channelAtmosphere() const noexcept {return channel_atmospheric_j_;}
    const std::vector<double>& channelToGround() const noexcept {return channel_ground_j_;}
    const std::vector<double>& channelSpeciesToGround() const noexcept {return channel_species_to_ground_;}
    const std::vector<double>& channelThermalIncrement() const noexcept {return channel_increment_j_;}
    std::string modelIdentity() const;
    void checkpoint(openswmm::detail::CompletedSourceArchive&);
private:
    SimulationContext* context_=nullptr;
    const MeshData* mesh_=nullptr;
    GroundHeatState* ground_=nullptr;
    SubsurfaceTransportState* transport_=nullptr;
    SurfaceStateData* surface_=nullptr;
    std::vector<ChannelGroundContact> contacts_;
    std::vector<uint8_t> cell_owned_,link_owned_;
    std::vector<int> cells_;
    std::vector<std::pair<std::size_t,std::size_t>> contact_ranges_;
    std::vector<double> channel_atmospheric_j_,channel_ground_j_,channel_increment_j_,channel_species_to_ground_;
};
} // namespace openswmm::twoD
