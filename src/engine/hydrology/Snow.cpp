/**
 * @file Snow.cpp
 * @brief Snowmelt — batch-oriented, vectorisable kernels.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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
    asc.assign(total, 1.0);
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
// Areal depletion with new-snow ADC transition (Gap #18)
// Matches legacy getArealDepletion() in snow.c.
// wsnow is the PRE-snowfall snow water equivalent for this step.
// ============================================================================

/// @param soa      Snow state arrays (sba/sbws/awe updated in place).
/// @param ui       Flat array index (subcatch * N_SUBAREAS + subarea).
/// @param subarea  Subarea index (SNOW_PLOWABLE, SNOW_IMPERV, SNOW_PERV).
/// @param snowfall Snowfall rate this step (ft/sec); 0 = melting/no-snow event.
/// @param dt       Timestep (sec).
/// @return         Areal snow coverage fraction (0 to 1).
static double getArealDepletion(SnowSoA& soa, std::size_t ui, int subarea,
                                double snowfall, double dt) {
    // Plowable sub-area not subject to areal depletion (always 100% covered).
    if (subarea == SNOW_PLOWABLE) return 1.0;

    double si_val = soa.si[ui];
    double wsnow  = soa.wsnow[ui];

    // No depletion if si == 0 or pack is at or above 100%-cover depth.
    if (si_val <= 0.0 || wsnow >= si_val) {
        soa.awe[ui] = 1.0;
        return 1.0;
    }
    // Zero snow → no coverage.
    if (wsnow <= 0.0) {
        soa.awe[ui] = 1.0;
        return 0.0;
    }

    const double* adc = (subarea == SNOW_PERV) ? soa.adc_perv : soa.adc_imperv;

    // Case: new snowfall this step (Gap #18 — new-snow ADC branch).
    // wsnow is pre-snowfall, so awe is the index before the new snow was added.
    if (snowfall > 0.0) {
        double awe   = wsnow / si_val;                             // pre-snow index
        awe = std::max(awe, 0.0);
        double sba   = getArealSnowCover(adc, awe);               // coverage at that index
        double sbws  = awe + (0.75 * snowfall * dt) / si_val;     // end of new-snow ADC
        sbws = std::min(sbws, 1.0);
        soa.awe[ui]  = awe;
        soa.sba[ui]  = sba;
        soa.sbws[ui] = sbws;
        return 1.0;   // full coverage while actively snowing
    }

    // Case: no new snow — deplete using stored new-snow ADC state.
    double awe   = soa.awe[ui];
    double sba   = soa.sba[ui];
    double sbws  = soa.sbws[ui];
    double awesi = wsnow / si_val;   // current relative index

    if (awesi < awe) {
        // Pack has melted below the start of the new-snow ADC → use regular curve.
        soa.awe[ui] = 1.0;   // reset for next snowfall event
        return getArealSnowCover(adc, awesi);
    }
    if (awesi >= sbws) {
        // Pack depth still at or above end of new-snow ADC → full coverage.
        return 1.0;
    }
    // On the linear new-snow ADC segment.
    if (sbws <= awe) return sba;   // degenerate: zero-width interval
    return sba + (1.0 - sba) / (sbws - awe) * (awesi - awe);
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
                          double snowfall, double gamma, double ea) {
    int n = soa_.n_subcatch;
    if (n == 0) return;
    int total = n * N_SUBAREAS;

    // -----------------------------------------------------------------------
    // Step 0 (Gap #19): 0.001-inch minimum pack threshold.
    // Packs thinner than this are melted instantly (matching legacy).
    // -----------------------------------------------------------------------
    constexpr double MIN_PACK_FT = 0.001 / 12.0;
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double ws = soa_.wsnow[ui];
        if (ws > 0.0 && ws <= MIN_PACK_FT) {
            soa_.imelt[ui]  += (ws + soa_.fw[ui]) / dt;
            soa_.wsnow[ui]   = 0.0;
            soa_.fw[ui]      = 0.0;
            soa_.coldc[ui]   = 0.0;
            soa_.asc[ui]     = 0.0;   // no coverage after instant melt
        }
    }

    // -----------------------------------------------------------------------
    // Step 1 (Gap #18): Compute areal snow coverage per subarea.
    // Uses sba/sbws new-snow ADC tracking (matching legacy getArealDepletion).
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) {
            soa_.asc[ui] = 0.0;
            continue;
        }
        int subarea = i % N_SUBAREAS;
        soa_.asc[ui] = getArealDepletion(soa_, ui, subarea, snowfall, dt);
    }

    // -----------------------------------------------------------------------
    // Step 2: ATI update — only during sub-freezing conditions (temp < tbase).
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;
        if (temp < soa_.tbase[ui]) {
            double tipm_adj = 1.0 - std::pow(1.0 - soa_.tipm, dt / 21600.0);
            soa_.ati[ui] += tipm_adj * (temp - soa_.ati[ui]);
            soa_.ati[ui]  = std::min(soa_.ati[ui], soa_.tbase[ui]);
        }
    }

    // -----------------------------------------------------------------------
    // Step 3: Cold content accumulation during sub-freezing periods.
    // Uses stored soa_.asc[ui] (already computed in Step 1).
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;
        if (temp < soa_.tbase[ui]) {
            double asc     = soa_.asc[ui];
            double cc_incr = soa_.rnm * soa_.dhm[ui] *
                             (soa_.ati[ui] - temp) * dt * asc;
            soa_.coldc[ui] += std::max(0.0, cc_incr);
            // Cap cold content (legacy: ccMax = wsnow * 0.007/12 * (tbase - ati))
            double ccMax = soa_.wsnow[ui] * 0.007 / 12.0 *
                           (soa_.tbase[ui] - soa_.ati[ui]);
            if (ccMax > 0.0 && soa_.coldc[ui] > ccMax)
                soa_.coldc[ui] = ccMax;
        }
    }

    // -----------------------------------------------------------------------
    // Step 4: Compute melt rate — degree-day or rain-on-snow.
    // -----------------------------------------------------------------------
    const double RAIN_THRESHOLD = 0.02 / ucf::Ucf[ucf::RAINFALL][0];
    if (rainfall > RAIN_THRESHOLD) {
        batchRainOnSnowMelt(temp, wind, gamma, ea, rainfall,
                            soa_.imelt.data(), total);
    } else {
        batchDegreeDayMelt(soa_.dhm.data(), soa_.tbase.data(), temp,
                           soa_.imelt.data(), total);
    }

    // Step 4b: Scale melt by areal coverage (using stored soa_.asc[ui]).
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0 || soa_.imelt[ui] <= 0.0) continue;
        soa_.imelt[ui] *= soa_.asc[ui];
    }

    // -----------------------------------------------------------------------
    // Step 5: Cold content absorption — melt only exits after cc is satisfied.
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) {
            soa_.imelt[ui] = 0.0;
            continue;
        }
        double melt_vol = soa_.imelt[ui] * dt;
        if (melt_vol <= soa_.coldc[ui]) {
            soa_.coldc[ui] -= melt_vol;
            soa_.imelt[ui]  = 0.0;
        } else {
            soa_.imelt[ui]  = (melt_vol - soa_.coldc[ui]) / dt;
            soa_.coldc[ui]  = 0.0;
        }
        // Limit melt to available snow
        soa_.imelt[ui] = std::min(soa_.imelt[ui], soa_.wsnow[ui] / dt);
    }

    // -----------------------------------------------------------------------
    // Step 6: Free water routing — excess drains as output melt.
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;

        double fw_cap = soa_.fwfrac[ui] * soa_.wsnow[ui];
        soa_.fw[ui]  += soa_.imelt[ui] * dt;

        if (soa_.fw[ui] > fw_cap) {
            double excess   = soa_.fw[ui] - fw_cap;
            soa_.fw[ui]     = fw_cap;
            soa_.imelt[ui]  = excess / dt;
        } else {
            soa_.imelt[ui]  = 0.0;
        }
    }

    // -----------------------------------------------------------------------
    // Step 7: Update SWE.
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa_.wsnow[ui] -= soa_.imelt[ui] * dt;
        soa_.wsnow[ui]  = std::max(soa_.wsnow[ui], 0.0);
        soa_.fw[ui]     = std::min(soa_.fw[ui], soa_.wsnow[ui]);
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
        // Accumulate removed volume: depth (ft) * plowable area fraction * non-LID area (ft2)
        // Gap #60: exclude LID area from plow volume (matches legacy snow.c Build 5.2.0).
        if (soa_.sfrac[sf + 0] > 0.0) {
            double lid_ft2 = (uj < ctx.subcatches.total_lid_area_ft2.size())
                             ? ctx.subcatches.total_lid_area_ft2[uj] : 0.0;
            double area_ft2 = ctx.subcatches.area[uj] * 43560.0 // acres → ft2
                              - lid_ft2;
            if (area_ft2 < 0.0) area_ft2 = 0.0;
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
