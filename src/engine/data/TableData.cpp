/**
 * @file TableData.cpp
 * @brief File-backed I/O, cache management, validation, and multicolumn
 *        lookup for Table/TableData.
 *
 * @details All value data uses flat row-major layout:
 *          `y[row * num_cols + col]`. This applies to in-memory storage,
 *          the sliding cache, and boundary buffers.
 *
 * @see TableData.hpp
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "TableData.hpp"
#include "../core/DateTime.hpp"
#include "../core/ErrorCodes.hpp"
#include "../core/charconv_compat.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

namespace openswmm {

// ============================================================================
// Internal helpers
// ============================================================================

namespace {

/**
 * @brief Detect whether a file line looks like a CSV header (contains
 *        non-numeric comma/tab separated tokens with at least one alpha char).
 */
bool looks_like_csv_header(const char* line) {
    bool has_delim = false;
    bool has_alpha = false;
    for (const char* p = line; *p && *p != '\n' && *p != '\r'; ++p) {
        if (*p == ',' || *p == '\t') has_delim = true;
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z')) has_alpha = true;
    }
    return has_delim && has_alpha;
}

/**
 * @brief Split a line by comma or tab, trimming whitespace from each token.
 */
std::vector<std::string> split_csv_line(const char* line) {
    std::vector<std::string> tokens;
    char delim = ',';
    if (std::strchr(line, ',') == nullptr) delim = '\t';

    const char* p = line;
    while (*p && *p != '\n' && *p != '\r') {
        while (*p == ' ') ++p;
        const char* start = p;
        while (*p && *p != delim && *p != '\n' && *p != '\r') ++p;
        const char* end = p;
        while (end > start && *(end - 1) == ' ') --end;
        tokens.emplace_back(start, end);
        if (*p == delim) ++p;
    }
    return tokens;
}

/**
 * @brief Parse a datetime from a single string token.
 *        Handles: "YYYY-MM-DD HH:MM:SS", "MM/DD/YYYY HH:MM", date-only, etc.
 * @returns Julian date as double, or -1.0 on failure.
 */
double parse_csv_datetime(const std::string& tok) {
    int month = 0, day = 0, year = 0;
    int hour = 0, minute = 0, second = 0;

    if (std::sscanf(tok.c_str(), "%d-%d-%d %d:%d:%d",
                    &year, &month, &day, &hour, &minute, &second) >= 5) {
        return datetime::encodeDate(year, month, day)
             + datetime::encodeTime(hour, minute, second);
    }
    if (std::sscanf(tok.c_str(), "%d/%d/%d %d:%d:%d",
                    &month, &day, &year, &hour, &minute, &second) >= 5) {
        return datetime::encodeDate(year, month, day)
             + datetime::encodeTime(hour, minute, second);
    }
    if (std::sscanf(tok.c_str(), "%d-%d-%d", &year, &month, &day) == 3) {
        return datetime::encodeDate(year, month, day);
    }
    if (std::sscanf(tok.c_str(), "%d/%d/%d", &month, &day, &year) == 3) {
        return datetime::encodeDate(year, month, day);
    }
    return -1.0;
}

/**
 * @brief Parse a legacy SWMM .dat line: "MM/DD/YYYY  H:MM  value [value ...]"
 * @returns Number of value columns parsed. Fills x_out and vals_out.
 */
int parse_dat_line(const char* line, double& x_out, std::vector<double>& vals_out) {
    vals_out.clear();

    char date_str[32] = {}, time_str[32] = {};
    int offset = 0;
    if (std::sscanf(line, "%31s %31s%n", date_str, time_str, &offset) < 2)
        return 0;

    int month = 0, day = 0, year = 0;
    if (std::sscanf(date_str, "%d/%d/%d", &month, &day, &year) != 3) return 0;

    int hour = 0, minute = 0, second = 0;
    if (std::sscanf(time_str, "%d:%d:%d", &hour, &minute, &second) < 2) return 0;

    x_out = datetime::encodeDate(year, month, day)
          + datetime::encodeTime(hour, minute, second);

    const char* p = line + offset;
    while (*p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (*p == '\n' || *p == '\r' || *p == '\0') break;
        double v = 0.0;
        auto [np, ec] = openswmm::from_chars_double(p, p + std::strlen(p), v);
        if (ec != std::errc{}) break;
        vals_out.push_back(v);
        p = np;
    }
    return static_cast<int>(vals_out.size());
}

