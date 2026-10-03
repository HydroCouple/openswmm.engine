// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file Snow.cpp
 * @brief Snowmelt — batch-oriented, vectorisable kernels.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Snow.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <vector>
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
    // 1.0, matching legacy `snow_initSnowpack` (snow.c:199). The new-snow
    // ADC index starts ABOVE any real pack, so `awesi < awe` sends the first
    // depleting step to the regular curve. Initialising it to 0 makes
    // `awesi >= awe` true instead and `getArealDepletion` returns full cover
    // forever — which was invisible while `si` was pinned to the initial pack
    // depth, because the `wsnow >= si` branch then fired on step 1 and set
    // `awe = 1.0` itself. Reading SD100 (F6) is what exposes it: a pack that
    // starts BELOW its SD100 never takes that branch.
    awe.assign(total, 1.0);
    imelt.assign(total, 0.0);
    tbase.assign(total, 32.0);
    dhm.assign(total, 0.0);
    dhmin.assign(total, 0.0);
    dhmax.assign(total, 0.0);
    fwfrac.assign(total, 0.1);
    fArea.assign(total, 0.0);
    age.assign(total, 0.0);
    out_age.assign(total, 0.0);
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
// ============================================================================
// S2b — complete-mix age arithmetic
// ============================================================================

/**
 * @brief Mix `v_in` of water at age `a_in` into a pool holding `v_have` at
 *        age `a_have`. Returns the pool's new age.
 *
 * @details Volume-weighted, and **guarded on the TOTAL rather than on either
 *          term**: a pool that is empty before and after has no age to
 *          report, and returning anything but 0 there would let a stale age
 *          survive a pack that no longer exists. That is the shape of the
 *          dry-element carried-temperature item still open from H1, and it
 *          is cheaper to not create it than to mask it later.
 *
 * @note Removing water at the pool's own age leaves the age UNCHANGED, so
 *       drainage and melt-out need no call here — only arrivals do.
 */
static inline double mixAge(double v_have, double a_have,
                            double v_in, double a_in) noexcept {
    const double v = v_have + v_in;
    if (!(v > 0.0)) return 0.0;
    return (v_have * a_have + v_in * a_in) / v;
}

// Legacy getArealSnowCover (snow.c:691): the ADC plots the covered fraction
// at 10 equal awesi increments; the last interval interpolates to 1.0.
static double getArealSnowCover(const double* adc, double awesi) {
    if (awesi <= 0.0)    return 0.0;
    if (awesi >= 0.9999) return 1.0;
    int m = static_cast<int>(awesi * 10.0 + 0.00001);
    double asc1 = adc[m];
    double asc2 = (m >= 9) ? 1.0 : adc[m + 1];
    return asc1 + (asc2 - asc1) / 0.1 * (awesi - 0.1 * static_cast<float>(m));
}

// ============================================================================
// Areal depletion with new-snow ADC transition — legacy getArealDepletion
// (snow.c:610), statement for statement. `wsnow` already carries this
// step's snowfall (snow_plowSnow ran first), which is why the new-snow
// branch subtracts it back out to find the pre-snow index.
// ============================================================================

/// @param soa      Snow state arrays (sba/sbws/awe updated in place).
/// @param ui       Flat array index (subcatch * N_SUBAREAS + subarea).
/// @param subarea  Subarea index (SNOW_PLOWABLE, SNOW_IMPERV, SNOW_PERV).
/// @param snowfall Snowfall rate this step (ft/sec).
/// @param dt       Timestep (sec).
/// @return         Areal snow coverage fraction (0 to 1).
static double getArealDepletion(SnowSoA& soa, std::size_t ui, int subarea,
                                double snowfall, double dt) {
    // Plowable sub-area not subject to areal depletion.
    if (subarea == SNOW_PLOWABLE) return 1.0;

    const double si = soa.si[ui];

    // No depletion if depth zero or above SI.
    if (si == 0.0 || soa.wsnow[ui] >= si) {
        soa.awe[ui] = 1.0;
        return 1.0;
    }
    if (soa.wsnow[ui] == 0.0) {
        soa.awe[ui] = 1.0;
        return 0.0;
    }

    const double* adc = (subarea == SNOW_PERV) ? soa.adc_perv : soa.adc_imperv;

    // Case of new snowfall.
    if (snowfall > 0.0) {
        double awe = (soa.wsnow[ui] - snowfall * dt) / si;
        awe = std::max(awe, 0.0);
        double sba  = getArealSnowCover(adc, awe);
        double sbws = awe + (0.75 * snowfall * dt) / si;
        sbws = std::min(sbws, 1.0);
        soa.awe[ui]  = awe;
        soa.sba[ui]  = sba;
        soa.sbws[ui] = sbws;
        return 1.0;
    }

    // Case of no new snow.
    const double awe   = soa.awe[ui];
    const double sba   = soa.sba[ui];
    const double sbws  = soa.sbws[ui];
    const double awesi = soa.wsnow[ui] / si;
    double asc;
    if (awesi < soa.awe[ui]) {
        // relative snow depth is below start of new snow ADC
        soa.awe[ui] = 1.0;
        asc = getArealSnowCover(adc, awesi);
    } else if (awesi >= soa.sbws[ui]) {
        // relative snow depth is above end of new snow ADC
        asc = 1.0;
    } else {
        // relative snow depth is on new snow ADC
        asc = sba + (1.0 - sba) / (sbws - awe) * (awesi - awe);
    }
    return asc;
}

