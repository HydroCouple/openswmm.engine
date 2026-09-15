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
 * @file MultiColumnSeriesFile.hpp
 * @brief Parse-once cache for multi-column external series files
 *        (CSV / TSV / PCSWMM TSF).
 *
 * @details Implements the eager parse-once design of
 *          `plans/MULTICOLUMN_SERIES_SINGLE_READ_2026-08-17.md` (§3.1):
 *          each external file referenced by any number of rain gages and/or
 *          FILE-backed [TIMESERIES] tables is read from disk exactly once
 *          per resolve pass; consumers copy their selected column out of the
 *          shared `ParsedSeriesFile` and the cache is freed at end of pass.
 *
 *          Values are stored RAW (file units, no rain-type or unit
 *          conversion) — each consumer applies its own transform on
 *          copy-out, exactly as it did when it read the file itself.
 *
 *          Format auto-detection:
 *          - First content line starting with `IDs:` → PCSWMM TSF
 *            (tab-delimited; line 1 tokens 1..N are the column names, the
 *            next two lines — parameter and units rows — are skipped;
 *            datetimes are 12-hour `MM/DD/YYYY hh:mm:ss AM/PM`).
 *          - Otherwise CSV/TSV: delimiter sniffed from the header row
 *            (tab when tabs dominate, else comma); row 1 = headers,
 *            column 0 = time.
 *
 *          Lines are read with std::getline, so there is no fixed line-width
 *          limit (replaces the 4096-byte fgets buffers that silently split
 *          wide rows — plan gap P2).
 *
 * @see src/engine/input/PostParseResolver.cpp — the two consumers
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_MULTI_COLUMN_SERIES_FILE_HPP
#define OPENSWMM_ENGINE_MULTI_COLUMN_SERIES_FILE_HPP

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openswmm::input {

/**
 * @brief One fully parsed multi-column series file (raw file-unit values).
 *
 * @details Rows are sorted ascending by datetime. A cell that was missing
 *          (short row) or unparseable holds a quiet NaN in its column so all
 *          columns stay aligned to `dates`; consumers skip NaNs on copy-out.
 */
struct ParsedSeriesFile {
    /// Header names; index 0 is the time-column label and never matches.
    std::vector<std::string>          headers;

    /// Row datetimes (SWMM OADate, whole file, ascending).
    std::vector<double>               dates;

    /// columns[i] (i >= 1) aligned to `dates`; columns[0] is unused/empty.
    std::vector<std::vector<double>>  columns;

    /// Whole-file first/last datetime (0 when the file has no data rows).
    double first_date = 0.0, last_date = 0.0;

    // Per-column statistics over the WHOLE file (index-aligned with
    // `headers`; entry 0 unused). These preserve the "Rainfall File
    // Summary" values a per-gage scan used to produce.
    std::vector<double>               col_first_date;
    std::vector<double>               col_last_date;
    std::vector<long>                 col_periods_precip;  ///< cells with value > 0
    std::vector<long>                 col_unparsed_cells;  ///< value-parse failures

    /// Data rows whose datetime cell failed to parse (skipped entirely).
    long                              unparsed_rows = 0;

    /**
     * @brief Case-insensitive column lookup by header name.
     * @returns Column index >= 1, or -1 when not found (a match on the time
     *          column 0 is as wrong as no match and also returns -1).
     */
    int find_column(std::string_view name) const;

    /// First data column (1) when one exists, else -1.
    int first_data_column() const {
        return headers.size() > 1 ? 1 : -1;
    }
};

/// Why a parse failed (or that it did not).
enum class SeriesFileStatus {
    OK,            ///< parsed (possibly zero data rows)
    OPEN_FAILED,   ///< file could not be opened
    FORMAT_FAILED  ///< no header / no data columns
};

/**
 * @brief Parse a full datetime cell.
 *
 * @details Accepts ISO-8601 `YYYY-MM-DD[ T]HH:MM[:SS]`, 24-hour US
 *          `MM/DD/YYYY[ HH:MM[:SS]]`, and 12-hour US with a trailing
 *          AM/PM token (`MM/DD/YYYY hh:mm[:ss] AM` — the TSF form).
 *          Relocated from PostParseResolver.cpp (csv_parse_datetime) per
 *          the 2026-08-17 plan; the `n<5` hour-dropping asymmetry of the
 *          US branch is fixed to mirror the ISO branch.
 *
 * @param cell  Text of the cell (already trimmed).
 * @param out   Receives the encoded SWMM OADate on success.
 * @returns true when the cell parsed as a datetime.
 */
bool parse_series_datetime(const std::string& cell, double& out);

/**
 * @brief Parse a multi-column series file (format auto-detected).
 *
 * @param abs_path  Resolved path to open.
 * @param out       Receives the parsed file on success.
 * @param errors    Receives one human-readable string per file-level
 *                  problem (cannot open, no header, no data columns).
 * @param status    Optional; receives why parsing failed (or OK).
 * @returns true on success (a well-formed file with zero data rows still
 *          succeeds — consumers decide whether emptiness is an error).
 */
bool parse_multicolumn_series_file(const std::string& abs_path,
                                   ParsedSeriesFile& out,
                                   std::vector<std::string>& errors,
                                   SeriesFileStatus* status = nullptr);

/**
 * @brief Cheap content sniff: does this file look multi-column?
 *
 * @details Reads only up to the first content line. True when it starts
 *          with `IDs:` (TSF), contains a comma (legacy .dat is whitespace
 *          delimited), or is tab-delimited with a non-datetime first cell
 *          (a TSV header row). Used by the timeseries loader to route a
 *          FILE reference without a `:column` suffix.
 */
bool looks_like_multicolumn_series_file(const std::string& abs_path);

/**
 * @brief Split a `path[:column]` FILE token into its path and column parts.
 *
 * @details ONE rule, shared by every consumer of the convention — the rain
 *          gage reader (`handlers/CatchmentHandler.cpp`) and the timeseries
 *          loader (`PostParseResolver.cpp`). They used to differ (first
 *          colon vs. last colon), which derived two different cache keys for
 *          the same file and silently defeated the single-read guarantee
 *          whenever a path contained a colon.
 *
 *          The rule: take the LAST colon in the token, then reject it as a
 *          column separator when either
 *          - it is the Windows drive-letter colon (index 1, preceded by a
 *            letter), or
 *          - the text after it contains a path separator ('/' or '\\'), i.e.
 *            the colon belongs to a directory name (legal on POSIX/macOS,
 *            e.g. `/data/Rich:2024/rain.csv`).
 *
 * @param token       The verbatim FILE token.
 * @param path_out    Receives the path (the whole token when there is no
 *                    column).
 * @param column_out  Receives the column name (empty when there is none, and
 *                    also when the token ends in a bare `:`).
 * @returns true when a column separator was found. An empty @p column_out
 *          with a true return is the `"path:"` form, which selects the first
 *          data column but still identifies the file as multi-column.
 */
bool split_series_file_token(std::string_view token,
                             std::string& path_out,
                             std::string& column_out);

/**
 * @brief Per-resolve-pass cache keyed by resolved path.
 *
 * @details One instance is shared by the rain-gage and timeseries loaders
 *          so a file referenced by both parses once (plan §5 single-read
 *          guarantee). Failures are cached too, so N consumers of a broken
 *          file trigger one open attempt.
 */
class MultiColumnFileCache {
public:
    /**
     * @brief Return the parsed file, parsing it on first request.
     * @param abs_path  Resolved path (the cache key).
     * @param errors    Receives file-level parse errors (first parse only).
     * @param status    Optional; receives the entry's status.
     * @returns Pointer into the cache (valid for the cache's lifetime), or
     *          nullptr when the file could not be parsed.
     */
    const ParsedSeriesFile* get_or_parse(const std::string& abs_path,
                                         std::vector<std::string>& errors,
                                         SeriesFileStatus* status = nullptr);

    /// Number of actual parse attempts (test hook for the single-read assertion).
    int parse_count() const noexcept { return parse_count_; }

private:
    struct Entry {
        SeriesFileStatus status = SeriesFileStatus::OPEN_FAILED;
        ParsedSeriesFile file;
    };
    std::unordered_map<std::string, Entry> files_;
    int parse_count_ = 0;
};

/**
 * @brief Process-wide count of multi-column file parses (test hook).
 *
 * @details Lets a black-box engine test assert the single-read guarantee
 *          across swmm_engine_open (capture before, compare after). Not part
 *          of the public C API.
 */
long multicolumn_parse_count_total() noexcept;

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_MULTI_COLUMN_SERIES_FILE_HPP */
