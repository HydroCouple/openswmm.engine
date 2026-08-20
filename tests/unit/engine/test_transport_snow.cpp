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
 * @file test_transport_snow.cpp
 * @brief Phase S1 gates — the snow mixing volume.
 *
 * @details **The first `[SNOWPACKS]` deck in this program.** No gate anywhere
 *          used one before, which is exactly why the defect S1 fixes went
 *          unobserved through A3, H5a, A4 and H5b.
 *
 * @par The configuration, and why it is the sharpest one available
 *      **No precipitation at all, and a pack seeded to melt.** Then
 *      `ctx.subcatches.rainfall` is exactly **0** while the solver's
 *      `snow_net_imperv` / `snow_net_perv` are positive. Water demonstrably
 *      arrives — there is runoff — but the field the transport modules used
 *      to read says none did.
 *
 *      That makes the defect gates reference-free. With the RAINFALL source
 *      set to 0, arriving meltwater can only pull the surface value DOWN:
 *      - the age must end **below the elapsed run time**, which is what it
 *        would be with no mixing at all;
 *      - the temperature must end **below INITIAL_STATE**.
 *      Neither needs an expected number, and under the defect the mixing
 *      volume is identically zero so neither can happen.
 *
 * @warning **Reachability is the risk here, not the assertions.** `sd0` and
 *          the melt coefficients are deck units, and issue #131 is the
 *          standing precedent for LID parameters reaching the solver
 *          unconverted. Every gate therefore asserts its SETUP — that a pack
 *          exists, that it is melting, and that rainfall really is zero —
 *          before asserting anything about transport. **If a SETUP fires,
 *          fix the deck; do not relax it.** Lesson 96: assert every term of
 *          the predicate you are trying to reach.
 *
 * @see plans/transport/SNOW_MIXING_VOLUME_FINDING_2026-08-20.md
 * @see src/engine/transport/components/WatershedCommon.hpp
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/WatershedCommon.hpp"

namespace tr = openswmm::transport;

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr int kNSubAge = static_cast<int>(openswmm::SubArea::COUNT_);

std::string series(const char* name, int last_min, double v) {
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        std::snprintf(buf, sizeof(buf), "%s 01/01/2026 %02d:%02d %.4f\n",
                      name, m / 60, m % 60, v);
        s += buf;
    }
    return s + "\n";
}

struct Opts {
    bool   water_age = false;
    bool   heat      = false;
    bool   pack      = true;
    bool   ignore_melt = false;
    double air_f     = 50.0;   ///< above tbase, so the pack melts
    double sd0       = 6.0;    ///< initial pack depth, deck units
    double fwfrac    = 0.10;   ///< free-water capacity, fraction of the pack
    double rain_h    = 0.0;    ///< arriving water is AGE ZERO
    double init_h    = 0.0;
    double rain_c    = 0.0;    ///< arriving water is 0 °C
    double init_c    = 25.0;
    int    end_min   = 60;
    double rain_inhr = 0.0;    ///< gage intensity; 0 on every gate but 7
    /// A SECOND subcatchment with no pack, on a project that HAS packs.
    /// Without this the only pack-less deck is a pack-less PROJECT, and
    /// `PostParseResolver.cpp:2199` forces IGNORE_SNOWMELT on there
    /// (legacy project.c:221) — so the guard such a deck exercises is the
    /// IGNORE_SNOWMELT one, not the per-subcatchment one.
    bool   bare_second = false;
};

