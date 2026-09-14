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
                            (ctx.climate_state.temperature <= ctx.climate_state.snow_divt);

    // Legacy order: gage_getPrecip divides by UCF(RAINFALL) first
    // (gage.c:524-526), then getNetPrecip applies the subcatchment's scale
    // factor (subcatch.c:789-790).
    const double ucf_rain = ucf::UCF(ucf::RAINFALL, ctx.options);
    if (is_snowing) {
        out.snowfall = intensity * ctx.gages.snow_factor[ug] / ucf_rain;
        out.snowfall *= ctx.subcatches.snow_scale_factor[sub];
    } else {
        out.rainfall = intensity / ucf_rain;
        out.rainfall *= ctx.subcatches.rain_scale_factor[sub];
    }
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

// ---------------------------------------------------------------------------
// Shared rainfall resolution (see Gage.hpp)
// ---------------------------------------------------------------------------

const Table* gageRainSeries(const SimulationContext& ctx, int gage_idx) {
    if (gage_idx < 0 || gage_idx >= ctx.n_gages()) return nullptr;
    const auto ug = static_cast<std::size_t>(gage_idx);

    if (ctx.gages.source[ug] == RainSource::FILE_RAIN) {
        if (ug < ctx.gages.rain_series.size() && !ctx.gages.rain_series[ug].empty())
            return &ctx.gages.rain_series[ug];
        return nullptr;
    }
    const int ts_idx = ctx.gages.ts_index[ug];
    if (ts_idx >= 0 && ts_idx < static_cast<int>(ctx.tables.tables.size()))
        return &ctx.tables.tables[static_cast<std::size_t>(ts_idx)];
    return nullptr;
}

double gageUnitsFactor(const SimulationContext& ctx, int gage_idx) {
    if (gage_idx < 0 || gage_idx >= ctx.n_gages()) return 1.0;
    const auto ug = static_cast<std::size_t>(gage_idx);

    // PARITY: legacy sets Gage.unitsFactor = MMperINCH for SI projects
    // (gage.c:300) because the rain interface file stores inches. Only the
    // STANDARD loader reproduces that storage convention — USER_CSV holds
    // project units directly — so the factor is pinned to the format, which
    // leaves the legacy path byte-identical.
    if (ctx.gages.source[ug] == RainSource::FILE_RAIN &&
        ctx.gages.file_format[ug] == RainFileFormat::STAN_PRCP &&
        static_cast<int>(ctx.options.flow_units) >= 3)
        return 25.40;  // legacy MMperINCH (consts.h:389)
    return 1.0;
}

double convertGageValue(double raw, int rain_type, double interval_sec,
                        double& cumul_accum, double units_factor,
                        double scale_factor) {
    if (rain_type == 1 && interval_sec > 0.0) {
        // VOLUME: depth per interval -> in/hr. Match legacy operand order
        // exactly (gage.c:692): r/interval*3600.0 — one divide then one
        // multiply, NOT r/(interval/3600.0), which forms a non-representable
        // constant first and rounds differently.
        raw = raw / interval_sec * 3600.0;
    } else if (rain_type == 2 && interval_sec > 0.0) {
        // CUMULATIVE: a decrease is a counter reset, and the new value is then
        // the whole interval's depth (legacy convertRainfall).
        const double depth = (raw < cumul_accum) ? raw : (raw - cumul_accum);
        cumul_accum = raw;
        raw = depth / interval_sec * 3600.0;
    }
    // INTENSITY (0): already in/hr.

    // PARITY: unitsFactor BEFORE scaleFactor (legacy gage.c:705).
    return raw * units_factor * scale_factor;
}

bool gageIsUsed(const SimulationContext& ctx, int gage_idx) {
    for (int i = 0; i < ctx.n_subcatches(); ++i)
        if (ctx.subcatches.gage[static_cast<std::size_t>(i)] == gage_idx) return true;
    for (const auto& gname : ctx.unit_hyds.gage_names)
        if (ctx.gage_names.find(gname) == gage_idx) return true;
    return false;
}

// ---------------------------------------------------------------------------
// Legacy rain-gage state machine — gage.c gage_initState / gage_setState /
// getNextRainfall / gage_getNextRainDate / gage_setReportRainfall — kept on
// the gage's series as record indices (GageData::st_*). A record's rate is
// legacy convertRainfall: VOLUME r / interval * 3600; CUMULATIVE the delta
// from the previous record (rainAccum is the previous record's raw value,
// legacy reads the records in order), a decrease being a counter reset;
// then unitsFactor, then scaleFactor. The monthly rain adjustment is applied
// by the runoff step afterwards (legacy folds it in at read time).
// ---------------------------------------------------------------------------
namespace {

constexpr int    kNoState   = -2;   ///< st_cur: legacy startDate == NO_DATE
constexpr int    kPreRecord = -1;   ///< st_cur: the [StartDateTime, x0) interval
constexpr int    kNoDate    = -1;   ///< st_next: legacy nextDate == NO_DATE
constexpr double kNoDateValue = -693594.0;   ///< legacy NO_DATE (1/1/0001)

double recordRate(const SimulationContext& ctx, int j, const Table& tbl, int k) {
    const auto uk = static_cast<std::size_t>(k);
    const auto uj = static_cast<std::size_t>(j);
    const int rain_type = ctx.gages.rain_type[uj];
    const double interval = ctx.gages.interval_sec[uj];
    double r = tbl.y[uk];
    if (rain_type == 1 && interval > 0.0) {
        // one divide then one multiply (gage.c:692), not r/(interval/3600)
        r = r / interval * 3600.0;
    } else if (rain_type == 2 && interval > 0.0) {
        const double prev = (k > 0) ? tbl.y[uk - 1] : 0.0;
        r = (r < prev) ? (r / interval * 3600.0)
                       : ((r - prev) / interval * 3600.0);
    }
    // unitsFactor BEFORE scaleFactor (gage.c:705)
    return r * gageUnitsFactor(ctx, j) * ctx.gages.scale_factor[uj];
}

// getNextRainfall: the next record after `from` whose rate is not zero
// (explicit zeros — and a cumulative gage's zero deltas — are skipped so the
// wet/dry accounting sees them as a gap).
int nextNonzeroRecord(const SimulationContext& ctx, int j, const Table& tbl, int from) {
    const int n = static_cast<int>(tbl.x.size());
    for (int k = from + 1; k < n; ++k)
        if (recordRate(ctx, j, tbl, k) != 0.0) return k;
    return kNoDate;
}

// gage_initState (project_init): seeded from the FIRST record whatever its
// value (getFirstRainfall); a record beginning after the simulation start
// makes the current interval [StartDateTime, x0) with no rain and record 0
// the next one. Under IGNORE_RAINFALL legacy returns before reading the
// series, so there is no state (and no runoff-step limit) at all.
void initGageState(SimulationContext& ctx, int j, const Table* tbl) {
    const auto uj = static_cast<std::size_t>(j);
    ctx.gages.st_init[uj] = 1;
    ctx.gages.st_used[uj] = gageIsUsed(ctx, j) ? 1 : 0;
    ctx.gages.st_cur[uj]  = kNoState;
    ctx.gages.st_next[uj] = kNoDate;
    ctx.gages.st_rain[uj] = 0.0;
    if (ctx.options.ignore_rainfall) return;
    if (!tbl || tbl->x.empty()) return;
    if (tbl->x[0] > ctx.options.start_date) {
        ctx.gages.st_cur[uj]  = kPreRecord;
        ctx.gages.st_next[uj] = 0;
    } else {
        ctx.gages.st_cur[uj]  = 0;
        ctx.gages.st_rain[uj] = recordRate(ctx, j, *tbl, 0);
        ctx.gages.st_next[uj] = nextNonzeroRecord(ctx, j, *tbl, 0);
    }
}

// legacy startDate / endDate of the current interval
void currentInterval(const SimulationContext& ctx, int j, const Table& tbl,
                     double& start, double& end) {
    const auto uj = static_cast<std::size_t>(j);
    const int cur = ctx.gages.st_cur[uj];
    if (cur == kPreRecord) {
        start = ctx.options.start_date;
        end   = tbl.x[0];
    } else {
        start = tbl.x[static_cast<std::size_t>(cur)];
        end   = datetime::addSeconds(start, ctx.gages.interval_sec[uj]);
    }
}

// gage_setState's march (t already carries legacy's +1 s)
void setGageState(SimulationContext& ctx, int j, const Table& tbl, double t) {
    const auto uj = static_cast<std::size_t>(j);
    for (;;) {
        if (ctx.gages.st_cur[uj] == kNoState) { ctx.gages.st_rain[uj] = 0.0; return; }
        double start, end;
        currentInterval(ctx, j, tbl, start, end);
        if (t < start) { ctx.gages.st_rain[uj] = 0.0; return; }   // before the interval
        if (t < end)   return;                                     // inside it: keep
        const int nxt = ctx.gages.st_next[uj];
        if (nxt < 0)   { ctx.gages.st_rain[uj] = 0.0; return; }   // no next interval
        if (t < tbl.x[static_cast<std::size_t>(nxt)]) { ctx.gages.st_rain[uj] = 0.0; return; }
        ctx.gages.st_cur[uj]  = nxt;                               // advance
        ctx.gages.st_rain[uj] = recordRate(ctx, j, tbl, nxt);
        ctx.gages.st_next[uj] = nextNonzeroRecord(ctx, j, tbl, nxt);
    }
}

} // namespace

