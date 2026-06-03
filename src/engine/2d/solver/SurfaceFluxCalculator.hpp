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

/**
 * @brief Compute the per-cell continuity residual (local mass-balance check).
 *
 * Evaluates the discrete semi-discrete balance for each cell:
 *   residual_i = (ψ_i − ψ_old_i)·A_i/dt
 *                − ( Σ_e F_e  +  (rainfall_i + coupling_flux_i)·A_i )
 * where F_e = edge_flux[i·3+e] is the inflow-positive volumetric edge flux
 * (m³/s). A perfectly conservative step yields ~0 (first-order diagnostic,
 * not the solver's internal error). Reads old_depth, depth, edge_flux,
 * rainfall, coupling_flux; writes cell_continuity_err (m³/s).
 *
 * Call AFTER the solver advance, with old_depth holding the start-of-step
 * depths (i.e. after save_state() but before the next save_state()).
 *
 * @param mesh  Mesh geometry (tri_area).
 * @param state Surface state (writes cell_continuity_err).
 * @param dt    Step over which old_depth→depth evolved (s).
 */
void computeCellContinuity(const MeshData& mesh, SurfaceStateData& state,
                            double dt);

/**
 * @brief Reconstruct cell-centred velocity (vx, vy) from edge fluxes (RT0).
 *
 * For each wet cell, solves the 3×2 least-squares system N·q ≈ b in closed
 * form via the normal equations (NᵀN)·q = Nᵀb, where each row of N is the
 * outward edge normal and b_e = edge_flux_e / edge_length_e is the
 * depth-integrated normal speed (m²/s). The resulting specific-discharge
 * vector is divided by cell depth to give velocity (m/s). Dry cells
 * (depth < dry_depth) get zero velocity. Mirrors the GUI RT0 reconstruction
 * (swmm2dresultslayer.cpp applyCurrentFlux_) without its scene-space Y-flip.
 *
 * @param mesh  Mesh geometry (edge normals, lengths).
 * @param state Surface state (reads edge_flux, depth; writes face_vx/face_vy).
 * @param opts  Solver options (dry_depth).
 */
void computeFaceVelocity(const MeshData& mesh, SurfaceStateData& state,
                          const SolverOptions2D& opts);

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_FLUX_CALCULATOR_HPP
