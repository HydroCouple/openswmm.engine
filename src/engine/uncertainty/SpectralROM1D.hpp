/**
 * @file SpectralROM1D.hpp
 * @brief Spectral reduced-order model for uncertainty propagation in 1D
 *        pipe network routing.
 *
 * @details Analogous to SpectralROM (2D surface), but operates on the node
 *          head field of a 1D pipe network represented by GraphEigenBasis.
 *
 *          DEVIATION FORM (see docs/uncertainty/DEVIATION_FORM.md): the state
 *          per member i is the modal projection of the deviation from the
 *          deterministic trajectory, δa_i = P^T (h_i − h_det), evolving as
 *
 *            d(δa_ij)/dt = −(λ_j·K1d/mm_i)·δa_ij
 *                          − λ_j·K1d·(1/mm_i − 1)·b_j(t)   (Manning sensitivity)
 *                          + (rm_i − 1)·r_j(t)             (forcing sensitivity)
 *
 *          where:
 *            λ_j    = j-th eigenvalue of the network Laplacian
 *            K1d    = effective 1D conductance scale (1/s after L² normalization)
 *            mm_i   = member i Manning multiplier, rm_i = runoff multiplier
 *            b_j(t) = P[:,j]^T · h_det   (deterministic head projection)
 *            r_j(t) = P[:,j]^T · runoff_per_node
 *
 *          The nominal member (mm = rm = 1) has zero forcing and zero deviation
 *          forever, so the median tracks the deterministic run by construction
 *          and no reseeding is ever required (spread is never collapsed).
 *
 *          Per-member runoff from RunoffEnsemble can be injected via
 *          setEnsembleRunoff(): (rm_i − 1) is replaced by (rate_i/mean − 1).
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP

#include "GraphEigenBasis.hpp"
#include "NetworkLaplacian1D.hpp"
#include "UncertaintyTypes.hpp"
#include "SoftSpatialField.hpp"
#include "RomPhaseCoordinate.hpp"
#include "RomQuantileGemm.hpp"
#include <vector>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace openswmm::uncertainty {

/**
 * @brief 1D spectral ROM for ensemble-based uncertainty propagation.
 *
 * Lifecycle:
 *   1. Set `basis` to a built GraphEigenBasis (num_kept > 0).
 *   2. Optionally set n_ensemble, mannings_pert, runoff_pert.
 *   3. Call initialize().
 *   4. Call seed(h_nodes) — zeroes all deviations, primes h_det_last_.
 *   5. Each timestep: call advance(dt, K1d, h_det_active, runoff_per_node),
 *      then computeQuantiles(h_det_active, invert_active) when output is due.
 *   6. Read q05, q50, q95 per active node.
 *
 * Optionally call setEnsembleRunoff() before each advance() to inject
 * per-member runoff rates from RunoffEnsemble (Phase 3 integration).
 */
struct SpectralROM1D {

    // -------------------------------------------------------------------------
    // Configuration (set before initialize())
    // -------------------------------------------------------------------------

    const GraphEigenBasis* basis = nullptr;  ///< Shared eigenbasis; not owned.

    int    n_ensemble    = 50;    ///< Number of ensemble members M.
    double mannings_pert = 0.20;  ///< Half-range: Manning's n ∈ [1-p, 1+p] × base.
    double runoff_pert   = 0.20;  ///< Half-range: runoff multiplier ∈ [1-p, 1+p].

    /// Seed for the internal-fallback LHS design (used only when
    /// setExternalSamples() was not called — the production path shares
    /// samples via UncertaintyEnsemble instead). Runoff strata are shuffled
    /// with sample_seed + 1.
    uint64_t sample_seed = 42;

    double mode_drop_threshold  = 1.0e-10; ///< Drop mode j when E_j and forcing scales < threshold.
    double basis_update_tol     = 0.05;    ///< Skip basis rebuild when max|Δw|/max|w| < tol.
    double basis_update_interval = 60.0;   ///< Min sim time (s) between Lanczos rebuilds.

