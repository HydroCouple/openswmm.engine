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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "TablesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../SectionParser.hpp"
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

    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.empty()) continue;

        // First column: name (non-empty) or continuation (empty — same name)
        const std::string& maybe_name = tok[0];
        bool is_new_table = false;
        if (!maybe_name.empty()) {
            current_name = maybe_name;
            // Ensure table exists (kind-scoped: a curve with the same name
            // is a DIFFERENT object, matching legacy's separate hash tables)
            current_idx = ctx.find_timeseries(current_name);
            if (current_idx < 0) {
                current_idx = ctx.tables.add(current_name, TableType::TIMESERIES);
                is_new_table = true;
            }
        }
        // Attach comment to the table on its first (name-introducing) row
        if (is_new_table && !pl.comment.empty() && current_idx >= 0)
            ctx.tables[current_idx].comment = pl.comment;

        if (current_idx < 0 || current_name.empty()) continue;
        Table& tbl = ctx.tables[current_idx];

        if (tok.size() < 2) continue;

        // Detect FILE reference
        const std::string tok1_upper = Tokenizer::to_upper(tok[1]);
        if (tok1_upper == "FILE") {
            // External file — tok[2] is the quoted path, optionally with a
            // `:column` suffix inside the quotes (e.g. "rain.csv:East_Gage").
            // Store the inner token verbatim in Table::file_path; the
            // PostParseResolver splits the column suffix when loading, and
            // InpWriter emits the whole thing unchanged for byte-fidelity
            // round-trip.  tbl.id is left as the table name set above.
            if (tok.size() > 2) {
                std::string path = tok[2];
                if (path.size() >= 2 && path.front() == '"' && path.back() == '"')
                    path = path.substr(1, path.size() - 2);
                tbl.file_path = std::move(path);
            }
            continue;
        }

        // Detect whether tok[1] looks like a date (contains '/')
        bool has_date = (tok[1].find('/') != std::string::npos);

        double x = 0.0;
        double y = 0.0;

        if (has_date && tok.size() >= 4) {
            // Name  Date  Time  Value
            x = parse_datetime(tok[1], tok[2]);
            // Carry only the DATE (midnight) to subsequent date-less
            // continuation rows — matching legacy table_readTimeseries, where
            // aDate holds the date and x = aDate + aTime. Storing the FULL
            // datetime here corrupted continuation rows (e.g. a row "00:30"
            // after a dated "10/01/2003 00:25" became 00:25+00:30 = 00:55),
            // scrambling rain/inflow hydrographs that repeat the date every N
            // rows.
            last_date = std::floor(x);
            y = to_double(tok[3]);
        } else if (!has_date && tok.size() >= 3) {
            // Name  Time  Value  (continuation, uses last_date)
            //
            // The time token may be EITHER decimal hours (a bare number such
            // as "0.25" or "3.0") OR a clock string "HH:MM[:SS]".  This mirrors
            // legacy table_readTimeseries() / datetime_strToTime(): try a full
            // decimal parse first — if the whole token is a number it is
            // decimal hours, converted to a fraction of a day (÷24); otherwise
            // fall back to the integer HH:MM:SS path.  The previous code only
            // handled HH:MM:SS, so a bare "0.25" was truncated to 0, dropping
            // most of the series (e.g. EXTRAN inflow hydrographs).
            double time_frac = 0.0;
            const char* tbeg = tok[1].data();
            const char* tend = tbeg + tok[1].size();
            double dec_hours = 0.0;
            auto [dp, dec] = openswmm::from_chars_double(tbeg, tend, dec_hours);
            if (dec == std::errc{} && dp == tend) {
                // Whole token is a number → decimal hours → fraction of day.
                time_frac = dec_hours / 24.0;
            } else {
                // Clock format HH:MM[:SS].  Use integer arithmetic matching
                // legacy datetime_encodeTime() for identical time fractions.
                unsigned th = 0, tm = 0;
                double   ts = 0.0;
                const char* tp = tbeg;
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
                time_frac = datetime::encodeTime(static_cast<int>(th),
                                                 static_cast<int>(tm),
                                                 static_cast<int>(ts));
            }

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
    {"PUMP5",     TableType::CURVE_PUMP5},
};

void handle_curves(SimulationContext& ctx, const std::vector<std::string>& lines) {
    std::string  current_name;
    int          current_idx = -1;
    TableType    current_type = TableType::CURVE_RATING;

    for (const auto& pl : parse_section(lines)) {
        auto tok = Tokenizer::tokenize(pl.data);
        if (tok.empty()) continue;

        const std::string& maybe_name = tok[0];
        bool is_new_table = false;
        if (!maybe_name.empty()) {
            current_name = maybe_name;
            // Kind-scoped: a timeseries with the same name is a DIFFERENT
            // object, matching legacy's separate hash tables.
            current_idx = ctx.find_curve(current_name);
            if (current_idx < 0) {
                // Type may appear in tok[1] (first row only)
                if (tok.size() > 1) {
                    auto it = CURVE_TYPE_MAP.find(Tokenizer::to_upper(tok[1]));
                    if (it != CURVE_TYPE_MAP.end()) {
                        current_type = it->second;
                    }
                }
                current_idx = ctx.tables.add(current_name, current_type);
                is_new_table = true;
            }
        }

        if (current_idx < 0) continue;
        Table& tbl = ctx.tables[current_idx];
        if (is_new_table && !pl.comment.empty())
            tbl.comment = pl.comment;

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
