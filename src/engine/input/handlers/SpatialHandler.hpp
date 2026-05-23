/**
 * @file SpatialHandler.hpp
 * @brief Section handlers for spatial sections: [MAP], [VERTICES], [POLYGONS].
 * @see SpatialHandler.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SPATIAL_HANDLER_HPP
#define OPENSWMM_ENGINE_SPATIAL_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [MAP] — fills spatial.map_x1/y1/x2/y2 and map_units. */
void handle_map(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [VERTICES] — fills spatial.link_vertices_x/y. */
void handle_vertices(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [POLYGONS] — fills spatial.subcatch_polygon_x/y. */
void handle_polygons(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [SYMBOLS] — fills spatial.gage_x/y. */
void handle_symbols(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [TAGS] — fills node_tags, link_tags, subcatch_tags. */
void handle_tags(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_SPATIAL_HANDLER_HPP */
