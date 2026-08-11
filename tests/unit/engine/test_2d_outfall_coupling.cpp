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
#include <openswmm/engine/openswmm_nodes.h>

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

// ============================================================================
// Buried-outfall drain-recovery regression (the dry-threshold deadlock).
// ============================================================================
//
// The model above sits on a FLAT mesh at Z=0 matching the outfall invert, so
// bed_z == z_inv and the wet/dry-gate bug never fires. The real failure needs a
// BURIED outfall: the 2D cell bed (ground surface) is ABOVE the outfall invert
// because the pipe daylights at the surface (the common physical case). Here the
// patch sits at Z=3.0 m while O1's invert is 0.0 m.
//
// Pre-fix, updateOutfallBoundaries gated the 2D tailwater override on a private
// 0.1 mm threshold — smaller than the 2D solver's own dry_depth. After the storm
// the patch drains to a residual film at/just below dry_depth that the surface
// solver treats as immovable (dry), yet the coupling still read it as wet and
// cached h_2d = bed_z + film ≈ 3 m as the outfall tailwater. That phantom 3 m
// stage pinned the downstream BC, so the upstream junction J1 could not drain
// below ~2 m (head 3 m − invert 1 m) — a deadlock, even though the surface is
// effectively dry.
//
// The fix shares the solver's dry_depth and ramps the override to zero at/below
// it (free discharge when dry), so once the patch drains the outfall frees and
// J1 empties. A two-phase inflow wets the patch, then stops so it drains; the
// NORMAL_FLOW outflow edges let the ponded water leave the domain.
std::string build_buried_outfall_model() {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
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
        "J1      FLOW         IN_TS    FLOW  1.0      1.0\n"
        "\n"
        "[TIMESERIES]\n"
        ";;Name  Time   Value      (0.05 CMS for 10 min, then off so the patch drains)\n"
        "IN_TS   0:00   0.05\n"
        "IN_TS   0:10   0.05\n"
        "IN_TS   0:11   0.0\n"
        "IN_TS   1:00   0.0\n"
        "\n"
        "[2D_OPTIONS]\n"
        "MAX_TIMESTEP     2\n"
        "DRY_DEPTH        0.002\n"
        "COUPLING_CD      0.7\n"
        "REPORT_2D        NO\n"
        "\n"
        "[2D_VERTICES]\n"
        ";;X      Y      Z   (flat patch elevated 3 m above the outfall invert)\n"
        " 0.0    0.0   3.0\n"   // v0 — coupled cell vertex (bed_z = 3 m > z_inv = 0)
        "20.0    0.0   3.0\n"   // v1
        "20.0   20.0   3.0\n"   // v2
        " 0.0   20.0   3.0\n"   // v3
        "\n"
        "[2D_TRIANGLES]\n"
        ";;V1  V2  V3  MANNINGS_N\n"
        "0     1   2   0.03\n"   // T0
        "0     2   3   0.03\n"   // T1
        "\n"
        "[2D_BOUNDARY_CONDITIONS]\n"
        ";;TRI  EDGE  TYPE         SLOPE   (open edges away from v0 so the patch drains)\n"
        "0      0     NORMAL_FLOW  0.05\n"   // T0 edge0 = v1-v2 (right, x=20)
        "1      0     NORMAL_FLOW  0.05\n"   // T1 edge0 = v2-v3 (top,   y=20)
        "\n"
        "[2D_VERTEX_NODE_MAP]\n"
        ";;Vertex  Node  Cd   Area\n"
        "0         O1    0.7  2.5\n";
}

struct BuriedRunResult {
    bool   ok = false;
    double peak_depth = 0.0;        // max per-cell 2D depth over the run (m)
    double final_j1_depth = 0.0;    // upstream junction depth at the last step (m)
    double final_o1_depth = 0.0;    // outfall node depth at the last step (m)
    double final_cell_depth = 0.0;  // coupled-cell 2D depth at the last step (m)
    double cont_2d = 0.0;
    double cont_routing = 0.0;
};

