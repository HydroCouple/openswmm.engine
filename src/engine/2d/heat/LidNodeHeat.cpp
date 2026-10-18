// SPDX-License-Identifier: Apache-2.0
#include "LidNodeHeat.hpp"
#include <algorithm>
#include <cmath>
#include <utility>

namespace openswmm::twoD {
namespace {
constexpr double ft_to_m=.3048;
constexpr double ft2_to_m2=.09290304;
constexpr double ft3_to_m3=.028316846592;
bool positive(double x){return x>0&&std::isfinite(x);}
bool nonnegative(double x){return x>=0&&std::isfinite(x);}
}
std::string buildLidNodeThermalGraph(const LidNodeState& state,double mobile_water_ft3,
    double water_heat_capacity,const ConductionConfig& material,double initial_celsius,
    LidNodeThermalGraph& output) {
    if(state.cells.empty()||!nonnegative(mobile_water_ft3)||!positive(water_heat_capacity)||
       !positive(material.water_conductivity)||!positive(material.sed_conductivity)||
       !positive(material.sed_density)||!positive(material.sed_specific_heat)||
       !std::isfinite(initial_celsius))
        return "Invalid LID thermal geometry, phase properties or initial condition.";
    if(!state.richards && std::any_of(state.cells.begin(),state.cells.end(),[](const auto& c){return !std::isfinite(c.theta);}))
        return "Nonfinite LID thermal moisture.";
    if(state.richards && state.richards_water.size()!=state.cells.size())
        return "Richards LID thermal water shape differs from its physical layers.";
    if(state.richards && std::any_of(state.richards_water.begin(),state.richards_water.end(),
        [](double v){return !nonnegative(v);}))
        return "Nonfinite or negative Richards LID thermal water.";
    if(!state.thermal_energy.empty()&&state.thermal_energy.size()!=state.cells.size())
        return "LID thermal energy shape differs from its physical layers.";

    LidNodeThermalGraph next;std::vector<double> thickness,area,conductivity;
    thickness.reserve(state.cells.size());area.reserve(state.cells.size());conductivity.reserve(state.cells.size());
    next.physical_thickness_m.resize(state.cells.size());
    next.physical_area_m2.resize(state.cells.size());
    next.physical_conductivity.resize(state.cells.size());
    for(std::size_t i=0;i<state.cells.size();++i) {
        const auto& cell=state.cells[i];
        if(!std::isfinite(cell.top)||!std::isfinite(cell.bottom)||!(cell.top>cell.bottom)||
           !positive(cell.area)||!positive(cell.geometric_volume)||
           (cell.kind!=LidNodeLayerKind::Surface&&(!std::isfinite(cell.porosity)||cell.porosity<0||cell.porosity>1)))
            return "Invalid LID thermal cell geometry or porosity.";
        if(i>0) {
            const double scale=std::max({1.,std::abs(cell.top),std::abs(state.cells[i-1].bottom)});
            if(std::abs(cell.top-state.cells[i-1].bottom)>1e-10*scale)
                return "LID thermal cells must be ordered, contiguous physical layers.";
        }
        const double dz=(cell.top-cell.bottom)*ft_to_m;
        const double a=cell.area*ft2_to_m2;
        const double bulk=cell.geometric_volume*ft3_to_m3;
        double water_ft3=0;
        if(cell.kind==LidNodeLayerKind::Surface) {
            if(i!=0)return "LID thermal surface storage must be the first physical cell.";
            water_ft3=mobile_water_ft3;
        } else if(state.richards) water_ft3=state.richards_water[i];
        else water_ft3=cell.theta*cell.geometric_volume;
        if(!nonnegative(water_ft3)||water_ft3>cell.geometric_volume*(1+1e-10))
            return "LID thermal water exceeds its physical cell volume.";
        const double water=std::min(water_ft3,cell.geometric_volume)*ft3_to_m3;
        // Match the existing LID conduction mixture: water and the remaining
        // bulk layer share heat capacity and conductivity in proportion.
        const double solid=cell.kind==LidNodeLayerKind::Surface?0:std::max(0.,bulk-water);
        const double capacity=water*water_heat_capacity+solid*material.sed_density*material.sed_specific_heat;
        const double saturation=cell.kind==LidNodeLayerKind::Surface?1:std::clamp(water/bulk,0.,1.);
        const double lambda=saturation*material.water_conductivity+(1-saturation)*material.sed_conductivity;
        if(!std::isfinite(capacity)||capacity<0||!positive(lambda))
            return "Invalid LID thermal layer capacity or conductivity.";
        next.physical_thickness_m[i]=dz;next.physical_area_m2[i]=a;next.physical_conductivity[i]=lambda;
        thickness.push_back(dz);area.push_back(a);conductivity.push_back(lambda);
        if(capacity>0) {
            const double energy=state.thermal_energy.empty()?capacity*initial_celsius:state.thermal_energy[i];
            if(!std::isfinite(energy))return "Nonfinite LID thermal layer energy.";
            next.cells.push_back(i);next.stores.push_back({capacity,energy});
        } else if(!state.thermal_energy.empty()&&state.thermal_energy[i]!=0) {
            // A water withdrawal must carry its producer-time sensible heat;
            // dropping a now-dry surface store here would silently lose it.
            return "LID thermal energy remains in a cell with no thermal capacity.";
        }
    }
    for(std::size_t j=1;j<next.cells.size();++j) {
        const std::size_t left=next.cells[j-1],right=next.cells[j];
        if(right!=left+1)continue; // no conductive path across an absent physical layer
        // Cell areas are secant areas from the native storage curve. Linear
        // interpolation between adjacent cell centres gives the interface.
        const double face_area=(area[left]*thickness[right]+area[right]*thickness[left])/
                               (thickness[left]+thickness[right]);
        const double resistance=.5*thickness[left]/conductivity[left]+
                                .5*thickness[right]/conductivity[right];
        const double conductance=face_area/resistance;
        if(!positive(face_area)||!positive(resistance)||!positive(conductance))
            return "Invalid LID thermal contact resistance.";
        next.contacts.push_back({int(j-1),int(j),conductance});
    }
    output=std::move(next);return {};
}
std::string buildLidNodeGroundThermalContact(const LidNodeThermalGraph& graph,
    std::size_t combined_store_count,int ground_store,double overlap_area,double ground_area,double soil_thickness,
    double soil_conductivity,double interface_resistance,ThermalContact& output) {
    const auto n=graph.physical_thickness_m.size();
    if(!n||graph.cells.empty()||graph.cells.size()!=graph.stores.size()||
       graph.physical_area_m2.size()!=n||graph.physical_conductivity.size()!=n||
       graph.cells.back()!=n-1||ground_store<int(graph.stores.size())||
       ground_store<0||std::size_t(ground_store)>=combined_store_count||combined_store_count<=graph.stores.size()||
       !positive(overlap_area)||!positive(ground_area)||overlap_area>ground_area*(1+1e-12)||
       overlap_area>graph.physical_area_m2.back()*(1+1e-12)||
       !positive(soil_thickness)||!positive(soil_conductivity)||!nonnegative(interface_resistance))
        return "Invalid LID-to-ground thermal contact geometry or owner index.";
    const double resistance=.5*graph.physical_thickness_m.back()/graph.physical_conductivity.back()+
        interface_resistance+.5*soil_thickness/soil_conductivity;
    const double conductance=overlap_area/resistance;
    if(!positive(resistance)||!positive(conductance))return "Invalid LID-to-ground thermal contact resistance.";
    output={int(graph.stores.size()-1),ground_store,conductance};return {};
}
std::string publishLidNodeThermalEnergy(LidNodeState& state,const LidNodeThermalGraph& graph,
    const std::vector<double>& energy) {
    if(graph.cells.size()!=graph.stores.size()||graph.cells.size()!=energy.size())
        return "LID thermal solution shape differs from its owner graph.";
    std::vector<double> next(state.cells.size(),0);
    for(std::size_t i=0;i<graph.cells.size();++i) {
        const auto cell=graph.cells[i];
        if(cell>=next.size()||!std::isfinite(energy[i]))return "Invalid LID thermal solution energy.";
        if(i>0 && graph.cells[i]<=graph.cells[i-1])return "LID thermal graph cells are duplicated or out of order.";
        next[cell]=energy[i];
    }
    state.thermal_energy.swap(next);return {};
}
std::string transferLidNodeThermalEnergy(const LidNodeThermalGraph& graph,std::size_t cell_count,
    double water_heat_capacity,const std::vector<LidThermalWaterTransfer>& transfers,
    std::vector<double>& physical_energy,double& external_j) {
    if(graph.cells.size()!=graph.stores.size()||!positive(water_heat_capacity))
        return "Invalid LID thermal transfer graph or water heat capacity.";
    std::vector<std::size_t> store_of(cell_count,graph.cells.size());
    std::vector<double> next(cell_count,0);
    for(std::size_t i=0;i<graph.cells.size();++i) {
        const auto cell=graph.cells[i];
        if(cell>=cell_count||(i>0&&cell<=graph.cells[i-1])||
           !positive(graph.stores[i].capacity)||!std::isfinite(graph.stores[i].energy))
            return "Invalid LID thermal transfer store layout.";
        store_of[cell]=i;next[cell]=graph.stores[i].energy;
    }
    double external=0;
    for(const auto& transfer:transfers) {
        if(!nonnegative(transfer.volume_m3)||transfer.from_cell < -1||transfer.to_cell < -1||
           (transfer.from_cell<0&&transfer.to_cell<0)||transfer.from_cell==transfer.to_cell||
           (transfer.from_cell>=0&&static_cast<std::size_t>(transfer.from_cell)>=cell_count)||
           (transfer.to_cell>=0&&static_cast<std::size_t>(transfer.to_cell)>=cell_count))
            return "Invalid accepted LID thermal water transfer.";
        if(transfer.volume_m3==0)continue;
        double temperature=transfer.source_temperature_c;
        if(transfer.from_cell>=0) {
            const auto from=static_cast<std::size_t>(transfer.from_cell);
            if(from>=cell_count||store_of[from]>=graph.cells.size())
                return "LID thermal donor has no active heat capacity.";
            const auto& donor=graph.stores[store_of[from]];
            temperature=donor.energy/donor.capacity;
        } else if(!std::isfinite(temperature))
            return "Nonfinite external LID water temperature.";
        const double joules=transfer.volume_m3*water_heat_capacity*temperature;
        if(!std::isfinite(joules))return "LID thermal water receipt overflow.";
        if(transfer.from_cell>=0)next[static_cast<std::size_t>(transfer.from_cell)]-=joules;
        else external+=joules;
        if(transfer.to_cell>=0)next[static_cast<std::size_t>(transfer.to_cell)]+=joules;
        else external-=joules;
        if(!std::isfinite(external))return "LID external thermal receipt overflow.";
    }
    for(double energy:next)if(!std::isfinite(energy))return "LID thermal transfer energy overflow.";
    physical_energy.swap(next);external_j=external;return {};
}
} // namespace openswmm::twoD
