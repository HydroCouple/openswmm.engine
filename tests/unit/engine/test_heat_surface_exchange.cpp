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
 * @file test_heat_surface_exchange.cpp
 * @brief Phase H2 gates — latent + sensible surface exchange.
 *
 * @details Two kinds of gate, deliberately separated:
 *
 *          **Formulation gates** check the published physics against values
 *          computed OUTSIDE this codebase (CSH §4.4–4.5 / Dingman 2008 /
 *          Martin & McCutcheon 1998). They are the only assertions here
 *          that can catch a transcription error in a coefficient, because
 *          the engine has no independent opinion about what e_s(20 °C) is.
 *
 *          **Binding gates** check that the flux reaches the right water.
 *          The load-bearing claim is that heat exchanges exactly where
 *          EVAPORATION does — storage-node free surfaces and open conduits,
 *          nothing else — which is what makes the module solver-independent
 *          instead of quietly dead under STEADY and KINWAVE.
 *
 *          `CpIsNowObservable` is the gate H1 could not have written: with
 *          no fluxes, ρw·cp cancelled on both sides of every mixing
 *          operation and their VALUE was unfalsifiable (validation proved
 *          this by scaling both sides by 42 and watching all nine H1 gates
 *          stay green). A flux competes with thermal mass, so cp now sets a
 *          rate that can be wrong.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §2.1, §2.4, §6 H2
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/HeatFluxModules/SurfaceExchange.hpp"

namespace se = openswmm::transport::heat;

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kTWater = 20.0;  ///< °C, the deck's INITIAL_STATE
constexpr double kTAirC  = 10.0;  ///< °C  (50 °F in the deck)
constexpr double kRH     = 50.0;  ///< %
constexpr double kWindMph = 5.0;

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// A storage node (the only node type with a free surface) fed by nothing,
/// draining through one conduit. `open_channel` selects RECT_OPEN vs
/// CIRCULAR so the conduit branch can be switched on and off by geometry
/// rather than by a flag.
void write_deck(const char* path, const std::string& extra_options,
                const std::string& pc_lines, bool open_channel = true,
                bool storage = true, const char* end_time = "01:00:00") {
    std::ofstream f(path);
    f << "[TITLE]\nH2 surface exchange gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "HEAT_TRANSPORT ON\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << end_time << "\n"
      << "ROUTING_STEP 10\nREPORT_STEP 00:05:00\n"
      << extra_options << "\n"
      << "[TEMPERATURE]\n"
      // 50 degF = 10 degC. Constant humidity and wind so the met inputs are
      // exactly the numbers the formulation gates use.
      << "TIMESERIES air_ts\n"
      << "WINDSPEED MONTHLY " << kWindMph << " " << kWindMph << " "
      << kWindMph << " " << kWindMph << " " << kWindMph << " " << kWindMph
      << " " << kWindMph << " " << kWindMph << " " << kWindMph << " "
      << kWindMph << " " << kWindMph << " " << kWindMph << "\n"
      << "HUMIDITY " << kRH << "\n\n"
      << "[TIMESERIES]\nair_ts 01/01/2026 00:00 50.0\n"
      << "air_ts 01/02/2026 00:00 50.0\n\n";
    if (storage)
        f << "[STORAGE]\n"
          << ";;Name Elev MaxD InitD Shape A1 A2 A0\n"
          << "S1 10.0 12 6.0 FUNCTIONAL 0 0 5000\n\n"
          << "[JUNCTIONS]\nJ1 9.0 10 1.0 0 0\n\n";
    else
        f << "[JUNCTIONS]\nS1 10.0 12 6.0 0 0\nJ1 9.0 10 1.0 0 0\n\n";
    f << "[OUTFALLS]\nOUT 8.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 S1 J1  500 0.013 0 0 0\nC2 J1 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << (open_channel
              ? "C1 RECT_OPEN 3.0 4.0 0 0\nC2 RECT_OPEN 3.0 4.0 0 0\n\n"
              : "C1 CIRCULAR 3.0 0 0 0\nC2 CIRCULAR 3.0 0 0 0\n\n");
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

