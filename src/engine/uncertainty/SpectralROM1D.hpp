/**
 * @file SpectralROM1D.hpp
 * @brief Spectral reduced-order model for uncertainty propagation in 1D
 *        pipe network routing.
 *
 * @details Analogous to SpectralROM (2D surface), but operates on the node
 *          head field of a 1D pipe network represented by GraphEigenBasis.
 *
 *          The linearized ROM for node head h evolves as:
 *
 *              da_j/dt = -λ_j * K1d / mannings_mult_i * a_j  +  f_j^i
 *
 *          where:
 *            a_j     = j-th spectral coefficient for member i
 *            λ_j     = j-th eigenvalue of the network Laplacian
 *            K1d     = effective 1D conductance (m^2/s)
 *            f_j^i   = P[:,j]^T · runoff_per_node × (runoff_mult_i or ensemble)
 *
 *          Per-member runoff from RunoffEnsemble can be injected via
 *          setEnsembleRunoff(): member i's rate scales the deterministic
 *          runoff projection r_coarse[j] by rate_i / mean_rate.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP

#include "GraphEigenBasis.hpp"
#include "NetworkLaplacian1D.hpp"
#include <vector>
#include <cstddef>
#include <memory>

namespace openswmm::uncertainty {

/**
 * @brief 1D spectral ROM for ensemble-based uncertainty propagation.
 *
 * Lifecycle:
 *   1. Set `basis` to a built GraphEigenBasis (num_kept > 0).
 *   2. Optionally set n_ensemble, mannings_pert, runoff_pert.
 *   3. Call initialize().
 *   4. Call seed(h_nodes) — all members start from the same head field.
 *   5. Each timestep: call advance(dt, K1d, runoff_per_node), then computeQuantiles().
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

    double mode_drop_threshold  = 1.0e-10; ///< Drop mode j when E_j < threshold.
    double basis_update_tol     = 0.05;    ///< Skip basis rebuild when max|Δw|/max|w| < tol.
    double basis_update_interval = 60.0;   ///< Min sim time (s) between Lanczos rebuilds.
    double reseed_head_fraction = 0.10;    ///< Reseed when mean head change > 10% of seed mean.
    double reseed_min_interval  = 60.0;    ///< Minimum simulation time (s) between reseeds.

    // -------------------------------------------------------------------------
    // State (set by initialize() and seed())
    // -------------------------------------------------------------------------

    int n_nodes      = 0;  ///< Number of active network nodes (from basis).
    int n_kept       = 0;  ///< Number of retained modes (from basis).
    int n_full_nodes = 0;  ///< Total node count including outfalls (set externally).

    int              n_modes_active = 0;
    std::vector<bool> mode_active;

    // Basis-update bookkeeping
    double last_basis_update_time_ = -1.0e9;  ///< Sim time (s) of last Lanczos rebuild (-1e9 → always fire on first call).

    // Reseed bookkeeping (written by seed() and checkAndReseed())
    std::vector<double> seed_heads_;   ///< Active-node heads at last seed (length n_nodes).
    double last_reseed_time_ = 0.0;    ///< Sim time (s) of last reseed.
    double seed_mean_head_   = 0.0;    ///< Mean head at last seed.

    /// Ensemble coefficient matrix: a_ensemble[i * n_kept + j] = a_{i,j}.
    std::vector<double> a_ensemble;

    /// Pre-projected runoff forcing: r_coarse[j] = P[:,j]^T * runoff_per_node.
    std::vector<double> r_coarse;

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
     * @brief Reseed the ensemble if the hydraulic state has drifted significantly.
     *
     * Compares the mean absolute change in active-node heads against
     * reseed_head_fraction * max(seed_mean_head_, 0.01).  If the threshold is
     * exceeded AND at least reseed_min_interval seconds have elapsed since the
     * last reseed, calls seed(current_heads) and updates seed bookkeeping.
     *
     * @param current_heads  Current active-node heads (length n_nodes).
     * @param sim_time       Current simulation time (s).
     */
    void checkAndReseed(const double* current_heads, double sim_time);

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
     */
    void updateBasis(const double* conduit_off, const int* conduit_n1,
                     const int* conduit_n2, int n_conduits,
                     double sim_time = 0.0);

    /**
     * @brief Allocate buffers and build the LHS parameter design.
     *        Must be called after setting basis and configuration fields.
     */
    void initialize();

    /**
     * @brief Project an initial head field onto all ensemble members.
     *
     * Sets a_i = P^T * h_nodes for every member (identical start).
     *
     * @param h_nodes  Head at each active node (length n_nodes).
     */
    void seed(const double* h_nodes);

    /**
     * @brief Advance all ensemble members by dt.
     *
     * For each member i and mode j:
     *   rate_j = λ_j * K1d / mannings_mult[i]
     *   f_j    = r_coarse[j] * runoff_mult[i]   (or ensemble path)
     *   a_{i,j}(t+dt) = (a_{i,j}(t) - f_j/rate_j)*exp(-rate_j*dt) + f_j/rate_j
     *
     * @param dt              Timestep (s).
     * @param K1d             Effective 1D network conductance (m^2/s).
     * @param runoff_per_node Per-active-node runoff rate (m/s), length n_nodes.
     *                        May be null (no forcing).
     */
    void advance(double dt, double K1d, const double* runoff_per_node);

    /**
     * @brief Reconstruct per-node head distribution and extract quantiles.
     *
     * Writes q05, q50, q95 (length n_nodes).  Negative reconstructed heads
     * are clamped to 0.
     */
    void computeQuantiles();

    /**
     * @brief Reconstruct absolute head for one ensemble member at one active node.
     *
     * Returns Σ_j a_{i,j} * P[j, active_node], clamped to ≥ 0.
     * Only active modes (mode_active[j] == true) contribute.
     *
     * Used by SpectralROM::applyCouplingFlux() when this ROM is registered as
     * the 1D sidecar, so the 2D coupling path sees per-member 1D heads.
     *
     * @param member       Ensemble member index (0..n_ensemble-1).
     * @param active_node  Active ROM node index (0..n_nodes-1).
     * @return Reconstructed head in metres (≥ 0).
     */
    double reconstructHead(int member, int active_node) const noexcept;

    /// true if initialize() succeeded.
    bool is_ready() const noexcept { return n_kept > 0 && !a_ensemble.empty(); }

private:
    std::vector<double> sort_buf_;

    std::vector<double> ensemble_runoff_;
    double mean_ensemble_runoff_ = 0.0;

    bool external_samples_set_ = false;
    std::vector<double> external_mann_;
    std::vector<double> external_run_;

    // PR 4 — time-varying basis update
    std::unique_ptr<GraphEigenBasis> basis_owned_;   ///< Basis owned by ROM after first update.
    std::vector<double> conduit_off_prev_;            ///< Previous weights for skip criterion.
    std::vector<double> work_R_;                      ///< k×k re-projection scratch.

    // PR 5 — vectorized computeQuantiles scratch buffer (node-major: nn × M)
    std::vector<double> recon_buf_;

};

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_SPECTRAL_ROM_1D_HPP
