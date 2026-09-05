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
 * @file DynamicWave.cpp
 * @brief Dynamic wave routing -- batch-oriented St. Venant equations.
 *
 * @details The solver follows this pattern per Picard iteration:
 *
 *   1. computeLinkGeometry() -- batch call to XSectGroups for ALL links:
 *      - depth1/depth2 from node heads and link offsets
 *      - Preissmann slot geometry applied when depth > y_full (closed conduits)
 *      - XSectGroups::computeAreas/HydRad/Widths for free-surface depths
 *      - Slot area/width/hrad overrides for surcharged depths
 *
 *   2. solveMomentumBatch() -- pure array arithmetic over ALL links:
 *      - velocity = old_flow / conveyArea(area_mid) -- the Preissmann slot is
 *        storage, not conveyance, so it is stripped from every momentum term
 *        (velocity, aWtd, dq3/dq4, local losses) while node continuity and
 *        link volume keep the slot-inclusive area. Issue #144.
 *      - Froude from velocity and hydraulic depth (vectorisable)
 *      - sigma (inertial damping) from Froude (vectorisable)
 *      - full inertial damping when closed conduit is surcharged
 *      - friction slope dq1 = dt * roughFactor / rMid^(4/3) * |v| (vectorisable)
 *      - head gradient dq2 = dt * g * aMid * (h2-h1)/L (vectorisable)
 *      - momentum update: q = (qOld - dq2 + dq3 + dq4) / (1 + dq1)
 *      - under-relaxation (vectorisable)
 *
 *   3. updateNodeFlows() -- scatter link flows to node inflow/outflow
 *
 *   4. updateNodeDepths() -- per-node Picard convergence check
 *      - EXTRAN surcharge: dQ/dH with smooth transition near crown
 *      - Separate convergence tracking for surcharged vs free-surface nodes
 *
 * @note Legacy reference: src/legacy/engine/dwflow.c, dynwave.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "DynamicWave.hpp"
#include "Node.hpp"
#include "Outfall.hpp"
#include "XSectBatch.hpp"
#include "ForceMain.hpp"
#include "Culvert.hpp"
#include "../core/Constants.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/ThreadInfo.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <numeric>

// OpenMP support — graceful degradation when not available
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
static inline int omp_get_num_threads() { return 1; }
static inline int omp_get_thread_num()  { return 0; }
#endif

// Darwin/Apple-Silicon scheduling helper (see execute): pthread QoS to keep
// team threads off the efficiency cores. The P-core count query lives in
// core/ThreadInfo.
#if defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#endif

namespace openswmm {

// A3 parity tracing: routing-step serial defined in SWMMEngine.cpp, used to
// step-gate the per-link term trace (SWMM_TRACE_LSTEP) below.
extern long g_trace_rstep_sn;

namespace dynwave {

using constants::GRAVITY;
using constants::SQRT_GRAVITY;
using constants::INV_SQRT_GRAVITY;
using constants::FUDGE;

// ============================================================================
// Preissmann slot helpers (matching legacy dwflow.c)
// ============================================================================

double DWSolver::getCrownCutoff() const {
    if (surcharge_method == SurchargeMethod::SLOT ||
        surcharge_method == SurchargeMethod::DYNAMIC_SLOT ||
        surcharge_method == SurchargeMethod::TPA)
        return SLOT_CROWN_CUTOFF;
    return EXTRAN_CROWN_CUTOFF;
}

/**
 * @brief Compute Preissmann slot width at depth y.
 *
 * @details Matches legacy dwflow.c::getSlotWidth():
 *   - Returns 0 if SLOT method not used, shape is open, or y/yFull < cutoff
 *   - For y/yFull > 1.78: slot width = 1% of max width
 *   - Otherwise: Sjoberg formula: wMax * 0.5423 * exp(-yNorm^2.4)
 *
 * When EXTRAN method is used: slot width = y_full / 1000 (constant)
 */
double DWSolver::getSlotWidth(double y, double y_full, double w_max,
                              XsectShape shape) const {
    if (y_full <= 0.0) return 0.0;

    // Open shapes (trapezoidal, triangular, rectangular open, parabolic)
    // never use a slot -- they have no crown
    bool is_open = (shape == XsectShape::RECT_OPEN ||
                    shape == XsectShape::TRAPEZOIDAL ||
                    shape == XsectShape::TRIANGULAR ||
                    shape == XsectShape::PARABOLIC);
    if (is_open) return 0.0;

    double yNorm = y / y_full;

    if (surcharge_method == SurchargeMethod::SLOT) {
        if (yNorm < SLOT_CROWN_CUTOFF) return 0.0;
        // For depth > 1.78 * pipe depth, slot width = 1% of max width
        if (yNorm > 1.78) return 0.01 * w_max;
        // Sjoberg formula: pow(yNorm, 2.4) = yNorm^2 * yNorm^0.4
        // Use cbrt(yNorm^2) = yNorm^(2/3), then yNorm^0.4 = (yNorm^2)^0.2
        // Faster: yNorm^2.4 = yNorm^2 * yNorm^(2/5) = yNorm^2 * sqrt(cbrt(yNorm^2))
        double y2 = yNorm * yNorm;
        return w_max * 0.5423 * std::exp(-y2 * std::sqrt(std::cbrt(y2)));
    }

    // EXTRAN method: no slot — surcharge uses dQ/dH in node depth solver.
    // Legacy getSlotWidth() returns 0.0 when SurchargeMethod != SLOT.
    return 0.0;
}

/**
 * @brief Strip the Preissmann slot from a flow area, leaving conveyance area.
 *
 * @details The slot is a fictitious gap above the crown that supplies a finite
 * free-surface top width so the Saint-Venant system stays hyperbolic once a
 * conduit pressurizes. It stores water but conveys none, so the momentum
 * equation — velocity, friction, inertial terms, local losses — must use the
 * closed-section area. This mirrors the hydraulic-radius clamp to `r_full`
 * applied in STEP E and in `applyDPSGeometry()`, which excludes the slot from
 * the wetted perimeter on exactly the same reasoning.
 *
 * Node continuity (`surf_area1/2_`, `width_mid_`) and link storage
 * (`links.volume`) deliberately keep the slot-inclusive area — that is where
 * the slot does its work, and where the water it holds is booked. Clamping
 * the volume too would delete water the node-depth solve already absorbed.
 * See issue #144 and plans/PREISSMANN_SLOT_VELOCITY_ISSUE_2026-08-22.md.
 *
 * Covers both surcharge methods: the static Sjoberg slot (STEP E) and DPS,
 * whose `applyDPSGeometry()` writes `area_mid_ = a_full + As`.
 *
 * Off the slot path this is the identity — an open shape or EXTRAN never
 * produces an area above `a_full`.
 */
static inline double conveyArea(double a, double a_full) {
    return (a > a_full) ? a_full : a;
}

// ============================================================================
// Dynamic Preissmann Slot (DPS) — geometry override (head-first formulation)
// Sharior et al. (2023) Eqs. 14, 15, 19 — applied in link-node form.
//
// SWMM's DW solver evolves node depth as the prognostic variable, so the
// surcharge head `hs = max(depth_mid − y_full, 0)` is read off the node-head
// solution rather than integrated from area continuity.  The DPS relation is
// then used in its forward form `dAs = T_s · dhs` where
// `T_s = T_s_target · P² = (g·A_C/c_pT²)·P²` is the dynamic slot top width.
// Accumulating `As` from dhs increments — not from `(area_mid − A_C)` —
// keeps previously stored slot volume invariant under pure P decay, which is
// the conservation property the original DPS formulation was designed for.
// ============================================================================

void DWSolver::applyDPSGeometry(SimulationContext& ctx) {
    auto& links = ctx.links;

    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        auto uci = static_cast<std::size_t>(ci);

        if (is_open_[uj]) continue;

        const double yf = links.xsect_y_full[uj];
        const double af = links.xsect_a_full[uj];
        const double rf = links.xsect_r_full[uj];
        if (yf <= 0.0 || af <= 0.0) continue;

        const double y1   = depth1_[uj];
        const double y2   = depth2_[uj];
        const double yMid = depth_mid_[uj];

        // hs_iter is purely geometric — it is what the latest node-depth
        // solution implies for the surcharge head.  Clamped to ≥ 0 so that a
        // pipe that has dropped back below crown delivers a non-negative hs
        // for the increment computation; the As decrement comes from a
        // negative dhs in that case, which the clamp below handles.
        const double hs_iter = (yMid > yf) ? (yMid - yf) : 0.0;
        const double hs_prev = dps_.hs_prev_iter[uci];
        const double dhs = hs_iter - hs_prev;

        // Dynamic slot top width — Eq. 19 inverted: T_s = g·A_C·P²/c_pT².
        // T_s_target = g·A_C/c_pT² is cached at init so this is a single mul.
        const double P  = dps_.P[uci];
        const double Ts = dps_.T_s_target[uci] * P * P;

        // Path-dependent accumulation: dAs uses the *current* T_s but past
        // contributions to As are not rewritten.  Eq. 14 is satisfied
        // incrementally, not by re-deriving As from current area_mid.
        double As_new = dps_.As[uci] + Ts * dhs;
        if (As_new < 0.0) As_new = 0.0;

        const bool slot_active = (hs_iter > 0.0) || (As_new > 0.0);

        // Update persistent state.  hs is now the geometric surcharge head
        // (consistent with depth_mid), not a derived diagnostic; hs_prev_iter
        // tracks within-Picard iterates so dhs reflects each sub-step.
        dps_.As[uci] = As_new;
        dps_.hs[uci] = hs_iter;
        dps_.hs_prev_iter[uci] = hs_iter;

        if (!slot_active) {
            // Below-crown — no slot override.  area_mid/width_mid from the
            // batch XSect call apply unchanged.
            continue;
        }

        // ---- Effective geometry overrides for surcharged closed conduit ----
        // Total midpoint area = closed-section + accumulated slot volume per
        // unit length.  Used by the momentum solve and routing-step CFL.
        const double A_total = af + As_new;
        area_mid_[uj] = A_total;
        if (y1 > yf) area1_[uj] = A_total;
        if (y2 > yf) area2_[uj] = A_total;

        // Slot width drives node-continuity surface area.  The batch width
        // kernel clamps depth to y_full and returns ~0 at the crown for
        // closed shapes, so for surcharged links we must overwrite both
        // width_mid AND the surf_area1/2 contributions that STEP C computed
        // from that ~0 width.  Without this the slot is decoupled from node
        // depth evolution and the reviewer's invariance argument is vacuous.
        width_mid_[uj] = Ts;

        const double L = cached_length_[uj];
        const double slot_surf_per_end = 0.25 * Ts * L;  // matches SUBCRITICAL: (w+w)·L/4
        if (y1 > yf) surf_area1_[uj] = slot_surf_per_end;
        if (y2 > yf) surf_area2_[uj] = slot_surf_per_end;

        // Friction excludes slot — hydraulic radius stays at full-pipe value.
        hrad_mid_[uj] = rf;
    }
}

// ============================================================================
// TPA (issue #156 Phase 5) — latch update + geometry override
// ============================================================================

// Once per routing step, BEFORE the Picard loop, from last-committed state:
// the operator stays fixed within the iteration (mirrors how DPS advances its
// state outside the iteration). Vent rule carries P4's SUBMERGENCE lesson:
// air enters only where a VENTED node's water level sits below the pipe crown
// at that end — a submerged connection holds the column. Sealed = virtual
// junction or SUR_DEPTH > 0. Column separation at yMid < y_full − 30 ft
// clears unconditionally (documented two-phase limitation).
void DWSolver::updateTpaLatch(SimulationContext& ctx) {
    const auto& links = ctx.links;
    const auto& nodes = ctx.nodes;
    constexpr double kCavHead = 30.0;   // ft, mirrors the FV closure
    for (int ci = 0; ci < n_conduits_; ++ci) {
        const auto uci = static_cast<std::size_t>(ci);
        const int j = conduit_idx_[uci];
        const auto uj = static_cast<std::size_t>(j);
        tpa_latch_changed_[uci] = 0;
        if (is_open_[uj]) { tpa_latch_[uci] = 0; continue; }
        const double yf = links.xsect_y_full[uj];
        if (yf <= 0.0) { tpa_latch_[uci] = 0; continue; }
        const double y1 = depth1_[uj];
        const double y2 = depth2_[uj];
        const uint8_t old = tpa_latch_[uci];
        uint8_t latched = old;
        if (!old) {
            // Set when the conduit reaches full — the same condition that
            // engages the slot branch today (TPA plan §B2).
            latched = (y1 >= yf && y2 >= yf) ? uint8_t{1} : uint8_t{0};
        } else {
            const double yMid = depth_mid_[uj];
            if (yMid < yf - kCavHead) {
                latched = 0;   // column separation
            } else {
                // Clear on atmosphere contact: a vented end whose level is
                // below the crown AT THAT END.
                auto vented_end = [&](int n, double y_end) -> bool {
                    if (n < 0) return false;
                    const auto un2 = static_cast<std::size_t>(n);
                    const bool sealed =
                        (un2 < nodes.is_virtual.size() &&
                         nodes.is_virtual[un2] != 0) ||
                        (un2 < nodes.sur_depth.size() &&
                         nodes.sur_depth[un2] > 0.0);
                    return !sealed && y_end < yf;
                };
                if (vented_end(links.node1[uj], y1) ||
                    vented_end(links.node2[uj], y2))
                    latched = 0;
            }
        }
        tpa_latch_[uci] = latched;
        tpa_latch_changed_[uci] = (latched != old) ? uint8_t{1} : uint8_t{0};
    }
}

// DPS-style geometry override (called where applyDPSGeometry is): engaged
// when latched OR above the crown cutoff (memoryless above the crown — the
// latch only matters below it). Area rides the SIGNED slot line
// A = A_full + w_tpa·(y − y_full) — "shrinkage" for sub-atmospheric heads —
// width is the constant w_tpa, the hydraulic radius stays r_full (the slot
// carries no wetted perimeter), and both ends contribute slot surface area
// to node continuity (the latch keeps sealed heads meaningful).
void DWSolver::applyTpaGeometry(SimulationContext& ctx) {
    auto& links = ctx.links;
    for (int ci = 0; ci < n_conduits_; ++ci) {
        const auto uci = static_cast<std::size_t>(ci);
        const int j = conduit_idx_[uci];
        const auto uj = static_cast<std::size_t>(j);
        if (is_open_[uj]) continue;
        const double yf = links.xsect_y_full[uj];
        const double af = links.xsect_a_full[uj];
        const double rf = links.xsect_r_full[uj];
        if (yf <= 0.0 || af <= 0.0) continue;
        const double w = tpa_w_[uci];
        if (w <= 0.0) continue;
        const double yMid = depth_mid_[uj];
        const bool engaged = (tpa_latch_[uci] != 0) ||
                             (yMid >= SLOT_CROWN_CUTOFF * yf);
        if (!engaged) continue;

        auto line_area = [&](double y) {
            return std::max(af + w * (y - yf), 0.05 * af);
        };
        area_mid_[uj] = line_area(yMid);
        area1_[uj]    = line_area(depth1_[uj]);
        area2_[uj]    = line_area(depth2_[uj]);
        width_mid_[uj] = w;
        const double L = cached_length_[uj];
        const double slot_surf_per_end = 0.25 * w * L;  // (w+w)·L/4 convention
        surf_area1_[uj] = slot_surf_per_end;
        surf_area2_[uj] = slot_surf_per_end;
        hrad_mid_[uj] = rf;   // slot/elastic area carries no wetted perimeter
    }
}

// ============================================================================
// DPS post-Picard state update — P decay, surcharge tracking
// Sharior et al. (2023) Eqs. 22, 23
// ============================================================================

void DWSolver::updateDPSState(SimulationContext& ctx, double dt) {
    auto& links = ctx.links;
    sim_time_ += dt;

    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        auto uci = static_cast<std::size_t>(ci);

        if (is_open_[uj]) continue;

        double yf = links.xsect_y_full[uj];
        bool was_surcharged = dps_.surcharged[uci] != 0;
        bool now_surcharged = (depth_mid_[uj] > yf && dps_.As[uci] > 0.0);

        // Track surcharge onset time
        if (now_surcharged && !was_surcharged) {
            dps_.t_s[uci] = sim_time_;
        }

        // Eq. 22: P_hat(t - t_s) = (P_hat_0 - 1) * exp(-10*(t - t_s)/r) + 1
        if (now_surcharged) {
            double dt_surcharge = sim_time_ - dps_.t_s[uci];
            dps_.P_hat[uci] = (dps_.P_hat_0[uci] - 1.0)
                             * std::exp(-10.0 * dt_surcharge / dps_config_.r) + 1.0;
        } else {
            // Fully depressurized: reset to initial P (Eq. 23) and clear the
            // accumulated slot state so the next surcharge episode starts from
            // a clean baseline.  hs_prev_iter must be cleared too — otherwise
            // the first iter of the next surcharge would see a phantom dhs.
            dps_.P_hat[uci] = dps_.P_hat_0[uci];
            dps_.As[uci] = 0.0;
            dps_.hs[uci] = 0.0;
            dps_.hs_prev_iter[uci] = 0.0;
        }

        dps_.surcharged[uci] = now_surcharged ? 1 : 0;
    }

    // Spatial smoothing of P across node boundaries
    spatialSmoothP(ctx);
}

// ============================================================================
// DPS spatial smoothing — element → face → element interpolation
// Adapted from Sharior et al. (2023) for link-node topology
// ============================================================================

void DWSolver::spatialSmoothP(const SimulationContext& ctx) {
    auto& links = ctx.links;

    // Step 1: Accumulate P_hat at nodes from connecting conduits
    auto un = static_cast<std::size_t>(n_nodes_);
    thread_local std::vector<double> node_P_sum;
    thread_local std::vector<int>    node_P_count;
    node_P_sum.assign(un, 0.0);
    node_P_count.assign(un, 0);

    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        auto uci = static_cast<std::size_t>(ci);

        if (is_open_[uj]) continue;

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 >= 0 && n1 < n_nodes_) {
            node_P_sum[static_cast<std::size_t>(n1)] += dps_.P_hat[uci];
            node_P_count[static_cast<std::size_t>(n1)]++;
        }
        if (n2 >= 0 && n2 < n_nodes_) {
            node_P_sum[static_cast<std::size_t>(n2)] += dps_.P_hat[uci];
            node_P_count[static_cast<std::size_t>(n2)]++;
        }
    }

    // Step 2: Compute smoothed P for each conduit from face averages
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        auto uci = static_cast<std::size_t>(ci);

        if (is_open_[uj]) continue;

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];

        double P_face1 = dps_.P_hat[uci];
        double P_face2 = dps_.P_hat[uci];
        if (n1 >= 0 && n1 < n_nodes_ && node_P_count[static_cast<std::size_t>(n1)] > 0)
            P_face1 = node_P_sum[static_cast<std::size_t>(n1)]
                     / node_P_count[static_cast<std::size_t>(n1)];
        if (n2 >= 0 && n2 < n_nodes_ && node_P_count[static_cast<std::size_t>(n2)] > 0)
            P_face2 = node_P_sum[static_cast<std::size_t>(n2)]
                     / node_P_count[static_cast<std::size_t>(n2)];

        // Linear smoothing: average of upstream and downstream face values
        dps_.P[uci] = 0.5 * (P_face1 + P_face2);
        dps_.P[uci] = std::max(dps_.P[uci], 1.0);  // P >= 1 always
    }
}

// ============================================================================
// Init
// ============================================================================

// Defined below (with the per-element cross-section helpers); forward-declared
// so init() can seed each conduit's initial area from its initial flow depth.
static XSectParams buildXSP(const SimulationContext& ctx, std::size_t uk);

