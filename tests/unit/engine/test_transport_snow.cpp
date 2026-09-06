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
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"
#include "hydrology/Snow.hpp"
#include "transport/components/WatershedCommon.hpp"

namespace tr = openswmm::transport;

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr int kNSubAge = static_cast<int>(openswmm::SubArea::COUNT_);

/// `stop_min > 0` writes `v` up to that minute and ZERO after it, which is
/// the only way this deck can put water of one age on the surface and then
/// take the source away. Every gate but 2 leaves it at 0 and gets the flat
/// series it always had.
std::string series(const char* name, int last_min, double v,
                   int stop_min = 0) {
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        const double x = (stop_min > 0 && m >= stop_min) ? 0.0 : v;
        std::snprintf(buf, sizeof(buf), "%s 01/01/2026 %02d:%02d %.4f\n",
                      name, m / 60, m % 60, x);
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
    /// `WaterAgeSource::RAINFALL` age, **in HOURS** — the `_h` is the unit,
    /// not decoration. `[WATER_AGE_SOURCES]` is parsed as hours and stored as
    /// seconds (`WaterAgeComponent.cpp:162`), so **every comparison against a
    /// value read back out of the engine needs `* 3600.0`.** Both gates that
    /// used a nonzero age got this wrong on their first writing, in opposite
    /// directions: one wrote seconds into the field, the other compared the
    /// field against seconds. Default 0.0, where the unit does not show.
    double rain_h    = 0.0;
    /// `WaterAgeSource::INITIAL_STATE` age, **in HOURS**. See `rain_h`.
    double init_h    = 0.0;
    double rain_c    = 0.0;    ///< arriving water is 0 °C
    double init_c    = 25.0;
    int    end_min   = 60;
    double rain_inhr = 0.0;    ///< gage intensity; 0 on every gate but 7
    /// Minute at which the gage goes dry. 0 = never. Only gate 2 uses it,
    /// and it needs it: see that gate for why a single-phase deck cannot
    /// observe the mixing volume in the AGE channel at all.
    int    rain_stop_min = 0;
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
    /// `SD100` — the depth at which areal coverage reaches 100 %, the 7th
    /// field of the IMPERVIOUS and PERVIOUS rows (the same slot that carries
    /// `snn0` on PLOWABLE). **0 means "no areal depletion"**, which both this
    /// engine and legacy treat as permanent full cover
    /// (`Snow.cpp` getArealDepletion / `snow.c:520`), so it is the value
    /// every gate but 16 wants.
    double sd100 = 0.0;
    /// F8 — `Dplow`, the depth at which ploughing begins, in deck units.
    /// **0 (the default) disables ploughing entirely**, which is every gate
    /// but 26 — so adding these fields moves no existing deck.
    double weplow = 0.0;
    /// F8 — the `REMOVAL` row's first fraction: snow ploughed OUT of the
    /// system. This is the only sink `runoff_snowremov` measures.
    double f_out  = 0.0;
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
      << "[TIMESERIES]\n"
      << series("rain_ts", o.end_min + 5, o.rain_inhr, o.rain_stop_min)
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
          << " " << fw0 << " " << o.sd100 << "\n"
          << "SP1 PERVIOUS   0.01 0.03 32.0 " << o.fwfrac << " " << o.sd0
          << " " << fw0 << " " << o.sd100 << "\n"
          << "SP1 REMOVAL    " << o.weplow << " " << o.f_out
          << " 0.0 0.0 0.0 0.0\n\n";
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

/// `age_at_stop` / `t_at_stop`, when given, capture `subarea_age` and the
/// simulation clock at the FIRST step past `rain_stop_min`. A gate that has
/// to show "the age moved on its own" needs the value it moved FROM, and on
/// this deck that value is not computable in advance -- it is whatever the
/// wet phase left behind. Both are optional and every other gate omits them.
SWMM_Engine run(const Opts& o, std::vector<double>* age_at_stop = nullptr,
                double* t_at_stop = nullptr) {
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
    // `swmm_engine_step` reports elapsed time in DAYS
    // (SWMMEngine.cpp:1234, `current_time / SEC_PER_DAY`).
    const double stop_days = o.rain_stop_min / 1440.0;
    bool captured = false;
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
        if (age_at_stop != nullptr && !captured && o.rain_stop_min > 0 &&
            elapsed >= stop_days) {
            *age_at_stop = as_cpp_engine(e).context().water_age_state
                               .subarea_age;
            if (t_at_stop != nullptr) *t_at_stop = elapsed * 86400.0;
            captured = true;
        }
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
// Gate 2 — AGE. Meltwater arrives and MIXES, so the surface age must move
//          off pure accrual. Under the defect the mixing volume is
//          identically zero and the age is `start + elapsed` exactly.
//
// TWO PHASES, AND THE SECOND PHASE IS THE GATE. A single-phase deck cannot
// observe this at all, and the reason is worth stating because two earlier
// writings of this gate both passed while observing nothing:
//
//   the surface age and the pack age are BOTH pure elapsed time on a deck
//   where each starts at zero, so mixing one into the other moves nothing
//   and the correct code and the defect print the same number.
//
// Measured, on the single-phase deck this gate used to run: every subarea
// read 3600.000000 s with the fix in place AND with the S1 defect restored
// (`arrivingPrecipRate` returning the gage). The gate passed both times.
//
// `INITIAL_STATE` is not the lever either -- it seeds the network and the
// LID layers, not `subarea_age` (WaterAgeWatershed.cpp has no seeding at
// all), so a surface configured "old" still starts at zero. That is what
// the previous writing assumed and it is why its ceiling sat 11x above the
// value it was bounding.
//
// So the deck rains OLD water for an hour, then goes dry for an hour while
// the pack melts. The wet phase gives the surface an age of its own; the
// dry phase is the one under test, and the value the age must move off is
// MEASURED at the changeover rather than assumed.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, MeltwaterMixesIntoTheSubareaAge) {
    Opts o{};
    o.water_age = true;
    o.rain_h        = 10.0;   // HOURS — old water, so the wet phase leaves
                              // the surface far above the pack's own age
    o.rain_inhr     = 1.0;
    o.rain_stop_min = 60;     // dry from here on: the gage reads zero and
                              // every drop that still arrives is melt
    o.end_min       = 120;
    // Partial cover, so the wet phase actually reaches the ground instead of
    // being swallowed whole by the pack. `sd100` above `sd0` is what puts the
    // pack below its 100 %-cover depth and lets the ADC curve apply at all
    // (S4/F6 — with sd100 at 0 the pack is permanently at full cover).
    o.adc_cover = 0.5;
    o.sd100     = 12.0;

    std::vector<double> at_stop;
    double t_stop = 0.0;
    SWMM_Engine e = run(o, &at_stop, &t_stop);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));
    ASSERT_EQ(static_cast<int>(at_stop.size()), kNSubAge)
        << "the changeover was never reached, so there is no measured value "
           "to compare against and nothing below this line means anything";

    // The DRY phase's duration, from the clock rather than from `end_min`:
    // the capture lands on the first step past the changeover, which
    // overshoots by up to one routing step.
    const double dry_s = o.end_min * 60.0 - t_stop;
    ASSERT_GT(dry_s, 60.0) << "the dry phase is too short to observe";

    const auto& ws = ctx.water_age_state;
    ASSERT_GE(static_cast<int>(ws.subarea_age.size()), kNSubAge);

    bool any_mixed = false;
    for (int k = 0; k < kNSubAge; ++k) {
        const double age = ws.subarea_age[static_cast<std::size_t>(k)];
        const double unmixed = at_stop[static_cast<std::size_t>(k)] + dry_s;
        EXPECT_TRUE(std::isfinite(age));
        // No reference value, and none is needed. A surface that mixed in
        // NOTHING during the dry phase reads exactly what it read at the
        // changeover plus the time since. Anything that arrived came out of
        // the pack, which is younger than the hour-old rain the surface is
        // carrying, so it can only pull the age DOWN.
        EXPECT_LE(age, unmixed + 1.0e-6)
            << "subarea " << k << " aged FASTER than the clock, which "
               "nothing in a dry phase can produce";
        // Half the dry phase, and the bar is set from measurement rather
        // than from taste. Pure accrual does NOT land exactly on `unmixed`:
        // the age advances one WET step (60 s) at a time while `t_stop` is
        // the routing clock, so the capture instant sits up to one step off
        // and accrual alone reads ~59.5 s below the bound. Measured on this
        // deck: accrual 3540.0 s of movement against a 3599.5 s window --
        // 59.5 s -- and correct mixing 12 638.8 s, which is 3.5x the window.
        // A 1-second band would therefore pass the defect; half the window
        // is two orders of magnitude clear of the quantisation and seven
        // times clear of the signal.
        if (unmixed - age > 0.5 * dry_s)
            any_mixed = true;
    }
    EXPECT_TRUE(any_mixed)
        << "no subarea age moved more than half the dry phase off pure "
           "accrual, which is what a surface reads when NOTHING mixes into "
           "it. Meltwater reached this subcatchment — the mixing volume is "
           "still being read from subcatches.rainfall, which is zero here";
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
    // SD100 as well as the curve. Areal depletion needs BOTH: a curve that
    // is not identically 1, and a positive `si` for `wsnow/si` to index it
    // with. Before S4 `si` was pinned to the initial pack depth, so the ADC
    // row alone was enough; now the deck owns both, as legacy does.
    o.sd100 = 24.0;
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
    // SD100 as well as the curve. Areal depletion needs BOTH: a curve that
    // is not identically 1, and a positive `si` for `wsnow/si` to index it
    // with. Before S4 `si` was pinned to the initial pack depth, so the ADC
    // row alone was enough; now the deck owns both, as legacy does.
    o.sd100 = 24.0;
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

