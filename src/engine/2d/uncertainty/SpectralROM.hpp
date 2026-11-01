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
#include "../../uncertainty/UncertaintyTypes.hpp"
#include "../../uncertainty/RomQuantileGemm.hpp"
#include "../../uncertainty/RomDensePropagator.hpp"
#include <vector>
#include <cstddef>
#include <cstdint>
#include <string>

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

    /// H13 (2D half): elasticity of the steady depth to Manning's n, applied
    /// to the Manning-sensitivity source on every advance() path (the fixed
    /// point becomes (mm−1)·e·b instead of (mm−1)·b). The source assumes
    /// depth moves one-for-one with n; sheet-flow normal depth moves as
    /// n^0.6 (Manning, wide section), the same exponent the 1D ROM uses as
    /// alpha_free (PR H5b). 1.0 reproduces the pre-H13 band bit-for-bit.
    double manning_elasticity = 1.0;

    /**
     * @brief H16 — reconstruct depth deviations as a LOCAL FRACTION of h_det
     *        instead of an absolute delta (default false: unchanged, bit-
     *        identical pre-H16 behaviour).
     *
     * W4b measured that the absolute-delta reconstruction `h_i = h_det +
     * P·δa` lets a cell's band inherit a neighbour's deviation in raw units:
     * on a channel with a 40 cm thalweg next to 1 cm flanks, a flank cell's
     * reconstructed band ran up to 8× its own true Monte Carlo spread,
     * because the handful of retained modes cannot resolve a 40× depth
     * contrast, and whatever magnitude leaks through is applied in
     * centimetres regardless of which cell it lands on.
     *
     * When true: (1) the Manning-sensitivity source projected into b_coarse
     * is the CONSTANT field (all ones), not h_det — a spatially uniform
     * target that any basis with ≥1 retained mode represents almost
     * exactly, instead of the sharply-varying h_det a truncated basis
     * cannot; (2) every place a member's absolute depth is reconstructed
     * from δa (both computeQuantiles() paths, applyCouplingFlux()) becomes
     * `h_i = h_det · (1 + Σ_j P[j,t]·δa_{i,j})` — a LOCAL FRACTION of h_det,
     * not an added constant. The fixed point for a frozen source is then
     * δa* = (mm−1)·manning_elasticity·(Pᵀ𝟙)[j], which reconstructs to
     * `h_i(t) ≈ h_det(t)·(1 + (mm_i−1)·manning_elasticity)` everywhere —
     * the SAME uniform relative perturbation at every cell, deep or
     * shallow, with no cross-depth leakage (confirmed in
     * VALIDATION.md "H16" against the W4 channel/plane harness).
     *
     * `manning_elasticity` keeps its existing meaning (H13): it is the e in
     * the fixed point above either way.
     *
     * TWO CHANNELS (2026-10-11). Manning uncertainty is MULTIPLICATIVE (it
     * scales depth everywhere) and belongs in fractional coordinates; rain,
     * soft rain, registered FORCING_VECTOR fields and 1D<->2D coupling are
     * ADDITIVE (they land mostly on thin films and drain to the channel) and
     * belong in absolute coordinates. Measured: one coordinate system for
     * both fails one or the other — fractional rain collapsed the channel
     * rain band (coverage 0.05), absolute Manning leaves the channel 2.5x
     * wide. So when a reduced operator AND an additive operator are both
     * installed, relative mode carries two coefficient sets per member:
     *   - a_ensemble: the Manning / RATE_MULT channel, δh = H·P·δa with
     *     H = diag(max(h_det, floor)), propagated with the depth-weighted
     *     Galerkin operator (setReducedOperator + setRelativeGram);
     *   - a_add: every additive forcing, plain δh = P·δa, propagated with the
     *     plain operator (setAdditiveOperator), exactly the absolute ROM.
     * Reconstruction: h_i = h_det·(1 + P·δa_i) + P·δa_add_i. The system is
     * linear, so the split is exact to first order (the Manning x rain cross
     * term is second order). Without an additive operator (e.g. the diagonal
     * path), relative mode is the original H16 Manning-only form.
     *
     * Not yet validated together with `spatial_mannings` (H15 per-member
     * operators) or a reduced operator other than the shared diagonal/
     * isotropic path; combine with those only after measuring. The 1D ROM
     * does not have this option — its depth-contrast case (H5b's J3) was
     * judged basis truncation on a 5-node fixture with no production
     * analogue, lower priority than the 2D case W4b measured.
     */
    bool relative_deviation = false;

    /// Additive-channel coefficients (two-channel relative mode only),
    /// a_add[i * n_kept + j]; zero and unused otherwise.
    std::vector<double> a_add;

    /// Two-channel relative mode: the reduced operator (k×k, 1/s) the
    /// additive channel propagates with. Empty = single-channel.
    /// @param Ginv_in  Optional k×k inverse of the volume Gram Pᵀ·A·P when
    ///                 M_in was assembled in the volume inner product
    ///                 (DeviationOperator2D::volume_inner_product). The
    ///                 coupling exchange is then projected consistently:
    ///                 a volume ΔV at cell c becomes G⁻¹·P[:,c]·ΔV, so one
    ///                 cubic metre has the same modal footprint whatever the
    ///                 size of the cell it enters. Empty = plain P[:,c]·ΔV/A_c.
    void setAdditiveOperator(const std::vector<double>& M_in,
                             const std::vector<double>& Ginv_in = {});

    /// True when relative mode runs the two-channel split.
    bool twoChannel() const noexcept {
        return relative_deviation && !reduced_M_add_.empty() && !reduced_M_.empty();
    }

    /// Relative mode: install G⁻¹ = ((HP)ᵀ(HP))⁻¹ (k×k row-major) from the
    /// same DeviationOperator2D assembly that produced the reduced operator.
    /// weight: the cell weights of Ginv's inner product (DeviationOperator2D::GramWeight);
    /// empty = plain. The Manning target is projected in the same inner product.
    void setRelativeGram(const std::vector<double>& Ginv, const std::vector<double>& weight = {});

    /// Relative mode only: the depth (m) below which h_det is floored when
    /// converting an absolute forcing to a fractional rate. A dry cell's
    /// reconstruction is h_det·(1+...) = 0 regardless; the floor keeps the
    /// weight finite. The router sets it to the marcher's dry_depth.
    double relative_depth_floor = 1.0e-3;

    // -------------------------------------------------------------------------
    // PR H4 — quantile-reconstruction GEMM. Debug/equivalence-testing knob:
    // when true, computeQuantiles() uses the original hand-rolled per-cell
    // reconstruction + full sort instead of reconstructEnsembleGemm()
    // (RomQuantileGemm.hpp) + std::nth_element selection. Both paths always
    // compiled in. Default false = GEMM + nth_element (or the portable
    // fallback where OPENSWMM_HAVE_CBLAS is not defined).
    // -------------------------------------------------------------------------
    bool rom_quantile_naive = false;

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
    /// Reporting threshold (m). > 0 makes computeQuantiles() also fill p_exceed.
    double exceed_depth = 0.0;
    /// Fraction of members with depth strictly above exceed_depth, per cell
    /// (empty when exceed_depth <= 0). Empirical over the M members.
    std::vector<double> p_exceed;

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
     * @param rom1d_head_scale  Factor converting @p rom1d heads into the 2D
     *               length unit (metres). The 1D ROM reconstructs heads in the
     *               engine's internal FEET, while @p node_heads and the mesh are
     *               metres, so the router passes 0.3048.
     *
     * Deviation form, kept exact: each member's exchange is compared with the
     * SAME exchange law evaluated at the deterministic state, and the member's
     * 1D head is the current deterministic head plus that member's 1D
     * deviation. Identical members therefore receive exactly zero deviation.
     * (Comparing against the exchange the 2D integrator booked, as before
     * 2026-10-21, leaks every difference between the integrator's clamps and
     * this law into all members alike: on Bellinge the zero-perturbation
     * median drifted 4 cm before rain and ~1 m by the storm peak.)
     *
     * Each exchange is integrated IMPLICITLY over the step: the cell level at
     * the end of the step satisfies h_new = h_old - Q(h_new)·dt/A. An explicit
     * update overshoots where the manhole and street levels are close (the
     * orifice slope dQ/dh grows like 1/sqrt|dh|), and on Bellinge that grew
     * without bound after the storm peak.
     */
    void applyCouplingFlux(const std::vector<CouplingPoint>& cps,
                           const double* node_heads,
                           const MeshData& mesh,
                           double dt,
                           const openswmm::uncertainty::SpectralROM1D* rom1d = nullptr,
                           double rom1d_head_scale = 1.0);

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
     *
     * @param spread_b Optional SECOND spread plane carrying a DIFFERENT
     *        distribution family (PR H7). Null (default) leaves the ROM
     *        single-family and bit-identical to the pre-H7 behaviour. When
     *        non-null the member forcing sensitivity becomes
     *
     *            c_i·(Pᵀ·spread)_j  +  c_i^B·(Pᵀ·spread_b)_j
     *
     *        i.e. two projections per advance, one per family. This is what a
     *        MIXED grid (`/family_code`) needs: cells of family A go in
     *        @p spread with zeros elsewhere, family B's cells in @p spread_b
     *        with zeros elsewhere, so every cell keeps its OWN marginal shape
     *        instead of borrowing one family's coefficient with a range-matched
     *        spread. NON-OWNING, same lifetime contract as @p spread.
     * @param family_b Family selecting c_i^B. Both coefficients derive from the
     *        SAME `u_i = shuffledStrata(M, sample_seed+4)` stream, so one rank
     *        per member holds across families — the COHERENCE FULL contract —
     *        and the nominal member is nominal in both channels simultaneously.
     *
     * @note Scope guard (PR H7): a second plane is REFUSED when @p soft_field
     *       is spatial (`COHERENCE CORR_LEN`), because the correlated path
     *       carries one coefficient family per source by construction. The call
     *       still succeeds as a single-family call and softPlanesError()
     *       returns a non-empty explanation; CORR_LEN × MIXED is additive
     *       future work.
     */
    void setSoftForcing(const double* loc, const double* spread,
                        openswmm::uncertainty::DistType family
                            = openswmm::uncertainty::DistType::NORMAL,
                        const SpatialUncertaintyField* soft_field = nullptr,
                        const double* spread_b = nullptr,
                        openswmm::uncertainty::DistType family_b
                            = openswmm::uncertainty::DistType::UNIFORM) noexcept;

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
     *
     * @note Single-family only (PR H7 scope guard): this call clears any second
     *       family plane previously armed via setSoftForcing's `spread_b`, for
     *       the same reason CORR_LEN × MIXED is refused there — the modal
     *       coefficients `a_im` fold in one family's `c_i` by construction.
     */
    void setSoftForcingReduced(const double* loc, const double* spread,
                               openswmm::uncertainty::DistType family,
                               const double* psi_modes, const double* a_coeffs,
                               int n_modes) noexcept;

    /// Per-member soft-forcing coefficient c_i (family-selected in
    /// setSoftForcing). Empty until setSoftForcing() has been called. Used by
    /// the engine to seed the CL-1c correlated coefficient field.
    const std::vector<double>& softCoeff() const noexcept { return soft_coeff_; }

    /// Per-member coefficient c_i^B of the second family plane (PR H7). Empty
    /// until a setSoftForcing() call supplied a non-null `spread_b`.
    const std::vector<double>& softCoeffB() const noexcept { return soft_coeff_b_; }

    /// Empty when the last setSoftForcing() call was honoured in full;
    /// non-empty when a requested second family plane was REFUSED (currently
    /// only the CORR_LEN × MIXED combination — see setSoftForcing()). The ROM
    /// stays in a valid single-family state either way.
    const std::string& softPlanesError() const noexcept { return soft_planes_error_; }

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
     *   represented by one shared M. Without per-member operators advance()
     *   falls back to the diagonal path in that configuration; with them
     *   (setReducedOperatorPerMember, PR H15) it integrates each member on
     *   its own operator.
     */
    void setReducedOperator(const std::vector<double>& M_in);

    /**
     * @brief Install one reduced operator PER MEMBER (PR H15), for a
     *        spatially correlated Manning field.
     *
     * @p M_all is n_ensemble × k × k row-major: member i's operator
     * M_i = Pᵀ·L_op(n_i(t))·P, assembled by DeviationOperator2D::assemble with
     * `cond_mult = 1/W_n,i(t)` (spatial_mannings row i). advance() then
     * integrates, for each member,
     *
     *     d(δa_i)/dt = −(M_i/ρ_i)·δa_i − (M_i/ρ_i − M₀)·b + g_i,
     *
     * where M₀ is the nominal operator from setReducedOperator() (required —
     * it is the anchor the deviation is taken against) and ρ_i is the product
     * of any extra RATE_MULT columns (1 when none). This is the exact
     * generalization of the shared-operator form −(M/mm_i)δa − (1/mm_i − 1)Mb:
     * for a spatially uniform field W_i(t) ≡ mm_i, M_i = M₀/mm_i and the two
     * coincide. Fixed point −(I − M_i⁻¹M₀)·b; a member whose field is
     * identically 1 has M_i ≡ M₀ and stays at exactly zero deviation.
     *
     * Used only when spatial_mannings.is_spatial(); otherwise ignored (the
     * shared operator is both cheaper and identical). Reassemble on the same
     * cadence as the nominal operator. Throws on a size mismatch.
     */
    void setReducedOperatorPerMember(const std::vector<double>& M_all);

    /// Remove the reduced operator(s); advance() returns to the diagonal path.
    void clearReducedOperator() noexcept { reduced_M_.clear(); reduced_M_members_.clear(); reduced_M_add_.clear(); }

    /// True when a reduced operator is installed.
    bool hasReducedOperator() const noexcept { return !reduced_M_.empty(); }

    /// True when per-member reduced operators are installed (PR H15).
    bool hasPerMemberOperators() const noexcept { return !reduced_M_members_.empty(); }
    /// Diagnostics: the installed reduced operators (k×k row-major, 1/s; empty
    /// when not installed). reducedOperatorAdd() is the additive channel's.
    const std::vector<double>& reducedOperator() const noexcept { return reduced_M_; }
    const std::vector<double>& reducedOperatorAdd() const noexcept { return reduced_M_add_; }
    /// Diagnostics: cumulative exchange deviation (m³) of member i at coupling
    /// point cp since the last seed(); 0 when never coupled.
    /// Diagnostics: the pond-level deviation (m) of member i at coupling point
    /// cp: its exchanged volume over the pond footprint's area.
    double couplingResidual(std::size_t cp, int member) const noexcept {
        const auto k = cp * static_cast<std::size_t>(n_ensemble) + static_cast<std::size_t>(member);
        if (k >= coupling_vol_.size() || cp >= pond_area_.size() || !(pond_area_[cp] > 0.0)) return 0.0;
        return -coupling_vol_[k] / pond_area_[cp];
    }
    /// Diagnostics: the pond footprint (cell indices) of coupling point cp.
    const std::vector<int>& pondCells(std::size_t cp) const { return pond_cells_.at(cp); }
    /// Pond area (m^2) of coupling point cp from the last applyCouplingFlux.
    double pondArea(std::size_t cp) const noexcept { return cp < pond_area_.size() ? pond_area_[cp] : 0.0; }
    /// Member i's water depth (m) at coupling point cp's cell, the level the
    /// exchange sees: deterministic depth plus the member's modal deviations,
    /// minus the member's exchanged volume over the pond. -1 when unknown.
    double memberCouplingDepth(std::size_t cp, int member) const noexcept;
    /// Deterministic water depth (m) at coupling point cp's cell, -1 when unknown.
    double detCouplingDepth(std::size_t cp) const noexcept {
        if (cp >= coupling_cell_.size() || coupling_cell_[cp] < 0) return -1.0;
        return std::max(h_det_last_[static_cast<std::size_t>(coupling_cell_[cp])], 0.0);
    }
    /// Exchange volumes (m^3, deviation from the deterministic exchange,
    /// positive into the pipe) per coupling point × member, computed by the
    /// 1D ROM where a member carries its own manhole level
    /// (ADDITIVE_CHANNEL.md §8). Booked by the next applyCouplingFlux instead
    /// of the local orifice law, then cleared, so both sides move the same
    /// water. NaN entries: computed locally as before.
    void setExternalCouplingVolume(std::vector<double> vol) { ext_vol_ = std::move(vol); }
    double couplingVolume(std::size_t cp, int member) const noexcept {
        const auto k = cp * static_cast<std::size_t>(n_ensemble) + static_cast<std::size_t>(member);
        return k < coupling_vol_.size() ? coupling_vol_[k] : 0.0;
    }

private:
    std::vector<double> reduced_M_;   ///< k×k row-major (1/s); empty = diagonal path.
    std::vector<double> reduced_Mb_;  ///< n_kept scratch: M·b_coarse per advance().
    std::vector<double> reduced_g_;   ///< n_kept scratch: per-member forcing vector.
    /// PR H15: per-member operators M_i (n_ensemble entries of k×k), used with
    /// spatial_mannings. Empty = shared operator only.
    std::vector<std::vector<double>> reduced_M_members_;
    std::vector<double> reduced_Mib_; ///< n_kept scratch: M_i·b_coarse per member.
    /// PR H14b: shared-operator batch propagator (powers of −dt·M per advance()).
    std::vector<double> reduced_A_;
    openswmm::uncertainty::BatchPropagator reduced_batch_;
    std::vector<double> reduced_M_add_;   ///< additive-channel operator (k×k); empty = off
    std::vector<double> add_ginv_;        ///< volume-Gram inverse for the additive projection; empty = plain
    std::vector<double> add_proj_buf_;    ///< k scratch: G⁻¹·P[:,c]·A_c for one coupling point
    /// Cumulative exchange DEVIATION per coupling point and member,
    /// [cp * n_ensemble + i], m³, + = drained from the surface into that
    /// member's manhole beyond the deterministic exchange; spread over the
    /// pond footprint (pond_cells_). Reset by seed().
    std::vector<double> coupling_vol_;
    /// Pond footprint per coupling point: the cells the exchanged volume of a
    /// member spreads over, and their total area. The k smooth modes cannot
    /// hold a pond (projected, a member's exchanged volume landed on the
    /// near-constant mode and spread over the whole 35 km² mesh), and one cell
    /// cannot either (the volume stood as a column on the manhole's cell while
    /// the members filled the cells around it). The footprint is the
    /// deterministic pond around the coupling cell: the connected cells deeper
    /// than pond_depth, at most pond_max_cells of them, always including the
    /// coupling cell. A member's volume V raises the whole footprint's level
    /// by V / area, which is how a still pond answers extra water; the
    /// exchange sees the same level, so the member's pond reaches its
    /// manhole level and the exchange equalises. Recomputed every exchange
    /// from the deterministic depth; the volumes are kept, so a growing or
    /// shrinking pond re-spreads them without loss. The 2D analogue of the 1D
    /// quasi-static residual (H17). Reset by seed().
    std::vector<std::vector<int>> pond_cells_;
    std::vector<double> pond_area_;
    std::vector<int> coupling_cell_;          ///< cell index per coupling point of the last call
    std::vector<double> ext_vol_;             ///< external exchange volumes (cp × member), consumed once
    double pond_depth = 0.01;      ///< m: a cell belongs to a pond when deeper than this
    int    pond_max_cells = 5000;  ///< cap: a footprint never spans a whole flooded plain
    std::vector<double> reduced_A_add_;
    std::vector<double> reduced_g_add_;
    openswmm::uncertainty::BatchPropagator reduced_batch_add_;
    std::vector<double> recon_add_buf_;   ///< GEMM scratch for the additive channel

    std::vector<double> h_work_;      ///< n_tri: scratch for reconstruction.
    std::vector<double> h_det_last_;  ///< n_tri: deterministic depth from the last advance()/seed().
    std::vector<double> sort_buf_;    ///< n_ensemble: per-cell sort/selection workspace.

    // PR H4 — GEMM reconstruction (RomQuantileGemm.hpp). recon_buf_ is
    // n_tri*n_ensemble, cell-major (recon_buf_[t*M+i]); gemm_*_active_ are
    // resized internally to k_active*n_tri / n_ensemble*k_active, reused
    // across calls.
    std::vector<double> recon_buf_;
    std::vector<double> gemm_P_active_;
    std::vector<double> gemm_A_active_;
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

    // PR H7 — second per-family coefficient plane. Null unless a caller
    // supplied a (spread_b, family_b) channel; when null every expression
    // that reads it adds an exact 0.0, so the single-family path is
    // bit-identical.
    const double* soft_spread_field_b_ = nullptr;
    std::vector<double> soft_r_spread_b_;    ///< (P^T spread_b)_j.
    std::vector<double> soft_coeff_b_;       ///< Per-member c_i^B (family_b-selected, same u_i).
    double soft_max_abs_coeff_b_ = 0.0;      ///< max_i |c_i^B| for mode activation.
    /// Empty on success; set when a second plane was REFUSED (see
    /// softPlanesError()). Never throws — setSoftForcing is noexcept.
    std::string soft_planes_error_;

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
    /// Per-cell absolute->fractional forcing weight for this advance():
    /// 1/max(h_det, relative_depth_floor) in relative mode, exactly 1.0 otherwise.
    std::vector<double> rel_w_;
    std::vector<double> rel_ginv_;     ///< G⁻¹ for relative mode (k×k); empty = identity.
    std::vector<double> rel_wt_;       ///< cell weights of rel_ginv_'s inner product; empty = plain
    std::vector<double> rel_tmp_;      ///< k scratch for applyGinv_().
    /// In place v ← G⁻¹·v (no-op unless relative mode has a Gram installed).
    void applyGinv_(double* v);

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
