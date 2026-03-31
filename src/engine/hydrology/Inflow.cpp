/**
 * @file Inflow.cpp
 * @brief External/DWF inflows — batch SoA, numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Inflow.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include "../data/TableData.hpp"
#include <cmath>
#include <algorithm>
#include <string>
#include <unordered_map>

namespace openswmm {
namespace inflow {

void ExtInflowSoA::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);
    node_idx.assign(un, -1);
    ts_idx.assign(un, -1);
    base_pat_idx.assign(un, -1);
    baseline.assign(un, 0.0);
    scale_factor.assign(un, 1.0);
    conv_factor.assign(un, 1.0);
}

void DwfInflowSoA::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);
    node_idx.assign(un, -1);
    avg_value.assign(un, 0.0);
    pat_monthly.assign(un, -1);
    pat_daily.assign(un, -1);
    pat_hourly.assign(un, -1);
    pat_weekend.assign(un, -1);
}

double InflowSolver::getPatternFactor(int pat_idx, int month, int day, int hour) const {
    if (pat_idx < 0 || pat_idx >= static_cast<int>(patterns_.size()))
        return 1.0;

    const auto& pat = patterns_[static_cast<std::size_t>(pat_idx)];
    switch (pat.type) {
        case 0: return pat.factors[month % 12];           // monthly
        case 1: return pat.factors[day % 7];              // daily
        case 2: return pat.factors[hour % 24];            // hourly
        case 3: return pat.factors[hour % 24];            // weekend (same layout)
        default: return 1.0;
    }
}

void InflowSolver::init(SimulationContext& ctx) {

    // ---- Build pattern name → index map for fast lookup ----
    // PatternData stores names; we build a local map to resolve DWF pattern
    // names to indices.  The pattern index is the position in ctx.patterns.
    std::unordered_map<std::string, int> pattern_map;
    int np = ctx.patterns.count();
    for (int i = 0; i < np; ++i) {
        pattern_map[ctx.patterns.names[static_cast<std::size_t>(i)]] = i;
    }

    // ---- Copy patterns into runtime structures ----
    patterns_.resize(static_cast<std::size_t>(np));
    for (int i = 0; i < np; ++i) {
        auto ui = static_cast<std::size_t>(i);
        patterns_[ui].type = ctx.patterns.types[ui];
        const auto& facs = ctx.patterns.factors[ui];
        // Initialize all 24 slots to 1.0 (default multiplier)
        for (int k = 0; k < 24; ++k) patterns_[ui].factors[k] = 1.0;
        for (std::size_t k = 0; k < facs.size() && k < 24; ++k) {
            patterns_[ui].factors[k] = facs[k];
        }
    }

    // ---- Populate external inflows with name resolution ----
    int ne = ctx.ext_inflows.count();
    ext_inflows_.resize(ne);
    for (int i = 0; i < ne; ++i) {
        auto ui = static_cast<std::size_t>(i);
        ext_inflows_.node_idx[ui]     = ctx.ext_inflows.node_idx[ui];
        ext_inflows_.baseline[ui]     = ctx.ext_inflows.baseline[ui];
        ext_inflows_.scale_factor[ui] = ctx.ext_inflows.s_factor[ui];

        // Unit conversion factor (m_factor).
        // For FLOW inflows, legacy uses cf = 1.0/UCF(FLOW) to convert from
        // user flow units to internal units (cfs).  The parser already stores
        // this in m_factor, so we use it directly.
        ext_inflows_.conv_factor[ui]  = ctx.ext_inflows.m_factor[ui];

        // Resolve timeseries name → table index
        const auto& ts_name = ctx.ext_inflows.ts_name[ui];
        if (!ts_name.empty()) {
            ext_inflows_.ts_idx[ui] = ctx.table_names.find(ts_name);
        } else {
            ext_inflows_.ts_idx[ui] = -1;
        }

        // Resolve baseline pattern name → pattern index
        const auto& pat_name = ctx.ext_inflows.pattern_name[ui];
        if (!pat_name.empty()) {
            auto pit = pattern_map.find(pat_name);
            ext_inflows_.base_pat_idx[ui] = (pit != pattern_map.end()) ? pit->second : -1;
        } else {
            ext_inflows_.base_pat_idx[ui] = -1;
        }
    }

    // ---- Populate DWF inflows with name resolution + pattern sorting ----
    int nd = ctx.dwf_inflows.count();
    dwf_inflows_.resize(nd);
    for (int i = 0; i < nd; ++i) {
        auto ui = static_cast<std::size_t>(i);
        dwf_inflows_.node_idx[ui]  = ctx.dwf_inflows.node_idx[ui];
        dwf_inflows_.avg_value[ui] = ctx.dwf_inflows.avg_value[ui];

        // Resolve up to 4 pattern names to indices, then sort by pattern type.
        // Legacy inflow_initDwfInflow() reorders patterns into:
        //   [0]=MONTHLY, [1]=DAILY, [2]=HOURLY, [3]=WEEKEND
        // regardless of the order they were supplied in the .inp file.
        int tmp_pats[4] = {-1, -1, -1, -1};

        // The four pattern name fields from parsed data (in input order)
        const std::string* pat_fields[4] = {
            &ctx.dwf_inflows.pat1[ui],
            &ctx.dwf_inflows.pat2[ui],
            &ctx.dwf_inflows.pat3[ui],
            &ctx.dwf_inflows.pat4[ui]
        };

        for (int p = 0; p < 4; ++p) {
            if (pat_fields[p]->empty()) continue;
            auto pit = pattern_map.find(*pat_fields[p]);
            if (pit == pattern_map.end()) continue;
            int pat_idx = pit->second;
            // Sort into correct position by pattern type
            int pat_type = ctx.patterns.types[static_cast<std::size_t>(pat_idx)];
            if (pat_type >= 0 && pat_type < 4) {
                tmp_pats[pat_type] = pat_idx;
            }
        }

        dwf_inflows_.pat_monthly[ui] = tmp_pats[MONTHLY_PATTERN];
        dwf_inflows_.pat_daily[ui]   = tmp_pats[DAILY_PATTERN];
        dwf_inflows_.pat_hourly[ui]  = tmp_pats[HOURLY_PATTERN];
        dwf_inflows_.pat_weekend[ui] = tmp_pats[WEEKEND_PATTERN];
    }
}

void InflowSolver::computeAll(SimulationContext& ctx, double current_date, double /*dt*/) {

    // ---- Extract date components from decimal days ----
    // Legacy uses datetime_monthOfYear, datetime_dayOfWeek, datetime_hourOfDay.
    // We use the centralized DateTime.hpp functions.
    //
    // For the legacy SWMM calendar:
    //   month = monthOfYear(aDate) - 1      (0-based, 0=Jan)
    //   day   = dayOfWeek(aDate) - 1         (0-based, 0=Sun)
    //   hour  = hourOfDay(aDate)             (0-based, 0-23)

    int h_tmp, m_tmp, s_tmp;
    datetime::decodeTime(current_date, h_tmp, m_tmp, s_tmp);
    int hour = h_tmp;

    // Day of week: Julian day 0 is Monday.  Legacy dayOfWeek returns 1=Sun..7=Sat,
    // then subtracts 1 giving 0=Sun..6=Sat.
    // Julian day number % 7 gives: 0=Mon, 1=Tue, ... , 6=Sun
    // We need 0=Sun, so: (julianDay % 7 + 1) % 7 maps Mon(0)->1, Sun(6)->0.
    int total_days = static_cast<int>(std::floor(current_date));
    int day = (total_days % 7 + 1) % 7;

    // Month of year using DateTime API (1-based), convert to 0-based.
    int month = datetime::monthOfYear(current_date) - 1;

    // ---- Batch external inflows (gather + multiply + scatter-add) ----
    for (int i = 0; i < ext_inflows_.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Baseline value, optionally modulated by a time pattern
        double base = ext_inflows_.baseline[ui];
        int bp = ext_inflows_.base_pat_idx[ui];
        if (bp >= 0) {
            base *= getPatternFactor(bp, month, day, hour);
        }

        // Timeseries value, looked up via cursor-optimized table lookup.
        // Legacy: tsv = table_tseriesLookup(&Tseries[k], aDate, FALSE) * sf
        double ts_val = 0.0;
        int ts = ext_inflows_.ts_idx[ui];
        if (ts >= 0 && ts < static_cast<int>(ctx.tables.count())) {
            ts_val = table_lookup_cursor(ctx.tables[ts], current_date);
            ts_val *= ext_inflows_.scale_factor[ui];
        }

        // Combined inflow: cf * (tsv + blv)
        // Matches legacy: cf * (tsv + blv) in inflow_getExtInflow
        double q = ext_inflows_.conv_factor[ui] * (ts_val + base);

        // Scatter-add to node lateral flow
        int ni = ext_inflows_.node_idx[ui];
        if (ni >= 0 && ni < static_cast<int>(ctx.nodes.lat_flow.size())) {
            ctx.nodes.lat_flow[static_cast<std::size_t>(ni)] += q;
        }

        // Accumulate for mass balance tracking
        if (q > 0.0) ctx.mass_balance.step_ext_inflow += q;
    }

    // ---- Batch DWF inflows (pattern multiply chain + scatter-add) ----
    // Matches legacy inflow_getDwfInflow: f = monthly * daily * (hourly|weekend)
    for (int i = 0; i < dwf_inflows_.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        double factor = 1.0;

        // Monthly pattern
        int pm = dwf_inflows_.pat_monthly[ui];
        if (pm >= 0) factor *= getPatternFactor(pm, month, day, hour);

        // Daily pattern
        int pd = dwf_inflows_.pat_daily[ui];
        if (pd >= 0) factor *= getPatternFactor(pd, month, day, hour);

        // Hourly vs weekend pattern (matches legacy logic exactly):
        //   if weekend pattern exists:
        //     if day is Sun(0) or Sat(6): use weekend pattern
        //     else if hourly pattern exists: use hourly pattern
        //   else if hourly pattern exists: use hourly pattern
        int ph = dwf_inflows_.pat_hourly[ui];
        int pw = dwf_inflows_.pat_weekend[ui];
        if (pw >= 0) {
            if (day == 0 || day == 6) {
                factor *= getPatternFactor(pw, month, day, hour);
            } else if (ph >= 0) {
                factor *= getPatternFactor(ph, month, day, hour);
            }
        } else if (ph >= 0) {
            factor *= getPatternFactor(ph, month, day, hour);
        }

        double q = factor * dwf_inflows_.avg_value[ui];

        int ni = dwf_inflows_.node_idx[ui];
        if (ni >= 0 && ni < static_cast<int>(ctx.nodes.lat_flow.size())) {
            ctx.nodes.lat_flow[static_cast<std::size_t>(ni)] += q;
        }

        // Accumulate for mass balance tracking
        if (q > 0.0) ctx.mass_balance.step_dw_inflow += q;
    }
}

} // namespace inflow
} // namespace openswmm
