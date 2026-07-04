/**
 * @file SpectralROM1D.cpp
 * @brief Spectral ROM for 1D network uncertainty propagation — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "SpectralROM1D.hpp"
#include "LhsShuffle.hpp"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace openswmm::uncertainty {

// ============================================================================
// setExternalSamples
// ============================================================================

void SpectralROM1D::setExternalSamples(const std::vector<double>& mann,
                                        const std::vector<double>& runoff) {
    if (static_cast<int>(mann.size()) != n_ensemble ||
        static_cast<int>(runoff.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM1D::setExternalSamples: sizes must equal n_ensemble");
    external_mann_  = mann;
    external_run_   = runoff;
    external_samples_set_ = true;
}

// ============================================================================
// setEnsembleRunoff / clearEnsembleRunoff
// ============================================================================

void SpectralROM1D::setEnsembleRunoff(const std::vector<double>& per_member_rates) {
    if (static_cast<int>(per_member_rates.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM1D::setEnsembleRunoff: size must equal n_ensemble");
    ensemble_runoff_ = per_member_rates;
    mean_ensemble_runoff_ = std::accumulate(per_member_rates.begin(),
                                            per_member_rates.end(), 0.0)
                            / static_cast<double>(n_ensemble);
}

void SpectralROM1D::clearEnsembleRunoff() {
    ensemble_runoff_.clear();
}

// ============================================================================
// initialize
// ============================================================================

void SpectralROM1D::initialize() {
    if (!basis || !basis->is_ready())
        throw std::runtime_error("SpectralROM1D::initialize: basis not built");
    if (n_ensemble < 2)
        throw std::runtime_error("SpectralROM1D::initialize: n_ensemble must be >= 2");

    n_nodes = basis->n_nodes;
    n_kept  = basis->num_kept;

    a_ensemble.assign(static_cast<std::size_t>(n_ensemble) *
                      static_cast<std::size_t>(n_kept), 0.0);
    r_coarse.assign(static_cast<std::size_t>(n_kept), 0.0);
    b_coarse.assign(static_cast<std::size_t>(n_kept), 0.0);

    q05.assign(static_cast<std::size_t>(n_nodes), 0.0);
    q50.assign(static_cast<std::size_t>(n_nodes), 0.0);
    q95.assign(static_cast<std::size_t>(n_nodes), 0.0);

    sort_buf_.assign(static_cast<std::size_t>(n_ensemble), 0.0);
    mode_energy_.assign(static_cast<std::size_t>(n_kept), 0.0);
    h_det_last_.assign(static_cast<std::size_t>(n_nodes), 0.0);

    mode_active.assign(static_cast<std::size_t>(n_kept), true);
    n_modes_active = n_kept;

    // Build LHS design
    mannings_mult.resize(static_cast<std::size_t>(n_ensemble));
    runoff_mult.resize(static_cast<std::size_t>(n_ensemble));

    if (external_samples_set_) {
        mannings_mult = external_mann_;
        runoff_mult   = external_run_;
    } else {
        double m_lo = 1.0 - mannings_pert;
        double m_hi = 1.0 + mannings_pert;
        double r_lo = 1.0 - runoff_pert;
        double r_hi = 1.0 + runoff_pert;

        for (int i = 0; i < n_ensemble; ++i) {
            double t = (static_cast<double>(i) + 0.5) / static_cast<double>(n_ensemble);
            mannings_mult[static_cast<std::size_t>(i)] = m_lo + t * (m_hi - m_lo);
        }
        // Independent Fisher-Yates shuffle (sample_seed+1) of the same strata,
        // giving near-zero rank correlation with Manning instead of the exact
        // -1 a reversed column would give.
        const auto runoff_t = shuffledStrata(n_ensemble, sample_seed + 1);
        for (int i = 0; i < n_ensemble; ++i) {
            auto ui = static_cast<std::size_t>(i);
            runoff_mult[ui] = r_lo + runoff_t[ui] * (r_hi - r_lo);
        }
    }
}

// ============================================================================
// seed
// ============================================================================

void SpectralROM1D::seed(const double* h_nodes) {
    assert(is_ready());

    // Deviation form: every member starts exactly on the deterministic
    // trajectory (δa = 0). h_nodes primes h_det_last_ for reconstructHead()
    // until the first advance() supplies a fresh deterministic reference.
    std::fill(a_ensemble.begin(), a_ensemble.end(), 0.0);
    h_det_last_.assign(h_nodes, h_nodes + static_cast<std::size_t>(n_nodes));
}

// ============================================================================
// advance
// ============================================================================

void SpectralROM1D::advance(double dt, double K1d,
                             const double* h_det_active,
                             const double* runoff_per_node) {
    assert(is_ready());
    assert(h_det_active != nullptr);

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    // ---- Step 1: Deviation mode energy E_j = mean_i(δa²_{i,j}) --------------
    for (std::size_t j = 0; j < nk; ++j) {
        double sum_sq = 0.0;
        for (int i = 0; i < n_ensemble; ++i) {
            double aij = a_ensemble[static_cast<std::size_t>(i) * nk + j];
            sum_sq += aij * aij;
        }
        mode_energy_[j] = sum_sq / static_cast<double>(n_ensemble);
    }

    // ---- Step 2: Project deterministic head and runoff into coarse space ----
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis->P[j * nn];
        double dot_b = 0.0;
        for (std::size_t i = 0; i < nn; ++i)
            dot_b += Pj[i] * h_det_active[i];
        b_coarse[j] = dot_b;
    }
    if (runoff_per_node) {
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nn];
            double dot = 0.0;
            for (std::size_t i = 0; i < nn; ++i)
                dot += Pj[i] * runoff_per_node[i];
            r_coarse[j] = dot;
        }
    } else {
        std::fill(r_coarse.begin(), r_coarse.end(), 0.0);
    }
    h_det_last_.assign(h_det_active, h_det_active + nn);

    // ---- Step 3: Active set ---------------------------------------------------
    // A mode participates when its current deviation energy OR either
    // first-order forcing-sensitivity magnitude exceeds the drop threshold.
    // Both forcing scales use |multiplier − 1| (deviation form): zero
    // perturbation ⇒ zero forcing ⇒ modes stay inactive and δa stays 0.
    const bool has_ensemble_runoff = !ensemble_runoff_.empty()
                                      && (runoff_per_node != nullptr)
                                      && (mean_ensemble_runoff_ > 1.0e-30);
    double max_rain_dev = 0.0;   // max_i |scale_i − 1|
    if (has_ensemble_runoff) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
            max_rain_dev = std::max(max_rain_dev,
                std::abs(ensemble_runoff_[i] / mean_ensemble_runoff_ - 1.0));
    } else {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
            max_rain_dev = std::max(max_rain_dev, std::abs(runoff_mult[i] - 1.0));
    }
    double max_mann_dev = 0.0;   // max_i |1/mm_i − 1|
    for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
        max_mann_dev = std::max(max_mann_dev,
                                std::abs(1.0 / mannings_mult[i] - 1.0));

    const double rain_scale = (runoff_per_node != nullptr)
                               ? std::abs(dt) * max_rain_dev : 0.0;
    const double mann_scale = std::abs(dt) * K1d * max_mann_dev;

    n_modes_active = 0;
    for (std::size_t j = 0; j < nk; ++j) {
        const double lam = basis->eigenvalues[j];
        bool by_energy  = mode_energy_[j] >= mode_drop_threshold;
        bool by_rain    = rain_scale > 0.0 &&
                          std::abs(r_coarse[j]) * rain_scale >= mode_drop_threshold;
        bool by_manning = mann_scale > 0.0 &&
                          lam * std::abs(b_coarse[j]) * mann_scale >= mode_drop_threshold;
        mode_active[j] = by_energy || by_rain || by_manning;
        if (mode_active[j]) ++n_modes_active;
    }

    // ---- Step 4: Advance deviations (exact exponential step) ------------------
    //   d(δa)/dt = −rate·δa + g,   rate = λ_j·K1d/mm_i
    //   g = −λ_j·K1d·(1/mm_i − 1)·b_j + (scale_i − 1)·r_j
    const double rate_floor = 1.0e-12;

    for (int i = 0; i < n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double* ai = &a_ensemble[ui * nk];
        const double mm       = mannings_mult[ui];
        const double inv_mm_1 = 1.0 / mm - 1.0;
        const double scale_1  = has_ensemble_runoff
            ? (ensemble_runoff_[ui] / mean_ensemble_runoff_ - 1.0)
            : (runoff_mult[ui] - 1.0);

        for (std::size_t j = 0; j < nk; ++j) {
            if (!mode_active[j]) continue;

            const double lam  = basis->eigenvalues[j];
            const double rate = lam * K1d / mm;
            const double g    = -lam * K1d * inv_mm_1 * b_coarse[j]
                                + scale_1 * r_coarse[j];

            if (rate > rate_floor) {
                const double steady = g / rate;
                ai[j] = (ai[j] - steady) * std::exp(-rate * dt) + steady;
            } else {
                ai[j] += g * dt;
            }
        }
    }
}

// ============================================================================
// computeQuantiles
// ============================================================================

void SpectralROM1D::computeQuantiles(const double* h_det_active,
                                      const double* invert_active) {
    assert(is_ready());
    assert(h_det_active != nullptr);

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);
    auto M  = static_cast<std::size_t>(n_ensemble);

    int idx05 = std::max(0, static_cast<int>(0.05 * (static_cast<double>(M) - 1.0) + 0.5));
    int idx50 = static_cast<int>(0.50 * (static_cast<double>(M) - 1.0) + 0.5);
    int idx95 = std::min(n_ensemble - 1,
                         static_cast<int>(0.95 * (static_cast<double>(M) - 1.0) + 0.5));

    // Recon buffer: H[node, member] = h_det[node] + Σ_j P[j,node]·δa[member,j]
    // (nn × M). Computed as one node-major pass per active mode; intermediate
    // storage is recon_buf_ (row-major: row = node).
    recon_buf_.assign(nn * M, 0.0);

    for (std::size_t j = 0; j < nk; ++j) {
        if (!mode_active[j]) continue;
        const double* Pj = &basis->P[j * nn];       // P[:,j]: length nn
        for (std::size_t t = 0; t < nn; ++t) {
            double pjt = Pj[t];
            double* row = &recon_buf_[t * M];        // H[t,:]: length M
            for (std::size_t i = 0; i < M; ++i)
                row[i] += pjt * a_ensemble[i * nk + j];
        }
    }

    // Add the deterministic reference, clamp to the physical floor (node
    // invert) when supplied, sort each node's row, extract quantiles.
    for (std::size_t t = 0; t < nn; ++t) {
        double* row = &recon_buf_[t * M];
        const double h_det = h_det_active[t];
        if (invert_active) {
            const double floor_t = invert_active[t];
            for (std::size_t i = 0; i < M; ++i)
                row[i] = std::max(h_det + row[i], floor_t);
        } else {
            for (std::size_t i = 0; i < M; ++i)
                row[i] = h_det + row[i];
        }
        std::sort(row, row + M);
        q05[t] = row[static_cast<std::size_t>(idx05)];
        q50[t] = row[static_cast<std::size_t>(idx50)];
        q95[t] = row[static_cast<std::size_t>(idx95)];
    }
}

// ============================================================================
// updateBasis
// ============================================================================

void SpectralROM1D::updateBasis(const double* conduit_off, const int* conduit_n1,
                                 const int* conduit_n2, int n_conduits,
                                 double sim_time) {
    if (!is_ready() || n_full_nodes < 4 || n_conduits <= 0) return;

    // --- Time-interval guard --------------------------------------------
    if (sim_time - last_basis_update_time_ < basis_update_interval) return;

    // --- Skip criterion -------------------------------------------------
    if (!conduit_off_prev_.empty() &&
        static_cast<int>(conduit_off_prev_.size()) == n_conduits) {
        double max_prev  = 0.0;
        double max_delta = 0.0;
        for (int ci = 0; ci < n_conduits; ++ci) {
            auto uci = static_cast<std::size_t>(ci);
            max_prev  = std::max(max_prev,  std::abs(conduit_off_prev_[uci]));
            max_delta = std::max(max_delta, std::abs(conduit_off[ci] - conduit_off_prev_[uci]));
        }
        if (max_delta / (max_prev + 1.0e-12) < basis_update_tol) return;
    }

    // Past this point we are committed to attempting a rebuild this call.
    // Every exit below — success or failure — must stamp last_basis_update_time_
    // so a persistently failing rebuild backs off for basis_update_interval
    // instead of retrying full Lanczos on every routing step.
    ++basis_updates_attempted_;

    // --- Build new weighted Laplacian ------------------------------------
    // Dry-start guard: if all weights are zero/floor, skip this update.
    std::vector<double> weights(static_cast<std::size_t>(n_conduits));
    bool any_wet = false;
    for (int ci = 0; ci < n_conduits; ++ci) {
        double w = std::max(conduit_off[ci], 1.0e-6);
        weights[static_cast<std::size_t>(ci)] = w;
        if (conduit_off[ci] > 1.0e-6) any_wet = true;
    }
    if (!any_wet) {
        conduit_off_prev_.assign(conduit_off, conduit_off + n_conduits);
        last_basis_update_time_ = sim_time;
        ++basis_updates_failed_;
        return;
    }

    // Normalize weights to mean 1.0 (same rationale as SWMMEngine::buildROM1D):
    // preserves relative conductance structure while keeping the eigenvalue
    // scale matching the topological Laplacian, independent of routing dt.
    {
        double sum_w = 0.0;
        for (double w : weights) sum_w += w;
        if (sum_w > 0.0) {
            const double scale = static_cast<double>(n_conduits) / sum_w;
            for (double& w : weights) w *= scale;
        }
    }

    // Derive is_outfall from full_to_active: negative entry → outfall.
    std::vector<int> is_outfall(static_cast<std::size_t>(n_full_nodes), 0);
    for (int i = 0; i < n_full_nodes && i < static_cast<int>(full_to_active.size()); ++i) {
        if (full_to_active[static_cast<std::size_t>(i)] < 0)
            is_outfall[static_cast<std::size_t>(i)] = 1;
    }

    std::vector<int> active_map_new, full_to_active_new;
    CsrGraph L_new = NetworkLaplacian1D::buildWeighted(
        n_full_nodes, n_conduits,
        conduit_n1, conduit_n2, is_outfall.data(), weights.data(),
        active_map_new, full_to_active_new);

    // Guard: active node count must match (topology must be stable).
    if (static_cast<int>(active_map_new.size()) != n_nodes ||
        static_cast<int>(active_map_new.size()) < 4) {
        conduit_off_prev_.assign(conduit_off, conduit_off + n_conduits);
        last_basis_update_time_ = sim_time;
        ++basis_updates_failed_;
        return;
    }

    // --- Warm-start re-solve --------------------------------------------
    std::vector<double> P_old = basis->P;  // column-major n_nodes × n_kept copy

    if (!basis_owned_) {
        basis_owned_ = std::make_unique<GraphEigenBasis>();
        basis_owned_->null_tol = basis->null_tol;
    }
    if (!basis_owned_->build(L_new, n_kept, P_old.data())) {
        conduit_off_prev_.assign(conduit_off, conduit_off + n_conduits);
        last_basis_update_time_ = sim_time;
        ++basis_updates_failed_;
        return;
    }
    // Sanity check: basis must have exactly n_kept modes (Lanczos must not have
    // broken down prematurely — the combo v0 starting vector prevents this for
    // normal networks, but guard against degenerate geometry).
    if (basis_owned_->num_kept != n_kept) {
        conduit_off_prev_.assign(conduit_off, conduit_off + n_conduits);
        last_basis_update_time_ = sim_time;
        ++basis_updates_failed_;
        return;
    }

    basis = basis_owned_.get();  // switch raw pointer to new owned basis

    // --- Re-project ensemble coefficients: a_new = R * a_old  ----------
    // R[j_new, j_old] = P_new[:,j_new]^T · P_old[:,j_old]
    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    work_R_.resize(nk * nk);
    for (std::size_t jn = 0; jn < nk; ++jn) {
        const double* Pn = &basis->P[jn * nn];
        for (std::size_t jo = 0; jo < nk; ++jo) {
            const double* Po = &P_old[jo * nn];
            double dot = 0.0;
            for (std::size_t i = 0; i < nn; ++i)
                dot += Pn[i] * Po[i];
            work_R_[jn * nk + jo] = dot;
        }
    }

    std::vector<double> a_tmp(nk);
    for (int m = 0; m < n_ensemble; ++m) {
        auto um = static_cast<std::size_t>(m);
        double* ai = &a_ensemble[um * nk];
        for (std::size_t jn = 0; jn < nk; ++jn) {
            double val = 0.0;
            for (std::size_t jo = 0; jo < nk; ++jo)
                val += work_R_[jn * nk + jo] * ai[jo];
            a_tmp[jn] = val;
        }
        for (std::size_t j = 0; j < nk; ++j)
            ai[j] = a_tmp[j];
    }

    conduit_off_prev_.assign(conduit_off, conduit_off + n_conduits);
    last_basis_update_time_ = sim_time;
}

// ============================================================================
// reconstructHead
// ============================================================================

double SpectralROM1D::reconstructHead(int member, int active_node) const noexcept {
    auto ui = static_cast<std::size_t>(member);
    auto uk = static_cast<std::size_t>(active_node);
    auto nk = static_cast<std::size_t>(n_kept);
    auto nn = static_cast<std::size_t>(n_nodes);
    const double* ai = &a_ensemble[ui * nk];
    double h = h_det_last_[uk];   // deterministic reference (last advance/seed)
    for (std::size_t j = 0; j < nk; ++j) {
        if (!mode_active[j]) continue;
        h += basis->P[j * nn + uk] * ai[j];
    }
    return std::max(h, 0.0);
}

} // namespace openswmm::uncertainty
