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
 * @file MsxLegacyTransport.cpp
 * @brief R4b body. Formula provenance is `routeLegacyHeat` — see the header.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "MsxLegacyTransport.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "ReactionLegacyBinding.hpp"
#include "../../../core/SimulationContext.hpp"

namespace openswmm::transport {

namespace {
/// Matches the quality path's ZERO_VOLUME semantics (QualityRouting.cpp).
constexpr double kZeroVolume = 1.0e-10;

/// Routing-thread scratch (the BindingScratch pattern). Strided per species:
/// the node/link snapshots and the per-node mass-rate accumulator.
struct MsxScratch {
    std::vector<double> node_old, link_old, mass_in;
    void ensure(std::size_t nn, std::size_t nl, std::size_t ns) {
        if (node_old.size() != nn * ns) {
            node_old.assign(nn * ns, 0.0);
            mass_in.assign(nn * ns, 0.0);
        }
        if (link_old.size() != nl * ns) link_old.assign(nl * ns, 0.0);
    }
};
MsxScratch& scratch() {
    static MsxScratch s;
    return s;
}
}  // namespace

void routeLegacyMsx(SimulationContext& ctx, double dt) {
    auto& rx = ctx.reactions;
    const int nsp = rx.n_species();
    if (nsp <= 0 || dt <= 0.0) return;

    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();
    const auto uns = static_cast<std::size_t>(nsp);

    // R4's seeding path — sizes the arrays and applies GLOBAL +
    // [REACTION_QUALITY] fills exactly once, whichever dispatch touches the
    // state first this run.
    ensureMsxState(ctx);
    if (rx.msx_node_conc.size() < static_cast<std::size_t>(nn) * uns ||
        rx.msx_link_conc.size() < static_cast<std::size_t>(nl) * uns)
        return;

    auto& sc = scratch();
    sc.ensure(static_cast<std::size_t>(nn), static_cast<std::size_t>(nl),
              uns);

    // ---- 1. Old-state snapshots (post-reaction: the react stages ran
    //         earlier in execute(), so transport moves this step's reacted
    //         concentrations — the Lie split the header documents). --------
    std::copy(rx.msx_node_conc.begin(),
              rx.msx_node_conc.begin() +
                  static_cast<std::ptrdiff_t>(static_cast<std::size_t>(nn) *
                                              uns),
              sc.node_old.begin());
    std::copy(rx.msx_link_conc.begin(),
              rx.msx_link_conc.begin() +
                  static_cast<std::ptrdiff_t>(static_cast<std::size_t>(nl) *
                                              uns),
              sc.link_old.begin());

    // ---- 2. Mass accumulation (accumulateLinkLoads mirror, RATE
    //         convention q·c_old into the downstream node). ----------------
    std::fill(sc.mass_in.begin(), sc.mass_in.end(), 0.0);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double q = std::fabs(ctx.links.flow[uj]);
        if (q <= 0.0) continue;
        const int dn = (ctx.links.flow[uj] >= 0.0) ? ctx.links.node2[uj]
                                                   : ctx.links.node1[uj];
        if (dn < 0 || dn >= nn) continue;
        const auto udn = static_cast<std::size_t>(dn);
        for (std::size_t s = 0; s < uns; ++s)
            sc.mass_in[udn * uns + s] += q * sc.link_old[uj * uns + s];
    }

    // ---- 3. Node mixing (mixAtNodes mirror; no evap factor and no
    //         external loads — header). The clamp is TWO-SIDED like the
    //         heat mirror's: mixing cannot exceed the larger of the held
    //         and incoming concentrations, nor go below zero. --------------
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double v_old = ctx.nodes.old_volume[ui];
        const double v_in  = ctx.nodes.qual_vol_in[ui];
        const bool zero_bf = ctx.options.outfall_backflow_zero &&
                             ctx.nodes.type[ui] == NodeType::OUTFALL &&
                             v_in <= 0.0;
        for (std::size_t s = 0; s < uns; ++s) {
            const double c_old = sc.node_old[ui * uns + s];
            if (v_in <= 0.0) {
                rx.msx_node_conc[ui * uns + s] = zero_bf ? 0.0 : c_old;
                continue;
            }
            const double mass = sc.mass_in[ui * uns + s] * dt;
            const double c_in = mass / v_in;
            const double c_max = std::max(c_old, c_in);
            double c_new = (v_old > kZeroVolume)
                               ? (c_old * v_old + mass) / (v_old + v_in)
                               : c_in;
            c_new = std::min(c_new, c_max);
            rx.msx_node_conc[ui * uns + s] = std::max(c_new, 0.0);
        }
    }

    // ---- 4. Link update (updateLinkQuality mirror; k = 0, no evap). ------
    const bool is_steady =
        (ctx.options.routing_model == RoutingModel::STEADY);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double q = std::fabs(ctx.links.flow[uj]);
        const int up = (ctx.links.flow[uj] >= 0.0) ? ctx.links.node1[uj]
                                                   : ctx.links.node2[uj];
        if (up < 0 || up >= nn) continue;
        const auto uup = static_cast<std::size_t>(up);

        const double v_old = ctx.links.old_volume[uj];
        const double v_new = ctx.links.volume[uj];

        for (std::size_t s = 0; s < uns; ++s) {
            const double c_old = sc.link_old[uj * uns + s];
            const double c_up  = rx.msx_node_conc[uup * uns + s];
            double c_new;
            if (is_steady) {
                c_new = c_up;
            } else if (q <= 0.0) {
                c_new = c_old;
            } else if (v_new <= kZeroVolume) {
                c_new = c_up;
            } else {
                double q_in = q;
                if (v_new > v_old) q_in += (v_new - v_old) / dt;
                q_in = std::max(q_in, 0.0);
                const double denom = v_old + q_in * dt;
                c_new = (denom > kZeroVolume)
                            ? (c_old * v_old + c_up * q_in * dt) / denom
                            : c_up;
            }
            rx.msx_link_conc[uj * uns + s] = std::max(c_new, 0.0);
        }
    }
}

}  // namespace openswmm::transport
