/**
 * @file SpectralROM.cpp
 * @brief Spectral ROM — implementation.
 *
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "SpectralROM.hpp"
#include "uncertainty/SpectralROM1D.hpp"

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

// ============================================================================
// initialize
// ============================================================================

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

    q05.assign(static_cast<std::size_t>(n_tri), 0.0);
    q50.assign(static_cast<std::size_t>(n_tri), 0.0);
    q95.assign(static_cast<std::size_t>(n_tri), 0.0);

    h_work_.assign(static_cast<std::size_t>(n_tri), 0.0);
    sort_buf_.assign(static_cast<std::size_t>(n_ensemble), 0.0);
    keff_modes_.assign(static_cast<std::size_t>(n_kept), 0.0);
    mode_energy_.assign(static_cast<std::size_t>(n_kept), 0.0);
    h_weight_.assign(static_cast<std::size_t>(n_tri), 1.0);

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
    //   mannings_mult ∈ [1 - mannings_pert, 1 + mannings_pert]
    //   rainfall_mult ∈ [1 - rainfall_pert, 1 + rainfall_pert]
    //   LHS stratum midpoint: val = low + (i+0.5)/M * (high-low), i=0..M-1
    //   Rainfall order is reversed to give near-zero correlation with Manning.
    // -------------------------------------------------------------------------

    mannings_mult.resize(static_cast<std::size_t>(n_ensemble));
    rainfall_mult.resize(static_cast<std::size_t>(n_ensemble));

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
            double t_r = (static_cast<double>(n_ensemble - 1 - i) + 0.5)
                       / static_cast<double>(n_ensemble);
            rainfall_mult[static_cast<std::size_t>(i)] = r_lo + t_r * (r_hi - r_lo);
        }
    }

}

// ============================================================================
// seed
// ============================================================================

void SpectralROM::seed(const double* h_full) {
    assert(is_ready());

    // a_i = P^T * h_full  (projection; same for every member)
    // P is n_tri × n_kept, column-major: P[j * n_tri + t] = P_{t,j}
    auto nt = static_cast<std::size_t>(n_tri);
    auto nk = static_cast<std::size_t>(n_kept);

    for (std::size_t j = 0; j < nk; ++j) {
        const double* Pj = &basis->P[j * nt];
        double dot = 0.0;
        for (std::size_t t = 0; t < nt; ++t)
            dot += Pj[t] * h_full[t];

        for (int i = 0; i < n_ensemble; ++i)
            a_ensemble[static_cast<std::size_t>(i) * nk + j] = dot;
    }
}

// ============================================================================
// advance
// ============================================================================

void SpectralROM::advance(double dt, double K_eff, const double* rainfall,
                          const double* h_cell) {
    assert(is_ready());

    auto nt = static_cast<std::size_t>(n_tri);
    auto nk = static_cast<std::size_t>(n_kept);

    // ---- Step 1: Compute per-mode energy E_j = mean_i(a²_{i,j}) -------------
    for (std::size_t j = 0; j < nk; ++j) {
        double sum_sq = 0.0;
        for (int i = 0; i < n_ensemble; ++i) {
            double aij = a_ensemble[static_cast<std::size_t>(i) * nk + j];
            sum_sq += aij * aij;
        }
        mode_energy_[j] = sum_sq / static_cast<double>(n_ensemble);
    }

    // ---- Step 2: Project rainfall into coarse space -------------------------
    std::fill(r_coarse.begin(), r_coarse.end(), 0.0);
    if (rainfall) {
        for (std::size_t j = 0; j < nk; ++j) {
            const double* Pj = &basis->P[j * nt];
            double dot = 0.0;
            for (std::size_t t = 0; t < nt; ++t)
                dot += Pj[t] * rainfall[t];
            r_coarse[j] = dot;
        }
    }

    // ---- Step 3: Update active set (transient dropping) ---------------------
    // Mode j is active if its current energy OR its first-order rain forcing
    // exceeds mode_drop_threshold.  Inactive modes are skipped in the advance
    // and reconstruction loops, reducing O(M·k) cost during inter-event decay.
    // The rain_scale estimate uses the maximum rainfall_mult across members so
    // no member is falsely skipped when it has significant forcing.
    const bool has_ensemble_rain = !ensemble_rainfall_.empty()
                                    && (rainfall != nullptr)
                                    && (mean_ensemble_rain_ > 1.0e-30);
    // Max intensity ratio across members: used to scale rain activation criterion.
    double max_scale = 1.0 + rainfall_pert;  // scalar path upper bound
    if (has_ensemble_rain) {
        for (std::size_t i = 0; i < static_cast<std::size_t>(n_ensemble); ++i)
            max_scale = std::max(max_scale,
                                 ensemble_rainfall_[i] / mean_ensemble_rain_);
    }
    const double rain_scale = (rainfall != nullptr)
                               ? std::abs(dt) * max_scale
                               : 0.0;
    n_modes_active = 0;
    for (std::size_t j = 0; j < nk; ++j) {
        bool by_energy = mode_energy_[j] >= mode_drop_threshold;
        bool by_rain   = rain_scale > 0.0 &&
                         std::abs(r_coarse[j]) * rain_scale >= mode_drop_threshold;
        mode_active[j] = by_energy || by_rain;
        if (mode_active[j]) ++n_modes_active;
    }

    // ---- Step 4: Precompute depth-weight h_weight[t] = (h[t]/h̄)^(5/3) ------
    // Used by both scalar and spatial Rayleigh-quotient K_eff computations.
    // When h_cell is null: uniform weights (1.0).
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

    // Scalar Rayleigh-quotient K_eff (per-mode, member-independent).
    // Used by the scalar Manning path; overridden per-member for spatial Manning.
    std::fill(keff_modes_.begin(), keff_modes_.end(), K_eff);
    for (std::size_t j = 0; j < nk; ++j) {
        if (!mode_active[j]) continue;
        const double* Pj = &basis->P[j * nt];
        double rq = 0.0;
        for (std::size_t t = 0; t < nt; ++t)
            rq += Pj[t] * Pj[t] * h_weight_[t];
        keff_modes_[j] = K_eff * rq;
    }

    // Flags for active paths (avoids repeated checks in the inner loop).
    const bool use_spatial_mann = spatial_mannings.is_spatial();
    const bool use_spatial_rain = spatial_rainfall.is_spatial() && (rainfall != nullptr)
                                  && !has_ensemble_rain;
    // has_ensemble_rain already accounts for rainfall != nullptr and mean > 0.

    // ---- Step 5: Advance active modes ---------------------------------------
    const double rate_floor = 1.0e-12;

    for (int i = 0; i < n_ensemble; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double* ai = &a_ensemble[ui * nk];
        double mm = mannings_mult[ui];  // scalar fallback
        double rm = rainfall_mult[ui];  // scalar fallback

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
                // Spatial Manning: keff_j^i = K_eff * Σ_t φ_j[t]² * h_wt / W_n[i][t]
                double rq = 0.0;
                for (std::size_t t = 0; t < nt; ++t)
                    rq += Pj[t] * Pj[t] * h_weight_[t] / W_n_i[t];
                keff_ji = K_eff * rq;
            } else {
                // Scalar Manning: keff_modes_[j] / mannings_mult[i]
                keff_ji = keff_modes_[j] / mm;
            }

            // --- mode forcing for mode j, member i ---
            double fj;
            if (has_ensemble_rain) {
                // RunoffEnsemble path: per-member multiplier on the deterministic
                // rainfall projection.  Preserves spatial structure (r_coarse[j])
                // while scaling intensity by each member's runoff rate.
                fj = r_coarse[j] * (ensemble_rainfall_[ui] / mean_ensemble_rain_);
            } else if (W_r_i) {
                // Spatial rainfall: f_j^i = Σ_t P[j,t] * rainfall[t] * W_rain[i][t]
                fj = 0.0;
                for (std::size_t t = 0; t < nt; ++t)
                    fj += Pj[t] * rainfall[t] * W_r_i[t];
            } else {
                // Scalar rainfall: r_coarse[j] * rainfall_mult[i]
                fj = r_coarse[j] * rm;
            }

            // --- exact exponential integrator ---
            double rate = lam * keff_ji;
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

void SpectralROM::computeQuantiles(bool parametric_tails) {
    assert(is_ready());

    auto nt = static_cast<std::size_t>(n_tri);
    auto nk = static_cast<std::size_t>(n_kept);

    // Compute quantile indices (into sorted n_ensemble values)
    // For M members, the p-th quantile index = floor(p * (M-1) + 0.5) (nearest rank)
    double M = static_cast<double>(n_ensemble);
    int idx05 = std::max(0, static_cast<int>(0.05 * (M - 1.0) + 0.5));
    int idx50 = static_cast<int>(0.50 * (M - 1.0) + 0.5);
    int idx95 = std::min(n_ensemble - 1, static_cast<int>(0.95 * (M - 1.0) + 0.5));

    for (std::size_t t = 0; t < nt; ++t) {
        // Reconstruct depth at cell t for all members
        for (int i = 0; i < n_ensemble; ++i) {
            const double* ai = &a_ensemble[static_cast<std::size_t>(i) * nk];
            double h = 0.0;
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
    const openswmm::uncertainty::SpectralROM1D* rom1d)
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

            // Per-member 1D head: use ROM1D reconstruction when available,
            // fall back to shared deterministic head for outfalls or unregistered nodes.
            double h_1d_abs = (active_1d_idx >= 0)
                              ? rom1d->reconstructHead(i, active_1d_idx)
                              : node_heads[cp.node_idx];

            // Reconstruct depth at coupling cell for this member
            double h_2d = 0.0;
            for (std::size_t j = 0; j < nk; ++j)
                h_2d += basis->P[j * nt + ci] * ai[j];
            h_2d = std::max(h_2d, 0.0);

            double dh = (cell_z + h_2d) - h_1d_abs;
            if (std::abs(dh) < dh_eps) continue;

            // Orifice exchange scaled by 1/W_n: higher Manning's n reduces
            // effective drainage conductance toward the inlet.
            // Spatial path: use the per-cell multiplier at the coupling cell.
            // Scalar path: use the global per-member multiplier.
            double sign = (dh > 0.0) ? 1.0 : -1.0;
            double mm = spatial_mannings.is_spatial()
                ? spatial_mannings.at(i, static_cast<int>(ci))
                : mannings_mult[ui];
            double Q    = sign * cp.cd * cp.area * std::sqrt(G2 * std::abs(dh)) / mm;

            // Limit drainage by available depth (cannot drain below zero)
            if (Q > 0.0)
                Q = std::min(Q, h_2d * tri_area / dt);

            // Track flux bounds across members.
            if (Q < q_min_cp) q_min_cp = Q;
            if (Q > q_max_cp) q_max_cp = Q;

            // Depth change (negative = drainage out of cell)
            double delta_h = -Q * dt / tri_area;

            // Update ROM coefficients: δa_j = P[j, ci] * δh
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
