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
#include "../core/SimulationContext.hpp"
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
    dhmin.assign(total, 0.0);
    dhmax.assign(total, 0.0);
    fwfrac.assign(total, 0.1);
    fArea.assign(total, 0.0);
    si.assign(total, 0.0);
    sba.assign(total, 0.0);
    sbws.assign(total, 0.0);
    snn.assign(un, 0.0);
    weplow.assign(un, 0.0);
    sfrac.assign(static_cast<std::size_t>(n * 5), 0.0);
    to_subcatch.assign(un, -1);
}

void SnowSolver::init(int n_subcatch) {
    soa_.resize(n_subcatch);
}

// ============================================================================
// Areal depletion curve interpolation (matching legacy getArealSnowCover)
// ============================================================================

/// Interpolate areal snow coverage from a 10-point ADC curve.
/// @param adc    10-point curve (index 0-9 maps to AWESI 0.0-0.9+)
/// @param awesi  Snow water equivalent relative to depth at 100% cover
/// @return       Areal snow coverage fraction (0 to 1)
static double getArealSnowCover(const double* adc, double awesi) {
    if (awesi >= 0.9999) return 1.0;
    if (awesi <= 0.0)    return 0.0;
    double x = awesi * 10.0;
    int k = static_cast<int>(x);
    if (k >= 9) return adc[9];
    double frac = x - static_cast<double>(k);
    return adc[k] + frac * (adc[k + 1] - adc[k]);
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
    smelt = std::max(smelt, 0.0);

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
                          double temp, double wind, double rainfall,
                          double gamma, double ea) {
    int n = soa_.n_subcatch;
    if (n == 0) return;
    int total = n * N_SUBAREAS;

    // 1. ATI update — only during NON-MELT (sub-freezing) conditions
    //    (matching legacy snow.c updateColdContent: ATI only updated when temp < tbase)
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;
        if (temp < soa_.tbase[ui]) {
            // Sub-freezing: update ATI
            double tipm_adj = 1.0 - std::pow(1.0 - soa_.tipm, dt / 21600.0);
            soa_.ati[ui] += tipm_adj * (temp - soa_.ati[ui]);
            soa_.ati[ui] = std::min(soa_.ati[ui], soa_.tbase[ui]);
        }
    }

    // 2. Cold content generation during sub-freezing periods
    //    (matching legacy: cc += rnm * dhm * (ati - Ta) * dt * asc)
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;
        if (temp < soa_.tbase[ui]) {
            // Areal snow coverage from ADC curve
            int subarea = i % N_SUBAREAS;
            const double* adc = (subarea == SNOW_PERV)
                                ? soa_.adc_perv : soa_.adc_imperv;
            double si_val = soa_.si[ui];
            double awesi = (si_val > 0.0) ? soa_.wsnow[ui] / si_val : 1.0;
            double asc = getArealSnowCover(adc, awesi);
            double cc_incr = soa_.rnm * soa_.dhm[ui] *
                             (soa_.ati[ui] - temp) * dt * asc;
            soa_.coldc[ui] += std::max(0.0, cc_incr);
            // Cap cold content (matching legacy ccMax = wsnow * 0.007/12 * (tbase - ati))
            double ccMax = soa_.wsnow[ui] * 0.007 / 12.0 *
                           (soa_.tbase[ui] - soa_.ati[ui]);
            if (ccMax > 0.0 && soa_.coldc[ui] > ccMax)
                soa_.coldc[ui] = ccMax;
        }
    }

    // 3. Compute melt rate — choose degree-day or rain-on-snow
    const double RAIN_THRESHOLD = 0.02 / ucf::Ucf[ucf::RAINFALL][0];
    if (rainfall > RAIN_THRESHOLD) {
        // Rain-on-snow using climate gamma/ea (Fix #16)
        batchRainOnSnowMelt(temp, wind, gamma, ea, rainfall,
                            soa_.imelt.data(), total);
    } else {
        // Degree-day melt
        batchDegreeDayMelt(soa_.dhm.data(), soa_.tbase.data(), temp,
                           soa_.imelt.data(), total);
    }

    // 3b. Scale melt by areal snow coverage (matching legacy meltSnowpack)
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0 || soa_.imelt[ui] <= 0.0) continue;
        int subarea = i % N_SUBAREAS;
        const double* adc = (subarea == SNOW_PERV)
                            ? soa_.adc_perv : soa_.adc_imperv;
        double si_val = soa_.si[ui];
        double awesi = (si_val > 0.0) ? soa_.wsnow[ui] / si_val : 1.0;
        double asc = getArealSnowCover(adc, awesi);
        soa_.imelt[ui] *= asc;
    }

    // 4. Apply cold content — melt only occurs after cold content is satisfied
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) {
            soa_.imelt[ui] = 0.0;
            continue;
        }
        // Cold content absorbs melt (matching legacy reduceColdContent)
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
        soa_.imelt[ui] = std::min(soa_.imelt[ui], max_melt);
    }

    // 5. Free water routing — melt fills pack liquid capacity first
    // (matching legacy snow.c routeSnowmelt logic)
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;

        // Maximum free water capacity = fwfrac * wsnow
        double fw_cap = soa_.fwfrac[ui] * soa_.wsnow[ui];

        // Add melt to free water
        soa_.fw[ui] += soa_.imelt[ui] * dt;

        // Excess drains as actual melt output; retained stays in pack
        if (soa_.fw[ui] > fw_cap) {
            double excess = soa_.fw[ui] - fw_cap;
            soa_.fw[ui] = fw_cap;
            soa_.imelt[ui] = excess / dt;
        } else {
            // All melt retained as liquid in pack
            soa_.imelt[ui] = 0.0;
        }
    }

    // 6. Update snow water equivalent (melt + free water drain)
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa_.wsnow[ui] -= soa_.imelt[ui] * dt;
        soa_.wsnow[ui] = std::max(soa_.wsnow[ui], 0.0);
        // Free water cannot exceed remaining SWE
        soa_.fw[ui] = std::min(soa_.fw[ui], soa_.wsnow[ui]);
    }
}

