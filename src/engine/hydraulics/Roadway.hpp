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
 * @file Roadway.hpp
 * @brief Roadway weir overflow — FHWA HDS-5.
 *
 * @details Q = Cd * L * hWr^1.5 where Cd depends on head/width ratio
 *          and road surface type (paved/gravel). Submergence correction
 *          factor Kt applied when tailwater is significant.
 *
 * @note Legacy reference: src/legacy/engine/roadway.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ROADWAY_HPP
#define OPENSWMM_ROADWAY_HPP

namespace openswmm {

namespace roadway {

enum class SurfaceType : int {
    PAVED  = 0,
    GRAVEL = 1
};

/**
 * @brief Compute roadway weir overflow.
 *
 * @param head_up    Upstream head above road crest (ft).
 * @param head_down  Downstream head above road crest (ft).
 * @param road_width Road width (ft).
 * @param road_length Crest length perpendicular to flow (ft).
 * @param surface    Road surface type.
 * @param[out] dqdh  Derivative dQ/dH.
 * @returns Overflow rate (cfs).
 */
double getFlow(double head_up, double head_down,
               double road_width, double road_length,
               SurfaceType surface, double& dqdh);

/**
 * @brief Get discharge coefficient Cr from head/width ratio.
 *
 * @param hw_ratio  head / road_width.
 * @param surface   Paved or gravel.
 * @returns Discharge coefficient.
 */
double getDischargeCoeff(double hw_ratio, SurfaceType surface);

/**
 * @brief Get submergence correction factor Kt.
 *
 * @param ht_ratio  head_down / head_up.
 * @param surface   Paved or gravel.
 * @returns Submergence factor (0-1).
 */
double getSubmergenceFactor(double ht_ratio, SurfaceType surface);

} // namespace roadway
} // namespace openswmm

#endif // OPENSWMM_ROADWAY_HPP
