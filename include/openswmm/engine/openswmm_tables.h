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
 * @file      openswmm_tables.h
 * @brief     OpenSWMM Engine — Tables (Time Series & Curves) and Patterns C API.
 *
 * @details Table identity, creation, data point management, cursor-optimized
 *          lookup, and time pattern management.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
 * @param[in] engine Engine handle.
 * @returns Number of tables, or -1 on error.
 */
SWMM_ENGINE_API int swmm_table_count(SWMM_Engine engine);

/**
 * @brief Look up a table's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated table identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note Searches timeseries first, then curves; the two kinds may share an ID, so this unqualified
 *     lookup prefers the timeseries. Null engine/ID returns -1.
 */
SWMM_ENGINE_API int swmm_table_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a table by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note ID storage is borrowed until name/table/pattern changes, close/reopen or engine
 *     destruction; copy before retaining it across edits.
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
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @param[out] type Receives the type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. type must be non-null or BADPARAM is returned.
 */
SWMM_ENGINE_API int swmm_table_get_type(SWMM_Engine engine, int idx, int* type);

/* =========================================================================
 * Creation (BUILDING or OPENED — references resolve by name at initialize())
 * ========================================================================= */

/**
 * @brief Add a new time series to the model.
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] id Unique null-terminated identifier.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. ID is copied. Null or duplicate ID within the same table kind returns
 *     BADPARAM; curves and timeseries may share a name.
 */
SWMM_ENGINE_API int swmm_timeseries_add(SWMM_Engine engine, const char* id);

/**
 * @brief Add a new curve to the model.
 *
 * @details Curve types use the unified TableType codes: 1=STORAGE,
 *          2=DIVERSION, 3=RATING, 4=SHAPE, 5=CONTROL, 6=TIDAL,
 *          7..11=PUMP1..PUMP5. Code 0 denotes TIMESERIES, not STORAGE.
 *
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] id Unique null-terminated identifier.
 * @param[in] type Curve type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. ID is copied. Null or duplicate ID within the same table kind returns
 *     BADPARAM; curves and timeseries may share a name. type is stored by direct enum cast without
 *     range validation; supply the TableType curve codes documented by swmm_table_get_type(),
 *     rather than a legacy curve-only numbering.
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
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @param[in] x X value (independent variable).
 * @param[in] y Y value (dependent variable).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle, finite-value or ascending-x validation is imposed;
 *     the point appends in insertion order and does not reset the existing lookup cursor. Caller
 *     must maintain a usable ordered table. Time-series x is an absolute SWMM DateTime; curve x/y
 *     units depend on curve type.
 */
SWMM_ENGINE_API int swmm_table_add_point(SWMM_Engine engine, int idx, double x, double y);

/**
 * @brief Get the number of data points in a table.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @param[out] count Receives the point count.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note count may be NULL to suppress that output. A null engine returns SWMM_ERR_BADHANDLE. A
 *     failed table/pattern/pollutant index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_table_get_point_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get a specific data point from a table.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @param[in] pt_idx Zero-based point index within the table.
 * @param[out] x Receives the X value.
 * @param[out] y Receives the Y value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note x may be NULL to suppress that output. y may be NULL to suppress that output. A null engine
 *     returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check returns
 *     SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_table_get_point(SWMM_Engine engine, int idx, int pt_idx, double* x,
                                         double* y);

/**
 * @brief Remove all data points from a table.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard is imposed. Clears x/y, resets the lookup
 *     cursor and removes relative-row metadata (n_relative=0, anchor=0).
 */
SWMM_ENGINE_API int swmm_table_clear(SWMM_Engine engine, int idx);

/**
 * @brief Get a timeseries' time-only (relative) row info.
 *
 * Rows authored without a date in a `[TIMESERIES]` section are elapsed
 * times anchored at the simulation start (legacy SWMM seeds every series'
 * date anchor with START_DATE + START_TIME). The engine stores those rows
 * as absolute date/times after resolution; this reports how many leading
 * rows were authored that way and the start_date offset currently baked
 * into their x-values, so a caller can recover the authored elapsed times
 * as `x - anchor` and the .inp writer emits them back in time-only form.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index (must be a TIMESERIES).
 * @param[out] n_relative Leading rows authored as elapsed time (0 = all
 *                         rows carry explicit dates).
 * @param[out] anchor OADate offset baked into those rows' x-values.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note n_relative may be NULL to suppress that output. anchor may be NULL to suppress that output.
 *     A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. A non-TIMESERIES table returns BADPARAM; optional outputs may both
 *     be NULL. Metadata retrieval does not shift any x-values.
 */