// ---------------------------------------------------------------------------
// Gate 16 — F6. THE DECK'S `SD100` IS READ.
//
//           `si` used to be pinned to the INITIAL pack depth, which makes
//           `wsnow >= si` true on the first step and every step after, so
//           `getArealDepletion` returned 1.0 unconditionally: **every snow
//           deck in this program sat at 100 % cover**, `rain·(1 − asc)` was
//           identically zero, and no rain ever reached the ground under a
//           pack. Legacy reads the field at `snow.c:352`.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, TheDecksSD100SetsTheHundredPercentCoverDepth) {
    Opts o{};
    o.water_age = true;
    o.sd0    = 6.0;
    o.sd100  = 24.0;   // FOUR TIMES the initial depth, so the pack starts
    o.air_f  = 40.0;   // well below full cover and depletion is live
    // ...and a curve to index. The DEFAULT ADC is all ones (`Snow.hpp:92`),
    // which means "no depletion at any index", so SD100 alone changes
    // nothing: `getArealSnowCover` returns 1 whatever `wsnow/si` is. Reading
    // the field is necessary for depletion, not sufficient.
    o.adc_cover = 0.5;
    o.end_min = 30;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& s   = as_cpp_engine(e).snowSolver().state();
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "no pack on this deck";

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);
    const double sd100_ft = o.sd100 / 12.0;
    const double sd0_ft   = o.sd0 / 12.0;

    // The field reached the solver at all.
    EXPECT_NEAR(s.si[perv], sd100_ft, 1.0e-9)
        << "si is " << s.si[perv] << " ft; the deck said SD100 = " << sd100_ft
        << " ft and the initial depth was " << sd0_ft
        << " ft. If si equals the initial depth, the 7th field is still "
           "being ignored";

    // SETUP: and the pack must be BELOW it, or depletion is off by the
    // `wsnow >= si` branch and cover is legitimately 1.
    ASSERT_LT(s.wsnow[perv], s.si[perv])
        << "the pack is at or above SD100, so full cover is correct here and "
           "this gate cannot observe depletion";

    EXPECT_LT(s.asc[perv], 1.0)
        << "areal cover is still exactly 1 on a pack below its SD100. That "
           "is the signature of si being pinned to the pack depth: "
           "wsnow >= si is then always true";
    EXPECT_GT(s.asc[perv], 0.0);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 17 — D1 RETRACTED. The seasonal melt factor peaks on the SOLSTICE.
