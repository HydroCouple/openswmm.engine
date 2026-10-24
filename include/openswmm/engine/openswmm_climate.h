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
 * @file      openswmm_climate.h
 * @brief     OpenSWMM Engine — Climatology configuration C API.
 *
 * @details Read/write accessors for the model's climate *configuration* — the
 *          data parsed from the [TEMPERATURE], [EVAPORATION], [WINDSPEED]
 *          (a sub-keyword of [TEMPERATURE]), [ADJUSTMENTS], and the snowmelt
 *          globals / areal-depletion curves. This complements the runtime
 *          climate *forcing* API in openswmm_forcing.h, which overrides the
 *          live air-temperature / wind / evaporation values while a simulation
 *          is RUNNING. The accessors here edit the inputs *before* a run.
 *
 *          Units: every value is expressed in the project's display units —
 *          exactly as it appears in the .inp file. The engine keeps these
 *          configuration values verbatim in SimulationOptions /
 *          SimulationContext and defers the internal-unit conversion to
 *          swmm_engine_initialize(); the accessors here are therefore a
 *          straight pass-through (no unit conversion at the boundary).
 *
 *          Setters require an editable lifecycle state (BUILDING or OPENED) and
 *          return SWMM_ERR_LIFECYCLE otherwise — once the simulation is
 *          initialized the configuration has already been baked into the
 *          climate/snow solver state. Getters work in any readable state.
 *
 *          DRY_ONLY is exposed by swmm_climate_get_dry_only /
 *          swmm_climate_set_dry_only in openswmm_forcing.h (not duplicated
 *          here). The climate temperature/evaporation FILE path is managed via
 *          swmm_file_path_get / swmm_file_path_set (SWMM_FILE_CLIMATE_TEMP) in
 *          openswmm_model.h.
 *
 * @ingroup engine_api
 * @see openswmm_engine.h
 * @see openswmm_forcing.h   — runtime climate forcing + DRY_ONLY
 * @see Legacy reference: src/legacy/engine/climate.c
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_CLIMATE_H
#define OPENSWMM_CLIMATE_H

#include "openswmm_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Number of monthly values in a climate monthly table. */
#define SWMM_CLIMATE_MONTHS 12

/** @brief Number of points in an areal-depletion curve. */
#define SWMM_CLIMATE_ADC_POINTS 10

/**
 * @brief Air-temperature data source (matches [TEMPERATURE]).
 */
typedef enum SWMM_TempSource {
    SWMM_TEMP_NONE = 0,       /**< No temperature data (snowmelt/PET disabled). */
    SWMM_TEMP_TIMESERIES = 1, /**< Temperature from an in-model time series.    */
    SWMM_TEMP_FILE = 2        /**< Temperature from an external climate file.   */
} SWMM_TempSource;

/**
 * @brief Evaporation method (matches [EVAPORATION]).
 */
typedef enum SWMM_EvapType {
    SWMM_EVAP_CONSTANT = 0,    /**< Single constant rate.                       */
    SWMM_EVAP_MONTHLY = 1,     /**< Twelve monthly rates.                       */
    SWMM_EVAP_TIMESERIES = 2,  /**< Rate from an in-model time series.          */
    SWMM_EVAP_TEMPERATURE = 3, /**< Hargreaves method from temperature.         */
    SWMM_EVAP_FILE = 4         /**< Pan rate from the external climate file.    */
} SWMM_EvapType;

/** Availability of one source-rate sample returned by the evaporation preview. */
typedef enum SWMM_EvapPreviewStatus {
    SWMM_EVAP_PREVIEW_AVAILABLE = 0, /**< Source and rate are available. */
    SWMM_EVAP_PREVIEW_HELD = 1,      /**< Engine holds the previous/edge value. */
    SWMM_EVAP_PREVIEW_MISSING = 2,   /**< No configured source value is available. */
    SWMM_EVAP_PREVIEW_ESTIMATED = 3  /**< Temperature-derived daily estimate. */
} SWMM_EvapPreviewStatus;

