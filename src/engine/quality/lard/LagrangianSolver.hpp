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
 * @brief LARD (Lagrangian ARD) quality engine — X2: LTD transport core.
 *
 * @details Subplan X2 (`plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md`;
 *          strategy `plans/LAGRANGIAN_QUALITY_STRATEGY.md` §2/§4, §16
 *          amendments D-L1/D-L2/D-L5 binding). One substep per routing step
 *          (a `QUALITY_STEP` sub-stepping option arrives with X3, where it
 *          starts to matter for RWPT).
 *
 *          Step orchestration (§4.2, trimmed to X2 scope):
 *            0. flow-reversal detection → ring reversal + topo invalidation
 *            1. DRAIN — every conduit sends |Q|·dt of back-end segment water
 *               (and its mass) to its downstream node's inflow ledger; a
 *               link whose new volume is below the remainder sheds the
 *               difference through its FRONT to the upstream ledger, so
 *               segment volume always sums exactly to `links.volume` and
 *               the volume change is booked, never rescaled away (the E2
 *               unbooked-resync family).
 *            2. MIX — nodes in flow-aware topological order (Kahn; cycles
 *               broken in index order, their residue carried in the ledger
 *               to the next step rather than dropped): CSTR over the node's
 *               own old volume — junctions, dividers, storages (CMSTR) and
 *               outfalls all reduce to the same formula. External loads
 *               join here from the SHARED loader seam (`qual_mass_in` rate ×
 *               dt, `qual_vol_in` volume — assembled by
 *               `QualitySolver::assembleExternalLoads`, the ARD precedent).
 *               Zero-volume links (pump/orifice/weir/outlet) pass the node's
 *               NEW concentration through in the same step — the reason the
 *               order is topological (§4.3, Davis et al.).
 *            3. RELEASE — each conduit gains a front segment at its upstream
 *               node's new concentration, sized so the slab total equals
 *               `links.volume` exactly; §4.5 merge tolerance collapses plug
 *               flow.
 *            4. DECAY — exact-exponential kdecay on segments and node
 *               stores (species-major stripes), booked to
 *               `qual_routing_reacted`.
 *            5. PUBLISH — `links.conc` = volume-weighted segment mean (so
 *               engine-side final-storage and outfall bookings are exact),
 *               then `conc_old = conc` (the ARD convention).
 *
 *          Deliberately NOT here: reactions module binding (deferred L3),
 *          RWPT (X3), water age / heat (X4 — their state does not advance
 *          under this dispatch and the open() warning says so), treatment
 *          interop (warned bypass), storage mixing models beyond CMSTR,
 *          the legacy evaporation up-concentration factor (recorded
 *          deviation — steady gates cannot see it; parity work owns it),
 *          D-NS1's clamp counter/warning (the max(0,·) floor is here, but
 *          its observer — a negative source reaching a node — only exists
 *          once X6 lands negative loads in the shared loaders).
 *
 *          Header-only for the same reason X1 was: the engine source glob
 *          lacks CONFIGURE_DEPENDS, and a patch-applied .cpp silently does
 *          not compile.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
#define OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../../core/SimulationContext.hpp"
#include "../QualityRouting.hpp"
#include "SegmentStore.hpp"

namespace openswmm {
namespace lard {

constexpr double kTinyFlow = 1.0e-8;  ///< cfs; below this a link moves nothing

class LagrangianSolver {
public:
    /**
     * @brief One routing step of LTD transport. Lazily initializes on the
     *        first call (needs router-set volumes, the ARD precedent).
     */
    void step(SimulationContext& ctx, double dt) {
        const int np = ctx.n_pollutants();
        if (np <= 0) return;  // age/heat under LARD arrive with X4
        if (!initialized_) init(ctx);

        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        auto& nodes = ctx.nodes;
        auto& links = ctx.links;

        // ---- 0. Flow reversal (§4.4) --------------------------------------
        bool topo_dirty = false;
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            const double q = links.flow[ul];
            if (std::abs(q) <= kTinyFlow) continue;
            const std::int8_t sign = (q >= 0.0) ? 1 : -1;
            if (sign != flow_sign_[ul]) {
                if (links.type[ul] == LinkType::CONDUIT) store_.reverse(l);
                flow_sign_[ul] = sign;
                topo_dirty = true;
            }
        }
        if (topo_dirty || topo_.empty()) computeTopoOrder(ctx);

