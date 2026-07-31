/**
 * @file SectionHandlers2D.hpp
 * @brief Input section parsers for the 2D surface routing module.
 *
 * @details Provides handler functions for the [2D_*] input sections:
 *          - [2D_OPTIONS]           — Solver options and tolerances
 *          - [2D_VERTICES]          — Mesh vertex coordinates
 *          - [2D_TRIANGLES]         — Triangle connectivity and roughness
 *          - [2D_VERTEX_NODE_MAP]   — Vertex-to-SWMM-node coupling
 *          - [2D_TRIANGLE_NODE_MAP] — Triangle-to-SWMM-node coupling
 *
 *          Each handler is registered via SectionRegistry::register_custom()
 *          and conforms to the SectionHandler signature.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §1
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP
#define OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../SurfaceRouter2D.hpp"
#include "../../input/SectionRegistry.hpp"

#include <string>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief Parse a single line from the [2D_OPTIONS] section.
 *
 * @param tokens   Whitespace-split tokens from the line.
 * @param opts     Output solver options to populate.
 * @param warnings When non-null (the .inp file-load path), keys retired with
 *                 the CVODE/ARKODE stack — and retired INTEGRATOR values —
 *                 are IGNORED with a WARNING 104 pushed here, so legacy
 *                 models still open. When null (the programmatic
 *                 swmm_options_set_ext path), they remain hard errors.
 * @return Empty string on success, or error description.
 */
std::string parse2DOptionsLine(const std::vector<std::string>& tokens,
                                SolverOptions2D& opts,
                                std::vector<std::string>* warnings = nullptr);

/**
 * @brief Parse a single line from the [2D_VERTICES] section.
 *
 * Format: X Y Z [TAG]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to append vertex to.
 * @return Empty string on success, or error description.
 */
std::string parse2DVertexLine(const std::vector<std::string>& tokens,
                               MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_TRIANGLES] section.
 *
 * Format: V1 V2 V3 MANNINGS_N [TAG]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to append triangle to.
 * @return Empty string on success, or error description.
 */
std::string parse2DTriangleLine(const std::vector<std::string>& tokens,
                                 MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_VERTEX_NODE_MAP] section.
 *
 * Format: VERTEX_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to update coupling map.
 * @return Empty string on success, or error description.
 */
std::string parse2DVertexNodeMapLine(const std::vector<std::string>& tokens,
                                      MeshData& mesh);

/**
 * @brief Parse a single line from the [2D_TRIANGLE_NODE_MAP] section.
 *
 * Format: TRIANGLE_INDEX_OR_TAG SWMM_NODE_NAME [CD] [AREA]
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param mesh   Mesh data to update coupling map.
 * @return Empty string on success, or error description.
 */
std::string parse2DTriangleNodeMapLine(const std::vector<std::string>& tokens,
                                        MeshData& mesh);

/**
 * @brief V-E3 — parse a single line from the [2D_BOUNDARY_CONDITIONS]
 *        section into a SurfaceRouter2D::PendingBoundaryRow appended to
 *        @p pending_rows.
 *
 * Format: TRI EDGE TYPE [PARAM_1 [PARAM_2 [GROUP]]]
 *   TYPE ∈ WALL / NORMAL_FLOW / SPECIFIED_STAGE / TS_STAGE /
 *          SPECIFIED_FLOW / TS_FLOW / RATING_CURVE
 *   PARAM_1 = slope / head / TS name / flow / TS name / curve name (by type)
 *   PARAM_2 reserved, always "*"
 *   GROUP   optional named group ("*" = none)
 *
 * Rows are drained into BoundaryData inside SurfaceRouter2D::initialize()
 * after boundary_.resize() has sized the per-edge slots.
 */
std::string parse2DBoundaryConditionsLine(
    const std::vector<std::string>& tokens,
    std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_rows);

/**
 * @brief §11A — parse a single `[2D_EDGE_CONVEYANCE]` line.
 *
 * Format: `FROM_VERTEX TO_VERTEX CONVEYANCE`
 *   FROM_VERTEX, TO_VERTEX : non-negative integer mesh-vertex indices.
 *                           Must differ. The pair is undirected.
 *   CONVEYANCE             : double in [0, 1] (strict, clamped at parse).
 *
 * Rows are accumulated into `pending` (a scratch buffer on
 * `SurfaceRouter2D`) and resolved against the mesh topology in
 * `SurfaceRouter2D::initialize()` after `buildMeshTopology` populates
 * the neighbour table.
 */
