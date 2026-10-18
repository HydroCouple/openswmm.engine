// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "PorousHeat.hpp"
#include <utility>
namespace openswmm::twoD {
// Species-major canonical mass in the existing fixed physical soil layers.
// Water and geometry are borrowed from the thermal/hydraulic owner. Bulk
// SAT/UNSAT masses become derived views, never an additional reservoir.
struct GroundPorewaterColumn {
    int n_species=0,age_row=-1,temp_row=-1;
    std::vector<double> mass,retention,initial_mass,contact_mass,generated_age;
    bool active() const noexcept {return n_species>0;}
    std::string initialize(const PorousHeatColumn&,const std::vector<double>& saturated_water,
        const std::vector<double>& saturated_mass,const std::vector<double>& unsaturated_mass,
        const std::vector<double>& kd,int age,int temperature);
    std::pair<std::vector<double>,std::vector<double>> zoneMasses(const PorousHeatColumn&,
        const std::vector<double>& saturated_water) const;
    bool generateAge(const PorousHeatColumn&,double seconds) noexcept;
};
} // namespace openswmm::twoD
