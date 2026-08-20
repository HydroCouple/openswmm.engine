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
#include "hydrology/Snow.hpp"
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
    /// A RIPE pack starts with its free-water store full, so melt leaves
    /// from the first step. Set false for a pack with capacity to SPARE —
    /// which after S3 is the only way a pack can absorb rain rather than
    /// transmit it, because a full store passes every drop straight through.
    bool   ripe      = true;
    double rain_h    = 0.0;    ///< arriving water is AGE ZERO
    double init_h    = 0.0;
    double rain_c    = 0.0;    ///< arriving water is 0 °C
    double init_c    = 25.0;
    int    end_min   = 60;
    double rain_inhr = 0.0;    ///< gage intensity; 0 on every gate but 7
    /// Areal snow cover, written as a flat `ADC` curve. The default curve is
    /// all ones (`Snow.hpp:92`) and `si` is pinned to the INITIAL pack depth
    /// (`SWMMEngine.cpp:5613`, the deck's SD100 field is not read), so
    /// without this every deck here is at 100 % cover — and then
    /// `rain·(1 − asc)` is identically zero and no rain ever reaches the
    /// ground. A partial cover is the only way to get both waters at once.
    double adc_cover = 1.0;
    /// Plowable fraction of the impervious area (`snn0`, the 8th field of
    /// the PLOWABLE row). Zero everywhere else, which makes `fPlow` zero and
    /// the impervious area blend degenerate to its non-plowable term — so
    /// without this no gate can see the blend at all.
    double snn0 = 0.0;
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
      << "SNOWMELT 0.5 0.5 0.6 40.0 0.0 0.0\n";
    if (o.adc_cover < 1.0) {
        // Flat curve: this fraction of the surface is bare at every
        // depletion index, so `rain·(1 − asc)` is a live term.
        for (const char* surf : {"IMPERVIOUS", "PERVIOUS"}) {
            f << "ADC " << surf;
            for (int i = 0; i < 10; ++i) f << " " << o.adc_cover;
            f << "\n";
        }
    }
    f << "WINDSPEED MONTHLY";
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
        const double fw0 = o.ripe ? o.fwfrac * o.sd0 : 0.0;
        //          Name SURFACE     cmin cmax tbase fwfrac sd0  fw0 snn0
        f << "[SNOWPACKS]\n"
          << "SP1 PLOWABLE   0.02 0.06 32.0 " << o.fwfrac << " " << o.sd0
          << " " << fw0 << " " << o.snn0 << "\n"
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
    // NOT ripe. S3 routes rain that falls on the covered fraction into the
    // free-water store, and a store already at capacity passes every drop
    // straight back out as `imelt` — the pack transmits rather than absorbs,
    // and `snow_net` is then the whole gage rate rather than zero. That is
    // correct physics for a ripe pack, and it is not what this gate is
    // about. An unripe pack at 20 F is also the physical state here.
    o.ripe = false;
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

// ---------------------------------------------------------------------------
// Gate 8 — S2a. MELTWATER ARRIVES AT 0 °C, not at the configured RAINFALL
//          temperature. This is the gate that fails on S1 alone.
//
//          The deck sets RAINFALL to 20 °C deliberately — NOT to 0 as the
//          S1 gates do. With zero rain and a melting pack, every drop that
//          arrives is meltwater, so S1's volume fix would carry it in at
//          20 °C. Melting happens at the freezing point; 20 °C meltwater is
//          not a small error on a winter deck, it is the difference between
//          a snowmelt-fed stream and a rain-fed one.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, MeltwaterArrivesAtFreezingNotAtTheRainTemperature) {
    Opts o{};
    o.heat = true;
    o.rain_c = 20.0;      // the S1-only answer, and the wrong one
    o.init_c = 25.0;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));
    ASSERT_FALSE(ctx.heat_config.surface_exchange)
        << "a flux module is on, so cooling could come from the atmosphere";

    // SETUP: with no rain, ALL arriving water is melt. If the fraction is
    // not 1 the deck is not the configuration this gate is about.
    for (int k = 0; k < kNSubAge; ++k) {
        const double f = tr::arrivingMeltFraction(ctx, 0, k);
        ASSERT_NEAR(f, 1.0, 1.0e-12)
            << "subarea " << k << ": melt fraction is " << f
            << " on a deck with zero rainfall — every drop arriving here "
               "came out of the pack, so it must be 1";
        EXPECT_DOUBLE_EQ(tr::arrivingPrecipTemperature(ctx, 0, k),
                         tr::kMeltwaterTempC)
            << "subarea " << k << ": pure meltwater is not arriving at the "
               "freezing point";
    }

    const auto& hs = ctx.heat_state;
    bool any_below_rain = false;
    for (int k = 0; k < openswmm::HeatState::kNSubArea; ++k) {
        const double t = hs.subarea_temp[static_cast<std::size_t>(k)];
        EXPECT_TRUE(std::isfinite(t));
        EXPECT_GE(t, tr::kMeltwaterTempC - 1.0e-6)
            << "subarea " << k << " is colder than the meltwater feeding it";
        if (t < o.rain_c - 1.0e-3) any_below_rain = true;
    }
    EXPECT_TRUE(any_below_rain)
        << "every subarea settled at or above the configured RAINFALL "
           "temperature (" << o.rain_c << " C). That is the S1-only answer: "
           "the right VOLUME of water arriving at the wrong VALUE";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 9 — the BLEND, not the endpoint. With rain AND a pack, part of the
//          arriving water is melt at 0 °C and part is rain that reached the
//          ground through the snow-free fraction. The result must lie
//          strictly between them — no reference value.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, RainThroughAndMeltBlendStrictlyBetweenTheirSources) {
    Opts o{};
    o.heat = true;
    o.rain_c = 20.0;
    o.init_c = 25.0;
    // Half the surface bare, and enough rain that the two waters are of
    // comparable size: measured f = 0.43, so the answer sits near the middle
    // of its own range rather than a hair from an endpoint.
    o.adc_cover = 0.5;
    o.rain_inhr = 0.2;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "no pack on this deck";

    bool any_blended = false;
    for (int k = 0; k < kNSubAge; ++k) {
        const double f = tr::arrivingMeltFraction(ctx, 0, k);
        EXPECT_GE(f, 0.0) << "subarea " << k;
        EXPECT_LE(f, 1.0) << "subarea " << k
            << ": a melt fraction above 1 means more water melted than "
               "arrived, so the two area blends have diverged";
        const double t = tr::arrivingPrecipTemperature(ctx, 0, k);
        // Bracketed by its own two sources, whatever the fraction is.
        EXPECT_GE(t, std::min(tr::kMeltwaterTempC, o.rain_c) - 1.0e-9);
        EXPECT_LE(t, std::max(tr::kMeltwaterTempC, o.rain_c) + 1.0e-9);
        if (f > 1.0e-6 && f < 1.0 - 1.0e-6) {
            any_blended = true;
            // STRICTLY between, with room to spare. Bracketing alone is
            // satisfied by either endpoint, which is precisely the answer a
            // gate about a blend must not accept: returning `t_rain`
            // unchanged (S1) or 0 for everything both pass a non-strict
            // check.
            EXPECT_GT(t, tr::kMeltwaterTempC + 1.0e-3)
                << "subarea " << k << ": melt fraction is " << f
                << ", so part of this water is rain at " << o.rain_c
                << " C, but the arriving temperature is the melt endpoint";
            EXPECT_LT(t, o.rain_c - 1.0e-3)
                << "subarea " << k << ": melt fraction is " << f
                << ", so part of this water is meltwater at "
                << tr::kMeltwaterTempC
                << " C, but the arriving temperature is the rain endpoint — "
                   "that is the S1-only answer";
        }
    }
    // SETUP, reported rather than asserted: a deck where the cover happens
    // to be total or absent gives f == 1 or 0 everywhere, and then this gate
    // has checked the endpoints again rather than the blend.
    if (!any_blended)
        GTEST_SKIP() << "no subarea saw a partial melt fraction on this "
                        "deck — areal cover is total or absent, so the "
                        "blend itself is untested here. Adjust the ADC "
                        "curve or the rain intensity rather than deleting "
                        "this gate";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 10 — no pack, no change. S2a must be inert everywhere a pack is not