/**
 * @brief Count data rows and build a sparse byte-offset index.
 *
 * @details Scans the file from data_start, counting data rows (skipping
 *          comments/blanks). Every INDEX_STRIDE rows, records the byte
 *          offset so table_load_cache() can fseek directly. Rewinds when done.
 *
 * @param fp          Open file handle.
 * @param data_start  Byte offset where data rows begin.
 * @param row_offsets Output: row_offsets[k] = byte offset of row (k * INDEX_STRIDE).
 * @returns           Total number of data rows.
 */
std::size_t count_and_index_rows(std::FILE* fp, long data_start,
                                 std::vector<long>& row_offsets) {
    std::fseek(fp, data_start, SEEK_SET);
    row_offsets.clear();
    row_offsets.push_back(data_start);  // row 0 starts at data_start

    std::size_t count = 0;
    char buf[1024];
    while (true) {
        long line_start = std::ftell(fp);
        if (!std::fgets(buf, sizeof(buf), fp)) break;
        if (buf[0] == ';' || buf[0] == '\n' || buf[0] == '\r' || buf[0] == '#')
            continue;
        ++count;
        // Record offset at every INDEX_STRIDE boundary
        if (count % Table::INDEX_STRIDE == 0) {
            row_offsets.push_back(std::ftell(fp));
        }
    }
    std::fseek(fp, data_start, SEEK_SET);
    return count;
}

/**
 * @brief Read up to max_rows data rows into flat row-major layout.
 *
 * @param fp        File positioned at the first row to read.
 * @param is_csv    true for CSV format, false for SWMM .dat format.
 * @param ncols     Number of value columns expected.
 * @param max_rows  Maximum rows to read.
 * @param x_out     Output: time values [num_rows].
 * @param y_out     Output: values [num_rows * ncols], row-major.
 * @returns         Number of rows actually read.
 */
std::size_t read_rows_flat(std::FILE* fp, bool is_csv, int ncols,
                           std::size_t max_rows,
                           std::vector<double>& x_out,
                           std::vector<double>& y_out) {
    x_out.clear();
    y_out.clear();

    char buf[2048];
    std::size_t rows_read = 0;

    while (rows_read < max_rows && std::fgets(buf, sizeof(buf), fp)) {
        if (buf[0] == ';' || buf[0] == '\n' || buf[0] == '\r' || buf[0] == '#')
            continue;

        if (is_csv) {
            auto tokens = split_csv_line(buf);
            if (tokens.empty()) continue;

            double dt = parse_csv_datetime(tokens[0]);
            if (dt < 0.0) continue;

            x_out.push_back(dt);
            for (int c = 0; c < ncols; ++c) {
                double v = 0.0;
                if (static_cast<std::size_t>(c + 1) < tokens.size()) {
                    openswmm::from_chars_double(tokens[c + 1].data(),
                                                tokens[c + 1].data() + tokens[c + 1].size(), v);
                }
                y_out.push_back(v);
            }
            ++rows_read;
        } else {
            double xv = 0.0;
            std::vector<double> vals;
            int nv = parse_dat_line(buf, xv, vals);
            if (nv == 0) continue;

            x_out.push_back(xv);
            for (int c = 0; c < ncols; ++c) {
                y_out.push_back((c < nv) ? vals[static_cast<std::size_t>(c)] : 0.0);
            }
            ++rows_read;
        }
    }
    return rows_read;
}

/**
 * @brief Detect whether column IDs are numeric ("1","2",...) or named headers.
 */
bool has_named_columns(const std::vector<std::string>& column_ids) {
    if (column_ids.empty()) return false;
    int dummy = 0;
    auto [p, ec] = std::from_chars(column_ids[0].data(),
                                   column_ids[0].data() + column_ids[0].size(), dummy);
    return (ec != std::errc{} || p != column_ids[0].data() + column_ids[0].size());
}

} // anonymous namespace

// ============================================================================
// validate_table
// ============================================================================

/**
 * @brief Format a DateTime value as "MM/DD/YYYY HH:MM:SS" for error messages.
 *        Matches legacy report_writeTseriesErrorMsg() output.
 */
static std::string format_datetime(double dt) {
    int y, m, d, h, mi, s;
    datetime::decodeDate(dt, y, m, d);
    datetime::decodeTime(dt, h, mi, s);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%02d/%02d/%04d %02d:%02d:%02d", m, d, y, h, mi, s);
    return buf;
}

