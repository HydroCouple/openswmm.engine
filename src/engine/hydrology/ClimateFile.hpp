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
 * @file      ClimateFile.hpp
 * @brief     Multi-format climate file reader for temperature, evaporation, wind.
 *
 * @details Supports four climate file formats:
 *   - USER_PREPARED: SWMM's own whitespace-delimited format
 *   - GHCND:  NCDC Global Historical Climatology Network Daily
 *   - TD3200: NCDC TD3200 (NWS cooperative observer)
 *   - DLY0204: Canadian DLY02/DLY04 fixed-format
 *
 *   The reader buffers one month at a time (matching legacy FileData[4][32]).
 *   Within a month, lookups are O(1) array access. Month transitions
 *   trigger a sequential file read of the next month's data.
 *
 * @note Legacy reference: src/legacy/engine/climate.c
 * @ingroup new_engine
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_CLIMATE_FILE_HPP
#define OPENSWMM_CLIMATE_FILE_HPP

#include <string>
#include <cstdio>
#include <cmath>

namespace openswmm {
namespace climate {

// ============================================================================
// Enums
// ============================================================================

enum class ClimateFileFormat {
    UNKNOWN,
    USER_PREPARED, ///< StationID YYYY MM DD TMAX TMIN EVAP WIND
    GHCND,         ///< NCDC Global Historical Climatology Network Daily
    TD3200,        ///< NCDC TD3200 (NWS cooperative observer)
    DLY0204        ///< Canadian DLY02/DLY04
};

/// Climate variable indices (matching legacy ClimateVarType)
enum ClimateVar { TMIN = 0, TMAX = 1, EVAP = 2, WIND = 3 };

/// Wind field type in GHCND
enum WindFieldType { WDMV = 0, AWND = 1 };

/// Temperature unit encoding in GHCND
enum TempUnits { DEG_C10 = 0, DEG_C = 1, DEG_F = 2 };

// ============================================================================
// Daily climate record (output of a lookup)
// ============================================================================

struct DailyClimateRecord {
    double tmin = NAN; ///< Minimum temperature (deg F)
    double tmax = NAN; ///< Maximum temperature (deg F)
    double evap = NAN; ///< Pan evaporation (in/day US, mm/day SI; NAN if missing)
    double wind = NAN; ///< Wind speed (mph; NAN if missing)
};

// ============================================================================
// ClimateFileReader
// ============================================================================

/** @brief Stateful forward-only daily climate-file reader with one month cache.
 * One owner serializes open/lookup/close; no concurrent queries. Request months
 * in chronological order in a compatible ordered file. No rewind/index is kept
 * for past months. Parsers are permissive and do not qualify all missing-value
 * flags/calendar dates; successful lookup can return entirely NaN fields. */
class ClimateFileReader {
public:
    /** @brief Construct a closed reader with no cached month and zero day offset.
     * Initial temperature encoding is tenths Celsius and wind encoding WDMV. No file
     * IO or format validation occurs until open(). */
    ClimateFileReader() = default;
    /** @brief Close the owned stream, ignoring fclose status.
     * Caller first ends all concurrent/borrowed use; no final diagnostic is emitted. */
    ~ClimateFileReader();

    /** @brief Disallow copying the uniquely owned file/cursor state.
     * No callable copy operation is provided. */
    ClimateFileReader(const ClimateFileReader&) = delete;
    /** @brief Disallow copy assignment of owned stream/cached cursor state.
     * @return No runtime result; this overload is deleted. */
    ClimateFileReader& operator=(const ClimateFileReader&) = delete;

    /** @brief Close any previous stream, open a file and detect its first-line format.
     * @param path UTF-8 filename opened through fopen_utf8 in text mode; no path resolution.
     * @param start_oa_date Unused compatibility argument; this method does not skip to
     * that date. setDayOffset() separately changes subsequent lookup dates.
     * @param unit_system Zero for US, one for SI. User-format temperatures use this
     * selection; returned evaporation uses inches/day or mm/day respectively.
     * @param temp_units Nonnegative override stored after format detection (0=C10,
     * 1=C,2=F); -1 retains detection/previous encoding. Only GHCND uses this encoding.
     * @return True after recognizing a format, false for open/first-read/detection failure,
     * leaving the reader closed. True does not prove data coverage or validity. Preserves
     * day_offset and some previous unit/header state across close/reopen. Non-GHCND's
     * first line is saved for parsing; GHCND consumes its header. Errors have no stored
     * message. Long lines are read in bounded chunks rather than explicitly rejected. */
    bool open(const std::string& path, double start_oa_date, int unit_system, int temp_units = -1);

    /** @brief Close the stream and invalidate cached month and saved read-ahead line.
     * Ignores fclose status. Resets format to UNKNOWN; retains day offset, numeric
     * buffer bytes and unit/header configuration for later open(). Safe when closed. */
    void close();

    /** @brief Fetch the day at a shifted serial date from the current/next month buffer.
     * @param oa_date Finite supported SWMM/OLE date in days since 1899-12-30;
     * day_offset is added before calendar decoding. Time-of-day does not select subdaily data.
     * @param[out] rec Assigned four daily fields on true: temperatures Fahrenheit,
     * evaporation inches/day (US) or mm/day (SI), wind mph. Missing values become NaN.
     * Retained on false.
     * @return False for a required buffer operation while closed or day outside [1,31];
     * true does not imply any field exists, even at EOF or for missing month data.
     * Cache fills consume the file forward without seeking. Chronological, compatible
     * requests/input are required; requesting past/skipped months is not a random
     * lookup and can associate permissively parsed rows with the requested month.
     * Read errors and malformed rows are not reported separately. */
    bool getRecord(double oa_date, DailyClimateRecord& rec);

    /** @brief Store the date shift used by subsequent lookups.
     * @param days Finite offset in days, normally a whole number. Positive reads later
     * file dates, negative earlier dates. No rounding/validation, rewind or cache reset
     * occurs; changing it must preserve the forward-month request discipline. */
    void setDayOffset(double days) { day_offset_ = days; }

    /** @brief Read the recognized first-line format without further IO.
     * @return Detected enum while open, UNKNOWN after close or failed open.
     * Recognition does not guarantee valid rows/coverage. */
    ClimateFileFormat format() const { return format_; }

    /** @brief Test ownership of a currently open stream.
     * @return True exactly when file_ is nonnull, including an exhausted/error stream;
     * this does not establish coverage or validate the current buffer. */
    bool isOpen() const { return file_ != nullptr; }

private:
    static constexpr double MISSING = -1.0e10;
    static constexpr double MM_PER_INCH = 25.4;
    static constexpr int MAX_LINE = 512;

    // File state
    std::FILE* file_ = nullptr;
    ClimateFileFormat format_ = ClimateFileFormat::UNKNOWN;
    int unit_system_ = 0; // 0=US, 1=SI

    // Current month buffer: [variable][day], day 0 unused, 1-31 valid
    double file_data_[4][32] = {};
    int buf_year_ = -1;
    int buf_month_ = -1;
    double day_offset_ = 0.0; // see setDayOffset()

    // GHCND header-derived field positions
    int field_pos_[4] = {-1, -1, -1, -1}; // column start for TMIN,TMAX,EVAP,WIND
    int date_field_pos_ = 0;
    int wind_type_ = WDMV;
    int temp_units_ = DEG_C10;

    // Saved line from month-boundary read-ahead
    char saved_line_[MAX_LINE + 1] = {};
    bool has_saved_line_ = false;

    // Format detection
    /** @brief Identify first-line format using legacy ordered heuristics.
     * @param first_line Required NUL-terminated readable line; no mutation of its bytes.
     * @return TD3200 signature first, Canadian fixed-layout code second, user-prepared
     * five-token date heuristic third, GHCND DATE header last; otherwise UNKNOWN.
     * GHCND detection mutates column/unit metadata. No complete row validation occurs. */
    ClimateFileFormat detectFormat(const char* first_line);
    /** @brief Recognize a case-sensitive DATE header and capture column offsets.
     * @param line Required NUL-terminated readable header.
     * @return False without DATE, retaining metadata. True records DATE and available
     * TMIN/TMAX/EVAP/WDMV-or-AWND offsets, resetting absent field offsets to -1.
     * WDMV takes precedence over AWND; temperature defaults to C10. No required weather
     * column set, width or duplicate-header validation is imposed. */
    bool isGhcndFormat(const char* line);

