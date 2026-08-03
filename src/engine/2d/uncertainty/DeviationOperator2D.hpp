/**
 * @file DeviationOperator2D.hpp
 * @brief Reduced k×k deviation operator M = Pᵀ·L_op·P for the 2D surface ROM.
 *
 * @details The surface ROM evolves member deviations δa = Pᵀ(h − h_det) on the
 *          fixed mesh eigenbasis P. This class assembles the full-mesh linear
 *          operator L_op governing d(δh)/dt = −L_op·δh and Galerkin-projects it
 *          once onto P, giving a small dense matrix M (k×k, units 1/s) that the
 *          ROM integrates exactly with a matrix exponential:
 *
 *              δa(t+Δt) = exp(−M·Δt)·δa(t) + φ₁(−M·Δt)·Δt·g
 *
 *          Every physics rung is just what is assembled into M — one code
 *          path, a dial:
 *
 *          - **diffusion, flow-aligned anisotropic**: edge conductance
 *            D_edge = D_scale·(α∥·cos²θ + α⊥·sin²θ), where θ is the angle
 *            between the edge (centroid line) and the local flow direction.
 *            α∥ = α⊥ = 1 is the isotropic baseline; the local-inertial
 *            linearization (LOCAL_INERTIAL_DEVIATION_OPERATOR.md §5) gives the
 *            starting points α∥ ≈ 0.31·K/D_scale, α⊥ ≈ 1.0·K/D_scale — Manning
 *            friction uses the flow-vector magnitude, so the linearized
 *            friction rate is 2r_f streamwise but r_f transverse.
 *          - **advection (skew)**: first-order upwind transport at celerity
 *            c⃗ = c_factor·u⃗ (c_factor = 5/3 is the Manning kinematic-wave
 *            celerity). c_factor = 0 disables. This is the term a symmetric
 *            Laplacian cannot carry: it makes deviation bands travel with the
 *            flow rather than only spread.
 *
 *          **Units convention — physical, not graph.** L_op is the
 *          finite-volume operator, i.e. every face flux is divided by the cell
 *          area, so M is in true 1/s with D_scale in m²/s. This differs from
 *          the ROM's historical diagonal path, which uses K_eff·λ with λ the
 *          eigenvalues of the *graph* Laplacian (conductance len/d, no 1/A) —
 *          a convention that silently carries an O(cell-size²) scale factor.
 *          The W3 marcher-MC harness measures both; the calibrated constants
 *          absorb any residual scale.
 *
 *          M reads only mesh geometry, the eigenbasis, and readable state
 *          (depth weights, cell velocity). It is reassembled on the ROM's
 *          basis-update cadence — when the flow field has moved materially —
 *          never per step, and never triggers a re-eigensolve: P stays fixed.
 *
 *          Cost: assemble O(nnz·k + n·k²); propagate O(k³) per member per
 *          batch via a (k+1)² augmented matrix exponential — microseconds at
 *          the ROM's k ≈ 24.
 *
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_DEVIATION_OPERATOR_2D_HPP
#define OPENSWMM_ENGINE_2D_DEVIATION_OPERATOR_2D_HPP

#include "../data/MeshData.hpp"
#include "MeshEigenBasis.hpp"

#include <vector>

namespace openswmm::twoD {

struct DeviationOperator2D {

    // ---- physics dials (set before assemble()) ----

    /// Streamwise diffusivity multiple of D_scale. 1.0 = isotropic baseline;
    /// local-inertial starting point ≈ 0.31·(K_eff/D_scale).
    double alpha_par = 1.0;

    /// Transverse diffusivity multiple of D_scale. 1.0 = isotropic baseline;
    /// local-inertial starting point ≈ 1.0·(K_eff/D_scale).
    double alpha_perp = 1.0;

    /// Advection celerity as a multiple of the local speed |u⃗|:
    /// c⃗ = c_factor·u⃗. 5/3 = Manning kinematic-wave celerity; 0 = off.
    double c_factor = 0.0;

    /// Speed floor (m/s): below it a face has no meaningful flow direction and
    /// the conductance blends to the direction-free mean (α∥+α⊥)/2.
    double u_eps = 1.0e-6;

    // ---- set by assemble() ----

    int k = 0;                ///< Retained modes (== basis.num_kept).
    std::vector<double> M;    ///< k×k row-major, 1/s: d(δa)/dt = −M·δa + g.

    /**
     * @brief Assemble M = Pᵀ·L_op·P from mesh geometry, flow state and dials.
     *
     * @param mesh    Mesh (topology finalized).
     * @param basis   Built eigenbasis; P defines the projection and k.
     * @param D_scale Diffusivity scale (m²/s). Typically the Manning
     *                diffusion-wave value h̄^{5/3}/(2·n̄·√S̄).
     * @param h_cell  Optional per-cell depth weights; when non-null, each edge
     *                conductance is multiplied by the harmonic mean of
     *                (h/h̄)^{5/3} of its two cells (the operator-level version
     *                of the diagonal path's Rayleigh weighting). Null = uniform.
     * @param cell_u  Optional per-cell velocity x (m/s). Null (either) =
     *                no flow direction: isotropic mean conductance, no
     *                advection regardless of c_factor.
     * @param cell_v  Optional per-cell velocity y (m/s).
     * @param ground_w Optional per-cell open-boundary grounding conductance
     *                (dimensionless len/d convention, matching
     *                MeshEigenBasis::build). Adds D_scale·⟨α⟩·ground_w/A to
     *                the cell's diagonal — deviations flush through open
     *                boundaries, so the operator must not be pure Neumann
     *                wherever the basis was grounded. Use the SAME weights as
     *                the basis build: an ungrounded operator on a grounded
     *                basis leaves the drain mode undamped (spurious spread
     *                growth), and the reverse hides the drain mode entirely.
     * @return true on success (basis ready, mesh non-degenerate).
     */
    bool assemble(const MeshData& mesh, const MeshEigenBasis& basis,
                  double D_scale,
                  const double* h_cell,
                  const double* cell_u, const double* cell_v,
                  const double* ground_w = nullptr);

    // ------------------------------------------------------------------------
    // Dense propagator utilities (static — used by SpectralROM and the tests)
    // ------------------------------------------------------------------------

    /**
     * @brief In-place dense matrix exponential A ← exp(A), n×n row-major.
     *
     * Scaling-and-squaring with a [6/6] Padé approximant — the standard
     * dependency-free algorithm, accurate to near round-off for the well-scaled
     * matrices produced here (‖A‖ is reduced below 1/2 before the Padé step).
     */
    static void expm(std::vector<double>& A, int n);

    /**
     * @brief One exact deviation step:
     *        δa ← exp(−s·Δt·M)·δa + φ₁(−s·Δt·M)·Δt·g.
     *
     * This is the matrix generalization of the diagonal path's exact
     * exponential integrator — for k = 1 it reduces to it identically:
     * (δa − g/r)·e^{−r·Δt} + g/r. φ₁ is evaluated via the augmented-matrix
     * identity exp([[A, Δt·g],[0,0]]) = [[e^A, φ₁(A)·Δt·g],[0,1]], which is
     * well-defined for singular A (φ₁(0) = I ⇒ the Euler limit δa += g·Δt).
     *
     * @param M        k×k row-major operator (1/s).
     * @param k        Dimension.
     * @param s        Per-member operator scaling (the ROM passes 1/mm_i:
     *                 Manning multiplies n, and both the friction diffusivity
     *                 and the kinematic celerity scale as 1/n, so the whole
     *                 operator scales together).
     * @param dt       Step (s).
     * @param delta_a  In/out: member deviation coefficients (length k).
     * @param g        Constant-over-step modal forcing (length k).
     */
    static void propagate(const std::vector<double>& M, int k,
                          double s, double dt,
                          double* delta_a, const double* g);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_DEVIATION_OPERATOR_2D_HPP
