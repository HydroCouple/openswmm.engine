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

void SpectralROM1D::setSoftForcing(const double* loc, const double* spread,
                                  DistType family,
                                  const SoftSpatialField* soft_field) noexcept {
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
    // Select the per-member coefficient c_i by family. UNIFORM uses the raw
    // half-range band (2u-1); NORMAL/LOGNORMAL use the standard-normal quantile
    // z_i (LOGNORMAL via the delta linearization, caller scales spread by loc).
    soft_max_abs_coeff_ = 0.0;
    for (int i = 0; i < n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double c = (family == DistType::UNIFORM)
            ? (2.0 * soft_u_[ui] - 1.0)
            : soft_z_[ui];
        soft_coeff_[ui] = c;
        soft_max_abs_coeff_ = std::max(soft_max_abs_coeff_, std::abs(c));
    }

    // CL-1b: when a spatial field is supplied it must be dimensionally
    // consistent and its per-node column mean must match the scalar-coefficient
    // mean c̄, so the nominal member stays zero-deviation and q50 tracks the
    // deterministic answer. Checked only in debug builds (config-time cost).
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

void SpectralROM1D::clearSoftForcing() noexcept {
    soft_loc_field_ = nullptr;
    soft_spread_field_ = nullptr;
    soft_field_ = nullptr;
    soft_reduced_psi_ = nullptr;
    soft_reduced_a_ = nullptr;
    soft_reduced_ks_ = 0;
}

void SpectralROM1D::setSoftForcingReduced(const double* loc, const double* spread,
                                          DistType family,
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

void SpectralROM1D::addRegisteredParam(ParamEntry entry,
                                       const std::vector<double>& column,
                                       const double* field) {
    if (static_cast<int>(column.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM1D::addRegisteredParam: column size must equal n_ensemble");
    if (entry == ParamEntry::FORCING_VECTOR && field == nullptr)
        throw std::invalid_argument(
            "SpectralROM1D::addRegisteredParam: FORCING_VECTOR requires a field");
    ExtraParam ep;
    ep.entry  = entry;
    ep.column = column;
    ep.field  = field;
    ep.rv.assign(static_cast<std::size_t>(n_kept), 0.0);
    extra_params_.push_back(std::move(ep));
}

void SpectralROM1D::clearRegisteredParams() {
    extra_params_.clear();
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
    soft_r_spread_.assign(static_cast<std::size_t>(n_kept), 0.0);
    soft_spatial_absmax_.assign(static_cast<std::size_t>(n_kept), 0.0);

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

    soft_z_.resize(static_cast<std::size_t>(n_ensemble));
    soft_u_.resize(static_cast<std::size_t>(n_ensemble));
    soft_coeff_.assign(static_cast<std::size_t>(n_ensemble), 0.0);
    soft_max_abs_coeff_ = 0.0;
    const auto soft_u = shuffledStrata(n_ensemble, sample_seed + 4);
    for (int i = 0; i < n_ensemble; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        soft_u_[ui] = soft_u[ui];
        soft_z_[ui] = probit(soft_u[ui]);
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
                             const double* runoff_per_node,
                             const double* sens_ref,
                             const double* alpha) {
    assert(is_ready());
    assert(h_det_active != nullptr);

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    // Manning-sensitivity reference: the field whose modal content the
    // (mm−1)-scaled steady state tracks. Default = h_det; the engine passes
    // depth (head − invert) so roughness sensitivity acts on conveyance, not
    // on the immovable invert relief (PR 10 finding, VALIDATION.md).
    const double* bref = sens_ref ? sens_ref : h_det_active;

    // ---- Step 1: Deviation mode energy E_j = mean_i(δa²_{i,j}) --------------
    for (std::size_t j = 0; j < nk; ++j) {
        double sum_sq = 0.0;
        for (int i = 0; i < n_ensemble; ++i) {
            double aij = a_ensemble[static_cast<std::size_t>(i) * nk + j];
            sum_sq += aij * aij;
        }
        mode_energy_[j] = sum_sq / static_cast<double>(n_ensemble);
    }

    // ---- Step 2: Project sensitivity reference and runoff into coarse space -
    // PR H5: alpha (per-active-node, [0,1]) folds elementwise into bref
    // BEFORE projection, damping only the Manning-sensitivity channel in
    // surcharged regime. alpha==nullptr is bit-identical to alpha[i]==1 for
    // every i (no attenuation) -- the pre-H5 code path exactly. Do NOT apply
    // alpha to the r_coarse/soft_r_spread_ projections below: those are the
    // forcing-sensitivity channel and must stay untouched (H5 spec).
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis->P[j * nn];
        double dot_b = 0.0;
        for (std::size_t i = 0; i < nn; ++i) {
            const double a = alpha ? alpha[i] : 1.0;
            dot_b += Pj[i] * (a * bref[i]);
        }
        b_coarse[j] = dot_b;
    }
    const double* loc_field = soft_loc_field_ ? soft_loc_field_ : runoff_per_node;
    if (loc_field) {
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nn];
            double dot = 0.0;
            for (std::size_t i = 0; i < nn; ++i)
                dot += Pj[i] * loc_field[i];
            r_coarse[j] = dot;
        }
    } else {
        std::fill(r_coarse.begin(), r_coarse.end(), 0.0);
    }
    if (soft_spread_field_) {
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nn];
            double dot = 0.0;
            for (std::size_t i = 0; i < nn; ++i)
                dot += Pj[i] * soft_spread_field_[i];
            soft_r_spread_[j] = dot;
        }
    } else {
        std::fill(soft_r_spread_.begin(), soft_r_spread_.end(), 0.0);
    }
    // CL-1b: correlated-coherence per-member projection. When a spatial field
    // is active, the comonotone factorization c_i·(Pᵀspread)_j no longer holds;
    // each member gets its own R_{ij} = Σ_t P_j[t]·spread[t]·W_i[t] (the
    // O(M·k·n) path). A constant field (W_i[t] = c_i ∀t) reduces this exactly
    // to c_i·soft_r_spread_[j], so the comonotone limit is bit-identical.
    const bool soft_reduced = (soft_reduced_psi_ != nullptr) &&
                              (soft_reduced_a_ != nullptr) &&
                              (soft_reduced_ks_ > 0) &&
                              (soft_spread_field_ != nullptr);
    const bool soft_spatial = !soft_reduced &&
                              (soft_field_ != nullptr) &&
                              soft_field_->is_spatial() &&
                              (soft_spread_field_ != nullptr);
    // Either spatial path fills soft_r_spread_spatial_ / soft_spatial_absmax_;
    // downstream mode-activation and forcing use this combined flag.
    const bool soft_use_rij = soft_spatial || soft_reduced;
    if (soft_spatial) {
        assert(soft_field_->n_members == n_ensemble);
        assert(static_cast<std::size_t>(soft_field_->n_cells) == nn);
        soft_r_spread_spatial_.assign(
            static_cast<std::size_t>(n_ensemble) * nk, 0.0);
        std::fill(soft_spatial_absmax_.begin(), soft_spatial_absmax_.end(), 0.0);
        for (int i = 0; i < n_ensemble; ++i) {
            const double* Wi = &soft_field_->values[
                static_cast<std::size_t>(i) * nn];
            double* Ri = &soft_r_spread_spatial_[
                static_cast<std::size_t>(i) * nk];
            for (std::size_t j = 0; j < nk; ++j) {
                const double* Pj = &basis->P[j * nn];
                double dot = 0.0;
                for (std::size_t t = 0; t < nn; ++t)
                    dot += Pj[t] * soft_spread_field_[t] * Wi[t];
                Ri[j] = dot;
                const double ad = std::abs(dot);
                if (ad > soft_spatial_absmax_[j]) soft_spatial_absmax_[j] = ad;
            }
        }
    } else if (soft_reduced) {
        // CL-2c reduced-basis projection. ψ_m already folds in the per-point
        // normalization g(t) (SpdeSpatialBasis::normalizedModes — the "seam"),
        // so R_{ij} = Σ_m a_im·(Σ_t P_j[t]·spread[t]·ψ_m[t]) reproduces the
        // materialized field's R_{ij} up to summation order.
        const auto Ks = static_cast<std::size_t>(soft_reduced_ks_);
        // R_m[j] = Σ_t P_j[t]·spread[t]·ψ_m(t)  (K_s·k projections).
        soft_reduced_rm_.assign(Ks * nk, 0.0);
        for (std::size_t m = 0; m < Ks; ++m) {
            const double* psim = soft_reduced_psi_ + m * nn;
            double* Rm = &soft_reduced_rm_[m * nk];
            for (std::size_t j = 0; j < nk; ++j) {
                const double* Pj = &basis->P[j * nn];
                double dot = 0.0;
                for (std::size_t t = 0; t < nn; ++t)
                    dot += Pj[t] * soft_spread_field_[t] * psim[t];
                Rm[j] = dot;
            }
        }
        // R_{ij} = Σ_m a_im·R_m[j]  (M·K_s·k reconstruction).
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
    // Registered FORCING_VECTOR fields (PR 9b): re-project each per call
    // (the fields are live engine buffers that change every routing step).
    for (auto& ep : extra_params_) {
        if (ep.entry != ParamEntry::FORCING_VECTOR) continue;
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nn];
            double dot = 0.0;
            for (std::size_t i = 0; i < nn; ++i)
                dot += Pj[i] * ep.field[i];
            ep.rv[j] = dot;
        }
    }
    h_det_last_.assign(h_det_active, h_det_active + nn);

    // Per-member effective multipliers from registered extra params
    // (PARAMETER_REGISTRY.md §5). Products over an empty list are exactly 1.0,
    // so the no-extra-params path is bit-identical to the built-in behavior.
    auto rate_mult_prod = [this](std::size_t ui) {
        double prod = 1.0;
        for (const auto& ep : extra_params_)
            if (ep.entry == ParamEntry::RATE_MULT) prod *= ep.column[ui];
        return prod;
    };
    auto forcing_mult_prod = [this](std::size_t ui) {
        double prod = 1.0;
        for (const auto& ep : extra_params_)
            if (ep.entry == ParamEntry::FORCING_MULT) prod *= ep.column[ui];
        return prod;
    };

    // ---- Step 3: Active set ---------------------------------------------------
    // A mode participates when its current deviation energy OR either
    // first-order forcing-sensitivity magnitude exceeds the drop threshold.
    // Both forcing scales use |multiplier − 1| (deviation form): zero
    // perturbation ⇒ zero forcing ⇒ modes stay inactive and δa stays 0.
    const bool has_ensemble_runoff = !ensemble_runoff_.empty()
                                      && (runoff_per_node != nullptr)
                                      && (mean_ensemble_runoff_ > 1.0e-30);
    double max_rain_dev = 0.0;   // max_i |scale_i − 1|  (incl. FORCING_MULT extras)
    for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i) {
        const double base = has_ensemble_runoff
            ? (ensemble_runoff_[i] / mean_ensemble_runoff_)
            : runoff_mult[i];
        max_rain_dev = std::max(max_rain_dev,
                                std::abs(base * forcing_mult_prod(i) - 1.0));
    }
    double max_mann_dev = 0.0;   // max_i |1/mm_eff_i − 1|  (incl. RATE_MULT extras)
    for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
        max_mann_dev = std::max(max_mann_dev,
            std::abs(1.0 / (mannings_mult[i] * rate_mult_prod(i)) - 1.0));
    // Per-FORCING_VECTOR-param max deviation max_i |θ_i − 1|.
    for (auto& ep : extra_params_) {
        if (ep.entry != ParamEntry::FORCING_VECTOR) continue;
        ep.max_dev = 0.0;
        for (double th : ep.column)
            ep.max_dev = std::max(ep.max_dev, std::abs(th - 1.0));
    }

    const double rain_scale = (loc_field != nullptr)
                               ? std::abs(dt) * max_rain_dev : 0.0;
    const double mann_scale = std::abs(dt) * K1d * max_mann_dev;
    // Soft-forcing activation scale. Comonotone: |dt|·max_i|c_i| times
    // |soft_r_spread_[j]|. Spatial (CL-1b/CL-2c): the coefficient is already
    // folded into R_{ij}, so the scale is |dt| times max_i|R_{ij}| =
    // soft_spatial_absmax_[j].
    const double soft_scale = (soft_spread_field_ != nullptr)
                               ? (soft_use_rij ? std::abs(dt)
                                               : std::abs(dt) * soft_max_abs_coeff_)
                               : 0.0;

    n_modes_active = 0;
    for (std::size_t j = 0; j < nk; ++j) {
        const double lam = basis->eigenvalues[j];
        bool by_energy  = mode_energy_[j] >= mode_drop_threshold;
        bool by_rain    = rain_scale > 0.0 &&
                          std::abs(r_coarse[j]) * rain_scale >= mode_drop_threshold;
        bool by_manning = mann_scale > 0.0 &&
                          lam * std::abs(b_coarse[j]) * mann_scale >= mode_drop_threshold;
        bool by_soft    = soft_scale > 0.0 &&
                  (soft_use_rij ? soft_spatial_absmax_[j]
                                : std::abs(soft_r_spread_[j])) * soft_scale
                      >= mode_drop_threshold;
        bool by_vector  = false;
        for (const auto& ep : extra_params_) {
            if (ep.entry == ParamEntry::FORCING_VECTOR &&
                ep.max_dev * std::abs(dt) * std::abs(ep.rv[j]) >= mode_drop_threshold) {
                by_vector = true;
                break;
            }
        }
        mode_active[j] = by_energy || by_rain || by_manning || by_soft || by_vector;
        if (mode_active[j]) ++n_modes_active;
    }

    // ---- Step 4: Advance deviations (exact exponential step) ------------------
    //   d(δa)/dt = −rate·δa + g,   rate = λ_j·K1d/mm_i
    //   g = −λ_j·K1d·(1/mm_i − 1)·b_j + (scale_i − 1)·r_j
    const double rate_floor = 1.0e-12;

    const bool has_extra = !extra_params_.empty();

    for (int i = 0; i < n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double* ai = &a_ensemble[ui * nk];
        const double mm       = mannings_mult[ui] * rate_mult_prod(ui);
        const double inv_mm_1 = 1.0 / mm - 1.0;
        const double base     = has_ensemble_runoff
            ? (ensemble_runoff_[ui] / mean_ensemble_runoff_)
            : runoff_mult[ui];
        const double scale_1  = base * forcing_mult_prod(ui) - 1.0;

        for (std::size_t j = 0; j < nk; ++j) {
            if (!mode_active[j]) continue;

            const double lam  = basis->eigenvalues[j];
            const double rate = lam * K1d / mm;
            double g          = -lam * K1d * inv_mm_1 * b_coarse[j]
                                + scale_1 * r_coarse[j];
            if (soft_spread_field_) {
                if (soft_use_rij)
                    g += soft_r_spread_spatial_[ui * nk + j];
                else
                    g += soft_coeff_[ui] * soft_r_spread_[j];
            }
            if (has_extra) {
                for (const auto& ep : extra_params_)
                    if (ep.entry == ParamEntry::FORCING_VECTOR)
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

void SpectralROM1D::commitBasisUpdateSuccess_(const double* conduit_off, int n_conduits,
                                              const uint8_t* node_surcharged,
                                              double sim_time) {
    conduit_off_prev_.assign(conduit_off, conduit_off + n_conduits);
    if (node_surcharged != nullptr) {
        node_surcharged_prev_.assign(static_cast<std::size_t>(n_nodes), 0);
        for (int i = 0; i < n_full_nodes && i < static_cast<int>(full_to_active.size()); ++i) {
            const int ai = full_to_active[static_cast<std::size_t>(i)];
            if (ai >= 0) {
                node_surcharged_prev_[static_cast<std::size_t>(ai)] =
                    node_surcharged[i] ? 1 : 0;
            }
        }
    }
    last_basis_update_time_ = sim_time;
}

void SpectralROM1D::updateBasis(const double* conduit_off, const int* conduit_n1,
                                 const int* conduit_n2, int n_conduits,
                                 double sim_time,
                                 const uint8_t* node_surcharged) {
    if (!is_ready() || n_full_nodes < 4 || n_conduits <= 0) return;

    // --- Time-interval guard --------------------------------------------
    if (sim_time - last_basis_update_attempt_time_ < basis_update_interval) return;

    // --- PR H1: cold-restart triggers, evaluated against the PREVIOUS
    // SUCCESSFUL rebuild's baseline. A failed rebuild must still back off
    // via last_basis_update_attempt_time_, but it must NOT erase the last
    // known-good baseline: if the basis is still pre-transition, the next
    // successful retry must remain eligible for a forced-cold restart.
    bool force_cold = false;

    // Primary trigger: surcharge-state transitions on active nodes.
    if (node_surcharged != nullptr &&
        static_cast<int>(node_surcharged_prev_.size()) == n_nodes) {
        int n_active_checked = 0, n_flipped = 0;
        for (int i = 0; i < n_full_nodes && i < static_cast<int>(full_to_active.size()); ++i) {
            const int ai = full_to_active[static_cast<std::size_t>(i)];
            if (ai < 0) continue;  // outfall, not part of the active set
            const uint8_t cur  = node_surcharged[i] ? 1 : 0;
            const uint8_t prev = node_surcharged_prev_[static_cast<std::size_t>(ai)];
            ++n_active_checked;
            if (cur != prev) ++n_flipped;
        }
        if (n_active_checked > 0 &&
            static_cast<double>(n_flipped) / static_cast<double>(n_active_checked)
                > surcharge_flip_frac_threshold) {
            force_cold = true;
        }
    }

    // Secondary trigger: per-edge dqdh drift (conduit_off is proportional to
    // dqdh, same convention already used by the skip criterion below).
    if (!force_cold && !conduit_off_prev_.empty() &&
        static_cast<int>(conduit_off_prev_.size()) == n_conduits) {
        int n_jumped = 0;
        for (int ci = 0; ci < n_conduits; ++ci) {
            auto uci = static_cast<std::size_t>(ci);
            const double prev = conduit_off_prev_[uci];
            const double r_e = std::abs(conduit_off[ci] - prev) / (std::abs(prev) + 1.0e-12);
            if (r_e > edge_drift_ratio_threshold) ++n_jumped;
        }
        if (static_cast<double>(n_jumped) / static_cast<double>(n_conduits)
                > edge_drift_frac_threshold) {
            force_cold = true;
        }
    }

    // --- Skip criterion ---------------------------------------------------
    // A forced-cold condition must never be silently skipped here: surcharge
    // onset / large dqdh jumps are exactly the events this tolerance guard
    // would otherwise misclassify as "nothing changed enough to bother".
    if (!force_cold && !conduit_off_prev_.empty() &&
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
    // Every exit below — success or failure — must stamp
    // last_basis_update_attempt_time_ so a persistently failing rebuild
    // backs off for basis_update_interval instead of retrying full Lanczos
    // on every routing step. Only a SUCCESSFUL rebuild may advance the
    // warm/cold comparison baseline.
    ++basis_updates_attempted_;
    last_basis_update_attempt_time_ = sim_time;

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
        ++basis_updates_failed_;
        return;
    }

    // --- Warm-start re-solve (or cold, if PR H1's triggers fired) --------
    std::vector<double> P_old = basis->P;  // column-major n_nodes × n_kept copy

    if (!basis_owned_) {
        basis_owned_ = std::make_unique<GraphEigenBasis>();
        basis_owned_->null_tol = basis->null_tol;
    }
    if (!basis_owned_->build(L_new, n_kept, force_cold ? nullptr : P_old.data())) {
        ++basis_updates_failed_;
        return;
    }
    // Sanity check: basis must have exactly n_kept modes (Lanczos must not have
    // broken down prematurely — the combo v0 starting vector prevents this for
    // normal networks, but guard against degenerate geometry).
    if (basis_owned_->num_kept != n_kept) {
        ++basis_updates_failed_;
        return;
    }

    if (force_cold) ++basis_rebuilds_cold_forced_;

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

    commitBasisUpdateSuccess_(conduit_off, n_conduits, node_surcharged, sim_time);
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
