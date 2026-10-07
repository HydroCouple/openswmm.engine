/*!
 * \file   test_2d_run_stats.cpp
 * \brief  swmm_2d_get_run_stats: the backend label, momentum closure,
 *         configured LTS_TIERS and the cumulative marcher counters
 *         (substeps, face-kernel evaluations, active-cell fractions, LTS
 *         tier occupancy) readable DURING a run — the numbers the GUI shows
 *         while a 2D simulation is in flight — plus the "2D solver: …"
 *         advisory line that names the solver in the report.
 *
 * Deck: a 4-triangle square sloping north→south with 0.3 m of initial
 * depth, one junction and outfall (no coupling), WALL boundaries,
 * LTS_TIERS 2 so the tier telemetry populates.
 */
#include <gtest/gtest.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include "platform_test_support.hpp"   // setEnvVar: MSVC has no setenv

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path("run_stats_2d_out");

std::string buildModel() {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:10:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         5\n"
        "\n[JUNCTIONS]\nJ1      0.0   3.0       0          0         0\n"
        "\n[OUTFALLS]\nO1     -1.5    FREE  NO\n"
        "\n[CONDUITS]\nC1      J1    O1  100.0    0.013      0         0          0\n"
        "\n[XSECTIONS]\nC1      CIRCULAR  1.0    0      0      0      1\n"
        "\n[2D_OPTIONS]\nMAX_TIMESTEP 2\nDRY_DEPTH 0.001\nLTS_TIERS 2\nREPORT_2D NO\n"
        "\n[2D_VERTICES]\n;; UNITS: SI (m)\n"
        ";;X      Y      Z\n"
        " 0.0    0.0   10.0\n"
        "20.0    0.0   10.0\n"
        "20.0   20.0    9.0\n"
        " 0.0   20.0    9.0\n"
        "10.0   10.0    9.5\n"
        "\n[2D_TRIANGLES]\n"
        ";;V1 V2 V3 MANNINGS_N INIT_DEPTH\n"
        "0     1   4   0.03        0.3\n"
        "1     2   4   0.03        0.3\n"
        "2     3   4   0.03        0.3\n"
        "3     0   4   0.03        0.3\n";
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST(RunStats2D, GetterPopulatesDuringTheRunAndReportNamesTheSolver) {
    // The AUTO backend is size-gated to the CPU marcher on 4 cells, but an
    // inherited OPENSWMM_2D_BACKEND would override it — pin it.
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "cpu");

    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path inp = kOutDir / "run_stats.inp";
    const fs::path rpt = kOutDir / "run_stats.rpt";
    const fs::path out = kOutDir / "run_stats.out";
    { std::ofstream f(inp); f << buildModel(); }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_NE(eng, nullptr);
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);
    int active = 0;
    ASSERT_EQ(swmm_2d_is_active(eng, &active), SWMM_OK);
    ASSERT_EQ(active, 1);
    ASSERT_EQ(swmm_2d_get_run_stats(eng, nullptr), SWMM_ERR_BADPARAM);

    ASSERT_EQ(swmm_engine_start(eng, 1), SWMM_OK);

    // Right after start: the solver is chosen (label + closure + tiers) and
    // nothing has been stepped yet.
    SWMM_2DRunStats s0{};
    ASSERT_EQ(swmm_2d_get_run_stats(eng, &s0), SWMM_OK);
    EXPECT_EQ(std::strncmp(s0.backend, "cpu", 3), 0) << s0.backend;
    EXPECT_EQ(s0.momentum, 0);      // LOCAL_INERTIAL (deck default)
    EXPECT_EQ(s0.lts_tiers, 2);
    EXPECT_EQ(s0.steps, 0);

    double elapsed = 0.0;
    for (int i = 0; i < 12; ++i) {
        ASSERT_EQ(swmm_engine_step(eng, &elapsed), SWMM_OK);
        if (elapsed <= 0.0) break;
    }

    SWMM_2DRunStats s1{};
    ASSERT_EQ(swmm_2d_get_run_stats(eng, &s1), SWMM_OK);
    EXPECT_GT(s1.steps, 0);
    EXPECT_GT(s1.face_evals, 0);
    EXPECT_GT(s1.last_step, 0.0);
    EXPECT_EQ(s1.n_tiers, 2) << "LTS telemetry populates at the first rebuild";
    long tier_sum = 0;
    for (int k = 0; k < s1.n_tiers; ++k) tier_sum += s1.tier_cells[k];
    EXPECT_GT(tier_sum, 0);
    EXPECT_GE(s1.active_frac_mean, 0.0);
    EXPECT_LE(s1.active_frac_max, 1.0 + 1e-12);

    while (true) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
    }
    ASSERT_EQ(swmm_engine_end(eng), SWMM_OK);
    ASSERT_EQ(swmm_engine_report(eng), SWMM_OK);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);

    const std::string report = readAll(rpt);
    EXPECT_NE(report.find("2D solver: cpu"), std::string::npos)
        << "the solver advisory should reach the report's warning block";
    EXPECT_NE(report.find("LTS_TIERS 2"), std::string::npos);
    EXPECT_NE(report.find("LTS Tier 0 Occupancy"), std::string::npos);
}
