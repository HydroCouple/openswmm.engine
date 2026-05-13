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
 *      - velocity = old_flow / area_mid (vectorisable)
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
 * @license  MIT License
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
#endif

namespace openswmm {
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
        surcharge_method == SurchargeMethod::DYNAMIC_SLOT)
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
 * @brief Compute flow area including Preissmann slot contribution.
 *
 * @details Matches legacy dwflow.c::getArea():
 *   - If y >= y_full: A = A_full + (y - y_full) * slot_width
 *   - Otherwise: standard cross-section area (from batch)
 */
double DWSolver::getSlotArea(double y, double y_full, double a_full,
                             double slot_width) const {
    if (y >= y_full && slot_width > 0.0) {
        return a_full + (y - y_full) * slot_width;
    }
    return a_full;  // caller should use batch area for y < y_full
}

/**
 * @brief Compute hydraulic radius including Preissmann slot.
 *
 * @details Matches legacy dwflow.c::getHydRad():
 *   - If y >= y_full: return R_full (hydraulic radius stays at full value)
 *   - Otherwise: standard hydraulic radius (from batch)
 */
double DWSolver::getSlotHydRad(double y, double y_full, double r_full) const {
    if (y >= y_full) return r_full;
    return r_full;  // caller should use batch hrad for y < y_full
}

// ============================================================================
// Dynamic Preissmann Slot (DPS) — geometry override
// Sharior et al. (2023) Eqs. 14, 15, 19
// ============================================================================

void DWSolver::applyDPSGeometry(SimulationContext& ctx) {
    auto& links = ctx.links;
    double g = GRAVITY;
    double c2 = dps_config_.c_pT_sq;

    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        auto uci = static_cast<std::size_t>(ci);

        if (is_open_[uj]) continue;

        double yf = links.xsect_y_full[uj];
        double af = links.xsect_a_full[uj];
        double rf = links.xsect_r_full[uj];
        double yMid = depth_mid_[uj];

        if (yMid > yf && af > 0.0) {
            // Eq. 14: Delta_As from volume excess above full conduit + prior slot
            double deltaAs = (area_mid_[uj] - af) - dps_.As[uci];

            // Eq. 19: Delta_hs = c_pT^2 * Delta_As / (g * A_C * P^2)
            double P2 = dps_.P[uci] * dps_.P[uci];
            double deltaHs = (P2 > 0.0) ? (c2 * deltaAs) / (g * af * P2) : 0.0;

            dps_.As[uci] += deltaAs;
            dps_.hs[uci] += deltaHs;

            // Hysteresis: if hs <= 0 but As > 0, treat as full-but-unpressurized
            if (dps_.hs[uci] < 0.0 && dps_.As[uci] > 0.0) {
                dps_.hs[uci] = 0.0;
            }

            // If fully depressurized (As <= 0), reset slot state
            if (dps_.As[uci] <= 0.0) {
                dps_.As[uci] = 0.0;
                dps_.hs[uci] = 0.0;
            }

            // Override areas with effective A = A_C + As (total including slot)
            double A_total = af + std::max(dps_.As[uci], 0.0);
            area_mid_[uj] = A_total;
            area1_[uj] = (depth1_[uj] > yf) ? A_total : area1_[uj];
            area2_[uj] = (depth2_[uj] > yf) ? A_total : area2_[uj];

            // Effective slot width for surface area continuity (Eq. 20, diagnostic)
            if (dps_.hs[uci] > 0.0 && std::abs(deltaHs) > 1e-30) {
                width_mid_[uj] = std::abs(deltaAs / deltaHs);
            } else if (dps_.hs[uci] > 0.0 && dps_.As[uci] > 0.0) {
                width_mid_[uj] = dps_.As[uci] / dps_.hs[uci];
            }

            // Friction excludes slot: hydraulic radius stays at full
            hrad_mid_[uj] = rf;
        } else if (!dps_.surcharged[uci] && dps_.As[uci] <= 0.0) {
            // Not surcharged and no residual slot area — no override needed
        }
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
            // Reset to initial P for unpressurized conduits (Eq. 23)
            dps_.P_hat[uci] = dps_.P_hat_0[uci];
            dps_.As[uci] = 0.0;
            dps_.hs[uci] = 0.0;
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
    // Unit conversion is already applied during input parsing.
    min_surf_area_ = (ctx.options.min_surf_area > 0.0)
        ? ctx.options.min_surf_area
        : constants::MIN_SURFAREA;

    auto un = static_cast<std::size_t>(n_nodes);
    auto ul = static_cast<std::size_t>(n_links);

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

    // Build conduit index list
    conduit_idx_.clear();
    conduit_idx_.reserve(ul);
    for (int j = 0; j < n_links; ++j) {
        if (ctx.links.type[static_cast<std::size_t>(j)] == LinkType::CONDUIT)
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
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        XsectShape shape = links.xsect_shape[uj];

        is_open_[uj] = (shape == XsectShape::RECT_OPEN ||
                        shape == XsectShape::TRAPEZOIDAL ||
                        shape == XsectShape::TRIANGULAR ||
                        shape == XsectShape::PARABOLIC);
        is_force_main_[uj] = (shape == XsectShape::FORCE_MAIN);
        has_losses_[uj] = (links.loss_inlet[uj] != 0.0 ||
                           links.loss_outlet[uj] != 0.0 ||
                           links.loss_avg[uj] != 0.0);
        barrels_d_[uj] = static_cast<double>(std::max(links.barrels[uj], 1));
        double len = links.mod_length[uj];
        if (len <= 0.0) len = links.length[uj];
        cached_length_[uj] = len;
        inv_length_[uj] = (len > 0.0) ? 1.0 / len : 0.0;
    }

    // Momentum category arrays
    category_.resize(ul, MomentumCategory::SKIP_DRY);

    // Phase A: build conduit-dense hot tile of timestep-invariant data.
    // Sized n_conduits_ so the inner loops index by ci with sequential
    // access — better L1 hit rate than sparse links.X[uj] reads.
    refreshConduitTile(ctx);

    // Anderson acceleration state arrays (allocated regardless; only used when enabled)
    aa_y_prev_.resize(un, 0.0);
    aa_g_prev_.resize(un, 0.0);
    aa_r_prev_.resize(un, 0.0);
    aa_skip_.resize(un, 0);

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
    tile_has_flap_gate_.resize(nc);
    tile_direction_.resize(nc);
    // Reverse map sized n_links_, -1 for non-conduits
    tile_uj_to_ci_.assign(static_cast<std::size_t>(n_links_), -1);

    const auto& links = ctx.links;
    const auto& nodes = ctx.nodes;
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        int j = conduit_idx_[uci];
        auto uj = static_cast<std::size_t>(j);
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
        tile_links_length_[uci]  = links.length[uj];
        tile_beta_[uci]          = links.beta[uj];
        tile_q_max_[uci]         = links.q_max[uj];
        tile_rough_factor_[uci]  = links.rough_factor[uj];
        tile_barrels_d_[uci]     = barrels_d_[uj];
        tile_is_open_[uci]       = is_open_[uj];
        tile_is_force_main_[uci] = is_force_main_[uj];
        tile_is_closed_[uci]     = links.is_closed[uj] ? 1 : 0;
        tile_has_losses_[uci]    = has_losses_[uj];
        tile_xsect_batch_shape_[uci] = links.xsect_batch_shape[uj];
        tile_shape_[uci]         = links.xsect_shape[uj];
        tile_has_offset_[uci]    = (links.offset1[uj] > 0.0 ||
                                    links.offset2[uj] > 0.0) ? 1 : 0;
        tile_culvert_code_[uci]  = links.culvert_code[uj];
        tile_slope_[uci]         = links.slope[uj];
        tile_q_limit_[uci]       = links.q_limit[uj];
        tile_loss_inlet_[uci]    = links.loss_inlet[uj];
        tile_loss_outlet_[uci]   = links.loss_outlet[uj];
        tile_has_flap_gate_[uci] = links.has_flap_gate[uj] ? 1 : 0;
        tile_direction_[uci]     = static_cast<int8_t>(links.direction[uj]);
        tile_uj_to_ci_[uj]       = ci;
    }
}