std::string heat_cfg(bool surface_on) {
    return std::string("[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
                       "[HEAT_FLUXES]\nSURFACE_EXCHANGE ") +
           (surface_on ? "ON\n" : "OFF\n");
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
// Gate 1 — the formulations, against values computed outside this codebase.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, FormulationsMatchPublishedValues) {
    // e_s(0 °C) is the coefficient itself — the sharpest single check that
    // 0.61275 was transcribed correctly.
    EXPECT_NEAR(se::saturationVapourPressure(0.0), 0.61275, 1.0e-9);
    EXPECT_NEAR(se::saturationVapourPressure(10.0), 1.23188293, 1.0e-6);
    EXPECT_NEAR(se::saturationVapourPressure(20.0), 2.34574631, 1.0e-6);

    // Le(0) = 2.499e6 J/kg; the 2.36 slope is what the 20 °C value pins.
    EXPECT_NEAR(se::latentHeatOfVaporization(0.0), 2'499'000.0, 1.0e-6);
    EXPECT_NEAR(se::latentHeatOfVaporization(20.0), 2'451'800.0, 1.0e-6);

    // f(w) = a + b·w with the Dunne & Leopold defaults at 5 mph.
    const double w_ms = kWindMph * 0.44704;
    EXPECT_NEAR(se::windFunction(w_ms, 1.505e-8, 1.6e-8), 5.08132e-08, 1.0e-13);

    // Bowen ratio, water 20 °C over air 10 °C at 50 % RH.
    EXPECT_NEAR(se::bowenRatio(kTWater, kTAirC, kRH, 1.0), 0.35264094, 1.0e-6);

    // Latent flux, same conditions. Positive = leaving the water.
    const double je = se::latentFlux(kTWater, kTAirC, kRH, w_ms, 1.505e-8,
                                     1.6e-8, 1000.0);
    EXPECT_NEAR(je, 215.5056677, 1.0e-4)
        << "Je is " << je << " W/m2 — check rho_w, Le and f(w) separately; "
           "each has its own assertion above.";
    EXPECT_NEAR(se::sensibleFlux(je, 0.35264094), 75.99612, 1.0e-3);
}

// ---------------------------------------------------------------------------
// Gate 2 — the vapour-deficit singularity is removable, not a NaN factory.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, SaturatedAirAtWaterTemperatureIsFinite) {
    // Air at the water's own temperature and 100 % RH: the deficit is
    // exactly zero, so Br's denominator vanishes. A naive division makes
    // every downstream temperature NaN in one step.
    const double br = se::bowenRatio(kTWater, kTWater, 100.0, 1.0);
    EXPECT_TRUE(std::isfinite(br)) << "Bowen ratio is not finite at zero "
                                      "vapour-pressure deficit";
    const double je = se::latentFlux(kTWater, kTWater, 100.0,
                                     kWindMph * 0.44704, 1.505e-8, 1.6e-8,
                                     1000.0);
    EXPECT_NEAR(je, 0.0, 1.0e-12);
    EXPECT_TRUE(std::isfinite(se::sensibleFlux(je, br)));
}

// ---------------------------------------------------------------------------
// Gate 3 — dry air over warm water cools it, and the module is what does it.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, SurfaceExchangeCoolsAStoragePool) {
    write_file("_hx_on.heat", heat_cfg(true));
    write_deck("_hx_on.inp", "", "org.hydrocouple.openswmm.heat "
                                 "config=\"_hx_on.heat\"");
    SWMM_Engine on = run_and_hold("_hx_on.inp", "_hx_on.rpt", "_hx_on.out");
    ASSERT_NE(on, nullptr);
    const auto& ctx_on = as_cpp_engine(on).context();
    ASSERT_FALSE(ctx_on.heat_state.node_temp.empty());
    const double t_on = ctx_on.heat_state.node_temp[0];

    write_file("_hx_off.heat", heat_cfg(false));
    write_deck("_hx_off.inp", "", "org.hydrocouple.openswmm.heat "
                                  "config=\"_hx_off.heat\"");
    SWMM_Engine off = run_and_hold("_hx_off.inp", "_hx_off.rpt", "_hx_off.out");
    ASSERT_NE(off, nullptr);
    const double t_off = as_cpp_engine(off).context().heat_state.node_temp[0];

    // SETUP FIRST (lesson 36): with the module off this is pure H1
    // transport on a deck with no inflow, so the pool must sit exactly at
    // its INITIAL_STATE. If it does not, the comparison below is measuring
    // something other than the flux.
    EXPECT_NEAR(t_off, kTWater, 1.0e-9)
        << "with SURFACE_EXCHANGE OFF the pool reads " << t_off
        << " degC, not its 20 degC initial state — something other than the "
           "flux module is moving temperature";

    EXPECT_LT(t_on, t_off)
        << "the pool reads " << t_on << " degC with exchange on versus "
        << t_off << " off. Dry air (50 % RH) over warmer water evaporates, "
                    "and evaporation cools; warming here means the sign of "
                    "-(Je + Jc) was dropped.";
    EXPECT_GT(t_on, 0.0) << "an hour of exchange should not freeze the pool — "
                            "check the ft2/m2 conversion";
    swmm_engine_destroy(on);
    swmm_engine_destroy(off);
}

