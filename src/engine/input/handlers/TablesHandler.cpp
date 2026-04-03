/**
 * @file TablesHandler.cpp
 * @brief Section handlers for [TIMESERIES] and [CURVES].
 *
 * ### [TIMESERIES] format — inline data
 * ```
 * ;; Name        Date       Time    Value
 * RAIN1          1/1/2024   0:00    0.00
 * RAIN1                     1:00    0.10
 * RAIN1                     2:00    0.05
 * ```
 *
 * ### [TIMESERIES] format — external file reference
 * ```
 * RAIN2          FILE  "rain_2024.csv"
 * RAIN3          FILE  "rain_2024.csv:EAST_GAGE"
 * ```
 *
 * ### [CURVES] format
 * ```
 * ;; Name      Type       X-Value  Y-Value
 * POND_CURVE   STORAGE    0.0      0.0
 * POND_CURVE              1.0      500.0
 * PUMP1_CURVE  PUMP4      0.0      0.0
 * PUMP1_CURVE             1.0      10.0
 * ```
 *
 * @see Legacy reference: src/solver/input.c — readTimeseries(), readCurve()
 * @see TableData.hpp — Table + TableCursor data structures
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "TablesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/DateTime.hpp"
#include "../../data/TableData.hpp"

#include "../InputParseUtils.hpp"

#include <charconv>
#include <string>
#include <unordered_map>

namespace openswmm::input {

// ============================================================================
// handle_timeseries()
// ============================================================================

void handle_timeseries(SimulationContext& ctx, const std::vector<std::string>& lines) {
    // SWMM timeseries rows can have:
    //   Name  Date  Time  Value   (with explicit date)
    //   Name        Time  Value   (date omitted — same day as previous row)
    //   Name  FILE  "path"        (external file reference)

    std::string current_name;
    double      last_date = 0.0;
    int         current_idx = -1;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        // First column: name (non-empty) or continuation (empty — same name)
        const std::string& maybe_name = tok[0];
        if (!maybe_name.empty()) {
            current_name = maybe_name;
            // Ensure table exists
            current_idx = ctx.table_names.find(current_name);
            if (current_idx < 0) {
                current_idx = ctx.table_names.add(current_name);
                ctx.tables.add(current_name, TableType::TIMESERIES);
            }
        }

        if (current_idx < 0 || current_name.empty()) continue;
        Table& tbl = ctx.tables[current_idx];

        if (tok.size() < 2) continue;

        // Detect FILE reference
        const std::string tok1_upper = Tokenizer::to_upper(tok[1]);
        if (tok1_upper == "FILE") {
            // External file — path (and optional :column) is tok[2]
            // Store in table's id as "FILE:path[:column]" for later loading
            if (tok.size() > 2) {
                tbl.id = "FILE:" + tok[2];
            }
            continue;
        }

        // Detect whether tok[1] looks like a date (contains '/')
        bool has_date = (tok[1].find('/') != std::string::npos);

        double x = 0.0;
        double y = 0.0;

        if (has_date && tok.size() >= 4) {
            // Name  Date  Time  Value
            last_date = parse_datetime(tok[1], tok[2]);
            x = last_date;
            y = to_double(tok[3]);
        } else if (!has_date && tok.size() >= 3) {
            // Name  Time  Value  (continuation, uses last_date)
            double time_frac = 0.0;
            // Parse time HH:MM[:SS] → fractional days
            unsigned th = 0, tm = 0;
            double   ts = 0.0;
            const char* tp   = tok[1].data();
            const char* tend = tok[1].data() + tok[1].size();
            auto rut = [&](unsigned& out) {
                auto [np, ec] = std::from_chars(tp, tend, out);
                if (ec != std::errc{}) return;
                tp = np;
            };
            auto rdt = [&](double& out) {
                auto [np, ec] = openswmm::from_chars_double(tp, tend, out);
                if (ec != std::errc{}) return;
                tp = np;
            };
            rut(th);
            if (tp < tend && *tp == ':') { ++tp; rut(tm); }
            if (tp < tend && *tp == ':') { ++tp; rdt(ts); }
            // Use integer arithmetic matching legacy datetime_encodeTime()
            // to produce identical floating-point time fractions
            time_frac = datetime::encodeTime(static_cast<int>(th),
                                             static_cast<int>(tm),
                                             static_cast<int>(ts));

            // If the time wraps back to 0 (midnight) and we already have
            // data, increment the day
            if (!tbl.x.empty() && time_frac <= std::fmod(tbl.x.back(), 1.0)) {
                last_date += 1.0;
            }
            x = last_date + time_frac;
            y = to_double(tok[2]);
        } else {
            continue;
        }

        tbl.x.push_back(x);
        tbl.y.push_back(y);
    }
}

// ============================================================================
// handle_curves()
// ============================================================================

static const std::unordered_map<std::string, TableType> CURVE_TYPE_MAP = {
    {"STORAGE",   TableType::CURVE_STORAGE},
    {"DIVERSION", TableType::CURVE_DIVERSION},
    {"RATING",    TableType::CURVE_RATING},
    {"SHAPE",     TableType::CURVE_SHAPE},
    {"CONTROL",   TableType::CURVE_CONTROL},
    {"TIDAL",     TableType::CURVE_TIDAL},
    {"PUMP1",     TableType::CURVE_PUMP1},
    {"PUMP2",     TableType::CURVE_PUMP2},
    {"PUMP3",     TableType::CURVE_PUMP3},
    {"PUMP4",     TableType::CURVE_PUMP4},
};

void handle_curves(SimulationContext& ctx, const std::vector<std::string>& lines) {
    std::string  current_name;
    int          current_idx = -1;
    TableType    current_type = TableType::CURVE_RATING;

    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        const std::string& maybe_name = tok[0];
        if (!maybe_name.empty()) {
            current_name = maybe_name;
            current_idx = ctx.table_names.find(current_name);
            if (current_idx < 0) {
                current_idx = ctx.table_names.add(current_name);
                // Type may appear in tok[1] (first row only)
                if (tok.size() > 1) {
                    auto it = CURVE_TYPE_MAP.find(Tokenizer::to_upper(tok[1]));
                    if (it != CURVE_TYPE_MAP.end()) {
                        current_type = it->second;
                    }
                }
                ctx.tables.add(current_name, current_type);
            }
        }

        if (current_idx < 0) continue;
        Table& tbl = ctx.tables[current_idx];

        // Data columns: potentially tok[1]/tok[2] (if type present) or tok[1]/tok[2]
        // After the type token, remaining tokens are x-y pairs
        std::size_t data_start = 1;
        if (tok.size() >= 2) {
            auto it = CURVE_TYPE_MAP.find(Tokenizer::to_upper(tok[1]));
            if (it != CURVE_TYPE_MAP.end()) {
                data_start = 2;  // type token consumed
            }
        }

        // Pairs: x y [x y ...]
        for (std::size_t i = data_start; i + 1 < tok.size(); i += 2) {
            tbl.x.push_back(to_double(tok[i]));
            tbl.y.push_back(to_double(tok[i + 1]));
        }
    }
}

} /* namespace openswmm::input */
