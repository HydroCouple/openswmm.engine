/**
 * @file test_2d_outfall_coupling.cpp
 * @brief End-to-end regression for two-way 1D↔2D OUTFALL coupling.
 *
 * @details A terminal outfall's pipe discharge accumulates into the node's
 *          `inflow` accumulator (water arriving from the connecting conduit),
 *          NOT its `outflow` accumulator (which stays ~0 except on backflow).
 *          The pre-fix `transferOutfallDischarges` read `nodes.outflow`, so the
 *          normal discharge was silently dropped — it never reached the 2D mesh
 *          and the coupled 1D+2D mass balance leaked it every step.
 *
 *          This test builds a 1D junction fed by a steady inflow, draining
 *          through a conduit to a FREE outfall that is coupled to a small 2D
 *          mesh. With the fix the outfall discharge is injected as a source into
 *          the coupled cell(s): the mesh wets, the 2D mass balance records the
 *          discharge under `outfall_in`, and BOTH the 2D surface continuity and
 *          the 1D routing continuity close. On the pre-fix code the mesh stays
 *          dry (peak depth ~0) and `outfall_in` ~0, so the assertions below fail.
 *
 *          Tailwater feedback (updateOutfallBoundaries → setAllOutfallDepths)
 *          throttles the discharge as the 2D cell fills, so the depth
 *          equilibrates rather than growing unbounded — the two-way coupling.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D); runs the real coupled
 *          engine through the public C API. Inputs and a mass-balance summary
 *          are written to ./outfall_coupling_out/ (the test's working dir is the
 *          repo's tests/unit/engine/data), so a reviewer can inspect them — no
 *          temp files (project convention).
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
#include <openswmm/engine/openswmm_massbalance.h>

namespace fs = std::filesystem;

namespace {

/// A 1D junction fed by a steady 0.05 CMS inflow, draining through a circular
/// conduit to a FREE outfall O1. O1 is coupled to vertex 0 of a flat 20×20 m,
/// 2-triangle 2D patch with wall boundaries; the steady discharge ponds on the
/// patch (gentle fill rate keeps the implicit 2D solver stable). All lengths in
/// metres, flows in m³/s (CMS); the 2D solver is SI natively.
std::string build_outfall_model() {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:30:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         2\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
        "J1      1.0   5.0       0          0         0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O1      0.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow\n"
        "C1      J1    O1  50.0    0.013      0         0          0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels\n"
        "C1      CIRCULAR  0.5    0      0      0      1\n"
        "\n"
        "[INFLOWS]\n"
        ";;Node  Constituent  Tseries  Type  Mfactor  Sfactor  Baseline\n"
        "J1      FLOW         \"\"       FLOW  1.0      1.0      0.05\n"
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
        ";;X      Y      Z\n"
        " 0.0    0.0   0.0\n"   // v0 — coupled cell vertex
        "20.0    0.0   0.0\n"   // v1
        "20.0   20.0   0.0\n"   // v2
        " 0.0   20.0   0.0\n"   // v3
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N\n"
        "0     1   2   0.03\n"
        "0     2   3   0.03\n"
        "\n"
        "[2D_VERTEX_NODE_MAP]\n"
        ";;Vertex  Node  Cd   Area\n"
        "0         O1    0.7  2.5\n";
}

struct RunResult {
    bool   ok = false;
    int    n_tri = 0;
    double peak_depth = 0.0;     // max per-cell 2D depth over the run (m)
    double cont_2d = 0.0;        // 2D surface continuity error (fraction)
    double cont_routing = 0.0;   // 1D routing continuity error (fraction)
    double outfall_in = 0.0;     // 2D ledger: 1D→2D outfall discharge (m³)
    double outfall_out = 0.0;    // 2D ledger: 2D→pipe withdrawal (m³, ~0 here)
};

RunResult run_outfall_model(const fs::path& dir) {
    RunResult r;
    const fs::path inp = dir / "outfall_coupling.inp";
    const fs::path rpt = dir / "outfall_coupling.rpt";
    const fs::path out = dir / "outfall_coupling.out";
    { std::ofstream f(inp); f << build_outfall_model(); }

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

    std::vector<double> depths(static_cast<std::size_t>(r.n_tri));
    double elapsed = 0.0;
    while (true) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        if (swmm_2d_get_depths_bulk(eng, depths.data()) == SWMM_OK) {
            for (double d : depths) r.peak_depth = std::max(r.peak_depth, d);
        }
    }

    swmm_engine_end(eng);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_get_routing_continuity_error(eng, &r.cont_routing);

    // 2D ledger terms (outfall_in proves the discharge actually reached 2D).
    double init_s = 0, final_s = 0, rain = 0, c12 = 0, c21 = 0, ofin = 0,
           ofout = 0, bin = 0, bout = 0, evap = 0;
    swmm_2d_get_mass_balance(eng, &init_s, &final_s, &rain, &c12, &c21, &ofin,
                             &ofout, &bin, &bout, &evap);
    r.outfall_in = ofin;
    r.outfall_out = ofout;

    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

} // namespace

class OutfallCoupling2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        // User-visible output dir (working dir is tests/unit/engine/data).
        dir_ = fs::path("outfall_coupling_out");
        fs::create_directories(dir_);
    }
};

// The bug regression: a coupled outfall's pipe discharge must reach the 2D mesh.
// Pre-fix (read of nodes.outflow≈0) leaves the mesh dry and outfall_in at 0.
TEST_F(OutfallCoupling2DTest, OutfallDischargeReaches2DAndContinuityCloses) {
    RunResult r = run_outfall_model(dir_);

    ASSERT_TRUE(r.ok) << "coupled outfall run failed";
    ASSERT_EQ(r.n_tri, 2);

    // (1) The discharge wetted the 2D mesh. Pre-fix: ~0.
    EXPECT_GT(r.peak_depth, 0.1)
        << "outfall discharge did not reach the 2D mesh (peak depth "
        << r.peak_depth << " m) — the transferOutfallDischarges bug is back";

    // (2) The 2D ledger recorded the discharge under outfall_in. Pre-fix: ~0.
    EXPECT_GT(r.outfall_in, 0.0)
        << "2D mass balance recorded no outfall inflow";

    // Pure discharge run: no surface→pipe withdrawal expected.
    EXPECT_NEAR(r.outfall_out, 0.0, 1e-6 * std::max(r.outfall_in, 1.0))
        << "unexpected outfall withdrawal: " << r.outfall_out;

    // (3) Both continuity ledgers close. The new outfall_in/outfall_out terms +
    //     the legacy-faithful 1D backflow booking keep each side conservative.
    EXPECT_LT(std::abs(r.cont_2d), 0.05)
        << "2D surface continuity error too large: " << r.cont_2d;
    EXPECT_LT(std::abs(r.cont_routing), 0.05)
        << "1D routing continuity error too large: " << r.cont_routing;

    // Persist a human-readable summary for review (no temp files).
    std::ofstream csv(dir_ / "outfall_coupling_massbalance.csv");
    csv << "metric,value\n"
        << "n_triangles," << r.n_tri << "\n"
        << "peak_2d_depth_m," << r.peak_depth << "\n"
        << "outfall_in_m3," << r.outfall_in << "\n"
        << "outfall_out_m3," << r.outfall_out << "\n"
        << "continuity_2d_frac," << r.cont_2d << "\n"
        << "continuity_routing_frac," << r.cont_routing << "\n";
}