/**
 * @brief Compute dx_min and check monotonicity for an x-value array.
 *
 * @param x       Array of x values.
 * @param n       Number of elements.
 * @param dx_min  Output: smallest positive interval found.
 * @param bad_idx Output: index of first violation (-1 if monotonic).
 * @returns       true if strictly non-decreasing.
 */
static bool check_monotonicity(const double* x, std::size_t n,
                               double& dx_min, int& bad_idx) {
    dx_min = 1.0e30;
    bad_idx = -1;
    for (std::size_t i = 1; i < n; ++i) {
        double dx = x[i] - x[i - 1];
        if (dx < 0.0) {
            bad_idx = static_cast<int>(i);
            return false;
        }
        if (dx > 0.0 && dx < dx_min) dx_min = dx;
    }
    return true;
}

TableValidation validate_table(Table& tbl) {
    TableValidation result;
    bool is_ts = (tbl.type == TableType::TIMESERIES);
    int seq_err = is_ts ? ERR_TIMESERIES_SEQUENCE : ERR_CURVE_SEQUENCE;

    // ---- Empty check ----
    if (tbl.empty()) {
        result.valid = false;
        result.errors.push_back(format_error(ERR_TIMESERIES_EMPTY, tbl.id));
        return result;
    }

    // ---- File-backed checks ----
    if (tbl.is_file_based) {
        if (!tbl.file_handle) {
            result.valid = false;
            result.errors.push_back(format_error(ERR_TABLE_FILE_OPEN, tbl.id));
        }

        if (tbl.first_boundary.empty()) {
            result.valid = false;
            result.errors.push_back(format_error(ERR_TABLE_FILE_READ, tbl.id));
        }

        // Boundary monotonicity
        double bdx_min = 1.0e30;
        int bad_idx = -1;
        if (!tbl.first_boundary.empty()) {
            check_monotonicity(tbl.first_boundary.x.data(),
                               tbl.first_boundary.num_rows(), bdx_min, bad_idx);
            if (bad_idx >= 0) {
                result.valid = false;
                result.errors.push_back(is_ts
                    ? format_error(seq_err, tbl.id,
                          "at " + format_datetime(tbl.first_boundary.x[bad_idx]))
                    : format_error(seq_err, tbl.id));
            }
            if (bdx_min < tbl.dx_min || tbl.dx_min == 0.0) tbl.dx_min = bdx_min;
        }
        if (!tbl.last_boundary.empty()) {
            check_monotonicity(tbl.last_boundary.x.data(),
                               tbl.last_boundary.num_rows(), bdx_min, bad_idx);
            if (bad_idx >= 0) {
                result.valid = false;
                result.errors.push_back(is_ts
                    ? format_error(seq_err, tbl.id,
                          "at " + format_datetime(tbl.last_boundary.x[bad_idx]))
                    : format_error(seq_err, tbl.id));
            }
            if (bdx_min < tbl.dx_min || tbl.dx_min == 0.0) tbl.dx_min = bdx_min;
        }

        // Cross-boundary ordering
        if (!tbl.first_boundary.empty() && !tbl.last_boundary.empty()) {
            if (tbl.first_boundary.x.back() > tbl.last_boundary.x.front()) {
                result.warnings.push_back(format_warning(WARN_BOUNDARY_OVERLAP, tbl.id));
            }
        }

        // Boundary y-size consistency
        std::size_t expected = tbl.first_boundary.num_rows() *
            static_cast<std::size_t>(tbl.first_boundary.num_cols);
        if (!tbl.first_boundary.empty() && tbl.first_boundary.y.size() != expected) {
            result.valid = false;
            result.errors.push_back(format_error(ERR_TABLE_COL_MISMATCH, tbl.id));
        }

        return result;
    }

    // ---- In-memory checks ----

    // Monotonicity + dx_min (matches legacy table_validate)
    {
        double dx_min_val = 1.0e30;
        int bad_idx = -1;
        check_monotonicity(tbl.x.data(), tbl.x.size(), dx_min_val, bad_idx);
        tbl.dx_min = dx_min_val;

        if (bad_idx >= 0) {
            result.valid = false;
            result.errors.push_back(is_ts
                ? format_error(seq_err, tbl.id,
                      "at " + format_datetime(tbl.x[bad_idx]))
                : format_error(seq_err, tbl.id));
        }
    }

    // NaN/Inf check
    for (std::size_t i = 0; i < tbl.x.size(); ++i) {
        if (!std::isfinite(tbl.x[i])) {
            result.valid = false;
            result.errors.push_back(format_error(ERR_TIMESERIES_NAN, tbl.id));
            break;
        }
    }

    // Flat y size consistency
    std::size_t expected_y = tbl.x.size() * static_cast<std::size_t>(tbl.num_cols);
    if (tbl.y.size() != expected_y) {
        result.valid = false;
        result.errors.push_back(format_error(ERR_TABLE_COL_MISMATCH, tbl.id));
    }

    // Duplicate x values (warning only)
    for (std::size_t i = 1; i < tbl.x.size(); ++i) {
        if (tbl.x[i] == tbl.x[i - 1]) {
            result.warnings.push_back(format_warning(WARN_TIMESERIES_DUPLICATE_X, tbl.id));
            break;
        }
    }

    return result;
}

