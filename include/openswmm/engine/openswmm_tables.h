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

/* =========================================================================
 * Creation (BUILDING state only)
 * ========================================================================= */

/**
 * @brief Add a new time series to the model.
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
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
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
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
 * @param engine  Engine handle (SWMM_STATE_BUILDING).
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

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_TABLES_H */
