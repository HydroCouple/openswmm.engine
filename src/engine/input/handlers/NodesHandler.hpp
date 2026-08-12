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
 * @file NodesHandler.hpp
 * @brief Section handlers for node types: JUNCTIONS, OUTFALLS, DIVIDERS, STORAGE.
 * @see NodesHandler.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_NODES_HANDLER_HPP
#define OPENSWMM_ENGINE_NODES_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [JUNCTIONS] into NodeData + node_names. */
void handle_junctions(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [VIRTUAL_JUNCTIONS] — junction-typed nodes with is_virtual = 1.
 *  @details One token per line (the node name); geometry is derived from the
 *           two attached conduits in PostParseResolver. Extra tokens are a
 *           parse error (ERR_VJ_EXTRA_TOKENS). Refactored engine only. */
void handle_virtual_junctions(SimulationContext& ctx, const std::vector<std::string>& lines);

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
