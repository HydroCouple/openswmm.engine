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
 * @file test_climate_humidity.cpp
 * @brief `[TEMPERATURE] HUMIDITY` — every deck form parses, survives the
 *        writer, and reaches `ClimateState::humidity` at run time.
 *
 * @details Forms:
 *            HUMIDITY [DEWPOINT] <value>
 *            HUMIDITY [DEWPOINT] MONTHLY h1 … h12
 *            HUMIDITY [DEWPOINT] TIMESERIES <name>
 *          DEWPOINT decks are converted to RH each step from the effective
 *          air temperature with the Magnus form SurfaceExchange uses:
 *          RH = 100·e_s(Td)/e_s(Ta), clamped to [0, 100].
 *
 *          Before this the writer dropped the HUMIDITY line entirely, so a
 *          GUI save silently reset RH to the 50 % default.
 *
 * @see plans/transport/HUMIDITY_DEWPOINT_CLIMATE_GUI_PLAN_2026-09-06.md
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_climate.h>

#include "core/SWMMEngine.hpp"
#include "../../src/engine/core/InpWriter.hpp"

// Working directory is tests/unit/engine/data/ (set by CMakeLists.txt).
namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine e) {
    return *static_cast<openswmm::SWMMEngine*>(e);
}

double es_kpa(double t_c) { return 0.61275 * std::exp(17.27 * t_c / (237.3 + t_c)); }

