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
 * @file      Culvert.hpp
 * @brief     Culvert inlet control — FHWA HEC-5 equations.
 *
 * @details Three flow regimes:
 *   - Unsubmerged (y <= 0.95*yFull): Q = AD * (h/yFull/K)^(1/M)
 *   - Transition:  linear interpolation
 *   - Submerged (y >= y2):  Q = AD * sqrt((h/yFull - Y + scf) / C)
 *
 *   Batch: classify all culverts by regime, compute flow per regime group.
 *
 * @note Legacy reference: src/legacy/engine/culvert.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_CULVERT_HPP
#define OPENSWMM_CULVERT_HPP

#include <vector>

namespace openswmm {

struct SimulationContext;
struct XSectParams;

namespace culvert {

/// Culvert curve coefficients per type code — legacy culvert.c Params[]:
/// the FHWA equation FORM (1 or 2) and K, M, C, Y.
struct CulvertCoeffs {
    double form = 0.0;
    double K = 0.0;
    double M = 0.0;
    double C = 0.0;
    double Y = 0.0;
};

/**
 * @brief Copy an FHWA culvert inlet coefficient record.
 *
 * @param[in] culvert_code Active coefficient-table code 1..57; every other value selects the
 *     inactive record.
 * @return Coefficient copy; all five members are zero for an invalid code.
 */
CulvertCoeffs getCoeffs(int culvert_code);

/**
 * @brief Apply an FHWA inlet-capacity limit to a proposed conduit flow.
 *
 * Uses the submerged equation above its FHWA threshold, unsubmerged equation at/below 0.95*y_full,
 * and a secant transition between. Form-1 root status is not propagated and no separate
 * finite/geometry validation or nonnegative clamp is performed. No pointer is retained.
 *
 * @param[in] q0 Positive proposed per-barrel flow in ft3/s.
 * @param[in] y Positive upstream head above the inlet invert in ft.
 * @param[in] xs Borrowed valid per-barrel geometry with positive full depth/area and valid Form-1
 *     width evaluations.
 * @param[in] slope Dimensionless conduit slope; correction is -7*slope for codes 5/37/46, otherwise
 *     0.5*slope.
 * @param[in] code FHWA inlet code 1..57; invalid codes return q0 with controls=false.
 * @param[in,out] dqdh Existing flow/head derivative in ft2/s; replaced only when inlet capacity is
 *     strictly below q0.
 * @param[out] controls Always reset to false; set true only if the computed inlet capacity is
 *     strictly below q0.
 * @return The computed inlet capacity when smaller than q0; otherwise q0.
 */
double getInflow(double q0, double y, const XSectParams& xs, double slope, int code, double& dqdh,
                 bool& controls);

} // namespace culvert
} // namespace openswmm

#endif // OPENSWMM_CULVERT_HPP
