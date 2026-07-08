/**
 * @file test_ignore_flags.cpp
 * @brief Runtime-effect tests for the [OPTIONS] IGNORE_* process flags.
 *
 * @details The existing option tests (test_options_parser.cpp,
 *          test_rdii.cpp) only assert parse->store. These tests close the
 *          coverage gap that let five of the six flags become dead no-ops:
 *          they run a small model twice — once with the flag off and once with
 *          it on — and assert the corresponding process is actually suppressed.
 *          They also verify the legacy auto-set couplings (no aquifers => ignore
 *          groundwater, no snowpacks => ignore snowmelt, FLOW_ROUTING NONE =>
 *          ignore routing).
 *
 *          Per the project's Transparent File IO rule, every model + its .rpt /
 *          .out artifacts are written to ./ignore_flags/ (relative to the test
 *          data dir) and are NOT deleted, so a user can review them.
 *
 * @see src/engine/core/SWMMEngine.cpp   (runtime guards)
 * @see src/engine/hydrology/Gage.cpp    (IGNORE_RAINFALL)
 * @see src/engine/input/PostParseResolver.cpp (auto-set couplings)
 * @see Legacy reference: src/legacy/engine/swmm5.c, runoff.c, gage.c, subcatch.c
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_statistics.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_output.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

// Persistent, reviewable artifact directory (relative to the test cwd, which
// CMake pins to tests/unit/engine/data). NOT deleted after the run.
const fs::path kArtifactDir = "ignore_flags";

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

// ---------------------------------------------------------------------------
// Model builders — each returns valid INP text. `ignore` lines are appended
// verbatim so the same model runs with the flag on or off.
// ---------------------------------------------------------------------------

// A 100%-impervious subcatchment under 1 in/hr rain draining J1 -> O1.
std::string rain_model(const std::string& ignore_lines,
                       const std::string& extra_sections = "") {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "INFILTRATION         HORTON\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "REPORT_START_DATE    01/01/2026\n"
         "REPORT_START_TIME    00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             02:00:00\n"
         "REPORT_STEP          00:05:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         0:00:30\n"
      << ignore_lines
      << "\n[RAINGAGES]\n"
         ";;Name Format Interval SCF Source\n"
         "RG1  INTENSITY 0:05 1.0 TIMESERIES TS1\n"
         "\n[SUBCATCHMENTS]\n"
         ";;Name RainGage Outlet Area %Imperv Width Slope Curblen SnowPack\n"
         "S1  RG1  J1  10.0  100  500  0.5  0  \n"
         "\n[SUBAREAS]\n"
         ";;Subcatch Nimp Nperv Simp Sperv PctZero RouteTo\n"
         "S1  0.01  0.1  0.05  0.05  25  OUTLET\n"
         "\n[INFILTRATION]\n"
         ";;Subcatch MaxRate MinRate Decay DryTime MaxInfil\n"
         "S1  3.0  0.5  4.0  7  0\n"
         "\n[TIMESERIES]\n"
         ";;Name Date Time Value\n"
         "TS1  01/01/2026 00:00 1.0\n"
         "TS1  01/01/2026 01:00 1.0\n"
         "TS1  01/01/2026 01:01 0.0\n"
         "\n[JUNCTIONS]\n"
         ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
         "J1  100.0  10.0  0.0  0.0  0.0\n"
         "\n[OUTFALLS]\n"
         ";;Name Elev Type\n"
         "O1  95.0  FREE\n"
         "\n[CONDUITS]\n"
         ";;Name From To Length N InOff OutOff\n"
         "C1  J1  O1  400.0  0.013  0  0\n"
         "\n[XSECTIONS]\n"
         ";;Link Shape Geom1 Geom2 Geom3 Geom4 Barrels\n"
         "C1  CIRCULAR  1.5  0  0  0  1\n"
      << extra_sections;
    return s.str();
}

// Quality sections: one pollutant with buildup + washoff on a land use that
// fully covers S1.
std::string quality_sections() {
    return
        "\n[POLLUTANTS]\n"
        ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
        "TSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n"
        "\n[LANDUSES]\n"
        ";;Name SweepInterval Availability LastSweep\n"
        "LU1  0  0  0\n"
        "\n[COVERAGES]\n"
        ";;Subcatch LandUse Percent\n"
        "S1  LU1  100\n"
        "\n[BUILDUP]\n"
        ";;LandUse Pollutant Func C1 C2 C3 PerUnit\n"
        "LU1  TSS  POW  100  0  1  AREA\n"
        "\n[WASHOFF]\n"
        ";;LandUse Pollutant Func C1 C2 SweepRmvl BmpRmvl\n"
        "LU1  TSS  EXP  0.1  1.0  0  0\n";
}

// Aquifer with a high initial water table so lateral GW flow to J1 is nonzero
// from t=0 (independent of rain).
std::string groundwater_sections() {
    return
        "\n[AQUIFERS]\n"
        ";;Name Por WP FC Ksat Kslope Tslope ETu ETs Seep Ebot Egw Umc\n"
        "AQ1  0.5  0.15  0.30  5.0  10  15  0.35  14  0.0  0.0  8.0  0.30\n"
        "\n[GROUNDWATER]\n"
        ";;Subcatch Aquifer Node Esurf A1 B1 A2 B2 A3 Dsw Egwt Ebot Wgr Umc\n"
        "S1  AQ1  J1  10.0  0.1  1.5  0  0  0  0  *  0  0  *\n";
}

// Snowpack whose surfaces hold snow (very small melt coefficients), plus a
// constant sub-freezing air temperature so rain falls as snow.
std::string snow_sections() {
    return
        "\n[SNOWPACKS]\n"
        ";;Name Surface Cmin Cmax Tbase FWF SD0 FW0 SNN0/SFrac\n"
        "SP1  PLOWABLE    0.001  0.001  32  0.10  0  0  0.5\n"
        "SP1  IMPERVIOUS  0.001  0.001  32  0.10  0  0  0.0\n"
        "SP1  PERVIOUS    0.001  0.001  32  0.10  0  0  0.0\n"
        "SP1  REMOVAL     1.0  0  0  0  0  0\n"
        "\n[TEMPERATURE]\n"
        "TIMESERIES TEMPTS\n";
}

// ---------------------------------------------------------------------------
// Fixture
// ---------------------------------------------------------------------------

class IgnoreFlagsTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;

    void TearDown() override {
        if (engine_) {
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    openswmm::SimulationContext& ctx() {
        return static_cast<openswmm::SWMMEngine*>(engine_)->context();
    }

    // Open a model from INP text; leaves the engine INITIALIZED (post open()).
    void open(const std::string& tag, const std::string& inp_text,
              std::string* out_path = nullptr) {
        const auto inp = kArtifactDir / (tag + ".inp");
        const auto rpt = kArtifactDir / (tag + ".rpt");
        const auto out = kArtifactDir / (tag + ".out");
        write_file(inp, inp_text);
        if (out_path) *out_path = out.string();

        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, inp.string().c_str(),
                                   rpt.string().c_str(), out.string().c_str(),
                                   nullptr), SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
    }

    // Full open -> initialize -> start -> run -> end.
    void run(const std::string& tag, const std::string& inp_text,
             std::string* out_path = nullptr) {
        open(tag, inp_text, out_path);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
            << swmm_get_last_error_msg(engine_);
        double elapsed = 0.0;
        for (;;) {
            int rc = swmm_engine_step(engine_, &elapsed);
            ASSERT_EQ(rc, SWMM_OK) << swmm_get_last_error_msg(engine_);
            if (elapsed <= 0.0) break;
        }
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
    }

    void reset() {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
};

// ===========================================================================
// IGNORE_RAINFALL — no runoff, no RDII
// ===========================================================================

TEST_F(IgnoreFlagsTest, IgnoreRainfall_ZeroesRunoff) {
    double runoff_on = -1.0, runoff_off = -1.0;

    run("rainfall_active", rain_model(""));
    ASSERT_EQ(swmm_get_runoff_total(engine_, SWMM_RUNOFF_RUNOFF, &runoff_on),
              SWMM_OK);
    reset();

    run("rainfall_ignored", rain_model("IGNORE_RAINFALL      YES\n"));
    ASSERT_EQ(swmm_get_runoff_total(engine_, SWMM_RUNOFF_RUNOFF, &runoff_off),
              SWMM_OK);

    EXPECT_GT(runoff_on, 0.0) << "baseline model should generate runoff";
    EXPECT_DOUBLE_EQ(runoff_off, 0.0)
        << "IGNORE_RAINFALL must suppress all runoff";
}

// ===========================================================================
// IGNORE_ROUTING — no hydraulic routing; FLOW_ROUTING NONE couples to it
// ===========================================================================

TEST_F(IgnoreFlagsTest, IgnoreRouting_SkipsRouting) {
    run("routing_active", rain_model(""));
    int c1 = swmm_link_index(engine_, "C1");
    ASSERT_GE(c1, 0);
    double qmax_on = -1.0;
    ASSERT_EQ(swmm_stat_link_max_flow(engine_, c1, &qmax_on), SWMM_OK);
    reset();

    run("routing_ignored", rain_model("IGNORE_ROUTING       YES\n"));
    c1 = swmm_link_index(engine_, "C1");
    ASSERT_GE(c1, 0);
    double qmax_off = -1.0;
    ASSERT_EQ(swmm_stat_link_max_flow(engine_, c1, &qmax_off), SWMM_OK);
    double wet_off = -1.0;
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_WET_WEATHER,
                                     &wet_off), SWMM_OK);

    EXPECT_GT(qmax_on, 0.0) << "baseline conduit should carry flow";
    EXPECT_DOUBLE_EQ(qmax_off, 0.0) << "IGNORE_ROUTING must skip routing";
    EXPECT_DOUBLE_EQ(wet_off, 0.0)
        << "no routing => no wet-weather inflow booked";
}

TEST_F(IgnoreFlagsTest, FlowRoutingNone_CouplesIgnoreRouting) {
    open("routing_none", rain_model("FLOW_ROUTING         NONE\n"));
    EXPECT_TRUE(ctx().options.ignore_routing)
        << "FLOW_ROUTING NONE must force IGNORE_ROUTING (legacy project.c:504)";
}

// ===========================================================================
// IGNORE_QUALITY — no pollutant columns in the .out file
// ===========================================================================

TEST_F(IgnoreFlagsTest, IgnoreQuality_DropsPollutantColumns) {
    std::string out_on, out_off;

    run("quality_active", rain_model("", quality_sections()), &out_on);
    reset();
    run("quality_ignored", rain_model("IGNORE_QUALITY       YES\n",
                                       quality_sections()), &out_off);
    reset();

    SWMM_Output h_on = swmm_output_open(out_on.c_str());
    ASSERT_NE(h_on, nullptr);
    int np_on = swmm_output_get_pollut_count(h_on);
    swmm_output_close(h_on);

    SWMM_Output h_off = swmm_output_open(out_off.c_str());
    ASSERT_NE(h_off, nullptr);
    int np_off = swmm_output_get_pollut_count(h_off);
    swmm_output_close(h_off);

    EXPECT_EQ(np_on, 1) << "baseline .out should carry the one pollutant";
    EXPECT_EQ(np_off, 0)
        << "IGNORE_QUALITY must drop all pollutant columns (output.c:150)";
}

// ===========================================================================
// IGNORE_GROUNDWATER — no GW inflow to the receiving node
// ===========================================================================

TEST_F(IgnoreFlagsTest, IgnoreGroundwater_ZeroesGwInflow) {
    double gw_on = -1.0, gw_off = -1.0;

    run("gw_active", rain_model("", groundwater_sections()));
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_GW_INFLOW, &gw_on),
              SWMM_OK);
    reset();

    run("gw_ignored", rain_model("IGNORE_GROUNDWATER   YES\n",
                                 groundwater_sections()));
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_GW_INFLOW, &gw_off),
              SWMM_OK);

    EXPECT_GT(gw_on, 0.0) << "baseline model should produce GW inflow";
    EXPECT_DOUBLE_EQ(gw_off, 0.0)
        << "IGNORE_GROUNDWATER must suppress GW inflow";
}

// ===========================================================================
// IGNORE_SNOWMELT — precip treated as rain instead of accumulating as snow
// ===========================================================================

TEST_F(IgnoreFlagsTest, IgnoreSnowmelt_ChangesRunoff) {
    // Sub-freezing temperature series so precip would fall as snow.
    const std::string temp_ts =
        "TEMPTS  01/01/2026 00:00 20\n"
        "TEMPTS  01/01/2026 12:00 20\n";
    // Assign S1 the snowpack SP1 (SnowPack column) by rebuilding the subcatch
    // line; simplest is to append the snow sections + a TEMPTS series and set
    // the subcatchment's snowpack via a dedicated model.
    auto snow_model = [&](const std::string& ignore_lines) {
        std::string extra = snow_sections();
        extra += "\n[TIMESERIES]\n" + temp_ts;
        // Re-point S1 to snowpack SP1: emit a model whose SUBCATCHMENTS line
        // carries SP1 in the SnowPack column.
        std::string inp = rain_model(ignore_lines, extra);
        const std::string from = "S1  RG1  J1  10.0  100  500  0.5  0  \n";
        const std::string to   = "S1  RG1  J1  10.0  100  500  0.5  0  SP1\n";
        auto pos = inp.find(from);
        if (pos != std::string::npos) inp.replace(pos, from.size(), to);
        return inp;
    };

    double runoff_keep = -1.0, runoff_ignore = -1.0;

    run("snow_active", snow_model(""));
    ASSERT_EQ(swmm_get_runoff_total(engine_, SWMM_RUNOFF_RUNOFF, &runoff_keep),
              SWMM_OK);
    reset();

    run("snow_ignored", snow_model("IGNORE_SNOWMELT      YES\n"));
    ASSERT_EQ(swmm_get_runoff_total(engine_, SWMM_RUNOFF_RUNOFF, &runoff_ignore),
              SWMM_OK);

    // With snowmelt honored + sub-freezing temps, precip accumulates as snow
    // and little/no runoff is produced this hour. Ignoring snowmelt routes the
    // same precip straight to runoff. The flag must change the outcome.
    EXPECT_GT(runoff_ignore, runoff_keep)
        << "IGNORE_SNOWMELT should route precip as rain (more runoff) than "
           "when snow accumulates";
}

// ===========================================================================
// Auto-set couplings (legacy project_validate) — verified at open() time
// ===========================================================================

TEST_F(IgnoreFlagsTest, AutoCouple_NoAquifer_IgnoresGroundwater) {
    open("couple_no_aquifer", rain_model(""));
    EXPECT_TRUE(ctx().options.ignore_groundwater)
        << "no aquifers => IGNORE_GROUNDWATER (legacy project.c:222)";
}

TEST_F(IgnoreFlagsTest, AutoCouple_NoSnowpack_IgnoresSnowmelt) {
    open("couple_no_snowpack", rain_model(""));
    EXPECT_TRUE(ctx().options.ignore_snow_melt)
        << "no snowpacks => IGNORE_SNOWMELT (legacy project.c:221)";
}

}  // namespace