// ============================================================================
// setNumThreads — configure OpenMP parallelism
// ============================================================================

void DWSolver::setNumThreads(int n) {
    int max_threads = omp_get_max_threads();

    if (n == 0)
        num_threads_ = max_threads;
    else
        num_threads_ = std::min(n, max_threads);

    // Threshold: if fewer than 4 * num_threads conduits, overhead exceeds
    // benefit — fall back to single-threaded (matching legacy dynwave.c).
    if (n_links_ < 4 * num_threads_)
        num_threads_ = 1;
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

    // Save area_mid from PREVIOUS TIMESTEP for the unsteady momentum term.
    // This must happen ONCE per timestep, BEFORE the iteration loop, matching
    // legacy dynwave.c:280 which sets a2 = a1 in initRoutingStep().
    // area_mid_ still holds the final midpoint areas from the previous timestep.
    std::copy(area_mid_.begin(), area_mid_.end(), area_old_.begin());

    // Clear bypass flags at the start of each timestep
    // (matching legacy initRoutingStep: Link[i].bypassed = FALSE)
    std::fill(bypassed_.begin(), bypassed_.end(), uint8_t{0});

    while (steps < max_trials) {
        initNodeStates(ctx);

        // Step 0: Update outfall boundary depths each iteration
        // (matching legacy findNodeDepths line 592: link_setOutfallDepth per iteration)
        openswmm::outfall::setAllOutfallDepths(ctx, ctx.current_date);

        // Step 1: batch compute ALL cross-section geometry (with slot overrides)
        computeLinkGeometry(ctx);

        // Step 2: batch solve momentum for ALL conduit links
        solveMomentumBatch(ctx, dt, steps);

        // Step 3: scatter link flows to nodes
        // When non_conduit_fn is active, only scatter conduits here;
        // non-conduit flows will be scattered by the callback in Step 4.
        updateNodeFlows(ctx, /*conduits_only=*/ non_conduit_fn != nullptr);

        // Step 4: compute non-conduit flows (pumps, orifices, weirs, outlets)
        //         INSIDE the iteration loop, matching legacy dynwave.c:370-399
        //         findLinkFlows() which calls findNonConduitFlow() per iteration.
        if (non_conduit_fn) {
            non_conduit_fn(ctx, dt, steps);
        }

        // Step 5: flag nodes where AA must be skipped (non-smooth operator)
        computeAASkipFlags(ctx);

        // Step 6: update node depths, check convergence
        converged = updateNodeDepths(ctx, dt, steps);
        steps++;

        if (steps > 1) {
            if (converged) break;

            // Mark links whose both end nodes converged so they can be
            // skipped in the next iteration (matching legacy findBypassedLinks)
            findBypassedLinks(ctx);
        }
    }

    // Post-Picard: update per-node non-convergence counts (matching legacy
    // updateConvergenceStats: increment count for each unconverged node when
    // the overall step did not converge).
    if (!converged) {
        for (int i = 0; i < n_nodes_; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (!xnode_.converged[ui])
                ++ctx.nodes.stat_non_converged_count[ui];
        }
    }

    // Post-Picard: update DPS temporal state (P decay, surcharge tracking)
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        updateDPSState(ctx, dt);
    }

    return steps;
}