//           involved, or it has moved every existing heat deck.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, WithoutAPackTheArrivingTemperatureIsTheConfiguredOne) {
    Opts o{};
    o.heat = true;
    o.pack = false;
    o.rain_c = 20.0;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_LT(ctx.subcatches.snowpack[0], 0);

    for (int k = 0; k < kNSubAge; ++k) {
        EXPECT_DOUBLE_EQ(tr::arrivingMeltFraction(ctx, 0, k), 0.0)
            << "subarea " << k << ": a pack-less subcatchment reported a "
               "melt fraction";
        EXPECT_DOUBLE_EQ(tr::arrivingPrecipTemperature(ctx, 0, k), o.rain_c)
            << "subarea " << k << ": S2a moved the arriving temperature on a "
               "deck with no snow anywhere";
    }
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 11 — the published melt term carries the SAME plowable/non-plowable
//           area blend as `snow_net_imperv`. If it did not, the melt fraction
//           would be a ratio of two differently-weighted numbers and could
//           exceed 1 on a plowed catchment.
//
//           Two things have to be true at once for this to be observable, and
//           neither holds on any other deck here: `snn0 > 0`, so a plowable
//           surface exists at all; and partial areal cover, because the
//           plowable surface is never depleted (`Snow.cpp:100` returns 1.0
//           for it) while the others are — which is what makes the two melt
//           rates differ in the first place.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, ThePublishedMeltTermCarriesTheAreaBlend) {
    Opts o{};
    o.heat = true;
    o.rain_c = 20.0;
    o.adc_cover = 0.5;
    o.rain_inhr = 0.2;
    o.snn0 = 0.4;          // 40 % of the impervious area is plowable
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    auto& eng = as_cpp_engine(e);
    const auto& ctx = eng.context();
    const auto& sn = eng.snowSolver().state();
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "no pack on this deck";

    const auto plow = static_cast<std::size_t>(openswmm::snow::SNOW_PLOWABLE);
    const auto imp  = static_cast<std::size_t>(openswmm::snow::SNOW_IMPERV);

    // SETUP: a plowable surface must exist, and its melt rate must differ
    // from the non-plowable one, or an area blend and a raw read agree.
    ASSERT_GT(sn.fArea[plow], 0.0)
        << "the plowable fraction is zero, so the impervious blend is just "
           "its non-plowable term and this gate cannot see the weighting";
    ASSERT_GT(std::fabs(sn.imelt[plow] - sn.imelt[imp]), 1.0e-12)
        << "the plowable and non-plowable surfaces are melting at the same "
           "rate (" << sn.imelt[plow] << "), so any weighting of them gives "
           "the same answer";

    const double lo = std::min(sn.imelt[plow], sn.imelt[imp]);
    const double hi = std::max(sn.imelt[plow], sn.imelt[imp]);
    const double published = ctx.subcatches.snow_melt_imperv[0];

    // STRICTLY between: a weighted mean of two distinct values with both
    // weights positive cannot land on either endpoint, and reading one
    // surface raw lands exactly on one.
    EXPECT_GT(published, lo + 1.0e-15)
        << "snow_melt_imperv is " << published << ", the lower of the two "
           "surface rates — it was read raw rather than area-weighted";
    EXPECT_LT(published, hi - 1.0e-15)
        << "snow_melt_imperv is " << published << ", the higher of the two "
           "surface rates — it was read raw rather than area-weighted";

    // And the fraction it feeds stays a fraction.
    for (int k = 0; k < kNSubAge; ++k) {
        const double f = tr::arrivingMeltFraction(ctx, 0, k);
        EXPECT_GE(f, 0.0) << "subarea " << k;
        EXPECT_LE(f, 1.0) << "subarea " << k
            << ": more water melted than arrived, which is what a mismatched "
               "pair of area blends produces";
    }
    swmm_engine_destroy(e);
}

// ===========================================================================
// S3 — the snowpack water balance. Four divergences from legacy
// `routeSnowmelt`, all of them unreachable until `274b6506` gave
// `setMeltCoeffs` its caller.
//
// Every gate below is a CONSERVATION statement, so none needs a reference
// value. That matters more here than usual: the correct magnitudes depend on
// melt coefficients, cover and timestep, and a gate pinned to one deck's
// numbers would rot the moment any of them changed.
// ===========================================================================