void DWSolver::init(int n_nodes, int n_links, const XSectGroups& groups,
                    const SimulationContext& ctx) {
    n_nodes_ = n_nodes;
    n_links_ = n_links;
    groups_  = &groups;

    // Resolve the effective MIN_SURFAREA floor: user override (from
    // INP [OPTIONS] MIN_SURFAREA) if > 0, else the compiled default.
    // Matches legacy dynwave.c:180-181:
    //   if (MinSurfArea == 0.0) MinSurfArea = DEFAULT_SURFAREA;
    //   else MinSurfArea /= UCF(LENGTH) * UCF(LENGTH);
    // The user value is entered in project area units (ft² for US, m² for SI),
    // so it must be divided by UCF(LENGTH)² to reach internal ft².  Without
    // this an SI model's 1.167 m² floor was used as 1.167 ft² (~10.8× too
    // small), driving large per-iteration depth swings and non-convergence.
    {
        const double ucf_len = ucf::Ucf[ucf::LENGTH][
            ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units))];
        min_surf_area_ = (ctx.options.min_surf_area > 0.0)
            ? ctx.options.min_surf_area / (ucf_len * ucf_len)
            : constants::MIN_SURFAREA;
    }

    auto un = static_cast<std::size_t>(n_nodes);
    auto ul = static_cast<std::size_t>(n_links);

    // Unsteady-friction cross-link stencil (issue #156): see the member
    // comment in DynamicWave.hpp. Topology is static, so build once.
    if (unsteady_friction != 0) {
        uf_nb_up_.assign(ul, -1);
        uf_nb_dn_.assign(ul, -1);
        uf_sg_up_.assign(ul, 0);
        uf_sg_dn_.assign(ul, 0);
        // Per-node incidence: count of incident CONDUITS (any other link
        // type attached disqualifies the node as a "simple" pass-through),
        // plus the first two conduit indices.
        std::vector<int> ccount(un, 0), other(un, 0);
        std::vector<std::array<int, 2>> inc(un, {-1, -1});
        for (int j = 0; j < n_links; ++j) {
            const auto ujj = static_cast<std::size_t>(j);
            const bool is_cond = (ctx.links.type[ujj] == LinkType::CONDUIT);
            for (const int nd : {ctx.links.node1[ujj], ctx.links.node2[ujj]}) {
                if (nd < 0 || nd >= n_nodes) continue;
                const auto und = static_cast<std::size_t>(nd);
                if (!is_cond) { ++other[und]; continue; }
                if (ccount[und] < 2) inc[und][ccount[und]] = j;
                ++ccount[und];
            }
        }
        for (int j = 0; j < n_links; ++j) {
            const auto ujj = static_cast<std::size_t>(j);
            if (ctx.links.type[ujj] != LinkType::CONDUIT) continue;
            const int n1 = ctx.links.node1[ujj];
            const int n2 = ctx.links.node2[ujj];
            if (n1 >= 0 && n1 < n_nodes) {
                const auto u1 = static_cast<std::size_t>(n1);
                if (ccount[u1] == 2 && other[u1] == 0) {
                    const int k = (inc[u1][0] == j) ? inc[u1][1] : inc[u1][0];
                    if (k >= 0) {
                        uf_nb_up_[ujj] = k;
                        uf_sg_up_[ujj] =
                            (ctx.links.node2[static_cast<std::size_t>(k)] == n1)
                                ? int8_t{1} : int8_t{-1};
                    }
                }
            }
            if (n2 >= 0 && n2 < n_nodes) {
                const auto u2 = static_cast<std::size_t>(n2);
                if (ccount[u2] == 2 && other[u2] == 0) {
                    const int k = (inc[u2][0] == j) ? inc[u2][1] : inc[u2][0];
                    if (k >= 0) {
                        uf_nb_dn_[ujj] = k;
                        uf_sg_dn_[ujj] =
                            (ctx.links.node1[static_cast<std::size_t>(k)] == n2)
                                ? int8_t{1} : int8_t{-1};
                    }
                }
            }
        }
    }

    // Seepage possibility is static input — scan once so the loss recompute
    // pass can be skipped bit-exactly on loss-free models (see
    // recomputeConduitLosses).
    any_conduit_seep_ = false;
    losses_all_zero_ = true;
    {
        const auto& CD = ctx.link_subtypes.conduits;
        for (int r = 0; r < CD.count(); ++r) {
            if (CD.seep_rate[static_cast<std::size_t>(r)] > 0.0) {
                any_conduit_seep_ = true;
                break;
            }
        }
    }

    xnode_.resize(un);  // SoA: allocates all per-node arrays

    area1_.resize(ul, 0.0);
    area2_.resize(ul, 0.0);
    area_mid_.resize(ul, 0.0);
    hrad_mid_.resize(ul, 0.0);
    width_mid_.resize(ul, 0.0);
    depth1_.resize(ul, 0.0);
    depth2_.resize(ul, 0.0);
    depth_mid_.resize(ul, 0.0);

    velocity_.resize(ul, 0.0);
    froude_.resize(ul, 0.0);
    sigma_.resize(ul, 0.0);
    dqdh_.resize(ul, 0.0);
    new_flow_.resize(ul, 0.0);
    area_old_.resize(ul, 0.0);
    bypassed_.assign(ul, 0);
    surf_area1_.resize(ul, 0.0);
    surf_area2_.resize(ul, 0.0);
    hrad1_.resize(ul, 0.0);
    width1_.resize(ul, 0.0);
    width2_.resize(ul, 0.0);
    h1_.resize(ul, 0.0);
    h2_.resize(ul, 0.0);
    fasnh_.resize(ul, 1.0);
    wcap_d1_.resize(ul, 0.0);
    wcap_d2_.resize(ul, 0.0);
    wcap_dm_.resize(ul, 0.0);

    // Build conduit index list. DUMMY cross-sections are excluded to match
    // legacy isTrueConduit (dynwave.c:411-414): a dummy conduit is never
    // momentum-solved — findNonConduitFlow passes the upstream node's
    // inflow + overflow straight through (conduit_getInflow). Solving it
    // here instead gives zero-area geometry, zero flow, and an upstream
    // node that fills without bound (221-h-h-si-units-elements node "1").
    conduit_idx_.clear();
    conduit_idx_.reserve(ul);
    for (int j = 0; j < n_links; ++j) {
        auto ujc = static_cast<std::size_t>(j);
        if (ctx.links.type[ujc] == LinkType::CONDUIT &&
            ctx.links.xsect_shape[ujc] != XsectShape::DUMMY)
            conduit_idx_.push_back(j);
    }
    n_conduits_ = static_cast<int>(conduit_idx_.size());

    // Pre-compute per-link invariants (shape flags, barrels, lengths)
    is_open_.resize(ul, 0);
    is_force_main_.resize(ul, 0);
    has_losses_.resize(ul, 0);
    barrels_d_.resize(ul, 1.0);
    cached_length_.resize(ul, 0.0);
    inv_length_.resize(ul, 0.0);

    const auto& links = ctx.links;
    const auto& CD = ctx.link_subtypes.conduits;
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));
        XsectShape shape = links.xsect_shape[uj];

        is_open_[uj] = (shape == XsectShape::RECT_OPEN ||
                        shape == XsectShape::TRAPEZOIDAL ||
                        shape == XsectShape::TRIANGULAR ||
                        shape == XsectShape::PARABOLIC);
        is_force_main_[uj] = (shape == XsectShape::FORCE_MAIN);
        has_losses_[uj] = (CD.loss_inlet[ucr] != 0.0 ||
                           CD.loss_outlet[ucr] != 0.0 ||
                           CD.loss_avg[ucr] != 0.0);
        barrels_d_[uj] = static_cast<double>(std::max(CD.barrels[ucr], 1));
        double len = CD.mod_length[ucr];
        if (len <= 0.0) len = CD.length[ucr];
        cached_length_[uj] = len;
        inv_length_[uj] = (len > 0.0) ? 1.0 / len : 0.0;

        // Seed the conduit's initial mid-area from its initial flow depth,
        // matching legacy initLinks (flowrout.c:502): Conduit.a1 = a2 =
        // xsect_getAofY(xsect, newDepth). area_mid_ is copied into area_old_
        // at the top of the first execute() (the legacy "a2 = a1" of
        // initRoutingStep), so this value becomes the aOld used in step 0's
        // unsteady momentum term dq3 = 2·v·(aMid − aOld)·σ. Without it a
        // conduit that starts with flow (q0 ≠ 0, e.g. extran8a's IRREGULAR
        // 10081 with q0 = 20) began step 0 with aOld = 0 instead of its
        // normal-depth area (~25.9 ft²), biasing the first-step flow by
        // ~0.7 cfs and seeding a slowly-decaying startup transient.
        XSectParams xs0 = buildXSP(ctx, uj);
        area_mid_[uj] = xsect::getAofY(xs0, links.depth[uj]);
    }

    // Momentum category arrays
    category_.resize(ul, MomentumCategory::SKIP_DRY);

    // Phase A: build conduit-dense hot tile of timestep-invariant data.
    // Sized n_conduits_ so the inner loops index by ci with sequential
    // access — better L1 hit rate than sparse links.X[uj] reads.
    refreshConduitTile(ctx);

    // B2 threading: node→incident-conduit CSR for the parallel flow gather.
    buildConduitNodeCSR(ctx);

    // Virtual-junction pair table (empty for models without [VIRTUAL_JUNCTIONS]).
    buildVirtualJunctionPairs(ctx);

    // Anderson acceleration state arrays (allocated regardless; only used when enabled)
    aa_y_prev_.resize(un, 0.0);
    aa_g_prev_.resize(un, 0.0);
    aa_r_prev_.resize(un, 0.0);
    aa_skip_.resize(un, 0);

    // TPA initialization (issue #156 Phase 5): per-conduit constant slot
    // width w = g·A_full/a², a in PROJECT units converted like the other
    // length-per-time options.
    if (surcharge_method == SurchargeMethod::TPA) {
        const double ucf_len2 = ucf::Ucf[ucf::LENGTH][
            ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units))];
        const double a_fts = std::max(tpa_celerity / ucf_len2, 1.0);
        const auto ucn = static_cast<std::size_t>(n_conduits_);
        tpa_w_.assign(ucn, 0.0);
        tpa_latch_.assign(ucn, 0);
        tpa_latch_changed_.assign(ucn, 0);
        for (int ci = 0; ci < n_conduits_; ++ci) {
            const auto uj = static_cast<std::size_t>(
                conduit_idx_[static_cast<std::size_t>(ci)]);
            const double af = ctx.links.xsect_a_full[uj];
            tpa_w_[static_cast<std::size_t>(ci)] =
                (af > 0.0) ? GRAVITY * af / (a_fts * a_fts) : 0.0;
        }
    }

    // Dynamic Preissmann Slot (DPS) initialization
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        // Convert c_pT from m/s to ft/s (internal units)
        dps_config_.c_pT   = ctx.options.dps_target_celerity * 3.28084;
        dps_config_.alpha   = std::max(ctx.options.dps_alpha, 2.0);
        dps_config_.r       = std::max(ctx.options.dps_decay_time, 0.001);
        dps_config_.c_pT_sq = dps_config_.c_pT * dps_config_.c_pT;

        dps_.resize(static_cast<std::size_t>(n_conduits_));
        sim_time_ = 0.0;

        for (int ci = 0; ci < n_conduits_; ++ci) {
            int j = conduit_idx_[static_cast<std::size_t>(ci)];
            auto uj = static_cast<std::size_t>(j);
            auto uci = static_cast<std::size_t>(ci);

            // Skip open shapes — no slot needed
            if (is_open_[uj]) {
                dps_.P_hat_0[uci] = 1.0;
                dps_.P[uci] = dps_.P_hat[uci] = 1.0;
                continue;
            }

            // Compute gravity-wave celerity at full depth:
            // c_g = sqrt(g * hydraulic_depth) where hydraulic_depth = A_full / T_w
            double af = links.xsect_a_full[uj];
            double tw = links.xsect_w_max[uj];
            double l_D = (tw > 0.0) ? af / tw : 0.0;
            double c_g = (l_D > 0.0) ? std::sqrt(GRAVITY * l_D) : 1.0;

            // Initial Preissmann Number: P_hat_0 = c_pT / (alpha * c_g) (Eq. 23)
            dps_.P_hat_0[uci] = dps_config_.c_pT / (dps_config_.alpha * c_g);
            dps_.P_hat_0[uci] = std::max(dps_.P_hat_0[uci], 1.0);

            dps_.P[uci] = dps_.P_hat[uci] = dps_.P_hat_0[uci];
            dps_.As[uci] = 0.0;
            dps_.hs[uci] = 0.0;
            dps_.hs_prev_iter[uci] = 0.0;

            // T_s_target = g · A_C / c_pT².  This is the slot top width at
            // steady-state (P = 1) and follows directly from inverting Eq. 19
            // for the increment: dAs/dhs = g · A_C · P² / c_pT².  The per-iter
            // dynamic width is then T_s = T_s_target · P².
            dps_.T_s_target[uci] = (dps_config_.c_pT_sq > 0.0)
                ? (GRAVITY * af) / dps_config_.c_pT_sq
                : 0.0;

            dps_.surcharged[uci] = 0;
            dps_.t_s[uci] = 0.0;
        }
    }
}

// ============================================================================
// refreshConduitTile — Phase A: gather timestep-invariant SoA fields into a
//                       conduit-dense tile for fast inner-loop access.
//
// Each ci-indexed read in the Picard hot path now hits a contiguous block
// of memory holding 8 doubles per cache line. Compared to sparse
// links.X[conduit_idx_[ci]] / nodes.X[links.node1[uj]] indirection, this
// removes ~3–4 wasted L1 fills per conduit per Picard iteration.
//
// Fields gathered are constants throughout the simulation (network topology,
// cross-section dimensions, conveyance parameters) so the gather runs ONCE
// at init, not per timestep or per Picard iter.
// ============================================================================

void DWSolver::refreshConduitTile(const SimulationContext& ctx) {
    auto nc = static_cast<std::size_t>(n_conduits_);
    tile_uj_.resize(nc);
    tile_n1_.resize(nc);
    tile_n2_.resize(nc);
    tile_inv1_elev_.resize(nc);
    tile_inv2_elev_.resize(nc);
    tile_z1_off_.resize(nc);
    tile_z2_off_.resize(nc);
    tile_y_full_.resize(nc);
    tile_a_full_.resize(nc);
    tile_r_full_.resize(nc);
    tile_w_max_.resize(nc);
    tile_length_.resize(nc);
    tile_inv_length_.resize(nc);
    tile_links_length_.resize(nc);
    tile_beta_.resize(nc);
    tile_q_max_.resize(nc);
    tile_rough_factor_.resize(nc);
    tile_barrels_d_.resize(nc);
    tile_is_open_.resize(nc);
    tile_is_force_main_.resize(nc);
    tile_is_closed_.resize(nc);
    tile_has_losses_.resize(nc);
    tile_xsect_batch_shape_.resize(nc);
    tile_shape_.resize(nc);
    tile_has_offset_.resize(nc);
    tile_culvert_code_.resize(nc);
    tile_slope_.resize(nc);
    tile_q_limit_.resize(nc);
    tile_loss_inlet_.resize(nc);
    tile_loss_outlet_.resize(nc);
    tile_loss_avg_.resize(nc);
    tile_roughness_.resize(nc);
    tile_fm_sbot_.resize(nc);
    tile_fm_rbot_.resize(nc);
    tile_has_flap_gate_.resize(nc);
    tile_direction_.resize(nc);
    // Reverse map sized n_links_, -1 for non-conduits
    tile_uj_to_ci_.assign(static_cast<std::size_t>(n_links_), -1);

    const auto& links = ctx.links;
    const auto& nodes = ctx.nodes;
    const auto& CD = ctx.link_subtypes.conduits;
    // Phase 6 Stage D: the side-tables are populated by parse/resolve/Router::init
    // (which all run before this, during router_.init) so the tile gathers its
    // conduit invariants directly from ConduitData.
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        int j = conduit_idx_[uci];
        auto uj = static_cast<std::size_t>(j);
        const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(j));
        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);

        tile_uj_[uci]            = j;
        tile_n1_[uci]            = n1;
        tile_n2_[uci]            = n2;
        tile_inv1_elev_[uci]     = nodes.invert_elev[un1];
        tile_inv2_elev_[uci]     = nodes.invert_elev[un2];
        tile_z1_off_[uci]        = links.offset1[uj];
        tile_z2_off_[uci]        = links.offset2[uj];
        tile_y_full_[uci]        = links.xsect_y_full[uj];
        tile_a_full_[uci]        = links.xsect_a_full[uj];
        tile_r_full_[uci]        = links.xsect_r_full[uj];
        tile_w_max_[uci]         = links.xsect_w_max[uj];
        tile_length_[uci]        = cached_length_[uj];
        tile_inv_length_[uci]    = inv_length_[uj];
        tile_links_length_[uci]  = CD.length[ucr];
        tile_beta_[uci]          = CD.beta[ucr];
        tile_q_max_[uci]         = CD.q_max[ucr];
        tile_rough_factor_[uci]  = CD.rough_factor[ucr];
        tile_barrels_d_[uci]     = barrels_d_[uj];
        tile_is_open_[uci]       = is_open_[uj];
        tile_is_force_main_[uci] = is_force_main_[uj];
        tile_is_closed_[uci]     = links.is_closed[uj] ? 1 : 0;
        tile_has_losses_[uci]    = has_losses_[uj];
        tile_xsect_batch_shape_[uci] = links.xsect_batch_shape[uj];
        tile_shape_[uci]         = links.xsect_shape[uj];
        tile_has_offset_[uci]    = (links.offset1[uj] > 0.0 ||
                                    links.offset2[uj] > 0.0) ? 1 : 0;
        tile_culvert_code_[uci]  = CD.culvert_code[ucr];
        tile_slope_[uci]         = CD.slope[ucr];
        tile_q_limit_[uci]       = links.q_limit[uj];
        tile_loss_inlet_[uci]    = CD.loss_inlet[ucr];
        tile_loss_outlet_[uci]   = CD.loss_outlet[ucr];
        tile_loss_avg_[uci]      = CD.loss_avg[ucr];
        tile_roughness_[uci]     = CD.roughness[ucr];
        tile_fm_sbot_[uci]       = links.xsect_s_bot[uj];
        tile_fm_rbot_[uci]       = links.xsect_r_bot[uj];
        tile_has_flap_gate_[uci] = links.has_flap_gate[uj] ? 1 : 0;
        tile_direction_[uci]     = static_cast<int8_t>(links.direction[uj]);
        tile_uj_to_ci_[uj]       = ci;
    }
}

// ============================================================================
// buildConduitNodeCSR — node→incident-conduit adjacency for the parallel
//                       node-centric flow gather (built once at init).
//
// Entries per node are stored in ascending conduit LINK index, with the
// node1-end entry preceding the node2-end entry of the same link — exactly
// the per-node projection of the serial per-link scatter order (legacy
// dynwave.c:385-388 iterates links ascending; each link updates node1 then
// node2). gatherConduitNodeFlows therefore performs the identical FP
// accumulation sequence per node accumulator — bit-exact at any thread count.
// ============================================================================

void DWSolver::buildConduitNodeCSR(const SimulationContext& ctx) {
    const auto& links = ctx.links;
    const auto& nodes = ctx.nodes;
    const auto un = static_cast<std::size_t>(n_nodes_);

    // Degree count over conduit ends
    csr_row_.assign(un + 1, 0);
    for (int ci = 0; ci < n_conduits_; ++ci) {
        const auto uj = static_cast<std::size_t>(
            conduit_idx_[static_cast<std::size_t>(ci)]);
        const int n1 = links.node1[uj];
        const int n2 = links.node2[uj];
        if (n1 >= 0 && n1 < n_nodes_) ++csr_row_[static_cast<std::size_t>(n1) + 1];
        if (n2 >= 0 && n2 < n_nodes_) ++csr_row_[static_cast<std::size_t>(n2) + 1];
    }
    for (std::size_t i = 0; i < un; ++i) csr_row_[i + 1] += csr_row_[i];

    const std::size_t n_entries = static_cast<std::size_t>(csr_row_[un]);
    csr_link_.assign(n_entries, 0);
    csr_is_n2_.assign(n_entries, 0);
    csr_other_outfall_.assign(n_entries, 0);

    // Fill in ascending conduit link index (conduit_idx_ is ascending-j), so
    // each node's entry list is automatically in serial-scatter order.
    std::vector<int> cursor(csr_row_.begin(), csr_row_.end() - 1);
    for (int ci = 0; ci < n_conduits_; ++ci) {
        const int j = conduit_idx_[static_cast<std::size_t>(ci)];
        const auto uj = static_cast<std::size_t>(j);
        const int n1 = links.node1[uj];
        const int n2 = links.node2[uj];
        const bool of1 = (n1 >= 0 && n1 < n_nodes_ &&
                          nodes.type[static_cast<std::size_t>(n1)] == NodeType::OUTFALL);
        const bool of2 = (n2 >= 0 && n2 < n_nodes_ &&
                          nodes.type[static_cast<std::size_t>(n2)] == NodeType::OUTFALL);
        if (n1 >= 0 && n1 < n_nodes_) {
            const auto k = static_cast<std::size_t>(cursor[static_cast<std::size_t>(n1)]++);
            csr_link_[k] = j;
            csr_is_n2_[k] = 0;
            csr_other_outfall_[k] = of2 ? 1 : 0;
        }
        if (n2 >= 0 && n2 < n_nodes_) {
            const auto k = static_cast<std::size_t>(cursor[static_cast<std::size_t>(n2)]++);
            csr_link_[k] = j;
            csr_is_n2_[k] = 1;
            csr_other_outfall_[k] = of1 ? 1 : 0;
        }
    }
}

// ============================================================================
// buildVirtualJunctionPairs — vjunc_ pair table (built once at init, post-CSR)
// ============================================================================
//
// One row per virtual junction. Orientation is recorded AFTER the adverse-
// slope reversal in PostParseResolver, so up_link/dn_link reflect the actual
// node1/node2 wiring. A pair with a through orientation (node2(up) == node ==
// node1(dn)) gets the full §3.2 momentum coupling; sag/peak orientations
// (both conduits pointing into or out of the node) still get zero-storage
// continuity but skip the directional pair coupling.

void DWSolver::buildVirtualJunctionPairs(const SimulationContext& ctx) {
    const auto& links = ctx.links;
    const auto& nodes = ctx.nodes;
    const auto& CD = ctx.link_subtypes.conduits;

    vjunc_.clear();
    vj_pair_n1_.assign(static_cast<std::size_t>(n_links_), -1);
    vj_pair_n2_.assign(static_cast<std::size_t>(n_links_), -1);

    std::vector<int> row_of_node(static_cast<std::size_t>(n_nodes_), -1);
    for (int i = 0; i < n_nodes_; ++i) {
        if (i < static_cast<int>(nodes.is_virtual.size()) &&
            nodes.is_virtual[static_cast<std::size_t>(i)]) {
            row_of_node[static_cast<std::size_t>(i)] = static_cast<int>(vjunc_.size());
            VJuncPair p;
            p.node = i;
            vjunc_.push_back(p);
        }
    }
    if (vjunc_.empty()) return;

    for (int j = 0; j < n_links_; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::CONDUIT) continue;
        for (const bool at_node1 : { true, false }) {
            const int n = at_node1 ? links.node1[uj] : links.node2[uj];
            if (n < 0 || n >= n_nodes_) continue;
            const int r = row_of_node[static_cast<std::size_t>(n)];
            if (r < 0) continue;
            VJuncPair& p = vjunc_[static_cast<std::size_t>(r)];
            if      (p.link_a < 0) p.link_a = j;
            else if (p.link_b < 0) p.link_b = j;
            // A chain link belongs to two pairs — record each end separately
            // (the old single vj_of_link_ map was last-write-wins, which left
            // interior links coupled only to their downstream pair: the
            // upwinding mechanism never fired and the convective correction
            // was applied one-sided).
            if (at_node1) vj_pair_n1_[uj] = r;
            else          vj_pair_n2_[uj] = r;
        }
    }

    for (VJuncPair& p : vjunc_) {
        if (p.link_a < 0 || p.link_b < 0) continue;  // validation rejects these
        const auto ua = static_cast<std::size_t>(p.link_a);
        const auto ub = static_cast<std::size_t>(p.link_b);

        // Through orientation: node2 of one conduit and node1 of the other.
        if (links.node2[ua] == p.node && links.node1[ub] == p.node) {
            p.up_link = p.link_a; p.dn_link = p.link_b; p.through = 1;
        } else if (links.node2[ub] == p.node && links.node1[ua] == p.node) {
            p.up_link = p.link_b; p.dn_link = p.link_a; p.through = 1;
        } else {
            p.through = 0;  // sag (both node2) or peak (both node1)
        }

        const int ca = ctx.link_subtypes.conduit_row(p.link_a);
        const int cb = ctx.link_subtypes.conduit_row(p.link_b);
        const double la = (ca >= 0) ? CD.length[static_cast<std::size_t>(ca)] : 0.0;
        const double lb = (cb >= 0) ? CD.length[static_cast<std::size_t>(cb)] : 0.0;
        p.lambda = 0.5 * (la + lb);
    }
}

// ============================================================================
// vjPrepareIteration — per-Picard-iteration virtual-junction pair cache
// ============================================================================
//
// Computes for each through pair: the shared junction sigma from the
// through-flow Froude number (velocity from the pair-average discharge and
// the junction-end area, hydraulic depth from the junction-end area/width —
// both evaluated from the shared node head on the identical cross-section),
// the cross-junction upwind states (up-link mid area / hyd. radius), and in
// FULL momentum mode the cross-junction convective correction
//   dq4_j = dt · σ_j · [(v²A)_dn,mid − (v²A)_up,mid] / Λ .
// Uses the pre-STEP-E geometry of this iteration; when the pair surcharges
// the per-link kernels force sig = 0 anyway (closed-full), so the small
// slot-override inconsistency is irrelevant.

void DWSolver::vjPrepareIteration(const SimulationContext& ctx, double dt) {
    const auto& links = ctx.links;
    const bool full_momentum = (ctx.options.virtual_junction_momentum == 1);

    for (VJuncPair& p : vjunc_) {
        p.active = 0;
        p.dirn = 0;
        p.dq4j = 0.0;
        if (!p.through || p.up_link < 0 || p.dn_link < 0) continue;

        const auto uu = static_cast<std::size_t>(p.up_link);
        const auto ud = static_cast<std::size_t>(p.dn_link);
        const auto ucu = static_cast<std::size_t>(tile_uj_to_ci_[uu]);
        const auto ucd = static_cast<std::size_t>(tile_uj_to_ci_[ud]);

        // Junction-end state from the up-link's downstream end (same node
        // head, identical xsect ⇒ same as the dn-link's upstream end).
        const double aj = area2_[uu];
        const double wj = width2_[uu];
        if (aj <= FUDGE) continue;    // dry junction — per-link behaviour

        // #144: the junction through-flow velocity and the upwind state handed
        // to the neighbour's aWtd are momentum quantities, so both use
        // conveyance areas. `dh` keeps `aj` — the hydraulic depth is a
        // free-surface property, and a surcharged closed pipe leaves width2_
        // at the crown-capped value so the wj > FUDGE gate already zeroes it.
        const double ajFull = links.xsect_a_full[uu];
        const double qU = links.flow[uu] / tile_barrels_d_[ucu];
        const double qD = links.flow[ud] / tile_barrels_d_[ucd];
        double vj = 0.5 * (qU + qD) / conveyArea(aj, ajFull);
        if (std::fabs(vj) > MAX_VELOCITY)
            vj = (vj > 0.0) ? MAX_VELOCITY : -MAX_VELOCITY;

        const double dh = (wj > FUDGE) ? aj / wj : 0.0;
        const double frj = (dh > 0.0)
            ? std::fabs(vj) / std::sqrt(constants::GRAVITY * dh) : 0.0;
        p.sigma_j = std::max(0.0, std::min(1.0, 2.0 * (1.0 - frj)));

        p.a_up_mid = conveyArea(area_mid_[uu], ajFull);
        p.r_up_mid = hrad_mid_[uu];
        p.active   = 1;
        // Coherent signed through-flow: momentum is transmitted across the
        // node only when both conduits carry flow the same way through it.
        // Opposing/still flows (seiche antinode, converging fill fronts)
        // have no momentum stream to transmit — the correction must vanish.
        if      (qU >= 0.0 && qD >= 0.0 && qU + qD > 0.0) p.dirn = 1;
        else if (qU <= 0.0 && qD <= 0.0 && qU + qD < 0.0) p.dirn = -1;

        if (full_momentum && p.lambda > 0.0 && p.dirn != 0) {
            const double aMu = std::max(area_mid_[uu], FUDGE);
            const double aMd = std::max(area_mid_[ud], FUDGE);
            const double vu = qU / aMu;
            const double vd = qD / aMd;
            // MEASURED DEFECT (2026-08-14, epaswmm5_qa suites/swashes
            // runs/_fv_vj_probe/vj_full_sign_test.py). This term is
            // SIGN-INVERTED relative to the per-link convective term it
            // supplements. At constant Q, Δ(v²A) = Q²Δ(1/A) = −v²ΔA, whereas
            // the per-link dq4 = σ·v²·(A₂−A₁)/L below is the correct
            // non-conservative EXTRAN form (d(v²A)/dx = −v²dA/dx at constant
            // Q is exactly what makes gA·dH/dx = v²dA/dx − gA·Sf reproduce
            // the steady momentum equation at σ=1). Negating dq4j on
            // macdonald-periodic restores mass conservation outright at
            // dx=10 (−224.2 % → 0.000 %) and moves the depth bias from +0.629
            // to +0.135 velocity heads. It does NOT make FULL useful: even
            // sign-corrected it is l1 5.24 % against BASIC's 0.163 % and
            // plain DW's 0.141 %, because dq4j is also added to BOTH adjacent
            // links (2485/2503) on top of each link's own full-length dq4, so
            // the convective term is applied ~3x. Attributing it to one side
            // instead is worse still (l1 86-103 %). No term-level correction
            // salvages this; see plans/VJ_MOMENTUM_SCOPE.md Phase 3.
            p.dq4j = dt * p.sigma_j * (vd * vd * aMd - vu * vu * aMu) / p.lambda;
        }
    }
}

