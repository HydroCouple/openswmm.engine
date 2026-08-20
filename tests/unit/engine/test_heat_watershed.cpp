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
 * @file test_heat_watershed.cpp
 * @brief Phase H5a gates — temperature on subcatchment surfaces.
 *
 * @details Every gate asserts its SETUP before its result (lesson 59): the
 *          code under test runs only on a deck that actually produces
 *          runoff, and the run-on path only where a subcatchment drains into
 *          another. A gate that cannot reach its branch passes vacuously,
 *          which is worse than failing.
 *
 * @par The one thing to read before changing a band here
 *      **Shortwave is a static config constant** (`RadiativeConfig::
 *      shortwave_wm2`, written once at `HeatComponent.cpp:136`) — there is
 *      no diurnal solar path in the engine. So no gate in this file may
 *      assume a day/night cycle, and "equilibration" here means approach to
 *      the steady temperature implied by CONSTANT forcing, not a daily
 *      swing. H3's round produced a gate whose premise was never
 *      established (lesson 57); this note exists so the same does not happen
 *      by assuming physics the deck cannot express.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H5a, §6.1 D-H5a/D-H5c
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

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr int kNSub = openswmm::HeatState::kNSubArea;

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// An INTENSITY gage on a 5-minute interval reads ONE value per interval, so
/// a series with entries only at 00:00 and 01:00 rains for five minutes, not
/// an hour. A3's round learned this the expensive way — its gates all ran on
/// a draining surface without meaning to. One entry per interval.
///
/// @param stop_min Last minute that rains; entries after it are 0.0. Without
///        it every "long dry tail" deck rained for its whole run, because
///        the series was always generated out to `end_min + 5` — which is
///        what left the dry-policy gate measuring a soaking-wet surface.
std::string rain_series(int last_min, double intensity, int stop_min = -1) {
    if (stop_min < 0) stop_min = last_min;
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        std::snprintf(buf, sizeof(buf), "rain_ts 01/01/2026 %02d:%02d %.3f\n",
                      m / 60, m % 60, m <= stop_min ? intensity : 0.0);
        s += buf;
    }
    return s + "\n";
}

/// A flat zero series, for a subcatchment that must receive run-on and
/// nothing else.
std::string zero_series(int last_min) {
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        std::snprintf(buf, sizeof(buf), "zero_ts 01/01/2026 %02d:%02d 0.000\n",
                      m / 60, m % 60);
        s += buf;
    }
    return s + "\n";
}

/// A flat air-temperature series in °F. Flat on purpose: the fluxes are
/// nonlinear in air temperature, so a varying series would make every
/// expected value a numerical integration rather than a fixed point.
std::string air_series(int last_min, double temp_f) {
    std::string s;
    char buf[80];
    for (int m = 0; m <= last_min; m += 5) {
        std::snprintf(buf, sizeof(buf), "air_ts 01/01/2026 %02d:%02d %.3f\n",
                      m / 60, m % 60, temp_f);
        s += buf;
    }
    return s + "\n";
}

struct DeckOpts {
    bool   cascade      = false;  ///< S1 drains to S2 rather than to J1
    bool   surface      = false;  ///< [HEAT_FLUXES] SURFACE_EXCHANGE ON
    bool   radiative    = false;
    double air_f        = 68.0;   ///< [TEMPERATURE] constant, °F
    double wind_mph     = 5.0;
    double humidity_pct = 50.0;
    int    end_min      = 60;
    double rain_in_hr   = 2.0;
    int    rain_stop_min = -1;    ///< -1 = rains for the whole run
    /// Put the cascade receiver on a zero-rain gage. Its surface then wets
    /// only as fast as run-on arrives, which is how it holds the films thin
    /// enough to expose the explicit step's stability limit (gate 9).
    bool   starve_receiver = false;
    const char* dry_policy = nullptr;  ///< HOLD | AIR | DEFAULT
};

