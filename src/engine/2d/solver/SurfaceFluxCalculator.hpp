/**
 * @file SurfaceFluxCalculator.hpp
 * @brief Edge flux computation, gradient calculation, and slope limiting.
 *
 * @details Implements the second-order finite volume flux computation:
 *          - Unlimited gradients via Green-Gauss (Eq. [25]–[26])
 *          - Slope limiter (Jawahar-Kamath, Eq. [23]–[24])
 *          - Edge flux with upwind selection (Eq. [15a], [22])
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §3, §10
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
 * For each triangle, the gradient is the area-weighted average of edge contributions:
 *   ∇h_i = (1/A_i) Σ_j h_edge_j * n_j * ξ_j
 *
 * @param mesh  Mesh geometry.
 * @param state Surface state (reads head[], writes grad_hx[], grad_hy[]).
 */
void computeUnlimitedGradients(const MeshData& mesh, SurfaceStateData& state);

/**
 * @brief Apply Jawahar-Kamath slope limiter (Eq. [23]–[24]).
 *
 * Computes continuously differentiable limited gradients from the unlimited
 * gradients of a cell and its neighbours.
 *
 * @param mesh    Mesh geometry (for neighbour lookup).
 * @param state   Surface state (reads grad_hx/hy, writes grad_hx_lim/hy_lim).
 * @param epsilon Limiter epsilon (small positive, typically 1e-6).
 */
void computeLimitedGradients(const MeshData& mesh, SurfaceStateData& state,
                              double epsilon);

/**
 * @brief Compute edge fluxes for all triangles.
 *
 * For each edge, reconstructs head at the edge from the upstream cell using
 * the limited gradient, computes diffusive conductance, and evaluates the
 * normal flux. Boundary edges use zero-flux (wall) condition.
 *
 * @param mesh  Mesh geometry.
 * @param state Surface state (reads depth, head, limited gradients; writes edge_flux).
 * @param opts  Solver options (dry_depth).
 */
void computeEdgeFluxes(const MeshData& mesh, SurfaceStateData& state,
                        const SolverOptions2D& opts);

/**
 * @brief Assemble the RHS of the ODE system: dψ/dt for each triangle.
 *
 * Combines edge fluxes, rainfall, and coupling fluxes into the net rate
 * of change of depth for each cell:
 *   dψ_i/dt = (1/A_i) Σ_j F_j + rainfall_i + coupling_flux_i
 *
 * @param mesh   Mesh geometry.
 * @param state  Surface state.
 * @param ydot   Output: dψ/dt for each triangle (size = n_triangles).
 */
void assembleRHS(const MeshData& mesh, const SurfaceStateData& state,
                  double* ydot);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_FLUX_CALCULATOR_HPP