// ============================================================================
// Rain-on-snow melt rate — legacy getRainmelt (snow.c:775). Returns the raw
// value (it can be NEGATIVE below 32 F); the caller applies legacy's
// `rmelt > 0` test.
// ============================================================================

double SnowSolver::rainMeltRate(double temp, double wind, double gamma,
                                 double ea, double rainfall) {
    rainfall = rainfall * 43200.0;     // convert rain to in/hr
    if (rainfall > 0.02) {
        double uadj = 0.006 * wind;
        double t1 = temp - 32.0;
        double t2 = 7.5 * gamma * uadj;
        double t3 = 8.5 * uadj * (ea - 0.18);
        double smelt = t1 * (0.001167 + t2 + 0.007 * rainfall) + t3;
        return smelt / 43200.0;
    }
    return 0.0;
}

// ============================================================================
// Execute — scalar convenience overload (broadcast to all subcatchments)
// ============================================================================

void SnowSolver::execute(SimulationContext& ctx, double dt,
                          double temp, double wind, double rainfall,
                          double snowfall, double gamma, double ea) {
    int n = soa_.n_subcatch;
    if (n == 0) return;
    std::vector<double> rain(static_cast<std::size_t>(n), rainfall);
    std::vector<double> snow(static_cast<std::size_t>(n), snowfall);
    execute(ctx, dt, temp, wind, rain.data(), snow.data(), gamma, ea);
}

// ============================================================================
// Execute — legacy snow_getSnowMelt (snow.c:510) for every subcatchment,
// each snow sub-area in legacy's order and with legacy's arithmetic:
// getArealDepletion -> meltSnowpack (getRainmelt / degree-day /
// updateColdContent, reduceColdContent) -> routeSnowmelt. The result per
// surface is `imelt` = the liquid melt rate leaving the pack (legacy's
// `smelt + snowpack->imelt`, the second being the plowed/instant melt), and
// `asc`, from which the engine forms legacy's netPrecip.
// ============================================================================

namespace {

// legacy updateColdContent (snow.c:801)
inline void updateColdContent(SnowSoA& soa, std::size_t ui, double temp,
                              double asc, double snowfall, double dt) {
    double ati = soa.ati[ui];
    double cc  = soa.coldc[ui];
    // if snowing, ATI = snow (air) temperature
    if (snowfall * 43200.0 > 0.02) ati = temp;
    else {
        // convert ATI weighting factor from 6-hr to tStep time basis
        double tipm = 1.0 - std::pow(1.0 - soa.tipm, dt / (6.0 * 3600.0));
        ati += tipm * (temp - ati);
    }
    // ATI cannot exceed snow melt base temperature
    ati = std::min(ati, soa.tbase[ui]);
    // update cold content
    cc += soa.rnm * soa.dhm[ui] * (ati - temp) * dt * asc;
    cc = std::max(cc, 0.0);
    // maximum cold content based on assumed specific heat of snow
    double ccMax = soa.wsnow[ui] * 0.007 / 12.0 * (soa.tbase[ui] - ati);
    cc = std::min(cc, ccMax);
    soa.coldc[ui] = cc;
    soa.ati[ui]   = ati;
}

// legacy reduceColdContent (snow.c:854)
inline double reduceColdContent(SnowSoA& soa, std::size_t ui, double smelt,
                                double ccFactor) {
    double cc = soa.coldc[ui];
    if (smelt * ccFactor > cc) {
        smelt -= cc / ccFactor;
        cc = 0.0;
    } else {
        cc -= smelt * ccFactor;
        smelt = 0.0;
    }
    soa.coldc[ui] = cc;
    return smelt;
}

// legacy meltSnowpack (snow.c:729)
inline double meltSnowpack(SnowSoA& soa, std::size_t ui, double temp,
                           double rmelt, double asc, double snowfall,
                           double dt) {
    double smelt;
    if (rmelt > 0.0) smelt = rmelt;
    else if (temp >= soa.tbase[ui])
        smelt = soa.dhm[ui] * (temp - soa.tbase[ui]);
    else {
        updateColdContent(soa, ui, temp, asc, snowfall, dt);
        return 0.0;
    }
    // adjust snowmelt for area of snow cover
    smelt *= asc;
    // reduce cold content of melting pack
    double ccFactor = dt * soa.rnm * asc;
    smelt = reduceColdContent(soa, ui, smelt, ccFactor);
    soa.ati[ui] = soa.tbase[ui];
    return smelt;
}

}  // namespace

