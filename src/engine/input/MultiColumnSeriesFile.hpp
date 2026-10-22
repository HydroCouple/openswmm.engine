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
 * @file      MultiColumnSeriesFile.hpp
 * @brief     Parse-once cache for multi-column external series files
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    std::vector<std::string> headers;

    /// Row datetimes (SWMM OADate, whole file, ascending).
    std::vector<double> dates;

    /// columns[i] (i >= 1) aligned to `dates`; columns[0] is unused/empty.
    std::vector<std::vector<double>> columns;

    /// Whole-file first/last datetime (0 when the file has no data rows).
    double first_date = 0.0, last_date = 0.0;

    // Per-column statistics over the WHOLE file (index-aligned with
    // `headers`; entry 0 unused). These preserve the "Rainfall File
    // Summary" values a per-gage scan used to produce.
    std::vector<double> col_first_date;
    std::vector<double> col_last_date;
    std::vector<long> col_periods_precip; ///< cells with value > 0
    std::vector<long> col_unparsed_cells; ///< value-parse failures

    /// Data rows whose datetime cell failed to parse (skipped entirely).
    long unparsed_rows = 0;

    /**
     * @brief Find the first non-time header matching with C-locale case folding.
     *
     * Linear scan; caller prevents concurrent mutation of the owned parsed-file headers.
     *
     * @param[in] name Borrowed exact-length header bytes; no whitespace/quote cleanup occurs here.
     * @return First matching index>=1, or -1 for no match/time-column-only match.
     */
    int find_column(std::string_view name) const;

    /**
     * @brief Select the first data column based on header shape.
     *
     * Does not require any parsed data rows or inspect column storage.
     * @return One if headers has at least two entries; -1 otherwise.
     */
    int first_data_column() const { return headers.size() > 1 ? 1 : -1; }
};

/// Why a parse failed (or that it did not).
enum class SeriesFileStatus {
    OK,           ///< parsed (possibly zero data rows)
    OPEN_FAILED,  ///< file could not be opened
    FORMAT_FAILED ///< no header / no data columns
};

/**
 * @brief Parse supported date/clock syntax into SWMM decimal days.
 *
 * Uses permissive sscanf prefixes, tolerates trailing text and missing clock components. encodeDate
 * supplies its invalid-date sentinel and encodeTime accepts extended nonnegative hours/minutes. US
 * AM/PM changes only hours 1..12; 00/13..23 remain as supplied. No timezone conversion or unit
 * scaling.
 *
 * @param[in] cell Borrowed NUL-terminated text; supports ISO numeric date or US slash date,
 *     optional clock and US AM/PM suffix.
 * @param[out] out Written on recognized date syntax; preserved if fewer than three date fields
 *     parse.
 * @return True when date field syntax parses, false otherwise; this is not independent
 *     Gregorian/clock validity validation.
 */
bool parse_series_datetime(const std::string& cell, double& out);

/**
 * @brief Parse CSV/TSV/TSF columns into owned sorted raw-value arrays.
 *
 * Increments process parse counter even on failed open. Raw numeric cells use C-locale strtod
 * prefixes without complete-token/range checks; absent/unparseable cells become NaN, present
 * unparseable cells increment diagnostics. Invalid datetime syntax skips a whole row. Sorts
 * dates/columns ascending without deduplicating equal timestamps; ties need not preserve source
 * order. Stream EOF/read failure are not separately distinguished. Allocation/path exceptions
 * propagate.
 *
 * @param[in] abs_path Borrowed filename expected resolved; opened through UTF-8/native-path helper.
 * @param[out] out Caller-owned parsed file RESET at entry; may contain a partial header/data prefix
 *     after refusal or exception.
 * @param[in,out] errors Caller-owned sink receiving appended file-level open/header/column
 *     diagnostics; not cleared.
 * @param[out] status Optional destination set to OK, OPEN_FAILED or FORMAT_FAILED on normal return.
 * @return True for a valid header with at least one data column, even with no successfully parsed
 *     data rows; false for file-level refusal.
 */
bool parse_multicolumn_series_file(const std::string& abs_path, ParsedSeriesFile& out,
                                   std::vector<std::string>& errors,
                                   SeriesFileStatus* status = nullptr);

/**
 * @brief Open a file and sniff its first nonblank/noncomment row.
 *
 * Reads until the first content row then closes the local stream. This open is separate from
 * parse/cache attempts and is not counted by multicolumn_parse_count_total(). Not a full format
 * validator.
 *
 * @param[in] abs_path Borrowed filename opened through UTF-8/native-path conversion.
 * @return True for an IDs: prefix, any comma, or tabs with a nondatetime first cell; false for open
 *     failure, empty input or no matching signature.
 */
bool looks_like_multicolumn_series_file(const std::string& abs_path);

/**
 * @brief Split the last eligible column-suffix colon from an external-file token.
 *
 * No trimming/unquoting, file lookup or encoding conversion. A true result with empty column
 * selects first-data-column conventions at consumers.
 *
 * @param[in] token Borrowed bytes; must not alias either output string because output assignment
 *     can invalidate the view.
 * @param[out] path_out Receives the whole token if no separator, otherwise the prefix; must be
 *     distinct from column_out.
 * @param[out] column_out Cleared first; receives suffix if a separator exists, including an empty
 *     suffix for a trailing colon.
 * @return True for an eligible colon, false if absent, a sole alphabetic drive colon at index one,
 *     or a suffix containing slash/backslash.
 */
bool split_series_file_token(std::string_view token, std::string& path_out,
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
     * @brief Borrow a path-keyed parsed result, caching normal failure as well as success.
     *
     * No file-change invalidation or internal synchronization. Caller shares one cache per resolve pass
     * and copies selected data before that cache dies. Exceptions during parsing/insertion propagate
     * and do not establish a normal cached result.
     *
     * @param[in] abs_path Borrowed exact-byte cache key; no canonicalization or case folding.
     *     Equivalent filesystem spellings can create separate entries.
     * @param[in,out] errors Appended file-level diagnostics only on first normal parse attempt for this
     *     exact key.
     * @param[out] status Optional cached status destination.
     * @return Pointer into owned cache for OK, nullptr for cached failure; result remains valid across
     *     map rehashes until cache destruction.
     */
    const ParsedSeriesFile* get_or_parse(const std::string& abs_path,
                                         std::vector<std::string>& errors,
                                         SeriesFileStatus* status = nullptr);

    /**
     * @brief Read this cache's normal parse-attempt count.
     *
     * Not an underlying OS-open counter; sniffing and thrown parse attempts are not included.
     * @return Number of parse calls that returned before cache insertion; failures count, cache hits do
     *     not.
     */
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
 * @brief Read the relaxed atomic process-wide parse-entry count.
 *
 * No reset or synchronization with completed parsing; use snapshots only as test diagnostics.
 * @return Cumulative calls entering parse_multicolumn_series_file(), including failed opens and
 *     attempts that later throw.
 */
long multicolumn_parse_count_total() noexcept;

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_MULTI_COLUMN_SERIES_FILE_HPP */
