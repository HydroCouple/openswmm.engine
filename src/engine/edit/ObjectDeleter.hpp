/**
 * @file ObjectDeleter.hpp
 * @brief Internal C++ API for object deletion and cascade analysis.
 *
 * @details Provides non-destructive impact analysis and destructive deletion
 *          for all major object types in a SimulationContext. All mutating
 *          functions require the context to be in BUILDING or OPENED state
 *          (enforced by the C ABI wrapper in openswmm_edit_impl.cpp, NOT here).
 *
 * @ingroup engine_edit
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_OBJECT_DELETER_HPP
#define OPENSWMM_ENGINE_OBJECT_DELETER_HPP

#include "../core/SimulationContext.hpp"
#include <vector>

namespace openswmm::edit {

// ============================================================================
// Cascade result
// ============================================================================

/**
 * @brief Describes one object that was impacted (deleted or nullified) during
 *        a cascade deletion.
 */
struct CascadeEntry {
    int         obj_type;  ///< SWMM_RefType value
    int         obj_idx;   ///< Zero-based index at the time of impact
    const char* field;     ///< Static field-name string literal
    bool        cascaded;  ///< true = deleted, false = reference nullified
};

/** @brief Aggregate cascade result for one deletion operation. */
struct CascadeResult {
    std::vector<CascadeEntry> entries;

    void add(int ot, int oi, const char* f, bool deleted) {
        entries.push_back({ot, oi, f, deleted});
    }
};

// ============================================================================
// Impact analysis — read-only
// ============================================================================

CascadeResult analyze_node_impact    (const SimulationContext& ctx, int node_idx);
CascadeResult analyze_link_impact    (const SimulationContext& ctx, int link_idx);
CascadeResult analyze_subcatch_impact(const SimulationContext& ctx, int sc_idx);
CascadeResult analyze_gage_impact    (const SimulationContext& ctx, int gage_idx);
CascadeResult analyze_table_impact   (const SimulationContext& ctx, int table_idx);
CascadeResult analyze_transect_impact(const SimulationContext& ctx, int transect_idx);

// ============================================================================
// Deletion — mutates ctx
// ============================================================================

CascadeResult delete_node    (SimulationContext& ctx, int node_idx);
CascadeResult delete_link    (SimulationContext& ctx, int link_idx);
CascadeResult delete_subcatch(SimulationContext& ctx, int sc_idx);
CascadeResult delete_gage    (SimulationContext& ctx, int gage_idx);
CascadeResult delete_table   (SimulationContext& ctx, int table_idx);
CascadeResult delete_transect(SimulationContext& ctx, int transect_idx);

} // namespace openswmm::edit

#endif /* OPENSWMM_ENGINE_OBJECT_DELETER_HPP */
