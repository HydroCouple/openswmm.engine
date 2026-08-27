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
 * @file KinematicWave.cpp
 * @brief Kinematic wave routing — batch-oriented, numerically identical to legacy.
 *
 * @details The solver is structured as:
 *   1. Gather inflows for all conduits
 *   2. Batch-compute inlet areas from inflows (via section factor inversion)
 *   3. Per-conduit Newton solve for outlet area (grouped by shape where possible)
 *   4. Batch-compute outflows from outlet areas (via section factor)
 *   5. Scatter results back to global link arrays
 *
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "KinematicWave.hpp"
#include "Link.hpp"
#include "XSectBatch.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "HydStructures.hpp"
#include "Node.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace kinwave {

static constexpr int    MAX_ITERS = 40;
static constexpr double TINY      = 1.0e-6;

// Storage successive-approximation constants (legacy flowrout.c:56-58).
static constexpr int    STOR_MAXITER = 10;
static constexpr double STOR_OMEGA   = 0.55;
static constexpr double STOR_STOPTOL = 0.005;

// ============================================================================
// Tree-layout routing helpers (legacy flowrout.c) — shared with STEADY
// ============================================================================

// MERGE NOTE: upstream declared this over (const LinkData&, size_t); this
// branch's definition takes the whole context so it can attach a compiled
// boundary (xs.cheb) — without it, a POLYGON conduit's getAofY/getSofA
// below silently return 0. Declaration matched to the real definition.
static XSectParams buildXSP_KW(const SimulationContext& ctx, std::size_t uk);

/// PARITY node.c:1008 storage_getOutflow — flow from a storage unit into a
/// CONDUIT is the conduit's NORMAL-DEPTH flow at the pond's current depth,
/// not the pond's inflow. (Non-conduit outlets have their own head-discharge
/// functions; legacy returns 0 for them here.)
static double storageConduitOutflow(SimulationContext& ctx, int i, int j) {
    const auto uj = static_cast<std::size_t>(j);
    const auto ui = static_cast<std::size_t>(i);
    if (ctx.links.type[uj] != LinkType::CONDUIT) return 0.0;

    const double y = ctx.nodes.depth[ui] - ctx.links.offset1[uj];
    if (y <= 0.0) return 0.0;

    const auto& CD = ctx.link_subtypes.conduits;
    const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));
    if (y >= ctx.links.xsect_y_full[uj]) return CD.q_full[ucr];

    const XSectParams xs = buildXSP_KW(ctx, uj);
    const double a = xsect::getAofY(xs, y);
    return CD.beta[ucr] * xsect::getSofA(xs, a);
}

double getLinkInflow(SimulationContext& ctx,
                     hydstruct::StructureSolver* structures,
                     int j, double dt) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    const auto uj = static_cast<std::size_t>(j);

    const int n1 = links.node1[uj];
    if (n1 < 0) return 0.0;
    const auto un1 = static_cast<std::size_t>(n1);

    const LinkType lt = links.type[uj];
    double q = 0.0;

    if (lt == LinkType::CONDUIT) {
        // PARITY link.c conduit_getInflow → node_getOutflow: a conduit
        // draining a STORAGE unit carries normal-depth flow at the pond's
        // depth; off any other node it carries that node's inflow.
        q = (nodes.type[un1] == NodeType::STORAGE)
            ? storageConduitOutflow(ctx, n1, j)
            : nodes.inflow[un1];
    } else if (lt == LinkType::PUMP || nodes.type[un1] == NodeType::STORAGE) {
        // PARITY link.c:546 link_getInflow — evaluate the structure's own
        // head-discharge relation at the upstream node's CURRENT depth.
        if (structures) {
            const double q_saved = links.flow[uj];
            structures->computeNonConduitFlowOne(ctx, dt, nullptr, j);
            q = links.flow[uj];
            // The trial evaluations inside updateStorageState must not leave
            // a flow behind; the caller stores the accepted value explicitly.
            links.flow[uj] = q_saved;
        }
    }
    // else: a non-conduit draining a non-storage node carries no flow under
    // KW/SF (legacy `else q = 0.0`) — there is no head to drive it.

    return node::getMaxOutflow(nodes, n1, q, dt);
}

