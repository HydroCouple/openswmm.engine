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
 * @file      TypeConverter.hpp
 * @brief     Internal C++ API for in-place node and link type conversion.
 *
 * @details Provides conversion between node subtypes (JUNCTION/OUTFALL/STORAGE/
 *          DIVIDER) and between link subtypes (CONDUIT/PUMP/ORIFICE/WEIR/OUTLET).
 *          Common fields are preserved; type-specific fields are cleared and
 *          new-type defaults are applied.
 *
 * @ingroup engine_edit
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TYPE_CONVERTER_HPP
#define OPENSWMM_ENGINE_TYPE_CONVERTER_HPP

#include "../core/SimulationContext.hpp"
#include "../data/NodeData.hpp"
#include "../data/LinkData.hpp"
#include <string>
#include <vector>

namespace openswmm::edit {

// ============================================================================
// Conversion result
// ============================================================================

struct ConversionResult {
    int new_type;                            ///< C-API type enum value
    std::vector<std::string> cleared_fields; ///< Type-specific fields cleared
    std::vector<std::string> warnings;       ///< Non-fatal topology warnings
};

// ============================================================================
// Conversion functions
// ============================================================================

/**
 * @brief Replace one node subtype row and report cleared fields/warnings.
 *
 * Preserves common base properties and creates default subtype properties. Clears virtual/inlet
 * flags and storage LID anchors when applicable. Caller must validate before running; no cache
 * rebuild or transactional rollback.
 * @see swmm_node_convert().
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] idx Existing zero-based node index; validated by caller.
 * @param[in] new_type Supported internal NodeType different from the existing type; caller
 *     validates it.
 * @return Owned result with public C new_type code, cleared-field names and nonfatal topology
 *     warnings.
 */
ConversionResult convert_node(SimulationContext& ctx, int idx, NodeType new_type);

/**
 * @brief Replace one link subtype row and report cleared fields/warnings.
 *
 * Preserves common base properties and creates default subtype properties. Clears old conduit
 * section/pump token fields and seeds target-type defaults. Caller must validate before running; no
 * cache rebuild or transactional rollback.
 * @see swmm_link_convert().
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] idx Existing zero-based link index; validated by caller.
 * @param[in] new_type Supported internal LinkType different from the existing type; caller
 *     validates it.
 * @return Owned result with public C new_type code, cleared-field names and nonfatal topology
 *     warnings.
 */
ConversionResult convert_link(SimulationContext& ctx, int idx, LinkType new_type);

} // namespace openswmm::edit

#endif /* OPENSWMM_ENGINE_TYPE_CONVERTER_HPP */
