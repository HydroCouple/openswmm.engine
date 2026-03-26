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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "DynamicWave.hpp"
#include "Node.hpp"
#include "XSectBatch.hpp"
#include "ForceMain.hpp"
#include "../core/SimulationContext.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>
#include <numeric>

// OpenMP support — graceful degradation when not available
#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#else
static inline int omp_get_max_threads() { return 1; }
#endif

namespace openswmm {
namespace dynwave {

static constexpr double GRAVITY = 32.2;
static constexpr double FUDGE   = 0.0001;

// ============================================================================
// Preissmann slot helpers (matching legacy dwflow.c)
// ============================================================================

double DWSolver::getCrownCutoff() const {
    if (surcharge_method == SurchargeMethod::SLOT)
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
        // Sjoberg formula
        return w_max * 0.5423 * std::exp(-std::pow(yNorm, 2.4));
    }

    // EXTRAN method: use fixed narrow slot when surcharged
    if (yNorm >= EXTRAN_CROWN_CUTOFF) {
        return y_full * SLOT_WIDTH_FACTOR;  // y_full / 1000
    }
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
// Init
// ============================================================================

void DWSolver::init(int n_nodes, int n_links, const XSectGroups& groups) {
    n_nodes_ = n_nodes;
    n_links_ = n_links;
    groups_  = &groups;

    auto un = static_cast<std::size_t>(n_nodes);
    auto ul = static_cast<std::size_t>(n_links);

    xnode_.resize(un);

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

int DWSolver::execute(SimulationContext& ctx, double dt) {
    int steps = 0;
    bool converged = false;

    while (steps < max_trials) {
        initNodeStates(ctx);

        // Save area from previous iteration for unsteady term
        if (steps == 0) {
            std::copy(area_mid_.begin(), area_mid_.end(), area_old_.begin());
        }

        // Step 1: batch compute ALL cross-section geometry (with slot overrides)
        computeLinkGeometry(ctx);

        // Step 2: batch solve momentum for ALL links
        solveMomentumBatch(ctx, dt, steps);

        // Step 3: scatter link flows to nodes
        updateNodeFlows(ctx);

        // Step 4: update node depths, check convergence
        converged = updateNodeDepths(ctx, dt, steps);
        steps++;

        if (steps > 1 && converged) break;
    }

    return steps;
}

// ============================================================================
// initNodeStates
// ============================================================================

void DWSolver::initNodeStates(SimulationContext& ctx) {
    auto& nodes = ctx.nodes;
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        xnode_[ui].converged = false;
        xnode_[ui].sumdqdh = 0.0;

        // Surface area at current depth
        xnode_[ui].new_surf_area = node::getSurfArea(nodes, i, nodes.depth[ui], &ctx.tables);

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
// computeLinkGeometry -- BATCH via XSectGroups with Preissmann slot
// ============================================================================

void DWSolver::computeLinkGeometry(SimulationContext& ctx) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    double crown_cutoff = getCrownCutoff();

    // Compute upstream/downstream flow depths from node heads and link offsets
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::CONDUIT) continue;

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);

        double z1 = nodes.invert_elev[un1] + links.offset1[uj];
        double z2 = nodes.invert_elev[un2] + links.offset2[uj];
        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];

        h1 = std::max(h1, z1);
        h2 = std::max(h2, z2);

        double y1 = h1 - z1;
        double y2 = h2 - z2;
        y1 = std::max(y1, FUDGE);
        y2 = std::max(y2, FUDGE);

        double yf = links.xsect_y_full[uj];

        // Preissmann slot: allow depth > yFull for closed conduits when using SLOT
        // For EXTRAN, clamp depth to yFull (surcharge handled via dQ/dH in setNodeDepth)
        if (surcharge_method != SurchargeMethod::SLOT) {
            y1 = std::min(y1, yf);
            y2 = std::min(y2, yf);
        }
        // For SLOT method, y1/y2 can exceed yFull -- slot geometry handles it