void SnowSolver::execute(SimulationContext& /*ctx*/, double dt,
                          double temp, double wind, const double* rainfall,
                          const double* snowfall, double gamma, double ea) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    for (int j = 0; j < n; ++j) {
        auto uj = static_cast<std::size_t>(j);
        const double rain = rainfall[uj];
        const double snow = snowfall[uj];

        // compute snowmelt over entire subcatchment when rain falling
        const double rmelt = rainMeltRate(temp, wind, gamma, ea, rain);

        for (int i = SNOW_PLOWABLE; i <= SNOW_PERV; ++i) {
            auto ui = static_cast<std::size_t>(j * N_SUBAREAS + i);
            double asc, smelt;
            // `imelt` holds the plowed melt from snow_plowSnow (legacy
            // snowpack->imelt); the instant melt of a thin pack adds to it.
            double imelt = soa_.imelt[ui];

            // completely melt pack if its depth is < 0.001 inch
            if (soa_.wsnow[ui] <= 0.001 / 12.0) {
                asc   = 0.0;
                smelt = 0.0;
                imelt += (soa_.wsnow[ui] + soa_.fw[ui]) / dt;
                // S2b — the water leaves at the age the pack HAD.
                if (soa_.track_age) {
                    soa_.out_age[ui] = soa_.age[ui];
                    soa_.age[ui]     = 0.0;
                }
                soa_.wsnow[ui] = 0.0;
                soa_.fw[ui]    = 0.0;
                soa_.coldc[ui] = 0.0;
            }
            // otherwise compute areal depletion, find snow melt and route it
            // through pack (legacy routeSnowmelt, snow.c:883)
            else {
                asc   = getArealDepletion(soa_, ui, i, snow, dt);
                smelt = meltSnowpack(soa_, ui, temp, rmelt, asc, snow, dt);

                double vmelt = smelt * dt;
                vmelt = std::min(vmelt, soa_.wsnow[ui]);
                soa_.wsnow[ui] -= vmelt;
                const double rain_on_snow = rain * dt * asc;
                // S2b — melt moves wsnow -> fw INSIDE the pool (no age
                // change); the rain arrives from outside and mixes against
                // the pool as it is when it lands.
                if (soa_.track_age && rain_on_snow > 0.0) {
                    soa_.age[ui] = mixAge(soa_.wsnow[ui] + soa_.fw[ui],
                                          soa_.age[ui], rain_on_snow,
                                          soa_.precip_age);
                }
                soa_.fw[ui] += vmelt + rain_on_snow;
                vmelt = soa_.fw[ui] - soa_.fwfrac[ui] * soa_.wsnow[ui];
                vmelt = std::max(vmelt, 0.0);
                soa_.fw[ui] -= vmelt;
                smelt = vmelt / dt;
                // S2b — water draining out of a complete-mix pool leaves at
                // the pool's age.
                if (soa_.track_age) soa_.out_age[ui] = soa_.age[ui];
            }

            soa_.asc[ui]   = asc;
            soa_.imelt[ui] = smelt + imelt;
            if (soa_.track_age && !(soa_.imelt[ui] > 0.0)) soa_.out_age[ui] = 0.0;
        }
    }
}

// ============================================================================
// Seasonal melt coefficient interpolation
// (matching legacy snow.c snow_setMeltCoeffs)
// ============================================================================

void SnowSolver::setMeltCoeffs(int day_of_year) {
    // Compute seasonal factor: -1.0 at winter solstice (Dec 21, day ~355),
    // +1.0 at summer solstice (Jun 21, day ~172).
    // season = sin(0.0172615 * (day - 81))   — legacy `climate.c:1176`.
    //
    // The constant is NOT a mis-divided year, and it was retracted from the
    // divergence register once that was checked. It gives a period of
    // exactly 364.000 days, and with the day-81 phase offset (the vernal
    // equinox) the sine peaks at day 172.000 — the summer solstice, June 21.
    // 364 = 4 x 91 is what makes the equinox-to-solstice quarter a whole
    // number of days. Substituting 2*pi/365 moves the seasonal melt peak to
    // day 172.25, off the solstice, and buys nothing.
    constexpr double kSeasonRad = 0.0172615;   // rad/day; period 364.000 d
    double season = std::sin(kSeasonRad * (day_of_year - 81.0));
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
    std::vector<double> snow(static_cast<std::size_t>(n), snowfall);
    plowSnow(ctx, dt, snow.data());
}

void SnowSolver::plowSnow(SimulationContext& ctx, double dt, const double* snowfall) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    for (int j = 0; j < n; ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Add this subcatchment's snowfall to all subareas
        for (int k = SNOW_PLOWABLE; k <= SNOW_PERV; ++k) {
            auto idx = static_cast<std::size_t>(j * N_SUBAREAS + k);
            if (soa_.fArea[idx] > 0.0) {
                const double add = snowfall[uj] * dt;
                // S2b — age, THEN mix, and both before the store moves.
                //
                // This is the step's first snow call (SWMMEngine.cpp:1596
                // calls plowSnow immediately before execute, matching legacy
                // runoff.c:254), so it is where the pack ages. Ageing here
                // rather than in `execute` means new snow is mixed in at its
                // source age and is aged from the NEXT step, which is the
                // A3/A4 convention: arriving water is not older than the
                // instant it arrived.
                if (soa_.track_age) {
                    const double have = soa_.wsnow[idx] + soa_.fw[idx];
                    if (have > 0.0) soa_.age[idx] += dt;
                    if (add > 0.0)
                        soa_.age[idx] = mixAge(have, soa_.age[idx],
                                               add, soa_.precip_age);
                }
                soa_.wsnow[idx] += add;
                soa_.imelt[idx] = 0.0;
            }
        }
    }

    // S2b — ageing is a SEPARATE PASS over every subcatchment, completed
    // before any plowing starts. Interleaving the two ages water twice when
    // it is plowed into a subcatchment the loop has not reached yet, and not
    // at all when it goes to one already passed: the answer then depends on
    // subcatchment ORDER, which nothing else in this model does. Measured on
    // the two-subcatchment transfer gate: age-volume gained exactly
    // `dt * moved`, 60 against a 40180 baseline.
    for (int j = 0; j < n; ++j) {
        auto uj = static_cast<std::size_t>(j);

        // Check if plowable area has excess snow
        auto plow_idx = static_cast<std::size_t>(j * N_SUBAREAS + SNOW_PLOWABLE);
        if (soa_.fArea[plow_idx] <= 0.0) continue;
        // legacy snow.c:454: `if (wsnow >= weplow)` — an SDplow of 0 plows
        // every step (there was a `weplow <= 0` skip here).
        if (soa_.wsnow[plow_idx] < soa_.weplow[uj]) continue;

        double exc = soa_.wsnow[plow_idx];
        auto sf = static_cast<std::size_t>(j * 5); // sfrac base index
        double sfracTotal = 0.0;
        // S2b — the donor's age, read ONCE before any transfer moves water.
        //
        // Every receiving surface below takes water from this same pool, so
        // they must all mix against the age the pool had at the start of the
        // transfer, not against a value that the previous transfer moved.
        // Plowing does not touch `fw`, and removing water from a complete-mix
        // pool at the pool's own age leaves that age unchanged — so the
        // plowable surface's own age needs no update here at all.
        const double donor_age = soa_.track_age ? soa_.age[plow_idx] : 0.0;

        // Plow out of system (sfrac[0])
        // Accumulate removed volume: depth (ft) * plowable area fraction * non-LID area (ft2)
        // Gap #60: exclude LID area from plow volume (matches legacy snow.c Build 5.2.0).
        if (soa_.sfrac[sf + 0] > 0.0) {
            double lid_ft2 = (uj < ctx.subcatches.total_lid_area_ft2.size())
                             ? ctx.subcatches.total_lid_area_ft2[uj] : 0.0;
            // F9 — this read `* 43560.0` with the comment "acres -> ft2".
            // `subcatches.area` is in PROJECT land-area units, which are
            // acres only in US; in SI they are hectares, and the plough
            // volume came out **2.471x too small** — the identical defect
            // SWMMEngine.cpp's rainfall-volume site carries a comment about
            // having already fixed once.
            //
            // Fixed HERE, in the ledger round rather than its own, and the
            // reason is specific: `runoff_snowremov` had no writer until this
            // round, so this number reached nobody. Wiring it up is what
            // makes the error visible, and shipping a newly-visible wrong
            // number is worse than shipping no number at all.
            double area_ft2 = ctx.subcatches.area[uj] /
                                  ucf::UCF(ucf::LANDAREA, ctx.options)
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
            const double moved = soa_.sfrac[sf + 1] * exc * f;
            if (soa_.track_age && moved > 0.0) {
                soa_.age[imperv_idx] =
                    mixAge(soa_.wsnow[imperv_idx] + soa_.fw[imperv_idx],
                           soa_.age[imperv_idx], moved, donor_age);
            }
            soa_.wsnow[imperv_idx] += moved;
            sfracTotal += soa_.sfrac[sf + 1];
        }

        // Plow onto pervious area (sfrac[2])
        auto perv_idx = static_cast<std::size_t>(j * N_SUBAREAS + SNOW_PERV);
        if (soa_.fArea[perv_idx] > 0.0) {
            double f = soa_.fArea[plow_idx] / soa_.fArea[perv_idx];
            const double moved = soa_.sfrac[sf + 2] * exc * f;
            if (soa_.track_age && moved > 0.0) {
                soa_.age[perv_idx] =
                    mixAge(soa_.wsnow[perv_idx] + soa_.fw[perv_idx],
                           soa_.age[perv_idx], moved, donor_age);
            }
            soa_.wsnow[perv_idx] += moved;
            sfracTotal += soa_.sfrac[sf + 2];
        }

        // Convert to immediate melt (sfrac[3])
        if (dt > 0.0) {
            soa_.imelt[plow_idx] = soa_.sfrac[sf + 3] * exc / dt;
            // S2b — plough-melt leaves the pack at the pack's age. `execute`
            // runs next and will overwrite both `imelt` and `out_age` for
            // this surface (step 4 assigns `imelt`), so this write is not
            // what a caller reads; it is here so that the pair is never
            // inconsistent at any point in the step, which is the invariant
            // that makes the pair safe to read from anywhere later.
            if (soa_.track_age) soa_.out_age[plow_idx] = donor_age;
        }
        sfracTotal += soa_.sfrac[sf + 3];

        // Send to another subcatchment (sfrac[4])
        if (soa_.sfrac[sf + 4] > 0.0 && soa_.to_subcatch[uj] >= 0) {
            int m = soa_.to_subcatch[uj];
            if (m < n) {
                auto target_perv = static_cast<std::size_t>(m * N_SUBAREAS + SNOW_PERV);
                if (soa_.fArea[target_perv] > 0.0) {
                    double f = soa_.fArea[plow_idx] / soa_.fArea[target_perv];
                    const double moved = soa_.sfrac[sf + 4] * exc * f;
                    // S2b — the CROSS-SUBCATCHMENT transfer, and the reason
                    // the age has to live in this solver. An update running
                    // after `execute` sees only that subcatchment `m` gained
                    // snow; it cannot know it came from `j`, nor at what age.
                    if (soa_.track_age && moved > 0.0) {
                        soa_.age[target_perv] =
                            mixAge(soa_.wsnow[target_perv] + soa_.fw[target_perv],
                                   soa_.age[target_perv], moved, donor_age);
                    }
                    soa_.wsnow[target_perv] += moved;
                    sfracTotal += soa_.sfrac[sf + 4];
                }
            }
        }

        // Reduce plowable snow by total fraction plowed
        sfracTotal = std::min(sfracTotal, 1.0);
        soa_.wsnow[plow_idx] = exc * (1.0 - sfracTotal);
        // S2b — a surface plowed clean has no water, so it has no age.
        // Leaving the old value would let it survive as the mixing partner
        // for the next snowfall, which would make new snow arrive old.
        if (soa_.track_age &&
            !(soa_.wsnow[plow_idx] + soa_.fw[plow_idx] > 0.0)) {
            soa_.age[plow_idx] = 0.0;
        }
    }
}

} // namespace snow
} // namespace openswmm
