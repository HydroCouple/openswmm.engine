// SPDX-License-Identifier: Apache-2.0

/**
 * @file      GroundwaterConcentration.hpp
 * @brief     Conversion of groundwater species inventories to reported concentrations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GROUNDWATER_CONCENTRATION_HPP
#define OPENSWMM_GROUNDWATER_CONCENTRATION_HPP
#include "SubsurfaceSolver.hpp"
#include <algorithm>
namespace openswmm::twoD {
// Legacy BULK reports total inventory per water volume. The explicit LAYERED
// profile reports a capacity-weighted dissolved concentration for both zones.
/**
 * @brief Derive a reported bulk-zone concentration using the active transport ownership model.
 * @param[in] gw Borrowed initialized aquifer with stable SI water stores, parameters and species
 * arrays.
 * @param[in] cell Valid zero-based covered cell index, unchecked.
 * @param[in] species Valid zero-based species row, unchecked.
 * @param[in] saturated True selects saturated water/inventory; false selects the derived
 * unsaturated bulk sum.
 * @return Native species concentration, age seconds or temperature degrees Celsius; zero when
 * effective capacity is nonpositive. BULK uses water volume only; LAYERED solutes add
 * Kd*rho_b*bulk-soil-volume sorption capacity, with age/temperature always water-only.
 * @pre LAYERED unsat_mass is already refreshed from layer_mass. No finite/bounds checks,
 * allocation, ownership transfer or state mutation; caller serializes with water/mass updates.
 */
inline double groundwaterZoneConcentration(const SubsurfaceSolver& gw, int cell, int species,
                                           bool saturated) {
    const auto& tr = gw.transport();
    const auto& state = gw.state();
    const auto k = tr.idx(species, cell);
    double capacity = saturated ? gw.satVolume(cell) : gw.unsatVolume(cell);
    if (tr.n_layers > 0 && species != tr.age_row && species != tr.temp_row) {
        const double thickness =
            saturated ? state.hg[cell] : std::max(0., state.zs[cell] - state.hg[cell]);
        capacity += state.area[cell] * thickness * tr.kd[k] * tr.rho_b[cell];
    }
    const double mass = saturated ? tr.sat_mass[k] : tr.unsat_mass[k];
    return capacity > 0 ? mass / capacity : 0;
}
}
#endif
