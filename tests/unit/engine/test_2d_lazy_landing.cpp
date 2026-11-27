/**
 * @file      test_2d_lazy_landing.cpp
 * @brief     [2D_OPTIONS] LAZY_SOURCE_LANDING (Track 3D, 2026-11-27): REBUILD lands the lazy
 *            tier's held sources at rebuilds, report instants and the run end instead of every
 *            advance; the option parses and round-trips; the run still closes its 2D continuity
 *            and infiltrates the same water as STEP to within a small tolerance.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infil2d.h>

namespace fs = std::filesystem;

namespace {

/// A four-triangle Horton mesh under light rain: every cell infiltrates all of it and stays
/// below the activation depth, so the lazy tier carries the whole source budget.
std::string model(const char* landing) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             01:00:00\n"
         "REPORT_STEP          00:05:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         10\n"
         "ALLOW_PONDING        NO\n"
         "\n[JUNCTIONS]\nJ1      0.0   1.0       0          0         0\n"
         "\n[OUTFALLS]\nO1     -0.5    FREE  NO\n"
         "\n[CONDUITS]\nC1      J1    O1  30.0    0.013      0         0          0\n"
         "\n[XSECTIONS]\nC1      CIRCULAR  0.3    0      0      0      1\n"
         "\n[2D_OPTIONS]\n"
         "MAX_TIMESTEP     2\n"
         "DRY_DEPTH        0.001\n"
         "REPORT_2D        NO\n"
         "LAZY_SOURCE_LANDING "
      << landing
      << "\n"
         "\n[2D_VERTICES]\n"
         " 0.0    0.0   10.0\n"
         "20.0    0.0   10.0\n"
         "20.0   20.0   10.0\n"
         " 0.0   20.0   10.0\n"
         "10.0   10.0   10.0\n"
         "\n[2D_TRIANGLES]\n"
         "0     1   4   0.03        0.0         LAWN\n"
         "1     2   4   0.03        0.0         LAWN\n"
         "2     3   4   0.03        0.0         LAWN\n"
         "3     0   4   0.03        0.0         LAWN\n"
         "\n[2D_INFILTRATION_DEFAULTS]\n"
         "LAWN     HORTON     3.0   0.5   4.14  7.0  0    LOST\n";
    return s.str();
}

struct Outcome {
    bool ok = false;
    double rainfall_in = 0, infil_out = 0, final_storage = 0, cont_2d = 0;
    std::string written;
};

Outcome runModel(const fs::path& dir, const std::string& name, const std::string& text) {
    Outcome r;
    fs::create_directories(dir);
    const fs::path inp = dir / (name + ".inp"), rpt = dir / (name + ".rpt"),
                   out = dir / (name + ".out"), saved = dir / (name + "_lazy_landing_written.inp");
    std::ofstream(inp) << text;
    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                         nullptr) != SWMM_OK ||
        swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_model_write(eng, saved.string().c_str()) == SWMM_OK) {
        std::ifstream f(saved);
        r.written.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    double elapsed = 0.0;
    for (int guard = 0; guard < 100000; ++guard) {
        // 3.6 mm/h for the first half hour: far below the Horton capacity.
        const double rain = (elapsed * 86400.0 < 1800.0) ? 1.0e-6 : 0.0;
        swmm_2d_force_rainfall_uniform(eng, rain, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
    }
    swmm_engine_end(eng);
    double init = 0;
    swmm_2d_get_mass_balance(eng, &init, &r.final_storage, &r.rainfall_in, nullptr, nullptr,
                             nullptr, nullptr, nullptr, nullptr, nullptr);
    swmm_infil2d_get_total_volume(eng, &r.infil_out);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

} // namespace

TEST(LazySourceLanding, RebuildLandingClosesAndMatchesStepWithinTolerance) {
    const fs::path dir = fs::current_path() / "test_2d_lazy_landing_artifacts";
    const Outcome step = runModel(dir, "step", model("STEP"));
    const Outcome rebuild = runModel(dir, "rebuild", model("REBUILD"));
    ASSERT_TRUE(step.ok);
    ASSERT_TRUE(rebuild.ok);
    ASSERT_GT(step.rainfall_in, 0.0);
    EXPECT_NEAR(rebuild.rainfall_in, step.rainfall_in, 1e-9 * step.rainfall_in)
        << "the same forcing rain";
    EXPECT_GT(step.infil_out, 0.25 * step.rainfall_in)
        << "a good part of the light rain infiltrates";
    EXPECT_NEAR(rebuild.infil_out, step.infil_out, 0.02 * step.infil_out)
        << "same held rates, landed in fewer steps";
    EXPECT_LT(std::fabs(step.cont_2d), 0.1) << "2D continuity error (%) under STEP";
    EXPECT_LT(std::fabs(rebuild.cont_2d), 0.1)
        << "2D continuity error (%) under REBUILD: the final batch must land the lazy tier";
    // The option round-trips through the writer only when it is not the default.
    EXPECT_EQ(step.written.find("LAZY_SOURCE_LANDING"), std::string::npos);
    EXPECT_NE(rebuild.written.find("LAZY_SOURCE_LANDING    REBUILD"), std::string::npos)
        << rebuild.written.substr(0, 2000);
}

TEST(LazySourceLanding, UnknownWordIsAnInputError) {
    const fs::path dir = fs::current_path() / "test_2d_lazy_landing_artifacts";
    const Outcome bad = runModel(dir, "bad", model("SOMETIMES"));
    EXPECT_FALSE(bad.ok);
}
