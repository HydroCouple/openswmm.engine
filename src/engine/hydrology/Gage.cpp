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
 * @file Gage.cpp
 * @brief Rain gage processing — numerically identical to legacy gage.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Gage.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../core/DateTime.hpp"
#include <cmath>
#include <algorithm>
#include <cstdio>

namespace openswmm {
namespace gage {

double convertRainfall(double raw_value, GageState& state) {
    double r = 0.0;

    switch (state.rain_type) {
        case RainType::INTENSITY:
            r = raw_value;
            break;

        case RainType::VOLUME:
            if (state.rain_interval > 0.0)
                r = raw_value / state.rain_interval * 3600.0;
            break;

        case RainType::CUMULATIVE:
            if (state.rain_interval > 0.0) {
                if (raw_value < state.rain_accum) {
                    // Reset on decrease (new event)
                    r = raw_value / state.rain_interval * 3600.0;
                } else {
                    r = (raw_value - state.rain_accum) / state.rain_interval * 3600.0;
                }
                state.rain_accum = raw_value;
            }
            break;
    }

    return r * state.units_factor * state.scale_factor * state.adjust_factor;
}

PrecipSplit splitPrecip(const SimulationContext& ctx, std::size_t sub) {
    PrecipSplit out{};

    const int gi = ctx.subcatches.gage[sub];
    if (gi < 0 || gi >= ctx.n_gages()) return out;
    const auto ug = static_cast<std::size_t>(gi);

    // Gage intensity already carries units_factor * scale_factor * adjust_factor
    // (see convertRainfall above), matching legacy Gage[].rainfall.
    const double intensity = ctx.gages.rainfall[ug];

    // Legacy gage.c:517 — the IgnoreSnowmelt guard is part of the split, not a
    // downstream concern. With snowmelt ignored, ALL precip is treated as rain
    // regardless of temperature.
    const bool is_snowing = !ctx.options.ignore_snow_melt &&
                            (ctx.climate_state.temperature <= ctx.options.snow_divt);

    if (is_snowing) {
        // snowfall = gage_intensity * SCF * subcatch snow scale
        out.snowfall = intensity
                     * ctx.gages.snow_factor[ug]
                     * ctx.subcatches.snow_scale_factor[sub];
    } else {
        // rainfall = gage_intensity * subcatch rain scale
        out.rainfall = intensity * ctx.subcatches.rain_scale_factor[sub];
    }

    // Convert to internal units (ft/sec), matching legacy's / UCF(RAINFALL).
    const double ucf_rain = ucf::UCF(ucf::RAINFALL, ctx.options);
    out.rainfall /= ucf_rain;
    out.snowfall /= ucf_rain;
    return out;
}

void updatePastRain(GageState& state, double current_time) {
    // Update every hour (3600 seconds)
    if (current_time - state.past_rain_time >= 3600.0) {
        // Shift past rain array backward
        for (int i = MAXPASTRAIN - 1; i > 0; --i) {
            state.past_rain[i] = state.past_rain[i - 1];
        }
        state.past_rain[0] = state.past_rain_accum;
        state.past_rain_accum = 0.0;
        state.past_rain_time = current_time;
    }

    // Accumulate current rainfall (intensity * 1 second => depth per second)
    state.past_rain_accum += state.rainfall / 3600.0;
}

double getPastRain(const GageState& state, int hours) {
    if (hours <= 0 || hours > MAXPASTRAIN) return 0.0;
    double total = 0.0;
    for (int i = 0; i < hours; ++i) {
        total += state.past_rain[i];
    }
    return total;
}

void updateAllGages(SimulationContext& ctx, double current_time) {
    // current_time is absolute OADate (days since 12/30/1899) in fractional days
    for (int j = 0; j < ctx.n_gages(); ++j) {
        auto uj = static_cast<std::size_t>(j);

        // IGNORE_RAINFALL: force this gage's rainfall to zero every step
        // (legacy gage_setState, gage.c:344-347). Zeroing here — ahead of the
        // API override, co-gage copy, and monthly scaling below — transitively
        // zeros the per-subcatchment rain/snow split, the runoff-solver precip,
        // and RDII excess, matching the legacy "no rainfall" behavior.
        if (ctx.options.ignore_rainfall) {
            ctx.gages.rainfall[uj] = 0.0;
            continue;
        }

        // Check API rainfall override (-1.0 means no override)
        if (ctx.gages.api_rainfall[uj] >= 0.0) {
            ctx.gages.rainfall[uj] = ctx.gages.api_rainfall[uj];
            continue;
        }

        // Gap #53: co-gage sharing — copy rainfall from the primary gage that
        // shares this gage's timeseries.  Matches legacy gage_setState() coGage path.
        // The primary's rainfall already has its own scale_factor baked in, so
        // we strip it and multiply by this gage's scale_factor (legacy gage.c:355).
        int co = (uj < ctx.gages.co_gage_index.size())
                 ? ctx.gages.co_gage_index[uj] : -1;
        if (co >= 0 && co < j) {
            const auto uco = static_cast<std::size_t>(co);
            double primary_sf = ctx.gages.scale_factor[uco];
            double this_sf    = ctx.gages.scale_factor[uj];
            double ratio = (primary_sf > 0.0) ? (this_sf / primary_sf) : 1.0;
            ctx.gages.rainfall[uj] = ctx.gages.rainfall[uco] * ratio;
            continue;
        }

        // Read gage properties
        int rain_type = ctx.gages.rain_type[uj];
        double interval = ctx.gages.interval_sec[uj]; // seconds

        // Look up raw rainfall from timeseries using step-function (piecewise constant).
        // Matches legacy gage_setState() exactly:
        //   1. Adds OneSecond offset to time for robust boundary comparison
        //   2. Rain applies for [entryTime, entryTime + rainInterval)
        //   3. Returns 0 in gaps between end-of-interval and next entry
        //   4. Returns 0 after the last time series entry
        //   5. Uses datetime::addSeconds for interval end computation
        //      (decompose-recompose via integer H:M:S — deterministic rounding)
        double t = current_time + datetime::OneSecond;

        int ts_idx = ctx.gages.ts_index[uj];
        double raw_value = 0.0;
        // Select the source series: a FILE_RAIN gage reads from its own resolved
        // rain_series (built by load_external_rain_files); a TIMESERIES gage reads
        // from the shared table pool.  Both reuse the identical step-function below.
        Table* rtbl = nullptr;
        if (ctx.gages.source[uj] == RainSource::FILE_RAIN) {
            if (uj < ctx.gages.rain_series.size() && !ctx.gages.rain_series[uj].empty())
                rtbl = &ctx.gages.rain_series[uj];
        } else if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size())) {
            rtbl = &ctx.tables.tables[static_cast<std::size_t>(ts_idx)];
        }
        if (rtbl) {
            auto& tbl = *rtbl;
            int n = static_cast<int>(tbl.x.size());

            // Step-function lookup: find rightmost entry where x[idx] <= t
            raw_value = table_step_cursor(tbl, t);

            int idx = tbl.cursor.index;
            if (idx >= 0 && idx < n) {
                double entry_start = tbl.x[static_cast<std::size_t>(idx)];
                // Use legacy-identical datetime arithmetic for interval end
                double entry_end = datetime::addSeconds(entry_start, interval);

                if (t >= entry_end) {
                    // Past end of this entry's rain interval.
                    // Check if there's a next entry and t has reached it.
                    int next_idx = idx + 1;
                    if (next_idx < n && t >= tbl.x[static_cast<std::size_t>(next_idx)]) {
                        // Advance to the next entry
                        raw_value = tbl.y[static_cast<std::size_t>(next_idx)];
                        tbl.cursor.index = next_idx;
                        // Check if we're also past this next entry's interval
                        double next_end = datetime::addSeconds(
                            tbl.x[static_cast<std::size_t>(next_idx)], interval);
                        if (t >= next_end) {
                            raw_value = 0.0; // In gap after next entry too
                        }
                    } else {
                        // In dry gap between entries, or past last entry
                        raw_value = 0.0;
                    }
                }
            }
        }

