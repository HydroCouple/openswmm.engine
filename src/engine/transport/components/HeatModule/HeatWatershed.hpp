// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file HeatWatershed.hpp
 * @brief Phase H5a — temperature on subcatchment surfaces.
 *
 * @details This is A3's `WaterAgeWatershed` with the same complete-mix
 *          bookkeeping and three deliberate differences, which are the
 *          whole content of the file:
 *
 *          1. **No aging term.** Age gains `+dt` every step because that is
 *             what age IS. Temperature gains nothing from the passage of
 *             time; it changes only by mixing and by flux.
 *          2. **No zero floor.** `WaterAgeWatershed` clamps with
 *             `std::max(a, 0.0)` because a negative age is meaningless. A
 *             negative temperature is ordinary — this is the same difference
 *             `HeatLegacy` makes against `WaterAgeLegacy`.
 *          3. **A dry subarea is not zero.** A4 writes 0 for a layer holding
 *             no water ("no water, no age"); 0 °C is a real temperature, so
 *             the answer is the deck's `DryTempPolicy` (plan D-H5c).
 *
 * @par Why the surface energy balance is here and not in H6
 *      The H5 plan line scoped "temperature states" but its verify criterion
 *      was a *runoff temperature equilibration* test, which advection alone
 *      cannot produce: without atmospheric exchange a ponded subarea holds
 *      the rain temperature forever. D-H5a resolved that by pulling the
 *      balance forward. The flux modules themselves are H2's and H3's,
 *      unchanged — only the binding is new.
 *
 * @par The exchange area, and the trap in it
 *      Heat crosses the free surface exactly where evaporation does (H2's
 *      precedent). On a subcatchment that surface is the ponded subarea:
 *      `RunoffSoA::area × frac[k]`.
 *
 *      `RunoffSoA::area` is **not** `ctx.subcatches.area`, and the
 *      difference is bigger than it looks. `Runoff.cpp:197-199` builds it as
 *      `ctx.subcatches.area / ucf_area − total_lid_area_ft2`: the SoA row is
 *      **ft²**, the context row is in the deck's **user area units** —
 *      acres in US customary, a factor of 43560. The LID footprint is the
 *      second, far smaller correction. Substituting `ctx.subcatches.area`
 *      here was measured at 14.34 °C against 12.95 °C on a LID deck, with
 *      the ponded volume ledger collapsing from 27342 ft³ to 0.78 ft³.
 *
 *      Note what does NOT go wrong: a temperature is intensive, and the
 *      exchange area cancels exactly against the thermal mass in `deltaT`
 *      (`a_ft2 / v_old ≡ 1 / depth_prev`), so the flux term alone cannot
 *      see the substitution. It survives only through
 *      `runon_depth_rate = runon_rate / area`, which converts a run-on flow
 *      into the depth rate that weights it against rainfall. That is the
 *      one place the number has to be right, and it is not where the
 *      double-counting argument would look.
 *
 * @par Clocks
 *      This runs on the RUNOFF clock, immediately after `runoff_.execute`,
 *      because that is when the depths it reads are current. H2's node and
 *      link bindings run on the ROUTING clock inside `routeLegacyHeat`.
 *      Both are correct; the `dt` each passes is its own, and conflating
 *      them would scale every flux by the wrong interval.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H5a, §6.1 D-H5a/D-H5c
 * @see src/engine/transport/components/WaterAgeModule/WaterAgeWatershed.hpp
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_HEAT_WATERSHED_HPP
#define OPENSWMM_ENGINE_TRANSPORT_HEAT_WATERSHED_HPP

namespace openswmm {
struct SimulationContext;
}
namespace openswmm::runoff {
struct RunoffSoA;
}

namespace openswmm::transport {

/**
 * @brief Accumulate run-on temperature from `donor_sc` onto `receiver_sc`.
 *
 * @details The RATE convention every loader uses: donors add `q · T`, the
 *          consumer divides by the total rate. Mirrors `addRunonAge`, and
 *          must be called at **every** contributor to `runon_inflow` — the
 *          subcatchment cascade, the LID underdrain return, and the outfall
 *          return. A3 filled this numerator from one of the three and
 *          divided by all three, which produced arriving water younger than
 *          anything entering the model. The temperature analogue of that
 *          defect is quieter and worse: a missing contributor pulls the
 *          arriving temperature toward 0 °C, which is a plausible number.
 *
 * @param q Volumetric flow rate, ft³/s.
 */
void addRunonTemperature(SimulationContext& ctx, int donor_sc,
                         int receiver_sc, double q);

/**
 * @brief Accumulate run-on at an explicitly supplied temperature.
 *
 * @details For contributors whose donor is not another subcatchment: the
 *          outfall return (which carries `heat_state.node_temp`) and, from
 *          H5b, the LID underdrain. Adds to the numerator **and** the rate
 *          together — the pairing is why this is one function and not two
 *          array writes at each call site.
 *
 * @param q      Volumetric flow rate, ft³/s.
 * @param temp_c Temperature of that flow, °C.
 */
void addRunonTemperatureAt(SimulationContext& ctx, int receiver_sc, double q,
                           double temp_c);

/**
 * @brief Advance ponded subarea temperatures one runoff step.
 *
 * @details Per subarea, in order: apply the surface energy balance to the
 *          water that was already there, mix in what arrived by GROSS
 *          inflow volume, then publish the subcatchment's runoff
 *          temperature as the volume-weighted mean of the subareas holding
 *          water. Outflow leaves at the subarea's own temperature, so only
 *          the inflow is needed — the same complete-mix argument A3 arrived
 *          at after its net-gain estimate proved to be a 6.3× defect.
 *
 * @param soa The solver's own SoA. `ctx.subcatches.ponded_depth` is declared
 *            but written by nobody; reading it would give zeros.
 * @param dt  Runoff timestep, seconds.
 */
void routeSubcatchmentTemperature(SimulationContext& ctx,
                                  const runoff::RunoffSoA& soa, double dt);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_HEAT_WATERSHED_HPP