// ---------------------------------------------------------------------------
// Gate 4 — cp is observable now. This is the gate H1 could not write.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, SpecificHeatIsNowObservable) {
    // FIVE MINUTES, not the shared hour. "Doubling cp halves the cooling" is
    // exact only while the flux is constant, and the flux depends on Tw
    // through e_s(Tw) and the Bowen ratio: the 1x pool cools further, so its
    // own flux decays, and the ratio drifts UP. Measured on this deck:
    //
    //     5 min  0.500229      30 min 0.511499      2 h  0.781205
    //     15 min 0.502017       1 h   0.564816
    //
    // The hour was inside the nonlinear regime, which is why a 0.02 band
    // around 0.5 could not hold. Shortening the window is the fix rather
    // than widening the band — at 5 min the law holds to 2e-4, so the razor
    // is TIGHTER here than a loosened one at an hour, and a ratio of 1.0
    // (cp ignored) still misses by half.
    write_file("_hx_cp.heat", heat_cfg(true));
    write_deck("_hx_cp1.inp", "", "org.hydrocouple.openswmm.heat "
                                  "config=\"_hx_cp.heat\"",
               /*open_channel=*/true, /*storage=*/true, "00:05:00");
    SWMM_Engine a = run_and_hold("_hx_cp1.inp", "_hx_cp1.rpt", "_hx_cp1.out");
    ASSERT_NE(a, nullptr);
    const double dt1 =
        kTWater - as_cpp_engine(a).context().heat_state.node_temp[0];
    swmm_engine_destroy(a);

    // Double the thermal mass; the same flux must move the temperature half
    // as far. A ratio test needs no geometry, so it cannot be fooled by a
    // wrong area.
    write_deck("_hx_cp2.inp", "WATER_SPECIFIC_HEAT_CAPACITY 8368\n",
               "org.hydrocouple.openswmm.heat config=\"_hx_cp.heat\"",
               /*open_channel=*/true, /*storage=*/true, "00:05:00");
    SWMM_Engine b = run_and_hold("_hx_cp2.inp", "_hx_cp2.rpt", "_hx_cp2.out");
    ASSERT_NE(b, nullptr);
    const double dt2 =
        kTWater - as_cpp_engine(b).context().heat_state.node_temp[0];
    swmm_engine_destroy(b);

    ASSERT_GT(dt1, 1.0e-6) << "the pool did not cool at all — nothing to halve";
    EXPECT_NEAR(dt2 / dt1, 0.5, 0.005)
        << "doubling WATER_SPECIFIC_HEAT_CAPACITY changed the cooling by a "
           "factor of " << (dt2 / dt1) << ", expected 0.5. A ratio of 1.0 "
           "means cp never reached the flux arithmetic — which is exactly "
           "the unobservability H1 shipped with and H2 was supposed to end.";
}

