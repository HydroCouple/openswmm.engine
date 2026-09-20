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
#include "XSectBatch.hpp"
#include "Link.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "HydStructures.hpp"
#include "Node.hpp"
#include "Divider.hpp"
#include "../math/FindRoot.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace kinwave {

static constexpr double TINY      = 1.0e-6;

// Storage successive-approximation constants (legacy flowrout.c:56-58).
static constexpr int    STOR_MAXITER = 10;
static constexpr double STOR_OMEGA   = 0.55;
static constexpr double STOR_STOPTOL = 0.005;

// ============================================================================
// Tree-layout routing helpers (legacy flowrout.c) — shared with STEADY
// ============================================================================

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

    // PARITY link.c:553 link_getInflow: a link whose setting is 0 (closed
    // by a control rule) admits NO inflow, before any type dispatch — the
    // conduit still routes (kinwave_execute runs with qin = 0) and drains.
    if (links.setting[uj] == 0.0) return 0.0;

    if (lt == LinkType::CONDUIT) {
        // PARITY link.c conduit_getInflow → node_getOutflow: a conduit
        // draining a STORAGE unit carries normal-depth flow at the pond's
        // depth; off a DIVIDER it carries that divider's split; off any
        // other node it carries that node's inflow + overflow (node.c:395).
        // qLimit then caps the inflow (link.c:1331).
        if (nodes.type[un1] == NodeType::STORAGE)
            q = storageConduitOutflow(ctx, n1, j);
        else if (nodes.type[un1] == NodeType::DIVIDER)
            q = divider::getOutflow(ctx, n1, j);
        else
            q = nodes.inflow[un1] + nodes.overflow[un1];
        const double qlim = links.q_limit[uj];
        if (qlim > 0.0 && q > qlim) q = qlim;
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
    // legacy: `iter = 1; while (iter < MAXITER && !stopped)` — at most
    // MAXITER - 1 = 9 passes. A storage whose pump switches at the trial
    // depth oscillates between two states, and the pass count decides
    // which one the step keeps (304-nodes-234-subs' wet well 1: a tenth pass
    // handed PumpSR the ON state legacy leaves OFF).
    for (int iter = 1; iter < STOR_MAXITER; ++iter) {
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
    y1_.assign(un, 0.0);
    y2_.assign(un, 0.0);
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

    // Legacy kinwave.c early no-flow branch (`qin <= TINY && q2 <= TINY`):
    // negligible inflow with a negligible previous outflow yields EXACT
    // zeros with no continuity solve. Skipping this let the Newton solve
    // produce ~1e-8 outflows from a dry start, seeding a startup transient
    // that took ~250 report periods to relax (extran1-kw-divider).
    if (q_in_norm <= TINY && prev_q2 <= TINY) {
        a_in_[ui]  = a_in_norm * a_full;
        a_out_[ui] = 0.0;
        q_out_[ui] = 0.0;
        // Same normalise → denormalise round trip as the main path below.
        if (q_in_norm > 1.0) q_in_norm = 1.0;
        q_in_[ui] = q_in_norm * q_full;
        q1_[ui] = q_in_[ui]; a1_[ui] = a_in_[ui];
        q2_[ui] = 0.0;       a2_[ui] = 0.0;
        return 1;
    }

    // Finite-difference coefficients
    double dxdt = length / dt * a_full / q_full;
    double dq = prev_q2 - prev_q1;

    double C1 = dxdt * WT / WX;
    double C2 = (1.0 - WT) * (a_in_norm - prev_a1);
    C2 -= WT * prev_a2;
    // legacy's statements, left to right: (C2 * dxdt) / WX, then
    // (C2 + k*dq) - qin — not C2 * (dxdt / WX) or C2 + (k*dq - qin), each
    // an ulp off on the first wet steps (runoff2-sw5).
    C2 = C2 * dxdt / WX;
    C2 = C2 + (1.0 - WX) / WX * dq - q_in_norm;
    C2 = C2 + q3 / WX;

    // legacy solveContinuity (kinwave.c), op for op: bracket f(a) =
    // Beta1*S(a) + C1*a + C2 between the area of maximum section factor
    // and full area, fall back to [0, aLo] when both ends share a sign,
    // hand the previous outlet area (or the bracket's midpoint when it lies
    // outside) to findroot_Newton with the ends switched when f(aLo) >
    // f(aHi); both ends negative = full flow, both positive = no flow. The
    // former home-grown Newton loop (own start guess, per-step bracket
    // clamps, its own stop test) took a different path on the first wet
    // step of every kinematic-wave conduit (runoff2-sw5's 101: 3.77e-5 vs
    // legacy 3.40e-5 cfs).
    double aHi = 1.0;
    double fHi = 1.0 + C1 + C2;
    // xsect_getAmax: Amax[type]*aFull, or aBot for IRREGULAR / CUSTOM
    // (the kernel's getAmax is the ratio Amax[type]).
    double aLo = (xs.type == static_cast<int>(XSectShape::IRREGULAR) ||
                  xs.type == static_cast<int>(XSectShape::CUSTOM))
                 ? xs.a_bot / a_full : xsect::getAmax(xs);
    double fLo;
    if (aLo < aHi) fLo = (beta1 * xs.s_max) + (C1 * aLo) + C2;
    else           fLo = fHi;

    if (fHi * fLo > 0.0) {
        aHi = aLo;
        fHi = fLo;
        aLo = 0.0;
        fLo = C2;
    }

    double a = prev_a2;
    int result;
    if (fHi * fLo <= 0.0) {
        if (a < aLo || a > aHi) a = 0.5 * (aLo + aHi);
        if (fLo > fHi) {
            const double aTmp = aLo;
            aLo = aHi;
            aHi = aTmp;
        }
        result = findroot::newton(aLo, aHi, &a, EPSIL,
            [&](double an, double* f, double* df) {
                *f  = (beta1 * xsect::getSofA(xs, an * a_full)) + (C1 * an) + C2;
                *df = (beta1 * a_full * xsect::getdSdA(xs, an * a_full)) + C1;
            });
        if (result <= 0) result = -1;
    } else if (fLo < 0.0) {
        // both bounds negative → full flow
        a = (q_in_norm > 1.0) ? a_in_norm : 1.0;
        result = -2;
    } else if (fLo > 0.0) {
        // both bounds positive → no flow
        a = 0.0;
        result = -3;
    } else {
        result = -1;
    }
    // legacy reports ERR_KINWAVE and returns when the root finder fails;
    // the engine keeps the finder's last iterate (the bracket holds it).
    if (result <= 0) result = 1;

    // Outflow from outlet area (legacy: qout = Beta1 * S(aout*Afull))
    double q_out_norm = beta1 * xsect::getSofA(xs, a * a_full);

    // De-normalise and store
    a_in_[ui]  = a_in_norm * a_full;
    a_out_[ui] = a * a_full;
    q_out_[ui] = q_out_norm * q_full;

    // Legacy post-solve inflow cap (kinwave.c: `if (qin > 1.0) qin = 1.0`) —
    // the accepted inflow returned to the node never exceeds qFull. Legacy
    // caps the NORMALISED inflow and then writes `Conduit[k].q1 = qin * Qfull`,
    // so the value handed back to the node has been through a divide by Qfull
    // and a multiply by it again — a round trip that is NOT the identity in
    // IEEE-754. Keeping the raw dimensional inflow instead left the upstream
    // node's outflow 1 ULP light (1710-2014-20year-r3's J1 at step 6).
    if (q_in_norm > 1.0) q_in_norm = 1.0;
    q_in_[ui] = q_in_norm * q_full;

    // Update state for next timestep
    q1_[ui] = q_in_[ui];
    a1_[ui] = a_in_[ui];
    q2_[ui] = q_out_[ui];
    a2_[ui] = a_out_[ui];

    return result;
}

// ============================================================================
// Main execute — batch-oriented
// ============================================================================

/// Build XSectParams from link SoA data (matching DynamicWave.cpp::buildXSP).
static XSectParams buildXSP_KW(const SimulationContext& ctx, std::size_t uk) {
    const LinkData& links = ctx.links;
    XSectParams xs{};
    auto ls = links.xsect_shape[uk];
    xs.type = link::translateShape(ls);
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
    // Tabulated shapes (IRREGULAR / CUSTOM / STREET) carry their A/R/W vs depth
    // in per-link transect tables; without them every scalar getter returns 0.
    // Under KW that made getAofS's Newton walk its whole bracket and report the
    // conduit's FULL inlet area for a trickle of inflow (1710-2014-20year-r3's
    // transect channels), so the upstream node depth came out full instead of
    // ~2 mm. Same block as DynamicWave.cpp::buildXSP.
    if (ls == XsectShape::IRREGULAR || ls == XsectShape::CUSTOM ||
        ls == XsectShape::STREET_XSECT) {
        const int ci = links.xsect_curve[uk];
        if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
            const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
            xs.transect          = ci;
            xs.area_tbl          = td.area_tbl;
            xs.hrad_tbl          = td.hrad_tbl;
            xs.width_tbl         = td.width_tbl;
            xs.area_lut          = &td.area_lut;
            xs.transect_tbl_size = transect::N_TRANSECT_TBL;
        }
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
    std::fill(y1_.begin(), y1_.end(), 0.0);
    std::fill(y2_.begin(), y2_.end(), 0.0);

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

        // Dummy cross-sections: legacy routes them like every other link —
        // getLinkInflow (divider/storage dispatch, qLimit and max-outflow
        // caps) then kinwave_execute returns qout = qin (kinwave.c:113-117)
        // — and scatters BOTH the upstream node's outflow and the
        // downstream node's inflow (flowrout.c:196-197).
        if (links.xsect_shape[uj] == XsectShape::DUMMY) {
            int n1 = links.node1[uj];
            int n2 = links.node2[uj];
            double q = getLinkInflow(ctx, structures, j, dt);
            links.flow[uj] = q;
            if (n1 >= 0) nodes.outflow[static_cast<std::size_t>(n1)] += q;
            if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += q;
            // legacy kinwave_execute leaves a dummy's a1/a2 at 0, so
            // setNewLinkState reports depth 0 and volume 0 (extran1-dummy's
            // 8040 kept the FUDGE depth its initialisation left).
            links.depth[uj]  = 0.0;
            links.volume[uj] = 0.0;
            continue;
        }
        auto& CD = ctx.link_subtypes.conduits;
        const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));

        // Gather inflow from upstream node
        // (matching legacy getLinkInflow: use node inflow, limited by max outflow)
        int n1 = links.node1[uj];
        double qin = 0.0;
        if (n1 >= 0) {
            // Legacy getLinkInflow (flowrout.c:517): storage/divider/junction
            // dispatch, the conduit's qLimit, then the max-outflow cap.
            qin = getLinkInflow(ctx, structures, j, dt);
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
        // legacy kinwave.c:126 / flowrout.c:507 take link_getLength(j) —
        // the routing length. Under KW modLength never differs from the
        // authored length (lengthening is dynamic-wave only), but an
        // IRREGULAR conduit's routing length does.
        double length = CD.true_length[ucr];
        if (length <= 0.0) length = CD.length[ucr];

        // Evaporation + seepage loss rate, capped on this solve's per-barrel
        // inflow (legacy link_getLossRate(j, KW, qin*Qfull, tStep)).
        double loss_rate = capConduitLoss(ctx, ucr, std::fabs(qin_per_barrel));

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
        y1_[uj] = y_in;
        y2_[uj] = y_out;

        // Gap #57: persist full-pipe state (bit 0 = upstream, bit 1 = downstream)
        {
            int8_t fs = 0;
            if (a_full > 0.0) {
                if (a_in_[uj]  >= a_full) fs |= 1;
                if (a_out_[uj] >= a_full) fs |= 2;
            }
            CD.full_state[ucr] = fs;
        }
        // Node depths are raised AFTER the per-node volume/overflow pass, in
        // finishRouting — legacy runs setNewNodeState for every node before
        // any setNewLinkState raises a depth (flowrout.c:203-205).
    }

    // Legacy end-of-step passes: setNewNodeState for every node, then
    // setNewLinkState's node-depth raises for every conduit.
    finishRouting(ctx, structures, order, storage_updated_, y1_, y2_, dt);

    return (n_solved > 0) ? total_iters / n_solved : 1;
}

