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
 * @file LagrangianSolver.hpp
 * @brief LARD (Lagrangian ARD) quality engine — X1 wiring skeleton.
 *
 * @details Subplan X1 (plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md;
 *          strategy plans/LAGRANGIAN_QUALITY_STRATEGY.md §12 Phase 0): the
 *          `QUALITY_SOLVER LAGRANGIAN` dispatch exists, transport does not.
 *          The skeleton's one behavior is honesty: it zeroes the published
 *          node/link concentrations (current AND old — the .out writer's
 *          old/new interpolation is live, lesson 39) every routing step, so
 *          a LARD run can never present frozen `Cinit` seeds or stale state
 *          as results. The matching open()-time warning lives in
 *          `SWMMEngine::open` beside `warnIfLegacyBindingBypassed` — silent
 *          no-quality runs are never allowed (the E1-era rule).
 *
 *          Header-only on purpose: `src/engine/CMakeLists.txt` globs
 *          sources without `CONFIGURE_DEPENDS`, so a changeset applied by
 *          patch that adds a .cpp silently does not compile (IO1 carried
 *          obligation (c)). X2 adds the real SegmentStore implementation
 *          and its .cpp files with an explicit reconfigure step in its
 *          handoff.
 *
 *          Deliberately NOT here (X2+): SegmentStore (D-L2 ring-buffer
 *          slabs), LTD advection, junction toposort mixing, storage CMSTR,
 *          decay, D-NS1 negative-source clamp, RWPT (X3), water age (X4).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
#define OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP

#include <algorithm>

#include "../../core/SimulationContext.hpp"

namespace openswmm {
namespace lard {

/**
 * @brief X1 no-op LARD engine: publishes zero quality, transports nothing.
 */
class LagrangianSolver {
public:
    /**
     * @brief One routing step under `QUALITY_SOLVER LAGRANGIAN`.
     *
     * @details Zeroes published pollutant state so the run reads as "no
     *          transport implemented" rather than as frozen initial
     *          quality. Reserved-species state (water age, temperature) is
     *          not touched here: its engines simply do not run under the
     *          LAGRANGIAN dispatch, so it stays at initial values — the
     *          open() warning names all three families. Subcatchment
     *          washoff is runoff-stage state and is left as computed.
     */
    void step(SimulationContext& ctx, double /*dt_routing*/) {
        std::fill(ctx.nodes.conc.begin(), ctx.nodes.conc.end(), 0.0);
        std::fill(ctx.nodes.conc_old.begin(), ctx.nodes.conc_old.end(), 0.0);
        std::fill(ctx.links.conc.begin(), ctx.links.conc.end(), 0.0);
        std::fill(ctx.links.conc_old.begin(), ctx.links.conc_old.end(), 0.0);
    }
};

}  // namespace lard
}  // namespace openswmm

#endif  // OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