double gageNextRainDate(const SimulationContext& ctx, int gage_idx, double t) {
    const auto ug = static_cast<std::size_t>(gage_idx);
    if (ug >= ctx.gages.st_cur.size() || !ctx.gages.st_used[ug]) return t;   // legacy: aDate
    const int cur = ctx.gages.st_cur[ug];
    const Table* tbl = gageRainSeries(ctx, gage_idx);
    if (cur == kNoState || !tbl || tbl->x.empty()) return kNoDateValue;
    const double t1 = t + datetime::OneSecond;
    double start, end;
    currentInterval(ctx, gage_idx, *tbl, start, end);
    if (t1 < start) return start;
    if (t1 < end)   return end;
    const int nxt = ctx.gages.st_next[ug];
    return (nxt >= 0) ? tbl->x[static_cast<std::size_t>(nxt)] : kNoDateValue;
}

void updateAllGages(SimulationContext& ctx, double current_time) {
    // current_time is absolute OADate (days since 12/30/1899) in fractional days
    for (int j = 0; j < ctx.n_gages(); ++j) {
        auto uj = static_cast<std::size_t>(j);
        Table* rtbl = const_cast<Table*>(gageRainSeries(ctx, j));
        if (uj < ctx.gages.st_init.size() && !ctx.gages.st_init[uj])
            initGageState(ctx, j, rtbl);   // legacy gage_initState, once

        // legacy gage_setState returns at once for a gage no subcatchment
        // or unit-hydrograph group reads; its rainfall keeps the seeded
        // value (runoff_execute still tests it for IsRaining).
        if (uj < ctx.gages.st_used.size() && !ctx.gages.st_used[uj]) {
            ctx.gages.rainfall[uj] = ctx.gages.st_rain[uj];
            continue;
        }

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

        // The legacy machine's march to this step's date (+1 s): the
        // current record's rate inside its interval, 0 before it, in the
        // gap before the next non-zero record, or past the last one.
        // (The value is kept in the project's rain units, in/hr or mm/hr,
        // as legacy Gage.rainfall; the runoff solver converts to ft/s.)
        double raw_value = 0.0;
        if (rtbl) {
            setGageState(ctx, j, *rtbl, current_time + datetime::OneSecond);
            raw_value = ctx.gages.st_rain[uj];
        }
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

    // legacy gage_setReportRainfall (gage.c:550-564) on the machine's state
    // as the last gage update left it (the start of the current runoff
    // step): the report instant + 1 s inside the current interval reads
    // Gage.rainfall as setState left it (0 when it found the date before
    // the interval), before the next record 0, otherwise the next record's
    // rate (a report instant on a record boundary reads the record that
    // begins there even though the runoff step reading it has not run).
    const Table* rtbl = gageRainSeries(ctx, gage_idx);
    if (!rtbl || rtbl->x.empty()) return 0.0;
    if (ug >= ctx.gages.st_cur.size() || ctx.gages.st_cur[ug] == kNoState) return 0.0;
    const double t = report_date + datetime::OneSecond;
    double start, end;
    currentInterval(ctx, gage_idx, *rtbl, start, end);
    if (t < end) return ctx.gages.st_rain[ug];
    const int nxt = ctx.gages.st_next[ug];
    if (nxt < 0) return 0.0;                                   // nextRainfall = 0 past the end
    if (t < rtbl->x[static_cast<std::size_t>(nxt)]) return 0.0;
    return recordRate(ctx, gage_idx, *rtbl, nxt);
}

} // namespace gage
} // namespace openswmm
