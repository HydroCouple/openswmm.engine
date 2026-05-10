/**
 * @file openswmm_edit.h
 * @brief OpenSWMM Engine — Model editing C API (deletion and type conversion).
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
    SWMM_REF_NODE        = 0, /**< A node holds a reference. */
    SWMM_REF_LINK        = 1, /**< A link holds a reference. */
    SWMM_REF_SUBCATCH    = 2, /**< A subcatchment holds a reference. */
    SWMM_REF_GAGE        = 3, /**< A rain gage holds a reference. */
    SWMM_REF_TABLE       = 4, /**< A time series or curve holds a reference. */
    SWMM_REF_TRANSECT    = 5, /**< A transect holds a reference. */
    SWMM_REF_INLET_USAGE = 6  /**< An inlet usage entry holds a reference. */
} SWMM_RefType;

/**
 * @brief One object that references the deletion target.
 *
 * @details `field` is a static string literal naming the specific cross-
 *          reference field (e.g. "node1", "outlet_node", "pump_curve").
 *          The caller must NOT free it.
 */
typedef struct SWMM_ImpactEntry {
    int         obj_type;  /**< SWMM_RefType value. */
    int         obj_idx;   /**< Zero-based index in the referencing object's array. */
    const char* field;     /**< Static field-name string — do NOT free. */
    int         cascaded;  /**< 1 if this object was deleted (cascade), 0 if nullified. */
} SWMM_ImpactEntry;

/**
 * @brief Aggregate impact report for a deletion operation.
 *
 * @details `entries` is heap-allocated by the engine.  The caller must pass
 *          this struct to @ref swmm_impact_report_free when done.
 */
typedef struct SWMM_ImpactReport {
    SWMM_ImpactEntry* entries;   /**< Array of impacted objects (may be NULL if n_entries==0). */
    int               n_entries; /**< Number of entries. */
} SWMM_ImpactReport;

/**
 * @brief Release heap memory owned by an impact report.
 *
 * @details Does not free the struct itself (typically stack-allocated by the
 *          caller).  Safe to call on a zero-initialised struct.
 */
SWMM_ENGINE_API void swmm_impact_report_free(SWMM_ImpactReport* report);

/* =========================================================================
 * Impact analysis — read-only, works in any state where data exists
 * ========================================================================= */

/**
 * @brief Analyse which objects reference a given node, without deleting it.
 *
 * @param engine      Engine handle.
 * @param idx         Zero-based node index.
 * @param report_out  Receives the impact report (may be NULL if not needed).
 * @returns SWMM_OK, SWMM_ERR_BADHANDLE, or SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_analyze_impact(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* report_out);

/** @copydoc swmm_node_analyze_impact */
SWMM_ENGINE_API int swmm_link_analyze_impact(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* report_out);

/** @copydoc swmm_node_analyze_impact */
SWMM_ENGINE_API int swmm_subcatch_analyze_impact(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* report_out);

/** @copydoc swmm_node_analyze_impact */
SWMM_ENGINE_API int swmm_gage_analyze_impact(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* report_out);

/** @copydoc swmm_node_analyze_impact */
SWMM_ENGINE_API int swmm_table_analyze_impact(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* report_out);

/** @copydoc swmm_node_analyze_impact */
SWMM_ENGINE_API int swmm_transect_analyze_impact(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* report_out);

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
 * @param engine           Engine handle.
 * @param idx              Zero-based node index.
 * @param cascade_out      If non-NULL, receives the cascade report describing
 *                         what was deleted or nullified.  Caller must free with
 *                         @ref swmm_impact_report_free.
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not BUILDING/OPENED,
 *          SWMM_ERR_BADHANDLE, or SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_delete(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a link and cascade-delete or nullify all referencing objects.
 *
 * @details Divider node divider_link fields that reference this link are
 *          nullified.  Inlet usage entries whose link_index equals `idx` are
 *          deleted.
 */
SWMM_ENGINE_API int swmm_link_delete(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a subcatchment and nullify all referencing objects.
 *
 * @details Other subcatchments' outlet_subcatch fields and outfall nodes'
 *          outfall_route_to fields that reference this subcatchment are
 *          nullified.
 */
SWMM_ENGINE_API int swmm_subcatch_delete(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a rain gage and nullify subcatchment gage references.
 */
SWMM_ENGINE_API int swmm_gage_delete(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a time series or curve and nullify all referencing objects.
 *
 * @details Affected references: gage ts_index, node storage_curve /
 *          outfall_param (TIDAL/TIMESERIES) / divider_curve, link pump_curve /
 *          xsect_curve.
 */
SWMM_ENGINE_API int swmm_table_delete(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

/**
 * @brief Delete a transect and nullify link xsect_curve references.
 *
 * @details Links with IRREGULAR cross-section shape whose xsect_curve equals
 *          `idx` have their xsect_curve set to -1 and shape reset to CIRCULAR.
 */
SWMM_ENGINE_API int swmm_transect_delete(
    SWMM_Engine engine, int idx, SWMM_ImpactReport* cascade_out);

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
    int          new_type;       /**< The resulting C-API type enum value. */
    const char** cleared_fields; /**< NULL-terminated array of cleared field names. */
    int          n_cleared;      /**< Number of cleared field names. */
    const char** warnings;       /**< NULL-terminated array of topology warning strings. */
    int          n_warnings;     /**< Number of warnings. */
} SWMM_ConversionResult;

/**
 * @brief Release heap memory owned by a conversion result.
 *
 * @details Does not free the struct itself.  Safe to call on a
 *          zero-initialised struct.
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
 * @param engine      Engine handle.
 * @param idx         Zero-based node index.
 * @param new_type    Target SWMM_NodeType value.
 * @param result_out  If non-NULL, receives the conversion result.  Caller must
 *                    free with @ref swmm_conversion_result_free.
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not BUILDING/OPENED,
 *          SWMM_ERR_BADPARAM if new_type is invalid or equals current type,
 *          SWMM_ERR_BADHANDLE, or SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_node_convert(
    SWMM_Engine engine, int idx, int new_type,
    SWMM_ConversionResult* result_out);

/**
 * @brief Convert a link to a different type in-place.
 *
 * @details Common properties (node1, node2, offset1, offset2, q0, q_limit,
 *          setting, comments) are preserved.  Type-specific fields are cleared
 *          and new-type defaults applied.
 *
 * @param engine      Engine handle.
 * @param idx         Zero-based link index.
 * @param new_type    Target SWMM_LinkType value.
 * @param result_out  If non-NULL, receives the conversion result.
 * @returns SWMM_OK on success, SWMM_ERR_BADPARAM if new_type equals current
 *          type, SWMM_ERR_LIFECYCLE, SWMM_ERR_BADHANDLE, SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_link_convert(
    SWMM_Engine engine, int idx, int new_type,
    SWMM_ConversionResult* result_out);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_EDIT_H */
