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
 * @file HeatLegacy.cpp
 * @brief Phase H1 body — LEGACY CSTR temperature transport.
 *
 * @details Formula provenance is the pollutant path in QualityRouting.cpp,
 *          by way of the A1b age mirror: accumulateLinkLoads (rate
 *          convention q·value), mixAtNodes ((v_old·T_old + mass_in) /
 *          (v_old + v_in)), updateLinkQuality (STEADY / no-flow /
 *          zero-volume / volume-balance branches with the DW q_in
 *          correction). Differences from the age mirror are enumerated in
 *          the header; the load-bearing one is that nothing here is
 *          floored at zero.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HeatLegacy.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../HeatFluxModules/HeatFluxes.hpp"

namespace openswmm::transport {

namespace {
/// Matches the quality path's ZERO_VOLUME semantics (QualityRouting.cpp).
constexpr double kZeroVolume = 1.0e-10;

/// Routing-thread scratch (the BindingScratch pattern): old-state
/// snapshots and the per-node temperature-volume accumulator.
struct HeatScratch {
    std::vector<double> node_old, link_old, temp_in;
    void ensure(int nn, int nl) {
        if (node_old.size() != static_cast<std::size_t>(nn)) {
            node_old.assign(static_cast<std::size_t>(nn), 0.0);
            temp_in.assign(static_cast<std::size_t>(nn), 0.0);
        }
        if (link_old.size() != static_cast<std::size_t>(nl))
            link_old.assign(static_cast<std::size_t>(nl), 0.0);
    }
};
HeatScratch& scratch() {
    static HeatScratch s;
    return s;
}
}  // namespace

void routeLegacyHeat(SimulationContext& ctx, double dt) {
    if (!ctx.options.heat_transport || dt <= 0.0) return;
    auto& hs = ctx.heat_state;
    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();
    const double t_init = ctx.heat_config.global_temp[static_cast<int>(
        HeatSource::INITIAL_STATE)];
    if (hs.node_temp.size() != static_cast<std::size_t>(nn))
        hs.resize(nn, nl, t_init);

    auto& sc = scratch();
    sc.ensure(nn, nl);

    // ---- 0. INITIAL_STATE seeding, once. Unlike age (whose initial state
    //         is usually 0, so an unseeded mirror merely looks plausible),
    //         an unseeded temperature field would sit at whatever resize()
    //         left and every gate would read a number — so the seed is
    //         asserted, not assumed. --------------------------------------
    if (!hs.legacy_seeded) {
        std::fill(hs.node_temp.begin(), hs.node_temp.end(), t_init);
        std::fill(hs.link_temp.begin(), hs.link_temp.end(), t_init);
        hs.legacy_seeded = true;
    }

    // ---- 1. Source terms, then old state. Age advances 1 s/s here;
    //         temperature changes only by exchange with its surroundings,
    //         so this is where plan §2's flux modules act — BEFORE the
    //         old-state snapshot, so the step's advective mixing sees the
    //         post-flux temperature exactly as it sees the post-aging age.
    //         H2 delivers SurfaceExchange; H3/H4 add radiative and
    //         sediment terms at this same point. ------------------------
    //
    //         D-H5e: ONE call. This was two, and once D-H5d made the step a
    //         relaxation the two stopped commuting — each relaxed fully
    //         toward its own module's equilibrium, so the result depended on
    //         which ran last. Every family now sums into one J(T) inside
    //         applyHeatFluxes before a single step.
    heat::applyHeatFluxes(ctx, dt);

    for (int i = 0; i < nn; ++i)
        sc.node_old[static_cast<std::size_t>(i)] =
            hs.node_temp[static_cast<std::size_t>(i)];
    for (int j = 0; j < nl; ++j)
        sc.link_old[static_cast<std::size_t>(j)] =
            hs.link_temp[static_cast<std::size_t>(j)];

    // ---- 2. Temperature-volume accumulation (accumulateLinkLoads mirror,
    //         RATE convention) + the loaders' per-source rates. ----------
    std::fill(sc.temp_in.begin(), sc.temp_in.end(), 0.0);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double q = std::fabs(ctx.links.flow[uj]);
        if (q <= 0.0) continue;
        const int downstream = (ctx.links.flow[uj] >= 0.0)
                                   ? ctx.links.node2[uj]
                                   : ctx.links.node1[uj];
        if (downstream < 0 || downstream >= nn) continue;
        sc.temp_in[static_cast<std::size_t>(downstream)] +=
            q * sc.link_old[uj];
    }
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        if (ui < hs.node_temp_vol_in.size())
            sc.temp_in[ui] += hs.node_temp_vol_in[ui];
    }

    // ---- 3. Node mixing (mixAtNodes mirror). The clamp is TWO-SIDED: a
    //         volume-weighted mean lies between its inputs, and unlike age
    //         the incoming value can be COLDER than what is held. -------
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double v_old = ctx.nodes.old_volume[ui];
        const double v_in  = ctx.nodes.qual_vol_in[ui];
        const double t_old = sc.node_old[ui];
        if (v_in <= 0.0) {
            hs.node_temp[ui] = t_old;
            continue;
        }
        const double mass_in = sc.temp_in[ui] * dt;
        const double t_in    = mass_in / v_in;
        double t_new = (v_old > kZeroVolume)
                           ? (t_old * v_old + mass_in) / (v_old + v_in)
                           : t_in;
        // No max(·, 0): sub-zero water temperatures are ordinary, and a
        // zero floor would silently warm every cold-weather model.
        t_new = std::clamp(t_new, std::min(t_old, t_in),
                           std::max(t_old, t_in));
        hs.node_temp[ui] = t_new;
    }

    // ---- 4. Link update (updateLinkQuality mirror; no decay, no evap). --
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
        const double t_old = sc.link_old[uj];
        const double t_up  = hs.node_temp[un];

        double t_new;
        if (is_steady) {
            t_new = t_up;
        } else if (q <= 0.0) {
            t_new = t_old;
        } else if (v_new <= kZeroVolume) {
            t_new = t_up;
        } else {
            double q_in = q;
            if (v_new > v_old) q_in += (v_new - v_old) / dt;
            q_in = std::max(q_in, 0.0);
            const double denom = v_old + q_in * dt;
            t_new = (denom > kZeroVolume)
                        ? (t_old * v_old + t_up * q_in * dt) / denom
                        : t_up;
            t_new = std::clamp(t_new, std::min(t_old, t_up),
                               std::max(t_old, t_up));
        }
        hs.link_temp[uj] = t_new;
    }
}

}  // namespace openswmm::transport