//
//           The engine used `2π/365`, recorded as a correction to legacy's
//           `0.0172615`. It is not one. Legacy's constant has a period of
//           exactly 364 days, and with the day-81 offset that puts the peak
//           on day 172 — the summer solstice — with an equinox-to-solstice
//           quarter of a whole 91 days. `2π/365` peaks at day 172.25.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, TheSeasonalMeltFactorPeaksOnTheSolstice) {
    openswmm::snow::SnowSolver solver;
    solver.init(1);

    // Day 172 = June 21: 31+28+31+30+31+21. The sine must be AT its maximum
    // there, not merely near it — that is the whole content of the constant.
    solver.setMeltCoeffs(172);
    EXPECT_NEAR(solver.state().season, 1.0, 1.0e-6)
        << "season at the summer solstice is " << solver.state().season
        << ", not 1. With 2*pi/365 the peak sits at day 172.25 and this "
           "reads 0.99999 — close, and off the solstice";

    // Day 81 = the vernal equinox, where the cycle crosses zero.
    solver.setMeltCoeffs(81);
    EXPECT_NEAR(solver.state().season, 0.0, 1.0e-6);

    // Day 172 + 182 = 354, the winter solstice half a period later.
    solver.setMeltCoeffs(354);
    EXPECT_NEAR(solver.state().season, -1.0, 1.0e-3)
        << "season at the winter solstice is " << solver.state().season;

    // And the quarter period is a WHOLE number of days — the property that
    // makes 364 the right choice rather than an arithmetic slip.
    solver.setMeltCoeffs(172 - 91);
    EXPECT_NEAR(solver.state().season, 0.0, 1.0e-6)
        << "day 81 is not exactly a quarter period before the peak, so the "
           "equinox-to-solstice interval is not a whole number of days";
}

