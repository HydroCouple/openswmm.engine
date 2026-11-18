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
 * @file      Climate.hpp
 * @brief     Climate processing — evaporation, temperature, wind.
 *
 * @details Daily climate values are computed once and broadcast to all
 *          subcatchments — no per-subcatchment branching needed.
 *          The Hargreaves ET formula is the most compute-intensive path.
 *
 *          Vectorization: climate state is scalar-per-day (broadcast pattern).
 *          The per-subcatchment evap distribution is a batch multiply.
 *
 * @note Legacy reference: src/legacy/engine/climate.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_CLIMATE_HPP
#define OPENSWMM_CLIMATE_HPP
#include "Radiation.hpp"

namespace openswmm {

struct SimulationContext;

namespace climate {

// ============================================================================
// Constants
// ============================================================================

constexpr double MM_PER_INCH = 25.40;

// ============================================================================
// Evaporation method
// ============================================================================

enum class EvapMethod : int {
    CONSTANT = 0,
    MONTHLY = 1,
    TIMESERIES = 2,
    TEMPERATURE = 3, ///< Hargreaves method
    PAN = 4
};

// ============================================================================
// 7-day temperature moving average (for Hargreaves ET)
// ============================================================================

struct MovingAvg7 {
    double ta[7] = {}; ///< Daily average temps (deg F)
    double tr[7] = {}; ///< Daily temp ranges (deg F)
    int front = 0;     ///< Circular buffer write index
    int count = 0;     ///< Number of values stored (max 7)

    /** @brief Append a daily temperature/range pair to the seven-slot circular history.
     * @param t_avg Finite daily mean Fahrenheit, stored without validation/conversion.
     * @param t_range Finite daily Fahrenheit range, normally nonnegative; stored unchanged.
     * Requires front in [0,6], count in [0,7]. Replaces oldest values after seven pushes,
     * advances front modulo seven and caps count. No calendar/deduplication check;
     * caller invokes once per desired sample and serializes access. */
    void push(double t_avg, double t_range);

    /** @brief Read the arithmetic mean of retained daily temperature samples.
     * @return Fahrenheit mean over count entries, or zero when count==0. Requires valid
     * count [0,7]; no filtering of negative/nonfinite values or history mutation occurs. */
    double avg_temp() const;

    /** @brief Read the arithmetic mean of retained daily temperature ranges.
     * @return Fahrenheit difference over count entries, or zero when count==0. Requires
     * count [0,7]; no finite/nonnegative filtering or history mutation occurs. */
    double avg_range() const;
};

// ============================================================================
// Daily climate state (scalar — broadcast to all subcatchments)
// ============================================================================

struct ClimateState {
    RadiationState radiation;
    double temperature = 70.0; ///< Air temperature (deg F) — effective (post-forcing)
    double temp_range = 0.0;   ///< Daily temperature range (deg F)
    double evap_rate = 0.0;    ///< Evaporation rate (ft/sec)
    double wind_speed = 0.0;   ///< Wind speed (mph) — effective (post-forcing)
    double humidity = 50.0;    ///< Relative humidity (%)

    // Source/default bases (pre-forcing). The per-step source lookup writes
    // these; forcing then resolves effective values on top. Keeping them
    // separate lets a one-shot/cleared forcing revert to the data source (or
    // default) even when no source overwrites the value each step.
    double temperature_src = 70.0; ///< Source/default air temperature (deg F)
    double file_tmin = 70.0;       ///< Last daily min read from a climate file (deg F)
    double file_tmax = 70.0;       ///< Last daily max read from a climate file (deg F)
    double wind_speed_src = 0.0;   ///< Source/default wind speed (mph)

    // Derived values
    double gamma = 0.0; ///< Psychrometric constant
    double ea = 0.0;    ///< Saturation vapor pressure

    // Hargreaves parameters
    double latitude = 0.0; ///< Latitude (degrees)

    // Site elevation for psychrometric constant (matching legacy Temp.elev)
    double elev = 0.0;       ///< Site elevation above sea level (ft)
    double snow_divt = 34.0; ///< Rain/snow dividing temperature (degF), legacy Snow.snotmp

    // Monthly evaporation table (for MONTHLY method)
    double monthly_evap[12] = {};

    // EVAPRATE unit-conversion factor (in/day US or mm/day SI → ft/sec) for the
    // project's unit system. Set at init; MONTHLY uses it so SI models convert
    // mm/day correctly (defaults to the US factor for back-compatibility).
    double evaprate_ucf = 1036800.0;

    // Monthly adjustment factors
    double adjust_evap[12] = {
        0, 0, 0, 0, 0, 0,
        0, 0, 0, 0, 0, 0}; ///< ADDED to the rate (ft/s), legacy Adjust.evap /= UCF(EVAPRATE)
    // Legacy climate.c NextEvapDate / NextEvapRate: the date the evaporation
    // source next changes (every type — a year ahead for CONSTANT, the first
    // of next month for MONTHLY, the next series entry for TIMESERIES, the
    // next day for a climate file) and the rate that entry carries. A
    // time-series evaporation is STEP-WISE: the rate holds until the date
    // is reached, and the runoff step never crosses it (runoff_getTimeStep).
    double next_evap_date = 0.0;
    double next_evap_rate = 0.0; ///< user units
    int evap_ts_pos =
        -1; ///< index of the last series entry read (legacy table_getNextEntry cursor)
    double adjust_temp[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    double adjust_rain[12] = {1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1};
    double adjust_hydcon[12] = {1, 1, 1, 1, 1, 1,
                                1, 1, 1, 1, 1, 1}; ///< Infiltration conductivity multipliers

    EvapMethod evap_method = EvapMethod::CONSTANT;

    // Monthly adjustment for infiltration capacity (scales f0/fmin/Ks)
    // @see Legacy: InfilFactor (set from hydcon adjustment pattern)
    double infil_factor = 1.0;

    // 7-day moving average for Hargreaves ET (matching legacy TMovAve Tma)
    MovingAvg7 temp_ma;

    // Recovery factor for infiltration models
    // @see Legacy: Evap.recoveryFactor (set from recovery adjustment pattern)
    double recovery_factor = 1.0;

    // Resolved timeseries/pattern indices (set at init, -1 = none)
    int temp_ts_index = -1;      ///< Temperature timeseries table index
    int evap_ts_index = -1;      ///< Evaporation timeseries table index
    int humidity_ts_index = -1;  ///< Humidity / dew-point timeseries table index
    int recovery_pat_index = -1; ///< Recovery pattern index in ctx.patterns

    // Sub-daily sinusoidal temperature interpolation (Gap #9)
    // Valid only when temperature comes from a daily min/max source.
    // @see Legacy: setTemp() / updateTempTimes() in climate.c
    double tmin_daily = 0.0; ///< Current day's minimum temperature (deg F)
    double tmax_daily = 0.0; ///< Current day's maximum temperature (deg F)
    double prev_tmax = 0.0;  ///< Previous day's maximum temperature (deg F)
    double hrsr = 6.0;       ///< Sunrise hour (time of min temp), 0-24
    double hrss = 15.0;      ///< Sunset-3 hour (time of max temp), 0-24
    double hrday = 10.5;     ///< Mid-hour between hrsr and hrss
    double dhrdy = -9.0;     ///< hrsr - hrss (negative)
    double dydif = 15.0;     ///< 24 + hrsr - hrss (hours from max to next min)
    double dtlong = 0.0;     ///< Longitude correction (hrs); 0 = solar time
    bool has_minmax = false; ///< True when tmin_daily/tmax_daily are valid
    int last_temp_doy = -1;  ///< Day-of-year when hrsr/hrss were last computed
};

// ============================================================================
// Functions
// ============================================================================

/** @brief Estimate potential evapotranspiration using Hargreaves radiation/temperature.
 * @param latitude Finite site latitude degrees north, normally [-90,90].
 * @param day_of_year Ordinal [1,366]; seasonal fit uses 365-day denominator.
 * @param t_avg Finite mean temperature Fahrenheit, converted internally to Celsius.
 * @param t_range Finite temperature difference Fahrenheit, converted to Celsius and
 * floored at zero before square root.
 * @return Nonnegative mm/day in every project unit system, not inches/day. Latent
 * heat is floored at 0.01, sunset-angle argument clamped to [-1,1], radiation and ET
 * floored at zero. No input/error validation or state mutation; caller converts the
 * result to internal units as needed. */
double hargreaves(double latitude, int day_of_year, double t_avg, double t_range);

/** @brief Update scalar evaporation and snow-related atmospheric coefficients.
 * @param[in,out] state Initialized climate state with effective temperature/range
 * already resolved in F, finite site latitude/elevation (feet), compatible moving
 * history, positive evaprate_ucf and monthly project evaporation inputs. CONSTANT/
 * MONTHLY recompute native ft/s, TEMPERATURE pushes one history sample and computes
 * Hargreaves; TIMESERIES/PAN rates are retained for external resolution. Adds monthly
 * adjust_evap only for the first three modes. Updates saturation pressure ea and
 * elevation-based psychrometric gamma. Temperature/wind and source clocks are not
 * resolved or adjusted by this routine.
 * @param day_of_year Valid ordinal [1,366] used by temperature-derived ET.
 * @param month Zero-based [0,11], unchecked monthly array index.
 * Call once per desired daily sample for TEMPERATURE to avoid duplicate history;
 * negative adjusted evaporation is not clamped and numeric failure has no status.
 * Caller serializes state mutation; no rollback is supplied. */
void updateDailyClimate(ClimateState& state, int day_of_year, int month);

/** @brief Derive daily sinusoidal temperature timing from latitude and season.
 * @param[in,out] state Finite latitude degrees north and dtlong hour correction;
 * writes hrsr, hrss, hrday, dhrdy and dydif. Does not update temperature, has_minmax,
 * last_temp_doy or any simulation date; caller manages those markers.
 * @param doy Ordinal [1,366], not validated.
 * Polar night uses noon/noon, midnight sun uses zero/24; ordinary maxima use sunset
 * minus three hours and the longitude correction. Call before getSubdailyTemp for
 * the selected day; no general finite/range validation or output normalization. */
void updateTempTimes(ClimateState& state, int doy);

/** @brief Evaluate the three-segment native diurnal temperature curve.
 * @param state Borrowed finite daily min/max/previous-max temperatures in F and
 * consistent timing from updateTempTimes(). has_minmax is not checked here.
 * @param hour Finite local clock hour, normally [0,24]; not normalized/clamped.
 * @return Fahrenheit from the pre-sunrise, daytime or post-maximum sinusoid.
 * Near-zero timing denominators use +1/-1-hour fallbacks. No finite-input checks,
 * clamping to min/max, calendar lookup or state mutation occurs. */
double getSubdailyTemp(const ClimateState& state, double hour);

/** @brief Bound one scalar evaporation rate by each ponded-water supply rate.
 * @param evap_rate Finite potential ft/s, normally nonnegative; no floor at zero.
 * @param ponded_depth Readable n-element depth array in feet, normally nonnegative;
 * may be null when n<=0.
 * @param[out] evap_out Writable n-element ft/s array, disjoint from ponded_depth or
 * exactly the same storage for safe in-place use; other overlap is unsupported.
 * @param n Number of entries; nonpositive performs no array accesses.
 * @param dt Finite seconds; positive uses 1/dt, nonpositive uses zero reciprocal.
 * Each output is min(evap_rate,depth/dt) for positive dt. Negative inputs can produce
 * negative rates; no finite-value validation, accepted-water booking or mutation of
 * the simulation clock is supplied. */
void batchDistributeEvap(double evap_rate, const double* ponded_depth, double* evap_out, int n,
                         double dt);

} // namespace climate
} // namespace openswmm

#endif // OPENSWMM_CLIMATE_HPP