/** One evaporation source-rate sample; rates use the model's display units. */
typedef struct SWMM_EvapPreviewSample {
    double date;           /**< OADate at which the sample is evaluated. */
    double source_rate;    /**< Rate before the monthly evaporation adjustment. */
    double adjustment;     /**< Adjustment actually applied at this sample. */
    double effective_rate; /**< Source plus adjustment, matching climate forcing. */
    int status;            /**< One of SWMM_EvapPreviewStatus. */
} SWMM_EvapPreviewSample;

/** Summary counts for one evaporation preview window. */
typedef struct SWMM_EvapPreviewSummary {
    int source_type; /**< One of SWMM_EvapType. */
    int available_count;
    int held_count;
    int missing_count;
    int estimated_count;
    int runtime_forcing_unknown; /**< Always 1: live runtime forcing is excluded. */
    double minimum_rate;         /**< Minimum finite effective rate; NAN if none. */
    double maximum_rate;         /**< Maximum finite effective rate; NAN if none. */
} SWMM_EvapPreviewSummary;

/** Draft values for previewing evaporation before the climatology dialog saves. */
typedef struct SWMM_EvapPreviewConfig {
    int evap_type;
    double evap_monthly[SWMM_CLIMATE_MONTHS];
    double pan_coeff[SWMM_CLIMATE_MONTHS];
    double adjust_evaporation[SWMM_CLIMATE_MONTHS];
    const char* evap_timeseries;
    int temp_source;
    const char* temp_timeseries;
    const char* climate_file_path;
    double climate_file_start;
    int climate_file_temp_units;
    double latitude;
    double longitude_correction_minutes;
    double adjust_temperature[SWMM_CLIMATE_MONTHS];
} SWMM_EvapPreviewConfig;

/**
 * @brief Wind-speed data source (matches [TEMPERATURE] WINDSPEED).
 */
typedef enum SWMM_WindType {
    SWMM_WIND_MONTHLY = 0, /**< Twelve monthly average wind speeds. */
    SWMM_WIND_FILE = 1     /**< Wind speed from the climate file.   */
} SWMM_WindType;

/**
 * @brief Humidity data source (matches [TEMPERATURE] HUMIDITY).
 */
typedef enum SWMM_HumidityType {
    SWMM_HUMIDITY_CONSTANT = 0,  /**< Single constant value.              */
    SWMM_HUMIDITY_MONTHLY = 1,   /**< Twelve monthly values.              */
    SWMM_HUMIDITY_TIMESERIES = 2 /**< Value from an in-model time series. */
} SWMM_HumidityType;

/**
 * @brief Quantity the HUMIDITY values express.
 */
typedef enum SWMM_HumidityVar {
    SWMM_HUMIDITY_RELATIVE = 0, /**< Relative humidity, %.                              */
    SWMM_HUMIDITY_DEWPOINT = 1  /**< Dew-point temperature (deg F US / deg C SI);
                                     converted to RH each step from air temperature. */
} SWMM_HumidityVar;

/* =========================================================================
 * Temperature  ([TEMPERATURE])
 * ========================================================================= */

/**
 * @brief Read the configured air-temperature source.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] source Optional destination: SWMM_TempSource: 0=NONE, 1=TIMESERIES, 2=FILE.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_temp_source(SWMM_Engine engine, int* source);
/**
 * @brief Set the configured air-temperature source.
 *
 * Selecting a source does not validate its file or series reference.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] source SWMM_TempSource: 0=NONE, 1=TIMESERIES, 2=FILE. Values outside 0..2 are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_temp_source(SWMM_Engine engine, int source);

/**
 * @brief Copy the configured temperature time-series ID.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned buffer; empty when no ID is configured, NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     buffer/capacity.
 */