// ============================================================================
// Seasonal melt coefficient interpolation
// (matching legacy snow.c snow_setMeltCoeffs)
// ============================================================================

void SnowSolver::setMeltCoeffs(int day_of_year) {
    // Compute seasonal factor: -1.0 at winter solstice (Dec 21, day ~355),
    // +1.0 at summer solstice (Jun 21, day ~172).
    // season = sin(2*pi*(day - 81)/365)  where day 81 ~ Mar 22 = equinox
    double season = std::sin(2.0 * 3.14159265358979 * (day_of_year - 81.0) / 365.0);
    soa_.season = season;  // Store for reporting (matching legacy Snow.season)

    int total = soa_.n_subcatch * N_SUBAREAS;
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        // dhm = 0.5 * (dhmax * (1 + season) + dhmin * (1 - season))
        // (matching legacy snow.c line 382-383)
        soa_.dhm[ui] = 0.5 * (soa_.dhmax[ui] * (1.0 + season)
                             + soa_.dhmin[ui] * (1.0 - season));
    }
}

// ============================================================================
// Snow plowing — redistribute excess snow between subareas
// (matching legacy snow.c snow_plowSnow)
// ============================================================================

void SnowSolver::plowSnow(SimulationContext& ctx, double dt, double snowfall) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    for (int j = 0; j < n; ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Add snowfall to all subareas
        for (int k = SNOW_PLOWABLE; k <= SNOW_PERV; ++k) {
            auto idx = static_cast<std::size_t>(j * N_SUBAREAS + k);
            if (soa_.fArea[idx] > 0.0) {
                soa_.wsnow[idx] += snowfall * dt;
                soa_.imelt[idx] = 0.0;
            }
        }

        // Check if plowable area has excess snow
        auto plow_idx = static_cast<std::size_t>(j * N_SUBAREAS + SNOW_PLOWABLE);
        if (soa_.fArea[plow_idx] <= 0.0) continue;
        if (soa_.weplow[uj] <= 0.0) continue;
        if (soa_.wsnow[plow_idx] < soa_.weplow[uj]) continue;

        double exc = soa_.wsnow[plow_idx];
        auto sf = static_cast<std::size_t>(j * 5); // sfrac base index
        double sfracTotal = 0.0;

        // Plow out of system (sfrac[0])
        // Accumulate removed volume: depth (ft) * plowable area fraction * subcatch area (ft2)
        if (soa_.sfrac[sf + 0] > 0.0) {
            double area_ft2 = ctx.subcatches.area[uj] * 43560.0; // acres → ft2
            soa_.removed += soa_.sfrac[sf + 0] * exc *
                            soa_.fArea[plow_idx] * area_ft2;
        }
        sfracTotal += soa_.sfrac[sf + 0];

        // Plow onto non-plowable impervious area (sfrac[1])
        auto imperv_idx = static_cast<std::size_t>(j * N_SUBAREAS + SNOW_IMPERV);
        if (soa_.fArea[imperv_idx] > 0.0) {
            double f = soa_.fArea[plow_idx] / soa_.fArea[imperv_idx];
            soa_.wsnow[imperv_idx] += soa_.sfrac[sf + 1] * exc * f;
            sfracTotal += soa_.sfrac[sf + 1];
        }

        // Plow onto pervious area (sfrac[2])
        auto perv_idx = static_cast<std::size_t>(j * N_SUBAREAS + SNOW_PERV);
        if (soa_.fArea[perv_idx] > 0.0) {
            double f = soa_.fArea[plow_idx] / soa_.fArea[perv_idx];
            soa_.wsnow[perv_idx] += soa_.sfrac[sf + 2] * exc * f;
            sfracTotal += soa_.sfrac[sf + 2];
        }

        // Convert to immediate melt (sfrac[3])
        if (dt > 0.0) {
            soa_.imelt[plow_idx] = soa_.sfrac[sf + 3] * exc / dt;
        }
        sfracTotal += soa_.sfrac[sf + 3];

        // Send to another subcatchment (sfrac[4])
        if (soa_.sfrac[sf + 4] > 0.0 && soa_.to_subcatch[uj] >= 0) {
            int m = soa_.to_subcatch[uj];
            if (m < n) {
                auto target_perv = static_cast<std::size_t>(m * N_SUBAREAS + SNOW_PERV);
                if (soa_.fArea[target_perv] > 0.0) {
                    double f = soa_.fArea[plow_idx] / soa_.fArea[target_perv];
                    soa_.wsnow[target_perv] += soa_.sfrac[sf + 4] * exc * f;
                    sfracTotal += soa_.sfrac[sf + 4];
                }
            }
        }

        // Reduce plowable snow by total fraction plowed
        sfracTotal = std::min(sfracTotal, 1.0);
        soa_.wsnow[plow_idx] = exc * (1.0 - sfracTotal);
    }
}

} // namespace snow
} // namespace openswmm
