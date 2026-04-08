/**
 * @file Link.hpp
 * @brief Link hydraulics — velocity, Froude, Manning conveyance, settings.
 *
 * @details Provides per-element and batch functions for conduit hydraulic
 *          computations. All formulas are numerically identical to legacy
 *          link.c and xsect.c.
 *
 * @note Legacy reference: src/legacy/engine/link.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_LINK_HPP
#define OPENSWMM_LINK_HPP

#include "../data/LinkData.hpp"
#include "XSectBatch.hpp"
#include "Node.hpp"

namespace openswmm {

namespace link {

// ============================================================================
// Per-element functions
// ============================================================================

/**
 * @brief Compute flow velocity from flow and depth.
 *
 * @details V = Q / (barrels * A(y)), where A is the cross-sectional area.
 *          Returns 0 if depth < 0.01 ft or area < FUDGE.
 *
 * @param xs       Cross-section parameters.
 * @param flow     Flow rate (ft3/s).
 * @param depth    Water depth (ft).
 * @param barrels  Number of identical barrels (default 1).
 * @returns Velocity (ft/s).
 */
double getVelocity(const XSectParams& xs, double flow, double depth, int barrels = 1);

/**
 * @brief Compute Froude number from velocity and depth.
 *
 * @details Fr = |V| / sqrt(g * D_h) where D_h = A/T (hydraulic depth).
 *          Returns 0 for dry or fully-full closed conduits.
 *
 * @param xs     Cross-section parameters.
 * @param v      Velocity (ft/s).
 * @param y      Water depth (ft).
 * @returns Froude number (dimensionless).
 */
double getFroude(const XSectParams& xs, double v, double y);

/**
 * @brief Compute Manning conveyance coefficients for a conduit.
 *
 * @details Sets:
 *   - beta = PHI * sqrt(|slope|) / roughness
 *   - rough_factor = GRAVITY * (roughness / PHI)^2
 *   - q_full = s_full * beta
 *
 * @param roughness Manning's n.
 * @param slope     Conduit slope (rise/run).
 * @param s_full    Section factor at full depth (ft^8/3).
 * @param[out] beta       Manning conveyance factor.
 * @param[out] rough_factor Head loss coefficient.
 * @param[out] q_full     Full-pipe flow rate (ft3/s).
 */
void computeConveyance(double roughness, double slope, double s_full,
                       double& beta, double& rough_factor, double& q_full);

/**
 * @brief Compute normal-flow depth from flow rate (inverse Manning).
 *
 * @details Given Q, find y such that Q = beta * S(y).
 *          S = Q / beta → A = getAofS(xs, S) → y = getYofA(xs, A).
 *
 * @param xs    Cross-section parameters.
 * @param beta  Manning conveyance factor.
 * @param q     Flow rate (ft3/s).
 * @returns Normal depth (ft).
 */
double getDepthFromFlow(const XSectParams& xs, double beta, double q);

/**
 * @brief Compute the capacity fraction (depth / full depth for conduits,
 *        setting for other links).
 */
double getCapacity(const XSectParams& xs, double depth);

/**
 * @brief Build XSectParams from LinkData SoA arrays.
 *
 * @param links  Link SoA data.
 * @param uj     Link index (size_t).
 * @returns Populated XSectParams struct.
 */
XSectParams buildXSectParams(const LinkData& links, std::size_t uj);

// ============================================================================
// Batch functions (for routing hot loop)
// ============================================================================

/**
 * @brief Compute velocity for all links in batch.
 *
 * @param links     SoA link data.
 * @param xs_groups Shape-grouped xsect manager (for area computation).
 * @param velocity  [out] Velocity array (indexed by link).
 */
void computeVelocities(const LinkData& links, double* velocity);

/**
 * @brief Compute Froude numbers for all conduit links.
 *
 * @param links   SoA link data.
 * @param velocity [in] Velocity array.
 * @param froude  [out] Froude number array (indexed by link).
 */
void computeFroude(const LinkData& links, const double* velocity, double* froude);

/**
 * @brief Compute Manning conveyance for all conduit links.
 *
 * @details Sets links.beta, links.rough_factor, links.q_full for each
 *          conduit link. Must be called once after slope/roughness/xsect
 *          are set (at init time, not each timestep).
 *
 * @param links  SoA link data (modified in place).
 */
void computeAllConveyance(LinkData& links);

/**
 * @brief Translate LinkData::XsectShape to batch XSectShape int code.
 *
 * @details The two enums have different orderings. This function provides
 *          a safe mapping for use wherever XSectParams.type is set from
 *          LinkData::xsect_shape.
 */
int translateShape(XsectShape link_shape);

} // namespace link

} // namespace openswmm

#endif // OPENSWMM_LINK_HPP
