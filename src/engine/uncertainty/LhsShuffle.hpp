/**
 * @file LhsShuffle.hpp
 * @brief Shared deterministic PRNG and shuffled Latin-hypercube stratification
 *        helpers, used by UncertaintyEnsemble, SpectralROM, and SpectralROM1D
 *        to build independent (near-zero rank correlation) LHS columns.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_UNCERTAINTY_LHS_SHUFFLE_HPP
#define OPENSWMM_ENGINE_UNCERTAINTY_LHS_SHUFFLE_HPP

#include "UncertaintyTypes.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numeric>
#include <vector>

namespace openswmm::uncertainty {

/// Deterministic 64-bit PRNG step (splitmix64). Advances `state` in place and
/// returns the next pseudo-random value.
inline uint64_t splitmix64(uint64_t& state) noexcept {
    state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

/**
 * @brief Fisher-Yates shuffle of the M Latin-hypercube strata midpoints.
 *
 * The unshuffled strata midpoints are t_k = (k + 0.5) / M for k = 0..M-1.
 * This function returns those same M values in a pseudo-random order driven
 * by `seed`, so the LHS coverage property (each stratum of [0,1) hit exactly
 * once) is preserved exactly — only the member-to-stratum assignment changes.
 *
 * Two calls with different seeds give columns with near-zero rank correlation
 * (expected |rho| ~ 1/sqrt(M-1)); a shuffled column against an ascending
 * reference column (e.g. Manning) is likewise near-zero, unlike a reversed
 * column which is exactly rank-correlation -1.
 *
 * Scale the result to a physical range via `lo + t * (hi - lo)`.
 *
 * @param M     Number of ensemble members (strata).
 * @param seed  Seed driving the shuffle (distinct seeds decorrelate columns).
 * @return      M strata midpoints in shuffled order.
 */
inline std::vector<double> shuffledStrata(int M, uint64_t seed) {
    std::vector<int> perm(static_cast<std::size_t>(M));
    std::iota(perm.begin(), perm.end(), 0);
    uint64_t rng = seed;
    for (int j = M - 1; j > 0; --j) {
        uint64_t r = splitmix64(rng);
        int      k = static_cast<int>(r % static_cast<uint64_t>(j + 1));
        std::swap(perm[static_cast<std::size_t>(j)], perm[static_cast<std::size_t>(k)]);
    }
    std::vector<double> t(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        t[static_cast<std::size_t>(i)] =
            (static_cast<double>(perm[static_cast<std::size_t>(i)]) + 0.5)
            / static_cast<double>(M);
    return t;
}

/**
 * @brief Standard-normal quantile function Φ⁻¹(u) (a.k.a. probit).
 *
 * Peter Acklam's rational approximation (2003), relative error < 1.15e-9
 * over the full open interval u ∈ (0, 1) — more than sufficient for LHS
 * sampling (strata midpoints never approach 0 or 1 closer than 1/(2M)).
 * Shared by the parameter registry (PR 9) and the soft-rainfall sampling
 * path (SR-1b) — add it once, here.
 *
 * @param u  Probability in (0, 1). Values outside are clamped to the
 *           nearest representable interior point.
 */
inline double probit(double u) noexcept {
    // Clamp to the open interval (protects the logs below).
    constexpr double eps = 1.0e-300;
    if (u < eps)       u = eps;
    if (u > 1.0 - 1e-16) u = 1.0 - 1e-16;

    // Acklam coefficients.
    constexpr double a1 = -3.969683028665376e+01, a2 =  2.209460984245205e+02,
                     a3 = -2.759285104469687e+02, a4 =  1.383577518672690e+02,
                     a5 = -3.066479806614716e+01, a6 =  2.506628277459239e+00;
    constexpr double b1 = -5.447609879822406e+01, b2 =  1.615858368580409e+02,
                     b3 = -1.556989798598866e+02, b4 =  6.680131188771972e+01,
                     b5 = -1.328068155288572e+01;
    constexpr double c1 = -7.784894002430293e-03, c2 = -3.223964580411365e-01,
                     c3 = -2.400758277161838e+00, c4 = -2.549732539343734e+00,
                     c5 =  4.374664141464968e+00, c6 =  2.938163982698783e+00;
    constexpr double d1 =  7.784695709041462e-03, d2 =  3.224671290700398e-01,
                     d3 =  2.445134137142996e+00, d4 =  3.754408661907416e+00;
    constexpr double u_low = 0.02425, u_high = 1.0 - 0.02425;

    if (u < u_low) {                                   // lower tail
        double q = std::sqrt(-2.0 * std::log(u));
        return (((((c1*q + c2)*q + c3)*q + c4)*q + c5)*q + c6)
             / ((((d1*q + d2)*q + d3)*q + d4)*q + 1.0);
    }
    if (u > u_high) {                                  // upper tail
        double q = std::sqrt(-2.0 * std::log(1.0 - u));
        return -(((((c1*q + c2)*q + c3)*q + c4)*q + c5)*q + c6)
              / ((((d1*q + d2)*q + d3)*q + d4)*q + 1.0);
    }
    double q = u - 0.5;                                // central region
    double r = q * q;
    return (((((a1*r + a2)*r + a3)*r + a4)*r + a5)*r + a6) * q
         / (((((b1*r + b2)*r + b3)*r + b4)*r + b5)*r + 1.0);
}

/// Standard-normal CDF Φ(z) (via erfc; used for truncation bounds).
inline double normalCdf(double z) noexcept {
    return 0.5 * std::erfc(-z / 1.4142135623730951);
}

/**
 * @brief Map an LHS stratum midpoint t ∈ (0,1) to a parameter multiplier θ.
 *
 * Inverse-CDF sampling per family (PARAMETER_REGISTRY.md §3). All families
 * share the same meaning of p — the half-range of the multiplier band:
 *
 *   UNIFORM   : θ = (1−p) + t·2p                            (band [1−p, 1+p])
 *   NORMAL    : ±3σ-truncated normal, mean 1, σ = p/3; t is affinely mapped
 *               into [Φ(−3), Φ(3)] before the probit (exact truncated
 *               inverse CDF, not clamp-after-sample)       (band [1−p, 1+p])
 *   LOGNORMAL : θ = exp(z(t)·σ_log), σ_log = ln(1+p)/1.6449 (median 1,
 *               q95 = 1+p, q05 = 1/(1+p) — asymmetric, always > 0)
 *
 * p = 0 returns exactly 1.0 for every family.
 */
inline double invCdfMultiplier(DistType dist, double p, double t) noexcept {
    if (p <= 0.0) return 1.0;
    switch (dist) {
        case DistType::NORMAL: {
            const double lo = normalCdf(-3.0);
            const double hi = normalCdf( 3.0);
            const double z  = probit(lo + t * (hi - lo));
            return 1.0 + (p / 3.0) * z;
        }
        case DistType::LOGNORMAL: {
            constexpr double z95 = 1.6448536269514722;
            const double sigma_log = std::log(1.0 + p) / z95;
            return std::exp(probit(t) * sigma_log);
        }
        case DistType::UNIFORM:
        default: {
            // EXACTLY the legacy PR-5 expression `lo + t*(hi−lo)` — do not
            // "simplify" to (1−p) + t·2p, which differs in the last ulp and
            // would break the bit-exact back-compat contract (§4 of the doc).
            const double lo = 1.0 - p;
            const double hi = 1.0 + p;
            return lo + t * (hi - lo);
        }
    }
}

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_LHS_SHUFFLE_HPP
