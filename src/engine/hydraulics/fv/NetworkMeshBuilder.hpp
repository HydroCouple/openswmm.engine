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
 * @file      NetworkMeshBuilder.hpp
 * @brief     Build the FV cell/face mesh from a SimulationContext.
 *
 * @details Runs once at Router::init. The mesh is INTERNAL numerical
 *          discretization — it creates no named objects and is invisible to
 *          topology (plan §3.2). Virtual junctions are consumed here: their two
 *          conduit chains are concatenated into one continuous face graph, so
 *          mass and momentum flux continuity across them become properties of
 *          the scheme rather than a special treatment.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §3.2, §3.4
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_FV_NETWORK_MESH_BUILDER_HPP
#define OPENSWMM_ENGINE_FV_NETWORK_MESH_BUILDER_HPP

#include <string>
#include <vector>

#include "FvOptions.hpp"
#include "NetworkMeshData.hpp"

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::fv {

/// Diagnostics produced while building the mesh. Warnings are non-fatal and
/// surface through the normal ctx.warnings channel; errors abort the run.
struct MeshBuildReport {
    int n_cells = 0;
    int n_faces = 0;
    int n_conduits = 0;
    int n_virtual = 0; ///< virtual junctions spliced into interior faces
    double min_dx = 0.0;
    double max_dx = 0.0;
    std::vector<std::string> warnings;
    std::vector<std::string> errors;
};

/**
 * @brief Build the cell/face mesh and the per-conduit geometry closures.
 *
 * @param ctx     Model context — links, nodes and conduit side-table must be
 *                fully resolved, and Router::init must already have computed
 *                `mod_length`/`rough_factor` (the mesh reuses the Courant
 *                lengthening as its Δx floor).
 * @param opts    FV options (cell length, min cells, slot celerity).
 * @param mesh    [out] Cleared and filled.
 * @return Build report; a non-empty `errors` list means the mesh is unusable.
 */
MeshBuildReport buildNetworkMesh(SimulationContext& ctx, const FvOptions& opts,
                                 NetworkMeshData& mesh);

/**
 * @brief Construct an aggregate-barrel FV section and its tapered slot/closure.
 *
 * Depth/radius remain per-barrel, area/width multiply max(1,barrels). Closed slot uses g*A/c^2,
 * caps at5% full width (fallback width1ft) and floors nonpositive result1e-6ft; crown uses
 * SLOT_CROWN_CUTOFF. Calls buildClosure to replace provisional crown area/integral. No complete
 * finite/physical validation or ownership transfer.
 *
 * @param[in] xs Borrowed initialized native scalar section; optional table pointers copy as
 *     non-owning references and must remain live.
 * @param[in] is_open True extends the open section above full depth with vertical-wall width; false
 *     creates a pressurization slot.
 * @param[in] slot_celerity Design speed ft/s; closed-section calculation floors nonpositive/<=1/NaN
 *     to1 through comparison.
 * @param[in,out] g Borrowed destination overwritten with copied geometry, barrel-scaled area/width
 *     and constructed closure; unrelated fields not assigned here retain prior values.
 * @param[in] barrels Parallel barrel count; <=0 becomes1.
 */
void buildGeometry(const XSectParams& xs, bool is_open, double slot_celerity, FvGeometry& g,
                   int barrels = 1);

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_NETWORK_MESH_BUILDER_HPP
