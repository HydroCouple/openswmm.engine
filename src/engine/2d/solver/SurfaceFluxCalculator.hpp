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
 * @param nthreads OpenMP thread count for the per-cell loop (1 = serial).
 */
void computeUnlimitedGradients(const MeshData& mesh, SurfaceStateData& state,
                                int nthreads = 1);

/**
 * @brief Apply Jawahar-Kamath slope limiter (Eq. [23]–[24]).
 *
 * Computes continuously differentiable limited gradients from the unlimited
 * gradients of a cell and its neighbours.
 *
 * @param mesh    Mesh geometry (for neighbour lookup).
 * @param state   Surface state (reads grad_hx/hy, writes grad_hx_lim/hy_lim).
 * @param epsilon Limiter epsilon (small positive, typically 1e-6).
 * @param nthreads OpenMP thread count for the per-cell loop (1 = serial).
 */
void computeLimitedGradients(const MeshData& mesh, SurfaceStateData& state,
                              double epsilon, int nthreads = 1);

/**
 * @brief Depth-limited evaporation sink rate (m/s) for one cell.
 *
 * Reuses the cubic Hermite wet/dry ramp applied to edge fluxes: the full
 * demand rate applies for depth ≥ dry_depth and shuts off smoothly (C¹) as
 * the cell dries, so evaporation can never drive a depth negative. Negative
 * rates are treated as zero (no condensation source — use rainfall).
 *
 * @param rate      Evaporation demand rate (m/s).
 * @param depth     Current cell depth (m).
 * @param dry_depth Dry-depth threshold (m).
 * @return Effective sink rate (m/s, ≥ 0).
 */
inline double evapSink(double rate, double depth, double dry_depth) noexcept {
    if (rate <= 0.0 || depth <= 0.0) return 0.0;
    if (depth >= dry_depth) return rate;
    double t = depth / dry_depth;
    return rate * t * t * (3.0 - 2.0 * t);
}

/**
 * @brief Boundary-edge flux for cell @p i across flat mesh edge slot @p idx
 *        (m³/s, inflow-positive). Exported wrapper over the file-local
 *        boundary kernel so non-DW solvers (the explicit marcher) evaluate
 *        the identical 5-type boundary math per substep.
 */
double computeBoundaryEdgeFlux(const MeshData& mesh,
                               const SurfaceStateData& state,
                               const SolverOptions2D& opts,
                               double dh_eps, int i, int idx) noexcept;

/**
 * @brief Compute the per-cell continuity residual (local mass-balance check).
 *
 * Evaluates the discrete semi-discrete balance for each cell:
 *   residual_i = (ψ_i − ψ_old_i)·A_i/dt
 *                − ( Σ_e F_e  +  (rainfall_i + coupling_flux_i
 *                                 − evapSink_i)·A_i )
 * where F_e = edge_flux[i·3+e] is the inflow-positive volumetric edge flux
 * (m³/s). A perfectly conservative step yields ~0 (first-order diagnostic,
 * not the solver's internal error). Reads old_depth, depth, edge_flux,
 * rainfall, evap_rate, coupling_flux; writes cell_continuity_err (m³/s).
 *
 * Call AFTER the solver advance, with old_depth holding the start-of-step
 * depths (i.e. after save_state() but before the next save_state()).
 *
 * @param mesh  Mesh geometry (tri_area).
 * @param state Surface state (writes cell_continuity_err).
 * @param opts  Solver options (dry_depth for the evaporation ramp).
 * @param dt    Step over which old_depth→depth evolved (s).
 */
void computeCellContinuity(const MeshData& mesh, SurfaceStateData& state,
                            const SolverOptions2D& opts, double dt);

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
