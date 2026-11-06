// SPDX-License-Identifier: Apache-2.0

/**
 * @file      PorewaterTransport.hpp
 * @brief     Transport of species in fixed physical porewater layers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "../subsurface/LayerTransport.hpp"
#include <string>
#include <vector>
namespace openswmm::twoD {
struct PorewaterAdvection {
    std::string error;
    std::vector<double> mass, concentration, withdrawn_mass, vertical_water, vertical_mass;
    double external_mass = 0, water_residual = 0;
};
// Exclusive caller-owned workspace. No input may alias writable scratch/result
// storage. Borrowed mutable results expire on reuse; owning APIs remain independent.
struct PorewaterTransportScratch {
    PorewaterAdvection result;
    LayerTransportScratch transport;
    std::vector<double> capacity, initial, diagonal, lower, upper, rhs, water;
    std::vector<unsigned char> residue;
    std::vector<LayerTransportMove> moves;
};
PorewaterAdvection& advectPorewater(
    const std::vector<double>& old_water, const std::vector<double>& new_water,
    const std::vector<double>& retention, const std::vector<double>& mass,
    const std::vector<double>& external_water, const std::vector<double>& external_mass,
    const std::vector<double>& withdrawal, PorewaterTransportScratch& scratch);
PorewaterAdvection& advectPorewaterEuler(
    const std::vector<double>& old_water, const std::vector<double>& retention,
    const std::vector<double>& mass, const std::vector<double>& water_change,
    const std::vector<double>& external_water, const std::vector<double>& external_mass,
    const std::vector<double>& withdrawal, const std::vector<double>& reserved_water,
    const std::vector<double>& reserved_mass, PorewaterTransportScratch& scratch);
// Fixed physical layers, native concentration*m3 mass. Retention is the
// equilibrium sorption capacity rho_bulk*Kd*bulk_volume (m3), zero for age.
// External water is NET accepted water; external mass excludes the unknown
// implicit withdrawals. Evaporation can remove water without removing solute.
// Zero-water stores retain dry residue; throughflow needs no invented volume.
// Refusal never mutates any input or publishes a partial mass solution.
/**
 * @brief Solve conservative implicit fixed-layer porewater advection without mutating inputs.
 * @par Argument contract All seven vectors have the same nonzero layer count.
 * old_water/new_water/retention are finite nonnegative m3 per layer; mass is finite nonnegative
 * native_concentration*m3. external_water is finite signed accepted net water m3 and
 * external_mass its paired signed inventory excluding implicit_withdrawal. That final vector is
 * nonnegative outgoing m3 per layer, solved at the new dissolved concentration. Each initial
 * mass plus external receipt must remain nonnegative.
 * @return Owned final inventories, concentrations, withdrawal/interior receipts and water
 * residual; empty error indicates success. Refusal publishes no partial mass solution. Water
 * balance tolerance is 1e-10 transfer scale plus 1e-12 m3; zero-capacity dry residue remains
 * immobile. Allocation may throw.
 */
PorewaterAdvection
advectPorewater(const std::vector<double>& old_water, const std::vector<double>& new_water,
                const std::vector<double>& retention, const std::vector<double>& mass,
                const std::vector<double>& external_water, const std::vector<double>& external_mass,
                const std::vector<double>& implicit_withdrawal);
// Beginning-state Euler stage with accepted physical water increments.
// Reserved producer packets cannot supply another export. concentration
// reports the accepted withdrawal composition when a donor budget binds.
/**
 * @brief Compute explicit accepted physical-layer transport with reserved-packet donor budgets.
 * @par Argument contract All required vectors have the same nonzero layer count:
 * old_water/retention are nonnegative m3, mass nonnegative native_concentration*m3;
 * water_change/external_water are signed accepted m3, external_mass signed paired inventory;
 * withdrawal is nonnegative outgoing m3. Values must be finite. Optional
 * reserved_water/reserved_mass are empty or same-sized, nonnegative and bounded by beginning
 * water/mass, respectively. They remain canonical but cannot supply another export.
 * @return Owned final masses, donor/accepted-withdrawal concentrations, withdrawal/interior
 * receipts, net external inventory and water residual, or a nonempty refusal error with no
 * partial solution. All inputs remain unchanged. Uses beginning-state Euler transport and water
 * closure tolerance 1e-10 transfer scale plus 1e-12 m3. Allocation failures propagate.
 */
PorewaterAdvection advectPorewaterEuler(
    const std::vector<double>& old_water, const std::vector<double>& retention,
    const std::vector<double>& mass, const std::vector<double>& water_change,
    const std::vector<double>& external_water, const std::vector<double>& external_mass,
    const std::vector<double>& withdrawal, const std::vector<double>& reserved_water = {},
    const std::vector<double>& reserved_mass = {});
} // namespace openswmm::twoD