// ============================================================================
// initNodeStates
// ============================================================================

void DWSolver::initNodeStates(SimulationContext& ctx) {
    auto& nodes = ctx.nodes;
    const int unit_sys = ucf::getUnitSystem(
        static_cast<int>(ctx.options.flow_units));
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        xnode_.converged[ui] = 0;
        xnode_.sumdqdh[ui] = 0.0;

        // Surface area at current depth. For STORAGE this is the storage
        // curve area; for JUNCTION / DIVIDER / OUTFALL the helper returns
        // MIN_SURFAREA. Legacy initNodeStates uses node_getSurfArea() here
        // too (dynwave.c:303) when AllowPonding=NO — the only nominal
        // difference is legacy returns 0 for non-storage, relying on the
        // MAX(MinSurfArea) clamp in setNodeDepth; tried that once and it
        // destabilised the Rich_BC_CSO DW iterations (continuity drifted
        // from -2.5% to -14.4% — the phantom 50 ft² baseline is acting
        // as an extra damper at low-surface-area junctions). Kept.
        xnode_.new_surf_area[ui] = node::getSurfArea(nodes, i, nodes.depth[ui],
            &ctx.tables, unit_sys);

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
    // Only conduits participate in momentum solving; non-conduits are never bypassed
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        auto un1 = static_cast<std::size_t>(links.node1[uj]);
        auto un2 = static_cast<std::size_t>(links.node2[uj]);
        bypassed_[uj] = (xnode_.converged[un1] && xnode_.converged[un2]) ? 1 : 0;
    }
}

// ============================================================================
// computeLinkGeometry -- BATCH via XSectGroups with flow classification
// ============================================================================

