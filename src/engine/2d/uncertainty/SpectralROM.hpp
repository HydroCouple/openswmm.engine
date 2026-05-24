/**
 * @file SpectralROM.hpp
 * @brief Spectral reduced-order model (ROM) for uncertainty propagation in 2D
 *        surface routing.
 *
 * @details Runs an M-member ensemble alongside the deterministic CVODE simulation
 *          to produce calibrated prediction intervals at near-zero marginal cost.
 *
 *          The ROM projects the linearized diffusion-wave ODE onto the k-dimensional
 *          Laplacian eigenbasis supplied by SpectralPrecond2D:
 *
 *              da_j/dt = -rate_j(θ) * a_j  +  f_j(θ)
 *
 *          where:
 *            rate_j(θ) = λ_j * K_eff / mannings_mult   (mode j decay rate)
 *            f_j(θ)    = (P[:,j]^T * rainfall) * rainfall_mult  (mode j forcing)
 *
 *          Each ensemble member carries independent parameters θ = {mannings_mult,
 *          rainfall_mult} drawn from a deterministic Latin-hypercube design.
 *          The ODE is solved exactly via an exponential integrator — no substep
 *          limit, no Krylov solves.  Cost: O(M·k) per advance (vs O(M·n) for
 *          full Monte Carlo).
 *
 *          After each advance, `computeQuantiles()` reconstructs per-cell depth
 *          distributions and produces 5th / 50th / 95th percentile fields.
 *
 * @note Requires that the SpectralPrecond2D basis was built successfully (num_kept > 0).
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_SPECTRAL_ROM_HPP
#define OPENSWMM_ENGINE_2D_SPECTRAL_ROM_HPP

#ifdef OPENSWMM_HAS_2D

#include "../solver/SpectralPrecond2D.hpp"
#include "../coupling/NodeCoupling.hpp"
#include "SpatialUncertaintyField.hpp"
#include <vector>
#include <cstddef>

// Forward declaration — avoids pulling 1D uncertainty headers into 2D solver.
namespace openswmm::uncertainty { struct SpectralROM1D; }

namespace openswmm::twoD {

// ============================================================================
// SpectralROM
// ============================================================================

/**
 * @brief Linear spectral ROM for ensemble-based uncertainty propagation.
 *
 * Lifecycle:
 *   1. Set `basis` to a fully-built SpectralPrecond2D (num_kept > 0).
 *   2. Optionally tune n_ensemble, mannings_pert, rainfall_pert.
 *   3. Call initialize() — allocates buffers and builds parameter design.
 *   4. Call seed(h_full) — projects initial state onto ROM; all members
 *      start from the same deterministic state.
 *   5. At each timestep: call advance(dt, K_eff, rainfall) then computeQuantiles().
 *   6. Read q05, q50, q95 for output.
 */
struct SpectralROM {

    // -------------------------------------------------------------------------
    // Configuration (set before initialize())
    // -------------------------------------------------------------------------

    const SpectralPrecond2D* basis = nullptr;  ///< Shared eigenbasis; not owned.

    int    n_ensemble    = 50;    ///< Number of ensemble members M.
    double mannings_pert = 0.20;  ///< Half-range: Manning's n ∈ [1-p, 1+p] × base.
    double rainfall_pert = 0.20;  ///< Half-range: rainfall ∈ [1-p, 1+p] × base.

    double mode_drop_threshold = 1.0e-10; ///< Drop mode j when E_j < threshold AND rain forcing < threshold.

    // -------------------------------------------------------------------------
    // State (set by initialize() and seed())
    // -------------------------------------------------------------------------

    int n_tri  = 0;  ///< Number of triangles (from basis).
    int n_kept = 0;  ///< Number of retained modes (from basis).

    int              n_modes_active = 0;   ///< Active modes after last advance() call.
    std::vector<bool> mode_active;          ///< length n_kept; true if mode was updated in last advance().

    /// Ensemble coefficient matrix: a_ensemble[i * n_kept + j] = a_{i,j}.
    /// Row i = member i; column j = mode j coefficient (length n_ensemble × n_kept).
    std::vector<double> a_ensemble;

    /// Pre-projected rainfall forcing: r_coarse[j] = P[:,j]^T * rainfall_full.
    /// Updated by advance() each call.
    std::vector<double> r_coarse;

    /// Parameter design — one entry per member (scalar path).
    std::vector<double> mannings_mult;  ///< Manning's n scale factor per member.
    std::vector<double> rainfall_mult;  ///< Rainfall scale factor per member.

    // -------------------------------------------------------------------------
    // Spatial fields (Phase 2 — optional; empty = scalar fallback active)
    // -------------------------------------------------------------------------

