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
 * @file LegacyShapeBoundary.hpp
 * @brief Reconstructs an exact arc/line boundary for a handful of built-in
 *        shapes, so `[OPTIONS] XSECT_GEOMETRY EXACT` can evaluate them through
 *        the same piecewise-Chebyshev path (chebAll) as POLYGON.
 *
 * @details Scope is deliberately narrower than "every shape": a shape's
 *          LEGACY path already carries zero interpolation error when it is a
 *          closed-form formula (RECT_CLOSED, RECT_OPEN, TRAPEZOIDAL,
 *          TRIANGULAR, PARABOLIC, POWERFUNC, RECT_TRIANG, RECT_ROUND,
 *          MOD_BASKET all compute A/W/R analytically from geometry
 *          parameters, not from a table) — recompiling those buys nothing.
 *          EXACT mode has real accuracy to gain only where the LEGACY path is
 *          a linearly-interpolated TABLE:
 *            - CIRCULAR / FORCE_MAIN: reconstructed as a TRUE circle (four
 *              exact quarter-circle arcs — fromArcSpec requires n >= 3
 *              points, so two antipodal points cannot express it) rather
 *              than the 51-point A_Circ/W_Circ/R_Circ tables.
 *            - EGGSHAPED, HORSESHOE, GOTHIC, CATENARY, SEMIELLIPTICAL,
 *              BASKETHANDLE, SEMICIRCULAR, ARCH, HORIZ_ELLIPSE, VERT_ELLIPSE:
 *              reconstructed from each shape's OWN existing normalized width
 *              table (xsect_tables.hpp) as a symmetric polyline — the same
 *              source data test_cheb_section.cpp's "Legacy parity" test uses
 *              to validate a POLYGON reconstruction of EGG against its
 *              tabulated legacy A/R/W. This is table-driven, not invented
 *              geometry: it uses exactly the data the engine already ships,
 *              at the 21-to-26-point resolution EPA's own width tables provide, then
 *              lets the Chebyshev fit interpolate BETWEEN those points with
 *              a smooth, physically-motivated polynomial in place of
 *              linear segments.
 *          FILLED_CIRCULAR and IRREGULAR/CUSTOM/STREET_XSECT/POLYGON are out
 *          of scope here (the first needs a circular-segment construction not
 *          yet implemented; the rest are already per-instance tabulated data,
 *          not a shared built-in table, so "compiling the built-in" does not
 *          apply to them).
 *
 * @note Whether a returned boundary should be treated as an open channel is
 *       answered by the SAME classification `xsect::isOpen(int)` already
 *       uses (RECT_OPEN/TRAPEZOIDAL/TRIANGULAR/PARABOLIC/POWERFUNC/IRREGULAR)
 *       — none of the shapes this module supports are open, so the caller
 *       always compiles them with `is_open = false`.
 *
 * @see XSectBoundary.hpp for the exact arc/line primitives this builds on.
 * @see ChebSection.hpp for the compiler this boundary feeds.
 * @ingroup engine_hydraulics
 *
 * @author   Claude (Anthropic), for the openswmm.engine project
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_LEGACY_SHAPE_BOUNDARY_HPP
#define OPENSWMM_LEGACY_SHAPE_BOUNDARY_HPP

#include <vector>

#include "XSectBatch.hpp"
#include "XSectBoundary.hpp"

namespace openswmm::xsboundary {

/**
 * @brief Build an exact boundary for a built-in shape, if this module
 *        supports it.
 *
 * @param shape    the BATCH XSectShape code.
 * @param xs       XSectParams AFTER xsect::setParams() has already resolved
 *                 y_full/w_max/y_bot/r_bot/s_bot from the raw [XSECTIONS]
 *                 geometry — this function reads only already-derived
 *                 fields, never the raw geom1-4 array.
 * @param[out] out destination boundary chain (fromArcSpec/fromPolyline
 *                 output — CCW-oriented, min(y) == 0).
 * @returns true if @p shape is supported and @p out was populated;
 *          false if this shape's legacy path is already exact (or is
 *          otherwise out of this module's scope) and the caller should keep
 *          evaluating it through the ordinary table/formula dispatch.
 */
bool buildLegacyBoundary(XSectShape shape, const XSectParams& xs,
                         std::vector<BElem>& out);

} // namespace openswmm::xsboundary

#endif // OPENSWMM_LEGACY_SHAPE_BOUNDARY_HPP
