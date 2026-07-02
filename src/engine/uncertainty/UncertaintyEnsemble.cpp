/**
 * @file UncertaintyEnsemble.cpp
 * @brief UncertaintyEnsemble — implementation.
 *
 * @ingroup engine_uncertainty
 */

#include "UncertaintyEnsemble.hpp"
#include "LhsShuffle.hpp"
#include <stdexcept>

namespace openswmm::uncertainty {

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
    cd_mult.resize(M);

    const double m_lo = 1.0 - mannings_pert_2d;
    const double m_hi = 1.0 + mannings_pert_2d;
    const double r_lo = 1.0 - rainfall_pert_2d;
    const double r_hi = 1.0 + rainfall_pert_2d;
    const double s_lo = 1.0 - soil_pert;
    const double s_hi = 1.0 + soil_pert;
    const double c_lo = 1.0 - cd_pert;
    const double c_hi = 1.0 + cd_pert;
    const double Md   = static_cast<double>(n_members);

    // Manning: ascending stratification — the reference column against which
    // every other parameter's rank correlation is judged.
    for (int i = 0; i < n_members; ++i) {
        double t_m = (static_cast<double>(i) + 0.5) / Md;
        mannings_mult_2d[static_cast<std::size_t>(i)] = m_lo + t_m * (m_hi - m_lo);
    }

    // Rainfall, soil, Cd: independently shuffled strata (seed+1/+2/+3). Unlike
    // a reversed column (rank correlation exactly -1), a shuffle gives each
    // pair near-zero rank correlation (expected |rho| ~ 1/sqrt(M-1)).
    const auto rainfall_t = shuffledStrata(n_members, seed + 1);
    const auto soil_t     = shuffledStrata(n_members, seed + 2);
    const auto cd_t       = shuffledStrata(n_members, seed + 3);
    for (int i = 0; i < n_members; ++i) {
        auto ui = static_cast<std::size_t>(i);
        rainfall_mult_2d[ui] = r_lo + rainfall_t[ui] * (r_hi - r_lo);
        soil_mult[ui]        = s_lo + soil_t[ui]     * (s_hi - s_lo);
        cd_mult[ui]          = c_lo + cd_t[ui]       * (c_hi - c_lo);
    }
}

} // namespace openswmm::uncertainty