BuriedRunResult run_buried_outfall_model(const fs::path& dir) {
    BuriedRunResult r;
    const fs::path inp = dir / "buried_outfall.inp";
    const fs::path rpt = dir / "buried_outfall.rpt";
    const fs::path out = dir / "buried_outfall.out";
    { std::ofstream f(inp); f << build_buried_outfall_model(); }

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
    int n_tri = 0;
    swmm_2d_triangle_count(eng, &n_tri);
    const int j1 = swmm_node_index(eng, "J1");
    const int o1 = swmm_node_index(eng, "O1");
    if (j1 < 0 || o1 < 0) { swmm_engine_close(eng); swmm_engine_destroy(eng); return r; }

    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }

    std::vector<double> depths(static_cast<std::size_t>(n_tri));
    double elapsed = 0.0;
    while (true) {
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0) break;
        if (swmm_2d_get_depths_bulk(eng, depths.data()) == SWMM_OK) {
            for (double d : depths) r.peak_depth = std::max(r.peak_depth, d);
            r.final_cell_depth = depths[0];  // T0 contains coupled vertex v0
        }
    }
    swmm_node_get_depth(eng, j1, &r.final_j1_depth);
    swmm_node_get_depth(eng, o1, &r.final_o1_depth);

    swmm_engine_end(eng);
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_get_routing_continuity_error(eng, &r.cont_routing);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = true;
    return r;
}

// Pre-fix this deadlocks: the residual film pins the outfall at the ~3 m bed
// elevation and J1 cannot drain below ~2 m. Post-fix the outfall frees once the
// surface dries and J1 empties.
TEST_F(OutfallCoupling2DTest, BuriedOutfallFreesAndJunctionDrainsWhenSurfaceDries) {
    BuriedRunResult r = run_buried_outfall_model(dir_);

    ASSERT_TRUE(r.ok) << "buried-outfall coupled run failed";

    // (1) The storm wetted the patch above the dry threshold.
    EXPECT_GT(r.peak_depth, 0.002)
        << "outfall discharge never wetted the patch (peak depth "
        << r.peak_depth << " m)";

    // (2) After the inflow stops the patch drains to a near-dry film.
    EXPECT_LT(r.final_cell_depth, 0.05)
        << "coupled cell never drained (final depth " << r.final_cell_depth
        << " m) — cannot exercise the dry-gate path";

    // (3) THE BUG: with the surface dry the outfall must use free discharge,
    //     NOT the phantom ~3 m bed tailwater, so the upstream junction drains.
    //     Pre-fix J1 is pinned at ~2 m (head 3 m − invert 1 m).
    constexpr double bed_above_invert = 3.0;  // bed_z (3 m) − z_inv (0 m)
    EXPECT_LT(r.final_j1_depth, 0.5)
        << "upstream junction did not drain (J1 depth " << r.final_j1_depth
        << " m) — the outfall is pinned by a phantom dry-film tailwater";
    EXPECT_LT(r.final_o1_depth, 0.5 * bed_above_invert)
        << "outfall stage pinned near bed elevation (O1 depth "
        << r.final_o1_depth << " m) instead of free discharge";

    // (4) Both continuity ledgers still close.
    EXPECT_LT(std::abs(r.cont_2d), 0.05)
        << "2D surface continuity error too large: " << r.cont_2d;
    EXPECT_LT(std::abs(r.cont_routing), 0.05)
        << "1D routing continuity error too large: " << r.cont_routing;

    std::ofstream csv(dir_ / "buried_outfall_massbalance.csv");
    csv << "metric,value\n"
        << "peak_2d_depth_m," << r.peak_depth << "\n"
        << "final_cell_depth_m," << r.final_cell_depth << "\n"
        << "final_j1_depth_m," << r.final_j1_depth << "\n"
        << "final_o1_depth_m," << r.final_o1_depth << "\n"
        << "continuity_2d_frac," << r.cont_2d << "\n"
        << "continuity_routing_frac," << r.cont_routing << "\n";
}