// ============================================================================
// vjAccumulateResiduals — post-step momentum-residual diagnostic
// ============================================================================
//
// R_j = (Q²/A)_up,end − (Q²/A)_dn,start + g·Ā·(y_up,end − y_dn,start)
// per barrel, evaluated at the converged state of the routing step. With
// equal cross-sections and matching end depths the hydrostatic terms cancel
// exactly; the mean-area form keeps the diagnostic first-order accurate when
// flow-classification patches leave the two end depths unequal. Mirrored
// into ctx.vj_diag for the .rpt Virtual Junction Summary.

void DWSolver::vjAccumulateResiduals(SimulationContext& ctx) {
    if (vjunc_.empty()) return;

    auto& d = ctx.vj_diag;
    if (d.node_idx.size() != vjunc_.size()) {
        d.clear();
        for (const VJuncPair& p : vjunc_) {
            d.node_idx.push_back(p.node);
            d.up_link.push_back(p.up_link);
            d.dn_link.push_back(p.dn_link);
            d.resid_max.push_back(0.0);
            d.resid_sum.push_back(0.0);
            d.resid_n.push_back(0);
        }
    }

    const auto& links = ctx.links;
    for (std::size_t r = 0; r < vjunc_.size(); ++r) {
        VJuncPair& p = vjunc_[r];
        if (!p.through || p.up_link < 0 || p.dn_link < 0) continue;
        const auto uu = static_cast<std::size_t>(p.up_link);
        const auto ud = static_cast<std::size_t>(p.dn_link);
        const double aU = area2_[uu];
        const double aD = area1_[ud];
        if (aU <= FUDGE || aD <= FUDGE) continue;

        const auto ucu = static_cast<std::size_t>(tile_uj_to_ci_[uu]);
        const auto ucd = static_cast<std::size_t>(tile_uj_to_ci_[ud]);
        const double qU = links.flow[uu] / tile_barrels_d_[ucu];
        const double qD = links.flow[ud] / tile_barrels_d_[ucd];

        const double flux = qU * qU / aU - qD * qD / aD;
        const double hyd  = constants::GRAVITY * 0.5 * (aU + aD)
                            * (depth2_[uu] - depth1_[ud]);
        const double R = flux + hyd;
        const double aR = std::fabs(R);

        p.resid_max = std::max(p.resid_max, aR);
        p.resid_sum += aR;
        ++p.resid_n;
        d.resid_max[r] = p.resid_max;
        d.resid_sum[r] = p.resid_sum;
        d.resid_n[r]   = p.resid_n;
    }
}

// ============================================================================
// setNumThreads — configure OpenMP parallelism
// ============================================================================

#if defined(__APPLE__)
// Pin the calling thread's QoS to USER_INTERACTIVE so the macOS scheduler
// keeps it on P-cores. Called once per team thread from the persistent
// parallel region prologue in execute() (thread_local latch). Errors are
// ignored — QoS is a scheduling hint, not a correctness requirement.
static void darwinPinThreadToPerfCores() {
    static thread_local bool applied = false;
    if (!applied) {
        (void)pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        applied = true;
    }
}
#endif

void DWSolver::setNumThreads(int n, std::vector<std::string>* warnings) {
    // A/B override: SWMM_DW_THREADS=<N> forces the DW Picard thread count,
    // bypassing the model-size gate below (unset or invalid → normal path).
    // Results are bit-identical at any thread count (single-producer writes,
    // integer-only cross-thread combines), so this is timing-only.
    if (const char* e = std::getenv("SWMM_DW_THREADS")) {
        const int forced = std::atoi(e);
        if (forced >= 1) {
            num_threads_ = forced;
            if (warnings) {
                char buf[160];
                std::snprintf(buf, sizeof buf,
                    "Dynamic-wave thread count forced to %d by the "
                    "SWMM_DW_THREADS environment variable ([OPTIONS] THREADS "
                    "ignored for dynamic wave).", forced);
                warnings->emplace_back(buf);
            }
            return;
        }
    }

    // Resolution rules (THREADS = 0 auto vs explicit N, model-size gate,
    // Apple Silicon P-core clamp for auto only) live in ThreadInfo so the
    // C API's swmm_get_effective_threads() reports exactly what runs here.
    num_threads_ = threadinfo::dwThreads(n, n_conduits_, warnings);
}

// ============================================================================
// Main execute -- Picard iteration
// ============================================================================

int DWSolver::execute(SimulationContext& ctx, double dt,
                      DWSolver::NonConduitFlowFunc non_conduit_fn) {
    int steps = 0;
    bool converged = false;

    // Per-timestep constant
    dt_gravity_ = dt * GRAVITY;

    // Zero-loss latch (see recomputeConduitLossOne): flips permanently the
    // first time a nonzero loss becomes possible. Hoisted out of the former
    // per-iteration loss pass — evap_rate is fixed for the whole execute()
    // call, so the per-iteration latch update was equivalent to this
    // per-step one. Keeping the member read-only inside the Picard loop also
    // makes it safely readable by every team thread.
    if (evap_rate > 0.0 || any_conduit_seep_) losses_all_zero_ = false;

    // Save area_mid from PREVIOUS TIMESTEP for the unsteady momentum term.
    // This must happen ONCE per timestep, BEFORE the iteration loop, matching
    // legacy dynwave.c:280 which sets a2 = a1 in initRoutingStep().
    // area_mid_ still holds the final midpoint areas from the previous timestep.
    std::copy(area_mid_.begin(), area_mid_.end(), area_old_.begin());

    // TPA latch (issue #156 Phase 5): advanced once per routing step from the
    // last committed state, so Picard/Anderson iterate a FIXED operator.
    if (surcharge_method == SurchargeMethod::TPA) updateTpaLatch(ctx);

    // Clear bypass flags at the start of each timestep
    // (matching legacy initRoutingStep: Link[i].bypassed = FALSE)
    std::fill(bypassed_.begin(), bypassed_.end(), uint8_t{0});

    // Refresh the node-invariant tile for this step (see NodeTile). One
    // sequential pass here replaces seven scattered array reads per node per
    // Picard iteration inside setNodeDepth/updateNodeDepths.
    {
        const auto& nd = ctx.nodes;
        if (node_tile_.size() != static_cast<std::size_t>(n_nodes_))
            node_tile_.resize(static_cast<std::size_t>(n_nodes_));
        unit_sys_ = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
        for (int i = 0; i < n_nodes_; ++i) {
            auto ui = static_cast<std::size_t>(i);
            NodeTile& t  = node_tile_[ui];
            t.full_depth  = nd.full_depth[ui];
            t.y_crown     = nd.crown_elev[ui] - nd.invert_elev[ui];
            t.invert_elev = nd.invert_elev[ui];
            t.ponded_area = nd.ponded_area[ui];
            t.sur_depth   = nd.sur_depth[ui];
            t.full_volume = nd.full_volume[ui];
            t.degree      = nd.degree[ui];
            t.is_storage  = (nd.type[ui] == NodeType::STORAGE) ? 1 : 0;
            t.is_outfall  = (nd.type[ui] == NodeType::OUTFALL) ? 1 : 0;
            t.is_virtual  = (ui < nd.is_virtual.size() && nd.is_virtual[ui]) ? 1 : 0;
        }
    }

    // ------------------------------------------------------------------------
    // Persistent-team Picard loop (B2 threading).
    //
    // ONE `omp parallel` region wraps the whole Picard iteration loop, so the
    // team is forked once per ROUTING STEP instead of once per phase per
    // iteration (legacy findLinkFlows/findNodeDepths each fork per iteration
    // — the barrier/fork churn is why legacy threading is net-negative at
    // ~1000 links). Serial phases run inside `omp single`; the momentum and
    // node-depth loops are orphaned `omp for` worksharing constructs binding
    // to this team.
    //
    // Loop control (`steps`, `converged`) is REPLICATED per thread: every
    // thread computes it from shared data that is only read behind barriers,
    // so all replicas are identical and all threads take the same branches —
    // required, since OpenMP demands every thread encounter the same sequence
    // of worksharing constructs.
    //
    // BIT-EXACTNESS: every parallel loop writes disjoint single-producer
    // elements (per-link / per-node), and the only cross-thread combine is an
    // integer unconverged-node count (order-free). Results are therefore
    // bit-identical at any thread count, and identical to the pre-threading
    // serial phase order (which matches legacy dynwave_execute).
    //
    // At num_threads_ == 1 the `if` clause keeps the region inactive (team of
    // one; barriers/singles are no-ops on the encountering thread).
    // ------------------------------------------------------------------------
    int  steps_final     = 0;
    bool converged_final = false;
    int  unconv_shared   = 0;   // shared team tally (see updateNodeDepthsTeam)
    const bool has_nc    = static_cast<bool>(non_conduit_fn);

#pragma omp parallel num_threads(num_threads_) if(num_threads_ > 1) default(shared)
    {
#if defined(__APPLE__)
        // One-time per pool thread (thread_local latch inside): QoS pin to
        // P-cores. Without it, macOS may schedule active-spinning team
        // threads onto E-cores, making every barrier wait on an E-core
        // straggler (the T=8 collapse — see threadinfo::dwThreads).
        darwinPinThreadToPerfCores();
#endif
        int  t_steps     = 0;      // per-thread replica of legacy `Steps`
        bool t_converged = false;  // per-thread replica of `converged`

        while (t_steps < max_trials) {
            // Step 0: init node states (team `omp for nowait` — overlaps with
            // geometry STEP A, which touches none of its outputs).
            initNodeStates(ctx);

            // Step 1: batch compute ALL cross-section geometry (with slot
            // overrides). Team-callable: parallel per-conduit passes (STEP
            // A/C/E) interleaved with `omp single` batch kernels (STEP B/D).
            // The outfall conduit's downstream depth comes from the outfall
            // node depth set at the END of the PREVIOUS Picard iteration
            // (Step 4b below), exactly as legacy findLinkFlows[iter N] uses
            // the outfall depth from findNodeDepths[iter N-1].
            computeLinkGeometry(ctx);

            // ---- Steps 1.5 + 2 fused (team): per-conduit loss recompute,
            // EXTRAN/static-slot STEP E overrides, momentum classification,
            // momentum kernel dispatch, links.flow commit — one `omp for`
            // over conduits (own-element sub-steps preserve the former
            // pass-by-pass per-element operation order exactly).
            momentumKernels(ctx, dt, t_steps);

            // ---- Step 3a: scatter conduit flows to nodes (team) ----
            // Production path (callback present): parallel CSR node-centric
            // gather — bit-exact per-node accumulation order (see
            // buildConduitNodeCSR). Non-conduit flows are scattered by the
            // callback in Step 4, exactly as legacy findLinkFlows scatters
            // conduits first (dynwave.c:385-388), then computes+scatters
            // each non-conduit serially (dynwave.c:391-398).
            if (has_nc) gatherConduitNodeFlows(ctx);

            // ---- Serial scatter/structures phase (one thread) ----
#pragma omp single
            {
                // Step 3b: no-callback path (tests/standalone): serial
                // all-links scatter, matching the pre-threading behaviour.
                if (!has_nc) updateNodeFlows(ctx);

                // Step 4: compute non-conduit flows (pumps, orifices, weirs,
                //         outlets) INSIDE the iteration loop, matching legacy
                //         dynwave.c:370-399 findLinkFlows() which calls
                //         findNonConduitFlow() per iteration.
                if (has_nc) {
                    non_conduit_fn(ctx, dt, t_steps);
                }

                // Step 4b: set outfall boundary depths from the CURRENT
                // iteration's link flows (now committed to links.flow by
                // updateNodeFlows / the non-conduit callback). This matches
                // legacy, which calls link_setOutfallDepth at the TOP of
                // findNodeDepths (dynwave.c:592) — i.e. AFTER findLinkFlows,
                // using the just-computed Link.newFlow. Running it at Step 0
                // with the previous iteration's flow lagged the free-outfall
                // critical/normal depth by one iteration (e.g. extran1's free
                // outfall 10208 read 0 while legacy had a non-zero yCrit),
                // seeding a per-iteration divergence.
                openswmm::outfall::setAllOutfallDepths(ctx, ctx.current_date);

                // Step 5: flag nodes where AA must be skipped (non-smooth
                // operator). Only needed when Anderson acceleration is active.
                if (anderson_accel) computeAASkipFlags(ctx);

                // Reset the shared unconverged tally for Step 6 (behind this
                // single's implicit barrier, so no thread has started adding).
                unconv_shared = 0;
            }

            // ---- Step 6: update node depths, tally convergence (team) ----
            updateNodeDepthsTeam(ctx, dt, t_steps, unconv_shared);
            t_converged = (unconv_shared == 0);

            t_steps++;

            if (t_steps > 1) {
                if (t_converged) break;

                // Mark links whose both end nodes converged so they can be
                // skipped in the next iteration (matching legacy
                // findBypassedLinks; contains a team `omp for`). Conditional
                // worksharing is legal here: every thread evaluates the
                // identical replicated condition.
                findBypassedLinks(ctx);
            }
        }

#pragma omp master
        {
            steps_final     = t_steps;
            converged_final = t_converged;
        }
    }   // end persistent parallel region (implicit barrier)

    steps = steps_final;
    converged = converged_final;

    // Post-Picard: update per-node non-convergence counts (matching legacy
    // updateConvergenceStats: increment count for each unconverged node when
    // the overall step did not converge).
    //
    // Outfalls are excluded: their `converged` flag is deliberately left
    // FALSE by updateNodeDepthsTeam (to keep outfall-connected links from
    // being bypassed mid-Picard), but they are never tested for convergence
    // and cannot cause a step to fail. Counting them here ranked boundary
    // outfalls at the top of "Most Frequent Nonconverging Nodes" (at exactly
    // the overall failed-step percentage), hiding the junctions/storage nodes
    // actually responsible. Reporting-only change: does not touch the
    // converged flags, unconv_shared, step convergence, or findBypassedLinks.
    if (!converged) {
        for (int i = 0; i < n_nodes_; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (!node_tile_[ui].is_outfall && !xnode_.converged[ui])
                ++ctx.nodes.stat_non_converged_count[ui];
        }
    }

    // Post-Picard: update DPS temporal state (P decay, surcharge tracking)
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        updateDPSState(ctx, dt);
    }

    // Post-Picard: virtual-junction momentum-residual diagnostic (no-op when
    // the model has no virtual junctions).
    vjAccumulateResiduals(ctx);

    // Record the actual final convergence (legacy counts a step as
    // non-converging only when this is false, even if it used all MaxTrials —
    // a step that converges ON the last allowed iteration is "converged").
    last_converged_ = converged;
    return steps;
}

// ============================================================================
// initNodeStates
// ============================================================================

void DWSolver::initNodeStates(SimulationContext& ctx) {
    auto& nodes = ctx.nodes;
    const int unit_sys = ucf::getUnitSystem(
        static_cast<int>(ctx.options.flow_units));
    // Team-callable: orphaned `omp for` on the persistent team (see execute()).
    // Per-node single-producer writes — bit-exact at any thread count.
    // `nowait`: the phase that follows in execute() (geometry STEP A / the
    // serial prep single) neither reads nor writes any of this pass's outputs
    // (node inflow/outflow/new_surf_area, xnode_ converged/sumdqdh), and every
    // consumer (updateNodeFlows / the non-conduit callback / setNodeDepth)
    // sits behind at least one later barrier.
    #pragma omp for schedule(static) nowait
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        xnode_.converged[ui] = 0;
        xnode_.sumdqdh[ui] = 0.0;

        // Initial nodal surface area, matching legacy initNodeStates
        // (dynwave.c:296-303): when ALLOW_PONDING is on, a node flooded above
        // its rim spreads over its ponded area, so the surface-area baseline
        // must come from node_getPondedArea(); otherwise node_getSurfArea()
        // (0 for non-storage, the curve area for STORAGE).  Always using
        // getSurfArea() understated the area of ponded nodes, so flood water
        // that legacy stores instead overflowed and broke routing continuity.
        //
        // 2D-coupled junctions always use the ponded-area baseline (their
        // ponded_area is the auto-assigned 2D-cell footprint) so the HGL can
        // rise above the crown to track the 2D surface, independent of the
        // global ALLOW_PONDING option. See ctx.coupled_node.
        const bool node_can_pond = ctx.options.allow_ponding
            || (ui < ctx.coupled_node.size() && ctx.coupled_node[ui]);
        const NodeTile& t = node_tile_[ui];
        if (!t.is_storage) {
            // Fast path (bit-exact): non-storage nodes have zero curve area —
            // node::getSurfArea returns 0.0 (Node.cpp, legacy node.c) — and
            // node::getPondedArea reduces to the raw ponded_area only when
            // flooded above the rim with a positive ponded area
            // (legacy node.c:535-551); every other branch yields 0.0. This
            // avoids two function calls per node per Picard iteration for
            // the ~98% of nodes that are not storage units.
            xnode_.new_surf_area[ui] =
                (node_can_pond && nodes.depth[ui] > t.full_depth &&
                 t.ponded_area > 0.0)
                    ? t.ponded_area
                    : 0.0;
        } else {
            xnode_.new_surf_area[ui] = node_can_pond
                ? node::getPondedArea(nodes, i, nodes.depth[ui], &ctx.tables, unit_sys, &ctx.node_subtypes)
                : node::getSurfArea(nodes, i, nodes.depth[ui], &ctx.tables, unit_sys, &ctx.node_subtypes);
        }

        // Reset node flows (matching legacy initNodeStates)
        nodes.inflow[ui] = 0.0;
        nodes.outflow[ui] = nodes.losses[ui];
        double lat = nodes.lat_flow[ui];
        if (lat >= 0.0) {
            nodes.inflow[ui] += lat;
        } else {
            nodes.outflow[ui] -= lat;
        }
    }
}

// ============================================================================
// findBypassedLinks -- skip converged links in next iteration
// (matching legacy dynwave.c::findBypassedLinks)
// ============================================================================

void DWSolver::findBypassedLinks(const SimulationContext& ctx) {
    const auto& links = ctx.links;
    // PARITY: legacy findBypassedLinks (dynwave.c) marks EVERY link — conduits
    // AND non-conduits (weirs/orifices/pumps/outlets) — whose both end nodes
    // converged, and legacy findLinkFlows then skips dwflow_findConduitFlow /
    // findNonConduitFlow for them (their flow is held). The non-conduit skip is
    // honoured in the non_conduit_fn callback (SWMMEngine), which restores the
    // held flow/dqdh for a bypassed structure. Without it, a weir/orifice whose
    // nodes have settled kept being recomputed and drifted from legacy's held
    // value (e.g. extran4 weir 90010).
    //
    // Team-callable: orphaned `omp for` on the persistent team (see execute()).
    // Per-link single-producer writes from read-only converged flags —
    // bit-exact at any thread count. The implicit closing barrier orders
    // bypassed_ for the next iteration's geometry/mask readers.
    #pragma omp for schedule(static)
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) { bypassed_[uj] = 0; continue; }
        const uint8_t b = (xnode_.converged[static_cast<std::size_t>(n1)] &&
                           xnode_.converged[static_cast<std::size_t>(n2)]) ? 1 : 0;
        bypassed_[uj] = b;
    }

    // Virtual-junction pairing: the two conduits of a pair bypass together or
    // not at all — a frozen half-pair would break flux continuity at the
    // zero-storage node. Tiny serial fixup behind the `omp for` barrier;
    // every thread evaluates the identical replicated emptiness condition.
    if (!vjunc_.empty()) {
        #pragma omp single
        {
            for (const VJuncPair& p : vjunc_) {
                if (p.link_a < 0 || p.link_b < 0) continue;
                const auto ua = static_cast<std::size_t>(p.link_a);
                const auto ub = static_cast<std::size_t>(p.link_b);
                if (bypassed_[ua] != bypassed_[ub]) {
                    bypassed_[ua] = 0;
                    bypassed_[ub] = 0;
                }
            }
        }
    }
}

// ============================================================================
// computeLinkGeometry -- BATCH via XSectGroups with flow classification
// ============================================================================

/// Build XSectParams from link SoA data (with shape translation for batch API).
static XSectParams buildXSP(const SimulationContext& ctx, std::size_t uk) {
    const LinkData& links = ctx.links;
    XSectParams xs{};
    // Translate LinkData enum (CIRCULAR=0) to batch enum (CIRCULAR=1)
    auto ls = links.xsect_shape[uk];
    xs.type = (ls == XsectShape::DUMMY) ? 0 : static_cast<int>(ls) + 1;
    xs.y_full = links.xsect_y_full[uk];
    xs.a_full = links.xsect_a_full[uk];
    xs.w_max  = links.xsect_w_max[uk];
    // Depth of maximum width. Read by the seepage clamp in
    // recomputeConduitLossOne (legacy link.c:1381 `if (d >= ywMax) d = ywMax`)
    // and by nothing else in xsect — leaving it 0 collapsed d_seep to 0, so the
    // wetted width was 0 and DYNWAVE conduit seepage was identically zero for
    // every shape. Router::computeConduitLosses (KINWAVE/STEADY/FV) always
    // passed it, which is why the loss appeared under those models only.
    xs.yw_max = links.xsect_yw_max[uk];
    xs.r_full = links.xsect_r_full[uk];
    xs.s_full = links.xsect_s_full[uk];
    xs.s_max  = links.xsect_s_max[uk];
    xs.y_bot  = links.xsect_y_bot[uk];
    xs.a_bot  = links.xsect_a_bot[uk];
    xs.s_bot  = links.xsect_s_bot[uk];
    xs.r_bot  = links.xsect_r_bot[uk];
    // Tabulated shapes (IRREGULAR / CUSTOM / STREET) carry their A/R/W vs depth
    // in per-link transect tables — without these the scalar getters
    // (getWofY/getAofY/getRofY/getAofS/getYofA/getYcrit) return 0, which silently
    // collapses every per-element flow-class patch (UP/DN_CRITICAL, UP/DN_DRY)
    // and the Froude reclassification for transect conduits. Point the table
    // fields at ctx.transect_tables (stable for the run), matching the batch path.
    if (ls == XsectShape::IRREGULAR || ls == XsectShape::CUSTOM ||
        ls == XsectShape::STREET_XSECT) {
        const int ci = links.xsect_curve[uk];
        if (ci >= 0 && static_cast<std::size_t>(ci) < ctx.transect_tables.size()) {
            const auto& td = ctx.transect_tables[static_cast<std::size_t>(ci)];
            xs.transect        = ci;
            xs.area_tbl        = td.area_tbl;
            xs.hrad_tbl        = td.hrad_tbl;
            xs.width_tbl       = td.width_tbl;
            xs.area_lut        = &td.area_lut;
            xs.transect_tbl_size = transect::N_TRANSECT_TBL;
        }
    }
    return xs;
}

