/**
 * @file ClimateFile.cpp
 * @brief Multi-format climate file reader — faithful to legacy climate.c.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "ClimateFile.hpp"
#include "../core/DateTime.hpp"
#include <cstring>
#include <cstdlib>
#include <cctype>

namespace openswmm {
namespace climate {

// ============================================================================
// Lifecycle
// ============================================================================

ClimateFileReader::~ClimateFileReader() {
    close();
}

void ClimateFileReader::close() {
    if (file_) {
        std::fclose(file_);
        file_ = nullptr;
    }
    format_ = ClimateFileFormat::UNKNOWN;
    buf_year_ = buf_month_ = -1;
    has_saved_line_ = false;
}

bool ClimateFileReader::open(const std::string& path, double /*start_oa_date*/,
                              int unit_system) {
    close();
    unit_system_ = unit_system;

    file_ = std::fopen(path.c_str(), "r");
    if (!file_) return false;

    // Read first line for format detection
    char line[MAX_LINE + 1];
    if (!std::fgets(line, MAX_LINE, file_)) {
        close();
        return false;
    }

    format_ = detectFormat(line);
    if (format_ == ClimateFileFormat::UNKNOWN) {
        close();
        return false;
    }

    // For GHCND, the first line is the header — parse field positions
    if (format_ == ClimateFileFormat::GHCND) {
        // Header already parsed by isGhcndFormat; do not save as data line
    } else {
        // For other formats, the first line IS data — save it
        std::strncpy(saved_line_, line, MAX_LINE);
        saved_line_[MAX_LINE] = '\0';
        has_saved_line_ = true;
    }

    return true;
}

// ============================================================================
// Format detection (matching legacy getFileFormat)
// ============================================================================

ClimateFileFormat ClimateFileReader::detectFormat(const char* line) {
    // 1. TD3200: starts with "DLY" and has "9999" at positions 23-26
    if (std::strlen(line) > 26) {
        if (line[0] == 'D' && line[1] == 'L' && line[2] == 'Y' &&
            line[23] == '9' && line[24] == '9' && line[25] == '9' && line[26] == '9') {
            return ClimateFileFormat::TD3200;
        }
    }

    // 2. DLY0204: line >= 233 chars, positions 13-15 are param code 1, 2, or 151
    if (std::strlen(line) >= 233) {
        char param[4] = {};
        std::strncpy(param, &line[13], 3);
        param[3] = '\0';
        int p = std::atoi(param);
        if (p == 1 || p == 2 || p == 151) {
            return ClimateFileFormat::DLY0204;
        }
    }

    // 3. USER_PREPARED: "StationID YYYY MM DD value..."
    {
        char sta[80]; int y, m, d; char s[80];
        if (std::sscanf(line, "%79s %d %d %d %79s", sta, &y, &m, &d, s) == 5) {
            return ClimateFileFormat::USER_PREPARED;
        }
    }

    // 4. GHCND: header contains "DATE" label
    if (isGhcndFormat(line)) {
        return ClimateFileFormat::GHCND;
    }

    return ClimateFileFormat::UNKNOWN;
}

bool ClimateFileReader::isGhcndFormat(const char* line) {
    const char* ptr = std::strstr(line, "DATE");
    if (!ptr) return false;
    date_field_pos_ = static_cast<int>(ptr - line);

    // Reset field positions
    for (int i = 0; i < 4; ++i) field_pos_[i] = -1;

    ptr = std::strstr(line, "TMIN");
    if (ptr) field_pos_[TMIN] = static_cast<int>(ptr - line);

    ptr = std::strstr(line, "TMAX");
    if (ptr) field_pos_[TMAX] = static_cast<int>(ptr - line);

    ptr = std::strstr(line, "EVAP");
    if (ptr) field_pos_[EVAP] = static_cast<int>(ptr - line);

    // Wind: try WDMV first, then AWND
    ptr = std::strstr(line, "WDMV");
    if (ptr) {
        field_pos_[WIND] = static_cast<int>(ptr - line);
        wind_type_ = WDMV;
    } else {
        ptr = std::strstr(line, "AWND");
        if (ptr) {
            field_pos_[WIND] = static_cast<int>(ptr - line);
            wind_type_ = AWND;
        }
    }

    // Detect temperature units from column labels (e.g., "TMIN_C10")
    ptr = std::strstr(line, "C10");
    if (ptr) temp_units_ = DEG_C10;
    else {
        // Default for GHCND is tenths of degrees Celsius
        temp_units_ = DEG_C10;
    }

    return true;
}

// ============================================================================
// OADate (days since 12/30/1899) conversion
// ============================================================================

void ClimateFileReader::oaDateToYMD(double oa_date, int& y, int& m, int& d) {
    datetime::decodeDate(oa_date, y, m, d);
}

// ============================================================================
// Month buffering
// ============================================================================

void ClimateFileReader::clearBuffer() {
    for (int v = 0; v < 4; ++v)
        for (int d = 0; d < 32; ++d)
            file_data_[v][d] = MISSING;
}

bool ClimateFileReader::bufferMonth(int year, int month) {
    if (!file_) return false;
    clearBuffer();
    buf_year_  = year;
    buf_month_ = month;

    // Parse lines until we hit a different month or EOF
    char line[MAX_LINE + 1];

    auto processLine = [&](const char* ln) {
        switch (format_) {
            case ClimateFileFormat::USER_PREPARED: parseUserLine(ln);  break;
            case ClimateFileFormat::GHCND:         parseGhcndLine(ln); break;
            case ClimateFileFormat::TD3200:        parseTD3200Line(ln); break;
            case ClimateFileFormat::DLY0204:       parseDLY0204Line(ln); break;
            default: break;
        }
    };

    // Process saved line from previous read-ahead
    if (has_saved_line_) {
        processLine(saved_line_);
        has_saved_line_ = false;
    }

    // Read remaining lines for this month
    while (std::fgets(line, MAX_LINE, file_)) {
        // Remove trailing newline
        size_t len = std::strlen(line);
        if (len > 0 && line[len - 1] == '\n') line[len - 1] = '\0';
        if (len > 1 && line[len - 2] == '\r') line[len - 2] = '\0';

        // Peek at the date to check if we've crossed into the next month
        int ly = 0, lm = 0, ld = 0;
        bool got_date = false;

        if (format_ == ClimateFileFormat::USER_PREPARED) {
            char sta[80];
            if (std::sscanf(line, "%79s %d %d %d", sta, &ly, &lm, &ld) >= 4)
                got_date = true;
        } else if (format_ == ClimateFileFormat::GHCND) {
            if (static_cast<int>(std::strlen(line)) > date_field_pos_ + 8) {
                if (std::sscanf(&line[date_field_pos_], "%4d%2d%2d", &ly, &lm, &ld) == 3)
                    got_date = true;
            }
        } else if (format_ == ClimateFileFormat::TD3200) {
            if (std::strlen(line) > 22) {
                char ys[5] = {}, ms[3] = {};
                std::strncpy(ys, &line[17], 4); ys[4] = '\0';
                std::strncpy(ms, &line[21], 2); ms[2] = '\0';
                ly = std::atoi(ys); lm = std::atoi(ms);
                got_date = true;
            }
        } else if (format_ == ClimateFileFormat::DLY0204) {
            if (std::strlen(line) >= 13) {
                char ys[5] = {}, ms[3] = {};
                std::strncpy(ys, &line[7], 4); ys[4] = '\0';
                std::strncpy(ms, &line[11], 2); ms[2] = '\0';
                ly = std::atoi(ys); lm = std::atoi(ms);
                got_date = true;
            }
        }

        // If this line belongs to a future month, save it and stop
        if (got_date && (ly > year || (ly == year && lm > month))) {
            std::strncpy(saved_line_, line, MAX_LINE);
            saved_line_[MAX_LINE] = '\0';
            has_saved_line_ = true;
            break;
        }

        processLine(line);
    }

    return true;
}

// ============================================================================
// USER_PREPARED parser
// Format: StationID YYYY MM DD TMAX TMIN EVAP WIND
// ============================================================================

