/**
 * @file SpatialUncertaintyField.hpp
 * @brief Per-member, per-cell uncertainty multiplier field for 2D spatial uncertainty.
 *
 * @details Wraps M×n_cells row-major storage for spatially-explicit uncertainty
 *          multipliers (e.g. Manning's n or rainfall scale factors). When the
 *          field is not populated (is_spatial() == false), callers fall back to
 *          their scalar per-member design.
 *
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_SPATIAL_UNCERTAINTY_FIELD_HPP
#define OPENSWMM_ENGINE_2D_SPATIAL_UNCERTAINTY_FIELD_HPP

#ifdef OPENSWMM_HAS_2D

#include <vector>
#include <cstddef>

namespace openswmm::twoD {

/**
 * @brief M×n_cells spatial uncertainty multiplier field.
 *
 * Storage layout is row-major: `values[i * n_cells + t]` is the multiplier
 * for ensemble member `i` at triangle `t`.
 *
 * Lifecycle:
 *   - Default-constructed: empty (scalar fallback active).
 *   - After `allocate()` or `fromScalar()`: is_spatial() == true.
 *   - After `clear()`: reverts to empty state.
 */
struct SpatialUncertaintyField {

    int n_members = 0;  ///< Number of ensemble members.
    int n_cells   = 0;  ///< Number of spatial cells (triangles).

    /// Row-major storage: values[i * n_cells + t].
    std::vector<double> values;

    // -------------------------------------------------------------------------
    // Queries
    // -------------------------------------------------------------------------

    /// True if the spatial field has been allocated and populated.
    bool is_spatial() const noexcept { return !values.empty(); }

    // -------------------------------------------------------------------------
    // Accessors
    // -------------------------------------------------------------------------

    /// Read multiplier for member i at cell t. Undefined if !is_spatial().
    double at(int i, int t) const noexcept {
        return values[static_cast<std::size_t>(i) * static_cast<std::size_t>(n_cells)
                      + static_cast<std::size_t>(t)];
    }

    /// Write multiplier for member i at cell t. Undefined if !is_spatial().
    double& at(int i, int t) noexcept {
        return values[static_cast<std::size_t>(i) * static_cast<std::size_t>(n_cells)
                      + static_cast<std::size_t>(t)];
    }

    // -------------------------------------------------------------------------
    // Mutators
    // -------------------------------------------------------------------------

    /**
     * @brief Allocate n_members × n_cells storage, all initialised to 1.0.
     */
    void allocate(int n_members_, int n_cells_) {
        n_members = n_members_;
        n_cells   = n_cells_;
        values.assign(static_cast<std::size_t>(n_members_) *
                      static_cast<std::size_t>(n_cells_), 1.0);
    }

    /**
     * @brief Populate from a scalar per-member multiplier vector.
     *
     * Sets every cell for member i to `scalar_mult[i]`.  This converts a
     * scalar design to a (trivially) spatial one so that callers can use a
     * uniform code path.
     *
     * @param scalar_mult  One multiplier per member (length == n_members_).
     * @param n_cells_     Number of spatial cells.
     */
    void fromScalar(const std::vector<double>& scalar_mult, int n_cells_) {
        const int M = static_cast<int>(scalar_mult.size());
        allocate(M, n_cells_);
        for (int i = 0; i < M; ++i) {
            const double v = scalar_mult[static_cast<std::size_t>(i)];
            double* row = values.data() + static_cast<std::size_t>(i) * n_cells_;
            for (int t = 0; t < n_cells_; ++t) row[t] = v;
        }
    }

    /// Clear all storage; reverts to scalar (inactive) state.
    void clear() {
        n_members = 0;
        n_cells   = 0;
        values.clear();
    }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
#endif // OPENSWMM_ENGINE_2D_SPATIAL_UNCERTAINTY_FIELD_HPP
