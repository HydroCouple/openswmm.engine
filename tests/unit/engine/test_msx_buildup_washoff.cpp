/**
 * @file test_msx_buildup_washoff.cpp
 * @brief BW-MSX (2026-09-19) — the reactions component's (MSX) species build
 *        up on land uses and wash off with runoff, by the same relations and
 *        arithmetic as pollutants.
 *
 * Design: plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md §8.9.
 *
 * Gates:
 *   a. SameParametersSameSurfaceLoads — an MSX species X (units MG) with the
 *      pollutant TSS's [BUILDUP]/[WASHOFF] rows and DRY_DAYS produces the
 *      SAME per-subcatchment washoff load (user mass) bit-for-bit, and its
 *      buildup store ends at TSS's per-land-use buildup bit-for-bit.
 *   b. LoadReachesTheNetwork — under LEGACY and EULERIAN_ARD the outlet
 *      junction's X concentration tracks TSS's (same load, same water)
 *      within a tight relative band once flow is established.
 *   c. LedgerCloses — init + net buildup − washed − swept − BMP == final
 *      store (user mass) to 1e-10 relative, with sweeping on.
 *   d. PollutantsAreBitIdenticalWithMsxRowsPresent — adding the X rows
 *      leaves every TSS trajectory and the TSS ledger bit-identical.
 *   e. RoundTripThroughWriterAndCApi — the writer emits the X rows, a
 *      reopened deck resolves them, and the C API reads them through the
 *      surface-species index (n_pollutants + m); a [LOADINGS] X row is
 *      applied as initial buildup.
 *   f. UnknownConstituentWarnsAndRuns — a row naming neither a pollutant nor
 *      a species produces one warning and the run proceeds.
 *   g. MsxOnlyDeckBuildsUpAndWashesOff — no [POLLUTANTS] at all.
 *
 * Artefacts: the decks/results this test writes land in the working
 * directory; run it from tests/output/msx_buildup_washoff (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_quality.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"

namespace {

openswmm::SWMMEngine& cpp(SWMM_Engine e) { return *static_cast<openswmm::SWMMEngine*>(e); }

// One 10-acre subcatchment, 50 % impervious, under 1 in/hr for an hour,
// draining J1 -> O1. Land use LU1 covers it fully.
std::string deck(const std::string& options_extra, const std::string& quality,
                 const std::string& process_components,
                 const std::string& landuse_line = "LU1  0  0  0\n") {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\nINFILTRATION         HORTON\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             03:00:00\n"
         "REPORT_STEP          00:05:00\nWET_STEP             00:05:00\nDRY_STEP             00:05:00\n"
         "ROUTING_STEP         0:00:30\nDRY_DAYS             5\n"
      << options_extra
      << "\n[RAINGAGES]\nRG1  INTENSITY 0:05 1.0 TIMESERIES TS1\n"
         "\n[SUBCATCHMENTS]\nS1  RG1  J1  10.0  50   500  0.5  0\n"
         "\n[SUBAREAS]\nS1  0.01  0.1  0.05  0.05  25  OUTLET\n"
         "\n[INFILTRATION]\nS1  3.0  0.5  4.0  7  0\n"
         "\n[TIMESERIES]\nTS1  01/01/2026 00:00 1.0\nTS1  01/01/2026 01:00 1.0\nTS1  01/01/2026 01:01 0.0\n"
         "\n[JUNCTIONS]\nJ1  100.0  10.0  0.0  0.0  0.0\n"
         "\n[OUTFALLS]\nO1  95.0  FREE\n"
         "\n[CONDUITS]\nC1  J1  O1  400.0  0.013  0  0\n"
         "\n[XSECTIONS]\nC1  CIRCULAR  1.5  0  0  0  1\n"
         "\n[LANDUSES]\n" << landuse_line
      << "\n[COVERAGES]\nS1  LU1  100\n"
      << quality
      << (process_components.empty() ? "" : "\n[PROCESS_COMPONENTS]\n" + process_components + "\n")
      << "\n[REPORT]\nINPUT NO\n";
    return s.str();
}

const char* kPollut = "\n[POLLUTANTS]\nTSS  MG/L  0  0  0  0  NO  *  0.0  0  0\n";

std::string rows(const char* name, const char* sweep = "0", const char* bmp = "0") {
    return std::string("LU1  ") + name + "  POW  100  2  1  AREA\n" +
           "LU1  " + name + "  EXP  0.1  1.0  " + sweep + "  " + bmp + "\n";
}
// [BUILDUP] then [WASHOFF] with the rows split by section
std::string bw(const std::vector<std::string>& names, const char* sweep = "0", const char* bmp = "0") {
    std::string b = "\n[BUILDUP]\n", w = "\n[WASHOFF]\n";
    for (const auto& n : names) {
        b += "LU1  " + n + "  POW  100  2  1  AREA\n";
        w += "LU1  " + n + "  EXP  0.1  1.0  " + sweep + "  " + bmp + "\n";
    }
    return b + w;
}

void write(const std::string& path, const std::string& body) { std::ofstream f(path); f << body; }
std::string rxn_inert() {
    return "[REACTION_OPTIONS]\nSOLVER RK5\n[REACTION_SPECIES]\nBULK X MG\n"
           "[REACTION_PIPES]\nRATE X 0\n[REACTION_TANKS]\nRATE X 0\n";
}
std::string pc(const char* rxn) {
    return std::string("org.hydrocouple.openswmm.reactions config=\"") + rxn + "\"";
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    bool ok = false;
    std::vector<double> tss_node;   ///< J1 TSS per step
    std::vector<double> x_node;     ///< J1 X per step
    std::vector<std::string> warnings;
};

DeckRun run(const std::string& tag, const std::string& body, bool record = true) {
    DeckRun r;
    write(tag + ".inp", body);
    r.e = swmm_engine_create();
    if (!r.e) return r;
    if (swmm_engine_open(r.e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK) return r;
    if (swmm_engine_initialize(r.e) != SWMM_OK || swmm_engine_start(r.e, 1) != SWMM_OK) return r;
    auto& ctx = cpp(r.e).context();
    const int j1 = ctx.node_names.find("J1");
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(r.e, &elapsed) != SWMM_OK) break;
        if (record && j1 >= 0) {
            const auto uj = static_cast<std::size_t>(j1);
            r.tss_node.push_back(ctx.n_pollutants() > 0 && uj * ctx.n_pollutants() < ctx.nodes.conc.size()
                                     ? ctx.nodes.conc[uj * ctx.n_pollutants()] : 0.0);
            const int nm = ctx.reactions.n_species();
            r.x_node.push_back(nm > 0 && uj * nm < ctx.reactions.msx_node_conc.size()
                                   ? ctx.reactions.msx_node_conc[uj * nm] : 0.0);
        }
    } while (elapsed > 0.0 && ++guard < 100000);
    r.warnings = ctx.warnings;
    r.ok = true;
    return r;
}
void close(DeckRun& r) {
    if (!r.e) return;
    swmm_engine_end(r.e); swmm_engine_close(r.e); swmm_engine_destroy(r.e); r.e = nullptr;
}

} // namespace

// ---------------------------------------------------------------------------
// a. Same parameters -> same surface loads and same final buildup, bit-for-bit.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, SameParametersSameSurfaceLoads) {
    write("_bw_a.rxn", rxn_inert());
    DeckRun r = run("_bw_a", deck("", std::string(kPollut) + bw({"TSS", "X"}), pc("_bw_a.rxn")));
    ASSERT_TRUE(r.ok);
    const auto& ctx = cpp(r.e).context();
    const auto& ms  = ctx.reactions.surface;
    ASSERT_TRUE(ms.active());
    ASSERT_EQ(ms.n_species, 1);
    ASSERT_EQ(ctx.n_pollutants(), 1);

    // Per-subcatchment washoff total, user mass: pollutant side is
    // subcatches.total_load[sc*np + p] (Washoff Summary), MSX side the mirror.
    ASSERT_GE(ctx.subcatches.total_load.size(), 1u);
    const double tss_load = ctx.subcatches.total_load[0];
    const double x_load   = ms.led_subcatch_load[ms.sidx(0, 0)];
    EXPECT_GT(tss_load, 0.0) << "the storm must wash something off";
    EXPECT_EQ(x_load, tss_load) << "same relations, same arithmetic — must be bit-identical";

    // Final buildup store per land use: SurfaceQualitySoA vs the MSX store.
    const auto& sq = cpp(r.e).surfaceQuality();
    const double tss_bu = sq.buildup[sq.bu_idx(0, 0, 0)];
    const double x_bu   = ms.buildup[ms.bu_idx(0, 0, 0)];
    EXPECT_EQ(x_bu, tss_bu);

    // Ledger totals: initial buildup at DRY_DAYS, net buildup and delivered load.
    EXPECT_EQ(ms.led_init_buildup[0], ctx.mass_balance.qual_init_buildup[0]);
    EXPECT_EQ(ms.led_runoff_load[0],  ctx.mass_balance.qual_runoff_load[0]);
    close(r);
}

// ---------------------------------------------------------------------------
// b. The load reaches the network: J1's X tracks J1's TSS under both quality
//    solvers. The delivery seams differ (qual_mass_in vs msx_ext_mass_in;
//    ARD stage 1b vs the legacy CSTR), so this is a band, not bit-identity.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, LoadReachesTheNetworkUnderBothSolvers) {
    for (const char* solver : {"LEGACY", "EULERIAN_ARD"}) {
        write("_bw_b.rxn", rxn_inert());
        const std::string tag = std::string("_bw_b_") + (solver[0] == 'L' ? "leg" : "ard");
        DeckRun r = run(tag, deck(std::string("QUALITY_SOLVER        ") + solver + "\n",
                              std::string(kPollut) + bw({"TSS", "X"}), pc("_bw_b.rxn")));
        ASSERT_TRUE(r.ok) << solver;
        ASSERT_GT(r.tss_node.size(), 10u);
        double max_tss = 0.0, max_x = 0.0, worst = 0.0; int compared = 0;
        for (std::size_t k = 0; k < r.tss_node.size(); ++k) {
            max_tss = std::max(max_tss, r.tss_node[k]);
            max_x   = std::max(max_x, r.x_node[k]);
        }
        EXPECT_GT(max_tss, 1.0) << solver << ": TSS must appear at J1";
        EXPECT_GT(max_x, 1.0)   << solver << ": X must appear at J1";
        for (std::size_t k = 0; k < r.tss_node.size(); ++k) {
            if (r.tss_node[k] < 0.05 * max_tss) continue;   // compare in the wet, established part
            worst = std::max(worst, std::fabs(r.x_node[k] - r.tss_node[k]) / r.tss_node[k]);
            ++compared;
        }
        EXPECT_GT(compared, 5);
        EXPECT_LT(worst, 5.0e-2) << solver << ": X at J1 must track TSS (same load, same water)";
        close(r);
    }
}

// ---------------------------------------------------------------------------
// c. Ledger closes with sweeping and BMP removal on.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, LedgerCloses) {
    write("_bw_c.rxn", rxn_inert());
    // LU1 sweeps every 0.05 day (1.2 h) with 60 % availability: the storm
    // stops at 01:01 and the dry tail of the 3 h run accumulates ~0.08 day,
    // so exactly one sweep event fires after the washoff. (The engine seeds
    // the per-(subcatchment, land use) last-swept counter at 0 regardless of
    // the [LANDUSES] LastSweep column — a pre-existing parity gap, noted in
    // the BW-MSX handoff — hence the short interval rather than LastSweep.)
    DeckRun r = run("_bw_c", deck("", std::string(kPollut) + bw({"TSS", "X"}, "50", "20"),
                              pc("_bw_c.rxn"), "LU1  0.05  60  0\n"));
    ASSERT_TRUE(r.ok);
    const auto& ctx = cpp(r.e).context();
    const auto& ms  = ctx.reactions.surface;
    ASSERT_TRUE(ms.active());
    // Final store in user mass over the land use's normalizer.
    const double frac = ctx.subcatches.coverage[0] / 100.0;
    const double norm = frac * ctx.subcatches.area[0];
    const double final_store = ms.buildup[ms.bu_idx(0, 0, 0)] * norm;
    const double lhs = ms.led_init_buildup[0] + ms.led_buildup[0]
                     - ms.led_subcatch_load[ms.sidx(0, 0)] - ms.led_bmp_removal[0]
                     - ms.led_sweeping[0];
    const double scale = std::max(1.0, ms.led_init_buildup[0] + ms.led_buildup[0]);
    EXPECT_NEAR(lhs, final_store, 1.0e-10 * scale)
        << "init + net buildup − washed − BMP − swept must equal the final store";
    EXPECT_GT(ms.led_bmp_removal[0], 0.0);
    EXPECT_GT(ms.led_sweeping[0], 0.0) << "one sweep event must have fired in the dry tail";
    // The pollutant's identical ledger closes the same way (same arithmetic).
    const auto& sq = cpp(r.e).surfaceQuality();
    const double tss_final = sq.buildup[sq.bu_idx(0, 0, 0)] * norm;
    EXPECT_EQ(final_store, tss_final);
    EXPECT_EQ(ms.led_sweeping[0], ctx.mass_balance.qual_sweeping[0]);
    EXPECT_EQ(ms.led_bmp_removal[0], ctx.mass_balance.qual_bmp_removal[0]);
    close(r);
}

// ---------------------------------------------------------------------------
// d. Pollutants untouched: adding the X rows leaves TSS bit-identical.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, PollutantsAreBitIdenticalWithMsxRowsPresent) {
    write("_bw_d.rxn", rxn_inert());
    DeckRun a = run("_bw_d_no",   deck("", std::string(kPollut) + bw({"TSS"}),      pc("_bw_d.rxn")));
    DeckRun b = run("_bw_d_with", deck("", std::string(kPollut) + bw({"TSS", "X"}), pc("_bw_d.rxn")));
    ASSERT_TRUE(a.ok); ASSERT_TRUE(b.ok);
    ASSERT_EQ(a.tss_node.size(), b.tss_node.size());
    for (std::size_t k = 0; k < a.tss_node.size(); ++k)
        ASSERT_EQ(a.tss_node[k], b.tss_node[k]) << "step " << k;
    const auto& ca = cpp(a.e).context(); const auto& cb = cpp(b.e).context();
    EXPECT_EQ(ca.mass_balance.qual_runoff_load[0], cb.mass_balance.qual_runoff_load[0]);
    EXPECT_EQ(ca.subcatches.total_load[0], cb.subcatches.total_load[0]);
    EXPECT_FALSE(ca.reactions.surface.active());
    EXPECT_TRUE(cb.reactions.surface.active());
    close(a); close(b);
}

// ---------------------------------------------------------------------------
// e. Round trip: writer emits the rows; reopened deck resolves them; C API
//    reads them at n_pollutants + m; a [LOADINGS] row seeds the store.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, RoundTripThroughWriterAndCApi) {
    write("_bw_e.rxn", rxn_inert());
    const std::string body = deck("", std::string(kPollut) + bw({"TSS", "X"}, "30", "10") +
                                  "\n[LOADINGS]\nS1  TSS  12.5\nS1  X  7.25\n", pc("_bw_e.rxn"));
    write("_bw_e.inp", body);
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "_bw_e.inp", "_bw_e.rpt", "_bw_e.out", nullptr), SWMM_OK);
    {
        const auto& ctx = cpp(e).context();
        const int np = ctx.n_pollutants();
        ASSERT_EQ(np, 1);
        ASSERT_EQ(ctx.reactions.n_species(), 1);
        int ft = -1; double c1 = 0, c2 = 0, c3 = 0; int nz = -1;
        ASSERT_EQ(swmm_buildup_get(e, 0, np + 0, &ft, &c1, &c2, &c3, &nz), SWMM_OK);
        EXPECT_EQ(ft, 1); EXPECT_DOUBLE_EQ(c1, 100.0); EXPECT_DOUBLE_EQ(c2, 2.0); EXPECT_DOUBLE_EQ(c3, 1.0); EXPECT_EQ(nz, 0);
        double co = 0, ex = 0, sw = 0, bm = 0;
        ASSERT_EQ(swmm_washoff_get(e, 0, np + 0, &ft, &co, &ex, &sw, &bm), SWMM_OK);
        EXPECT_EQ(ft, 1); EXPECT_DOUBLE_EQ(co, 0.1); EXPECT_DOUBLE_EQ(ex, 1.0);
        EXPECT_DOUBLE_EQ(sw, 30.0); EXPECT_DOUBLE_EQ(bm, 10.0);
        double ld = 0.0;
        ASSERT_EQ(swmm_subcatch_get_initial_loading(e, 0, np + 0, &ld), SWMM_OK);
        EXPECT_DOUBLE_EQ(ld, 7.25);
        EXPECT_NE(swmm_buildup_get(e, 0, np + 1, &ft, &c1, &c2, &c3, &nz), SWMM_OK) << "past the species";
        // Edit through the API, then write.
        ASSERT_EQ(swmm_washoff_set(e, 0, np + 0, 3, 55.0, 0.0, 30.0, 10.0), SWMM_OK);   // EMC 55 mg/L
    }
    ASSERT_EQ(swmm_model_write(e, "_bw_e_rt.inp"), SWMM_OK);
    swmm_engine_close(e); swmm_engine_destroy(e);

    // The written deck carries the X rows in the pollutant sections.
    {
        std::ifstream f("_bw_e_rt.inp"); std::stringstream ss; ss << f.rdbuf();
        const std::string t = ss.str();
        EXPECT_NE(t.find("LU1              X                POW"), std::string::npos) << t;
        EXPECT_NE(t.find("LU1              X                EMC"), std::string::npos) << t;
        EXPECT_NE(t.find("S1               X                    7.2500"), std::string::npos) << t;
    }
    // Reopen: same values back, and the initial loading seeds the store.
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, "_bw_e_rt.inp", "_bw_e_rt.rpt", "_bw_e_rt.out", nullptr), SWMM_OK);
    int ft = -1; double co = 0, ex = 0, sw = 0, bm = 0;
    ASSERT_EQ(swmm_washoff_get(e2, 0, 1, &ft, &co, &ex, &sw, &bm), SWMM_OK);
    EXPECT_EQ(ft, 3); EXPECT_DOUBLE_EQ(co, 55.0);
    ASSERT_EQ(swmm_engine_initialize(e2), SWMM_OK);
    {
        const auto& ctx = cpp(e2).context();
        const auto& ms = ctx.reactions.surface;
        ASSERT_TRUE(ms.active());
        EXPECT_DOUBLE_EQ(ms.buildup[ms.bu_idx(0, 0, 0)], 7.25) << "[LOADINGS] wins over DRY_DAYS";
        EXPECT_DOUBLE_EQ(ms.led_init_buildup[0], 7.25 * ctx.subcatches.area[0]);
    }
    swmm_engine_close(e2); swmm_engine_destroy(e2);
}

// ---------------------------------------------------------------------------
// f. Unknown constituent: one warning, run proceeds; MSX store inactive.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, UnknownConstituentWarnsAndRuns) {
    write("_bw_f.rxn", rxn_inert());
    DeckRun r = run("_bw_f", deck("", std::string(kPollut) + bw({"TSS", "NOPE"}), pc("_bw_f.rxn")), false);
    ASSERT_TRUE(r.ok);
    int hits = 0;
    for (const auto& w : r.warnings)
        if (w.find("NOPE") != std::string::npos && w.find("neither a pollutant") != std::string::npos) ++hits;
    EXPECT_EQ(hits, 2) << "one warning per row ([BUILDUP], [WASHOFF])";
    EXPECT_FALSE(cpp(r.e).context().reactions.surface.active());
    close(r);
}

// ---------------------------------------------------------------------------
// g. MSX-only deck (no [POLLUTANTS]): X builds up at DRY_DAYS, washes off,
//    and reaches J1.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, MsxOnlyDeckBuildsUpAndWashesOff) {
    write("_bw_g.rxn", rxn_inert());
    DeckRun r = run("_bw_g", deck("QUALITY_SOLVER        LEGACY\n", bw({"X"}), pc("_bw_g.rxn")));
    ASSERT_TRUE(r.ok);
    const auto& ctx = cpp(r.e).context();
    const auto& ms  = ctx.reactions.surface;
    ASSERT_TRUE(ms.active());
    EXPECT_EQ(ctx.n_pollutants(), 0);
    EXPECT_GT(ms.led_init_buildup[0], 0.0) << "DRY_DAYS seeds the store";
    EXPECT_GT(ms.led_subcatch_load[ms.sidx(0, 0)], 0.0);
    double max_x = 0.0;
    for (double v : r.x_node) max_x = std::max(max_x, v);
    EXPECT_GT(max_x, 1.0) << "X must reach J1 with no pollutant in the deck";
    close(r);
}