SWMM_ENGINE_API int swmm_climate_get_temp_timeseries(SWMM_Engine engine, char* buf, int buflen);
/**
 * @brief Copy a configured temperature time-series ID.
 *
 * Also sets temperature source to TIMESERIES, including for an empty ID. This does not add a
 * missing series/pattern or open a data file.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] ts_id Non-null NUL-terminated identifier to copy; empty is accepted. The referenced
 *     object need not resolve at this authoring call.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_temp_timeseries(SWMM_Engine engine, const char* ts_id);

/**
 * @brief Read the configured climate-file start date.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] date Optional destination: SWMM DateTime decimal days since 1899-12-30; 0 starts at
 *     the file beginning.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_temp_file_start(SWMM_Engine engine, double* date);
/**
 * @brief Set the configured climate-file start date.
 *
 * No finite/calendar validation is applied; caller must provide a valid date.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] date SWMM DateTime decimal days since 1899-12-30; 0 starts at the file beginning.
 *     Stored directly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_climate_set_temp_file_start(SWMM_Engine engine, double date);

/**
 * @brief Read the configured climate-file temperature-unit selector.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] units Optional destination: -1=unspecified per-format default, 0=tenths degrees C,
 *     1=degrees C, 2=degrees F.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_temp_units(SWMM_Engine engine, int* units);
/**
 * @brief Set the configured climate-file temperature-unit selector.
 *
 * Applies to reading climate-file records, not the model display-unit choice.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] units -1=unspecified per-format default, 0=tenths degrees C, 1=degrees C, 2=degrees F.
 *     Values outside -1..2 are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_temp_units(SWMM_Engine engine, int units);

/**
 * @brief Read the configured site elevation.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] elev Optional destination: project display length (ft in US, m in SI).
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_elevation(SWMM_Engine engine, double* elev);
/**
 * @brief Set the configured site elevation.
 *
 * Used for psychrometric/snow calculations. Assigned without finite/range validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] elev project display length (ft in US, m in SI). Stored directly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_climate_set_elevation(SWMM_Engine engine, double elev);

/**
 * @brief Read the configured site latitude.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] latitude Optional destination: degrees north in [-90,90].
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_latitude(SWMM_Engine engine, double* latitude);
/**
 * @brief Set the configured site latitude.
 *
 * Used by Hargreaves evaporation and snowmelt. Caller must provide a finite value; the range guard
 * does not independently reject NaN.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] latitude degrees north in [-90,90]. Values outside [-90,90] are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_latitude(SWMM_Engine engine, double latitude);

/**
 * @brief Read the configured longitude/solar-time correction.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] minutes Optional destination: minutes; 0 selects true solar time.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_longitude_correction(SWMM_Engine engine, double* minutes);
/**
 * @brief Set the configured longitude/solar-time correction.
 *
 * Matches the legacy SNOWMELT correction field; assigned without finite/range validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] minutes minutes; 0 selects true solar time. Stored directly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_climate_set_longitude_correction(SWMM_Engine engine, double minutes);

/* =========================================================================
 * Evaporation  ([EVAPORATION])
 * ========================================================================= */

/**
 * @brief Read the configured evaporation method.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] type Optional destination: SWMM_EvapType: CONSTANT=0, MONTHLY=1, TIMESERIES=2,
 *     TEMPERATURE=3, FILE=4.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_evap_type(SWMM_Engine engine, int* type);
/**
 * @brief Set the configured evaporation method.
 *
 * Stores the method only; associated rates/series/file configuration are managed separately.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] type SWMM_EvapType: CONSTANT=0, MONTHLY=1, TIMESERIES=2, TEMPERATURE=3, FILE=4. Values
 *     outside 0..4 are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_evap_type(SWMM_Engine engine, int type);

/**
 * @brief Copy all monthly evaporation rates in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in in/day in US or mm/day in
 *     SI.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_evap_monthly(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly evaporation rates in January through December.
 *
 * All 12 supplied values are retained; CONSTANT runtime use selects the first value. The setter
 * does not replicate it across months or change the method. Caller must supply physically valid
 * finite values; this setter copies entries without additional validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in in/day in US or mm/day in SI; copied
 *     synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_evap_monthly(SWMM_Engine engine, const double* values,
                                                  int count);

/**
 * @brief Copy the configured evaporation time-series ID.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned buffer; empty when no ID is configured, NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     buffer/capacity.
 */
SWMM_ENGINE_API int swmm_climate_get_evap_timeseries(SWMM_Engine engine, char* buf, int buflen);
/**
 * @brief Copy a configured evaporation time-series ID.
 *
 * Also sets evaporation method to TIMESERIES, including for an empty ID. This does not add a
 * missing series/pattern or open a data file.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] ts_id Non-null NUL-terminated identifier to copy; empty is accepted. The referenced
 *     object need not resolve at this authoring call.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_evap_timeseries(SWMM_Engine engine, const char* ts_id);

/**
 * @brief Preview saved evaporation without runtime forcing.
 *
 * Samples [start_date,end_date) at the specified interval. TIMESERIES uses step-and-hold
 * scheduling; FILE holds the last valid pan rate. TEMPERATURE uses daily Hargreaves estimates
 * because runtime air forcing and climate-step frequency are unknown. System/subcatchment forcing
 * is excluded and runtime_forcing_unknown is always 1. Missing file/series data can yield MISSING
 * samples and a diagnostic while returning success; inspect statuses and summary rather than return
 * code alone.
 *
 * @param[in] engine Non-null live engine, excluding RUNNING; no simulation state or stored options
 *     are changed.
 * @param[in] start_date Finite inclusive OADate/SWMM DateTime window start, within model simulation
 *     dates.
 * @param[in] end_date Finite exclusive window end, greater than start_date and within simulation
 *     dates (1e-10 day tolerance).
 * @param[in] sample_interval_days Finite positive sampling interval in days; the window must
 *     produce between 1 and 1,000,000 samples.
 * @param[out] samples Caller-owned capacity-record array; NULL with capacity=0 queries the count
 *     and summary. A short non-null array is refused before sampling.
 * @param[in] capacity Nonnegative number of available sample records; nonzero requires non-null
 *     samples.
 * @param[out] sample_count Non-null required-count output, written after validating the time window
 *     and before short-buffer/draft refusals.
 * @param[out] summary Non-null coverage/rate-summary destination; written after successful sampling
 *     even in count-only mode. Rates use project evaporation units (in/day US, mm/day SI).
 * @param[out] diagnostic Non-null caller-owned diagnostic buffer; cleared after basic validation,
 *     NUL-terminated/truncated for later errors or unavailable file-source notices.
 * @param[in] diagnostic_capacity Positive diagnostic capacity in bytes including NUL.
 * @return SWMM_OK for a completed preview (including missing-source samples); SWMM_ERR_BADHANDLE
 *     for null engine, SWMM_ERR_LIFECYCLE while RUNNING, or SWMM_ERR_BADPARAM for invalid
 *     inputs/draft, unsupported sample count or insufficient sample capacity.
 */
SWMM_ENGINE_API int swmm_climate_preview_evaporation(SWMM_Engine engine, double start_date,
                                                     double end_date, double sample_interval_days,
                                                     SWMM_EvapPreviewSample* samples, int capacity,
                                                     int* sample_count,
                                                     SWMM_EvapPreviewSummary* summary,
                                                     char* diagnostic, int diagnostic_capacity);

/**
 * @brief Preview draft or saved evaporation without runtime forcing.
 *
 * Samples [start_date,end_date) at the specified interval. TIMESERIES uses step-and-hold
 * scheduling; FILE holds the last valid pan rate. TEMPERATURE uses daily Hargreaves estimates
 * because runtime air forcing and climate-step frequency are unknown. System/subcatchment forcing
 * is excluded and runtime_forcing_unknown is always 1. Missing file/series data can yield MISSING
 * samples and a diagnostic while returning success; inspect statuses and summary rather than return
 * code alone.
 *
 * @param[in] engine Non-null live engine, excluding RUNNING; no simulation state or stored options
 *     are changed.
 * @param[in] config Optional borrowed complete preview draft; NULL uses saved values. String fields
 *     may be NULL for empty references and are copied for this call. Arrays must contain finite
 *     values; source selectors/site metadata are validated.
 * @param[in] start_date Finite inclusive OADate/SWMM DateTime window start, within model simulation
 *     dates.
 * @param[in] end_date Finite exclusive window end, greater than start_date and within simulation
 *     dates (1e-10 day tolerance).
 * @param[in] sample_interval_days Finite positive sampling interval in days; the window must
 *     produce between 1 and 1,000,000 samples.
 * @param[out] samples Caller-owned capacity-record array; NULL with capacity=0 queries the count
 *     and summary. A short non-null array is refused before sampling.
 * @param[in] capacity Nonnegative number of available sample records; nonzero requires non-null
 *     samples.
 * @param[out] sample_count Non-null required-count output, written after validating the time window
 *     and before short-buffer/draft refusals.
 * @param[out] summary Non-null coverage/rate-summary destination; written after successful sampling
 *     even in count-only mode. Rates use project evaporation units (in/day US, mm/day SI).
 * @param[out] diagnostic Non-null caller-owned diagnostic buffer; cleared after basic validation,
 *     NUL-terminated/truncated for later errors or unavailable file-source notices.
 * @param[in] diagnostic_capacity Positive diagnostic capacity in bytes including NUL.
 * @return SWMM_OK for a completed preview (including missing-source samples); SWMM_ERR_BADHANDLE
 *     for null engine, SWMM_ERR_LIFECYCLE while RUNNING, or SWMM_ERR_BADPARAM for invalid
 *     inputs/draft, unsupported sample count or insufficient sample capacity.
 */
SWMM_ENGINE_API int swmm_climate_preview_evaporation_with_config(
    SWMM_Engine engine, const SWMM_EvapPreviewConfig* config, double start_date, double end_date,
    double sample_interval_days, SWMM_EvapPreviewSample* samples, int capacity, int* sample_count,
    SWMM_EvapPreviewSummary* summary, char* diagnostic, int diagnostic_capacity);

/**
 * @brief Copy all monthly pan coefficients in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in dimensionless multipliers
 *     applied by FILE evaporation.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_pan_coeff(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly pan coefficients in January through December.
 *
 * Does not select the FILE method or validate/open its climate source. Caller must supply
 * physically valid finite values; this setter copies entries without additional validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in dimensionless multipliers applied by
 *     FILE evaporation; copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_pan_coeff(SWMM_Engine engine, const double* values, int count);

/**
 * @brief Copy the configured soil-moisture recovery pattern ID.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned buffer; empty when no ID is configured, NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     buffer/capacity.
 */
SWMM_ENGINE_API int swmm_climate_get_evap_recovery(SWMM_Engine engine, char* buf, int buflen);
/**
 * @brief Copy a configured soil-moisture recovery pattern ID.
 *
 * Empty clears the pattern without changing the evaporation method. This does not add a missing
 * series/pattern or open a data file.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] pattern_id Non-null NUL-terminated identifier to copy; empty is accepted. The
 *     referenced object need not resolve at this authoring call.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_evap_recovery(SWMM_Engine engine, const char* pattern_id);

/* =========================================================================
 * Wind speed  ([TEMPERATURE] WINDSPEED)
 * ========================================================================= */

/**
 * @brief Read the configured wind-speed source.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] type Optional destination: SWMM_WindType: MONTHLY=0 or FILE=1.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_wind_type(SWMM_Engine engine, int* type);
/**
 * @brief Set the configured wind-speed source.
 *
 * Does not open or validate the climate file.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] type SWMM_WindType: MONTHLY=0 or FILE=1. Values outside 0..1 are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_wind_type(SWMM_Engine engine, int type);

/**
 * @brief Copy all monthly average wind speeds in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in mph in US or km/h in SI.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_wind_monthly(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly average wind speeds in January through December.
 *
 * Does not change the wind source selector. Caller must supply physically valid finite values; this
 * setter copies entries without additional validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in mph in US or km/h in SI; copied
 *     synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_wind_monthly(SWMM_Engine engine, const double* values,
                                                  int count);

/* =========================================================================
 * Humidity  ([TEMPERATURE] HUMIDITY) — heat-model met input
 * ========================================================================= */

/**
 * @brief Read the configured humidity source.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] type Optional destination: SWMM_HumidityType: CONSTANT=0, MONTHLY=1, TIMESERIES=2.
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_humidity_type(SWMM_Engine engine, int* type);
/**
 * @brief Set the configured humidity source.
 *
 * Does not change the humidity quantity selector or validate a referenced series.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] type SWMM_HumidityType: CONSTANT=0, MONTHLY=1, TIMESERIES=2. Values outside 0..2 are
 *     refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_humidity_type(SWMM_Engine engine, int type);

/**
 * @brief Read the configured humidity quantity.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] var Optional destination: SWMM_HumidityVar: RELATIVE=0 (percent) or DEWPOINT=1
 *     (degrees F US/degrees C SI).
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_humidity_variable(SWMM_Engine engine, int* var);
/**
 * @brief Set the configured humidity quantity.
 *
 * Dew-point values are converted to relative humidity against runtime air temperature.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] var SWMM_HumidityVar: RELATIVE=0 (percent) or DEWPOINT=1 (degrees F US/degrees C SI).
 *     Values outside 0..1 are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_humidity_variable(SWMM_Engine engine, int var);

/**
 * @brief Copy all monthly humidity values in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in RH percent or dew-point
 *     degrees F/C according to the humidity quantity/unit system.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_humidity_monthly(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly humidity values in January through December.
 *
 * All 12 values are retained as supplied; CONSTANT mode uses the configured constant entry. Does
 * not change source or quantity. Caller must supply physically valid finite values; this setter
 * copies entries without additional validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in RH percent or dew-point degrees F/C
 *     according to the humidity quantity/unit system; copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_humidity_monthly(SWMM_Engine engine, const double* values,
                                                      int count);

/**
 * @brief Copy the configured humidity time-series ID.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned buffer; empty when no ID is configured, NUL-terminated and
 *     silently truncated to buflen-1 bytes.
 * @param[in] buflen Positive byte capacity including NUL.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for invalid
 *     buffer/capacity.
 */
SWMM_ENGINE_API int swmm_climate_get_humidity_timeseries(SWMM_Engine engine, char* buf, int buflen);
/**
 * @brief Copy a configured humidity time-series ID.
 *
 * Also sets humidity source to TIMESERIES, including for an empty ID. This does not add a missing
 * series/pattern or open a data file.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] ts_id Non-null NUL-terminated identifier to copy; empty is accepted. The referenced
 *     object need not resolve at this authoring call.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_humidity_timeseries(SWMM_Engine engine, const char* ts_id);

/* =========================================================================
 * Snowmelt globals  ([TEMPERATURE] SNOWMELT)
 * ========================================================================= */

/**
 * @brief Read the configured snow/rain dividing temperature.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] divide_temp Optional destination: project temperature (degrees F in US, degrees C in
 *     SI).
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_snow_temp(SWMM_Engine engine, double* divide_temp);
/**
 * @brief Set the configured snow/rain dividing temperature.
 *
 * Assigned without finite/range validation; affects precipitation partitioning when snowmelt is
 * initialized.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] divide_temp project temperature (degrees F in US, degrees C in SI). Stored directly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_LIFECYCLE outside
 *     BUILDING/OPENED.
 */
SWMM_ENGINE_API int swmm_climate_set_snow_temp(SWMM_Engine engine, double divide_temp);

/**
 * @brief Read the configured antecedent-temperature-index weight.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] tipm Optional destination: dimensionless fraction in [0,1].
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_ati_weight(SWMM_Engine engine, double* tipm);
/**
 * @brief Set the configured antecedent-temperature-index weight.
 *
 * Caller must supply a finite value; only lower/upper comparisons guard this setter.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] tipm dimensionless fraction in [0,1]. Values outside [0,1] are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_ati_weight(SWMM_Engine engine, double tipm);

/**
 * @brief Read the configured negative-melt-coefficient ratio.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] rnm Optional destination: dimensionless fraction in [0,1].
 * @return SWMM_OK (including a NULL optional scalar output), or SWMM_ERR_BADHANDLE for null engine.
 */
SWMM_ENGINE_API int swmm_climate_get_neg_melt_ratio(SWMM_Engine engine, double* rnm);
/**
 * @brief Set the configured negative-melt-coefficient ratio.
 *
 * Caller must supply a finite value; only lower/upper comparisons guard this setter.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] rnm dimensionless fraction in [0,1]. Values outside [0,1] are refused.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_neg_melt_ratio(SWMM_Engine engine, double rnm);

/* =========================================================================
 * Areal-depletion curves  ([TEMPERATURE] ADC)
 * ========================================================================= */

