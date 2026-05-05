/**
 * @file TypeHelpers.hpp
 * @brief Shared C↔internal type mapping utilities for node and link type enums.
 *
 * @details The public C API uses SWMM_NodeType / SWMM_LinkType enums whose
 *          ordinal values differ from the internal NodeType / LinkType enums
 *          (STORAGE vs DIVIDER are swapped for nodes). These helpers centralise
 *          the mapping so that both the C API impl files and the edit layer
 *          (TypeConverter, ObjectDeleter) use the same translation.
 *
 * @ingroup engine_core
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_TYPE_HELPERS_HPP
#define OPENSWMM_ENGINE_TYPE_HELPERS_HPP

#include "../data/NodeData.hpp"
#include "../data/LinkData.hpp"
#include "../../../include/openswmm/engine/openswmm_nodes.h"
#include "../../../include/openswmm/engine/openswmm_links.h"

namespace openswmm {

// ============================================================================
// Node type mapping
// ============================================================================

inline bool c_to_internal_node_type(int c_type, NodeType& out_type) noexcept {
    switch (c_type) {
        case SWMM_NODE_JUNCTION: out_type = NodeType::JUNCTION; return true;
        case SWMM_NODE_OUTFALL:  out_type = NodeType::OUTFALL;  return true;
        case SWMM_NODE_STORAGE:  out_type = NodeType::STORAGE;  return true;
        case SWMM_NODE_DIVIDER:  out_type = NodeType::DIVIDER;  return true;
        default: return false;
    }
}

inline int internal_to_c_node_type(NodeType type) noexcept {
    switch (type) {
        case NodeType::JUNCTION: return SWMM_NODE_JUNCTION;
        case NodeType::OUTFALL:  return SWMM_NODE_OUTFALL;
        case NodeType::DIVIDER:  return SWMM_NODE_DIVIDER;
        case NodeType::STORAGE:  return SWMM_NODE_STORAGE;
        default:                 return SWMM_NODE_JUNCTION;
    }
}

// ============================================================================
// Link type mapping (C API and internal values match, but wrap for symmetry)
// ============================================================================

inline bool c_to_internal_link_type(int c_type, LinkType& out_type) noexcept {
    switch (c_type) {
        case SWMM_LINK_CONDUIT: out_type = LinkType::CONDUIT; return true;
        case SWMM_LINK_PUMP:    out_type = LinkType::PUMP;    return true;
        case SWMM_LINK_ORIFICE: out_type = LinkType::ORIFICE; return true;
        case SWMM_LINK_WEIR:    out_type = LinkType::WEIR;    return true;
        case SWMM_LINK_OUTLET:  out_type = LinkType::OUTLET;  return true;
        default: return false;
    }
}

inline int internal_to_c_link_type(LinkType type) noexcept {
    switch (type) {
        case LinkType::CONDUIT: return SWMM_LINK_CONDUIT;
        case LinkType::PUMP:    return SWMM_LINK_PUMP;
        case LinkType::ORIFICE: return SWMM_LINK_ORIFICE;
        case LinkType::WEIR:    return SWMM_LINK_WEIR;
        case LinkType::OUTLET:  return SWMM_LINK_OUTLET;
        default:                return SWMM_LINK_CONDUIT;
    }
}

} // namespace openswmm

#endif /* OPENSWMM_ENGINE_TYPE_HELPERS_HPP */
