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
 * @file HeatFluxes.hpp
 * @brief Plan D-H5e — the single node/link surface-flux binding.
 *
 * @details **One traversal, one net flux, one relaxation.** Every enabled
 *          flux family contributes to `J(T)`; the element is then stepped
 *          once.
 *
 * @par The defect this exists to make unrepresentable
 *      H2 and H3 each owned a binding: `applySurfaceExchange` then
 *      `applyRadiativeExchange`, called back to back from `routeLegacyHeat`.
 *      Under forward Euler that was harmless — the two increments were
 *      linear and added exactly. D-H5d replaced the step with an exponential
 *      relaxation, and relaxations **do not commute**: each sub-step relaxes
 *      FULLY toward its own module's equilibrium, so the pair overshoots the
 *      true combined one and the result depends on which module ran last.
 *      Measured, two equal modules with equilibria at 30 °C and 10 °C, true
 *      combined 20 °C, from 5 °C:
 *
 *      | k·dt   | split    | combined |
 *      |--------|----------|----------|
 *      | 4.1e-3 | 5.061317 | 5.061359 |
 *      | 0.41   | 9.700850 | 10.044256|
 *      | 39.4   | 10.000000| 20.000000|
 *
 *      At large `k·dt` the split lands exactly on the last module's
 *      equilibrium — the first module's contribution has been erased.
 *
 *      **The general lesson, worth more than this instance:** replacing an
 *      integrator underneath an existing operator split silently changes
 *      what the split means. Forward Euler's linearity was load-bearing and
 *      nobody had written that down.
 *
 * @par Why a separate file
 *      The traversal belongs to neither module. It lived in both — H2's copy
 *      and H3's near-identical copy — which is also how H3 came to carry
 *      H2's divergence through a different spelling. A SURFACE flux family
 *      is one line in `netFluxOut` here and cannot acquire a binding of its
 *      own — but that sentence once claimed H6's SEDIMENT_EXCHANGE too, and
 *      it was wrong: the bed acts on the wetted perimeter (a different area,
 *      nonzero exactly when the free surface is zero) and adds a SECOND state
 *      variable, so `relaxT`'s fixed equilibrium does not exist for it. H6b
 *      therefore steps the water/bed pair as ONE coupled relaxation
 *      (`BedExchange.hpp`) inside this file's link loop — coupled rather than
 *      sequential for exactly the D-H5e reason above.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6.2 D-H5d, §6.3 D-H5e
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_HEAT_FLUXES_HPP
#define OPENSWMM_ENGINE_TRANSPORT_HEAT_FLUXES_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport::heat {

/**
 * @brief Net outward surface flux at `t_w`, summed over every enabled
 *        module [W/m²].
 *
 * @details Each module's evaluator returns 0 when its own `[HEAT_FLUXES]`
 *          toggle is off, so this sums unconditionally and no caller has to
 *          know which families exist. **This is the extension point**: a new
 *          flux family is one added term here and nowhere else.
 *
 *          Shared with the ARD mesh and watershed bindings, which have their
 *          own element geometry but must compose the flux identically — the
 *          alternative being four hand-rolled copies of the same sum, which
 *          is the duplication that produced the defect above.
 */
double netFluxOut(const SimulationContext& ctx, double t_w) noexcept;

/**
 * @brief Apply one step of surface heat exchange to every exchanging
 *        node and link.
 *
 * @details Storage nodes (`node::getSurfArea`) and open conduits (top width
 *          × length × barrels) — the engine's own evaporation surfaces, so
 *          heat crosses the free surface exactly where water leaves it.
 *          Junctions, outfalls, dividers and closed conduits have no free
 *          surface and no exchange.
 *
 *          Modifies `heat_state.node_temp` and `link_temp` in place. No-op
 *          unless HEAT_TRANSPORT is on and at least one flux module is
 *          enabled.
 */
void applyHeatFluxes(SimulationContext& ctx, double dt);

}  // namespace openswmm::transport::heat

#endif  // OPENSWMM_ENGINE_TRANSPORT_HEAT_FLUXES_HPP