    /**
     * @brief Per-cell Manning's n multiplier [M × n_tri].
     *
     * When is_spatial() is true, advance() uses W_n[i][t] per-member per-cell
     * instead of the scalar mannings_mult[i].  applyCouplingFlux() uses
     * W_n[i][cell_idx] at each coupling point.
     *
     * Set by CvodeSurfaceSolver after correlated field generation; left empty
     * for the scalar fallback path.
     */
    SpatialUncertaintyField spatial_mannings;

    /**
     * @brief Per-cell rainfall multiplier [M × n_tri].
     *
     * When is_spatial() is true, advance() computes per-member forcing
     * f_j^i = P[:,j]^T · (rainfall ⊙ W_rain[i]) instead of the scalar
     * r_coarse[j] * rainfall_mult[i].
     */
    SpatialUncertaintyField spatial_rainfall;

    // -------------------------------------------------------------------------
    // Quantile output (filled by computeQuantiles(); length n_tri each)
    // -------------------------------------------------------------------------

    std::vector<double> q05;   ///< 5th-percentile depth per cell (m).
    std::vector<double> q50;   ///< Median depth per cell (m).
    std::vector<double> q95;   ///< 95th-percentile depth per cell (m).

    // -------------------------------------------------------------------------
    // Coupling uncertainty diagnostics (filled by applyCouplingFlux())
    // -------------------------------------------------------------------------

    /**
     * @brief Per-coupling-point exchange flux bounds across the ensemble.
     *
     * Updated on every applyCouplingFlux() call.  Empty until the first call.
     * Outfall coupling points contribute 0.0 to both bounds (they are skipped
     * by the ROM).
     */
    CouplingUncertaintyOutput coupling_unc_output;

    // -------------------------------------------------------------------------
    // Public methods
    // -------------------------------------------------------------------------

    /**
     * @brief Supply externally generated sample columns instead of building
     *        the internal LHS design.
     *
     * When this method is called before initialize(), initialize() will copy
     * these samples rather than building its own LHS.  This is the intended
     * long-term path: `UncertaintyEnsemble` owns the LHS generation and
     * calls this method to hand the result to `SpectralROM`.
     *
     * Requires: `mann.size() == n_ensemble && rain.size() == n_ensemble`.
     * The internal fallback path (LHS built by initialize()) remains available
     * when this method is NOT called.
     *
     * @param mann  Manning's n multiplier per member (length n_ensemble).
     * @param rain  Rainfall multiplier per member (length n_ensemble).
     * @throws std::invalid_argument if sizes do not match n_ensemble.
     */
    void setExternalSamples(const std::vector<double>& mann,
                            const std::vector<double>& rain);

    /**
     * @brief Allocate buffers and build deterministic Latin-hypercube parameter
     *        design.  Call once after setting basis and configuration fields.
     *
     * If setExternalSamples() was called first, uses those samples instead of
     * building the internal LHS.
     */
    void initialize();

    /**
     * @brief Project a full-resolution depth field onto the ROM basis.
     *
     * Sets a_i = P^T * h_full for every ensemble member (same initial state;
     * members diverge only due to their different parameters during advance()).
     *
     * @param h_full  Depth at each triangle (length n_tri).
     */
    void seed(const double* h_full);

    /**
     * @brief Advance all ensemble members by dt using the exact exponential
     *        integrator for the linearized diffusion-wave ROM ODE.
     *
     * For each member i and mode j:
     *   rate_j = K_eff_j / mannings_mult[i]  *  λ_j
     *   f_j    = r_coarse[j] * rainfall_mult[i]
     *   a_{i,j}(t+dt) = (a_{i,j}(t) - f_j/rate_j) * exp(-rate_j*dt) + f_j/rate_j
     *
     * When rate_j ≈ 0 (near-null modes or negligible K_eff), falls back to
     * Euler: a_{i,j}(t+dt) ≈ a_{i,j}(t) + f_j * dt.
     *
     * Option A — per-mode Rayleigh-quotient K_eff:
     *   When h_cell is provided, each mode j gets its own effective conductance
     *   K_eff_j = K_eff * Σ_t φ_j[t]² * (h[t]/h̄)^(5/3)
     *   so modes concentrated in deeper cells decay faster.  When h_cell is
     *   null, K_eff_j = K_eff for all j (uniform, original behaviour).
     *
     * @param dt        Timestep (s).
     * @param K_eff     Global effective diffusive conductance (m^{4/3}/s).
     * @param rainfall  Per-triangle rainfall rate (m/s), length n_tri.
     *                  May be null if there is no rainfall (forcing = 0).
     * @param h_cell    Current per-triangle depth (m), length n_tri.
     *                  When non-null, enables per-mode Rayleigh-quotient K_eff
     *                  (Option A).  May be null for uniform-K_eff behaviour.
     */
    void advance(double dt, double K_eff, const double* rainfall,
                 const double* h_cell = nullptr);

