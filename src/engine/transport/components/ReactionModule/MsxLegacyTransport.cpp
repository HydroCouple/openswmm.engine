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
#include "../../../quality/QualityRouting.hpp"

namespace openswmm::transport {

namespace {
/// The quality path's own thresholds — one litre and one millimetre, the
/// numbers legacy qualrout.c routes on. This mirror previously carried a
/// local 1e-10, so it dispatched differently from the family it mirrors.
using quality::ZERO_DEPTH;
using quality::LEGACY_ZERO;

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

    // ---- 3. Node mixing (mixAtNodes mirror; no evap factor). External
    //         loads: U2 (2026-09-07) — [INFLOWS] rows naming a species
    //         arrive through msx_ext_mass_in (a RATE, the qual_mass_in
    //         shape) and join the link mass here; empty when no such row
    //         exists, so species-only-from-reactions decks are unchanged.
    //         The clamp is TWO-SIDED like the heat mirror's: mixing cannot
    //         exceed the larger of the held and incoming concentrations,
    //         nor go below zero. --------------------------------------------
    const bool has_ext = rx.msx_ext_mass_in.size() >=
                         static_cast<std::size_t>(nn) * uns;
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double v_old = ctx.nodes.old_volume[ui];
        const double v_in  = ctx.nodes.qual_vol_in[ui];
        const double q_in  = (dt > 0.0) ? v_in / dt : 0.0;
        const bool zero_bf = ctx.options.outfall_backflow_zero &&
                             ctx.nodes.type[ui] == NodeType::OUTFALL &&
                             v_in <= 0.0;
        // The quality path's dispatch and dry-node rule, asked rather than
        // re-derived — a species IS a concentration, so both carry over
        // exactly as they do for a pollutant.
        const bool reactor = quality::nodeIsReactor(ctx, i);
        const bool dry     = quality::nodeIsDry(ctx, i, q_in);
        for (std::size_t s = 0; s < uns; ++s) {
            const double c_old = sc.node_old[ui * uns + s];
            const double ext_rate = has_ext ? rx.msx_ext_mass_in[ui * uns + s] : 0.0;
            const double mass = (sc.mass_in[ui * uns + s] + ext_rate) * dt;

            if (!reactor) {
                // findNodeQual: no storage volume, so the concentration is
                // the inflow's.
                double c;
                if (v_in > 0.0)            c = mass / v_in;
                else if (zero_bf)          c = 0.0;
                else if (ctx.nodes.depth[ui] > ZERO_DEPTH) c = c_old;
                else                       c = 0.0;
                rx.msx_node_conc[ui * uns + s] = std::max(c, 0.0);
                continue;
            }

            double c_new;
            if (q_in <= LEGACY_ZERO) {
                c_new = zero_bf ? 0.0 : c_old;
            } else {
                const double c_in  = mass / v_in;
                const double c_max = std::max(c_old, c_in);
                c_new = (c_old * v_old + mass) / (v_old + v_in);
                c_new = std::min(c_new, c_max);
            }
            if (dry) c_new = 0.0;
            rx.msx_node_conc[ui * uns + s] = std::max(c_new, 0.0);
        }
    }

    // ---- 4. Link update (updateLinkQuality mirror; k = 0, no evap). ------
    const bool is_steady =
        (ctx.options.routing_model == RoutingModel::STEADY);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        // Steady flow draws on node1 unconditionally (findSFLinkQual).
        const int up = is_steady
                           ? ctx.links.node1[uj]
                           : ((ctx.links.flow[uj] >= 0.0) ? ctx.links.node1[uj]
                                                          : ctx.links.node2[uj]);
        if (up < 0 || up >= nn) continue;
        const auto uup = static_cast<std::size_t>(up);

        // A non-conduit or DUMMY link holds no water: upstream outright.
        const bool passthrough = quality::linkTakesUpstreamValue(ctx, j);
        const double v_old = ctx.links.old_volume[uj];
        const double q_in  = passthrough
                                 ? 0.0
                                 : quality::conduitMixingInflow(ctx, j, dt);
        const bool dry = !is_steady && !passthrough && quality::linkIsDry(ctx, j);

        for (std::size_t s = 0; s < uns; ++s) {
            const double c_old = sc.link_old[uj * uns + s];
            const double c_up  = rx.msx_node_conc[uup * uns + s];
            double c_new;
            if (passthrough || is_steady) {
                c_new = c_up;
            } else if (q_in <= LEGACY_ZERO) {
                c_new = c_old;
            } else {
                const double v_in  = q_in * dt;
                const double c_max = std::max(c_old, c_up);
                c_new = (c_old * v_old + c_up * v_in) / (v_old + v_in);
                c_new = std::min(c_new, c_max);
            }
            if (dry) c_new = 0.0;
            rx.msx_link_conc[uj * uns + s] = std::max(c_new, 0.0);
        }
    }
}

}  // namespace openswmm::transport