/// @param cfg_path The heat component config this deck must point at. A
///        parameter and not a constant: every gate writes its own
///        config, and a shared literal here silently ran all of them
///        against gate 1's file — fluxes off, RAINFALL 8 C, no policy
///        key — which is what five of the seven gates were measuring.
void write_deck(const char* path, const char* cfg_path,
                const DeckOpts& o) {
    std::ofstream f(path);
    f << "[TITLE]\nH5a watershed heat gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
      << "INFILTRATION HORTON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << (o.end_min / 60) << ":"
      << (o.end_min % 60 < 10 ? "0" : "") << (o.end_min % 60) << ":00\n"
      << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      // Constant air temperature, wind and humidity: the forcing must be
      // flat or "equilibration" has no fixed point to approach. The air
      // temperature is a TIMESERIES because that is the only air-temperature
      // source the engine reads; a MONTHLY row exists for wind and humidity
      // but not for temperature, and omitting the series entirely leaves
      // ClimateState::temperature at its init value — which would make every
      // gate below read a forcing the deck never set.
      << "[TEMPERATURE]\n"
      << "TIMESERIES air_ts\n"
      << "WINDSPEED MONTHLY " << o.wind_mph << " " << o.wind_mph << " "
      << o.wind_mph << " " << o.wind_mph << " " << o.wind_mph << " "
      << o.wind_mph << " " << o.wind_mph << " " << o.wind_mph << " "
      << o.wind_mph << " " << o.wind_mph << " " << o.wind_mph << " "
      << o.wind_mph << "\n";
    f << "HUMIDITY";
    for (int m = 0; m < 12; ++m) f << " " << o.humidity_pct;
    f << "\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n";
    if (o.starve_receiver)
        f << "RG0 INTENSITY 0:05 1.0 TIMESERIES zero_ts\n";
    f << "\n[TIMESERIES]\n"
      << rain_series(o.end_min + 5, o.rain_in_hr, o.rain_stop_min);
    if (o.starve_receiver) f << zero_series(o.end_min + 5);
    f << air_series(o.end_min + 5, o.air_f)
      << "[SUBCATCHMENTS]\n"
      << "S1 RG1 " << (o.cascade ? "S2" : "J1")
      << " 5 50 500 0.5 0\nS2 " << (o.starve_receiver ? "RG0" : "RG1")
      << " J1 5 50 500 0.5 0\n\n"
      << "[SUBAREAS]\n"
      << "S1 0.01 0.1 0.05 0.10 25 OUTLET\n"
      << "S2 0.01 0.1 0.05 0.10 25 OUTLET\n\n"
      << "[INFILTRATION]\n"
      << "S1 3.0 0.5 4 7 0\nS2 3.0 0.5 4 7 0\n\n"
      << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
      << "[PROCESS_COMPONENTS]\n"
      << "org.hydrocouple.openswmm.heat config=\"" << cfg_path
      << "\"\n\n"
      << "[REPORT]\nINPUT NO\n";
    (void)path;
}

/// The heat component config. `rain_c` is the RAINFALL source temperature —
/// deliberately distinct from the INITIAL_STATE value so "carried" and
/// "invented locally" cannot produce the same number (lesson 26).
void write_heat_cfg(const char* path, double rain_c, double initial_c,
                    const DeckOpts& o) {
    std::string s = "[HEAT_SOURCES]\n";
    s += "RAINFALL GLOBAL " + std::to_string(rain_c) + "\n";
    s += "INITIAL_STATE GLOBAL " + std::to_string(initial_c) + "\n\n";
    s += "[HEAT_FLUXES]\n";
    s += std::string("SURFACE_EXCHANGE ") + (o.surface ? "ON" : "OFF") + "\n";
    s += std::string("RADIATIVE_EXCHANGE ") +
         (o.radiative ? "ON" : "OFF") + "\n";
    if (o.dry_policy != nullptr)
        s += std::string("DRY_ELEMENT_TEMPERATURE ") + o.dry_policy + "\n";
    write_file(path, s);
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

/// SETUP assertion shared by every gate: the deck must have produced runoff,
/// or every temperature below is an untouched seed value.
::testing::AssertionResult ProducedRunoff(const openswmm::SimulationContext& c) {
    for (int i = 0; i < c.n_subcatches(); ++i)
        if (c.subcatches.stat_runoff_vol[static_cast<std::size_t>(i)] > 0.0)
            return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
           << "no subcatchment produced runoff — the deck is not exercising "
              "the code under test";
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — the state exists, is per-SUBAREA, and is seeded from INITIAL_STATE
//          rather than from zero or from kDefaultTemp.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, SubareaTemperatureStateIsSizedAndSeeded) {
    DeckOpts o{};
    write_heat_cfg("_h5a.heat", 8.0, 14.0, o);
    write_deck("_h5a.inp", "_h5a.heat", o);
    SWMM_Engine e = run_and_hold("_h5a.inp", "_h5a.rpt", "_h5a.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    const int nsc = ctx.n_subcatches();

    ASSERT_EQ(nsc, 2) << "deck did not build two subcatchments";
    ASSERT_EQ(static_cast<int>(hs.subarea_temp.size()), nsc * kNSub)
        << "subarea_temp is sized " << hs.subarea_temp.size() << ", expected "
        << (nsc * kNSub) << " — per-subarea state was not allocated";
    ASSERT_EQ(static_cast<int>(hs.subcatch_runoff_temp.size()), nsc);
    ASSERT_EQ(static_cast<int>(hs.subcatch_runon_temp_rate.size()), nsc)
        << "the run-on RATE companion is missing — without it the consumer "
           "divides by the total run-on and reproduces A3's defect";
    ASSERT_TRUE(ProducedRunoff(ctx));

    // Nothing may sit at exactly 0 °C: that is A4's age sentinel, and it is
    // the value this phase exists to stop being written as a temperature.
    for (double t : hs.subarea_temp)
        EXPECT_NE(t, 0.0)
            << "a subarea reports exactly 0 C — the age module's "
               "'no water, no value' sentinel leaked into the heat track";
    swmm_engine_destroy(e);

    // The SEED, observed rather than assumed. On the deck above every
    // subarea is wet, so the seed has been mixed away by the end of the run
    // and swapping INITIAL_STATE for kDefaultTemp changes nothing any
    // assertion here can see. A deck that never rains has no mixing at all:
    // under HOLD every subarea reports the value it was seeded with, and
    // that value must be INITIAL_STATE — the source meaning "water already
    // in the model at t = 0", which is what QualityRouting seeds the node
    // and link temperatures from — and NOT kDefaultTemp.
    constexpr double kInitC = 14.0;
    DeckOpts d{};
    d.rain_in_hr    = 0.0;
    d.rain_stop_min = 0;
    d.dry_policy    = "HOLD";
    write_heat_cfg("_h5h.heat", 8.0, kInitC, d);
    write_deck("_h5h.inp", "_h5h.heat", d);
    SWMM_Engine e2 = run_and_hold("_h5h.inp", "_h5h.rpt", "_h5h.out");
    ASSERT_NE(e2, nullptr);
    const auto& dry = as_cpp_engine(e2).context().heat_state;
    ASSERT_FALSE(dry.subarea_temp.empty());
    for (std::size_t i = 0; i < dry.subarea_temp.size(); ++i)
        EXPECT_NEAR(dry.subarea_temp[i], kInitC, 1.0e-12)
            << "subarea " << i << " of a deck that never rained reports "
            << dry.subarea_temp[i] << " C. Nothing mixed, so this IS the "
               "seed: it must be the INITIAL_STATE source ("
            << kInitC << " C), not HeatConfigData::kDefaultTemp ("
            << openswmm::HeatConfigData::kDefaultTemp << " C).";
    swmm_engine_destroy(e2);
}

// ---------------------------------------------------------------------------
// Gate 2 — runoff leaves at the SUBCATCHMENT's temperature, not the
//          configured RAINFALL temperature. With fluxes OFF and no run-on,
//          those two coincide, so this gate turns the fluxes ON to separate
//          them (lesson 56: a falsifier is unobservable when the deck makes
//          both branches the same number).
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, RunoffLeavesAtTheSurfaceTemperatureNotTheRainTemperature) {
    constexpr double kRainC = 5.0;
    DeckOpts o{};
    o.surface = true;
    o.air_f   = 86.0;   // ~30 C air over 5 C rain: a large, one-signed forcing
    write_heat_cfg("_h5b.heat", kRainC, 5.0, o);
    write_deck("_h5b.inp", "_h5b.heat", o);
    SWMM_Engine e = run_and_hold("_h5b.inp", "_h5b.rpt", "_h5b.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    ASSERT_TRUE(ProducedRunoff(ctx));
    ASSERT_TRUE(ctx.heat_config.surface_exchange)
        << "SURFACE_EXCHANGE did not parse — with fluxes off this gate "
           "compares a number against itself";

    bool any_moved = false;
    for (double t : hs.subcatch_runoff_temp)
        if (std::fabs(t - kRainC) > 1.0e-6) any_moved = true;
    EXPECT_TRUE(any_moved)
        << "every subcatchment's runoff temperature equals the configured "
           "RAINFALL temperature exactly — the surface energy balance is "
           "not reaching the ponded subareas";

    // And the loader seam, which the assertion above cannot see: it reads
    // heat_state.subcatch_runoff_temp, which QualityRouting does not write.
    // Reverting the wet-weather loader to addTempVolume(... RAINFALL) leaves
    // every subarea temperature untouched and fails nothing — so the node
    // is where that half of the changeset has to be observed. J1 is fed by
    // runoff alone, so its temperature IS what the loader delivered.
    ASSERT_GE(ctx.n_nodes(), 1);
    EXPECT_GT(ctx.heat_state.node_temp[0], kRainC + 1.0e-3)
        << "J1 sits at " << ctx.heat_state.node_temp[0] << " C. Its only "
           "inflow is runoff from surfaces the balance warmed to "
        << hs.subcatch_runoff_temp[0] << " C, so the rain temperature at the "
           "node means the wet-weather loader is still delivering the "
           "configured RAINFALL source instead of the computed one.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — run-on carries the DONOR's temperature. The gate that fails
//          before H5a.
//
// @par Why this deck has no flux modules and a returning outfall
//      The obvious deck — two subcatchments, warm air, the receiver ending
//      warmer than the donor — cannot see the defect, and two different
//      versions of it were measured before this one:
//
//      * Both on the same gage: the receiver's own rain (10.1 cfs)
//        outweighs the run-on (7.7 cfs) and the extra throughput pulls it
//        BELOW the donor, 9.62438 against 9.62463. The sign is set by
//        throughput, not by what arrived.
//      * Receiver starved of rain: the sign comes right (13.98 against
//        9.62), but dropping the run-on scatter still leaves 10.78 — also
//        above the donor, because a surface fed slowly sits nearer the
//        atmospheric equilibrium whatever its inflow temperature. The
//        margin narrows; the assertion does not fire.
//
//      The confound in both is the surface energy balance: it warms the
//      receiver whether or not the cascade carried anything. So the fluxes
//      go OFF and the contrast comes from transport alone — a 40 C
//      dry-weather flow at J1, discharged through OUT and routed back onto
//      S1 as run-on. S1 rises above the 12 C rain; S2 is on a zero gage and
//      drinks nothing but S1's runoff.
//
//      With no flux module enabled, water above 12 C on S2 can only have
//      come across the cascade. Drop the scatter and S2 falls back to the
//      rain temperature EXACTLY, so the assertion is a strict inequality
//      against the value the failure mode produces, not a band.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, RunonCarriesTheDonorsTemperature) {
    constexpr double kRainC = 12.0;
    constexpr double kDwfC  = 40.0;
    write_file("_h5c.heat",
               "[HEAT_SOURCES]\nRAINFALL GLOBAL 12.0\n"
               "INITIAL_STATE GLOBAL 12.0\nDWF GLOBAL 40.0\n\n"
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE OFF\nRADIATIVE_EXCHANGE OFF\n");
    {
        std::ofstream f("_h5c.inp");
        f << "[TITLE]\nH5a cascade deck\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
          << "INFILTRATION HORTON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 3:00:00\n"
          << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n"
          << "RG0 INTENSITY 0:05 1.0 TIMESERIES zero_ts\n\n"
          << "[TIMESERIES]\n" << rain_series(185, 2.0) << zero_series(185)
          << "[SUBCATCHMENTS]\nS1 RG1 S2 5 50 500 0.5 0\n"
          << "S2 RG0 J1 5 50 500 0.5 0\n\n"
          << "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n"
          << "S2 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
          << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\nS2 3.0 0.5 4 7 0\n\n"
          << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
          << "[DWF]\nJ1 FLOW 5.0\n\n"
          << "[OUTFALLS]\nOUT 9.0 FREE  NO  S1\n\n"
          << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.heat config=\"_h5c.heat\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = run_and_hold("_h5c.inp", "_h5c.rpt", "_h5c.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    ASSERT_TRUE(ProducedRunoff(ctx));

    // SETUP: no flux module may be on, or warmth on S2 proves nothing.
    ASSERT_FALSE(ctx.heat_config.surface_exchange);
    ASSERT_FALSE(ctx.heat_config.radiative_exchange);
    // SETUP: the cascade must be delivering water, and the receiver must
    // have no rain of its own — otherwise its own 12 C rain is a second
    // source and the inequality below stops being decisive.
    ASSERT_EQ(ctx.subcatches.outlet_subcatch[0], 1)
        << "S1 does not drain to S2 — the run-on path is not reachable";
    ASSERT_EQ(ctx.subcatches.rainfall[1], 0.0)
        << "the receiver is being rained on — the zero gage did not take";
    ASSERT_GT(ctx.subcatches.runon_inflow[1], 0.0)
        << "no run-on reached the receiver at the final step";
    // SETUP: and the donor must actually be warm, or there is nothing for
    // the cascade to carry.
    ASSERT_GE(static_cast<int>(hs.subcatch_runoff_temp.size()), 2);
    ASSERT_GT(hs.subcatch_runoff_temp[0], kRainC + 1.0e-3)
        << "S1 sheds at " << hs.subcatch_runoff_temp[0] << " C, not above "
           "the " << kRainC << " C rain — the outfall return is not warming "
           "the donor, so this gate has no contrast to measure";

    EXPECT_GT(hs.subcatch_runoff_temp[1], kRainC + 1.0e-3)
        << "S2 sheds at " << hs.subcatch_runoff_temp[1] << " C. It receives "
           "no rain and no flux module is enabled, so its only possible "
           "source of water above " << kRainC << " C is S1's runoff arriving "
           "across the cascade. The rain temperature exactly is what the "
           "fallback returns when the run-on scatter never wrote a numerator.";
    EXPECT_LE(hs.subcatch_runoff_temp[1], kDwfC + 1.0e-9)
        << "S2 sheds at " << hs.subcatch_runoff_temp[1] << " C, above the "
        << kDwfC << " C warmest source in a model with no fluxes.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — the run-on divisor is the KNOWN rate, not the total run-on.
//          This is the structural guard against A3's defect recurring when
//          H5b adds the LID underdrain contributor.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, RunonTemperatureNeverFallsBelowEveryContributor) {
    DeckOpts o{};
    o.cascade = true;
    write_heat_cfg("_h5d.heat", 12.0, 12.0, o);   // fluxes OFF: pure transport
    write_deck("_h5d.inp", "_h5d.heat", o);
    SWMM_Engine e = run_and_hold("_h5d.inp", "_h5d.rpt", "_h5d.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    ASSERT_TRUE(ProducedRunoff(ctx));
    ASSERT_FALSE(ctx.heat_config.surface_exchange);
    ASSERT_FALSE(ctx.heat_config.radiative_exchange);

    // With no fluxes, nothing can add or remove energy: every parcel in the
    // model is at 12 C, so every reported temperature must be 12 C. This
    // needs no reference value beyond the source itself — the same
    // no-reference shape as A3's run-on floor gate, which is what stops it
    // rotting when a deck or a timestep changes.
    for (std::size_t i = 0; i < hs.subcatch_runoff_temp.size(); ++i)
        EXPECT_NEAR(hs.subcatch_runoff_temp[i], 12.0, 1.0e-9)
            << "subcatchment " << i << " reports "
            << hs.subcatch_runoff_temp[i] << " C in a model where every "
               "source is 12 C and no flux module is enabled. A value pulled "
               "toward 0 is the signature of dividing by a run-on rate "
               "larger than the one whose temperature was counted";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 5 — DRY_ELEMENT_TEMPERATURE selects, and the three policies differ.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, DryElementPolicyIsSelectableAndTheModesDiffer) {
    // A long dry tail after a short storm, so subareas are genuinely dry at
    // the end and the policy governs what they report.
    auto run_policy = [](const char* policy, double* out_perv) {
        DeckOpts o{};
        o.dry_policy = policy;
        o.end_min    = 120;
        // The storm STOPS at 10 minutes. `rain_series` is generated out to
        // end_min + 5, so without this the "long dry tail" rained for its
        // whole two hours and every policy read a soaking subarea — all
        // three returning the same number for a reason that had nothing to
        // do with the policy key.
        o.rain_stop_min = 10;
        o.air_f      = 95.0;   // ~35 C, far from both 20 C and the last wet
        write_heat_cfg("_h5e.heat", 5.0, 5.0, o);
        write_deck("_h5e.inp", "_h5e.heat", o);
        SWMM_Engine e = run_and_hold("_h5e.inp", "_h5e.rpt", "_h5e.out");
        if (e == nullptr) return false;
        const auto& ctx = as_cpp_engine(e).context();
        const bool parsed_ok = ctx.heat_config.configured;
        // PERV subarea of S1 — index (0 * kNSub + PERV).
        const auto idx = static_cast<std::size_t>(
            static_cast<int>(openswmm::HeatSubArea::PERV));
        if (idx >= ctx.heat_state.subarea_temp.size() ||
            ctx.heat_state.subarea_vol_prev[idx] > 1.0e-12) {
            // Not dry: the policy branch was never reached and whatever is
            // in the slot is a mixing result, not a policy value.
            *out_perv = std::nan("");
            swmm_engine_destroy(e);
            return false;
        }
        *out_perv = ctx.heat_state.subarea_temp[idx];
        swmm_engine_destroy(e);
        return parsed_ok;
    };

    double t_hold = 0.0, t_air = 0.0, t_default = 0.0;
    ASSERT_TRUE(run_policy("HOLD", &t_hold))
        << "either the heat config failed to parse with "
           "DRY_ELEMENT_TEMPERATURE HOLD, or the pervious subarea still "
           "held water at the end of the run so the dry branch never ran";
    ASSERT_TRUE(run_policy("AIR", &t_air));
    ASSERT_TRUE(run_policy("DEFAULT", &t_default));

    EXPECT_NEAR(t_default, openswmm::HeatConfigData::kDefaultTemp, 1.0e-9)
        << "DEFAULT did not fall to kDefaultTemp";
    EXPECT_NEAR(t_air, 35.0, 0.5)
        << "AIR did not track the air temperature (95 F = 35 C)";
    // HOLD must differ from both, or the key is being ignored and all three
    // runs are the same code path (lesson 56).
    EXPECT_GT(std::fabs(t_hold - t_default), 1.0e-6)
        << "HOLD and DEFAULT produced the same value — the policy key is "
           "not reaching the dry branch";
    EXPECT_GT(std::fabs(t_hold - t_air), 1.0e-6)
        << "HOLD and AIR produced the same value";
}

// ---------------------------------------------------------------------------
// Gate 6 — a bad policy value is REFUSED, not clamped.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, AnUnknownDryPolicyIsRefused) {
    DeckOpts o{};
    o.dry_policy = "SOMETIMES";
    write_heat_cfg("_h5f.heat", 5.0, 5.0, o);
    write_deck("_h5f.inp", "_h5f.heat", o);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const int rc = swmm_engine_open(e, "_h5f.inp", "_h5f.rpt", "_h5f.out",
                                    nullptr);
    const int rc2 = (rc == SWMM_OK) ? swmm_engine_initialize(e) : rc;
    EXPECT_TRUE(rc != SWMM_OK || rc2 != SWMM_OK)
        << "an unrecognised DRY_ELEMENT_TEMPERATURE value was accepted. "
           "HeatComponent rolls its config back wholesale on any error "
           "(HeatComponent.cpp:300-303); a silent fallback to HOLD would "
           "hand the user a policy they did not ask for";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 8 — every run-on contributor, on one deck. The structural guard for
//          §4.1's design decision, and the reason it is here rather than
//          recorded as owed: on every other deck in this file the cascade is
//          the ONLY contributor to `runon_inflow`, so `subcatch_runon_temp_
//          rate` and `subcatches.runon_inflow` hold bit-identical values
//          (measured: 7.68722753 both) and dividing by either gives the same
//          answer. A3's defect was not merely absent from those decks — it
//          was UNOBSERVABLE on them, which is what let it ship.
//
// This deck separates the two contributors so each has a witness:
//
//   S1  hosts a bioretention cell whose underdrain returns to S1. That flow
//       is in `runon_inflow` and NOT in the known rate — H5b supplies its
//       temperature. Correct code divides by the known rate, finds it zero,
//       and falls back to the RAINFALL temperature: S1 reads exactly 12 C.
//       Dividing by the total instead divides a zero numerator by 0.98 cfs
//       and hands the surface 0 C — below every source in the model.
//
//   S2  receives the outfall's discharge, which carries a 40 C dry-weather
//       flow. Correct code carries it: S2 reads 16.4 C. Dropping the outfall
//       producer takes the numerator and the rate together, so the fallback
//       returns 12 C exactly — a warmed surface going cold.
//
// Both assertions are bounds on the sources, not remembered numbers: with
// every flux module off nothing can add or remove energy, so every
// temperature in the model must lie inside [12, 40].
//
// LID layer parameters are in FEET and FEET/SECOND — see the warning in
// test_water_age_lid.cpp; the engine does not unit-convert them (issue #131).
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, EveryRunonContributorKeepsTemperaturesInsideTheSources) {
    constexpr double kFloorC = 12.0;   // RAINFALL and INITIAL_STATE
    constexpr double kCeilC  = 40.0;   // DWF, the only warmer source
    write_file("_h5i.heat",
               "[HEAT_SOURCES]\nRAINFALL GLOBAL 12.0\n"
               "INITIAL_STATE GLOBAL 12.0\nDWF GLOBAL 40.0\n\n"
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE OFF\nRADIATIVE_EXCHANGE OFF\n");
    {
        std::ofstream f("_h5i.inp");
        f << "[TITLE]\nH5a run-on contributor deck\n\n[OPTIONS]\n"
          << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nHEAT_TRANSPORT ON\n"
          << "INFILTRATION HORTON\n"
          << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
          << "END_DATE 01/01/2026\nEND_TIME 3:00:00\n"
          << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
          << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
          << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
          << "[TIMESERIES]\n" << rain_series(185, 2.0)
          << "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0\n"
          << "S2 RG1 J1 5 50 500 0.5 0\n\n"
          << "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n"
          << "S2 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
          << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\nS2 3.0 0.5 4 7 0\n\n"
          << "[LID_CONTROLS]\n"
          << "BC1 BC\n"
          << "BC1 SURFACE  0.05   0.0  0.1  1.0  5\n"
          << "BC1 SOIL     0.25   0.5  0.2  0.1  2.0e-5 10.0 0.3\n"
          << "BC1 STORAGE  1.0    0.75 0.0  0\n"
          << "BC1 DRAIN    1.0e-3 0.5  0    0\n\n"
          << "[LID_USAGE]\nS1 BC1 1 43560 500 50 100 0\n\n"
          << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
          << "[DWF]\nJ1 FLOW 5.0\n\n"
          // The trailing S2 is the RouteTo column: this outfall's discharge
          // re-enters the runoff system as run-on onto S2.
          << "[OUTFALLS]\nOUT 9.0 FREE  NO  S2\n\n"
          << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.heat config=\"_h5i.heat\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
    SWMM_Engine e = run_and_hold("_h5i.inp", "_h5i.rpt", "_h5i.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    ASSERT_TRUE(ProducedRunoff(ctx));
    ASSERT_FALSE(ctx.heat_config.surface_exchange);
    ASSERT_FALSE(ctx.heat_config.radiative_exchange);
    ASSERT_EQ(ctx.n_subcatches(), 2);

    // SETUP: both extra contributors must be returning water, or this is
    // the cascade deck again under another name.
    ASSERT_GT(ctx.subcatches.runon_inflow[0], 0.0)
        << "no run-on reached S1 — the LID underdrain is not returning";
    ASSERT_GT(ctx.subcatches.runon_inflow[1], 0.0)
        << "no run-on reached S2 — the outfall RouteTo column did not parse";

    // COMPLETENESS, and the single most important assertion in this file.
    //
    // It used to read the other way round. H5a could not supply a
    // temperature for the LID underdrain — a drain's temperature is a
    // per-layer quantity that did not exist until H5b — so this deck
    // deliberately carried an UNCOUNTED contributor and the gate asserted
    // that `runon_inflow` exceeded the known rate, to prove the two divisors
    // could be told apart.
    //
    // H5b supplies it, so the two now coincide (measured 0.9797978893380731
    // for both) and the old assertion fails. That is the phase doing its
    // job, and the assertion inverts into the stronger claim it was always
    // reaching for: **every cfs of run-on has a known temperature.** A
    // fourth contributor added to `runon_inflow` without a matching
    // `addRunonTemperatureAt` fires here, which is the failure A3 shipped
    // and A4 found.
    //
    // Note what this costs: with every contributor counted, dividing by
    // `subcatches.runon_inflow` instead of `subcatch_runon_temp_rate` is now
    // arithmetically identical, so A3's defect is UNREPRESENTABLE in the
    // heat track rather than merely absent. This assertion is what keeps it
    // that way.
    EXPECT_NEAR(hs.subcatch_runon_temp_rate[0], ctx.subcatches.runon_inflow[0],
                1.0e-12)
        << "S1 receives " << ctx.subcatches.runon_inflow[0] << " cfs of "
           "run-on but only " << hs.subcatch_runon_temp_rate[0] << " cfs of "
           "it has a temperature. Some contributor is adding to the flow "
           "path without going through addRunonTemperatureAt.";
    EXPECT_NEAR(hs.subcatch_runon_temp_rate[1], ctx.subcatches.runon_inflow[1],
                1.0e-12)
        << "S2 receives " << ctx.subcatches.runon_inflow[1] << " cfs of "
           "run-on but only " << hs.subcatch_runon_temp_rate[1] << " cfs of "
           "it has a temperature.";

    // (0) The volume ledger must be in ft3. `RunoffSoA::area` is ft2 and
    //     LID-excluded; `ctx.subcatches.area` is the deck's USER area units,
    //     so reaching for the obvious one is a 43560x error here and not the
    //     LID-footprint double-count it reads as. A temperature is intensive
    //     and the area cancels inside `relaxT`, so the flux cannot see it —
    //     the ledger can. Five acres under three hours of 2 in/hr rain hold
    //     far more than a cubic foot; under the substitution the whole
    //     subcatchment ledgers 0.78 ft3.
    double ponded_ft3 = 0.0;
    for (int k = 0; k < kNSub; ++k)
        ponded_ft3 += hs.subarea_vol_prev[static_cast<std::size_t>(k)];
    EXPECT_GT(ponded_ft3, 1.0)
        << "S1's three subareas hold " << ponded_ft3 << " ft3 between them "
           "after three hours of 2 in/hr rain on five acres. That is not a "
           "volume — it is an area in the wrong units (acres for ft2, "
           "43560x) reaching the ponded-volume expression.";

    // (1) Nothing may fall below the coldest source. With every flux off,
    //     water at 0 C cannot exist in this model — it is the signature of a
    //     numerator that some contributor never wrote to, divided by a rate
    //     that counted it anyway.
    for (std::size_t i = 0; i < hs.subarea_temp.size(); ++i) {
        if (hs.subarea_vol_prev[i] <= 0.0) continue;   // holds no water
        EXPECT_GE(hs.subarea_temp[i], kFloorC - 1.0e-9)
            << "subarea " << i << " holds water at " << hs.subarea_temp[i]
            << " C, below the " << kFloorC << " C coldest source in a model "
               "with no flux module enabled.";
        EXPECT_LE(hs.subarea_temp[i], kCeilC + 1.0e-9)
            << "subarea " << i << " holds water at " << hs.subarea_temp[i]
            << " C, above the " << kCeilC << " C warmest source.";
    }

    // (2) S1's uncounted contributor must leave the mean over LESS water,
    //     not drag it toward zero: with nothing known, the fallback is the
    //     rain temperature exactly.
    EXPECT_NEAR(hs.subcatch_runoff_temp[0], kFloorC, 1.0e-9)
        << "S1 sheds at " << hs.subcatch_runoff_temp[0] << " C. Its only "
           "run-on is a LID underdrain, which since H5b leaves at its own "
           "storage-layer temperature — and with no flux enabled the whole "
           "column is still at the " << kFloorC << " C it was rained on "
           "with. Before H5b this held for a different reason: the known "
           "rate was zero and the fallback was the rain temperature. Both "
           "answers are " << kFloorC << ", which is why the deck could not "
           "tell them apart and the completeness assertion above exists.";

    // (3) S2's counted contributor must actually be carried. Without it the
    //     same fallback returns exactly the rain temperature, so this is a
    //     strict inequality against a value the failure mode produces
    //     exactly — not a band around a remembered number.
    EXPECT_GT(hs.subcatch_runoff_temp[1], kFloorC + 1.0e-3)
        << "S2 sheds at " << hs.subcatch_runoff_temp[1] << " C, which is the "
           "rain temperature. It receives the outfall's discharge carrying a "
           << kCeilC << " C dry-weather flow, so its temperature must be "
                        "above the rain's — the outfall return is not "
                        "reaching addRunonTemperatureAt.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 9 — the surface balance is a FORWARD EULER step, and a ponded film
//          has almost no thermal mass. This gate holds the bound that keeps
//          it from diverging.
//
// Measured without the bound, on this deck: a 0.52 ft3 film spread over
// 27226 ft2 takes a +862 C step in 60 s, the flux is then re-evaluated at
// 182 C, and the sequence runs 5 -> 182 -> -1.8e4 -> -3.9e9 -> 3.9e15 ->
// ... -> inf -> NaN. The NaN reaches subcatch_runoff_temp, the wet-weather
// loader carries it to the node, and the report prints it.
//
// The receiver is starved of rain on purpose: fed only by run-on its
// surface wets slowly, so it holds the thin films long enough for the
// instability to run away. A rain-fed surface deepens fast enough to
// recover, which is the only reason the other gates in this file did not
// see it — they were taking excursions of several hundred degrees and
// coming back.
//
// The assertions are what physics allows, not remembered numbers: a
// temperature must be finite, and water forced by 30 C air over 5 C rain
// cannot leave a band far wider than either.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, AThinFilmDoesNotDivergeUnderTheExplicitStep) {
    DeckOpts o{};
    o.cascade         = true;
    o.starve_receiver = true;
    o.surface         = true;
    o.air_f           = 86.0;   // ~30 C over 5 C rain
    write_heat_cfg("_h5j.heat", 5.0, 5.0, o);
    write_deck("_h5j.inp", "_h5j.heat", o);
    SWMM_Engine e = run_and_hold("_h5j.inp", "_h5j.rpt", "_h5j.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    ASSERT_TRUE(ProducedRunoff(ctx));
    ASSERT_TRUE(ctx.heat_config.surface_exchange)
        << "SURFACE_EXCHANGE is off, so no flux is being integrated and "
           "there is no stability limit to test";
    // SETUP: the receiver must be the slowly-wetting one, or its films are
    // never thin enough for the unresolved step to occur.
    ASSERT_EQ(ctx.subcatches.rainfall[1], 0.0)
        << "the receiver is being rained on — its surface deepens fast "
           "enough to mask the instability";

    for (std::size_t i = 0; i < hs.subarea_temp.size(); ++i) {
        EXPECT_TRUE(std::isfinite(hs.subarea_temp[i]))
            << "subarea " << i << " reports " << hs.subarea_temp[i]
            << ". A forward-Euler surface balance on a film with no thermal "
               "mass diverges in a handful of steps; the step bound is gone.";
        EXPECT_GE(hs.subarea_temp[i], -50.0) << "subarea " << i;
        EXPECT_LE(hs.subarea_temp[i], 100.0)
            << "subarea " << i << " reports " << hs.subarea_temp[i]
            << " C under 30 C air over 5 C rain — no flux in this deck can "
               "put water there, so the explicit step has overshot.";
    }
    for (std::size_t i = 0; i < hs.subcatch_runoff_temp.size(); ++i)
        EXPECT_TRUE(std::isfinite(hs.subcatch_runoff_temp[i]))
            << "subcatchment " << i << " sheds water at "
            << hs.subcatch_runoff_temp[i] << ", which the wet-weather loader "
               "then carries into the node temperatures and the report";
    for (double t : hs.node_temp)
        EXPECT_TRUE(std::isfinite(t))
            << "a node temperature is " << t << " — a non-finite subcatchment "
               "runoff temperature has escaped the watershed stage";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 10 — a freezing surface reports a sub-zero temperature.
//
// `WaterAgeWatershed` clamps with max(a, 0) because a negative age is
// meaningless. Carrying that clamp across to temperature would manufacture
// energy on every frozen surface AND hide it: 0 C is a real temperature, so
// the clamped value reads as data. Every other deck in this file is forced
// above freezing, which is why the clamp is invisible to them.
//
// Forcing: -15 C air, 30% humidity, 15 mph wind, both flux modules on, over
// 2 C rain. Measured -5.47 C; the clamp returns exactly 0.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, AFreezingSurfaceIsNotClampedToZero) {
    DeckOpts o{};
    o.surface      = true;
    o.radiative    = true;
    o.air_f        = 5.0;    // -15 C
    o.wind_mph     = 15.0;
    o.humidity_pct = 30.0;
    write_heat_cfg("_h5k.heat", 2.0, 2.0, o);
    write_deck("_h5k.inp", "_h5k.heat", o);
    SWMM_Engine e = run_and_hold("_h5k.inp", "_h5k.rpt", "_h5k.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    const auto& hs  = ctx.heat_state;
    ASSERT_TRUE(ProducedRunoff(ctx));
    ASSERT_TRUE(ctx.heat_config.surface_exchange);
    ASSERT_TRUE(ctx.heat_config.radiative_exchange);
    // SETUP: the forcing must actually be below freezing, or nothing here
    // can drive a surface under 0 C and the gate passes on nothing.
    const double t_air_c = (ctx.climate_state.temperature - 32.0) * 5.0 / 9.0;
    ASSERT_LT(t_air_c, -5.0)
        << "air is at " << t_air_c << " C — the deck is not cold enough to "
           "drive a ponded surface below freezing";

    bool any_sub_zero = false;
    for (std::size_t i = 0; i < hs.subarea_temp.size(); ++i)
        if (hs.subarea_vol_prev[i] > 0.0 && hs.subarea_temp[i] < 0.0)
            any_sub_zero = true;
    EXPECT_TRUE(any_sub_zero)
        << "no ponded subarea is below 0 C under " << t_air_c << " C air. "
           "A value resting at exactly 0 is the age module's floor applied "
           "to a temperature: it manufactures energy on every frozen surface "
           "and reads as data rather than as a clamp.";
    EXPECT_LT(hs.subcatch_runoff_temp[0], 0.0)
        << "S1 sheds at " << hs.subcatch_runoff_temp[0] << " C into "
        << t_air_c << " C air.";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 7 — HEAT_TRANSPORT off takes no new path at all.
// ---------------------------------------------------------------------------
TEST(HeatWatershedTest, HeatOffLeavesTheWatershedStateEmpty) {
    std::ofstream f("_h5g.inp");
    f << "[TITLE]\nH5a inert deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "WET_STEP 00:01:00\nDRY_STEP 00:05:00\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
      << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES rain_ts\n\n"
      << "[TIMESERIES]\n" << rain_series(65, 2.0)
      << "[SUBCATCHMENTS]\nS1 RG1 J1 5 50 500 0.5 0\n\n"
      << "[SUBAREAS]\nS1 0.01 0.1 0.05 0.10 25 OUTLET\n\n"
      << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
      << "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 9.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n"
      << "[REPORT]\nINPUT NO\n";
    f.close();

    SWMM_Engine e = run_and_hold("_h5g.inp", "_h5g.rpt", "_h5g.out");
    ASSERT_NE(e, nullptr);
    const auto& ctx = as_cpp_engine(e).context();
    ASSERT_FALSE(ctx.options.heat_transport);
    ASSERT_TRUE(ProducedRunoff(ctx));
    EXPECT_TRUE(ctx.heat_state.subarea_temp.empty())
        << "the watershed heat state was allocated on a deck with "
           "HEAT_TRANSPORT off — the guard is not where it needs to be "
           "(lesson 52: reaching loadersNeeded is not reaching the stage)";
    EXPECT_TRUE(ctx.heat_state.subcatch_runon_temp_rate.empty());
    swmm_engine_destroy(e);
}
