/**
 * @file ClimateFile.hpp
 * @brief Multi-format climate file reader for temperature, evaporation, wind.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
    USER_PREPARED,   ///< StationID YYYY MM DD TMAX TMIN EVAP WIND
    GHCND,           ///< NCDC Global Historical Climatology Network Daily
    TD3200,          ///< NCDC TD3200 (NWS cooperative observer)
    DLY0204          ///< Canadian DLY02/DLY04
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
    double tmin = NAN;  ///< Minimum temperature (deg F)
    double tmax = NAN;  ///< Maximum temperature (deg F)
    double evap = NAN;  ///< Pan evaporation (in/day US, mm/day SI; NAN if missing)
    double wind = NAN;  ///< Wind speed (mph; NAN if missing)
};

// ============================================================================
// ClimateFileReader
// ============================================================================

class ClimateFileReader {
public:
    ClimateFileReader() = default;
    ~ClimateFileReader();

    ClimateFileReader(const ClimateFileReader&) = delete;
    ClimateFileReader& operator=(const ClimateFileReader&) = delete;

    /// Open file and detect format. Returns false on error.
    /// @param path           File path.
    /// @param start_julian   Start date (Julian) — skip data before this date.
    /// @param unit_system    0 = US, 1 = SI.
    bool open(const std::string& path, double start_julian, int unit_system);

    /// Close file and release resources.
    void close();

    /// Get the daily climate record for a Julian date.
    /// Automatically buffers the month if needed.
    /// @param julian_date  Julian date (e.g. from DateTime).
    /// @param[out] rec     Output record.
    /// @returns true if data was found, false if date is out of range.
    bool getRecord(double julian_date, DailyClimateRecord& rec);

    /// Detected file format.
    ClimateFileFormat format() const { return format_; }

    /// True if file is open and valid.
    bool isOpen() const { return file_ != nullptr; }

private:
    static constexpr double MISSING = -1.0e10;
    static constexpr double MM_PER_INCH = 25.4;
    static constexpr int MAX_LINE = 512;

    // File state
    std::FILE*        file_   = nullptr;
    ClimateFileFormat format_ = ClimateFileFormat::UNKNOWN;
    int               unit_system_ = 0;  // 0=US, 1=SI

    // Current month buffer: [variable][day], day 0 unused, 1-31 valid
    double file_data_[4][32] = {};
    int    buf_year_  = -1;
    int    buf_month_ = -1;

    // GHCND header-derived field positions
    int  field_pos_[4]   = {-1, -1, -1, -1}; // column start for TMIN,TMAX,EVAP,WIND
    int  date_field_pos_ = 0;
    int  wind_type_      = WDMV;
    int  temp_units_     = DEG_C10;

    // Saved line from month-boundary read-ahead
    char saved_line_[MAX_LINE + 1] = {};
    bool has_saved_line_ = false;

    // Format detection
    ClimateFileFormat detectFormat(const char* first_line);
    bool isGhcndFormat(const char* line);

    // Month buffering
    bool bufferMonth(int year, int month);
    void clearBuffer();

    // Format-specific line parsers (populate file_data_ for one line)
    void parseUserLine(const char* line);
    void parseGhcndLine(const char* line);
    void parseTD3200Line(const char* line);
    void parseDLY0204Line(const char* line);

    // Helpers
    static void julianToYMD(double julian, int& y, int& m, int& d);
    double convertTemp(double raw) const;
    double convertEvap(double raw) const;
    double convertWind(double raw, int wind_type) const;
};

} // namespace climate
} // namespace openswmm

#endif // OPENSWMM_CLIMATE_FILE_HPP