// ---------------------------------------------------------------------------
// Gate 18 (S2b) — MELTWATER CARRIES THE PACK'S RESIDENCE TIME.
//
//    Deck: a melting pack, no precipitation, RAINFALL age 0. Every drop that
//    arrives came out of the pack, so the arriving age IS the pack's age.
//
//    Under the defect — no pack age at all — arriving water takes the
//    configured RAINFALL age, and this deck sets that to exactly 0. The
//    assertion is therefore `> 0` with no reference value: the pack has been
//    holding water for the whole run, so anything it delivers is older than
//    the instant it arrived, and only a model with no pack age says otherwise.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, MeltwaterCarriesThePacksResidenceTime) {
    Opts o{};
    o.water_age = true;
    o.rain_h = 0.0;   // the DEFECT's answer, so it is the value to beat
    o.init_h = 0.0;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_TRUE(MeltingWithNoRain(ctx));

    // SETUP: the pack published a melt-only rate on at least one surface.
    // Without it there is no meltwater and nothing below means anything.
    const double m_imp = ctx.subcatches.snow_melt_imperv[0];
    const double m_prv = ctx.subcatches.snow_melt_perv[0];
    ASSERT_TRUE(m_imp > 0.0 || m_prv > 0.0)
        << "no melt was published on either surface (" << m_imp << " / "
        << m_prv << "), so this deck never reached the state the gate is "
           "about. Fix the deck; do NOT relax this";

    const double elapsed_s = o.end_min * 60.0;
    bool any_aged = false;
    for (int k = 0; k < kNSubAge; ++k) {
        const double a = tr::arrivingPrecipAge(ctx, 0, k);
        EXPECT_TRUE(std::isfinite(a));
        // A pack cannot deliver water older than the run that made it.
        EXPECT_LE(a, elapsed_s + 1.0e-6)
            << "arriving age on subarea " << k << " is " << a
            << " s, older than the " << elapsed_s << " s run";
        if (a > 1.0e-9) any_aged = true;
    }
    EXPECT_TRUE(any_aged)
        << "arriving water on every subarea is age 0 — the configured "
           "RAINFALL value — so the pack's residence time is not reaching "
           "the blend. Either snow_melt_age_* is still at its -1.0 sentinel "
           "or the pack age is never advanced";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 19 (S2b) — SNOWFALL LOWERS THE PACK'S AGE, STRICTLY BETWEEN THE TWO.
//
//    A solver-level gate, deliberately. The deck writer cannot put snowfall
//    and melt on the same pack at the same time — snowfall needs air below
//    the dividing temperature and melt needs it above — and driving the
//    solver directly is the honest way to reach a state a deck cannot.
//
//    STRICT on both sides (lesson 111): a non-strict bracket is satisfied by
//    its own endpoints, and BOTH endpoints are defects here. Equal to the old
//    pack age means the snowfall did not mix; equal to the snowfall age means
//    it replaced the pack instead of mixing into it.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, SnowfallLowersThePackAgeStrictlyBetweenTheTwo) {
    openswmm::SimulationContext ctx;
    openswmm::snow::SnowSolver solver;
    solver.init(1);
    auto& st = solver.state();
    st.track_age  = true;
    st.precip_age = 0.0;              // fresh snow

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);
    st.fArea[perv] = 1.0;             // one surface carries the whole area
    st.wsnow[perv] = 1.0;             // 1 ft of old snow
    st.fw[perv]    = 0.0;
    st.age[perv]   = 10000.0;         // it has been there a while

    const double dt = 60.0;
    const double snowfall = 0.5 / dt; // adds 0.5 ft this step
    solver.plowSnow(ctx, dt, snowfall);

    const double a = st.age[perv];
    EXPECT_TRUE(std::isfinite(a));
    EXPECT_GT(a, st.precip_age)
        << "the pack age fell to the snowfall's age (" << a << "), so the "
           "new snow REPLACED the pack rather than mixing into it";
    EXPECT_LT(a, 10000.0 + dt)
        << "the pack age is " << a << ", unchanged by half a foot of fresh "
           "snow on one foot of old — the snowfall is being added to wsnow "
           "without an age mix";
}

