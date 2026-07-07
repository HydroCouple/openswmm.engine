/**
 * @file test_2d_junction_coupling.cpp
 * @brief End-to-end regression for two-way 1D↔2D JUNCTION coupling.
 *
 * @details A 2D-coupled junction must exchange with the overland surface in two
 *          physical regimes (NodeCoupling.cpp computeCouplingExchange):
 *            • surcharge spill — when the pipe pressurises above its crown the
 *              junction HGL rises above the rim and spills onto the 2D patch
 *              (bidirectional orifice on the head difference); and
 *            • inlet capture — when the pipe has freeboard but the surface is
 *              wet, surface water drains INTO the inlet driven by the surface
 *              ponding depth alone (one-way), and is abstracted from the
 *              surrounding cells.
 *
 *          Both require the junction to be able to surcharge above its crown.
 *          The old code zeroed a coupled node's ponded_area, so setNodeDepth
 *          capped the head at the crown and junction spill could never fire
 *          (the reason test_2d_coupling_units couples an outfall instead). The
 *          fix auto-sets ponded_area to the surrounding 2D-cell footprint and
 *          flags the node so it ponds regardless of the global ALLOW_PONDING.
 *
 *          This model feeds a coupled junction J1 (draining through a small
 *          pipe to a free outfall O1) with a two-phase inflow: high enough to
 *          surcharge and spill onto a flat 10×10 m patch whose bed sits at the
 *          crown, then off so the ponded surface water drains back through the
 *          inlet. ALLOW_PONDING is deliberately OFF to prove the coupled-node
 *          ponding exception. With the fix: J1 surcharges above the crown, the
 *          patch wets, then drains; both continuity ledgers close. Pre-fix the
 *          patch stays dry (peak ~0) and J1 is pinned at the crown.
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D); runs the real coupled
 *          engine through the public C API. Inputs and a summary are written to
 *          ./junction_coupling_out/ (working dir is tests/unit/engine/data) for
 *          review — no temp files (project convention).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

// Coupled junction J1 (crown at 1.0 m above its invert) drains through a narrow
// 0.2 m pipe to free outfall O1. A two-phase inflow surcharges J1 (spill onto
// the patch), then stops (patch drains back through the inlet). The flat 10×10 m
// patch bed sits at z = 1.0 m = J1's crown elevation, so spill/capture happen at
// the rim. ALLOW_PONDING is OFF on purpose: only the 2D-coupled exception lets
// J1's HGL climb above the crown.
std::string build_junction_model() {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:40:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         1\n"
        "ALLOW_PONDING        NO\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
        "J1      0.0   1.0       0          0         0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev   Type  Gated\n"
        "O1     -0.5    FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow\n"
        "C1      J1    O1  30.0    0.013      0         0          0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels\n"
        "C1      CIRCULAR  0.2    0      0      0      1\n"
        "\n"
        "[INFLOWS]\n"
        ";;Node  Constituent  Tseries  Type  Mfactor  Sfactor  Baseline\n"
        "J1      FLOW         IN_TS    FLOW  1.0      1.0\n"
        "\n"
        "[TIMESERIES]\n"
        ";;Name  Time   Value   (surcharge for 12 min, then off so the patch drains)\n"
        "IN_TS   0:00   0.10\n"
        "IN_TS   0:12   0.10\n"
        "IN_TS   0:13   0.0\n"
        "IN_TS   0:40   0.0\n"
        "\n"
        "[2D_OPTIONS]\n"
        "MAX_TIMESTEP     1\n"
        "DRY_DEPTH        0.002\n"
        "COUPLING_CD      0.7\n"
        "LINEAR_SOLVER    GMRES\n"
        "PRECONDITIONER   JACOBI\n"
        "REPORT_2D        NO\n"
        "\n"
        "[2D_VERTICES]\n"
        ";;X      Y      Z   (flat patch at the junction crown elevation, 1.0 m)\n"
        " 0.0    0.0   1.0\n"   // v0 — coupled cell vertex
        "10.0    0.0   1.0\n"   // v1
        "10.0   10.0   1.0\n"   // v2
        " 0.0   10.0   1.0\n"   // v3
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N\n"
        "0     1   2   0.03\n"
        "0     2   3   0.03\n"
        "\n"
        "[2D_VERTEX_NODE_MAP]\n"
        ";;Vertex  Node  Cd   Area\n"
        "0         J1    0.7  1.0\n";
}

struct RunResult {
    bool   ok = false;
    int    n_tri = 0;
    double peak_j1_depth = 0.0;     // max junction depth over the run (m)
    double peak_patch_depth = 0.0;  // max per-cell 2D depth over the run (m)
    double final_patch_depth = 0.0; // max per-cell 2D depth at the last step (m)
    double cont_2d = 0.0;
    double cont_routing = 0.0;
};

RunResult run_junction_model(const fs::path& dir) {
    RunResult r;
    const fs::path inp = dir / "junction_coupling.inp";
    const fs::path rpt = dir / "junction_coupling.rpt";
    const fs::path out = dir / "junction_coupling.out";
    { std::ofstream f(inp); f << build_junction_model(); }

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
    const int j1 = swmm_node_index(eng, "J1");
    if (j1 < 0) { swmm_engine_close(eng); swmm_engine_destroy(eng); return r; }

    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }

    std::vector<double> depths(static_cast<std::size_t>(r.n_tri));
    double elapsed = 0.0;
    while (true) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        double jd = 0.0;
        if (swmm_node_get_depth(eng, j1, &jd) == SWMM_OK)
            r.peak_j1_depth = std::max(r.peak_j1_depth, jd);
        if (swmm_2d_get_depths_bulk(eng, depths.data()) == SWMM_OK) {
            double step_max = 0.0;
            for (double d : depths) step_max = std::max(step_max, d);
            r.peak_patch_depth = std::max(r.peak_patch_depth, step_max);
            r.final_patch_depth = step_max;
        }
    }

    swmm_engine_end(eng);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_get_routing_continuity_error(eng, &r.cont_routing);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

} // namespace

class JunctionCoupling2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::path("junction_coupling_out");
        fs::create_directories(dir_);
    }
};

// Pre-fix this fails: with ponded_area zeroed the junction is pinned at the
// crown, junction spill never fires, and the patch stays dry. Post-fix the HGL
// rises above the crown, the patch wets on surcharge and drains on capture.
TEST_F(JunctionCoupling2DTest, JunctionSpillsAndCapturesAcrossTheCrown) {
    RunResult r = run_junction_model(dir_);

    ASSERT_TRUE(r.ok) << "coupled junction run failed";
    ASSERT_EQ(r.n_tri, 2);

    // (1) THE FIX: the coupled junction HGL rose ABOVE its crown (full_depth =
    //     1.0 m) even though ALLOW_PONDING is off — pre-fix it was pinned at 1.0.
    EXPECT_GT(r.peak_j1_depth, 1.05)
        << "junction HGL did not rise above the crown (peak depth "
        << r.peak_j1_depth << " m) — coupled-node ponding is not engaged";

    // (2) Surcharge spill wetted the 2D patch. Pre-fix: ~0 (spill can't fire).
    EXPECT_GT(r.peak_patch_depth, 0.01)
        << "junction surcharge did not spill onto the 2D mesh (peak depth "
        << r.peak_patch_depth << " m)";

    // (3) Inlet capture drained the ponded surface back through the junction
    //     after the inflow stopped (one-way capture on the surface-depth gradient).
    EXPECT_LT(r.final_patch_depth, 0.5 * r.peak_patch_depth)
        << "surface water was not recaptured through the inlet (final patch depth "
        << r.final_patch_depth << " m vs peak " << r.peak_patch_depth << " m)";

    // (4) Both continuity ledgers close.
    EXPECT_LT(std::abs(r.cont_2d), 0.05)
        << "2D surface continuity error too large: " << r.cont_2d;
    EXPECT_LT(std::abs(r.cont_routing), 0.05)
        << "1D routing continuity error too large: " << r.cont_routing;

    std::ofstream csv(dir_ / "junction_coupling_massbalance.csv");
    csv << "metric,value\n"
        << "peak_j1_depth_m," << r.peak_j1_depth << "\n"
        << "peak_patch_depth_m," << r.peak_patch_depth << "\n"
        << "final_patch_depth_m," << r.final_patch_depth << "\n"
        << "continuity_2d_frac," << r.cont_2d << "\n"
        << "continuity_routing_frac," << r.cont_routing << "\n";
}
