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
 * @license  MIT License
 */

#include "KinematicWave.hpp"
#include "XSectBatch.hpp"
#include "../core/SimulationContext.hpp"
#include "Node.hpp"

#include <cmath>
#include <algorithm>

namespace openswmm {
namespace kinwave {

static constexpr int    MAX_ITERS = 40;
static constexpr double TINY      = 1.0e-6;

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
        double s_needed = q_in_norm / beta1;
        a_in_norm = xsect::getAofS(xs, s_needed * a_full) / a_full;
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
static XSectParams buildXSP_KW(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    auto ls = links.xsect_shape[uk];
    xs.type = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
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
    return xs;
}

int KWSolver::execute(SimulationContext& ctx, double dt) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    int total_iters = 0;
    int n_solved = 0;

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

        // Skip non-conduit links — pass through flow unchanged
        if (links.type[uj] != LinkType::CONDUIT) {
            // Non-conduit: outflow = inflow from upstream node
            int n1 = links.node1[uj];
            if (n1 >= 0) {
                double q = nodes.inflow[static_cast<std::size_t>(n1)];
                links.flow[uj] = q;
                int n2 = links.node2[uj];
                if (n2 >= 0) nodes.inflow[static_cast<std::size_t>(n2)] += q;
            }
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

        // Gather inflow from upstream node
        // (matching legacy getLinkInflow: use node inflow, limited by max outflow)
        int n1 = links.node1[uj];
        double qin = 0.0;
        if (n1 >= 0) {
            auto un1 = static_cast<std::size_t>(n1);
            qin = nodes.inflow[un1];
            // Limit by available volume at node (prevent negative depth)
            double q_max = node::getMaxOutflow(nodes, n1, qin, dt);
            qin = std::min(qin, q_max);
        }

        // Divide by barrels (KW solves per barrel)
        double barrels = static_cast<double>(std::max(links.barrels[uj], 1));
        double qin_per_barrel = qin / barrels;

        // Build XSectParams for this conduit
        XSectParams xs = buildXSP_KW(links, uj);

        double q_full = links.q_full[uj];
        double a_full = links.xsect_a_full[uj];
        double s_full = links.xsect_s_full[uj];
        double beta   = links.beta[uj];
        double length = links.mod_length[uj];
        if (length <= 0.0) length = links.length[uj];

        // Compute evaporation + seepage loss rate
        double loss_rate = links.evap_loss_rate[uj] + links.seep_loss_rate[uj];

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
            links.full_state[uj] = fs;
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