// ---------------------------------------------------------------------------
// Gate 20 (S2b) — ON A PARTIALLY COVERED PACK, ARRIVING AGE LIES STRICTLY
//                 BETWEEN THE RAIN AGE AND THE PACK AGE.
//
//    This is `arrivingPrecipAge`'s blend, and it is the age analogue of gate
//    9. Needs SD100 AND a graded ADC row — after S4, SD100 alone still gives
//    full cover because the default curve is all ones (Snow.hpp:92).
//
//    The rain is made OLD (10 h) and the pack can be at most the run length
//    (1 h), so the two sources are an order of magnitude apart and the
//    bracket is not marginal.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, ArrivingAgeBlendsStrictlyBetweenItsTwoSources) {
    Opts o{};
    o.water_age = true;
    o.rain_h    = 10.0;            // OLD rain, in HOURS — see gate 2
    o.init_h    = 0.0;
    o.rain_inhr = 0.2;             // rain AND melt at once
    o.adc_cover = 0.5;             // partial cover: both waters arrive
    o.sd100     = 24.0;            // 4x sd0, so `wsnow < si` is not marginal
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();

    // SETUP, asserted term by term (lesson 96) — the gate is meaningless
    // unless BOTH waters actually arrive.
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "S1 has no snowpack";
    const double f = tr::arrivingMeltFraction(ctx, 0, tr::kSubIMPERV0);
    ASSERT_GT(f, 0.0)
        << "the melt fraction is 0, so no meltwater arrived and there is "
           "nothing to blend against the rain";
    ASSERT_LT(f, 1.0)
        << "the melt fraction is 1, so NO rain reached the ground — cover is "
           "still 1. SD100 and the ADC curve are both needed after S4, and "
           "this deck sets both; if this fires, one of them is not reaching "
           "the solver";

    // `rain_h` is the DECK value and the deck is parsed in hours; every age
    // the engine publishes is in SECONDS. Comparing the two directly is a
    // 3600x error that reads as "the pack is older than the rain".
    const double rain_age_s = o.rain_h * 3600.0;
    const double a_pack = ctx.subcatches.snow_melt_age_imperv[0];
    ASSERT_GE(a_pack, 0.0) << "snow_melt_age_imperv is still at its -1.0 "
                              "sentinel while melt is arriving";
    ASSERT_LT(a_pack, rain_age_s)
        << "the pack is not younger than the rain on this deck, so the "
           "bracket below cannot discriminate";

    const double a = tr::arrivingPrecipAge(ctx, 0, tr::kSubIMPERV0);
    // STRICT both ways. Reading either source raw lands on an endpoint, and
    // that is exactly lesson 111's failure — gate 9 passed on its own defect
    // for this reason before it was tightened.
    EXPECT_GT(a, a_pack)
        << "arriving age " << a << " sits on the PACK age, so the rain that "
           "reached the ground through the bare fraction is not in the blend";
    EXPECT_LT(a, rain_age_s)
        << "arriving age " << a << " sits on the RAIN age, so the meltwater "
           "is not in the blend";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 21 (S2b) — A PLOW TRANSFER CARRIES THE DONOR'S AGE, AND CONSERVES