// ============================================================================
// finishRouting — legacy flowrout_execute post-passes (KW/steady)
// ============================================================================

void finishRouting(SimulationContext& ctx,
                   hydstruct::StructureSolver* structures,
                   const std::vector<int>& order,
                   const std::vector<char>& storage_updated,
                   const std::vector<double>& link_y1,
                   const std::vector<double>& link_y2,
                   double dt) {
    auto& nodes = ctx.nodes;
    auto& links = ctx.links;
    const int n_nodes = ctx.n_nodes();
    const int n_links = ctx.n_links();
    const int us = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));

    // Legacy Node.fullVolume for the non-storage overflow threshold:
    // identically 0 (node_getVolume's default branch is 0 at init) EXCEPT a
    // Type-1 pump wet well, whose fullVolume is the pump curve's largest
    // volume (legacy pump_validate). The engine-internal full_volume
    // convention (MIN_SURFAREA·fullDepth, for the DW surcharge test) must
    // not leak in here — it would let a junction store water legacy sheds.
    std::vector<double> legacy_fv(static_cast<std::size_t>(n_nodes), 0.0);
    {
        const double ucf_vol = ucf::Ucf[ucf::VOLUME][us];
        for (int j = 0; j < n_links; ++j) {
            auto uj = static_cast<std::size_t>(j);
            if (links.type[uj] != LinkType::PUMP) continue;
            const int pr = ctx.link_subtypes.pump_row(j);
            const int ci = (pr >= 0)
                ? ctx.link_subtypes.pumps.curve[static_cast<std::size_t>(pr)] : -1;
            if (ci < 0 || ci >= static_cast<int>(ctx.tables.tables.size())) continue;
            const auto& tbl = ctx.tables.tables[static_cast<std::size_t>(ci)];
            if (tbl.type != TableType::CURVE_PUMP1) continue;
            const int n1 = links.node1[uj];
            if (n1 < 0 || nodes.type[static_cast<std::size_t>(n1)] == NodeType::STORAGE)
                continue;
            double xmax = tbl.x_max;
            if (xmax <= 0.0 && !tbl.x.empty())
                xmax = *std::max_element(tbl.x.begin(), tbl.x.end());
            auto un1 = static_cast<std::size_t>(n1);
            legacy_fv[un1] = std::max(legacy_fv[un1], xmax / ucf_vol);
        }
    }

    // Outflow-link count per node (legacy toposort.c:75-91 — a link whose
    // upstream node is an outfall counts toward its DOWNSTREAM node).
    std::vector<int> degree(static_cast<std::size_t>(n_nodes), 0);
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        int n = (links.direction[uj] < 0) ? links.node2[uj] : links.node1[uj];
        if (n < 0 || n >= n_nodes) continue;
        if (nodes.type[static_cast<std::size_t>(n)] == NodeType::OUTFALL) {
            n = (links.direction[uj] < 0) ? links.node1[uj] : links.node2[uj];
            if (n < 0 || n >= n_nodes) continue;
        }
        degree[static_cast<std::size_t>(n)]++;
    }

    // --- legacy setNewNodeState for every node (flowrout.c:558-599) ---
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (nodes.type[ui] == NodeType::STORAGE) {
            // Terminal storage (no outlet link) was never visited by the
            // sorted-link loop; update it here (legacy flowrout.c:88-91).
            if (ui >= storage_updated.size() || !storage_updated[ui])
                updateStorageState(ctx, structures, order,
                                   static_cast<int>(order.size()), i, dt);
            continue;
        }

        double net = nodes.inflow[ui] - nodes.outflow[ui] - nodes.losses[ui];
        double v = nodes.old_volume[ui] + net * dt;
        if (v < constants::FUDGE) v = 0.0;

        nodes.overflow[ui] = 0.0;
        const bool can_pond =
            ctx.options.allow_ponding && nodes.ponded_area[ui] > 0.0;
        if (v > legacy_fv[ui]) {
            nodes.overflow[ui] =
                (v - std::max(nodes.old_volume[ui], legacy_fv[ui])) / dt;
            if (nodes.overflow[ui] < constants::FUDGE) nodes.overflow[ui] = 0.0;
            if (!can_pond) v = legacy_fv[ui];
        }
        nodes.volume[ui] = v;

        // legacy node_getDepth: 0 for anything but storage — the depth comes
        // from the conduit-end raises below.
        nodes.depth[ui] = 0.0;
        nodes.head[ui]  = nodes.invert_elev[ui];
    }

    // --- legacy setNewLinkState → updateNodeDepth raises (flowrout.c:632) ---
    auto raise = [&](int ni, double y) {
        if (ni < 0 || ni >= n_nodes) return;
        auto uni = static_cast<std::size_t>(ni);
        NodeType nt = nodes.type[uni];
        if (nt == NodeType::STORAGE) return;
        // A flooded non-outfall node WITH an outlet link reads full depth.
        if (nt != NodeType::OUTFALL && degree[uni] > 0 &&
            nodes.overflow[uni] > 0.0)
            y = nodes.full_depth[uni];
        if (nodes.depth[uni] < y) {
            nodes.depth[uni] = (nodes.full_depth[uni] > 0.0 &&
                                y > nodes.full_depth[uni])
                                   ? nodes.full_depth[uni] : y;
            nodes.head[uni] = nodes.invert_elev[uni] + nodes.depth[uni];
        }
    };
    for (int j = 0; j < n_links; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::CONDUIT) {
            // setNewLinkState starts every link at newDepth = newVolume = 0
            // and fills them for conduits only: a non-conduit's depth that
            // its head-discharge routine wrote during the step is not what
            // the step reports (control-rules-test's orifice 3 under
            // STEADY reported a growing depth where legacy reports 0).
            links.depth[uj]  = 0.0;
            links.volume[uj] = 0.0;
            continue;
        }
        const double y1 = (uj < link_y1.size()) ? link_y1[uj] : 0.0;
        const double y2 = (uj < link_y2.size()) ? link_y2[uj] : 0.0;
        raise(links.node1[uj], y1 + links.offset1[uj]);
        raise(links.node2[uj], y2 + links.offset2[uj]);
    }
}

double capConduitLoss(SimulationContext& ctx, std::size_t ucr, double q) {
    auto& CD = ctx.link_subtypes.conduits;
    double evap  = CD.evap_loss_rate[ucr];
    double seep  = CD.seep_loss_rate[ucr];
    double total = evap + seep;
    if (total > q) {
        evap  = evap * q / total;
        seep  = seep * q / total;
        total = q;
        CD.evap_loss_rate[ucr] = evap;
        CD.seep_loss_rate[ucr] = seep;
    }
    return total;
}

} // namespace kinwave
} // namespace openswmm
