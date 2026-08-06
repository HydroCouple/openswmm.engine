/**
 * @file test_engine_1d_rom_lifecycle.cpp
 * @brief End-to-end 1D network spectral ROM lifecycle through the real
 *        engine: build (buildROM1D), advance (stepRouting), quantile output
 *        (postOutputSnapshot), and the <rpt>.uncertainty.csv sidecar.
 *
 * @details Restores the 1D half of the uncertainty sidecar onto the
 *          explicit-marcher base. The pieces this test exercises did not
 *          exist on this base before: `DWSolver::HSnapshot` (the read-only
 *          Picard-Jacobian view `buildROM1D()`/`stepRouting()` weight the
 *          network Laplacian and re-solve the eigenbasis from), and
 *          `SWMMEngine::rom1d_`/`buildROM1D()`/`computeK1d()` themselves.
 *
 *          Every case here drives the public C API on an ordinary 1D `.inp`
 *          — no direct construction of `SpectralROM1D`/`GraphEigenBasis`,
 *          which is already covered at the unit level. This is the
 *          lifecycle guard: does an `[UNCERTAINTY] 1D ...` line in a real
 *          model actually produce a live, advancing, ordered, spreading
 *          ensemble and a written CSV — not whether the ROM math itself is
 *          correct.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

// A 5-junction chain draining to a free outfall — enough conduits for a
// nontrivial (>= 4 active node) network Laplacian, with a real slope so the
// steady heads are non-uniform (a uniform head field would project to
// ~nothing on the zero-mean retained eigenmodes, same pitfall as the 2D ROM).
std::string buildChainModel(const std::string& extra_sections) {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:20:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         5\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
        "J1      10.0  8.0       1.0        0         0\n"
        "J2       8.0  8.0       1.0        0         0\n"
        "J3       6.0  8.0       1.0        0         0\n"
        "J4       4.0  8.0       1.0        0         0\n"
        "J5       2.0  8.0       1.0        0         0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Gated\n"
        "O1      0.0   FREE  NO\n"
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow\n"
        "C1      J1    J2  200.0   0.013      0         0          2.0\n"
        "C2      J2    J3  200.0   0.013      0         0          2.0\n"
        "C3      J3    J4  200.0   0.013      0         0          2.0\n"
        "C4      J4    J5  200.0   0.013      0         0          2.0\n"
        "C5      J5    O1  200.0   0.013      0         0          2.0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels\n"
        "C1      CIRCULAR  3.0    0      0      0      1\n"
        "C2      CIRCULAR  3.0    0      0      0      1\n"
        "C3      CIRCULAR  3.0    0      0      0      1\n"
        "C4      CIRCULAR  3.0    0      0      0      1\n"
        "C5      CIRCULAR  3.0    0      0      0      1\n"
        "\n"
        "[INFLOWS]\n"
        ";;Node  Constituent  TimeSeries  Type   Mfactor  Sfactor  Baseline  Pattern\n"
        "J1      FLOW         \"\"          FLOW   1.0      1.0      3.0\n"
        + extra_sections;
}

struct RunResult {
    SWMM_Engine eng = nullptr;
    fs::path rpt_path;
};

RunResult driveRun(const fs::path& dir, const std::string& tag,
                   const std::string& extra_sections) {
    RunResult r;
    const fs::path inp = dir / (tag + ".inp");
    r.rpt_path = dir / (tag + ".rpt");
    const fs::path out = dir / (tag + ".out");
    { std::ofstream f(inp); f << buildChainModel(extra_sections); }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), r.rpt_path.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng);
        return r;
    }

    double elapsed = 0.0;
    int n_steps = 0;
    while (swmm_engine_step(eng, &elapsed) == SWMM_OK && elapsed > 0.0) {
        if (++n_steps > 2000) break;   // safety valve
    }
    swmm_engine_end(eng);
    swmm_engine_report(eng);

    r.eng = eng;
    return r;
}

/// Count CSV data rows (excludes the header) and confirm a q05<=q50<=q95 row
/// exists somewhere in the file.
struct CsvSummary {
    int    n_rows = 0;
    bool   ordered_everywhere = true;
    double max_spread = 0.0;
};

CsvSummary summarizeCsv(const fs::path& csv_path) {
    CsvSummary s;
    std::ifstream f(csv_path);
    if (!f) return s;
    std::string line;
    std::getline(f, line);  // header
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string time_s, name, q05s, q50s, q95s;
        std::getline(ss, time_s, ',');
        std::getline(ss, name, ',');
        std::getline(ss, q05s, ',');
        std::getline(ss, q50s, ',');
        std::getline(ss, q95s, ',');
        const double q05 = std::stod(q05s);
        const double q50 = std::stod(q50s);
        const double q95 = std::stod(q95s);
        if (!(q05 <= q50 && q50 <= q95)) s.ordered_everywhere = false;
        s.max_spread = std::max(s.max_spread, q95 - q05);
        ++s.n_rows;
    }
    return s;
}

}  // namespace

// ============================================================================

TEST(SwmmEngine1DRomLifecycle, ExplicitUncertaintySpecBuildsAdvancesAndWritesCsv) {
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);

    auto r = driveRun(dir, "explicit_spec", "\n[UNCERTAINTY]\n1D MANNINGS_N 0.20\n");
    ASSERT_NE(r.eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(r.eng);

    const auto* rom1d = impl->rom1d();
    ASSERT_NE(rom1d, nullptr) << "buildROM1D() never fired for an explicit "
                                 "[UNCERTAINTY] 1D spec";
    EXPECT_TRUE(rom1d->is_ready());
    EXPECT_GT(rom1d->n_kept, 0);
    ASSERT_EQ(rom1d->q05.size(), rom1d->q50.size());
    ASSERT_EQ(rom1d->q50.size(), rom1d->q95.size());

    double max_spread = 0.0;
    for (std::size_t i = 0; i < rom1d->q05.size(); ++i) {
        EXPECT_LE(rom1d->q05[i], rom1d->q50[i]) << "node " << i;
        EXPECT_LE(rom1d->q50[i], rom1d->q95[i]) << "node " << i;
        max_spread = std::max(max_spread, rom1d->q95[i] - rom1d->q05[i]);
    }
    EXPECT_GT(max_spread, 0.0)
        << "ensemble produced no spread anywhere after a 20-minute run with "
           "+-20% Manning's n";

    const fs::path csv = dir / "explicit_spec.uncertainty.csv";
    ASSERT_TRUE(fs::exists(csv)) << "buildROM1D() opened no CSV at " << csv;
    const CsvSummary sum = summarizeCsv(csv);
    EXPECT_GT(sum.n_rows, 0);
    EXPECT_TRUE(sum.ordered_everywhere);
    EXPECT_GT(sum.max_spread, 0.0);

    swmm_engine_close(r.eng);
    swmm_engine_destroy(r.eng);
}

TEST(SwmmEngine1DRomLifecycle, NoUncertaintySpecLeavesRomUnbuilt) {
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);

    auto r = driveRun(dir, "no_spec", "");
    ASSERT_NE(r.eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(r.eng);

    EXPECT_EQ(impl->rom1d(), nullptr)
        << "the 1D ROM must stay off (and cost nothing) when nothing asked for it";
    EXPECT_FALSE(fs::exists(dir / "no_spec.uncertainty.csv"));

    swmm_engine_close(r.eng);
    swmm_engine_destroy(r.eng);
}

TEST(SwmmEngine1DRomLifecycle, SpreadScalesWithPerturbationLevel) {
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);

    auto low  = driveRun(dir, "pert_low",  "\n[UNCERTAINTY]\n1D MANNINGS_N 0.05\n");
    auto high = driveRun(dir, "pert_high", "\n[UNCERTAINTY]\n1D MANNINGS_N 0.35\n");
    ASSERT_NE(low.eng, nullptr);
    ASSERT_NE(high.eng, nullptr);

    auto spreadOf = [](SWMM_Engine e) {
        const auto* rom1d = static_cast<openswmm::SWMMEngine*>(e)->rom1d();
        double m = 0.0;
        if (rom1d)
            for (std::size_t i = 0; i < rom1d->q05.size(); ++i)
                m = std::max(m, rom1d->q95[i] - rom1d->q05[i]);
        return m;
    };
    const double spread_low  = spreadOf(low.eng);
    const double spread_high = spreadOf(high.eng);
    EXPECT_GT(spread_low, 0.0);
    EXPECT_GT(spread_high, spread_low)
        << "a wider Manning's n prior must produce wider bands: low=" << spread_low
        << " high=" << spread_high;

    swmm_engine_close(low.eng);  swmm_engine_destroy(low.eng);
    swmm_engine_close(high.eng); swmm_engine_destroy(high.eng);
}

TEST(SwmmEngine1DRomLifecycle, HSnapshotBecomesValidAfterFirstRoutingStep) {
    // The weighted-Laplacian re-solve (stepRouting -> updateBasis) depends on
    // DWSolver::isHSnapshotValid(); this pins that HSnapshot is actually wired
    // through the real 1D solve, not just compiling.
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);

    auto r = driveRun(dir, "hsnapshot", "\n[UNCERTAINTY]\n1D MANNINGS_N 0.20\n");
    ASSERT_NE(r.eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(r.eng);

    EXPECT_TRUE(impl->router().dwSolver().isHSnapshotValid())
        << "DWSolver::execute() should have captured at least one H snapshot "
           "over a 20-minute DYNWAVE run";
    const auto snap = impl->router().dwSolver().lastConvergedH();
    EXPECT_TRUE(snap.valid);
    EXPECT_GT(snap.n_conduits, 0);
    EXPECT_NE(snap.conduit_off, nullptr);

    swmm_engine_close(r.eng);
    swmm_engine_destroy(r.eng);
}

// ============================================================================
// PR P5 — HSnapshot widened with the regime fields (node_surcharged,
// link_froude). H1/H3/H5 read these from HSnapshot rather than the
// reference-only SWMM_OperatorSnapshot.
// ============================================================================

TEST(SwmmEngine1DRomLifecycle, HSnapshotExposesSurchargeAndFroudeArraysAfterFirstStep) {
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);

    auto r = driveRun(dir, "hsnapshot_regime", "\n[UNCERTAINTY]\n1D MANNINGS_N 0.20\n");
    ASSERT_NE(r.eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(r.eng);
    const auto& solver = impl->router().dwSolver();

    ASSERT_TRUE(solver.isHSnapshotValid());
    const auto snap = solver.lastConvergedH();
    EXPECT_TRUE(snap.valid);

    EXPECT_NE(snap.node_surcharged, nullptr)
        << "HSnapshot must expose the live is_surcharged array (PR P5)";
    EXPECT_NE(snap.link_froude, nullptr)
        << "HSnapshot must expose the live froude_ array (PR P5)";
    EXPECT_EQ(snap.n_nodes, 6)   // J1..J5 + O1
        << "n_nodes must match the full node space, not just active/routed nodes";
    EXPECT_EQ(snap.n_links, 5)  // C1..C5
        << "n_links must match the full link space (conduits here)";

    swmm_engine_close(r.eng);
    swmm_engine_destroy(r.eng);
}

TEST(SwmmEngine1DRomLifecycle, HSnapshotInvalidBeforeFirstExecuteDoesNotClaimReadyRegimeData) {
    // Before the first execute() call, valid==false; callers must not read
    // node_surcharged/link_froude in that state. This mirrors the existing
    // contract for conduit_off/conduit_n1/conduit_n2 and confirms the P5
    // widening didn't relax it.
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);
    const fs::path inp = dir / "hsnapshot_prestep.inp";
    const fs::path rpt = dir / "hsnapshot_prestep.rpt";
    { std::ofstream f(inp); f << buildChainModel("\n[UNCERTAINTY]\n1D MANNINGS_N 0.20\n"); }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);
    // Deliberately no swmm_engine_start()/step() — check the pre-first-execute state.
    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    const auto& solver = impl->router().dwSolver();

    EXPECT_FALSE(solver.isHSnapshotValid());
    const auto snap = solver.lastConvergedH();
    EXPECT_FALSE(snap.valid);

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

TEST(SwmmEngine1DRomLifecycle, HSnapshotForcedSurchargeFlagIsVisibleThroughSnapshot) {
    // node_surcharged is a non-owning pointer into DWSolver::xnode_.is_surcharged
    // (no per-step copy) — forcing the underlying flag via the existing
    // nodeSurchargedFlag() test/scatter accessor must be immediately visible
    // through the snapshot without requiring another execute() call. This
    // is a plumbing/aliasing test, not a claim that this small fixture
    // naturally surcharges.
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);

    auto r = driveRun(dir, "hsnapshot_forced_surcharge",
                      "\n[UNCERTAINTY]\n1D MANNINGS_N 0.20\n");
    ASSERT_NE(r.eng, nullptr);
    auto* impl = static_cast<openswmm::SWMMEngine*>(r.eng);
    auto& solver = impl->router().dwSolver();
    ASSERT_TRUE(solver.isHSnapshotValid());

    // Force node 0's flag on, then read it straight back through the snapshot.
    solver.nodeSurchargedFlag(0) = 1;
    auto snap = solver.lastConvergedH();
    ASSERT_NE(snap.node_surcharged, nullptr);
    EXPECT_EQ(snap.node_surcharged[0], 1)
        << "forced is_surcharged[0] must be visible through the live pointer";

    // And clearing it must also be immediately visible (true aliasing, not a
    // one-shot copy taken at some earlier point).
    solver.nodeSurchargedFlag(0) = 0;
    snap = solver.lastConvergedH();
    EXPECT_EQ(snap.node_surcharged[0], 0);

    swmm_engine_close(r.eng);
    swmm_engine_destroy(r.eng);
}

TEST(SwmmEngine1DRomLifecycle, HSnapshotFroudeIsZeroInStillWater) {
    // A perfectly flat network (equal inverts everywhere, equal initial
    // depths, no inflow) has zero head gradient anywhere -> zero flow ->
    // zero Froude on every conduit. (A sloping-invert version, even with
    // InitFlow=0, is NOT still water: gravity alone drives real flow from
    // the very first step, which is exactly what the first attempt at this
    // fixture got wrong -- measured Froude up to 0.32, not "still".)
    const fs::path dir = fs::current_path() / "1d_rom_lifecycle_out";
    fs::create_directories(dir);
    const std::string still_model =
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         DYNWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             00:20:00\n"
        "REPORT_STEP          00:01:00\n"
        "ROUTING_STEP         5\n"
        "\n"
        "[JUNCTIONS]\n"
        ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
        "J1      0.0   8.0       1.0        0         0\n"
        "J2       0.0  8.0       1.0        0         0\n"
        "J3       0.0  8.0       1.0        0         0\n"
        "J4       0.0  8.0       1.0        0         0\n"
        "J5       0.0  8.0       1.0        0         0\n"
        "\n"
        "[OUTFALLS]\n"
        ";;Name  Elev  Type  Stage  Gated\n"
        "O1      0.0   FIXED 1.0   NO\n"   // matches J5 invert+depth -> no head gradient
        "\n"
        "[CONDUITS]\n"
        ";;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow\n"
        "C1      J1    J2  200.0   0.013      0         0          0.0\n"
        "C2      J2    J3  200.0   0.013      0         0          0.0\n"
        "C3      J3    J4  200.0   0.013      0         0          0.0\n"
        "C4      J4    J5  200.0   0.013      0         0          0.0\n"
        "C5      J5    O1  200.0   0.013      0         0          0.0\n"
        "\n"
        "[XSECTIONS]\n"
        ";;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels\n"
        "C1      CIRCULAR  3.0    0      0      0      1\n"
        "C2      CIRCULAR  3.0    0      0      0      1\n"
        "C3      CIRCULAR  3.0    0      0      0      1\n"
        "C4      CIRCULAR  3.0    0      0      0      1\n"
        "C5      CIRCULAR  3.0    0      0      0      1\n";

    const fs::path inp = dir / "hsnapshot_still.inp";
    const fs::path rpt = dir / "hsnapshot_still.rpt";
    { std::ofstream f(inp); f << still_model; }

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(eng), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(eng, 1), SWMM_OK);

    double elapsed = 0.0;
    int n_steps = 0;
    while (swmm_engine_step(eng, &elapsed) == SWMM_OK && elapsed > 0.0) {
        if (++n_steps > 500) break;
    }
    swmm_engine_end(eng);

    auto* impl = static_cast<openswmm::SWMMEngine*>(eng);
    const auto& solver = impl->router().dwSolver();
    ASSERT_TRUE(solver.isHSnapshotValid());
    const auto snap = solver.lastConvergedH();
    ASSERT_NE(snap.link_froude, nullptr);
    for (int j = 0; j < snap.n_links; ++j) {
        EXPECT_NEAR(snap.link_froude[j], 0.0, 1e-6)
            << "conduit " << j << " should show zero Froude in still water";
    }

    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}
