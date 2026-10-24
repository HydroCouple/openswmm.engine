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
 * @file      ObjectDeleter.hpp
 * @brief     Internal C++ API for object deletion and cascade analysis.
 *
 * @details Provides non-destructive impact analysis and destructive deletion
 *          for all major object types in a SimulationContext. All mutating
 *          functions require the context to be in BUILDING or OPENED state
 *          (enforced by the C ABI wrapper in openswmm_edit_impl.cpp, NOT here).
 *
 * @ingroup engine_edit
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_edit Model editing and reference maintenance
 * @brief Analysis, conversion and deletion of authored model objects, including references and cascade impacts. C ABI entry points enforce editable lifecycle states; internal helpers document their mutation and ownership contracts.
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
    int obj_type;      ///< SWMM_RefType value
    int obj_idx;       ///< Zero-based index at the time of impact
    const char* field; ///< Static field-name string literal
    bool cascaded;     ///< true = deleted, false = reference nullified
};

/** @brief Aggregate cascade result for one deletion operation. */
struct CascadeResult {
    std::vector<CascadeEntry> entries;

    /**
     * @brief Append one owned impact descriptor with a borrowed static field label.
     *
     * No validation/deduplication; vector allocation exceptions propagate.
     *
     * @param[in] ot SWMM_RefType category code.
     * @param[in] oi Referencing object index at this stage of analysis/deletion; may be -1 for global
     *     option references.
     * @param[in] f Static NUL-terminated field-name literal; stored by pointer and must outlive the
     *     report.
     * @param[in] deleted True marks cascade deletion, false reference nullification/report-only impact.
     */
    void add(int ot, int oi, const char* f, bool deleted) {
        entries.push_back({ot, oi, f, deleted});
    }
};

// ============================================================================
// Impact analysis — read-only
// ============================================================================

