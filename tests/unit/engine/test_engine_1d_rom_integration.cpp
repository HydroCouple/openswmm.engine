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

// ============================================================================
// Dry-start: zero initial depth, large inflow → reseed fires + spread exists
// ============================================================================

static const char* k_inp_dry = R"(
[TITLE]
1D ROM dry-start reseed test

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
J1      100        5         0          0         0
J2       95        5         0          0         0
J3       90        5         0          0         0
J4       85        5         0          0         0
J5       80        5         0          0         0

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
J1      FLOW         2.0

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

static constexpr const char* k_dry_inp_path = "/tmp/rom1d_dry_test.inp";
static constexpr const char* k_dry_rpt_path = "/tmp/rom1d_dry_test.rpt";

TEST(DryStart, NetworkBuildsSpread) {
    std::ofstream f(k_dry_inp_path);
    f << k_inp_dry;
    f.close();

    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);

    auto cleanup = [&] {
        if (handle) {
            swmm_engine_end(handle);
            swmm_engine_close(handle);
            swmm_engine_destroy(handle);
        }
        std::remove(k_dry_inp_path);
        std::remove(k_dry_rpt_path);
    };

    ASSERT_EQ(swmm_engine_open(handle, k_dry_inp_path, k_dry_rpt_path, nullptr, nullptr), 0);
    ASSERT_EQ(swmm_engine_initialize(handle), 0);
    ASSERT_EQ(swmm_engine_start(handle, 0), 0);

    double elapsed = 1.0;
    int steps = 0;
    while (elapsed != 0.0 && steps < 200) {
        if (swmm_engine_step(handle, &elapsed) != 0) break;
        ++steps;
    }

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();
    ASSERT_NE(rom, nullptr);
    EXPECT_TRUE(rom->is_ready());

    // Spread must be non-zero — dry start with large inflow drives heads from
    // zero; Manning perturbations produce measurable head differences.
    double max_spread = 0.0;
    for (int i = 0; i < rom->n_nodes; ++i) {
        double s = rom->q95[static_cast<std::size_t>(i)]
                 - rom->q05[static_cast<std::size_t>(i)];
        max_spread = std::max(max_spread, s);
    }
    EXPECT_GT(max_spread, 1e-6)
        << "Dry-start network with 2 m³/s inflow must produce non-zero spread";

    cleanup();
}

// ============================================================================
// ROM quality: zero perturbation → near-zero spread
// ============================================================================
//
// With MANNINGS_N 0.00, all ensemble members get the same Manning's n
// (mannings_mult[i] = 1.0 for all i).  All members are seeded identically
// and advance with the same rate → q05 = q50 = q95 everywhere.
//
// The CSV q50 values (DynWave head) must be above the invert at the upstream
// node (J1 invert = 100 m, DWF = 0.5 m³/s → head > 100 m by t=60 s).
//
static const char* k_inp_zero_pert = R"(
[TITLE]
1D ROM zero-perturbation bias test

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
1D  MANNINGS_N  0.00
)";

static constexpr const char* k_zero_pert_inp = "/tmp/rom1d_zero_pert_test.inp";
static constexpr const char* k_zero_pert_rpt = "/tmp/rom1d_zero_pert_test.rpt";

TEST(ROMQuality, ZeroPerturbationGivesNearZeroSpread) {
    std::ofstream f(k_zero_pert_inp);
    f << k_inp_zero_pert;
    f.close();

    SWMM_Engine handle = swmm_engine_create();
    ASSERT_NE(handle, nullptr);

    auto cleanup = [&] {
        if (handle) {
            swmm_engine_end(handle);
            swmm_engine_close(handle);
            swmm_engine_destroy(handle);
        }
        std::remove(k_zero_pert_inp);
        std::remove(k_zero_pert_rpt);
        std::remove("/tmp/rom1d_zero_pert_test.uncertainty.csv");
    };

    ASSERT_EQ(swmm_engine_open(handle, k_zero_pert_inp, k_zero_pert_rpt, nullptr, nullptr), 0);
    ASSERT_EQ(swmm_engine_initialize(handle), 0);
    ASSERT_EQ(swmm_engine_start(handle, 0), 0);

    double elapsed = 1.0;
    int steps = 0;
    while (elapsed != 0.0 && steps < 200) {
        if (swmm_engine_step(handle, &elapsed) != 0) break;
        ++steps;
    }
    swmm_engine_end(handle);

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();

    // With perturbation=0.0, is_active() returns false → ROM is not built.
    // No ROM means no ensemble → spread is trivially zero (intent satisfied).
    if (rom == nullptr) {
        cleanup();
        return;
    }

    ASSERT_TRUE(rom->is_ready());

    // With 0% perturbation all LHS members have mannings_mult = 1.0.
    // All members seed and advance identically → q05 = q50 = q95 within
    // floating-point noise (< 1e-9 m relative spread).
    double max_spread = 0.0;
    double max_q50    = 0.0;
    for (int i = 0; i < rom->n_nodes; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        double spread = rom->q95[ui] - rom->q05[ui];
        max_spread = std::max(max_spread, spread);
        max_q50    = std::max(max_q50,    rom->q50[ui]);
    }
    EXPECT_LT(max_spread, 1e-9)
        << "0% perturbation must yield zero ensemble spread";
    EXPECT_GT(max_q50, 100.0)
        << "DynWave head at J1 should be above invert (100 m) after 60 s";

    cleanup();
}

