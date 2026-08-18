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
 * @file test_heat_radiative_exchange.cpp
 * @brief Phase H3 gates — shortwave and longwave radiation.
 *
 * @details The formulation gates carry values computed outside this
 *          codebase from `RHEComponent/src/element.cpp:106-135`, which is
 *          what makes them able to catch a transcription error. Two of them
 *          exist specifically because the plan's §2.2 summary omits detail
 *          the reference has:
 *
 *          - `SkyViewSplitsTheLongwaveBudget` — `Jan` carries `fsky` and
 *            `Jlc` carries `(1 − fsky)`. Written without it (the plan's
 *            spelling), an open-sky element double-counts.
 *          - `BruntEmissivityTakesPascals` — `0.0027 √(e_a · 1000)`. In kPa
 *            the term is understated by √1000 ≈ 31.6, which is a plausible
 *            small emissivity rather than an obviously wrong one, so only a
 *            reference value catches it.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.2, §6 H3
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/HeatFluxModules/RadiativeExchange.hpp"
#include "transport/components/HeatFluxModules/SurfaceExchange.hpp"

namespace re = openswmm::transport::heat;
namespace se = openswmm::transport::heat;  // same namespace; se:: reads as the H2 module

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kTWater = 20.0;
constexpr double kTAirC  = 10.0;   ///< 50 °F in the deck
constexpr double kRH     = 50.0;

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// Storage pool, no inflow, five minutes — the regime where a flux law is
/// linear in the state (H2 lesson 55: pick the regime, don't widen the band).
void write_deck(const char* path, const std::string& pc_lines,
                const char* end_time = "00:05:00") {
    std::ofstream f(path);
    f << "[TITLE]\nH3 radiative exchange gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << end_time << "\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      << "[TEMPERATURE]\nTIMESERIES air_ts\nHUMIDITY " << kRH << "\n\n"
      << "[TIMESERIES]\nair_ts 01/01/2026 00:00 50.0\n"
      << "air_ts 01/02/2026 00:00 50.0\n\n"
      << "[STORAGE]\nS1 10.0 12 6.0 FUNCTIONAL 0 0 5000\n\n"
      << "[JUNCTIONS]\nJ1 9.0 10 1.0 0 0\n\n"
      << "[OUTFALLS]\nOUT 8.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 S1 J1 500 0.013 0 0 0\nC2 J1 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\nC2 CIRCULAR 3.0 0 0 0\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

std::string heat_cfg(bool on, const std::string& rad_rows = "") {
    return std::string("[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
                       "[HEAT_FLUXES]\nRADIATIVE_EXCHANGE ") +
           (on ? "ON\n" : "OFF\n") +
           (rad_rows.empty() ? "" : "\n[RADIATIVE_FLUXES]\n" + rad_rows);
}

SWMM_Engine run_and_hold(const char* inp, const char* rpt, const char* out) {
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    if (swmm_engine_open(e, inp, rpt, out, nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 20000);
    swmm_engine_end(e);
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the four terms against RHEComponent's arithmetic.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, TermsMatchTheReferenceImplementation) {
    // Jbr = eps_w sigma Tw^4, water 20 C  (element.cpp:113-117)
    EXPECT_NEAR(re::backLongwave(kTWater, 0.97), 406.1761205, 1.0e-5);

    // Jan at open sky (element.cpp:119-129).
    EXPECT_NEAR(re::atmosphericLongwave(kTAirC, kRH, 0.5, 0.03, 1.0),
                200.4523032, 1.0e-5);

    // Jlc vanishes under an open sky and appears as the sky closes.
    EXPECT_NEAR(re::landCoverLongwave(kTAirC, 0.97, 1.0), 0.0, 1.0e-12);
    EXPECT_NEAR(re::landCoverLongwave(kTAirC, 0.97, 0.4), 212.1154523, 1.0e-5);

    // Jsn = (1 - Rs) Jin (1 - fs)  (element.cpp:106-111)
    EXPECT_NEAR(re::netShortwave(800.0, 0.08, 0.0), 736.0, 1.0e-9);
    EXPECT_NEAR(re::netShortwave(800.0, 0.08, 0.25), 552.0, 1.0e-9);
    // An over-unity shade factor shades completely; it does not invert.
    EXPECT_NEAR(re::netShortwave(800.0, 0.08, 1.5), 0.0, 1.0e-12);
}

// ---------------------------------------------------------------------------
// Gate 2 — Brunt's square root takes PASCALS.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, BruntEmissivityTakesPascals) {
    // e_a = 0.6159414661 kPa at 10 C / 50 % RH.
    // Correct:  0.5 + 0.0027*sqrt(615.94) = 0.56700905
    // In kPa:   0.5 + 0.0027*sqrt(0.6159) = 0.50211887  <- plausible, wrong
    EXPECT_NEAR(re::atmosphericEmissivity(0.6159414661, 0.5), 0.56700905,
                1.0e-7)
        << "atmospheric emissivity is off. A value near 0.502 means the "
           "vapour pressure went into sqrt() as kPa rather than Pa — the "
           "term is then understated by sqrt(1000), which looks like a "
           "plausible emissivity instead of an obvious error.";
}

// ---------------------------------------------------------------------------
// Gate 3 — the sky-view factor SPLITS the longwave budget.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, SkyViewSplitsTheLongwaveBudget) {
    // The plan's §2.2 spelling omits fsky from Jan. If it were absent, the
    // two terms would not be complementary and the total incoming longwave
    // would depend on fsky in the wrong direction. Assert the SUM is
    // invariant when both emissivities are equal — the physical meaning of
    // "shares of one hemisphere".
    // The third argument of atmosphericLongwave is Brunt's Aa COEFFICIENT,
    // not an emissivity: the atmospheric emissivity is Aa + 0.0027*sqrt(e_a
    // in Pa). Passing 0.97 there makes eps_atm = 1.037 — above unity, so the
    // two emissivities differ by 0.067 and the sum drifts 6.1 W/m2 per 0.25
    // of fsky whether or not the code is correct. Derive the land-cover
    // emissivity from the SAME call the atmospheric term uses, so "equal
    // emissivities" is true by construction rather than by hope.
    const double kAa = 0.5;   // an ordinary Brunt coefficient
    const double e_a =
        se::saturationVapourPressure(kTAirC) * kRH / 100.0;
    const double eps = re::atmosphericEmissivity(e_a, kAa);
    ASSERT_LT(eps, 1.0) << "atmospheric emissivity " << eps
                        << " exceeds unity — the coefficient is being read "
                           "as an emissivity again";
    double prev = -1.0;
    for (const double fsky : {0.0, 0.25, 0.5, 0.75, 1.0}) {
        const double jan =
            re::atmosphericLongwave(kTAirC, kRH, kAa, 0.0, fsky);
        const double jlc = re::landCoverLongwave(kTAirC, eps, fsky);
        const double total = jan + jlc;
        if (prev >= 0.0)
            EXPECT_NEAR(total, prev, 1.0e-9)
                << "incoming longwave changed with the sky-view factor at "
                   "equal emissivities (fsky = " << fsky << "). Jan must "
                   "carry fsky and Jlc (1 - fsky); dropping either makes an "
                   "open-sky element double-count.";
        prev = total;
    }
}