        if (rain_type == 1 && interval > 0.0) {
            // VOLUME: value is depth per interval → convert to in/hr.
            // Match legacy gage.c:692 operand order exactly: r/interval*3600.0
            // (one divide then one multiply), NOT r/(interval/3600.0) which forms
            // the non-representable 1/12 constant first and rounds differently.
            raw_value = raw_value / interval * 3600.0;
        } else if (rain_type == 2 && interval > 0.0) {
            // CUMULATIVE (Gap #31): raw_value is cumulative depth; compute delta.
            // Matches legacy convertRainfall() CUMULATIVE_RAINFALL case:
            //   if new < accum (counter reset) → treat new value as depth this interval
            //   else → delta = new - accum (depth this interval)
            //   always update accumulator with new raw value
            double prev = ctx.gages.cumul_rain_accum[uj];
            double depth = (raw_value < prev)
                ? raw_value              // counter reset — use full value as depth
                : (raw_value - prev);    // normal incremental delta
            ctx.gages.cumul_rain_accum[uj] = raw_value;
            raw_value = depth / interval * 3600.0;  // depth → in/hr (legacy gage.c:697 order)
        }
        // INTENSITY (type 0): already in/hr — no conversion needed

        // PARITY: legacy convertRainfall (gage.c:705) applies
        //   r1 * unitsFactor * scaleFactor * Adjust.rainFactor
        // where Gage.unitsFactor = MMperINCH for FILE gages in SI projects
        // (gage.c:300 — "rain depths on interface file are in inches") and
        // 1.0 otherwise. The FILE series here holds float-quantized inches
        // (see load_external_rain_files), so this restores project mm/hr.
        // Multiplication order must match legacy: unitsFactor BEFORE
        // scaleFactor.
        {
            double units_factor = 1.0;
            if (ctx.gages.source[uj] == RainSource::FILE_RAIN &&
                static_cast<int>(ctx.options.flow_units) >= 3)
                units_factor = 25.40;  // legacy MMperINCH (consts.h:389)
            raw_value = raw_value * units_factor * ctx.gages.scale_factor[uj];
        }

