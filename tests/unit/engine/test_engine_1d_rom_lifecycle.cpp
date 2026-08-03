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
