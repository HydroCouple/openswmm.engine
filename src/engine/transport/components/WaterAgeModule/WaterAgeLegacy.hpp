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
 * @file WaterAgeLegacy.hpp
 * @brief Phase A1b — the LEGACY (CSTR) engine's water-age mirror.
 *
 * @details A single-scalar rerun of the CSTR quality stages for the
 *          reserved age quantity, in the same order the pollutant arrays
 *          take (water age plan §5, LEGACY row):
 *
 *          1. **Aging**: node and link ages advance by dt ("age advances
 *             by dt then mixes volume-weighted" — plan §1).
 *          2. **Link-load accumulation** (accumulateLinkLoads mirror):
 *             each flowing link contributes `q · age_link` to its
 *             downstream node; the loaders' per-source
 *             `node_age_vol_in` rates add on top. The VOLUME denominator
 *             is the same `qual_vol_in` the pollutant stages accumulated
 *             — this runs at the END of QualitySolver::execute, after
 *             those totals are complete.
 *          3. **Node mixing** (mixAtNodes mirror): the identical
 *             volume-balance formula and max-principle clamp. The
 *             EVAPORATION concentration factor is deliberately SKIPPED:
 *             per the plan's §8 proposal, evaporation removes volume at
 *             the parcel's current age, leaving the volume-intensive
 *             mean age unchanged.
 *          4. **Link update** (updateLinkQuality mirror with k = 0 and no
 *             evap factor): STEADY assigns the upstream node age;
 *             no-flow retains; zero-volume takes upstream; otherwise the
 *             legacy volume-balance mix with the DW q_in correction.
 *
 *          The mirror reads pollutant-side arrays (flows, volumes,
 *          qual_vol_in) and writes ONLY water_age_state — WATER_AGE ON
 *          leaves every pollutant trajectory bit-identical under LEGACY.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §5, §7 A1
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_LEGACY_HPP
#define OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_LEGACY_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// One LEGACY age routing step (call at the END of QualitySolver::execute,
/// after qual_vol_in is fully accumulated). No-op when WATER_AGE is off.
void routeLegacyAge(SimulationContext& ctx, double dt);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_LEGACY_HPP