// ---------------------------------------------------------------------------
// Gate 4 — net flux sign, and the day/night reversal.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, NetFluxIsPositiveOutAtNightAndInByDay) {
    openswmm::RadiativeConfig cfg{};  // defaults: no sun, open sky
    const double night = re::netRadiativeFluxOut(kTWater, kTAirC, kRH, cfg);
    EXPECT_NEAR(night, 205.7238174, 1.0e-5)
        << "night-time net radiative flux should be Jbr - Jan = 406.176 - "
           "200.452, POSITIVE out of the water (the module's convention, "
           "opposite to RHE's).";

    cfg.shortwave_wm2 = 800.0;
    const double day = re::netRadiativeFluxOut(kTWater, kTAirC, kRH, cfg);
    EXPECT_LT(day, 0.0)
        << "800 W/m2 of sun must make the net flux negative (into the "
           "water); it reads " << day;
    EXPECT_NEAR(night - day, 800.0, 1.0e-6)
        << "with albedo 0 and no shade, all 800 W/m2 must enter";
}

// ---------------------------------------------------------------------------
// Gate 5 — the module warms a pool under sun and cools it at night.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, SunWarmsAndNightCoolsThePool) {
    write_file("_hr_night.heat", heat_cfg(true));
    write_deck("_hr_night.inp", "org.hydrocouple.openswmm.heat "
                                "config=\"_hr_night.heat\"");
    SWMM_Engine n = run_and_hold("_hr_night.inp", "_hr_night.rpt",
                                 "_hr_night.out");
    ASSERT_NE(n, nullptr);
    const double t_night = as_cpp_engine(n).context().heat_state.node_temp[0];
    swmm_engine_destroy(n);

    write_file("_hr_day.heat", heat_cfg(true, "SHORTWAVE GLOBAL 800.0\n"));
    write_deck("_hr_day.inp", "org.hydrocouple.openswmm.heat "
                              "config=\"_hr_day.heat\"");
    SWMM_Engine d = run_and_hold("_hr_day.inp", "_hr_day.rpt", "_hr_day.out");
    ASSERT_NE(d, nullptr);
    const double t_day = as_cpp_engine(d).context().heat_state.node_temp[0];
    swmm_engine_destroy(d);

    // SETUP FIRST: the module off must leave the pool at its seed, or the
    // two comparisons below are measuring something else.
    write_file("_hr_off.heat", heat_cfg(false));
    write_deck("_hr_off.inp", "org.hydrocouple.openswmm.heat "
                              "config=\"_hr_off.heat\"");
    SWMM_Engine o = run_and_hold("_hr_off.inp", "_hr_off.rpt", "_hr_off.out");
    ASSERT_NE(o, nullptr);
    const double t_off = as_cpp_engine(o).context().heat_state.node_temp[0];
    swmm_engine_destroy(o);
    EXPECT_NEAR(t_off, kTWater, 1.0e-9)
        << "RADIATIVE_EXCHANGE OFF left the pool at " << t_off
        << " degC, not its 20 degC seed";

    EXPECT_LT(t_night, kTWater)
        << "a 20 degC pool under a 10 degC sky must radiate away net heat; "
           "it reads " << t_night;
    EXPECT_GT(t_day, kTWater)
        << "800 W/m2 of sun must warm the pool; it reads " << t_day;
}

