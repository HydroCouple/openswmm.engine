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
 * Scope: the interior + boundary flux divergence and the evaporation sink — i.e.
 * the whole DEFAULT (held-coupling) RHS. The augmented accumulator rows are
 * y-independent on that path (the deviation forcing is a function of time only),
 * so their J·v row is exactly zero. The live-RHS orifice path (opt-in) has
 * y-dependent accumulator rows; buildSurfaceTangents does not linearize it, so
 * the caller keeps finite-difference J·v whenever state.node_coupling != null
 * (Phase 3 adds the coupling tangent for the preconditioner).
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
                          const SolverOptions2D& opts, SurfaceTangents& tang);

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
