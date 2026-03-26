/**
 * @file Snow.cpp
 * @brief Snowmelt — batch-oriented, vectorisable kernels.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Snow.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>

namespace openswmm {
namespace snow {

void SnowSoA::resize(int n) {
    n_subcatch = n;
    auto total = static_cast<std::size_t>(n * N_SUBAREAS);
    auto un = static_cast<std::size_t>(n);

    wsnow.assign(total, 0.0);
    fw.assign(total, 0.0);
    coldc.assign(total, 0.0);
    ati.assign(total, 32.0);
    awe.assign(total, 0.0);
    imelt.assign(total, 0.0);
    tbase.assign(total, 32.0);
    dhm.assign(total, 0.0);
    fwfrac.assign(total, 0.1);
    snn.assign(un, 0.0);
}

void SnowSolver::init(int n_subcatch) {
    soa_.resize(n_subcatch);
}

// ============================================================================
// Batch ATI update — VECTORISABLE
// ============================================================================

void SnowSolver::batchATIUpdate(double* ati, double temp, double tipm,
                                 double dt, int count) {
    // tipm adjusted for timestep: tipm_adj = 1 - (1-tipm)^(dt/21600)
    double tipm_adj = 1.0 - std::pow(1.0 - tipm, dt / 21600.0);
    for (int i = 0; i < count; ++i) {
        ati[i] += tipm_adj * (temp - ati[i]);
    }
}

// ============================================================================
// Batch degree-day melt — VECTORISABLE
// ============================================================================

void SnowSolver::batchDegreeDayMelt(const double* dhm, const double* tbase,
                                     double temp, double* melt, int count) {
    for (int i = 0; i < count; ++i) {
        double excess = temp - tbase[i];
        melt[i] = (excess > 0.0) ? dhm[i] * excess : 0.0;
    }
}

// ============================================================================
// Batch rain-on-snow melt — VECTORISABLE (uniform inputs broadcast)
// ============================================================================

void SnowSolver::batchRainOnSnowMelt(double temp, double wind, double gamma,
                                      double ea, double rainfall,
                                      double* melt, int count) {
    // Rain-on-snow formula (legacy: getRainmelt)
    // Only applies when rainfall > 0.02 in/hr converted to ft/sec
    const double ucf_rain_us = ucf::Ucf[ucf::RAINFALL][0]; // US: in/hr ↔ ft/sec
    const double RAIN_THRESHOLD = 0.02 / ucf_rain_us;
    if (rainfall <= RAIN_THRESHOLD) {
        std::fill(melt, melt + count, 0.0);
        return;
    }

    double rain_in_hr = rainfall * ucf_rain_us;  // ft/sec → in/hr
    double uadj = 0.006 * wind;
    double t1 = temp - 32.0;
    double t2 = 7.5 * gamma * uadj;
    double t3 = 8.5 * uadj * (ea - 0.18);
    double smelt_in_hr = t1 * (0.001167 + t2 + 0.007 * rain_in_hr) + t3;
    double smelt = smelt_in_hr / ucf_rain_us;  // in/hr → ft/sec
    if (smelt < 0.0) smelt = 0.0;

    // Broadcast same melt rate to all subareas (uniform climate)
    std::fill(melt, melt + count, smelt);
}

// ============================================================================
// Batch snow accumulation — VECTORISABLE
// ============================================================================

void SnowSolver::batchAccumulate(double* wsnow, double snowfall, double dt, int count) {
    double accum = snowfall * dt;
    for (int i = 0; i < count; ++i) {
        wsnow[i] += accum;
    }
}

// ============================================================================
// Execute — all subcatchments batch
// ============================================================================

void SnowSolver::execute(SimulationContext& /*ctx*/, double dt,
                          double temp, double wind, double rainfall) {
    int n = soa_.n_subcatch;
    if (n == 0) return;
    int total = n * N_SUBAREAS;

    // 1. Batch ATI update for all subareas
    batchATIUpdate(soa_.ati.data(), temp, soa_.tipm, dt, total);

    // 2. Clamp ATI to tbase (ATI cannot exceed tbase)
    for (int i = 0; i < total; ++i) {
        soa_.ati[static_cast<std::size_t>(i)] =
            std::min(soa_.ati[static_cast<std::size_t>(i)],
                     soa_.tbase[static_cast<std::size_t>(i)]);
    }

    // 3. Compute melt rate — choose degree-day or rain-on-snow
    const double RAIN_THRESHOLD = 0.02 / ucf::Ucf[ucf::RAINFALL][0];
    if (rainfall > RAIN_THRESHOLD) {
        // Rain-on-snow for all subareas
        batchRainOnSnowMelt(temp, wind, 0.0, 0.0, rainfall,
                            soa_.imelt.data(), total);
    } else {
        // Degree-day melt
        batchDegreeDayMelt(soa_.dhm.data(), soa_.tbase.data(), temp,
                           soa_.imelt.data(), total);
    }

    // 4. Apply cold content — melt only occurs after cold content is satisfied
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) {
            soa_.imelt[ui] = 0.0;
            continue;
        }
        // Cold content absorbs melt
        double melt_vol = soa_.imelt[ui] * dt;
        if (melt_vol <= soa_.coldc[ui]) {
            soa_.coldc[ui] -= melt_vol;
            soa_.imelt[ui] = 0.0;
        } else {
            soa_.imelt[ui] = (melt_vol - soa_.coldc[ui]) / dt;
            soa_.coldc[ui] = 0.0;
        }

        // Limit melt to available snow
        double max_melt = soa_.wsnow[ui] / dt;
        if (soa_.imelt[ui] > max_melt) soa_.imelt[ui] = max_melt;
    }

    // 5. Update snow water equivalent
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa_.wsnow[ui] -= soa_.imelt[ui] * dt;
        if (soa_.wsnow[ui] < 0.0) soa_.wsnow[ui] = 0.0;
    }
}

} // namespace snow
} // namespace openswmm