/// Normal depth from Manning's equation: s = q/beta → a = getAofS → y = getYofA.
static double computeYnorm(const XSectParams& xs, double beta, double q_max,
                           double q) {
    if (beta <= 0.0 || q <= 0.0) return 0.0;
    if (q_max > 0.0 && q > q_max) q = q_max;
    double s = q / beta;
    double a = xsect::getAofS(xs, s);
    return xsect::getYofA(xs, a);
}

void DWSolver::computeLinkGeometry(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // Restrict this iteration's batch triple kernels (STEP B widths, STEP D
    // areas/hyd-radii) to non-bypassed links. Legacy findLinkFlows skips a
    // bypassed link outright, HOLDING all its per-link state — including
    // values written by the momentum kernels after the geometry pass (e.g.
    // the DRY branch's Conduit.a1 = 0.5*(a1+a2), dwflow.c:168, mirrored in
    // processDryLink). The unmasked batch recompute clobbered those held
    // values with the raw shape-kernel result each iteration — a subtle
    // legacy deviation invisible to the fixed-step QA suite (dry links carry
    // v≈0 so dq3 never surfaces it) but visible through the variable-step
    // CFL, which reads area_mid_ of bypassed links (getLinkStep). Masking is
    // therefore BOTH the faster and the more legacy-faithful behaviour
    // (Bellinge: step_loop 95.8→80.7 s, −15.7%). With ~9 Picard iterations
    // per step on converging networks, most kernel invocations otherwise run
    // over a mostly-converged link set.
    // SWMM_DISABLE_BYPASS_MASK reverts to the old clobbering behaviour for
    // A/B verification only.
    //
    // The mask is passed whenever enabled (the old any_bypassed_ pre-gate is
    // gone): setBypassMask's own per-group byte pre-count already handles the
    // nothing-bypassed case with no packing work, and maskedGroup() returns
    // the identical full group either way — bit-exact, and it removes the
    // cross-thread OR that any_bypassed_ would need under the persistent team.
    // `single nowait`: one thread packs the mask while the rest start STEP A
    // (which never reads the mask); the mask's only consumers are the STEP B/D
    // triple kernels behind STEP A's closing barrier.
    static const bool mask_disabled = std::getenv("SWMM_DISABLE_BYPASS_MASK") != nullptr;
    #pragma omp single nowait
    {
        groups_->setBypassMask(!mask_disabled ? bypassed_.data() : nullptr);
    }

    // ---- STEP A + STEP B prep (fused): depths/heads + width-cap buffers ----
    // Phase B-1: collapse the previous two per-conduit passes into one. STEP B
    // prep wrote wcap_d1/d2/dm based on STEP A's freshly written depth1/2/mid
    // — now we compute and write wcap_d* directly from the depth values that
    // are still in registers, avoiding a second cache traversal.
    // Both SLOT and DYNAMIC_SLOT skip the depth/width crown clamp so the
    // surcharged depth propagates through to applyDPSGeometry (DPS) or the
    // Sjoberg slot override (SLOT).  Without this, the midpoint depth clamps
    // to y_full and the slot machinery sees a non-surcharged state even when
    // node depth has clearly exceeded the crown.
    const bool slot_mode = (surcharge_method == SurchargeMethod::SLOT ||
                            surcharge_method == SurchargeMethod::DYNAMIC_SLOT);
    // B2 threading: orphaned `omp for` on the persistent team. Per-conduit
    // single-producer writes (depth1/2/mid, h1/h2, fasnh, wcap_*) from
    // read-only node depths — bit-exact at any thread count. SIMD inside
    // each shape-kernel call is preserved.
    #pragma omp for schedule(static)
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        int j = tile_uj_[uci];
        auto uj = static_cast<std::size_t>(j);

        // Legacy parity + perf: a bypassed conduit (both end nodes converged)
        // is skipped entirely by legacy findLinkFlows, so its depths/widths/
        // areas are never recomputed.  Its end-node depths are unchanged, so
        // the depth1_/depth2_/depth_mid_/h1_/h2_/wcap_* values already in the
        // arrays are exactly correct — recomputing them is pure waste.  This
        // mirrors the bypass skip in solveMomentumBatch and classifyMomentum.
        if (bypassed_[uj]) continue;

        auto un1 = static_cast<std::size_t>(tile_n1_[uci]);
        auto un2 = static_cast<std::size_t>(tile_n2_[uci]);

        const double inv1 = tile_inv1_elev_[uci];
        const double inv2 = tile_inv2_elev_[uci];
        const double z1 = inv1 + tile_z1_off_[uci];
        const double z2 = inv2 + tile_z2_off_[uci];
        const double h1 = std::max(nodes.depth[un1] + inv1, z1);
        const double h2 = std::max(nodes.depth[un2] + inv2, z2);

        double y1 = std::max(h1 - z1, FUDGE);
        double y2 = std::max(h2 - z2, FUDGE);

        const double yf = tile_y_full_[uci];
        if (!slot_mode) {
            y1 = std::min(y1, yf);
            y2 = std::min(y2, yf);
        }
        const double yMid = 0.5 * (y1 + y2);

        depth1_[uj] = y1;
        depth2_[uj] = y2;
        depth_mid_[uj] = yMid;
        h1_[uj] = h1;
        h2_[uj] = h2;
        fasnh_[uj] = 1.0;

        // STEP B prep — fused inline. yCap is the EXTRAN crown cap; SLOT
        // mode disables it (yCap = ∞). Branchless via select.
        if (!slot_mode) {
            const int bs = tile_xsect_batch_shape_[uci];
            const bool is_open = xsect::isOpen(bs);
            const double yCap = (!is_open && yf > 0.0) ? EXTRAN_CROWN_CUTOFF * yf : 1e30;
            wcap_d1_[uj] = std::min(y1,   yCap);
            wcap_d2_[uj] = std::min(y2,   yCap);
            wcap_dm_[uj] = std::min(yMid, yCap);
        }
    }

    // ---- STEP B: Batch widths from depths (needed for surface area) ----
    // For EXTRAN/static-slot, the cap-buffers above feed the width kernel;
    // for SLOT mode, depths feed it directly (no crown cap).
    // Team-split (B3): each thread runs the triple kernels over ITS static
    // slice of every shape group (computeWidthsTripleTeam) — disjoint
    // single-producer element writes, including the shared group scratch
    // buffers which are sliced on cache-line boundaries, so results are
    // bit-identical to the former serial `omp single` at any thread count.
    // STEP A's closing barrier ordered the depth/cap inputs AND the bypass
    // mask (single nowait above); the explicit barrier below orders
    // width1/2/mid for their cross-slice consumers (STEP C / the SLOT
    // override loop, which partition conduits differently than the shape
    // groups do).
    {
        const int b_tid = omp_get_thread_num();
        const int b_nth = omp_get_num_threads();
        if (!slot_mode) {
            groups_->computeWidthsTripleTeam(
                wcap_d1_.data(), wcap_d2_.data(), wcap_dm_.data(),
                width1_.data(), width2_.data(), width_mid_.data(),
                n_links_, b_tid, b_nth);
        } else {
            groups_->computeWidthsTripleTeam(
                depth1_.data(), depth2_.data(), depth_mid_.data(),
                width1_.data(), width2_.data(), width_mid_.data(),
                n_links_, b_tid, b_nth);
        }
    }
    #pragma omp barrier
    if (slot_mode) {
        // For a surcharged closed conduit the tabulated top width goes to 0 at
        // the crown (e.g. circular W_Circ[full]=0), so the surface-area that
        // STEP C builds for the connected nodes would collapse to MinSurfArea.
        // Legacy getWidth() instead returns the Preissmann slot width when
        // surcharged (dwflow.c). Without this the SLOT node surface area is too
        // small, the depth update overshoots the rim, and the excess is booked
        // as phantom flooding (test1 SLOT continuity −16% vs legacy −2%).
        // Team `omp for`: per-conduit single-producer overwrites of the width
        // arrays, reading only STEP-A depths (ordered by STEP A's barrier) and
        // kernel widths (ordered by the barrier above). Its implicit closing
        // barrier orders the final widths for STEP C.
        #pragma omp for schedule(static)
        for (int ci = 0; ci < n_conduits_; ++ci) {
            auto uci = static_cast<std::size_t>(ci);
            auto uj  = static_cast<std::size_t>(tile_uj_[uci]);
            double yf = tile_y_full_[uci];
            double wm = tile_w_max_[uci];
            XsectShape shape = tile_shape_[uci];
            if (depth1_[uj] > yf) {
                double ws = getSlotWidth(depth1_[uj], yf, wm, shape);
                if (ws > 0.0) width1_[uj] = ws;
            }
            if (depth2_[uj] > yf) {
                double ws = getSlotWidth(depth2_[uj], yf, wm, shape);
                if (ws > 0.0) width2_[uj] = ws;
            }
            if (depth_mid_[uj] > yf) {
                double ws = getSlotWidth(depth_mid_[uj], yf, wm, shape);
                if (ws > 0.0) width_mid_[uj] = ws;
            }
        }
    }   // end STEP B

    // ---- STEP C: Flow classification + surface area (conduits only) ----
    // Matches legacy dwflow.c findSurfArea + getFlowClass.
    // Only links with offsets can trigger non-SUBCRITICAL classification,
    // so the expensive getYnorm/getYcrit Newton solves are rarely needed.
    // B2 threading: orphaned `omp for` on the persistent team. Per-conduit
    // writes (surf_area1/2, flow_class, fasnh_, depth_mid_, h1_/h2_,
    // width_mid_) are single-producer, and all cross-element reads
    // (nodes.depth/type, links.flow, width buffers) are read-only in this
    // phase — bit-exact at any thread count.
    #pragma omp for schedule(static)
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        // Phase A: timestep-invariant data is read from the conduit-dense
        // tile (sequential cache lines). Only nodes.depth, links.flow, and
        // nodes.type are still read from the sparse SoA — those are the
        // only quantities that vary across Picard iters in this branch.
        int j = tile_uj_[uci];
        auto uj = static_cast<std::size_t>(j);

        // Legacy parity + perf: skip bypassed conduits (see STEP A skip above).
        // surf_area1/2, flow_class, fasnh and any critical-depth overrides are
        // unchanged for a bypassed link, so the cached values are correct.
        if (bypassed_[uj]) continue;

        auto un1 = static_cast<std::size_t>(tile_n1_[uci]);
        auto un2 = static_cast<std::size_t>(tile_n2_[uci]);

        const double yf      = tile_y_full_[uci];
        const double z1_off  = tile_z1_off_[uci];
        const double z2_off  = tile_z2_off_[uci];
        const double beta_j  = tile_beta_[uci];
        const double qmax_j  = tile_q_max_[uci];
        const double inv1    = tile_inv1_elev_[uci];
        const double inv2    = tile_inv2_elev_[uci];
        double y1   = depth1_[uj];
        double y2   = depth2_[uj];
        double h1   = h1_[uj];
        double h2   = h2_[uj];
        double length = tile_length_[uci];

        double surfArea1 = 0.0, surfArea2 = 0.0;
        FlowClass fc = FlowClass::SUBCRITICAL;
        double fasnh = 1.0;

        // Both ends full → always SUBCRITICAL (skip expensive classification)
        // Cache yN/yC to avoid recomputation in the surface area switch below.
        // Also cache the per-link XSectParams on its first construction so the
        // UP_CRITICAL / DN_CRITICAL / UP_DRY / DN_DRY switch bodies reuse it
        // without a second buildXSP gather (item 3 tail).
        double cached_yN = 0.0, cached_yC = 0.0;
        (void)cached_yN;
        XSectParams cached_xs;
        bool xs_built = false;
        auto xs_ref = [&]() -> XSectParams& {
            if (!xs_built) { cached_xs = buildXSP(ctx, uj); xs_built = true; }
            return cached_xs;
        };

        // Phase C: branchless fast path for the trivial cases.
        // Pre-compute the four mutually exclusive depth states once and
        // dispatch on them with a single integer code instead of nested
        // if/else cascades. The compiler can keep these as flags in
        // registers across the whole STEP C body.
        const bool both_full = (y1 >= yf) & (y2 >= yf);
        const bool both_wet  = (y1 >  FUDGE) & (y2 >  FUDGE);
        const bool both_dry  = (y1 <= FUDGE) & (y2 <= FUDGE);
        const bool up_dry_only = (y1 <= FUDGE) & (y2 >  FUDGE);
        // dn_dry_only is the implicit remainder when none of the above match.

        if (both_full) {
            fc = FlowClass::SUBCRITICAL;
        } else if (both_dry) {
            fc = FlowClass::DRY;
        } else if (!tile_has_offset_[uci]) {
            // Phase C fast-path: no offsets → no Newton solver needed.
            // The legacy cascade for offset-free conduits collapses to:
            //   both_wet               → SUBCRITICAL (no critical depth check)
            //   up_dry_only & h2<z1    → UP_DRY     (z1_off=0 → z1_elev=inv1)
            //   up_dry_only & h2>=z1   → SUBCRITICAL  (else branch, no Newton)
            //   dn_dry_only & h1<z2    → DN_DRY     (z2_off=0 → z2_elev=inv2)
            //   dn_dry_only & h1>=z2   → SUBCRITICAL  (else branch)
            // Branchless via short-circuit comparisons; default fc is
            // already SUBCRITICAL.
            if (up_dry_only) {
                if (h2 < inv1) fc = FlowClass::UP_DRY;
            } else if (!both_wet) {           // dn_dry_only
                if (h1 < inv2) fc = FlowClass::DN_DRY;
            }
            // else both_wet → keep fc=SUBCRITICAL
        } else {
            // ---- Slow path: at least one end has an offset → may need Newton ----
            // (legacy dwflow.c getFlowClass lines 294-409)
            double z1_off_eff = z1_off;
            double z2_off_eff = z2_off;
            if (nodes.type[un1] == NodeType::OUTFALL)
                z1_off_eff = std::max(0.0, z1_off_eff - nodes.depth[un1]);
            if (nodes.type[un2] == NodeType::OUTFALL)
                z2_off_eff = std::max(0.0, z2_off_eff - nodes.depth[un2]);

            const double qLast = std::fabs(links.flow[uj]);
            const double q = qLast / tile_barrels_d_[uci];

            if (both_wet) {
                if (links.flow[uj] < 0.0) {
                    // Reverse flow: check upstream end
                    if (z1_off_eff > 0.0) {
                        XSectParams& xs = xs_ref();
                        cached_yN = computeYnorm(xs, beta_j, qmax_j, q);
                        cached_yC = xsect::getYcrit(xs, q);
                        double ycMin = std::min(cached_yN, cached_yC);
                        if (y1 < ycMin) fc = FlowClass::UP_CRITICAL;
                    }
                } else {
                    // Normal flow: check downstream end
                    if (z2_off_eff > 0.0) {
                        XSectParams& xs = xs_ref();
                        cached_yN = computeYnorm(xs, beta_j, qmax_j, q);
                        cached_yC = xsect::getYcrit(xs, q);
                        double ycMin = std::min(cached_yN, cached_yC);
                        double ycMax = std::max(cached_yN, cached_yC);
                        if (y2 < ycMin) {
                            fc = FlowClass::DN_CRITICAL;
                        } else if (y2 < ycMax) {
                            if (ycMax - ycMin < FUDGE) fasnh = 0.0;
                            else fasnh = (ycMax - y2) / (ycMax - ycMin);
                        }
                    }
                }
            } else if (up_dry_only) {
                const double z1_elev = inv1 + z1_off;
                if (h2 < z1_elev) {
                    fc = FlowClass::UP_DRY;
                } else if (z1_off_eff > 0.0) {
                    XSectParams& xs = xs_ref();
                    cached_yN = computeYnorm(xs, beta_j, qmax_j, q);
                    cached_yC = xsect::getYcrit(xs, q);
                    fc = FlowClass::UP_CRITICAL;
                }
            } else {  // dn_dry_only
                const double z2_elev = inv2 + z2_off;
                if (h1 < z2_elev) {
                    fc = FlowClass::DN_DRY;
                } else if (z2_off_eff > 0.0) {
                    XSectParams& xs = xs_ref();
                    cached_yN = computeYnorm(xs, beta_j, qmax_j, q);
                    cached_yC = xsect::getYcrit(xs, q);
                    fc = FlowClass::DN_CRITICAL;
                }
            }
        }

        // ---- Apply flow classification to surface area and depths ----
        // Reuse cached yN/yC from classification above to avoid recomputation.
        switch (fc) {
            case FlowClass::SUBCRITICAL: {
                // Surface-area contribution matches legacy findSurfArea (dwflow.c:464-472):
                //   surfArea1 = (w1 + wMid) * length / 4
                //   surfArea2 = (wMid + w2) * length / 4 * fasnh
                // For closed conduits in surcharge, w1/w2/wMid come from STEP B
                // with a CrownCutoff cap (not zero), matching legacy getWidth(),
                // so we must NOT skip this branch when y1/y2 >= yf — the capped
                // widths provide the correct (small, non-zero) contribution that
                // keeps the Picard denominator at end nodes bounded.
                double w1 = width1_[uj];
                double wM = width_mid_[uj];
                double w2 = width2_[uj];
                if (width_mid_[uj] > FUDGE) {
                    surfArea1 = (w1 + wM) * length / 4.0;
                    surfArea2 = (wM + w2) * length / 4.0 * fasnh;
                }
                break;
            }
            case FlowClass::UP_CRITICAL: {
                // Use cached yN/yC (already computed during classification)
                y1 = cached_yC;
                if (cached_yN < cached_yC) y1 = cached_yN;
                y1 = std::max(y1, FUDGE);
                const double z1_elev = inv1 + z1_off;
                h1 = z1_elev + y1;
                double yMid = std::max(0.5 * (y1 + y2), FUDGE);
                double w2 = width2_[uj];
                XSectParams& xs = xs_ref();
                double wM = xsect::getWofY(xs, yMid);
                // PARITY dwflow.c:521-523 + getWidth() (dwflow.c:632-641):
                // the surface-area midpoint width is getWidth(yMid) — the
                // Preissmann slot width when active, else capped at
                // CrownCutoff*yFull for closed sections. Only width_mid_
                // (Froude) keeps the raw getWofY value, matching legacy
                // link_getFroude's uncapped xsect_getWofY.
                double wMsa;
                double wSlotM = getSlotWidth(yMid, yf, tile_w_max_[uci],
                                             tile_shape_[uci]);
                if (wSlotM > 0.0)
                    wMsa = wSlotM;
                else if (yMid / yf >= getCrownCutoff() &&
                         !xsect::isOpen(tile_xsect_batch_shape_[uci]))
                    wMsa = xsect::getWofY(xs, getCrownCutoff() * yf);
                else
                    wMsa = wM;
                surfArea2 = (wMsa + w2) * length * 0.5;
                depth1_[uj] = y1;
                depth_mid_[uj] = yMid;
                width_mid_[uj] = wM;  // patch width for modified depth
                h1_[uj] = h1;
                break;
            }
            case FlowClass::DN_CRITICAL: {
                // Use cached yN/yC (already computed during classification)
                y2 = cached_yC;
                if (cached_yN < cached_yC) y2 = cached_yN;
                y2 = std::max(y2, FUDGE);
                const double z2_elev = inv2 + z2_off;
                h2 = z2_elev + y2;
                double yMid = std::max(0.5 * (y1 + y2), FUDGE);
                double w1 = width1_[uj];
                XSectParams& xs = xs_ref();
                double wM = xsect::getWofY(xs, yMid);
                // PARITY dwflow.c:531-535 + getWidth() (dwflow.c:632-641):
                // the surface-area midpoint width is getWidth(yMid) — the
                // Preissmann slot width when active, else capped at
                // CrownCutoff*yFull for closed sections. Only width_mid_
                // (Froude) keeps the raw getWofY value, matching legacy
                // link_getFroude's uncapped xsect_getWofY.
                double wMsa;
                double wSlotM = getSlotWidth(yMid, yf, tile_w_max_[uci],
                                             tile_shape_[uci]);
                if (wSlotM > 0.0)
                    wMsa = wSlotM;
                else if (yMid / yf >= getCrownCutoff() &&
                         !xsect::isOpen(tile_xsect_batch_shape_[uci]))
                    wMsa = xsect::getWofY(xs, getCrownCutoff() * yf);
                else
                    wMsa = wM;
                surfArea1 = (w1 + wMsa) * length * 0.5;
                depth2_[uj] = y2;
                depth_mid_[uj] = yMid;
                width_mid_[uj] = wM;  // patch width for modified depth
                h2_[uj] = h2;
                break;
            }
            case FlowClass::UP_DRY: {
                y1 = FUDGE;
                double yMid = std::max(0.5 * (FUDGE + y2), FUDGE);
                double w1 = width1_[uj];
                double w2 = width2_[uj];
                double wM = width_mid_[uj];  // use current width for surface area
                surfArea2 = (wM + w2) * length / 4.0;
                if (z1_off <= 0.0)
                    surfArea1 = (w1 + wM) * length / 4.0;
                depth1_[uj] = FUDGE;
                depth_mid_[uj] = yMid;
                // Patch width_mid for modified depth (used by momentum solver)
                { XSectParams& xs = xs_ref();
                  width_mid_[uj] = xsect::getWofY(xs, yMid); }
                break;
            }
            case FlowClass::DN_DRY: {
                y2 = FUDGE;
                double yMid = std::max(0.5 * (y1 + FUDGE), FUDGE);
                double w1 = width1_[uj];
                double w2 = width2_[uj];
                double wM = width_mid_[uj];  // use current width for surface area
                surfArea1 = (wM + w1) * length / 4.0;
                if (z2_off <= 0.0)
                    surfArea2 = (w2 + wM) * length / 4.0;
                depth2_[uj] = FUDGE;
                depth_mid_[uj] = yMid;
                // Patch width_mid for modified depth (used by momentum solver)
                { XSectParams& xs = xs_ref();
                  width_mid_[uj] = xsect::getWofY(xs, yMid); }
                break;
            }
            case FlowClass::DRY:
                surfArea1 = FUDGE * length / 2.0;
                surfArea2 = surfArea1;
                break;
            default:
                break;
        }

        surf_area1_[uj] = surfArea1;
        surf_area2_[uj] = surfArea2;
        fasnh_[uj] = fasnh;
        links.flow_class[uj] = fc;
    }

    // ---- STEP D: Batch compute areas and hyd-rad from (modified) depths ----
    // Fused from previous 4-pass (computeAreas d1, computeAreas d2,
    // computeAreaAndHydRad dm, computeHydRad d1) into 3 passes:
    //   d1 → (a1, hrad1)   via computeAreaAndHydRad (was 2 passes + slot)
    //   d2 → a2            via computeAreas
    //   dm → (am, hrad_mid) via computeAreaAndHydRad
    // Saves one gather of depth1_ plus kernel bookkeeping per Picard iter.
    double* OPENSWMM_RESTRICT p_d1  = depth1_.data();
    double* OPENSWMM_RESTRICT p_d2  = depth2_.data();
    double* OPENSWMM_RESTRICT p_dm  = depth_mid_.data();
    double* OPENSWMM_RESTRICT p_a1  = area1_.data();
    double* OPENSWMM_RESTRICT p_a2  = area2_.data();
    double* OPENSWMM_RESTRICT p_am  = area_mid_.data();
    double* OPENSWMM_RESTRICT p_hm  = hrad_mid_.data();
    double* OPENSWMM_RESTRICT p_h1  = hrad1_.data();
    double* OPENSWMM_RESTRICT p_wm  = width_mid_.data();
    (void)p_wm;  // width_mid_ already computed in STEP B

    // Team-split (B3) — same slice contract as STEP B (see
    // computeAreaHydRadTripleTeam): disjoint single-producer element writes,
    // bit-identical to the former serial `omp single` at any thread count.
    // STEP C's closing barrier ordered the (modified) depth inputs; the
    // explicit barrier below orders areas/hyd-radii for their cross-slice
    // consumers (STEP E / momentumKernels, which partition by conduit, not
    // by shape group).
    {
        const int d_tid = omp_get_thread_num();
        const int d_nth = omp_get_num_threads();
        groups_->computeAreaHydRadTripleTeam(p_d1, p_d2, p_dm,
                                             p_a1, p_a2, p_am,
                                             p_h1, p_hm, n_links_,
                                             d_tid, d_nth);
    }
    #pragma omp barrier
    // width_mid_ already computed in STEP B; UP/DN_CRITICAL/DRY cases
    // patched inline in STEP C.

    // ---- STEP E: Preissmann slot overrides (conduits only) ----
    //   Overrides area1/2/mid, width_mid, hrad_mid, hrad1 for surcharged
    //   conduits (depth > xsect_y_full). Applied AFTER the batch geometry
    //   kernels above so the slot overwrites whatever the shape kernel
    //   produced for above-full depth.
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        // Dynamic Preissmann Slot: area-based transient storage (Sharior et
        // al. 2023). Serial inside `omp single`: dps_ state accumulation is
        // kept off the team on purpose (not a hot path; not used by the
        // parity models).
        #pragma omp single
        {
        applyDPSGeometry(ctx);
        }
    }
    else if (surcharge_method == SurchargeMethod::TPA) {
        // TPA (issue #156): same serialization rationale as DPS — the
        // override is a cheap per-conduit pass and stays off the team.
        #pragma omp single
        {
        applyTpaGeometry(ctx);
        }
    }
    // Static slot (Sjoberg formula) / EXTRAN STEP E overrides are FUSED into
    // the per-conduit momentumKernels loop (own-element writes only; a
    // bypassed conduit's overrides are held — identical values, since its
    // depth inputs are held by the bypass mask above). See momentumKernels.
}