/**
 * @brief Copy all impervious areal-depletion fractions in the configured ten-point depletion-curve
 * order.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 10 doubles, in dimensionless fractions in
 *     [0,1].
 * @param[in] count Must equal SWMM_CLIMATE_ADC_POINTS (10) exactly; no partial-copy or size-query
 *     mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_adc_impervious(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all impervious areal-depletion fractions in the configured ten-point
 * depletion-curve order.
 *
 * Each fraction is range checked, without a separate finite-value or monotonicity check.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 10 doubles in dimensionless fractions in [0,1];
 *     copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_ADC_POINTS (10) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_adc_impervious(SWMM_Engine engine, const double* values,
                                                    int count);

/**
 * @brief Copy all pervious areal-depletion fractions in the configured ten-point depletion-curve
 * order.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 10 doubles, in dimensionless fractions in
 *     [0,1].
 * @param[in] count Must equal SWMM_CLIMATE_ADC_POINTS (10) exactly; no partial-copy or size-query
 *     mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_adc_pervious(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all pervious areal-depletion fractions in the configured ten-point depletion-curve
 * order.
 *
 * Each fraction is range checked, without a separate finite-value or monotonicity check.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 10 doubles in dimensionless fractions in [0,1];
 *     copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_ADC_POINTS (10) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_adc_pervious(SWMM_Engine engine, const double* values,
                                                  int count);

/* =========================================================================
 * Monthly adjustments  ([ADJUSTMENTS])
 * ========================================================================= */

/**
 * @brief Copy all monthly temperature offsets in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in degrees F in US or degrees
 *     C in SI.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_adjust_temperature(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly temperature offsets in January through December.
 *
 * Offsets are added to the temperature source in display units. Caller must supply physically valid
 * finite values; this setter copies entries without additional validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in degrees F in US or degrees C in SI;
 *     copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_adjust_temperature(SWMM_Engine engine, const double* values,
                                                        int count);

/**
 * @brief Copy all monthly evaporation offsets in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in in/day in US or mm/day in
 *     SI.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_adjust_evaporation(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly evaporation offsets in January through December.
 *
 * Offsets are added to the evaporation source rate; these are not multiplicative coefficients.
 * Caller must supply physically valid finite values; this setter copies entries without additional
 * validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in in/day in US or mm/day in SI; copied
 *     synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_adjust_evaporation(SWMM_Engine engine, const double* values,
                                                        int count);

/**
 * @brief Copy all monthly rainfall multipliers in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in dimensionless (1 means
 *     unchanged).
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_adjust_rainfall(SWMM_Engine engine, double* buf, int count);
/**
 * @brief Replace all monthly rainfall multipliers in January through December.
 *
 * Multipliers are copied directly; this setter imposes no nonnegative/finite-value validation.
 * Caller must supply physically valid finite values; this setter copies entries without additional
 * validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in dimensionless (1 means unchanged);
 *     copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_adjust_rainfall(SWMM_Engine engine, const double* values,
                                                     int count);

/**
 * @brief Copy all monthly hydraulic-conductivity multipliers in January through December.
 *
 * @param[in] engine Non-null live engine with exclusive caller access; getters read stored
 *     configuration without changing lifecycle state.
 * @param[out] buf Non-null caller-owned array of exactly 12 doubles, in dimensionless (1 means
 *     unchanged).
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly; no partial-copy or size-query mode.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine or SWMM_ERR_BADPARAM for null buf/wrong
 *     count.
 */
SWMM_ENGINE_API int swmm_climate_get_adjust_conductivity(SWMM_Engine engine, double* buf,
                                                         int count);
/**
 * @brief Replace all monthly hydraulic-conductivity multipliers in January through December.
 *
 * Each value <=0 is stored as 1 (legacy convention); positive values are retained. No separate
 * finite-value validation.
 *
 * @param[in] engine Non-null engine in BUILDING or OPENED. Configure before initialize; runtime
 *     climate forcing has a separate API.
 * @param[in] values Non-null borrowed array of 12 doubles in dimensionless (1 means unchanged);
 *     copied synchronously.
 * @param[in] count Must equal SWMM_CLIMATE_MONTHS (12) exactly.
 * @return SWMM_OK; SWMM_ERR_BADHANDLE for null engine, SWMM_ERR_LIFECYCLE outside BUILDING/OPENED,
 *     or SWMM_ERR_BADPARAM for a refused value.
 */
SWMM_ENGINE_API int swmm_climate_set_adjust_conductivity(SWMM_Engine engine, const double* values,
                                                         int count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_CLIMATE_H */
