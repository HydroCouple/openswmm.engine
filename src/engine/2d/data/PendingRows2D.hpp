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
 * @file PendingRows2D.hpp
 * @brief Parse-time scratch rows for [2D_BOUNDARY_CONDITIONS] and
 *        [2D_EDGE_CONVEYANCE] input sections.
 *
 * @details Hoisted out of SurfaceRouter2D so serialization consumers
 *          (InpWriter, GeoPackage reader/writer) can reference the row
 *          types through SimulationContext::twod_io without including
 *          the router (this header is define-free and compiles in
 *          builds without OPENSWMM_HAS_2D).
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_PENDING_ROWS_HPP
#define OPENSWMM_ENGINE_2D_PENDING_ROWS_HPP

#include <string>

namespace openswmm::twoD {

/**
 * @brief Per-row buffer for `[2D_BOUNDARY_CONDITIONS]` parse output.
 *
 * V-E3. Populated by the input parser during reading (before the
 * mesh is finalized), drained into BoundaryData during
 * SurfaceRouter2D::initialize() after boundary_.resize() allocates
 * the per-edge slots. Retained after the drain (NOT cleared) so
 * writers can serialize the authored rows faithfully — the drained
 * BoundaryData form loses the group label and the TS-vs-constant
 * distinction for re-emission.
 */
struct PendingBoundaryRow {
    int         tri      = 0;
    int         edge     = 0;     ///< 0..2
    int         bc_type  = 0;     ///< openswmm::twoD::BoundaryType cast
    double      param1   = 0.0;   ///< slope (NormalFlow) / head (Stage) / flow (Flow)
    std::string name;             ///< TS name or curve name (TS_/Rating variants)
    std::string group;            ///< named group ("" = none)
};

/**
 * @brief Per-row buffer for `[2D_EDGE_CONVEYANCE]` parse output (§11A).
 *
 * Populated by the input parser during reading (vertices known but
 * mesh topology not yet built), drained into mesh edge_conveyance
 * during SurfaceRouter2D::initialize() after buildMeshTopology has
 * populated the neighbour table. Mirrored to both slots of an interior
 * edge so antisymmetric FV flux integration stays mass-conservative.
 * Retained after the drain for faithful re-serialization.
 */
struct PendingEdgeConveyanceRow {
    int    v_from     = 0;   ///< FROM_VERTEX index
    int    v_to       = 0;   ///< TO_VERTEX index (≠ v_from)
    double conveyance = 1.0; ///< Validated to [0, 1] at parse time
};

/**
 * @brief One `[2D_INITIAL_QUALITY]` row (overland transport S1/S2).
 *
 * @details `CELL <n> <species> <conc>`, `TAG <tag> <species> <conc>` or
 *          `* <species> <conc>`. Stored raw at parse: the mesh, its tags and
 *          the species table are all finalised AFTER the .inp is read, so
 *          resolution (and every fatal diagnostic — unknown species, unknown
 *          tag, cell out of range) happens in `SurfaceRouter2D::initialize`
 *          once the volumes exist to multiply against. Precedence at resolve
 *          is `* < TAG < CELL`, the D-I3 order the infiltration rows use.
 */
struct PendingInitialQualityRow {
    int         tri = -1;     ///< 0-based cell, or -1 for TAG / '*' scope
    std::string tag;          ///< tag name; empty with tri >= 0 or for '*'
    bool        all = false;  ///< the '*' spelling
    std::string species;      ///< name in [POLLUTANTS]
    double      conc = 0.0;   ///< species units (mass = conc x cell volume)
};

/// S2 — one `[2D_BOUNDARY_QUALITY]` row: `TRI EDGE SPECIES CONC`, the same
/// 0-based TRI / 0..2 EDGE spelling as `[2D_BOUNDARY_CONDITIONS]`. Gives the
/// concentration INFLOW through that boundary edge carries; outflow always
/// leaves at the cell's concentration, so no row is needed for it.
struct PendingBoundaryQualityRow {
    int         tri  = 0;
    int         edge = 0;     ///< 0..2
    std::string species;      ///< name in [POLLUTANTS]
    double      conc = 0.0;   ///< species units
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_PENDING_ROWS_HPP