/// Build XSectParams from link SoA data (with shape translation for batch API).
static XSectParams buildXSP(const LinkData& links, std::size_t uk) {
    XSectParams xs{};
    // Translate LinkData enum (CIRCULAR=0) to batch enum (CIRCULAR=1)
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

#if defined(SWMM_USE_OPENMP)
    const int nt_cg = (n_conduits_ >= 8 * num_threads_) ? num_threads_ : 1;
#endif

    // ---- STEP A + STEP B prep (fused): depths/heads + width-cap buffers ----
    // Phase B-1: collapse the previous two per-conduit passes into one. STEP B
    // prep wrote wcap_d1/d2/dm based on STEP A's freshly written depth1/2/mid
    // — now we compute and write wcap_d* directly from the depth values that
    // are still in registers, avoiding a second cache traversal.
    const bool slot_mode = (surcharge_method == SurchargeMethod::SLOT);
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(nt_cg) if(nt_cg > 1) schedule(static) \
    default(none) shared(ctx, links, nodes, slot_mode)
#endif
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        int j = tile_uj_[uci];
        auto uj = static_cast<std::size_t>(j);
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
    if (!slot_mode) {
        groups_->computeWidthsTriple(wcap_d1_.data(), wcap_d2_.data(), wcap_dm_.data(),
                                      width1_.data(), width2_.data(), width_mid_.data(),
                                      n_links_);
    } else {
        groups_->computeWidthsTriple(depth1_.data(), depth2_.data(), depth_mid_.data(),
                                     width1_.data(), width2_.data(), width_mid_.data(),
                                     n_links_);
    }

    // ---- STEP C: Flow classification + surface area (conduits only) ----
    // Matches legacy dwflow.c findSurfArea + getFlowClass.
    // Only links with offsets can trigger non-SUBCRITICAL classification,
    // so the expensive getYnorm/getYcrit Newton solves are rarely needed.
    // Per-conduit writes (surf_area1/2, flow_class, fasnh_, depth_mid_,
    // h1_/h2_, width_mid_) are single-producer so parallel-safe.
    // `schedule(dynamic, 64)` re-tested after the KMP_BLOCKTIME fix and
    // regressed by ~10 % (145 s vs 133 s) because libomp's per-chunk
    // atomic-increment overhead exceeds the load-imbalance benefit at
    // n_conduits ≈ 1 285. Static scheduling stays.
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(nt_cg) if(nt_cg > 1) schedule(static) \
    default(none) shared(ctx, links, nodes)
#endif
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        // Phase A: timestep-invariant data is read from the conduit-dense
        // tile (sequential cache lines). Only nodes.depth, links.flow, and
        // nodes.type are still read from the sparse SoA — those are the
        // only quantities that vary across Picard iters in this branch.
        int j = tile_uj_[uci];
        auto uj = static_cast<std::size_t>(j);
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
            if (!xs_built) { cached_xs = buildXSP(links, uj); xs_built = true; }
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
                surfArea2 = (wM + w2) * length * 0.5;
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
                surfArea1 = (w1 + wM) * length * 0.5;
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

    groups_->computeAreaHydRadTriple(p_d1, p_d2, p_dm,
                                     p_a1, p_a2, p_am,
                                     p_h1, p_hm, n_links_);
    // width_mid_ already computed in STEP B; UP/DN_CRITICAL/DRY cases
    // patched inline in STEP C.

    // ---- STEP E: Preissmann slot overrides (conduits only) ----
    //   Overrides area1/2/mid, width_mid, hrad_mid, hrad1 for surcharged
    //   conduits (depth > xsect_y_full). Applied AFTER the batch geometry
    //   kernels above so the slot overwrites whatever the shape kernel
    //   produced for above-full depth.
    if (surcharge_method == SurchargeMethod::DYNAMIC_SLOT) {
        // Dynamic Preissmann Slot: area-based transient storage (Sharior et al. 2023)
        applyDPSGeometry(ctx);
    } else {
        // Static slot (Sjoberg formula) or EXTRAN (no slot).
        // Per-conduit single-producer writes — safe to parallelise.
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(nt_cg) if(nt_cg > 1) schedule(static) \
    default(none) shared(links)
#endif
        for (int ci = 0; ci < n_conduits_; ++ci) {
            auto uci = static_cast<std::size_t>(ci);
            // Phase A: invariants from conduit-dense tile.
            int j = tile_uj_[uci];
            auto uj = static_cast<std::size_t>(j);

            double yf = tile_y_full_[uci];
            double af = tile_a_full_[uci];
            double rf = tile_r_full_[uci];
            double wm = tile_w_max_[uci];
            XsectShape shape = tile_shape_[uci];

            if (depth1_[uj] > yf) {
                double wSlot = getSlotWidth(depth1_[uj], yf, wm, shape);
                if (wSlot > 0.0) area1_[uj] = af + (depth1_[uj] - yf) * wSlot;
                // Upstream hyd-rad clamps to r_full once surcharged (legacy behaviour:
                // slot is narrow so wetted perimeter stays ~constant).
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
    }
}

// ============================================================================
// solveMomentumBatch -- category-classified dispatch for branch-free kernels
// ============================================================================

void DWSolver::solveMomentumBatch(SimulationContext& ctx, double dt, int step) {
    auto& links = ctx.links;

    // Pre-init conduit flows: copy current flow, zero dqdh
    // (non-conduit flows are handled by the non_conduit_fn callback).
    // Phase A: index by ci through the tile to keep the access pattern dense.
    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        auto uj = static_cast<std::size_t>(tile_uj_[uci]);
        new_flow_[uj] = links.flow[uj];
        dqdh_[uj] = 0.0;
    }

    // Classify each conduit into a momentum category.
    classifyMomentumCategories(ctx);

    // Single parallel-for over all conduits with per-element category dispatch.
#if defined(SWMM_USE_OPENMP)
    const int nt = (n_conduits_ >= 4 * num_threads_) ? num_threads_ : 1;
#pragma omp parallel for num_threads(nt) if(nt > 1) schedule(static) \
    default(none) shared(ctx, links, dt, step)
#endif
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);

        if (bypassed_[uj]) continue;

        MomentumCategory cat = category_[uj];
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
    }
}

// ============================================================================
// classifyMomentumCategories -- O(n_conduits) classification pass
// ============================================================================

