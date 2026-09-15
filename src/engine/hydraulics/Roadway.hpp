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
 * @brief Roadway weir overflow — FHWA HDS-5 (legacy roadway.c, op for op).
 *
 * @details Q = Cd * L * hWr^1.5 over the road crest. With a road width and
 *          surface the coefficient is the HDS-5 curve value: Cr from the
 *          HEAD (ft) when head / width <= 0.15, from the ratio above that,
 *          times the submergence factor Kt(ht / hWr); without them the
 *          user's Cd (SI decks: / 0.552).
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

/// legacy roadway.c RoadSurface: 0 = none (user Cd), 1 = PAVED, 2 = GRAVEL.
constexpr int SURFACE_NONE   = 0;
constexpr int SURFACE_PAVED  = 1;
constexpr int SURFACE_GRAVEL = 2;

/**
 * @brief legacy roadway_getInflow: flow across a roadway weir.
 *
 * @param dir         Flow direction (+1 or -1); the heads are already
 *                    swapped for reverse flow.
 * @param h_road      Road (crest) elevation (ft).
 * @param h1          Upstream head (ft).
 * @param h2          Downstream head (ft).
 * @param cd_user     The weir's discharge coefficient as written (Cd for
 *                    cfs; divided by 0.552 on an SI project).
 * @param si          True on an SI project.
 * @param road_width  Road width across the flow (ft); 0 = unknown.
 * @param road_surf   SURFACE_PAVED / SURFACE_GRAVEL / SURFACE_NONE.
 * @param length      Weir length (the section's wMax, ft).
 * @param[out] dqdh   dQ/dH (ft2/s), 0 when dry.
 * @param[out] depth  Link depth: max(h1 - h_road, 0).
 * @returns dir * q (cfs).
 */
double getInflow(double dir, double h_road, double h1, double h2,
                 double cd_user, bool si, double road_width, int road_surf,
                 double length, double& dqdh, double& depth);

} // namespace roadway
} // namespace openswmm

#endif // OPENSWMM_ROADWAY_HPP
