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
 * @file      openswmm_edit.h
 * @brief     OpenSWMM Engine — Model editing C API (deletion and type conversion).
 *
 * @details Provides two capabilities for interactive model editing:
 *
 *   **1. Object deletion** — remove any node, link, subcatchment, rain gage,
 *   table/curve, or transect from the model while the engine is in BUILDING
 *   or OPENED state.  A pre-deletion impact-analysis function is provided so
 *   callers can inspect what would be affected before committing.
 *
 *   **2. Type conversion** — convert a node between JUNCTION/OUTFALL/STORAGE/
 *   DIVIDER, or a link between CONDUIT/PUMP/ORIFICE/WEIR/OUTLET, in-place.
 *   Common properties are preserved; type-specific properties are cleared and
 *   sensible defaults are applied.
 *
 * @section edit_safety Safety guards
 *
 *   All mutating operations (delete, convert) require the engine to be in
 *   SWMM_STATE_BUILDING or SWMM_STATE_OPENED.  The impact-analysis functions
 *   are read-only and work in any state where data is present (not CREATED,
 *   CLOSED, or ERROR).
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_EDIT_H
#define OPENSWMM_EDIT_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Impact report — returned by analyze_impact and delete functions
 * ========================================================================= */

/**
 * @brief Category of an object that holds a cross-reference to a deleted object.
 */
typedef enum SWMM_RefType {
    SWMM_REF_NODE = 0,          /**< A node holds a reference. */
    SWMM_REF_LINK = 1,          /**< A link holds a reference. */
    SWMM_REF_SUBCATCH = 2,      /**< A subcatchment holds a reference. */
    SWMM_REF_GAGE = 3,          /**< A rain gage holds a reference. */
    SWMM_REF_TABLE = 4,         /**< A time series or curve holds a reference. */
    SWMM_REF_TRANSECT = 5,      /**< A transect holds a reference. */
    SWMM_REF_INLET_USAGE = 6,   /**< An inlet usage entry holds a reference. */
    SWMM_REF_EXT_INFLOW = 7,    /**< An external-inflow row holds a reference. */
    SWMM_REF_DWF_INFLOW = 8,    /**< A dry-weather-flow row holds a reference. */
    SWMM_REF_RDII_ASSIGN = 9,   /**< An RDII assignment row holds a reference. */
    SWMM_REF_TREATMENT = 10,    /**< A treatment expression holds a reference. */
    SWMM_REF_LID_USAGE = 11,    /**< A LID usage row holds a reference. */
    SWMM_REF_SNOWPACK = 12,     /**< A snowpack holds a reference. */
    SWMM_REF_HYDROGRAPH = 13,   /**< A unit-hydrograph group holds a reference. */
    SWMM_REF_POLLUTANT = 14,    /**< A pollutant holds a reference. */
    SWMM_REF_PATTERN = 15,      /**< A time pattern holds a reference. */
    SWMM_REF_AQUIFER = 16,      /**< An aquifer holds a reference. */
    SWMM_REF_LID_CONTROL = 17,  /**< A LID control holds a reference. */
    SWMM_REF_STREET = 18,       /**< A street holds a reference. */
    SWMM_REF_INLET_DESIGN = 19, /**< An inlet design holds a reference. */
    SWMM_REF_LANDUSE = 20,      /**< A land use holds a reference. */
    SWMM_REF_CONTROL_RULE = 21  /**< A control rule references the object by name. */
} SWMM_RefType;

/**
 * @brief One object that references the deletion target.
 *
 * @details `field` is a static string literal naming the specific cross-
 *          reference field (e.g. "node1", "outlet_node", "pump_curve").
 *          The caller must NOT free it.
 */
typedef struct SWMM_ImpactEntry {
    int obj_type;      /**< SWMM_RefType value. */
    int obj_idx;       /**< Zero-based index in the referencing object's array. */
    const char* field; /**< Static field-name string — do NOT free. */
    int cascaded;      /**< 1 if this object was deleted (cascade), 0 if nullified. */
} SWMM_ImpactEntry;

/**
 * @brief Aggregate impact report for a deletion operation.
 *
 * @details `entries` is heap-allocated by the engine.  The caller must pass
 *          this struct to @ref swmm_impact_report_free when done.
 */