// ============================================================================
// recomputeConduitLossOne -- per-Picard-iteration evap/seepage (legacy parity)
//
// Mirrors legacy dwflow_findConduitFlow: link_getLossRate (link.c:1337) is
// called EVERY iteration for each conduit that is NOT DRY/UP_DRY/DN_DRY (those
// hit the dwflow.c:162 early return, so their stored loss rate is left
// unchanged). Uses depth = 0.5*(oldDepth + newDepth) at the current iterate and
// the FAITHFUL transect top width (xsect::getWofY via buildXSP), with the DW
// volume cap = newVolume/tstep and legacy's per-component clamp order
// (comp*q/total). The once-per-step Router::computeConduitLosses used the
// start-of-step depth with NO flow-class gate, which (a) leaked a spurious
// evap/seep loss onto dry/up-dry conduits (e.g. user2 TW01250) and (b) froze
// the rate for the whole step — both seeds amplified by surcharge.
//
// Per-element form: called from the fused momentumKernels team loop for one
// conduit at a time (own-element reads/writes only — parallel-safe). Callers
// must skip the call while losses_all_zero_ (dead-work latch, bit-exact:
// with zero evaporation and no conduit seepage anywhere the pass would write
// zeros over already-zero rates; the latch is updated once per timestep in
// execute()).
// ============================================================================

void DWSolver::recomputeConduitLossOne(SimulationContext& ctx, double dt,
                                       int ci) {
    auto& links = ctx.links;
    auto& CD = ctx.link_subtypes.conduits;
    const double evap = evap_rate;

    {
        const auto uci = static_cast<std::size_t>(ci);
        const int uj = conduit_idx_[uci];
        const auto u = static_cast<std::size_t>(uj);

        // Legacy dwflow.c:162 early return for DRY/UP_DRY/DN_DRY: link_getLossRate
        // is NOT called, so the previously-stored rate is retained. Skip (leave
        // CD.evap_loss_rate/seep_loss_rate[uci] untouched).
        const FlowClass fc = links.flow_class[u];
        if (fc == FlowClass::DRY || fc == FlowClass::UP_DRY ||
            fc == FlowClass::DN_DRY)
            return;

        // depth = 0.5*(oldDepth + newDepth) — current iterate (legacy link.c:1349)
        const double depth = 0.5 * (links.old_depth[u] + links.depth[u]);
        double evap_loss = 0.0;
        double seep_loss = 0.0;

        if (depth > FUDGE) {
            // Raw user length (legacy conduit_getLength), not modLength.
            double length = CD.length[uci];
            if (length <= 0.0) length = CD.mod_length[uci];
            const int shape = links.xsect_batch_shape[u];

            const bool wantEvap = xsect::isOpen(shape) && evap > 0.0;
            const bool wantSeep = CD.seep_rate[uci] > 0.0;
            if (wantEvap || wantSeep) {
                const XSectParams xs = buildXSP(ctx, u);  // faithful incl. transect
                if (wantEvap) {
                    const double topWidth = xsect::getWofY(xs, depth);
                    evap_loss = topWidth * length * evap;
                }
                if (wantSeep) {
                    double d_seep = depth;
                    if (shape != static_cast<int>(XSectShape::RECT_CLOSED) &&
                        d_seep >= xs.yw_max)
                        d_seep = xs.yw_max;
                    const double width =
                        (shape == static_cast<int>(XSectShape::RECT_CLOSED))
                            ? xs.w_max
                            : xsect::getWofY(xs, d_seep);
                    seep_loss = CD.seep_rate[uci] * width * length;
                    // Monthly conductivity adjustment (legacy link.c:1378:
                    // seepLossRate *= Adjust.hydconFactor). infil_factor
                    // mirrors adjust_hydcon[mon] each step (A2d) and is 1.0
                    // exactly on unadjusted decks.
                    seep_loss *= ctx.climate_state.infil_factor;
                }
            }

            // DW volume cap (legacy link.c:1389): q = newVolume/tstep; if the
            // total loss exceeds it, scale each component (comp*q/total order).
            double total = evap_loss + seep_loss;
            if (total > 0.0) {
                const double q = links.volume[u] / dt;
                if (total > q) {
                    evap_loss = evap_loss * q / total;
                    seep_loss = seep_loss * q / total;
                }
            }
        }

        CD.evap_loss_rate[uci] = evap_loss;
        CD.seep_loss_rate[uci] = seep_loss;
    }
}

// ============================================================================
// momentumKernels -- fused per-conduit Picard tail (formerly the separate
// solveMomentumBatch pre-init + classifyMomentumCategories + STEP E slot
// overrides + recomputeConduitLosses passes + kernel dispatch + flow commit).
//
// ONE orphaned `omp for` over conduits on the persistent team (legacy
// findLinkFlows forks per iteration instead, dynwave.c:370). Every sub-step
// reads and writes ONLY its own conduit's elements, so fusing them preserves
// the exact per-element operation order of the former pass-by-pass structure
// — bit-exact at any thread count, and fewer full-array traversals + team
// barriers per Picard iteration.
// ============================================================================

void DWSolver::momentumKernels(SimulationContext& ctx, double dt, int step) {
    auto& links = ctx.links;
    // Replicated per-thread constants — identical across the team.
    const bool static_slot_e =
        (surcharge_method != SurchargeMethod::DYNAMIC_SLOT);
    const bool do_losses = !losses_all_zero_;

    // Virtual-junction pair cache for this Picard iteration (shared sigma,
    // cross-junction upwind states, dq4j). Serial: pair count is tiny; the
    // single's implicit barrier orders the cache before the kernel loop.
    // No-op (no single, no barrier) for models without virtual junctions.
    if (!vjunc_.empty()) {
        #pragma omp single
        vjPrepareIteration(ctx, dt);
    }

    #pragma omp for schedule(static)
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        int j = tile_uj_[uci];
        auto uj = static_cast<std::size_t>(j);

        // Pre-init conduit flow: copy current flow (non-conduit flows are
        // handled by the non_conduit_fn callback).
        //
        // A BYPASSED conduit (both end nodes already converged) skips the
        // momentum solve below, so it must RETAIN its last-computed dqdh —
        // exactly as legacy does (it never clears Link[i].dqdh; findLinkFlows
        // still scatters the cached value via updateNodeFlows for every
        // conduit, bypassed or not). Zeroing it dropped each bypassed link's
        // dQ/dH from the node's sumdqdh, collapsing the surcharge
        // depth-update denominator to 0 and wrecking Picard convergence
        // (90% non-converging vs legacy's ~36%).
        new_flow_[uj] = links.flow[uj];

        // Per-iterate evap/seepage loss recompute (flow-class gated inside).
        // Runs for bypassed conduits too, keeping the former whole-array
        // pass's cadence bit-exactly (its inputs are all own-element).
        if (do_losses) recomputeConduitLossOne(ctx, dt, ci);

        if (bypassed_[uj]) continue;

        dqdh_[uj] = 0.0;

        // ---- STEP E: Preissmann slot overrides (static slot / EXTRAN) ----
        // Overrides own-element area1/2/mid, width_mid, hrad_mid, hrad1 for
        // surcharged depths (depth > y_full), AFTER the batch geometry
        // kernels (behind computeLinkGeometry's STEP D barrier) and BEFORE
        // classification/kernel below, preserving the former phase order.
        // Bypassed conduits keep their held overrides — identical values,
        // since their depth inputs are held by the bypass mask.
        // (DYNAMIC_SLOT applies its DPS overrides inside computeLinkGeometry.)
        if (static_slot_e) {
            double yf = tile_y_full_[uci];
            double af = tile_a_full_[uci];
            double rf = tile_r_full_[uci];
            double wm = tile_w_max_[uci];
            XsectShape shape = tile_shape_[uci];

            if (depth1_[uj] > yf) {
                double wSlot = getSlotWidth(depth1_[uj], yf, wm, shape);
                if (wSlot > 0.0) area1_[uj] = af + (depth1_[uj] - yf) * wSlot;
                // Upstream hyd-rad clamps to r_full once surcharged (legacy
                // behaviour: slot is narrow so wetted perimeter stays
                // ~constant).
                hrad1_[uj] = rf;
            }
            if (depth2_[uj] > yf) {
                double wSlot = getSlotWidth(depth2_[uj], yf, wm, shape);
                if (wSlot > 0.0) area2_[uj] = af + (depth2_[uj] - yf) * wSlot;
            }
            double yMid = depth_mid_[uj];
            if (yMid > yf) {
                double wSlot = getSlotWidth(yMid, yf, wm, shape);
                if (wSlot > 0.0) {
                    area_mid_[uj] = af + (yMid - yf) * wSlot;
                    width_mid_[uj] = wSlot;
                }
                hrad_mid_[uj] = rf;
            }
        }

        // ---- Momentum category classification (own-element inputs) ----
        FlowClass fc = links.flow_class[uj];
        double aMid = area_mid_[uj];
        double yf_c = tile_y_full_[uci];
        bool isFull = (depth1_[uj] >= yf_c && depth2_[uj] >= yf_c);

        MomentumCategory cat;
        if (fc == FlowClass::DRY || fc == FlowClass::UP_DRY ||
            fc == FlowClass::DN_DRY || aMid <= FUDGE || tile_is_closed_[uci]) {
            cat = MomentumCategory::SKIP_DRY;
        } else if (tile_is_force_main_[uci] && isFull) {
            // Legacy selects by the global FORCE_MAIN_EQUATION option
            // (forcmain.c ForceMainEqn), not by inspecting the coefficient.
            cat = (ctx.options.force_main_eqn == 1)
                ? MomentumCategory::FORCE_MAIN_DW
                : MomentumCategory::FORCE_MAIN_HW;
        } else if (!tile_is_open_[uci] && isFull) {
            cat = MomentumCategory::MANNING_CLOSED_FULL;
        } else if (tile_is_open_[uci]) {
            cat = MomentumCategory::MANNING_OPEN;
        } else {
            cat = MomentumCategory::MANNING_CLOSED_FS;
        }
        category_[uj] = cat;

        // ---- Momentum kernel dispatch ----
        switch (cat) {
            case MomentumCategory::SKIP_DRY:
                processDryLink(ctx, dt, uj);
                break;
            case MomentumCategory::MANNING_OPEN:
            case MomentumCategory::MANNING_CLOSED_FS:
            case MomentumCategory::MANNING_CLOSED_FULL:
                processManningLink(ctx, dt, step, uj, cat);
                break;
            case MomentumCategory::FORCE_MAIN_HW:
            case MomentumCategory::FORCE_MAIN_DW:
                processForceMainLink(ctx, dt, step, uj, cat);
                break;
            default:
                break;
        }

        // Commit the computed flow (former updateNodeFlows line 1). Own
        // element: no kernel reads another link's flow, and the bypassed
        // path above holds links.flow == new_flow_ from its last commit.
        links.flow[uj] = new_flow_[uj];
        // Publish the conduit dqdh alongside (legacy keeps Link.dqdh
        // current for every link). Nothing in the conduit path reads it —
        // the CSR gather uses dqdh_ directly — but the SWMM_TRACE_DUMP_STEP
        // element dump prints links.dqdh, which held 0 for conduits and
        // made per-element dqdh comparison against legacy impossible.
        links.dqdh[uj] = dqdh_[uj];
    }
}

// ============================================================================
// processDryLink -- trivial: zero flow, minimal bookkeeping (per-element)
// ============================================================================

void DWSolver::processDryLink(SimulationContext& ctx, double dt,
                              std::size_t uj) {
    (void)dt;  // unused — kept for signature uniformity across kernels
    auto& links = ctx.links;
    const double dt_g = dt_gravity_;

    auto uci = static_cast<std::size_t>(tile_uj_to_ci_[uj]);
    double aMid = area_mid_[uj];
    double barrels_d = tile_barrels_d_[uci];
    area_mid_[uj] = 0.5 * (area1_[uj] + area2_[uj]);
    // PARITY dwflow.c:171: dry-link dqdh = GRAVITY*dt*aMid / length * barrels.
    // Divide by the cached (mod)length directly — x/L != x*(1/L) in IEEE-754,
    // and this dqdh is scattered into the node sumdqdh denominator, so a 1-ULP
    // error here (e.g. high-offset dry conduit TW01221) shifts the surcharge
    // head solve and gets amplified by the sign-flip clamp. (dt_g == dt*GRAVITY
    // matches legacy GRAVITY*dt by commutativity.)
    dqdh_[uj] = dt_g * aMid / tile_length_[uci] * barrels_d;
    froude_[uj] = 0.0;
    new_flow_[uj] = 0.0;
    double yf = tile_y_full_[uci];
    links.depth[uj] = std::min(depth_mid_[uj], yf);
    // Volume uses RAW user-input length to match legacy. Legacy
    // `dwflow.c:174` computes `Link[j].newVolume = aMid * link_getLength(j)
    // * barrels`, where `link_getLength` returns `Conduit[k].length` for
    // non-IRREGULAR conduits (`link.c:1211`). `Conduit[k].length` is set
    // once at parse-time (`link.c:346`) and never reassigned — it is the
    // raw input length, NOT the routing-lengthened `Conduit[k].modLength`.
    // The lengthened length is used only in the momentum equation
    // (dq2/dq4/dq5/dqdh) per `dwflow.c:133`.
    links.volume[uj] = area_mid_[uj] * tile_links_length_[uci] * barrels_d;
}

// ============================================================================
// applyFlowLimits -- shared post-processing (inlet, normal flow, relaxation,
//                    flap gates, dry node checks, depth/volume update)
// ============================================================================

void DWSolver::applyFlowLimits(SimulationContext& ctx, double dt, int step,
                               std::size_t uj, double& q, double qLast,
                               double barrels_d, bool isFull) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    auto& CD = ctx.link_subtypes.conduits;  // Phase 6 Stage B: inlet_control / normal_flow_limited (row==uci)
    const int normal_flow_ltd = ctx.options.normal_flow_ltd;
    // Phase A extension: read invariants from the conduit-dense tile.
    auto uci = static_cast<std::size_t>(tile_uj_to_ci_[uj]);
    double yf = tile_y_full_[uci];
    double h1 = h1_[uj];
    int n1 = tile_n1_[uci];
    int n2 = tile_n2_[uci];
    auto un1 = static_cast<std::size_t>(n1);
    auto un2 = static_cast<std::size_t>(n2);

    // Inlet control and normal flow limiting
    CD.inlet_control[uci] = uint8_t{0};
    CD.normal_flow_limited[uci] = uint8_t{0};
    if (q > 0.0) {
        if (tile_culvert_code_[uci] > 0 && !isFull) {
            double dqdh_culv = 0.0;
            double q_inlet = culvert::getInflow(
                q, h1, yf, tile_a_full_[uci],
                tile_slope_[uci], tile_culvert_code_[uci], dqdh_culv);
            if (q_inlet < q) {
                q = q_inlet;
                CD.inlet_control[uci] = uint8_t{1};
            }
        } else {
            FlowClass fc2 = links.flow_class[uj];
            int nfl = normal_flow_ltd;
            if (nfl != 3 && depth1_[uj] < yf &&
                (fc2 == FlowClass::SUBCRITICAL || fc2 == FlowClass::SUPERCRITICAL)) {
                bool hasOutfall = false;
                if (n1 >= 0 && n2 >= 0) {
                    hasOutfall = (nodes.type[un1] == NodeType::OUTFALL ||
                                  nodes.type[un2] == NodeType::OUTFALL);
                }
                bool slope_check = (nfl == 0 || nfl == 2 || hasOutfall) &&
                                   (depth1_[uj] < depth2_[uj]);
                bool froude_check = false;
                if (!slope_check && (nfl == 1 || nfl == 2) && !hasOutfall) {
                    if (depth1_[uj] > FUDGE && depth2_[uj] > FUDGE) {
                        // PARITY dwflow.c checkNormalFlow → link_getFroude
                        // (link.c:860-873): the hydraulic depth uses the RAW
                        // top width xsect_getWofY(y1) — NOT the STEP-B
                        // CrownCutoff-capped width1_ (which overstates Froude
                        // for a nearly-full pipe and spuriously fires the
                        // normal-flow limit) — and a CLOSED conduit within
                        // FUDGE of full has Froude 0 (no limit check).
                        const double y1d = depth1_[uj];
                        const bool closed_nearfull =
                            !tile_is_open_[uci] && (yf - y1d <= FUDGE);
                        if (!closed_nearfull) {
                            double v1 = q / area1_[uj];
                            XSectParams xs = buildXSP(ctx, uj);
                            double w1 = xsect::getWofY(xs, y1d);
                            // Legacy divides A/W with no zero guard; y1 < yf
                            // and not near-full keeps W finite here.
                            double dh1 = area1_[uj] / w1;
                            double f1 = std::fabs(v1) /
                                        std::sqrt(constants::GRAVITY * dh1);
                            froude_check = (f1 >= 1.0);
                        }
                    }
                }
                if (slope_check || froude_check) {
                    // PARITY dwflow.c:675: qNorm = beta*a1*pow(r1,2./3.) — (beta*a1)
                    // grouped first, libm std::pow (NOT cbrt(x*x)), raw upstream
                    // hyd radius (no FUDGE clamp). §6 fast-mode: cbrt(x*x) (opt-in).
#ifdef SWMM_FAST_MANNING_POW
                    double qNorm = tile_beta_[uci] * area1_[uj]
                                 * fastmath::pow2_3(hrad1_[uj]);
#else
                    double qNorm = tile_beta_[uci] * area1_[uj]
                                 * std::pow(hrad1_[uj], 2.0 / 3.0);
#endif
                    if (qNorm < q) {
                        q = qNorm;
                        CD.normal_flow_limited[uci] = uint8_t{1};
                    }
                }
            }
        }
    }

    // Under-relaxation (after first iteration)
    if (step > 0) {
        q = (1.0 - omega) * qLast + omega * q;
        if (q * qLast < 0.0) q = 0.001 * ((q > 0.0) ? 1.0 : -1.0);
    }

    // Flow limit
    if (tile_q_limit_[uci] > 0.0) {
        if (std::fabs(q) > tile_q_limit_[uci])
            q = ((q > 0.0) ? 1.0 : -1.0) * tile_q_limit_[uci];
    }

    // Flap gate checks
    if (tile_has_flap_gate_[uci]) {
        if (q * static_cast<double>(tile_direction_[uci]) < 0.0) q = 0.0;
    }
    if (q < 0.0 && n2 >= 0 && nodes.type[un2] == NodeType::OUTFALL) {
        const int ofr = ctx.node_subtypes.outfall_row(n2);
        if (ofr >= 0 && ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(ofr)])
            q = 0.0;
    }
    if (q > 0.0 && n1 >= 0 && nodes.type[un1] == NodeType::OUTFALL) {
        const int ofr = ctx.node_subtypes.outfall_row(n1);
        if (ofr >= 0 && ctx.node_subtypes.outfalls.has_flap_gate[static_cast<std::size_t>(ofr)])
            q = 0.0;
    }

    // Dry node check
    if (q >  FUDGE && nodes.depth[un1] <= FUDGE) q =  FUDGE;
    if (q < -FUDGE && nodes.depth[un2] <= FUDGE) q = -FUDGE;

    // Save new flow
    new_flow_[uj] = q * barrels_d;

    // Update link depth and volume
    links.depth[uj] = std::min(depth_mid_[uj], yf);
    double aMidAvg = (area1_[uj] + area2_[uj]) * 0.5;
    // Volume uses RAW user-input length (matching legacy `dwflow.c:288`:
    // `Link[j].newVolume = aMid * link_getLength(j) * barrels`).
    // `link_getLength` returns the raw `Conduit[k].length`, not the
    // routing-lengthened `Conduit[k].modLength`. See processDryLink note.
    links.volume[uj] = aMidAvg * tile_links_length_[uci] * barrels_d;
}

// ============================================================================
// processManningLink -- per-element kernel for MANNING_OPEN,
//                       MANNING_CLOSED_FS, and MANNING_CLOSED_FULL
// ============================================================================

