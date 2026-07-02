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

#include <algorithm>
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

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_UNCERTAINTY_LHS_SHUFFLE_HPP