namespace {

/// Total water the pack holds on one snow surface, ft: snow plus the liquid
/// held in its pores. Both stores, because the divergence S3 fixes was
/// precisely water moving between them without being debited from the first.
double packWater(const openswmm::snow::SnowSoA& s, std::size_t idx) {
    return s.wsnow[idx] + s.fw[idx];
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 12 — A SLOWLY MELTING PACK DEPLETES. The gate that fails on the
//           pre-S3 form.
//
//           SWE used to be reduced by the DRAINED EXCESS rather than by the
//           melt, so snow that melted but stayed within the free-water
//           capacity was counted twice — still snow, and also free water.
//           A pack melting slower than its capacity never lost any SWE at
//           all.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, ASlowlyMeltingPackLosesSnowWaterEquivalent) {
    Opts o{};
    o.water_age = true;
    o.fwfrac = 0.90;      // a large store: melt stays inside it for a long
    o.air_f  = 40.0;      // time, which is the regime the defect lived in
    o.end_min = 60;
    // ...and the store must have ROOM. A ripe pack starts at capacity, so
    // every drop of melt drains the instant it appears and "SWE minus the
    // melt" and "SWE minus the drained excess" are the SAME NUMBER — the
    // defect is arithmetically invisible. Measured: with the shared writer's
    // ripe default this gate passes with F2 fully restored.
    o.ripe = false;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& s   = as_cpp_engine(e).snowSolver().state();
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "no pack on this deck";

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);
    const double sd0_ft = o.sd0 / 12.0;
    // Everything the pack was ever given: the SWE *and* the free water it
    // starts ripe with. Comparing against the SWE alone reads a 0.90
    // capacity as mass creation.
    const double given = sd0_ft + (o.ripe ? o.fwfrac * sd0_ft : 0.0);

    // SETUP: the pack must have been melting. If nothing melted, "it did not
    // deplete" proves nothing (lesson 96 — assert the whole predicate).
    ASSERT_LT(packWater(s, perv), given + 1.0e-9)
        << "the pack holds MORE water than it started with and nothing was "
           "added — that is the mass-creation signature, in its purest form";

    EXPECT_LT(s.wsnow[perv], sd0_ft - 1.0e-9)
        << "SWE is still " << s.wsnow[perv] << " ft against an initial "
        << sd0_ft << " ft after an hour of melt. A pack melting slower than "
           "its free-water capacity is not losing snow: SWE is being reduced "
           "by the drained excess instead of by the melt, so the melted water "
           "is counted as snow AND as free water";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 13 — RAIN ON A COVERED PACK IS NOT LOST. Under full cover no rain
//           reaches the ground (`snow_net` carries `rain·(1 − asc)`), so if
//           it also never enters the pack it has left the model.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, RainFallingOnACoveredPackEntersTheFreeWaterStore) {
    Opts o{};
    o.water_age = true;
    o.rain_inhr = 0.20;    // well above the rain-on-snow threshold
    // BELOW tbase, so the pack is not melting. At 40 F the free-water store
    // fills with MELTWATER whether or not the rain reaches it, and `fw > 0`
    // below says nothing about the rain — measured: this gate passes with F3
    // fully restored. Cold content absorbs the rain-on-snow term, so the
    // only thing that can put water in the store is the rain itself.
    o.air_f     = 20.0;
    o.fwfrac    = 0.90;    // capacity large enough to HOLD the rain, so this
    o.end_min   = 30;      // gate observes storage rather than drainage
    // Empty to start with, or `fw > 0` below is satisfied by the pack's
    // INITIAL free water and says nothing about the rain. Measured: with the
    // ripe default this gate passes with F3 fully restored.
    o.ripe      = false;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& s   = as_cpp_engine(e).snowSolver().state();
    ASSERT_GE(ctx.subcatches.snowpack[0], 0);

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);

    // SETUP: full cover, so every drop landed on snow and none of it is in
    // `snow_net`. Without this the gate cannot tell "entered the pack" from
    // "fell on bare ground".
    ASSERT_NEAR(s.asc[perv], 1.0, 1.0e-9)
        << "areal cover is " << s.asc[perv] << ", not 1 — some rain reached "
           "the ground directly and this gate cannot attribute the water";
    // SETUP: and nothing melted, or the store fills from the snow instead.
    ASSERT_NEAR(s.wsnow[perv], o.sd0 / 12.0, 1.0e-9)
        << "SWE moved from " << (o.sd0 / 12.0) << " to " << s.wsnow[perv]
        << " ft, so the pack IS melting and the free water below could have "
           "come from the snow rather than from the rain";

    EXPECT_GT(s.fw[perv], 0.0)
        << "the free-water store is empty after 30 minutes of rain onto a "
           "fully-covered pack. That rain is excluded from snow_net by "
           "construction, so if it is not here it is nowhere";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 14 — AN INSTANTLY-MELTED THIN PACK DELIVERS ITS WATER. Step 0 melts
