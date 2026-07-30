/**
 * @file openswmm_tables.h
 * @brief OpenSWMM Engine — Tables (Time Series & Curves) and Patterns C API.
 *
 * @details Table identity, creation, data point management, cursor-optimized
 *          lookup, and time pattern management.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_TABLES_H
#define OPENSWMM_TABLES_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of tables (time series + curves) in the model.
 * @param engine  Engine handle.
 * @returns Number of tables, or -1 on error.
 */
SWMM_ENGINE_API int swmm_table_count(SWMM_Engine engine);

/**
 * @brief Look up a table's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated table identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_table_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a table by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based table index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_table_id(SWMM_Engine engine, int idx);

/**
 * @brief Get a table's type code.
 *
 * @details Tables are stored in a single unified array; this lets callers
 *          partition the array into time series vs. each curve type
 *          (e.g. for the GUI's Data Objects browser). Type values mirror
 *          ::openswmm::TableType:
 *            0 = TIMESERIES
 *            1 = CURVE_STORAGE
 *            2 = CURVE_DIVERSION
 *            3 = CURVE_RATING
 *            4 = CURVE_SHAPE
 *            5 = CURVE_CONTROL
 *            6 = CURVE_TIDAL
 *            7..11 = CURVE_PUMP1..PUMP5
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based table index.
 * @param[out] type  Receives the type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_table_get_type(SWMM_Engine engine, int idx, int* type);

/* =========================================================================
 * Creation (BUILDING or OPENED — references resolve by name at initialize())
 * ========================================================================= */

/**
 * @brief Add a new time series to the model.
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param id      Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_timeseries_add(SWMM_Engine engine, const char* id);

/**
 * @brief Add a new curve to the model.
 *
 * @details Curve types: 0=STORAGE, 1=DIVERSION, 2=TIDAL, 3=RATING,
 *          4=CONTROL, 5=SHAPE, 6=PUMP1..PUMP4, etc.
 *
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param id      Unique null-terminated identifier.
 * @param type    Curve type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_curve_add(SWMM_Engine engine, const char* id, int type);

/* =========================================================================
 * Data points
 * ========================================================================= */

/**
 * @brief Append a data point (x, y) to a table.
 *
 * @details For time series, x is in decimal days; for curves, x depends on
 *          the curve type (e.g., depth for storage curves).
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based table index.
 * @param x       X value (independent variable).
 * @param y       Y value (dependent variable).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_table_add_point(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the number of data points in a table.
 * @param engine      Engine handle.
 * @param idx         Zero-based table index.
 * @param[out] count  Receives the point count.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_table_get_point_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get a specific data point from a table.
 * @param engine  Engine handle.
 * @param idx     Zero-based table index.
 * @param pt_idx  Zero-based point index within the table.
 * @param[out] x  Receives the X value.
 * @param[out] y  Receives the Y value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_table_get_point(SWMM_Engine engine, int idx, int pt_idx, double* x, double* y);

/**
 * @brief Remove all data points from a table.
 * @param engine  Engine handle.
 * @param idx     Zero-based table index.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_table_clear(SWMM_Engine engine, int idx);

/* =========================================================================
 * Lookup (cursor-optimized)
 * ========================================================================= */

/**
 * @brief Interpolate a Y value from a table for a given X.
 *
 * @details Uses cursor-based lookup for efficient sequential access
 *          (e.g., during time-stepping through a time series).
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based table index.
 * @param x       X value to look up.
 * @param[out] y  Receives the interpolated Y value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_table_lookup(SWMM_Engine engine, int idx, double x, double* y);

/* =========================================================================
 * Patterns
 * ========================================================================= */

/**
 * @brief Add a new time pattern to the model.
 *
 * @details Pattern types: 0=MONTHLY, 1=DAILY, 2=HOURLY, 3=WEEKEND.
 *
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param id      Unique null-terminated identifier.
 * @param type    Pattern type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pattern_add(SWMM_Engine engine, const char* id, int type);

/**
 * @brief Set the multiplier factors for a time pattern.
 * @param engine   Engine handle.
 * @param idx      Zero-based pattern index.
 * @param factors  Array of multiplier values.
 * @param count    Number of factors (e.g., 12 for MONTHLY, 24 for HOURLY).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pattern_set_factors(SWMM_Engine engine, int idx, const double* factors, int count);

/**
 * @brief Get the total number of time patterns in the model.
 * @param engine  Engine handle.
 * @returns Number of patterns, or -1 on error.
 */
SWMM_ENGINE_API int swmm_pattern_count(SWMM_Engine engine);

/**
 * @brief Look up a pattern's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated pattern identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_pattern_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a pattern by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based pattern index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_pattern_id(SWMM_Engine engine, int idx);

/**
 * @brief Get a pattern's type code.
 *
 * @details Pattern types: 0=MONTHLY, 1=DAILY, 2=HOURLY, 3=WEEKEND. Used by
 *          the GUI to filter pattern pickers by pattern kind (e.g. the
 *          four DWF picker rows each accept a specific type).
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based pattern index.
 * @param[out] type  Receives the pattern type code.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pattern_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Get the number of multiplier factors stored for a pattern.
 * @param engine      Engine handle.
 * @param idx         Zero-based pattern index.
 * @param[out] count  Receives the factor count (typically 12, 7, or 24).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pattern_get_factor_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get one multiplier factor from a pattern.
 * @param engine    Engine handle.
 * @param idx       Zero-based pattern index.
 * @param i         Zero-based factor index within the pattern.
 * @param[out] v    Receives the multiplier value.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_pattern_get_factor(SWMM_Engine engine, int idx, int i, double* v);

/**
 * @brief Remove a time pattern by index, clearing any reference sites.
 *
 * @details Walks every place the engine stores a pattern name and clears
 *          any entry matching the removed pattern: external inflows
 *          (`ext_inflows.pattern_name`), dry-weather-flow patterns
 *          (`dwf.pat1..pat4`), aquifer ET patterns
 *          (`aquifers.upper_evap_pat`), and the evaporation recovery
 *          option (`options.evap_recovery_pat`). The removal itself
 *          shifts subsequent pattern indices down by one — callers that
 *          hold cached pattern indices must re-resolve via
 *          ::swmm_pattern_index.
 *
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param idx     Zero-based pattern index.
 * @returns SWMM_OK on success (idempotent: returns SWMM_OK if @p idx is
 *          out of range, matching the GUI's repeated-click expectations);
 *          SWMM_ERR_LIFECYCLE if not editable.
 */
SWMM_ENGINE_API int swmm_pattern_remove(SWMM_Engine engine, int idx);

/**
 * @brief Rename a time pattern; updates every stored reference to the
 *        previous name across inflows, DWF, aquifer ET, and options.
 *
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param idx     Zero-based pattern index.
 * @param newId   New null-terminated identifier; must not already be in use.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if @p newId is empty or
 *          collides with an existing pattern; SWMM_ERR_LIFECYCLE if not
 *          editable; SWMM_ERR_BADINDEX if @p idx is out of range.
 */
SWMM_ENGINE_API int swmm_pattern_rename(SWMM_Engine engine, int idx, const char* newId);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_TABLES_H */