void ClimateFileReader::parseUserLine(const char* line) {
    char sta[80], s0[80] = {}, s1[80] = {}, s2[80] = {}, s3[80] = {};
    int y, m, d;

    if (std::sscanf(line, "%79s %d %d %d %79s %79s %79s %79s",
                    sta, &y, &m, &d, s0, s1, s2, s3) < 4) return;
    if (d < 1 || d > 31) return;

    // TMAX
    if (s0[0] != '\0' && s0[0] != '*') {
        double v = std::atof(s0);
        // User-prepared files: temps in user units, convert if SI
        if (unit_system_ == 1) v = v * 9.0 / 5.0 + 32.0;  // °C → °F
        file_data_[TMAX][d] = v;
    }
    // TMIN
    if (s1[0] != '\0' && s1[0] != '*') {
        double v = std::atof(s1);
        if (unit_system_ == 1) v = v * 9.0 / 5.0 + 32.0;
        file_data_[TMIN][d] = v;
    }
    // EVAP
    if (s2[0] != '\0' && s2[0] != '*') {
        file_data_[EVAP][d] = std::atof(s2);
    }
    // WIND
    if (s3[0] != '\0' && s3[0] != '*') {
        file_data_[WIND][d] = std::atof(s3);
    }
}

// ============================================================================
// GHCND parser
// Date: YYYYMMDD at date_field_pos_; data fields at field_pos_[i], 8 chars wide
// ============================================================================

void ClimateFileReader::parseGhcndLine(const char* line) {
    int len = static_cast<int>(std::strlen(line));
    int y = 0, m = 0, d = 0;

    if (len <= date_field_pos_ + 8) return;
    if (std::sscanf(&line[date_field_pos_], "%4d%2d%2d", &y, &m, &d) != 3) return;
    if (d < 1 || d > 31) return;

    for (int i = 0; i < 4; ++i) {
        if (field_pos_[i] < 0 || field_pos_[i] + 8 > len) continue;

        double v;
        if (std::sscanf(&line[field_pos_[i]], "%8lf", &v) != 1) continue;

        // Convert based on variable type and units
        if (i == TMIN || i == TMAX) {
            if (temp_units_ == DEG_C10)
                v = v / 10.0 * 9.0 / 5.0 + 32.0;  // tenths °C → °F
            else if (temp_units_ == DEG_C)
                v = v * 9.0 / 5.0 + 32.0;           // °C → °F
            // DEG_F: no conversion
        }
        else if (i == EVAP) {
            if (temp_units_ == DEG_C10)
                v /= 10.0;  // tenths mm → mm
            if (unit_system_ == 0)
                v /= MM_PER_INCH;  // mm → inches for US
        }
        else if (i == WIND) {
            if (wind_type_ == WDMV) {
                // WDMV: km/day in GHCND → convert to mph
                v = v / 10.0;           // tenths km → km (GHCND stores tenths)
                v = v * 0.621371 / 24.0; // km/day → mph
            } else {
                // AWND: tenths m/s → mph
                v = v / 10.0 * 2.23694;  // tenths m/s → m/s → mph
            }
        }

        file_data_[i][d] = v;
    }
}

// ============================================================================
// TD3200 parser
// Positions: 0-2="DLY", 11-14=param, 17-20=year, 21-22=month
// Data at 30+, each day=12 chars: 2(day) + 2(?) + 1(sign) + 5(value) + 1(?) + 1(flag)
// ============================================================================