typedef struct SWMM_ImpactReport {
    SWMM_ImpactEntry* entries; /**< Array of impacted objects (may be NULL if n_entries==0). */
    int n_entries;             /**< Number of entries. */
} SWMM_ImpactReport;

/**
 * @brief Release report entries and reset their pointer/count.
 *
 * May be called after engine destruction. Field-name literals are static and must not be freed
 * individually.
 *
 * @param[in,out] report Previously returned report or a zero-initialized struct; NULL is a no-op.
 *     The struct itself remains caller-owned.
 */
SWMM_ENGINE_API void swmm_impact_report_free(SWMM_ImpactReport* report);

/* =========================================================================
 * Impact analysis — read-only, works in any state where data exists
 * ========================================================================= */

/**
 * @brief Inspect incoming references to a node without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing node index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_node_analyze_impact(SWMM_Engine engine, int idx,
                                             SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a link without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing link index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_link_analyze_impact(SWMM_Engine engine, int idx,
                                             SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a subcatchment without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_subcatch_analyze_impact(SWMM_Engine engine, int idx,
                                                 SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a rain gage without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing rain gage index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_gage_analyze_impact(SWMM_Engine engine, int idx,
                                             SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a time-series/curve table without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing time-series/curve table index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_table_analyze_impact(SWMM_Engine engine, int idx,
                                              SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a transect without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing transect index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_transect_analyze_impact(SWMM_Engine engine, int idx,
                                                 SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a pollutant without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing pollutant index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_pollutant_analyze_impact(SWMM_Engine engine, int idx,
                                                  SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a time pattern without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing time pattern index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_pattern_analyze_impact(SWMM_Engine engine, int idx,
                                                SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to an aquifer without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing aquifer index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_aquifer_analyze_impact(SWMM_Engine engine, int idx,
                                                SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a snowpack without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_snowpack_analyze_impact(SWMM_Engine engine, int idx,
                                                 SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a LID control without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing LID control index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_lid_analyze_impact(SWMM_Engine engine, int idx,
                                            SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a street without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing street index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_street_analyze_impact(SWMM_Engine engine, int idx,
                                               SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to an inlet design without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing inlet design index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_inlet_analyze_impact(SWMM_Engine engine, int idx,
                                              SWMM_ImpactReport* report_out);

/**
 * @brief Inspect incoming references to a land use without changing model data.
 *
 * Allowed whenever model data is readable: every state except CREATED, CLOSED and ERROR. With
 * report_out=NULL performs handle/state/index validation only. Report indices describe the current
 * model; later edits can invalidate them.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing land use index.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 */
SWMM_ENGINE_API int swmm_landuse_analyze_impact(SWMM_Engine engine, int idx,
                                                SWMM_ImpactReport* report_out);

/**
 * @brief Inspect references to an existing hydrograph group by exact-case name.
 *
 * Requires readable data (all states except CREATED/CLOSED/ERROR); with NULL output only validates
 * existence.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] uh_name Borrowed nonempty name matching a parameter row or gage assignment exactly;
 *     decay-only names do not establish a group.
 * @param[out] report_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for refused state, or
 *     SWMM_ERR_BADPARAM for null/empty/unknown name.
 */
SWMM_ENGINE_API int swmm_hydrograph_analyze_impact(SWMM_Engine engine, const char* uh_name,
                                                   SWMM_ImpactReport* report_out);

/* =========================================================================
 * Deletion — BUILDING or OPENED state only
 * ========================================================================= */