/**
 * @brief Inspect references to a node.
 *
 * Scans current incoming references without mutating ctx. Covers attached links, inflow/DWF/RDII
 * rows, subtype/reaction references and spatial rows. Returned report predicts supported reference
 * changes rather than certifying model validity.
 * @see swmm_node_analyze_impact() for the checked
 * C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] node_idx Existing zero-based node index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_node_impact(const SimulationContext& ctx, int node_idx);
/**
 * @brief Inspect references to a link.
 *
 * Scans current incoming references without mutating ctx. Covers divider/inlet usage references,
 * subtype/reaction rows and spatial geometry. Returned report predicts supported reference changes
 * rather than certifying model validity.
 * @see swmm_link_analyze_impact() for the checked C
 * wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] link_idx Existing zero-based link index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_link_impact(const SimulationContext& ctx, int link_idx);
/**
 * @brief Inspect references to a subcatchment.
 *
 * Scans current incoming references without mutating ctx. Covers downstream subcatchment/outfall
 * references, LID usages, coverage/quality rows and spatial geometry. Returned report predicts
 * supported reference changes rather than certifying model validity.
 * @see
 * swmm_subcatch_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] sc_idx Existing zero-based subcatchment index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_subcatch_impact(const SimulationContext& ctx, int sc_idx);
/**
 * @brief Inspect references to a rain gage.
 *
 * Scans current incoming references without mutating ctx. Covers subcatchment gage references and
 * spatial rows. Returned report predicts supported reference changes rather than certifying model
 * validity.
 * @see swmm_gage_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] gage_idx Existing zero-based rain gage index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_gage_impact(const SimulationContext& ctx, int gage_idx);
/**
 * @brief Inspect references to a time-series/curve table.
 *
 * Scans current incoming references without mutating ctx. Covers
 * gage/outfall/storage/divider/pump/cross-section references. Returned report predicts supported
 * reference changes rather than certifying model validity.
 * @see swmm_table_analyze_impact() for
 * the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] table_idx Existing zero-based time-series/curve table index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_table_impact(const SimulationContext& ctx, int table_idx);
/**
 * @brief Inspect references to a transect.
 *
 * Scans current incoming references without mutating ctx. Covers irregular conduit references and
 * shape resets. Returned report predicts supported reference changes rather than certifying model
 * validity.
 * @see swmm_transect_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] transect_idx Existing zero-based transect index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_transect_impact(const SimulationContext& ctx, int transect_idx);

/**
 * @brief Inspect references to a pollutant.
 *
 * Scans current incoming references without mutating ctx. Covers buildup/washoff/treatment/quality
 * columns, inflow/DWF rows and LID/co-pollutant references. Returned report predicts supported
 * reference changes rather than certifying model validity.
 * @see swmm_pollutant_analyze_impact()
 * for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] pollut_idx Existing zero-based pollutant index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_pollutant_impact(const SimulationContext& ctx, int pollut_idx);
/**
 * @brief Inspect references to a time pattern.
 *
 * Scans current incoming references without mutating ctx. Covers exact-case
 * DWF/external-inflow/aquifer/global evaporation pattern-name references. Returned report predicts
 * supported reference changes rather than certifying model validity.
 * @see
 * swmm_pattern_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] pattern_idx Existing zero-based time pattern index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_pattern_impact(const SimulationContext& ctx, int pattern_idx);
/**
 * @brief Inspect references to a aquifer.
 *
 * Scans current incoming references without mutating ctx. Covers subcatchment groundwater
 * assignments. Returned report predicts supported reference changes rather than certifying model
 * validity.
 * @see swmm_aquifer_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] aquifer_idx Existing zero-based aquifer index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_aquifer_impact(const SimulationContext& ctx, int aquifer_idx);
/**
 * @brief Inspect references to a snowpack.
 *
 * Scans current incoming references without mutating ctx. Covers subcatchment snowpack indices and
 * deferred names. Returned report predicts supported reference changes rather than certifying model
 * validity.
 * @see swmm_snowpack_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] snowpack_idx Existing zero-based snowpack index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_snowpack_impact(const SimulationContext& ctx, int snowpack_idx);
/**
 * @brief Inspect references to a LID control.
 *
 * Scans current incoming references without mutating ctx. Covers subcatchment usages and
 * storage-node LID assignments/outlet anchors. Returned report predicts supported reference changes
 * rather than certifying model validity.
 * @see swmm_lid_analyze_impact() for the checked C
 * wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] lid_idx Existing zero-based LID control index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_lid_impact(const SimulationContext& ctx, int lid_idx);
/**
 * @brief Inspect references to a street.
 *
 * Scans current incoming references without mutating ctx. Covers street inlet usages and conduit
 * section references. Returned report predicts supported reference changes rather than certifying
 * model validity.
 * @see swmm_street_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] street_idx Existing zero-based street index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_street_impact(const SimulationContext& ctx, int street_idx);
/**
 * @brief Inspect references to a inlet design.
 *
 * Scans current incoming references without mutating ctx. Covers inlet usages referencing the
 * design. Returned report predicts supported reference changes rather than certifying model
 * validity.
 * @see swmm_inlet_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] inlet_idx Existing zero-based inlet design index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_inlet_impact(const SimulationContext& ctx, int inlet_idx);
/**
 * @brief Inspect references to a land use.
 *
 * Scans current incoming references without mutating ctx. Covers buildup/washoff rows and
 * subcatchment coverage columns. Returned report predicts supported reference changes rather than
 * certifying model validity.
 * @see swmm_landuse_analyze_impact() for the checked C wrapper.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] landuse_idx Existing zero-based land use index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult analyze_landuse_impact(const SimulationContext& ctx, int landuse_idx);
/**
 * @brief Inspect RDII references to an exact-case hydrograph group.
 *
 * Exact-case matching applies to all scanned/deleted references. Does not mutate the context.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] uh_name Borrowed exact-case group name. This low-level helper accepts absent names and
 *     returns an empty report.
 * @return Owned report of matching RDII assignments; group-owned parameter/gage/decay erasures are
 *     not separately reported.
 */
CascadeResult analyze_hydrograph_impact(const SimulationContext& ctx, const std::string& uh_name);

/**
 * @brief Find stored rule-text tokens naming an object after an object keyword.
 *
 * Scans whitespace-separated token pairs for NODE/LINK/CONDUIT/PUMP/ORIFICE/WEIR/OUTLET/GAGE then
 * name; this is not a full control-expression parser and does not unquote tokens or modify rule
 * text.
 *
 * @param[in] ctx Borrowed context with aligned object/reference stores; no concurrent edits during
 *     this read.
 * @param[in] object_name Borrowed nonempty object name; empty returns no hits. Matching uses
 *     ASCII-insensitive comparison.
 * @return Owned ascending rule-text row indices with at most one entry per row.
 */
std::vector<int> find_control_rule_refs(const SimulationContext& ctx,
                                        const std::string& object_name);

// ============================================================================
// Deletion — mutates ctx
// ============================================================================

