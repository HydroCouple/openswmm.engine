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
 * @file Gage.hpp
 * @brief Rain gage processing — rainfall interpolation, type conversion.
 *
 * @details Processes rainfall from timeseries or files for each gage,
 *          converts between INTENSITY/VOLUME/CUMULATIVE rain types,
 *          separates rainfall from snowfall based on temperature,
 *          and tracks past n-hour rainfall totals.
 *
 * @note Legacy reference: src/legacy/engine/gage.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_GAGE_HPP
#define OPENSWMM_GAGE_HPP

#include "../data/SubcatchData.hpp"
#include <cstddef>
#include <vector>

namespace openswmm {

struct SimulationContext;
struct Table;

namespace gage {

// ============================================================================
// Constants
// ============================================================================

constexpr double ONE_SECOND    = 1.1574074e-5;  ///< One second in days
constexpr int    MAXPASTRAIN   = 48;             ///< Max past hours tracked

// ============================================================================
// Rain type codes
// ============================================================================

enum class RainType : int {
    INTENSITY  = 0,
    VOLUME     = 1,
    CUMULATIVE = 2
};

// ============================================================================
// Per-gage state
// ============================================================================

struct GageState {
    double rainfall      = 0.0;   ///< Current rainfall intensity (project units/sec)
    double snowfall      = 0.0;   ///< Current snowfall intensity (project units/sec)
    double total_precip  = 0.0;   ///< rainfall + snowfall
    double rain_accum    = 0.0;   ///< Cumulative rain accumulator (for CUMULATIVE type)
    double api_rainfall  = -1.0;  ///< API-overridden rainfall (-1 = no override)
    double snow_factor   = 1.0;   ///< Snow catch factor
    double scale_factor  = 1.0;   ///< Rainfall scaling factor (1.0 = no scaling)
    double units_factor  = 1.0;   ///< Unit conversion factor
    double adjust_factor = 1.0;   ///< Monthly/seasonal adjustment factor
    double rain_interval = 0.0;   ///< Rain recording interval (seconds)
    RainType rain_type   = RainType::INTENSITY;

    double past_rain[MAXPASTRAIN] = {}; ///< Past hourly rainfall totals
    double past_rain_accum = 0.0;       ///< Accumulator for current hour
    double past_rain_time  = 0.0;       ///< Time of last past-rain update
};

// ============================================================================
// Functions
// ============================================================================

/**
 * @brief Convert raw rainfall value based on rain type.
 *
 * @param raw_value      Raw value from timeseries.
 * @param state          Gage state (for accumulators, factors).
 * @returns Rainfall intensity (project units/sec).
 */
double convertRainfall(double raw_value, GageState& state);

// ---------------------------------------------------------------------------
// Shared rainfall resolution
// ---------------------------------------------------------------------------
//
// The sequence "pick the gage's series -> apply the rain-type transform ->
// apply units factor -> apply scale factor" is needed by the routing update,
// by report rainfall, and by the swmm_gage_get_rainfall_series C API. Three
// private copies would drift, so the pieces that are genuinely common live
// here. (getReportRainfall deliberately does NOT apply the CUMULATIVE delta —
// see its comment — so it composes these itself rather than calling
// convertGageValue.)

/**
 * @brief The rainfall table a gage actually reads.
 *
 * @details A FILE_RAIN gage reads its own resolved `rain_series`, built at load
 *          by load_external_rain_files; every other gage reads the shared table
 *          pool. Deliberately kept out of `ctx.tables` so an INP round-trip
 *          still emits FILE.
 * @returns nullptr when the gage has no usable series (unresolvable file, or a
 *          rain-file format the loader does not support).
 */
const Table* gageRainSeries(const SimulationContext& ctx, int gage_idx);

/**
 * @brief The units factor the engine applies to a gage's raw values.
 *
 * @details 25.40 (MMperINCH) for a STANDARD rain-file gage in an SI project,
 *          because that loader stores float-quantized INCHES for legacy
 *          bit-parity; 1.0 for everything else.
 *
 *          Scoped to STAN_PRCP on purpose. USER_CSV data is stored in the
 *          project's own rain units, exactly like a [TIMESERIES] gage, so it
 *          must not be rescaled — and pinning the factor to the format keeps
 *          the legacy standard-file path byte-identical.
 */
double gageUnitsFactor(const SimulationContext& ctx, int gage_idx);

/**
 * @brief Apply the rain-type transform, units factor and scale factor.
 *
 * @details The exact arithmetic updateAllGages performs, factored out so the
 *          read-back API cannot drift from what the run actually applies.
 *          Operand order is load-bearing for legacy parity: `raw / interval *
 *          3600.0` as one divide then one multiply, and units factor BEFORE
 *          scale factor.
 *
 * @param cumul_accum [in/out] Running total for CUMULATIVE gages; a decrease is
 *                    read as a counter reset. Ignored for other rain types, but
 *                    callers walking a whole series must carry it entry to
 *                    entry or the deltas are wrong.
 */
double convertGageValue(double raw, int rain_type, double interval_sec,
                        double& cumul_accum, double units_factor,
                        double scale_factor);

/**
 * @brief Result of splitting a gage's precipitation for one subcatchment.
 */
struct PrecipSplit {
    double rainfall = 0.0;  ///< Rainfall (ft/sec, internal units)
    double snowfall = 0.0;  ///< Snowfall (ft/sec, internal units)
};

/**
 * @brief Split a subcatchment's gage precipitation into rain and snow.
 *
 * @details The single source of truth for the rain/snow split, mirroring legacy
 *          gage_getPrecip() (gage.c:513-523) and adding the subcatchment-level
 *          scale factors. Applies, in order:
 *            - the IgnoreSnowmelt guard and the temperature test
 *            - the gage snow catch factor (SCF) on the snow branch
 *            - the subcatchment rain/snow scale factors
 *            - conversion to internal units (ft/sec)
 *
 *          Both the snowpack path (SWMMEngine) and the plain runoff path
 *          (RunoffSolver) MUST route through this. They previously duplicated
 *          the logic and drifted: the snowpack path dropped the SCF entirely and
 *          the runoff path performed no split at all.
 *
 *          API/forcing overrides are applied by the CALLER, after this returns —
 *          they are absolute injections and are deliberately not scaled.
 *
 * @param ctx  Simulation context (gages, subcatchments, climate, options).
 * @param sub  Subcatchment index.
 * @returns Rainfall and snowfall in ft/sec. Zero if the subcatchment has no gage.
 */
PrecipSplit splitPrecip(const SimulationContext& ctx, std::size_t sub);

/**
 * @brief Update past n-hour rainfall accumulation.
 *
 * @param state      [in/out] Gage state.
 * @param current_time  Current simulation time (seconds from start).
 */
void updatePastRain(GageState& state, double current_time);

/**
 * @brief Get past n-hour rainfall total.
 *
 * @param state  Gage state.
 * @param hours  Number of past hours (1 to MAXPASTRAIN).
 * @returns Cumulative rainfall over past n hours (project depth units).
 */
double getPastRain(const GageState& state, int hours);

/**
 * @brief Process all gages for one timestep.
 *
 * @param ctx  Simulation context (gages + timeseries accessed).
 * @param current_time  Current simulation time (seconds).
 */
void updateAllGages(SimulationContext& ctx, double current_time);

/**
 * @brief Query a gage's rainfall at a specific report date.
 *
 * @details Matches legacy gage_setReportRainfall(): queries the gage's
 *          timeseries at the report time without advancing the cursor.
 *          Returns rainfall in user units (in/hr or mm/hr).
 *
 * @param ctx          Simulation context.
 * @param gage_idx     Gage index.
 * @param report_date  Absolute OADate (days since 12/30/1899) to query.
 * @returns Rainfall rate in user units (in/hr or mm/hr).
 */
double getReportRainfall(const SimulationContext& ctx, int gage_idx,
                         double report_date);

} // namespace gage
} // namespace openswmm

#endif // OPENSWMM_GAGE_HPP
