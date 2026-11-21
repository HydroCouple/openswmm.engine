// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebLumped.hpp
 * @brief     Lumped Utah Energy Balance snow model integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "UebSnow.hpp"
#include <array>

namespace openswmm::surface::ueb {
// The point state describes the snow-covered part; water is the average over
// the entire element. Curves contain the SWMM ordinates at 0, .1, ... .9,
// with an implicit final ordinate of 1. Mesh elements use the project
// pervious-area curve, matching their UEB snowpack ownership.
struct LumpedState {
    State point;
    double water = 0, maximum = 0, cover = 0;
};
struct LumpedFluxes {
    Fluxes point;
    double rain = 0, snow = 0, liquid = 0, throughfall = 0, sublimation = 0;
    double active_fraction = 0, water_residual = 0;
    // UEB's depletion adjustment preserves temperature/liquid fraction. Its
    // change in represented energy is reported separately from physical
    // forcing; it must never be disguised as numerical energy closure.
    double net_energy = 0, geometry_energy = 0, energy_residual = 0;
};
/** @brief Check a ten-point snow-cover depletion curve.
 * @param curve Ordinates for relative water 0, 0.1, ..., 0.9; implicit value at1 is1.
 * @return Empty for finite nondecreasing values in [0,1], otherwise diagnostic.
 * First ordinate need not be zero and last need not be one. Does not test whether
 * a particular positive-water state receives positive cover or mutate any values. */
std::string validateDepletion(const std::array<double, 10>& curve);
/** @brief Interpolate coverage from relative basin-average snow water.
 * @param curve Qualified finite monotone fraction ordinates from validateDepletion().
 * @param relative_water Finite basin water divided by its maximum reference depth.
 * @return Zero for ratio <= 0, one for >= 1, otherwise linear interpolation including
 * implicit endpoint 1. No curve/finite validation or output clamp. NaN input is not
 * a supported query because the interior ratio converts to an integer index. */
double depletionCover(const std::array<double, 10>& curve, double relative_water) noexcept;
/** @brief Prepare a basin-average water inventory and covered-area point state.
 * @param[in,out] state Replaced only on normal success; prior state retained on refusal.
 * @param water Finite nonnegative whole-element water-equivalent depth metres.
 * @param point_energy Finite covered-point energy kJ/m2, assigned directly rather
 * than divided by cover. Physical consistency follows the point model/caller.
 * @param maximum Finite nonnegative maximum/reference basin SWE metres. Nonempty
 * water raises it to at least water; empty water forces reference zero.
 * @param curve Valid depletion ordinates; nonempty water must yield positive cover.
 * @return Empty on success, otherwise curve/inventory/zero-cover diagnostic. Creates
 * a fresh default point, water=basin/cover and energy as supplied; other point history
 * is reset. No Parameters/site/thermal equilibrium validation or time advancement. */
std::string initializeLumped(LumpedState& state, double water, double point_energy, double maximum,
                             const std::array<double, 10>& curve);
/** @brief Trial one UEB point interval then reconcile basin cover and represented energy.
 * @param[in,out] state Consistent finite nonnegative water/maximum metres, cover [0,1],
 * positive cover for nonempty water and point.water*cover agreeing with basin water.
 * Owns covered-point thermal history. Publication replaces it only after normal success.
 * @param parameters Qualified point UEB physical/numerical parameters; advance validates them.
 * @param site Qualified point site forcing/pressure/forest configuration.
 * @param forcing Held interval point forcing in UEB units, positive seconds; passed
 * unchanged to the point kernel. Precipitation partition follows that kernel.
 * @param curve Finite nondecreasing depletion fractions, checked each call.
 * @param[out] output Assigned on success: whole-element integrated water depths m,
 * point diagnostics and kJ/m2 forcing/geometry-energy accounts; retained on refusal.
 * @return Empty on success, otherwise curve/state/point/conservation diagnostic.
 * Fresh snowfall with positive point-water growth acts over full area; other changes
 * act over prior cover, with uncovered precipitation as throughfall. Coverage remaps
 * point water while preserving bulk temperature/liquid fraction where applicable;
 * geometry_energy separately records that representation change, not numerical
 * physical forcing. Full-cover path keeps point arithmetic. Checks basin water
 * closure and finite geometry energy; caller handles exceptions and external state
 * serialization. Does not update a source clock or settle external runoff water. */
std::string advanceLumped(LumpedState& state, const Parameters& parameters, const Site& site,
                          const Forcing& forcing, const std::array<double, 10>& curve,
                          LumpedFluxes& output);
} // namespace openswmm::surface::ueb