        // ---- 1. DRAIN (all conduits, before any node mixes) ---------------
        scratch_.assign(static_cast<std::size_t>(np), 0.0);
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT) continue;
            const double q = std::abs(links.flow[ul]);
            const int dn = downstreamNode(ctx, l);
            const int up = upstreamNode(ctx, l);

            if (q > kTinyFlow && dn >= 0) {
                std::fill(scratch_.begin(), scratch_.end(), 0.0);
                const double drained = store_.drain_back(l, q * dt,
                                                         scratch_.data());
                addToLedger(dn, drained, scratch_.data(), np);
            }
            // Volume reconciliation: the slab must sum to links.volume.
            // A shortfall is filled at RELEASE with upstream water; an
            // excess left after the outflow drain leaves through the FRONT
            // to the upstream ledger — booked, not rescaled (see header).
            const double v_new = links.volume[ul];
            const double v_rem = store_.total_volume(l);
            if (v_rem > v_new && up >= 0) {
                std::fill(scratch_.begin(), scratch_.end(), 0.0);
                const double shed = store_.drain_front(l, v_rem - v_new,
                                                       scratch_.data());
                addToLedger(up, shed, scratch_.data(), np);
            }
        }

        // ---- 2. MIX in topo order, passthrough zero-volume links ----------
        for (const int n : topo_) {
            const auto un = static_cast<std::size_t>(n);
            const double v_old = nodes.old_volume[un];
            const double v_in = node_vol_in_[un];

            for (int p = 0; p < np; ++p) {
                const auto idx = un * static_cast<std::size_t>(np) +
                                 static_cast<std::size_t>(p);
                const double c_old = nodes.conc[idx];
                // External loads from the shared seam: rate × dt (mass) —
                // the same convention mixAtNodes consumes.
                const double m_ext = nodes.qual_mass_in[idx] * dt;
                double m = c_old * v_old + node_mass_in_[idx] + m_ext;
                // D-NS1 floor (defensive until X6 makes it observable):
                // extraction can never drive a store's mass negative.
                if (m < 0.0) m = 0.0;
                const double denom = v_old + v_in + nodes.qual_vol_in[un];
                // ALWAYS divide by the full denominator. m/denom is a convex
                // combination of c_old and the arriving concentrations, so it
                // can never exceed its inputs; the fallback this replaces
                // divided a mass that included c_old*v_old by a divisor that
                // EXCLUDED v_old, and at a nearly-dry junction that quotient
                // amplified step over step -- measured on a receding-flow
                // deck (inflow stops at 1 h): node concentrations reached
                // 2.7e30, 2.0e281, then inf, and the final-storage row went
                // NaN. Below 1e-12 ft^3 there is no meaningful water and the
                // store keeps its concentration.
                nodes.conc[idx] = (denom > 1.0e-12) ? m / denom : c_old;
                node_mass_in_[idx] = 0.0;  // consumed; cycle residue carries
            }
            node_vol_in_[un] = 0.0;

            // Zero-volume passthrough (§2.4): outgoing pump/orifice/weir/
            // outlet links deliver the node's NEW concentration downstream
            // within this step — the property the topo order exists for.
            for (const int l : node_out_links_[un]) {
                const auto ul = static_cast<std::size_t>(l);
                if (links.type[ul] == LinkType::CONDUIT) continue;
                const double q = std::abs(links.flow[ul]);
                if (q <= kTinyFlow) continue;
                const int dn = downstreamNode(ctx, l);
                if (dn < 0) continue;
                for (int p = 0; p < np; ++p) {
                    const auto ni = un * static_cast<std::size_t>(np) +
                                    static_cast<std::size_t>(p);
                    const auto li = ul * static_cast<std::size_t>(np) +
                                    static_cast<std::size_t>(p);
                    scratch_[static_cast<std::size_t>(p)] = nodes.conc[ni];
                    links.conc[li] = nodes.conc[ni];
                }
                addToLedgerRate(dn, q * dt, scratch_.data(), np);
            }
        }

        // ---- 3. RELEASE new front segments --------------------------------
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT) continue;
            const int up = upstreamNode(ctx, l);
            const double need = links.volume[ul] - store_.total_volume(l);
            if (need <= 0.0 || up < 0) continue;
            const auto uu = static_cast<std::size_t>(up);
            for (int p = 0; p < np; ++p)
                scratch_[static_cast<std::size_t>(p)] =
                    nodes.conc[uu * static_cast<std::size_t>(np) +
                               static_cast<std::size_t>(p)];
            store_.push_front(l, need, scratch_.data());
        }

        // ---- 4. DECAY (exact exponential; species-major stripes) ----------
        for (int p = 0; p < np; ++p) {
            const double k = ctx.pollutants.k_decay[static_cast<std::size_t>(p)];
            if (k == 0.0) continue;
            const double f = std::exp(-k * dt);
            double removed = store_.decay_species(p, f);
            for (int n = 0; n < nn; ++n) {
                const auto idx = static_cast<std::size_t>(n) *
                                     static_cast<std::size_t>(np) +
                                 static_cast<std::size_t>(p);
                const double v = nodes.volume[static_cast<std::size_t>(n)];
                removed += nodes.conc[idx] * (1.0 - f) * v;
                nodes.conc[idx] *= f;
            }
            if (static_cast<std::size_t>(p) <
                ctx.mass_balance.qual_routing_reacted.size())
                ctx.mass_balance.qual_routing_reacted[
                    static_cast<std::size_t>(p)] += removed;
        }

        // ---- 5. PUBLISH ---------------------------------------------------
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT) continue;
            store_.mean_conc(l, scratch_.data());
            for (int p = 0; p < np; ++p)
                links.conc[ul * static_cast<std::size_t>(np) +
                           static_cast<std::size_t>(p)] =
                    scratch_[static_cast<std::size_t>(p)];
        }
        // conc_old bookkeeping matches the ARD/legacy convention.
        links.conc_old = links.conc;
        nodes.conc_old = nodes.conc;
    }

