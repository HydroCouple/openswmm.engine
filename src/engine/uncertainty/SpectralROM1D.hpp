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
 *
 *          where:
 *            λ_j    = j-th rate of the basis (1/s on the engine's
 *                     conductance/storage basis, H18; the engine passes K1d = 1)
 *            mm_i   = member i Manning multiplier
 *            b_j(t) = P[:,j]^T · S · φ   (projection of the roughness response)
 *
 *          The nominal member (mm = 1) has zero forcing and zero deviation
 *          forever, so the median tracks the deterministic run by construction
 *          and no reseeding is ever required (spread is never collapsed).
 *
 *          Additive uncertainties (rain, inflow) are not in the modes: the
 *          engine carries them at full order (H19, ADDITIVE_CHANNEL.md) and
 *          hands the fields over with setAdditivePlanes().
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP

#include "GraphEigenBasis.hpp"
#include "RomDensePropagator.hpp"
#include "NetworkLaplacian1D.hpp"
#include "UncertaintyTypes.hpp"
#include "RomPhaseCoordinate.hpp"
#include "RomQuantileGemm.hpp"
#include <vector>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace openswmm::uncertainty {

/**
 * @brief 1D spectral ROM for ensemble-based uncertainty propagation.
 *
 * Lifecycle:
 *   1. Set `basis` to a built GraphEigenBasis (num_kept > 0).
 *   2. Optionally set n_ensemble, mannings_pert, runoff_pert.
 *   3. Call initialize().
 *   4. Call seed(h_nodes) — zeroes all deviations, primes h_det_last_.
 *   5. Each timestep: call advance(dt, K1d, h_det_active, sens_ref),
 *      then computeQuantiles(h_det_active, invert_active) when output is due.
 *   6. Read q05, q50, q95 per active node.
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

    /// H17 (2026-10-12): quasi-static (mode-acceleration) correction of the
    /// Manning channel. The part of the response field phi that the kept modes
    /// cannot represent, r = phi - P P^T phi, belongs to modes faster than any
    /// kept one, so it is added at its steady value (mm_i - 1) * r instead of
    /// being dropped. The steady band is then exactly (mm_i - 1) * phi at every
    /// node, with the kept modes carrying only the dynamics. On Bellinge
    /// (1,011 nodes, 20 modes) truncation kept 5-15 % of phi on shallow
    /// laterals; with the correction the network's bands are calibrated
    /// (VALIDATION.md "H17 §10"). Applies only when advance() is given a
    /// response field (sens_ref); off only as an internal A/B hook.
    bool quasi_static_residual = true;


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
     * @brief Per-member coefficients of a soft-rain distribution family.
     *
     * One rank per member, u_i = shuffledStrata(M, sample_seed + 4), shared by
     * every family so a member sits at the same rank everywhere (COHERENCE
     * FULL): UNIFORM gives c_i = 2 u_i - 1 (half-range band); NORMAL and
     * LOGNORMAL give c_i = probit(u_i) (LOGNORMAL by the delta linearization,
     * the caller scaling the spread by its location). The engine's full-order
     * additive channel (H19) multiplies its rain planes by these. Valid after
     * initialize().
     */
    std::vector<double> softCoefficients(DistType family) const;

    /**
     * @brief Register an additional uncertain parameter column (PR 9b).
     *
     * RATE_MULT multiplies the effective Manning multiplier
     * (mm_eff = mm · Π θ), shaping both the decay rate and the Manning target.
     * COUPLING_MULT is accepted and ignored here (coupling reads cd_mult).
     * Additive entries (FORCING_MULT, FORCING_VECTOR) are carried by the
     * engine's full-order additive channel (H19) and are refused.
     *
     * Call after initialize(). @p column length must equal n_ensemble.
     * @throws std::invalid_argument on size mismatch or an additive entry.
     */
    void addRegisteredParam(ParamEntry entry, const std::vector<double>& column);

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
                     const uint8_t* node_surcharged = nullptr,
                     const CsrGraph* Lc = nullptr,
                     const double* storage = nullptr);

    // -------------------------------------------------------------------------
    // PR H18 — per-mode relaxation rates (docs/uncertainty/RELAXATION_RATES.md)
    // -------------------------------------------------------------------------
    //
    // updateBasis() with a conductance Laplacian Lc (ft^2/s, active-node space)
    // and a node storage S (ft^2, length n_nodes) solves Lc v = lambda S v
    // instead of the dimensionless weighted Laplacian: the basis becomes
    // S-orthonormal (P^T S P = I), every projection becomes P^T S (field), and
    // the eigenvalues are rates in 1/s. The triggers and cadence still read
    // conduit_off. A rebuild is forced while no storage is held yet. The
    // caller then supplies per-step rates (setModeRates) and K1d = 1.

    /// PR H19 (docs/uncertainty/ADDITIVE_CHANNEL.md): the engine carries every
    /// additive deviation (soft rain, runoff multiplier, registered inflow
    /// sources) at full order; reconstruction adds sum_p kappa_ip * Psi_p.
    /// Psi: n_planes x n_nodes (row-major); kappa: n_ensemble x n_planes. Copied.
    void setAdditivePlanes(int n_planes, const double* psi, const double* kappa);

    /// Per-member additive correction (n_ensemble x n_nodes, row-major), added
    /// to member i's head next to sum_p kappa_ip * Psi_p. It carries what the
    /// shared planes cannot: deviations that are not kappa times one shape,
    /// such as a storage node whose outlet starts at a threshold (each member's
    /// own level, docs/uncertainty/ADDITIVE_CHANNEL.md §7). Copied; nullptr clears.
    void setMemberAdditive(const double* corr);
    bool hasMemberAdditive() const noexcept { return !add_member_.empty(); }

    /// Per-mode rates (1/s, length n_kept) used in place of the basis
    /// eigenvalues by advance(). Cleared by every successful rebuild.
    void setModeRates(const double* rates);
    void clearModeRates() noexcept { mode_rates_.clear(); }

    /// True when the basis is the generalized (S-orthonormal) one.
    bool hasStorage() const noexcept { return !mass_.empty(); }
    const std::vector<double>& storage() const noexcept { return mass_; }

    // -------------------------------------------------------------------------
    // PR H14 — reduced (non-symmetric) operator
    // -------------------------------------------------------------------------

    /**
     * @brief Install a reduced k×k operator M (row-major, DIMENSIONLESS — the
     *        same units as the basis eigenvalues; advance() multiplies by K1d).
     *
     * When set, advance() integrates every mode with the dense matrix
     * exponential instead of the per-mode diagonal decay:
     *
     *     d(δa_i)/dt = −(K1d·M/mm_i)·δa_i − K1d·(1/mm_i − 1)·M·b + g_i,
     *
     * every forcing term (runoff, soft rain, registered vectors) entering g_i
     * exactly as on the diagonal path. For M = diag(λ) this IS the diagonal
     * path (tested to round-off). The engine installs the Froude-gated
     * directional operator from RomDirectionalOperator.hpp here, so that a
     * supercritical reach no longer carries a downstream deviation upstream.
     *
     * Cleared automatically by a successful updateBasis() (the projection P
     * changed, so M is stale) — the caller re-assembles on its own cadence.
     * Throws on a size mismatch; call after initialize().
     */
    void setReducedOperator(const std::vector<double>& M_in);

    /// Remove the reduced operator; advance() returns to the diagonal path.
    void clearReducedOperator() noexcept { reduced_M_.clear(); }

    /// True when a reduced operator is installed.
    bool hasReducedOperator() const noexcept { return !reduced_M_.empty(); }

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
     *   g     = −λ_j·K1d·(1/mm_i − 1)·b_j
     *   δa   ← (δa − g/rate)·exp(−rate·dt) + g/rate
     * with b_j = P[:,j]^T·(sensitivity reference) recomputed at every call.
     *
     * @param dt              Timestep (s).
     * @param K1d             Effective 1D conductance scale (1/s).
     * @param h_det_active    Deterministic head per active node (length
     *                        n_nodes). Must be non-null; also stored as
     *                        h_det_last_ for reconstructHead().
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
     *                        at that node. Null (default) is bit-identical to
     *                        alpha≡1 (no attenuation).
     */
    void advance(double dt, double K1d, const double* h_det_active,
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
    /**
     * @brief As reconstructHead() but ignoring setPinnedNodes(): at a spilling
     *        node it returns the head the member's deviation would give if the
     *        rim did not hold it, i.e. the member's extra water expressed as a
     *        level. The 2D coupling drives each member's spill with this; the
     *        pinned value would make every member spill identically (Bellinge,
     *        real storm: 2D coverage 0.23 with the pinned head).
     */
    double reconstructHeadUnpinned(int member, int active_node) const noexcept;
    double reconstructHead(int member, int active_node) const noexcept;

    /// Deterministic head the deviations are taken against at @p active_node
    /// (same frame and units as reconstructHead(): internal feet). A member's
    /// head deviation is reconstructHead(i, k) - detHead(k).
    double detHead(int active_node) const noexcept {
        return h_det_last_[static_cast<std::size_t>(active_node)];
    }

    /**
     * @brief Nodes whose head is set by a spill (overflowing at the rim), per
     *        active node; non-owning, may be null. At such a node every member
     *        reconstructs to the deterministic head: no roughness or inflow
     *        change can move a head that the rim holds, and the Monte Carlo
     *        says so (Bellinge 2026-10-21: 476 spilling manholes, members
     *        within 6 cm of the rim, the unpinned band 2.5 m below to 1.5 m
     *        above it). The engine already pins the deviation TARGETS there;
     *        this pins the reconstruction too, because the slow modes keep the
     *        deviation built up before the spill for hours. Members that would
     *        stop spilling are not represented; see VALIDATION.md.
     */
    void setPinnedNodes(const std::vector<char>* pinned_active) noexcept { pinned_ = pinned_active; }

    bool isPinned(int active_node) const noexcept {
        return pinned_ && static_cast<std::size_t>(active_node) < pinned_->size()
               && (*pinned_)[static_cast<std::size_t>(active_node)] != 0;
    }

    /// true if initialize() succeeded.
    bool is_ready() const noexcept { return n_kept > 0 && !a_ensemble.empty(); }

private:
    std::vector<double> sort_buf_;
    std::vector<double> mode_energy_;  ///< Per-mode deviation energy E_j = mean_i(δa²_ij).

    /// Deterministic head passed to the most recent advance()/seed() call
    /// (length n_nodes). Used by reconstructHead().
    std::vector<double> h_det_last_;
    const std::vector<char>* pinned_ = nullptr;   ///< see setPinnedNodes()
    std::vector<double> mass_;        ///< H18: storage S at the last generalized rebuild (empty = identity)
    std::vector<double> projP_;       ///< H18: S*P column by column (projection matrix; empty = P)
    std::vector<double> mode_rates_;  ///< H18: per-mode rates overriding the eigenvalues (empty = eigenvalues)
    const double* projCol_(std::size_t j) const noexcept {
        const std::size_t nn = static_cast<std::size_t>(n_nodes);
        return projP_.empty() ? &basis->P[j * nn] : &projP_[j * nn];
    }
    double modeRate_(std::size_t j) const noexcept {
        return mode_rates_.empty() ? basis->eigenvalues[j] : mode_rates_[j];
    }
    std::vector<double> qsr_r_, qsr_mm1_;
    std::size_t add_np_ = 0;                    ///< H19 additive planes
    std::vector<double> add_psi_, add_kappa_;   ///< H19 Psi (P x n) and kappa (M x P)
    std::vector<double> add_member_;            ///< per-member correction (M x n), empty when none

    std::vector<double> soft_u_;             ///< Fixed shuffled-strata percentiles u_i.
    std::vector<double> soft_z_;             ///< probit(u_i) — normal/lognormal coefficient.

    /// One registered extra parameter (PR 9b): RATE_MULT or COUPLING_MULT.
    struct ExtraParam {
        ParamEntry          entry;
        std::vector<double> column;         ///< θ_i per member (copied)
    };
    std::vector<ExtraParam> extra_params_;

    bool external_samples_set_ = false;
    std::vector<double> external_mann_;
    std::vector<double> external_run_;

    // PR H14 — reduced non-symmetric operator (dimensionless k×k; empty =
    // diagonal path). Scratch: M·b and the K1d-scaled copy handed to the
    // dense propagator, plus the per-member forcing vector.
    std::vector<double> reduced_M_;
    std::vector<double> reduced_Mb_;
    std::vector<double> reduced_MK_;
    std::vector<double> reduced_g_;
    BatchPropagator     reduced_batch_;   ///< PR H14b: shared powers of −dt·K1d·M per advance()

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
