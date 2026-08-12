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
 * @file openswmm_climate.h
 * @brief OpenSWMM Engine — Climatology configuration C API.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
    SWMM_TEMP_NONE       = 0, /**< No temperature data (snowmelt/PET disabled). */
    SWMM_TEMP_TIMESERIES = 1, /**< Temperature from an in-model time series.    */
    SWMM_TEMP_FILE       = 2  /**< Temperature from an external climate file.   */
} SWMM_TempSource;

/**
 * @brief Evaporation method (matches [EVAPORATION]).
 */
typedef enum SWMM_EvapType {
    SWMM_EVAP_CONSTANT    = 0, /**< Single constant rate.                       */
    SWMM_EVAP_MONTHLY     = 1, /**< Twelve monthly rates.                       */
    SWMM_EVAP_TIMESERIES  = 2, /**< Rate from an in-model time series.          */
    SWMM_EVAP_TEMPERATURE = 3, /**< Hargreaves method from temperature.         */
    SWMM_EVAP_FILE        = 4  /**< Pan rate from the external climate file.    */
} SWMM_EvapType;

/**
 * @brief Wind-speed data source (matches [TEMPERATURE] WINDSPEED).
 */
typedef enum SWMM_WindType {
    SWMM_WIND_MONTHLY = 0, /**< Twelve monthly average wind speeds. */
    SWMM_WIND_FILE    = 1  /**< Wind speed from the climate file.   */
} SWMM_WindType;

/* =========================================================================
 * Temperature  ([TEMPERATURE])
 * ========================================================================= */

/** @brief Get the air-temperature data source (see @ref SWMM_TempSource). */
SWMM_ENGINE_API int swmm_climate_get_temp_source(SWMM_Engine engine, int* source);
/** @brief Set the air-temperature data source (0..2, see @ref SWMM_TempSource).
 *  @returns SWMM_ERR_BADPARAM if out of range; SWMM_ERR_LIFECYCLE if not editable. */
SWMM_ENGINE_API int swmm_climate_set_temp_source(SWMM_Engine engine, int source);

/** @brief Get the temperature time-series id (empty if none). */
SWMM_ENGINE_API int swmm_climate_get_temp_timeseries(SWMM_Engine engine, char* buf, int buflen);
/** @brief Assign the temperature time-series id and set the source to TIMESERIES. */
SWMM_ENGINE_API int swmm_climate_set_temp_timeseries(SWMM_Engine engine, const char* ts_id);

/** @brief Get the climate-file start date (DateTime decimal days; 0 = file start). */
SWMM_ENGINE_API int swmm_climate_get_temp_file_start(SWMM_Engine engine, double* date);
/** @brief Set the climate-file start date (DateTime decimal days; 0 = file start). */
SWMM_ENGINE_API int swmm_climate_set_temp_file_start(SWMM_Engine engine, double date);

/** @brief Get the climate-file temperature units: 0=tenths-degC (C10),
 *  1=degC (C), 2=degF (F); -1 = unspecified (reader's per-format default). */
SWMM_ENGINE_API int swmm_climate_get_temp_units(SWMM_Engine engine, int* units);
/** @brief Set the climate-file temperature units (-1, 0, 1, or 2).
 *  @returns SWMM_ERR_BADPARAM if not in {-1,0,1,2}. */
SWMM_ENGINE_API int swmm_climate_set_temp_units(SWMM_Engine engine, int units);

/** @brief Get the site elevation (project length units; for psychrometric constant). */
SWMM_ENGINE_API int swmm_climate_get_elevation(SWMM_Engine engine, double* elev);
/** @brief Set the site elevation (project length units). */
SWMM_ENGINE_API int swmm_climate_set_elevation(SWMM_Engine engine, double elev);

/** @brief Get the site latitude (degrees, for Hargreaves ET / snowmelt). */
SWMM_ENGINE_API int swmm_climate_get_latitude(SWMM_Engine engine, double* latitude);
/** @brief Set the site latitude (degrees, -90..90).
 *  @returns SWMM_ERR_BADPARAM if outside [-90, 90]. */
SWMM_ENGINE_API int swmm_climate_set_latitude(SWMM_Engine engine, double latitude);

/** @brief Get the longitude/solar-time correction (minutes; matches the legacy
 *         [TEMPERATURE] SNOWMELT field; 0 = use true solar time). */
SWMM_ENGINE_API int swmm_climate_get_longitude_correction(SWMM_Engine engine, double* minutes);
/** @brief Set the longitude/solar-time correction (minutes). */
SWMM_ENGINE_API int swmm_climate_set_longitude_correction(SWMM_Engine engine, double minutes);

/* =========================================================================
 * Evaporation  ([EVAPORATION])
 * ========================================================================= */

/** @brief Get the evaporation method (see @ref SWMM_EvapType). */
SWMM_ENGINE_API int swmm_climate_get_evap_type(SWMM_Engine engine, int* type);
/** @brief Set the evaporation method (0..4, see @ref SWMM_EvapType).
 *  @returns SWMM_ERR_BADPARAM if out of range. */
SWMM_ENGINE_API int swmm_climate_set_evap_type(SWMM_Engine engine, int type);

/** @brief Get the 12 monthly evaporation rates (project evap-rate units).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_evap_monthly(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly evaporation rates. A CONSTANT method stores the
 *         first value across all months; @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_evap_monthly(SWMM_Engine engine, const double* values, int count);

/** @brief Get the evaporation time-series id (empty if none). */
SWMM_ENGINE_API int swmm_climate_get_evap_timeseries(SWMM_Engine engine, char* buf, int buflen);
/** @brief Assign the evaporation time-series id and set the method to TIMESERIES. */
SWMM_ENGINE_API int swmm_climate_set_evap_timeseries(SWMM_Engine engine, const char* ts_id);

/** @brief Get the 12 monthly pan coefficients (used with the FILE method).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_pan_coeff(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly pan coefficients; @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_pan_coeff(SWMM_Engine engine, const double* values, int count);

/** @brief Get the soil-moisture recovery pattern id (empty if none). */
SWMM_ENGINE_API int swmm_climate_get_evap_recovery(SWMM_Engine engine, char* buf, int buflen);
/** @brief Set the soil-moisture recovery pattern id (empty string clears it). */
SWMM_ENGINE_API int swmm_climate_set_evap_recovery(SWMM_Engine engine, const char* pattern_id);

/* =========================================================================
 * Wind speed  ([TEMPERATURE] WINDSPEED)
 * ========================================================================= */

/** @brief Get the wind-speed data source (see @ref SWMM_WindType). */
SWMM_ENGINE_API int swmm_climate_get_wind_type(SWMM_Engine engine, int* type);
/** @brief Set the wind-speed data source (0..1, see @ref SWMM_WindType).
 *  @returns SWMM_ERR_BADPARAM if out of range. */
SWMM_ENGINE_API int swmm_climate_set_wind_type(SWMM_Engine engine, int type);

/** @brief Get the 12 monthly average wind speeds (project wind-speed units).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_wind_monthly(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly average wind speeds; @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_wind_monthly(SWMM_Engine engine, const double* values, int count);

/* =========================================================================
 * Snowmelt globals  ([TEMPERATURE] SNOWMELT)
 * ========================================================================= */

/** @brief Get the snow/rain dividing temperature (project temperature units). */
SWMM_ENGINE_API int swmm_climate_get_snow_temp(SWMM_Engine engine, double* divide_temp);
/** @brief Set the snow/rain dividing temperature (project temperature units). */
SWMM_ENGINE_API int swmm_climate_set_snow_temp(SWMM_Engine engine, double divide_temp);

/** @brief Get the antecedent-temperature-index weight (TIPM, 0..1). */
SWMM_ENGINE_API int swmm_climate_get_ati_weight(SWMM_Engine engine, double* tipm);
/** @brief Set the antecedent-temperature-index weight (TIPM).
 *  @returns SWMM_ERR_BADPARAM if outside [0, 1]. */
SWMM_ENGINE_API int swmm_climate_set_ati_weight(SWMM_Engine engine, double tipm);

/** @brief Get the negative-melt-coefficient ratio (RNM, 0..1). */
SWMM_ENGINE_API int swmm_climate_get_neg_melt_ratio(SWMM_Engine engine, double* rnm);
/** @brief Set the negative-melt-coefficient ratio (RNM).
 *  @returns SWMM_ERR_BADPARAM if outside [0, 1]. */
SWMM_ENGINE_API int swmm_climate_set_neg_melt_ratio(SWMM_Engine engine, double rnm);

/* =========================================================================
 * Areal-depletion curves  ([TEMPERATURE] ADC)
 * ========================================================================= */

/** @brief Get the impervious areal-depletion curve (10 fractions, 0..1).
 *  @param count Must equal SWMM_CLIMATE_ADC_POINTS. */
SWMM_ENGINE_API int swmm_climate_get_adc_impervious(SWMM_Engine engine, double* buf, int count);
/** @brief Set the impervious areal-depletion curve; each fraction in [0, 1];
 *         @p count must equal SWMM_CLIMATE_ADC_POINTS. */
SWMM_ENGINE_API int swmm_climate_set_adc_impervious(SWMM_Engine engine, const double* values, int count);

/** @brief Get the pervious areal-depletion curve (10 fractions, 0..1).
 *  @param count Must equal SWMM_CLIMATE_ADC_POINTS. */
SWMM_ENGINE_API int swmm_climate_get_adc_pervious(SWMM_Engine engine, double* buf, int count);
/** @brief Set the pervious areal-depletion curve; each fraction in [0, 1];
 *         @p count must equal SWMM_CLIMATE_ADC_POINTS. */
SWMM_ENGINE_API int swmm_climate_set_adc_pervious(SWMM_Engine engine, const double* values, int count);

/* =========================================================================
 * Monthly adjustments  ([ADJUSTMENTS])
 * ========================================================================= */

/** @brief Get the 12 monthly temperature adjustments (offsets, project temp units).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_adjust_temperature(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly temperature adjustments; @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_adjust_temperature(SWMM_Engine engine, const double* values, int count);

/** @brief Get the 12 monthly evaporation adjustment multipliers (1.0 = none).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_adjust_evaporation(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly evaporation adjustment multipliers; @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_adjust_evaporation(SWMM_Engine engine, const double* values, int count);

/** @brief Get the 12 monthly rainfall adjustment multipliers (1.0 = none).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_adjust_rainfall(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly rainfall adjustment multipliers; @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_adjust_rainfall(SWMM_Engine engine, const double* values, int count);

/** @brief Get the 12 monthly hydraulic-conductivity adjustment multipliers (1.0 = none).
 *  @param count Must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_get_adjust_conductivity(SWMM_Engine engine, double* buf, int count);
/** @brief Set the 12 monthly hydraulic-conductivity adjustment multipliers; values <= 0
 *         are stored as 1.0 (legacy behaviour); @p count must equal SWMM_CLIMATE_MONTHS. */
SWMM_ENGINE_API int swmm_climate_set_adjust_conductivity(SWMM_Engine engine, const double* values, int count);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* OPENSWMM_CLIMATE_H */
