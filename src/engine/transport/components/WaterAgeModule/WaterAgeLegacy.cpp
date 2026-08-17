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
 *          q·value), mixAtNodes ((c_old·v_old + mass_in)/(v_old + v_in)
 *          with the c_max clamp), updateLinkQuality (STEADY / no-flow /
 *          zero-volume / volume-balance branches with the DW q_in
 *          correction). Deliberate differences, both documented in the
 *          header: no evaporation factor (plan §8 — evaporation leaves
 *          the mean age unchanged) and no decay (age has none).
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

namespace openswmm::transport {

namespace {
/// Matches the quality path's ZERO_VOLUME semantics (QualityRouting.cpp).
constexpr double kZeroVolume = 1.0e-10;

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
        ws.resize(nn, nl);

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
    //         evaporation leaves the mean age unchanged). -----------------
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double v_old = ctx.nodes.old_volume[ui];
        const double v_in  = ctx.nodes.qual_vol_in[ui];
        const double a_old = sc.node_old[ui];
        if (v_in <= 0.0) {
            ws.node_age[ui] = a_old;
            continue;
        }
        const double mass_in = sc.age_in[ui] * dt;
        const double a_in    = mass_in / v_in;
        const double a_max   = std::max(a_old, a_in);
        double a_new = (v_old > kZeroVolume)
                           ? (a_old * v_old + mass_in) / (v_old + v_in)
                           : a_in;
        a_new = std::min(a_new, a_max);
        ws.node_age[ui] = std::max(a_new, 0.0);
    }

    // ---- 4. Link update (updateLinkQuality mirror; k = 0, no evap). -----
    const bool is_steady =
        (ctx.options.routing_model == RoutingModel::STEADY);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double q = std::fabs(ctx.links.flow[uj]);
        const int upstream = (ctx.links.flow[uj] >= 0.0)
                                 ? ctx.links.node1[uj]
                                 : ctx.links.node2[uj];
        if (upstream < 0 || upstream >= nn) continue;
        const auto un = static_cast<std::size_t>(upstream);

        const double v_old = ctx.links.old_volume[uj];
        const double v_new = ctx.links.volume[uj];
        const double a_old = sc.link_old[uj];
        const double a_up  = ws.node_age[un];

        double a_new;
        if (is_steady) {
            a_new = a_up;
        } else if (q <= 0.0) {
            a_new = a_old;
        } else if (v_new <= kZeroVolume) {
            a_new = a_up;
        } else {
            double q_in = q;
            if (v_new > v_old) q_in += (v_new - v_old) / dt;
            q_in = std::max(q_in, 0.0);
            const double denom = v_old + q_in * dt;
            a_new = (denom > kZeroVolume)
                        ? (a_old * v_old + a_up * q_in * dt) / denom
                        : a_up;
        }
        ws.link_age[uj] = std::max(a_new, 0.0);
    }
}

}  // namespace openswmm::transport