        depth1_[uj] = y1;
        depth2_[uj] = y2;
        depth_mid_[uj] = 0.5 * (y1 + y2);
    }

    // BATCH compute areas and hydraulic radii for ALL links at once
    // (these compute standard cross-section values; for depths > yFull,
    //  the batch functions will clamp at yFull internally)
    // Using raw data pointers with restrict semantics for the batch calls —
    // depth and result arrays do not alias each other within each call.
    double* __restrict__ p_d1  = depth1_.data();
    double* __restrict__ p_d2  = depth2_.data();
    double* __restrict__ p_dm  = depth_mid_.data();
    double* __restrict__ p_a1  = area1_.data();
    double* __restrict__ p_a2  = area2_.data();
    double* __restrict__ p_am  = area_mid_.data();
    double* __restrict__ p_hm  = hrad_mid_.data();
    double* __restrict__ p_wm  = width_mid_.data();

    groups_->computeAreas(p_d1, p_a1, n_links_);
    groups_->computeAreas(p_d2, p_a2, n_links_);
    groups_->computeAreas(p_dm, p_am, n_links_);
    groups_->computeHydRad(p_dm, p_hm, n_links_);
    groups_->computeWidths(p_dm, p_wm, n_links_);

    // Apply Preissmann slot overrides for surcharged conduits
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::CONDUIT) continue;

        double yf = links.xsect_y_full[uj];
        double af = links.xsect_a_full[uj];
        double rf = links.xsect_r_full[uj];
        double wm = links.xsect_w_max[uj];
        XsectShape shape = links.xsect_shape[uj];

        // Upstream end
        if (depth1_[uj] > yf) {
            double wSlot = getSlotWidth(depth1_[uj], yf, wm, shape);
            if (wSlot > 0.0) {
                area1_[uj] = af + (depth1_[uj] - yf) * wSlot;
            }
        }

        // Downstream end
        if (depth2_[uj] > yf) {
            double wSlot = getSlotWidth(depth2_[uj], yf, wm, shape);
            if (wSlot > 0.0) {
                area2_[uj] = af + (depth2_[uj] - yf) * wSlot;
            }
        }

        // Midpoint
        double yMid = depth_mid_[uj];
        if (yMid > yf) {
            double wSlot = getSlotWidth(yMid, yf, wm, shape);
            if (wSlot > 0.0) {
                area_mid_[uj] = af + (yMid - yf) * wSlot;
                width_mid_[uj] = wSlot;
            }
            // Hydraulic radius stays at R_full when surcharged
            hrad_mid_[uj] = rf;
        }
    }
}

// ============================================================================
// solveMomentumBatch -- vectorisable array arithmetic with surcharge handling
// ============================================================================

void DWSolver::solveMomentumBatch(SimulationContext& ctx, double dt, int step) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    // Restrict-qualified local pointers to internal arrays for vectorisation hints.
    // The compiler can assume these do not alias each other or the ctx arrays.
    double* __restrict__ p_area_mid  = area_mid_.data();
    double* __restrict__ p_area1     = area1_.data();
    double* __restrict__ p_area2     = area2_.data();
    double* __restrict__ p_hrad_mid  = hrad_mid_.data();
    double* __restrict__ p_width_mid = width_mid_.data();
    double* __restrict__ p_depth1    = depth1_.data();
    double* __restrict__ p_depth2    = depth2_.data();
    double* __restrict__ p_depth_mid = depth_mid_.data();
    double* __restrict__ p_velocity  = velocity_.data();
    double* __restrict__ p_froude    = froude_.data();
    double* __restrict__ p_sigma     = sigma_.data();
    double* __restrict__ p_dqdh      = dqdh_.data();
    double* __restrict__ p_new_flow  = new_flow_.data();
    double* __restrict__ p_area_old  = area_old_.data();

    // Compiler auto-vectorisation hint: iterations are independent
    // (each iteration reads/writes distinct array slots indexed by uj).
    // OpenMP parallelises the outer loop over links (matching legacy
    // dynwave.c::findLinkFlows). Each thread computes flow for a subset
    // of conduits — no cross-link data dependencies exist.
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(num_threads_) schedule(dynamic, 64) default(none) \
    shared(links, nodes, p_area_mid, p_area1, p_area2, p_hrad_mid, p_width_mid, \
           p_depth1, p_depth2, p_depth_mid, p_velocity, p_froude, p_sigma, \
           p_dqdh, p_new_flow, p_area_old, dt, step)
#endif
    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);
        if (links.type[uj] != LinkType::CONDUIT) {
            p_new_flow[uj] = links.flow[uj];
            p_dqdh[uj] = 0.0;
            continue;
        }

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);
        double length = links.length[uj];
        int barrels = links.barrels[uj];
        double barrels_d = static_cast<double>(std::max(barrels, 1));

        double aMid = p_area_mid[uj];
        double rMid = p_hrad_mid[uj];
        double qLast = links.flow[uj] / barrels_d;
        double yf = links.xsect_y_full[uj];

        // --- Determine if conduit is flowing full ---
        bool isFull = (p_depth1[uj] >= yf && p_depth2[uj] >= yf);

        // --- Check if conduit is dry or closed ---
        if (aMid <= FUDGE || links.is_closed[uj]) {
            p_area_mid[uj] = 0.5 * (p_area1[uj] + p_area2[uj]);
            p_dqdh[uj] = GRAVITY * dt * aMid / length * barrels_d;
            p_froude[uj] = 0.0;
            p_new_flow[uj] = 0.0;
            links.depth[uj] = std::min(p_depth_mid[uj], yf);
            links.volume[uj] = p_area_mid[uj] * length * barrels_d;
            continue;
        }

        // --- Velocity (clamped) ---
        double v = qLast / aMid;
        if (std::fabs(v) > MAXVELOCITY)
            v = (v > 0.0) ? MAXVELOCITY : -MAXVELOCITY;
        p_velocity[uj] = v;

        // --- Froude number ---
        double wMid = p_width_mid[uj];
        double dh = (wMid > FUDGE) ? aMid / wMid : 0.0;
        double fr = (dh > 0.0) ? std::fabs(v) / std::sqrt(GRAVITY * dh) : 0.0;
        p_froude[uj] = fr;

        // --- Inertial damping sigma ---
        double sig;
        if (fr <= 0.5)      sig = 1.0;
        else if (fr >= 1.0) sig = 0.0;
        else                sig = 2.0 * (1.0 - fr);

        // Determine if shape is open
        XsectShape shape = links.xsect_shape[uj];
        bool is_open = (shape == XsectShape::RECT_OPEN ||
                        shape == XsectShape::TRAPEZOIDAL ||
                        shape == XsectShape::TRIANGULAR ||
                        shape == XsectShape::PARABOLIC);

        // Use full inertial damping if closed conduit is surcharged
        // (matching legacy: sigma = 0 when full and not open)
        if (isFull && !is_open) sig = 0.0;
        p_sigma[uj] = sig;

        // --- Head values ---
        double h1 = nodes.depth[un1] + nodes.invert_elev[un1];
        double h2 = nodes.depth[un2] + nodes.invert_elev[un2];

        // --- Upstream weighting (R. Dickinson's slope weighting) ---
        double r1_val = p_hrad_mid[uj];  // approximation; use batch-computed
        double rho = 1.0;
        if (!isFull && qLast > 0.0 && h1 >= h2) rho = sig;
        double aWtd = p_area1[uj] + (aMid - p_area1[uj]) * rho;
        double rWtd = r1_val + (rMid - r1_val) * rho;
        if (rWtd < FUDGE) rWtd = FUDGE;

        // --- Momentum terms ---
        // dq1: friction slope
        double dq1;
        if (shape == XsectShape::FORCE_MAIN && isFull) {
            // Force main friction (P8-G10): use Hazen-Williams or Darcy-Weisbach
            // The force main coefficient (HW-C or DW roughness height) is stored
            // in links.roughness[] which is repurposed for force main conduits.
            // Default to Hazen-Williams; Darcy-Weisbach used when roughness < 1.0
            // (HW coefficients are typically 100-150, DW roughness is << 1)
            double fm_coeff = links.roughness[uj];
            double sf;
            if (fm_coeff < 1.0) {
                // Darcy-Weisbach: roughness is pipe roughness height (ft)
                sf = forcemain::getFricSlope_DW(v, rWtd, fm_coeff);
            } else {
                // Hazen-Williams: roughness is C coefficient
                sf = forcemain::getFricSlope_HW(v, rWtd, fm_coeff);
            }
            dq1 = dt * GRAVITY * sf;
        } else {
            // Standard Manning friction
            double r43 = std::pow(rWtd, 4.0 / 3.0);
            dq1 = (r43 > FUDGE) ? dt * links.rough_factor[uj] / r43 * std::fabs(v) : 0.0;
        }

        // dq2: pressure/gravity (head gradient)
        // When surcharged, the head gradient includes the pressurization term
        // (head above crown acts as pressure head driving the flow)
        double dq2 = dt * GRAVITY * aWtd * (h2 - h1) / length;

        // dq3: unsteady acceleration (if sigma > 0)
        double dq3 = 0.0;
        if (sig > 0.0) {
            dq3 = 2.0 * v * (aMid - p_area_old[uj]) * sig;
        }

        // dq4: convective acceleration (if sigma > 0)
        double dq4 = 0.0;
        if (sig > 0.0 && length > 0.0) {
            dq4 = dt * v * v * (p_area2[uj] - p_area1[uj]) / length * sig;
        }

        // --- Flow update ---
        double qOld = links.old_flow[uj] / barrels_d;
        double denom = 1.0 + dq1;
        double q = (qOld - dq2 + dq3 + dq4) / denom;

        // dQ/dH for node depth update
        p_dqdh[uj] = (1.0 / denom) * GRAVITY * dt * aWtd / length * barrels_d;

        // Under-relaxation (after first iteration)
        if (step > 0) {
            q = (1.0 - omega) * qLast + omega * q;
            // Prevent spurious flow reversal
            if (q * qLast < 0.0) q = 0.001 * ((q > 0.0) ? 1.0 : -1.0);
        }

        // Normal flow limitation (P8-G06): Q cannot exceed normal flow
        // Only for non-surcharged conduits (matching legacy checkNormalFlow)
        if (!isFull && links.beta[uj] > 0.0 && q > 0.0 && h1 >= h2) {
            double r1_for_norm = (p_hrad_mid[uj] > FUDGE) ? p_hrad_mid[uj] : FUDGE;
            double s1 = p_area1[uj] * std::pow(r1_for_norm, 2.0/3.0);
            double qNorm = links.beta[uj] * s1;
            if (q > qNorm) q = qNorm;
        }

        // --- Check user-supplied flow limit ---
        if (links.q_limit[uj] > 0.0) {
            if (std::fabs(q) > links.q_limit[uj])
                q = ((q > 0.0) ? 1.0 : -1.0) * links.q_limit[uj];
        }

        // --- Do not allow flow out of a dry node (legacy dwflow.c) ---
        if (q >  FUDGE && nodes.depth[un1] <= FUDGE) q =  FUDGE;
        if (q < -FUDGE && nodes.depth[un2] <= FUDGE) q = -FUDGE;

        // --- Save new values ---
        p_new_flow[uj] = q * barrels_d;

        // Update link depth and volume
        links.depth[uj] = std::min(p_depth_mid[uj], yf);
        double aMidAvg = (p_area1[uj] + p_area2[uj]) / 2.0;
        // Slot can have aMid > aFull, so do NOT clamp (matching legacy)
        links.volume[uj] = aMidAvg * length * barrels_d;
    }

    // Save area for next iteration's unsteady term
    std::copy(area_mid_.begin(), area_mid_.end(), area_old_.begin());
}