// ---------------------------------------------------------------------------
// Gate 6 — the config surface refuses what it cannot represent.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, ConfigRefusesOutOfRangeAndUnknown) {
    // An emissivity typed as a percentage would scale every longwave term
    // by ~100 and still produce finite, plausible-looking numbers.
    write_file("_hr_bad.heat", heat_cfg(true, "EMISS_WATER GLOBAL 97\n"));
    write_deck("_hr_bad.inp", "org.hydrocouple.openswmm.heat "
                              "config=\"_hr_bad.heat\"");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_NE(swmm_engine_open(e, "_hr_bad.inp", "_hr_bad.rpt", "_hr_bad.out",
                               nullptr),
              SWMM_OK)
        << "an emissivity of 97 was accepted as a fraction";
    swmm_engine_destroy(e);

    write_file("_hr_unk.heat", heat_cfg(true, "EXTINCTION GLOBAL 0.5\n"));
    write_deck("_hr_unk.inp", "org.hydrocouple.openswmm.heat "
                              "config=\"_hr_unk.heat\"");
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    EXPECT_NE(swmm_engine_open(e2, "_hr_unk.inp", "_hr_unk.rpt", "_hr_unk.out",
                               nullptr),
              SWMM_OK)
        << "EXTINCTION is the H4 sediment split and must not be silently "
           "ignored";
    swmm_engine_destroy(e2);
}

// ---------------------------------------------------------------------------
// Gate 7 — RADIATIVE_EXCHANGE is no longer deferred, and SEDIMENT still is.
// ---------------------------------------------------------------------------
TEST(HeatRadiativeExchangeTest, TheH3DeferralIsRetiredAndH4IsNot) {
    // H2 shipped a "RADIATIVE_EXCHANGE arrives with phase H3" error. Landing
    // H3 must flip that toggle to real — retiring a deferral means flipping
    // its gate in the same changeset (lesson 21).
    write_file("_hr_on.heat", heat_cfg(true));
    write_deck("_hr_on.inp", "org.hydrocouple.openswmm.heat "
                             "config=\"_hr_on.heat\"");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_hr_on.inp", "_hr_on.rpt", "_hr_on.out",
                               nullptr),
              SWMM_OK)
        << "RADIATIVE_EXCHANGE ON still refuses — the H3 deferral was not "
           "retired with the phase that implements it";
    EXPECT_TRUE(as_cpp_engine(e).context().heat_config.radiative_exchange);
    swmm_engine_destroy(e);

    write_file("_hr_sed.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nSEDIMENT_EXCHANGE ON\n");
    write_deck("_hr_sed.inp", "org.hydrocouple.openswmm.heat "
                              "config=\"_hr_sed.heat\"");
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    EXPECT_NE(swmm_engine_open(e2, "_hr_sed.inp", "_hr_sed.rpt", "_hr_sed.out",
                               nullptr),
              SWMM_OK)
        << "SEDIMENT_EXCHANGE is H4 and must still refuse";
    swmm_engine_destroy(e2);
}
