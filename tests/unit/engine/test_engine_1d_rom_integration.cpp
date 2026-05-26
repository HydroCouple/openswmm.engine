/**
 * @file test_engine_1d_rom_integration.cpp
 * @brief Integration test: 1D spectral ROM engine lifecycle.
 *
 * @details Tests the full pipeline:
 *   parse .inp with [UNCERTAINTY] 1D MANNINGS_N 0.20
 *   → swmm_engine_open → initialize → start → step loop → end → close
 *
 * Verifications:
 *   1. rom1d() is non-null after initialize().
 *   2. is_ready() returns true.
 *   3. Quantile arrays have the correct length (n_active nodes).
 *   4. q05 ≤ q50 ≤ q95 at every active node.
 *   5. All quantiles are ≥ 0.
 *   6. Non-zero spread exists (at least one node with q95 > q05 + ε).
 *
 * Network: linear chain J1–J2–J3–J4–J5 → O1.
 * 5 conduits (C1–C5), 5 active nodes, 1 outfall (excluded from ROM).
 * Forced inflow at J1 to ensure non-trivial depth field.
 *
 * @ingroup engine_integration
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "core/SWMMEngine.hpp"
#include "uncertainty/SpectralROM1D.hpp"

namespace {

// ============================================================================
// Minimal 1D .inp with [UNCERTAINTY] 1D MANNINGS_N 0.20
// ============================================================================
//
// Network: linear chain
//   J1 → C1 → J2 → C2 → J3 → C3 → J4 → C4 → J5 → C5 → O1
//
// Conduit slope ≈ (100–5)/5 = 19 m over 100 m each = 0.19.
// Forced inflow at J1 (DWF + INFLOW) so depths are non-zero by t=60s.
//
static const char* k_inp_1d = R"(
[TITLE]
1D ROM lifecycle integration test

[OPTIONS]
FLOW_UNITS           CMS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2025
START_TIME           00:00:00
REPORT_START_DATE    01/01/2025
REPORT_START_TIME    00:00:00
END_DATE             01/01/2025
END_TIME             00:03:00
REPORT_STEP          00:01:00
ROUTING_STEP         0:00:30
MIN_SURFAREA         1.0
MAX_TRIALS           8
HEAD_TOLERANCE       0.005
MINIMUM_STEP         0.5
THREADS              1

[JUNCTIONS]
;;Name  Elevation  MaxDepth  InitDepth  SurDepth  Aponded
J1      100        5         0.5        0         0
J2       95        5         0.5        0         0
J3       90        5         0.5        0         0
J4       85        5         0.5        0         0
J5       80        5         0.5        0         0

[OUTFALLS]
;;Name  Elevation  Type  Stage  Gated
O1      75         FREE         NO

[CONDUITS]
;;Name  From  To   Length  Roughness  InOffset  OutOffset  InitFlow  MaxFlow
C1      J1    J2   100     0.013      0         0          0         0
C2      J2    J3   100     0.013      0         0          0         0
C3      J3    J4   100     0.013      0         0          0         0
C4      J4    J5   100     0.013      0         0          0         0
C5      J5    O1   100     0.013      0         0          0         0

[XSECTIONS]
;;Link  Shape    Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR 1.0    0      0      0      1
C2      CIRCULAR 1.0    0      0      0      1
C3      CIRCULAR 1.0    0      0      0      1
C4      CIRCULAR 1.0    0      0      0      1
C5      CIRCULAR 1.0    0      0      0      1

[DWF]
;;Node  Constituent  Baseline  Patterns
J1      FLOW         0.5

[REPORT]
INPUT      NO
CONTINUITY YES
FLOWSTATS  YES
CONTROLS   NO
SUBCATCHMENTS NONE
NODES      ALL
LINKS      ALL

[UNCERTAINTY]
1D  MANNINGS_N  0.20
)";

static constexpr const char* k_inp_path = "/tmp/rom1d_lifecycle_test.inp";
static constexpr const char* k_rpt_path = "/tmp/rom1d_lifecycle_test.rpt";

// ============================================================================
// Test fixture
// ============================================================================

class ROM1DLifecycle : public ::testing::Test {
protected:
    SWMM_Engine handle = nullptr;

    void SetUp() override {
        std::ofstream f(k_inp_path);
        f << k_inp_1d;
    }

    void TearDown() override {
        if (handle) {
            swmm_engine_end(handle);
            swmm_engine_close(handle);
            swmm_engine_destroy(handle);
            handle = nullptr;
        }
        std::remove(k_inp_path);
        std::remove(k_rpt_path);
    }

    // Run engine to completion and return true on success
    bool runToEnd() {
        if (swmm_engine_open(handle, k_inp_path, k_rpt_path, nullptr, nullptr) != 0)
            return false;
        if (swmm_engine_initialize(handle) != 0)
            return false;
        if (swmm_engine_start(handle, 0) != 0)
            return false;
        double elapsed = 1.0;
        int steps = 0;
        while (elapsed != 0.0 && steps < 200) {
            if (swmm_engine_step(handle, &elapsed) != 0) break;
            ++steps;
        }
        return true;
    }
};

// ============================================================================
// Tests
// ============================================================================

TEST_F(ROM1DLifecycle, ROM1DIsBuiltAndReady) {
    handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(runToEnd());

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();

    ASSERT_NE(rom, nullptr) << "rom1d() must be non-null when 1D uncertainty is configured";
    EXPECT_TRUE(rom->is_ready()) << "ROM must be ready after initialize()";
}

TEST_F(ROM1DLifecycle, QuantileArraysSized) {
    handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(runToEnd());

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();
    ASSERT_NE(rom, nullptr);

    // 5 junctions → 5 active nodes (O1 excluded as outfall)
    EXPECT_EQ(static_cast<int>(rom->q05.size()), rom->n_nodes);
    EXPECT_EQ(static_cast<int>(rom->q50.size()), rom->n_nodes);
    EXPECT_EQ(static_cast<int>(rom->q95.size()), rom->n_nodes);
    EXPECT_GE(rom->n_nodes, 4) << "Need >= 4 active nodes for GraphEigenBasis";
}

TEST_F(ROM1DLifecycle, QuantileMonotonicity) {
    handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(runToEnd());

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();
    ASSERT_NE(rom, nullptr);

    for (int i = 0; i < rom->n_nodes; ++i) {
        EXPECT_GE(rom->q05[static_cast<std::size_t>(i)], 0.0);
        EXPECT_LE(rom->q05[static_cast<std::size_t>(i)],
                  rom->q50[static_cast<std::size_t>(i)] + 1e-10);
        EXPECT_LE(rom->q50[static_cast<std::size_t>(i)],
                  rom->q95[static_cast<std::size_t>(i)] + 1e-10);
    }
}

TEST_F(ROM1DLifecycle, NonZeroEnsembleSpread) {
    handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(runToEnd());

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();
    ASSERT_NE(rom, nullptr);

    double max_spread = 0.0;
    for (int i = 0; i < rom->n_nodes; ++i) {
        double spread = rom->q95[static_cast<std::size_t>(i)]
                      - rom->q05[static_cast<std::size_t>(i)];
        max_spread = std::max(max_spread, spread);
    }
    EXPECT_GT(max_spread, 1e-8)
        << "±20% Manning's n must produce non-zero head spread";
}

TEST_F(ROM1DLifecycle, FullToActiveMapIsCorrect) {
    handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);
    ASSERT_TRUE(runToEnd());

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();
    ASSERT_NE(rom, nullptr);

    // full_to_active must be sized for all nodes (including outfalls)
    EXPECT_GE(static_cast<int>(rom->full_to_active.size()),
              rom->n_nodes + 1 /*at least one outfall*/);

    // exactly n_nodes entries should be >= 0
    int active_count = 0;
    for (int v : rom->full_to_active) {
        if (v >= 0) ++active_count;
    }
    EXPECT_EQ(active_count, rom->n_nodes);
}

} // anonymous namespace

#ifndef OPENSWMM_HAS_2D
// Stub out when 2D is not compiled (rom1d() still works, but header needs 2D guard)
#endif
