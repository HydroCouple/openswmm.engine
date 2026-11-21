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
 * @file      VertexReconstruction.hpp
 * @brief     Pseudo-Laplacian vertex reconstruction stencil computation.
 *
 * @details Implements Eq. [19]–[21] from Kumar et al. (2009): builds weighted
 *          interpolation stencils at each vertex from surrounding cell centres.
 *          Stencils are stored in CSR format in MeshData for efficient evaluation.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §5.2
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP
#define OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"

namespace openswmm::twoD {

/**
 * @brief Build pseudo-Laplacian reconstruction stencils for all vertices.
 *
 * For each vertex, collects all triangles sharing that vertex, computes moments (I_xx, I_yy,
 * I_xy, R_x, R_y), Lagrange multipliers (λ_x, λ_y), and weights. Stores results in
 * mesh.vert_stencil_ptr/idx/wt (CSR format).
 *
 * @param[in,out] mesh The mesh (must have topology already built).
 *
 * @pre Connectivity, XY cell centroids and vertex arrays are sized and valid in a shared
 * coordinate frame.
 * @post Replaces CSR stencil arrays. Degenerate/fully clipped stencils fall back to uniform
 * nonnegative weights; isolated vertices have empty rows. Allocates temporary incidence/weight
 * vectors and may throw.
 * @note Run after topology/geometry construction and before solver reads, with exclusive mesh
 * access. No references are retained.
 */
void buildVertexStencils(MeshData& mesh);

/**
 * @brief Reconstruct head values at vertices from cell-centred heads.
 *
 * Evaluates: h_vertex[b] = Σ_i ω_i * h_cell[stencil_idx[i]] Uses the CSR stencil built by
 * buildVertexStencils().
 *
 * @param[in] mesh  The mesh (with stencils built).
 * @param[in,out] state Surface state (reads head[], writes vert_head[]).
 * @param[in] nthreads OpenMP thread count for the per-vertex loop (1 = serial).
 *
 * @pre Head array has at least cell_count entries and vert_head is already sized to
 * vertex_count. CSR indices/offsets are valid; no extents are checked.
 * @post Writes only reconstructed vertex heads, in the same length units as cell heads; isolated
 * vertices become zero. No resizing or allocation.
 * @note Positive nthreads controls OpenMP; 1 is serial, ignored without OpenMP. Each vertex uses
 * a fixed-order gather; input mesh/head arrays must remain stable and outputs must not be
 * concurrently accessed.
 */
void reconstructVertexHeads(const MeshData& mesh, SurfaceStateData& state, int nthreads = 1);

/**
 * @brief Pseudo-Laplacian head at ONE vertex, evaluated on demand from the current cell heads.
 *
 * Same CSR stencil and weights as reconstructVertexHeads(), for callers that need a handful of
 * vertices at the current evaluation state (the coupling points inside the CVODE RHS) without
 * paying the all-vertex pass on every RHS/Jv evaluation. The all-vertex pass still runs once per
 * accepted window so every between-window consumer (output, held-exchange evaluation, outfall
 * boundaries) sees exactly the values it saw when the pass lived in the RHS.
 *
 * @param[in] mesh Borrowed valid CSR stencils; no topology/extent checks.
 * @param[in] state Borrowed cell heads in metres, stable for the entire gather.
 * @param[in] v Valid zero-based vertex index with a row in vert_stencil_ptr.
 * @return Weighted head in metres, or zero for an empty stencil. No clipping, allocation, cached
 * vertex-field updates, or retained references.
 */
inline double vertexHeadAt(const MeshData& mesh, const SurfaceStateData& state, int v) noexcept {
    const int start = mesh.vert_stencil_ptr[v];
    const int end = mesh.vert_stencil_ptr[v + 1];
    double h = 0.0;
    for (int k = start; k < end; ++k)
        h += mesh.vert_stencil_wt[k] * state.head[mesh.vert_stencil_idx[k]];
    return h;
}

/**
 * @brief Free-surface elevation η of one triangular cell from its mean depth (render/output
 * closure).
 *
 * Inverts the stage–storage relation V(η) of a cell whose bed is the plane through its three
 * vertex elevations. For a partially wet cell (η below the highest vertex) this places the free
 * surface over the wetted fraction only, instead of the flat-cell closure η = z̄ + h̄ which
 * overstates η on cells spanning a bed step. Fully wet cells reduce exactly to η = z̄ + h̄
 * (tri_cz is the vertex-elevation mean), so the closure is continuous and consistent with the
 * solver's conserved volume V = A·h̄.
 *
 * Piecewise in η with sorted vertex elevations z1 ≤ z2 ≤ z3: z1 < η ≤ z2 : h̄(η) = (η−z1)³ /
 * (3(z2−z1)(z3−z1))          (closed form) z2 < η ≤ z3 : h̄(η) = (η−z̄) + (z3−η)³ /
 * (3(z3−z1)(z3−z2))  (safeguarded Newton) η ≥ z3      : h̄(η) = η − z̄ (flat)
 *
 * @param[in] mean_depth Cell mean depth h̄ = V/A (m), ≥ 0.
 * @param[in] za,zb,zc   The cell's three vertex elevations (any order).
 * @return η (m). For mean_depth ≤ 0 returns the lowest vertex elevation.
 *
 * @note Elevations must be finite and use the same metre datum. Nonpositive or NaN mean_depth
 * returns the lowest vertex. Delegates to the exact VFR closure, including its finite-iteration
 * inverse; no solver state or input storage is retained.
 */
double cellFreeSurfaceElevation(double mean_depth, double za, double zb, double zc);

/**
 * @brief Cell-shape-aware form: triangle → cellFreeSurfaceElevation over the three vertex beds;
 * quad → the B&S 2007 two-plane closure (mesh/QuadVfr.hpp).
 *
 * @param[in] mesh Borrowed built mixed mesh with vertex beds and quad VFR arrays in metres.
 * @param[in] t Valid zero-based cell index, not checked here.
 * @param[in] mean_depth Cell-mean depth V/A, m; nonpositive or NaN selects the lowest bed.
 * @return Shape-aware exact free-surface elevation in m. No geometry/state mutation, allocation,
 * or retained storage.
 */
double cellFreeSurfaceElevationOf(const MeshData& mesh, int t, double mean_depth);

/**
 * @brief Wet-masked, depth-weighted free-surface reconstruction at vertices for RENDERING/OUTPUT
 * only (writes state.vert_depth_signed).
 *
 * For each vertex, averages the free-surface elevations η_i of incident WET cells (depth ≥
 * dry_depth) weighted by their depths, then stores the SIGNED vertex depth η_v − z_v. Dry
 * incident cells are excluded, so bed elevations of dry cells can never lift the reconstructed
 * water surface up an adverse slope / bed step (the depth-weighted mean is bounded by the
 * incident wet-cell η's — no-new-maxima). A wet cell additionally votes only where its water
 * actually reaches the corner (wetted-contact gate, η_i > z_v), so a thin film pooled at a
 * cell's base cannot drag the level at the cell's high vertex down to the film (no-new-minima
 * follows from the gate — the mean is bounded below by min η_i > z_v). Consequently the emitted
 * field is either strictly positive or the 0 no-data sentinel; readers should stay tolerant of
 * negatives from files written by older engines. A vertex with no qualifying incident cell gets
 * 0.
 *
 * Deliberately separate from reconstructVertexHeads(): vert_head is a SOLVER field
 * (gradients/limiters/fluxes and the active-set seed pass rely on dry-cell head = bed elevation)
 * and must keep its semantics.
 *
 * @param[in] mesh      The mesh (with stencil topology built; uses stencil cell lists only, not
 * the pseudo-Laplacian weights).
 * @param[in,out] state     Surface state (reads depth[], writes vert_depth_signed[]).
 * @param[in] dry_depth Wet/dry threshold (m) — cells below it are excluded.
 * @param[in] nthreads  OpenMP thread count for the per-vertex loop (1 = serial).
 * @param[in,out] eta_scratch Caller-owned scratch resized to n_triangles(); the per-cell
 * free-surface elevations are evaluated ONCE here and gathered per vertex. Passing a persistent
 * buffer keeps the closure evaluations at O(nt) instead of O(nt x incidence) and avoids a
 * per-call allocation. Bit-identical to the per-incidence evaluation it replaces (same value,
 * same gather order).
 *
 * @pre CSR cell incidence and vertex/cell arrays are valid; state.vert_depth_signed is
 * preallocated to vertex_count. dry_depth is finite nonnegative and nthreads positive.
 * @post Resizes eta_scratch to cell_count; only qualifying wet cells' scratch values are
 * meaningful. Produces positive render depths or zero no-data, without changing solver
 * head/depth.
 * @note OpenMP gathers use fixed per-vertex order. Requires stable inputs and exclusive
 * output/scratch access; resizing may allocate/throw. References are borrowed only during the
 * call.
 */
void reconstructVertexRenderDepths(const MeshData& mesh, SurfaceStateData& state, double dry_depth,
                                   int nthreads, std::vector<double>& eta_scratch);

/**
 * @brief Convenience overload for callers with no persistent scratch (tests, one-shot seeding):
 * allocates the per-cell buffer for the duration of the call.
 *
 * @param[in] mesh Borrowed valid built mesh and CSR incidence.
 * @param[in,out] state Reads cell depths and writes preallocated vertex render depths, m.
 * @param[in] dry_depth Finite nonnegative wet/dry threshold, m.
 * @param[in] nthreads Positive OpenMP worker count, 1 for serial; ignored without OpenMP.
 * @note Allocates a private cell_count-double temporary buffer, then delegates to the
 * persistent-scratch overload. Output/scratch lifetime does not escape the call; allocation may
 * throw.
 */
inline void reconstructVertexRenderDepths(const MeshData& mesh, SurfaceStateData& state,
                                          double dry_depth, int nthreads = 1) {
    std::vector<double> eta_scratch;
    reconstructVertexRenderDepths(mesh, state, dry_depth, nthreads, eta_scratch);
}

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP
