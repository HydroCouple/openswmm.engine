/**
 * @file test_engine_2d_rom_integration.cpp
 * @brief Phase 8 integration test: full engine lifecycle with 2D ROM.
 *
 * @details Tests the complete pipeline:
 *   parse .inp with [2D_VERTICES], [2D_TRIANGLES], [2D_ROM], [UNCERTAINTY]
 *   → swmm_engine_open → initialize → start → step loop → end → close
 *
 * Verifications:
 *   1. 2D module activates (triangle count matches .inp).
 *   2. ROM sidecar is non-null and ready after the first 2D advance.
 *   3. Quantile arrays have the correct shape (n_tri).
 *   4. q05 ≤ q50 ≤ q95 at every cell.
 *   5. Non-zero spread exists (at least one cell with q95 > q05 + ε).
 *   6. Some cells are wet after simulation (system advanced).
 *
 * Spread is driven by non-uniform per-cell rainfall forcing (alternating
 * rates), which projects non-trivially onto the graph Laplacian eigenmodes,
 * combined with ±20% Manning's n LHS perturbation per ensemble member.
 *
 * @ingroup engine_integration
 */

#ifdef OPENSWMM_HAS_2D

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

// Public C API (SWMM_Engine handle, lifecycle, 2D API)
#include "openswmm/engine/openswmm_engine.h"

// Private C++ headers for ROM access via SWMMEngine* cast
#include "core/SWMMEngine.hpp"
#include "2d/uncertainty/SpectralROM.hpp"

namespace {

// ============================================================================
// Minimal 2D+1D .inp content
// ============================================================================
//
// 2D mesh: 5 vertices, 4 triangles on a 3 m × 3 m sloped domain.
//
//   v3(0,3,0.05) ─── v2(3,3,0.10)
//       |      \   /      |
//       |    v4(1.5,1.5,0.12)   |
//       |      /   \      |
//   v0(0,0,0.20) ─── v1(3,0,0.15)
//
// Triangles: t0(0,1,4), t1(1,2,4), t2(2,3,4), t3(3,0,4)
//
// 1D backbone: J1 → C1 → O1 (not coupled to 2D, provides a valid network).
//
// [2D_ROM]: 20 members, 4 modes, ±20% Manning's n.
// No [2D_OPTIONS] OUTPUT_FILE — no HDF5 needed; ROM is read via C++ cast.
//
static const char* k_inp_content = R"(
[TITLE]
Phase 8 — 2D ROM integration test

[OPTIONS]
FLOW_UNITS           CMS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2025
START_TIME           00:00:00
REPORT_START_DATE    01/01/2025
REPORT_START_TIME    00:00:00
END_DATE             01/01/2025
END_TIME             00:05:00
REPORT_STEP          00:01:00
ROUTING_STEP         0:00:30
MIN_SURFAREA         1.0
MAX_TRIALS           8
HEAD_TOLERANCE       0.005
MINIMUM_STEP         0.5
THREADS              1

[JUNCTIONS]
;;Name  Elev   MaxDepth  InitDepth  SurDepth  Aponded
J1      1.0    10.0      0.0        0.0       0

[OUTFALLS]
;;Name  Elev  Type  Stage  Gated
O1      0.0   FREE         NO

[CONDUITS]
;;Name  FromNode  ToNode  Length  Roughness  InOffset  OutOffset
C1      J1        O1      100.0   0.035      0.0       0.0

[XSECTIONS]
;;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR  1.0    0.0    0.0    0.0    1

[2D_OPTIONS]
MAX_TIMESTEP      30.0
MIN_TIMESTEP      0.01
REL_TOLERANCE     1.0e-3
ABS_TOLERANCE     1.0e-5
DRY_DEPTH         0.001
MAX_CVODE_STEPS   5000

[2D_VERTICES]
;;X     Y     Z      TAG
0.0   0.0   0.20   v0
3.0   0.0   0.15   v1
3.0   3.0   0.10   v2
0.0   3.0   0.05   v3
1.5   1.5   0.12   v4

[2D_TRIANGLES]
;;V0  V1  V2  Manning  TAG
0     1   4   0.035    t0
1     2   4   0.035    t1
2     3   4   0.035    t2
3     0   4   0.035    t3

[2D_ROM]
ENABLE    YES
MEMBERS   20
MODES     4

[UNCERTAINTY]
2D  MANNINGS_N  0.20
)";

static std::string write_inp_to_tmp() {
    std::string path = "/tmp/phase8_2d_rom_integration.inp";
    std::ofstream f(path);
    f << k_inp_content;
    return path;
}

} // anonymous namespace


// ============================================================================
// Integration test
// ============================================================================

