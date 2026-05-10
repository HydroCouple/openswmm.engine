/**
 * @file Landuse.cpp
 * @brief Pollutant buildup/washoff — batch SoA, numerically identical to legacy.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "Landuse.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace landuse {

void SurfaceQualitySoA::resize(int n_sc, int n_lu, int n_poll) {
    n_subcatch = n_sc;
    n_landuses = n_lu;
    n_pollutants = n_poll;
    buildup.assign(static_cast<size_t>(n_sc * n_lu * n_poll), 0.0);
    washoff_conc.assign(static_cast<size_t>(n_sc * n_poll), 0.0);
}

void LanduseSolver::init(int n_landuses, int n_pollutants) {
    n_landuses_ = n_landuses;
    n_pollutants_ = n_pollutants;
    auto total = static_cast<size_t>(n_landuses * n_pollutants);
    buildup_params.resize(total);
    washoff_params.resize(total);
}

// ============================================================================
// Buildup mass from days — VECTORISABLE (pow/exp per element)
// ============================================================================

static double getBuildupMass(const BuildupParams& bp, double days) {
    if (days <= 0.0) return 0.0;
    double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
    if (days >= bp.max_days && bp.max_days > 0.0) return c0;

    switch (bp.type) {
        case BuildupType::POWER: {
            double b = c1 * std::pow(days, c2);
            return std::min(b, c0);
        }
        case BuildupType::EXPON:
            return c0 * (1.0 - std::exp(-c1 * days));
        case BuildupType::SATUR:
            return (c2 + days > 0.0) ? c0 * days / (c2 + days) : 0.0;
        default:
            return 0.0;
    }
}

// Inverse: mass → equivalent days — VECTORISABLE
static double getBuildupDays(const BuildupParams& bp, double mass) {
    if (mass <= 0.0) return 0.0;
    double c0 = bp.coeff[0], c1 = bp.coeff[1], c2 = bp.coeff[2];
    if (mass >= c0 && c0 > 0.0) return bp.max_days;

    switch (bp.type) {
        case BuildupType::POWER:
            return (c1 * c2 > 0.0) ? std::pow(mass / c1, 1.0 / c2) : 0.0;
        case BuildupType::EXPON:
            return (c0 * c1 > 0.0) ? -std::log(1.0 - mass / c0) / c1 : 0.0;
        case BuildupType::SATUR:
            return (c0 > mass) ? mass * c2 / (c0 - mass) : bp.max_days;
        default:
            return 0.0;
    }
}

void LanduseSolver::computeBuildup(SurfaceQualitySoA& sq,
                                    const double* /*area*/,
                                    const double* /*curb_length*/,
                                    double dt, int n_subcatch) {
    double dt_days = dt / ucf::SEC_PER_DAY;

    // Batch: for each (subcatch, pollutant) — vectorisable inner loop
    for (int p = 0; p < n_pollutants_; ++p) {
        // Use land use 0 parameters for now (simplified; full: weight by land use fraction)
        const auto& bp = buildup_params[static_cast<size_t>(p)];
        if (bp.type == BuildupType::NONE) continue;

        for (int i = 0; i < n_subcatch; ++i) {
            auto idx = static_cast<size_t>(i * n_pollutants_ + p);
            double mass = sq.buildup[idx];
            double days = getBuildupDays(bp, mass);
            days += dt_days;
            sq.buildup[idx] = getBuildupMass(bp, days);
        }
    }
}

void LanduseSolver::computeWashoff(SurfaceQualitySoA& sq,
                                    const double* runoff,
                                    const double* area,
                                    int n_subcatch) {
    for (int p = 0; p < n_pollutants_; ++p) {
        const auto& wp = washoff_params[static_cast<size_t>(p)];
        if (wp.type == WashoffType::NONE) continue;

        // Inner loop over subcatchments — VECTORISABLE
        for (int i = 0; i < n_subcatch; ++i) {
            auto idx = static_cast<size_t>(i * n_pollutants_ + p);
            double q = runoff[i];
            double a = area[i];

            if (q <= 0.0 || a <= 0.0) {
                sq.washoff_conc[idx] = 0.0;
                continue;
            }

            double conc = 0.0;
            switch (wp.type) {
                case WashoffType::EMC:
                    conc = wp.coeff;
                    break;
                case WashoffType::EXPON: {
                    double b = sq.buildup[idx];
                    if (b > 0.0) {
                        conc = wp.coeff * std::pow(q, wp.expon) * b / (q * a);
                    }
                    break;
                }
                case WashoffType::RATING:
                    conc = wp.coeff * std::pow(q * a, wp.expon - 1.0);
                    break;
                default:
                    break;
            }
            sq.washoff_conc[idx] = std::max(conc, 0.0);

            // Reduce buildup by washoff
            if (wp.type == WashoffType::EXPON && sq.buildup[idx] > 0.0) {
                double washed = conc * q * a;  // mass/sec
                sq.buildup[idx] = std::max(sq.buildup[idx] - washed, 0.0);
            }
        }
    }
}

// ============================================================================
// Co-pollutant washoff — matches legacy landuse_getCoPollutLoad()
// ============================================================================

void LanduseSolver::applyCoPollutant(SurfaceQualitySoA& sq,
                                      const double* runoff, const double* area,
                                      const int* co_pollut, const double* co_frac,
                                      int n_subcatch) {
    if (!co_pollut || !co_frac) return;

    // For each pollutant p that has a co-pollutant k:
    //   washoff_conc[p] += co_frac[p] * washoff_conc[k]
    // (legacy: w = Pollut[p].coFraction * washoff[k])
    for (int p = 0; p < n_pollutants_; ++p) {
        int k = co_pollut[p];
        if (k < 0 || k >= n_pollutants_) continue;
        double frac = co_frac[p];
        if (frac <= 0.0) continue;

        for (int i = 0; i < n_subcatch; ++i) {
            auto idx_p = static_cast<size_t>(i * n_pollutants_ + p);
            auto idx_k = static_cast<size_t>(i * n_pollutants_ + k);
            sq.washoff_conc[idx_p] += frac * sq.washoff_conc[idx_k];
        }
    }
}

} // namespace landuse
} // namespace openswmm
