/**
 * @file SurfaceTangent.hpp
 * @brief Analytic tangent (Jacobian-vector product) for the 2D diffusive-wave
 *        surface RHS — replaces SUNDIALS' finite-difference J·v.
 *
 * @details Under matrix-free SPGMR, SUNDIALS forms J·v by a difference quotient
 *          of the RHS: every Krylov iteration re-runs the ENTIRE nonlinear flux
 *          pipeline (measured: ~78% of all RHS calls on demo road_culvert are
 *          these FD J·v evaluations). The collapsed Manning diffusive-wave flux
 *          is a closed form with a 4-nonzero-per-row sparsity, so its tangent is
 *          analytic. buildSurfaceTangents() precomputes the per-edge
 *          linearization coefficients once per linear-solver setup; applyJv()
 *          then evaluates J·v as a cheap sparse mat-vec (no transcendentals, no
 *          flux recompute), matching the RHS's per-cell gather so it is race-free
 *          under OpenMP and antisymmetric (mass-conservative) by construction.
 *
 * Scope: the interior flux divergence, the y-dependent boundary edges
 * (NORMAL_FLOW / SPECIFIED_STAGE — the diagonal ∂F_bc/∂V_i), the evaporation
 * sink, and — under single-cell live coupling (Phase 3d) — the orifice exchange
 * ∂Q_k/∂V_c (its self term on diag[c] and the augmented ∫Q dt accumulator rows).
 * The orifice tangent is a local central FD of computeNodeCouplingQ about the
 * driving cell volume, assembled ONCE per linear-solve setup (a handful of Q
 * evals, «cells) so J·v stays a pure SpMV — consistent with SUNDIALS' own FD J·v
 * by construction. On the default (held) path node_coupling == null, the orifice
 * loop is skipped, and the accumulator rows are exactly zero (the mean-rate
 * coupling_flux source is constant in y).
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_TANGENT_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_TANGENT_HPP

#include "../data/MeshData.hpp"
#include "../data/SurfaceStateData.hpp"
#include "../data/SolverOptions2D.hpp"

#include <vector>

namespace openswmm::twoD {

/**
 * @brief Precomputed linearization of the surface RHS about a state.
 *
 * Per-cell×3 slot layout, mirroring state.edge_flux and the RHS per-cell gather:
 *   dfdvi[i*3+e]   = ∂(inflow to cell i across local edge e)/∂V_i
 *   dfdvnbr[i*3+e] = ∂(inflow to cell i across local edge e)/∂V_nbr
 * (dfdvnbr is 0 for a boundary edge; its diagonal part folds into dfdvi.)
 *   diag[i]        = ∂(source term for cell i)/∂V_i  (evaporation sink)
 * The rows telescope antisymmetrically because each incident cell computes its
 * own slot from the shared centroid-Δη and upwind depth — exactly as the RHS
 * flux does.
 */
struct SurfaceTangents {
    std::vector<double> dfdvi;    ///< size n_triangles*3
    std::vector<double> dfdvnbr;  ///< size n_triangles*3
    std::vector<double> diag;     ///< size n_triangles

    // Live single-cell orifice-coupling linearization (Phase 3d). One entry per
    // live coupling point k (single-cell centroid points only): the point's
    // driving cell c = coupling_cell[k] and dQ_k/dV_c = coupling_dQdV[k]. The
    // cell-row self term −dQ/dV_c is already folded into diag[c] by
    // buildSurfaceTangents; these two vectors additionally drive the augmented
    // ∫Q dt accumulator rows in applyTangentJv (Jv[nt+k] = dQ/dV_c · v[c]).
    // Empty on the default (held) path — accumulator rows are then zero.
    std::vector<int>    coupling_cell;   ///< size nc (or empty); −1 = no tangent
    std::vector<double> coupling_dQdV;   ///< size nc (or empty)

    int                 nt = 0;

    void resize(int n) {
        nt = n;
        dfdvi.assign(static_cast<std::size_t>(n) * 3, 0.0);
        dfdvnbr.assign(static_cast<std::size_t>(n) * 3, 0.0);
        diag.assign(static_cast<std::size_t>(n), 0.0);
    }
};

/**
 * @brief Build the tangent coefficients from the current (head, depth) state.
 *
 * Requires state.head and state.depth to be current for @p y (the caller
 * unpacks y via reconstructFromVolume before calling — the same unpack rhs_fn
 * does). Boundary-edge diagonal terms are obtained by a local central
 * difference of boundaryEdgeFlux (a handful of edges — negligible, and exactly
 * consistent with the flux the RHS applies).
 */
void buildSurfaceTangents(const MeshData& mesh, SurfaceStateData& state,
                          const SolverOptions2D& opts, SurfaceTangents& tang,
                          const double* y = nullptr);

/**
 * @brief Apply J·v using the precomputed tangents.
 *
 * @param nc   number of augmented (accumulator) rows after the nt cell rows;
 *             their J·v is zero on the held path (y-independent forcing).
 * Writes Jv[0..nt) (cell rows) and Jv[nt..nt+nc) (zeroed).
 */
void applyTangentJv(const MeshData& mesh, const SolverOptions2D& opts,
                    const SurfaceTangents& tang, int nc,
                    const double* v, double* Jv);

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SURFACE_TANGENT_HPP
