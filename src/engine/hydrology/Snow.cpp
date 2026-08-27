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
// Rain-on-snow melt rate (legacy: getRainmelt) — one rainfall value
// ============================================================================

double SnowSolver::rainMeltRate(double temp, double wind, double gamma,
                                 double ea, double rainfall) {
    // Only applies when rainfall > 0.02 in/hr converted to ft/sec
    const double ucf_rain_us = ucf::Ucf[ucf::RAINFALL][0]; // US: in/hr ↔ ft/sec
    if (rainfall <= 0.02 / ucf_rain_us) return 0.0;

    double rain_in_hr = rainfall * ucf_rain_us;  // ft/sec → in/hr
    double uadj = 0.006 * wind;
    double t1 = temp - 32.0;
    double t2 = 7.5 * gamma * uadj;
    double t3 = 8.5 * uadj * (ea - 0.18);
    double smelt_in_hr = t1 * (0.001167 + t2 + 0.007 * rain_in_hr) + t3;
    return std::max(smelt_in_hr / ucf_rain_us, 0.0);  // in/hr → ft/sec
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
// Execute — all subcatchments batch
// ============================================================================

void SnowSolver::execute(SimulationContext& /*ctx*/, double dt,
                          double temp, double wind, const double* rainfall,
                          const double* snowfall, double gamma, double ea) {
    int n = soa_.n_subcatch;
    if (n == 0) return;
    int total = n * N_SUBAREAS;

    // -----------------------------------------------------------------------
    // Step 0 (Gap #19): 0.001-inch minimum pack threshold.
    // Packs thinner than this are melted instantly (matching legacy).
    // -----------------------------------------------------------------------
    constexpr double MIN_PACK_FT = 0.001 / 12.0;
    // S3: held aside rather than written into `imelt`. Steps 4 and 5 ASSIGN
    // `imelt` unconditionally, so the old `imelt +=` here was overwritten,
    // and step 5 then zeroed it because `wsnow` is 0 by that point — the
    // water of an instantly-melted thin pack was silently discarded. It is
    // added back after the routing loop, where nothing reassigns it.
    std::vector<double> instant_melt(static_cast<std::size_t>(total), 0.0);
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double ws = soa_.wsnow[ui];
        if (ws > 0.0 && ws <= MIN_PACK_FT) {
            instant_melt[ui] = (ws + soa_.fw[ui]) / dt;
            // S2b — the water leaves at the age the pack HAD. Reading
            // `age` after the pack is emptied would give 0, which is the
            // age of water that fell this instant; this water did not.
            if (soa_.track_age) {
                soa_.out_age[ui] = soa_.age[ui];
                soa_.age[ui]     = 0.0;   // nothing left to carry an age
            }
            soa_.imelt[ui]   = 0.0;
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
        soa_.asc[ui] = getArealDepletion(soa_, ui, subarea,
                                         snowfall[i / N_SUBAREAS], dt);
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
    // Step 4: Compute melt rate — degree-day or rain-on-snow, selected per
    // subcatchment from that subcatchment's own rainfall (matching legacy
    // meltSnowpack: rmelt > 0 wins, else degree-day).
    // -----------------------------------------------------------------------
    const double RAIN_THRESHOLD = 0.02 / ucf::Ucf[ucf::RAINFALL][0];
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double rain_sc = rainfall[i / N_SUBAREAS];
        if (rain_sc > RAIN_THRESHOLD) {
            soa_.imelt[ui] = rainMeltRate(temp, wind, gamma, ea, rain_sc);
        } else {
            double excess = temp - soa_.tbase[ui];
            soa_.imelt[ui] = (excess > 0.0) ? soa_.dhm[ui] * excess : 0.0;
        }
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
    // Step 6: Route melt through the free-water store, and update SWE.
    //
    // S3 — this is legacy `routeSnowmelt` (snow.c) in its own order, and the
    // order is the whole point. The previous form split this across two
    // loops and diverged three ways:
    //
    //   (A) SWE was reduced by the DRAINED EXCESS, not by the melt, because
    //       step 6 overwrote `imelt` with the excess before step 7 read it.
    //       Snow that melted but stayed within the free-water capacity was
    //       therefore counted TWICE — still snow, and also free water — and
    //       a pack whose melt never exceeded its capacity never depleted.
    //   (B) Rain falling on the snow-COVERED fraction was dropped entirely.
    //       It is excluded from what reaches the ground (`snow_net` carries
    //       `rain·(1 − asc)`), and it was never added to the pack either, so
    //       it left the water balance altogether.
    //   (C) The free-water capacity was taken from the PRE-melt SWE.
    //
    // All three were unreachable until `274b6506` gave `setMeltCoeffs` its
    // caller: with `dhm` at zero there was no degree-day melt to mis-account.
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (soa_.wsnow[ui] <= 0.0) continue;

        // (A) SWE falls by the MELT, before any free-water bookkeeping.
        double vmelt = std::min(soa_.imelt[ui] * dt, soa_.wsnow[ui]);
        soa_.wsnow[ui] -= vmelt;

        // (B) The melt AND the rain that fell on the covered fraction both
        //     enter the free-water store.
        const double rain_on_snow =
            rainfall[static_cast<std::size_t>(i / N_SUBAREAS)] * dt *
            soa_.asc[ui];
        // S2b — `vmelt` moves wsnow -> fw INSIDE the pool, so it carries no
        // age change; the rain arrives from outside and does. Mixed against
        // the post-melt total, which is the water actually in the pool when
        // the rain lands. Order matters: this must run BEFORE `fw` is
        // updated, because `mixAge` needs the pool as it was.
        if (soa_.track_age && rain_on_snow > 0.0) {
            soa_.age[ui] = mixAge(soa_.wsnow[ui] + soa_.fw[ui], soa_.age[ui],
                                  rain_on_snow, soa_.precip_age);
        }
        soa_.fw[ui] += vmelt + rain_on_snow;

        // (C) Capacity is measured against the SWE that is left.
        double excess = soa_.fw[ui] - soa_.fwfrac[ui] * soa_.wsnow[ui];
        excess        = std::max(excess, 0.0);
        soa_.fw[ui]  -= excess;
        soa_.imelt[ui] = excess / dt;
        // S2b — water draining out of a complete-mix pool leaves at the
        // pool's age and does not change it. Recorded unconditionally, not
        // only when `excess > 0`: a step that drains nothing must not leave
        // a previous step's departing age standing for a caller to read.
        if (soa_.track_age) soa_.out_age[ui] = soa_.age[ui];
    }

    // -----------------------------------------------------------------------
    // Step 7: Non-negativity, and the instant-melt water added back.
    // -----------------------------------------------------------------------
    for (int i = 0; i < total; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa_.wsnow[ui] = std::max(soa_.wsnow[ui], 0.0);
        soa_.fw[ui]    = std::max(soa_.fw[ui], 0.0);
        // Step 0's water. Nothing below reassigns `imelt`, which is exactly
        // what the previous placement got wrong.
        // S2b — the two contributions are mutually exclusive TODAY: step 0
        // zeroes `wsnow`, and both step 5 and step 6 skip a surface with no
        // snow, so a surface with instant melt has `imelt == 0` from the
        // routing loop. The blend below therefore degenerates to a copy.
        // Written as a blend anyway because the exclusivity is a property of
        // three separate guards agreeing, not of anything that says so — and
        // a `+=` that assumes one of its two terms is always zero is how F2
        // was written.
        if (soa_.track_age && instant_melt[ui] > 0.0) {
            soa_.out_age[ui] = mixAge(soa_.imelt[ui], soa_.out_age[ui],
                                      instant_melt[ui], soa_.out_age[ui]);
        }
        soa_.imelt[ui] += instant_melt[ui];
        // S2b — a surface that published nothing must not leave a PREVIOUS
        // step's departing age standing. Callers gate on `imelt > 0`, so
        // this is belt-and-braces; it is here because a stale age that only
        // becomes visible when a guard elsewhere is relaxed is exactly the
        // kind of thing that gets found four rounds later.
        if (soa_.track_age && !(soa_.imelt[ui] > 0.0)) soa_.out_age[ui] = 0.0;
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
        if (soa_.weplow[uj] <= 0.0) continue;
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
