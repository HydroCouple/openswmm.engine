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
 * @file      Link.hpp
 * @brief     Link hydraulics — velocity, Froude, Manning conveyance, settings.
 *
 * @details Provides per-element and batch functions for conduit hydraulic
 *          computations. All formulas are numerically identical to legacy
 *          link.c and xsect.c.
 *
 * @note Legacy reference: src/legacy/engine/link.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_LINK_HPP
#define OPENSWMM_LINK_HPP

#include "../data/LinkData.hpp"
#include "XSectBatch.hpp"
#include "Node.hpp"
#include "Transect.hpp"

#include <vector>

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
void computeConveyance(double roughness, double slope, double s_full, double& beta,
                       double& rough_factor, double& q_full);

/**
 * @brief Compute normal-flow depth from flow rate (inverse Manning).
 *
 * @details Given Q, find y such that Q = beta * S(y).
 *          S = Q / beta → A = getAofS(xs, S) → y = getYofA(xs, A).
 *
 * @param xs    Cross-section parameters.
 * @param beta  Manning conveyance factor.
 * @param q     Flow rate (ft3/s).
 * @param q_max Conduit's flow at the maximum section factor; q is capped at it
 *              first, as legacy link_getYnorm does (link.c:800). getAofS caps
 *              the section factor at sMax anyway, but the tabular shapes take
 *              their inverse-lookup argument from the UNcapped psi = s/sFull,
 *              so an over-capacity q0 on an egg / horseshoe / gothic / catenary
 *              / semi-elliptical / basket-handle / semi-circular conduit reads
 *              the table past its end. Negative disables the cap.
 * @returns Normal depth (ft).
 */
double getDepthFromFlow(const XSectParams& xs, double beta, double q, double q_max = -1.0);

/**
 * @brief Compute the wetted-area fraction relative to full cross-section area.
 *
 * This helper reports area fraction, not depth fraction or regulator setting. It does not cap an
 * analytic open shape's above-full area ratio at1.
 *
 * @param[in] xs Borrowed valid native section with live optional tables.
 * @param[in] depth Finite nonnegative native water depth ft.
 * @return A(depth)/a_full, or0 when y_full<=0 or a_full<=0.
 */
double getCapacity(const XSectParams& xs, double depth);

/**
 * @brief Compute hydraulic power dissipated in a link.
 *
 * @details P = gamma * Q * hL where gamma = specific weight of water,
 *          Q = flow rate, hL = head loss across link.
 *          hL = friction slope * length (for conduits).
 *  @see Legacy: link_getPower() in link.c
 *
 * @param flow       Flow rate (ft3/s).
 * @param head_upstream   Head at upstream node (ft).
 * @param head_downstream Head at downstream node (ft).
 * @returns Power (ft-lbs/s). Divide by 550 for horsepower.
 */
double getHydPower(double flow, double head_upstream, double head_downstream);

/**
 * @brief Build XSectParams from LinkData SoA arrays.
 *
 * @param links      Link SoA data.
 * @param uj         Link index (size_t).
 * @param transects  Optional transect-table pool (ctx.transect_tables). When
 *                   supplied, IRREGULAR/CUSTOM/STREET_XSECT links get their
 *                   A/R/W table pointers attached so the per-element accessors
 *                   can evaluate tabulated geometry. Pass nullptr when the link
 *                   is known to be a self-contained shape.
 * @returns Populated XSectParams struct.
 */
XSectParams buildXSectParams(const LinkData& links, std::size_t uj,
                             const std::vector<transect::TransectData>* transects = nullptr);

// ============================================================================
// Batch functions (for routing hot loop)
// ============================================================================

// Phase 6: computeVelocities/computeFroude/computeAllConveyance removed (dead
// code; referenced conduit fields now in ConduitData — see LinkSubtypes.hpp).

/**
 * @brief Map model-storage section type to its distinct kernel numbering.
 *
 * @param[in] link_shape Storage XsectShape candidate; known types map by name.
 * @return Matching XSectShape integer code; DUMMY for DUMMY or unrecognized storage value.
 */
int translateShape(XsectShape link_shape);

/**
 * @brief Read the resolved sediment-depth adjustment for a filled circular conduit.
 *
 * Does not mutate offsets or validate the index; callers add/subtract this bump when crossing
 * authored/resolved offset conventions.
 *
 * @param[in] links Borrowed aligned link type/shape/bottom-depth columns.
 * @param[in] j Valid global link slot if resolved=true; not read when false.
 * @param[in] resolved True only after geometry resolution has applied the filled-bottom adjustment.
 * @return Stored xsect_y_bot in native ft for a resolved FILLED_CIRCULAR conduit, otherwise0.
 */
inline double filledCircularOffsetBump(const LinkData& links, std::size_t j, bool resolved) {
    if (!resolved)
        return 0.0;
    if (links.type[j] != LinkType::CONDUIT || links.xsect_shape[j] != XsectShape::FILLED_CIRCULAR)
        return 0.0;
    return links.xsect_y_bot[j];
}

/**
 * @brief Derive the full-flow properties of a tabulated cross-section.
 *
 * @details The counterpart of xsect::setParams() for the three shapes whose
 *          geometry comes from a transect table rather than a formula:
 *          IRREGULAR, CUSTOM and STREET_XSECT. Fills the y_full/a_full/r_full/
 *          w_max/s_full/s_max/a_bot/yw_max fields of @p xs from the built
 *          table, and attaches the table pointers.
 *
 *          Beyond a/r/w_full this sets the PHYSICAL s_max and a_bot (the area
 *          at the maximum section factor), which drive link_getYnorm's getAofS
 *          normal-depth solve and the flow limit q_max. Leaving s_max = s_full
 *          and a_bot = 0 diverges every transect whose section factor peaks
 *          below full depth.
 *
 * @note PARITY: legacy getTransectParams (xsect.c:1339-1358) for IRREGULAR /
 *       STREET_XSECT, and setCustomXsectParams (xsect.c:668-700) for CUSTOM.
 *       @p xs.type must already be set to the batch XSectShape code.
 *
 * @param xs      [in/out] Params to fill; `type` must be set on entry.
 * @param td      Built transect table (buildTables / buildCustomTables output).
 * @param y_full  Full depth (ft). Used only by CUSTOM, whose table is built at
 *                unit height and scaled here; ignored for IRREGULAR/STREET,
 *                which take y_full from @p td.
 * @param transect_idx  Index to record in `xs.transect`, or -1 when the table
 *                      is owned rather than pooled.
 */
void applyTabulatedXSectParams(XSectParams& xs, const transect::TransectData& td, double y_full,
                               int transect_idx);

} // namespace link

} // namespace openswmm

#endif // OPENSWMM_LINK_HPP