TEST(EngineIntegration2D, ROMLifecycleAndQuantileMonotonicity) {
    const std::string inp_path = write_inp_to_tmp();
    const std::string rpt_path = "/tmp/phase8_2d_rom_integration.rpt";

    // -------------------------------------------------------------------
    // 1. Open and initialise
    // -------------------------------------------------------------------
    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);

    int rc = swmm_engine_open(handle, inp_path.c_str(), rpt_path.c_str(),
                               nullptr, nullptr);
    ASSERT_EQ(rc, SWMM_OK)
        << "open() failed: " << swmm_get_last_error_msg(handle);

    rc = swmm_engine_initialize(handle);
    ASSERT_EQ(rc, SWMM_OK)
        << "initialize() failed: " << swmm_get_last_error_msg(handle);

    // -------------------------------------------------------------------
    // 2. Verify 2D module activated with correct mesh
    // -------------------------------------------------------------------
    int is_active = 0;
    ASSERT_EQ(swmm_2d_is_active(handle, &is_active), SWMM_OK);
    ASSERT_TRUE(is_active) << "2D module should be active after initialize()";

    int n_tri = 0;
    ASSERT_EQ(swmm_2d_triangle_count(handle, &n_tri), SWMM_OK);
    ASSERT_EQ(n_tri, 4) << "Mesh should have exactly 4 triangles";

    int n_vert = 0;
    ASSERT_EQ(swmm_2d_vertex_count(handle, &n_vert), SWMM_OK);
    ASSERT_EQ(n_vert, 5) << "Mesh should have exactly 5 vertices";

    // -------------------------------------------------------------------
    // 3. Start (no .out file — ROM is read directly via C++ cast)
    // -------------------------------------------------------------------
    rc = swmm_engine_start(handle, /*save_results=*/0);
    ASSERT_EQ(rc, SWMM_OK)
        << "start() failed: " << swmm_get_last_error_msg(handle);

    // -------------------------------------------------------------------
    // 4. Force non-uniform per-cell rainfall to seed non-trivial ROM state.
    //
    //    Alternating pattern (odd/even triangles get different rates):
    //      t0, t2 → 8e-5 m/s  (≈ 29 cm/hr)
    //      t1, t3 → 2e-5 m/s  (≈  7 cm/hr)
    //
    //    This spatial gradient is non-constant and projects non-trivially
    //    onto the graph Laplacian eigenmodes (r_coarse[j] ≠ 0), giving the
    //    ROM members different forcing magnitudes per Manning's n member.
    //    The result is non-zero spread in q05/q95 after the first advance.
    // -------------------------------------------------------------------
    for (int i = 0; i < n_tri; ++i) {
        double rate = (i % 2 == 0) ? 8.0e-5 : 2.0e-5;
        rc = swmm_2d_force_rainfall(handle, i, rate,
                                     SWMM_FORCING_OVERRIDE,
                                     SWMM_FORCING_PERSIST);
        ASSERT_EQ(rc, SWMM_OK) << "rainfall forcing failed for cell " << i;
    }

    // -------------------------------------------------------------------
    // 5. Step through the simulation
    // -------------------------------------------------------------------
    double elapsed = 1.0;
    int n_steps = 0;
    while (elapsed > 0.0) {
        rc = swmm_engine_step(handle, &elapsed);
        ASSERT_EQ(rc, SWMM_OK)
            << "step() failed at step " << n_steps
            << ": " << swmm_get_last_error_msg(handle);
        ++n_steps;
        if (n_steps > 2000) break; // safety guard
    }
    EXPECT_GT(n_steps, 0) << "Should have advanced at least one routing step";

    // -------------------------------------------------------------------
    // 6. Read ROM quantiles via C++ cast (SWMM_Engine is SWMMEngine*)
    // -------------------------------------------------------------------
    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const openswmm::twoD::SpectralROM* rom =
        eng->surfaceRouter2D().rom();

    ASSERT_NE(rom, nullptr) << "ROM sidecar should be non-null after simulation";
    ASSERT_TRUE(rom->is_ready()) << "ROM should be seeded (is_ready() == true)";

    // -------------------------------------------------------------------
    // 7. Quantile arrays have correct shape
    // -------------------------------------------------------------------
    ASSERT_EQ(static_cast<int>(rom->q05.size()), n_tri)
        << "q05 size should match triangle count";
    ASSERT_EQ(static_cast<int>(rom->q50.size()), n_tri)
        << "q50 size should match triangle count";
    ASSERT_EQ(static_cast<int>(rom->q95.size()), n_tri)
        << "q95 size should match triangle count";

    // -------------------------------------------------------------------
    // 8. q05 ≤ q50 ≤ q95 at every cell; all ≥ 0
    // -------------------------------------------------------------------
    for (int i = 0; i < n_tri; ++i) {
        auto ui = static_cast<std::size_t>(i);
        EXPECT_LE(rom->q05[ui], rom->q50[ui] + 1e-12)
            << "q05 > q50 at cell " << i;
        EXPECT_LE(rom->q50[ui], rom->q95[ui] + 1e-12)
            << "q50 > q95 at cell " << i;
        EXPECT_GE(rom->q05[ui], 0.0)
            << "q05 < 0 at cell " << i;
    }

    // -------------------------------------------------------------------
    // 9. Non-zero ensemble spread exists at at least one cell.
    //
    //    Non-uniform rainfall → r_coarse[j] ≠ 0 → ROM members diverge
    //    due to their different Manning's n decay rates.
    // -------------------------------------------------------------------
    bool any_spread = false;
    for (int i = 0; i < n_tri; ++i) {
        auto ui = static_cast<std::size_t>(i);
        if (rom->q95[ui] - rom->q05[ui] > 1e-8) {
            any_spread = true;
            break;
        }
    }
    EXPECT_TRUE(any_spread)
        << "Non-uniform rainfall + Manning's n perturbation should produce "
           "non-zero ensemble spread (q95 - q05 > 1e-8 at at least one cell)";

    // -------------------------------------------------------------------
    // 10. Some cells are wet (CVODE advanced and built up depth)
    // -------------------------------------------------------------------
    bool any_wet = false;
    for (int i = 0; i < n_tri; ++i) {
        double d = 0.0;
        swmm_2d_get_depth(handle, i, &d);
        if (d > 1e-3) { any_wet = true; break; }
    }
    EXPECT_TRUE(any_wet)
        << "At least one cell should be wet (d > 1 mm) after 5 minutes of rainfall";

    // -------------------------------------------------------------------
    // Teardown
    // -------------------------------------------------------------------
    swmm_engine_end(handle);
    swmm_engine_close(handle);
    swmm_engine_destroy(handle);

    std::remove(inp_path.c_str());
    std::remove(rpt_path.c_str());
}

#endif // OPENSWMM_HAS_2D
