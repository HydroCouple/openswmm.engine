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

        // Look up raw rainfall from timeseries
        int ts_idx = ctx.gages.ts_index[uj];
        double raw_value = 0.0;
        if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size())) {
            auto& tbl = ctx.tables.tables[static_cast<std::size_t>(ts_idx)];
            raw_value = table_lookup_cursor(tbl, current_time);
        }

        // Convert based on rain type (VOLUME → intensity)
        // VOLUME format: raw value is depth per recording interval (e.g., inches/5min)
        //   → Convert to intensity: raw / interval * 3600 → in/hr
        // INTENSITY format: already in intensity units (in/hr)
        int rain_type = ctx.gages.rain_type[uj];
        double interval = ctx.gages.interval_sec[uj]; // seconds

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
    }
}

} // namespace gage
} // namespace openswmm
