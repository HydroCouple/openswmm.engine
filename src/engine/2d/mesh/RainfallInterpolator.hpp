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
 * @file RainfallInterpolator.hpp
 * @brief Static per-cell weights mapping raingage rainfall onto the 2D mesh.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_RAINFALL_INTERPOLATOR_HPP
#define OPENSWMM_ENGINE_2D_RAINFALL_INTERPOLATOR_HPP

#include <vector>

namespace openswmm::twoD {

/**
 * @brief Builds and applies static rainfall-interpolation weights for the mesh.
 *
 * Degenerate fallbacks handled by build():
 *   - 0 located gages         → ready()==false (caller uses the SYSTEM mean).
 *   - 1 located gage          → that gage everywhere.
 *   - 2 located / collinear   → inverse-distance weighting everywhere.
 *   - ≥3 non-collinear        → Laplace inside hull, IDW outside.
 */
class RainfallInterpolator {
public:
    /**
     * @brief Precompute per-cell interpolation weights.
     *
     * @param cx,cy      Mesh cell centroids (SI metres), size = n_triangles.
     * @param gage_x,gage_y  Gage coordinates in project map units. An entry of
     *                   exactly (0,0) marks an un-located gage (no [SYMBOLS]
     *                   row) and is excluded from the interpolation.
     * @param n_gages    Number of gages (rainfall arrays are indexed 0..n_gages-1;
     *                   the stored gage indices are these global indices).
     * @param gage_scale Factor applied to the gage coords to bring them into the
     *                   SI mesh frame — the same FLOW_UNITS ft→m factor
     *                   initialize() applies to the mesh (1.0 for SI projects).
     */
    void build(const std::vector<double>& cx, const std::vector<double>& cy,
               const std::vector<double>& gage_x, const std::vector<double>& gage_y,
               int n_gages, double gage_scale);

    /// True once build() produced usable weights (≥ 1 located gage).
    bool ready() const noexcept { return ready_; }

    /**
     * @brief Map per-gage values onto per-cell values: out[i] = Σ w·rain[gage].
     *
     * @param rain Per-gage value already in the desired output units (m/s),
     *             indexed by global gage index (size ≥ n_gages).
     * @param out  Filled with one value per cell; resized to n_triangles if
     *             its size does not already match.
     */
    void apply(const std::vector<double>& rain, std::vector<double>& out) const;

private:
    bool ready_ = false;
    int  nt_    = 0;

    // CSR weight store: cell i owns weights [w_ptr_[i], w_ptr_[i+1]); each slot
    // pairs a global gage index (w_gage_) with a normalized weight (w_val_,
    // sums to 1 per cell).
    std::vector<int>    w_ptr_;
    std::vector<int>    w_gage_;
    std::vector<double> w_val_;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_RAINFALL_INTERPOLATOR_HPP
