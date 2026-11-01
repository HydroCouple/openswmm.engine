/**
 * @file SpectralROM1D.cpp
 * @brief Spectral ROM for 1D network uncertainty propagation — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "SpectralROM1D.hpp"
#include "RomDensePropagator.hpp"
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
// softCoefficients / registered parameters
// ============================================================================

std::vector<double> SpectralROM1D::softCoefficients(DistType family) const {
    // One rank per member, u_i = shuffledStrata(M, sample_seed + 4), shared by
    // every family: UNIFORM gives the half-range band 2u - 1, NORMAL and
    // LOGNORMAL the standard-normal quantile z = probit(u) (LOGNORMAL through
    // the delta linearization; the caller scales the spread by its location).
    std::vector<double> c(soft_u_.size());
    for (std::size_t i = 0; i < c.size(); ++i)
        c[i] = (family == DistType::UNIFORM) ? 2.0 * soft_u_[i] - 1.0 : soft_z_[i];
    return c;
}

void SpectralROM1D::addRegisteredParam(ParamEntry entry,
                                       const std::vector<double>& column) {
    if (static_cast<int>(column.size()) != n_ensemble)
        throw std::invalid_argument(
            "SpectralROM1D::addRegisteredParam: column size must equal n_ensemble");
    if (entry == ParamEntry::FORCING_MULT || entry == ParamEntry::FORCING_VECTOR)
        throw std::invalid_argument(
            "SpectralROM1D::addRegisteredParam: additive parameters (FORCING_MULT, "
            "FORCING_VECTOR) are carried by the engine's full-order additive channel");
    ExtraParam ep;
    ep.entry  = entry;
    ep.column = column;
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
    mass_.clear();          // H18: a fresh basis is the plain orthonormal one
    projP_.clear();
    mode_rates_.clear();

    a_ensemble.assign(static_cast<std::size_t>(n_ensemble) *
                      static_cast<std::size_t>(n_kept), 0.0);
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

    soft_z_.resize(static_cast<std::size_t>(n_ensemble));
    soft_u_.resize(static_cast<std::size_t>(n_ensemble));
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
    qsr_r_.clear();
    h_det_last_.assign(h_nodes, h_nodes + static_cast<std::size_t>(n_nodes));

    // PR H11: reset the ROM's own clock and phase-coordinate history so a
    // re-seed (dry-start reinit, or a fresh simulation) starts the ring
    // clean rather than mixing in stale planes from a prior run.
    phase_time_ = 0.0;
    hist_.reset();
    if (!tbar_.empty())
        hist_.push(phase_time_, h_nodes);
}

// ============================================================================
// advance
// ============================================================================

void SpectralROM1D::setReducedOperator(const std::vector<double>& M_in) {
    const auto nk = static_cast<std::size_t>(n_kept);
    if (!is_ready() || nk == 0 || M_in.size() != nk * nk)
        throw std::invalid_argument(
            "SpectralROM1D::setReducedOperator: M must be n_kept x n_kept and "
            "the ROM must be initialized first");
    reduced_M_ = M_in;
    reduced_Mb_.assign(nk, 0.0);
    reduced_MK_.assign(nk * nk, 0.0);
    reduced_g_.assign(nk, 0.0);
}

void SpectralROM1D::advance(double dt, double K1d,
                             const double* h_det_active,
                             const double* sens_ref,
                             const double* alpha) {
    assert(is_ready());
    assert(h_det_active != nullptr);

    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);

    // Manning-sensitivity reference: the field whose modal content the
    // (mm−1)-scaled steady state tracks. The engine passes the flow-response
    // field phi = dh/d(ln n) (H17); without one, h_det. Additive uncertainties
    // (rain, inflow) are not in the modes: the engine carries them at full
    // order and hands them over with setAdditivePlanes() (H19).
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

    // ---- Step 2: Project the sensitivity reference --------------------------
    // PR H5: alpha (per-active-node, [0,1]) folds elementwise into bref before
    // projection (used only when the flow response is unavailable; null = 1).
    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = projCol_(j);
        double dot_b = 0.0;
        for (std::size_t i = 0; i < nn; ++i) {
            const double a = alpha ? alpha[i] : 1.0;
            dot_b += Pj[i] * (a * bref[i]);
        }
        b_coarse[j] = dot_b;
    }

    auto rate_mult_prod = [this](std::size_t ui) {
        double prod = 1.0;
        for (const auto& ep : extra_params_)
            if (ep.entry == ParamEntry::RATE_MULT) prod *= ep.column[ui];
        return prod;
    };

    // Quasi-static residual: the part of the target the kept modes leave out.
    // Only for a supplied response field: the h_det fallback is an absolute
    // head, not a response.
    if (quasi_static_residual && sens_ref) {
        qsr_r_.assign(nn, 0.0);
        for (std::size_t i = 0; i < nn; ++i)
            qsr_r_[i] = (alpha ? alpha[i] : 1.0) * bref[i];
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nn];
            const double bj = b_coarse[j];
            for (std::size_t i = 0; i < nn; ++i) qsr_r_[i] -= Pj[i] * bj;
        }
        qsr_mm1_.assign(static_cast<std::size_t>(n_ensemble), 0.0);
        for (int m = 0; m < n_ensemble; ++m) {
            const auto um = static_cast<std::size_t>(m);
            qsr_mm1_[um] = mannings_mult[um] * rate_mult_prod(um) - 1.0;
        }
    } else {
        qsr_r_.clear();
    }
    h_det_last_.assign(h_det_active, h_det_active + nn);

    // ---- Step 3: Active set ---------------------------------------------------
    // A mode participates when its deviation energy or its first-order Manning
    // forcing exceeds the drop threshold. Zero perturbation => zero forcing =>
    // no mode activates and δa stays exactly 0.
    double max_mann_dev = 0.0;   // max_i |1/mm_eff_i − 1|
    for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
        max_mann_dev = std::max(max_mann_dev,
            std::abs(1.0 / (mannings_mult[i] * rate_mult_prod(i)) - 1.0));
    const double mann_scale = std::abs(dt) * K1d * max_mann_dev;
    n_modes_active = 0;
    for (std::size_t j = 0; j < nk; ++j) {
        const double lam = modeRate_(j);
        const bool by_energy  = mode_energy_[j] >= mode_drop_threshold;
        const bool by_manning = mann_scale > 0.0 &&
                                lam * std::abs(b_coarse[j]) * mann_scale >= mode_drop_threshold;
        mode_active[j] = by_energy || by_manning;
        if (mode_active[j]) ++n_modes_active;
    }

    // ---- Step 4 (PR H14), reduced-operator path -------------------------------
    //   d(δa_i)/dt = −(K1d·M/mm_i)·δa_i − K1d·(1/mm_i − 1)·M·b
    // Modes couple through M, so every mode advances.
    if (hasReducedOperator()) {
        for (std::size_t p = 0; p < nk; ++p) {
            double dot = 0.0;
            for (std::size_t q = 0; q < nk; ++q)
                dot += reduced_M_[p * nk + q] * b_coarse[q];
            reduced_Mb_[p] = dot;
        }
        // PR H14b: A = −dt·K1d·M shared by every member; the member scalar
        // s_i = 1/mm_i sits inside the exponent, so the matrix powers are
        // formed once and each member costs one (k+1)×(k+1) solve.
        for (std::size_t idx = 0; idx < nk * nk; ++idx)
            reduced_MK_[idx] = -dt * K1d * reduced_M_[idx];
        double s_max = 1.0;
        for (int i = 0; i < n_ensemble; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            const double mm = mannings_mult[ui] * rate_mult_prod(ui);
            s_max = std::max(s_max, (mm > 1.0e-12) ? 1.0 / mm : 1.0);
        }
        reduced_batch_.prepare(reduced_MK_, n_kept, s_max);
        for (std::size_t j = 0; j < nk; ++j) mode_active[j] = true;
        n_modes_active = n_kept;

        for (int i = 0; i < n_ensemble; ++i) {
            auto ui = static_cast<std::size_t>(i);
            double* ai = &a_ensemble[ui * nk];
            const double mm = mannings_mult[ui] * rate_mult_prod(ui);
            const double s  = (mm > 1.0e-12) ? 1.0 / mm : 1.0;
            for (std::size_t j = 0; j < nk; ++j)
                reduced_g_[j] = -K1d * (s - 1.0) * reduced_Mb_[j] * dt;   // forcing integral c = dt·g
            reduced_batch_.apply(s, reduced_g_.data(), ai);
        }

        phase_time_ += dt;
        if (!tbar_.empty())
            hist_.push(phase_time_, h_det_active);
        return;
    }

    // ---- Step 4: Advance deviations (exact exponential step) ------------------
    //   d(δa)/dt = −rate·δa + g,   rate = λ_j·K1d/mm_i,
    //   g = −λ_j·K1d·(1/mm_i − 1)·b_j   (fixed point (mm_i − 1)·b_j)
    const double rate_floor = 1.0e-12;
    for (int i = 0; i < n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double* ai = &a_ensemble[ui * nk];
        const double mm       = mannings_mult[ui] * rate_mult_prod(ui);
        const double inv_mm_1 = 1.0 / mm - 1.0;
        for (std::size_t j = 0; j < nk; ++j) {
            if (!mode_active[j]) continue;
            const double lam  = modeRate_(j);
            const double rate = lam * K1d / mm;
            const double g    = -lam * K1d * inv_mm_1 * b_coarse[j];
            if (rate > rate_floor) {
                const double steady = g / rate;
                ai[j] = (ai[j] - steady) * std::exp(-rate * dt) + steady;
            } else {
                ai[j] += g * dt;
            }
        }
    }

    // ---- PR H11: advance the ROM's own clock and push h_det into the
    // phase-coordinate history ring (a no-op when no travel-time field has
    // been supplied -- tbar_ stays empty until setTravelTime() is called).
    phase_time_ += dt;
    if (!tbar_.empty())
        hist_.push(phase_time_, h_det_active);
}

// ============================================================================
// setTravelTime / clearTravelTime
// ============================================================================

void SpectralROM1D::setTravelTime(const double* tbar_active) {
    auto nn = static_cast<std::size_t>(n_nodes);
    tbar_.assign(tbar_active, tbar_active + nn);

    if (hist_configured_) return;  // periodic refresh: field only, ring untouched

    // First call since construction/clearTravelTime(): size the ring from
    // this measurement. Horizon = max_i|mm_i − 1| * max_t T̄(t): the largest
    // phase offset any member can ever need to look back (or reflect
    // forward) by, given the current LHS design. Sizes the ring's push
    // spacing so that horizon stays coverable within phase_cfg.max_planes
    // regardless of the caller's (possibly adaptive) step size.
    double max_tbar = 0.0;
    for (double t : tbar_) max_tbar = std::max(max_tbar, t);
    double max_dev = 0.0;
    for (double mm : mannings_mult) max_dev = std::max(max_dev, std::fabs(mm - 1.0));
    const double horizon = max_dev * max_tbar;

    const int denom = std::max(1, phase_cfg.max_planes - 2);
    const double min_spacing = horizon / static_cast<double>(denom);

    hist_.configure(n_nodes, phase_cfg.max_planes, min_spacing);
    // Prime the ring with the most recent deterministic reference so a
    // computeQuantiles() call issued before the next advance() still has at
    // least one plane to sample from, rather than an empty ring.
    if (!h_det_last_.empty())
        hist_.push(phase_time_, h_det_last_.data());
    hist_configured_ = true;
}

void SpectralROM1D::clearTravelTime() noexcept {
    tbar_.clear();
    hist_.reset();
    hist_configured_ = false;
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
    // (nn × M), row-major: row = node. PR H4: the reconstruction (this
    // buffer's fill) is the O(N·M·k) term that dominates at large N; below
    // is either the original hand-rolled loop (rom_quantile_naive == true,
    // kept byte-for-byte as the equivalence-test baseline) or
    // reconstructEnsembleGemm() (RomQuantileGemm.hpp), which computes the
    // identical active-mode-masked sum via cblas_dgemm (or a portable
    // fallback) in one call. Everything after this block -- phase
    // reference selection, sort, quantile extraction -- is unchanged by
    // and unaware of which path filled recon_buf_.
    recon_buf_.assign(nn * M, 0.0);

    if (rom_quantile_naive) {
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
    } else {
        reconstructEnsembleGemm(basis->P.data(), a_ensemble.data(), mode_active,
                                n_nodes, n_kept, n_ensemble, recon_buf_.data(),
                                gemm_P_active_, gemm_A_active_);
    }

    // PR H11: per-member phase coordinate. Active only when a travel-time
    // field has been supplied AND phase_cfg.enabled AND the history ring
    // actually holds at least one plane (guards a configured-but-unprimed
    // ring, which should not happen via the public API but is a cheap
    // defensive check). When inactive every member's reference is
    // h_det_active[t] -- the loop below is then bit-identical to the
    // pre-H11 code path.
    // Quasi-static residual of the Manning channel, at its steady value.
    if (qsr_r_.size() == nn && qsr_mm1_.size() == M) {
        for (std::size_t t = 0; t < nn; ++t) {
            const double r = qsr_r_[t];
            if (r == 0.0) continue;
            double* row = &recon_buf_[t * M];
            for (std::size_t i = 0; i < M; ++i) row[i] += qsr_mm1_[i] * r;
        }
    }
    // PR H19: full-order additive deviations, sum_p kappa_ip * Psi_p.
    if (add_np_ > 0 && add_psi_.size() == add_np_ * nn &&
        add_kappa_.size() == M * add_np_) {
        for (std::size_t t = 0; t < nn; ++t) {
            double* row = &recon_buf_[t * M];
            for (std::size_t i = 0; i < M; ++i) {
                double v = 0.0;
                for (std::size_t p = 0; p < add_np_; ++p) v += add_kappa_[i * add_np_ + p] * add_psi_[p * nn + t];
                row[i] += v;
            }
        }
    }
    if (add_member_.size() == M * nn) {
        for (std::size_t t = 0; t < nn; ++t) {
            double* row = &recon_buf_[t * M];
            for (std::size_t i = 0; i < M; ++i) row[i] += add_member_[i * nn + t];
        }
    }

    // Spilling nodes: every member sits at the deterministic head (see
    // setPinnedNodes()). Applied after every deviation term, before the
    // reference is added, so the phase-shifted reference is also bypassed.
    if (pinned_) {
        for (std::size_t t = 0; t < nn && t < pinned_->size(); ++t)
            if ((*pinned_)[t]) std::fill_n(&recon_buf_[t * M], M, 0.0);
    }

    const bool phase_active = phase_cfg.enabled && !tbar_.empty() && !hist_.empty();

    // Add the deterministic (or, under H11, per-member phase-shifted)
    // reference, clamp to the physical floor (node invert) when supplied,
    // sort each node's row, extract quantiles.
    for (std::size_t t = 0; t < nn; ++t) {
        double* row = &recon_buf_[t * M];
        const double h_det = h_det_active[t];
        const double floor_t = invert_active ? invert_active[t] : 0.0;
        const int node_i = static_cast<int>(t);

        if (!phase_active || tbar_[t] == 0.0 || isPinned(static_cast<int>(t))) {
            // No phase field, disabled, or this node's own T̄ is zero (e.g.
            // the source of the deterministic flow) -- every member's
            // reference is h_det, exactly the pre-H11 behavior.
            if (invert_active) {
                for (std::size_t i = 0; i < M; ++i)
                    row[i] = std::max(h_det + row[i], floor_t);
            } else {
                for (std::size_t i = 0; i < M; ++i)
                    row[i] = h_det + row[i];
            }
        } else {
            const double tbar_t = tbar_[t];
            for (std::size_t i = 0; i < M; ++i) {
                const double tau = phaseOffset(mannings_mult[i], tbar_t);
                double h_ref;
                if (tau == 0.0) {
                    h_ref = h_det;  // exact short-circuit (DEVIATION_FORM invariant)
                } else if (tau > 0.0) {
                    h_ref = hist_.sample(phase_time_ - tau, node_i);
                } else {
                    // Reflection: no future history exists for a faster
                    // member (τ < 0). First-order forward extrapolation
                    // over the same interval using only past history.
                    const double h_now  = hist_.sample(phase_time_, node_i);
                    const double h_past = hist_.sample(phase_time_ + tau, node_i);  // tau<0
                    h_ref = 2.0 * h_now - h_past;
                }
                row[i] = invert_active ? std::max(h_ref + row[i], floor_t)
                                        : (h_ref + row[i]);
            }
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
                                 const uint8_t* node_surcharged,
                                 const CsrGraph* Lc,
                                 const double* storage) {
    if (!is_ready() || n_full_nodes < 4 || n_conduits <= 0) return;
    // PR H18: generalized (conductance/storage) rebuild. Forced while the
    // basis is still the dimensionless one, so P^T S P = I holds before any
    // S-weighted projection is used.
    const bool generalized = (Lc != nullptr) && (storage != nullptr) && (Lc->n == n_nodes);
    const bool force_generalized = generalized && mass_.empty();

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
    if (!force_cold && !force_generalized && !conduit_off_prev_.empty() &&
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

    CsrGraph L_new;
    if (!generalized) {
        std::vector<int> active_map_new, full_to_active_new;
        L_new = NetworkLaplacian1D::buildWeighted(
            n_full_nodes, n_conduits,
            conduit_n1, conduit_n2, is_outfall.data(), weights.data(),
            active_map_new, full_to_active_new);

        // Guard: active node count must match (topology must be stable).
        if (static_cast<int>(active_map_new.size()) != n_nodes ||
            static_cast<int>(active_map_new.size()) < 4) {
            ++basis_updates_failed_;
            return;
        }
    }

    // --- Warm-start re-solve (or cold, if PR H1's triggers fired) --------
    std::vector<double> P_old = basis->P;  // column-major n_nodes × n_kept copy

    // Build into a fresh basis and swap it in only on success. Building in
    // place overwrote the LIVE basis when `basis` already pointed at
    // basis_owned_, so a failed rebuild left the ROM reconstructing with a
    // basis of a different size than its coefficients: out-of-bounds reads,
    // run-to-run differences and divergence (found 2026-10-16 under H18).
    auto fresh = std::make_unique<GraphEigenBasis>();
    fresh->null_tol = basis->null_tol;
    const double* v0 = force_cold ? nullptr : P_old.data();
    const bool built = generalized
        ? fresh->buildGeneralized(*Lc, storage, n_kept, v0)
        : fresh->build(L_new, n_kept, v0);
    // Sanity check: basis must have exactly n_kept modes (Lanczos must not have
    // broken down prematurely — the combo v0 starting vector prevents this for
    // normal networks, but guard against degenerate geometry).
    if (!built || fresh->num_kept != n_kept) {
        ++basis_updates_failed_;
        return;
    }
    basis_owned_ = std::move(fresh);

    if (force_cold) ++basis_rebuilds_cold_forced_;

    basis = basis_owned_.get();  // switch raw pointer to new owned basis
    reduced_M_.clear();          // PR H14: M was projected on the old P; caller re-assembles

    // --- Re-project ensemble coefficients: a_new = R * a_old  ----------
    // R[j_new, j_old] = P_new[:,j_new]^T · S · P_old[:,j_old]  (S = I unless
    // the basis is the generalized one: the S-projection of P_old·a_old).
    auto nn = static_cast<std::size_t>(n_nodes);
    auto nk = static_cast<std::size_t>(n_kept);
    if (generalized) {
        mass_.assign(storage, storage + nn);
        projP_.resize(nk * nn);
        for (std::size_t j = 0; j < nk; ++j)
            for (std::size_t i = 0; i < nn; ++i)
                projP_[j * nn + i] = mass_[i] * basis->P[j * nn + i];
    } else {
        mass_.clear();
        projP_.clear();
    }
    mode_rates_.clear();

    work_R_.resize(nk * nk);
    for (std::size_t jn = 0; jn < nk; ++jn) {
        const double* Pn = projCol_(jn);
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

void SpectralROM1D::setAdditivePlanes(int n_planes, const double* psi, const double* kappa) {
    add_np_ = (n_planes > 0 && psi && kappa) ? static_cast<std::size_t>(n_planes) : 0;
    const auto nn = static_cast<std::size_t>(n_nodes);
    const auto M = static_cast<std::size_t>(n_ensemble);
    add_psi_.assign(psi, psi + add_np_ * nn);
    add_kappa_.assign(kappa, kappa + M * add_np_);
}

void SpectralROM1D::setMemberAdditive(const double* corr) {
    if (corr == nullptr) { add_member_.clear(); return; }
    const auto n = static_cast<std::size_t>(n_ensemble) * static_cast<std::size_t>(n_nodes);
    add_member_.assign(corr, corr + n);
}

void SpectralROM1D::setModeRates(const double* rates) {
    if (rates == nullptr) { mode_rates_.clear(); return; }
    mode_rates_.assign(rates, rates + n_kept);
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
    if (isPinned(active_node)) return std::max(h, 0.0);   // spilling: rim holds every member
    for (std::size_t j = 0; j < nk; ++j) {
        if (!mode_active[j]) continue;
        h += basis->P[j * nn + uk] * ai[j];
    }
    if (qsr_r_.size() == nn && ui < qsr_mm1_.size()) h += qsr_mm1_[ui] * qsr_r_[uk];
    if (add_np_ > 0 && add_psi_.size() == add_np_ * nn &&
        add_kappa_.size() == static_cast<std::size_t>(n_ensemble) * add_np_)
        for (std::size_t p = 0; p < add_np_; ++p) h += add_kappa_[ui * add_np_ + p] * add_psi_[p * nn + uk];
    if (add_member_.size() == static_cast<std::size_t>(n_ensemble) * nn) h += add_member_[ui * nn + uk];
    return std::max(h, 0.0);
}

double SpectralROM1D::reconstructHeadUnpinned(int member, int active_node) const noexcept {
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
    if (qsr_r_.size() == nn && ui < qsr_mm1_.size()) h += qsr_mm1_[ui] * qsr_r_[uk];
    if (add_np_ > 0 && add_psi_.size() == add_np_ * nn &&
        add_kappa_.size() == static_cast<std::size_t>(n_ensemble) * add_np_)
        for (std::size_t p = 0; p < add_np_; ++p) h += add_kappa_[ui * add_np_ + p] * add_psi_[p * nn + uk];
    if (add_member_.size() == static_cast<std::size_t>(n_ensemble) * nn) h += add_member_[ui * nn + uk];
    return std::max(h, 0.0);
}

} // namespace openswmm::uncertainty
