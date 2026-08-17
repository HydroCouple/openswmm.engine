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
 * @file MultiColumnSeriesFile.cpp
 * @brief Parse-once cache for multi-column series files (CSV/TSV/TSF).
 * @see MultiColumnSeriesFile.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "MultiColumnSeriesFile.hpp"
#include "../core/DateTime.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <numeric>

namespace openswmm::input {

namespace {

std::atomic<long> g_parse_count_total{0};

/// Trim ASCII whitespace and surrounding double quotes.
/// (Relocated from PostParseResolver.cpp csv_trim.)
std::string cell_trim(const std::string& s) {
    std::size_t a = 0, b = s.size();
    auto space = [](char c) {
        return c == ' ' || c == '\t' || c == '\r' || c == '\n';
    };
    while (a < b && space(s[a])) ++a;
    while (b > a && space(s[b - 1])) --b;
    if (b - a >= 2 && s[a] == '"' && s[b - 1] == '"') { ++a; --b; }
    return s.substr(a, b - a);
}

/// Quote-aware split on a single-character delimiter.
/// (Generalises PostParseResolver.cpp csv_split to comma or tab.)
std::vector<std::string> cell_split(const std::string& line, char delim) {
    std::vector<std::string> out;
    std::string cur;
    bool in_quotes = false;
    for (const char c : line) {
        if (c == '"') { in_quotes = !in_quotes; cur.push_back(c); }
        else if (c == delim && !in_quotes) { out.push_back(cell_trim(cur)); cur.clear(); }
        else cur.push_back(c);
    }
    out.push_back(cell_trim(cur));
    return out;
}

/// Count delimiters OUTSIDE double quotes — the delimiter sniff has to use
/// the same notion of a delimiter as cell_split above, or a quoted header
/// name containing the other delimiter (`Date<TAB>"a,b,c"`) sniffs wrong and
/// the whole file mis-splits.
long count_unquoted(const std::string& line, char delim) {
    long n = 0;
    bool in_quotes = false;
    for (const char c : line) {
        if (c == '"') in_quotes = !in_quotes;
        else if (c == delim && !in_quotes) ++n;
    }
    return n;
}

/// Case-insensitive equality (relocated from PostParseResolver.cpp csv_iequals).
bool iequals(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i]))
            != std::tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

/// 0 = none, 1 = AM, 2 = PM — detected from the cell's trailing token.
int trailing_meridiem(const std::string& cell) {
    std::size_t e = cell.find_last_not_of(" \t\r\n");
    if (e == std::string::npos || e < 1) return 0;
    const char m1 = static_cast<char>(std::toupper(static_cast<unsigned char>(cell[e])));
    const char m0 = static_cast<char>(std::toupper(static_cast<unsigned char>(cell[e - 1])));
    if (m1 != 'M' || (m0 != 'A' && m0 != 'P')) return 0;
    return m0 == 'A' ? 1 : 2;
}

/// Strip a UTF-8 BOM and a trailing '\r' (files written on Windows).
void normalize_line(std::string& line, bool first_line) {
    if (first_line && line.size() >= 3 &&
        static_cast<unsigned char>(line[0]) == 0xEF &&
        static_cast<unsigned char>(line[1]) == 0xBB &&
        static_cast<unsigned char>(line[2]) == 0xBF)
        line.erase(0, 3);
    if (!line.empty() && line.back() == '\r') line.pop_back();
}

bool is_blank_or_comment(const std::string& line) {
    for (const char c : line) {
        if (c == ' ' || c == '\t') continue;
        return c == ';' || c == '#';
    }
    return true;  // all-whitespace / empty
}

bool istarts_with(const std::string& line, std::string_view prefix) {
    if (line.size() < prefix.size()) return false;
    return iequals(std::string_view(line).substr(0, prefix.size()), prefix);
}

} // namespace

// ---------------------------------------------------------------------------
// ParsedSeriesFile
// ---------------------------------------------------------------------------

int ParsedSeriesFile::find_column(std::string_view name) const {
    // Column 0 is the time stamp, so a match there is as wrong as no match.
    for (std::size_t i = 1; i < headers.size(); ++i)
        if (iequals(headers[i], name)) return static_cast<int>(i);
    return -1;
}

// ---------------------------------------------------------------------------
// Datetime parsing (relocated csv_parse_datetime + 12-hour AM/PM)
// ---------------------------------------------------------------------------

bool parse_series_datetime(const std::string& cell, double& out) {
    int y = 0, mo = 0, d = 0, h = 0, mi = 0, s = 0;
    const char* c = cell.c_str();

    // ISO-8601: YYYY-MM-DD[ T]HH:MM[:SS]
    int n = std::sscanf(c, "%d-%d-%d%*[ T]%d:%d:%d", &y, &mo, &d, &h, &mi, &s);
    if (n >= 3) {
        if (n < 4) { h = mi = s = 0; }
        else if (n < 6) { s = 0; }
        out = datetime::encodeDate(y, mo, d) + datetime::encodeTime(h, mi, s);
        return true;
    }

    // US: MM/DD/YYYY[ HH:MM[:SS]], optionally 12-hour with a trailing AM/PM
    // token (the PCSWMM TSF form). The `n < 5` guard here used to drop a
    // parsed hour when the minutes were absent — fixed to mirror the ISO
    // branch above (plan §3.1).
    n = std::sscanf(c, "%d/%d/%d %d:%d:%d", &mo, &d, &y, &h, &mi, &s);
    if (n >= 3) {
        if (n < 4) { h = mi = s = 0; }
        else if (n < 6) { s = 0; }
        if (n >= 4) {
            const int meridiem = trailing_meridiem(cell);
            if (meridiem != 0) {
                if (h == 12) h = 0;          // 12:xx AM → 00:xx, 12:xx PM → 12:xx
                if (meridiem == 2) h += 12;  // PM
            }
        }
        out = datetime::encodeDate(y, mo, d) + datetime::encodeTime(h, mi, s);
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Sniff
// ---------------------------------------------------------------------------

bool looks_like_multicolumn_series_file(const std::string& abs_path) {
    std::ifstream in(abs_path);
    if (!in.is_open()) return false;

    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        normalize_line(line, first);
        first = false;
        if (is_blank_or_comment(line)) continue;

        if (istarts_with(line, "IDs:")) return true;      // PCSWMM TSF
        if (line.find(',') != std::string::npos) return true;  // comma CSV
        if (line.find('\t') != std::string::npos) {
            // Tab-delimited: a legacy .dat data row starts with a date; a
            // TSV header row does not.
            const auto cells = cell_split(line, '\t');
            double dt = 0.0;
            return !cells.empty() && !parse_series_datetime(cells[0], dt);
        }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// `path:column` token split (one rule for gages AND timeseries)
// ---------------------------------------------------------------------------

bool split_series_file_token(std::string_view token,
                             std::string& path_out,
                             std::string& column_out) {
    path_out.assign(token);
    column_out.clear();

    const auto colon = token.rfind(':');
    if (colon == std::string_view::npos) return false;

    // Windows drive letter ("C:\dir\f.csv"): not a column separator.
    if (colon == 1 && std::isalpha(static_cast<unsigned char>(token[0])))
        return false;

    // A colon inside a directory name ("/data/Rich:2024/f.csv"): the text
    // after it is still path, not a column name.
    const auto suffix = token.substr(colon + 1);
    if (suffix.find('/') != std::string_view::npos ||
        suffix.find('\\') != std::string_view::npos)
        return false;

    path_out.assign(token.substr(0, colon));
    column_out.assign(suffix);
    return true;
}

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

bool parse_multicolumn_series_file(const std::string& abs_path,
                                   ParsedSeriesFile& out,
                                   std::vector<std::string>& errors,
                                   SeriesFileStatus* status) {
    auto fail = [&](SeriesFileStatus st, const std::string& msg) {
        if (status) *status = st;
        errors.push_back(msg);
        return false;
    };

    out = ParsedSeriesFile{};
    g_parse_count_total.fetch_add(1, std::memory_order_relaxed);

    std::ifstream in(abs_path);
    if (!in.is_open())
        return fail(SeriesFileStatus::OPEN_FAILED,
                    "cannot open series file " + abs_path);

    // --- Header ---------------------------------------------------------
    std::string line;
    bool first = true;
    bool have_header = false;
    while (std::getline(in, line)) {
        normalize_line(line, first);
        first = false;
        if (is_blank_or_comment(line)) continue;
        have_header = true;
        break;
    }
    if (!have_header)
        return fail(SeriesFileStatus::FORMAT_FAILED,
                    "no header row in series file " + abs_path);

    char delim = ',';
    if (istarts_with(line, "IDs:")) {
        // PCSWMM TSF: tab-delimited; line 1 tokens 1..N name the columns,
        // line 2 (parameter row, e.g. "Date/Time  Rainfall …") and line 3
        // (units row, e.g. "  in.  in.") carry no data — skip both.
        delim = '\t';
        out.headers = cell_split(line, delim);
        std::string skip;
        for (int i = 0; i < 2 && std::getline(in, skip); ++i) {}
    } else {
        // CSV/TSV: sniff the delimiter from the header row, counting only
        // delimiters outside quotes (see count_unquoted).
        const long tabs   = count_unquoted(line, '\t');
        const long commas = count_unquoted(line, ',');
        delim = (tabs > 0 && tabs >= commas) ? '\t' : ',';
        out.headers = cell_split(line, delim);
    }

    const std::size_t ncols = out.headers.size();
    if (ncols < 2)
        return fail(SeriesFileStatus::FORMAT_FAILED,
                    "no data columns in series file " + abs_path);

    out.columns.assign(ncols, {});
    out.col_first_date.assign(ncols, 0.0);
    out.col_last_date.assign(ncols, 0.0);
    out.col_periods_precip.assign(ncols, 0L);
    out.col_unparsed_cells.assign(ncols, 0L);

    // --- Data rows ------------------------------------------------------
    constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
    while (std::getline(in, line)) {
        normalize_line(line, false);
        if (is_blank_or_comment(line)) continue;

        const std::vector<std::string> cells = cell_split(line, delim);
        double dt = 0.0;
        if (cells.empty() || !parse_series_datetime(cells[0], dt)) {
            ++out.unparsed_rows;
            continue;
        }

        out.dates.push_back(dt);
        if (out.first_date == 0.0 || dt < out.first_date) out.first_date = dt;
        if (dt > out.last_date) out.last_date = dt;

        for (std::size_t i = 1; i < ncols; ++i) {
            double v = kNaN;
            if (i < cells.size()) {
                const std::string& cell = cells[i];
                char* endp = nullptr;
                const double parsed = std::strtod(cell.c_str(), &endp);
                if (endp != cell.c_str()) {
                    v = parsed;
                    if (out.col_first_date[i] == 0.0 || dt < out.col_first_date[i])
                        out.col_first_date[i] = dt;
                    if (dt > out.col_last_date[i]) out.col_last_date[i] = dt;
                    if (v > 0.0) ++out.col_periods_precip[i];
                } else {
                    // Includes present-but-empty cells: the per-gage scan
                    // this replaces counted them as unreadable (strtod
                    // failure) and they feed the same skipped-rows warning.
                    ++out.col_unparsed_cells[i];
                }
            }
            // A short row (cell absent for this column) stays NaN without
            // counting — the per-gage scan silently skipped those too.
            out.columns[i].push_back(v);
        }
    }

    // --- Sort rows ascending (records may be interleaved/out of order) --
    if (!std::is_sorted(out.dates.begin(), out.dates.end())) {
        std::vector<std::size_t> order(out.dates.size());
        std::iota(order.begin(), order.end(), std::size_t{0});
        std::sort(order.begin(), order.end(),
                  [&](std::size_t a, std::size_t b){ return out.dates[a] < out.dates[b]; });
        auto permute = [&](std::vector<double>& v) {
            std::vector<double> tmp(v.size());
            for (std::size_t i = 0; i < order.size(); ++i) tmp[i] = v[order[i]];
            v.swap(tmp);
        };
        permute(out.dates);
        for (std::size_t i = 1; i < ncols; ++i) permute(out.columns[i]);
    }

    if (status) *status = SeriesFileStatus::OK;
    return true;
}

// ---------------------------------------------------------------------------
// Cache
// ---------------------------------------------------------------------------

const ParsedSeriesFile* MultiColumnFileCache::get_or_parse(
        const std::string& abs_path,
        std::vector<std::string>& errors,
        SeriesFileStatus* status) {
    auto it = files_.find(abs_path);
    if (it == files_.end()) {
        Entry entry;
        parse_multicolumn_series_file(abs_path, entry.file, errors, &entry.status);
        ++parse_count_;
        it = files_.emplace(abs_path, std::move(entry)).first;
    }
    if (status) *status = it->second.status;
    return it->second.status == SeriesFileStatus::OK ? &it->second.file : nullptr;
}

long multicolumn_parse_count_total() noexcept {
    return g_parse_count_total.load(std::memory_order_relaxed);
}

} /* namespace openswmm::input */
