// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <string>
#include <vector>
namespace openswmm::twoD {
struct PorewaterAdvection {
    std::string error;
    std::vector<double> mass,concentration,withdrawn_mass,vertical_water,vertical_mass;
    double external_mass=0,water_residual=0;
};
// Fixed physical layers, native concentration*m3 mass. Retention is the
// equilibrium sorption capacity rho_bulk*Kd*bulk_volume (m3), zero for age.
// External water is NET accepted water; external mass excludes the unknown
// implicit withdrawals. Evaporation can remove water without removing solute.
// Zero-water stores retain dry residue; throughflow needs no invented volume.
// Refusal never mutates any input or publishes a partial mass solution.
PorewaterAdvection advectPorewater(const std::vector<double>& old_water,
    const std::vector<double>& new_water,const std::vector<double>& retention,
    const std::vector<double>& mass,const std::vector<double>& external_water,
    const std::vector<double>& external_mass,const std::vector<double>& implicit_withdrawal);
// Beginning-state Euler stage with accepted physical water increments.
// Reserved producer packets cannot supply another export. concentration
// reports the accepted withdrawal composition when a donor budget binds.
PorewaterAdvection advectPorewaterEuler(const std::vector<double>& old_water,
    const std::vector<double>& retention,const std::vector<double>& mass,
    const std::vector<double>& water_change,const std::vector<double>& external_water,
    const std::vector<double>& external_mass,const std::vector<double>& withdrawal,
    const std::vector<double>& reserved_water={},const std::vector<double>& reserved_mass={});
} // namespace openswmm::twoD
