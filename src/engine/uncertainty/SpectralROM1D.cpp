/**
 * @file SpectralROM1D.cpp
 * @brief Spectral ROM for 1D network uncertainty propagation — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "SpectralROM1D.hpp"

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

    q05.assign(static_cast<std::size_t>(n_nodes), 0.0);
    q50.assign(static_cast<std::size_t>(n_nodes), 0.0);
    q95.assign(static_cast<std::size_t>(n_nodes), 0.0);

    sort_buf_.assign(static_cast<std::size_t>(n_ensemble), 0.0);

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
            // Reversed for decorrelation with Manning
            double t_r = (static_cast<double>(n_ensemble - 1 - i) + 0.5)
                       / static_cast<double>(n_ensemble);
            runoff_mult[static_cast<std::size_t>(i)] = r_lo + t_r * (r_hi - r_lo);
        }
    }
}

// ============================================================================
// seed
// ============================================================================

void SpectralROM1D::seed(const double* h_nodes) {
    assert(is_ready());

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis->P[j * nn];
        double dot = 0.0;
        for (std::size_t i = 0; i < nn; ++i)
            dot += Pj[i] * h_nodes[i];
        for (int m = 0; m < n_ensemble; ++m)
            a_ensemble[static_cast<std::size_t>(m) * nk + j] = dot;
    }
}

// ============================================================================
// advance
// ============================================================================

void SpectralROM1D::advance(double dt, double K1d,
                             const double* runoff_per_node) {
    assert(is_ready());

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    // ---- Step 1: Mode energy E_j = mean_i(a²_{i,j}) -------------------------
    for (std::size_t j = 0; j < nk; ++j) {
        double sum_sq = 0.0;
        for (int i = 0; i < n_ensemble; ++i) {
            double aij = a_ensemble[static_cast<std::size_t>(i) * nk + j];
            sum_sq += aij * aij;
        }
        // store in r_coarse temporarily — replaced below
        r_coarse[j] = sum_sq / static_cast<double>(n_ensemble);
    }

    // ---- Step 2: Project runoff into coarse space ----------------------------
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

    // ---- Step 3: Active set --------------------------------------------------
    const bool has_ensemble_runoff = !ensemble_runoff_.empty()
                                      && (runoff_per_node != nullptr)
                                      && (mean_ensemble_runoff_ > 1.0e-30);
    double max_scale = 1.0 + runoff_pert;
    if (has_ensemble_runoff) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
            max_scale = std::max(max_scale,
                                 ensemble_runoff_[i] / mean_ensemble_runoff_);
    }
    const double rain_scale = (runoff_per_node != nullptr)
                               ? std::abs(dt) * max_scale : 0.0;
    n_modes_active = 0;
    for (std::size_t j = 0; j < nk; ++j) {
        double Ej = a_ensemble[j];  // placeholder; recalculate from Step 1 result
        // Recover E_j: r_coarse now holds projected runoff, but we need E_j.
        // E_j was saved in sort_buf_ as a side-channel? No — use a separate pass.
        // Fix: keep mode_energy separate from r_coarse.
        (void)Ej;
        bool by_rain = rain_scale > 0.0 &&
                       std::abs(r_coarse[j]) * rain_scale >= mode_drop_threshold;
        mode_active[j] = by_rain;  // energy check done below
        if (mode_active[j]) ++n_modes_active;
    }

    // Recalculate energy properly (Steps 1 was overwritten above).
    // Recompute E_j directly here.
    for (std::size_t j = 0; j < nk; ++j) {
        double sum_sq = 0.0;
        for (int i = 0; i < n_ensemble; ++i) {
            double aij = a_ensemble[static_cast<std::size_t>(i) * nk + j];
            sum_sq += aij * aij;
        }
        double Ej = sum_sq / static_cast<double>(n_ensemble);
        if (Ej >= mode_drop_threshold) {
            if (!mode_active[j]) { mode_active[j] = true; ++n_modes_active; }
        }
    }

    // ---- Step 4: Advance modes -----------------------------------------------
    const double rate_floor = 1.0e-12;

    for (int i = 0; i < n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double* ai = &a_ensemble[ui * nk];
        double mm = mannings_mult[ui];
        double rm = runoff_mult[ui];

        for (std::size_t j = 0; j < nk; ++j) {
            if (!mode_active[j]) continue;

            double lam  = basis->eigenvalues[j];
            double rate = lam * K1d / mm;

            double fj;
            if (has_ensemble_runoff) {
                fj = r_coarse[j] * (ensemble_runoff_[ui] / mean_ensemble_runoff_);
            } else {
                fj = r_coarse[j] * rm;
            }

            if (rate > rate_floor) {
                double steady = fj / rate;
                ai[j] = (ai[j] - steady) * std::exp(-rate * dt) + steady;
            } else {
                ai[j] += fj * dt;
            }
        }
    }
}

// ============================================================================
// computeQuantiles
// ============================================================================

void SpectralROM1D::computeQuantiles() {
    assert(is_ready());

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    double M  = static_cast<double>(n_ensemble);
    int idx05 = std::max(0, static_cast<int>(0.05 * (M - 1.0) + 0.5));
    int idx50 = static_cast<int>(0.50 * (M - 1.0) + 0.5);
    int idx95 = std::min(n_ensemble - 1, static_cast<int>(0.95 * (M - 1.0) + 0.5));

    for (std::size_t t = 0; t < nn; ++t) {
        for (int i = 0; i < n_ensemble; ++i) {
            const double* ai = &a_ensemble[static_cast<std::size_t>(i) * nk];
            double h = 0.0;
            for (std::size_t j = 0; j < nk; ++j) {
                if (!mode_active[j]) continue;
                h += basis->P[j * nn + t] * ai[j];
            }
            sort_buf_[static_cast<std::size_t>(i)] = std::max(h, 0.0);
        }
        std::sort(sort_buf_.begin(), sort_buf_.end());
        q05[t] = sort_buf_[static_cast<std::size_t>(idx05)];
        q50[t] = sort_buf_[static_cast<std::size_t>(idx50)];
        q95[t] = sort_buf_[static_cast<std::size_t>(idx95)];
    }
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
    double h = 0.0;
    for (std::size_t j = 0; j < nk; ++j) {
        if (!mode_active[j]) continue;
        h += basis->P[j * nn + uk] * ai[j];
    }
    return std::max(h, 0.0);
}

} // namespace openswmm::uncertainty