// ============================================================================
// updateNodeFlows -- scatter link flows to nodes (matching legacy)
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
        xnode_[un1].sumdqdh += dqdh_[uj];
        xnode_[un2].sumdqdh += dqdh_[uj];
    }
}

// ============================================================================
// updateNodeDepths -- per-node, Picard convergence check
// ============================================================================

bool DWSolver::updateNodeDepths(SimulationContext& ctx, double dt, int step) {
    auto& nodes = ctx.nodes;

    // Parallel node depth update (matching legacy dynwave.c::findNodeDepths).
    // Each thread handles a subset of nodes; per-node data (xnode_, nodes.depth,
    // etc.) is written only by the owning thread (no cross-node dependencies).
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(num_threads_) schedule(static) default(none) \
    shared(nodes, ctx, dt, step)
#endif
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Skip outfalls (fixed boundary)
        if (nodes.type[ui] == NodeType::OUTFALL) {
            xnode_[ui].converged = true;
            continue;
        }

        double y_last = nodes.depth[ui];
        setNodeDepth(ctx, i, dt, step);

        if (std::fabs(nodes.depth[ui] - y_last) > head_tol) {
            xnode_[ui].converged = false;
        } else {
            xnode_[ui].converged = true;
        }
    }

    // Sequential convergence check (matching legacy: separate pass after parallel region)
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (nodes.type[ui] == NodeType::OUTFALL) continue;
        if (!xnode_[ui].converged) return false;
    }
    return true;
}

// ============================================================================
// setNodeDepth -- single node depth update (EXTRAN surcharge algorithm)
// ============================================================================

