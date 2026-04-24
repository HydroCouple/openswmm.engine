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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP
#define OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP

#include "../data/MeshData.hpp"
#include "../data/SolverOptions2D.hpp"
#include "../../input/SectionRegistry.hpp"

#include <string>
#include <vector>

namespace openswmm::twoD {

/**
 * @brief Parse a single line from the [2D_OPTIONS] section.
 *
 * @param tokens Whitespace-split tokens from the line.
 * @param opts   Output solver options to populate.
 * @return Empty string on success, or error description.
 */
std::string parse2DOptionsLine(const std::vector<std::string>& tokens,
                                SolverOptions2D& opts);

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
 * @returns Empty string on success, or an error description on failure.
 */
std::string load2DMeshExternalFile(MeshData& mesh,
                                   SolverOptions2D& opts,
                                   const std::string& mesh_file,
                                   const std::string& inp_base_dir);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SECTION_HANDLERS_HPP
