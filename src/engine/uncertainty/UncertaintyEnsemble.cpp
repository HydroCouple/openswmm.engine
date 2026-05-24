/**
 * @file UncertaintyEnsemble.cpp
 * @brief UncertaintyEnsemble — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "UncertaintyEnsemble.hpp"
#include <stdexcept>
#include <numeric>
#include <algorithm>

namespace openswmm::uncertainty {

// splitmix64 for deterministic Fisher-Yates shuffle
static uint64_t splitmix64(uint64_t& state) {
    state += UINT64_C(0x9e3779b97f4a7c15);
    uint64_t z = state;
    z = (z ^ (z >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
    z = (z ^ (z >> 27)) * UINT64_C(0x94d049bb133111eb);
    return z ^ (z >> 31);
}

// ============================================================================
// generate
// ============================================================================

void UncertaintyEnsemble::generate() {
    if (n_members < 2)
        throw std::invalid_argument(
            "UncertaintyEnsemble::generate: n_members must be >= 2");

    const std::size_t M = static_cast<std::size_t>(n_members);
    mannings_mult_2d.resize(M);
    rainfall_mult_2d.resize(M);
    soil_mult.resize(M);

    const double m_lo = 1.0 - mannings_pert_2d;
    const double m_hi = 1.0 + mannings_pert_2d;
    const double r_lo = 1.0 - rainfall_pert_2d;
    const double r_hi = 1.0 + rainfall_pert_2d;
    const double s_lo = 1.0 - soil_pert;
    const double s_hi = 1.0 + soil_pert;
    const double Md   = static_cast<double>(n_members);

    for (int i = 0; i < n_members; ++i) {
        auto ui = static_cast<std::size_t>(i);
        // Manning: ascending stratification
        double t_m = (static_cast<double>(i) + 0.5) / Md;
        mannings_mult_2d[ui] = m_lo + t_m * (m_hi - m_lo);
        // Rainfall: reversed stratification — decorrelates from Manning
        double t_r = (static_cast<double>(n_members - 1 - i) + 0.5) / Md;
        rainfall_mult_2d[ui] = r_lo + t_r * (r_hi - r_lo);
    }

    // Soil: Fisher-Yates shuffle of strata indices using seed+2 — gives a
    // different (decorrelated) ordering from the ascending/descending columns.
    std::vector<int> perm(M);
    std::iota(perm.begin(), perm.end(), 0);
    uint64_t rng = seed + 2;
    for (int j = n_members - 1; j > 0; --j) {
        uint64_t r   = splitmix64(rng);
        int      k   = static_cast<int>(r % static_cast<uint64_t>(j + 1));
        std::swap(perm[static_cast<std::size_t>(j)],
                  perm[static_cast<std::size_t>(k)]);
    }
    for (int i = 0; i < n_members; ++i) {
        double t_s = (static_cast<double>(perm[static_cast<std::size_t>(i)]) + 0.5) / Md;
        soil_mult[static_cast<std::size_t>(i)] = s_lo + t_s * (s_hi - s_lo);
    }
}

} // namespace openswmm::uncertainty