void DWSolver::processManningLink(SimulationContext& ctx, double dt, int step,
                                  std::size_t uj, MomentumCategory cat) {
    auto& links = ctx.links;
    const auto& CD = ctx.link_subtypes.conduits;  // Phase 6 Stage B: per-step evap/seep
    const int inert_damping = ctx.options.inertial_damping;
    const double dt_g = dt_gravity_;
    const bool is_closed_full = (cat == MomentumCategory::MANNING_CLOSED_FULL);
    const bool is_open_cat    = (cat == MomentumCategory::MANNING_OPEN);

    // Phase A extension: read invariants from the conduit-dense tile.
    auto uci = static_cast<std::size_t>(tile_uj_to_ci_[uj]);
    double barrels_d = tile_barrels_d_[uci];
    double length = tile_length_[uci];
    double inv_len = tile_inv_length_[uci];
    double aMid = area_mid_[uj];
    double rMid = hrad_mid_[uj];
    double qLast = links.flow[uj] / barrels_d;
    double yf = tile_y_full_[uci];
    bool isFull = (depth1_[uj] >= yf && depth2_[uj] >= yf);

    // Conveyance areas for the momentum equation — Preissmann slot excluded
    // (issue #144, see conveyArea). aMid/area1_/area2_ keep the slot for
    // storage and node continuity; only the momentum terms below are clamped.
    const double aFull   = tile_a_full_[uci];
    const double a1Conv  = conveyArea(area1_[uj], aFull);
    const double a2Conv  = conveyArea(area2_[uj], aFull);
    const double aMidConv = conveyArea(aMid, aFull);

    // Velocity (clamped)
    double v = qLast / aMidConv;
    double absv = std::fabs(v);
    if (absv > MAX_VELOCITY) {
        v = (v > 0.0) ? MAX_VELOCITY : -MAX_VELOCITY;
        absv = MAX_VELOCITY;
    }
    velocity_[uj] = v;

    // Froude number — skip entirely for MANNING_CLOSED_FULL (fr=0, sig=0)
    double fr = 0.0;
    double sig = 0.0;
    if (!is_closed_full) {
        double wMid = width_mid_[uj];
        // PARITY: legacy link_getFroude (link.c) always uses the UNCAPPED top
        // width xsect_getWofY(yMid). STEP B stored a CrownCutoff-CAPPED width in
        // width_mid_ (correct for surface area). The flow-class branches
        // (UP/DN_CRITICAL, UP/DN_DRY) already re-patched width_mid_ to the
        // uncapped getWofY(depth_mid_) at their modified depths, so the ONLY
        // residual capped case is SUBCRITICAL with the mid-depth above the crown
        // cutoff. Recompute the uncapped width there to match legacy exactly;
        // every other path leaves width_mid_ untouched (bit-identical).
        if (links.flow_class[uj] == FlowClass::SUBCRITICAL &&
            wcap_dm_[uj] < depth_mid_[uj]) {
            XSectParams xs = buildXSP(ctx, uj);
            wMid = xsect::getWofY(xs, depth_mid_[uj]);
        }
        // PARITY: legacy link_getFroude (link.c:864-873) zeros Froude ONLY for a
        // CLOSED conduit within FUDGE of full (yFull - yMid <= FUDGE); for an OPEN
        // conduit it computes a real Froude even when full (NO isFull short-
        // circuit). Use that exact per-shape gate, not the both-ends-full isFull
        // test — otherwise open/IRREGULAR channels that fill (user2/5) get fr=0
        // (and sigma=1) and closed pipes straddling the crown (user3) get a
        // spurious nonzero fr, both seeding surcharge divergence.
        const bool closed_nearfull =
            !tile_is_open_[uci] && (yf - depth_mid_[uj] <= FUDGE);
        if (depth_mid_[uj] > FUDGE && !closed_nearfull) {
            // #144: aMid, not aMidConv, is deliberate and equivalent here.
            // The slot only inflates area once depth_mid > y_full, and that
            // case is unreachable in this branch: a closed conduit above its
            // crown trips closed_nearfull above (fr = 0), and an open shape
            // gets slot width 0 from getSlotWidth() so STEP E never overrides
            // it. Keeping aMid preserves the legacy hydraulic-depth grouping.
            double dh = (wMid > FUDGE) ? aMid / wMid : 0.0;
            // PARITY: legacy link_getFroude computes sqrt(GRAVITY * y) directly
            // (link.c). Using the precomputed SQRT_GRAVITY constant (a truncated
            // sqrt(32.2)) times sqrt(dh) differs by ~3e-9 and reorders the FP
            // ops; that shifts Froude across the sigma (0.5/1.0) and SUB/SUPER
            // critical knife-edges, amplifying into macroscopic transients.
            fr = (dh > 0.0) ? absv / std::sqrt(constants::GRAVITY * dh) : 0.0;
        }

        // Reclassify SUBCRITICAL → SUPERCRITICAL
        FlowClass fc = links.flow_class[uj];
        if (fc == FlowClass::SUBCRITICAL && fr > 1.0)
            links.flow_class[uj] = FlowClass::SUPERCRITICAL;

        // Branchless sigma
        sig = std::max(0.0, std::min(1.0, 2.0 * (1.0 - fr)));
    }
    froude_[uj] = fr;

    // Head values
    double h1 = h1_[uj];
    double h2 = h2_[uj];

    // Upstream weighting (use Froude-based sigma for rho BEFORE override)
    double r1_val = hrad1_[uj];
    double rho = 1.0;
    if (!is_closed_full && !isFull && qLast > 0.0 && h1 >= h2)
        rho = sig;
    double aWtd = a1Conv + (aMidConv - a1Conv) * rho;
    // PARITY dwflow.c:198: legacy uses the raw interpolated rWtd in pow(rWtd,
    // 1.33333) — no clamp. The DRY / aMid<=FUDGE branch already returned, and
    // r1_val/rMid > 0 for any wet section, so rWtd > 0 here (no div-by-zero).
    double rWtd = r1_val + (rMid - r1_val) * rho;

    // ---- Virtual-junction pair coupling (plan §3.2, mechanisms 3–4) ----
    // Chain-aware: this link may sit on TWO pairs (its node1's and its
    // node2's). Mechanism 3 — when coherent through-flow ENTERS this link
    // across its upstream (node1) interface, the junction-end upstream
    // weighting uses the neighbour link's mid-state as the upwind state
    // (rho keeps the per-link value; reversed flow stays central, matching
    // the per-link rho convention). Mechanism 4 (FULL mode) — each active
    // interface contributes its convective flux correction to BOTH adjacent
    // links; dq4j is direction-gated in vjPrepareIteration (zero when the
    // through-flow is incoherent).
    // Mechanism 2 (blanket σ_j override) is REMOVED: the SWASHES bump probes
    // showed it destabilizes frictionless chains (l1 76% alone vs 0.2% off);
    // EXTRAN's per-link Froude sigma already carries the damping policy.
    // Diagnostic toggles (SWMM_VJ_M3/M4=0 disables; default on).
    static const bool vj_m3 = [] {
        const char* s = std::getenv("SWMM_VJ_M3"); return !(s && *s == '0'); }();
    static const bool vj_m4 = [] {
        const char* s = std::getenv("SWMM_VJ_M4"); return !(s && *s == '0'); }();
    double vj_dq4 = 0.0;
    if (!vjunc_.empty()) {
        const int r1 = vj_pair_n1_[uj];
        const int r2 = vj_pair_n2_[uj];
        if (r1 >= 0) {
            const VJuncPair& p = vjunc_[static_cast<std::size_t>(r1)];
            if (p.active) {
                if (vj_m3 && p.dirn > 0 && static_cast<int>(uj) == p.dn_link &&
                    !is_closed_full && !isFull && qLast > 0.0) {
                    // p.a_up_mid is already a conveyance area (#144).
                    aWtd = p.a_up_mid + (aMidConv - p.a_up_mid) * rho;
                    rWtd = p.r_up_mid + (rMid - p.r_up_mid) * rho;
                }
                if (vj_m4) vj_dq4 += p.dq4j;
            }
        }
        // NOTE (2026-08-14): each interface's dq4j spans midpoint-to-midpoint
        // (Λ = ½(L_a+L_b)) and is added HERE as well as at r1 above, so an
        // interior link picks up two of them on top of its own full-length
        // dq4. Attributing each interface to a single link instead was
        // measured and is far worse (l1 86-103 % vs 4-9 %), so the
        // duplication is not the primary defect — the sign inversion noted at
        // vjPrepareIteration is. See plans/VJ_MOMENTUM_SCOPE.md Phase 3.
        if (r2 >= 0) {
            const VJuncPair& p = vjunc_[static_cast<std::size_t>(r2)];
            if (p.active && vj_m4) vj_dq4 += p.dq4j;
        }
    }

    // Apply InertDamping override AFTER rho computation
    if (!is_closed_full) {
        if      (inert_damping == 0) sig = 1.0;  // NO_DAMPING
        else if (inert_damping == 2) sig = 0.0;  // FULL_DAMPING
        // MANNING_CLOSED_FS: full damping when surcharged closed conduit
        if (!is_open_cat && isFull) sig = 0.0;
    }
    sigma_[uj] = sig;

    // Manning friction — rWtd > 0 (wet section; DRY branch already returned), so
    // r43 > 0 always. The legacy (dwflow.c:211) applies this unconditionally on
    // the raw rWtd; the damping from large dq1 (small rWtd) is correct physics.
    // PARITY: legacy uses the truncated literal exponent pow(rWtd, 1.33333),
    // NOT the exact 4/3. The two differ by ~3.3e-6 in the exponent, which is a
    // ~2e-6..1e-5 relative error in r^exp (largest for nearly-dry conduits with
    // small rWtd) — the dominant timestep-by-timestep divergence from legacy.
    // Match the legacy literal exactly (dwflow.c:211) for bit-level parity.
    //
    // §6 fast-mode (opt-in, NOT bit-exact): std::pow(x,1.33333) via
    // exp(1.33333*log(x)) is the single hottest transcendental in DW routing
    // (~10% of compute in profiling). fastmath::pow4_3(x)=x*cbrt(x) computes the
    // exact 4/3 power with a ~10-15 cycle cbrt instead of ~60-80 cycle pow. It
    // differs from legacy by the exponent truncation (~1e-5 rel) — within the
    // fast-mode tolerance gate. Default OFF keeps the legacy std::pow.
#ifdef SWMM_FAST_MANNING_POW
    double r43 = fastmath::pow4_3(rWtd);
#else
    // EXACT legacy literal (dwflow.c:211): 1.33333, six digits — NOT
    // 1.33333333333 and NOT 4.0/3.0. The exponent value itself is part of
    // the parity contract; a longer literal shifts r^exp by ~1e-9 relative
    // and seeds macroscopic divergence within a few routing steps.
    double r43 = std::pow(rWtd, 1.33333);
#endif
    double dq1 = dt * tile_rough_factor_[uci] / r43 * absv;

    // Head gradient. PARITY dwflow.c:214: divide by length directly
    // (x/L != x*(1/L) in IEEE-754); dt_g == dt*GRAVITY matches legacy grouping.
    double dq2 = dt_g * aWtd * (h2 - h1) / length;

    // Unsteady + convective acceleration (skip if sig==0)
    // #144: area_old_ is a copy of the slot-inclusive area_mid_ taken at the
    // start of the step, so it must be clamped alongside aMid — otherwise the
    // dq3 difference mixes a conveyance area with a slot area and manufactures
    // a spurious inertial impulse on any step that crosses the crown.
    double aOld = std::max(conveyArea(area_old_[uj], aFull), FUDGE);
    double dq3 = 0.0, dq4 = 0.0;
    if (sig > 0.0) {
        dq3 = 2.0 * v * (aMidConv - aOld) * sig;
        if (length > 0.0)
            dq4 = dt * v * v * (a2Conv - a1Conv) / length * sig;  // PARITY dwflow.c:222
        // Cross-junction convective correction for virtual-junction pairs
        // (0 unless VIRTUAL_JUNCTION_MOMENTUM FULL and the pair is active);
        // gated on sig so the inertial-damping overrides silence it too.
        dq4 += vj_dq4;
    }

    // Local losses
    double dq5 = 0.0;
    if (tile_has_losses_[uci]) {
        double absq = std::fabs(qLast);
        double losses = 0.0;
        // #144: local losses are velocity heads — conveyance areas.
        if (a1Conv > FUDGE) losses += tile_loss_inlet_[uci] * (absq / a1Conv);
        if (a2Conv > FUDGE) losses += tile_loss_outlet_[uci] * (absq / a2Conv);
        if (aMidConv > FUDGE) losses += tile_loss_avg_[uci] * (absq / aMidConv);
        dq5 = losses / 2.0 / length * dt;  // PARITY dwflow.c:229
    }

    // Evaporation/seepage. Length divisor uses RAW length (matching legacy
    // `dwflow.c:233`: `dq6 = ... / link_getLength(j)`, where
    // `link_getLength` returns `Conduit[k].length` (raw, not modLength).
    double dq6 = 0.0;
    {
        double conduit_length = tile_links_length_[uci];
        double loss_rate = CD.evap_loss_rate[uci] + CD.seep_loss_rate[uci];  // row==uci
        if (loss_rate > 0.0 && conduit_length > 0.0)
            dq6 = loss_rate * 2.5 * dt * v / conduit_length;
    }

    // Flow update
    double qOld = links.old_flow[uj] / barrels_d;
    double denom = 1.0 + dq1 + dq5;

    // Unsteady friction (issue #156 Phase 3). S_fu = (k3/g)(∂V/∂t +
    // c·sgn(V)|∂V/∂x|), Pinto et al. (2025). The local-acceleration half is
    // folded into the SAME denominator the dqdh Jacobian is built from
    // (q·(denom + k3) = ... + k3·aWtd·vOld), so the surcharge node iteration
    // stays consistent by construction; the convective half is explicit from
    // the end-velocity difference (same |Q| through both ends, conveyance
    // areas), clamped to half the incoming momentum. Celerity is
    // regime-consistent through width_mid_: the slot width when surcharged
    // under SLOT/DYNAMIC_SLOT (acoustic), the free-surface width otherwise,
    // and the near-crown width under EXTRAN (plan §3.2 O-1 default). The
    // 0.01 ft/s dead-band mirrors kernels::ufUpdate (added-inertia noise
    // amplification, measured on the FV side). When inactive, the original
    // expression is evaluated verbatim so the default path stays bit-exact.
    bool   uf_applied = false;
    double uf_num = 0.0;
    if (unsteady_friction != 0 && uf_k3 > 0.0) {
        constexpr double kUfVelFloor = 0.01;  // ft/s, mirrors FvKernels
        const double vOld = (aOld > FUDGE) ? qOld / aOld : 0.0;
        if (std::fabs(vOld) >= kUfVelFloor || absv >= kUfVelFloor) {
            const double wMid = width_mid_[uj];
            const double c_wave = (wMid > FUDGE && aMid > FUDGE)
                ? std::sqrt(GRAVITY * aMid / wMid) : 0.0;
            // ∂V/∂x: cross-link stencil first (a full link has equal end
            // areas, so the within-link estimator is structurally zero in
            // exactly the pressurized cases UF exists for — measured
            // anti-damping without this). Neighbor previous-iterate
            // velocities, sign-mapped into this link's frame; falls back to
            // the within-link end-velocity difference when no simple
            // degree-2 conduit neighbor exists on either side.
            double dv_dx = 0.0;
            {
                auto nb_vel = [&](int nbj, int8_t sg) -> double {
                    const auto unb = static_cast<std::size_t>(nbj);
                    const auto uci_nb =
                        static_cast<std::size_t>(tile_uj_to_ci_[unb]);
                    const double a_nb = std::max(
                        conveyArea(area_mid_[unb], tile_a_full_[uci_nb]),
                        FUDGE);
                    return static_cast<double>(sg) *
                           (links.flow[unb] / tile_barrels_d_[uci_nb]) / a_nb;
                };
                double vL = v, vR = v, dsum = 0.0;
                if (uf_nb_up_[uj] >= 0) {
                    const auto uci_nb = static_cast<std::size_t>(
                        tile_uj_to_ci_[static_cast<std::size_t>(uf_nb_up_[uj])]);
                    vL = nb_vel(uf_nb_up_[uj], uf_sg_up_[uj]);
                    dsum += 0.5 * (length + tile_length_[uci_nb]);
                }
                if (uf_nb_dn_[uj] >= 0) {
                    const auto uci_nb = static_cast<std::size_t>(
                        tile_uj_to_ci_[static_cast<std::size_t>(uf_nb_dn_[uj])]);
                    vR = nb_vel(uf_nb_dn_[uj], uf_sg_dn_[uj]);
                    dsum += 0.5 * (length + tile_length_[uci_nb]);
                }
                if (dsum > 0.0) {
                    dv_dx = std::fabs(vR - vL) / dsum;
                } else if (length > 0.0) {
                    const double absq = std::fabs(qLast);
                    const double v1e = (a1Conv > FUDGE) ? absq / a1Conv : 0.0;
                    const double v2e = (a2Conv > FUDGE) ? absq / a2Conv : 0.0;
                    dv_dx = std::fabs(v2e - v1e) / length;
                }
            }
            const double sgn = (v > 0.0) ? 1.0 : ((v < 0.0) ? -1.0 : 0.0);
            double dq_grad = dt * uf_k3 * aWtd * c_wave * sgn * dv_dx;
            const double cap = 0.5 * std::fabs(qOld);
            if (std::fabs(dq_grad) > cap)
                dq_grad = (dq_grad > 0.0) ? cap : -cap;
            denom += uf_k3;
            uf_num = uf_k3 * aWtd * vOld - dq_grad;
            uf_applied = true;
        }
    }
    double q = uf_applied
        ? (qOld - dq2 + dq3 + dq4 + dq6 + uf_num) / denom
        : (qOld - dq2 + dq3 + dq4 + dq6) / denom;
    // PARITY dwflow.c:240: legacy groups ((1/denom)*GRAVITY)*dt and divides by
    // length directly (NOT dt_g=dt*GRAVITY). dqdh feeds the surcharge node-depth
    // Jacobian (sumdqdh denominator), so the grouping/divide must match exactly.
    dqdh_[uj] = 1.0 / denom * GRAVITY * dt * aWtd / length * barrels_d;

    // A3 parity term tracing for one link (SWMM_TRACE_LINK=<index>, first 64
    // invocations; format-matched to the legacy trace in dwflow.c).
    {
        static FILE* lf = nullptr;
        static long  lf_target = -2;
        static long  lf_skip = 0;
        static long  lf_step = 0;
        static int   lf_count = 0;
        static int   lf_rows = 0;
        if (lf_target == -2) {
            const char* p  = std::getenv("SWMM_TRACE_LINK");
            const char* tr = std::getenv("SWMM_TRACE_RSTEP");
            const char* sk = std::getenv("SWMM_TRACE_SKIP");
            const char* ls = std::getenv("SWMM_TRACE_LSTEP");
            lf_target = -1;
            if (sk && *sk) lf_skip = std::atol(sk);
            if (ls && *ls) lf_step = std::atol(ls);
            if (p && *p && tr && *tr) {
                char fname[512];
                lf_target = std::atol(p);
                std::snprintf(fname, sizeof(fname), "%s.link%ld", tr, lf_target);
                lf = std::fopen(fname, "w");
                if (lf) std::fprintf(lf,
                    "n,qLast,v,sigma,rho,aWtd,rWtd,dq1,dq2,dq3,dq4,dq5,dq6,qOld,q,sa1,sa2,fc,y1,yMid,a1,aMid,r1,rMid,aMidConv,dqdh\n");
            }
        }
        if (lf && static_cast<long>(uj) == lf_target) {
            ++lf_count;
            // SWMM_TRACE_LSTEP=N: capture while computing routing step >= N
            // (the RSTEP serial increments at the END of each step, so during
            // step N the serial still reads N-1). Otherwise use the
            // invocation-count window (SWMM_TRACE_SKIP).
            const bool in_window = lf_step > 0
                ? (openswmm::g_trace_rstep_sn + 1 >= lf_step)
                : (lf_count > lf_skip);
            if (in_window && lf_rows < 128) {
                ++lf_rows;
                std::fprintf(lf, "%d,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%a,%d,%a,%a,%a,%a,%a,%a,%a,%a\n",
                             lf_count, qLast, v, sig, rho, aWtd, rWtd,
                             dq1, dq2, dq3, dq4, dq5, dq6, qOld, q,
                             surf_area1_[uj], surf_area2_[uj],
                             static_cast<int>(links.flow_class[uj]),
                             depth1_[uj], depth_mid_[uj], area1_[uj],
                             area_mid_[uj], hrad1_[uj], hrad_mid_[uj],
                             aMidConv, dqdh_[uj]);
                if (lf_rows >= 128) { std::fclose(lf); lf = nullptr; }
            }
        }
    }

    // Shared post-processing
    applyFlowLimits(ctx, dt, step, uj, q, qLast, barrels_d, isFull);
}

// ============================================================================
// processForceMainLink -- per-element kernel for FORCE_MAIN_HW/FORCE_MAIN_DW
// ============================================================================

void DWSolver::processForceMainLink(SimulationContext& ctx, double dt, int step,
                                    std::size_t uj, MomentumCategory cat) {
    auto& links = ctx.links;
    const auto& CD = ctx.link_subtypes.conduits;  // Phase 6 Stage B: per-step evap/seep
    const double dt_g = dt_gravity_;
    const bool is_dw = (cat == MomentumCategory::FORCE_MAIN_DW);

    // Phase A extension: read invariants from the conduit-dense tile.
    auto uci = static_cast<std::size_t>(tile_uj_to_ci_[uj]);
    double barrels_d = tile_barrels_d_[uci];
    double aMid = area_mid_[uj];
    double rMid = hrad_mid_[uj];
    double qLast = links.flow[uj] / barrels_d;

    // Conveyance areas — Preissmann slot excluded (issue #144). A force main
    // is full for the whole simulation, so under SLOT the slot is always
    // active here; this is the systematically affected case.
    const double aFull    = tile_a_full_[uci];
    const double a1Conv   = conveyArea(area1_[uj], aFull);
    const double a2Conv   = conveyArea(area2_[uj], aFull);
    const double aMidConv = conveyArea(aMid, aFull);

    // Force main is always full (that's the classification condition)
    bool isFull = true;

    // Velocity (clamped)
    double v = qLast / aMidConv;
    double absv = std::fabs(v);
    if (absv > MAX_VELOCITY) {
        v = (v > 0.0) ? MAX_VELOCITY : -MAX_VELOCITY;
        absv = MAX_VELOCITY;
    }
    velocity_[uj] = v;

    // Force main when full: fr=0, sig=0 (no inertial terms)
    froude_[uj] = 0.0;
    sigma_[uj] = 0.0;

    // Head values
    double h1 = h1_[uj];
    double h2 = h2_[uj];

    // No upstream weighting when full (rho=1). The a1Conv +/- form is kept
    // rather than collapsing to aMidConv: the two differ in the last ULP.
    double aWtd = a1Conv + (aMidConv - a1Conv);  // = aMidConv
    double rWtd = std::max(rMid, FUDGE);
    (void)rWtd;  // reserved for future friction variants

    // Force main friction — legacy dwflow.c:236-237 via forcmain.c
    // getFricSlope, using the PRECOMPUTED sBot rough factor
    // (link.c:1127-1131: gravity and the artificial-lengthening
    // compensation lf^0.54 are baked in; PostParseResolver stores it in
    // xsect_s_bot). The kernel previously recomputed a textbook Sf from
    // the raw coefficient (velocity exponent 1/0.54 vs legacy's literal
    // 1.852, R exponent 0.63/0.54 vs 1.1667, no lengthFactor) — a
    // systematic ~1e-4 relative friction bias that flips knife-edge
    // surcharge onsets (PS4D on 800-node-sewer at t = 4.55 h).
    const double sbot = tile_fm_sbot_[uci];
    double dq1;
    // PARITY dwflow.c:237: dq1 = dt * forcemain_getFricSlope(...) — the
    // slope expression is evaluated FIRST and multiplied by dt LAST.
    // Regrouping (dt*sbot)*... rounds differently by 1 ULP, and dq1 feeds
    // the link dqdh scattered into the node sumdqdh surcharge denominator,
    // where a single ULP flips the PS4D knife-edge onset (800-node-sewer).
    if (is_dw) {
        // forcmain.c:108-112: re = 4·R·|v|/VISCOS; f from Colebrook;
        // Sf-term = f · sBot · |v| / R  (sBot = 1/(8·lengthFactor)).
        const double re = 4.0 * rMid * absv / forcemain::VISCOS;
        const double f  = forcemain::getFricFactor(tile_fm_rbot_[uci],
                                                   rMid, re);
        dq1 = dt * (f * sbot * absv / rMid);
    } else {
        // forcmain.c:105-106: Sf-term = sBot · |v|^0.852 / R^1.1667.
        dq1 = dt * (sbot * std::pow(absv, 0.852) / std::pow(rMid, 1.1667));
    }

    // Head gradient — PARITY dwflow.c:242: divide by length directly, like
    // the Manning kernel; x*inv_len rounds differently by 1 ULP, and on a
    // pressurized force main that ULP reaches dqdh and the node sumdqdh
    // surcharge denominator (PS4D on 800-node-sewer).
    const double fm_length = tile_length_[uci];
    double dq2 = dt_g * aWtd * (h2 - h1) / fm_length;

    // sig=0: no unsteady/convective terms (dq3=dq4=0)

    // Local losses
    double dq5 = 0.0;
    if (has_losses_[uj]) {
        double absq = std::fabs(qLast);
        double losses = 0.0;
        // #144: local losses are velocity heads — conveyance areas.
        if (a1Conv > FUDGE) losses += tile_loss_inlet_[uci] * (absq / a1Conv);
        if (a2Conv > FUDGE) losses += tile_loss_outlet_[uci] * (absq / a2Conv);
        if (aMidConv > FUDGE) losses += tile_loss_avg_[uci] * (absq / aMidConv);
        dq5 = losses / 2.0 / fm_length * dt;  // PARITY dwflow.c:229
    }

    // Evaporation/seepage. Length divisor uses RAW length (matching legacy
    // `dwflow.c:233`: `dq6 = ... / link_getLength(j)`, where
    // `link_getLength` returns `Conduit[k].length` (raw, not modLength).
    double dq6 = 0.0;
    {
        double conduit_length = tile_links_length_[uci];
        double loss_rate = CD.evap_loss_rate[uci] + CD.seep_loss_rate[uci];  // row==uci
        if (loss_rate > 0.0 && conduit_length > 0.0)
            dq6 = loss_rate * 2.5 * dt * v / conduit_length;
    }

    // Flow update
    double qOld = links.old_flow[uj] / barrels_d;
    double denom = 1.0 + dq1 + dq5;

    // Unsteady friction (issue #156 Phase 3), force-main arm: full pipe, so
    // the end areas are equal and the convective |∂V/∂x| term vanishes within
    // the link — only the local-acceleration fold applies (vOld from the
    // constant full-flow area). Same dead-band as the Manning arm; the
    // default path evaluates the original expression verbatim.
    bool uf_applied = false;
    if (unsteady_friction != 0 && uf_k3 > 0.0) {
        constexpr double kUfVelFloor = 0.01;  // ft/s, mirrors FvKernels
        const double vOld = (aWtd > FUDGE)
            ? qOld / aWtd : 0.0;
        if (std::fabs(vOld) >= kUfVelFloor ||
            std::fabs(v) >= kUfVelFloor) {
            denom += uf_k3;
            uf_applied = true;
        }
    }
    // k3·aWtd·vOld with vOld = qOld/aWtd collapses to k3·qOld exactly.
    double q = uf_applied
        ? (qOld - dq2 + dq6 + uf_k3 * qOld) / denom
        : (qOld - dq2 + dq6) / denom;
    // PARITY dwflow.c:240 (same grouping as the Manning kernel):
    // ((1/denom)*GRAVITY)*dt, divide by length — NOT dt_g / inv_len.
    dqdh_[uj] = 1.0 / denom * GRAVITY * dt * aWtd / fm_length * barrels_d;

    // Shared post-processing
    applyFlowLimits(ctx, dt, step, uj, q, qLast, barrels_d, isFull);
}

