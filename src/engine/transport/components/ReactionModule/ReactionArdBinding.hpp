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
 * @file ReactionArdBinding.hpp
 * @brief Phase E4/R6 — reaction binding for the Eulerian ARD engine.
 *
 * @details One reaction stage per ROUTING step on the ARD mesh state,
 *          Lie-split after the advection–dispersion subcycle (first-order
 *          splitting, the documented decision of roadmap lesson 13: the
 *          integrator substeps adaptively inside the stage, routing steps
 *          are short relative to typical kinetics, and the projection error
 *          dominates — revisit if the LEGACY-vs-ARD convergence gate says
 *          otherwise):
 *
 *          1. **Pollutant kdecay as the exact exponential** on every cell
 *             (concentration) and node store (mass) — the closed form of
 *             the implicit first-order RATE the plan's E4 row names. This
 *             runs whether or not a reactions component is configured,
 *             retiring the E1 "kdecay not yet applied" warning.
 *          2. **MSX species integrate per cell** (pipe scope) and **per
 *             node store** (tank scope, HRT populated) via
 *             ReactionIntegrator, with the element's pollutant
 *             concentrations readable (PUSH_POLLUT). FORMULA sees
 *             post-decay pollutants — the R4 ordering, kept identical so
 *             the two bindings agree at the CSTR limit.
 *
 *          MSX species are TRANSPORTED on the mesh under this engine (R6):
 *          the ARD state carries n_pollutants + n_msx species rows, so the
 *          R4b element-local limitation does not apply here. WALL species
 *          have no transport semantics yet — a model with WALL species
 *          under EULERIAN_ARD falls back to LEGACY with a warning.
 *
 *          Failure containment matches R4: a failed integration leaves the
 *          element's block unchanged and warns once per run with the
 *          element kind/index and the integrator's remedy text.
 *
 * @see plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md §6 E4
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R6
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_REACTION_ARD_BINDING_HPP
#define OPENSWMM_ENGINE_TRANSPORT_REACTION_ARD_BINDING_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// True when the reactions component is active (configured + compiled) —
/// the ARD engine's condition for carrying MSX species on the mesh.
bool ardReactionsActive(const SimulationContext& ctx);

/// True when any declared MSX species is WALL — unsupported under the ARD
/// engine (no transport semantics for attached species yet); the engine
/// falls back to LEGACY, whose R4 binding runs them element-locally.
bool ardHasWallSpecies(const SimulationContext& ctx);

/**
 * @brief The E4 reaction stage over one routing step.
 *
 * @param cell_phi  ARD cell state, species-major [s * n_cells + c]; rows
 *                  0..n_pollut-1 are pollutants, rows n_pollut.. are the
 *                  MSX species in ReactionData order.
 * @param cell_a    Cell areas (read-only; dry cells still react — their
 *                  carried concentration evolves like LEGACY's, and holds
 *                  negligible mass).
 * @param node_mass Node-store species MASS [nd * ns_total + s].
 * @param node_vol  Node-store water volume [nd].
 * @param n_pollut  Pollutant row count (may be 0 — MSX-only model).
 * @param ns_total  Total species rows (n_pollut + rx.n_species() when the
 *                  component is active, else n_pollut).
 * @param min_store_vol Below this volume a store has no meaningful
 *                  concentration: MSX tank integration is skipped there
 *                  (mass-form kdecay still applies).
 */
void reactArdStage(SimulationContext& ctx, double dt, double* cell_phi,
                   const double* cell_a, int n_cells, double* node_mass,
                   const double* node_vol, int n_nodes, int n_pollut,
                   int ns_total, double min_store_vol);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_REACTION_ARD_BINDING_HPP
