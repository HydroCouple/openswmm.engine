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
 * @file      test_frozen_ground.cpp
 * @brief     Frozen-ground slab kernel and its subcatchment infiltration limit.
 *
 * @details Kernel cases check the enthalpy form against closed-form answers (freeze time on
 *          the 0 C plateau, exponential relaxation, no overshoot, ledger closure). Deck cases
 *          run one pervious subcatchment and check that frozen ground takes no water under
 *          every infiltration model, that thaw restores infiltration, that snow insulates,
 *          and that a deck whose ground never freezes is bit-identical to one without the
 *          section. Decks are written under `frozen_ground/` in the test working directory.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_infil2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_output.h>
#include <openswmm/engine/openswmm_output2d.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include "core/SWMMEngine.hpp"
#include "hydrology/LID.hpp"
#include "hydrology/FrozenGround.hpp"

namespace fs = std::filesystem;
using namespace openswmm;

namespace {

// ---------------------------------------------------------------------------
// Kernel
// ---------------------------------------------------------------------------

TEST(FrozenGroundKernel, EnthalpyAndTemperatureAreInverse) {
    const frost::Slab s;
    for (double t : {12.0, 0.5, 0.0, -0.5, -9.0})
        EXPECT_NEAR(frost::temperatureOf(s, frost::enthalpyAt(s, t)), t, 1e-12) << t;
    EXPECT_EQ(frost::frozenFraction(s, frost::enthalpyAt(s, 3.0)), 0.0);
    EXPECT_EQ(frost::frozenFraction(s, frost::enthalpyAt(s, -3.0)), 1.0);
    // Half the latent store removed: half frozen, still at 0 C.
    const double half = -0.5 * frost::latentStore(s);
    EXPECT_DOUBLE_EQ(frost::frozenFraction(s, half), 0.5);
    EXPECT_EQ(frost::temperatureOf(s, half), 0.0);
}

TEST(FrozenGroundKernel, FactorIsOneOpenAndExactlyZeroFrozen) {
    frost::Slab s;
    s.exponent = 3.0;
    const double hf = frost::latentStore(s);
    EXPECT_EQ(frost::infiltrationFactor(s, 1.0), 1.0);
    EXPECT_EQ(frost::infiltrationFactor(s, 0.0), 1.0);
    EXPECT_DOUBLE_EQ(frost::infiltrationFactor(s, -0.5 * hf), 0.125);
    EXPECT_EQ(frost::infiltrationFactor(s, -hf), 0.0);
    EXPECT_EQ(frost::infiltrationFactor(s, -2.0 * hf), 0.0);
    double last = 1.0;
    for (int k = 1; k <= 20; ++k) {
        const double f = frost::infiltrationFactor(s, -hf * k / 20.0);
        EXPECT_LT(f, last) << k;
        last = f;
    }
}

// On the plateau the slab sits at 0 C, so a cold contact removes heat at the constant rate
// G |Tc| and the slab is fully frozen after exactly Hf / (G |Tc|) (lumped Stefan time).
TEST(FrozenGroundKernel, FreezeTimeOnThePlateauMatchesTheClosedForm) {
    const frost::Slab s;
    const frost::Contact cold{frost::halfSlabConductance(s), -6.0};
    const double freeze_time = frost::latentStore(s) / (cold.conductance * 6.0);
    const int steps = 400;
    const double dt = freeze_time / steps;
    double h = 0.0;
    for (int k = 1; k <= steps; ++k) {
        h = frost::advance(s, h, &cold, 1, dt);
        if (k < steps) {
            EXPECT_EQ(frost::temperatureOf(s, h), 0.0) << k;
            EXPECT_NEAR(frost::frozenFraction(s, h), double(k) / steps, 1e-12) << k;
        }
    }
    EXPECT_NEAR(frost::frozenFraction(s, h), 1.0, 1e-12);
    // One more step leaves the plateau and cools below 0 C.
    h = frost::advance(s, h, &cold, 1, dt);
    EXPECT_EQ(frost::frozenFraction(s, h), 1.0);
    EXPECT_LT(frost::temperatureOf(s, h), 0.0);
}

// Away from the plateau the slab relaxes exponentially; backward Euler is first order.
TEST(FrozenGroundKernel, SensibleRelaxationConvergesAtFirstOrder) {
    const frost::Slab s;
    const frost::Contact warm{frost::halfSlabConductance(s), 20.0};
    const double tau = frost::sensibleStore(s) / warm.conductance, horizon = tau;
    const double exact = 20.0 + (5.0 - 20.0) * std::exp(-horizon / tau);
    double error[3];
    for (int level = 0; level < 3; ++level) {
        const int steps = 20 << level;
        double h = frost::enthalpyAt(s, 5.0);
        for (int k = 0; k < steps; ++k)
            h = frost::advance(s, h, &warm, 1, horizon / steps);
        error[level] = std::abs(frost::temperatureOf(s, h) - exact);
    }
    EXPECT_NEAR(error[0] / error[1], 2.0, 0.1);
    EXPECT_NEAR(error[1] / error[2], 2.0, 0.05);
}

TEST(FrozenGroundKernel, AHugeStepLandsOnTheContactsAndNeverBeyond) {
    const frost::Slab s;
    const frost::Contact contacts[2] = {{20.0, -10.0}, {5.0, 4.0}};
    const double equilibrium = (20.0 * -10.0 + 5.0 * 4.0) / 25.0;
    for (double start : {15.0, 0.0, -30.0}) {
        const double h = frost::advance(s, frost::enthalpyAt(s, start), contacts, 2, 1e12);
        EXPECT_NEAR(frost::temperatureOf(s, h), equilibrium, 1e-5) << start;
    }
    // A thawing step from deep frost passes the plateau without leaving the contact range.
    const frost::Contact warm{20.0, 10.0};
    const double h = frost::advance(s, frost::enthalpyAt(s, -5.0), &warm, 1, 1e12);
    EXPECT_NEAR(frost::temperatureOf(s, h), 10.0, 1e-5);
    EXPECT_LE(frost::temperatureOf(s, h), 10.0);
    EXPECT_EQ(frost::frozenFraction(s, h), 0.0);
}

TEST(FrozenGroundKernel, ReceivedHeatSumsToTheEnthalpyChange) {
    const frost::Slab s;
    const frost::Contact contacts[3] = {{13.0, -8.0}, {0.6, 8.0}, {40.0, 0.0}};
    double h = frost::enthalpyAt(s, 4.0), total[3] = {0, 0, 0};
    const double start = h;
    for (int k = 0; k < 500; ++k) { // crosses the plateau into frost
        double received[3];
        h = frost::advance(s, h, contacts, 3, 900.0, received);
        for (int c = 0; c < 3; ++c)
            total[c] += received[c];
    }
    EXPECT_LT(frost::temperatureOf(s, h), 0.0);
    EXPECT_NEAR(h - start, total[0] + total[1] + total[2], 1e-9 * frost::latentStore(s));
}

TEST(FrozenGroundKernel, SnowLowersTheSurfaceConductance) {
    const frost::Slab s;
    const double bare = frost::halfSlabConductance(s);
    EXPECT_DOUBLE_EQ(frost::snowContactConductance(s, 0.0), bare);
    const double thin = frost::snowContactConductance(s, 0.01);
    const double deep = frost::snowContactConductance(s, 0.10);
    EXPECT_LT(thin, bare);
    EXPECT_LT(deep, thin);
    // 0.10 m of water at 250 kg/m3 is 0.40 m of snow: 1 / (D/2k + 0.40/0.25).
    EXPECT_NEAR(deep, 1.0 / (0.05 + 1.6), 1e-12);
}

TEST(FrozenGroundKernel, ValidationNamesTheBadValue) {
    frost::Slab s;
    EXPECT_TRUE(frost::validate(s).empty());
    s.thickness = 0.0;
    EXPECT_NE(frost::validate(s).find("thickness"), std::string::npos);
    s = {};
    s.deep_depth = 0.05;
    EXPECT_NE(frost::validate(s).find("deep depth"), std::string::npos);
    s = {};
    s.exponent = 0.5;
    EXPECT_NE(frost::validate(s).find("exponent"), std::string::npos);
    s = {};
    s.water_content = 1.5;
    EXPECT_NE(frost::validate(s).find("water content"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Decks
// ---------------------------------------------------------------------------

const fs::path kArtifactDir = "frozen_ground";

struct Deck {
    std::string infiltration = "HORTON";
    std::string infiltration_row = "S1  80  10  4  7  0";
    double air_start = -8.0, air_end = -8.0; // C, linear over the run
    std::string snowpack;                    // "SP1" assigns a pack to S1
    std::string frozen_ground;               // [FROZEN_GROUND] rows, empty = no section
    int hours = 12;
    bool report = false; // report every object to the binary output
};

std::string model(const Deck& d) {
    std::ostringstream s;
    char end[16];
    std::snprintf(end, sizeof end, "%02d:00:00", d.hours);
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "INFILTRATION         "
      << d.infiltration
      << "\n"
         "START_DATE           01/10/2026\n"
         "START_TIME           00:00:00\n"
         "REPORT_START_DATE    01/10/2026\n"
         "REPORT_START_TIME    00:00:00\n"
         "END_DATE             01/10/2026\n"
         "END_TIME             "
      << end
      << "\n"
         "REPORT_STEP          00:15:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         0:01:00\n"
         "\n[RAINGAGES]\n"
         "RG1  INTENSITY 1:00 1.0 TIMESERIES RAIN\n"
         "\n[TEMPERATURE]\n"
         "TIMESERIES AIR\n"
         "SNOWMELT 0 0.5 0.6 0 45 0\n"
         "\n[SUBCATCHMENTS]\n"
         ";;Name RainGage Outlet Area %Imperv Width Slope Curblen SnowPack\n"
         "S1  RG1  J1  2.0  0  100  1.0  0  "
      << d.snowpack
      << "\n"
         "\n[SUBAREAS]\n"
         "S1  0.015  0.2  1.0  2.0  0  OUTLET\n"
         "\n[INFILTRATION]\n"
      << d.infiltration_row
      << "\n"
         "\n[SNOWPACKS]\n"
         "SP1  PLOWABLE    0.001  0.001  0  0.10  0  0  0.0\n"
         "SP1  IMPERVIOUS  0.001  0.001  0  0.10  0  0  0.0\n"
         "SP1  PERVIOUS    0.001  0.001  0  0.10  0  0  0.0\n"
         "\n[TIMESERIES]\n"
         "RAIN  01/10/2026 00:00 0\n"
         "RAIN  01/10/2026 02:00 12\n"
         "RAIN  01/10/2026 06:00 0\n"
         "AIR   01/10/2026 00:00 "
      << d.air_start << "\nAIR   01/10/2026 " << end << " " << d.air_end
      << "\n"
         "\n[JUNCTIONS]\n"
         "J1  10.0  3.0  0.0  0.0  0.0\n"
         "\n[OUTFALLS]\n"
         "O1  9.0  FREE\n"
         "\n[CONDUITS]\n"
         "C1  J1  O1  100.0  0.013  0  0\n"
         "\n[XSECTIONS]\n"
         "C1  CIRCULAR  1.0  0  0  0  1\n";
    if (!d.frozen_ground.empty())
        s << "\n[FROZEN_GROUND]\n"
             ";;Subcatch Thickness Conductivity HeatCapacity WaterContent DeepTemp DeepDepth "
             "InitTemp Exponent SnowConductivity SnowDensity\n"
          << d.frozen_ground << "\n";
    if (d.report)
        s << "\n[REPORT]\nSUBCATCHMENTS ALL\nNODES ALL\nLINKS ALL\n";
    return s.str();
}

struct Result {
    bool opened = false, initialized = false;
    std::string error;
    double infiltration = 0, runoff = 0; // cumulative, ft3
    double frozen = 0, factor = 1, temperature = 0, enthalpy = 0, ledger = 0, water_j = 0;
    bool enabled = false;
    std::vector<double> frozen_history, infiltration_history, rain_history;
};

Result run(const std::string& tag, const Deck& deck) {
    Result r;
    fs::create_directories(kArtifactDir);
    const auto inp = kArtifactDir / (tag + ".inp");
    std::ofstream(inp, std::ios::trunc) << model(deck);
    SWMM_Engine engine = swmm_engine_create();
    const auto fail = [&] {
        r.error = swmm_get_last_error_msg(engine);
        swmm_engine_destroy(engine);
        return r;
    };
    if (swmm_engine_open(engine, inp.string().c_str(),
                         (kArtifactDir / (tag + ".rpt")).string().c_str(),
                         (kArtifactDir / (tag + ".out")).string().c_str(), nullptr) != SWMM_OK)
        return fail();
    r.opened = true;
    if (swmm_engine_initialize(engine) != SWMM_OK || swmm_engine_start(engine, 1) != SWMM_OK)
        return fail();
    r.initialized = true;
    auto& ctx = static_cast<SWMMEngine*>(engine)->context();
    const auto& f = ctx.frozen_ground;
    r.enabled = f.enabled();
    double elapsed = 0;
    for (;;) {
        if (swmm_engine_step(engine, &elapsed) != SWMM_OK)
            return fail();
        if (elapsed <= 0)
            break;
        r.infiltration_history.push_back(ctx.subcatches.stat_infil_vol[0]);
        r.rain_history.push_back(ctx.subcatches.rainfall[0]);
        if (r.enabled)
            r.frozen_history.push_back(f.frozen[0]);
    }
    r.infiltration = ctx.subcatches.stat_infil_vol[0];
    r.runoff = ctx.subcatches.stat_runoff_vol[0];
    if (r.enabled) {
        r.frozen = f.frozen[0];
        r.factor = f.factor[0];
        r.temperature = f.temperature[0];
        r.enthalpy = f.enthalpy[0];
        r.ledger = f.initial_j[0] + f.surface_j[0] + f.deep_j[0] + f.water_j[0];
        r.water_j = f.water_j[0];
    }
    swmm_engine_end(engine);
    swmm_engine_destroy(engine);
    return r;
}

// Frozen at the start (-4 C), cold air throughout.
const char* const kFrozenRow = "S1  0.15  1.5  2.4e6  0.30  8  3  -4  11  0.25  250";

TEST(FrozenGroundDeck, NoSectionLeavesTheModelOff) {
    const Result r = run("off", Deck{});
    ASSERT_TRUE(r.initialized) << r.error;
    EXPECT_FALSE(r.enabled);
    EXPECT_GT(r.infiltration, 0.0);
}

TEST(FrozenGroundDeck, FrozenGroundTakesNoWaterUnderEveryInfiltrationModel) {
    const struct {
        const char* name;
        const char* row;
    } models[] = {
        {"HORTON", "S1  80  10  4  7  0"},  {"MODIFIED_HORTON", "S1  80  10  4  7  0"},
        {"GREEN_AMPT", "S1  90  12  0.25"}, {"MODIFIED_GREEN_AMPT", "S1  90  12  0.25"},
        {"CURVE_NUMBER", "S1  70  0  7"},
    };
    for (const auto& m : models) {
        Deck open;
        open.infiltration = m.name;
        open.infiltration_row = m.row;
        Deck frozen = open;
        frozen.frozen_ground = kFrozenRow;
        const Result a = run(std::string("open_") + m.name, open);
        const Result b = run(std::string("frozen_") + m.name, frozen);
        ASSERT_TRUE(a.initialized) << m.name << ": " << a.error;
        ASSERT_TRUE(b.initialized) << m.name << ": " << b.error;
        EXPECT_GT(a.infiltration, 0.0) << m.name;
        EXPECT_EQ(b.infiltration, 0.0) << m.name;
        EXPECT_EQ(b.frozen, 1.0) << m.name;
        EXPECT_EQ(b.factor, 0.0) << m.name;
        EXPECT_LT(b.temperature, 0.0) << m.name;
        EXPECT_EQ(b.water_j, 0.0) << m.name;
        // Everything the open ground took now runs off or stays ponded.
        EXPECT_GT(b.runoff, a.runoff) << m.name;
    }
}

TEST(FrozenGroundDeck, GroundThatNeverFreezesIsBitIdenticalToNoSection) {
    Deck warm;
    warm.air_start = warm.air_end = 12.0;
    Deck with = warm;
    with.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  5";
    const Result a = run("warm_off", warm), b = run("warm_on", with);
    ASSERT_TRUE(a.initialized && b.initialized) << a.error << b.error;
    EXPECT_TRUE(b.enabled);
    EXPECT_EQ(b.frozen, 0.0);
    EXPECT_EQ(b.factor, 1.0);
    EXPECT_GT(b.temperature, 0.0);
    EXPECT_EQ(a.infiltration, b.infiltration);
    EXPECT_EQ(a.runoff, b.runoff);
    EXPECT_EQ(a.infiltration_history, b.infiltration_history);
}

TEST(FrozenGroundDeck, ThawRestoresInfiltration) {
    Deck d;
    d.air_start = d.air_end = 18.0;
    d.hours = 24;
    // Barely frozen at the start; the latent store takes about seven hours to melt, so the
    // ground is still mostly frozen when the rain starts at 02:00 and open by the end.
    d.frozen_ground = "S1  0.10  1.5  2.4e6  0.40  8  3  -0.5  11";
    const Result r = run("thaw", d);
    ASSERT_TRUE(r.initialized) << r.error;
    ASSERT_FALSE(r.frozen_history.empty());
    EXPECT_GT(r.frozen_history.front(), 0.9);
    EXPECT_EQ(r.frozen, 0.0);
    EXPECT_EQ(r.factor, 1.0);
    EXPECT_GT(r.infiltration, 0.0);
    // The frozen fraction only falls, and almost no water enters while the ground is still
    // half frozen: the factor there is 0.5^11.
    bool rained_on_frozen = false;
    for (std::size_t k = 1; k < r.frozen_history.size(); ++k) {
        EXPECT_LE(r.frozen_history[k], r.frozen_history[k - 1]) << k;
        if (r.frozen_history[k - 1] >= 0.5) {
            EXPECT_LT(r.infiltration_history[k], 1e-3 * r.infiltration) << k;
            rained_on_frozen |= r.rain_history[k] > 0.0;
        }
    }
    EXPECT_TRUE(rained_on_frozen);
    // The 18 C rain that infiltrated gave up heat to ground that sat at or above 0 C: more
    // than nothing, and no more than cooling all of it to 0 C.
    const double depth_m = r.infiltration * 0.3048 * 0.3048 * 0.3048 / 20000.0; // 2 ha
    const double ceiling = frost::kWaterCapacity * depth_m * 18.0;
    EXPECT_GT(r.water_j, 0.2 * ceiling);
    EXPECT_LE(r.water_j, ceiling);
}

// Two dry hours of cold air: the engine's slab is the kernel advanced once per five-minute
// runoff step against the air and the deep ground, and nothing else.
TEST(FrozenGroundDeck, TheSlabAdvancesOncePerRunoffStep) {
    Deck d;
    d.hours = 2;
    d.frozen_ground = "S1  0.15  1.5  2.4e6  0.30  8  3  0.2  11";
    const Result r = run("clock", d);
    ASSERT_TRUE(r.initialized) << r.error;
    frost::Slab s;
    s.init_temp = 0.2;
    const frost::Contact contacts[2] = {{frost::halfSlabConductance(s), -8.0},
                                        {frost::deepConductance(s), 8.0}};
    double h = frost::enthalpyAt(s, s.init_temp);
    for (int k = 0; k < 24; ++k)
        h = frost::advance(s, h, contacts, 2, 300.0);
    EXPECT_GT(frost::frozenFraction(s, h), 0.0);
    EXPECT_LT(frost::frozenFraction(s, h), 1.0);
    EXPECT_NEAR(r.enthalpy, h, 1e-9 * std::abs(h));
}

TEST(FrozenGroundDeck, TheHeatLedgerClosesOverARun) {
    Deck d;
    d.air_start = 6.0;
    d.air_end = -12.0; // unfrozen start, rain, then freeze-up
    d.hours = 24;
    d.frozen_ground = "S1  0.10  1.5  2.4e6  0.05  8  3  4  11";
    const Result r = run("ledger", d);
    ASSERT_TRUE(r.initialized) << r.error;
    EXPECT_GT(r.frozen, 0.0);
    EXPECT_NEAR(r.enthalpy, r.ledger, 1e-9 * std::abs(r.enthalpy));
}

TEST(FrozenGroundDeck, ASnowPackInsulatesTheGround) {
    // Same deck twice; the second gives the snow a very large conductivity, which removes
    // the pack's resistance and nothing else.
    Deck d;
    d.snowpack = "SP1";
    d.air_start = d.air_end = -10.0;
    d.hours = 24;
    d.frozen_ground = "S1  0.15  1.5  2.4e6  0.30  8  3  2  11  0.25  250";
    Deck bare = d;
    bare.frozen_ground = "S1  0.15  1.5  2.4e6  0.30  8  3  2  11  1e9  250";
    const Result insulated = run("snow_insulated", d), exposed = run("snow_exposed", bare);
    ASSERT_TRUE(insulated.initialized && exposed.initialized) << insulated.error << exposed.error;
    EXPECT_GT(exposed.frozen, 0.0);
    EXPECT_LT(insulated.frozen, exposed.frozen);
    EXPECT_GT(insulated.enthalpy, exposed.enthalpy);
}

TEST(FrozenGroundDeck, ANamedRowOverridesTheDefaultRowInEitherOrder) {
    for (const char* rows :
         {"*  0.15  1.5  2.4e6  0.30  8  3  5\nS1  0.40  1.5  2.4e6  0.30  8  3  -4",
          "S1  0.40  1.5  2.4e6  0.30  8  3  -4\n*  0.15  1.5  2.4e6  0.30  8  3  5"}) {
        Deck d;
        d.frozen_ground = rows;
        const Result r = run("override", d);
        ASSERT_TRUE(r.initialized) << r.error;
        EXPECT_EQ(r.frozen, 1.0);
        EXPECT_EQ(r.infiltration, 0.0);
    }
}

TEST(FrozenGroundDeck, BadRowsAreRefusedWithTheirReason) {
    Deck d;
    d.frozen_ground = "NOPE  0.15";
    Result r = run("bad_name", d);
    EXPECT_FALSE(r.initialized);
    EXPECT_NE(r.error.find("NOPE"), std::string::npos) << r.error;

    d.frozen_ground = "S1  -0.15";
    r = run("bad_thickness", d);
    EXPECT_FALSE(r.initialized);
    EXPECT_NE(r.error.find("thickness"), std::string::npos) << r.error;

    d.frozen_ground = "S1  abc";
    r = run("bad_number", d);
    EXPECT_FALSE(r.initialized);
}

// The saved input carries the section; values read back bit for bit.
TEST(FrozenGroundDeck, TheSectionSurvivesAnInputFileRoundTrip) {
    Deck d;
    d.frozen_ground = "*  0.2  1.3  2.1e6  0.33  7.5  2.5  1.25  9  0.3  300\n"
                      "S1  0.123456789012345  1.5  2.4e6  0.30  8  3  -4  11  0.25  250";
    fs::create_directories(kArtifactDir);
    const auto first = kArtifactDir / "roundtrip.inp",
               second = kArtifactDir / "roundtrip_saved.inp";
    std::ofstream(first, std::ios::trunc)
        << model(d) << "\n[2D_FROZEN_GROUND]\n*  0.11  1.7  2.2e6  0.21  6.5  4  -1.5  13\n";
    std::vector<std::pair<std::string, frost::Slab>> rows[2], mesh_rows[2];
    const fs::path* paths[2] = {&first, &second};
    for (int pass = 0; pass < 2; ++pass) {
        SWMM_Engine engine = swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(engine, paths[pass]->string().c_str(),
                                   (kArtifactDir / "roundtrip.rpt").string().c_str(),
                                   (kArtifactDir / "roundtrip.out").string().c_str(), nullptr),
                  SWMM_OK)
            << swmm_get_last_error_msg(engine);
        rows[pass] = static_cast<SWMMEngine*>(engine)->context().frozen_ground.rows;
        mesh_rows[pass] = static_cast<SWMMEngine*>(engine)->context().frozen_ground.mesh_rows;
        if (pass == 0)
            ASSERT_EQ(swmm_model_write(engine, second.string().c_str()), SWMM_OK);
        swmm_engine_destroy(engine);
    }
    ASSERT_EQ(rows[0].size(), 2u);
    ASSERT_EQ(rows[1].size(), 2u);
    for (int k = 0; k < 2; ++k) {
        EXPECT_EQ(rows[0][k].first, rows[1][k].first);
        const frost::Slab &a = rows[0][k].second, &b = rows[1][k].second;
        EXPECT_EQ(a.thickness, b.thickness);
        EXPECT_EQ(a.conductivity, b.conductivity);
        EXPECT_EQ(a.heat_capacity, b.heat_capacity);
        EXPECT_EQ(a.water_content, b.water_content);
        EXPECT_EQ(a.deep_temp, b.deep_temp);
        EXPECT_EQ(a.deep_depth, b.deep_depth);
        EXPECT_EQ(a.init_temp, b.init_temp);
        EXPECT_EQ(a.exponent, b.exponent);
        EXPECT_EQ(a.snow_conductivity, b.snow_conductivity);
        EXPECT_EQ(a.snow_density, b.snow_density);
    }
    EXPECT_EQ(rows[0][1].second.thickness, 0.123456789012345);
    for (int pass = 0; pass < 2; ++pass) {
        ASSERT_EQ(mesh_rows[pass].size(), 1u);
        EXPECT_EQ(mesh_rows[pass][0].first, "*");
        EXPECT_EQ(mesh_rows[pass][0].second.thickness, 0.11);
        EXPECT_EQ(mesh_rows[pass][0].second.init_temp, -1.5);
        EXPECT_EQ(mesh_rows[pass][0].second.exponent, 13.0);
        EXPECT_EQ(mesh_rows[pass][0].second.snow_density, 250.0);
    }
}

// A checkpoint taken while the ground is freezing restores the slab: the restored engine
// finishes the run bit-identically to the one that never stopped.
TEST(FrozenGroundDeck, ACheckpointMidFreezeContinuesBitIdentically) {
    Deck d;
    d.air_start = 6.0;
    d.air_end = -12.0;
    d.hours = 24;
    d.frozen_ground = "S1  0.10  1.5  2.4e6  0.05  8  3  4  11";
    fs::create_directories(kArtifactDir);
    const auto inp = kArtifactDir / "checkpoint.inp";
    std::ofstream(inp, std::ios::trunc) << model(d);
    SWMMEngine a, b;
    for (SWMMEngine* e : {&a, &b}) {
        const std::string tag = e == &a ? "checkpoint_a" : "checkpoint_b";
        ASSERT_EQ(e->open(inp.string().c_str(), (kArtifactDir / (tag + ".rpt")).string().c_str(),
                          nullptr),
                  SWMM_OK);
        ASSERT_EQ(e->initialize(), SWMM_OK);
        ASSERT_EQ(e->start(0), SWMM_OK);
        const std::string error = e->enableCheckpointing();
        ASSERT_TRUE(error.empty()) << error;
    }
    double elapsed = 0;
    const auto& fa = a.context().frozen_ground;
    const auto& fb = b.context().frozen_ground;
    // Run the source until the slab is partly frozen.
    int guard = 0;
    while (fa.frozen[0] < 0.3) {
        ASSERT_EQ(a.step(&elapsed), SWMM_OK);
        ASSERT_GT(elapsed, 0.0);
        ASSERT_LT(++guard, 100000);
    }
    ASSERT_LT(fa.frozen[0], 1.0);
    ASSERT_NE(fa.enthalpy[0], fb.enthalpy[0]);
    const auto path = kArtifactDir / "checkpoint.bin";
    ASSERT_EQ(a.saveCheckpoint(path.string()), "");
    ASSERT_EQ(b.loadCheckpoint(path.string()), "");
    EXPECT_EQ(fa.enthalpy[0], fb.enthalpy[0]);
    EXPECT_EQ(fa.frozen[0], fb.frozen[0]);
    EXPECT_EQ(fa.factor[0], fb.factor[0]);
    for (;;) {
        double ea = 0, eb = 0;
        ASSERT_EQ(a.step(&ea), SWMM_OK);
        ASSERT_EQ(b.step(&eb), SWMM_OK);
        ASSERT_EQ(ea, eb);
        ASSERT_EQ(fa.enthalpy[0], fb.enthalpy[0]);
        if (ea <= 0)
            break;
    }
    EXPECT_EQ(fa.frozen[0], 1.0);
    EXPECT_EQ(fa.surface_j[0], fb.surface_j[0]);
    EXPECT_EQ(fa.deep_j[0], fb.deep_j[0]);
    EXPECT_EQ(fa.water_j[0], fb.water_j[0]);
    EXPECT_EQ(a.context().subcatches.stat_infil_vol[0], b.context().subcatches.stat_infil_vol[0]);
    EXPECT_EQ(a.context().subcatches.stat_runoff_vol[0], b.context().subcatches.stat_runoff_vol[0]);
}

// The public getter reports the slab, and "no row" as inactive with a factor of one.
TEST(FrozenGroundDeck, TheGetterReportsTheSlabState) {
    for (const bool with : {false, true}) {
        Deck d;
        if (with)
            d.frozen_ground = kFrozenRow;
        fs::create_directories(kArtifactDir);
        const auto inp = kArtifactDir / "getter.inp";
        std::ofstream(inp, std::ios::trunc) << model(d);
        SWMM_Engine engine = swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(engine, inp.string().c_str(),
                                   (kArtifactDir / "getter.rpt").string().c_str(),
                                   (kArtifactDir / "getter.out").string().c_str(), nullptr),
                  SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
        int active = -1;
        double t = 99, frozen = 99, factor = 99;
        ASSERT_EQ(swmm_subcatch_get_frozen_ground(engine, 0, &active, &t, &frozen, &factor),
                  SWMM_OK);
        EXPECT_EQ(active, with ? 1 : 0);
        EXPECT_EQ(t, with ? -4.0 : 0.0);
        EXPECT_EQ(frozen, with ? 1.0 : 0.0);
        EXPECT_EQ(factor, with ? 0.0 : 1.0);
        EXPECT_EQ(swmm_subcatch_get_frozen_ground(engine, 0, nullptr, nullptr, nullptr, nullptr),
                  SWMM_OK);
        EXPECT_NE(swmm_subcatch_get_frozen_ground(engine, 7, &active, &t, &frozen, &factor),
                  SWMM_OK);
        swmm_engine_destroy(engine);
    }
}

// Rows can be read, replaced, added and removed through the public API.
// The binary output carries frozen fraction and slab temperature as extension variables; a model
// without the section has neither and keeps its layout.
TEST(FrozenGroundDeck, TheBinaryOutputCarriesTheSlabState) {
    Deck d, plain_deck;
    d.report = plain_deck.report = true;
    d.frozen_ground = kFrozenRow;
    const Result cold = run("out_frozen", d), open = run("out_open", plain_deck);
    ASSERT_TRUE(cold.initialized && open.initialized) << cold.error << open.error;
    SWMM_Output plain = swmm_output_open((kArtifactDir / "out_open.out").string().c_str());
    ASSERT_NE(plain, nullptr);
    EXPECT_EQ(swmm_output_has_subcatch_variable(plain, SWMM_OUT_SUBCATCH_FROZEN_FRACTION), 0);
    EXPECT_EQ(swmm_output_has_subcatch_variable(plain, SWMM_OUT_SUBCATCH_FROZEN_GROUND_TEMP), 0);
    const int periods = swmm_output_get_period_count(plain);
    ASSERT_EQ(swmm_output_get_subcatch_count(plain), 1);
    swmm_output_close(plain);

    SWMM_Output out = swmm_output_open((kArtifactDir / "out_frozen.out").string().c_str());
    ASSERT_NE(out, nullptr);
    EXPECT_EQ(swmm_output_has_subcatch_variable(out, SWMM_OUT_SUBCATCH_FROZEN_FRACTION), 1);
    EXPECT_EQ(swmm_output_has_subcatch_variable(out, SWMM_OUT_SUBCATCH_FROZEN_GROUND_TEMP), 1);
    ASSERT_EQ(swmm_output_get_period_count(out), periods);
    ASSERT_EQ(swmm_output_get_subcatch_count(out), 1);
    ASSERT_GT(periods, 1);
    float frozen = -99, temperature = -99, rain = -99;
    ASSERT_EQ(swmm_output_get_subcatch_result(out, periods - 1, SWMM_OUT_SUBCATCH_FROZEN_FRACTION,
                                              &frozen),
              0);
    ASSERT_EQ(swmm_output_get_subcatch_result(out, periods - 1,
                                              SWMM_OUT_SUBCATCH_FROZEN_GROUND_TEMP, &temperature),
              0);
    EXPECT_EQ(frozen, static_cast<float>(cold.frozen));
    EXPECT_EQ(temperature, static_cast<float>(cold.temperature));
    EXPECT_EQ(frozen, 1.0f);
    EXPECT_LT(temperature, 0.0f);
    // The standard columns are still where readers expect them: it rains at 02:30.
    ASSERT_EQ(swmm_output_get_subcatch_result(out, 9, SWMM_OUT_SUBCATCH_RAINFALL, &rain), 0);
    EXPECT_GT(rain, 0.0f);
    swmm_output_close(out);
}

TEST(FrozenGroundDeck, TheRowsCanBeAuthoredThroughTheApi) {
    Deck d;
    d.frozen_ground = "*  0.2  1.3";
    fs::create_directories(kArtifactDir);
    const auto inp = kArtifactDir / "authoring.inp";
    std::ofstream(inp, std::ios::trunc) << model(d);
    SWMM_Engine engine = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(engine, inp.string().c_str(),
                               (kArtifactDir / "authoring.rpt").string().c_str(),
                               (kArtifactDir / "authoring.out").string().c_str(), nullptr),
              SWMM_OK);
    int count = -1;
    ASSERT_EQ(swmm_frozen_ground_count(engine, 0, &count), SWMM_OK);
    EXPECT_EQ(count, 1);
    char name[32];
    double v[10];
    ASSERT_EQ(swmm_frozen_ground_get(engine, 0, 0, name, sizeof name, v), SWMM_OK);
    EXPECT_STREQ(name, "*");
    EXPECT_EQ(v[0], 0.2);
    EXPECT_EQ(v[1], 1.3);
    EXPECT_EQ(v[2], 2.4e6); // defaults fill the omitted columns
    EXPECT_EQ(v[9], 250.0);
    v[6] = -4.0;
    ASSERT_EQ(swmm_frozen_ground_set(engine, 0, "S1", v), SWMM_OK); // add
    v[0] = 0.25;
    ASSERT_EQ(swmm_frozen_ground_set(engine, 0, "S1", v), SWMM_OK); // replace
    ASSERT_EQ(swmm_frozen_ground_count(engine, 0, &count), SWMM_OK);
    EXPECT_EQ(count, 2);
    double back[10];
    ASSERT_EQ(swmm_frozen_ground_get(engine, 0, 1, name, sizeof name, back), SWMM_OK);
    EXPECT_STREQ(name, "S1");
    EXPECT_EQ(back[0], 0.25);
    EXPECT_EQ(back[6], -4.0);
    v[0] = -1.0;
    EXPECT_EQ(swmm_frozen_ground_set(engine, 0, "S1", v), SWMM_ERR_BADPARAM);
    v[0] = 0.25;
    EXPECT_EQ(swmm_frozen_ground_set(engine, 1, "", v), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_frozen_ground_set(engine, 1, "*", v), SWMM_OK);
    ASSERT_EQ(swmm_frozen_ground_count(engine, 1, &count), SWMM_OK);
    EXPECT_EQ(count, 1);
    EXPECT_EQ(swmm_frozen_ground_remove(engine, 1, "*"), SWMM_OK);
    EXPECT_EQ(swmm_frozen_ground_remove(engine, 1, "*"), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_frozen_ground_count(engine, 2, &count), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_frozen_ground_get(engine, 0, 5, name, sizeof name, v), SWMM_ERR_BADINDEX);
    // The authored row takes effect at initialization: S1 starts frozen.
    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
    int active = 0;
    double frozen = 0;
    ASSERT_EQ(swmm_subcatch_get_frozen_ground(engine, 0, &active, nullptr, &frozen, nullptr),
              SWMM_OK);
    EXPECT_EQ(active, 1);
    EXPECT_EQ(frozen, 1.0);
    swmm_engine_destroy(engine);
}

// ---------------------------------------------------------------------------
// Mesh cells ([2D_FROZEN_GROUND] with the per-cell infiltration models)
// ---------------------------------------------------------------------------

struct MeshDeck {
    double air = -8.0;         // C, constant
    std::string frozen_ground; // [2D_FROZEN_GROUND] rows, empty = no section
    int hours = 2;
    double rain_hours = 1.0; // 20 mm/h from the start
    bool snow = false;       // a degree-day pack with 100 mm of water on every cell
    bool results = false;    // write <tag>.2d.h5 beside the other files
};

std::string meshModel(const MeshDeck& d) {
    std::ostringstream s;
    char end[16];
    std::snprintf(end, sizeof end, "%02d:00:00", d.hours);
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/10/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/10/2026\n"
         "END_TIME             "
      << end
      << "\n"
         "REPORT_STEP          00:05:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         10\n"
         "ALLOW_PONDING        NO\n"
         "\n[TEMPERATURE]\n"
         "TIMESERIES AIR\n"
         "\n[TIMESERIES]\n"
         "AIR   01/10/2026 00:00 "
      << d.air << "\nAIR   01/10/2026 " << end << " " << d.air
      << "\n"
         "\n[JUNCTIONS]\n"
         "J1      0.0   1.0       0          0         0\n"
         "\n[OUTFALLS]\n"
         "O1     -0.5    FREE  NO\n"
         "\n[CONDUITS]\n"
         "C1      J1    O1  30.0    0.013      0         0          0\n"
         "\n[XSECTIONS]\n"
         "C1      CIRCULAR  0.3    0      0      0      1\n"
         "\n[2D_OPTIONS]\n"
         "MAX_TIMESTEP     2\n"
         "DRY_DEPTH        0.001\n"
         "REPORT_2D        NO\n"
         "\n[2D_VERTICES]\n"
         " 0.0    0.0   10.0\n"
         "20.0    0.0   10.0\n"
         "20.0   20.0   10.0\n"
         " 0.0   20.0   10.0\n"
         "10.0   10.0   10.0\n"
         "\n[2D_TRIANGLES]\n"
         "0     1   4   0.03        0.0         LAWN\n"
         "1     2   4   0.03        0.0         WOODS\n"
         "2     3   4   0.03        0.0         LAWN\n"
         "3     0   4   0.03        0.0         WOODS\n"
         "\n[2D_INFILTRATION_DEFAULTS]\n"
         "*       CONSTANT   10.0   -     -     -    -    LOST\n";
    if (d.snow)
        s << "\n[SNOWPACKS]\nSP PERVIOUS 0.001 0.001 0 0.1 100 0 0\n"
             "\n[2D_SNOW_DEFAULTS]\n* SNOWPACK SP\n";
    if (!d.frozen_ground.empty())
        s << "\n[2D_FROZEN_GROUND]\n" << d.frozen_ground << "\n";
    return s.str();
}

struct MeshResult {
    bool started = false;
    std::string error;
    std::vector<double> infiltration; // cumulative depth per cell, m
    std::vector<double> factor, enthalpy;
    std::vector<double> first_factor; // after the first engine step
    std::vector<double> api_temperature, api_frozen, api_factor; // the public bulk getter
    int api_status = -1;
    frost::Slab slab;
};

MeshResult runMesh(const std::string& tag, const MeshDeck& deck) {
    MeshResult r;
    fs::create_directories(kArtifactDir);
    const auto inp = kArtifactDir / (tag + ".inp");
    std::ofstream(inp, std::ios::trunc) << meshModel(deck);
    SWMM_Engine engine = swmm_engine_create();
    const auto fail = [&] {
        r.error = swmm_get_last_error_msg(engine);
        swmm_engine_destroy(engine);
        return r;
    };
    if (swmm_engine_open(engine, inp.string().c_str(),
                         (kArtifactDir / (tag + ".rpt")).string().c_str(),
                         (kArtifactDir / (tag + ".out")).string().c_str(), nullptr) != SWMM_OK)
        return fail();
    if (deck.results) {
        const auto h5 = fs::absolute(kArtifactDir / (tag + ".2d.h5"));
        fs::remove(h5);
        if (swmm_2d_set_results_file(engine, h5.string().c_str()) != SWMM_OK)
            return fail();
    }
    if (swmm_engine_initialize(engine) != SWMM_OK || swmm_engine_start(engine, 1) != SWMM_OK)
        return fail();
    r.started = true;
    const auto& router = static_cast<SWMMEngine*>(engine)->surfaceRouter2D();
    int cells = 0;
    swmm_2d_triangle_count(engine, &cells);
    double elapsed = 0;
    for (;;) {
        const bool raining = elapsed * 24.0 < deck.rain_hours;
        swmm_2d_force_rainfall_uniform(engine, raining ? 0.020 / 3600.0 : 0.0,
                                       SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        if (swmm_engine_step(engine, &elapsed) != SWMM_OK)
            return fail();
        if (r.first_factor.empty())
            r.first_factor = router.frozenGroundFactor();
        if (elapsed <= 0)
            break;
    }
    r.infiltration.assign(static_cast<std::size_t>(cells), 0.0);
    swmm_infil2d_get_cum_bulk(engine, r.infiltration.data(), cells);
    r.api_temperature.assign(static_cast<std::size_t>(cells), -99.0);
    r.api_frozen = r.api_factor = r.api_temperature;
    r.api_status = swmm_infil2d_get_frozen_ground_bulk(
        engine, r.api_temperature.data(), r.api_frozen.data(), r.api_factor.data(), cells);
    r.factor = router.frozenGroundFactor();
    r.enthalpy = router.frozenGroundEnthalpy();
    if (const frost::Slab* slab = router.frozenGroundSlab(0))
        r.slab = *slab;
    swmm_engine_end(engine);
    swmm_engine_destroy(engine);
    return r;
}

TEST(FrozenGroundMesh, FrozenCellsTakeNoWater) {
    MeshDeck frozen;
    frozen.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  -4  11";
    const MeshResult open = runMesh("mesh_open", MeshDeck{}), cold = runMesh("mesh_frozen", frozen);
    ASSERT_TRUE(open.started) << open.error;
    ASSERT_TRUE(cold.started) << cold.error;
    EXPECT_TRUE(open.factor.empty());
    ASSERT_EQ(open.infiltration.size(), 4u);
    ASSERT_EQ(cold.factor.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_GT(open.infiltration[i], 0.0) << i;
        EXPECT_EQ(cold.infiltration[i], 0.0) << i;
        EXPECT_EQ(cold.factor[i], 0.0) << i;
    }
}

TEST(FrozenGroundMesh, CellsThatNeverFreezeAreBitIdenticalToNoSection) {
    MeshDeck warm;
    warm.air = 12.0;
    MeshDeck with = warm;
    with.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  5";
    const MeshResult a = runMesh("mesh_warm_off", warm), b = runMesh("mesh_warm_on", with);
    ASSERT_TRUE(a.started && b.started) << a.error << b.error;
    ASSERT_EQ(b.factor.size(), 4u);
    for (double f : b.factor)
        EXPECT_EQ(f, 1.0);
    EXPECT_GT(a.infiltration[0], 0.0);
    EXPECT_EQ(a.infiltration, b.infiltration);
}

TEST(FrozenGroundMesh, ThawOpensTheCells) {
    MeshDeck d;
    d.air = 18.0;
    d.hours = 12;
    d.rain_hours = 12.0;
    d.frozen_ground = "*  0.10  1.5  2.4e6  0.40  8  3  -0.5  11";
    MeshDeck open = d;
    open.frozen_ground.clear();
    const MeshResult r = runMesh("mesh_thaw", d), control = runMesh("mesh_thaw_open", open);
    ASSERT_TRUE(r.started && control.started) << r.error << control.error;
    ASSERT_EQ(r.factor.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_LT(r.first_factor[i], 1e-3) << i; // still frozen after the first step
        EXPECT_EQ(r.factor[i], 1.0) << i;        // open at the end
        EXPECT_GT(r.infiltration[i], 0.0) << i;
        EXPECT_LT(r.infiltration[i], 0.8 * control.infiltration[i]) << i;
    }
}

// Two dry, cold hours: each cell's slab is the kernel advanced once per five-minute
// infiltration interval (23 or 24 intervals complete, depending on the last partial one).
TEST(FrozenGroundMesh, TheSlabAdvancesOncePerInfiltrationInterval) {
    MeshDeck d;
    d.rain_hours = 0.0;
    d.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  0.2  11";
    const MeshResult r = runMesh("mesh_clock", d);
    ASSERT_TRUE(r.started) << r.error;
    const frost::Contact contacts[2] = {{frost::halfSlabConductance(r.slab), -8.0},
                                        {frost::deepConductance(r.slab), 8.0}};
    double h = frost::enthalpyAt(r.slab, r.slab.init_temp), h23 = 0;
    for (int k = 1; k <= 24; ++k) {
        h = frost::advance(r.slab, h, contacts, 2, 300.0);
        if (k == 23)
            h23 = h;
    }
    ASSERT_LT(h, h23);
    ASSERT_EQ(r.enthalpy.size(), 4u);
    for (double cell : r.enthalpy) {
        EXPECT_LE(cell, h23 + 1e-9 * std::abs(h23));
        EXPECT_GE(cell, h - 1e-9 * std::abs(h));
    }
}

// A tag row wins over the default row in either order; cells 1 and 3 carry the WOODS tag.
TEST(FrozenGroundMesh, ATagRowOverridesTheDefaultRow) {
    const MeshResult open = runMesh("mesh_tag_open", MeshDeck{});
    ASSERT_TRUE(open.started) << open.error;
    for (const char* rows :
         {"*      0.15  1.5  2.4e6  0.30  8  3  5\nWOODS  0.15  1.5  2.4e6  0.30  8  3  -4",
          "WOODS  0.15  1.5  2.4e6  0.30  8  3  -4\n*      0.15  1.5  2.4e6  0.30  8  3  5"}) {
        MeshDeck d;
        d.air = 3.0; // the default-row cells stay open, the frozen ones do not thaw in two hours
        d.frozen_ground = rows;
        MeshDeck control = d;
        control.frozen_ground.clear();
        const MeshResult r = runMesh("mesh_tag", d), c = runMesh("mesh_tag_control", control);
        ASSERT_TRUE(r.started && c.started) << r.error << c.error;
        ASSERT_EQ(r.factor.size(), 4u);
        for (std::size_t i = 0; i < 4; ++i) {
            const bool woods = i % 2 == 1;
            EXPECT_EQ(r.first_factor[i], woods ? 0.0 : 1.0) << i;
            if (woods)
                EXPECT_LT(r.infiltration[i], 0.05 * c.infiltration[i]) << i;
            else
                EXPECT_GT(r.infiltration[i], 0.0) << i;
        }
    }
}

// The 2D results file carries the slab state per cell and report time; a cell no row covers is
// NaN, and a model without the section has neither dataset.
TEST(FrozenGroundMesh, TheResultsFileCarriesTheSlabState) {
    if (!swmm_output2d_available())
        GTEST_SKIP() << "2D results reader not built";
    MeshDeck d;
    d.results = true;
    d.frozen_ground = "WOODS  0.15  1.5  2.4e6  0.30  8  3  -4";
    MeshDeck plain;
    plain.results = true;
    const MeshResult r = runMesh("mesh_results", d), c = runMesh("mesh_results_plain", plain);
    ASSERT_TRUE(r.started && c.started) << r.error << c.error;
    const auto read = [](const std::string& tag, const char* name, std::vector<double>& last) {
        SWMM_Output2D out = nullptr;
        if (swmm_output2d_open((kArtifactDir / (tag + ".2d.h5")).string().c_str(), &out) != SWMM_OK)
            return -1;
        int rank = 0, integer = 0;
        uint64_t dims[4] = {}, unlimited = 0;
        int steps = 0;
        if (swmm_output2d_shape(out, name, &rank, dims, &integer, &unlimited) == SWMM_OK &&
            rank == 2 && dims[0] > 0) {
            steps = static_cast<int>(dims[0]);
            last.assign(dims[1], -99.0);
            const uint64_t start[2] = {dims[0] - 1, 0}, count[2] = {1, dims[1]};
            const uint64_t stride[2] = {1, 1};
            if (swmm_output2d_read_double(out, name, start, count, stride, last.data(),
                                          last.size()) != SWMM_OK)
                steps = -1;
        }
        swmm_output2d_close(out);
        return steps;
    };
    std::vector<double> frozen, temperature, none;
    ASSERT_GT(read("mesh_results", "Mesh2_face_frozen_fraction", frozen), 1);
    ASSERT_GT(read("mesh_results", "Mesh2_face_frozen_ground_temperature", temperature), 1);
    ASSERT_EQ(frozen.size(), 4u);
    ASSERT_EQ(temperature.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        if (i % 2 == 1) { // WOODS
            // The file holds single precision.
            EXPECT_EQ(static_cast<float>(frozen[i]), static_cast<float>(r.api_frozen[i])) << i;
            EXPECT_EQ(static_cast<float>(temperature[i]), static_cast<float>(r.api_temperature[i]))
                << i;
            EXPECT_EQ(frozen[i], 1.0) << i;
            EXPECT_LT(temperature[i], 0.0) << i;
        } else {
            EXPECT_TRUE(std::isnan(frozen[i])) << i;
            EXPECT_TRUE(std::isnan(temperature[i])) << i;
        }
    }
    EXPECT_EQ(read("mesh_results_plain", "Mesh2_face_frozen_fraction", none), 0);
    EXPECT_EQ(read("mesh_results_plain", "Mesh2_face_frozen_ground_temperature", none), 0);
    // The plain model still has its infiltration datasets.
    EXPECT_GT(read("mesh_results_plain", "Mesh2_face_infil_rate", none), 1);
}

// The memory of a per-cell infiltration method (Horton's decay, Green-Ampt's wetting front) is in
// no surface archive. That is safe only because a mesh whose cells keep their own method cannot
// be checkpointed: the session is refused with its reason, with or without frozen ground.
TEST(FrozenGroundMesh, AMeshWithItsOwnCellMethodsCannotStartACheckpointSession) {
    for (const bool frozen : {false, true}) {
        MeshDeck d;
        if (frozen)
            d.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  -4  11";
        fs::create_directories(kArtifactDir);
        const std::string tag = frozen ? "mesh_session_frozen" : "mesh_session_plain";
        const auto inp = kArtifactDir / (tag + ".inp");
        std::ofstream(inp, std::ios::trunc) << meshModel(d);
        SWMMEngine e;
        ASSERT_EQ(
            e.open(inp.string().c_str(), (kArtifactDir / (tag + ".rpt")).string().c_str(), nullptr),
            SWMM_OK);
        const std::string error =
            e.startCheckpointSession(SWMM_CHECKPOINT_OUTPUT_NONE, "", false, "");
        EXPECT_NE(error.find("aquifer"), std::string::npos) << tag << ": " << error;
    }
}

// With only a tag row, the other cells have no slab and behave as if the section were absent.
TEST(FrozenGroundMesh, CellsWithoutARowAreNotLimited) {
    MeshDeck d;
    d.frozen_ground = "WOODS  0.15  1.5  2.4e6  0.30  8  3  -4";
    MeshDeck control = d;
    control.frozen_ground.clear();
    const MeshResult r = runMesh("mesh_partial", d), c = runMesh("mesh_partial_control", control);
    ASSERT_TRUE(r.started && c.started) << r.error << c.error;
    ASSERT_EQ(r.factor.size(), 4u);
    ASSERT_EQ(r.api_status, SWMM_OK);
    ASSERT_EQ(c.api_status, SWMM_OK);
    for (std::size_t i = 0; i < 4; ++i) {
        const bool woods = i % 2 == 1;
        EXPECT_EQ(r.factor[i], woods ? 0.0 : 1.0) << i;
        EXPECT_EQ(r.enthalpy[i] == 0.0, !woods) << i;
        // The public getter reports the slab, and 0, 0, 1 where there is none.
        EXPECT_EQ(r.api_factor[i], r.factor[i]) << i;
        EXPECT_EQ(r.api_frozen[i], woods ? 1.0 : 0.0) << i;
        if (woods)
            EXPECT_LT(r.api_temperature[i], 0.0) << i;
        else
            EXPECT_EQ(r.api_temperature[i], 0.0) << i;
        EXPECT_EQ(c.api_factor[i], 1.0) << i;
        EXPECT_EQ(c.api_frozen[i], 0.0) << i;
        if (woods)
            EXPECT_EQ(r.infiltration[i], 0.0) << i;
        else
            EXPECT_GT(r.infiltration[i], 0.0) << i;
    }
}

// Same deck twice; the second gives the snow a very large conductivity, which removes the
// pack's resistance and nothing else.
TEST(FrozenGroundMesh, ASnowPackInsulatesTheCells) {
    MeshDeck d;
    d.snow = true;
    d.air = -10.0;
    d.hours = 12;
    d.rain_hours = 0.0;
    d.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  2  11  0.25  250";
    MeshDeck bare = d;
    bare.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  2  11  1e9  250";
    const MeshResult insulated = runMesh("mesh_snow_insulated", d),
                     exposed = runMesh("mesh_snow_exposed", bare);
    ASSERT_TRUE(insulated.started && exposed.started) << insulated.error << exposed.error;
    ASSERT_EQ(insulated.enthalpy.size(), 4u);
    for (std::size_t i = 0; i < 4; ++i) {
        EXPECT_LT(exposed.factor[i], 1.0) << i; // the exposed ground starts to freeze
        EXPECT_GT(insulated.enthalpy[i], exposed.enthalpy[i]) << i;
        EXPECT_GT(insulated.factor[i], exposed.factor[i]) << i;
    }
}

TEST(FrozenGroundMesh, BadRowsAreRefused) {
    MeshDeck d;
    d.frozen_ground = "NO_SUCH_TAG  0.15";
    MeshResult r = runMesh("mesh_bad_name", d);
    EXPECT_FALSE(r.started);
    EXPECT_NE(r.error.find("NO_SUCH_TAG"), std::string::npos) << r.error;
    d.frozen_ground = "*  0.15  -1";
    r = runMesh("mesh_bad_value", d);
    EXPECT_FALSE(r.started);
}

// ---------------------------------------------------------------------------
// Mesh cells over a 2D aquifer: the same factor limits the aquifer's intake
// ---------------------------------------------------------------------------

struct AquiferDeck {
    std::string closure = "CLOSED_FORM";
    bool cell_method = true;   // also author a per-cell CONSTANT method
    double air = -8.0;         // C
    std::string frozen_ground; // [2D_FROZEN_GROUND] rows
    bool second_order = false; // FULL_SWE with RECONSTRUCTION_ORDER 2
};

std::string aquiferModel(const AquiferDeck& d) {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/10/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/10/2026\nEND_TIME             00:30:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[TEMPERATURE]\nTIMESERIES AIR\n\n"
         "[TIMESERIES]\nAIR 01/10/2026 00:00 "
      << d.air << "\nAIR 01/10/2026 01:00 " << d.air
      << "\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nMOMENTUM_EQUATION "
      << (d.second_order ? "FULL_SWE\nRECONSTRUCTION_ORDER 2" : "LOCAL_INERTIAL")
      << "\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\n"
         "MAX_TIMESTEP 5\nDRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
         "\n[2D_VERTICES]\n"
         "0.0 0.0 -10.0\n10.0 0.0 -10.0\n10.0 10.0 -10.0\n0.0 10.0 -10.0\n\n"
         "[2D_TRIANGLES]\n0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n";
    if (d.cell_method)
        m << "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50  -  -  -  -\n\n";
    m << "[2D_AQUIFER_OPTIONS]\nCLOSURE " << d.closure
      << "\n\n[2D_AQUIFER]\n*  36.0  4.0  0.45  0.10  2.0  HG0 1.0\n\n";
    if (!d.frozen_ground.empty())
        m << "[2D_FROZEN_GROUND]\n" << d.frozen_ground << "\n\n";
    m << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct AquiferResult {
    bool started = false;
    std::string error;
    double infil_in = 0, storage = 0, continuity = 0; // aquifer ledger, m3
    std::vector<double> depth, factor;
};

AquiferResult runAquifer(const std::string& tag, const AquiferDeck& deck) {
    AquiferResult r;
    fs::create_directories(kArtifactDir);
    const auto inp = kArtifactDir / (tag + ".inp");
    std::ofstream(inp, std::ios::trunc) << aquiferModel(deck);
    SWMM_Engine engine = swmm_engine_create();
    const auto fail = [&] {
        r.error = swmm_get_last_error_msg(engine);
        swmm_engine_destroy(engine);
        return r;
    };
    if (swmm_engine_open(engine, inp.string().c_str(),
                         (kArtifactDir / (tag + ".rpt")).string().c_str(),
                         (kArtifactDir / (tag + ".out")).string().c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(engine) != SWMM_OK || swmm_engine_start(engine, 1) != SWMM_OK)
        return fail();
    r.started = true;
    double elapsed = 0;
    for (;;) {
        if (swmm_engine_step(engine, &elapsed) != SWMM_OK)
            return fail();
        if (elapsed <= 0)
            break;
    }
    swmm_gw2d_get_ledger(engine, SWMM_GW2D_LED_INFIL_IN, &r.infil_in);
    swmm_gw2d_get_ledger(engine, SWMM_GW2D_LED_STORAGE, &r.storage);
    swmm_gw2d_get_continuity_error(engine, &r.continuity);
    r.depth.assign(2, 0.0);
    swmm_2d_get_depths_bulk(engine, r.depth.data());
    r.factor = static_cast<SWMMEngine*>(engine)->surfaceRouter2D().frozenGroundFactor();
    swmm_engine_end(engine);
    swmm_engine_destroy(engine);
    return r;
}

TEST(FrozenGroundAquifer, FrozenCellsGiveTheAquiferNothingUnderEveryClosure) {
    for (const char* closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"})
        for (const bool cell_method : {true, false}) {
            const std::string tag = std::string(closure) + (cell_method ? "_method" : "_owned");
            AquiferDeck open;
            open.closure = closure;
            open.cell_method = cell_method;
            AquiferDeck frozen = open;
            frozen.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  -4  11";
            const AquiferResult a = runAquifer("aquifer_open_" + tag, open);
            const AquiferResult b = runAquifer("aquifer_frozen_" + tag, frozen);
            ASSERT_TRUE(a.started) << tag << ": " << a.error;
            ASSERT_TRUE(b.started) << tag << ": " << b.error;
            EXPECT_GT(a.infil_in, 0.0) << tag;
            EXPECT_EQ(b.infil_in, 0.0) << tag;
            ASSERT_EQ(b.factor.size(), 2u) << tag;
            EXPECT_EQ(b.factor[0], 0.0) << tag;
            // The ponded water the open ground took is still on the frozen surface.
            EXPECT_GT(b.depth[0], a.depth[0]) << tag;
            EXPECT_LT(std::abs(b.continuity), 1e-9) << tag;
        }
}

TEST(FrozenGroundAquifer, GroundThatNeverFreezesIsBitIdenticalToNoSection) {
    for (const char* closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"}) {
        AquiferDeck warm;
        warm.closure = closure;
        warm.cell_method = false;
        warm.air = 12.0;
        AquiferDeck with = warm;
        with.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  5";
        const AquiferResult a = runAquifer(std::string("aquifer_warm_off_") + closure, warm);
        const AquiferResult b = runAquifer(std::string("aquifer_warm_on_") + closure, with);
        ASSERT_TRUE(a.started && b.started) << closure << a.error << b.error;
        EXPECT_GT(a.infil_in, 0.0) << closure;
        EXPECT_EQ(a.infil_in, b.infil_in) << closure;
        EXPECT_EQ(a.storage, b.storage) << closure;
        EXPECT_EQ(a.depth, b.depth) << closure;
    }
}

// The two-stage (second order) step publishes its own stage rates; they carry the factor too.
TEST(FrozenGroundAquifer, TheSecondOrderStageRatesAreLimited) {
    for (const char* closure : {"CLOSED_FORM", "ENSLAVED", "SIGMA"}) {
        AquiferDeck open;
        open.closure = closure;
        open.cell_method = false;
        open.second_order = true;
        AquiferDeck frozen = open;
        frozen.frozen_ground = "*  0.15  1.5  2.4e6  0.30  8  3  -4  11";
        const AquiferResult a = runAquifer(std::string("aquifer_o2_open_") + closure, open);
        const AquiferResult b = runAquifer(std::string("aquifer_o2_frozen_") + closure, frozen);
        ASSERT_TRUE(a.started) << closure << ": " << a.error;
        ASSERT_TRUE(b.started) << closure << ": " << b.error;
        EXPECT_GT(a.infil_in, 0.0) << closure;
        EXPECT_EQ(b.infil_in, 0.0) << closure;
        EXPECT_LT(std::abs(b.continuity), 1e-9) << closure;
    }
}

// A surface checkpoint taken while mesh cells are freezing carries the slabs: the restored
// session finishes bit-identically to the one that never stopped. Managed sessions need the
// mesh aquifer profile, so the deck is the aquifer one.
TEST(FrozenGroundAquifer, ACheckpointMidFreezeContinuesBitIdentically) {
    for (const bool second_order : {true}) { // sessions take the second-order aquifer profile
        AquiferDeck d;
        d.cell_method = false;
        d.second_order = second_order;
        // Thin slab with little water: it freezes within the half-hour run.
        d.frozen_ground = "*  0.05  1.5  2.4e6  0.02  8  3  0.2  11";
        fs::create_directories(kArtifactDir);
        const std::string stem = second_order ? "mesh_checkpoint_o2" : "mesh_checkpoint";
        // A managed session refuses to overwrite the outputs of an earlier run.
        for (const auto& entry : fs::directory_iterator(kArtifactDir))
            if (entry.path().filename().string().rfind(stem, 0) == 0)
                fs::remove_all(entry.path());
        const auto inp = kArtifactDir / (stem + ".inp");
        std::ofstream(inp, std::ios::trunc) << aquiferModel(d);
        SWMMEngine a, b;
        for (SWMMEngine* e : {&a, &b}) {
            const std::string tag = stem + (e == &a ? "_a" : "_b");
            ASSERT_EQ(e->open(inp.string().c_str(),
                              (kArtifactDir / (tag + ".rpt")).string().c_str(), nullptr),
                      SWMM_OK);
            const std::string error =
                e->startCheckpointSession(SWMM_CHECKPOINT_OUTPUT_NONE, "", false, "");
            ASSERT_TRUE(error.empty()) << stem << ": " << error;
        }
        const auto& fa = a.surfaceRouter2D().frozenGroundEnthalpy();
        const auto& fb = b.surfaceRouter2D().frozenGroundEnthalpy();
        const auto& oa = a.surfaceRouter2D().frozenGroundFactor();
        const auto& ob = b.surfaceRouter2D().frozenGroundFactor();
        ASSERT_EQ(fa.size(), 2u);
        double elapsed = 0;
        int guard = 0;
        while (oa[0] > 0.5) { // run the source until the cells are partly frozen
            ASSERT_EQ(a.step(&elapsed), SWMM_OK);
            ASSERT_GT(elapsed, 0.0) << stem;
            ASSERT_LT(++guard, 100000);
        }
        ASSERT_GT(oa[0], 0.0) << stem;
        ASSERT_NE(fa, fb);
        const auto path = kArtifactDir / (stem + ".bin");
        ASSERT_EQ(a.saveCheckpoint(path.string()), "") << stem;
        ASSERT_EQ(b.loadCheckpoint(path.string()), "") << stem;
        EXPECT_EQ(fa, fb);
        EXPECT_EQ(oa, ob);
        for (;;) {
            double ea = 0, eb = 0;
            ASSERT_EQ(a.step(&ea), SWMM_OK);
            ASSERT_EQ(b.step(&eb), SWMM_OK);
            ASSERT_EQ(ea, eb);
            ASSERT_EQ(fa, fb) << stem;
            if (ea <= 0)
                break;
        }
        EXPECT_EQ(oa[0], 0.0) << stem; // frozen by the end
        EXPECT_EQ(oa, ob);
        EXPECT_EQ(a.surfaceRouter2D().state().depth, b.surfaceRouter2D().state().depth);
    }
}

// ---------------------------------------------------------------------------
// LID units take the frozen factor of the subcatchment they sit in
// ---------------------------------------------------------------------------

struct LidDeck {
    std::string control;       // [LID_CONTROLS] rows for a control named L
    double unit_area = 43560;  // ft2; the subcatchment is 5 acres (217800 ft2)
    int imperv = 50;           // subcatchment percent impervious
    double air_f = 14.0;       // deg F
    std::string frozen_ground; // [FROZEN_GROUND] rows (SI)
};

std::string lidModel(const LidDeck& d) {
    std::ostringstream f;
    f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION HORTON\n"
         "START_DATE 01/10/2026\nSTART_TIME 00:00:00\nEND_DATE 01/10/2026\nEND_TIME 03:00:00\n"
         "WET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 10\nREPORT_STEP 00:05:00\n\n"
         "[TEMPERATURE]\nTIMESERIES AIR\n\n"
         "[RAINGAGES]\nRG1 INTENSITY 1:00 1.0 TIMESERIES RAIN\n\n"
         "[TIMESERIES]\nRAIN 01/10/2026 00:00 1.0\nRAIN 01/10/2026 01:00 1.0\n"
         "RAIN 01/10/2026 02:00 0\n"
         "AIR 01/10/2026 00:00 "
      << d.air_f << "\nAIR 01/10/2026 04:00 " << d.air_f << "\n\n[SUBCATCHMENTS]\nS1 RG1 J1 5 "
      << d.imperv
      << " 500 0.5 0\n\n"
         "[SUBAREAS]\nS1 0.01 0.1 0.02 0.02 25 OUTLET\n\n"
         "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
         "[LID_CONTROLS]\n"
      << d.control << "\n[LID_USAGE]\nS1 L 1 " << d.unit_area
      << " 500 0 100 0\n\n"
         "[JUNCTIONS]\nJ1 10.0 10 0 0 0\n\n[OUTFALLS]\nOUT 9.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 OUT 400 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 3.0 0 0 0\n\n";
    if (!d.frozen_ground.empty())
        f << "[FROZEN_GROUND]\n" << d.frozen_ground << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
    return f.str();
}

struct LidResult {
    bool started = false;
    std::string error;
    double inflow = 0, exfiltration = 0, surface_flow = 0, drain_flow = 0, evaporation = 0;
    double soil_moisture = 0;                    // at the end
    double below_surface = 0;                    // pavement plus storage depth at the end, ft
    double initial_volume = 0, final_volume = 0; // water held by the unit, ft over its area
    double factor = 1;
};

LidResult runLid(const std::string& tag, const LidDeck& deck, lid::LIDType type) {
    LidResult r;
    fs::create_directories(kArtifactDir);
    const auto inp = kArtifactDir / (tag + ".inp");
    std::ofstream(inp, std::ios::trunc) << lidModel(deck);
    SWMM_Engine engine = swmm_engine_create();
    const auto fail = [&] {
        r.error = swmm_get_last_error_msg(engine);
        swmm_engine_destroy(engine);
        return r;
    };
    if (swmm_engine_open(engine, inp.string().c_str(),
                         (kArtifactDir / (tag + ".rpt")).string().c_str(),
                         (kArtifactDir / (tag + ".out")).string().c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(engine) != SWMM_OK || swmm_engine_start(engine, 1) != SWMM_OK)
        return fail();
    r.started = true;
    double elapsed = 0;
    for (;;) {
        if (swmm_engine_step(engine, &elapsed) != SWMM_OK)
            return fail();
        if (elapsed <= 0)
            break;
    }
    auto* e = static_cast<SWMMEngine*>(engine);
    const auto& g = e->lid().group(static_cast<int>(type));
    if (!g.wb_inflow.empty()) {
        r.inflow = g.wb_inflow[0];
        r.exfiltration = g.wb_infil[0];
        r.surface_flow = g.wb_surf_flow[0];
        r.drain_flow = g.wb_drain_flow[0];
        r.evaporation = g.wb_evap[0];
        r.soil_moisture = g.soil_moist[0];
        r.below_surface = g.pave_depth[0] + g.stor_depth[0];
        r.initial_volume = g.wb_init_vol[0];
        r.final_volume = g.wb_final_vol[0];
    }
    r.factor = e->context().frozen_ground.factorOf(0);
    swmm_engine_end(engine);
    swmm_engine_destroy(engine);
    return r;
}

const char* const kFrozenLidRow = "S1  0.15  1.5  2.4e6  0.30  8  3  -4  11";
const char* const kBioCell = "L BC\nL SURFACE 6 0 0.1 1 5\nL SOIL 6 0.5 0.2 0.1 5 10 3.5\n"
                             "L STORAGE 12 0.75 0.5 0\n";
const char* const kPavement = "L PP\nL SURFACE 0.1 0 0.1 1 5\nL PAVEMENT 6 0.15 0 100 0 0\n"
                              "L STORAGE 12 0.75 0.5 0\n";
const char* const kTrench = "L IT\nL SURFACE 6 0 0.1 1 5\nL STORAGE 12 0.75 0.5 0\n";
const char* const kSwale = "L VS\nL SURFACE 12 0 0.1 1 5\n";
const char* const kGreenRoof = "L GR\nL SURFACE 1 0 0.1 1 5\nL SOIL 4 0.5 0.2 0.1 0.5 10 3.5\n"
                               "L DRAINMAT 3 0.5 0.1\n";
const char* const kRainBarrel = "L RB\nL STORAGE 48 0.75 0 0\nL DRAIN 1 0.5 0 0\n";

TEST(FrozenGroundLid, NothingEntersFrozenGroundFromAUnit) {
    const struct {
        const char* name;
        const char* control;
        lid::LIDType type;
    } units[] = {{"bio_cell", kBioCell, lid::LIDType::BIO_CELL},
                 {"pavement", kPavement, lid::LIDType::PERM_PAVEMENT},
                 {"trench", kTrench, lid::LIDType::INFIL_TRENCH},
                 {"swale", kSwale, lid::LIDType::VEG_SWALE}};
    for (const auto& u : units) {
        LidDeck open;
        open.control = u.control;
        LidDeck frozen = open;
        frozen.frozen_ground = kFrozenLidRow;
        const LidResult a = runLid(std::string("lid_open_") + u.name, open, u.type);
        const LidResult b = runLid(std::string("lid_frozen_") + u.name, frozen, u.type);
        ASSERT_TRUE(a.started) << u.name << ": " << a.error;
        ASSERT_TRUE(b.started) << u.name << ": " << b.error;
        EXPECT_EQ(b.factor, 0.0) << u.name;
        EXPECT_GT(a.inflow, 0.0) << u.name;
        EXPECT_GT(a.exfiltration, 0.0)
            << u.name << " inflow " << a.inflow << " surface " << a.surface_flow << " drain "
            << a.drain_flow << " soil " << a.soil_moisture << " held " << a.final_volume;
        EXPECT_EQ(b.exfiltration, 0.0) << u.name;
        // The unit's water balance closes with nothing lost to the ground: what the open
        // ground took now leaves over the surface, by the drain, or stays in the unit.
        for (const LidResult* r : {&a, &b})
            EXPECT_NEAR(r->inflow + r->initial_volume,
                        r->evaporation + r->exfiltration + r->surface_flow + r->drain_flow +
                            r->final_volume,
                        5e-3 * r->inflow)
                << u.name; // the unit ledger closes to the layer solver's tolerance, frozen or not
        EXPECT_GT(b.surface_flow + b.final_volume, a.surface_flow + a.final_volume) << u.name;
    }
}

// Frozen pavement is closed at its surface: nothing reaches the pavement or storage layers.
TEST(FrozenGroundLid, FrozenPavementLetsNothingThrough) {
    LidDeck open;
    open.control = kPavement;
    LidDeck frozen = open;
    frozen.frozen_ground = kFrozenLidRow;
    const LidResult a = runLid("lid_pave_open", open, lid::LIDType::PERM_PAVEMENT);
    const LidResult b = runLid("lid_pave_frozen", frozen, lid::LIDType::PERM_PAVEMENT);
    ASSERT_TRUE(a.started && b.started) << a.error << b.error;
    EXPECT_GT(a.below_surface, 0.0);
    EXPECT_EQ(b.below_surface, 0.0);
    EXPECT_GT(b.surface_flow, a.surface_flow);
}

// A bio-retention cell on frozen ground takes no water into its soil either.
TEST(FrozenGroundLid, TheSoilLayerOfAFrozenUnitStaysDry) {
    LidDeck open;
    open.control = kBioCell;
    LidDeck frozen = open;
    frozen.frozen_ground = kFrozenLidRow;
    const LidResult a = runLid("lid_soil_open", open, lid::LIDType::BIO_CELL);
    const LidResult b = runLid("lid_soil_frozen", frozen, lid::LIDType::BIO_CELL);
    ASSERT_TRUE(a.started && b.started) << a.error << b.error;
    EXPECT_GT(a.soil_moisture, b.soil_moisture);
}

// Units that put no water into the ground are untouched, bit for bit.
TEST(FrozenGroundLid, GreenRoofsAndRainBarrelsAreUnchanged) {
    const struct {
        const char* name;
        const char* control;
        lid::LIDType type;
    } units[] = {{"green_roof", kGreenRoof, lid::LIDType::GREEN_ROOF},
                 {"rain_barrel", kRainBarrel, lid::LIDType::RAIN_BARREL}};
    for (const auto& u : units) {
        LidDeck open;
        open.control = u.control;
        LidDeck frozen = open;
        frozen.frozen_ground = kFrozenLidRow;
        const LidResult a = runLid(std::string("lid_same_open_") + u.name, open, u.type);
        const LidResult b = runLid(std::string("lid_same_frozen_") + u.name, frozen, u.type);
        ASSERT_TRUE(a.started && b.started) << u.name << a.error << b.error;
        EXPECT_EQ(b.factor, 0.0) << u.name;
        EXPECT_GT(a.inflow, 0.0) << u.name;
        EXPECT_EQ(a.inflow, b.inflow) << u.name;
        EXPECT_EQ(a.surface_flow, b.surface_flow) << u.name;
        EXPECT_EQ(a.drain_flow, b.drain_flow) << u.name;
        EXPECT_EQ(a.evaporation, b.evaporation) << u.name;
        EXPECT_EQ(a.soil_moisture, b.soil_moisture) << u.name;
    }
}

// A swale that fills the whole subcatchment infiltrates at the native soil rate, which the
// engine evaluates on a separate path; frozen ground closes that path too.
TEST(FrozenGroundLid, AUnitCoveringTheWholeSubcatchmentIsLimited) {
    LidDeck open;
    open.control = kSwale;
    open.unit_area = 217800;
    open.imperv = 0;
    LidDeck frozen = open;
    frozen.frozen_ground = kFrozenLidRow;
    const LidResult a = runLid("lid_full_open", open, lid::LIDType::VEG_SWALE);
    const LidResult b = runLid("lid_full_frozen", frozen, lid::LIDType::VEG_SWALE);
    ASSERT_TRUE(a.started && b.started) << a.error << b.error;
    EXPECT_GT(a.exfiltration, 0.0);
    EXPECT_EQ(b.exfiltration, 0.0);
}

TEST(FrozenGroundLid, GroundThatNeverFreezesIsBitIdenticalToNoSection) {
    for (const char* control : {kBioCell, kPavement, kSwale}) {
        LidDeck warm;
        warm.control = control;
        warm.air_f = 55.0;
        LidDeck with = warm;
        with.frozen_ground = "S1  0.15  1.5  2.4e6  0.30  8  3  5";
        const lid::LIDType type = control == kBioCell    ? lid::LIDType::BIO_CELL
                                  : control == kPavement ? lid::LIDType::PERM_PAVEMENT
                                                         : lid::LIDType::VEG_SWALE;
        const LidResult a = runLid("lid_warm_off", warm, type),
                        b = runLid("lid_warm_on", with, type);
        ASSERT_TRUE(a.started && b.started) << a.error << b.error;
        EXPECT_EQ(b.factor, 1.0);
        EXPECT_GT(a.exfiltration, 0.0);
        EXPECT_EQ(a.exfiltration, b.exfiltration);
        EXPECT_EQ(a.surface_flow, b.surface_flow);
        EXPECT_EQ(a.soil_moisture, b.soil_moisture);
    }
}

} // namespace