// ============================================================================
// table_open_file
// ============================================================================

bool table_open_file(Table& tbl, std::size_t boundary_rows) {
    if (tbl.file_path.empty()) return false;

    std::FILE* fp = std::fopen(tbl.file_path.c_str(), "r");
    if (!fp) return false;

    tbl.file_handle = fp;
    tbl.is_file_based = true;

    // Skip UTF-8 BOM if present
    unsigned char bom[3] = {};
    if (std::fread(bom, 1, 3, fp) == 3) {
        if (!(bom[0] == 0xEF && bom[1] == 0xBB && bom[2] == 0xBF))
            std::fseek(fp, 0, SEEK_SET);
    } else {
        std::fseek(fp, 0, SEEK_SET);
    }

    // Read first non-comment, non-empty line to detect format
    char first_line[2048] = {};
    long pre_data_pos = std::ftell(fp);
    bool is_csv = false;

    while (std::fgets(first_line, sizeof(first_line), fp)) {
        if (first_line[0] == ';' || first_line[0] == '#' ||
            first_line[0] == '\n' || first_line[0] == '\r') {
            pre_data_pos = std::ftell(fp);
            continue;
        }
        break;
    }

    // Detect CSV with header vs SWMM .dat
    if (looks_like_csv_header(first_line)) {
        is_csv = true;
        auto headers = split_csv_line(first_line);

        tbl.column_ids.clear();
        tbl.column_map.clear();
        for (std::size_t i = 1; i < headers.size(); ++i) {
            int col_idx = static_cast<int>(tbl.column_ids.size());
            tbl.column_ids.push_back(headers[i]);
            tbl.column_map[headers[i]] = col_idx;
        }
        tbl.data_start_offset = std::ftell(fp);
    } else {
        // SWMM .dat format — detect column count from first data line
        double xv = 0.0;
        std::vector<double> vals;
        int ncols = parse_dat_line(first_line, xv, vals);
        if (ncols == 0) ncols = 1;

        tbl.column_ids.clear();
        tbl.column_map.clear();
        for (int c = 0; c < ncols; ++c) {
            std::string cid = std::to_string(c + 1);
            tbl.column_ids.push_back(cid);
            tbl.column_map[cid] = c;
        }
        tbl.data_start_offset = pre_data_pos;
    }

    int ncols = static_cast<int>(tbl.column_ids.size());
    if (ncols == 0) ncols = 1;
    tbl.num_cols = ncols;

    // Count total data rows and build sparse byte-offset index
    tbl.total_rows = count_and_index_rows(fp, tbl.data_start_offset, tbl.row_offsets);

    // ---- Read first boundary ----
    std::fseek(fp, tbl.data_start_offset, SEEK_SET);
    std::size_t first_n = std::min(boundary_rows, tbl.total_rows);
    tbl.first_boundary.num_cols = ncols;
    read_rows_flat(fp, is_csv, ncols, first_n,
                   tbl.first_boundary.x, tbl.first_boundary.y);

    // ---- Read last boundary ----
    if (tbl.total_rows > boundary_rows) {
        // Use row-offset index for O(1) seek to the last boundary region
        std::size_t skip = tbl.total_rows - boundary_rows;
        std::size_t chunk = skip / Table::INDEX_STRIDE;
        if (chunk < tbl.row_offsets.size()) {
            std::fseek(fp, tbl.row_offsets[chunk], SEEK_SET);
            // Skip remaining rows within the chunk
            std::size_t remaining = skip - chunk * Table::INDEX_STRIDE;
            char buf[2048];
            std::size_t skipped = 0;
            while (skipped < remaining && std::fgets(buf, sizeof(buf), fp)) {
                if (buf[0] == ';' || buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r')
                    continue;
                ++skipped;
            }
        } else {
            std::fseek(fp, tbl.data_start_offset, SEEK_SET);
            char buf[2048];
            std::size_t skipped = 0;
            while (skipped < skip && std::fgets(buf, sizeof(buf), fp)) {
                if (buf[0] == ';' || buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r')
                    continue;
                ++skipped;
            }
        }
        tbl.last_boundary.num_cols = ncols;
        read_rows_flat(fp, is_csv, ncols, boundary_rows,
                       tbl.last_boundary.x, tbl.last_boundary.y);
    } else {
        tbl.last_boundary = tbl.first_boundary;
    }

    // Set x_min / x_max from boundaries
    if (!tbl.first_boundary.empty())
        tbl.x_min = tbl.first_boundary.x.front();
    if (!tbl.last_boundary.empty())
        tbl.x_max = tbl.last_boundary.x.back();

    // Rewind to data start for future cache loads
    std::fseek(fp, tbl.data_start_offset, SEEK_SET);

    return true;
}

// ============================================================================
// table_load_cache
// ============================================================================

std::size_t table_load_cache(Table& tbl, std::size_t start_row) {
    if (!tbl.file_handle || !tbl.is_file_based) return 0;

    bool is_csv = has_named_columns(tbl.column_ids);

    int ncols = tbl.num_cols;
    if (ncols == 0) ncols = 1;

    // Seek to start_row using the row-offset index for O(1) positioning
    {
        std::size_t chunk = start_row / Table::INDEX_STRIDE;
        if (!tbl.row_offsets.empty() && chunk < tbl.row_offsets.size()) {
            std::fseek(tbl.file_handle, tbl.row_offsets[chunk], SEEK_SET);
            // Skip remaining rows within the chunk
            std::size_t remaining = start_row - chunk * Table::INDEX_STRIDE;
            char buf[2048];
            std::size_t skipped = 0;
            while (skipped < remaining && std::fgets(buf, sizeof(buf), tbl.file_handle)) {
                if (buf[0] == ';' || buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r')
                    continue;
                ++skipped;
            }
        } else {
            // Fallback: linear scan from data start
            std::fseek(tbl.file_handle, tbl.data_start_offset, SEEK_SET);
            char buf[2048];
            std::size_t skipped = 0;
            while (skipped < start_row && std::fgets(buf, sizeof(buf), tbl.file_handle)) {
                if (buf[0] == ';' || buf[0] == '#' || buf[0] == '\n' || buf[0] == '\r')
                    continue;
                ++skipped;
            }
        }
    }

    tbl.cache.num_cols = ncols;
    std::size_t rows = read_rows_flat(tbl.file_handle, is_csv, ncols,
                                      tbl.num_cache_rows,
                                      tbl.cache.x, tbl.cache.y);
    tbl.cache.file_row_start = start_row;

    return rows;
}

// ============================================================================
// Multicolumn lookup helpers (internal)
// ============================================================================

namespace {

/**
 * @brief Set up pointers for column lookup across in-memory / file-backed data.
 *
 * For file-backed tables, ensures the cache covers x_query and returns
 * pointers into the cache. For in-memory, returns pointers to x/y directly.
 *
 * @param x_data     Output: pointer to time array.
 * @param y_base     Output: pointer to start of flat y array.
 * @param stride     Output: num_cols (stride between same-column values).
 * @param n          Output: number of rows.
 * @param cursor_idx Output: cursor position (row-relative to returned arrays).
 * @returns          true if data is available.
 */
bool prepare_lookup(Table& tbl, double x_query,
                    const double*& x_data, const double*& y_base,
                    int& stride, int& n, int& cursor_idx) {

    if (tbl.is_file_based) {
        if (tbl.cache.empty() || tbl.total_rows == 0) {
            table_load_cache(tbl, 0);
        }
        if (tbl.cache.empty()) return false;

        // Check if x_query is outside cache window
        if (x_query < tbl.cache.x.front() || x_query > tbl.cache.x.back()) {
            std::size_t target_row = 0;
            if (tbl.x_max > tbl.x_min && tbl.total_rows > 1) {
                double frac = std::clamp(
                    (x_query - tbl.x_min) / (tbl.x_max - tbl.x_min), 0.0, 1.0);
                target_row = static_cast<std::size_t>(
                    frac * static_cast<double>(tbl.total_rows - 1));
            }
            std::size_t half = tbl.num_cache_rows / 2;
            std::size_t start = (target_row > half) ? target_row - half : 0;
            table_load_cache(tbl, start);
            if (tbl.cache.empty()) return false;
        }

        n      = static_cast<int>(tbl.cache.num_rows());
        stride = tbl.cache.num_cols;
        x_data = tbl.cache.x.data();
        y_base = tbl.cache.y.data();

        int row_offset = static_cast<int>(tbl.cache.file_row_start);
        cursor_idx = std::clamp(tbl.cursor.index - row_offset, 0, n - 1);
        return true;
    }

    // In-memory
    n = static_cast<int>(tbl.x.size());
    if (n == 0) return false;

    stride = tbl.num_cols;
    x_data = tbl.x.data();
    y_base = tbl.y.data();
    cursor_idx = std::clamp(tbl.cursor.index, 0, n - 1);
    return true;
}

} // anonymous namespace

// ============================================================================
// table_lookup_column — linear interpolation for a specific column
// ============================================================================

double table_lookup_column(Table& tbl, int col_idx, double x_query) {
    const double* x_data = nullptr;
    const double* y_base = nullptr;
    int stride = 1, n = 0, idx = 0;

    if (!prepare_lookup(tbl, x_query, x_data, y_base, stride, n, idx))
        return 0.0;

    if (col_idx < 0 || col_idx >= stride) col_idx = 0;
    if (n == 1) return y_base[col_idx];

    // Clamp below
    if (x_query <= x_data[0]) {
        tbl.cursor.index = tbl.is_file_based
            ? static_cast<int>(tbl.cache.file_row_start) : 0;
        tbl.cursor.direction = +1;
        return y_base[col_idx];
    }

    // Clamp above
    if (x_query >= x_data[n - 1]) {
        tbl.cursor.index = tbl.is_file_based
            ? static_cast<int>(tbl.cache.file_row_start) + n - 1 : n - 1;
        tbl.cursor.direction = -1;
        return y_base[static_cast<std::size_t>(n - 1) * stride + col_idx];
    }

    idx = std::clamp(idx, 0, n - 2);

    while (idx < n - 1 && x_data[idx + 1] < x_query) {
        ++idx;
        tbl.cursor.direction = +1;
    }
    while (idx > 0 && x_data[idx] > x_query) {
        --idx;
        tbl.cursor.direction = -1;
    }

    tbl.cursor.index = tbl.is_file_based
        ? static_cast<int>(tbl.cache.file_row_start) + idx : idx;

    const double dx = x_data[idx + 1] - x_data[idx];
    const double y0 = y_base[static_cast<std::size_t>(idx) * stride + col_idx];
    const double y1 = y_base[static_cast<std::size_t>(idx + 1) * stride + col_idx];
    if (dx <= 0.0) return y0;
    const double t = (x_query - x_data[idx]) / dx;
    return y0 + t * (y1 - y0);
}

// ============================================================================
// table_step_column — step function for a specific column
// ============================================================================

double table_step_column(Table& tbl, int col_idx, double x_query) {
    const double* x_data = nullptr;
    const double* y_base = nullptr;
    int stride = 1, n = 0, idx = 0;

    if (!prepare_lookup(tbl, x_query, x_data, y_base, stride, n, idx))
        return 0.0;

    if (col_idx < 0 || col_idx >= stride) col_idx = 0;
    if (n == 1) return y_base[col_idx];

    if (x_query < x_data[0]) {
        tbl.cursor.index = tbl.is_file_based
            ? static_cast<int>(tbl.cache.file_row_start) : 0;
        tbl.cursor.direction = +1;
        return 0.0;
    }

    if (x_query >= x_data[n - 1]) {
        tbl.cursor.index = tbl.is_file_based
            ? static_cast<int>(tbl.cache.file_row_start) + n - 1 : n - 1;
        tbl.cursor.direction = -1;
        return y_base[static_cast<std::size_t>(n - 1) * stride + col_idx];
    }

    idx = std::clamp(idx, 0, n - 2);

    while (idx < n - 1 && x_data[idx + 1] <= x_query) {
        ++idx;
        tbl.cursor.direction = +1;
    }
    while (idx > 0 && x_data[idx] > x_query) {
        --idx;
        tbl.cursor.direction = -1;
    }

    tbl.cursor.index = tbl.is_file_based
        ? static_cast<int>(tbl.cache.file_row_start) + idx : idx;

    return y_base[static_cast<std::size_t>(idx) * stride + col_idx];
}

} /* namespace openswmm */