/**
 * @brief Delete a node and cascade-delete or nullify all referencing objects.
 *
 * @details Links that reference the node as node1 or node2 are deleted.
 *          Subcatchment outlet_node and inlet_usage node_index references are
 *          nullified (set to -1). All remaining integer cross-references whose
 *          value is greater than `idx` are decremented by 1.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing node index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_node_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a link and cascade-delete or nullify all referencing objects.
 *
 * @details Divider node divider_link fields that reference this link are
 *          nullified.  Inlet usage entries whose link_index equals `idx` are
 *          deleted.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing link index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_link_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a subcatchment and nullify all referencing objects.
 *
 * @details Other subcatchments' outlet_subcatch fields and outfall nodes'
 *          outfall_route_to fields that reference this subcatchment are
 *          nullified.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing subcatchment index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_subcatch_delete(SWMM_Engine engine, int idx,
                                         SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a rain gage and nullify subcatchment gage references.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing rain gage index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_gage_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a batch of node rows using pre-batch indices.
 *
 * Requires BUILDING/OPENED. Validates every index before any mutation, then applies the same
 * cascade logic as descending single-row deletions and rebuilds name indices once. Cascade report
 * indices follow that descending sequence, rather than all sharing the original or final index
 * space. All cached model indices should be re-resolved after success.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] indices Borrowed array of n zero-based indices; required only when n>0. Duplicates are
 *     deduplicated.
 * @param[in] n Array length; n<=0 succeeds as a no-op after handle/lifecycle validation.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state. SWMM_ERR_BADPARAM for a null indices
 *     array with n>0.
 */
SWMM_ENGINE_API int swmm_node_delete_many(SWMM_Engine engine, const int* indices, int n,
                                          SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a batch of link rows using pre-batch indices.
 *
 * Requires BUILDING/OPENED. Validates every index before any mutation, then applies the same
 * cascade logic as descending single-row deletions and rebuilds name indices once. Cascade report
 * indices follow that descending sequence, rather than all sharing the original or final index
 * space. All cached model indices should be re-resolved after success.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] indices Borrowed array of n zero-based indices; required only when n>0. Duplicates are
 *     deduplicated.
 * @param[in] n Array length; n<=0 succeeds as a no-op after handle/lifecycle validation.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state. SWMM_ERR_BADPARAM for a null indices
 *     array with n>0.
 */
SWMM_ENGINE_API int swmm_link_delete_many(SWMM_Engine engine, const int* indices, int n,
                                          SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a batch of subcatchment rows using pre-batch indices.
 *
 * Requires BUILDING/OPENED. Validates every index before any mutation, then applies the same
 * cascade logic as descending single-row deletions and rebuilds name indices once. Cascade report
 * indices follow that descending sequence, rather than all sharing the original or final index
 * space. All cached model indices should be re-resolved after success.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] indices Borrowed array of n zero-based indices; required only when n>0. Duplicates are
 *     deduplicated.
 * @param[in] n Array length; n<=0 succeeds as a no-op after handle/lifecycle validation.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state. SWMM_ERR_BADPARAM for a null indices
 *     array with n>0.
 */
SWMM_ENGINE_API int swmm_subcatch_delete_many(SWMM_Engine engine, const int* indices, int n,
                                              SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a batch of rain gage rows using pre-batch indices.
 *
 * Requires BUILDING/OPENED. Validates every index before any mutation, then applies the same
 * cascade logic as descending single-row deletions and rebuilds name indices once. Cascade report
 * indices follow that descending sequence, rather than all sharing the original or final index
 * space. All cached model indices should be re-resolved after success.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] indices Borrowed array of n zero-based indices; required only when n>0. Duplicates are
 *     deduplicated.
 * @param[in] n Array length; n<=0 succeeds as a no-op after handle/lifecycle validation.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state. SWMM_ERR_BADPARAM for a null indices
 *     array with n>0.
 */