// ---------------------------------------------------------------------------
// Carved-vertex phantom-tailwater regression (Bellinge in miniature).
//
// GUI-written meshes place the outfall's coupling vertex at the pipe INVERT,
// below the surrounding terrain vertices. The pseudo-Laplacian vert_head
// averages neighbour CELL heads and ignores the vertex's own z, so on a
// bone-dry mesh the carved vertex reads vert_head ≈ terrain → phantom depth
// equal to the bed relief → updateOutfallBoundaries reported the outfall as
// wet (ramp_2d = 1) and pinned its stage at terrain level with ZERO water
// anywhere. Every outfall in the model then chokes/reverses from the first
// step, the network floods, and the 1D routing step collapses to its floor.
//
// The model here has NO inflow at all: everything must stay dry, and the
// outfall must keep its FREE (zero-depth) boundary. Pre-fix, O1's depth reads
// the bed relief (~1.33 m = mean neighbour-cell head above the carved vertex)
// the whole run, and the phantom tailwater drives backflow that withdraws
// water from an empty mesh (negative 2D storage).
TEST_F(OutfallCoupling2DTest, CarvedOutfallVertexStaysDryOnDryMesh) {
    const fs::path inp = dir_ / "carved_outfall.inp";
    const fs::path rpt = dir_ / "carved_outfall.rpt";
    { std::ofstream f(inp); f <<
        "[OPTIONS]\n"
        "FLOW_UNITS           CMS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:10:00\n"
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
        "[2D_OPTIONS]\n"
        "MAX_TIMESTEP     2\n"
        "DRY_DEPTH        0.002\n"
        "COUPLING_CD      0.7\n"
        "REPORT_2D        NO\n"
        "\n"
        // v0 is the outfall vertex, carved 2 m below the surrounding terrain
        // (mimics a GUI mesh that stamps the pipe invert into the vertex).
        "[2D_VERTICES]\n"
        ";;X      Y      Z\n"
        " 0.0    0.0   0.0\n"
        "20.0    0.0   2.0\n"
        "20.0   20.0   2.0\n"
        " 0.0   20.0   2.0\n"
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

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);
    int active = 0;
    swmm_2d_is_active(eng, &active);
    ASSERT_TRUE(active);
    ASSERT_EQ(swmm_engine_start(eng, 1), SWMM_OK);

    const int o1 = swmm_node_index(eng, "O1");
    ASSERT_GE(o1, 0);

    double max_o1_depth = 0.0;
    double elapsed = 0.0;
    while (swmm_engine_step(eng, &elapsed) == SWMM_OK && elapsed > 0.0) {
        double d = 0.0;
        if (swmm_node_get_depth(eng, o1, &d) == SWMM_OK)
            max_o1_depth = std::max(max_o1_depth, d);
    }
    swmm_engine_end(eng);

    double init_s = 0, final_s = 0, rain = 0, c12 = 0, c21 = 0, ofin = 0,
           ofout = 0, bin = 0, bout = 0, evap = 0;
    swmm_2d_get_mass_balance(eng, &init_s, &final_s, &rain, &c12, &c21, &ofin,
                             &ofout, &bin, &bout, &evap);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);

    // THE BUG: with zero water anywhere, the outfall stage must stay at its
    // free (dry) condition. Pre-fix it is pinned at the phantom bed-relief
    // tailwater (~1.33 m here) for the entire run.
    EXPECT_LT(max_o1_depth, 0.02)
        << "outfall depth " << max_o1_depth << " m on a bone-dry model — "
        << "phantom tailwater from the carved coupling vertex";

    // No exchange in either direction, and the (empty) surface must not have
    // been drawn negative by phantom backflow.
    EXPECT_NEAR(ofin, 0.0, 1e-6);
    EXPECT_NEAR(ofout, 0.0, 1e-6);
    EXPECT_GE(final_s, -1e-6)
        << "2D storage went negative on a dry mesh (withdrawal from nothing)";
}