    // Month buffering
    /** @brief Clear and refill the one-month cache by consuming the owned stream.
     * @param year Requested calendar year; must follow prior file/caller progression.
     * @param month Requested month in [1,12].
     * @return False only if closed; otherwise true even for EOF, read error or no records.
     * Parses a saved read-ahead row before peeking dates, then consumes rows until the
     * first recognized future month, saved for next time. Parsers do not filter earlier
     * row year/month; rows can overwrite a day entry. No rewind, full month validation,
     * missing-value carry-forward or station selection is performed here. */
    bool bufferMonth(int year, int month);
    /** @brief Set every variable/day cell, including unused day zero, to MISSING.
     * Does not change cached year/month, stream position, format or read-ahead state. */
    void clearBuffer();

    // Format-specific line parsers (populate file_data_ for one line)
    /** @brief Parse one permissive whitespace daily row into day-indexed buffer cells.
     * @param line Required NUL-terminated StationID YYYY MM DD TMAX TMIN EVAP WIND row.
     * Only day [1,31] is checked; station/year/month are not selected here. Missing or
     * asterisk-prefixed fields retain existing cells, other numeric fields use atof
     * without strict/finite validation. SI temperatures convert C to F; US are F.
     * Evap remains project units and wind is stored directly as mph. temp_units override
     * is unused. Duplicate day fields overwrite earlier values; no status is returned. */
    void parseUserLine(const char* line);
    /** @brief Parse one fixed-column GHCND row using recognized header offsets.
     * @param line Required NUL-terminated input. Incomplete date/columns or days outside
     * [1,31] are ignored. Year/month are not compared with the cache; numeric fields use
     * permissive eight-character scanning, without quality/sentinel filtering.
     * Converts selected temperature encoding to F, evaporation from mm (tenths when C10)
     * to project units, WDMV tenths-km/day or AWND tenths-m/s to mph. Writes available
     * day cells; does not diagnose nonfinite/bad-quality data. */
    void parseGhcndLine(const char* line);
    /** @brief Decode one monthly TD3200 parameter row into buffered daily values.
     * @param line Required NUL-terminated fixed-layout text. Reads TMIN/TMAX/EVAP/WDMV/
     * AWND, bounded complete 12-byte day blocks and days [1,31]. Skips 99999 and quality
     * flags other than zero, one or space. Numeric values use atof; row month/station
     * are not checked. Temperature is already F, evaporation hundredths-inch converts
     * to project units; either wind parameter is divided by 24 to mph as implemented.
     * Malformed/unsupported rows silently retain old cells; no diagnostic is returned. */
    void parseTD3200Line(const char* line);
    /** @brief Decode a Canadian monthly fixed-layout temperature/evaporation row.
     * @param line Required NUL-terminated text at least 233 bytes. Codes 1/2/151 map to
     * TMAX/TMIN/EVAP; others are ignored. Reads thirty-one seven-byte daily blocks,
     * skipping 99999/blank values but not filtering quality flags or calendar length.
     * Temperature tenths-C converts to F; evaporation tenths-mm converts to project
     * units. Year/month are not selected here; permissive atof values overwrite cells. */
    void parseDLY0204Line(const char* line);

    // Helpers
    /** @brief Decode a supported serial day through the shared datetime utility.
     * @param oa_date Finite SWMM/OLE day in the representable calendar range.
     * @param[out] y Calendar year.
     * @param[out] m One-based month.
     * @param[out] d One-based day; time fraction does not select subdaily climate.
     * No additional validation or exception translation is provided. */
    static void oaDateToYMD(double oa_date, int& y, int& m, int& d);
    /** @brief Reserved temperature-conversion declaration without a callable definition.
     * @param raw Reserved raw temperature operand.
     * @return No runtime result is supplied: maintained reader implementations convert
     * inside individual line parsers and do not define or call this helper. */
    double convertTemp(double raw) const;
    /** @brief Reserved evaporation-conversion declaration without a callable definition.
     * @param raw Reserved raw evaporation operand.
     * @return No runtime result is supplied; line parsers perform their own format/unit
     * conversions. Calling this unimplemented helper would require a definition. */
    double convertEvap(double raw) const;
    /** @brief Reserved wind-conversion declaration without a callable definition.
     * @param raw Reserved raw wind operand.
     * @param wind_type Reserved WDMV/AWND selector.
     * @return No runtime result is supplied; actual conversion occurs in line parsers.
     * No maintained out-of-line definition or call exists for this private hook. */
    double convertWind(double raw, int wind_type) const;
};

} // namespace climate
} // namespace openswmm

#endif // OPENSWMM_CLIMATE_FILE_HPP