        // Convert from in/hr to ft/sec for internal use
        // Legacy: rainfall stored as in/hr for reporting, converted to ft/sec for runoff
        // We store in in/hr (project rain units) and convert in the runoff solver
        ctx.gages.rainfall[uj] = raw_value;

        // Update past-rain history (hourly buckets for control rules)
        {
            constexpr int MPR = GageData::MAXPASTRAIN;
            double ct_sec = current_time * 86400.0; // to seconds for comparison
            double last = ctx.gages.past_rain_time[uj];
            if (ct_sec - last >= 3600.0) {
                // Shift ring buffer backward
                auto base = uj * static_cast<std::size_t>(MPR);
                for (int k = MPR - 1; k > 0; --k)
                    ctx.gages.past_rain[base + static_cast<std::size_t>(k)] =
                        ctx.gages.past_rain[base + static_cast<std::size_t>(k - 1)];
                ctx.gages.past_rain[base] = ctx.gages.past_rain_accum[uj];
                ctx.gages.past_rain_accum[uj] = 0.0;
                ctx.gages.past_rain_time[uj] = ct_sec;
            }
            // Accumulate: rainfall (in/hr) * (1 second / 3600)
            ctx.gages.past_rain_accum[uj] += raw_value / 3600.0;
        }
    }
}