// ============================================================================
// ROM quality: higher perturbation → larger spread
// ============================================================================
//
// Run twice on the same network: once with MANNINGS_N 0.10, once with 0.20.
// The larger perturbation must produce strictly larger max ensemble spread.
//
static double runAndGetMaxSpread(const char* inp_body,
                                  const char* inp_path,
                                  const char* rpt_path) {
    std::ofstream f(inp_path);
    f << inp_body;
    f.close();

    SWMM_Engine handle = swmm_engine_create();
    if (!handle) return -1.0;

    auto cleanup = [&] {
        swmm_engine_end(handle);
        swmm_engine_close(handle);
        swmm_engine_destroy(handle);
        std::remove(inp_path);
        std::remove(rpt_path);
    };

    if (swmm_engine_open(handle, inp_path, rpt_path, nullptr, nullptr) != 0) { cleanup(); return -1.0; }
    if (swmm_engine_initialize(handle) != 0) { cleanup(); return -1.0; }
    if (swmm_engine_start(handle, 0) != 0) { cleanup(); return -1.0; }

    double elapsed = 1.0;
    int steps = 0;
    while (elapsed != 0.0 && steps < 200) {
        if (swmm_engine_step(handle, &elapsed) != 0) break;
        ++steps;
    }
    swmm_engine_end(handle);

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto* rom = eng->rom1d();
    double max_spread = 0.0;
    if (rom && rom->is_ready()) {
        for (int i = 0; i < rom->n_nodes; ++i) {
            const auto ui = static_cast<std::size_t>(i);
            max_spread = std::max(max_spread, rom->q95[ui] - rom->q05[ui]);
        }
    }
    cleanup();
    return max_spread;
}

static std::string inpWithPert(double pert) {
    // Swap the MANNINGS_N perturbation value in k_inp_1d
    std::string inp = k_inp_1d;
    const std::string old_line = "1D  MANNINGS_N  0.20";
    const std::string new_line = "1D  MANNINGS_N  " + std::to_string(pert).substr(0, 4);
    auto pos = inp.find(old_line);
    if (pos != std::string::npos) inp.replace(pos, old_line.size(), new_line);
    return inp;
}

TEST(ROMQuality, SpreadScalesWithPerturbationLevel) {
    const std::string inp_lo = inpWithPert(0.10);
    const std::string inp_hi = inpWithPert(0.20);

    const double spread_lo = runAndGetMaxSpread(
        inp_lo.c_str(),
        "/tmp/rom1d_pert_lo.inp", "/tmp/rom1d_pert_lo.rpt");
    const double spread_hi = runAndGetMaxSpread(
        inp_hi.c_str(),
        "/tmp/rom1d_pert_hi.inp", "/tmp/rom1d_pert_hi.rpt");

    ASSERT_GE(spread_lo, 0.0) << "Low-pert run failed";
    ASSERT_GE(spread_hi, 0.0) << "High-pert run failed";

    EXPECT_GT(spread_hi, spread_lo)
        << "20% Manning perturbation must produce larger spread than 10%"
        << "  (spread_lo=" << spread_lo << ", spread_hi=" << spread_hi << ")";
}

} // anonymous namespace

#ifndef OPENSWMM_HAS_2D
// Stub out when 2D is not compiled (rom1d() still works, but header needs 2D guard)
#endif