// ============================================================================
// updateNodeFlows -- serial all-links scatter (matching legacy)
// Only used on the no-callback path (tests / standalone execute); the
// production path uses gatherConduitNodeFlows + the callback's own scatter.
// ============================================================================

void DWSolver::updateNodeFlows(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Apply computed flow
        links.flow[uj] = new_flow_[uj];

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);

        double q = new_flow_[uj];

        // Update total inflow & outflow at upstream/downstream nodes
        // (matching legacy dynwave.c::updateNodeFlows)
        if (q >= 0.0) {
            nodes.outflow[un1] += q;
            nodes.inflow[un2]  += q;
        } else {
            nodes.inflow[un1]  -= q;
            nodes.outflow[un2] -= q;
        }

        // Accumulate dQ/dH for node depth solver
        xnode_.sumdqdh[un1] += dqdh_[uj];
        xnode_.sumdqdh[un2] += dqdh_[uj];

        // Legacy updateNodeFlows (dynwave.c:562-563) adds conduit
        // surfArea1/2 to BOTH end nodes unconditionally. The STORAGE
        // carve-out only applies to non-conduit links — legacy's
        // findNonConduitSurfArea (dynwave.c:507-510) zeroes surfArea1/2
        // for orifices/weirs at STORAGE ends, and in the refactored
        // engine that skip is already enforced by StructureSolver's
        // scatter (HydStructures.cpp). For conduits (this path),
        // matching legacy means no STORAGE skip.
        double sa1 = surf_area1_[uj];
        double sa2 = surf_area2_[uj];

        // Add conduit evap/seepage loss to node outflows (matching legacy lines 542-558)
        if (links.type[uj] == LinkType::CONDUIT) {
            const auto uci_l = static_cast<std::size_t>(tile_uj_to_ci_[uj]);  // row==ci
            double conduit_loss = (ctx.link_subtypes.conduits.evap_loss_rate[uci_l]
                                   + ctx.link_subtypes.conduits.seep_loss_rate[uci_l])
                                  * tile_barrels_d_[uci_l];
            if (conduit_loss > 0.0) {
                // Split loss between nodes unless one is an outfall
                if (nodes.type[un1] != NodeType::OUTFALL &&
                    nodes.type[un2] != NodeType::OUTFALL)
                    conduit_loss /= 2.0;
                if (nodes.type[un1] != NodeType::OUTFALL)
                    nodes.outflow[un1] += conduit_loss;
                if (nodes.type[un2] != NodeType::OUTFALL)
                    nodes.outflow[un2] += conduit_loss;
            }
        }

        // Accumulate link surface area contributions to nodes
        // (matching legacy dynwave.c updateNodeFlows: surfArea * barrels).
        // Virtual junctions accumulate their natural half-link areas too —
        // the area is the continuity linearization, not bookkept storage
        // (their committed volume is identically zero); what they skip is
        // the artificial MIN_SURFAREA floor in setNodeDepth.
        const int ci_b = tile_uj_to_ci_[uj];
        int barrels = (ci_b >= 0)
            ? static_cast<int>(tile_barrels_d_[static_cast<std::size_t>(ci_b)]) : 1;
        double b = static_cast<double>(barrels);
        xnode_.new_surf_area[un1] += sa1 * b;
        xnode_.new_surf_area[un2] += sa2 * b;
    }
}

// ============================================================================
// gatherConduitNodeFlows -- parallel node-centric conduit scatter
//
// Replaces the serial conduits-only updateNodeFlows pass with a team
// `omp for` over NODES: each node accumulates its incident conduits'
// contributions in ascending link-index order (see buildConduitNodeCSR for
// the bit-exactness proof). Each node is owned by exactly one thread, and
// the per-entry operation order below — (1) flow, (2) dQ/dH, (3) evap/seep
// loss, (4) surface area — is the per-node projection of the serial
// per-link scatter's accumulator updates.
//
// links.flow commit is fused into momentumKernels (own-element write); this
// gather reads new_flow_ directly.
// ============================================================================

void DWSolver::gatherConduitNodeFlows(SimulationContext& ctx) {
    auto& nodes = ctx.nodes;
    const auto& CD = ctx.link_subtypes.conduits;

    #pragma omp for schedule(static)
    for (int n = 0; n < n_nodes_; ++n) {
        const auto un = static_cast<std::size_t>(n);
        const int b0 = csr_row_[un];
        const int b1 = csr_row_[un + 1];
        if (b0 == b1) continue;   // no incident conduits — untouched, like serial

        const bool self_outfall = (node_tile_[un].is_outfall != 0);
        double inflow  = nodes.inflow[un];
        double outflow = nodes.outflow[un];
        double sumdqdh = xnode_.sumdqdh[un];
        double surf    = xnode_.new_surf_area[un];

        for (int k = b0; k < b1; ++k) {
            const auto uk = static_cast<std::size_t>(k);
            const auto uj = static_cast<std::size_t>(csr_link_[uk]);
            const bool at_n2 = (csr_is_n2_[uk] != 0);
            const auto uci = static_cast<std::size_t>(tile_uj_to_ci_[uj]);

            // (1) Total inflow & outflow contribution (legacy sign rule).
            const double q = new_flow_[uj];
            if (q >= 0.0) {
                if (at_n2) inflow += q; else outflow += q;
            } else {
                if (at_n2) outflow -= q; else inflow -= q;
            }

            // (2) dQ/dH — conduits add to both end nodes unconditionally.
            sumdqdh += dqdh_[uj];

            // (3) Conduit evap/seep loss to node outflow (legacy dynwave.c
            // 542-558): halved when neither end is an outfall; outfall ends
            // receive no share. Recomputing the halved value per node is
            // bit-identical to the serial once-per-link computation.
            double conduit_loss = (CD.evap_loss_rate[uci]
                                   + CD.seep_loss_rate[uci])
                                  * tile_barrels_d_[uci];
            if (conduit_loss > 0.0) {
                if (!self_outfall && !csr_other_outfall_[uk])
                    conduit_loss /= 2.0;
                if (!self_outfall)
                    outflow += conduit_loss;
            }

            // (4) Surface-area contribution (surfArea * barrels), keeping
            // the serial path's int-cast round-trip of the barrel count.
            const int barrels = static_cast<int>(tile_barrels_d_[uci]);
            surf += (at_n2 ? surf_area2_[uj] : surf_area1_[uj])
                    * static_cast<double>(barrels);
        }

        nodes.inflow[un]         = inflow;
        nodes.outflow[un]        = outflow;
        xnode_.sumdqdh[un]       = sumdqdh;
        xnode_.new_surf_area[un] = surf;
    }
}

// ============================================================================
// computeAASkipFlags -- identify nodes where Anderson acceleration is invalid
// ============================================================================
//
// AA assumes the fixed-point operator G is the same at iterations k-1 and k.
// These situations violate that assumption and must mark the affected nodes:
//   EXTRAN surcharge: discontinuous dQ/dH at crown → skip surcharged nodes
//   DYNAMIC_SLOT:     per-iterate geometry rewrite → skip nodes with active DPS
//   SLOT:             C⁰ kink near y/yFull ≈ 0.985 → skip nodes near cutoff
//   Weir / orifice:   flow equation switches at structure crown
//   Pump:             on/off is discrete, always non-smooth at end nodes
//
// Flags are scatter-computed: walk conduits / non-conduits once, set skip
// on end-nodes. A residual-magnitude gate in updateNodeDepths provides an
// additional per-iteration safety net for edge cases not enumerated here.

void DWSolver::computeAASkipFlags(const SimulationContext& ctx) {
    if (!anderson_accel) return;

    const auto& links = ctx.links;
    std::fill(aa_skip_.begin(), aa_skip_.end(), uint8_t(0));

    // EXTRAN surcharged-node skip: required ONLY under the EXPLICIT two-branch
    // continuity formulation, where setNodeDepth switches to the dQ/dH surcharge
    // branch at the crown — a branch-discontinuous operator that violates AA's
    // smooth-G assumption. Under SEMI_IMPLICIT the unified Crank-Nicolson update
    // (dy = dV / (A + 0.5*dt*sumdqdh)) is C1-smooth through the free-surface ⟷
    // surcharge transition, so plain surcharged junctions are AA-eligible. The
    // genuinely-discrete cases (pumps, weir/orifice at crown, active DPS slot,
    // static-slot kink) are non-smooth in the link-level sumdqdh inputs — not in
    // the node-continuity branch — so the dedicated walks below still skip them
    // in BOTH continuity modes.
    if (surcharge_method == SurchargeMethod::EXTRAN &&
        node_continuity == NodeContinuity::EXPLICIT) {
        for (int i = 0; i < n_nodes_; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (xnode_.is_surcharged[ui])
                aa_skip_[ui] = 1;
        }
    }

    // TPA (issue #156, plan §B3): skip AA at both end nodes of any conduit
    // whose latch CHANGED this step — a discrete operator switch, the same
    // reasoning as the static-slot kink walk. Steady latched or steady free
    // conduits remain AA-eligible.
    if (surcharge_method == SurchargeMethod::TPA) {
        for (int ci = 0; ci < n_conduits_; ++ci) {
            const auto uci = static_cast<std::size_t>(ci);
            if (!tpa_latch_changed_[uci]) continue;
            const auto uj = static_cast<std::size_t>(conduit_idx_[uci]);
            const int n1 = links.node1[uj];
            const int n2 = links.node2[uj];
            if (n1 >= 0) aa_skip_[static_cast<std::size_t>(n1)] = 1;
            if (n2 >= 0) aa_skip_[static_cast<std::size_t>(n2)] = 1;
        }
    }

    // DYNAMIC_SLOT: skip AA for nodes incident to a conduit with active slot area
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        for (int ci = 0; ci < n_conduits_; ++ci) {
            auto uci = static_cast<std::size_t>(ci);
            if (dps_.As[uci] > 0.0) {
                int j = conduit_idx_[uci];
                auto uj = static_cast<std::size_t>(j);
                aa_skip_[static_cast<std::size_t>(links.node1[uj])] = 1;
                aa_skip_[static_cast<std::size_t>(links.node2[uj])] = 1;
            }
        }
    }

    // SLOT: skip AA for nodes incident to a conduit near the slot cutoff
    if (surcharge_method == SurchargeMethod::SLOT) {
        for (int ci = 0; ci < n_conduits_; ++ci) {
            auto uci = static_cast<std::size_t>(ci);
            int j = conduit_idx_[uci];
            auto uj = static_cast<std::size_t>(j);
            if (is_open_[uj]) continue;  // open shapes have no slot
            double yf = links.xsect_y_full[uj];
            if (yf <= 0.0) continue;
            double ratio = depth_mid_[uj] / yf;
            if (ratio >= 0.98 && ratio <= 1.02) {
                aa_skip_[static_cast<std::size_t>(links.node1[uj])] = 1;
                aa_skip_[static_cast<std::size_t>(links.node2[uj])] = 1;
            }
        }
    }

    // Weir / orifice: both types switch flow equations discontinuously at
    // their crown elevation (weir → orifice; orifice f<1 → f=1). That kink
    // breaks AA's smooth-G assumption, and the kink happens mid-network —
    // AA overshoots on the first post-surcharge iter and won't recover.
    // Mark both end-nodes when the upstream-side HGL is at or above the
    // structure crown.
    //
    // Legacy doesn't run AA at all, so there's no parallel in legacy code;
    // this is a pure refactored-engine stability guard.
    //
    // This walks all links (not just non-conduits) — cheap (~1.3 k links)
    // and keeps DWSolver independent of StructureSolver's SoA groups.
    const auto& nodes = ctx.nodes;
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        auto lt = links.type[uj];
        if (lt != LinkType::WEIR && lt != LinkType::ORIFICE) continue;

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 < 0 || n2 < 0) continue;
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);

        double y_full  = links.xsect_y_full[uj];
        double setting = links.setting[uj];
        if (y_full <= 0.0 || setting <= 0.0) continue;

        // Crown elevation — matches the flow code in HydStructures.cpp:
        //   Weir   : hcrown = invert(n1) + crest_height + y_full
        //            (setting raises the effective crest; the crown stays
        //             at design height because crown = crest + s·yf and
        //             crest = crest_height + (1-s)·yf.)
        //   Orifice: hcrown = invert(n1) + offset1 + y_full · setting
        //            (setting is the OPENING fraction; sill height is
        //             fixed at offset1.)
        double hcrown;
        if (lt == LinkType::WEIR) {
            const int wr = ctx.link_subtypes.weir_row(j);
            const double crest = (wr >= 0)
                ? ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)] : 0.0;
            hcrown = nodes.invert_elev[un1] + crest + y_full;
        } else {
            hcrown = nodes.invert_elev[un1] + links.offset1[uj]
                   + y_full * setting;
        }

        double hgl1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double hgl2 = nodes.depth[un2] + nodes.invert_elev[un2];
        double hgl_up = std::max(hgl1, hgl2);

        if (hgl_up >= hcrown) {
            aa_skip_[un1] = 1;
            aa_skip_[un2] = 1;
        }
    }

    // Pumps: on/off state is discrete (setting jumps 0↔1), so the fixed-point
    // operator at both end nodes has no smooth representation. Skip both ends
    // of every pump unconditionally — pumps are a small fraction of links and
    // legacy has no parallel (no AA), so this is a pure stability guard.
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::PUMP) continue;
        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        if (n1 >= 0) aa_skip_[static_cast<std::size_t>(n1)] = 1;
        if (n2 >= 0) aa_skip_[static_cast<std::size_t>(n2)] = 1;
    }
}

// ============================================================================
// updateNodeDepthsTeam -- per-node, Picard convergence check (team-callable)
// ============================================================================

void DWSolver::updateNodeDepthsTeam(SimulationContext& ctx, double dt, int step,
                                    int& unconv_shared) {
    auto& nodes = ctx.nodes;
    const bool use_anderson = anderson_accel;
    int local_unconv = 0;   // per-thread partial of the unconverged-node count

    // Phase 1: Compute G(y) for each node via setNodeDepth.
    // Each thread handles a subset of nodes; per-node data (xnode_, nodes.depth,
    // etc.) is written only by the owning thread (no cross-node dependencies).
    // Parallel by analogy with legacy findNodeDepths (src/legacy/engine/dynwave.c:580),
    // but as an ORPHANED `omp for` binding to the persistent team opened in
    // execute(). `nowait` — each thread proceeds to its atomic tally combine
    // below; the explicit barrier after it orders the shared count for all.
    #pragma omp for schedule(static) nowait
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Skip outfalls (fixed boundary)
        // PARITY: leave the outfall's `converged` flag FALSE, matching legacy
        // (dynwave.c initRoutingStep sets converged=FALSE and findNodeDepths
        // skips outfalls, never setting it TRUE). Marking it converged=1 here
        // made findBypassedLinks() bypass every outfall-connected conduit once
        // its other end converged, FREEZING that conduit's geometry / node
        // surface-area for the rest of the Picard iteration — whereas legacy
        // keeps recomputing it (outfall end never "converges"). That stale
        // surface area was the first per-iteration divergence from legacy on
        // every free-outfall model (e.g. extran1 node 10309). The overall
        // convergence tally below already skips outfalls, so converged=0 here
        // does not block step convergence; the per-node stat counts the outfall
        // as non-converged exactly as legacy's updateConvergenceStats does.
        if (node_tile_[ui].is_outfall) {
            xnode_.converged[ui] = 0;
            continue;
        }

        double y_last = nodes.depth[ui];
        setNodeDepth(ctx, i, dt, step);
        double g_k = nodes.depth[ui];  // G(y_k) — the Picard update

        // --- Anderson acceleration (depth-2 mixing) ---
        // On step 0: just record state for next iteration.
        // On step 1+: use previous iterate to compute optimal blend.
        // Skip AA when the fixed-point operator G is non-smooth at this node
        // (see computeAASkipFlags for the enumerated conditions) or when the
        // residual is too large to trust the linear-convergence assumption
        // AA is built on.
        if (use_anderson && step >= 1 && !aa_skip_[ui]) {
            double r_k = g_k - y_last;                     // residual at current iterate

            // Residual-magnitude safety gate. When |r_k| is many tolerances
            // large, we are far from the linear regime where AA is provably
            // accelerating; the blended iterate can overshoot badly. 20×
            // head_tol is an empirical threshold — loose enough to leave
            // room for normal early-iteration residuals while catching the
            // pathological cases.
            if (std::fabs(r_k) <= 20.0 * head_tol) {
                double r_km1 = aa_r_prev_[ui];             // residual from previous iterate
                double dr = r_k - r_km1;
                double dr2 = dr * dr;

                if (dr2 > 1e-30) {  // avoid division by zero
                    // Two-point Anderson / Aitken coefficient. alpha =
                    // r_k*dr/dr^2 = r_k/(r_k - r_km1) is the weight on the
                    // PREVIOUS mapped value: the textbook secant update is
                    //   y = g_k - alpha*(g_k - g_prev)
                    //     = (1 - alpha)*g_k + alpha*g_prev,
                    // which zeroes the linear-model blended residual
                    // (1-alpha)*r_k + alpha*r_km1. The [0,1] clamp keeps the
                    // update interpolation-only (no extrapolation), so with
                    // same-sign shrinking residuals (alpha < 0 unclamped) the
                    // blend degenerates to the NEW Picard iterate g_k — never
                    // to the older one.
                    double alpha = std::max(0.0, std::min(1.0, r_k * dr / dr2));

                    // Anderson mixed update (alpha weights g_prev; the
                    // complementary weight goes to the current g_k)
                    double y_anderson = (1.0 - alpha) * g_k + alpha * aa_g_prev_[ui];

                    // Physical bounds safeguard: depth must be >= 0
                    // Fall back to standard Picard if Anderson produces
                    // unphysical result. (With alpha clamped to [0,1],
                    // y_anderson is a convex blend of two committed, already-
                    // bounded depths, so this check is a belt-and-braces
                    // guard rather than an active constraint.)
                    if (y_anderson >= 0.0) {
                        // Commit the ACCEPTED depth through the same canonical
                        // state-commit routine used for the raw Picard result,
                        // so volume, overflow and dYdT (CFL) describe the
                        // mixed depth rather than the unmixed candidate g_k.
                        // This matters when the accepted mix is the FINAL
                        // Picard iteration: that state feeds flooding totals,
                        // mass balance, storage losses and the next adaptive
                        // timestep. dV is recomputed from the same inputs and
                        // arithmetic order as setNodeDepth() (inflow/outflow
                        // are unchanged since that call, same iteration).
                        const double dQ = nodes.inflow[ui] - nodes.outflow[ui];
                        const double dV =
                            0.5 * (nodes.old_net_inflow[ui] + dQ) * dt;
                        commitNodeDepthState(ctx, i, y_anderson, dV, dt);
                    }
                    // else: keep g_k (standard Picard result from setNodeDepth)
                }
            }
        }

        // Record state for next Anderson iteration
        if (use_anderson) {
            aa_y_prev_[ui] = y_last;
            aa_g_prev_[ui] = g_k;
            aa_r_prev_[ui] = g_k - y_last;
        }

        // Convergence check. The tally counts non-outfall unconverged nodes
        // only (outfalls take the `continue` above), matching the former
        // sequential pass / legacy findNodeDepths' final scan. Integer count:
        // the cross-thread combine below is order-free, hence bit-exact.
        //
        // Require BOTH the raw Picard residual |G(y_k) - y_k| AND the accepted
        // (possibly Anderson-mixed) movement |depth - y_k| to be within
        // tolerance. Testing accepted movement alone let an Anderson mix that
        // lands back near y_last mark a node converged while the operator
        // residual was still large — e.g. when alpha clamps to 1 the mix
        // returns g_prev, and if the previous iteration did not itself mix then
        // y_last == g_prev, so the movement is exactly zero regardless of how
        // far G(y_last) actually is. That false convergence is not local: the
        // flag feeds findBypassedLinks() (which freezes both endpoint links)
        // and the loop-exit test t_converged = (unconv_shared == 0), so a
        // network that all false-converges exits the Picard loop with an
        // unconverged hydraulic state.
        //
        // With Anderson off / fallback nodes.depth[ui] == g_k, so
        // accepted_movement == raw_residual and this collapses bit-exactly to
        // the previous single-test behavior. Only an accepted mix is affected:
        // small mixed movement can no longer hide a large raw residual, and a
        // large accepted jump can no longer be declared converged on the
        // strength of a small raw residual alone.
        const double raw_residual      = std::fabs(g_k - y_last);
        const double accepted_movement = std::fabs(nodes.depth[ui] - y_last);
        const uint8_t conv_flag =
            (raw_residual <= head_tol && accepted_movement <= head_tol) ? 1 : 0;
        xnode_.converged[ui] = conv_flag;
        if (!conv_flag) ++local_unconv;
    }

    // Combine per-thread partials into the shared tally. The caller zeroed
    // unconv_shared behind the previous single's implicit barrier; after the
    // explicit barrier below every thread reads the final combined value.
    if (local_unconv != 0) {
        #pragma omp atomic
        unconv_shared += local_unconv;
    }
    #pragma omp barrier
}

// ============================================================================
// setNodeDepth -- single node depth update (EXTRAN surcharge algorithm)
// ============================================================================

