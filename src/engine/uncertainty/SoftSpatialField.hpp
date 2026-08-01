/**
 * @file SoftSpatialField.hpp
 * @brief Minimal per-member, per-node spatial coefficient field for the 1D
 *        soft-rainfall correlated-coherence path (CL-1b).
 *
 * @details The comonotone (`COHERENCE FULL`) soft-forcing path uses a single
 *          scalar coefficient `c_i` per ensemble member, shared across all
 *          space. `COHERENCE CORR_LEN` instead supplies a per-member,
 *          per-active-node coefficient field `W_i[t]` produced by
 *          `CorrelatedFieldGenerator`, so member `i` may deviate in opposite
 *          directions in different regions.
 *
 *          This is a deliberately minimal, 2D-independent analog of
 *          `openswmm::twoD::SpatialUncertaintyField` (which lives behind the
 *          `OPENSWMM_HAS_2D` guard). The 1D ROM must not depend on the 2D
 *          layer, so the shared storage contract is duplicated here in the
 *          `openswmm::uncertainty` namespace.
 *
 *          Storage is row-major: `values[i * n_cells + t]` is the coefficient
 *          for ensemble member `i` at active node `t`. Semantically the values
 *          are *deviation coefficients* (like the scalar `c_i`), so the
 *          per-node column mean over members must equal the scalar-coefficient
 *          mean `c̄` for the deviation-form invariants to hold (q50 tracks the
 *          deterministic answer).
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_SOFT_SPATIAL_FIELD_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_SOFT_SPATIAL_FIELD_HPP

#include <cstddef>
#include <vector>

namespace openswmm::uncertainty {

/**
 * @brief M×n_cells row-major per-member spatial coefficient field.
 *
 * Lifecycle:
 *   - Default-constructed: empty (scalar fallback active, `is_spatial()==false`).
 *   - After `allocate()`: `is_spatial()==true`, all cells zero-initialised.
 *   - After `clear()`: reverts to the empty (scalar) state.
 */
struct SoftSpatialField {

    int n_members = 0;  ///< Number of ensemble members.
    int n_cells   = 0;  ///< Number of spatial cells (active ROM nodes).

    /// Row-major storage: values[i * n_cells + t].
    std::vector<double> values;

    /// True once the field has been allocated (scalar fallback otherwise).
    bool is_spatial() const noexcept { return !values.empty(); }

    /// Read coefficient for member i at cell t. Undefined if !is_spatial().
    double at(int i, int t) const noexcept {
        return values[static_cast<std::size_t>(i) * static_cast<std::size_t>(n_cells)
                      + static_cast<std::size_t>(t)];
    }

    /// Write coefficient for member i at cell t. Undefined if !is_spatial().
    double& at(int i, int t) noexcept {
        return values[static_cast<std::size_t>(i) * static_cast<std::size_t>(n_cells)
                      + static_cast<std::size_t>(t)];
    }

    /// Allocate n_members × n_cells storage, all initialised to 0.0 (deviation).
    void allocate(int n_members_, int n_cells_) {
        n_members = n_members_;
        n_cells   = n_cells_;
        values.assign(static_cast<std::size_t>(n_members_) *
                      static_cast<std::size_t>(n_cells_), 0.0);
    }

    /// Clear all storage; reverts to scalar (inactive) state.
    void clear() {
        n_members = 0;
        n_cells   = 0;
        values.clear();
    }
};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_SOFT_SPATIAL_FIELD_HPP
