/**
 * @file NodesHandler.hpp
 * @brief Section handlers for node types: JUNCTIONS, OUTFALLS, DIVIDERS, STORAGE.
 * @see NodesHandler.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_NODES_HANDLER_HPP
#define OPENSWMM_ENGINE_NODES_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [JUNCTIONS] into NodeData + node_names. */
void handle_junctions(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [OUTFALLS] — sets outfall-specific fields for nodes of type OUTFALL. */
void handle_outfalls(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [DIVIDERS] — sets divider-specific fields. */
void handle_dividers(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [STORAGE] — adds storage nodes and their geometry. */
void handle_storage(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [COORDINATES] — fills spatial.node_x / node_y. */
void handle_coordinates(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_NODES_HANDLER_HPP */
