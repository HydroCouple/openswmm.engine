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
    // `reserved_out` is a non-inventory overlay for accepted face packets
    // that have not yet reached this cell's gather. Finite contacts may use
    // only mass - reserved_out; the canonical mass still owns the queued
    // packet until its existing gather consumes it.
    std::vector<double> mass,retention,initial_mass,contact_mass,reserved_out,generated_age;
    bool active() const noexcept {return n_species>0;}
    std::string initialize(const PorousHeatColumn&,const std::vector<double>& saturated_water,
        const std::vector<double>& saturated_mass,const std::vector<double>& unsaturated_mass,
        const std::vector<double>& kd,int age,int temperature);
    std::pair<std::vector<double>,std::vector<double>> zoneMasses(const PorousHeatColumn&,
        const std::vector<double>& saturated_water) const;
    std::pair<double,double> zoneMass(const PorousHeatColumn&,const std::vector<double>& saturated_water,int species) const noexcept;
    bool generateAge(const PorousHeatColumn&,double seconds) noexcept;
    double contactAvailableMass(int species,std::size_t layer) const noexcept;
    bool canReserveOutgoing(int species,std::size_t layer,double amount) const noexcept;
    bool reserveOutgoing(int species,std::size_t layer,double amount) noexcept;
    bool settleOutgoingReservation(int species,std::size_t layer,double amount) noexcept;
    bool canApplyContactMassIncrement(int species,std::size_t layer,double increment) const noexcept;
    bool applyContactMassIncrement(int species,std::size_t layer,double increment) noexcept;
};
} // namespace openswmm::twoD