void updateStorageState(SimulationContext& ctx,
                        hydstruct::StructureSolver* structures,
                        const std::vector<int>& order,
                        int pos, int i, double dt) {
    auto& nodes = ctx.nodes;
    const auto ui = static_cast<std::size_t>(i);
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    // --- terms of the flow balance that do not depend on end-of-step depth
    //     (legacy flowrout.c:563-565). The trapezoidal average of old and new
    //     net inflow is what makes this second-order in time.
    const double v_fixed = nodes.old_volume[ui]
        + 0.5 * (nodes.old_net_inflow[ui] + nodes.inflow[ui] - nodes.outflow[ui]) * dt;

    const double full_vol = node::getVolume(nodes, i, nodes.full_depth[ui],
                                            &ctx.tables, us, &ctx.node_subtypes);

    // --- sum the outflow of every link this node drains through, at the
    //     current depth estimate (legacy getStorageOutflow). The sorted order
    //     emits a node's outgoing links contiguously.
    auto storageOutflow = [&]() {
        double outflow = 0.0;
        for (std::size_t k = static_cast<std::size_t>(pos); k < order.size(); ++k) {
            const int m = order[k];
            if (ctx.links.node1[static_cast<std::size_t>(m)] != i) break;
            outflow += getLinkInflow(ctx, structures, m, dt);
        }
        return outflow;
    };

    double d1 = nodes.depth[ui];
    for (int iter = 0; iter < STOR_MAXITER; ++iter) {
        double v2 = v_fixed - 0.5 * storageOutflow() * dt;
        v2 = std::max(v2, 0.0);

        nodes.overflow[ui] = 0.0;
        if (v2 > full_vol) {
            nodes.overflow[ui] =
                (v2 - std::max(nodes.old_volume[ui], full_vol)) / dt;
            if (nodes.overflow[ui] < constants::FUDGE) nodes.overflow[ui] = 0.0;
            if (!ctx.options.allow_ponding || nodes.ponded_area[ui] == 0.0)
                v2 = full_vol;
        }

        nodes.volume[ui] = v2;
        double d2 = node::getDepth(nodes, i, v2, &ctx.tables, us,
                                   &ctx.node_subtypes);
        nodes.depth[ui] = d2;

        // Under-relaxation on the DEPTH iterate only — the volume and depth
        // written above stay the un-relaxed pair that satisfies the balance.
        d2 = (1.0 - STOR_OMEGA) * d1 + STOR_OMEGA * d2;
        const bool stop = std::fabs(d2 - d1) <= STOR_STOPTOL;
        d1 = d2;
        if (stop) break;
    }
    nodes.head[ui] = nodes.invert_elev[ui] + nodes.depth[ui];
}

// ============================================================================
// Init
// ============================================================================

void KWSolver::init(int n_conduits, const XSectGroups& /*groups*/) {
    n_conduits_ = n_conduits;
    auto un = static_cast<std::size_t>(n_conduits);

    q1_.assign(un, 0.0);
    a1_.assign(un, 0.0);
    q2_.assign(un, 0.0);
    a2_.assign(un, 0.0);

    q_in_.resize(un);
    a_in_.resize(un);
    q_out_.resize(un);
    a_out_.resize(un);
    sf_in_.resize(un);
}

// ============================================================================
// Per-conduit Newton solve
// ============================================================================

int KWSolver::solveConduit(int idx, const XSectParams& xs,
                            double q_full, double a_full, double s_full,
                            double beta, double length, double dt,
                            double loss_rate) {
    auto ui = static_cast<std::size_t>(idx);
    if (q_full <= 0.0 || a_full <= 0.0) {
        q_out_[ui] = 0.0;
        a_out_[ui] = 0.0;
        return 0;
    }

    // Normalise
    double q_in_norm = q_in_[ui] / q_full;
    double q3 = loss_rate / q_full;
    double beta1 = beta / q_full;

    double prev_q1 = q1_[ui] / q_full;
    double prev_a1 = a1_[ui] / a_full;
    double prev_q2 = q2_[ui] / q_full;
    double prev_a2 = a2_[ui] / a_full;

    // Normalised inlet area
    double a_in_norm;
    if (q_in_norm >= 1.0) {
        a_in_norm = 1.0;
    } else if (q_in_norm <= 0.0) {
        a_in_norm = 0.0;
    } else {
        double s_needed = q_in_norm / beta1;  // dimensional section factor = Q_in/beta
        a_in_norm = xsect::getAofS(xs, s_needed) / a_full;
    }

    // Finite-difference coefficients
    double dxdt = length / dt * a_full / q_full;
    double dq = prev_q2 - prev_q1;

    double C1 = dxdt * WT / WX;
    double C2 = (1.0 - WT) * (a_in_norm - prev_a1);
    C2 -= WT * prev_a2;
    C2 *= dxdt / WX;
    C2 += (1.0 - WX) / WX * dq - q_in_norm;
    C2 += q3 / WX;

    // Gap #59: bound the Newton iteration using the same Amax bracket that
    // legacy kinwave.c uses (findroot_Newton with aLo/aHi bounds).
    //
    // The section factor S(a) peaks at a = Amax (< a_full for non-circular
    // shapes). Above Amax, S decreases back toward s_full, so the continuity
    // function f(a) = beta1*S(a) + C1*a + C2 can have two roots.
    // Legacy pre-screens for this:
    //   aHi = 1.0 (full area),   fHi = 1 + C1 + C2
    //   aLo = getAmax(xs),       fLo = beta1*s_max + C1*aLo + C2
    // If fLo and fHi share the same sign, reset the bracket:
    //   [0, aLo] → handles near-zero-flow and high-flow cases
    // If both bounds produce negative f → full flow (no sub-critical root).
    // If both bounds produce positive f → zero flow.
    double aHi = 1.0;
    double fHi = 1.0 + C1 + C2;     // f(a=1.0): beta1*s_full = 1 by construction
    double aLo = xsect::getAmax(xs); // normalized area at max section factor
    double fLo = beta1 * xs.s_max + C1 * aLo + C2;

    if (aLo >= aHi) { aLo = 0.0; fLo = C2; }  // shouldn't happen; guard anyway

    if (fHi * fLo > 0.0) {
        // Same sign — root is not between [aLo, aHi]; reset bracket to [0, aLo]
        aHi = aLo;
        fHi = fLo;
        aLo = 0.0;
        fLo = C2;
    }

    // Both bounds negative → flow always exceeds maximum; use full flow
    if (fLo < 0.0 && fHi < 0.0) {
        a_in_[ui]  = a_in_norm * a_full;
        a_out_[ui] = a_full;   // full-pipe area
        q_out_[ui] = q_full;   // cap at full flow
        q1_[ui] = q_in_[ui];  a1_[ui] = a_in_[ui];
        q2_[ui] = q_out_[ui]; a2_[ui] = a_out_[ui];
        return -2;
    }

    // Both bounds positive → no flow
    if (fLo > 0.0 && fHi > 0.0) {
        a_in_[ui]  = a_in_norm * a_full;
        a_out_[ui] = 0.0;
        q_out_[ui] = 0.0;
        q1_[ui] = q_in_[ui];  a1_[ui] = a_in_[ui];
        q2_[ui] = 0.0;        a2_[ui] = 0.0;
        return -3;
    }

    // Ensure fLo < fHi for monotone bracketing
    if (fLo > fHi) {
        std::swap(aLo, aHi);
        std::swap(fLo, fHi);
    }

    // Newton-Raphson: solve f(a) = beta1*S(a*Afull) + C1*a + C2 = 0
    // Initial guess: previous outlet area (warm start), clamped to [aLo, aHi].
    double a = (prev_a2 > TINY) ? prev_a2 : a_in_norm;
    a = std::max(std::min(a, aHi), aLo);

    int iters = 0;
    for (; iters < MAX_ITERS; ++iters) {
        double a_abs = a * a_full;
        double s = xsect::getSofA(xs, a_abs);
        double f = beta1 * s + C1 * a + C2;

        double dsda = xsect::getdSdA(xs, a_abs);
        double df = beta1 * a_full * dsda + C1;

        if (std::fabs(df) < TINY) break;

        double da = -f / df;
        a += da;
        // Clamp to bracket so Newton doesn't wander past Amax or below zero
        a = std::max(std::min(a, aHi), aLo);
        if (std::fabs(da) < EPSIL) break;
    }
    a = std::max(a, 0.0);

    // Outflow from outlet area
    double s_out = xsect::getSofA(xs, a * a_full);
    double q_out_norm = beta1 * s_out;
    q_out_norm = std::max(q_out_norm, 0.0);

    // De-normalise and store
    a_in_[ui]  = a_in_norm * a_full;
    a_out_[ui] = a * a_full;
    q_out_[ui] = q_out_norm * q_full;

    // Update state for next timestep
    q1_[ui] = q_in_[ui];
    a1_[ui] = a_in_[ui];
    q2_[ui] = q_out_[ui];
    a2_[ui] = a_out_[ui];

    return iters;
}

// ============================================================================
// Main execute — batch-oriented
// ============================================================================

/// Build XSectParams from link SoA data (matching DynamicWave.cpp::buildXSP).
static XSectParams buildXSP_KW(const SimulationContext& ctx, std::size_t uk) {
    const LinkData& links = ctx.links;
    XSectParams xs{};
    // link::translateShape is the canonical LinkData-enum -> batch-enum
    // translation (Link.cpp) — POLYGON=26 was appended to both enums at the
    // same numeric value, breaking the flat +1 offset every earlier shape
    // follows, so this delegates rather than re-deriving the mapping here.
    xs.type = link::translateShape(links.xsect_shape[uk]);
    xs.y_full = links.xsect_y_full[uk];
    xs.a_full = links.xsect_a_full[uk];
    xs.w_max  = links.xsect_w_max[uk];
    xs.r_full = links.xsect_r_full[uk];
    xs.s_full = links.xsect_s_full[uk];
    xs.s_max  = links.xsect_s_max[uk];
    xs.y_bot  = links.xsect_y_bot[uk];
    xs.a_bot  = links.xsect_a_bot[uk];
    xs.s_bot  = links.xsect_s_bot[uk];
    xs.r_bot  = links.xsect_r_bot[uk];
    {
        const int ci = links.xsect_cheb_idx[uk];
        if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.cheb_sections.size())
            xs.cheb = &ctx.cheb_sections[static_cast<std::size_t>(ci)];
    }
    return xs;
}

int KWSolver::execute(SimulationContext& ctx, double dt,
                      hydstruct::StructureSolver* structures) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    int total_iters = 0;
    int n_solved = 0;

    storage_updated_.assign(static_cast<std::size_t>(ctx.n_nodes()), 0);

    // Process links in topological order (upstream → downstream).
    // If no sorted order set, fall back to natural order.
    const auto& order = sorted_links_.empty()
        ? [&]() -> const std::vector<int>& {
            // Build a simple 0..n_links order as fallback
            static thread_local std::vector<int> fallback;
            fallback.resize(static_cast<std::size_t>(ctx.n_links()));
            for (int j = 0; j < ctx.n_links(); ++j) fallback[static_cast<std::size_t>(j)] = j;
            return fallback;
          }()
        : sorted_links_;

    for (int idx = 0; idx < static_cast<int>(order.size()); ++idx) {
        int j = order[static_cast<std::size_t>(idx)];
        auto uj = static_cast<std::size_t>(j);

        // PARITY flowrout.c:181-183 — if this link drains a storage unit,
        // converge that unit's depth BEFORE routing the link, so an outlet
        // structure is evaluated at the end-of-step depth it actually sees.
        // `updated` guards the node against a second pass when it has more
        // than one outlet link (legacy Node[i].updated).
        {
            int n1s = links.node1[uj];
            if (n1s >= 0 && nodes.type[static_cast<std::size_t>(n1s)] == NodeType::STORAGE
                && !storage_updated_[static_cast<std::size_t>(n1s)]) {
                updateStorageState(ctx, structures, order, idx, n1s, dt);
                storage_updated_[static_cast<std::size_t>(n1s)] = 1;
            }
        }

        // Non-conduit links (pump / orifice / weir / outlet) are not routed —
        // kinwave_execute returns *qoutflow = *qinflow for them (legacy
        // kinwave.c:113-117) — but their INFLOW is their own head-discharge
        // relation, not the upstream node's inflow (legacy getLinkInflow).
        if (links.type[uj] != LinkType::CONDUIT) {
            int n1 = links.node1[uj];
            int n2 = links.node2[uj];
            double q = getLinkInflow(ctx, structures, j, dt);
            links.flow[uj] = q;
            if (n1 >= 0) nodes.outflow[static_cast<std::size_t>(n1)] += q;
            if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += q;
            continue;
        }

        // Skip dummy cross-sections
        if (links.xsect_shape[uj] == XsectShape::DUMMY) {
            int n1 = links.node1[uj];
            int n2 = links.node2[uj];
            if (n1 >= 0 && n2 >= 0) {
                double q = nodes.inflow[static_cast<std::size_t>(n1)];
                links.flow[uj] = q;
                nodes.inflow[static_cast<std::size_t>(n2)] += q;
            }
            continue;
        }
        auto& CD = ctx.link_subtypes.conduits;
        const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));

        // Gather inflow from upstream node
        // (matching legacy getLinkInflow: use node inflow, limited by max outflow)
        int n1 = links.node1[uj];
        double qin = 0.0;
        if (n1 >= 0) {
            auto un1 = static_cast<std::size_t>(n1);
            qin = (nodes.type[un1] == NodeType::STORAGE)
                ? storageConduitOutflow(ctx, n1, j)
                : nodes.inflow[un1];
            // Limit by available volume at node (prevent negative depth)
            double q_max = node::getMaxOutflow(nodes, n1, qin, dt);
            qin = std::min(qin, q_max);
        }

        // Divide by barrels (KW solves per barrel)
        double barrels = static_cast<double>(std::max(CD.barrels[ucr], 1));
        double qin_per_barrel = qin / barrels;

        // Build XSectParams for this conduit
        XSectParams xs = buildXSP_KW(ctx, uj);

        double q_full = CD.q_full[ucr];
        double a_full = links.xsect_a_full[uj];
        double s_full = links.xsect_s_full[uj];
        double beta   = CD.beta[ucr];
        double length = CD.mod_length[ucr];
        if (length <= 0.0) length = CD.length[ucr];

        // Compute evaporation + seepage loss rate
        double loss_rate = CD.evap_loss_rate[ucr] + CD.seep_loss_rate[ucr];

        // Set inflow for this conduit
        q_in_[uj] = qin_per_barrel;

        // Solve continuity equation (Newton-Raphson)
        int iters = solveConduit(static_cast<int>(uj), xs,
                                  q_full, a_full, s_full,
                                  beta, length, dt, loss_rate);
        total_iters += iters;
        n_solved++;

        // Update link flow (multiply by barrels)
        double qout = q_out_[uj] * barrels;
        qin = q_in_[uj] * barrels;  // may have been capped at qFull
        links.flow[uj] = qout;

        // Update node flows
        if (n1 >= 0) {
            nodes.outflow[static_cast<std::size_t>(n1)] += qin;
        }
        int n2 = links.node2[uj];
        if (n2 >= 0) {
            nodes.inflow[static_cast<std::size_t>(n2)] += qout;
        }

        // Update link depth and volume from inlet/outlet areas
        double y_in  = xsect::getYofA(xs, a_in_[uj]);
        double y_out = xsect::getYofA(xs, a_out_[uj]);
        links.depth[uj]  = 0.5 * (y_in + y_out);
        links.volume[uj] = 0.5 * (a_in_[uj] + a_out_[uj]) * length * barrels;

        // Gap #57: persist full-pipe state (bit 0 = upstream, bit 1 = downstream)
        {
            int8_t fs = 0;
            if (a_full > 0.0) {
                if (a_in_[uj]  >= a_full) fs |= 1;
                if (a_out_[uj] >= a_full) fs |= 2;
            }
            CD.full_state[ucr] = fs;
        }

        // Update non-storage end-node depths (Gap #13)
        // Matches legacy setNewLinkState/updateNodeDepth in flowrout.c:
        //   non-storage nodes get max(current_depth, conduit_end_depth + offset)
        auto updateNodeDepth = [&](int ni, double y_conduit, double link_offset) {
            if (ni < 0) return;
            auto uni = static_cast<std::size_t>(ni);
            NodeType nt = nodes.type[uni];
            if (nt == NodeType::STORAGE) return;  // storage updated separately
            double y = y_conduit + link_offset;
            // If flooded non-outfall, clamp to full depth
            if (nt != NodeType::OUTFALL && nodes.overflow[uni] > 0.0)
                y = nodes.full_depth[uni];
            // Only raise depth, never lower (take max)
            if (nodes.depth[uni] < y) {
                nodes.depth[uni] = std::min(y, nodes.full_depth[uni] > 0.0
                                              ? nodes.full_depth[uni] : y);
                nodes.head[uni] = nodes.invert_elev[uni] + nodes.depth[uni];
            }
        };
        updateNodeDepth(n1, y_in,  links.offset1[uj]);
        updateNodeDepth(n2, y_out, links.offset2[uj]);
    }

    return (n_solved > 0) ? total_iters / n_solved : 1;
}

} // namespace kinwave
} // namespace openswmm
