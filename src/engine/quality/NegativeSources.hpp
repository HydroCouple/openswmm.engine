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
 * @file NegativeSources.hpp
 * @brief D-NS1: one clamp-bookkeeping seam for all three quality engines.
 *
 * @details Subplan §3.1 (user requirement, 2026-08-23): negative sources
 *          (mass extraction) are legal; extraction beyond an element's
 *          held mass clamps to the available amount, the ledger books the
 *          mass ACTUALLY removed (the shortfall is un-booked from the
 *          extraction's own row — otherwise continuity breaks by
 *          construction), the first clamp warns, and the counts are
 *          summarized at end of run. LEGACY, ARD, and LARD all call the
 *          same helper so the semantics cannot drift apart (the lesson-52
 *          family, applied prospectively).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_NEGATIVE_SOURCES_HPP
#define OPENSWMM_QUALITY_NEGATIVE_SOURCES_HPP

#include <string>

#include "../core/SimulationContext.hpp"

namespace openswmm {
namespace quality {

/**
 * @brief Book one pollutant-row clamp: count it, un-book the shortfall
 *        from the external-extraction ledger row, warn on the first.
 *
 * @param shortfall  the UNMET extraction (positive, internal mass units).
 */
inline void bookNegativeSourceClamp(SimulationContext& ctx, int node, int p,
                                    double shortfall) {
    auto& st = ctx.negsrc;
    st.clamp_events += 1;
    st.shortfall_mass += shortfall;
    // The negative request was booked (signed) into qual_routing_ex_in by
    // the loaders; the unmet part did not actually leave the water, so it
    // is returned to the row. v1 scope: EXTERNAL_INFLOW and the API flux
    // are the admitted negative pathways, and both book into ex_in —
    // negative DWF/GW/RDII concentrations remain out of scope, recorded.
    if (p >= 0 &&
        static_cast<std::size_t>(p) <
            ctx.mass_balance.qual_routing_ex_in.size())
        ctx.mass_balance.qual_routing_ex_in[static_cast<std::size_t>(p)] +=
            shortfall;
    if (!st.runtime_warned) {
        st.runtime_warned = true;
        st.first_node = node;
        ctx.warnings.push_back(
            "D-NS1: a negative source requested more mass than its element "
            "held — extraction was clamped to the available amount (first "
            "at node index " + std::to_string(node) +
            "). Total clamp counts are summarized at end of run.");
    }
}

/// Age-row clamp: counted and warned, not ledgered (no age row until A2c).
inline void bookNegativeAgeClamp(SimulationContext& ctx, int node) {
    auto& st = ctx.negsrc;
    st.age_clamp_events += 1;
    if (!st.runtime_warned) {
        st.runtime_warned = true;
        st.first_node = node;
        ctx.warnings.push_back(
            "D-NS1: a negative water-age source requested more age-volume "
            "than its element held — extraction was clamped (first at node "
            "index " + std::to_string(node) +
            "). Total clamp counts are summarized at end of run.");
    }
}

/**
 * @brief P1.4: book one ARD **cell**-source clamp. Counted and warned, NOT
 *        ledgered — and the reason is structural, not an omission.
 *
 * @details The node seam un-books its shortfall from `qual_routing_ex_in`
 *          because the loaders booked the signed request there in advance.
 *          Cell sources have no such row to correct:
 *
 *          1. `[TRANSPORT_SOURCES]` rows resolve to **MSX species rows
 *             only** — `ArdEngine.cpp` pushes `np + src_msx[i]`, so a cell
 *             source can never name a pollutant; and
 *          2. **MSX species have no mass-balance row at all.** The ARD
 *             component writes to `ctx.mass_balance` nowhere.
 *
 *          So a per-pollutant row for this would be permanently zero, and
 *          inventing an MSX row here would be a third self-consistent
 *          balance (lesson 147) bolted on inside a defect fix. Giving MSX
 *          species real ledger rows (sizing the `qual_*` vectors by
 *          `np + nm`) is a deliberate, separate round — until it lands, the
 *          delivered mass reaches `qual_routing_final` through the
 *          end-of-run inventory sweep and therefore surfaces as CONTINUITY
 *          ERROR rather than as a labelled term. **Recorded, not hidden.**
 *
 * @param conduit   mesh conduit row of the first clamp (diagnostic only).
 * @param shortfall the UNMET extraction (positive, internal mass units).
 */
inline void bookNegativeCellSourceClamp(SimulationContext& ctx, int conduit,
                                        double shortfall) {
    auto& st = ctx.negsrc;
    st.clamp_events += 1;
    st.shortfall_mass += shortfall;
    if (!st.runtime_warned) {
        st.runtime_warned = true;
        st.first_node = conduit;
        ctx.warnings.push_back(
            "D-NS1/P1.4: a negative [TRANSPORT_SOURCES] row requested more "
            "mass than its conduit's cells held — extraction was clamped to "
            "the available amount (first at mesh conduit row " +
            std::to_string(conduit) +
            "). Total clamp counts are summarized at end of run.");
    }
}

/// End-of-run summary (pushed once by SWMMEngine::end when anything
/// clamped; gates read ctx.warnings, so .rpt ordering is not load-bearing).
inline void summarizeNegativeSourceClamps(SimulationContext& ctx) {
    const auto& st = ctx.negsrc;
    if (st.clamp_events == 0 && st.age_clamp_events == 0) return;
    ctx.warnings.push_back(
        "D-NS1 summary: extraction exceeded available mass " +
        std::to_string(st.clamp_events) + " time(s) (unmet extraction " +
        std::to_string(st.shortfall_mass) + " internal mass units)" +
        (st.age_clamp_events > 0
             ? ", water-age extraction clamped " +
                   std::to_string(st.age_clamp_events) + " time(s)"
             : std::string()) +
        ". Node-seam clamps are ledgered (the ledger carries the mass "
        "actually removed); ARD cell-source clamps are counted only — "
        "see bookNegativeCellSourceClamp.");
}

}  // namespace quality
}  // namespace openswmm

#endif  // OPENSWMM_QUALITY_NEGATIVE_SOURCES_HPP