//           packs under 0.001 in outright; steps 4 and 5 used to overwrite
//           the `imelt` it wrote, discarding the water.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, AnInstantlyMeltedThinPackDeliversItsWater) {
    Opts o{};
    o.water_age = true;
    o.sd0    = 0.0005;    // BELOW the 0.001-inch instant-melt threshold
    o.air_f  = 40.0;
    o.end_min = 15;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& s   = as_cpp_engine(e).snowSolver().state();
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "no pack was built";

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);

    // SETUP: the pack must be GONE — that is what "instantly melted" means,
    // and it is what distinguishes this branch from ordinary melt.
    ASSERT_NEAR(packWater(s, perv), 0.0, 1.0e-12)
        << "the pack still holds " << packWater(s, perv)
        << " ft, so the sub-threshold branch never fired and this gate is "
           "about a pack that melted normally";

    EXPECT_GT(ctx.subcatches.stat_runoff_vol[0], 0.0)
        << "a pack was melted instantly and the subcatchment produced no "
           "runoff at all. The water went nowhere: step 0 wrote it into "
           "imelt and steps 4-5 assigned over it";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 15 — the pack never holds more than it was given. The blunt
//           conservation ceiling, and the one assertion here that no change
//           to melt physics can invalidate.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, ThePackNeverGainsWaterItWasNotGiven) {
    Opts o{};
    o.water_age = true;
    o.rain_inhr = 0.0;     // NOTHING is added: no rain, no snowfall
    o.air_f     = 40.0;
    o.fwfrac    = 0.90;
    o.end_min   = 60;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& s   = as_cpp_engine(e).snowSolver().state();
    ASSERT_GE(ctx.subcatches.snowpack[0], 0);
    ASSERT_DOUBLE_EQ(ctx.subcatches.rainfall[0], 0.0)
        << "something is falling on this deck, so the pack CAN legitimately "
           "gain water and the ceiling below is not a conservation statement";

    const double sd0_ft = o.sd0 / 12.0;
    const double fw0_ft = o.fwfrac * sd0_ft;
    const double given  = sd0_ft + fw0_ft;

    for (int k = 0; k < openswmm::snow::N_SUBAREAS; ++k) {
        const auto idx = static_cast<std::size_t>(k);
        EXPECT_LE(packWater(s, idx), given + 1.0e-9)
            << "snow surface " << k << " holds " << packWater(s, idx)
            << " ft against " << given << " ft ever put into it, with no "
               "precipitation on this deck. Water is being created";
    }
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 15b — the free-water store never exceeds its CURRENT capacity.
//
//            The capacity is `fwfrac · wsnow`, and `wsnow` shrinks as the
//            pack melts, so a shrinking pack has to give up stored water as
//            well as its melt. Measuring the capacity against the PRE-melt
//            SWE instead leaves the store one step's worth above the line
//            every step — the pack holds water it no longer has the snow to
//            hold. Measured on this deck: 2148.145 ft3 of runoff against
//            2125.994 with the capacity taken pre-melt, a 1.0 % retiming.
//
//            Numbered 15b so S4's gates 16 and 17 keep the numbers its
//            handoff assigned them.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, TheFreeWaterStoreNeverExceedsItsCurrentCapacity) {
    Opts o{};
    o.water_age = true;
    o.rain_inhr = 0.0;     // nothing added, so every change is the melt
    o.air_f     = 40.0;
    o.fwfrac    = 0.90;
    o.ripe      = true;    // AT capacity, which is where the threshold bites
    o.end_min   = 60;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& s   = as_cpp_engine(e).snowSolver().state();
    ASSERT_GE(ctx.subcatches.snowpack[0], 0);

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);
    // SETUP: the pack must be melting, or the capacity never moves and the
    // pre-melt and post-melt forms agree.
    ASSERT_LT(s.wsnow[perv], o.sd0 / 12.0 - 1.0e-9)
        << "SWE did not fall, so the capacity never shrank and this gate "
           "cannot distinguish the two ways of measuring it";

    for (int k = 0; k < openswmm::snow::N_SUBAREAS; ++k) {
        const auto idx = static_cast<std::size_t>(k);
        if (s.wsnow[idx] <= 0.0) continue;
        EXPECT_LE(s.fw[idx], s.fwfrac[idx] * s.wsnow[idx] + 1.0e-12)
            << "snow surface " << k << " holds " << s.fw[idx]
            << " ft of free water against a capacity of "
            << (s.fwfrac[idx] * s.wsnow[idx]) << " ft. The capacity is being "
               "measured against the SWE the pack had BEFORE it melted";
    }
    swmm_engine_destroy(e);
}
