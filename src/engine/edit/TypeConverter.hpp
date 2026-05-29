/**
 * @file TypeConverter.hpp
 * @brief Internal C++ API for in-place node and link type conversion.
 *
 * @details Provides conversion between node subtypes (JUNCTION/OUTFALL/STORAGE/
 *          DIVIDER) and between link subtypes (CONDUIT/PUMP/ORIFICE/WEIR/OUTLET).
 *          Common fields are preserved; type-specific fields are cleared and
 *          new-type defaults are applied.
 *
 * @ingroup engine_edit
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    int                      new_type;        ///< C-API type enum value
    std::vector<std::string> cleared_fields;  ///< Type-specific fields cleared
    std::vector<std::string> warnings;        ///< Non-fatal topology warnings
};

// ============================================================================
// Conversion functions
// ============================================================================

/**
 * @brief Convert a node in-place to `new_type`.
 * @pre  idx is valid; engine must be in BUILDING or OPENED state (not enforced here).
 */
ConversionResult convert_node(SimulationContext& ctx, int idx, NodeType new_type);

/**
 * @brief Convert a link in-place to `new_type`.
 * @pre  idx is valid; engine must be in BUILDING or OPENED state (not enforced here).
 */
ConversionResult convert_link(SimulationContext& ctx, int idx, LinkType new_type);

} // namespace openswmm::edit

#endif /* OPENSWMM_ENGINE_TYPE_CONVERTER_HPP */
