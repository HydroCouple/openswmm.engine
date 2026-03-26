/**
 * @file Gage.cpp
 * @brief Rain gage processing — numerically identical to legacy gage.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Gage.hpp"
#include "../core/SimulationContext.hpp"
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

    return r * state.units_factor * state.adjust_factor;
}

void separatePrecip(GageState& state, double intensity,
                    double temperature, double snow_temp) {
    if (temperature <= snow_temp) {
        state.snowfall = intensity * state.snow_factor;
        state.rainfall = 0.0;
    } else {
        state.rainfall = intensity;
        state.snowfall = 0.0;
    }
    state.total_precip = state.rainfall + state.snowfall;
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
    // current_time is absolute Julian date in fractional days
    for (int j = 0; j < ctx.n_gages(); ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Check API rainfall override (-1.0 means no override)
        if (ctx.gages.api_rainfall[uj] >= 0.0) {
            ctx.gages.rainfall[uj] = ctx.gages.api_rainfall[uj];
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
        if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size())) {
            auto& tbl = ctx.tables.tables[static_cast<std::size_t>(ts_idx)];
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
            // VOLUME: value is depth per interval → convert to in/hr
            raw_value = raw_value / (interval / 3600.0);
        }
        // INTENSITY (type 0): already in/hr — no conversion
        // CUMULATIVE (type 2): would need state tracking — not yet implemented

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

} // namespace gage
} // namespace openswmm
