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
 * @file HeatLid.hpp
 * @brief Phase H5b — temperature through the LID layer stack.
 *
 * @details A4 built `LidLayerSpeciesState` with a species stride for exactly
 *          this. Temperature is row 1; nothing about the layer indexing
 *          changes. What differs from `WaterAgeLid` is the same three things
 *          `HeatWatershed` differs from `WaterAgeWatershed` by — no aging
 *          term, no zero floor, and a dry layer takes the D-H5c policy
 *          rather than A4's `= 0` — plus one that is new here: the column
 *          exchanges heat with itself.
 *
 * @par The thermal step is COLUMN-COUPLED, and that is the call to challenge
 *      Conduction couples adjacent layers, so it is a second operator on the
 *      same state. Lesson 80 says an operator applied sequentially after a
 *      relaxation does not compose — that is the defect D-H5e just fixed
 *      between SurfaceExchange and RadiativeExchange, and applying
 *      conduction as its own pass would reproduce it inside one phase.
 *
 *      So the whole column is stepped at once: atmospheric flux on the
 *      exposed layer, conduction on every adjacent pair, solved implicitly
 *      as one tridiagonal system.
 *
 *      Two alternatives were measured and rejected:
 *        - **Per-layer relaxation with frozen neighbours** is stable, but the
 *          energy leaving a layer is not the energy its neighbour receives,
 *          so the column leaks. On a thin surface film relaxing fully toward
 *          its equilibrium with the soil in one step, the leak is the whole
 *          exchange.
 *        - **Explicit conduction** conserves energy exactly but is stiff
 *          where it matters. A 1e-4 m surface film against a 0.3 m soil
 *          layer gives **`k·dt = 1.91` at a 60 s step** — the same regime
 *          that produced H5a's NaN. Not "probably fine": computed, with the
 *          module's own capacity and conductivity expressions. The same
 *          column with the soil layer alone gives `k·dt = 5.3e-4`, which is
 *          why conduction looks harmless until a film appears.
 *
 * @par Adjacency comes from the DONOR MAP, not a second table
 *      `WaterAgeLid`'s `donorsFor` already encodes which layer feeds which,
 *      per LID type, and for a vertical stack that IS the physical
 *      adjacency. Writing a second stacking table would be a third copy of
 *      the same topology, which is the shape of lesson 81.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H5b, §6.1 D-H5b/D-H5c
 * @see src/engine/transport/components/WaterAgeModule/WaterAgeLid.hpp
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_HEAT_LID_HPP
#define OPENSWMM_ENGINE_TRANSPORT_HEAT_LID_HPP

#include <cstddef>

namespace openswmm {
struct SimulationContext;
}
namespace openswmm::lid {
class LIDSolver;
struct LIDGroupSoA;
}

namespace openswmm::transport {

/// Size the shared layer block if needed and seed the TEMPERATURE row from
/// `HeatSource::INITIAL_STATE`. Independent of `initLidLayerAge`: either may
/// run first, neither wipes the other (`LidLayerSpeciesState::ensureSized`).
void initLidLayerTemperature(SimulationContext& ctx,
                             const lid::LIDSolver& solver);

/// Temperature of the water arriving at one unit this step, flow-weighted
/// over the same four rates `setLidInflowAge` weights. Mirrors it exactly,
/// including its hard-won choice to take the SUBCATCHMENT's published
/// runoff value rather than reconstructing a mean from the subarea rows.
void setLidInflowTemperature(SimulationContext& ctx, int type_index, int unit,
                             int subcatch, double rain_rate, double q_imperv,
                             double q_perv, double q_runon, double lid_area);

/// Advance every LID column one runoff step: advective mixing per layer,
/// then one coupled thermal solve over the stack. Publishes the underdrain
/// temperature, retiring `HeatSource::RAINFALL`'s "(and LID drains until
/// H5)" marker.
void routeLidLayerTemperature(SimulationContext& ctx,
                              const lid::LIDSolver& solver, double dt);

/**
 * @brief Heat capacity of one layer, J/m²/K over the layer's footprint.
 *
 * @details Exported so a gate can sum a column's heat content and assert
 *          conservation WITHOUT restating the water/matrix mixture — a
 *          second copy of it in the test would make the gate agree with
 *          itself rather than with the code. Zero for an absent layer.
 */
double lidLayerHeatCapacity(const SimulationContext& ctx,
                            const lid::LIDGroupSoA& g, std::size_t unit,
                            int layer) noexcept;

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_HEAT_LID_HPP
