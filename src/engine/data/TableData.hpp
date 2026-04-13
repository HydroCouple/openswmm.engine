/**
 * @file TableData.hpp
 * @brief Time series and rating curve data with bidirectional cursor.
 *
 * @details Provides the Table struct (a single time series or curve) and
 *          TableData (the SoA collection of all tables in the model), plus
 *          the cursor-optimized lookup function.
 *
 * ### Cursor Optimization
 *
 * The legacy SWMM table_lookup() in src/solver/table.c performs a linear scan
 * from index 0 on every call. For a simulation with many output steps and
 * long time series, this becomes O(N*T) total work where N is the number of
 * table entries and T is the number of timesteps.
 *
 * The new engine uses a bidirectional cursor that remembers where the last
 * lookup landed. For monotonically advancing simulation time (the common case),
 * each lookup costs O(1) — the cursor simply advances one or a few positions.
 * For backward seeks (e.g., restart scenarios), the cursor retreats.
 *
 * @see Legacy reference: src/solver/table.c — table_lookup()
 * @see tests/unit/test_timeseries_cursor.cpp
 * @see tests/benchmarks/bench_timeseries_lookup.cpp
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_TABLE_DATA_HPP
#define OPENSWMM_ENGINE_TABLE_DATA_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdio>
#include <cstdint>
#include <cassert>
#include <algorithm>

namespace openswmm {

// ============================================================================
// Table types (matching legacy SWMM enums.h)
// ============================================================================

/**
 * @brief Type of data stored in a Table.
 * @see Legacy reference: TableType in src/solver/enums.h
 */
enum class TableType : int {
    TIMESERIES    = 0,  ///< Rainfall, inflow, or other time-varying values
    CURVE_STORAGE = 1,  ///< Storage node volume-depth curve
    CURVE_DIVERSION = 2, ///< Diversion rating curve
    CURVE_RATING  = 3,  ///< Outfall/weir rating curve
    CURVE_SHAPE   = 4,  ///< Cross-section shape curve
    CURVE_CONTROL = 5,  ///< Control rule action curve
    CURVE_TIDAL   = 6,  ///< Tidal stage curve
    CURVE_PUMP1   = 7,  ///< Pump curve type 1 (ON/OFF depth)
    CURVE_PUMP2   = 8,  ///< Pump curve type 2 (head vs flow)
    CURVE_PUMP3   = 9,  ///< Pump curve type 3 (volume vs time)
    CURVE_PUMP4   = 10, ///< Pump curve type 4 (depth vs speed)
    CURVE_PUMP5   = 11  ///< Pump curve type 5 (head vs flow, variable speed)
};

// ============================================================================
// TableCursor
// ============================================================================

/**
 * @brief Bidirectional cursor tracking the last accessed index in a Table.
 *
 * @details The cursor maintains a hint about where the previous lookup landed.
 *          The next lookup starts from the cursor position and scans in the
 *          direction determined by comparing the query to x[cursor.index].
 *
 * This is a value type — copies are cheap and independent.
 *
 * @ingroup engine_data
 */
struct TableCursor {
    int index     =  0;  ///< Index of the last successful lookup entry.
    int direction = +1;  ///< Last seek direction: +1 = forward, -1 = backward.

    void reset() noexcept { index = 0; direction = +1; }
};

// ============================================================================
// Table
// ============================================================================

/**
 * @brief A single time series or rating curve.
 *
 * @details Stores parallel x and y arrays plus a bidirectional cursor for
 *          efficient sequential lookup. The x values must be monotonically
 *          non-decreasing.
 *
 * @ingroup engine_data
 *
 * @see Legacy reference: TTable in src/solver/objects.h
 */
/**
 * @brief Block of rows for file-backed table boundaries/cache.
 */
struct TableBlock {
    std::vector<double> x;           ///< Independent variable values
    std::vector<double> y;           ///< Dependent values (flat: row-major, num_cols per row)
    int                 num_cols = 1; ///< Number of value columns
    std::size_t         file_row_start = 0; ///< Row offset in file (cache only)

    std::size_t num_rows() const noexcept { return x.size(); }
    bool        empty()    const noexcept { return x.empty(); }
};

struct Table {
    std::string         id;      ///< Table identifier (from input file)
    TableType           type;    ///< Table type (TIMESERIES, CURVE_*, etc.)
    std::vector<double> x;       ///< Independent variable (time, depth, etc.)
    std::vector<double> y;       ///< Dependent variable (flow, volume, etc.)
    TableCursor         cursor;  ///< Bidirectional lookup cursor

    // ---- File-backed time series support ----
    bool               is_file_based = false; ///< True if data is read from external file
    std::FILE*         file_handle = nullptr;  ///< Open file handle (owned)
    std::string        file_path;              ///< Path to external data file
    TableBlock         first_boundary;         ///< First rows from file (for validation)
    TableBlock         last_boundary;          ///< Last rows from file (for validation)
    TableBlock         cache;                  ///< Sliding cache window for file lookups
    double             dx_min  = 0.0;          ///< Minimum inter-entry x spacing
    double             x_min   = 0.0;          ///< Minimum x value in file
    double             x_max   = 0.0;          ///< Maximum x value in file
    int                num_cols = 1;           ///< Number of value columns
    std::size_t        total_rows = 0;         ///< Total data rows in file
    std::size_t        num_cache_rows = 8192;  ///< Cache window size (rows)
    long               data_start_offset = 0;  ///< File offset to first data row
    std::vector<long>  row_offsets;            ///< Sparse byte-offset index into file
    std::vector<std::string> column_ids;       ///< Column identifiers
    std::unordered_map<std::string, int> column_map; ///< Column name → index

    static constexpr std::size_t INDEX_STRIDE = 4096; ///< Rows between offset index entries

    /** @brief Number of data points. */
    std::size_t size() const noexcept { return x.size(); }

    /** @brief True if the table has at least one data point. */
    bool empty() const noexcept { return x.empty(); }
};

// ============================================================================
// Cursor-optimized lookup
// ============================================================================

/**
 * @brief Look up a value in a Table using the bidirectional cursor.
 *
 * @details Performs linear interpolation between adjacent entries.
 *          Clamping behavior at boundaries:
 *          - x_query < x[0]: returns y[0]
 *          - x_query > x[last]: returns y[last]
 *          - Otherwise: linear interpolation between bracketing entries
 *
 * The cursor is updated on each call to reflect the current position.
 * For monotonically advancing queries (typical), this is O(1) amortized.
 *
 * @param tbl      Table to look up (cursor is modified in-place).
 * @param x_query  The independent variable value to look up.
 * @returns        Interpolated y value.
 *
 * @note NOT thread-safe — do not call from multiple threads on the same Table
 *       without external synchronization (each thread should own its cursor copy).
 *
 * @see Legacy reference: src/solver/table.c — table_lookup()
 */
inline double table_lookup_cursor(Table& tbl, double x_query) noexcept {
    const int n = static_cast<int>(tbl.x.size());
    if (n == 0) return 0.0;
    if (n == 1) return tbl.y[0];

    // Clamp below first entry
    if (x_query <= tbl.x[0]) {
        tbl.cursor.index     = 0;
        tbl.cursor.direction = +1;
        return tbl.y[0];
    }

    // Clamp above last entry
    if (x_query >= tbl.x[n - 1]) {
        tbl.cursor.index     = n - 1;
        tbl.cursor.direction = -1;
        return tbl.y[n - 1];
    }

    // Start from cursor position and seek in the most likely direction
    int idx = std::clamp(tbl.cursor.index, 0, n - 2);

    // Forward seek
    while (idx < n - 1 && tbl.x[idx + 1] < x_query) {
        ++idx;
        tbl.cursor.direction = +1;
    }

    // Backward seek
    while (idx > 0 && tbl.x[idx] > x_query) {
        --idx;
        tbl.cursor.direction = -1;
    }

    tbl.cursor.index = idx;

    // Linear interpolation between x[idx] and x[idx+1]
    const double dx = tbl.x[idx + 1] - tbl.x[idx];
    if (dx <= 0.0) return tbl.y[idx];  // guard against duplicate x values
    const double t = (x_query - tbl.x[idx]) / dx;
    return tbl.y[idx] + t * (tbl.y[idx + 1] - tbl.y[idx]);
}

/**
 * @brief Piecewise-constant (step function) table lookup with cursor.
 *
 * @details Returns y[idx] where x[idx] <= x_query < x[idx+1].
 *          No interpolation — the value is held constant until the next
 *          table entry. This is the correct behavior for rain time series
 *          where each entry represents the value for the following interval.
 *
 * @param tbl      Table to look up (cursor is modified in-place).
 * @param x_query  The independent variable value to look up.
 * @returns        Step-function y value (no interpolation).
 *
 * @see Legacy reference: gage.c — gage_setState() uses step-function logic
 */
