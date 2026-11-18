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
 * @file      SurfaceFluxCalculator.hpp
 * @brief     Edge flux computation, gradient calculation, and slope limiting.
 *
 * @details Implements the second-order finite volume flux computation:
 *          - Unlimited gradients via Green-Gauss (Eq. [25]–[26])
 *          - Slope limiter (Jawahar-Kamath, Eq. [23]–[24])
 *          - Edge flux with upwind selection (Eq. [15a], [22])
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §3, §10
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_FLUX_CALCULATOR_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_FLUX_CALCULATOR_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"

namespace openswmm::twoD {

/**
 * @brief Compute unlimited gradients for all triangles via Green-Gauss theorem.
 *
 * For each triangle, the gradient is the area-weighted average of edge contributions: ∇h_i =
 * (1/A_i) Σ_j h_edge_j * n_j * ξ_j
 *
 * @param mesh  Mesh geometry.
 * @param state Surface state (reads head[], writes grad_hx[], grad_hy[]).
 * @param nthreads OpenMP thread count for the per-cell loop (1 = serial).
 *
 * @note Mixed triangles/quadrilaterals are supported. Head is m, edge lengths m and cell area
 * m2, so gradients are dimensionless. All state/geometry arrays must already span the mesh
 * cell/slot counts, and each nonnegative neighbor must be valid. Areas <= 1e-30 produce zero
 * gradients. Boundary heads extrapolate the cell head rather than reading boundary stage
 * signals. Positive nthreads requests that OpenMP thread count when enabled; exclusive output
 * arrays and stable input heads/geometry are required. No allocation, shape validation or
 * synchronization is provided.
 */
void computeUnlimitedGradients(const MeshData& mesh, SurfaceStateData& state, int nthreads = 1);

/**
 * @brief Apply Jawahar-Kamath slope limiter (Eq. [23]–[24]).
 *
 * Computes continuously differentiable limited gradients from the unlimited gradients of a cell
 * and its neighbours.
 *
 * @param mesh    Mesh geometry (for neighbour lookup).
 * @param state   Surface state (reads grad_hx/hy, writes grad_hx_lim/hy_lim).
 * @param epsilon Limiter epsilon (small positive, typically 1e-6).
 * @param nthreads OpenMP thread count for the per-cell loop (1 = serial).
 *
 * @note Uses the cell plus three/four neighboring gradients, mirroring self at exterior edges.
 * The limiter regularization is in dimensionless gradient units; finite nonzero epsilon avoids
 * the zero-norm degeneracy, but its magnitude is squared without range/overflow validation.
 * Input unlimited gradients must already be computed for every cell and remain stable while
 * exclusive limited-gradient output arrays are written. Arrays must span the built mixed mesh;
 * positive nthreads applies when OpenMP is enabled. No allocation or bounds validation occurs.
 */
void computeLimitedGradients(const MeshData& mesh, SurfaceStateData& state, double epsilon,
                             int nthreads = 1);

/**
 * @brief Depth-limited evaporation sink rate (m/s) for one cell.
 *
 * Reuses the cubic Hermite wet/dry ramp applied to edge fluxes: the full demand rate applies for
 * depth ≥ dry_depth and shuts off smoothly (C¹) as the cell dries, reducing the instantaneous
 * sink near dryness. Negative rates are treated as zero (no condensation source — use rainfall).
 *
 * @param rate      Evaporation demand rate (m/s).
 * @param depth     Current cell depth (m).
 * @param dry_depth Dry-depth threshold (m).
 * @return Effective sink rate (m/s, ≥ 0).
 *
 * @note Finite rate/depth and positive dry_depth are the intended physical inputs. This function
 * computes only a rate ramp; it has no timestep or available-volume argument and cannot itself
 * bound an explicit step's removed volume. Budget limiting or volume flooring belongs to the
 * caller. A nonpositive rate/depth returns zero; no general NaN/threshold validation occurs.
 */
inline double evapSink(double rate, double depth, double dry_depth) noexcept {
    if (rate <= 0.0 || depth <= 0.0)
        return 0.0;
    if (depth >= dry_depth)
        return rate;
    double t = depth / dry_depth;
    return rate * t * t * (3.0 - 2.0 * t);
}

/**
 * @brief Depth-limited infiltration sink rate (m/s) for one cell (plan §5.5).
 *
 * Identical contract to evapSink: the held per-cell rate (D-I1, SurfaceStateData::infil_rate)
 * applies in full for depth ≥ dry_depth and shuts off through the same cubic Hermite wet/dry
 * ramp as the cell dries. Negative rates are treated as zero (infiltration is a loss only;
 * exfiltration is not a v1 destination — D-I4).
 *
 * D-I2 sink ordering: evaporation is taken FIRST with its semantics unchanged, and infiltration
 * is then applied against what is left, so a surface-only project (infil_rate all zero) is
 * bitwise unchanged. Both sinks ramp to zero together as the cell dries, and every call site
 * feeds the sum into a volume update that floors at zero, so the pair cannot leave a cell with
 * negative water.
 *
 * @param rate      Held infiltration rate (m/s).
 * @param depth     Current cell depth (m).
 * @param dry_depth Dry-depth threshold (m).
 * @return Effective sink rate (m/s, ≥ 0).
 *
 * @note This is only an instantaneous rate ramp, with the same physical preconditions and lack
 * of timestep/donor-volume limiting as evapSink(). Stable finite inputs and caller-managed water
 * budgets are required; the scalar calculation changes no model state.
 */
inline double infilSink(double rate, double depth, double dry_depth) noexcept {
    if (rate <= 0.0 || depth <= 0.0)
        return 0.0;
    if (depth >= dry_depth)
        return rate;
    double t = depth / dry_depth;
    return rate * t * t * (3.0 - 2.0 * t);
}

/**
 * @brief Boundary-edge flux for cell @p i across flat mesh edge slot @p idx (m³/s,
 * inflow-positive). Exported wrapper over the file-local boundary kernel for host boundary
 * queries. Explicit inertial marchers may use their own prognostic stage-boundary update
 * instead.
 *
 * @param[in] mesh Built stable SI mesh; geometry/connectivity arrays span cell i and slot idx.
 * @param[in] state Stable surface state with cell depth/head and optional borrowed boundary
 * arrays covering idx. The boundary values must already have their per-step signal resolution
 * applied.
 * @param[in] opts Validated face-depth reconstruction options.
 * @param[in] dh_eps Head-difference square-root regularization m; nonpositive disables it.
 * @param[in] i Valid cell index.
 * @param[in] idx Valid padded local-edge slot belonging to i, normally an exterior edge.
 * @return Inflow-positive volume rate m3/s. WALL, absent boundary data and unknown boundary kind
 * return zero. Specified/rating flow is outward per metre and changes sign after multiplying
 * length. Normal slope is signed; zero slope blocks flow. Stage uses diffusive Manning
 * conductance with regularized head difference and optional VFR face depth, rather than a
 * prognostic inertial update.
 * @note No index, array-shape or general finite validation occurs, no signal is sampled and no
 * water budget is applied. Allocation-free, noexcept and read-only with stable borrowed arrays.
 */
double computeBoundaryEdgeFlux(const MeshData& mesh, const SurfaceStateData& state,
                               const SolverOptions2D& opts, double dh_eps, int i, int idx) noexcept;

/**
 * @brief Compute the per-cell continuity residual (local mass-balance check).
 *
 * Evaluates the discrete semi-discrete balance for each cell: residual_i = (volume_i −
 * old_volume_i)/dt − ( Σ_e F_e  +  (rainfall_i + coupling_flux_i − evapSink_i − infilSink_i)·A_i
 * ) where F_e = edge_flux[MeshData::slot(i,e)] is the inflow-positive volumetric edge flux
 * (m³/s). A perfectly conservative step yields ~0 (first-order diagnostic, not the solver's
 * internal error). Reads old_volume, volume, depth, edge_flux, rainfall, evap_rate, infil_rate,
 * coupling_flux; writes cell_continuity_err (m³/s).
 *
 * Call AFTER the solver advance, with old_volume holding the start-of-step volumes (i.e. after
 * save_state() but before the next save_state()).
 *
 * @param mesh  Mesh geometry (tri_area).
 * @param state Surface state (writes cell_continuity_err).
 * @param opts  Solver options (dry_depth for the evaporation ramp).
 * @param dt    Step over which old_depth→depth evolved (s).
 *
 * @note Uses conserved volume/old_volume in m3, not depth times area; rainfall and evaporation
 * also pass through runtime forcing and snow ownership accessors. It walks actual cell edge
 * counts through MeshData::slot(), including quads. Nonpositive dt clears the entire error
 * vector and returns; finite positive dt is required otherwise. All source/volume/edge/output
 * arrays must be prepared and stable except the exclusively written cell_continuity_err. OpenMP
 * uses opts.num_threads, which must be positive; no allocation or shape/finite validation
 * occurs.
 */
void computeCellContinuity(const MeshData& mesh, SurfaceStateData& state,
                           const SolverOptions2D& opts, double dt);

/**
 * @brief Reconstruct diagnostic cell velocity from wetted-edge normal velocities.
 *
 * Minimizes sum A_e (n_e.u - Q_e/A_e)^2 over each cell's three/four edges,
 * where A_e is hydraulic depth times edge length and interior transmissivity.
 * Uses VFR endpoint geometry or MEAN depths, including stage-boundary closure
 * rules. Area weights avoid magnifying tiny wet sections. There is no division
 * by cell-average depth, discharge clip or speed cap. FULL_SWE output must use
 * its conserved cell momentum instead of this diagnostic.
 *
 * Dry cells get zero. Wet cells with rank-deficient normals, nonfinite input,
 * or nonzero flow across a currently dry section get paired NaNs (unavailable).
 * The relative determinant tolerance is 1e-10 * trace^2. API and file readers
 * must retain unavailable values rather than substituting a legacy velocity.
 *
 * @param mesh Prepared SI geometry, topology and interior conveyance.
 * @param state Stable depth/head/edge flow and optional boundary state; writes
 * only preallocated face_vx/face_vy. Sign follows internal edge flux.
 * @param opts Validated face reconstruction, momentum, dry depth and threads.
 * @note Allocation-free O(cells), exclusive per-cell writes, static OpenMP loop.
 */
void computeFaceVelocity(const MeshData& mesh, SurfaceStateData& state,
                         const SolverOptions2D& opts);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_FLUX_CALCULATOR_HPP