void DWSolver::classifyMomentumCategories(SimulationContext& ctx) {
    auto& links = ctx.links;

    for (int ci = 0; ci < n_conduits_; ++ci) {
        auto uci = static_cast<std::size_t>(ci);
        // Phase A: read invariants from the conduit-dense tile.
        int j = tile_uj_[uci];
        auto uj = static_cast<std::size_t>(j);

        if (bypassed_[uj]) continue;

        FlowClass fc = links.flow_class[uj];
        double aMid = area_mid_[uj];
        double yf = tile_y_full_[uci];
        bool isFull = (depth1_[uj] >= yf && depth2_[uj] >= yf);

        MomentumCategory cat;
        if (fc == FlowClass::DRY || fc == FlowClass::UP_DRY ||
            fc == FlowClass::DN_DRY || aMid <= FUDGE || tile_is_closed_[uci]) {
            cat = MomentumCategory::SKIP_DRY;
        } else if (tile_is_force_main_[uci] && isFull) {
            cat = (links.roughness[uj] < 1.0)
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
    dqdh_[uj] = dt_g * aMid * tile_inv_length_[uci] * barrels_d;
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
    links.inlet_control[uj] = false;
    links.normal_flow_limited[uj] = false;
    if (q > 0.0) {
        if (tile_culvert_code_[uci] > 0 && !isFull) {
            double dqdh_culv = 0.0;
            double q_inlet = culvert::getInflow(
                q, h1, yf, tile_a_full_[uci],
                tile_slope_[uci], tile_culvert_code_[uci], dqdh_culv);
            if (q_inlet < q) {
                q = q_inlet;
                links.inlet_control[uj] = true;
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
                        double v1 = q / area1_[uj];
                        double w1 = width1_[uj];
                        double dh1 = (w1 > FUDGE) ? area1_[uj] / w1 : 0.0;
                        double f1 = (dh1 > 0.0) ? std::fabs(v1) / (SQRT_GRAVITY * std::sqrt(dh1)) : 0.0;
                        froude_check = (f1 >= 1.0);
                    }
                }
                if (slope_check || froude_check) {
                    double r1_for_norm = (hrad1_[uj] > FUDGE) ? hrad1_[uj] : FUDGE;
                    double s1 = area1_[uj] * fastmath::pow2_3(r1_for_norm);
                    double qNorm = tile_beta_[uci] * s1;
                    if (qNorm < q) {
                        q = qNorm;
                        links.normal_flow_limited[uj] = true;
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
    if (q < 0.0 && n2 >= 0 &&
        nodes.type[un2] == NodeType::OUTFALL &&
        nodes.outfall_has_flap_gate[un2]) {
        q = 0.0;
    }
    if (q > 0.0 && n1 >= 0 &&
        nodes.type[un1] == NodeType::OUTFALL &&
        nodes.outfall_has_flap_gate[un1]) {
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

    // Velocity (clamped)
    double v = qLast / aMid;
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
        if (depth_mid_[uj] > FUDGE && !isFull) {
            double dh = (wMid > FUDGE) ? aMid / wMid : 0.0;
            fr = (dh > 0.0) ? absv / (SQRT_GRAVITY * std::sqrt(dh)) : 0.0;
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
    double aWtd = area1_[uj] + (aMid - area1_[uj]) * rho;
    double rWtd = r1_val + (rMid - r1_val) * rho;
    rWtd = std::max(rWtd, FUDGE);

    // Apply InertDamping override AFTER rho computation
    if (!is_closed_full) {
        if      (inert_damping == 0) sig = 1.0;  // NO_DAMPING
        else if (inert_damping == 2) sig = 0.0;  // FULL_DAMPING
        // MANNING_CLOSED_FS: full damping when surcharged closed conduit
        if (!is_open_cat && isFull) sig = 0.0;
    }
    sigma_[uj] = sig;

    // Manning friction
    double r43 = fastmath::pow4_3(rWtd);
    double dq1 = (r43 > FUDGE) ? dt * tile_rough_factor_[uci] / r43 * absv : 0.0;

    // Head gradient
    double dq2 = dt_g * aWtd * (h2 - h1) * inv_len;

    // Unsteady + convective acceleration (skip if sig==0)
    double aOld = std::max(area_old_[uj], FUDGE);
    double dq3 = 0.0, dq4 = 0.0;
    if (sig > 0.0) {
        dq3 = 2.0 * v * (aMid - aOld) * sig;
        if (length > 0.0)
            dq4 = dt * v * v * (area2_[uj] - area1_[uj]) * inv_len * sig;
    }

    // Local losses
    double dq5 = 0.0;
    if (tile_has_losses_[uci]) {
        double absq = std::fabs(qLast);
        double losses = 0.0;
        if (area1_[uj] > FUDGE) losses += tile_loss_inlet_[uci] * (absq / area1_[uj]);
        if (area2_[uj] > FUDGE) losses += tile_loss_outlet_[uci] * (absq / area2_[uj]);
        if (aMid > FUDGE) losses += links.loss_avg[uj] * (absq / aMid);
        dq5 = losses * 0.5 * inv_len * dt;
    }

    // Evaporation/seepage. Length divisor uses RAW length (matching legacy
    // `dwflow.c:233`: `dq6 = ... / link_getLength(j)`, where
    // `link_getLength` returns `Conduit[k].length` (raw, not modLength).
    double dq6 = 0.0;
    {
        double conduit_length = tile_links_length_[uci];
        double loss_rate = links.evap_loss_rate[uj] + links.seep_loss_rate[uj];
        if (loss_rate > 0.0 && conduit_length > 0.0)
            dq6 = loss_rate * 2.5 * dt * v / conduit_length;
    }

    // Flow update
    double qOld = links.old_flow[uj] / barrels_d;
    double denom = 1.0 + dq1 + dq5;
    double q = (qOld - dq2 + dq3 + dq4 + dq6) / denom;
    dqdh_[uj] = (1.0 / denom) * dt_g * aWtd * inv_len * barrels_d;

    // Shared post-processing
    applyFlowLimits(ctx, dt, step, uj, q, qLast, barrels_d, isFull);
}

// ============================================================================
// processForceMainLink -- per-element kernel for FORCE_MAIN_HW/FORCE_MAIN_DW
// ============================================================================

void DWSolver::processForceMainLink(SimulationContext& ctx, double dt, int step,
                                    std::size_t uj, MomentumCategory cat) {
    auto& links = ctx.links;
    const double dt_g = dt_gravity_;
    const bool is_dw = (cat == MomentumCategory::FORCE_MAIN_DW);

    // Phase A extension: read invariants from the conduit-dense tile.
    auto uci = static_cast<std::size_t>(tile_uj_to_ci_[uj]);
    double barrels_d = tile_barrels_d_[uci];
    double inv_len = tile_inv_length_[uci];
    double aMid = area_mid_[uj];
    double rMid = hrad_mid_[uj];
    double qLast = links.flow[uj] / barrels_d;

    // Force main is always full (that's the classification condition)
    bool isFull = true;

    // Velocity (clamped)
    double v = qLast / aMid;
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

    // No upstream weighting when full (rho=1)
    double aWtd = area1_[uj] + (aMid - area1_[uj]);  // = aMid
    double rWtd = std::max(rMid, FUDGE);
    (void)rWtd;  // reserved for future friction variants

    // Force main friction
    double fm_coeff = links.roughness[uj];
    double sf;
    if (is_dw)
        sf = forcemain::getFricSlope_DW(v, rMid, fm_coeff);
    else
        sf = forcemain::getFricSlope_HW(v, rMid, fm_coeff);
    double dq1 = (absv > FUDGE) ? dt * GRAVITY * sf / absv : 0.0;

    // Head gradient
    double dq2 = dt_g * aWtd * (h2 - h1) * inv_len;

    // sig=0: no unsteady/convective terms (dq3=dq4=0)

    // Local losses
    double dq5 = 0.0;
    if (has_losses_[uj]) {
        double absq = std::fabs(qLast);
        double losses = 0.0;
        if (area1_[uj] > FUDGE) losses += links.loss_inlet[uj] * (absq / area1_[uj]);
        if (area2_[uj] > FUDGE) losses += links.loss_outlet[uj] * (absq / area2_[uj]);
        if (aMid > FUDGE) losses += links.loss_avg[uj] * (absq / aMid);
        dq5 = losses * 0.5 * inv_len * dt;
    }

    // Evaporation/seepage. Length divisor uses RAW length (matching legacy
    // `dwflow.c:233`: `dq6 = ... / link_getLength(j)`, where
    // `link_getLength` returns `Conduit[k].length` (raw, not modLength).
    double dq6 = 0.0;
    {
        double conduit_length = tile_links_length_[uci];
        double loss_rate = links.evap_loss_rate[uj] + links.seep_loss_rate[uj];
        if (loss_rate > 0.0 && conduit_length > 0.0)
            dq6 = loss_rate * 2.5 * dt * v / conduit_length;
    }

    // Flow update
    double qOld = links.old_flow[uj] / barrels_d;
    double denom = 1.0 + dq1 + dq5;
    double q = (qOld - dq2 + dq6) / denom;
    dqdh_[uj] = (1.0 / denom) * dt_g * aWtd * inv_len * barrels_d;

    // Shared post-processing
    applyFlowLimits(ctx, dt, step, uj, q, qLast, barrels_d, isFull);
}

// ============================================================================
// updateNodeFlows -- scatter link flows to nodes (matching legacy)
// ============================================================================

void DWSolver::updateNodeFlows(SimulationContext& ctx, bool conduits_only) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // When conduits_only, iterate conduit index directly (avoids checking
    // all n_links_ and skipping non-conduits). Non-conduit flows are
    // handled by the non_conduit_fn callback.
    const int loop_count = conduits_only ? n_conduits_ : n_links_;
    for (int idx = 0; idx < loop_count; ++idx) {
        int j = conduits_only
            ? conduit_idx_[static_cast<std::size_t>(idx)]
            : idx;
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
            double conduit_loss = (links.evap_loss_rate[uj] + links.seep_loss_rate[uj])
                                  * static_cast<double>(std::max(links.barrels[uj], 1));
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
        // (matching legacy dynwave.c updateNodeFlows: surfArea * barrels)
        int barrels = std::max(links.barrels[uj], 1);
        double b = static_cast<double>(barrels);
        xnode_.new_surf_area[un1] += sa1 * b;
        xnode_.new_surf_area[un2] += sa2 * b;
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

    // EXTRAN: skip AA for surcharged nodes (per-node check, no conduit walk)
    if (surcharge_method == SurchargeMethod::EXTRAN) {
        for (int i = 0; i < n_nodes_; ++i) {
            auto ui = static_cast<std::size_t>(i);
            if (xnode_.is_surcharged[ui])
                aa_skip_[ui] = 1;
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
            hcrown = nodes.invert_elev[un1] + links.crest_height[uj] + y_full;
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
// updateNodeDepths -- per-node, Picard convergence check
// ============================================================================

bool DWSolver::updateNodeDepths(SimulationContext& ctx, double dt, int step) {
    auto& nodes = ctx.nodes;
    const bool use_anderson = anderson_accel;

    // Phase 1: Compute G(y) for each node via setNodeDepth.
    // Each thread handles a subset of nodes; per-node data (xnode_, nodes.depth,
    // etc.) is written only by the owning thread (no cross-node dependencies).
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(num_threads_) schedule(static) default(none) \
    shared(nodes, ctx, dt, step, use_anderson)
#endif
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Skip outfalls (fixed boundary)
        if (nodes.type[ui] == NodeType::OUTFALL) {
            xnode_.converged[ui] = 1;
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
                    double alpha = std::max(0.0, std::min(1.0, r_k * dr / dr2));

                    // Anderson mixed update
                    double y_anderson = (1.0 - alpha) * aa_g_prev_[ui] + alpha * g_k;

                    // Physical bounds safeguard: depth must be >= 0
                    // Fall back to standard Picard if Anderson produces unphysical result
                    if (y_anderson >= 0.0) {
                        nodes.depth[ui] = y_anderson;
                        nodes.head[ui] = nodes.invert_elev[ui] + y_anderson;
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

        // Convergence check
        xnode_.converged[ui] = (std::fabs(nodes.depth[ui] - y_last) <= head_tol) ? 1 : 0;
    }

    // Sequential convergence check (matching legacy: separate pass after parallel region)
    int n_unconverged = 0;
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (nodes.type[ui] == NodeType::OUTFALL) continue;
        if (!xnode_.converged[ui]) n_unconverged++;
    }

    return (n_unconverged == 0);
}

// ============================================================================
// setNodeDepth -- single node depth update (EXTRAN surcharge algorithm)
// ============================================================================

void DWSolver::setNodeDepth(SimulationContext& ctx, int node_idx, double dt,
                            int step) {
    auto& nodes = ctx.nodes;
    auto ui = static_cast<std::size_t>(node_idx);

    // Cache `nodes.type[ui]` once at function entry (DWSOLVER_DATA_OPTIMIZATION
    // item 6). The compiler cannot prove no aliasing between nodes.type and the
    // subsequent writes to nodes.depth/volume/etc., so without this cache every
    // reference below would reload from memory.
    const NodeType ntype = nodes.type[ui];

    // --- Initialize ---
    double y_old = nodes.old_depth[ui];
    double y_last = nodes.depth[ui];
    double full_depth = nodes.full_depth[ui];
    double yCrown = nodes.crown_elev[ui] - nodes.invert_elev[ui];

    // Legacy dynwave.c:649: canPond = (AllowPonding && pondedArea > 0).
    // The global ALLOW_PONDING option must gate ponding; otherwise a node
    // with a non-zero ponded_area (set out of habit by modellers) will pond
    // against the user's intent and accumulate water above full_depth that
    // legacy would have discarded via the "add to losses" branch.
    bool can_pond = ctx.options.allow_ponding && (nodes.ponded_area[ui] > 0.0);
    bool is_ponded = (can_pond && y_last > full_depth);

    nodes.overflow[ui] = 0.0;
    double surf_area = xnode_.new_surf_area[ui];
    // Use the user-configured MIN_SURFAREA (resolved in init()) rather than
    // the compiled-in constants::MIN_SURFAREA — matches legacy
    // dynwave.c:658 `surfArea = MAX(surfArea, MinSurfArea)` where
    // MinSurfArea is the runtime value from the INP [OPTIONS] block.
    surf_area = std::max(surf_area, min_surf_area_);

    // --- Net flow volume change (trapezoidal averaging with previous step) ---
    double dQ = nodes.inflow[ui] - nodes.outflow[ui];
    double dV = 0.5 * (nodes.old_net_inflow[ui] + dQ) * dt;

    // --- Determine if node is surcharged (EXTRAN method, matching legacy) ---
    bool is_surcharged = false;
    if (surcharge_method == SurchargeMethod::EXTRAN) {
        // Ponded nodes don't surcharge
        if (is_ponded) {
            is_surcharged = false;
        }
        // Closed storage units that are full are in surcharge
        else if (ntype == NodeType::STORAGE) {
            is_surcharged = (nodes.sur_depth[ui] > 0.0 &&
                             y_last > full_depth);
        }
        // Surcharge occurs when node depth exceeds top of its highest link
        else {
            is_surcharged = (yCrown > 0.0 && y_last > yCrown);
        }
    }
    xnode_.is_surcharged[ui] = is_surcharged ? 1 : 0;

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
        // where dQ_net/dH = sumdqdh (positive: higher head ⟶ more net
        // outflow through connected links).  Substituting and rearranging:
        //
        //   dH = dV / (A - dt * sumdqdh / 2)
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

        double denom = surf_area - 0.5 * dt * xnode_.sumdqdh[ui];
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
        // Also used if storage node is surcharged but has no connecting links
        if (!is_surcharged ||
            (ntype == NodeType::STORAGE && xnode_.sumdqdh[ui] == 0.0)) {

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
            if (nodes.degree[ui] < 0) corr = 0.6;

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

    // --- Depth cannot be negative ---
    y_new = std::max(y_new, 0.0);

    // --- Determine max non-flooded depth ---
    double y_max = full_depth;
    if (!can_pond) y_max += nodes.sur_depth[ui];

    // --- Flooding logic (matching legacy getFloodedDepth) ---
    if (y_new > y_max) {
        if (!can_pond) {
            // Non-ponded flooding: cap at max, excess is overflow
            nodes.overflow[ui] = dV / dt;
            nodes.volume[ui] = nodes.full_volume[ui];
            y_new = y_max;
        } else {
            // Ponded: volume can exceed full volume
            nodes.volume[ui] = std::max(nodes.old_volume[ui] + dV,
                                        nodes.full_volume[ui]);
            nodes.overflow[ui] = (nodes.volume[ui] -
                std::max(nodes.old_volume[ui], nodes.full_volume[ui])) / dt;
        }
        if (nodes.overflow[ui] < FUDGE) nodes.overflow[ui] = 0.0;
    } else {
        nodes.volume[ui] = node::getVolume(nodes, node_idx, y_new, &ctx.tables,
            ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units)));
    }

    // --- Compute change in depth w.r.t. time (for CFL) ---
    if (dt > 0.0) {
        xnode_.dYdT[ui] = std::fabs(y_new - y_old) / dt;
    }

    // --- Save new depth ---
    nodes.depth[ui] = y_new;
    nodes.head[ui] = nodes.invert_elev[ui] + y_new;

    // Optional per-node Picard-iter trace (controlled by OPENSWMM_TRACE_NODE env var).
    {
        static const char* trace_name = std::getenv("OPENSWMM_TRACE_NODE");
        if (trace_name && ctx.node_names.name_of(node_idx) == trace_name) {
            std::fprintf(stderr,
                "REFAC %s step=%d t=%.3f y_old=%.6f y_last=%.6f "
                "surf_area=%.4f sumdqdh=%.6e dQ=%.6e dV=%.6e "
                "is_surch=%d y_new=%.6f overflow=%.6e volume=%.6e dt=%.4f\n",
                trace_name, step, ctx.current_time,
                y_old, y_last, surf_area, xnode_.sumdqdh[ui], dQ, dV,
                (int)is_surcharged, y_new, nodes.overflow[ui],
                nodes.volume[ui], dt);
        }
    }
}

// ============================================================================
// getRoutingStep -- CFL-based adaptive timestep
// ============================================================================

double DWSolver::getRoutingStep(SimulationContext& ctx,
                                 double fixed_step, double courant_factor) {
    if (courant_factor <= 0.0) return fixed_step;

    // On first call (no flows yet), use minimum step (matching legacy line 201-204:
    // "if (VariableStep == 0.0) VariableStep = MinRouteStep")
    if (variable_step_ <= 0.0) {
        variable_step_ = std::max(ctx.options.min_routing_step, MIN_TIMESTEP);
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

    // Update CFL-critical element counters (matching legacy stats_updateCriticalTimeCount)
    if (min_node >= 0) {
        ctx.nodes.stat_time_courant_critical[static_cast<std::size_t>(min_node)] += 1.0;
    } else if (min_link >= 0) {
        ctx.links.stat_time_courant_critical[static_cast<std::size_t>(min_link)] += 1.0;
    }

    // Apply user's minimum step (from MINIMUM_STEP option, typically 0.5 sec)
    double min_step = ctx.options.min_routing_step;
    min_step = std::max(min_step, MIN_TIMESTEP);
    dt_min = std::max(dt_min, min_step);
    // Round to milliseconds for deterministic behavior
    dt_min = std::floor(1000.0 * dt_min) / 1000.0;
    return dt_min;
}

double DWSolver::getLinkStep(const SimulationContext& ctx, int link_idx) const {
    auto uj = static_cast<std::size_t>(link_idx);
    if (ctx.links.type[uj] != LinkType::CONDUIT) return 1.0e10;

    // Match legacy getLinkStep (dynwave.c lines 846-856):
    // q = |newFlow| / barrels (per-barrel flow)
    int barrels = std::max(ctx.links.barrels[uj], 1);
    double q = std::fabs(ctx.links.flow[uj]) / static_cast<double>(barrels);
    if (q <= FUDGE) return 1.0e10;

    // Legacy: Conduit[k].a1 (midpoint area from solver, per barrel)
    double a = area_mid_[uj];
    if (a <= FUDGE) return 1.0e10;

    double fr = froude_[uj];
    if (fr <= 0.01) return 1.0e10;

    // Legacy: t = newVolume / barrels / q
    double vol = ctx.links.volume[uj] / static_cast<double>(barrels);
    double t = vol / q;

    // Apply modified length factor for short conduits / culverts
    // (matching legacy dynwave.c line 855: t *= modLength / length)
    double L = ctx.links.length[uj];
    double modL = ctx.links.mod_length[uj];
    if (L > 0.0 && modL > 0.0) {
        t *= modL / L;
    }

    t *= fr / (1.0 + fr);  // Froude-based CFL factor
    return t;              // CourantFactor applied per-link in getRoutingStep
}

} // namespace dynwave
} // namespace openswmm