    /**
     * @brief Reconstruct per-cell depth and compute 5th/50th/95th percentiles.
     *
     * For each cell: reconstructs h_i = max(0, P * a_i) for all M members,
     * sorts, and extracts the three quantile levels.
     *
     * When `parametric_tails` is true, fits a log-normal distribution to the
     * wet-member sub-population (n_wet >= 4) and replaces the sort-based q95
     * with exp(mu + 1.6449·sigma).  This reduces sensitivity to the top 1-2
     * samples when M is small.  q05 and q50 are always sort-based.
     *
     * Result is written to q05, q50, q95 (length n_tri).
     */
    void computeQuantiles(bool parametric_tails = false);

    /**
     * @brief Adjust ensemble ROM coefficients for 2D↔1D coupling exchange.
     *
     * For each non-outfall coupling point, reconstructs the per-member depth at
     * the coupling cell, evaluates the orifice flow using each member's individual
     * depth and Manning's multiplier, and updates the ROM coefficients:
     *
     *   Q_i   = (Cd * A * sign(dh) * sqrt(2g|dh|)) / mannings_mult[i]
     *   δa_{i,j} = P[j, ci] * (-Q_i * dt / tri_area)
     *
     * Scaling Q by 1/mannings_mult[i] captures that higher Manning's n reduces
     * the effective drainage conductance of the 2D surface toward the inlet.
     * Members with low n drain faster; members with high n drain slower.
     *
     * Must be called AFTER computeCouplingExchange() and BEFORE advance().
     *
     * @param cps        Coupling points (from buildCouplingPoints()).
     * @param node_heads Absolute SWMM node head per node (m); length = n_nodes.
     *                   Used as the 1D head when @p rom1d is null or not ready.
     * @param mesh       Mesh data (tri_area, tri_cz).
     * @param dt         Routing timestep (s).
     * @param rom1d      Optional 1D network ROM.  When non-null and ready,
     *                   each ensemble member uses its own reconstructed head at
     *                   the coupling node instead of the shared deterministic head.
     *                   rom1d->full_to_active must be populated for the lookup.
     */
    void applyCouplingFlux(const std::vector<CouplingPoint>& cps,
                           const double* node_heads,
                           const MeshData& mesh,
                           double dt,
                           const openswmm::uncertainty::SpectralROM1D* rom1d = nullptr);

    /**
     * @brief Supply per-member runoff rates (m/s) from RunoffEnsemble.
     *
     * When set, advance() replaces the scalar `rainfall_mult[i]` path with a
     * per-member intensity multiplier derived from the runoff ensemble.
     * Specifically, for mode j and member i:
     *
     *   f_j^i = r_coarse[j] * (ensemble_rainfall_[i] / mean_rate)
     *
     * where `r_coarse[j] = P[:,j]^T * rainfall` is the deterministic spatial
     * projection and `mean_rate = mean(per_member_rates)`.  This preserves the
     * spatial structure of the rainfall field while varying the intensity per
     * member — correctly propagating infiltration-driven runoff uncertainty
     * through the non-null Laplacian modes.
     *
     * Requires that `advance()` is called with a non-null `rainfall` argument;
     * if `rainfall` is null, forcing is zero regardless.
     *
     * Call clearEnsembleRainfall() to revert to the rainfall_mult scalar path.
     *
     * @param per_member_rates  Infiltration-subtracted surface runoff rate for
     *                          each member (m/s), length n_ensemble.
     * @throws std::invalid_argument if size != n_ensemble.
     */
    void setEnsembleRainfall(const std::vector<double>& per_member_rates);

    /// Revert to the scalar rainfall_mult path (undo setEnsembleRainfall).
    void clearEnsembleRainfall();

    /// True if initialize() has been called and n_kept > 0.
    bool is_ready() const noexcept { return n_kept > 0 && !a_ensemble.empty(); }

private:
    std::vector<double> h_work_;      ///< n_tri: scratch for reconstruction.
    std::vector<double> sort_buf_;    ///< n_ensemble: per-cell sort workspace.
    std::vector<double> keff_modes_;  ///< n_kept: per-mode K_eff (Rayleigh quotient, scalar path).
    std::vector<double> mode_energy_; ///< n_kept: E_j = mean_i(a²_{i,j}), recomputed each advance().
    std::vector<double> h_weight_;    ///< n_tri: depth-based cell weights (h/h̄)^(5/3) for keff.

    /// Per-member runoff rates (m/s) from RunoffEnsemble; empty = scalar path.
    std::vector<double> ensemble_rainfall_;

    /// Mean of ensemble_rainfall_; used to normalise per-member multipliers.
    double mean_ensemble_rain_ = 0.0;

    /// When true, initialize() uses external_mann_ / external_rain_ instead of
    /// building the internal LHS.
    bool external_samples_set_ = false;
    std::vector<double> external_mann_;
    std::vector<double> external_rain_;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
#endif // OPENSWMM_ENGINE_2D_SPECTRAL_ROM_HPP