void ClimateFileReader::parseTD3200Line(const char* line) {
    int len = static_cast<int>(std::strlen(line));
    if (len < 30) return;

    // Identify parameter
    char param[5] = {};
    std::strncpy(param, &line[11], 4); param[4] = '\0';
    // Trim trailing spaces
    for (int i = 3; i >= 0; --i) {
        if (param[i] == ' ') param[i] = '\0';
        else break;
    }

    int var = -1;
    if (std::strcmp(param, "TMIN") == 0) var = TMIN;
    else if (std::strcmp(param, "TMAX") == 0) var = TMAX;
    else if (std::strcmp(param, "EVAP") == 0) var = EVAP;
    else if (std::strcmp(param, "WDMV") == 0) { var = WIND; wind_type_ = WDMV; }
    else if (std::strcmp(param, "AWND") == 0) { var = WIND; wind_type_ = AWND; }
    else return;

    // Number of values
    char nstr[4] = {};
    std::strncpy(nstr, &line[27], 3); nstr[3] = '\0';
    int nvals = std::atoi(nstr);

    for (int j = 0; j < nvals; ++j) {
        int k = 30 + j * 12;
        if (k + 12 > len) break;

        // Day of month (2 chars)
        char ds[3] = {};
        std::strncpy(ds, &line[k], 2); ds[2] = '\0';
        int d = std::atoi(ds);
        if (d < 1 || d > 31) continue;

        // Sign + value (1 + 5 chars)
        char sign = line[k + 4];
        char vstr[6] = {};
        std::strncpy(vstr, &line[k + 5], 5); vstr[5] = '\0';

        // Quality flag
        char flag = line[k + 11];
        if (std::strcmp(vstr, "99999") == 0) continue;   // missing
        if (flag != '0' && flag != '1' && flag != ' ') continue;  // bad quality

        double v = std::atof(vstr);
        if (sign == '-') v = -v;

        // Unit conversions
        if (var == TMIN || var == TMAX) {
            // Already in °F for TD3200
        }
        else if (var == EVAP) {
            v /= 100.0;  // hundredths of inches → inches
            if (unit_system_ == 1) v *= MM_PER_INCH;  // → mm for SI
        }
        else if (var == WIND) {
            v /= 24.0;   // miles/day → mph
        }

        file_data_[var][d] = v;
    }
}

// ============================================================================
// DLY0204 parser
// Positions: 7-10=year, 11-12=month, 13-15=param code
// Data at 16: 31 x 7-char blocks (sign + 5-digit value + flag)
// ============================================================================

void ClimateFileReader::parseDLY0204Line(const char* line) {
    int len = static_cast<int>(std::strlen(line));
    if (len < 233) return;

    // Parameter code
    char pstr[4] = {};
    std::strncpy(pstr, &line[13], 3); pstr[3] = '\0';
    int pcode = std::atoi(pstr);

    int var;
    if (pcode == 1)        var = TMAX;
    else if (pcode == 2)   var = TMIN;
    else if (pcode == 151) var = EVAP;
    else return;  // skip other parameters

    int k = 16;
    for (int d = 1; d <= 31; ++d) {
        char sign = line[k];
        char vstr[6] = {};
        std::strncpy(vstr, &line[k + 1], 5); vstr[5] = '\0';
        // char flag = line[k + 6];  // quality flag (not used for filtering here)
        k += 7;

        // Skip missing values
        if (std::strcmp(vstr, "99999") == 0 || std::strcmp(vstr, "     ") == 0)
            continue;

        double v = std::atof(vstr);
        if (sign == '-') v = -v;

        // Unit conversions
        if (var == TMIN || var == TMAX) {
            // Tenths of degrees Celsius → °F
            v = v / 10.0 * 9.0 / 5.0 + 32.0;
        }
        else if (var == EVAP) {
            v /= 10.0;  // tenths mm → mm
            if (unit_system_ == 0) v /= MM_PER_INCH;  // → inches for US
        }

        file_data_[var][d] = v;
    }
}

// ============================================================================
// Public lookup
// ============================================================================

bool ClimateFileReader::getRecord(double oa_date, DailyClimateRecord& rec) {
    int y, m, d;
    oaDateToYMD(oa_date, y, m, d);

    // Buffer the month if needed
    if (y != buf_year_ || m != buf_month_) {
        if (!bufferMonth(y, m)) return false;
    }

    if (d < 1 || d > 31) return false;

    rec.tmin = (file_data_[TMIN][d] > MISSING + 1.0) ? file_data_[TMIN][d] : NAN;
    rec.tmax = (file_data_[TMAX][d] > MISSING + 1.0) ? file_data_[TMAX][d] : NAN;
    rec.evap = (file_data_[EVAP][d] > MISSING + 1.0) ? file_data_[EVAP][d] : NAN;
    rec.wind = (file_data_[WIND][d] > MISSING + 1.0) ? file_data_[WIND][d] : NAN;

    return true;
}

} // namespace climate
} // namespace openswmm