//                 AGE-VOLUME ACROSS THE SUBCATCHMENT BOUNDARY.
//
//    THE gate for the published transfer. `plowSnow` moves water between
//    surfaces and to another subcatchment INSIDE the solver; an age update
//    running afterwards sees only that subcatchment 1 gained snow and cannot
//    know it came from subcatchment 0, nor at what age. So it would arrive
//    at the receiver's own age, or at 0 — both of which this gate rejects.
//
//    The receiver starts with YOUNG water and the donor is OLD, so a transfer
//    that carries age must move the receiver UP. Age-volume conservation is
//    asserted alongside, because "it moved up" is also satisfied by moving up
//    the wrong amount.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, PlowedSnowCarriesTheDonorsAgeAcrossSubcatchments) {
    openswmm::SimulationContext ctx;
    openswmm::snow::SnowSolver solver;
    solver.init(2);
    auto& st = solver.state();
    st.track_age  = true;
    st.precip_age = 0.0;

    constexpr int NS = openswmm::snow::N_SUBAREAS;
    const auto d_plow = static_cast<std::size_t>(openswmm::snow::SNOW_PLOWABLE);
    const auto r_perv = static_cast<std::size_t>(1 * NS + openswmm::snow::SNOW_PERV);

    // Donor: subcatchment 0, plowable surface, deep and OLD.
    st.fArea[d_plow] = 1.0;
    st.wsnow[d_plow] = 2.0;
    st.age[d_plow]   = 20000.0;
    // Receiver: subcatchment 1, pervious surface, shallow and YOUNG.
    st.fArea[r_perv] = 1.0;
    st.wsnow[r_perv] = 1.0;
    st.age[r_perv]   = 0.0;

    st.weplow[0] = 1.0;                         // 2.0 ft is over the trigger
    st.sfrac[0 * 5 + 4] = 0.5;                  // half of it crosses over
    st.to_subcatch[0] = 1;

    const double donor_age0 = st.age[d_plow];
    const double dt = 60.0;
    // The baseline has to include the step's AGEING, because plowSnow ages
    // every surface that holds water before it moves any of it — it is the
    // step's first snow call. Comparing against the pre-ageing total would
    // fail by exactly `dt * total_volume` on correct behaviour, which is the
    // kind of baseline error that gets "fixed" by widening a band.
    const double av_before  = st.wsnow[d_plow] * (st.age[d_plow] + dt) +
                              st.wsnow[r_perv] * (st.age[r_perv] + dt);

    solver.plowSnow(ctx, dt, 0.0);              // NO snowfall: plowing only

    // SETUP: snow actually moved. Without this the assertions below are
    // satisfied by a plow that never fired.
    ASSERT_GT(st.wsnow[r_perv], 1.0)
        << "the receiving surface did not gain snow, so no transfer happened "
           "— weplow, sfrac[4] or to_subcatch did not take effect";

    EXPECT_GT(st.age[r_perv], 0.0)
        << "the receiver's age is still 0 after taking on snow that had been "
           "sitting for " << donor_age0 << " s. The transfer is arriving "
           "AGELESS — which is what an age update running after plowSnow can "
           "only produce, because it cannot see where the water came from";
    EXPECT_LT(st.age[r_perv], donor_age0)
        << "the receiver's age jumped to the donor's, so the arriving water "
           "REPLACED the receiver's own rather than mixing into it";

    // Age-volume is conserved: nothing left the system on this configuration
    // (sfrac[0], the plow-out fraction, is 0), and ageing has not run because
    // both surfaces are handled in the same call.
    const double av_after = st.wsnow[d_plow] * st.age[d_plow] +
                            st.wsnow[r_perv] * st.age[r_perv];
    EXPECT_NEAR(av_after, av_before, 1.0e-6 * std::fabs(av_before) + 1.0e-9)
        << "age-volume before " << av_before << ", after " << av_after
        << ". The transfer moved water at the wrong age — the direction was "
           "right and the amount was not, which 'it went up' alone cannot "
           "catch";
}