SWMM_ENGINE_API int swmm_timeseries_get_relative_info(SWMM_Engine engine, int idx, int* n_relative,
                                                      double* anchor);

/**
 * @brief Declare a timeseries' leading rows as time-only (relative).
 *
 * Use after rebuilding a series through swmm_table_clear() /
 * swmm_table_add_point() to restore its authored time-only format: the
 * first @p n_relative points' x-values are treated as absolute date/times
 * containing @p anchor as their simulation-start offset, and the .inp
 * writer emits them as elapsed times (`x - anchor`) rather than dates.
 * Pass 0 to mark every row as explicitly dated.
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index (must be a TIMESERIES).
 * @param[in] n_relative Leading rows authored as elapsed time (clamped to the
 *                    current point count).
 * @param[in] anchor OADate offset baked into those rows' x-values.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard is imposed. A non-TIMESERIES table or negative
 *     n_relative returns BADPARAM; count is clamped to current point count. Stores anchor directly
 *     without shifting x-values or checking finite dates.
 */
SWMM_ENGINE_API int swmm_timeseries_set_relative_info(SWMM_Engine engine, int idx, int n_relative,
                                                      double anchor);

/* =========================================================================
 * Lookup (cursor-optimized)
 * ========================================================================= */

/**
 * @brief Interpolate a Y value from a table for a given X.
 *
 * @details Uses cursor-based lookup for efficient sequential access
 *          (e.g., during time-stepping through a time series).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based table index.
 * @param[in] x X value to look up.
 * @param[out] y Receives the interpolated Y value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. y must be non-null or BADPARAM is returned. Empty tables return 0;
 *     a singleton or out-of-range query returns the nearest endpoint value. Interior values use
 *     linear interpolation; nearly duplicate x entries use their mean. Updates the table cursor, so
 *     exclusive access is required even though x/y data are not changed.
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
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] id Unique null-terminated identifier.
 * @param[in] type Pattern type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. Copies the ID and appends an empty-factor row. This wrapper does not
 *     enforce uniqueness or a type range; caller must supply an unambiguous name and supported
 *     type.
 */
SWMM_ENGINE_API int swmm_pattern_add(SWMM_Engine engine, const char* id, int type);

/**
 * @brief Set the multiplier factors for a time pattern.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pattern index.
 * @param[in] factors Array of multiplier values.
 * @param[in] count Number of factors (e.g., 12 for MONTHLY, 24 for HOURLY).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. No lifecycle guard is imposed. factors must be non-null and count
 *     positive or BADPARAM is returned. Copies exactly count values and refreshes the inflow solver
 *     pattern cache for next-step use; factor count/type compatibility and finite/range values are
 *     not checked.
 */
SWMM_ENGINE_API int swmm_pattern_set_factors(SWMM_Engine engine, int idx, const double* factors,
                                             int count);

/**
 * @brief Get the total number of time patterns in the model.
 * @param[in] engine Engine handle.
 * @returns Number of patterns, or -1 on error.
 */
SWMM_ENGINE_API int swmm_pattern_count(SWMM_Engine engine);

/**
 * @brief Look up a pattern's zero-based index by its string identifier.
 * @param[in] engine Engine handle.
 * @param[in] id Null-terminated pattern identifier.
 * @returns Zero-based index, or -1 if not found.
 *
 *
 * @note ID matching is case-insensitive; null engine/ID or absent name returns -1.
 */
SWMM_ENGINE_API int swmm_pattern_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a pattern by index.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pattern index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 *
 *
 * @note ID storage is borrowed until name/table/pattern changes, close/reopen or engine
 *     destruction; copy before retaining it across edits.
 */
SWMM_ENGINE_API const char* swmm_pattern_id(SWMM_Engine engine, int idx);

/**
 * @brief Get a pattern's type code.
 *
 * @details Pattern types: 0=MONTHLY, 1=DAILY, 2=HOURLY, 3=WEEKEND. Used by
 *          the GUI to filter pattern pickers by pattern kind (e.g. the
 *          four DWF picker rows each accept a specific type).
 *
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pattern index.
 * @param[out] type Receives the pattern type code.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. type must be non-null or BADPARAM is returned.
 */
SWMM_ENGINE_API int swmm_pattern_get_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Get the number of multiplier factors stored for a pattern.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pattern index.
 * @param[out] count Receives the factor count (typically 12, 7, or 24).
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. count must be non-null or BADPARAM is returned.
 */
SWMM_ENGINE_API int swmm_pattern_get_factor_count(SWMM_Engine engine, int idx, int* count);

/**
 * @brief Get one multiplier factor from a pattern.
 * @param[in] engine Engine handle.
 * @param[in] idx Zero-based pattern index.
 * @param[in] i Zero-based factor index within the pattern.
 * @param[out] v Receives the multiplier value.
 * @returns SWMM_OK on success, or an error code.
 *
 *
 * @note A null engine returns SWMM_ERR_BADHANDLE. A failed table/pattern/point index check
 *     returns SWMM_ERR_BADINDEX. v must be non-null or BADPARAM is returned.
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
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] idx Zero-based pattern index.
 * @returns SWMM_OK on success (idempotent: returns SWMM_OK if @p idx is
 *          out of range, matching the GUI's repeated-click expectations);
 *          SWMM_ERR_LIFECYCLE if not editable.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE.
 */
SWMM_ENGINE_API int swmm_pattern_remove(SWMM_Engine engine, int idx);

/**
 * @brief Rename a time pattern; updates every stored reference to the
 *        previous name across inflows, DWF, aquifer ET, and options.
 *
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] idx Zero-based pattern index.
 * @param[in] newId New null-terminated identifier; must not already be in use.
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if @p newId is empty or
 *          collides with an existing pattern; SWMM_ERR_LIFECYCLE if not
 *          editable; SWMM_ERR_BADINDEX if @p idx is out of range.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed table/pattern/point index check returns SWMM_ERR_BADINDEX.
 *     New ID is copied and checked case-insensitively against other patterns; a case-only
 *     respelling of this row is allowed. Stored pattern-reference rewriting matches the previous
 *     spelling exactly. Null/empty names return BADPARAM.
 */
SWMM_ENGINE_API int swmm_pattern_rename(SWMM_Engine engine, int idx, const char* newId);

/**
 * @brief Rename a timeseries or curve; rewrites every name-based reference
 *        to it (gages, external inflows, outfall stage data, climate options,
 *        snow radiation series, storage/divider/pump/inlet curves and 2D
 *        boundary series/rating curves). Index-based references follow on
 *        their own.
 *
 * @details Uniqueness is kind-scoped and case-insensitive, as in
 *          swmm_timeseries_add / swmm_curve_add: a curve and a timeseries may
 *          share a name. Control-rule text is stored verbatim and is not
 *          rewritten.
 *
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] idx Zero-based table index.
 * @param[in] newId New null-terminated identifier.
 * @returns SWMM_OK on success (also when @p newId equals the current id);
 *          SWMM_ERR_BADPARAM if @p newId is empty or names another table of
 *          the same kind; SWMM_ERR_LIFECYCLE if not editable;
 *          SWMM_ERR_BADINDEX if @p idx is out of range.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed table/pattern/point index check returns SWMM_ERR_BADINDEX.
 *     New ID is copied; null/empty names return BADPARAM. Rewrites recognized stored name-reference
 *     sites case-insensitively, including applicable 2D boundaries, while preserving the numeric
 *     table index.
 */
SWMM_ENGINE_API int swmm_table_rename(SWMM_Engine engine, int idx, const char* newId);

/**
 * @brief Change a curve's type (STORAGE … PUMP5) in place, keeping its points
 *        and every reference to it.
 *
 * @param[in] engine Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param[in] idx Zero-based table index of a curve.
 * @param[in] type New curve type code (1 = STORAGE … 11 = PUMP5).
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if @p idx is a timeseries or
 *          @p type is not a curve type; SWMM_ERR_LIFECYCLE if not editable;
 *          SWMM_ERR_BADINDEX if @p idx is out of range.
 *
 *
 * @note Requires BUILDING or OPENED; other states return SWMM_ERR_LIFECYCLE. A null engine returns
 *     SWMM_ERR_BADHANDLE. A failed table/pattern/point index check returns SWMM_ERR_BADINDEX.
 */
SWMM_ENGINE_API int swmm_table_set_type(SWMM_Engine engine, int idx, int type);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_TABLES_H */
