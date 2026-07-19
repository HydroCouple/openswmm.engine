/**
 * @file GridFileReader.hpp
 * @brief HDF5 grid-file reader for soft-rainfall gridded input (SR-2a).
 *
 * @details Streams one time-plane at a time from an HDF5 file conforming to
 *          the layout described in SOFT_RAINFALL_DESIGN.md §3.3:
 *
 *          /               attrs: family, spread_kind, units, crs (optional)
 *          /time           (T)        float64
 *          /x, /y          (nx),(ny)  float64 grid coordinates (cell centers)
 *          /location       (T,ny,nx)  float32   — optional
 *          /spread         (T,ny,nx)  float32   — required
 *          /family_code    (ny,nx)    uint8     — only when family == "MIXED"
 *
 *          Memory contract (§4.2): only the current and next time-planes are
 *          resident at any time, mirroring the gage rainfall/next_rainfall
 *          interpolation convention.
 *
 *          This reader is a pure I/O component — it knows nothing about
 *          mapping, targets, or uncertainty propagation. It validates the
 *          schema, exposes grid dimensions and metadata, and streams planes.
 *
 * @ingroup engine_uncertainty
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_GRID_FILE_READER_HPP
#define OPENSWMM_ENGINE_GRID_FILE_READER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace openswmm {

// ============================================================================
// Enums mirroring the design-doc attributes
// ============================================================================

/// Distribution family declared in the file's root `family` attribute.
enum class GridFamily : int8_t {
    UNKNOWN  = -1,
    NORMAL   = 0,
    LOGNORMAL = 1,
    UNIFORM  = 2,
    MIXED    = 3,
};

/// Spread kind declared in the file's root `spread_kind` attribute.
enum class GridSpreadKind : int8_t {
    UNKNOWN   = -1,
    SD        = 0,   ///< Absolute standard deviation
    CV        = 1,   ///< Coefficient of variation (relative)
    HALFRANGE = 2,   ///< Uniform half-range (absolute)
};

// ============================================================================
// GridFileReader
// ============================================================================

/**
 * @brief Streaming reader for HDF5 soft-rainfall grid files.
 *
 * Open → validate schema → advance through time-planes. Only two planes
 * (current + next) are resident simultaneously for temporal interpolation.
 *
 * Usage:
 * ```cpp
 * GridFileReader reader;
 * if (!reader.open("radar.h5")) { ... error ... }
 * // reader.n_time(), reader.nx(), reader.ny(), reader.family(), etc.
 * reader.advance();  // load t=0 (current) and t=1 (next)
 * while (reader.has_current()) {
 *     const auto* loc = reader.location_now();  // may be nullptr
 *     const auto* sp  = reader.spread_now();
 *     reader.advance();
 * }
 * ```
 */
class GridFileReader {
public:
    GridFileReader();
    ~GridFileReader();

    // Non-copyable (owns HDF5 handles)
    GridFileReader(const GridFileReader&) = delete;
    GridFileReader& operator=(const GridFileReader&) = delete;

    // Movable
    GridFileReader(GridFileReader&&) noexcept;
    GridFileReader& operator=(GridFileReader&&) noexcept;

    // ------------------------------------------------------------------
    // Lifecycle
    // ------------------------------------------------------------------

    /**
     * @brief Open and validate an HDF5 grid file.
     * @param path Filesystem path to the .h5 file.
     * @return true on success; false on any error (call last_error()).
     */
    bool open(const std::string& path);

    /// Close the file and release all resources. Safe to call multiple times.
    void close();

    // ------------------------------------------------------------------
    // Schema metadata (valid after open())
    // ------------------------------------------------------------------

    int n_time()  const noexcept { return n_time_; }
    int nx()      const noexcept { return nx_; }
    int ny()      const noexcept { return ny_; }

    GridFamily     family()      const noexcept { return family_; }
    GridSpreadKind spread_kind() const noexcept { return spread_kind_; }
    const std::string& units()   const noexcept { return units_; }
    const std::string& crs()    const noexcept { return crs_; }
    bool has_location()          const noexcept { return has_location_; }
    bool has_family_code()       const noexcept { return has_family_code_; }

    /// Grid coordinate arrays (cell centers). Valid after open().
    const std::vector<double>& x_coords() const noexcept { return x_coords_; }
    const std::vector<double>& y_coords() const noexcept { return y_coords_; }

    /// Time axis values (n_time entries). Valid after open().
    const std::vector<double>& time_axis() const noexcept { return time_axis_; }

    // ------------------------------------------------------------------
    // Plane streaming
    // ------------------------------------------------------------------

    /**
     * @brief Advance to the next time step.
     *
     * On the first call, loads t=0 as current and t=1 as next (if available).
     * Subsequent calls shift next→current and load the following plane as next.
     * When the last plane is reached, has_current() returns false.
     *
     * @return true if a current plane is available after the call.
     */
    bool advance();

    /// True if a current plane is loaded.
    bool has_current() const noexcept { return cur_index_ >= 0 && cur_index_ < n_time_; }

    /// Index of the current plane (0-based), or -1 if none.
    int current_index() const noexcept { return cur_index_; }

    /// Pointer to the current location plane (ny*nx floats), or nullptr if
    /// the file has no /location dataset.
    const float* location_now() const noexcept;

    /// Pointer to the current spread plane (ny*nx floats). Never null when
    /// has_current() is true.
    const float* spread_now() const noexcept;

    /// Pointer to the next location plane (for temporal interpolation), or
    /// nullptr if no next plane or no /location dataset.
    const float* location_next() const noexcept;

    /// Pointer to the next spread plane, or nullptr if no next plane.
    const float* spread_next() const noexcept;

    /// Pointer to the current family_code plane (ny*nx uint8 values), or
    /// nullptr when the file's root family is not MIXED. Each cell's code maps
    /// to a GridFamily: 0=NORMAL, 1=LOGNORMAL, 2=UNIFORM.
    const uint8_t* family_code_now() const noexcept;

    /// Time value of the current plane.
    double time_now() const noexcept;

    /// Time value of the next plane (or time_now() if no next plane).
    double time_next() const noexcept;

    // ------------------------------------------------------------------
    // Error reporting
    // ------------------------------------------------------------------

    /// Last error message (empty if no error).
    const std::string& last_error() const noexcept { return last_error_; }

private:
    // Internal helpers
    bool validate_schema_();
    bool read_metadata_();
    bool read_axes_();
    bool read_time_axis_();
    bool read_plane_(int t, std::vector<float>& loc_buf,
                              std::vector<float>& sp_buf) const;
    bool read_family_code_plane_();

    // HDF5 file handle
    void* file_id_;  // hid_t stored as void* to avoid leaking HDF5 into header

    // Schema metadata
    int n_time_ = 0;
    int nx_     = 0;
    int ny_     = 0;
    GridFamily     family_      = GridFamily::UNKNOWN;
    GridSpreadKind spread_kind_  = GridSpreadKind::UNKNOWN;
    std::string units_;
    std::string crs_;
    bool has_location_ = false;
    bool has_family_code_ = false;

    std::vector<double> x_coords_;
    std::vector<double> y_coords_;
    std::vector<double> time_axis_;

    // Streaming state: only current + next planes resident
    int cur_index_ = -1;
    int next_index_ = -1;
    std::vector<float> loc_cur_, loc_next_;
    std::vector<float> sp_cur_, sp_next_;
    std::vector<uint8_t> family_code_;  // (ny*nx) — only when family == MIXED

    // last_error_ is mutable so const plane-reading methods can set it
    mutable std::string last_error_;
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_GRID_FILE_READER_HPP