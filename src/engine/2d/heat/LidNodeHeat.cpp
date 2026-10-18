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
} // namespace openswmm::twoD