SWMM_ENGINE_API int swmm_gage_delete_many(SWMM_Engine engine, const int* indices, int n,
                                          SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a time series or curve and nullify all referencing objects.
 *
 * @details Affected references: gage ts_index, node storage_curve /
 *          outfall_param (TIDAL/TIMESERIES) / divider_curve, link pump_curve /
 *          xsect_curve.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing time-series/curve table index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_table_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a transect and nullify link xsect_curve references.
 *
 * @details Links with IRREGULAR cross-section shape whose xsect_curve equals
 *          `idx` have their xsect_curve set to -1 and shape reset to CIRCULAR.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing transect index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_transect_delete(SWMM_Engine engine, int idx,
                                         SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a pollutant and re-pack every per-pollutant matrix.
 *
 * @details Cascades: ext-inflow/DWF rows whose constituent names the
 *          pollutant. Re-packs: buildup/washoff (per-pollutant columns),
 *          treatment expressions, per-object concentration state (including
 *          [LOADINGS] initial buildup). Nullifies: co-pollutant references,
 *          LID removal pairs. Remaining co_pollut indices are renumbered.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing pollutant index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_pollutant_delete(SWMM_Engine engine, int idx,
                                          SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a time pattern and clear all name-based references.
 *
 * @details Clears matching DWF pat1..4, ext-inflow baseline patterns, aquifer
 *          ET patterns, and the global evaporation-recovery pattern. Unlike
 *          @ref swmm_pattern_remove this reports what was cleared and rejects
 *          out-of-range indices.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing time pattern index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_pattern_delete(SWMM_Engine engine, int idx,
                                        SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete an aquifer; referencing subcatchments lose their groundwater
 *        (gw_aquifer set to -1) and surviving indices are renumbered.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing aquifer index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_aquifer_delete(SWMM_Engine engine, int idx,
                                        SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a snowpack; referencing subcatchments' snowpack index and
 *        raw-name mirror are cleared, surviving indices renumbered.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing snowpack index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_snowpack_delete(SWMM_Engine engine, int idx,
                                         SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a LID control; LID-usage rows referencing it are
 *        cascade-deleted and surviving lid_index values renumbered.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing LID control index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_lid_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a street; inlet-usage rows referencing it are cascade-deleted
 *        (consistent with link deletion) and street_index values renumbered.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing street index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_street_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete an inlet design; inlet-usage rows referencing it are
 *        cascade-deleted and design_index values renumbered.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing inlet design index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_inlet_delete(SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a land use; buildup/washoff rows and subcatchment coverage
 *        columns for it are re-packed.
 *
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] idx Zero-based existing land use index.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_BADINDEX for an invalid object
 *     index, or SWMM_ERR_LIFECYCLE for a refused engine state.
 * @note Requires BUILDING or OPENED. Erasing rows can shift object and dependent indices;
 *     re-resolve cached indices after success.
 */
SWMM_ENGINE_API int swmm_landuse_delete(SWMM_Engine engine, int idx,
                                        SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete an existing hydrograph group by exact-case name.
 *
 * Requires BUILDING/OPENED. Removes exact-case parameter, gage, decay and RDII assignment rows; the
 * report lists affected RDII references. No running solver cache is refreshed.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] uh_name Borrowed nonempty name matching a parameter row or gage assignment exactly;
 *     decay-only names do not establish a group.
 * @param[out] cascade_out Optional caller-owned result struct. Receives a newly allocated report;
 *     free any previous contents first and release new contents with swmm_impact_report_free().
 *     Preserved on validation failure.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE for refused state, or
 *     SWMM_ERR_BADPARAM for null/empty/unknown name.
 */
SWMM_ENGINE_API int swmm_hydrograph_delete(SWMM_Engine engine, const char* uh_name,
                                           SWMM_ImpactReport* cascade_out);

/* =========================================================================
 * Type conversion result — returned by convert functions
 * ========================================================================= */

/**
 * @brief Result of a node or link type conversion.
 *
 * @details Lists type-specific fields that were cleared and any topology
 *          warnings generated.  Caller must pass to
 *          @ref swmm_conversion_result_free when done.
 */
typedef struct SWMM_ConversionResult {
    int new_type; /**< The resulting C-API type enum value. */
    const char**
        cleared_fields; /**< Counted array of allocated cleared-field strings; not NUL-terminated. */
    int n_cleared;      /**< Number of cleared field names. */
    const char** warnings; /**< Counted array of allocated warning strings; not NUL-terminated. */
    int n_warnings;        /**< Number of warnings. */
} SWMM_ConversionResult;

/**
 * @brief Release all allocated conversion field/warning strings and arrays.
 *
 * May be called after engine destruction. Use this function rather than freeing engine-allocated
 * contents individually.
 *
 * @param[in,out] result Previously returned result or zero-initialized struct; NULL is a no-op.
 *     Resets arrays/counts but leaves new_type unchanged; does not free the struct.
 */
SWMM_ENGINE_API void swmm_conversion_result_free(SWMM_ConversionResult* result);

/* =========================================================================
 * Type conversion — BUILDING or OPENED state only
 * ========================================================================= */

/**
 * @brief Convert a node to a different type in-place.
 *
 * @details Common properties (invert_elev, full_depth, init_depth, sur_depth,
 *          ponded_area, coordinates) are preserved.  Type-specific fields for
 *          the old type are cleared and type-specific defaults for the new type
 *          are applied.  Topology warnings are generated but conversion still
 *          proceeds.
 *
 * @param[in] engine      Engine handle.
 * @param[in] idx         Zero-based node index.
 * @param[in] new_type    Target SWMM_NodeType value.
 * @param[out] result_out  If non-NULL, receives the conversion result.  Caller must
 *                    free with @ref swmm_conversion_result_free.
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not BUILDING/OPENED,
 *          SWMM_ERR_BADPARAM if new_type is invalid or equals current type,
 *          SWMM_ERR_BADHANDLE, or SWMM_ERR_BADINDEX.
 *
 * @note Requires BUILDING/OPENED; result/output pointers may be NULL. Free any previous result_out
 *     contents before reusing it. Returned arrays contain exactly n_cleared/n_warnings entries and
 *     are NOT NUL-terminated; free with swmm_conversion_result_free(). Validation failure leaves
 *     result_out unchanged.
 */
SWMM_ENGINE_API int swmm_node_convert(SWMM_Engine engine, int idx, int new_type,
                                      SWMM_ConversionResult* result_out);

/**
 * @brief Convert a link to a different type in-place.
 *
 * @details Common properties (node1, node2, offset1, offset2, q0, q_limit,
 *          setting, comments) are preserved.  Type-specific fields are cleared
 *          and new-type defaults applied.
 *
 * @param[in] engine      Engine handle.
 * @param[in] idx         Zero-based link index.
 * @param[in] new_type    Target SWMM_LinkType value.
 * @param[out] result_out  If non-NULL, receives the conversion result.
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM if new_type equals current
 *          type, SWMM_ERR_LIFECYCLE, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX.
 *
 * @note Requires BUILDING/OPENED; result/output pointers may be NULL. Free any previous result_out
 *     contents before reusing it. Returned arrays contain exactly n_cleared/n_warnings entries and
 *     are NOT NUL-terminated; free with swmm_conversion_result_free(). Validation failure leaves
 *     result_out unchanged.
 */
SWMM_ENGINE_API int swmm_link_convert(SWMM_Engine engine, int idx, int new_type,
                                      SWMM_ConversionResult* result_out);

/* =========================================================================
 * Conduit split / virtual-junction fusion — BUILDING or OPENED state only
 * (refactored engine only; see openswmm_nodes.h swmm_node_set_virtual)
 * ========================================================================= */

/**
 * @brief Split a conduit at normalized position t, inserting a new node.
 *
 * @details The split point lies at fraction @p t of the conduit's
 *          vertex-aware polyline length (0 < t < 1). The original conduit
 *          keeps its name and upstream end and is shortened to t·L; a new
 *          conduit named @p new_link_name carries the remaining length,
 *          copying cross-section, roughness and barrels, and taking over the
 *          downstream offset. The break-point invert is interpolated along
 *          the conduit gradient and interior vertices are partitioned.
 *          With @p make_virtual != 0 the new node becomes a virtual junction
 *          (validation runs; on a rule failure the split still stands as a
 *          regular junction and the rule code is returned).
 *
 * @param[in] engine         Engine handle.
 * @param[in] link_idx       Zero-based conduit index.
 * @param[in] t              Normalized split position along the polyline (0..1 exclusive).
 * @param[in] new_node_name  Unique name for the inserted node.
 * @param[in] new_link_name  Unique name for the new downstream conduit.
 * @param[in] make_virtual   1 to flag the new node as a virtual junction.
 * @param[out] new_node_idx  Receives the new node index (may be NULL).
 * @param[out] new_link_idx  Receives the new conduit index (may be NULL).
 * @returns SWMM_OK on success; SWMM_ERR_LIFECYCLE, SWMM_ERR_BADHANDLE,
 *          SWMM_ERR_BADINDEX, SWMM_ERR_BADPARAM (bad t / duplicate or empty
 *          name / non-conduit); or an ERR_VJ_* rule code (609/611/613/617)
 *          when make_virtual validation fails after a successful split.
 *
 * @note Requires BUILDING/OPENED; result/output pointers may be NULL. Output indices are written
 *     when the geometric split succeeds, including a subsequent nonzero virtual-junction rule
 *     result; that error does not roll back the split.
 */
SWMM_ENGINE_API int swmm_conduit_split(SWMM_Engine engine, int link_idx, double t,
                                       const char* new_node_name, const char* new_link_name,
                                       int make_virtual, int* new_node_idx, int* new_link_idx);

/**
 * @brief Re-fuse the two conduits of a virtual junction into one, deleting
 *        the node (inverse of a virtual split).
 *
 * @details The upstream conduit's name survives; lengths sum; the node
 *          coordinate becomes an interior vertex of the merged conduit so
 *          map alignment is preserved; the downstream conduit's identifiers
 *          are retired. Requires a through orientation (one conduit flowing
 *          in, one flowing out).
 *
 * @param[in] engine    Engine handle.
 * @param[in] node_idx  Zero-based node index of the virtual junction.
 * @param[out] surviving_link_idx  Receives the surviving conduit's index
 *                                 AFTER deletions renumber (may be NULL).
 * @returns SWMM_OK on success; SWMM_ERR_LIFECYCLE, SWMM_ERR_BADHANDLE,
 *          SWMM_ERR_BADINDEX, SWMM_ERR_BADPARAM (not a virtual junction);
 *          or 609 (ERR_VJ_LINK_COUNT) when the node is not a two-conduit
 *          through pair.
 *
 * @note Requires BUILDING/OPENED; result/output pointers may be NULL. The survivor index is written
 *     only on success and uses the final renumbered link table.
 */
SWMM_ENGINE_API int swmm_virtual_junction_fuse(SWMM_Engine engine, int node_idx,
                                               int* surviving_link_idx);

/**
 * @brief Split an eligible conduit and create a configured inlet junction.
 *
 * Requires BUILDING/OPENED. Creates one inlet, zero clogging, no capture limit/depression and
 * AUTOMATIC placement. The edit helper attempts fusion to undo a later rule refusal. Undo can fail
 * if virtual
 * promotion left a regular node; a failed call can therefore leave a split while output
 * indices remain unchanged. Adjust usage parameters with swmm_inlet_usage_set().
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] link_idx Zero-based existing conduit index with a cross-section compatible with the
 *     design.
 * @param[in] t Finite normalized location strictly between 0 and 1 along the vertex-aware polyline.
 * @param[in] new_node_name Non-null, nonempty unique copied name for the inserted node.
 * @param[in] new_link_name Non-null, nonempty unique copied name for the downstream conduit.
 * @param[in] inlet_id Non-null existing inlet-design name, looked up case-insensitively.
 * @param[in] capture_node Non-null existing capture-node name; cannot select a virtual junction or
 *     the inserted node.
 * @param[out] new_node_idx Optional inserted node index, written only on success.
 * @param[out] new_link_idx Optional downstream conduit index, written only on success.
 * @return SWMM_OK; BADHANDLE/LIFECYCLE/BADINDEX for invalid engine/state/link; BADPARAM for null
 *     strings or rejected split; or ERR_VJ_/ERR_IJ_ rule codes including 625 for unknown design and
 *     627 for bad capture node.
 */
SWMM_ENGINE_API int swmm_conduit_split_inlet(SWMM_Engine engine, int link_idx, double t,
                                             const char* new_node_name, const char* new_link_name,
                                             const char* inlet_id, const char* capture_node,
                                             int* new_node_idx, int* new_link_idx);

/**
 * @brief Remove an inlet junction usage and fuse its through-conduit pair.
 *
 * Requires BUILDING/OPENED. Removes the inlet usage and downstream conduit/node; the upstream name
 * survives, lengths sum and the old junction coordinate becomes an interior vertex. Re-resolve
 * cached indices afterward.
 *
 * @param[in] engine Non-null live engine; caller serializes access.
 * @param[in] node_idx Zero-based existing inlet-junction node index.
 * @param[out] surviving_link_idx Optional output for the final renumbered surviving upstream
 *     conduit index; written only on success.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE, SWMM_ERR_LIFECYCLE, SWMM_ERR_BADINDEX; SWMM_ERR_BADPARAM if
 *     not an inlet junction; or ERR_VJ_LINK_COUNT when no valid two-conduit through pair exists.
 */
SWMM_ENGINE_API int swmm_inlet_junction_fuse(SWMM_Engine engine, int node_idx,
                                             int* surviving_link_idx);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_EDIT_H */
