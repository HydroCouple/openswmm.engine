/**
 * @file openswmm_gages.h
 * @brief OpenSWMM Engine — Rain Gage C API.
 *
 * @details Gage creation, property setters, rainfall get/set, bulk access.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_GAGES_H
#define OPENSWMM_GAGES_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Rain gage data source type.
 */
typedef enum SWMM_GageDataSource {
    SWMM_GAGE_TIMESERIES = 0, /**< Rainfall data from an in-model time series. */
    SWMM_GAGE_FILE       = 1  /**< Rainfall data from an external file. */
} SWMM_GageDataSource;

/**
 * @brief Rain gage rainfall data format.
 */
typedef enum SWMM_GageRainType {
    SWMM_RAIN_INTENSITY  = 0, /**< Rainfall given as intensity (rate). */
    SWMM_RAIN_VOLUME     = 1, /**< Rainfall given as depth per interval. */
    SWMM_RAIN_CUMULATIVE = 2  /**< Rainfall given as cumulative depth. */
} SWMM_GageRainType;

/* =========================================================================
 * Identity
 * ========================================================================= */

/**
 * @brief Get the total number of rain gages in the model.
 * @param engine  Engine handle.
 * @returns Number of gages, or -1 on error.
 */
SWMM_ENGINE_API int swmm_gage_count(SWMM_Engine engine);

/**
 * @brief Look up a rain gage's zero-based index by its string identifier.
 * @param engine  Engine handle.
 * @param id      Null-terminated gage identifier.
 * @returns Zero-based index, or -1 if not found.
 */
SWMM_ENGINE_API int swmm_gage_index(SWMM_Engine engine, const char* id);

/**
 * @brief Get the string identifier of a rain gage by index.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @returns Null-terminated string owned by the engine, or NULL on error.
 */
SWMM_ENGINE_API const char* swmm_gage_id(SWMM_Engine engine, int idx);

/* =========================================================================
 * Creation (BUILDING or OPENED — "editable" states)
 * ========================================================================= */

/**
 * @brief Add a new rain gage to the model.
 * @param engine  Engine handle (SWMM_STATE_BUILDING or SWMM_STATE_OPENED).
 * @param id      Unique null-terminated identifier for the new gage.
 * @returns SWMM_OK on success, SWMM_ERR_LIFECYCLE if not in an editable
 *          state, or another error code.
 */
SWMM_ENGINE_API int swmm_gage_add(SWMM_Engine engine, const char* id);

/* =========================================================================
 * Property setters (BUILDING or OPENED)
 * ========================================================================= */

/**
 * @brief Set the rainfall data format for a gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param type    Rainfall type (see @ref SWMM_GageRainType).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_set_rain_type(SWMM_Engine engine, int idx, int type);

/**
 * @brief Set the rainfall recording interval for a gage.
 * @param engine   Engine handle.
 * @param idx      Zero-based gage index.
 * @param seconds  Recording interval in seconds.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_set_rain_interval(SWMM_Engine engine, int idx, double seconds);

/**
 * @brief Set the data source type for a gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param source  Data source (see @ref SWMM_GageDataSource).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_set_data_source(SWMM_Engine engine, int idx, int source);

/**
 * @brief Assign a time series as the data source for a gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param ts_id   Null-terminated time series identifier.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_set_timeseries(SWMM_Engine engine, int idx, const char* ts_id);

/**
 * @brief Assign an external rainfall file as the data source for a gage.
 * @param engine      Engine handle.
 * @param idx         Zero-based gage index.
 * @param path        File path to the external rainfall file.
 * @param station_id  Station identifier within the file.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_set_filename(SWMM_Engine engine, int idx, const char* path,
                                                   const char* station_id);

/* =========================================================================
 * Property getters
 * ========================================================================= */

/**
 * @brief Get the rainfall data format for a gage.
 * @param engine     Engine handle.
 * @param idx        Zero-based gage index.
 * @param[out] type  Receives the rain type code (see @ref SWMM_GageRainType).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_rain_type(SWMM_Engine engine, int idx, int* type);

/**
 * @brief Get the data source type for a gage.
 * @param engine       Engine handle.
 * @param idx          Zero-based gage index.
 * @param[out] source  Receives the data source code (see @ref SWMM_GageDataSource).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_data_source(SWMM_Engine engine, int idx, int* source);

/* =========================================================================
 * State
 * ========================================================================= */

/** @brief Get current rainfall rate at a gage (project rate units). */
SWMM_ENGINE_API int swmm_gage_get_rainfall(SWMM_Engine engine, int idx, double* rainfall);

/**
 * @brief Override rainfall at a gage for the current timestep.
 *
 * @details Overrides gage-driven rainfall for all subcatchments that use
 *          this gage. Applied for one timestep only.
 */
SWMM_ENGINE_API int swmm_gage_set_rainfall(SWMM_Engine engine, int idx, double rainfall);

/* =========================================================================
 * Bulk access
 * ========================================================================= */

/**
 * @brief Get current rainfall rates for all gages in a single call.
 * @param engine    Engine handle.
 * @param[out] buf  Caller-allocated buffer of at least @p count doubles.
 * @param count     Number of elements (should equal swmm_gage_count()).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_rainfall_bulk(SWMM_Engine engine, double* buf, int count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_GAGES_H */
