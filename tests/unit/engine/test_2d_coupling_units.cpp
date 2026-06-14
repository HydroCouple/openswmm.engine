/**
 * @file test_2d_coupling_units.cpp
 * @brief Unit-system equivalence test for the 1D↔2D coupling boundary.
 *
 * @details The 2D surface router runs internally in SI (metres, m³/s) while the
 *          1D engine ALWAYS computes internally in feet/cfs — even for SI
 *          (CMS) projects, whose metric inputs the 1D reader converts to feet
 *          on load. The coupling boundary must therefore convert feet⇄metres
 *          for EVERY project, independent of FLOW_UNITS.
 *
 *          A regression (the coupling factors were tied to FLOW_UNITS and
 *          collapsed to 1.0 for SI projects) left every coupled head/depth off
 *          by 3.28× and every exchanged flow/volume off by 35× for CMS models,
 *          corrupting the coupled mass balance.
 *
 *          This test builds ONE physical model — a 1D junction fed by a steady
 *          inflow, surcharging and spilling into a small 2D mesh through a
 *          coupling point — and expresses it twice: once in CMS (metric) and
 *          once in CFS (the exact unit conversion of the same physics). Because
 *          the 2D solver works in metres for both, the per-cell 2D depths and
 *          the 2D continuity error must MATCH between the two runs. They only
 *          diverge if the coupling boundary mishandles units.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D) — runs the real
 *          coupled engine through the public C API.
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

// Exact display-unit conversion factors for an SI→US restatement of the same
// physical model. LENGTH: metres→feet; FLOW: m³/s→cfs (= 1 / Qcf[CMS]).
constexpr double M_TO_FT  = 3.280839895013123;   // 1 / 0.3048
constexpr double CMS_TO_CFS = 35.31466672148859; // 1 / 0.0283168

/// Build the coupled 1D-2D model in either CMS (si=true) or the exact CFS
/// restatement (si=false). Every length-dimensioned field scales by M_TO_FT,
/// every flow by CMS_TO_CFS; dimensionless fields (Manning n, Cd) and the 2D
/// solver options (always SI) are identical in both.
std::string build_model(bool si) {
    const double L = si ? 1.0 : M_TO_FT;   // metre value → display length
    const double Q = si ? 1.0 : CMS_TO_CFS;
    const char* units = si ? "CMS" : "CFS";

    char buf[8192];
    std::snprintf(buf, sizeof(buf),
        "[OPTIONS]\n"
        "FLOW_UNITS           %s\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         2\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev      MaxDepth   InitDepth  SurDepth  Aponded\n"
        "J1      %.6f  %.6f   0          0         0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev      Type  Gated\n"
        "O1      %.6f  FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To  Length     Roughness  InOffset  OutOffset  InitFlow\n"
        "C1      J1    O1  %.6f   0.013      0         0          0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     Geom1     Geom2  Geom3  Geom4  Barrels\n"
        "C1      CIRCULAR  %.6f  0      0      0      1\n"
        "\n"
        "[INFLOWS]\n"
        ";;Node  Constituent  Tseries  Type  Mfactor  Sfactor  Baseline\n"
        "J1      FLOW         \"\"       FLOW  1.0      1.0      %.6f\n"
        "\n"
        "[2D_OPTIONS]\n"
        "MAX_TIMESTEP     2\n"
        "DRY_DEPTH        0.002\n"
        "COUPLING_CD      0.7\n"
        "LINEAR_SOLVER    GMRES\n"
        "PRECONDITIONER   JACOBI\n"
        "REPORT_2D        NO\n"
        "\n"
        "[2D_VERTICES]\n"
        ";;X         Y         Z\n"
        "%.6f  %.6f  %.6f\n"     // v0 (coupled, low point)
        "%.6f  %.6f  %.6f\n"     // v1
        "%.6f  %.6f  %.6f\n"     // v2
        "%.6f  %.6f  %.6f\n"     // v3
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N\n"
        "0     1   2   0.03\n"
        "0     2   3   0.03\n"
        "\n"
        "[2D_VERTEX_NODE_MAP]\n"
        ";;Vertex  Node  Cd   Area\n"
        "0         J1    0.7  %.6f\n",
        units,
        0.0 * L, 0.5 * L,              // J1 invert, maxdepth (low rim → surcharges)
        -1.0 * L,                       // O1 invert
        50.0 * L,                       // conduit length
        0.30 * L,                       // conduit diameter
        0.5 * Q,                        // inflow baseline (steady)
        0.0 * L, 0.0 * L, 0.0 * L,      // v0
        5.0 * L, 0.0 * L, 1.0 * L,      // v1
        5.0 * L, 5.0 * L, 1.0 * L,      // v2
        0.0 * L, 5.0 * L, 1.0 * L,      // v3
        2.5 * L * L);                   // coupling area (m²→ft²)
    return std::string(buf);
}

struct RunResult {
    bool   ok = false;
    int    n_tri = 0;
    double peak_depth = 0.0;   // max per-cell depth (metres, SI internal)
    double continuity = 0.0;   // 2D surface continuity error (fraction)
};

RunResult run_model(const std::string& inp_text, const fs::path& dir,
                    const char* tag) {
    RunResult r;
    const fs::path inp = dir / (std::string("coupling_") + tag + ".inp");
    const fs::path rpt = dir / (std::string("coupling_") + tag + ".rpt");
    const fs::path out = dir / (std::string("coupling_") + tag + ".out");
    { std::ofstream f(inp); f << inp_text; }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng); return r;
    }
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

    // Step to completion, tracking the peak per-cell 2D depth over the run.
    std::vector<double> depths(static_cast<std::size_t>(r.n_tri));
    double elapsed = 0.0;
    while (true) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        if (swmm_2d_get_depths_bulk(eng, depths.data()) == SWMM_OK) {
            for (double d : depths) r.peak_depth = std::max(r.peak_depth, d);
        }
    }

    swmm_engine_end(eng);
    swmm_2d_get_continuity_error(eng, &r.continuity);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

} // namespace

class Coupling2DUnitsTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::temp_directory_path() / "openswmm_coupling_units_test";
        fs::create_directories(dir_);
    }
};

// The same physical coupled model, expressed in CMS and in CFS, must produce
// the same 2D depths and continuity — the 2D solver is SI in both, so the only
// thing that could make them differ is the 1D⇄2D unit conversion.
TEST_F(Coupling2DUnitsTest, SiAndUsRunsAgreeOn2DDepth) {
    RunResult si = run_model(build_model(true),  dir_, "cms");
    RunResult us = run_model(build_model(false), dir_, "cfs");

    ASSERT_TRUE(si.ok) << "CMS run failed";
    ASSERT_TRUE(us.ok) << "CFS run failed";
    ASSERT_EQ(si.n_tri, us.n_tri);

    // Guard against a false pass: coupling must have actually driven water into
    // the 2D domain, otherwise both runs would trivially agree at ~0 depth.
    ASSERT_GT(us.peak_depth, 0.01)
        << "coupling moved no water — test would not discriminate";

    // The discriminating assertion: identical physics ⇒ identical 2D depth.
    // The pre-fix bug made the CMS coupling factors 1.0 (vs the correct ft⇄m),
    // which would blow this far past tolerance.
    EXPECT_NEAR(si.peak_depth, us.peak_depth, 0.02 * us.peak_depth)
        << "CMS peak 2D depth " << si.peak_depth
        << " m disagrees with CFS " << us.peak_depth << " m";

    // Both continuity errors should be small and comparable.
    EXPECT_LT(std::abs(si.continuity), 0.05);
    EXPECT_LT(std::abs(us.continuity), 0.05);
    EXPECT_NEAR(si.continuity, us.continuity, 0.01);
}