void write_files(const Opts& o) {
    {
        std::ofstream f("_snow.age");
        f << "[WATER_AGE_SOURCES]\nRAINFALL GLOBAL " << o.rain_h
          << "\nINITIAL_STATE GLOBAL " << o.init_h << "\n";
    }
    {
        std::ofstream f("_snow.heat");
        f << "[HEAT_SOURCES]\nRAINFALL GLOBAL " << o.rain_c
          << "\nINITIAL_STATE GLOBAL " << o.init_c << "\n";
    }
    std::ofstream f("_snow.inp");
    f << "[TITLE]\nS1 snow mixing volume gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
      << (o.water_age ? "WATER_AGE ON\n" : "")
      << (o.heat ? "HEAT_TRANSPORT ON\n" : "")
      << (o.ignore_melt ? "IGNORE_SNOWMELT YES\n" : "")
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 0" << (o.end_min / 60) << ":"
      << (o.end_min % 60 < 10 ? "0" : "") << (o.end_min % 60) << ":00\n"
      << "WET_STEP 00:01:00\nDRY_STEP 00:01:00\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      // Air well above the pack's base melt temperature.
      // SNOWMELT is `Stemp ATIwt RNM Elev Lat DTLong` — the dividing
      // rain/snow temperature, the ATI weight, the negative-melt ratio,
      // elevation, latitude and the longitude correction. The melt
      // coefficients live on the pack rows, not here.
      << "[TEMPERATURE]\nTIMESERIES air_ts\n"
      << "SNOWMELT 0.5 0.5 0.6 40.0 0.0 0.0\n"
      << "WINDSPEED MONTHLY";
    for (int m = 0; m < 12; ++m) f << " 5.0";
    f << "\n\n[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
      // Rainfall is IDENTICALLY ZERO on every gate but 7. Every drop that
      // reaches the surface comes out of the pack, which is what makes the
      // defect visible.
      << "[TIMESERIES]\n" << series("rain_ts", o.end_min + 5, o.rain_inhr)
      << series("air_ts", o.end_min + 5, o.air_f);
    if (o.pack) {
        // The pack starts RIPE — `fw0` at the free-water capacity — and that
        // is not decoration. Melt does not leave a pack until its free water
        // exceeds `fwfrac * wsnow` (Snow.cpp step 6). At 0.10 of a 0.5 ft
        // pack that store is 0.05 ft, and the degree-day rate here fills it
        // in about 98 minutes: on a 60-minute deck an unripe pack melts
        // steadily and releases NOTHING, `snow_net_*` stay at 0.0, and every
        // gate below is unreachable. A ripe pack is also the physical state
        // this deck describes — one that is actively melting.
        const double fw0 = o.fwfrac * o.sd0;
        //          Name SURFACE     cmin cmax tbase fwfrac sd0  fw0 snn0
        f << "[SNOWPACKS]\n"
          << "SP1 PLOWABLE   0.02 0.06 32.0 " << o.fwfrac << " " << o.sd0
          << " " << fw0 << " 0.0\n"
          << "SP1 IMPERVIOUS 0.02 0.06 32.0 " << o.fwfrac << " " << o.sd0
          << " " << fw0 << " 0.0\n"
          << "SP1 PERVIOUS   0.01 0.03 32.0 " << o.fwfrac << " " << o.sd0
          << " " << fw0 << " 0.0\n"
          << "SP1 REMOVAL    0.0 0.0 0.0 0.0 0.0 0.0\n\n";
    }
    // The 9th token is the snowpack name; "*" means none.
    f << "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0 "
      << (o.pack ? "SP1" : "*") << "\n";
    if (o.bare_second) f << "S2 RG1 J1 5 50 500 0.5 0 *\n";
    f << "\n[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n";
    if (o.bare_second) f << "S2 0.01 0.1 0.02 0.02 25 OUTLET\n";
    f << "\n[INFILTRATION]\nS1 3.0 0.5 4 7 0\n";
    if (o.bare_second) f << "S2 3.0 0.5 4 7 0\n";
    f << "\n"
      << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
      << "[PROCESS_COMPONENTS]\n";
    if (o.water_age) f << "org.hydrocouple.openswmm.waterage config=\"_snow.age\"\n";
    if (o.heat)      f << "org.hydrocouple.openswmm.heat config=\"_snow.heat\"\n";
    f << "\n[REPORT]\nINPUT NO\n";
}

SWMM_Engine run(const Opts& o) {
    write_files(o);
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    if (swmm_engine_open(e, "_snow.inp", "_snow.rpt", "_snow.out", nullptr)
            != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 40000);
    swmm_engine_end(e);
    return e;
}

