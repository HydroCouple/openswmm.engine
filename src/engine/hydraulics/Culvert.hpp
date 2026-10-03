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
 * @file Culvert.hpp
 * @brief Culvert inlet control — FHWA HEC-5 equations.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

/// Get coefficients for a culvert type code (1-57).
CulvertCoeffs getCoeffs(int culvert_code);

/**
 * @brief legacy culvert_getInflow (culvert.c): FHWA HDS-5 inlet-controlled
 *        flow through a culvert, as the dynamic wave applies it inside
 *        dwflow_findConduitFlow once per iteration.
 *
 * @param q0     The conduit's dynamic-wave flow (cfs, positive).
 * @param y      Upstream head above the culvert's upstream invert (ft):
 *               legacy `h - (Node[n1].invertElev + Link.offset1)`.
 * @param xs     The culvert's cross section (form1Eqn evaluates its area and
 *               top width at the trial critical depth).
 * @param slope  Conduit slope (the slope correction factor).
 * @param code   Culvert type code (1-57).
 * @param[out] dqdh      Legacy culvert.dQdH — replaces the link's dqdh when
 *                       the inlet controls.
 * @param[out] controls  TRUE when the inlet flow is below q0 (legacy
 *                       Link.inletControl).
 * @returns The inlet flow when it is below q0, else q0.
 */
double getInflow(double q0, double y, const XSectParams& xs, double slope,
                 int code, double& dqdh, bool& controls);

} // namespace culvert
} // namespace openswmm

#endif // OPENSWMM_CULVERT_HPP
