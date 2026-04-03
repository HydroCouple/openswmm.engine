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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
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
 */
void reconstructVertexHeads(const MeshData& mesh, SurfaceStateData& state);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_VERTEX_RECONSTRUCTION_HPP
