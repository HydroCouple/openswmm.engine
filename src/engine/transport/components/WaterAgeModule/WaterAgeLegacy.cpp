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
 * @file WaterAgeLegacy.cpp
 * @brief Phase A1b body — LEGACY CSTR water-age mirror.
 *
 * @details Formula provenance, line by line, is the pollutant path in
 *          QualityRouting.cpp: accumulateLinkLoads (rate convention
 *          q·value), mixAtNodes (the findStorageQual / findNodeQual
 *          dispatch, the volume-balance mix with the c_max clamp and the
 *          dry-node rule), updateLinkQuality (the non-conduit shortcut,
 *          findSFLinkQual's node1, the DW volume-change inflow and the
 *          dry-link rule). Those rules are ASKED for through the shared
 *          predicates in QualityRouting.hpp rather than re-derived here —
 *          four hand-copies had already drifted apart. Deliberate
 *          differences, both documented in the header: no evaporation
 *          factor (plan §8 — evaporation leaves the mean age unchanged)
 *          and no decay (age has none).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "WaterAgeLegacy.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../../../quality/NegativeSources.hpp"
#include "../../../quality/QualityRouting.hpp"
#include "../../InitialQualitySeeds.hpp"

namespace openswmm::transport {

namespace {
/// The quality path's own thresholds — one litre and one millimetre, the
/// numbers legacy qualrout.c routes on. This mirror previously carried a
/// local 1e-10 under a comment claiming it matched the quality path; it did
/// not, and the two families dispatched differently as a result.
using quality::ZERO_DEPTH;
using quality::LEGACY_ZERO;

/// Routing-thread scratch (the BindingScratch pattern): aged old-state
/// snapshots and the per-node age-mass accumulator.
struct AgeScratch {
    std::vector<double> node_old, link_old, age_in;
    void ensure(int nn, int nl) {
        if (node_old.size() != static_cast<std::size_t>(nn)) {
            node_old.assign(static_cast<std::size_t>(nn), 0.0);
            age_in.assign(static_cast<std::size_t>(nn), 0.0);
        }
        if (link_old.size() != static_cast<std::size_t>(nl))
            link_old.assign(static_cast<std::size_t>(nl), 0.0);
    }
};
AgeScratch& scratch() {
    static AgeScratch s;
    return s;
}
}  // namespace

void routeLegacyAge(SimulationContext& ctx, double dt) {
    if (!ctx.options.water_age || dt <= 0.0) return;
    auto& ws = ctx.water_age_state;
    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();
    if (ws.node_age.size() != static_cast<std::size_t>(nn))
        ws.resize(nn, nl, ctx.n_subcatches());  // A3: keep watershed rows

    auto& sc = scratch();
    sc.ensure(nn, nl);

    // ---- 0. INITIAL_STATE seeding, once (the ARD engine seeds at its own
    //         init; here the first routing step is the natural site). An
    //         unseeded mirror would leave a configured INITIAL_STATE age
    //         silently inert under LEGACY — the lesson-10 shape. ----------
    if (!ws.legacy_seeded) {
        const double a0 = ctx.water_age_config.global_age[static_cast<int>(
            WaterAgeSource::INITIAL_STATE)];
        if (a0 > 0.0) {
            std::fill(ws.node_age.begin(), ws.node_age.end(), a0);
            std::fill(ws.link_age.begin(), ws.link_age.end(), a0);
        }
        // E-A3: per-element [INITIAL_QUALITY] __WATER_AGE__ rows override
        // the global fill (hotstart wins inside the helper, D-IQ7).
        applyInitialAgeOverrides(ctx);
        ws.legacy_seeded = true;
    }

    // ---- 1. Aging: +dt, then the aged values are this step's "old" state
    //         (plan §1: age advances by dt then mixes). -------------------
    for (int i = 0; i < nn; ++i)
        sc.node_old[static_cast<std::size_t>(i)] =
            ws.node_age[static_cast<std::size_t>(i)] + dt;
    for (int j = 0; j < nl; ++j)
        sc.link_old[static_cast<std::size_t>(j)] =
            ws.link_age[static_cast<std::size_t>(j)] + dt;

    // ---- 2. Age-mass accumulation (accumulateLinkLoads mirror, RATE
    //         convention) + the loaders' per-source rates. ----------------
    std::fill(sc.age_in.begin(), sc.age_in.end(), 0.0);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double q = std::fabs(ctx.links.flow[uj]);
        if (q <= 0.0) continue;
        const int downstream = (ctx.links.flow[uj] >= 0.0)
                                   ? ctx.links.node2[uj]
                                   : ctx.links.node1[uj];
        if (downstream < 0 || downstream >= nn) continue;
        sc.age_in[static_cast<std::size_t>(downstream)] += q * sc.link_old[uj];
    }
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ui < ws.node_age_vol_in.size())
            sc.age_in[ui] += ws.node_age_vol_in[ui];
    }

    // ---- 3. Node mixing (mixAtNodes mirror; NO evap factor — plan §8:
    //         evaporation leaves the mean age unchanged, and no reaction:
    //         age has no decay constant). Dispatch, dry-node rule and the
    //         inflow test are the quality path's, asked rather than
    //         re-derived. ----------------------------------------------
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double v_old = ctx.nodes.old_volume[ui];
        const double v_in  = ctx.nodes.qual_vol_in[ui];
        const double q_in  = (dt > 0.0) ? v_in / dt : 0.0;
        const double a_old = sc.node_old[ui];

        if (!quality::nodeIsReactor(ctx, i)) {
            // findNodeQual: no storage volume, so the age is the inflow's.
            if (v_in > 0.0) {
                double mass_in = sc.age_in[ui] * dt;
                if (mass_in < 0.0 && a_old * v_old + mass_in < 0.0) {
                    quality::bookNegativeAgeClamp(ctx, i);
                    mass_in = -(a_old * v_old);
                }
                ws.node_age[ui] = std::max(mass_in / v_in, 0.0);
            } else if (ctx.options.outfall_backflow_zero &&
                       ctx.nodes.type[ui] == NodeType::OUTFALL) {
                // OUTFALL_BACKFLOW_QUALITY ZERO: a supplying outfall delivers
                // fresh (age-zero) boundary water — without this, the held
                // boundary water ages 1:1 forever and every reversal imports
                // it.
                ws.node_age[ui] = 0.0;
            } else {
                ws.node_age[ui] =
                    (ctx.nodes.depth[ui] > ZERO_DEPTH) ? a_old : 0.0;
            }
            continue;
        }

        // findStorageQual: a mixed reactor.
        double mass_in = sc.age_in[ui] * dt;
        // D-NS1 (X6): a negative age source extracts age·volume, clamped
        // to what the store holds — counted and warned, not ledgered
        // (age has no continuity row until A2c). Branch untaken on
        // non-negative decks.
        if (mass_in < 0.0 && a_old * v_old + mass_in < 0.0) {
            quality::bookNegativeAgeClamp(ctx, i);
            mass_in = -(a_old * v_old);
        }
        double a_new;
        if (q_in <= LEGACY_ZERO) {
            a_new = a_old;
        } else {
            const double a_in  = mass_in / v_in;
            const double a_max = std::max(a_old, a_in);
            a_new = (a_old * v_old + mass_in) / (v_old + v_in);
            a_new = std::min(a_new, a_max);
        }
        // A reactor that ends the step empty with nothing coming in holds no
        // water, so it holds no aged water either — the age analogue of
        // legacy zeroing the concentration.
        if (quality::nodeIsDry(ctx, i, q_in)) a_new = 0.0;
        ws.node_age[ui] = std::max(a_new, 0.0);
    }

    // ---- 4. Link update (updateLinkQuality mirror; k = 0, no evap). -----
    const bool is_steady =
        (ctx.options.routing_model == RoutingModel::STEADY);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        // Steady flow draws on node1 unconditionally — legacy
        // findSFLinkQual never consults the downstream end, even on reverse
        // flow.
        const int upstream =
            is_steady ? ctx.links.node1[uj]
                      : ((ctx.links.flow[uj] >= 0.0) ? ctx.links.node1[uj]
                                                     : ctx.links.node2[uj]);
        if (upstream < 0 || upstream >= nn) continue;
        const auto un = static_cast<std::size_t>(upstream);
        const double a_up  = ws.node_age[un];

        // A pump, orifice, weir, outlet or DUMMY conduit holds no water, so
        // it carries the upstream age outright.
        if (quality::linkTakesUpstreamValue(ctx, j)) {
            ws.link_age[uj] = std::max(a_up, 0.0);
            continue;
        }

        const double v_old = ctx.links.old_volume[uj];
        const double a_old = sc.link_old[uj];

        double a_new;
        if (is_steady) {
            a_new = a_up;
        } else {
            const double q_in = quality::conduitMixingInflow(ctx, j, dt);
            if (q_in <= LEGACY_ZERO) {
                a_new = a_old;
            } else {
                const double v_in  = q_in * dt;
                const double a_max = std::max(a_old, a_up);
                a_new = (a_old * v_old + a_up * v_in) / (v_old + v_in);
                a_new = std::min(a_new, a_max);
            }
            // An essentially empty conduit holds no water and so no aged
            // water — the age analogue of legacy zeroing the concentration.
            // This engine handed such a link the UPSTREAM age instead, so a
            // filling conduit reported its inflow's age from the first period
            // rather than mixing up to it.
            if (quality::linkIsDry(ctx, j)) a_new = 0.0;
        }
        ws.link_age[uj] = std::max(a_new, 0.0);
    }
}

}  // namespace openswmm::transport
