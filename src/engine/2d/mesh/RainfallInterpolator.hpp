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
 * @file      RainfallInterpolator.hpp
 * @brief     Static per-cell weights mapping raingage rainfall onto the 2D mesh.
 *
 * @details Precomputes, once, the weights that distribute the located
 *          raingages' rainfall onto every 2D cell centroid, then applies them
 *          each step as a sparse weighted sum. Gage POSITIONS are constant for a
 *          run, so only the per-step gage VALUES vary — making this a one-time
 *          build() + cheap per-step apply().
 *
 *          Scheme: natural-neighbour (Laplace) weights inside the convex hull of
 *          the located gages; inverse-distance weighting (power 2) for cells
 *          outside the hull. Laplace weights reproduce a linear rainfall field
 *          exactly inside the hull, with no polygon-area integration (the weight
 *          for neighbour g is the length of the shared Voronoi facet divided by
 *          the distance to g). The Delaunay triangulation of the gage sites is
 *          built with a small self-contained Bowyer–Watson routine — gage counts
 *          are tiny (typically < 30), so there is no external geometry dependency.
 *
 * @see SurfaceRouter2D::initialize / updateRainfall, SolverOptions2D::RainfallMode
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_RAINFALL_INTERPOLATOR_HPP
#define OPENSWMM_ENGINE_2D_RAINFALL_INTERPOLATOR_HPP

#include <vector>

namespace openswmm::twoD {

/**
 * @brief Builds and applies static rainfall-interpolation weights for the mesh.
 *
 * Degenerate fallbacks handled by build():
 * - 0 located gages         → ready()==false (caller uses the SYSTEM mean).
 * - 1 located gage          → that gage everywhere.
 * - 2 located / collinear   → inverse-distance weighting everywhere.
 * - ≥3 non-collinear        → Laplace inside hull, IDW outside.
 */
class RainfallInterpolator {
public:
    enum class Method { NaturalNeighbour, NearestNeighbour };
    /// How build() weighted one cell (values match swmm_2d_get_rainfall_weights).
    enum class CellMethod : signed char {
        NaturalNeighbour = 0, ///< Laplace weights inside the gage hull.
        InverseDistance = 1,  ///< IDW: outside the hull, 2 sites, or degenerate.
        Nearest = 2           ///< One gage, weight 1 (nearest mode or 1 site).
    };
    struct Diagnostics {
        int unlocated = 0;
        int invalid = 0;
        int duplicates = 0;
        int idwCells = 0;
    };
    /**
     * @brief Borrow diagnostics from the most recent interpolation build.
     * @return Const reference owned by this interpolator; invalid after object destruction and
     * updated/replaced by the next build(). No allocation. Serialize reads with build().
     */
    const Diagnostics& diagnostics() const noexcept { return diagnostics_; }
    /**
     * @brief Precompute per-cell interpolation weights.
     *
     * @param[in] cx,cy      Mesh cell centroids (SI metres), size = n_triangles.
     * @param[in] gage_x,gage_y  Gage coordinates in project map units. An entry of exactly (0,0)
     * marks an un-located gage (no [SYMBOLS] row) and is excluded from the interpolation.
     * @param[in] n_gages    Number of gages (rainfall arrays are indexed 0..n_gages-1; the
     * stored gage indices are these global indices).
     * @param[in] gage_scale Factor applied to the gage coords to bring them into the SI mesh
     * frame — the same FLOW_UNITS ft→m factor initialize() applies to the mesh (1.0 for SI
     * projects).
     *
     * @param[in] method NaturalNeighbour selects Laplace with IDW fallback; NearestNeighbour
     * selects one nearest located gage.
     * @pre n_gages is nonnegative; cx/cy have identical size and finite metre coordinates.
     * gage_scale must be finite positive; missing gage coordinate entries are treated as zero.
     * @post Replaces CSR weights, method codes and diagnostic counts. Duplicate locations within
     * 1e-6 m and invalid/unlocated gages are excluded. Empty gage/cell sets leave ready() false.
     * @throws std::invalid_argument For invalid centroid dimensions/coordinates or scale.
     * @throws std::runtime_error If a generated weight is nonfinite/negative or normalization
     * fails.
     * @note Allocation may also throw. ready() is cleared before validation; a failed rebuild is
     * not transactional and the prior interpolation must not be used. All inputs are
     * borrowed/copied; building requires exclusive access.
     */
    void build(const std::vector<double>& cx, const std::vector<double>& cy,
               const std::vector<double>& gage_x, const std::vector<double>& gage_y, int n_gages,
               double gage_scale, Method method = Method::NaturalNeighbour);

    /**
     * @brief True once build() produced usable weights (≥ 1 located gage).
     *
     * @return True after a successful build produced weights for at least one cell and located
     * gage; false initially or after a failed/empty rebuild. Read-only; serialize with build().
     */
    bool ready() const noexcept { return ready_; }

    /**
     * @brief Map per-gage values onto per-cell values: out[i] = Σ w·rain[gage].
     *
     * @param[in] rain Per-gage value already in the desired output units (m/s), indexed by
     * global gage index (size ≥ n_gages).
     * @param[out] out  Filled with one value per cell; resized to n_triangles if its size does
     * not already match.
     *
     * @pre build() has initialized CSR offsets, and rain covers every retained global gage
     * index. rain and out must be different vectors; no bounds or alias checks.
     * @post Replaces every output cell with a weighted sum in rain's units; no-ready built empty
     * rows produce zero. Resizing out may allocate/throw.
     * @note Read-only interpolator operation; concurrent calls need distinct output vectors and
     * no concurrent build(). Caller retains both vectors.
     */
    void apply(const std::vector<double>& rain, std::vector<double>& out) const;

    /**
     * @brief Per-cell diagnostic: the method used and the contributing (global gage index,
     * weight) pairs of cell @p i. Requires ready() and 0 ≤ i < cells.
     *
     * @param[in] i Valid zero-based built cell index; no bounds/ready check.
     * @return Stored interpolation-method enum by value. No allocation or mutation; serialize
     * with build().
     */
    CellMethod cellMethod(int i) const { return cell_method_[static_cast<std::size_t>(i)]; }
    /**
     * @brief Copy one built cell's interpolation contributors.
     * @param[in] i Valid zero-based built cell index; requires ready() and valid CSR offsets.
     * @param[out] gages Replaced with global gage indices in the built gather order.
     * @param[out] weights Replaced with matching normalized dimensionless weights, summing to
     * one.
     * @note Output vectors are caller-owned and resized, potentially allocating. A failure after
     * assigning gages may leave weights unchanged; no transactional guarantee. Interpolator
     * remains read-only; serialize with build().
     */
    void cellWeights(int i, std::vector<int>& gages, std::vector<double>& weights) const;

private:
    Diagnostics diagnostics_;
    std::vector<CellMethod> cell_method_;
    bool ready_ = false;
    int nt_ = 0;

    // CSR weight store: cell i owns weights [w_ptr_[i], w_ptr_[i+1]); each slot
    // pairs a global gage index (w_gage_) with a normalized weight (w_val_,
    // sums to 1 per cell).
    std::vector<int> w_ptr_;
    std::vector<int> w_gage_;
    std::vector<double> w_val_;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_RAINFALL_INTERPOLATOR_HPP