/// The SETUP every defect gate needs: a pack that is MELTING, with the
/// rainfall field at exactly zero. If any leg fails, the deck never reached
/// the state the gate is about and nothing below it means anything.
::testing::AssertionResult MeltingWithNoRain(
        const openswmm::SimulationContext& c) {
    if (c.n_subcatches() < 1)
        return ::testing::AssertionFailure() << "no subcatchment";
    if (c.subcatches.snowpack[0] < 0)
        return ::testing::AssertionFailure()
               << "S1 has no snowpack — the 9th [SUBCATCHMENTS] token did "
                  "not resolve to SP1, so this is a bare deck";
    if (c.subcatches.rainfall[0] != 0.0)
        return ::testing::AssertionFailure()
               << "rainfall is " << c.subcatches.rainfall[0]
               << ", not zero — the whole point of this deck is that the "
                  "field the defect read is empty while water still arrives";
    const double sni = c.subcatches.snow_net_imperv[0];
    const double snp = c.subcatches.snow_net_perv[0];
    if (!(sni >= 0.0) || !(snp >= 0.0))
        return ::testing::AssertionFailure()
               << "snow_net_imperv/perv are " << sni << " / " << snp
               << "; a negative value is the -1.0 'no pack' sentinel, so the "
                  "pack never published a net precip rate";
    if (!(sni > 0.0 || snp > 0.0))
        return ::testing::AssertionFailure()
               << "the pack published 0.0 on both surfaces — it exists but "
                  "is not melting. Raise the air temperature above tbase, or "
                  "the melt coefficients, or sd0. Do NOT relax this";
    return ::testing::AssertionSuccess();
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the deck itself. Water arrives from a melting pack while the
//          rainfall field reads zero. Everything below depends on this.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, AMeltingPackDeliversWaterWhileRainfallReadsZero) {
    Opts o{};
    o.water_age = true;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr) << "the snow deck failed to open or run at all";
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));

    EXPECT_GT(ctx.subcatches.stat_runoff_vol[0], 0.0)
        << "the pack is melting but the subcatchment produced no runoff, so "
           "no water reached the surface and the defect is unreachable here";

    // And the helper must agree with the solver, per subarea.
    EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, tr::kSubIMPERV0),
                     ctx.subcatches.snow_net_imperv[0]);
    EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, tr::kSubIMPERV1),
                     ctx.subcatches.snow_net_imperv[0]);
    EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, tr::kSubPERV),
                     ctx.subcatches.snow_net_perv[0]);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — AGE. Meltwater arrives at age 0, so the surface must end YOUNGER
//          than the elapsed run time. Under the defect the mixing volume is
//          identically zero and the age is pure elapsed time.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, MeltwaterMixesIntoTheSubareaAge) {
    Opts o{};
    o.water_age = true;
    o.rain_h = 0.0;      // arriving water is AGE ZERO
    o.init_h = 0.0;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));

    const double elapsed_s = o.end_min * 60.0;
    const auto& ws = ctx.water_age_state;
    ASSERT_GE(static_cast<int>(ws.subarea_age.size()), kNSubAge);

    bool any_mixed = false;
    for (int k = 0; k < kNSubAge; ++k) {
        const double age = ws.subarea_age[static_cast<std::size_t>(k)];
        EXPECT_TRUE(std::isfinite(age));
        // No reference value: with every source at age 0 and the run only
        // `elapsed_s` long, a surface that mixed in ANY arriving water is
        // younger than a surface that mixed in none.
        EXPECT_LE(age, elapsed_s + 1.0e-6)
            << "subarea " << k << " is older than the run itself";
        if (age < elapsed_s - 1.0)
            any_mixed = true;
    }
    EXPECT_TRUE(any_mixed)
        << "every subarea age equals the elapsed run time, which is what a "
           "surface reads when NOTHING mixes into it. Meltwater reached this "
           "subcatchment — the mixing volume is still being read from "
           "subcatches.rainfall, which is zero on this deck";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — TEMPERATURE. Same shape: melt arrives at the RAINFALL value,
//          which the deck sets to 0 °C, so a 25 °C surface must cool.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, MeltwaterMixesIntoTheSubareaTemperature) {
    Opts o{};
    o.heat = true;
    o.rain_c = 0.0;
    o.init_c = 25.0;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));
    ASSERT_FALSE(ctx.heat_config.surface_exchange)
        << "a flux module is on, so cooling could come from the atmosphere "
           "rather than from meltwater and this gate would not be about S1";

    const auto& hs = ctx.heat_state;
    ASSERT_GE(static_cast<int>(hs.subarea_temp.size()),
              openswmm::HeatState::kNSubArea);

    bool any_cooled = false;
    for (int k = 0; k < openswmm::HeatState::kNSubArea; ++k) {
        const double t = hs.subarea_temp[static_cast<std::size_t>(k)];
        EXPECT_TRUE(std::isfinite(t));
        // Nothing in this model is warmer than INITIAL_STATE and nothing is
        // colder than the melt source, so the answer is bracketed with no
        // reference value.
        EXPECT_LE(t, o.init_c + 1.0e-6) << "subarea " << k;
        EXPECT_GE(t, o.rain_c - 1.0e-6) << "subarea " << k;
        if (t < o.init_c - 1.0e-3) any_cooled = true;
    }
    EXPECT_TRUE(any_cooled)
        << "every subarea is still at INITIAL_STATE (" << o.init_c
        << " C) after an hour of meltwater at " << o.rain_c
        << " C arrived. With no flux module on, mixing is the only thing "
           "that can move this number — so nothing mixed in";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — the rate is PER-SUBAREA. The impervious value is an area-weighted
//          blend over plowable and non-plowable fractions; the pervious one
//          is not. A single scalar cannot be right for both.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, TheArrivingRateDiffersBetweenSubareas) {
    Opts o{};
    o.water_age = true;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));

    const double imp = ctx.subcatches.snow_net_imperv[0];
    const double per = ctx.subcatches.snow_net_perv[0];

    // SETUP: the deck gives the pervious surface slower melt coefficients
    // (cmin/cmax 0.01/0.03 against 0.02/0.06), so the two rates should
    // differ. If they do not, this deck cannot observe the per-subarea
    // claim and the assertion below would be vacuous.
    ASSERT_GT(std::fabs(imp - per), 1.0e-12)
        << "snow_net_imperv and snow_net_perv are identical (" << imp
        << "), so this deck does not distinguish the subareas. Widen the "
           "melt coefficients between the IMPERVIOUS and PERVIOUS rows";

    EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, tr::kSubIMPERV0), imp);
    EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, tr::kSubPERV), per);
    EXPECT_NE(tr::arrivingPrecipRate(ctx, 0, tr::kSubIMPERV0),
              tr::arrivingPrecipRate(ctx, 0, tr::kSubPERV))
        << "the helper returns the same rate for an impervious and a "
           "pervious subarea on a deck where the solver's own rates differ";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 5 — a subcatchment with NO pack is untouched. The regression guard:
//          A3 and H5a are shipped and heavily gated, and S1 must not move
//          them.
//
//          The deck carries TWO subcatchments and a `[SNOWPACKS]` section:
//          S1 has the pack, S2 has none. A pack-less PROJECT would not
//          exercise this at all — `PostParseResolver.cpp:2199` forces
//          IGNORE_SNOWMELT on when no snowpack is defined (legacy
//          project.c:221), so such a deck returns at the FIRST guard and
//          never reaches the per-subcatchment one.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, ASubcatchmentWithoutAPackReadsTheGageRate) {
    Opts o{};
    o.water_age = true;
    o.bare_second = true;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_GE(ctx.n_subcatches(), 2) << "the second subcatchment is missing";
    ASSERT_FALSE(ctx.options.ignore_snow_melt)
        << "IGNORE_SNOWMELT is on, so the first guard answers and this gate "
           "is about the wrong one";
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "S1 lost its pack";
    ASSERT_LT(ctx.subcatches.snowpack[1], 0)
        << "S2 built a pack despite '*' in the snowpack column";

    for (int k = 0; k < kNSubAge; ++k)
        EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 1, k),
                         ctx.subcatches.rainfall[1])
            << "subarea " << k << ": a pack-less subcatchment must read the "
               "gage rate unchanged, or S1 has moved every existing deck";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 6 — IGNORE_SNOWMELT falls back to the gage, matching Runoff.cpp:548
//          and legacy subcatch.c:784.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, IgnoreSnowmeltFallsBackToTheGageRate) {
    Opts o{};
    o.water_age = true;
    o.ignore_melt = true;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_TRUE(ctx.options.ignore_snow_melt)
        << "IGNORE_SNOWMELT did not parse, so this gate is about the wrong "
           "configuration";

    for (int k = 0; k < kNSubAge; ++k)
        EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, k),
                         ctx.subcatches.rainfall[0])
            << "subarea " << k << ": under IGNORE_SNOWMELT the transport "
               "mixing volume must follow the solver back to the gage rate";
    swmm_engine_destroy(e);

    // The leg above passes with the IGNORE_SNOWMELT check DELETED, because
    // the whole snow block is skipped for such a deck and `snow_net_*` never
    // leave the -1.0 sentinel. The check only earns its place when the flag
    // is raised on a run whose pack has ALREADY published a rate — the stale
    // arrays Runoff.cpp:551 calls harmless. IGNORE_SNOWMELT is settable at
    // runtime (openswmm_model_impl.cpp:1189), so that state is reachable.
    Opts m{};
    m.water_age = true;
    SWMM_Engine e2 = run(m);
    ASSERT_NE(e2, nullptr);
    auto& ctx2 = as_cpp_engine(e2).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx2));
    ASSERT_GT(ctx2.subcatches.snow_net_imperv[0], 0.0);
    ctx2.options.ignore_snow_melt = true;
    for (int k = 0; k < kNSubAge; ++k)
        EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx2, 0, k),
                         ctx2.subcatches.rainfall[0])
            << "subarea " << k << ": with the flag raised the solver returns "
               "to the gage rate and leaves snow_net_* stale at "
            << ctx2.subcatches.snow_net_imperv[0]
            << "; the transport mixing volume must follow it back";
    swmm_engine_destroy(e2);
}

// ---------------------------------------------------------------------------
// Gate 7 — the `>= 0.0` sentinel test, not `> 0.0`. A pack can publish a
//          genuine ZERO: below `tbase` nothing melts, and under full areal
//          cover no rain reaches the ground either, so
//          `imelt + rain·(1 − asc)` is exactly 0. That is not "no pack" — it
//          is a pack absorbing everything that falls on it. Reading the gage
//          instead would hand the surface rain that went into the snow.
//
//          `snow_net_*` initialise to −1.0 (SubcatchData.hpp:642) and the
//          solver's own test is `>= 0.0` (Runoff.cpp:551-552); only the
//          negative sentinel means "no pack".
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, APackAbsorbingRainPublishesAGenuineZero) {
    Opts o{};
    o.water_age = true;
    o.air_f = 20.0;      // below tbase: nothing melts
    o.rain_inhr = 0.5;   // but it IS raining, so the gage rate is not zero
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "no pack on this deck";

    // SETUP: the two numbers must actually differ, or the gate cannot tell
    // the sentinel test from the gage fallback.
    ASSERT_GT(ctx.subcatches.rainfall[0], 0.0)
        << "the gage delivered nothing, so a fallback to it is invisible";
    ASSERT_DOUBLE_EQ(ctx.subcatches.snow_net_imperv[0], 0.0)
        << "the pack published " << ctx.subcatches.snow_net_imperv[0]
        << " rather than a genuine zero — either it is melting (raise tbase "
           "or lower the air temperature) or its areal cover is below one, "
           "so some rain is reaching bare ground";
    ASSERT_DOUBLE_EQ(ctx.subcatches.snow_net_perv[0], 0.0);

    for (int k = 0; k < kNSubAge; ++k)
        EXPECT_DOUBLE_EQ(tr::arrivingPrecipRate(ctx, 0, k), 0.0)
            << "subarea " << k << ": the pack absorbed every drop, but the "
               "mixing volume reads the gage rate of "
            << ctx.subcatches.rainfall[0]
            << " — a genuine 0.0 was mistaken for the 'no pack' sentinel";

    // And the transport consequence: nothing arrived, so nothing mixed and
    // every surface is exactly as old as the run.
    const auto& ws = ctx.water_age_state;
    ASSERT_GE(static_cast<int>(ws.subarea_age.size()), kNSubAge);
    for (int k = 0; k < kNSubAge; ++k)
        EXPECT_NEAR(ws.subarea_age[static_cast<std::size_t>(k)],
                    o.end_min * 60.0, 1.0)
            << "subarea " << k << " is younger than the run, so water was "
               "mixed into a surface the pack never let anything reach";
    swmm_engine_destroy(e);
}