// ---------------------------------------------------------------------------
// Gate 5 — only water with a free surface exchanges.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, JunctionsAndClosedConduitsDoNotExchange) {
    // (a) The same deck with the storage replaced by a JUNCTION. Legacy
    //     gives a junction no surface area (node::getSurfArea returns 0),
    //     and its evaporation obeys that too — a manhole is closed.
    write_file("_hx_j.heat", heat_cfg(true));
    write_deck("_hx_j.inp", "", "org.hydrocouple.openswmm.heat "
                                "config=\"_hx_j.heat\"",
               /*open_channel=*/false, /*storage=*/false);
    SWMM_Engine j = run_and_hold("_hx_j.inp", "_hx_j.rpt", "_hx_j.out");
    ASSERT_NE(j, nullptr);
    const auto& ctx_j = as_cpp_engine(j).context();
    ASSERT_FALSE(ctx_j.heat_state.node_temp.empty());
    EXPECT_NEAR(ctx_j.heat_state.node_temp[0], kTWater, 1.0e-9)
        << "a junction exchanged heat with the atmosphere. It has no free "
           "surface by the engine's own convention — the same one that "
           "stops it evaporating.";

    // (b) Closed conduits likewise: xsect::isOpen gates the link branch.
    ASSERT_FALSE(ctx_j.heat_state.link_temp.empty());
    EXPECT_NEAR(ctx_j.heat_state.link_temp[0], kTWater, 1.0e-9)
        << "a CIRCULAR conduit exchanged heat — closed sections have no free "
           "surface";
    swmm_engine_destroy(j);

    // (c) The positive control: the SAME geometry opened up must exchange,
    //     or (a) and (b) prove only that the module is dead everywhere.
    write_deck("_hx_open.inp", "", "org.hydrocouple.openswmm.heat "
                                   "config=\"_hx_j.heat\"",
               /*open_channel=*/true, /*storage=*/false);
    SWMM_Engine o = run_and_hold("_hx_open.inp", "_hx_open.rpt", "_hx_open.out");
    ASSERT_NE(o, nullptr);
    const auto& ctx_o = as_cpp_engine(o).context();
    EXPECT_LT(ctx_o.heat_state.link_temp[0], kTWater - 1.0e-9)
        << "an OPEN conduit did not exchange either — the module is inert, "
           "and legs (a)/(b) above were passing vacuously";
    swmm_engine_destroy(o);
}

// ---------------------------------------------------------------------------
// Gate 6 — HUMIDITY reaches the climate state.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, HumidityReachesTheClimateState) {
    // ClimateState::humidity has carried a 50 % default with NO writer since
    // before this program. Without the new [TEMPERATURE] HUMIDITY key it is
    // frozen there, and 50 happens to be this deck's own value — so the
    // gate uses 90 %, a number the default cannot produce.
    write_file("_hx_rh.heat", heat_cfg(true));
    write_deck("_hx_rh.inp", "", "org.hydrocouple.openswmm.heat "
                                 "config=\"_hx_rh.heat\"");
    {
        std::ifstream in("_hx_rh.inp");
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        const std::string from = "HUMIDITY 50";
        const auto at = body.find(from);
        ASSERT_NE(at, std::string::npos) << "deck helper changed shape";
        body.replace(at, from.size(), "HUMIDITY 90");
        write_file("_hx_rh.inp", body);
    }
    SWMM_Engine e = run_and_hold("_hx_rh.inp", "_hx_rh.rpt", "_hx_rh.out");
    ASSERT_NE(e, nullptr);
    EXPECT_NEAR(as_cpp_engine(e).context().climate_state.humidity, 90.0, 1.0e-9)
        << "climate_state.humidity is "
        << as_cpp_engine(e).context().climate_state.humidity
        << " — the [TEMPERATURE] HUMIDITY row never reached the running "
           "state, so RH is still the dead 50 % default";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 7 — the flux-module toggles: deferrals name their phase.
// ---------------------------------------------------------------------------
TEST(HeatSurfaceExchangeTest, FluxModuleTogglesParseAndDefer) {
    // This leg asserted that RADIATIVE_EXCHANGE refuses, which was right
    // until H3 implemented it. Retiring a deferral has to flip the gate that
    // asserted it, in the same changeset — H3's own gate covers its side, and
    // this is the other one. SEDIMENT_EXCHANGE (H6) is now the deferral still
    // owed, so the leg moves there rather than being deleted.
    write_file("_hx_h4.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nSEDIMENT_EXCHANGE ON\n");
    write_deck("_hx_h4.inp", "", "org.hydrocouple.openswmm.heat "
                                 "config=\"_hx_h4.heat\"");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_NE(swmm_engine_open(e, "_hx_h4.inp", "_hx_h4.rpt", "_hx_h4.out",
                               nullptr),
              SWMM_OK)
        << "SEDIMENT_EXCHANGE is H6 and must refuse, not silently do nothing";
    swmm_engine_destroy(e);

    write_file("_hx_bad.heat",
               "[HEAT_SOURCES]\nINITIAL_STATE GLOBAL 20.0\n\n"
               "[HEAT_FLUXES]\nSURFACE_EXCHANGE MAYBE\n");
    write_deck("_hx_bad.inp", "", "org.hydrocouple.openswmm.heat "
                                  "config=\"_hx_bad.heat\"");
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    EXPECT_NE(swmm_engine_open(e2, "_hx_bad.inp", "_hx_bad.rpt", "_hx_bad.out",
                               nullptr),
              SWMM_OK)
        << "'MAYBE' was accepted as a module toggle";
    swmm_engine_destroy(e2);
}