/**
 * @brief Delete a node.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers attached links, inflow/DWF/RDII rows, subtype/reaction references and spatial rows.
 * Re-resolve cached indices after success; exceptions can leave partial mutation because no
 * transactional rollback is implemented.
 * @see swmm_node_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] node_idx Existing zero-based node index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_node(SimulationContext& ctx, int node_idx);
/**
 * @brief Delete a link.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers divider/inlet usage references, subtype/reaction rows and spatial geometry. Re-resolve
 * cached indices after success; exceptions can leave partial mutation because no transactional
 * rollback is implemented.
 * @see swmm_link_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] link_idx Existing zero-based link index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_link(SimulationContext& ctx, int link_idx);
/**
 * @brief Delete a subcatchment.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers downstream subcatchment/outfall references, LID usages, coverage/quality rows and spatial
 * geometry. Re-resolve cached indices after success; exceptions can leave partial mutation because
 * no transactional rollback is implemented.
 * @see swmm_subcatch_delete() for the checked C
 * wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] sc_idx Existing zero-based subcatchment index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_subcatch(SimulationContext& ctx, int sc_idx);
/**
 * @brief Delete a rain gage.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers subcatchment gage references and spatial rows. Re-resolve cached indices after success;
 * exceptions can leave partial mutation because no transactional rollback is implemented.
 * @see
 * swmm_gage_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] gage_idx Existing zero-based rain gage index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_gage(SimulationContext& ctx, int gage_idx);

/**
 * @brief Delete node rows in deduplicated descending pre-batch order.
 *
 * Runs the same individual cascade functions while deferring affected name-index rebuilds to scope
 * exit. Caller validates every input before entry if atomic refusal is required;
 * deletion/allocation exceptions do not roll back earlier deletions. Map lookup must not run
 * concurrently while the batch leaves maps stale.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] indices Owned index-vector copy/move; every value must name an existing pre-batch
 *     node. Empty is a no-op; duplicates are deduplicated.
 * @return Owned aggregate cascade report with each entry using the index at its sequential deletion
 *     time.
 */
CascadeResult delete_nodes_many(SimulationContext& ctx, std::vector<int> indices);
/**
 * @brief Delete link rows in deduplicated descending pre-batch order.
 *
 * Runs the same individual cascade functions while deferring affected name-index rebuilds to scope
 * exit. Caller validates every input before entry if atomic refusal is required;
 * deletion/allocation exceptions do not roll back earlier deletions. Map lookup must not run
 * concurrently while the batch leaves maps stale.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] indices Owned index-vector copy/move; every value must name an existing pre-batch
 *     link. Empty is a no-op; duplicates are deduplicated.
 * @return Owned aggregate cascade report with each entry using the index at its sequential deletion
 *     time.
 */
CascadeResult delete_links_many(SimulationContext& ctx, std::vector<int> indices);
/**
 * @brief Delete subcatchment rows in deduplicated descending pre-batch order.
 *
 * Runs the same individual cascade functions while deferring affected name-index rebuilds to scope
 * exit. Caller validates every input before entry if atomic refusal is required;
 * deletion/allocation exceptions do not roll back earlier deletions. Map lookup must not run
 * concurrently while the batch leaves maps stale.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] indices Owned index-vector copy/move; every value must name an existing pre-batch
 *     subcatchment. Empty is a no-op; duplicates are deduplicated.
 * @return Owned aggregate cascade report with each entry using the index at its sequential deletion
 *     time.
 */
CascadeResult delete_subcatches_many(SimulationContext& ctx, std::vector<int> indices);
/**
 * @brief Delete rain gage rows in deduplicated descending pre-batch order.
 *
 * Runs the same individual cascade functions while deferring affected name-index rebuilds to scope
 * exit. Caller validates every input before entry if atomic refusal is required;
 * deletion/allocation exceptions do not roll back earlier deletions. Map lookup must not run
 * concurrently while the batch leaves maps stale.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] indices Owned index-vector copy/move; every value must name an existing pre-batch rain
 *     gage. Empty is a no-op; duplicates are deduplicated.
 * @return Owned aggregate cascade report with each entry using the index at its sequential deletion
 *     time.
 */
