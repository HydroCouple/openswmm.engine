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
 * @file MsxLegacyTransport.hpp
 * @brief Phase R4b (transport half) — MSX species advect under LEGACY.
 *
 * @details R4 gave MSX species per-element STATE and per-element REACTION
 *          under the LEGACY dispatch, but nothing moved that state between
 *          elements: a species injected via `[REACTION_QUALITY]` upstream
 *          reacted in place and never arrived downstream, and a warning
 *          said so at open. This is the mirror that retires that warning.
 *
 * @par Provenance
 *      Line for line, `routeLegacyHeat` (`HeatLegacy.cpp`) — the CSTR
 *      mirror family A1b established: snapshot old state, accumulate
 *      `q · c_old` into each downstream node, mix as
 *      `(c_old·v_old + mass_in)/(v_old + v_in)` with the two-sided clamp,
 *      then the `updateLinkQuality` link branches with the DW `q_in`
 *      correction. Differences from the heat mirror, each deliberate:
 *      - **No external-load pathway.** Species take no `[INFLOWS]`; their
 *        only sources are initial state and reactions (L3's own note).
 *        The `node_temp_vol_in` term has no analogue here.
 *      - **No aging and no decay.** Reaction kinetics belong to the shared
 *        integrator (`reactLegacyNodes/Links`), which runs in its own
 *        stage; re-applying anything here would double-count, which is the
 *        trap the L3 record names for LARD's stage 4/4b split.
 *      - **No evaporation factor**, matching the age and heat mirrors
 *        rather than the pollutant path. Recorded divergence: during
 *        evaporation an MSX concentration will not up-concentrate. The
 *        pollutant path's own evap factor was found CREATING mass at
 *        draining nodes (KD1), so the omission is the safer side.
 *
 * @par Ordering
 *      Runs beside the age and heat mirrors at the END of
 *      `QualitySolver::execute` — after `reactLegacyNodes/Links`, so a
 *      RATE species reacts against this step's pre-transport state and is
 *      then advected (Lie split, the roadmap's lesson-13 convention). With
 *      no reaction system configured this function returns before touching
 *      anything, so pollutant-only models are bit-identical by
 *      construction.
 *
 * @see plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md (R4b)
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_MSX_LEGACY_TRANSPORT_HPP
#define OPENSWMM_ENGINE_TRANSPORT_MSX_LEGACY_TRANSPORT_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// Advect the MSX element state one routing step on the LEGACY CSTR path.
/// No-op unless a reaction system with at least one species is configured.
void routeLegacyMsx(SimulationContext& ctx, double dt);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_MSX_LEGACY_TRANSPORT_HPP
