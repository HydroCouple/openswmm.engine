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
 * @file ReactionLegacyBinding.hpp
 * @brief Phase R4 — reaction binding for the LEGACY (CSTR) quality engine.
 *
 * @details When a reactions component is configured:
 *          - Pollutant kdecay upgrades from the legacy linearized factor
 *            `(1 − k·dt)` to the EXACT exponential `exp(−k·dt)` at nodes
 *            and links (the "kdecay-as-RATE" semantics realized in closed
 *            form — pollutant decay is exactly first-order, so the
 *            integrator would only approximate what the exponential IS).
 *            Bit-parity holds when NO reactions component is present: the
 *            legacy applyDecay / in-mix decay paths are untouched then.
 *          - MSX species evolve per element (nodes: tank scope; links:
 *            pipe scope) via ReactionIntegrator, with the element's
 *            pollutant concentrations exposed READ-ONLY to their
 *            expressions (PUSH_POLLUT). MSX species are NOT yet transported
 *            between elements under LEGACY (R4b) — a RATE MSX species
 *            triggers a once-per-run warning. EQUIL/FORMULA MSX species
 *            are fully meaningful (element-local algebra over pollutants).
 *          - Integration failures are LOUD but not fatal: the element's
 *            block is left unchanged and a once-per-run warning carries
 *            the element id and the integrator's remedy text.
 *
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R4
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTION_LEGACY_BINDING_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTION_LEGACY_BINDING_HPP

#include <limits>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// True when the reactions component is active (configured + compiled) —
/// the QualitySolver's branch condition.
bool legacyReactionsActive(const SimulationContext& ctx);

/// L3: lazily size + seed the MSX element state (GLOBAL fill +
/// [REACTION_QUALITY] overrides). Shared by the LEGACY dispatch and the
/// LARD engine's init — one seeding spelling for both engines.
void ensureMsxState(SimulationContext& ctx);

/// L3: integrate ONE species block in place over dt through the shared
/// integrator. `pollut` may be null (no pollutant context); `tank`
/// selects tank-scope expressions; `hrt_seconds` feeds RxHydVar::HRT and
/// `temp_c` feeds RxHydVar::TEMP (NaN ⇒ the [REACTION_OPTIONS]
/// TEMPERATURE constant). On an integrator failure the block is left
/// unchanged and the once-per-run failure warning fires — the
/// reactElements contract.
void reactSpeciesBlock(SimulationContext& ctx, bool tank, double dt,
                       double* species_block, const double* pollut,
                       double hrt_seconds,
                       double temp_c = std::numeric_limits<double>::quiet_NaN());

/// Node-side step: exact-exponential pollutant decay + MSX tank-scope
/// integration. Runs where applyDecay() ran (before updateLinkQuality).
void reactLegacyNodes(SimulationContext& ctx, double dt);

/// Link-side step: exact-exponential pollutant decay + MSX pipe-scope
/// integration. Runs AFTER updateLinkQuality() (whose internal linear decay
/// is zeroed when reactions are active), so the reacted concentrations are
/// not overwritten by the mixing pass.
void reactLegacyLinks(SimulationContext& ctx, double dt);

/// Warn (once, at open) when a reactions component is configured but no
/// engine will run it this simulation. As of E4/R6 the only remaining
/// bypass is IGNORE_QUALITY — EULERIAN_ARD now runs its own binding
/// (ReactionArdBinding) with MSX species transported on the mesh. Without
/// this a user who wrote a .rxn file gets a run in which nothing reacted
/// and nothing said so.
void warnIfLegacyBindingBypassed(SimulationContext& ctx);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_REACTION_LEGACY_BINDING_HPP
