/**
 * @file VertexReconstruction.hpp
 * @brief Pseudo-Laplacian vertex reconstruction stencil computation.
 *
 * @details Implements Eq. [19]–[21] from Kumar et al. (2009): builds weighted
 *          interpolation stencils at each vertex from surrounding cell centres.
 *          Stencils are stored in CSR format in MeshData for efficient evaluation.
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §5.2
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP
#define OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"

namespace openswmm::twoD {

/**
 * @brief Build pseudo-Laplacian reconstruction stencils for all vertices.
 *
 * For each vertex, collects all triangles sharing that vertex, computes
 * moments (I_xx, I_yy, I_xy, R_x, R_y), Lagrange multipliers (λ_x, λ_y),
 * and weights. Stores results in mesh.vert_stencil_ptr/idx/wt (CSR format).
 *
 * @param mesh The mesh (must have topology already built).
 */
void buildVertexStencils(MeshData& mesh);

/**
 * @brief Reconstruct head values at vertices from cell-centred heads.
 *
 * Evaluates: h_vertex[b] = Σ_i ω_i * h_cell[stencil_idx[i]]
 * Uses the CSR stencil built by buildVertexStencils().
 *
 * @param mesh  The mesh (with stencils built).
 * @param state Surface state (reads head[], writes vert_head[]).
 * @param nthreads OpenMP thread count for the per-vertex loop (1 = serial).
 */
void reconstructVertexHeads(const MeshData& mesh, SurfaceStateData& state,
                             int nthreads = 1);

/**
 * @brief Free-surface elevation η of one triangular cell from its mean depth
 *        (render/output closure).
 *
 * Inverts the stage–storage relation V(η) of a cell whose bed is the plane
 * through its three vertex elevations. For a partially wet cell (η below the
 * highest vertex) this places the free surface over the wetted fraction only,
 * instead of the flat-cell closure η = z̄ + h̄ which overstates η on cells
 * spanning a bed step. Fully wet cells reduce exactly to η = z̄ + h̄
 * (tri_cz is the vertex-elevation mean), so the closure is continuous and
 * consistent with the solver's conserved volume V = A·h̄.
 *
 * Piecewise in η with sorted vertex elevations z1 ≤ z2 ≤ z3:
 *   z1 < η ≤ z2 : h̄(η) = (η−z1)³ / (3(z2−z1)(z3−z1))          (closed form)
 *   z2 < η ≤ z3 : h̄(η) = (η−z̄) + (z3−η)³ / (3(z3−z1)(z3−z2))  (safeguarded Newton)
 *   η ≥ z3      : h̄(η) = η − z̄                                  (flat)
 *
 * @param mean_depth Cell mean depth h̄ = V/A (m), ≥ 0.
 * @param za,zb,zc   The cell's three vertex elevations (any order).
 * @return η (m). For mean_depth ≤ 0 returns the lowest vertex elevation.
 */
double cellFreeSurfaceElevation(double mean_depth, double za, double zb,
                                double zc);

/**
 * @brief Wet-masked, depth-weighted free-surface reconstruction at vertices
 *        for RENDERING/OUTPUT only (writes state.vert_depth_signed).
 *
 * For each vertex, averages the free-surface elevations η_i of incident WET
 * cells (depth ≥ dry_depth) weighted by their depths, then stores the SIGNED
 * vertex depth η_v − z_v. Dry incident cells are excluded, so bed elevations
 * of dry cells can never lift the reconstructed water surface up an adverse
 * slope / bed step (the depth-weighted mean is bounded by the incident wet-cell
 * η's — no-new-maxima). A vertex with no wet incident cell gets 0.
 *
 * Deliberately separate from reconstructVertexHeads(): vert_head is a SOLVER
 * field (gradients/limiters/fluxes and the active-set seed pass rely on
 * dry-cell head = bed elevation) and must keep its semantics.
 *
 * @param mesh      The mesh (with stencil topology built; uses stencil cell
 *                  lists only, not the pseudo-Laplacian weights).
 * @param state     Surface state (reads depth[], writes vert_depth_signed[]).
 * @param dry_depth Wet/dry threshold (m) — cells below it are excluded.
 * @param nthreads  OpenMP thread count for the per-vertex loop (1 = serial).
 */
void reconstructVertexRenderDepths(const MeshData& mesh, SurfaceStateData& state,
                                   double dry_depth, int nthreads = 1);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP
