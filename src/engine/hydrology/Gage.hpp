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
 * @file      Gage.hpp
 * @brief     Rain gage processing — rainfall interpolation, type conversion.
 *
 * @details Processes rainfall from timeseries or files for each gage,
 *          converts between INTENSITY/VOLUME/CUMULATIVE rain types,
 *          separates rainfall from snowfall based on temperature,
 *          and tracks past n-hour rainfall totals.
 *
 * @note Legacy reference: src/legacy/engine/gage.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_GAGE_HPP
#define OPENSWMM_GAGE_HPP

#include "../data/SubcatchData.hpp"
#include <cstddef>
#include <vector>

namespace openswmm {

struct SimulationContext;
struct Table;

/** @brief Rain records, accepted runoff-clock state and read-only report lookups.
 * Context routines require aligned initialized gage/subcatchment arrays, valid
 * project units and ordered finite table x/y pairs unless stated otherwise.
 * State mutations are immediate; serialize updates with reads and use private
 * copies for trials. Only precipitation splits convert rates to internal ft/s. */
namespace gage {

// ============================================================================
// Constants
// ============================================================================

constexpr double ONE_SECOND = 1.1574074e-5; ///< One second in days
constexpr int MAXPASTRAIN = 48;             ///< Max past hours tracked

// ============================================================================
// Rain type codes
// ============================================================================

enum class RainType : int { INTENSITY = 0, VOLUME = 1, CUMULATIVE = 2 };

// ============================================================================
// Per-gage state
// ============================================================================

struct GageState {
    double rainfall = 0.0;      ///< Current intensity (project in/hr or mm/hr)
    double snowfall = 0.0;      ///< Current snow intensity (project in/hr or mm/hr)
    double total_precip = 0.0;  ///< rainfall + snowfall
    double rain_accum = 0.0;    ///< Cumulative rain accumulator (for CUMULATIVE type)
    double api_rainfall = -1.0; ///< API-overridden rainfall (-1 = no override)
    double snow_factor = 1.0;   ///< Snow catch factor
    double scale_factor = 1.0;  ///< Rainfall scaling factor (1.0 = no scaling)
    double units_factor = 1.0;  ///< Unit conversion factor
    double adjust_factor = 1.0; ///< Monthly/seasonal adjustment factor
    double rain_interval = 0.0; ///< Rain recording interval (seconds)
    RainType rain_type = RainType::INTENSITY;