inline double table_step_cursor(Table& tbl, double x_query) noexcept {
    const int n = static_cast<int>(tbl.x.size());
    if (n == 0) return 0.0;
    if (n == 1) return tbl.y[0];

    // Before first entry → 0 (no rain before first recorded value)
    if (x_query < tbl.x[0]) {
        tbl.cursor.index     = 0;
        tbl.cursor.direction = +1;
        return 0.0;
    }

    // At or past last entry → return last value.
    // The caller (Gage.cpp) handles the rain interval cutoff and returns 0
    // after entry_time + rainInterval. This allows the last entry's value to
    // be used for its full recording interval before going to zero.
    if (x_query >= tbl.x[n - 1]) {
        tbl.cursor.index     = n - 1;
        tbl.cursor.direction = -1;
        return tbl.y[n - 1];
    }

    // Seek to the interval containing x_query: x[idx] <= x_query < x[idx+1]
    int idx = std::clamp(tbl.cursor.index, 0, n - 2);

    while (idx < n - 1 && tbl.x[idx + 1] <= x_query) {
        ++idx;
        tbl.cursor.direction = +1;
    }
    while (idx > 0 && tbl.x[idx] > x_query) {
        --idx;
        tbl.cursor.direction = -1;
    }

    tbl.cursor.index = idx;
    return tbl.y[idx];
}

// ============================================================================
// Storage volume by trapezoidal integration of area curve
// ============================================================================

/**
 * @brief Compute storage volume by trapezoidal integration of an area-vs-depth
 *        curve, matching legacy table_getStorageVolume() in table.c.
 *
 * @param tbl    Table with x = depth, y = surface area.
 * @param depth  Depth to integrate to.
 * @returns      Volume (same units as area * depth).
 */
inline double table_getStorageVolume(Table& tbl, double depth) noexcept {
    const int n = static_cast<int>(tbl.x.size());
    if (n == 0 || depth <= 0.0) return 0.0;

    double x1 = tbl.x[0];
    double a1 = tbl.y[0];

    // Target below first entry — triangular approximation
    if (depth <= x1) {
        if (x1 < 1.0e-6) return 0.0;
        return (a1 / x1) * depth * depth / 2.0;
    }

    // Traverse entries using end-area (trapezoidal) method
    double v = 0.0;
    double dx = 0.0, dy = 0.0;
    for (int i = 1; i < n; ++i) {
        double x2 = tbl.x[i];
        double a2 = tbl.y[i];
        if (x2 >= depth) {
            // Bracketed — interpolate area at target depth
            double frac = (x2 > x1) ? (depth - x1) / (x2 - x1) : 0.0;
            double a = a1 + frac * (a2 - a1);
            return v + (a1 + a) / 2.0 * (depth - x1);
        }
        dx = x2 - x1;
        dy = a2 - a1;
        v += (a1 + a2) / 2.0 * dx;
        x1 = x2;
        a1 = a2;
    }

    // Extrapolate beyond last entry
    if (dx > 1.0e-6) {
        double s = dy / dx;
        double a = a1 + s * (depth - x1);
        if (a < 0.0) {
            v -= a1 * a1 / s / 2.0;
        } else {
            v += (a1 + a) / 2.0 * (depth - x1);
        }
    }
    return v;
}

// ============================================================================
// TableData — SoA collection of all tables
// ============================================================================

/**
 * @brief SoA collection of all time series and curves in the model.
 *
 * @details Indexed by table index (integer). Lookup by name is done via
 *          the SimulationContext name-to-index hash map.
 *
 * @ingroup engine_data
 */
struct TableData {
    std::vector<Table> tables;  ///< All tables in index order

    std::size_t count() const noexcept { return tables.size(); }

    Table&       operator[](int idx)       { return tables[static_cast<std::size_t>(idx)]; }
    const Table& operator[](int idx) const { return tables[static_cast<std::size_t>(idx)]; }

    /**
     * @brief Add a new empty table with the given ID and type.
     * @returns Index of the new table.
     */
    int add(const std::string& id, TableType type) {
        tables.push_back({id, type, {}, {}, {}});
        return static_cast<int>(tables.size()) - 1;
    }

    /**
     * @brief Reset all cursors (call before re-running a simulation).
     */
    void reset_cursors() noexcept {
        for (auto& t : tables) t.cursor.reset();
    }
};

// ============================================================================
// Table validation
// ============================================================================

struct TableValidation {
    bool valid = true;
    std::vector<std::string> errors;
    std::vector<std::string> warnings;
};

TableValidation validate_table(Table& tbl);

// ============================================================================
// File-backed table API
// ============================================================================

bool table_open_file(Table& tbl, std::size_t boundary_rows = 128);
std::size_t table_load_cache(Table& tbl, std::size_t start_row);

// ============================================================================
// Multicolumn lookup API
// ============================================================================

double table_lookup_column(Table& tbl, int col_idx, double x_query);
double table_step_column(Table& tbl, int col_idx, double x_query);

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_TABLE_DATA_HPP */