double getReportRainfall(const SimulationContext& ctx, int gage_idx,
                         double report_date) {
    // IGNORE_RAINFALL: nothing is reported (legacy leaves gage rainfall 0).
    if (ctx.options.ignore_rainfall) return 0.0;

    auto ug = static_cast<std::size_t>(gage_idx);

    // Co-gage: report the primary's report rainfall re-scaled by this gage's
    // factor (legacy gage_setReportRainfall, gage.c:535-541). Co-gage index is
    // always lower than this gage's, so the recursion terminates.
    int co = (ug < ctx.gages.co_gage_index.size())
             ? ctx.gages.co_gage_index[ug] : -1;
    if (co >= 0 && co < gage_idx) {
        const auto uco = static_cast<std::size_t>(co);
        double primary_sf = ctx.gages.scale_factor[uco];
        double this_sf    = ctx.gages.scale_factor[ug];
        double ratio = (primary_sf > 0.0) ? (this_sf / primary_sf) : 1.0;
        return getReportRainfall(ctx, co, report_date) * ratio;
    }

    // API override (legacy gage.c:544-548)
    if (ctx.gages.api_rainfall[ug] >= 0.0) {
        return ctx.gages.api_rainfall[ug];
    }

    // Select the same series the gage state machine reads (Gage.cpp setState):
    // FILE_RAIN gages use their private resolved rain_series; TIMESERIES gages
    // use the shared table pool. (The previous version read only the shared
    // pool, so FILE gages always reported 0.)
    const Table* rtbl = nullptr;
    if (ctx.gages.source[ug] == RainSource::FILE_RAIN) {
        if (ug < ctx.gages.rain_series.size() && !ctx.gages.rain_series[ug].empty())
            rtbl = &ctx.gages.rain_series[ug];
    } else {
        int ts_idx = ctx.gages.ts_index[ug];
        if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size()))
            rtbl = &ctx.tables.tables[static_cast<std::size_t>(ts_idx)];
    }
    if (!rtbl) return 0.0;

    const auto& tbl = *rtbl;
    int n = static_cast<int>(tbl.x.size());
    int idx = tbl.cursor.index;
    if (idx < 0 || idx >= n) return 0.0;

    double interval = ctx.gages.interval_sec[ug];

    // Legacy gage_setReportRainfall (gage.c:550-564): advance the report time
    // by one second, then three-way branch on the current interval end and the
    // next interval start.
    double t = report_date + datetime::OneSecond;

    double entry_start = tbl.x[static_cast<std::size_t>(idx)];
    double entry_end = datetime::addSeconds(entry_start, interval);

    double result;
    if (t < entry_end) {
        // Report time is within current rain interval
        result = tbl.y[static_cast<std::size_t>(idx)];
    } else {
        // Check next entry
        int next_idx = idx + 1;
        if (next_idx < n && t >= tbl.x[static_cast<std::size_t>(next_idx)]) {
            result = tbl.y[static_cast<std::size_t>(next_idx)];
        } else {
            result = 0.0; // In dry gap between entries
        }
    }

    // Convert VOLUME to INTENSITY. Match legacy operand order exactly
    // (gage.c:692): r / interval * 3600.0 — NOT r / (interval/3600.0), which
    // forms a non-representable constant first and rounds differently.
    int rain_type = ctx.gages.rain_type[ug];
    if (rain_type == 1 && interval > 0.0) {
        result = result / interval * 3600.0;
    }
    // CUMULATIVE gages would need the interval delta here (legacy carries the
    // converted value in gage state); none of the parity models use them with
    // report rainfall — revisit if one does.

    // PARITY: unitsFactor (MMperINCH for SI-project FILE gages, gage.c:300)
    // BEFORE scaleFactor — legacy convertRainfall (gage.c:705).
    {
        double units_factor = 1.0;
        if (ctx.gages.source[ug] == RainSource::FILE_RAIN &&
            static_cast<int>(ctx.options.flow_units) >= 3)
            units_factor = 25.40;  // legacy MMperINCH (consts.h:389)
        result = result * units_factor * ctx.gages.scale_factor[ug];
    }

    return result;
}

} // namespace gage
} // namespace openswmm