void DWSolver::setNodeDepth(SimulationContext& ctx, int node_idx, double dt,
                            int step) {
    auto& nodes = ctx.nodes;
    auto ui = static_cast<std::size_t>(node_idx);

    // Step-invariant node fields come from the node-dense tile (one cache
    // line) instead of seven scattered SoA arrays — see NodeTile. The tile
    // also pre-evaluates yCrown with the identical subtraction this function
    // previously performed per call.
    const NodeTile& t = node_tile_[ui];
    const bool is_storage = (t.is_storage != 0);

    // --- Initialize ---
    double y_old = nodes.old_depth[ui];
    double y_last = nodes.depth[ui];
    double full_depth = t.full_depth;
    double yCrown = t.y_crown;

    // Legacy dynwave.c:649: canPond = (AllowPonding && pondedArea > 0).
    // The global ALLOW_PONDING option must gate ponding; otherwise a node
    // with a non-zero ponded_area (set out of habit by modellers) will pond
    // against the user's intent and accumulate water above full_depth that
    // legacy would have discarded via the "add to losses" branch.
    //
    // 2D-coupled junctions are an exception: their ponded_area is the auto-
    // assigned 2D-cell footprint, and they must pond above the crown so the
    // 1D HGL tracks the overlying 2D surface — regardless of ALLOW_PONDING.
    const bool is_coupled = (ui < ctx.coupled_node.size() && ctx.coupled_node[ui]);
    bool can_pond = (ctx.options.allow_ponding || is_coupled) && (t.ponded_area > 0.0);
    bool is_ponded = (can_pond && y_last > full_depth);

    nodes.overflow[ui] = 0.0;
    double surf_area = xnode_.new_surf_area[ui];
    // Use the user-configured MIN_SURFAREA (resolved in init()) rather than
    // the compiled-in constants::MIN_SURFAREA — matches legacy
    // dynwave.c:658 `surfArea = MAX(surfArea, MinSurfArea)` where
    // MinSurfArea is the runtime value from the INP [OPTIONS] block.
    //
    // Virtual junctions skip the floor: their surface area is exactly the
    // natural half-link free-surface area of the fused reach (the artificial
    // MIN_SURFAREA storage smearing is the thing the feature removes; the
    // natural area is the correct continuity linearization and their
    // committed volume stays identically zero).
    if (t.is_virtual == 0)
        surf_area = std::max(surf_area, min_surf_area_);

    // --- Net flow volume change (trapezoidal averaging with previous step) ---
    double dQ = nodes.inflow[ui] - nodes.outflow[ui];
    double dV = 0.5 * (nodes.old_net_inflow[ui] + dQ) * dt;

    // --- Determine if node is surcharged ---
    //
    // The flag is used by AA skip logic, statistics, and (for EXTRAN only)
    // the explicit continuity branch's dQ/dH path.  EXTRAN and DYNAMIC_SLOT
    // share the same geometric definition (depth above crown), but only
    // EXTRAN actually switches to the dQ/dH formulation — DYNAMIC_SLOT
    // handles the surcharge transition through the slot-augmented surface
    // area patched by applyDPSGeometry, so it takes the standard
    // dy = dV / A_surf path with hs implicit in y.
    bool is_surcharged = false;
    if (surcharge_method == SurchargeMethod::EXTRAN ||
        surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        if (is_ponded) {
            is_surcharged = false;
        }
        else if (is_storage) {
            is_surcharged = (t.sur_depth > 0.0 &&
                             y_last > full_depth);
        }
        else {
            is_surcharged = (yCrown > 0.0 && y_last > yCrown);
        }
    }
    xnode_.is_surcharged[ui] = is_surcharged ? 1 : 0;

    // ---- Virtual junction: unfloored, sealed node update ----
    // (plans/VIRTUAL_JUNCTION_IMPLEMENTATION_PLAN.md §3.1.) The head update
    // follows the standard free-surface/surcharge formulations with the
    // natural (unfloored) half-link surface area. When that area vanishes
    // (dry pair, or fully surcharged with slot geometry) the update falls
    // back to the zero-storage Newton step dy = dQ/Σdqdh — the engine
    // accumulates sumdqdh POSITIVE with dQ_net/dH = −sumdqdh, the same sign
    // convention the EXTRAN surcharge branch divides by. Flooding cannot
    // occur (commitNodeDepthState seals the node: volume ≡ 0, overflow ≡ 0,
    // no cap at full depth).
    // Under SEMI_IMPLICIT the two regimes below collapse into one denominator,
    // exactly as they do for real nodes: a virtual junction carries no storage,
    // so its natural area can be arbitrarily small (a dry pass-through node
    // between two conduits has almost none) and the explicit dV/A step then
    // divides a finite volume change by ~0. On a 5028-VJ transmission main a
    // full-pipe neighbour drove dV = -4.1e6 ft^3 into a node holding
    // A = 2.4 ft^2, reaching head 1.9e14 by the fourth routing step. Folding
    // the head-response Jacobian into the denominator keeps it bounded as
    // A -> 0, where it degrades to the zero-storage Newton step dy = dQ/Σdqdh
    // — the relation this node type actually obeys. sumdqdh is accumulated
    // non-negative (g*dt*A/L/denom per link), so the sum is positive whenever
    // the node has either storage or a live flow path.
    if (t.is_virtual != 0) {
        const double sum = xnode_.sumdqdh[ui];
        const double denom_semi = surf_area + 0.5 * dt * sum;
        const bool semi_implicit =
            (node_continuity == NodeContinuity::SEMI_IMPLICIT) &&
            denom_semi > FUDGE;
        double y_vj;
        if (semi_implicit) {
            // Unified path — free-surface and surcharged alike, matching the
            // real-node SEMI_IMPLICIT branch (which likewise ignores the
            // is_surcharged dQ/dH split and the crown clamp).
            y_vj = y_old + dV / denom_semi;
            xnode_.old_surf_area[ui] = surf_area;
            if (step > 0) y_vj = (1.0 - omega) * y_last + omega * y_vj;
        } else if (surf_area > FUDGE && !is_surcharged) {
            // EXPLICIT free surface: historical dV/A path on the natural area,
            // with the usual under-relaxation. Decks that do not ask for
            // semi-implicit node continuity stay bit-identical.
            y_vj = y_old + dV / surf_area;
            xnode_.old_surf_area[ui] = surf_area;
            if (step > 0) y_vj = (1.0 - omega) * y_last + omega * y_vj;
        } else if (sum > FUDGE) {
            // Surcharged or area-less: zero-storage Newton on the flow
            // balance, with the EXTRAN-style crown-proximity blending so the
            // transition into/out of surcharge stays smooth.
            double denomv = sum;
            if (yCrown > 0.0 && y_last < 1.25 * yCrown) {
                const double f = (y_last - yCrown) / yCrown;
                denomv += (xnode_.old_surf_area[ui] / dt - sum)
                          * std::exp(-15.0 * f);
            }
            y_vj = (denomv != 0.0) ? (y_last + dQ / denomv) : y_last;
            // Keep the surcharged iterate at or above the crown, mirroring
            // the EXTRAN branch (the free-surface path recovers below it).
            if (is_surcharged && y_vj < yCrown) y_vj = yCrown - FUDGE;
        } else {
            y_vj = y_last;   // dry pair — hold
        }
        commitNodeDepthState(ctx, node_idx, y_vj, dV, dt);
        return;
    }

    // Only EXTRAN takes the dQ/dH surcharge branch in the explicit solver.
    // DYNAMIC_SLOT uses the slot's effective top width T_s (fed into
    // surf_area1/2 by applyDPSGeometry) so the standard dV/A path produces
    // physically sensible head evolution with `head = invert + y =
    // invert + y_full + hs` falling out naturally when y > y_full.
    const bool use_surcharge_dqdh =
        is_surcharged && (surcharge_method == SurchargeMethod::EXTRAN);

    double y_new;

    if (node_continuity == NodeContinuity::SEMI_IMPLICIT) {
        // =================================================================
        // Semi-implicit unified formulation (Crank-Nicolson, theta = 0.5)
        // =================================================================
        // Linearise the node continuity equation with head-dependent flows:
        //
        //   A * dH/dt = Q_net(H)
        //
        // Trapezoidal (Crank-Nicolson) time integration gives:
        //
        //   A * dH = 0.5 * [Q_net_old + Q_net_new] * dt
        //
        // Linearising Q_net_new around the current head estimate:
        //
        //   Q_net_new ≈ Q_net + (dQ_net/dH) * dH
        //
        // where sumdqdh is accumulated POSITIVE from the link dqdh values
        // (higher head ⟶ more net outflow through connected links), so
        // dQ_net/dH = -sumdqdh — the same sign convention the EXTRAN
        // surcharge branch divides by.  Substituting and rearranging:
        //
        //   dH = dV / (A + dt * sumdqdh / 2)
        //
        // dV already contains the trapezoidal average of old_net_inflow and
        // current dQ, so the sumdqdh correction folds the head-dependent
        // flow response into the same timestep.
        //
        // The equation unifies the free-surface and surcharged regimes:
        // when surfArea dominates the denominator ≈ A (classic dV/A path);
        // when the node surcharges and A shrinks, the sumdqdh term takes
        // over, producing a smooth transition without a branch.
        // =================================================================

        double denom = surf_area + 0.5 * dt * xnode_.sumdqdh[ui];
        denom = std::max(denom, min_surf_area_);

        double dy = dV / denom;
        y_new = y_old + dy;

        // Save non-ponded surface area (used by flooding logic and CFL)
        if (!is_ponded) {
            xnode_.old_surf_area[ui] = surf_area;
        }

        // Apply under-relaxation to new depth estimate
        if (step > 0) {
            y_new = (1.0 - omega) * y_last + omega * y_new;
        }

        // Don't allow a ponded node to drop much below full depth
        if (is_ponded && y_new < full_depth) {
            y_new = full_depth - FUDGE;
        }
    }
    else {
        // =================================================================
        // Explicit (legacy) two-branch formulation
        // =================================================================

        // --- Non-surcharged path: depth change based on surface area ---
        // Also used if storage node is surcharged but has no connecting links,
        // and for DYNAMIC_SLOT in all cases (the slot's contribution to
        // surf_area handles the surcharge regime smoothly, no dQ/dH branch).
        if (!use_surcharge_dqdh ||
            (is_storage && xnode_.sumdqdh[ui] == 0.0)) {

            double dy = dV / surf_area;
            y_new = y_old + dy;

            // Save non-ponded surface area for use in surcharge algorithm
            if (!is_ponded) {
                xnode_.old_surf_area[ui] = surf_area;
            }

            // Apply under-relaxation to new depth estimate
            if (step > 0) {
                y_new = (1.0 - omega) * y_last + omega * y_new;
            }

            // Don't allow a ponded node to drop much below full depth
            if (is_ponded && y_new < full_depth) {
                y_new = full_depth - FUDGE;
            }
        }
        // --- Surcharged path: depth change based on dQ/dH (matching legacy) ---
        else {
            // Apply correction factor for upstream terminal nodes
            double corr = 1.0;
            if (t.degree < 0) corr = 0.6;

            // Allow surface area from last non-surcharged condition to influence
            // dqdh if depth is close to crown depth (smooth transition)
            double denom = xnode_.sumdqdh[ui];
            if (y_last < 1.25 * yCrown && yCrown > 0.0) {
                double f = (y_last - yCrown) / yCrown;
                denom += (xnode_.old_surf_area[ui] / dt -
                          xnode_.sumdqdh[ui]) * std::exp(-15.0 * f);
            }

            // Compute new estimate of node depth
            double dy = 0.0;
            if (denom != 0.0) {
                dy = corr * dQ / denom;
            }
            y_new = y_last + dy;

            // Don't drop below crown
            if (y_new < yCrown) y_new = yCrown - FUDGE;

            // Don't allow a newly ponded node to rise much above full depth
            if (can_pond && y_new > full_depth) {
                y_new = full_depth + FUDGE;
            }
        }
    }

    // A3 parity term tracing for one node (SWMM_TRACE_NODE=<index>, first 64
    // invocations; format-matched to the legacy trace in dynwave.c).
    {
        static FILE* nf = nullptr;
        static long  nf_target = -2;
        static long  nf_skip = 0;
        static int   nf_count = 0;
        if (nf_target == -2) {
            const char* p  = std::getenv("SWMM_TRACE_NODE");
            const char* tr = std::getenv("SWMM_TRACE_RSTEP");
            const char* sk = std::getenv("SWMM_TRACE_SKIP");
            nf_target = -1;
            if (sk && *sk) nf_skip = std::atol(sk);
            if (p && *p && tr && *tr) {
                char fname[512];
                nf_target = std::atol(p);
                std::snprintf(fname, sizeof(fname), "%s.node%ld", tr, nf_target);
                nf = std::fopen(fname, "w");
                if (nf) std::fprintf(nf,
                    "n,yOld,yLast,dQ,dV,surfArea,sumdqdh,surch,yNew\n");
            }
        }
        if (nf && node_idx == nf_target) {
            ++nf_count;
            if (nf_count > nf_skip && nf_count <= nf_skip + 128) {
                std::fprintf(nf, "%d,%a,%a,%a,%a,%a,%a,%d,%a\n", nf_count,
                             y_old, y_last, dQ, dV, surf_area, xnode_.sumdqdh[ui],
                             is_surcharged ? 1 : 0, y_new);
                if (nf_count >= nf_skip + 128) { std::fclose(nf); nf = nullptr; }
            }
        }
    }

    // --- Commit the accepted candidate through the canonical routine ---
    // (bounds, flooding/ponding caps, overflow, volume, dYdT, depth/head).
    commitNodeDepthState(ctx, node_idx, y_new, dV, dt);
}

// ============================================================================
// commitNodeDepthState -- canonical commit of an accepted node depth
// ============================================================================
//
// The ONLY place an accepted depth candidate becomes committed node state:
// the physical lower bound, the flooding/ponding upper cap, overflow, volume,
// dYdT (used by the CFL adaptive-timestep logic) and the depth/head pair are
// all derived here from the SAME candidate.
//
// Callers: setNodeDepth() for the ordinary Picard result, and the accepted-
// Anderson branch in updateNodeDepthsTeam(). Before this helper existed, the
// Anderson branch overwrote only depth and head, so an accepted mix on the
// FINAL Picard iteration left volume, overflow and dYdT describing the
// unmixed candidate — feeding inconsistent state into flooding totals, mass
// balance, next-step storage losses and the next adaptive routing step.
//
// With Anderson OFF (the default) the single call from setNodeDepth()
// performs the identical arithmetic the previously-inlined block did, in the
// same order — bit-exact with the prior behavior.

void DWSolver::commitNodeDepthState(SimulationContext& ctx, int node_idx,
                                    double y_new, double dV, double dt) {
    auto& nodes = ctx.nodes;
    auto ui = static_cast<std::size_t>(node_idx);
    const NodeTile& t = node_tile_[ui];

    // --- Depth cannot be negative ---
    y_new = std::max(y_new, 0.0);

    // --- Virtual junction: zero storage, no flooding by construction ---
    // The head may rise above the pipe crown without cap (surcharge is
    // expressed through the connecting conduits' slot/EXTRAN treatment,
    // like a sealed manhole); volume and overflow are identically zero.
    if (t.is_virtual != 0) {
        nodes.overflow[ui] = 0.0;
        nodes.volume[ui]   = 0.0;
        if (dt > 0.0)
            xnode_.dYdT[ui] = std::fabs(y_new - nodes.old_depth[ui]) / dt;
        nodes.depth[ui] = y_new;
        nodes.head[ui]  = t.invert_elev + y_new;
        return;
    }

    // --- Ponding eligibility (same rule as setNodeDepth's entry logic) ---
    const bool is_coupled =
        (ui < ctx.coupled_node.size() && ctx.coupled_node[ui]);
    const bool can_pond =
        (ctx.options.allow_ponding || is_coupled) && (t.ponded_area > 0.0);

    // --- Determine max non-flooded depth ---
    double y_max = t.full_depth;
    if (!can_pond) y_max += t.sur_depth;

    // --- Flooding logic (matching legacy getFloodedDepth) ---
    // Reset first so a re-commit (accepted Anderson mix after the raw Picard
    // commit) cannot inherit stale overflow from the earlier candidate.
    nodes.overflow[ui] = 0.0;
    if (y_new > y_max) {
        if (!can_pond) {
            // Non-ponded flooding: cap at max, excess is overflow
            nodes.overflow[ui] = dV / dt;
            nodes.volume[ui] = t.full_volume;
            y_new = y_max;
        } else {
            // Ponded: volume can exceed full volume
            nodes.volume[ui] = std::max(nodes.old_volume[ui] + dV,
                                        t.full_volume);
            nodes.overflow[ui] = (nodes.volume[ui] -
                std::max(nodes.old_volume[ui], t.full_volume)) / dt;
        }
        if (nodes.overflow[ui] < FUDGE) nodes.overflow[ui] = 0.0;
    } else {
        nodes.volume[ui] = node::getVolume(nodes, node_idx, y_new, &ctx.tables,
                                           unit_sys_, &ctx.node_subtypes);
    }

    // --- Compute change in depth w.r.t. time (for CFL) ---
    if (dt > 0.0) {
        xnode_.dYdT[ui] = std::fabs(y_new - nodes.old_depth[ui]) / dt;
    }

    // --- Save new depth ---
    nodes.depth[ui] = y_new;
    nodes.head[ui] = t.invert_elev + y_new;
}

// ============================================================================
// getRoutingStep -- CFL-based adaptive timestep
// ============================================================================

double DWSolver::getRoutingStep(SimulationContext& ctx,
                                 double fixed_step, double courant_factor) {
    if (courant_factor <= 0.0) return fixed_step;
    // Legacy dynwave_getRoutingStep: fixed steps below MINTIMESTEP bypass the
    // variable-step machinery entirely.
    if (fixed_step < MIN_TIMESTEP) return fixed_step;

    // Effective minimum step — legacy dynwave_validate (dynwave.c:178-179)
    // clamps MinRouteStep = min(MinRouteStep, RouteStep), then >= MINTIMESTEP,
    // so the floor can never exceed the user's fixed routing step.
    const double min_route_step =
        std::max(std::min(ctx.options.min_routing_step, fixed_step),
                 MIN_TIMESTEP);

    // On first call (no flows yet), use minimum step (matching legacy line 201-204:
    // "if (VariableStep == 0.0) VariableStep = MinRouteStep")
    if (variable_step_ <= 0.0) {
        variable_step_ = min_route_step;
        return variable_step_;
    }

    double dt_min = fixed_step;
    int min_link = -1;   // index of CFL-critical link
    int min_node = -1;   // index of CFL-critical node

    // Link-based CFL (matching legacy getLinkStep with CourantFactor per-link)
    for (int j = 0; j < n_links_; ++j) {
        double t = getLinkStep(ctx, j);
        if (t <= 0.0 || t > 1.0e9) continue;
        // Apply Courant factor per-link (matching legacy dynwave.c line 856)
        t *= courant_factor;
        if (t < dt_min) {
            dt_min = t;
            min_link = j;
            min_node = -1;
        }
    }

    // Node-based CFL (matching legacy getNodeStep)
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (ctx.nodes.type[ui] == NodeType::OUTFALL) continue;
        if (ctx.nodes.depth[ui] <= FUDGE) continue;

        // Skip nodes near or above crown elevation
        double yCrown = ctx.nodes.crown_elev[ui] - ctx.nodes.invert_elev[ui];
        if (ctx.nodes.depth[ui] + FUDGE >= yCrown) continue;

        double max_depth = yCrown * 0.25;
        if (max_depth < FUDGE) continue;

        double dYdT = xnode_.dYdT[ui];
        if (dYdT < FUDGE) continue;

        double t = max_depth / dYdT;
        if (t > 0.0 && t < dt_min) {
            dt_min = t;
            min_node = i;
            min_link = -1;
        }
    }

    // Virtual-junction pair check (plan §3.3): the fused-reach convective
    // coupling respects a local Courant condition Λ/(|v|+c) with the gravity
    // wave speed from the junction depth. In practice the per-link CFL checks
    // above dominate; this is a safety net for very short pair legs.
    for (const VJuncPair& p : vjunc_) {
        if (!p.through || p.lambda <= 0.0) continue;
        const auto uu = static_cast<std::size_t>(p.up_link);
        const double aj = area2_[uu];
        const double wj = width2_[uu];
        if (aj <= FUDGE || wj <= FUDGE) continue;
        const auto ucu = static_cast<std::size_t>(tile_uj_to_ci_[uu]);
        const double qU = ctx.links.flow[uu] / tile_barrels_d_[ucu];
        const double vmag = std::fabs(qU) / aj;
        const double c = std::sqrt(constants::GRAVITY * (aj / wj));
        if (vmag + c <= FUDGE) continue;
        double t = p.lambda / (vmag + c) * courant_factor;
        if (t > 0.0 && t < dt_min) {
            dt_min = t;
            min_node = p.node;
            min_link = -1;
        }
    }

    // Update CFL-critical element counters (matching legacy stats_updateCriticalTimeCount)
    if (min_node >= 0) {
        ctx.nodes.stat_time_courant_critical[static_cast<std::size_t>(min_node)] += 1.0;
    } else if (min_link >= 0) {
        ctx.links.stat_time_courant_critical[static_cast<std::size_t>(min_link)] += 1.0;
    }

    // Apply user's minimum step (MINIMUM_STEP clamped to the fixed routing
    // step per legacy dynwave_validate — see min_route_step above)
    const double min_step = min_route_step;
    const bool floored = dt_min < min_step;
    dt_min = std::max(dt_min, min_step);
    // Round to milliseconds for deterministic behavior
    dt_min = std::floor(1000.0 * dt_min) / 1000.0;

    // Temporary diagnostic (2026-07-29 Task 2, step-shrink investigation):
    // OPENSWMM_DT_TRACE=1 prints the governing constraint each routing step.
    static const bool dt_trace = [] {
        const char* s = std::getenv("OPENSWMM_DT_TRACE");
        return s && *s && *s != '0';
    }();
    if (dt_trace) {
        const char* gov = "fixed";
        const char* nm  = "-";
        if (floored) {
            gov = "floor";
        } else if (min_link >= 0) {
            gov = "link";
            nm  = ctx.link_names.name_of(min_link).c_str();
        } else if (min_node >= 0) {
            gov = "node";
            nm  = ctx.node_names.name_of(min_node).c_str();
        }
        std::fprintf(stderr, "[DT] t=%.3f dt=%.3f gov=%s name=%s\n",
                     ctx.elapsed_ms / 1000.0, dt_min, gov, nm);
    }
    return dt_min;
}

double DWSolver::getLinkStep(const SimulationContext& ctx, int link_idx) const {
    auto uj = static_cast<std::size_t>(link_idx);
    if (ctx.links.type[uj] != LinkType::CONDUIT) return 1.0e10;
    const auto& CD = ctx.link_subtypes.conduits;
    const auto ucr = static_cast<std::size_t>(ctx.link_subtypes.conduit_row(link_idx));

    // Match legacy getLinkStep (dynwave.c lines 846-856):
    // q = |newFlow| / barrels (per-barrel flow)
    int barrels = std::max(CD.barrels[ucr], 1);
    double q = std::fabs(ctx.links.flow[uj]) / static_cast<double>(barrels);
    if (q <= FUDGE) return 1.0e10;

    // Legacy: Conduit[k].a1 (midpoint area from solver, per barrel)
    double a = area_mid_[uj];
    if (a <= FUDGE) return 1.0e10;

    double L = CD.length[ucr];
    double modL = CD.mod_length[ucr];
    const double Lscale = (L > 0.0 && modL > 0.0) ? (modL / L) : 1.0;

    // ---- DPS-surcharged path: CFL against pressure celerity c_p = c_pT / P ----
    // The Froude-based factor below uses the gravity-wave celerity sqrt(g·D),
    // which underestimates the dominant signal speed once a pipe pressurizes.
    // Use c_p directly so the variable timestep tracks the actual pressure-wave
    // speed.  t = L / (|v| + c_p), then scale by modL/L for short/culvert links.
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT && link_idx >= 0 &&
        static_cast<std::size_t>(link_idx) < tile_uj_to_ci_.size()) {
        int ci = tile_uj_to_ci_[uj];
        if (ci >= 0) {
            auto uci = static_cast<std::size_t>(ci);
            if (dps_.surcharged[uci] && L > 0.0) {
                // #144: conveyance area — the DPS slot stores, it does not
                // convey, so it must not dilute the advective signal speed.
                double v = q / conveyArea(a, ctx.links.xsect_a_full[uj]);
                double P = std::max(dps_.P[uci], 1.0);
                double c_p = dps_config_.c_pT / P;           // pressure celerity
                double denom = std::fabs(v) + c_p;
                if (denom > 0.0) return (L * Lscale) / denom;
            }
        }
    }

    double fr = froude_[uj];
    if (fr <= 0.01) return 1.0e10;

    // Legacy: t = newVolume / barrels / q
    double vol = ctx.links.volume[uj] / static_cast<double>(barrels);
    double t = vol / q;

    // Apply modified length factor for short conduits / culverts
    // (matching legacy dynwave.c line 855: t *= modLength / length)
    t *= Lscale;

    t *= fr / (1.0 + fr);  // Froude-based CFL factor
    return t;              // CourantFactor applied per-link in getRoutingStep
}

} // namespace dynwave
} // namespace openswmm