void DWSolver::setNodeDepth(SimulationContext& ctx, int node_idx, double dt,
                            int step) {
    auto& nodes = ctx.nodes;
    auto ui = static_cast<std::size_t>(node_idx);

    // --- Initialize ---
    double y_old = nodes.old_depth[ui];
    double y_last = nodes.depth[ui];
    double full_depth = nodes.full_depth[ui];
    double yCrown = nodes.crown_elev[ui] - nodes.invert_elev[ui];

    bool can_pond = (nodes.ponded_area[ui] > 0.0);
    bool is_ponded = (can_pond && y_last > full_depth);

    nodes.overflow[ui] = 0.0;
    double surf_area = xnode_[ui].new_surf_area;
    if (surf_area < constants::MIN_SURFAREA)
        surf_area = constants::MIN_SURFAREA;

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
        else if (nodes.type[ui] == NodeType::STORAGE) {
            is_surcharged = (nodes.sur_depth[ui] > 0.0 &&
                             y_last > full_depth);
        }
        // Surcharge occurs when node depth exceeds top of its highest link
        else {
            is_surcharged = (yCrown > 0.0 && y_last > yCrown);
        }
    }
    xnode_[ui].is_surcharged = is_surcharged;

    double y_new;

    // --- Non-surcharged path: depth change based on surface area ---
    // Also used if storage node is surcharged but has no connecting links
    if (!is_surcharged ||
        (nodes.type[ui] == NodeType::STORAGE && xnode_[ui].sumdqdh == 0.0)) {

        double dy = dV / surf_area;
        y_new = y_old + dy;

        // Save non-ponded surface area for use in surcharge algorithm
        if (!is_ponded) {
            xnode_[ui].old_surf_area = surf_area;
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
        double denom = xnode_[ui].sumdqdh;
        if (y_last < 1.25 * yCrown && yCrown > 0.0) {
            double f = (y_last - yCrown) / yCrown;
            denom += (xnode_[ui].old_surf_area / dt -
                      xnode_[ui].sumdqdh) * std::exp(-15.0 * f);
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

    // --- Depth cannot be negative ---
    if (y_new < 0.0) y_new = 0.0;

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
        nodes.volume[ui] = node::getVolume(nodes, node_idx, y_new, &ctx.tables);
    }

    // --- Compute change in depth w.r.t. time (for CFL) ---
    if (dt > 0.0) {
        xnode_[ui].dYdT = std::fabs(y_new - y_old) / dt;
    }

    // --- Save new depth ---
    nodes.depth[ui] = y_new;
    nodes.head[ui] = nodes.invert_elev[ui] + y_new;
}

// ============================================================================
// getRoutingStep -- CFL-based adaptive timestep
// ============================================================================

double DWSolver::getRoutingStep(const SimulationContext& ctx,
                                 double fixed_step, double courant_factor) const {
    if (courant_factor <= 0.0) return fixed_step;

    double dt_min = fixed_step;

    // Link-based CFL
    for (int j = 0; j < n_links_; ++j) {
        double t = getLinkStep(ctx, j);
        if (t > 0.0 && t < dt_min) dt_min = t;
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

        double dYdT = xnode_[ui].dYdT;
        if (dYdT < FUDGE) continue;

        double t = max_depth / dYdT;
        if (t > 0.0 && t < dt_min) dt_min = t;
    }

    dt_min = std::max(dt_min, MINTIMESTEP);
    // Round to milliseconds
    dt_min = std::floor(1000.0 * dt_min) / 1000.0;
    return dt_min;
}

double DWSolver::getLinkStep(const SimulationContext& ctx, int link_idx) const {
    auto uj = static_cast<std::size_t>(link_idx);
    if (ctx.links.type[uj] != LinkType::CONDUIT) return 1.0e10;

    double q = std::fabs(ctx.links.flow[uj]);
    if (q <= FUDGE) return 1.0e10;

    double a = area_mid_[uj];
    if (a <= FUDGE) return 1.0e10;

    double fr = froude_[uj];
    if (fr <= 0.01) return 1.0e10;

    double vol = ctx.links.volume[uj];
    int barrels = ctx.links.barrels[uj];
    if (barrels > 1) vol /= static_cast<double>(barrels);

    double t = vol / q;
    t *= fr / (1.0 + fr);  // Courant factor
    return t;
}

} // namespace dynwave
} // namespace openswmm