private:
    void init(SimulationContext& ctx) {
        const int np = ctx.n_pollutants();
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        store_.resize(nl, np);
        flow_sign_.assign(static_cast<std::size_t>(nl), 1);
        node_mass_in_.assign(
            static_cast<std::size_t>(nn) * static_cast<std::size_t>(np), 0.0);
        node_vol_in_.assign(static_cast<std::size_t>(nn), 0.0);
        scratch_.assign(static_cast<std::size_t>(np), 0.0);
        node_out_links_.assign(static_cast<std::size_t>(nn), {});

        // Seed: one segment per conduit at the link's current volume and
        // (initQuality-seeded) concentration — a dry link seeds nothing.
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            store_.clear_link(l);
            if (ctx.links.type[ul] != LinkType::CONDUIT) continue;
            const double v = ctx.links.volume[ul];
            if (v <= 0.0) continue;
            for (int p = 0; p < np; ++p)
                scratch_[static_cast<std::size_t>(p)] =
                    ctx.links.conc[ul * static_cast<std::size_t>(np) +
                                   static_cast<std::size_t>(p)];
            store_.push_front(l, v, scratch_.data());
            flow_sign_[ul] = (ctx.links.flow[ul] >= 0.0) ? 1 : -1;
        }
        computeTopoOrder(ctx);
        initialized_ = true;
    }

    int upstreamNode(const SimulationContext& ctx, int l) const {
        const auto ul = static_cast<std::size_t>(l);
        return (flow_sign_[ul] >= 0) ? ctx.links.node1[ul]
                                     : ctx.links.node2[ul];
    }
    int downstreamNode(const SimulationContext& ctx, int l) const {
        const auto ul = static_cast<std::size_t>(l);
        return (flow_sign_[ul] >= 0) ? ctx.links.node2[ul]
                                     : ctx.links.node1[ul];
    }

    void addToLedger(int n, double vol, const double* mass, int np) {
        const auto un = static_cast<std::size_t>(n);
        node_vol_in_[un] += vol;
        for (int p = 0; p < np; ++p)
            node_mass_in_[un * static_cast<std::size_t>(np) +
                          static_cast<std::size_t>(p)] +=
                mass[static_cast<std::size_t>(p)];
    }
    /// Ledger add where `conc` (not mass) is supplied — passthrough links.
    void addToLedgerRate(int n, double vol, const double* conc, int np) {
        const auto un = static_cast<std::size_t>(n);
        node_vol_in_[un] += vol;
        for (int p = 0; p < np; ++p)
            node_mass_in_[un * static_cast<std::size_t>(np) +
                          static_cast<std::size_t>(p)] +=
                vol * conc[static_cast<std::size_t>(p)];
    }

    /// Kahn over the node graph, edges from links with |Q| > kTinyFlow in
    /// current flow direction (§4.3). Cycles: leftovers appended in index
    /// order — their same-step passthrough mass carries in the ledger to
    /// the next step instead of being dropped.
    void computeTopoOrder(SimulationContext& ctx) {
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        std::vector<int> indeg(static_cast<std::size_t>(nn), 0);
        for (auto& v : node_out_links_) v.clear();
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            const int up = upstreamNode(ctx, l);
            const int dn = downstreamNode(ctx, l);
            if (up < 0 || dn < 0) continue;
            node_out_links_[static_cast<std::size_t>(up)].push_back(l);
            if (std::abs(ctx.links.flow[ul]) > kTinyFlow)
                indeg[static_cast<std::size_t>(dn)] += 1;
        }
        topo_.clear();
        topo_.reserve(static_cast<std::size_t>(nn));
        std::vector<int> q;
        for (int n = 0; n < nn; ++n)
            if (indeg[static_cast<std::size_t>(n)] == 0) q.push_back(n);
        std::vector<char> seen(static_cast<std::size_t>(nn), 0);
        std::size_t qi = 0;
        while (qi < q.size()) {
            const int n = q[qi++];
            if (seen[static_cast<std::size_t>(n)]) continue;
            seen[static_cast<std::size_t>(n)] = 1;
            topo_.push_back(n);
            for (const int l : node_out_links_[static_cast<std::size_t>(n)]) {
                if (std::abs(ctx.links.flow[static_cast<std::size_t>(l)]) <=
                    kTinyFlow)
                    continue;
                const int dn = downstreamNode(ctx, l);
                if (dn >= 0 && --indeg[static_cast<std::size_t>(dn)] == 0)
                    q.push_back(dn);
            }
        }
        for (int n = 0; n < nn; ++n)  // cycle leftovers
            if (!seen[static_cast<std::size_t>(n)]) topo_.push_back(n);
    }

    SegmentStore store_;
    std::vector<int> topo_;
    std::vector<std::vector<int>> node_out_links_;
    std::vector<std::int8_t> flow_sign_;
    std::vector<double> node_mass_in_;  ///< per-step ledger, mass
    std::vector<double> node_vol_in_;   ///< per-step ledger, volume
    std::vector<double> scratch_;       ///< np-sized work array
    bool initialized_ = false;
};

}  // namespace lard
}  // namespace openswmm

#endif  // OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