void write_file(const std::string& path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

std::string slurp(const std::string& path) {
    std::ifstream in(path);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

/// Minimal routing deck: air temperature 50 degF from a series, plus the
/// caller's HUMIDITY line. `flow_units` selects US/SI for the dew-point units.
std::string deck(const std::string& humidity_line,
                 const char* flow_units = "CFS",
                 const char* air_temp = "50.0") {
    std::ostringstream f;
    f << "[TITLE]\nRH gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS " << flow_units << "\nFLOW_ROUTING KINWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "ROUTING_STEP 60\nREPORT_STEP 00:15:00\nWET_STEP 00:05:00\nDRY_STEP 00:05:00\n\n"
      << "[TEMPERATURE]\nTIMESERIES air_ts\n"
      << "WINDSPEED MONTHLY 5 5 5 5 5 5 5 5 5 5 5 5\n"
      << humidity_line << "\n\n"
      << "[TIMESERIES]\n"
      << "air_ts 01/01/2026 00:00 " << air_temp << "\n"
      << "air_ts 01/02/2026 00:00 " << air_temp << "\n"
      << "rh_ts  01/01/2026 00:00 80.0\n"
      << "rh_ts  01/02/2026 00:00 80.0\n"
      << "td_ts  01/01/2026 00:00 40.0\n"
      << "td_ts  01/02/2026 00:00 40.0\n"
      << "rain   01/01/2026 00:00 0.0\n"
      << "rain   01/02/2026 00:00 0.0\n\n"
      << "[RAINGAGES]\nRG1 INTENSITY 1:00 1.0 TIMESERIES rain\n\n"
      << "[SUBCATCHMENTS]\nS1 RG1 J1 1 50 100 0.5 0\n\n"
      << "[SUBAREAS]\nS1 0.01 0.1 0.05 0.05 25 OUTLET\n\n"
      << "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
      << "[JUNCTIONS]\nJ1 10 0 0 0 0\n\n"
      << "[OUTFALLS]\nOUT 8 FREE NO\n\n"
      << "[CONDUITS]\nC1 J1 OUT 400 0.01 0 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 1 0 0 0 1\n\n"
      << "[REPORT]\nINPUT NO\n";
    return f.str();
}

/// Open only (BUILDING state) so config getters can be exercised.
SWMM_Engine open_only(const std::string& inp) {
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    const std::string rpt = inp + ".rpt";
    if (swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr, nullptr) != SWMM_OK) {
        ADD_FAILURE() << "open(" << inp << "): "
                      << (swmm_get_last_error_msg(e) ? swmm_get_last_error_msg(e) : "");
        swmm_engine_destroy(e);
        return nullptr;
    }
    return e;
}

/// Run to the end and hold the handle so climate_state is inspectable.
SWMM_Engine run_and_hold(const std::string& inp) {
    SWMM_Engine e = open_only(inp);
    if (e == nullptr) return nullptr;
    if (swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        ADD_FAILURE() << "init/start(" << inp << "): "
                      << (swmm_get_last_error_msg(e) ? swmm_get_last_error_msg(e) : "");
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

void destroy(SWMM_Engine e) {
    if (e) { swmm_engine_close(e); swmm_engine_destroy(e); }
}

struct Parsed {
    int type = -1, var = -1;
    double monthly[12] = {};
    std::string ts;
};

Parsed read_config(SWMM_Engine e) {
    Parsed p;
    swmm_climate_get_humidity_type(e, &p.type);
    swmm_climate_get_humidity_variable(e, &p.var);
    swmm_climate_get_humidity_monthly(e, p.monthly, 12);
    char buf[64] = {};
    swmm_climate_get_humidity_timeseries(e, buf, sizeof(buf));
    p.ts = buf;
    return p;
}

}  // namespace

// ---------------------------------------------------------------------------
// Parsing — every deck form lands in the config the C API exposes.
// ---------------------------------------------------------------------------

TEST(ClimateHumidity, ParsesEveryDeckForm) {
    struct Case { const char* line; int type; int var; double m0; double m11; const char* ts; };
    const Case cases[] = {
        {"HUMIDITY 65",                                          0, 0, 65, 65, ""},
        {"HUMIDITY MONTHLY 10 20 30 40 50 60 70 80 90 91 92 93", 1, 0, 10, 93, ""},
        {"HUMIDITY TIMESERIES rh_ts",                            2, 0, 50, 50, "rh_ts"},
        {"HUMIDITY DEWPOINT 40",                                 0, 1, 40, 40, ""},
        {"HUMIDITY DEWPOINT MONTHLY 1 2 3 4 5 6 7 8 9 10 11 12", 1, 1,  1, 12, ""},
        {"HUMIDITY DEWPOINT TIMESERIES td_ts",                   2, 1, 50, 50, "td_ts"},
    };
    int k = 0;
    for (const auto& c : cases) {
        const std::string inp = "_rh_parse_" + std::to_string(k++) + ".inp";
        write_file(inp, deck(c.line));
        SWMM_Engine e = open_only(inp);
        ASSERT_NE(e, nullptr) << c.line;
        const Parsed p = read_config(e);
        EXPECT_EQ(p.type, c.type) << c.line;
        EXPECT_EQ(p.var,  c.var)  << c.line;
        EXPECT_DOUBLE_EQ(p.monthly[0],  c.m0)  << c.line;
        EXPECT_DOUBLE_EQ(p.monthly[11], c.m11) << c.line;
        EXPECT_EQ(p.ts, c.ts) << c.line;
        destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Writer — the line survives a save and reloads to the same config.
// ---------------------------------------------------------------------------

TEST(ClimateHumidity, WriterRoundTripsEveryForm) {
    const char* lines[] = {
        "HUMIDITY 65",
        "HUMIDITY MONTHLY 10 20 30 40 50 60 70 80 90 91 92 93",
        "HUMIDITY TIMESERIES rh_ts",
        "HUMIDITY DEWPOINT 40",
        "HUMIDITY DEWPOINT MONTHLY 1 2 3 4 5 6 7 8 9 10 11 12",
        "HUMIDITY DEWPOINT TIMESERIES td_ts",
    };
    int k = 0;
    for (const char* line : lines) {
        const std::string tag = std::to_string(k++);
        const std::string inp = "_rh_rt_" + tag + ".inp";
        const std::string out = "_rh_rt_" + tag + "_saved.inp";
        write_file(inp, deck(line));

        SWMM_Engine a = open_only(inp);
        ASSERT_NE(a, nullptr) << line;
        const Parsed before = read_config(a);
        std::vector<std::string> warnings;
        ASSERT_EQ(openswmm::inp_writer::writeInpFile(as_cpp_engine(a).context(), out, &warnings), 0);
        for (const auto& w : warnings) ADD_FAILURE() << line << " writer warning: " << w;
        destroy(a);

        EXPECT_NE(slurp(out).find("HUMIDITY"), std::string::npos)
            << line << ": the saved deck lost its HUMIDITY line";

        SWMM_Engine b = open_only(out);
        ASSERT_NE(b, nullptr) << line;
        const Parsed after = read_config(b);
        EXPECT_EQ(after.type, before.type) << line;
        EXPECT_EQ(after.var,  before.var)  << line;
        EXPECT_EQ(after.ts,   before.ts)   << line;
        for (int i = 0; i < 12; ++i)
            EXPECT_NEAR(after.monthly[i], before.monthly[i], 1.0e-9) << line << " [" << i << "]";
        destroy(b);
    }
}

TEST(ClimateHumidity, DefaultDeckWritesNoHumidityLine) {
    const std::string inp = "_rh_default.inp";
    const std::string out = "_rh_default_saved.inp";
    write_file(inp, deck(""));
    SWMM_Engine e = open_only(inp);
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(as_cpp_engine(e).context(), out, nullptr), 0);
    destroy(e);
    EXPECT_EQ(slurp(out).find("HUMIDITY"), std::string::npos)
        << "an untouched 50 % default must not be written out";
}

// ---------------------------------------------------------------------------
// Runtime — the value reaches ClimateState::humidity.
// ---------------------------------------------------------------------------

TEST(ClimateHumidity, TimeseriesReachesTheClimateState) {
    const std::string inp = "_rh_run_ts.inp";
    write_file(inp, deck("HUMIDITY TIMESERIES rh_ts"));
    SWMM_Engine e = run_and_hold(inp);
    ASSERT_NE(e, nullptr);
    EXPECT_NEAR(as_cpp_engine(e).context().climate_state.humidity, 80.0, 1.0e-9)
        << "the rh_ts series holds 80 %; the monthly default is 50 %";
    destroy(e);
}

TEST(ClimateHumidity, DewPointConvertsToRelativeHumidityUS) {
    // Air 50 degF (10 degC), dew point 40 degF (4.444 degC).
    const std::string inp = "_rh_run_td_us.inp";
    write_file(inp, deck("HUMIDITY DEWPOINT 40"));
    SWMM_Engine e = run_and_hold(inp);
    ASSERT_NE(e, nullptr);
    const double expected = 100.0 * es_kpa((40.0 - 32.0) * 5.0 / 9.0) / es_kpa(10.0);
    EXPECT_NEAR(as_cpp_engine(e).context().climate_state.humidity, expected, 1.0e-6);
    EXPECT_GT(expected, 60.0);
    EXPECT_LT(expected, 80.0);
    destroy(e);
}

TEST(ClimateHumidity, DewPointConvertsToRelativeHumiditySI) {
    // SI deck: dew point authored in degC. Air temperature from the series
    // is 50 (held in degF internally, like every other climate temperature).
    const std::string inp = "_rh_run_td_si.inp";
    write_file(inp, deck("HUMIDITY DEWPOINT 4.0", "CMS"));
    SWMM_Engine e = run_and_hold(inp);
    ASSERT_NE(e, nullptr);
    const double expected = 100.0 * es_kpa(4.0) / es_kpa(10.0);
    EXPECT_NEAR(as_cpp_engine(e).context().climate_state.humidity, expected, 1.0e-6);
    destroy(e);
}

TEST(ClimateHumidity, DewPointAboveAirClampsToSaturation) {
    const std::string inp = "_rh_run_td_sat.inp";
    write_file(inp, deck("HUMIDITY DEWPOINT 60"));  // above the 50 degF air
    SWMM_Engine e = run_and_hold(inp);
    ASSERT_NE(e, nullptr);
    EXPECT_DOUBLE_EQ(as_cpp_engine(e).context().climate_state.humidity, 100.0);
    destroy(e);
}

TEST(ClimateHumidity, DewPointTimeseriesReachesTheClimateState) {
    const std::string inp = "_rh_run_td_ts.inp";
    write_file(inp, deck("HUMIDITY DEWPOINT TIMESERIES td_ts"));  // 40 degF
    SWMM_Engine e = run_and_hold(inp);
    ASSERT_NE(e, nullptr);
    const double expected = 100.0 * es_kpa((40.0 - 32.0) * 5.0 / 9.0) / es_kpa(10.0);
    EXPECT_NEAR(as_cpp_engine(e).context().climate_state.humidity, expected, 1.0e-6);
    destroy(e);
}
