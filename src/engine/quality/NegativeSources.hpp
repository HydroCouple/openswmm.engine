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
 * @file      NegativeSources.hpp
 * @brief     D-NS1: one clamp-bookkeeping seam for all three quality engines.
 *
 * @details Subplan §3.1 (user requirement, 2026-08-23): negative sources
 *          (mass extraction) are legal; extraction beyond an element's
 *          held mass clamps to the available amount, the ledger books the
 *          mass ACTUALLY removed (the shortfall is un-booked from the
 *          extraction's own row — otherwise continuity breaks by
 *          construction), and the counts are
 *          summarized at end of run. LEGACY, ARD, and LARD all call the
 *          same helper so the semantics cannot drift apart (the lesson-52
 *          family, applied prospectively).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_NEGATIVE_SOURCES_HPP
#define OPENSWMM_QUALITY_NEGATIVE_SOURCES_HPP

#include <string>

#include "../core/SimulationContext.hpp"

namespace openswmm {
namespace quality {

/**
 * @brief Book one pollutant-row clamp: count it and un-book the shortfall from the
 * external-extraction ledger row.
 *
 * @note NO per-clamp warning. There was one until 2026-08-29; it fired on every correct
 * extraction deck, because during fill every cell is near-empty and any extraction clamps
 * trivially (measured: 108 clamps on a deck extracting 40 % of its inflow). A notice that fires
 * on ordinary models is one users learn to ignore — which destroys it on the models where it
 * matters (lesson 148). The end-of-run summary carries the count and the unmet mass; that is the
 * diagnostic.
 *
 * @param shortfall  the UNMET extraction (positive, internal mass units).
 *
 * @param[in,out] ctx Native extraction statistics and pollutant external-inflow balance, mutated
 * once per actual caller clamp; shared stats/ledger require serial access or external
 * synchronization.
 * @param[in] node Zero-based diagnostic element identity, not dereferenced; first clamp
 * retained.
 * @param[in] p Pollutant ledger index; out-of-range skips ledger correction but still counts
 * event.
 * @note shortfall must be finite positive unmet internal mass from an already-booked signed
 * external request; helper does not validate, remove physical mass, clamp inventory or advance
 * time. Adds it back to qual_routing_ex_in, increments counts and records first element.
 * Allocation-free.
 */
inline void bookNegativeSourceClamp(SimulationContext& ctx, int node, int p, double shortfall) {
    auto& st = ctx.negsrc;
    st.clamp_events += 1;
    st.shortfall_mass += shortfall;
    // The negative request was booked (signed) into qual_routing_ex_in by
    // the loaders; the unmet part did not actually leave the water, so it
    // is returned to the row. v1 scope: EXTERNAL_INFLOW and the API flux
    // are the admitted negative pathways, and both book into ex_in —
    // negative DWF/GW/RDII concentrations remain out of scope, recorded.
    if (p >= 0 && static_cast<std::size_t>(p) < ctx.mass_balance.qual_routing_ex_in.size())
        ctx.mass_balance.qual_routing_ex_in[static_cast<std::size_t>(p)] += shortfall;
    if (!st.first_clamp_recorded) {
        st.first_clamp_recorded = true;
        st.first_node = node;
    }
}

/**
 * @brief Age-row clamp: counted, not ledgered (no age row until A2c) and not warned per-clamp
 * (see bookNegativeSourceClamp's note).
 *
 * @details Count an age-row extraction clamp and retain first diagnostic element.
 * @param[in,out] ctx Mutable shared negative-source diagnostic counters; no physical age ledger
 * here.
 * @param[in] node Zero-based diagnostic element identity, not dereferenced.
 * @note Counts every call, does not itself clamp any state, book age mass or validate input.
 * Requires serialized/shared-counter ownership; no allocation/physics/clock change.
 */
inline void bookNegativeAgeClamp(SimulationContext& ctx, int node) {
    auto& st = ctx.negsrc;
    st.age_clamp_events += 1;
    if (!st.first_clamp_recorded) {
        st.first_clamp_recorded = true;
        st.first_node = node;
    }
}

/**
 * @brief P1.4: book one ARD **cell**-source clamp. Counted, NOT ledgered — and the reason is
 * structural, not an omission.
 *
 * @details The node seam un-books its shortfall from `qual_routing_ex_in` because the loaders
 * booked the signed request there in advance. Cell sources have no such row to correct:
 *
 * 1. `[TRANSPORT_SOURCES]` rows resolve to **MSX species rows only** — `ArdEngine.cpp` pushes
 * `np + src_msx[i]`, so a cell source can never name a pollutant; and
 * 2. **MSX species have no mass-balance row at all.** The ARD component writes to
 * `ctx.mass_balance` nowhere.
 *
 * So a per-pollutant row for this would be permanently zero, and inventing an MSX row here would
 * be a third self-consistent balance (lesson 147) bolted on inside a defect fix. Giving MSX
 * species real ledger rows (sizing the `qual_*` vectors by `np + nm`) is a deliberate, separate
 * round — until it lands, the delivered mass reaches `qual_routing_final` through the end-of-run
 * inventory sweep and therefore surfaces as CONTINUITY ERROR rather than as a labelled term.
 * **Recorded, not hidden.**
 *
 * @param conduit   mesh conduit row of the first clamp (diagnostic only).
 * @param shortfall the UNMET extraction (positive, internal mass units).
 *
 * @param[in,out] ctx Mutable shared extraction count/shortfall and first-element diagnostics
 * only.
 * @note Caller supplies finite positive unmet native extraction and already performs its actual
 * physical/native receipt booking. This helper does not repair any mass ledger or remove
 * inventory. Counts every call; requires serialized diagnostic ownership, allocation-free/no
 * clock mutation.
 */
inline void bookNegativeCellSourceClamp(SimulationContext& ctx, int conduit, double shortfall) {
    auto& st = ctx.negsrc;
    st.clamp_events += 1;
    st.shortfall_mass += shortfall;
    if (!st.first_clamp_recorded) {
        st.first_clamp_recorded = true;
        st.first_node = conduit;
    }
}

/**
 * @brief End-of-run summary (pushed once by SWMMEngine::end when anything clamped; gates read
 * ctx.warnings, so .rpt ordering is not load-bearing).
 *
 * @details Append the accumulated extraction-clamp summary when any pollutant/native or age
 * event occurred.
 * @param[in,out] ctx Reads counters/first element and appends owned message to warnings.
 * @note No events returns. Does not clear counts or suppress repeated calls, so owner invokes
 * once at run end. Message includes internal mass shortfall, age event count and zero-based
 * diagnostic identity. Allocation may throw; exclusive warning sink/stable counters, no
 * physics/clock advance.
 */
inline void summarizeNegativeSourceClamps(SimulationContext& ctx) {
    const auto& st = ctx.negsrc;
    if (st.clamp_events == 0 && st.age_clamp_events == 0)
        return;
    ctx.warnings.push_back(
        "D-NS1 summary: extraction exceeded available mass " + std::to_string(st.clamp_events) +
        " time(s) (unmet extraction " + std::to_string(st.shortfall_mass) +
        " internal mass units)" +
        (st.age_clamp_events > 0
             ? ", water-age extraction clamped " + std::to_string(st.age_clamp_events) + " time(s)"
             : std::string()) +
        ", first at element index " + std::to_string(st.first_node) +
        ". Extraction clamps ROUTINELY while the system fills, when elements "
        "are near-empty; a nonzero count is not by itself a modelling error. "
        "Node-seam clamps are ledgered (the ledger carries the mass actually "
        "removed); ARD cell-source clamps are counted only — see "
        "bookNegativeCellSourceClamp.");
}

} // namespace quality
} // namespace openswmm

#endif // OPENSWMM_QUALITY_NEGATIVE_SOURCES_HPP