    // -------------------------------------------------------------------------
    // PR H1 — surcharge-onset cold-restart guard (config; defaults per spec)
    // -------------------------------------------------------------------------

    /// Primary trigger: force a COLD (v0_block=nullptr) basis rebuild instead
    /// of the warm-start when the fraction of active nodes whose
    /// node_surcharged flag flipped since the last successful rebuild exceeds
    /// this threshold. Surcharge onset changes dqdh discontinuously on the
    /// affected conduits, rotating the invariant subspace by O(1) -- P_old
    /// becomes a bad starting guess with no detection otherwise.
    double surcharge_flip_frac_threshold = 0.05;
    /// Secondary trigger: an edge counts as "jumped" when its relative
    /// conduit_off (proportional to dqdh) change exceeds this ratio.
    double edge_drift_ratio_threshold = 1.0;
    /// Secondary trigger: force cold when the fraction of edges that jumped
    /// (per edge_drift_ratio_threshold) exceeds this fraction. Catches
    /// non-surcharge regime shifts (pump switching, gate operations) that the
    /// surcharge flag alone would miss.
    double edge_drift_frac_threshold = 0.05;

    // -------------------------------------------------------------------------
    // PR H11 — per-member phase coordinate (config; defaults per
    // RomPhaseCoordinate.hpp). No effect unless setTravelTime() is called --
    // an unset (empty) T̄ leaves computeQuantiles() on the pre-H11 code path,
    // bit-identical to before this PR.
    // -------------------------------------------------------------------------
    PhaseConfig phase_cfg;

    // -------------------------------------------------------------------------
    // PR H4 — quantile-reconstruction GEMM. Debug/equivalence-testing knob:
    // when true, computeQuantiles() uses the original hand-rolled O(N*M*k)
    // reconstruction loop instead of reconstructEnsembleGemm() (RomQuantileGemm.hpp).
    // Both paths are always compiled in; this never disables the mode-active
    // masking or the sort/quantile-extraction step, which are unaffected by
    // this flag and identical either way. Default false = GEMM path (or the
    // portable fallback where OPENSWMM_HAVE_CBLAS is not defined).
    // -------------------------------------------------------------------------
    bool rom_quantile_naive = false;

    // -------------------------------------------------------------------------
    // State (set by initialize() and seed())
    // -------------------------------------------------------------------------

    int n_nodes      = 0;  ///< Number of active network nodes (from basis).
    int n_kept       = 0;  ///< Number of retained modes (from basis).
    int n_full_nodes = 0;  ///< Total node count including outfalls (set externally).

    int              n_modes_active = 0;
    std::vector<bool> mode_active;

    // Basis-update bookkeeping
    double last_basis_update_time_ = -1.0e9;  ///< Sim time (s) of last SUCCESSFUL Lanczos rebuild / trigger baseline commit.
    double last_basis_update_attempt_time_ = -1.0e9;  ///< Sim time (s) of last attempted rebuild, successful or not; drives retry backoff.
    int basis_updates_attempted_ = 0;  ///< Count of updateBasis() calls that passed the interval/tolerance guards and attempted a rebuild.
    int basis_updates_failed_    = 0;  ///< Count of attempted rebuilds that did not complete successfully.
    int basis_rebuilds_cold_forced_ = 0;  ///< PR H1: count of rebuilds forced cold by the surcharge-flip or edge-drift trigger.

    /// Ensemble DEVIATION coefficient matrix: a_ensemble[i * n_kept + j] = δa_{i,j}
    /// = P[:,j]^T (h_i − h_det).  Zero for the nominal member at all times.
    std::vector<double> a_ensemble;

    /// Pre-projected runoff forcing: r_coarse[j] = P[:,j]^T * runoff_per_node.
    std::vector<double> r_coarse;

    /// Pre-projected deterministic head: b_coarse[j] = P[:,j]^T * h_det.
    /// Refreshed at the top of every advance() call.
    std::vector<double> b_coarse;

    /// Parameter LHS design — one entry per member.
    std::vector<double> mannings_mult;  ///< Ascending LHS in [1-p, 1+p].
    std::vector<double> runoff_mult;    ///< Descending LHS (decorrelated from Manning).

    // -------------------------------------------------------------------------
    // Quantile output (filled by computeQuantiles(); length n_nodes each)
    // -------------------------------------------------------------------------

    std::vector<double> q05;  ///< 5th-percentile head per active node (m).
    std::vector<double> q50;  ///< Median head per active node (m).
    std::vector<double> q95;  ///< 95th-percentile head per active node (m).

    /// Optional full→active node index map for 2D coupling integration.
    ///
    /// full_to_active[full_node_idx] = active ROM node index, or -1 if the
    /// node is an outfall / not in the ROM's active set.
    ///
    /// Populated externally from NetworkLaplacian1D::buildWeighted()'s
    /// full_to_active_out parameter.  Not required for standalone ROM use.
    std::vector<int> full_to_active;

    // -------------------------------------------------------------------------
    // Public methods
    // -------------------------------------------------------------------------

    /**
     * @brief Supply externally generated parameter samples.
     *
     * When called before initialize(), these replace the internal LHS design.
     * Intended for use with UncertaintyEnsemble to share sample columns
     * across 1D and 2D ROM instances.
     *
     * @param mann  Manning's n multiplier per member (length n_ensemble).
     * @param runoff Runoff multiplier per member (length n_ensemble).
     * @throws std::invalid_argument if sizes do not match n_ensemble.
     */
    void setExternalSamples(const std::vector<double>& mann,
                            const std::vector<double>& runoff);

    /**
     * @brief Supply per-member runoff rates from RunoffEnsemble.
     *
     * When set, advance() uses these rates as per-member multipliers on
     * r_coarse[j]:  f_j^i = r_coarse[j] * rate_i / mean_rate.
     * Requires a non-null runoff_per_node argument to advance().
     *
     * @param per_member_rates Per-member surface runoff rate (m/s),
     *                         length n_ensemble.
     * @throws std::invalid_argument if size != n_ensemble.
     */
    void setEnsembleRunoff(const std::vector<double>& per_member_rates);

    /// Revert to the scalar runoff_mult path.
    void clearEnsembleRunoff();

    /**
     * @brief Supply a location/spread forcing pair for soft rainfall / inflow.
     *
     * The deterministic forcing projection uses @p loc (when non-null); the
     * uncertainty sensitivity uses c_i * (P^T spread) with the per-member
     * coefficient c_i selected by @p family (fixed at initialize()):
     *   - NORMAL / LOGNORMAL : c_i = z_i = probit(u_i)   (location-scale delta)
     *   - UNIFORM            : c_i = 2*u_i - 1            (half-range band)
     * so member i's realized forcing is loc + c_i * spread. For LOGNORMAL the
     * caller passes spread = sigma_log * loc (delta linearization, §4.3).
     *
     * Both pointers are NON-OWNING and must remain valid until changed or
     * cleared. Passing nullptr for @p spread disables the soft spread path.
     *
     * @param soft_field Optional per-member spatial coefficient field
     *        (`COHERENCE CORR_LEN`, CL-1b). NON-OWNING; must outlive the ROM
     *        or be cleared. When null (default) or non-spatial, the comonotone
     *        scalar `c_i` path is used and the result is bit-identical to the
     *        pre-CL-1b behaviour. When spatial, member i's per-mode forcing
     *        sensitivity becomes `Σ_t P_j[t]·spread[t]·W_i[t]` (a per-member
     *        projection) in place of the scalar `c_i · (P^T spread)_j`. The
     *        field's `n_cells` must equal the number of active ROM nodes and
     *        `n_members` must equal n_ensemble; its per-node column mean over
     *        members must equal mean_i(c_i) so q50 still tracks the
     *        deterministic answer (checked by assertion in debug builds).
     */
    void setSoftForcing(const double* loc, const double* spread,
                        DistType family = DistType::NORMAL,
                        const SoftSpatialField* soft_field = nullptr) noexcept;

    /// Clear the soft forcing path and revert to the legacy forcing inputs.
    void clearSoftForcing() noexcept;

    /**
     * @brief Correlated soft forcing via a reduced spatial basis (CL-2c).
     *
     * Equivalent to `setSoftForcing(loc, spread, family)` for the location and
     * comonotone-coefficient setup, but replaces the materialized `M×n` field
     * of the CL-1b/CL-1c path with a reduced basis: `K_s` per-point-normalized
     * mode fields `ψ_m(t)` (from `SpdeSpatialBasis::normalizedModes`, so the
     * per-point normalization `g(t)` is already folded in) and per-member modal
     * coefficients `a_im`. Member i's per-mode forcing sensitivity becomes
     *
     *     R_{ij} = Σ_m a_im · Σ_t P_j[t]·spread[t]·ψ_m(t)
     *
     * computed as `K_s` mode projections + an `M×K_s` reconstruction per
     * advance — `O(K_s·k·n) + O(M·K_s·k)` instead of the materialized path's
     * `O(M·k·n)`. Numerically equals the materialized field's `R_{ij}` (the two
     * only differ in summation order). `K_s = 1` (comonotone limit) reproduces
     * the scalar `c_i` path exactly.
     *
     * Both @p psi_modes and @p a_coeffs are NON-OWNING and must outlive the ROM
     * (or be cleared). @p psi_modes is `K_s × n_nodes` row-major; @p a_coeffs is
     * `n_ensemble × K_s` row-major. Takes precedence over any `soft_field` set
     * via setSoftForcing.
     *
     * @param loc        Location field (may be null); same role as setSoftForcing.
     * @param spread     Spread field (non-null to activate); NON-OWNING.
     * @param family     Distribution family (selects the comonotone c_i).
     * @param psi_modes  Normalized mode fields ψ_m(t), K_s × n_nodes row-major.
     * @param a_coeffs   Per-member modal coefficients a_im, M × K_s row-major.
     * @param n_modes    Number of retained basis modes K_s (≥ 1).
     */
    void setSoftForcingReduced(const double* loc, const double* spread,
                               DistType family,
                               const double* psi_modes, const double* a_coeffs,
                               int n_modes) noexcept;

    /// Per-member soft-forcing coefficient c_i (family-selected in
    /// setSoftForcing). Empty until setSoftForcing() has been called. Used by
    /// the engine to seed the CL-1c correlated coefficient field.
    const std::vector<double>& softCoeff() const noexcept { return soft_coeff_; }

    /**
     * @brief Register an additional uncertain parameter column (PR 9b).
     *
     * The built-in Manning/runoff machinery is untouched; registered params
     * fold into the deviation dynamics per their taxonomy
     * (PARAMETER_REGISTRY.md §5):
     *   RATE_MULT      — multiplies the effective Manning multiplier
     *                    (mm_eff = mm · Π θ), shaping both the decay rate and
     *                    the Manning-sensitivity forcing.
     *   FORCING_MULT   — multiplies the runoff forcing scale
     *                    (scale = rm · Π θ, sensitivity (scale − 1)·r_j).
     *   FORCING_VECTOR — @p field points to a per-active-node field v
     *                    (length n_nodes, NON-OWNING, must outlive the ROM or
     *                    be cleared); projected each advance();
     *                    sensitivity (θ_i − 1)·(Pᵀv)_j.
     *   COUPLING_MULT  — ignored by advance() (coupling reads cd_mult).
     *
     * Call after initialize(). @p column length must equal n_ensemble.
     *
     * @throws std::invalid_argument on size mismatch, or FORCING_VECTOR
     *         without a field.
     */
    void addRegisteredParam(ParamEntry entry, const std::vector<double>& column,
                            const double* field = nullptr);

    /// Remove all registered extra parameters (restores built-in-only behavior).
    void clearRegisteredParams();

    /**
     * @brief Rebuild the eigenbasis when the hydraulic operator has changed.
     *
     * Skips the rebuild when max|conduit_off_new − conduit_off_prev| /
     * (max|conduit_off_prev| + 1e-12) < basis_update_tol.
     *
     * On rebuild: warm-starts Lanczos from the current P, then re-projects
     * the ensemble coefficient matrix via R = P_new^T * P_old (exact in the
     * limit where the operator changes slowly).
     *
     * @param conduit_off  0.5*dt*dqdh per conduit (length n_conduits).
     * @param conduit_n1   Upstream node index per conduit (length n_conduits).
     * @param conduit_n2   Downstream node index per conduit (length n_conduits).
     * @param n_conduits   Number of conduits.
     * @param node_surcharged  PR H1: per-full-node surcharge flags (length
     *                         n_full_nodes), from DWSolver::HSnapshot. When
     *                         the fraction of ACTIVE nodes whose flag flipped
     *                         since the last successful rebuild exceeds
     *                         surcharge_flip_frac_threshold, the rebuild uses
     *                         a cold Lanczos start (v0_block=nullptr) instead
     *                         of the usual warm start from P_old -- surcharge
     *                         onset rotates the invariant subspace by O(1),
     *                         making the warm guess actively harmful. Null
     *                         disables the primary trigger (the secondary
     *                         per-edge dqdh-drift trigger still applies).
     */
    void updateBasis(const double* conduit_off, const int* conduit_n1,
                     const int* conduit_n2, int n_conduits,
                     double sim_time = 0.0,
                     const uint8_t* node_surcharged = nullptr);

    /**
     * @brief Allocate buffers and build the LHS parameter design.
     *        Must be called after setting basis and configuration fields.
     */
    void initialize();

    /**
     * @brief Reset all ensemble deviations to zero and prime h_det_last_.
     *
     * Under the deviation form every member starts exactly on the
     * deterministic trajectory (δa = 0); h_nodes is stored as the initial
     * deterministic reference for reconstructHead() until the first advance().
     *
     * @param h_nodes  Deterministic head at each active node (length n_nodes).
     */
    void seed(const double* h_nodes);

    /**
     * @brief Advance all ensemble deviations by dt (exact exponential step).
     *
     * For each member i and mode j (see DEVIATION_FORM.md §3):
     *   rate  = λ_j·K1d / mm_i
     *   g     = −λ_j·K1d·(1/mm_i − 1)·b_j + (rm_i − 1)·r_j
     *   δa   ← (δa − g/rate)·exp(−rate·dt) + g/rate
     * with b_j = P[:,j]^T·(sensitivity reference) recomputed at every call.
     *
     * @param dt              Timestep (s).
     * @param K1d             Effective 1D conductance scale (1/s).
     * @param h_det_active    Deterministic head per active node (length
     *                        n_nodes). Must be non-null; also stored as
     *                        h_det_last_ for reconstructHead().
     * @param runoff_per_node Per-active-node runoff rate (m/s), length n_nodes.
     *                        May be null (no forcing-sensitivity term).
     * @param sens_ref        Optional Manning-sensitivity reference field
     *                        (length n_nodes). When non-null, b_j is projected
     *                        from THIS field instead of h_det_active. The
     *                        engine passes the DEPTH field (head − invert):
     *                        roughness acts on conveyance, i.e. on the depth
     *                        component of head — using absolute head lets the
     *                        (mm−1)·b_j steady state scale the immovable
     *                        invert relief and overestimates spread by orders
     *                        of magnitude on sloped networks (measured ~15×
     *                        median in the PR-10 MC validation; see
     *                        docs/uncertainty/VALIDATION.md). Null preserves
     *                        the h_det projection (standalone/unit-test use).
     * @param alpha           Optional per-active-node Manning-sensitivity
     *                        attenuation factor in [0,1] (length n_nodes),
     *                        folded elementwise into the sensitivity reference
     *                        BEFORE projection: b_j = P[:,j]^T·(alpha ⊙ bref).
     *                        PR H5: in surcharged regime the free-surface
     *                        conveyance sensitivity the ROM assumes no longer
     *                        holds (heads are set by mass balance/backwater,
     *                        not friction), which over-predicted band widths
     *                        50-190x (VALIDATION.md). alpha=1 at a node
     *                        reproduces the pre-H5 projection exactly; alpha=0
     *                        zeroes ONLY the Manning-sensitivity contribution
     *                        at that node — the forcing-sensitivity term
     *                        (r_coarse, from runoff_per_node) is projected
     *                        separately and is never attenuated. Null (default)
     *                        is bit-identical to alpha≡1 (no attenuation).
     */
    void advance(double dt, double K1d, const double* h_det_active,
                 const double* runoff_per_node,
                 const double* sens_ref = nullptr,
                 const double* alpha = nullptr);

    /**
     * @brief PR H11 — supply the per-active-node travel-time field T̄(x)
     *        used by computeQuantiles() to phase-shift each member's
     *        reconstruction (RomPhaseCoordinate.hpp).
     *
     * Always copies @p tbar_active (length n_nodes) into the current field.
     * The internal DetHistoryRing's storage (plane count / push spacing) is
     * sized ONLY on the FIRST call after construction or clearTravelTime(),
     * from a horizon derived at that moment: `max_i|mannings_mult_i − 1| ·
     * max_t tbar_active[t]`. Subsequent calls (the engine's periodic T̄
     * refresh) update the field values only and leave the ring's existing
     * configuration and accumulated history untouched -- reconfiguring
     * (which resets the ring) on every refresh would discard exactly the
     * lookback history the phase channel needs, since the engine's default
     * refresh cadence matches typical REPORT_STEP values. A member whose
     * required lookback exceeds the ring's sized capacity degrades
     * gracefully (DetHistoryRing clamps to the oldest retained plane) rather
     * than losing accumulated history outright.
     *
     * Does NOT itself push a history plane on refresh calls -- pushing
     * happens inside advance() (every step) and once here on the first call
     * (priming the ring with the current h_det_last_). Passing an all-zero
     * field is equivalent to clearTravelTime() in effect (τ_i ≡ 0
     * everywhere) but still counts as "the first call" for ring sizing.
     *
     * @param tbar_active  Travel time per active node (s), length n_nodes.
     */
    void setTravelTime(const double* tbar_active);

    /// Revert to the pre-H11 unphased computeQuantiles() path.
    void clearTravelTime() noexcept;

    /// Current travel-time field (empty when unset). For diagnostics/tests.
    const std::vector<double>& travelTime() const noexcept { return tbar_; }

    /**
     * @brief Reconstruct per-node head distribution and extract quantiles.
     *
     * Per node t, member i:  h = h_ref(t,i) + Σ_j P[j,t]·δa_{i,j},
     * clamped to invert_active[t] when invert_active is non-null.
     * Writes q05, q50, q95 (length n_nodes).
     *
     * Without a travel-time field (setTravelTime() never called, or
     * phase_cfg.enabled == false), `h_ref(t,i) == h_det_active[t]` for every
     * member -- bit-identical to the pre-H11 behavior.
     *
     * With a travel-time field, PR H11's per-member phase coordinate applies:
     * `τ_i(t) = (mannings_mult_i − 1) · tbar_[t]`, and
     *   τ_i > 0 :  h_ref = H(t_now − τ_i)          (sampled from the history ring)
     *   τ_i < 0 :  h_ref = 2·H(t_now) − H(t_now − |τ_i|)   (reflection -- no
     *                                                        future history exists)
     *   τ_i = 0 :  h_ref = h_det_active[t]          (exact short-circuit)
     * where H(·) is DetHistoryRing::sample() and t_now is the ROM's own
     * internal clock (advanced by advance(), reset by seed()). On a STEADY
     * h_det this makes every member's h_ref equal h_det_active[t] exactly,
     * so the phase channel adds no width once the network has settled
     * (DEVIATION_FORM.md's saturated-regime invariant, extended by H11).
     *
     * @param h_det_active   Deterministic head per active node (non-null).
     * @param invert_active  Node invert elevation per active node (physical
     *                       lower bound), or null for no clamping.
     */
    void computeQuantiles(const double* h_det_active,
                          const double* invert_active);

    /**
     * @brief PR H10 — the per-node member values the last computeQuantiles()
     *        call sorted, as a read-only view. Node-major: row `t` is
     *        `[t*n_ensemble, (t+1)*n_ensemble)` and is sorted ASCENDING.
     *
     * computeQuantiles() already reconstructs every member value and sorts
     * each node's row in place to pick q05/q50/q95. Threshold-exceedance
     * fractions and the gap-statistic modality flag need exactly that sorted
     * row, so they read it here rather than reconstructing a second time —
     * the entire cost of H10 is a scan of a buffer that already exists.
     *
     * Valid only after a computeQuantiles() call, and invalidated by the next
     * one. Empty before the first call.
     */
    const std::vector<double>& sortedMemberValues() const noexcept {
        return recon_buf_;
    }

    /**
     * @brief Reconstruct absolute head for one ensemble member at one active node.
     *
     * Returns h_det_last_[active_node] + Σ_j δa_{i,j} · P[j, active_node],
     * guarded to ≥ 0 (numerical safety for the orifice coupling formula).
     * Only active modes (mode_active[j] == true) contribute.  h_det_last_ is
     * the head passed to the most recent advance()/seed() call — at most one
     * routing step old when called from the 2D coupling path.
     *
     * @param member       Ensemble member index (0..n_ensemble-1).
     * @param active_node  Active ROM node index (0..n_nodes-1).
     * @return Reconstructed head (≥ 0).
     */
    double reconstructHead(int member, int active_node) const noexcept;

    /// true if initialize() succeeded.
    bool is_ready() const noexcept { return n_kept > 0 && !a_ensemble.empty(); }

private:
    std::vector<double> sort_buf_;
    std::vector<double> mode_energy_;  ///< Per-mode deviation energy E_j = mean_i(δa²_ij).

    /// Deterministic head passed to the most recent advance()/seed() call
    /// (length n_nodes). Used by reconstructHead().
    std::vector<double> h_det_last_;

    std::vector<double> ensemble_runoff_;
    double mean_ensemble_runoff_ = 0.0;

    const double* soft_loc_field_ = nullptr;
    const double* soft_spread_field_ = nullptr;
    std::vector<double> soft_r_spread_;
    std::vector<double> soft_u_;             ///< Fixed shuffled-strata percentiles u_i.
    std::vector<double> soft_z_;             ///< probit(u_i) — normal/lognormal coefficient.
    std::vector<double> soft_coeff_;         ///< Active per-member coefficient c_i (family-selected).
    double soft_max_abs_coeff_ = 0.0;        ///< max_i |c_i| for mode activation.

    /// CL-1b correlated-coherence (`COHERENCE CORR_LEN`) spatial field. When
    /// non-null and spatial, replaces the scalar `c_i · (P^T spread)_j` term
    /// with the per-member projection `Σ_t P_j[t]·spread[t]·W_i[t]`. NON-OWNING.
    const SoftSpatialField* soft_field_ = nullptr;
    /// Per-member per-mode projection R_{ij} = Σ_t P_j[t]·spread[t]·W_i[t],
    /// row-major [i * n_kept + j]; populated each advance() when spatial.
    std::vector<double> soft_r_spread_spatial_;
    /// Per-mode max_i |R_{ij}| for the spatial mode-activation criterion.
    std::vector<double> soft_spatial_absmax_;

    /// CL-2c reduced spatial basis. When set (takes precedence over
    /// soft_field_), the per-member per-mode projection R_{ij} is assembled
    /// from K_s normalized mode fields ψ_m and per-member coefficients a_im
    /// instead of a materialized M×n field. Both pointers NON-OWNING.
    const double* soft_reduced_psi_ = nullptr;  ///< ψ_m(t), K_s × n_nodes row-major.
    const double* soft_reduced_a_   = nullptr;  ///< a_im, n_ensemble × K_s row-major.
    int           soft_reduced_ks_  = 0;        ///< K_s (0 ⇒ inactive).
    /// Scratch R_m[j] = Σ_t P_j[t]·spread[t]·ψ_m(t), K_s × n_kept row-major.
    std::vector<double> soft_reduced_rm_;

    /// One registered extra parameter (PR 9b). `rv` is the per-mode
    /// projection scratch for FORCING_VECTOR fields, refreshed each advance().
    struct ExtraParam {
        ParamEntry          entry;
        std::vector<double> column;         ///< θ_i per member (copied)
        const double*       field = nullptr;///< FORCING_VECTOR: per-node v (non-owning)
        std::vector<double> rv;             ///< n_kept projections of field
        double              max_dev = 0.0;  ///< max_i |θ_i − 1| (refreshed per advance)
    };
    std::vector<ExtraParam> extra_params_;

    bool external_samples_set_ = false;
    std::vector<double> external_mann_;
    std::vector<double> external_run_;

    // PR 4 — time-varying basis update
    std::unique_ptr<GraphEigenBasis> basis_owned_;   ///< Basis owned by ROM after first update.
    std::vector<double> conduit_off_prev_;            ///< Previous weights for skip criterion.
    std::vector<double> work_R_;                      ///< k×k re-projection scratch.

    // PR H1 — surcharge-onset cold-restart guard
    /// Previous per-ACTIVE-node surcharge flags (length n_nodes), derived from
    /// the last SUCCESSFUL node_surcharged snapshot via full_to_active.
    /// Compared against the current snapshot at the top of the next
    /// updateBasis() call.
    std::vector<uint8_t> node_surcharged_prev_;
    /// Commit a SUCCESSFUL rebuild's conduit_off/node_surcharged/sim_time as
    /// the baseline for the next call's warm-vs-cold decision.
    void commitBasisUpdateSuccess_(const double* conduit_off, int n_conduits,
                                   const uint8_t* node_surcharged, double sim_time);

    // PR 5 — vectorized computeQuantiles scratch buffer (node-major: nn × M)
    std::vector<double> recon_buf_;

    // PR H4 — GEMM reconstruction scratch (RomQuantileGemm.hpp); resized to
    // k_active*n_nodes / n_ensemble*k_active internally, reused across calls.
    std::vector<double> gemm_P_active_;
    std::vector<double> gemm_A_active_;

    // PR H11 — per-member phase coordinate
    /// Per-active-node travel time T̄(x) (s), length n_nodes; empty when
    /// unset (setTravelTime() never called, or clearTravelTime()'d).
    std::vector<double> tbar_;
    /// Bounded h_det history, populated by advance()/seed(); consulted by
    /// computeQuantiles() when tbar_ is non-empty and phase_cfg.enabled.
    DetHistoryRing hist_;
    /// ROM's own internal clock (s since seed()), advanced by dt in
    /// advance(); NOT the same as the caller's simulation time -- only
    /// relative offsets (τ_i) are ever used, so no synchronization is needed.
    double phase_time_ = 0.0;
    /// True once hist_ has been sized by a first setTravelTime() call since
    /// construction or clearTravelTime(). See setTravelTime()'s doc comment
    /// for why the ring is sized once, not on every refresh.
    bool hist_configured_ = false;

};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP
