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
#include <string>
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

CascadeResult analyze_pollutant_impact(const SimulationContext& ctx, int pollut_idx);
CascadeResult analyze_pattern_impact  (const SimulationContext& ctx, int pattern_idx);
CascadeResult analyze_aquifer_impact  (const SimulationContext& ctx, int aquifer_idx);
CascadeResult analyze_snowpack_impact (const SimulationContext& ctx, int snowpack_idx);
CascadeResult analyze_lid_impact      (const SimulationContext& ctx, int lid_idx);
CascadeResult analyze_street_impact   (const SimulationContext& ctx, int street_idx);
CascadeResult analyze_inlet_impact    (const SimulationContext& ctx, int inlet_idx);
CascadeResult analyze_landuse_impact  (const SimulationContext& ctx, int landuse_idx);
/// Unit-hydrograph groups are keyed by name (no stable index exists).
CascadeResult analyze_hydrograph_impact(const SimulationContext& ctx, const std::string& uh_name);

/**
 * @brief Find control rules whose text references an object by name.
 *
 * @details Scans each rule's premise/action clauses for an object-type
 *          keyword (NODE, LINK, CONDUIT, PUMP, ORIFICE, WEIR, OUTLET)
 *          followed by @p object_name (case-insensitive, matching legacy
 *          rule parsing). Read-only; rule text is never modified by any
 *          delete — the caller decides what to do with affected rules.
 *
 * @returns Indices into ControlRuleStore::rule_text, ascending.
 */
std::vector<int> find_control_rule_refs(const SimulationContext& ctx,
                                        const std::string& object_name);

// ============================================================================
// Deletion — mutates ctx
// ============================================================================

CascadeResult delete_node    (SimulationContext& ctx, int node_idx);
CascadeResult delete_link    (SimulationContext& ctx, int link_idx);
CascadeResult delete_subcatch(SimulationContext& ctx, int sc_idx);
CascadeResult delete_gage    (SimulationContext& ctx, int gage_idx);
CascadeResult delete_table   (SimulationContext& ctx, int table_idx);
CascadeResult delete_transect(SimulationContext& ctx, int transect_idx);

CascadeResult delete_pollutant(SimulationContext& ctx, int pollut_idx);
CascadeResult delete_pattern  (SimulationContext& ctx, int pattern_idx);
CascadeResult delete_aquifer  (SimulationContext& ctx, int aquifer_idx);
CascadeResult delete_snowpack (SimulationContext& ctx, int snowpack_idx);
CascadeResult delete_lid      (SimulationContext& ctx, int lid_idx);
CascadeResult delete_street   (SimulationContext& ctx, int street_idx);
CascadeResult delete_inlet    (SimulationContext& ctx, int inlet_idx);
CascadeResult delete_landuse  (SimulationContext& ctx, int landuse_idx);
CascadeResult delete_hydrograph(SimulationContext& ctx, const std::string& uh_name);

} // namespace openswmm::edit

#endif /* OPENSWMM_ENGINE_OBJECT_DELETER_HPP */
