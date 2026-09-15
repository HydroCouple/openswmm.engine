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
 * @license  Apache-2.0
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
 * @param station_id  Station identifier within the file (standard SWMM rain
 *                    file grammar `Fname Station Units`).
 * @returns SWMM_OK on success, or an error code.
 *
 * @details Sets the data source to FILE. The file format is preserved when
 *          the gage is already USER_CSV (multi-column "path:col"), otherwise
 *          it is auto-detected: a non-empty file column implies USER_CSV and
 *          anything else selects the standard SWMM rain file format
 *          (STAN_PRCP). The station id is stored in the gage's `station_id`
 *          slot (the token matched against the file's first column), not the
 *          CSV column-name slot — see swmm_gage_set_file_column().
 */
SWMM_ENGINE_API int swmm_gage_set_filename(SWMM_Engine engine, int idx, const char* path,
                                                   const char* station_id);

/**
 * @brief Set the station id for a file-based gage (standard SWMM rain file).
 * @param engine      Engine handle.
 * @param idx         Zero-based gage index.
 * @param station_id  Station identifier within the file ("*" or empty = all rows).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_set_station_id(SWMM_Engine engine, int idx, const char* station_id);

/**
 * @brief Set the data column name for a multi-column rain-file gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param column  Column header name inside the file (empty = use the first
 *                data column).
 * @returns SWMM_OK on success, or an error code.
 *
 * @details Stores the column selector used by the "FILE path:col" form
 *          (multi-column CSV/TSV/TSF). Setting a non-empty column switches
 *          the gage's file format to USER_CSV; clearing it leaves the format
 *          unchanged (an empty column on a USER_CSV gage reads the file's
 *          first data column).
 */
SWMM_ENGINE_API int swmm_gage_set_file_column(SWMM_Engine engine, int idx, const char* column);

/**
 * @brief Set the rain file format for a file-based gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param format  RainFileFormat code (-1 = UNKNOWN, 5 = STAN_PRCP standard
 *                SWMM rain file, 6 = USER_CSV multi-column CSV/TSV/TSF).
 * @returns SWMM_OK on success, or SWMM_ERR_BADPARAM for a code that is not a
 *          RainFileFormat value.
 *
 * @details The counterpart of swmm_gage_get_file_format(), and the only way
 *          back out of USER_CSV: swmm_gage_set_file_column() and
 *          swmm_gage_set_filename() both preserve USER_CSV by design, so
 *          without this a host that ever set a column could not return the
 *          gage to a standard rain file.
 *
 *          The two formats' row selectors are mutually exclusive, so this
 *          clears the one that does not apply: selecting USER_CSV clears the
 *          gage's `station_id` (a multi-column file has no station column),
 *          and selecting any station-based format — STAN_PRCP included —
 *          clears its file column name.
 */
SWMM_ENGINE_API int swmm_gage_set_file_format(SWMM_Engine engine, int idx, int format);

/**
 * @brief Set the snow-catch deficiency correction factor (SCF) for a gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param factor  Strictly positive correction factor (1.0 = no correction).
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if factor <= 0.0.
 */
SWMM_ENGINE_API int swmm_gage_set_snow_factor(SWMM_Engine engine, int idx, double factor);

/**
 * @brief Set the rain-depth units declared for a file-based gage.
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param units   0 = IN (inches), 1 = MM (millimetres).
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if units not in {0,1}.
 */
SWMM_ENGINE_API int swmm_gage_set_rain_units(SWMM_Engine engine, int idx, int units);

/**
 * @brief Set the rainfall scaling factor for a gage.
 *
 * @details The scaling factor multiplies the gage's rainfall intensity after
 *          rain-type and unit conversion. May be changed at any time (including
 *          while the simulation is RUNNING) to support parameter sweeps; the
 *          new value takes effect on the next timestep.
 *
 * @param engine  Engine handle.
 * @param idx     Zero-based gage index.
 * @param factor  Strictly positive scaling factor (1.0 = no scaling).
 * @returns SWMM_OK on success; SWMM_ERR_BADPARAM if factor <= 0.0.
 */
SWMM_ENGINE_API int swmm_gage_set_scale_factor(SWMM_Engine engine, int idx, double factor);

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

/**
 * @brief Get the rainfall scaling factor for a gage.
 * @param engine       Engine handle.
 * @param idx          Zero-based gage index.
 * @param[out] factor  Receives the current scaling factor (1.0 = no scaling).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_scale_factor(SWMM_Engine engine, int idx, double* factor);

/**
 * @brief Get the rainfall recording interval for a gage.
 * @param engine        Engine handle.
 * @param idx           Zero-based gage index.
 * @param[out] seconds  Receives the recording interval in seconds.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_rain_interval(SWMM_Engine engine, int idx, double* seconds);

/**
 * @brief Get the snow-catch deficiency correction factor (SCF) for a gage.
 * @param engine       Engine handle.
 * @param idx          Zero-based gage index.
 * @param[out] factor  Receives the current correction factor (1.0 = none).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_snow_factor(SWMM_Engine engine, int idx, double* factor);

/**
 * @brief Get the assigned time series id for a TIMESERIES-source gage.
 * @param engine       Engine handle.
 * @param idx          Zero-based gage index.
 * @param[out] buf     Caller buffer that receives the NUL-terminated id
 *                    (empty string when no series is assigned).
 * @param buflen       Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_timeseries(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get the station id for a file-based gage.
 * @param engine       Engine handle.
 * @param idx          Zero-based gage index.
 * @param[out] buf     Caller buffer that receives the NUL-terminated station
 *                    id (empty string when none is set).
 * @param buflen       Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_station_id(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get the data column name for a multi-column rain-file gage.
 * @param engine       Engine handle.
 * @param idx          Zero-based gage index.
 * @param[out] buf     Caller buffer that receives the NUL-terminated column
 *                    name (empty string when none is set).
 * @param buflen       Size of @p buf in bytes.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_file_column(SWMM_Engine engine, int idx, char* buf, int buflen);

/**
 * @brief Get the rain file format for a file-based gage.
 * @param engine        Engine handle.
 * @param idx           Zero-based gage index.
 * @param[out] format   Receives the RainFileFormat code (-1 = UNKNOWN,
 *                      5 = STAN_PRCP standard SWMM rain file, 6 = USER_CSV
 *                      multi-column CSV/TSV/TSF). Meaningful only when the
 *                      data source is FILE.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_file_format(SWMM_Engine engine, int idx, int* format);

/**
 * @brief Get the rain-depth units declared for a file-based gage.
 * @param engine        Engine handle.
 * @param idx           Zero-based gage index.
 * @param[out] units    Receives 0 = IN (inches) or 1 = MM (millimetres).
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_rain_units(SWMM_Engine engine, int idx, int* units);

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

/** @brief Rename the rain gage at `idx` to `newId`.
 *  Returns SWMM_ERR_BADPARAM if newId is null, empty, already in use, or
 *  idx is out of range. */
SWMM_ENGINE_API int swmm_gage_rename(SWMM_Engine engine, int idx, const char* newId);

/* =========================================================================
 * Resolved rainfall series
 * ========================================================================= */

/**
 * @brief Number of entries in a gage's resolved rainfall series.
 *
 * @details Works for both data sources: a TIMESERIES gage reports its table's
 *          length, a FILE gage the length of the series loaded from disk at
 *          open. A FILE gage reports 0 when its file could not be read or its
 *          format is not one the engine loads — in which case that gage also
 *          contributes no rainfall to the run.
 *
 * @param engine     Engine handle.
 * @param idx        Zero-based gage index.
 * @param[out] count Receives the entry count.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_rainfall_series_count(SWMM_Engine engine, int idx,
                                                        int* count);

/**
 * @brief Copy a gage's resolved rainfall series.
 *
 * @details Returns the rainfall the engine will ACTUALLY apply, so a caller
 *          never has to know how a gage stores its data. The rain-type
 *          transform, the rain-file units factor, and the gage scale factor
 *          are all applied, using the same conversion the routing update runs
 *          — so a series read back here and replayed through a TIMESERIES gage
 *          of type INTENSITY reproduces the original gage.
 *
 *          Each entry is the intensity that applies from its own time stamp
 *          until the recording interval elapses or the next entry begins,
 *          whichever comes first; rainfall is zero in between. Pair this with
 *          swmm_gage_get_rain_interval() to reconstruct that behaviour.
 *
 * @note A FILE gage's series is windowed to the [OPTIONS] simulation dates
 *       (± one day) and reflects the file as it was read at open. Call
 *       swmm_gage_reload_rain_files() first if the path, station, units, or
 *       simulation dates have changed since.
 *
 * @param engine      Engine handle.
 * @param idx         Zero-based gage index.
 * @param[out] times  Receives entry times as SWMM DateTime (decimal days),
 *                    matching swmm_table_get_point(). May be NULL.
 * @param[out] values Receives rainfall INTENSITY in the project's rain units
 *                    per hour (in/hr for US flow units, mm/hr for SI).
 *                    May be NULL.
 * @param count       Capacity of each output array; at most this many entries
 *                    are written.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_get_rainfall_series(SWMM_Engine engine, int idx,
                                                  double* times, double* values,
                                                  int count);

/**
 * @brief Re-read every FILE-source rain gage's data from disk.
 *
 * @details Rain files are loaded once, during open(). Nothing re-runs that
 *          afterwards, so changing a gage's path, station id, or rain units —
 *          or the simulation dates the data is windowed to — has no effect
 *          until the model is reopened, and readers keep seeing the previous
 *          file's contents. Call this after such an edit.
 *
 *          Rebuilds the resolved series and the rainfall-file summary
 *          statistics for every FILE gage. Requires the model to be editable
 *          (BUILDING or OPENED); it is not valid mid-run.
 *
 * @param engine Engine handle.
 * @returns SWMM_OK on success, or an error code.
 */
SWMM_ENGINE_API int swmm_gage_reload_rain_files(SWMM_Engine engine);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_GAGES_H */
