/**
 * @file SpectralROM.hpp
 * @brief Spectral reduced-order model (ROM) for uncertainty propagation in 2D
 *        surface routing.
 *
 * @details Runs an M-member ensemble alongside the deterministic CVODE simulation
 *          to produce calibrated prediction intervals at near-zero marginal cost.
 *
 *          DEVIATION FORM (see docs/uncertainty/DEVIATION_FORM.md): the state per
 *          member i is the modal projection of the deviation from the deterministic
 *          CVODE depth field, δa_i = P^T (h_i − h_det), evolving as
 *
 *            d(δa_ij)/dt = −(λ_j·keff_ji)·δa_ij
 *                          − λ_j·(keff_ji − keff_j^nom)·b_j(t)   (Manning sensitivity)
 *                          + (f_ij − r_coarse[j])               (forcing sensitivity)
 *
 *          where keff_ji is the per-member depth-weighted mode conductance,
 *          keff_j^nom = keff_modes_[j] the mm=1 nominal, b_j = P[:,j]^T·h_det, and
 *          f_ij the member rainfall forcing (r_coarse[j] its mm=rm=1 nominal).
 *
 *          The nominal member (mm=rm=1) has zero forcing and zero deviation forever,
 *          so the median tracks the deterministic run by construction. The ODE is
 *          solved exactly via an exponential integrator — O(M·k) per advance.
 *
 *          After each advance, `computeQuantiles(h_det)` reconstructs per-cell depth
 *          as h_det + P·δa (clamped at 0) and produces 5/50/95 percentile fields.
 *
 * @note Requires a MeshEigenBasis that built successfully (num_kept > 0).
 * @ingroup engine_2d
 */

#ifndef OPENSWMM_ENGINE_2D_SPECTRAL_ROM_HPP
#define OPENSWMM_ENGINE_2D_SPECTRAL_ROM_HPP

#ifdef OPENSWMM_HAS_2D