CascadeResult delete_gages_many(SimulationContext& ctx, std::vector<int> indices);
/**
 * @brief Delete a time-series/curve table.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers gage/outfall/storage/divider/pump/cross-section references. Re-resolve cached indices
 * after success; exceptions can leave partial mutation because no transactional rollback is
 * implemented.
 * @see swmm_table_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] table_idx Existing zero-based time-series/curve table index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_table(SimulationContext& ctx, int table_idx);
/**
 * @brief Delete a transect.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers irregular conduit references and shape resets. Re-resolve cached indices after success;
 * exceptions can leave partial mutation because no transactional rollback is implemented.
 * @see
 * swmm_transect_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] transect_idx Existing zero-based transect index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_transect(SimulationContext& ctx, int transect_idx);

/**
 * @brief Delete a pollutant.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers buildup/washoff/treatment/quality columns, inflow/DWF rows and LID/co-pollutant
 * references. Re-resolve cached indices after success; exceptions can leave partial mutation
 * because no transactional rollback is implemented.
 * @see swmm_pollutant_delete() for the checked
 * C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] pollut_idx Existing zero-based pollutant index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_pollutant(SimulationContext& ctx, int pollut_idx);
/**
 * @brief Delete a time pattern.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers exact-case DWF/external-inflow/aquifer/global evaporation pattern-name references.
 * Re-resolve cached indices after success; exceptions can leave partial mutation because no
 * transactional rollback is implemented.
 * @see swmm_pattern_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] pattern_idx Existing zero-based time pattern index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_pattern(SimulationContext& ctx, int pattern_idx);
/**
 * @brief Delete a aquifer.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers subcatchment groundwater assignments. Re-resolve cached indices after success; exceptions
 * can leave partial mutation because no transactional rollback is implemented.
 * @see
 * swmm_aquifer_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] aquifer_idx Existing zero-based aquifer index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_aquifer(SimulationContext& ctx, int aquifer_idx);
/**
 * @brief Delete a snowpack.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers subcatchment snowpack indices and deferred names. Re-resolve cached indices after success;
 * exceptions can leave partial mutation because no transactional rollback is implemented.
 * @see
 * swmm_snowpack_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] snowpack_idx Existing zero-based snowpack index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_snowpack(SimulationContext& ctx, int snowpack_idx);
/**
 * @brief Delete a LID control.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers subcatchment usages and storage-node LID assignments/outlet anchors. Re-resolve cached
 * indices after success; exceptions can leave partial mutation because no transactional rollback is
 * implemented.
 * @see swmm_lid_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] lid_idx Existing zero-based LID control index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_lid(SimulationContext& ctx, int lid_idx);
/**
 * @brief Delete a street.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers street inlet usages and conduit section references. Re-resolve cached indices after
 * success; exceptions can leave partial mutation because no transactional rollback is implemented.
 *@see swmm_street_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] street_idx Existing zero-based street index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_street(SimulationContext& ctx, int street_idx);
/**
 * @brief Delete a inlet design.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers inlet usages referencing the design. Re-resolve cached indices after success; exceptions
 * can leave partial mutation because no transactional rollback is implemented.
 * @see
 * swmm_inlet_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] inlet_idx Existing zero-based inlet design index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_inlet(SimulationContext& ctx, int inlet_idx);
/**
 * @brief Delete a land use.
 *
 * Erases the object, cascades/nullifies supported references and shifts later object indices.
 * Covers buildup/washoff rows and subcatchment coverage columns. Re-resolve cached indices after
 * success; exceptions can leave partial mutation because no transactional rollback is implemented.
 *@see swmm_landuse_delete() for the checked C wrapper.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] landuse_idx Existing zero-based land use index, validated by the caller.
 * @return Owned impact entries with static field-name literals and indices at the time each entry
 *     was recorded.
 */
CascadeResult delete_landuse(SimulationContext& ctx, int landuse_idx);
/**
 * @brief Delete all authoring rows belonging to an exact-case hydrograph group.
 *
 * Exact-case matching applies to all scanned/deleted references. Deletes RDII assignments,
 * parameter rows, gage assignments and exponential-decay rows. No running RDII cache refresh or
 * rollback.
 *
 * @param[in,out] ctx Caller-owned editable BUILDING/OPENED context with aligned stores. Caller
 *     enforces lifecycle/index validity; no guards or solver-cache rebuild are provided here.
 * @param[in] uh_name Borrowed exact-case group name. This low-level helper accepts absent names and
 *     returns an empty report.
 * @return Owned report of matching RDII assignments; group-owned parameter/gage/decay erasures are
 *     not separately reported.
 */
CascadeResult delete_hydrograph(SimulationContext& ctx, const std::string& uh_name);

} // namespace openswmm::edit

#endif /* OPENSWMM_ENGINE_OBJECT_DELETER_HPP */