// ---------------------------------------------------------------------------
// Gate 22 (S2b) — A PACK THAT MELTS OUT AND RE-FORMS DOES NOT CARRY ITS OLD
//                 AGE INTO THE NEW SNOW.
//
//    The dry-element shape: state that survives the water it described. H1
//    left exactly this open for temperature — a dry element reports a carried
//    value indefinitely — and 0 s is a REAL age, so the same defect here is
//    unreadable from the output. It has to be gated at the source.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, APackThatMeltsOutDoesNotCarryItsAgeIntoNewSnow) {
    openswmm::SimulationContext ctx;
    openswmm::snow::SnowSolver solver;
    solver.init(1);
    auto& st = solver.state();
    st.track_age  = true;
    st.precip_age = 0.0;

    const auto perv = static_cast<std::size_t>(openswmm::snow::SNOW_PERV);
    st.fArea[perv]  = 1.0;
    st.tbase[perv]  = 32.0;
    st.fwfrac[perv] = 0.1;
    // Below the 0.001-inch instant-melt threshold, and OLD.
    st.wsnow[perv]  = 0.5 * (0.001 / 12.0);
    st.fw[perv]     = 0.0;
    st.age[perv]    = 50000.0;

    const double dt = 60.0;
    const double old_age = st.age[perv];
    solver.execute(ctx, dt, 40.0, 5.0, 0.0, 0.0);

    // SETUP: the pack really did melt out.
    ASSERT_LE(st.wsnow[perv], 0.0)
        << "the pack did not melt out, so this gate never reached its state";
    // The water that LEFT still carries what the pack had — that is the
    // whole point of `out_age` being separate from `age`.
    EXPECT_NEAR(st.out_age[perv], old_age, 1.0e-9)
        << "the departing meltwater reads age " << st.out_age[perv]
        << " instead of the pack's " << old_age
        << ". Reading `age` after the pack is emptied gives 0, which is the "
           "age of water that fell this instant — this water did not";

    // And the empty pack carries nothing forward.
    EXPECT_NEAR(st.age[perv], 0.0, 1.0e-9)
        << "an emptied pack still reads age " << st.age[perv]
        << ", which will become the mixing partner for the next snowfall and "
           "make fresh snow arrive old";

    // Now let it re-form from fresh snow and confirm it starts fresh.
    solver.plowSnow(ctx, dt, 1.0 / dt);
    EXPECT_NEAR(st.age[perv], 0.0, 1.0e-9)
        << "a pack re-formed entirely from age-0 snowfall reads "
        << st.age[perv] << " s";
}

// ---------------------------------------------------------------------------
// Gate 24 (F8) — THE LEDGER ACCOUNTS FOR A PACK THE DECK STARTS WITH.
//
//    A pack sitting below freezing with no precipitation: nothing arrives,
//    nothing leaves, nothing melts. The runoff balance must close.
//
//    Under the defect it cannot. `runoff_init_snow` did not exist, so the
//    pack the deck was GIVEN was never on the input side, and the same pack
//    still standing at the end was never on the output side. The gate asserts
//    the CLOSURE, not the row — a gate on the row would pass on a row that is
//    populated and not used, which is half of what F8 was.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, TheRunoffLedgerClosesOnAPackThatJustSitsThere) {
    Opts o{};
    o.air_f = 10.0;        // well below tbase: no melt at all
    // Rain, and it is not decoration. `runoff_error()` divides by `total_in`
    // and returns 0.0 when that is zero -- so on a deck whose ONLY input is
    // the pack, dropping `runoff_init_snow` from the input side does not
    // produce a large error, it produces a vacuous zero and this gate passes.
    // Measured: falsifier i escaped both ledger gates for exactly that
    // reason. A second, snow-independent input is what makes the division
    // real. At 10 degF the pack cannot melt and 0.2 in/hr for an hour is far
    // inside its free-water capacity, so nothing leaves and the gate's
    // premise is intact -- the pack now also absorbs, which the final-cover
    // term has to carry.
    o.rain_inhr = 0.2;
    o.ripe = false;
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& mb = ctx.mass_balance;

    // SETUP, asserted term by term (lesson 96). If the deck has no pack, or
    // the pack melted after all, the closure below is trivially satisfied by
    // a balance with no snow in it — which is exactly the state the defect
    // produced and this gate must not accept.
    ASSERT_GE(ctx.n_subcatches(), 1);
    ASSERT_GE(ctx.subcatches.snowpack[0], 0) << "S1 has no snowpack";
    ASSERT_GT(mb.runoff_init_snow, 0.0)
        << "the deck starts with no snow on the books at all, so this gate "
           "cannot see the term it is about. Fix the deck; do NOT relax this";
    ASSERT_GT(mb.runoff_final_snow, 0.0)
        << "the pack did not survive a run held below freezing — check air_f "
           "against tbase before touching anything else";
    ASSERT_GT(mb.runoff_rainfall, 0.0)
        << "no rain reached the books, so `total_in` is the pack alone and "
           "runoff_error() returns its total_in == 0 fallback rather than a "
           "measured balance — the gate would pass on a ledger with the "
           "initial pack missing entirely";
    ASSERT_DOUBLE_EQ(mb.runoff_runoff, 0.0)
        << "water left the subcatchment on a deck held below freezing";

    // The closure. No reference value: a balance either closes or it does not.
    EXPECT_LT(std::fabs(mb.runoff_error()), 1.0e-3)
        << "runoff continuity is " << mb.runoff_error() * 100.0
        << " % on a deck where nothing arrived, nothing left and nothing "
           "melted. The only water in the model is the pack, so an error "
           "here IS the missing snow term";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 25 (F8) — AND IT STILL CLOSES WHEN THE PACK MELTS AND THE WATER LEAVES.
//
//    Gate 24's deck never moves any water, so it cannot tell a ledger that
//    counts snow correctly from one that counts it twice on both sides. This
//    one melts: initial snow becomes runoff and infiltration, final snow is
//    smaller than initial, and the balance must still close.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, TheRunoffLedgerClosesWhileThePackIsMeltingAway) {
    Opts o{};
    o.air_f = 50.0;        // well above tbase
    o.rain_inhr = 0.2;     // a snow-independent input — see gate 24 for why
    o.ripe = true;         // melt leaves from the first step
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& mb = ctx.mass_balance;

    ASSERT_GT(mb.runoff_init_snow, 0.0) << "no pack on the books";
    // SETUP: the pack really did shed water. Without this, gate 25 is gate 24.
    ASSERT_LT(mb.runoff_final_snow, mb.runoff_init_snow)
        << "the pack did not shrink, so no snow-to-water conversion happened "
           "and this gate is testing nothing gate 24 did not";
    ASSERT_GT(mb.runoff_runoff + mb.runoff_infil, 0.0)
        << "the pack shrank but no water reached the ground";
    ASSERT_GT(mb.runoff_rainfall, 0.0)
        << "no snow-independent input on the books — see gate 24";

    EXPECT_LT(std::fabs(mb.runoff_error()), 1.0e-3)
        << "runoff continuity is " << mb.runoff_error() * 100.0
        << " % — initial snow " << mb.runoff_init_snow << " ft3, final "
        << mb.runoff_final_snow << " ft3, runoff " << mb.runoff_runoff
        << ", infil " << mb.runoff_infil;
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 26 (F8) — SNOW PLOUGHED OUT OF THE SYSTEM IS BOOKED AS REMOVED.
//
//    `runoff_snowremov` HAD NO WRITER ANYWHERE. It was declared, exposed
//    through SWMM_RUNOFF_SNOWREMOV, returned by the mass-balance API and read
//    by callers, while `SnowSoA::removed` accumulated the real figure with no
//    consumer — the same shape as F1 and as the snapshot quality vectors.
//
//    The gate asserts the OBSERVABLE (the ledger figure) against the solver's
//    own accumulator. Asserting only that the balance closes would not catch
//    it: with `snowremov` at 0 the ploughed water is simply missing from both
//    sides of a term that was itself missing.
// ---------------------------------------------------------------------------
TEST(TransportSnowTest, PloughedSnowIsBookedAsRemovedInTheLedger) {
    Opts o{};
    o.air_f = 10.0;        // no melt: the ONLY sink is the plough
    o.rain_inhr = 0.0;
    o.snn0 = 0.5;          // half the impervious area is plowable
    o.weplow = 0.05;       // well below sd0, so the plough fires on step 1
    o.f_out = 0.40;        // and 40 % of it leaves the system
    SWMM_Engine e = run(o);
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& mb = ctx.mass_balance;
    const auto& soa = as_cpp_engine(e).snowSolver().state();

    // SETUP: the plough actually fired. Everything below is vacuous otherwise.
    ASSERT_GT(soa.removed, 0.0)
        << "the solver's own accumulator is 0, so no snow was ploughed out — "
           "check snn0, weplow and f_out in that order";

    EXPECT_NEAR(mb.runoff_snowremov, soa.removed,
                1.0e-9 * std::fabs(soa.removed) + 1.0e-12)
        << "the ledger books " << mb.runoff_snowremov
        << " ft3 of snow removal while the solver accumulated " << soa.removed
        << ". The field is exposed through SWMM_RUNOFF_SNOWREMOV and read by "
           "callers — if it is 0 here it has no writer, which is what F8 was";

    EXPECT_LT(std::fabs(mb.runoff_error()), 1.0e-3)
        << "runoff continuity is " << mb.runoff_error() * 100.0
        << " % on a deck whose only sink is the plough";
    swmm_engine_destroy(e);
}