#include "MeshEigenBasis.hpp"
#include "../coupling/NodeCoupling.hpp"
#include "SpatialUncertaintyField.hpp"
#include "uncertainty/UncertaintyTypes.hpp"
#include <vector>
#include <cstddef>
#include <cstdint>

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
 *   1. Set `basis` to a fully-built MeshEigenBasis (num_kept > 0).
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

    const MeshEigenBasis* basis = nullptr;  ///< Shared eigenbasis; not owned.

    int    n_ensemble    = 50;    ///< Number of ensemble members M.
    double mannings_pert = 0.20;  ///< Half-range: Manning's n ∈ [1-p, 1+p] × base.
    double rainfall_pert = 0.20;  ///< Half-range: rainfall ∈ [1-p, 1+p] × base.
    double cd_pert       = 0.00;  ///< Half-range: Cd ∈ [1-p, 1+p] × nominal; 0 = disabled.

    /// Seed for the internal-fallback LHS design (used only when
    /// setExternalSamples() was not called — the production path shares
    /// samples via UncertaintyEnsemble instead). Rainfall strata are shuffled
    /// with sample_seed + 1.
    uint64_t sample_seed = 42;

    double mode_drop_threshold = 1.0e-10; ///< Drop mode j when E_j < threshold AND rain forcing < threshold.

    // -------------------------------------------------------------------------
    // State (set by initialize() and seed())
    // -------------------------------------------------------------------------

    int n_tri  = 0;  ///< Number of triangles (from basis).
    int n_kept = 0;  ///< Number of retained modes (from basis).

    int              n_modes_active = 0;   ///< Active modes after last advance() call.
    std::vector<bool> mode_active;          ///< length n_kept; true if mode was updated in last advance().

    /// Ensemble DEVIATION coefficient matrix: a_ensemble[i * n_kept + j] = δa_{i,j}
    /// = P[:,j]^T (h_i − h_det).  Zero for the nominal member (mm=rm=1) at all times.
    /// Row i = member i; column j = mode j coefficient (length n_ensemble × n_kept).
    std::vector<double> a_ensemble;

    /// Pre-projected rainfall forcing: r_coarse[j] = P[:,j]^T * rainfall_full
    /// (the mm=rm=1 nominal projection). Updated by advance() each call.
    std::vector<double> r_coarse;

    /// Pre-projected deterministic depth: b_coarse[j] = P[:,j]^T * h_det.
    /// Refreshed at the top of every advance() call.
    std::vector<double> b_coarse;

    /// Parameter design — one entry per member (scalar path).
    std::vector<double> mannings_mult;  ///< Manning's n scale factor per member.
    std::vector<double> rainfall_mult;  ///< Rainfall scale factor per member.
    std::vector<double> cd_mult;        ///< Discharge coefficient scale factor per member.

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
     * @brief Supply externally generated Cd multiplier samples.
     *
     * When called before initialize(), initialize() uses these samples for
     * cd_mult instead of building the internal LHS.  This is the production
     * path: UncertaintyEnsemble::cdSamples() → setCdSamples() → applyCouplingFlux().
     *
     * @param cd  Cd multiplier per member (length n_ensemble), from
     *            UncertaintyEnsemble::cdSamples().
     * @throws std::invalid_argument if size != n_ensemble.
     */
    void setCdSamples(const std::vector<double>& cd);

    /**
     * @brief Allocate buffers and build deterministic Latin-hypercube parameter
     *        design.  Call once after setting basis and configuration fields.
     *
     * If setExternalSamples() was called first, uses those samples instead of
     * building the internal LHS.
     */
    void initialize();

    /**
     * @brief Reset all ensemble deviations to zero and prime the deterministic
     *        reference for reconstruction.
     *
     * Deviation form: every member starts exactly on the deterministic depth
     * field (δa = 0). h_full is stored as h_det_last_ (used by applyCouplingFlux
     * and by computeQuantiles until the next advance() supplies a fresh field).
     *
     * @param h_full  Deterministic depth at each triangle (length n_tri).
     */
    void seed(const double* h_full);

    /**
     * @brief Advance all ensemble deviations by dt (exact exponential step).
     *
     * For each member i and mode j (see DEVIATION_FORM.md §7):
     *   rate = λ_j · keff_ji
     *   g    = −λ_j·(keff_ji − keff_j^nom)·b_j + (f_ij − r_coarse[j])
     *   δa  ← (δa − g/rate)·exp(−rate·dt) + g/rate      (Euler fallback below floor)
     * with b_j = P[:,j]^T·h_det recomputed every call and keff_j^nom the mm=1
     * per-mode conductance keff_modes_[j].
     *
     * Option A — per-mode Rayleigh-quotient K_eff via h_cell — is unchanged
     * (it shapes keff_modes_[j]); h_cell may be null when the basis is already
     * depth-weighted. h_det is the deterministic reference and is REQUIRED.
     *
     * @param dt        Timestep (s).
     * @param K_eff     Global effective diffusive conductance (m^{4/3}/s).
     * @param rainfall  Per-triangle rainfall rate (m/s), length n_tri, or null.
     * @param h_cell    Per-triangle depth for the Rayleigh-quotient K_eff, or
     *                  null for uniform-K_eff (e.g. depth-weighted basis).
     * @param h_det     Deterministic per-triangle depth (m), length n_tri.
     *                  Required (non-null); also stored as h_det_last_.
     */
    void advance(double dt, double K_eff, const double* rainfall,
                 const double* h_cell, const double* h_det);

    /**
     * @brief Reconstruct per-cell depth and compute 5th/50th/95th percentiles.
     *
     * Per cell t, member i:  depth = max(0, h_det[t] + Σ_j P[j,t]·δa_{i,j}).
     * Sorts the M depths and extracts the three quantile levels.
     *
     * When `parametric_tails` is true, fits a log-normal distribution to the
     * wet-member sub-population (n_wet >= 4) and replaces the sort-based q95
     * with exp(mu + 1.6449·sigma).  q05 and q50 are always sort-based.
     *
     * @param h_det            Deterministic per-triangle depth (length n_tri),
     *                         non-null.
     * @param parametric_tails Enable the log-normal upper-tail estimate.
     */
    void computeQuantiles(const double* h_det, bool parametric_tails = false);

    /**
     * @brief Adjust ensemble ROM coefficients for 2D↔1D coupling exchange.
     *
     * Deviation form: the deterministic coupling exchange is already inside
     * h_det (via the CVODE/coupling pipeline), so only the per-member DIFFERENCE
     * from the deterministic flux is applied to each member's deviation:
     *
     *   Q_det = Cd · A · sign(dh_det) · sqrt(2g|dh_det|)         (nominal Cd, deterministic depth/head)
     *   Q_i   = Cd · cd_mult[i] · A · sign(dh_i) · sqrt(2g|dh_i|) (per-member Cd, reconstructed depth/head)
     *   δa_{i,j} += P[j, ci] · (−(Q_i − Q_det) · dt / tri_area)
     *
     * Orifice discharge does NOT depend on Manning's n (that governs surface
     * conveyance, not the inlet), so the former /mannings_mult scaling is
     * removed — inlet-conveyance uncertainty, if wanted, belongs to the Cd
     * multiplier (cd_mult) or a future dedicated parameter.
     * coupling_unc_output still reports absolute per-member flux bounds Q_i.
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
    /**
     * @param q_det  Optional per-point deterministic exchange flow (m³/s,
     *               + = 2D→1D drain), indexed like @p cps. When supplied it
     *               replaces the internally computed orifice estimate as the
     *               reference each member's deviation is taken against.
     *
     *               Prefer supplying it. The internal estimate re-derives Q_det
     *               from the orifice formula, but the integrator applies its own
     *               clamps and availability limits during the step, so the two
     *               can disagree — and any disagreement is a spurious deviation
     *               applied identically to every member, which walks the
     *               ensemble median off the deterministic run. Passing the
     *               exchange the integrator actually booked keeps the nominal
     *               member at exactly zero deviation, which is what makes the
     *               median track the deterministic solution by construction.
     */
    void applyCouplingFlux(const std::vector<CouplingPoint>& cps,
                           const double* node_heads,
                           const MeshData& mesh,
                           double dt,
                           const openswmm::uncertainty::SpectralROM1D* rom1d = nullptr,
                           const double* q_det = nullptr);

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

    /**
     * @brief Supply a location/spread forcing pair for soft rainfall.
     *
     * The deterministic forcing projection uses @p loc (when non-null); the
     * uncertainty sensitivity uses c_i * (P^T spread) with the per-member
     * coefficient c_i selected by @p family (fixed at initialize()):
     *   - NORMAL / LOGNORMAL : c_i = z_i = probit(u_i)
     *   - UNIFORM            : c_i = 2*u_i - 1
     * For LOGNORMAL the caller passes spread = sigma_log * loc (delta form).
     *
     * Both pointers are NON-OWNING and must remain valid until changed or
     * cleared. Passing nullptr for @p spread disables the soft spread path.
     *
     * @param soft_field Optional per-member spatial coefficient field
     *        (`COHERENCE CORR_LEN`, CL-1c). NON-OWNING; must outlive the ROM or
     *        be cleared. When null (default) or non-spatial, the comonotone
     *        scalar `c_i` path is used and the result is bit-identical to the
     *        pre-CL-1c behaviour. When spatial, member i's per-mode forcing
     *        sensitivity becomes `Σ_t P_j[t]·spread[t]·W_i[t]` (a per-member
     *        projection) in place of the scalar `c_i · (P^T spread)_j`. The
     *        field's `n_cells` must equal n_triangles and `n_members` must
     *        equal n_ensemble; its per-cell column mean over members must equal
     *        mean_i(c_i) so q50 still tracks the deterministic answer (checked
     *        by assertion in debug builds).
     */
    void setSoftForcing(const double* loc, const double* spread,
                        openswmm::uncertainty::DistType family
                            = openswmm::uncertainty::DistType::NORMAL,
                        const SpatialUncertaintyField* soft_field = nullptr) noexcept;

    /// Clear the soft forcing path and revert to the legacy forcing inputs.
    void clearSoftForcing() noexcept;

    /**
     * @brief Correlated soft forcing via a reduced spatial basis (CL-2c).
     *
     * Reduced-basis analogue of `setSoftForcing(loc, spread, family,
     * soft_field)`: instead of a materialized `M×n` field, member i's per-mode
     * forcing sensitivity is assembled from `K_s` per-point-normalized mode
     * fields `ψ_m(t)` (`SpdeSpatialBasis::normalizedModes`, so the per-point
     * normalization `g(t)` is already folded in) and per-member modal
     * coefficients `a_im`:
     *
     *     R_{ij} = Σ_m a_im · Σ_t P_j[t]·spread[t]·ψ_m(t)
     *
     * — `O(K_s·k·n) + O(M·K_s·k)` per advance instead of `O(M·k·n)`, and
     * numerically equal to the materialized field's `R_{ij}` up to summation
     * order. `K_s = 1` reproduces the comonotone scalar `c_i` path exactly.
     *
     * @p psi_modes (K_s × n_tri row-major) and @p a_coeffs (n_ensemble × K_s
     * row-major) are NON-OWNING and must outlive the ROM (or be cleared). Takes
     * precedence over any `soft_field` set via setSoftForcing.
     */
    void setSoftForcingReduced(const double* loc, const double* spread,
                               openswmm::uncertainty::DistType family,
                               const double* psi_modes, const double* a_coeffs,
                               int n_modes) noexcept;

    /// Per-member soft-forcing coefficient c_i (family-selected in
    /// setSoftForcing). Empty until setSoftForcing() has been called. Used by
    /// the engine to seed the CL-1c correlated coefficient field.
    const std::vector<double>& softCoeff() const noexcept { return soft_coeff_; }

    /**
     * @brief Register an additional uncertain parameter column (PR 9b).
     *
     * Same contract as SpectralROM1D::addRegisteredParam
     * (PARAMETER_REGISTRY.md §5): RATE_MULT multiplies the effective Manning
     * multiplier (also on the spatial-Manning path), FORCING_MULT multiplies
     * the member rainfall-forcing scale, FORCING_VECTOR carries a non-owning
     * per-triangle field (length n_tri) re-projected each advance() with
     * sensitivity (θ_i − 1)·(Pᵀv)_j, COUPLING_MULT is ignored by advance()
     * (the coupling path reads cd_mult).
     *
     * Call after initialize(). @p column length must equal n_ensemble.
     *
     * @throws std::invalid_argument on size mismatch, or FORCING_VECTOR
     *         without a field.
     */
    void addRegisteredParam(openswmm::uncertainty::ParamEntry entry,
                            const std::vector<double>& column,
                            const double* field = nullptr);

    /// Remove all registered extra parameters (restores built-in-only behavior).
    void clearRegisteredParams();

    /// True if initialize() has been called and n_kept > 0.
    bool is_ready() const noexcept { return n_kept > 0 && !a_ensemble.empty(); }

    // -------------------------------------------------------------------------
    // Reduced k×k deviation operator (the W3 physics dial)
    // -------------------------------------------------------------------------

    /**
     * @brief Install a reduced deviation operator M (k×k row-major, 1/s).
     *
     * When set, advance() integrates d(δa_i)/dt = −(M/mm_i)·δa_i + g_i with an
     * exact matrix exponential instead of the per-mode diagonal decay
     * λ_j·keff_j/mm_i. The Manning-sensitivity forcing generalizes to
     * −(1/mm_i − 1)·M·b; every other forcing term (rainfall, soft, registered
     * vectors) enters g_i unchanged. Dividing the whole operator by mm is
     * consistent: the Manning multiplier scales n, and both the friction
     * diffusivity and the kinematic celerity scale as 1/n, so the operator
     * scales as a whole.
     *
     * The matrix is typically assembled by DeviationOperator2D — isotropic,
     * flow-aligned anisotropic, and advective operators differ only in what
     * was assembled — and is reassembled on the basis-update cadence, never
     * per step.
     *
     * Notes:
     * - Call after initialize(); the dimension must equal n_kept (throws).
     * - h_cell passed to advance() is IGNORED on this path: depth weighting
     *   belongs in the assembly, where it acts on edge conductances rather
     *   than through the diagonal Rayleigh-quotient approximation.
     * - The per-member spatial Manning field (spatial_mannings) cannot be
     *   represented by one shared M; advance() falls back to the diagonal
     *   path in that configuration.
     */
    void setReducedOperator(const std::vector<double>& M_in);

    /// Remove the reduced operator; advance() returns to the diagonal path.
    void clearReducedOperator() noexcept { reduced_M_.clear(); }

    /// True when a reduced operator is installed.
    bool hasReducedOperator() const noexcept { return !reduced_M_.empty(); }

private:
    std::vector<double> reduced_M_;   ///< k×k row-major (1/s); empty = diagonal path.
    std::vector<double> reduced_Mb_;  ///< n_kept scratch: M·b_coarse per advance().
    std::vector<double> reduced_g_;   ///< n_kept scratch: per-member forcing vector.

    std::vector<double> h_work_;      ///< n_tri: scratch for reconstruction.
    std::vector<double> h_det_last_;  ///< n_tri: deterministic depth from the last advance()/seed().
    std::vector<double> sort_buf_;    ///< n_ensemble: per-cell sort workspace.
    std::vector<double> keff_modes_;  ///< n_kept: per-mode K_eff (Rayleigh quotient, scalar path).
    std::vector<double> mode_energy_; ///< n_kept: E_j = mean_i(a²_{i,j}), recomputed each advance().
    std::vector<double> h_weight_;    ///< n_tri: depth-based cell weights (h/h̄)^(5/3) for keff.

    /// Per-member runoff rates (m/s) from RunoffEnsemble; empty = scalar path.
    std::vector<double> ensemble_rainfall_;

    /// Mean of ensemble_rainfall_; used to normalise per-member multipliers.
    double mean_ensemble_rain_ = 0.0;

    const double* soft_loc_field_ = nullptr;
    const double* soft_spread_field_ = nullptr;
    std::vector<double> soft_r_spread_;
    std::vector<double> soft_u_;             ///< Fixed shuffled-strata percentiles u_i.
    std::vector<double> soft_z_;             ///< probit(u_i) — normal/lognormal coefficient.
    std::vector<double> soft_coeff_;         ///< Active per-member coefficient c_i (family-selected).
    double soft_max_abs_coeff_ = 0.0;        ///< max_i |c_i| for mode activation.

    /// CL-1c correlated-coherence (`COHERENCE CORR_LEN`) spatial field. When
    /// non-null and spatial, replaces the scalar `c_i · (P^T spread)_j` term
    /// with the per-member projection `Σ_t P_j[t]·spread[t]·W_i[t]`. NON-OWNING.
    const SpatialUncertaintyField* soft_field_ = nullptr;
    /// Per-member per-mode projection R_{ij} = Σ_t P_j[t]·spread[t]·W_i[t],
    /// row-major [i * n_kept + j]; populated each advance() when spatial.
    std::vector<double> soft_r_spread_spatial_;
    /// Per-mode max_i |R_{ij}| for the spatial mode-activation criterion.
    std::vector<double> soft_spatial_absmax_;

    /// CL-2c reduced spatial basis. When set (takes precedence over
    /// soft_field_), R_{ij} is assembled from K_s normalized mode fields ψ_m
    /// and per-member coefficients a_im instead of a materialized M×n field.
    /// Both pointers NON-OWNING.
    const double* soft_reduced_psi_ = nullptr;  ///< ψ_m(t), K_s × n_tri row-major.
    const double* soft_reduced_a_   = nullptr;  ///< a_im, n_ensemble × K_s row-major.
    int           soft_reduced_ks_  = 0;        ///< K_s (0 ⇒ inactive).
    /// Scratch R_m[j] = Σ_t P_j[t]·spread[t]·ψ_m(t), K_s × n_kept row-major.
    std::vector<double> soft_reduced_rm_;

    /// One registered extra parameter (PR 9b). `rv` is the per-mode
    /// projection scratch for FORCING_VECTOR fields, refreshed each advance().
    struct ExtraParam {
        openswmm::uncertainty::ParamEntry entry;
        std::vector<double> column;          ///< θ_i per member (copied)
        const double*       field = nullptr; ///< FORCING_VECTOR: per-triangle v (non-owning)
        std::vector<double> rv;              ///< n_kept projections of field
        double              max_dev = 0.0;   ///< max_i |θ_i − 1| (refreshed per advance)
    };
    std::vector<ExtraParam> extra_params_;

    /// When true, initialize() uses external_mann_ / external_rain_ instead of
    /// building the internal LHS.
    bool external_samples_set_ = false;
    std::vector<double> external_mann_;
    std::vector<double> external_rain_;

    /// When true, initialize() uses external_cd_ instead of building internal LHS.
    bool external_cd_set_ = false;
    std::vector<double> external_cd_;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
#endif // OPENSWMM_ENGINE_2D_SPECTRAL_ROM_HPP
