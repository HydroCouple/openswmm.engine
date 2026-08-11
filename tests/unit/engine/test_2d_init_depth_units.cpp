/**
 * @file test_2d_init_depth_units.cpp
 * @brief Unit-system regression for the [2D_TRIANGLES] INIT_DEPTH column.
 *
 * @details INIT_DEPTH is a depth above the cell bed and therefore shares the
 *          mesh's vertical datum — it is authored in the project's display
 *          length units (feet for US FLOW_UNITS, metres for SI), exactly like
 *          the vertex Z column. The 2D solver works in SI, so initialize()
 *          must scale it by the same mesh_to_si factor it applies to vz.
 *
 *          Before the fix, vertex coordinates were scaled but tri_init_depth
 *          was consumed raw as metres: a US-unit deck asking for 1.64 ft of
 *          standing water was initialized with 1.64 m — a 3.28× error in the
 *          initial 2D storage that silently corrupted every downstream depth.
 *
 *          The test states ONE physical initial condition (0.5 m of standing
 *          water on a flat, walled patch) two ways — a CMS deck and its exact
 *          CFS restatement — and asserts both initialize to the same SI depth.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>

namespace fs = std::filesystem;

namespace {

constexpr double M_TO_FT = 3.280839895013123;   // 1 / 0.3048

/// The initial depth under test, in SI metres.
constexpr double INIT_DEPTH_M = 0.5;

/// Build a quiescent 2D-only model: a flat 20×20 m walled patch holding a
/// uniform INIT_DEPTH of standing water, with a trivial (dry, uncoupled) 1D
/// network so the deck is a valid SWMM model. Expressed in CMS (si=true) or
/// in the exact CFS restatement (si=false) — every length-dimensioned field,
/// INIT_DEPTH included, scales by M_TO_FT.
std::string build_model(bool si) {
    const double L = si ? 1.0 : M_TO_FT;   // metre value → display length
    const char* units = si ? "CMS" : "CFS";

    char buf[8192];
    std::snprintf(buf, sizeof(buf),
        "[OPTIONS]\n"
        "FLOW_UNITS           %s\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:02:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         2\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev      MaxDepth  InitDepth  SurDepth  Aponded\n"
        "J1      %.6f  %.6f  0          0         0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev      Type  Gated\n"
        "O1      %.6f  FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To  Length    Roughness  InOffset  OutOffset  InitFlow\n"
        "C1      J1    O1  %.6f  0.013      0         0          0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     Geom1     Geom2  Geom3  Geom4  Barrels\n"
        "C1      CIRCULAR  %.6f  0      0      0      1\n"
        "\n"
        "[2D_OPTIONS]\n"
        "MAX_TIMESTEP     1\n"
        "DRY_DEPTH        0.002\n"
        "REPORT_2D        NO\n"
        "\n"
        "[2D_VERTICES]\n"
        ";;X         Y         Z\n"
        "%.6f  %.6f  %.6f\n"
        "%.6f  %.6f  %.6f\n"
        "%.6f  %.6f  %.6f\n"
        "%.6f  %.6f  %.6f\n"
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N  INIT_DEPTH\n"
        "0     1   2   0.03        %.6f\n"
        "0     2   3   0.03        %.6f\n",
        units,
        1.0 * L, 5.0 * L,                   // J1 invert, maxdepth
        0.0 * L,                            // O1 invert
        50.0 * L,                           // conduit length
        0.50 * L,                           // conduit diameter
        0.0  * L,  0.0 * L, 0.0 * L,        // flat patch at z = 0, 20×20 m
        20.0 * L,  0.0 * L, 0.0 * L,
        20.0 * L, 20.0 * L, 0.0 * L,
        0.0  * L, 20.0 * L, 0.0 * L,
        INIT_DEPTH_M * L,                   // INIT_DEPTH follows mesh units
        INIT_DEPTH_M * L);
    return std::string(buf);
}

struct RunResult {
    bool   ok = false;
    int    n_tri = 0;
    double min_depth = 0.0;   // per-cell depth at t=0 (metres, SI internal)
    double max_depth = 0.0;
    double api_depth = 0.0;   // INIT_DEPTH read back through the C API
};

/// Open + initialize + start the deck and read the t=0 per-cell depths. The
/// patch is flat and walled with a uniform initial depth, so the surface is in
/// equilibrium: the depths read here are the applied initial condition.
RunResult run_model(const std::string& inp_text, const fs::path& dir,
                    const char* tag) {
    RunResult r;
    const fs::path inp = dir / (std::string("initdepth_") + tag + ".inp");
    const fs::path rpt = dir / (std::string("initdepth_") + tag + ".rpt");
    const fs::path out = dir / (std::string("initdepth_") + tag + ".out");
    { std::ofstream f(inp); f << inp_text; }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng); return r;
    }
    // Read back through the C API before initialize() — the getter reports the
    // authored value in MESH units, unscaled.
    swmm_2d_triangle_get_init_depth(eng, 0, &r.api_depth);

    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    if (!active) { swmm_engine_close(eng); swmm_engine_destroy(eng); return r; }
    swmm_2d_triangle_count(eng, &r.n_tri);

    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }

    std::vector<double> depths(static_cast<std::size_t>(r.n_tri));
    if (swmm_2d_get_depths_bulk(eng, depths.data()) == SWMM_OK && r.n_tri > 0) {
        r.min_depth = *std::min_element(depths.begin(), depths.end());
        r.max_depth = *std::max_element(depths.begin(), depths.end());
    }

    swmm_engine_end(eng);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

} // namespace

class InitDepth2DUnitsTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "openswmm_2d_init_depth_units_test";
        fs::create_directories(dir_);
    }
};

// The same physical initial condition, authored in CMS and in CFS, must seed
// the same SI depth. Pre-fix the CFS deck seeded 1.64 m instead of 0.5 m.
TEST_F(InitDepth2DUnitsTest, SiAndUsDecksSeedTheSameInitialDepth) {
    RunResult si = run_model(build_model(true),  dir_, "cms");
    RunResult us = run_model(build_model(false), dir_, "cfs");

    ASSERT_TRUE(si.ok) << "CMS run failed";
    ASSERT_TRUE(us.ok) << "CFS run failed";
    ASSERT_EQ(si.n_tri, us.n_tri);
    ASSERT_GT(si.n_tri, 0);

    // The C API reports mesh units, so the two decks differ there by design.
    EXPECT_NEAR(si.api_depth, INIT_DEPTH_M, 1e-6);
    EXPECT_NEAR(us.api_depth, INIT_DEPTH_M * M_TO_FT, 1e-4);

    // The discriminating assertion: both decks initialize to the same SI depth.
    EXPECT_NEAR(si.max_depth, INIT_DEPTH_M, 1e-3)
        << "CMS deck seeded " << si.max_depth << " m, expected "
        << INIT_DEPTH_M;
    EXPECT_NEAR(us.max_depth, INIT_DEPTH_M, 1e-3)
        << "CFS deck seeded " << us.max_depth << " m, expected "
        << INIT_DEPTH_M << " (feet read as metres?)";

    // Uniform depth on a flat patch — every cell agrees, in both decks.
    EXPECT_NEAR(si.min_depth, si.max_depth, 1e-6);
    EXPECT_NEAR(us.min_depth, us.max_depth, 1e-6);
}