std::string parse2DEdgeConveyanceLine(
    const std::vector<std::string>& tokens,
    std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_rows);

/**
 * @brief True when @p key (case-insensitive) is a [2D_OPTIONS] parameter
 *        accepted by parse2DOptionsLine.
 *
 * Used by the swmm_options_get_ext / swmm_options_set_ext C API to route
 * these keys to the live SolverOptions2D (via SimulationContext::twod_io)
 * instead of the generic ext_options map — that routing is what makes
 * GUI/API edits of 2D options reach the solver and persist through the
 * InpWriter [2D_OPTIONS] emission and the GeoPackage 2D_* option keys.
 */
bool is2DOptionKey(const std::string& key);

/**
 * @brief True when @p key (case-insensitive) is a [2D_OPTIONS] parameter
 *        retired with the CVODE/ARKODE stack (D2, 2026-07-29).
 *
 * parse2DOptionsLine hard-errors on these unless given a warnings sink
 * (the file-load path warns and ignores them); swmm_options_set_ext uses
 * this predicate to reject them up front so they can never land in the
 * generic ext_options map (where they would silently persist to the next
 * save).
 */
bool is2DRetiredOptionKey(const std::string& key);

/**
 * @brief Format the current value of a [2D_OPTIONS] parameter as the
 *        string token parse2DOptionsLine accepts (round-trip safe).
 *
 * @param opts Solver options to read.
 * @param key  Parameter name (case-insensitive).
 * @return The value token, or an empty string for unknown keys
 *         (and for an unset OUTPUT_FILE, whose value token is optional).
 */
std::string format2DOptionValue(const SolverOptions2D& opts,
                                const std::string& key);

/**
 * @brief Register all 2D input section handlers with the section registry.
 *
 * Call during input reader setup (conditional on OPENSWMM_HAS_2D).
 * The handlers will populate the mesh and options data in SimulationContext.
 *
 * @param mesh     Mesh data to populate.
 * @param options  Solver options to populate.
 * @param registry Section registry to register handlers into.
 */
void register2DSections(MeshData& mesh,
                        SolverOptions2D& options,
                        std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                        std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                        input::SectionRegistry& registry);

/**
 * @brief Load 2D mesh sections from an external file.
 *
 * Opens the file referenced by @p mesh_file (resolved relative to
 * @p inp_base_dir if it is a relative path) and parses any
 * [2D_OPTIONS], [2D_VERTICES], [2D_TRIANGLES], [2D_VERTEX_NODE_MAP],
 * and [2D_TRIANGLE_NODE_MAP] sections found in it.
 *
 * @param mesh         Mesh data to populate.
 * @param opts         Solver options to populate.
 * @param mesh_file    Path from the [2D_MESH_FILE] FILE token.
 * @param inp_base_dir Directory of the parent .inp file (may be empty).
 * @param warnings     Optional sink for retired-key warnings (see
 *                     parse2DOptionsLine); pass &ctx.warnings so a legacy
 *                     external mesh file still loads.
 * @returns Empty string on success, or an error description on failure.
 */
std::string load2DMeshExternalFile(MeshData& mesh,
                                   SolverOptions2D& opts,
                                   std::vector<SurfaceRouter2D::PendingBoundaryRow>& pending_bc_rows,
                                   std::vector<SurfaceRouter2D::PendingEdgeConveyanceRow>& pending_ec_rows,
                                   const std::string& mesh_file,
                                   const std::string& inp_base_dir,
                                   std::vector<std::string>* warnings = nullptr);

/**
 * @brief Scan @p inp_path for a `;; UNITS: <value>` comment header and set
 *        @p opts.mesh_units_si to true when the value names metres.
 *
 * Recognised SI markers (case-insensitive): `SI (m)`, `m`, `metre`,
 * `metres`, `meter`, `meters`.  Any other value (or absent header) leaves
 * the flag unchanged.
 *
 * Safe to call multiple times; e.g. once on the inline `.inp` and again
 * on the resolved `.2dm` — the most recent observation wins, which is
 * the intended precedence (external file overrides inline).
 *
 * Quietly does nothing if the file cannot be opened — the caller already
 * reports the missing-file error via the normal read path.
 */
void prescan2DUnitsHeader(const std::string& inp_path, SolverOptions2D& opts);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP
