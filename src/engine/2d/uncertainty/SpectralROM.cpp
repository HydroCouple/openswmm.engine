/**
 * @file SpectralROM.cpp
 * @brief Spectral ROM — implementation.
 *
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "SpectralROM.hpp"
#include "uncertainty/SpectralROM1D.hpp"
#include "DeviationOperator2D.hpp"
#include "uncertainty/LhsShuffle.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <cassert>
#include <stdexcept>

namespace openswmm::twoD {

// ============================================================================
// setExternalSamples
// ============================================================================

void SpectralROM::setExternalSamples(const std::vector<double>& mann,
                                      const std::vector<double>& rain) {
    if (static_cast<int>(mann.size()) != n_ensemble ||
        static_cast<int>(rain.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM::setExternalSamples: sample sizes must equal n_ensemble");
    external_mann_ = mann;
    external_rain_ = rain;
    external_samples_set_ = true;
}

// ============================================================================
// setEnsembleRainfall / clearEnsembleRainfall
// ============================================================================

void SpectralROM::setCdSamples(const std::vector<double>& cd) {
    if (static_cast<int>(cd.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM::setCdSamples: size must equal n_ensemble");
    external_cd_ = cd;
    external_cd_set_ = true;
}

void SpectralROM::setEnsembleRainfall(const std::vector<double>& per_member_rates) {
    if (static_cast<int>(per_member_rates.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM::setEnsembleRainfall: size must equal n_ensemble");
    ensemble_rainfall_ = per_member_rates;
    mean_ensemble_rain_ = std::accumulate(per_member_rates.begin(),
                                          per_member_rates.end(), 0.0)
                          / static_cast<double>(n_ensemble);
}

void SpectralROM::clearEnsembleRainfall() {
    ensemble_rainfall_.clear();
}

void SpectralROM::setSoftForcing(const double* loc, const double* spread,
                                openswmm::uncertainty::DistType family,
                                const SpatialUncertaintyField* soft_field) noexcept {
    soft_loc_field_ = loc;
    soft_spread_field_ = spread;
    soft_field_ = soft_field;
    // Switching to the scalar/materialized path must retire any previously
    // configured reduced basis, otherwise advance() would keep taking the
    // reduced path and dereference now-stale pointers. setSoftForcingReduced()
    // re-arms these after calling us, so the clear-then-set order is safe.
    soft_reduced_psi_ = nullptr;
    soft_reduced_a_ = nullptr;
    soft_reduced_ks_ = 0;
    soft_max_abs_coeff_ = 0.0;
    for (int i = 0; i < n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double c = (family == openswmm::uncertainty::DistType::UNIFORM)
            ? (2.0 * soft_u_[ui] - 1.0)
            : soft_z_[ui];
        soft_coeff_[ui] = c;
        soft_max_abs_coeff_ = std::max(soft_max_abs_coeff_, std::abs(c));
    }

    // CL-1c: a supplied spatial field must be dimensionally consistent and its
    // per-cell column mean must match c̄ (mean_i c_i) so the nominal member
    // stays zero-deviation and q50 tracks the deterministic answer. Debug-only.
#ifndef NDEBUG
    if (soft_field_ != nullptr && soft_field_->is_spatial()) {
        assert(soft_field_->n_members == n_ensemble &&
               "soft spatial field n_members must equal n_ensemble");
        double cbar = 0.0;
        for (int i = 0; i < n_ensemble; ++i)
            cbar += soft_coeff_[static_cast<std::size_t>(i)];
        cbar /= static_cast<double>(n_ensemble);
        for (int t = 0; t < soft_field_->n_cells; ++t) {
            double colmean = 0.0;
            for (int i = 0; i < n_ensemble; ++i)
                colmean += soft_field_->at(i, t);
            colmean /= static_cast<double>(n_ensemble);
            assert(std::abs(colmean - cbar) < 1.0e-9 &&
                   "soft spatial field column mean must equal mean_i(c_i)");
        }
    }
#endif
}

void SpectralROM::clearSoftForcing() noexcept {
    soft_loc_field_ = nullptr;
    soft_spread_field_ = nullptr;
    soft_field_ = nullptr;
    soft_reduced_psi_ = nullptr;
    soft_reduced_a_ = nullptr;
    soft_reduced_ks_ = 0;
}

void SpectralROM::setSoftForcingReduced(const double* loc, const double* spread,
                                        openswmm::uncertainty::DistType family,
                                        const double* psi_modes,
                                        const double* a_coeffs,
                                        int n_modes) noexcept {
    // Reuse the scalar location + comonotone-coefficient setup (soft_field
    // stays null; the reduced basis takes precedence in advance()).
    setSoftForcing(loc, spread, family, nullptr);
    soft_reduced_psi_ = psi_modes;
    soft_reduced_a_   = a_coeffs;
    soft_reduced_ks_  = (psi_modes != nullptr && a_coeffs != nullptr)
                        ? n_modes : 0;
}

// ============================================================================
// addRegisteredParam / clearRegisteredParams (PR 9b)
// ============================================================================

void SpectralROM::addRegisteredParam(openswmm::uncertainty::ParamEntry entry,
                                     const std::vector<double>& column,
                                     const double* field) {
    if (static_cast<int>(column.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM::addRegisteredParam: column size must equal n_ensemble");
    if (entry == openswmm::uncertainty::ParamEntry::FORCING_VECTOR && field == nullptr)
        throw std::invalid_argument(
            "SpectralROM::addRegisteredParam: FORCING_VECTOR requires a field");
    ExtraParam ep;
    ep.entry  = entry;
    ep.column = column;
    ep.field  = field;
    ep.rv.assign(static_cast<std::size_t>(n_kept), 0.0);
    extra_params_.push_back(std::move(ep));
}

void SpectralROM::clearRegisteredParams() {
    extra_params_.clear();
}

// ============================================================================
// initialize
// ============================================================================

void SpectralROM::setReducedOperator(const std::vector<double>& M_in) {
    const auto nk = static_cast<std::size_t>(n_kept);
    if (nk == 0 || M_in.size() != nk * nk)
        throw std::invalid_argument(
            "SpectralROM::setReducedOperator: M must be n_kept x n_kept and "
            "the ROM must be initialized first");
    reduced_M_ = M_in;
    reduced_Mb_.assign(nk, 0.0);
    reduced_g_.assign(nk, 0.0);
}


void SpectralROM::initialize() {
    if (!basis || basis->num_kept <= 0 || basis->n_triangles <= 0)
        throw std::runtime_error("SpectralROM::initialize: basis not built");
    if (n_ensemble < 2)
        throw std::runtime_error("SpectralROM::initialize: n_ensemble must be >= 2");

    n_tri  = basis->n_triangles;
    n_kept = basis->num_kept;

    a_ensemble.assign(static_cast<std::size_t>(n_ensemble) *
                      static_cast<std::size_t>(n_kept), 0.0);
    r_coarse.assign(static_cast<std::size_t>(n_kept), 0.0);
    b_coarse.assign(static_cast<std::size_t>(n_kept), 0.0);

    q05.assign(static_cast<std::size_t>(n_tri), 0.0);
    q50.assign(static_cast<std::size_t>(n_tri), 0.0);
    q95.assign(static_cast<std::size_t>(n_tri), 0.0);

    h_work_.assign(static_cast<std::size_t>(n_tri), 0.0);
    h_det_last_.assign(static_cast<std::size_t>(n_tri), 0.0);
    sort_buf_.assign(static_cast<std::size_t>(n_ensemble), 0.0);
    keff_modes_.assign(static_cast<std::size_t>(n_kept), 0.0);
    mode_energy_.assign(static_cast<std::size_t>(n_kept), 0.0);
    h_weight_.assign(static_cast<std::size_t>(n_tri), 1.0);
    soft_r_spread_.assign(static_cast<std::size_t>(n_kept), 0.0);
    soft_spatial_absmax_.assign(static_cast<std::size_t>(n_kept), 0.0);

    // All modes start active; mode_active is updated at the start of each advance().
    mode_active.assign(static_cast<std::size_t>(n_kept), true);
    n_modes_active = n_kept;

    // -------------------------------------------------------------------------
    // Parameter design.
    //
    // External path (UncertaintyEnsemble): setExternalSamples() was called
    // before initialize() — copy those samples directly.
    //
    // Internal fallback (legacy / standalone): build a deterministic LHS.
    //   mannings_mult ∈ [1 - mannings_pert, 1 + mannings_pert] — ascending
    //   rainfall_mult ∈ [1 - rainfall_pert, 1 + rainfall_pert] — independent
    //   Fisher-Yates shuffle of the same strata (seed sample_seed+1), giving
    //   near-zero rank correlation with Manning instead of the exact -1 a
    //   reversed column would give.
    // -------------------------------------------------------------------------

    mannings_mult.resize(static_cast<std::size_t>(n_ensemble));
    rainfall_mult.resize(static_cast<std::size_t>(n_ensemble));
    cd_mult.resize(static_cast<std::size_t>(n_ensemble));

    if (external_samples_set_) {
        mannings_mult = external_mann_;
        rainfall_mult = external_rain_;
    } else {
        double m_lo = 1.0 - mannings_pert;
        double m_hi = 1.0 + mannings_pert;
        double r_lo = 1.0 - rainfall_pert;
        double r_hi = 1.0 + rainfall_pert;

        for (int i = 0; i < n_ensemble; ++i) {
            double t = (static_cast<double>(i) + 0.5) / static_cast<double>(n_ensemble);
            mannings_mult[static_cast<std::size_t>(i)] = m_lo + t * (m_hi - m_lo);
        }
        const auto rainfall_t = openswmm::uncertainty::shuffledStrata(
            n_ensemble, sample_seed + 1);
        for (int i = 0; i < n_ensemble; ++i) {
            auto ui = static_cast<std::size_t>(i);
            rainfall_mult[ui] = r_lo + rainfall_t[ui] * (r_hi - r_lo);
        }
    }

    // Cd multiplier: external path (setCdSamples) or internal ascending LHS.
    // Production path always uses setCdSamples(ens.cdSamples()) for decorrelation.
    // Default cd_pert=0 → all entries are 1.0 (no Cd uncertainty).
    if (external_cd_set_) {
        cd_mult = external_cd_;
    } else {
        double c_lo = 1.0 - cd_pert;
        double c_hi = 1.0 + cd_pert;
        for (int i = 0; i < n_ensemble; ++i) {
            double t = (static_cast<double>(i) + 0.5) / static_cast<double>(n_ensemble);
            cd_mult[static_cast<std::size_t>(i)] = c_lo + t * (c_hi - c_lo);
        }
    }

    soft_z_.resize(static_cast<std::size_t>(n_ensemble));
    soft_u_.resize(static_cast<std::size_t>(n_ensemble));
    soft_coeff_.assign(static_cast<std::size_t>(n_ensemble), 0.0);
    soft_max_abs_coeff_ = 0.0;
    const auto soft_u = openswmm::uncertainty::shuffledStrata(n_ensemble, sample_seed + 4);
    for (int i = 0; i < n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        soft_u_[ui] = soft_u[ui];
        soft_z_[ui] = openswmm::uncertainty::probit(soft_u[ui]);
    }

}

// ============================================================================
// seed
// ============================================================================

void SpectralROM::seed(const double* h_full) {
    assert(is_ready());

    // Deviation form: every member starts exactly on the deterministic depth
    // field (δa = 0). h_full primes h_det_last_ for applyCouplingFlux() and
    // computeQuantiles() until the next advance() supplies a fresh reference.
    std::fill(a_ensemble.begin(), a_ensemble.end(), 0.0);
    h_det_last_.assign(h_full, h_full + static_cast<std::size_t>(n_tri));
}

// ============================================================================
// advance
// ============================================================================

void SpectralROM::advance(double dt, double K_eff, const double* rainfall,
                          const double* h_cell, const double* h_det) {
    assert(is_ready());
    assert(h_det != nullptr);

    auto nt = static_cast<std::size_t>(n_tri);
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

    // ---- Step 2: Project rainfall and deterministic depth into coarse space -
    const double* loc_field = soft_loc_field_ ? soft_loc_field_ : rainfall;
    std::fill(r_coarse.begin(), r_coarse.end(), 0.0);
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis->P[j * nt];
        double dot_b = 0.0;
        for (std::size_t t = 0; t < nt; ++t)
            dot_b += Pj[t] * h_det[t];
        b_coarse[j] = dot_b;
        if (loc_field) {
            double dot_r = 0.0;
            for (std::size_t t = 0; t < nt; ++t)
                dot_r += Pj[t] * loc_field[t];
            r_coarse[j] = dot_r;
        }
    }
    if (soft_spread_field_) {
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nt];
            double dot = 0.0;
            for (std::size_t t = 0; t < nt; ++t)
                dot += Pj[t] * soft_spread_field_[t];
            soft_r_spread_[j] = dot;
        }
    } else {
        std::fill(soft_r_spread_.begin(), soft_r_spread_.end(), 0.0);
    }
    // CL-1c: correlated-coherence per-member projection. When a spatial field
    // is active, the comonotone factorization c_i·(Pᵀspread)_j no longer holds;
    // each member gets its own R_{ij} = Σ_t P_j[t]·spread[t]·W_i[t] (O(M·k·n)).
    // A constant field (W_i[t] = c_i ∀t) reduces this exactly to
    // c_i·soft_r_spread_[j], so the comonotone limit is bit-identical.
    const bool use_soft_reduced = (soft_reduced_psi_ != nullptr) &&
                                  (soft_reduced_a_ != nullptr) &&
                                  (soft_reduced_ks_ > 0) &&
                                  (soft_spread_field_ != nullptr);
    const bool use_soft_spatial = !use_soft_reduced &&
                                  (soft_field_ != nullptr) &&
                                  soft_field_->is_spatial() &&
                                  (soft_spread_field_ != nullptr);
    // Either spatial path fills soft_r_spread_spatial_ / soft_spatial_absmax_;
    // downstream mode-activation and forcing use this combined flag.
    const bool use_soft_rij = use_soft_spatial || use_soft_reduced;
    if (use_soft_spatial) {
        assert(soft_field_->n_members == n_ensemble);
        assert(static_cast<std::size_t>(soft_field_->n_cells) == nt);
        soft_r_spread_spatial_.assign(
            static_cast<std::size_t>(n_ensemble) * nk, 0.0);
        std::fill(soft_spatial_absmax_.begin(), soft_spatial_absmax_.end(), 0.0);
        for (int i = 0; i < n_ensemble; ++i) {
            const double* Wi = &soft_field_->values[
                static_cast<std::size_t>(i) * nt];
            double* Ri = &soft_r_spread_spatial_[
                static_cast<std::size_t>(i) * nk];
            for (std::size_t j = 0; j < nk; ++j) {
                const double* Pj = &basis->P[j * nt];
                double dot = 0.0;
                for (std::size_t t = 0; t < nt; ++t)
                    dot += Pj[t] * soft_spread_field_[t] * Wi[t];
                Ri[j] = dot;
                const double ad = std::abs(dot);
                if (ad > soft_spatial_absmax_[j]) soft_spatial_absmax_[j] = ad;
            }
        }
    } else if (use_soft_reduced) {
        // CL-2c reduced-basis projection. ψ_m already folds in the per-point
        // normalization g(t) (SpdeSpatialBasis::normalizedModes — the "seam"),
        // so R_{ij} = Σ_m a_im·(Σ_t P_j[t]·spread[t]·ψ_m[t]) reproduces the
        // materialized field's R_{ij} up to summation order.
        const auto Ks = static_cast<std::size_t>(soft_reduced_ks_);
        soft_reduced_rm_.assign(Ks * nk, 0.0);
        for (std::size_t m = 0; m < Ks; ++m) {
            const double* psim = soft_reduced_psi_ + m * nt;
            double* Rm = &soft_reduced_rm_[m * nk];
            for (std::size_t j = 0; j < nk; ++j) {
                const double* Pj = &basis->P[j * nt];
                double dot = 0.0;
                for (std::size_t t = 0; t < nt; ++t)
                    dot += Pj[t] * soft_spread_field_[t] * psim[t];
                Rm[j] = dot;
            }
        }
        soft_r_spread_spatial_.assign(
            static_cast<std::size_t>(n_ensemble) * nk, 0.0);
        std::fill(soft_spatial_absmax_.begin(), soft_spatial_absmax_.end(), 0.0);
        for (int i = 0; i < n_ensemble; ++i) {
            const double* ai = soft_reduced_a_ +
                               static_cast<std::size_t>(i) * Ks;
            double* Ri = &soft_r_spread_spatial_[
                static_cast<std::size_t>(i) * nk];
            for (std::size_t j = 0; j < nk; ++j) {
                double s = 0.0;
                for (std::size_t m = 0; m < Ks; ++m)
                    s += ai[m] * soft_reduced_rm_[m * nk + j];
                Ri[j] = s;
                const double ad = std::abs(s);
                if (ad > soft_spatial_absmax_[j]) soft_spatial_absmax_[j] = ad;
            }
        }
    }
    // Registered FORCING_VECTOR fields (PR 9b): re-project each per call.
    for (auto& ep : extra_params_) {
        if (ep.entry != openswmm::uncertainty::ParamEntry::FORCING_VECTOR) continue;
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nt];
            double dot = 0.0;
            for (std::size_t t = 0; t < nt; ++t)
                dot += Pj[t] * ep.field[t];
            ep.rv[j] = dot;
        }
    }

    // Per-member effective multipliers from registered extra params
    // (PARAMETER_REGISTRY.md §5). Products over an empty list are exactly 1.0,
    // so the no-extra-params path is bit-identical to the built-in behavior.
    auto rate_mult_prod = [this](std::size_t ui) {
        double prod = 1.0;
        for (const auto& ep : extra_params_)
            if (ep.entry == openswmm::uncertainty::ParamEntry::RATE_MULT)
                prod *= ep.column[ui];
        return prod;
    };
    auto forcing_mult_prod = [this](std::size_t ui) {
        double prod = 1.0;
        for (const auto& ep : extra_params_)
            if (ep.entry == openswmm::uncertainty::ParamEntry::FORCING_MULT)
                prod *= ep.column[ui];
        return prod;
    };

    // ---- Step 4 (moved up): depth-weight + nominal per-mode keff ------------
    // keff_modes_[j] is the mm=1 nominal conductance; needed both for the
    // active-set Manning bound and for the Manning-sensitivity forcing.
    const bool has_hcell = (h_cell != nullptr) && (K_eff > 0.0);
    std::fill(h_weight_.begin(), h_weight_.end(), 1.0);
    if (has_hcell) {
        double h_sum = 0.0;
        for (std::size_t t = 0; t < nt; ++t)
            h_sum += std::max(h_cell[t], 0.0);
        const double h_mean = h_sum / static_cast<double>(nt);
        if (h_mean > 1.0e-9) {
            const double inv_h53 = 1.0 / std::pow(h_mean, 5.0 / 3.0);
            for (std::size_t t = 0; t < nt; ++t) {
                double h_t = std::max(h_cell[t], 0.0);
                h_weight_[t] = std::pow(h_t, 5.0 / 3.0) * inv_h53;
            }
        }
    }
    std::fill(keff_modes_.begin(), keff_modes_.end(), K_eff);
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis->P[j * nt];
        double rq = 0.0;
        for (std::size_t t = 0; t < nt; ++t)
            rq += Pj[t] * Pj[t] * h_weight_[t];
        keff_modes_[j] = K_eff * rq;
    }

    // Flags for active paths (avoids repeated checks in the inner loop).
    const bool has_ensemble_rain = !ensemble_rainfall_.empty()
                                    && (rainfall != nullptr)
                                    && (mean_ensemble_rain_ > 1.0e-30);
    const bool use_spatial_mann = spatial_mannings.is_spatial();
    const bool use_spatial_rain = spatial_rainfall.is_spatial() && (rainfall != nullptr)
                                  && !has_ensemble_rain;

    // ---- Step 3: Active set --------------------------------------------------
    // Deviation form: a mode participates when its current deviation energy OR
    // either first-order forcing-sensitivity magnitude exceeds the drop
    // threshold. Both sensitivity scales use |factor − 1| so zero perturbation
    // ⇒ zero forcing ⇒ modes stay inactive and δa stays 0.
    //   rain scale : max_i |scale_i − 1|
    //   Manning scale : max deviation of keff_ji/keff_nom from 1, bounded by
    //     max_i,t |1/W_n − 1| (spatial) or max_i |1/mm_i − 1| (scalar).
    double max_rain_dev = 0.0;   // max_i |scale_i − 1|  (incl. FORCING_MULT extras)
    for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i) {
        const double base = has_ensemble_rain
            ? (ensemble_rainfall_[i] / mean_ensemble_rain_)
            : rainfall_mult[i];
        max_rain_dev = std::max(max_rain_dev,
                                std::abs(base * forcing_mult_prod(i) - 1.0));
    }
    double max_mann_dev = 0.0;   // max |1/(w·Πθ) − 1|  (incl. RATE_MULT extras)
    {
        // Extremes of the extra RATE_MULT product across members (1.0 when none).
        double p_min = 1.0, p_max = 1.0;
        if (!extra_params_.empty()) {
            p_min = p_max = rate_mult_prod(0);
            for (std::size_t i = 1; i < static_cast<std::size_t>(n_ensemble); ++i) {
                const double p = rate_mult_prod(i);
                p_min = std::min(p_min, p);
                p_max = std::max(p_max, p);
            }
        }
        if (use_spatial_mann) {
            // 1/(w·p) is monotone in both factors, so the extremes of w and p
            // bound the full cross product exactly.
            double w_min = spatial_mannings.values[0], w_max = w_min;
            for (const double w : spatial_mannings.values) {
                w_min = std::min(w_min, w);
                w_max = std::max(w_max, w);
            }
            max_mann_dev = std::max(std::abs(1.0 / (w_min * p_min) - 1.0),
                                    std::abs(1.0 / (w_max * p_max) - 1.0));
        } else {
            for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
                max_mann_dev = std::max(max_mann_dev,
                    std::abs(1.0 / (mannings_mult[i] * rate_mult_prod(i)) - 1.0));
        }
    }
    // Per-FORCING_VECTOR-param max deviation max_i |θ_i − 1|.
    for (auto& ep : extra_params_) {
        if (ep.entry != openswmm::uncertainty::ParamEntry::FORCING_VECTOR) continue;
        ep.max_dev = 0.0;
        for (double th : ep.column)
            ep.max_dev = std::max(ep.max_dev, std::abs(th - 1.0));
    }
    const double rain_scale = (loc_field != nullptr)
                               ? std::abs(dt) * max_rain_dev : 0.0;
    const double mann_scale = std::abs(dt) * max_mann_dev;
    const double soft_scale = (soft_spread_field_ != nullptr)
                               ? (use_soft_rij ? std::abs(dt)
                                               : std::abs(dt) * soft_max_abs_coeff_)
                               : 0.0;

    n_modes_active = 0;
    for (std::size_t j = 0; j < nk; ++j) {
        const double lam = basis->eigenvalues[j];
        bool by_energy  = mode_energy_[j] >= mode_drop_threshold;
        bool by_rain    = rain_scale > 0.0 &&
                          std::abs(r_coarse[j]) * rain_scale >= mode_drop_threshold;
        bool by_manning = mann_scale > 0.0 &&
                          lam * keff_modes_[j] * std::abs(b_coarse[j]) * mann_scale
                              >= mode_drop_threshold;
        bool by_soft    = soft_scale > 0.0 &&
                          (use_soft_rij ? soft_spatial_absmax_[j]
                                        : std::abs(soft_r_spread_[j])) * soft_scale
                              >= mode_drop_threshold;
        bool by_vector  = false;
        for (const auto& ep : extra_params_) {
            if (ep.entry == openswmm::uncertainty::ParamEntry::FORCING_VECTOR &&
                ep.max_dev * std::abs(dt) * std::abs(ep.rv[j]) >= mode_drop_threshold) {
                by_vector = true;
                break;
            }
        }
        mode_active[j] = by_energy || by_rain || by_manning || by_soft || by_vector;
        if (mode_active[j]) ++n_modes_active;
    }

    // ---- Step 5, reduced-operator path (matrix exponential) ------------------
    // d(δa_i)/dt = −(M/mm_i)·δa_i − (1/mm_i − 1)·M·b + (f_i − r) + soft + vector.
    // The forcing terms are assembled exactly as on the diagonal path; only the
    // decay operator and its Manning sensitivity generalize from diag(λ·keff)
    // to the full projected matrix. Modes couple through M, so the per-mode
    // active-set optimization does not apply: all modes advance.
    const bool has_extra = !extra_params_.empty();

    if (hasReducedOperator() && !use_spatial_mann) {
        const auto& Mr = reduced_M_;

        // Shared across members: Mb = M · b_coarse.
        for (std::size_t p = 0; p < nk; ++p) {
            double dot = 0.0;
            for (std::size_t q = 0; q < nk; ++q)
                dot += Mr[p * nk + q] * b_coarse[q];
            reduced_Mb_[p] = dot;
        }

        for (std::size_t j = 0; j < nk; ++j) mode_active[j] = true;
        n_modes_active = n_kept;

        for (int i = 0; i < n_ensemble; ++i) {
            auto ui = static_cast<std::size_t>(i);
            double* ai = &a_ensemble[ui * nk];
            const double rate_prod    = rate_mult_prod(ui);
            const double forcing_prod = forcing_mult_prod(ui);
            const double mm = mannings_mult[ui] * rate_prod;
            const double rm = rainfall_mult[ui];
            const double s  = (mm > 1.0e-12) ? 1.0 / mm : 1.0;

            const double* W_r_i = use_spatial_rain
                ? (spatial_rainfall.values.data() + ui * nt) : nullptr;

            for (std::size_t j = 0; j < nk; ++j) {
                // Rainfall forcing — identical to the diagonal path.
                double fj;
                if (has_ensemble_rain) {
                    fj = r_coarse[j] * (ensemble_rainfall_[ui] / mean_ensemble_rain_);
                } else if (W_r_i) {
                    const double* Pj = &basis->P[j * nt];
                    fj = 0.0;
                    for (std::size_t t = 0; t < nt; ++t)
                        fj += Pj[t] * rainfall[t] * W_r_i[t];
                } else {
                    fj = r_coarse[j] * rm;
                }
                fj *= forcing_prod;

                double g = -(s - 1.0) * reduced_Mb_[j] + (fj - r_coarse[j]);
                if (soft_spread_field_) {
                    if (use_soft_rij)
                        g += soft_r_spread_spatial_[ui * nk + j];
                    else
                        g += soft_coeff_[ui] * soft_r_spread_[j];
                }
                if (has_extra) {
                    for (const auto& ep : extra_params_)
                        if (ep.entry == openswmm::uncertainty::ParamEntry::FORCING_VECTOR)
                            g += (ep.column[ui] - 1.0) * ep.rv[j];
                }
                reduced_g_[j] = g;
            }

            DeviationOperator2D::propagate(Mr, n_kept, s, dt,
                                           ai, reduced_g_.data());
        }

        h_det_last_.assign(h_det, h_det + nt);
        return;
    }

    // ---- Step 5: Advance active deviations (exact exponential step) ----------
    //   rate = λ_j · keff_ji
    //   g    = −λ_j·(keff_ji − keff_modes_[j])·b_j + (f_ij − r_coarse[j])
    const double rate_floor = 1.0e-12;

    for (int i = 0; i < n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double* ai = &a_ensemble[ui * nk];
        const double rate_prod    = rate_mult_prod(ui);     // extra RATE_MULT product
        const double forcing_prod = forcing_mult_prod(ui);  // extra FORCING_MULT product
        double mm = mannings_mult[ui] * rate_prod;  // scalar path effective multiplier
        double rm = rainfall_mult[ui];              // scalar fallback

        // Pointer to this member's spatial fields (null if scalar path).
        const double* W_n_i = use_spatial_mann
            ? (spatial_mannings.values.data() + ui * nt) : nullptr;
        const double* W_r_i = use_spatial_rain
            ? (spatial_rainfall.values.data() + ui * nt) : nullptr;

        for (std::size_t j = 0; j < nk; ++j) {
            if (!mode_active[j]) continue;
            double lam = basis->eigenvalues[j];
            const double* Pj = &basis->P[j * nt];

            // --- effective decay rate for mode j, member i ---
            double keff_ji;
            if (W_n_i) {
                double rq = 0.0;
                for (std::size_t t = 0; t < nt; ++t)
                    rq += Pj[t] * Pj[t] * h_weight_[t] / W_n_i[t];
                keff_ji = K_eff * rq / rate_prod;
            } else {
                keff_ji = keff_modes_[j] / mm;
            }

            // --- member rainfall forcing f_ij (× extra FORCING_MULT product) ---
            double fj;
            if (has_ensemble_rain) {
                fj = r_coarse[j] * (ensemble_rainfall_[ui] / mean_ensemble_rain_);
            } else if (W_r_i) {
                fj = 0.0;
                for (std::size_t t = 0; t < nt; ++t)
                    fj += Pj[t] * rainfall[t] * W_r_i[t];
            } else {
                fj = r_coarse[j] * rm;
            }
            fj *= forcing_prod;

            // --- deviation forcing g and exact exponential integrator ---
            const double rate = lam * keff_ji;
            double g          = -lam * (keff_ji - keff_modes_[j]) * b_coarse[j]
                                + (fj - r_coarse[j]);
            if (soft_spread_field_) {
                if (use_soft_rij)
                    g += soft_r_spread_spatial_[ui * nk + j];
                else
                    g += soft_coeff_[ui] * soft_r_spread_[j];
            }
            if (has_extra) {
                for (const auto& ep : extra_params_)
                    if (ep.entry == openswmm::uncertainty::ParamEntry::FORCING_VECTOR)
                        g += (ep.column[ui] - 1.0) * ep.rv[j];
            }

            if (rate > rate_floor) {
                const double steady = g / rate;
                ai[j] = (ai[j] - steady) * std::exp(-rate * dt) + steady;
            } else {
                ai[j] += g * dt;
            }
        }
    }

    h_det_last_.assign(h_det, h_det + nt);
}

// ============================================================================
// computeQuantiles
// ============================================================================

void SpectralROM::computeQuantiles(const double* h_det, bool parametric_tails) {
    assert(is_ready());
    assert(h_det != nullptr);

    auto nt = static_cast<std::size_t>(n_tri);
    auto nk = static_cast<std::size_t>(n_kept);

    // Compute quantile indices (into sorted n_ensemble values)
    // For M members, the p-th quantile index = floor(p * (M-1) + 0.5) (nearest rank)
    double M = static_cast<double>(n_ensemble);
    int idx05 = std::max(0, static_cast<int>(0.05 * (M - 1.0) + 0.5));
    int idx50 = static_cast<int>(0.50 * (M - 1.0) + 0.5);
    int idx95 = std::min(n_ensemble - 1, static_cast<int>(0.95 * (M - 1.0) + 0.5));

    for (std::size_t t = 0; t < nt; ++t) {
        const double h_det_t = h_det[t];
        // Reconstruct depth at cell t for all members: h_det + P·δa, floored at 0.
        for (int i = 0; i < n_ensemble; ++i) {
            const double* ai = &a_ensemble[static_cast<std::size_t>(i) * nk];
            double h = h_det_t;
            for (std::size_t j = 0; j < nk; ++j) {
                if (!mode_active[j]) continue;
                h += basis->P[j * nt + t] * ai[j];
            }
            sort_buf_[static_cast<std::size_t>(i)] = std::max(h, 0.0);
        }

        // Sort to extract quantiles
        std::sort(sort_buf_.begin(), sort_buf_.end());

        q05[t] = sort_buf_[static_cast<std::size_t>(idx05)];
        q50[t] = sort_buf_[static_cast<std::size_t>(idx50)];
        q95[t] = sort_buf_[static_cast<std::size_t>(idx95)];

        // Log-normal parametric upper tail: uses all M members to estimate
        // distribution, reducing sensitivity to the single observed maximum
        // when M is small.  Only replaces q95; q05/q50 remain sort-based.
        if (parametric_tails) {
            constexpr double DRY_THRESH = 1.0e-4;
            double sum_log = 0.0, sum_log2 = 0.0;
            int n_wet = 0;
            for (int i = 0; i < n_ensemble; ++i) {
                double h = sort_buf_[static_cast<std::size_t>(i)];
                if (h > DRY_THRESH) {
                    double lh = std::log(h);
                    sum_log  += lh;
                    sum_log2 += lh * lh;
                    ++n_wet;
                }
            }
            if (n_wet >= 4) {
                double mu    = sum_log / n_wet;
                double var   = std::max(sum_log2 / n_wet - mu * mu, 0.0);
                double sigma = std::sqrt(var);
                if (sigma > 1.0e-10) {
                    constexpr double Z95 = 1.6449;
                    q95[t] = std::exp(mu + Z95 * sigma);
                }
            }
        }
    }
}

// ============================================================================
// applyCouplingFlux
// ============================================================================

void SpectralROM::applyCouplingFlux(
    const std::vector<CouplingPoint>& cps,
    const double* node_heads,
    const MeshData& mesh,
    double dt,
    const openswmm::uncertainty::SpectralROM1D* rom1d,
    const double* q_det)
{
    if (!is_ready() || cps.empty() || dt <= 0.0) return;

    auto nt = static_cast<std::size_t>(n_tri);
    auto nk = static_cast<std::size_t>(n_kept);

    constexpr double G2     = 2.0 * 9.80665;
    constexpr double dh_eps = 1.0e-12;

    // Initialise diagnostic output: 0.0 for all coupling points (outfalls stay 0).
    const auto n_cps = cps.size();
    coupling_unc_output.q_min.assign(n_cps, 0.0);
    coupling_unc_output.q_max.assign(n_cps, 0.0);

    for (std::size_t cp_idx = 0; cp_idx < n_cps; ++cp_idx) {
        const auto& cp = cps[cp_idx];
        if (cp.is_outfall) continue;

        auto   ci       = static_cast<std::size_t>(cp.cell_idx);
        double cell_z   = mesh.tri_cz[ci];
        double tri_area = mesh.tri_area[ci];
        if (tri_area < 1.0e-30) continue;

        // Orifice discharge (no Manning dependence): Q = Cd·mult · A · sign(dh)·sqrt(2g|dh|),
        // capped by the draining depth so it cannot pull a cell below zero.
        auto orifice_Q = [&](double h_2d, double h_1d, double cd_mult_i) -> double {
            double dh = (cell_z + h_2d) - h_1d;
            if (std::abs(dh) < dh_eps) return 0.0;
            double sign = (dh > 0.0) ? 1.0 : -1.0;
            double Q = sign * cp.cd * cd_mult_i * cp.area * std::sqrt(G2 * std::abs(dh));
            if (Q > 0.0) Q = std::min(Q, h_2d * tri_area / dt);
            return Q;
        };

        // Deterministic reference flux. The deterministic exchange is already
        // inside h_det via the coupling pipeline, so only Q_i − Q_det is applied
        // to each member. Prefer the caller's measured value — what the
        // integrator actually booked — over re-deriving it from the orifice
        // formula, which ignores the clamps the integrator applied. See the
        // q_det note on the declaration.
        const double h_2d_det = std::max(h_det_last_[ci], 0.0);
        const double h_1d_det = node_heads[cp.node_idx];
        const double Q_det    = q_det ? q_det[cp_idx]
                                      : orifice_Q(h_2d_det, h_1d_det, 1.0);

        // Pre-compute rom1d active index for this coupling node (if 1D ROM active).
        const bool use_rom1d = rom1d && rom1d->is_ready()
                               && !rom1d->full_to_active.empty()
                               && cp.node_idx >= 0
                               && cp.node_idx < static_cast<int>(rom1d->full_to_active.size());
        const int active_1d_idx = use_rom1d
                                  ? rom1d->full_to_active[static_cast<std::size_t>(cp.node_idx)]
                                  : -1;

        double q_min_cp = std::numeric_limits<double>::max();
        double q_max_cp = std::numeric_limits<double>::lowest();

        for (int i = 0; i < n_ensemble; ++i) {
            auto    ui = static_cast<std::size_t>(i);
            double* ai = &a_ensemble[ui * nk];

            // Per-member 1D head: ROM1D reconstruction when available, else the
            // shared deterministic head (outfalls / unregistered nodes).
            double h_1d_abs = (active_1d_idx >= 0)
                              ? rom1d->reconstructHead(i, active_1d_idx)
                              : node_heads[cp.node_idx];

            // Reconstruct per-member depth at the coupling cell: h_det + P·δa.
            double h_2d = h_2d_det;
            for (std::size_t j = 0; j < nk; ++j)
                h_2d += basis->P[j * nt + ci] * ai[j];
            h_2d = std::max(h_2d, 0.0);

            const double Q = orifice_Q(h_2d, h_1d_abs, cd_mult[ui]);

            // Track absolute per-member flux bounds (diagnostic output).
            if (Q < q_min_cp) q_min_cp = Q;
            if (Q > q_max_cp) q_max_cp = Q;

            // Apply only the deviation from the deterministic flux to δa.
            const double delta_h = -(Q - Q_det) * dt / tri_area;
            for (std::size_t j = 0; j < nk; ++j)
                ai[j] += basis->P[j * nt + ci] * delta_h;
        }

        // Commit bounds for this coupling point (guard against no-flux case).
        if (q_min_cp <= q_max_cp) {
            coupling_unc_output.q_min[cp_idx] = q_min_cp;
            coupling_unc_output.q_max[cp_idx] = q_max_cp;
        }
    }
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