    double past_rain[MAXPASTRAIN] = {}; ///< Past hourly rainfall totals
    double past_rain_accum = 0.0;       ///< Accumulator for current hour
    double past_rain_interval = 0.0;    ///< Seconds in the current partial hour (0..3600)
};

// ============================================================================
// Functions
// ============================================================================

/** @brief Transform one raw rain record with standalone GageState factors.
 * @param raw_value Intensity in project in/hr or mm/hr, or interval/cumulative depth
 * in project inches/mm according to state.rain_type; caller qualifies finite data.
 * @param[in,out] state Recording interval seconds and units/scale/monthly factors;
 * only rain_accum changes for CUMULATIVE with a positive interval. A counter decrease
 * starts a new accumulation event. Other rain state fields are retained.
 * @return Project intensity per hour times units_factor*scale_factor*adjust_factor.
 * VOLUME/CUMULATIVE with nonpositive interval and unknown enum return zero before
 * factor multiplication. Does not floor negative records or reject nonfinite inputs. */
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

/** @brief Borrow the actual resolved source table for a gage.
 * @param ctx Compatible initialized gage source/index arrays and owned table pools.
 * @param gage_idx Zero-based gage index; invalid returns null.
 * @return Borrowed nonempty resolved FILE_RAIN table, or the indexed shared table
 * for other sources (which can itself be empty); null for absent/unresolved source.
 * Never free; owner/table replacement invalidates it. Does not advance a cursor or
 * fall back from an unresolved file gage into the shared table pool. */
const Table* gageRainSeries(const SimulationContext& ctx, int gage_idx);

/** @brief Test whether the model or enabled rainfall mesh consumes a gage.
 * @param ctx Compatible initialized subcatchment, hydrograph-name and optional mesh state.
 * @param gage_idx Valid zero-based gage index; no range check is performed here.
 * @return True for a subcatchment binding, a resolved unit-hydrograph gage name,
 * or any nonempty configured mesh with rainfall_mode!=NONE. Mesh selection marks
 * every gage used. Invalid/unresolved -1 is not a supported query. No cache mutation. */
bool gageIsUsed(const SimulationContext& ctx, int gage_idx);

/** @brief Read the next runoff-clock rainfall boundary from cached gage state.
 * @param ctx Compatible state last produced by updateAllGages, with aligned state
 * arrays and table indices; does not advance it.
 * @param gage_idx Zero-based gage index; out-of-state-range or unused returns t.
 * @param t Finite absolute SWMM query day; comparisons internally use t plus one second.
 * @return Current interval start/end or next nonzero record start; legacy NO_DATE
 * (-693594 days) if no source/boundary remains. Result is not necessarily later
 * than t; runoff callers apply only a positive difference. Invalidates no borrows. */
double gageNextRainDate(const SimulationContext& ctx, int gage_idx, double t);

// Read-only forecast for held surface-process forcing. Adjacent records
// with the same converted rate are one interval; gaps still end rainfall.
// Unlike the legacy runoff clock this uses the exact interval start.
/** @brief Forecast the end of the current held rain rate without changing source state.
 * @param ctx Compatible initialized, current gage state and ordered record tables.
 * @param gage_idx Valid zero-based gage with current/next record indices consistent
 * with date; this is a held-state forecast, not an arbitrary historical query.
 * @param date Finite exact interval-start SWMM day (no legacy +1-second shift).
 * @param limit Finite upper-bound SWMM day at/after date.
 * @return Rate-change/gap boundary capped by limit, or limit for missing/ignored
 * rainfall. Joins adjacent exactly equal converted rates within 1e-5-second boundary
 * tolerance. Uses record/unit/scale factors, not monthly adjustment, API or forcing
 * overrides; caller qualifies those separately. Does not mutate table/gage cursors. */
double gageNextRateChange(const SimulationContext& ctx, int gage_idx, double date, double limit);

/** @brief Return the source-storage-to-project rainfall depth multiplier.
 * @param ctx Compatible gage source/format arrays and project unit enum.
 * @param gage_idx Zero-based gage index; invalid returns one.
 * @return 25.40 only for STANDARD FILE_RAIN in SI (enum>=3), otherwise one. STANDARD
 * loader stores float-quantized inches; USER_CSV/shared tables already use project
 * units. Does not convert the record's time interval or apply scale/month factors. */
double gageUnitsFactor(const SimulationContext& ctx, int gage_idx);

/** @brief Select the transform appropriate to resolved source storage.
 * @param ctx Compatible initialized source/format/rain_type arrays.
 * @param gage_idx Valid zero-based gage index; unchecked.
 * @return VOLUME (1) for STANDARD FILE_RAIN because its loader already converted to
 * interval depths; otherwise the authored rain_type integer unchanged. Does not
 * modify authored type or validate unknown enum values. */
int gageSeriesRainType(const SimulationContext& ctx, int gage_idx);

/** @brief Transform one source record and apply units then scale factors.
 * @param raw Raw intensity per project hour or interval/cumulative depth, depending on type.
 * @param rain_type Zero intensity, one volume, two cumulative; other integers fall
 * through as raw intensity. No enum/finite validation occurs.
 * @param interval_sec Positive recording seconds for depth transforms; nonpositive
 * interval skips the transform and returns scaled raw, unlike convertRainfall().
 * @param[in,out] cumul_accum Previous cumulative raw depth; on a positive-interval
 * cumulative record a decrease resets the counter, otherwise differences it. Retained
 * for other types/invalid intervals. Carry across records in source order.
 * @param units_factor Source-depth to project-depth multiplier.
 * @param scale_factor Dimensionless gage scale; no monthly adjustment applied here.
 * @return Intensity per project hour, preserving divide-then-multiply and factor
 * order. Negative/nonfinite inputs are not rejected or clamped. */
double convertGageValue(double raw, int rain_type, double interval_sec, double& cumul_accum,
                        double units_factor, double scale_factor);

/**
 * @brief Result of splitting a gage's precipitation for one subcatchment.
 */
struct PrecipSplit {
    double rainfall = 0.0; ///< Rainfall (ft/sec, internal units)
    double snowfall = 0.0; ///< Snowfall (ft/sec, internal units)
};

/** @brief Split a subcatchment's current gage intensity into internal rain and snow.
 * @param ctx Compatible initialized arrays, valid units and validated snow parameters;
 * reads climate temperature in F and current effective gage rainfall in project/hour.
 * @param sub Valid zero-based subcatchment index; unchecked before array access.
 * @return ft/s rain/snow, both zero for invalid/no gage. Ignore-snowmelt forces all
 * precipitation to rain. Ordinary snow uses the division temperature; UEB pack
 * assignment can use a mixed-temperature fraction and pack drift. Applies gage snow
 * catch and subcatchment rain/snow scales after unit conversion. Does not mutate
 * state or apply downstream subcatchment forcing overrides; callers add those.
 * No nonnegative/finite output validation is performed. */
PrecipSplit splitPrecip(const SimulationContext& ctx, std::size_t sub);

/** @brief Accumulate a standalone gage's held intensity into completed-hour history.
 * @param[in,out] state rainfall is project/hour; past_rain holds completed project
 * depths and partial accumulator holds current-hour depth. Partial interval must
 * be in [0,3600] seconds, normally integer-valued.
 * @param time_step Actual accepted runoff duration in integer seconds; nonpositive
 * is a no-op. May span multiple hours. An exactly full hour stays in the partial
 * bucket until a later positive update; completed history excludes it until then.
 * Does not split snow or reset rainfall; caller owns accepted-step invocation. */
void updatePastRain(GageState& state, int time_step);

/** @brief Accumulate every gage's current held rate over one accepted runoff step.
 * @param[in,out] ctx Aligned rainfall/partial/history arrays, including n_gages*48
 * completed-depth slots. Reads final resolved project/hour rates and changes only
 * history accumulators/intervals; processes unused gages too.
 * @param time_step Integer elapsed seconds, nonpositive no-op. Exact-hour rollover
 * matches the standalone overload; no model clock or rainfall update occurs. */
void updatePastRain(SimulationContext& ctx, int time_step);

/** @brief Sum completed hourly depth buckets, excluding the current partial hour.
 * @param state Borrowed standalone history in project inches/mm.
 * @param hours Requested completed hours in [1,48]; other values return zero.
 * @return Sum of newest requested buckets in project depth units. Does not include
 * an exactly full partial hour awaiting the next positive update or mutate state. */
double getPastRain(const GageState& state, int hours);

/** @brief Advance used gage record state and publish current effective rainfall.
 * @param[in,out] ctx Compatible initialized model with aligned gage state/source/
 * forcing arrays, sorted finite resolved tables and coherent co-gage indices. Can
 * initialize used flags, change record/lookahead indices/rates and rainfall; does
 * not update precipitation splits, past-hour history or the simulation date itself.
 * @param current_time Finite absolute SWMM/OLE day, not elapsed seconds. Monotonic
 * forward requests are required; the state machine does not rewind. Record marching
 * uses current_time plus one second and retains each lookahead's adjustment factor.
 * Unused gages retain seeded rate. Ignore-rainfall wins before overrides; earlier
 * co-gage rates precede API override, then ordinary source resolution. Final used-gage
 * OVERRIDE/ADD forcing applies after all source reads. Rates remain project/hour;
 * no finite/range validation or transaction rollback occurs here. */
void updateAllGages(SimulationContext& ctx, double current_time);

/** @brief Query rainfall using the most recently advanced runoff-gage state.
 * @param ctx Compatible initialized state/tables/co-gage arrays, borrowed read-only.
 * @param gage_idx Valid zero-based gage index, unchecked before several accesses.
 * @param report_date Finite absolute SWMM/OLE day; lookup uses a one-second shift.
 * @return Project/hour rate from cached current/lookahead records, co-gage scaling
 * or API override, zero when ignored/missing. Does not march cursors or independently
 * search historical records; use the source-series overload when runoff ran ahead.
 * Final gage forcing OVERRIDE/ADD values are not separately applied by this reader. */
double getReportRainfall(const SimulationContext& ctx, int gage_idx, double report_date);

/** @brief Query report-time source records independently of a later runoff cursor.
 * @param ctx Compatible initialized gage/table state; st_cur must still be initialized
 * and not NO_STATE even though rate selection searches source timestamps directly.
 * @param gage_idx Valid zero-based gage index, unchecked before several accesses.
 * @param report_date Supported finite SWMM/OLE day; source search uses date plus one second.
 * @return Project/hour rate during the selected record interval, using report-month
 * adjustment, source conversion/scale, co-gage or API override. Zero outside covered
 * intervals or under ignored/missing state. Does not mutate cursors, include final
 * gage forcing OVERRIDE/ADD, or reproduce lookahead adjustment history for source searches. */
double getReportRainfallFromSeries(const SimulationContext& ctx, int gage_idx, double report_date);

} // namespace gage
} // namespace openswmm

#endif // OPENSWMM_GAGE_HPP
