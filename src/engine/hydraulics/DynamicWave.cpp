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
#include "Outfall.hpp"
#include "XSectBatch.hpp"
#include "ForceMain.hpp"
#include "../core/Constants.hpp"
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

using constants::GRAVITY;
using constants::FUDGE;

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

    // Build conduit index list once (lazy init)
    if (conduit_idx_.empty() && n_links_ > 0) {
        conduit_idx_.reserve(static_cast<std::size_t>(n_links_));
        for (int j = 0; j < n_links_; ++j) {
            if (ctx.links.type[static_cast<std::size_t>(j)] == LinkType::CONDUIT)
                conduit_idx_.push_back(j);
        }
        n_conduits_ = static_cast<int>(conduit_idx_.size());
    }

    // Save area_mid from PREVIOUS TIMESTEP for the unsteady momentum term.
    // This must happen ONCE per timestep, BEFORE the iteration loop, matching
    // legacy dynwave.c:280 which sets a2 = a1 in initRoutingStep().
    // area_mid_ still holds the final midpoint areas from the previous timestep.
    std::copy(area_mid_.begin(), area_mid_.end(), area_old_.begin());

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

        // Step 5: update node depths, check convergence
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

    // ---- STEP A: Compute raw depths and heads per conduit link ----
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);

        double z1 = nodes.invert_elev[un1] + links.offset1[uj];
        double z2 = nodes.invert_elev[un2] + links.offset2[uj];
        double h1 = std::max(nodes.depth[un1] + nodes.invert_elev[un1], z1);
        double h2 = std::max(nodes.depth[un2] + nodes.invert_elev[un2], z2);

        double y1 = std::max(h1 - z1, FUDGE);
        double y2 = std::max(h2 - z2, FUDGE);

        double yf = links.xsect_y_full[uj];
        if (surcharge_method != SurchargeMethod::SLOT) {
            y1 = std::min(y1, yf);
            y2 = std::min(y2, yf);
        }

        depth1_[uj] = y1;
        depth2_[uj] = y2;
        depth_mid_[uj] = 0.5 * (y1 + y2);
        h1_[uj] = h1;
        h2_[uj] = h2;
        fasnh_[uj] = 1.0;
    }

    // ---- STEP B: Batch widths from depths (needed for surface area) ----
    // For EXTRAN mode, cap depth at CrownCutoff * yFull for width computation
    // (matching legacy getWidth() which caps at CrownCutoff to avoid near-zero
    //  widths at crown of closed conduits). Use temporary arrays so the real
    //  depths are preserved for area/momentum computation.
    if (surcharge_method != SurchargeMethod::SLOT) {
        // Build width-capped depth copies using pre-allocated buffers
        auto nl = static_cast<std::size_t>(n_links_);
        std::copy(depth1_.begin(), depth1_.begin() + nl, wcap_d1_.begin());
        std::copy(depth2_.begin(), depth2_.begin() + nl, wcap_d2_.begin());
        std::copy(depth_mid_.begin(), depth_mid_.begin() + nl, wcap_dm_.begin());
        for (int ci = 0; ci < n_conduits_; ++ci) {
            int j = conduit_idx_[static_cast<std::size_t>(ci)];
            auto uj = static_cast<std::size_t>(j);
            double yf = links.xsect_y_full[uj];
            XsectShape shape = links.xsect_shape[uj];
            bool is_open = (shape == XsectShape::RECT_OPEN ||
                            shape == XsectShape::TRAPEZOIDAL ||
                            shape == XsectShape::TRIANGULAR ||
                            shape == XsectShape::PARABOLIC);
            if (!is_open && yf > 0.0) {
                double yCap = EXTRAN_CROWN_CUTOFF * yf;
                if (wcap_d1_[uj] >= yCap) wcap_d1_[uj] = yCap;
                if (wcap_d2_[uj] >= yCap) wcap_d2_[uj] = yCap;
                if (wcap_dm_[uj] >= yCap) wcap_dm_[uj] = yCap;
            }
        }
        groups_->computeWidths(wcap_d1_.data(), width1_.data(), n_links_);
        groups_->computeWidths(wcap_d2_.data(), width2_.data(), n_links_);
        groups_->computeWidths(wcap_dm_.data(), width_mid_.data(), n_links_);
    } else {
        groups_->computeWidths(depth1_.data(), width1_.data(), n_links_);
        groups_->computeWidths(depth2_.data(), width2_.data(), n_links_);
        groups_->computeWidths(depth_mid_.data(), width_mid_.data(), n_links_);
    }

    // ---- STEP C: Flow classification + surface area (conduits only) ----
    // Matches legacy dwflow.c findSurfArea + getFlowClass.
    // Only links with offsets can trigger non-SUBCRITICAL classification,
    // so the expensive getYnorm/getYcrit are rarely needed.
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);

        int n1 = links.node1[uj];
        int n2 = links.node2[uj];
        auto un1 = static_cast<std::size_t>(n1);
        auto un2 = static_cast<std::size_t>(n2);

        double yf   = links.xsect_y_full[uj];
        double y1   = depth1_[uj];
        double y2   = depth2_[uj];
        double h1   = h1_[uj];
        double h2   = h2_[uj];
        double length = links.mod_length[uj];
        if (length <= 0.0) length = links.length[uj];

        double surfArea1 = 0.0, surfArea2 = 0.0;
        FlowClass fc = FlowClass::SUBCRITICAL;
        double fasnh = 1.0;

        // Both ends full → always SUBCRITICAL (skip expensive classification)
        if (y1 >= yf && y2 >= yf) {
            fc = FlowClass::SUBCRITICAL;
        } else {
            // ---- getFlowClass logic (legacy dwflow.c lines 294-409) ----
            double z1_off = links.offset1[uj];
            double z2_off = links.offset2[uj];
            // Adjust offsets for outfall nodes
            if (nodes.type[un1] == NodeType::OUTFALL)
                z1_off = std::max(0.0, z1_off - nodes.depth[un1]);
            if (nodes.type[un2] == NodeType::OUTFALL)
                z2_off = std::max(0.0, z2_off - nodes.depth[un2]);

            double qLast = std::fabs(links.flow[uj]);
            int barrels = std::max(links.barrels[uj], 1);
            double q = qLast / static_cast<double>(barrels);

            if (y1 > FUDGE && y2 > FUDGE) {
                // Both ends wet
                if (links.flow[uj] < 0.0) {
                    // Reverse flow: check upstream end
                    if (z1_off > 0.0) {
                        XSectParams xs = buildXSP(links, uj);
                        double yN = computeYnorm(xs, links.beta[uj], links.q_max[uj], q);
                        double yC = xsect::getYcrit(xs, q);
                        double ycMin = std::min(yN, yC);
                        if (y1 < ycMin) fc = FlowClass::UP_CRITICAL;
                    }
                } else {
                    // Normal flow: check downstream end
                    if (z2_off > 0.0) {
                        XSectParams xs = buildXSP(links, uj);
                        double yN = computeYnorm(xs, links.beta[uj], links.q_max[uj], q);
                        double yC = xsect::getYcrit(xs, q);
                        double ycMin = std::min(yN, yC);
                        double ycMax = std::max(yN, yC);
                        if (y2 < ycMin) {
                            fc = FlowClass::DN_CRITICAL;
                        } else if (y2 < ycMax) {
                            // Compute fasnh: fraction between normal & critical
                            if (ycMax - ycMin < FUDGE) fasnh = 0.0;
                            else fasnh = (ycMax - y2) / (ycMax - ycMin);
                        }
                    }
                }
            } else if (y1 <= FUDGE && y2 <= FUDGE) {
                fc = FlowClass::DRY;
            } else if (y2 > FUDGE) {
                // Upstream dry, downstream wet
                double z1_elev = nodes.invert_elev[un1] + links.offset1[uj];
                if (h2 < z1_elev) {
                    fc = FlowClass::UP_DRY;
                } else if (z1_off > 0.0) {
                    XSectParams xs = buildXSP(links, uj);
                    computeYnorm(xs, links.beta[uj], links.q_max[uj], q);
                    xsect::getYcrit(xs, q);
                    fc = FlowClass::UP_CRITICAL;
                }
            } else {
                // Upstream wet, downstream dry
                double z2_elev = nodes.invert_elev[un2] + links.offset2[uj];
                if (h1 < z2_elev) {
                    fc = FlowClass::DN_DRY;
                } else if (z2_off > 0.0) {
                    XSectParams xs = buildXSP(links, uj);
                    computeYnorm(xs, links.beta[uj], links.q_max[uj], q);
                    xsect::getYcrit(xs, q);
                    fc = FlowClass::DN_CRITICAL;
                }
            }
        }

        // ---- Apply flow classification to surface area and depths ----
        // (matching legacy findSurfArea cases)
        switch (fc) {
            case FlowClass::SUBCRITICAL: {
                double w1 = width1_[uj];
                double wM = width_mid_[uj];
                double w2 = width2_[uj];
                if (y1 >= yf && y2 >= yf) {
                    // Full: no surface area contribution
                } else if (width_mid_[uj] <= FUDGE) {
                    // Essentially dry
                } else {
                    surfArea1 = (w1 + wM) * length / 4.0;
                    surfArea2 = (wM + w2) * length / 4.0 * fasnh;
                }
                break;
            }
            case FlowClass::UP_CRITICAL: {
                // Clamp upstream depth to critical/normal
                XSectParams xs = buildXSP(links, uj);
                double q = std::fabs(links.flow[uj]) / std::max(links.barrels[uj], 1);
                double yN = computeYnorm(xs, links.beta[uj], links.q_max[uj], q);
                double yC = xsect::getYcrit(xs, q);
                y1 = yC;
                if (yN < yC) y1 = yN;
                y1 = std::max(y1, FUDGE);
                double z1_elev = nodes.invert_elev[un1] + links.offset1[uj];
                h1 = z1_elev + y1;
                double yMid = std::max(0.5 * (y1 + y2), FUDGE);
                double w2 = width2_[uj];
                double wM = xsect::getWofY(xs, yMid);
                surfArea2 = (wM + w2) * length * 0.5;
                // Update stored depths/heads for momentum equation
                depth1_[uj] = y1;
                depth_mid_[uj] = yMid;
                h1_[uj] = h1;
                break;
            }
            case FlowClass::DN_CRITICAL: {
                XSectParams xs = buildXSP(links, uj);
                double q = std::fabs(links.flow[uj]) / std::max(links.barrels[uj], 1);
                double yN = computeYnorm(xs, links.beta[uj], links.q_max[uj], q);
                double yC = xsect::getYcrit(xs, q);
                y2 = yC;
                if (yN < yC) y2 = yN;
                y2 = std::max(y2, FUDGE);
                double z2_elev = nodes.invert_elev[un2] + links.offset2[uj];
                h2 = z2_elev + y2;
                double yMid = std::max(0.5 * (y1 + y2), FUDGE);
                double w1 = width1_[uj];
                double wM = xsect::getWofY(xs, yMid);
                surfArea1 = (w1 + wM) * length * 0.5;
                depth2_[uj] = y2;
                depth_mid_[uj] = yMid;
                h2_[uj] = h2;
                break;
            }
            case FlowClass::UP_DRY: {
                y1 = FUDGE;
                double yMid = std::max(0.5 * (FUDGE + y2), FUDGE);
                double w1 = width1_[uj];
                double w2 = width2_[uj];
                double wM = width_mid_[uj];
                surfArea2 = (wM + w2) * length / 4.0;
                if (links.offset1[uj] <= 0.0)
                    surfArea1 = (w1 + wM) * length / 4.0;
                depth1_[uj] = FUDGE;
                depth_mid_[uj] = yMid;
                break;
            }
            case FlowClass::DN_DRY: {
                y2 = FUDGE;
                double yMid = std::max(0.5 * (y1 + FUDGE), FUDGE);
                double w1 = width1_[uj];
                double w2 = width2_[uj];
                double wM = width_mid_[uj];
                surfArea1 = (wM + w1) * length / 4.0;
                if (links.offset2[uj] <= 0.0)
                    surfArea2 = (w2 + wM) * length / 4.0;
                depth2_[uj] = FUDGE;
                depth_mid_[uj] = yMid;
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
    double* OPENSWMM_RESTRICT p_d1  = depth1_.data();
    double* OPENSWMM_RESTRICT p_d2  = depth2_.data();
    double* OPENSWMM_RESTRICT p_dm  = depth_mid_.data();
    double* OPENSWMM_RESTRICT p_a1  = area1_.data();
    double* OPENSWMM_RESTRICT p_a2  = area2_.data();
    double* OPENSWMM_RESTRICT p_am  = area_mid_.data();
    double* OPENSWMM_RESTRICT p_hm  = hrad_mid_.data();
    double* OPENSWMM_RESTRICT p_wm  = width_mid_.data();

    groups_->computeAreas(p_d1, p_a1, n_links_);
    groups_->computeAreas(p_d2, p_a2, n_links_);
    groups_->computeAreas(p_dm, p_am, n_links_);
    groups_->computeHydRad(p_dm, p_hm, n_links_);
    groups_->computeWidths(p_dm, p_wm, n_links_);

    // ---- STEP E: Preissmann slot overrides (conduits only) ----
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);

        double yf = links.xsect_y_full[uj];
        double af = links.xsect_a_full[uj];
        double rf = links.xsect_r_full[uj];
        double wm = links.xsect_w_max[uj];
        XsectShape shape = links.xsect_shape[uj];

        if (depth1_[uj] > yf) {
            double wSlot = getSlotWidth(depth1_[uj], yf, wm, shape);
            if (wSlot > 0.0) area1_[uj] = af + (depth1_[uj] - yf) * wSlot;
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

    // ---- STEP F: Upstream hyd-rad and slot override (conduits only) ----
    groups_->computeHydRad(p_d1, hrad1_.data(), n_links_);
    for (int ci = 0; ci < n_conduits_; ++ci) {
        int j = conduit_idx_[static_cast<std::size_t>(ci)];
        auto uj = static_cast<std::size_t>(j);
        if (depth1_[uj] > links.xsect_y_full[uj])
            hrad1_[uj] = links.xsect_r_full[uj];
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
    double* OPENSWMM_RESTRICT p_area_mid  = area_mid_.data();
    double* OPENSWMM_RESTRICT p_area1     = area1_.data();
    double* OPENSWMM_RESTRICT p_area2     = area2_.data();
    double* OPENSWMM_RESTRICT p_hrad_mid  = hrad_mid_.data();
    double* OPENSWMM_RESTRICT p_width_mid = width_mid_.data();
    double* OPENSWMM_RESTRICT p_depth1    = depth1_.data();
    double* OPENSWMM_RESTRICT p_depth2    = depth2_.data();
    double* OPENSWMM_RESTRICT p_depth_mid = depth_mid_.data();
    double* OPENSWMM_RESTRICT p_velocity  = velocity_.data();
    double* OPENSWMM_RESTRICT p_froude    = froude_.data();
    double* OPENSWMM_RESTRICT p_sigma     = sigma_.data();
    double* OPENSWMM_RESTRICT p_dqdh      = dqdh_.data();
    double* OPENSWMM_RESTRICT p_new_flow  = new_flow_.data();
    double* OPENSWMM_RESTRICT p_area_old  = area_old_.data();
    double* OPENSWMM_RESTRICT p_h1        = h1_.data();
    double* OPENSWMM_RESTRICT p_h2        = h2_.data();

    // Hoist options outside parallel region for OpenMP data sharing
    const int normal_flow_ltd = ctx.options.normal_flow_ltd;
    const int inert_damping = ctx.options.inertial_damping;  // 0=NONE, 1=PARTIAL, 2=FULL

    // Compiler auto-vectorisation hint: iterations are independent
    // (each iteration reads/writes distinct array slots indexed by uj).
    // OpenMP parallelises the outer loop over links (matching legacy
    // dynwave.c::findLinkFlows). Each thread computes flow for a subset
    // of conduits — no cross-link data dependencies exist.
#if defined(SWMM_USE_OPENMP)
#pragma omp parallel for num_threads(num_threads_) schedule(static) default(none) \
    shared(ctx, links, nodes, p_area_mid, p_area1, p_area2, p_hrad_mid, p_width_mid, \
           p_depth1, p_depth2, p_depth_mid, p_velocity, p_froude, p_sigma, \
           p_dqdh, p_new_flow, p_area_old, p_h1, p_h2, dt, step, normal_flow_ltd, inert_damping)
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
        // Use Courant-modified length for momentum equation
        // (matching legacy dwflow.c line 133: length = Conduit[k].modLength)
        double length = links.mod_length[uj];
        if (length <= 0.0) length = links.length[uj];
        int barrels = links.barrels[uj];
        double barrels_d = static_cast<double>(std::max(barrels, 1));

        double aMid = p_area_mid[uj];
        double rMid = p_hrad_mid[uj];
        double qLast = links.flow[uj] / barrels_d;
        double yf = links.xsect_y_full[uj];

        // --- Determine if conduit is flowing full ---
        bool isFull = (p_depth1[uj] >= yf && p_depth2[uj] >= yf);

        // --- Check if conduit is dry or closed ---
        // (matching legacy: zero flow for DRY, UP_DRY, DN_DRY flow classes)
        FlowClass fc = links.flow_class[uj];
        if (fc == FlowClass::DRY || fc == FlowClass::UP_DRY ||
            fc == FlowClass::DN_DRY || aMid <= FUDGE || links.is_closed[uj]) {
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
        if (std::fabs(v) > MAX_VELOCITY)
            v = (v > 0.0) ? MAX_VELOCITY : -MAX_VELOCITY;
        p_velocity[uj] = v;

        // --- Froude number (matching legacy link_getFroude) ---
        double wMid = p_width_mid[uj];
        double fr = 0.0;
        // Legacy: return 0 for empty links or closed conduits that are full
        if (p_depth_mid[uj] <= FUDGE) {
            fr = 0.0;
        } else if (isFull) {
            fr = 0.0;  // Closed conduit at full depth → Froude = 0 (legacy link_getFroude)
        } else {
            double dh = (wMid > FUDGE) ? aMid / wMid : 0.0;
            fr = (dh > 0.0) ? std::fabs(v) / std::sqrt(GRAVITY * dh) : 0.0;
        }
        p_froude[uj] = fr;

        // Reclassify SUBCRITICAL → SUPERCRITICAL when Froude > 1
        // (matching legacy dwflow.c line 186)
        if (fc == FlowClass::SUBCRITICAL && fr > 1.0)
            links.flow_class[uj] = FlowClass::SUPERCRITICAL;

        // --- Inertial damping sigma ---
        double sig;
        if (fr <= 0.5)      sig = 1.0;
        else if (fr >= 1.0) sig = 0.0;
        else                sig = 2.0 * (1.0 - fr);

        // Apply global InertDamping override (matching legacy dwflow.c lines 200-202)
        if      (inert_damping == 0) sig = 1.0;  // NO_DAMPING
        else if (inert_damping == 2) sig = 0.0;  // FULL_DAMPING
        // else PARTIAL (1): keep Froude-based sigma from above

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

        // --- Head values (from computeLinkGeometry, possibly modified by flow class) ---
        double h1 = p_h1[uj];
        double h2 = p_h2[uj];

        // --- Upstream weighting (R. Dickinson's slope weighting) ---
        // Use actual upstream hrad (matching legacy: r1 = getHydRad(xsect, y1))
        double r1_val = hrad1_[uj];
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

        // --- 5. local losses term (matching legacy dwflow.c findLocalLosses) ---
        double dq5 = 0.0;
        if (links.loss_inlet[uj] != 0.0 || links.loss_outlet[uj] != 0.0 ||
            links.loss_avg[uj] != 0.0) {
            double absq = std::fabs(qLast);
            double losses = 0.0;
            if (p_area1[uj] > FUDGE)
                losses += links.loss_inlet[uj] * (absq / p_area1[uj]);
            if (p_area2[uj] > FUDGE)
                losses += links.loss_outlet[uj] * (absq / p_area2[uj]);
            if (aMid > FUDGE)
                losses += links.loss_avg[uj] * (absq / aMid);
            dq5 = losses / 2.0 / length * dt;
        }

        // --- 6. evaporation and seepage momentum correction ---
        // (matching legacy dwflow.c line 233: dq6 = lossRate * 2.5 * dt * v / length)
        double dq6 = 0.0;
        {
            double conduit_length = links.length[uj];
            double loss_rate = links.seep_rate[uj];  // evap computed separately
            if (loss_rate > 0.0 && conduit_length > 0.0) {
                dq6 = loss_rate * barrels_d * 2.5 * dt * v / conduit_length;
            }
        }

        // --- Flow update ---
        double qOld = links.old_flow[uj] / barrels_d;
        double denom = 1.0 + dq1 + dq5;
        double q = (qOld - dq2 + dq3 + dq4 + dq6) / denom;

        // dQ/dH for node depth update
        p_dqdh[uj] = (1.0 / denom) * GRAVITY * dt * aWtd / length * barrels_d;

        // Normal flow limitation (matching legacy checkNormalFlow + NormalFlowLtd)
        // Only applies to SUBCRITICAL/SUPERCRITICAL flow (legacy dwflow.c line 252-254)
        // Legacy checks y1 < yFull (upstream depth only, not both ends)
        // 0=SLOPE, 1=FROUDE, 2=BOTH, 3=NEITHER
        FlowClass fc2 = links.flow_class[uj];
        int nfl = normal_flow_ltd;
        // Normal flow limiting (matching legacy checkNormalFlow in dwflow.c:634-683)
        if (nfl != 3 && p_depth1[uj] < yf &&
            (fc2 == FlowClass::SUBCRITICAL || fc2 == FlowClass::SUPERCRITICAL)) {
            int n1l = links.node1[uj], n2l = links.node2[uj];
            bool hasOutfall = false;
            if (n1l >= 0 && n2l >= 0) {
                hasOutfall = (ctx.nodes.type[static_cast<std::size_t>(n1l)] == NodeType::OUTFALL ||
                              ctx.nodes.type[static_cast<std::size_t>(n2l)] == NodeType::OUTFALL);
            }
            // Slope check: water surface slope < conduit slope when y1 < y2
            // (matching legacy: "if (y1 < y2) check = TRUE")
            bool slope_check = (nfl == 0 || nfl == 2 || hasOutfall) &&
                               (p_depth1[uj] < p_depth2[uj]);
            // Froude check at upstream end (matching legacy: f1 >= 1.0)
            bool froude_check = false;
            if (!slope_check && (nfl == 1 || nfl == 2) && !hasOutfall) {
                if (p_depth1[uj] > FUDGE && p_depth2[uj] > FUDGE) {
                    froude_check = (fr >= 1.0);
                }
            }
            if (slope_check || froude_check) {
                double r1_for_norm = (hrad1_[uj] > FUDGE) ? hrad1_[uj] : FUDGE;
                double s1 = p_area1[uj] * std::pow(r1_for_norm, 2.0/3.0);
                double qNorm = links.beta[uj] * s1;
                if (qNorm < q) {
                    q = qNorm;
                    links.normal_flow_limited[uj] = true;
                }
            }
        }

        // Under-relaxation (after first iteration)
        if (step > 0) {
            q = (1.0 - omega) * qLast + omega * q;
            // Prevent spurious flow reversal
            if (q * qLast < 0.0) q = 0.001 * ((q > 0.0) ? 1.0 : -1.0);
        }

        // --- Check user-supplied flow limit ---
        if (links.q_limit[uj] > 0.0) {
            if (std::fabs(q) > links.q_limit[uj])
                q = ((q > 0.0) ? 1.0 : -1.0) * links.q_limit[uj];
        }

        // --- Flap gate check: prevent reverse flow (matching legacy) ---
        if (links.has_flap_gate[uj]) {
            // node1 is upstream; if flow is negative (reverse), zero it
            if (q < 0.0) q = 0.0;
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

    // NOTE: area_old_ is set once at the start of execute() (step==0) to preserve
    // the previous timestep's midpoint area. It must NOT be overwritten here —
    // legacy uses Conduit[k].a2 (previous timestep) for the unsteady term (dq3)
    // throughout ALL Picard iterations within a single timestep.
}

// ============================================================================
// updateNodeFlows -- scatter link flows to nodes (matching legacy)
// ============================================================================

void DWSolver::updateNodeFlows(SimulationContext& ctx, bool conduits_only) {
    auto& links = ctx.links;
    auto& nodes = ctx.nodes;

    for (int j = 0; j < n_links_; ++j) {
        auto uj = static_cast<std::size_t>(j);

        // When non-conduit callback is active, skip non-conduits here
        // (they will be handled by the callback to avoid double-counting)
        if (conduits_only && links.type[uj] != LinkType::CONDUIT) continue;

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

        // Zero surface area for STORAGE nodes (matching legacy dynwave.c lines 507-510)
        // Storage nodes get their surface area from the storage curve, not from links.
        double sa1 = surf_area1_[uj];
        double sa2 = surf_area2_[uj];
        if (nodes.type[un1] == NodeType::STORAGE) sa1 = 0.0;
        if (nodes.type[un2] == NodeType::STORAGE) sa2 = 0.0;

        // Add conduit evap/seepage loss to node outflows (matching legacy lines 542-558)
        if (links.type[uj] == LinkType::CONDUIT) {
            double conduit_loss = links.seep_rate[uj] * static_cast<double>(std::max(links.barrels[uj], 1));
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
        xnode_[un1].new_surf_area += sa1 * b;
        xnode_[un2].new_surf_area += sa2 * b;
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
    int n_unconverged = 0;
    for (int i = 0; i < n_nodes_; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (nodes.type[ui] == NodeType::OUTFALL) continue;
        if (!xnode_[ui].converged) n_unconverged++;
    }

    // (convergence diagnostic removed)

    return (n_unconverged == 0);
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

    // On first call (no flows yet), use minimum step (matching legacy line 201-204:
    // "if (VariableStep == 0.0) VariableStep = MinRouteStep")
    if (variable_step_ <= 0.0) {
        variable_step_ = std::max(ctx.options.min_routing_step, MIN_TIMESTEP);
        return variable_step_;
    }

    double dt_min = fixed_step;

    // Link-based CFL (matching legacy getLinkStep with CourantFactor per-link)
    for (int j = 0; j < n_links_; ++j) {
        double t = getLinkStep(ctx, j);
        if (t <= 0.0) continue;
        // Apply Courant factor per-link (matching legacy dynwave.c line 856)
        t *= courant_factor;
        if (t < dt_min) dt_min = t;
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

    // Apply user's minimum step (from MINIMUM_STEP option, typically 0.5 sec)
    double min_step = ctx.options.min_routing_step;
    if (min_step < MIN_TIMESTEP) min_step = MIN_TIMESTEP;
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
