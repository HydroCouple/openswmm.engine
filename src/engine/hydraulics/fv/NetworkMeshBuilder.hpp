/**
 * @file NetworkMeshBuilder.hpp
 * @brief Build the FV cell/face mesh from a SimulationContext.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    int  n_cells    = 0;
    int  n_faces    = 0;
    int  n_conduits = 0;
    int  n_virtual  = 0;   ///< virtual junctions spliced into interior faces
    double min_dx   = 0.0;
    double max_dx   = 0.0;
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
MeshBuildReport buildNetworkMesh(SimulationContext& ctx,
                                 const FvOptions& opts,
                                 NetworkMeshData& mesh);

/**
 * @brief Populate one conduit's cross-section closure, including the tapered
 *        Preissmann slot and the first-moment table.
 *
 * Exposed for the unit tests, which exercise the closure directly (continuity
 * of dA/dh through the crown, A↔h round-trip, the analytic I₁ extension).
 */
/// @param barrels  parallel identical barrels; area and top width are scaled
///                 by it so a cell is their aggregate section (hydraulic
///                 radius is per barrel and stays unscaled).
void buildGeometry(const XSectParams& xs, bool is_open, double slot_celerity,
                   FvGeometry& g, int barrels = 1);

} // namespace openswmm::fv

#endif // OPENSWMM_ENGINE_FV_NETWORK_MESH_BUILDER_HPP
