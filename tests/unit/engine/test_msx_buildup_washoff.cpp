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
#include <cstdlib>
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
         // Spacing >= the 0:05 recording interval (legacy ERROR 159).
         "\n[TIMESERIES]\nTS1  01/01/2026 00:00 1.0\nTS1  01/01/2026 01:00 1.0\nTS1  01/01/2026 01:05 0.0\n"
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
    // LU1 sweeps every 0.05 day (1.2 h) with availability 0.6 — a FRACTION,
    // as legacy reads it (landuse.c:70; :81 rejects anything outside [0,1]).
    // The sweep clock runs on calendar time seeded from the LastSweep column
    // (legacy landuse.c:377), so with LastSweep 0 the first sweep comes due
    // 1.2 h in — during the storm — and fires at the first dry step after
    // 01:01, with a second in the dry tail of the 3 h run. The assertion is
    // ledger closure, which every firing must preserve.
    DeckRun r = run("_bw_c", deck("", std::string(kPollut) + bw({"TSS", "X"}, "50", "20"),
                              pc("_bw_c.rxn"), "LU1  0.05  0.6  0\n"));
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
    // BMP removal is the one quantity in this group that is NOT bit-comparable,
    // and the comment above overclaimed for it. The two sides reach the same
    // mathematical number by different expressions — the MSX row books
    // `bmp_removed * dt_runoff * mcf_m` (MsxSurfaceQuality.cpp), the pollutant
    // row `c_out * (v_lost - v_infil) * mcf_p` (SWMMEngine.cpp) — so they agree
    // only to rounding. Clang/macOS happens to produce identical doubles and
    // GCC/Linux differs by 1 ULP (0.11693027411237766 vs …65), which is why
    // this stood for weeks: it was only ever exercised on one toolchain.
    //
    // This is a corrected assertion, not a widened one. The property that
    // matters — adding MSX rows leaves the pollutant arithmetic untouched — is
    // asserted bitwise by the two EXPECT_EQs that remain and by test (d)
    // below; nothing here is weaker as a result.
    EXPECT_DOUBLE_EQ(ms.led_bmp_removal[0], ctx.mass_balance.qual_bmp_removal[0]);
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

// ---------------------------------------------------------------------------
// Review C3 (2026-10-15): run-on. S1 (the only subcatchment with land-use
// buildup) drains onto S2, an impervious subcatchment that drains to J1. Every
// unit of X that reaches the network must come through S2 as run-on. At base
// a subcatchment whose outlet is another subcatchment was skipped: S1's X was
// washed off its land and delivered nowhere.
// ---------------------------------------------------------------------------
TEST(MsxBuildupWashoff, RunonCarriesSpeciesToTheReceivingSubcatchment) {
    write("_bw_runon.rxn", rxn_inert());
    std::string body = deck("", std::string(kPollut) + bw({"TSS", "X"}), pc("_bw_runon.rxn"));
    auto rep = [&](const std::string& a, const std::string& b) {
        const auto at = body.find(a); ASSERT_NE(at, std::string::npos) << a;
        body.replace(at, a.size(), b);
    };
    rep("S1  RG1  J1  10.0  50   500  0.5  0\n",
        "S1  RG1  S2  10.0  50   500  0.5  0\nS2  RG1  J1  2.0  100   200  0.5  0\n");
    rep("S1  0.01  0.1  0.05  0.05  25  OUTLET\n",
        "S1  0.01  0.1  0.05  0.05  25  OUTLET\nS2  0.01  0.1  0.05  0.05  0  OUTLET\n");
    rep("S1  3.0  0.5  4.0  7  0\n", "S1  3.0  0.5  4.0  7  0\nS2  3.0  0.5  4.0  7  0\n");
    DeckRun r = run("_bw_runon", body);
    ASSERT_TRUE(r.ok);
    const auto& ctx = cpp(r.e).context();
    const auto& ms = ctx.reactions.surface;
    double x_peak = 0.0, tss_peak = 0.0;
    for (double v : r.x_node) x_peak = std::max(x_peak, v);
    for (double v : r.tss_node) tss_peak = std::max(tss_peak, v);
    ASSERT_GT(tss_peak, 0.0) << "TSS control never reached J1; deck defect";
    EXPECT_GT(x_peak, 0.0) << "S1's species never reached the network";
    // Multispecies Phase 2: the species now runs the pollutant kernel, run-on
    // included (ponded mix at the receiver, the same one-step lag), so the
    // check is the twin rule: S1's species reaches J1 exactly as TSS does.
    // The earlier ledger closure leaned on a private run-on ledger that the
    // shared kernel replaced.
    ASSERT_EQ(r.x_node.size(), r.tss_node.size());
    for (std::size_t k = 0; k < r.x_node.size(); ++k)
        ASSERT_EQ(r.x_node[k], r.tss_node[k]) << "J1 at step " << k;
    EXPECT_EQ(ms.led_runoff_load[0], ctx.mass_balance.qual_runoff_load[0]);
    close(r);
}

// ===========================================================================
// Phase 2 twin gates (2026-10-15, MULTISPECIES_PHASE2_WATERSHED_LID_DESIGN).
// A species X carrying TSS's [BUILDUP]/[WASHOFF] rows must produce TSS's
// numbers exactly, in every regime the pollutant kernel supports — not only
// in the EXP/OUTLET/no-LID regime gate (a) runs, where every difference
// between the two kernels happens to vanish.
// ===========================================================================
namespace {

struct TwinOpts {
    std::string flow_units = "CFS";
    std::string routing    = "OUTLET";   ///< [SUBAREAS] RouteTo
    std::string pct_routed = "100";
    std::string buildup    = "POW  100  2  1  AREA";   ///< "" = no buildup row
    std::string washoff    = "EXP  0.1  1.0";
    std::string sweep_bmp  = "0  0";
    std::string tss_rain   = "0";        ///< [POLLUTANTS] Crain for TSS
    bool        runon      = false;      ///< S1 drains to S2, S2 to J1
    std::string lid_controls;            ///< full [LID_CONTROLS] body
    std::string lid_usage;               ///< full [LID_USAGE] body
    std::string rxn_extra;               ///< appended to the reactions file
    std::string lid_extra;               ///< appended to [LID_CONTROLS] (e.g. REMOVALS)
};

std::string twinDeck(const TwinOpts& o) {
    const bool si = o.flow_units == "CMS";
    std::ostringstream s;
    s << "[OPTIONS]\nFLOW_UNITS           " << o.flow_units
      << "\nFLOW_ROUTING         DYNWAVE\nINFILTRATION         HORTON\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             03:00:00\n"
         "REPORT_STEP          00:05:00\nWET_STEP             00:05:00\nDRY_STEP             00:05:00\n"
         "ROUTING_STEP         0:00:30\nDRY_DAYS             5\n"
         "\n[RAINGAGES]\nRG1  INTENSITY 0:05 1.0 TIMESERIES TS1\n"
         "\n[SUBCATCHMENTS]\n";
    if (si) {
        s << "S1  RG1  " << (o.runon ? "S2" : "J1") << "  4.0  50  150  0.5  0\n";
        if (o.runon) s << "S2  RG1  J1  0.8  100  60  0.5  0\n";
    } else {
        s << "S1  RG1  " << (o.runon ? "S2" : "J1") << "  10.0  50  500  0.5  0\n";
        if (o.runon) s << "S2  RG1  J1  2.0  100  200  0.5  0\n";
    }
    s << "\n[SUBAREAS]\n";
    const char* sub = si ? "0.01  0.1  1.27  1.27" : "0.01  0.1  0.05  0.05";
    // Nimp Nperv Simp Sperv PctZero RouteTo PctRouted.
    s << "S1  " << sub << "  25  " << o.routing << "  " << o.pct_routed << "\n";
    if (o.runon) s << "S2  " << sub << "  0  OUTLET\n";
    s << "\n[INFILTRATION]\n";
    const char* inf = si ? "76  12.7  4.0  7  0" : "3.0  0.5  4.0  7  0";
    s << "S1  " << inf << "\n";
    if (o.runon) s << "S2  " << inf << "\n";
    s << "\n[TIMESERIES]\nTS1  01/01/2026 00:00 " << (si ? "25.4" : "1.0")
      << "\nTS1  01/01/2026 01:00 " << (si ? "25.4" : "1.0") << "\nTS1  01/01/2026 01:05 0.0\n";
    if (si)
        s << "\n[JUNCTIONS]\nJ1  30.0  3.0  0.0  0.0  0.0\n\n[OUTFALLS]\nO1  28.5  FREE\n"
             "\n[CONDUITS]\nC1  J1  O1  120.0  0.013  0  0\n\n[XSECTIONS]\nC1  CIRCULAR  0.45  0  0  0  1\n";
    else
        s << "\n[JUNCTIONS]\nJ1  100.0  10.0  0.0  0.0  0.0\n\n[OUTFALLS]\nO1  95.0  FREE\n"
             "\n[CONDUITS]\nC1  J1  O1  400.0  0.013  0  0\n\n[XSECTIONS]\nC1  CIRCULAR  1.5  0  0  0  1\n";
    s << "\n[POLLUTANTS]\nTSS  MG/L  " << o.tss_rain << "  0  0  0  NO  *  0.0  0  0\n"
      << "\n[LANDUSES]\nLU1  0  0  0\n\n[COVERAGES]\nS1  LU1  100\n";
    if (o.runon) s << "S2  LU1  100\n";
    s << "\n[BUILDUP]\n";
    if (!o.buildup.empty())
        for (const char* n : {"TSS", "X"}) s << "LU1  " << n << "  " << o.buildup << "\n";
    s << "\n[WASHOFF]\n";
    for (const char* n : {"TSS", "X"}) s << "LU1  " << n << "  " << o.washoff << "  " << o.sweep_bmp << "\n";
    if (!o.lid_controls.empty()) s << "\n[LID_CONTROLS]\n" << o.lid_controls << o.lid_extra;
    if (!o.lid_usage.empty())    s << "\n[LID_USAGE]\n" << o.lid_usage;
    return s.str();
}

/// Largest |X − TSS| over a quantity, beside the largest |TSS| it was
/// compared against (so a failure says how big the gap is, not just that).
struct Gap { double diff = 0.0, scale = 0.0; int n = 0;
    void add(double tss, double x) { diff = std::max(diff, std::fabs(x - tss)); scale = std::max(scale, std::fabs(tss)); ++n; } };

struct Twin {
    bool ok = false;
    double drain_seen = 0.0;   ///< LID drain flow that reached a node (cfs, summed over steps)
    double drain_sub  = 0.0;   ///< subcatchment LID drain flow (cfs, summed over steps)
    Gap conc, node, load, buildup, init, runoff, bmp, sweep;
};

Twin runTwin(const std::string& tag, const TwinOpts& o) {
    Twin t;
    write(tag + ".rxn", rxn_inert() + o.rxn_extra);
    std::string body = twinDeck(o) + "\n[PROCESS_COMPONENTS]\n" + pc((tag + ".rxn").c_str()) +
                       "\n\n[REPORT]\nINPUT NO\n";
    write(tag + ".inp", body);
    SWMM_Engine e = swmm_engine_create();
    if (!e) return t;
    if (swmm_engine_open(e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(), (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return t;
    }
    auto& ctx = cpp(e).context();
    const auto& ms = ctx.reactions.surface;
    const int np = ctx.n_pollutants(), nm = ctx.reactions.n_species(), ns = ctx.n_subcatches();
    const int j1 = ctx.node_names.find("J1");
    double elapsed = 0.0; int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
        for (int sc = 0; sc < ns; ++sc)
            t.conc.add(ctx.subcatches.conc[static_cast<std::size_t>(sc * np)],
                       ms.washoff_conc[ms.sidx(sc, 0)]);
        const auto uj = static_cast<std::size_t>(j1);
        t.node.add(ctx.nodes.conc[uj * np], ctx.reactions.msx_node_conc[uj * nm]);
        for (double v : ctx.nodes.lid_drain_qual_vol) t.drain_seen += v;
        for (double v : ctx.subcatches.lid_drain_flow) t.drain_sub += v;
        if (std::getenv("TWIN_LID_DEBUG"))
            for (int g = 0; g < cpp(e).lid().numGroups(); ++g) {
                const auto& grp = cpp(e).lid().group(g);
                for (int u = 0; u < grp.count; ++u)
                    std::printf("LID t=%.0f g%d u%d surf %.4g soil %.4g stor %.4g drain %.4g node %d\n", elapsed * 86400.0, g, u,
                                grp.surf_depth[u], grp.soil_moist[u], grp.stor_depth[u], grp.drain_flow[u], grp.drain_node[u]);
            }
    } while (elapsed > 0.0 && ++guard < 100000);
    swmm_engine_end(e);
    const auto& sq = cpp(e).surfaceQuality();
    for (int sc = 0; sc < ns; ++sc) {
        t.load.add(ctx.subcatches.total_load[static_cast<std::size_t>(sc * np)], ms.led_subcatch_load[ms.sidx(sc, 0)]);
        t.buildup.add(sq.buildup[sq.bu_idx(sc, 0, 0)], ms.buildup[ms.bu_idx(sc, 0, 0)]);
    }
    const auto& mb = ctx.mass_balance;
    t.init.add(mb.qual_init_buildup[0], ms.led_init_buildup[0]);
    t.runoff.add(mb.qual_runoff_load[0], ms.led_runoff_load[0]);
    t.bmp.add(mb.qual_bmp_removal[0], ms.led_bmp_removal[0]);
    t.sweep.add(mb.qual_sweeping[0], ms.led_sweeping[0]);
    t.ok = true;
    // Evidence line for the round records.
    std::printf("TWIN %-24s conc %.6g/%.6g node %.6g/%.6g load %.6g/%.6g bu %.6g/%.6g runoff %.6g/%.6g bmp %.6g/%.6g\n",
                tag.c_str(), t.conc.diff, t.conc.scale, t.node.diff, t.node.scale, t.load.diff, t.load.scale,
                t.buildup.diff, t.buildup.scale, t.runoff.diff, t.runoff.scale, t.bmp.diff, t.bmp.scale);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return t;
}

void expectTwin(const Twin& t, const std::string& tag) {
    ASSERT_TRUE(t.ok) << tag << " did not run";
    ASSERT_GT(t.load.scale, 0.0) << tag << ": nothing washed off; deck defect";
    EXPECT_EQ(t.conc.diff, 0.0)    << tag << ": subcatchment concentration differs (TSS up to " << t.conc.scale << ")";
    EXPECT_EQ(t.node.diff, 0.0)    << tag << ": J1 concentration differs (TSS up to " << t.node.scale << ")";
    EXPECT_EQ(t.load.diff, 0.0)    << tag << ": subcatchment load differs (TSS " << t.load.scale << ")";
    EXPECT_EQ(t.buildup.diff, 0.0) << tag << ": final buildup differs";
    EXPECT_EQ(t.init.diff, 0.0)    << tag << ": initial buildup differs";
    EXPECT_EQ(t.runoff.diff, 0.0)  << tag << ": runoff load ledger differs (TSS " << t.runoff.scale << ")";
    EXPECT_EQ(t.bmp.diff, 0.0)     << tag << ": BMP removal ledger differs (TSS " << t.bmp.scale << ")";
    EXPECT_EQ(t.sweep.diff, 0.0)   << tag << ": sweeping ledger differs";
}

const char* kBioRetention =
    "BC1  BC\nBC1  SURFACE  6.0  0.0  0.1  1.0  5\nBC1  SOIL  12.0  0.5  0.2  0.1  0.5  10.0  3.5\n"
    "BC1  STORAGE  12.0  0.75  0.5  0\nBC1  DRAIN  0.5  0.5  6  6\n";

}  // namespace

TEST(MsxSurfaceTwin, R1_ExpOutletNoLid) {
    TwinOpts o; expectTwin(runTwin("_tw_r1", o), "R1");
}
TEST(MsxSurfaceTwin, R2_ExpPervious) {
    TwinOpts o; o.routing = "PERVIOUS"; o.pct_routed = "50"; expectTwin(runTwin("_tw_r2", o), "R2");
}
TEST(MsxSurfaceTwin, R3_ExpImpervious) {
    TwinOpts o; o.routing = "IMPERVIOUS"; o.pct_routed = "50"; expectTwin(runTwin("_tw_r3", o), "R3");
}
TEST(MsxSurfaceTwin, R4_RatingCurveOutletAndPervious) {
    TwinOpts o; o.washoff = "RC  2.0  1.2"; expectTwin(runTwin("_tw_r4a", o), "R4 outlet");
    o.routing = "PERVIOUS"; o.pct_routed = "50"; expectTwin(runTwin("_tw_r4b", o), "R4 pervious");
}
TEST(MsxSurfaceTwin, R5_EmcWithBuildupFunction) {
    TwinOpts o; o.washoff = "EMC  20  0"; o.routing = "PERVIOUS"; o.pct_routed = "50";
    expectTwin(runTwin("_tw_r5", o), "R5");
}
TEST(MsxSurfaceTwin, R6_RatingCurveWithoutBuildup) {
    TwinOpts o; o.buildup = ""; o.washoff = "RC  2.0  1.2"; expectTwin(runTwin("_tw_r6", o), "R6");
}
TEST(MsxSurfaceTwin, R7_BioRetentionDrainToNode) {
    TwinOpts o; o.routing = "PERVIOUS"; o.pct_routed = "50"; o.lid_controls = kBioRetention;
    o.lid_usage = "S1  BC1  1  20000  100  0  50  0  *  J1\n";
    expectTwin(runTwin("_tw_r7", o), "R7");
}
TEST(MsxSurfaceTwin, R9_RainBarrelReturnToPervious) {
    TwinOpts o; o.lid_controls = "RB1  RB\nRB1  STORAGE  36  0.75  0  0\nRB1  DRAIN  0.5  0.5  0  6\n";
    o.lid_usage = "S1  RB1  4  12  0  0  30  1\n";
    expectTwin(runTwin("_tw_r9", o), "R9");
}
TEST(MsxSurfaceTwin, R10_RunonOntoFullyCoveredLid) {
    TwinOpts o; o.runon = true; o.lid_controls = kBioRetention;
    o.lid_usage = "S2  BC1  1  87120  100  0  100  0\n";   // 2 ac = 87120 ft²: S2 fully covered
    expectTwin(runTwin("_tw_r10", o), "R10");
}
TEST(MsxSurfaceTwin, R11_RunonPondedMix) {
    TwinOpts o; o.runon = true; expectTwin(runTwin("_tw_r11", o), "R11");
}
TEST(MsxSurfaceTwin, R15_SiTwinOfR2) {
    TwinOpts o; o.flow_units = "CMS"; o.routing = "PERVIOUS"; o.pct_routed = "50";
    expectTwin(runTwin("_tw_r15", o), "R15");
}

// S3: a bio-retention cell whose underdrain actually flows (no offset, no
// seepage into native soil), with the drain removing 30 % of both. At base
// the species' drain mass reached nothing and its removal row was dropped.
namespace {
const char* kFlowingBioRetention =
    "BC2  BC\nBC2  SURFACE  6.0  0.0  0.1  1.0  5\nBC2  SOIL  12.0  0.5  0.2  0.1  10.0  10.0  3.5\n"
    "BC2  STORAGE  12.0  0.75  0.0  0\nBC2  DRAIN  2.0  0.5  0  0\n";
}
TEST(MsxSurfaceTwin, R7b_FlowingUnderdrainWithRemoval) {
    TwinOpts o; o.routing = "PERVIOUS"; o.pct_routed = "50"; o.lid_controls = kFlowingBioRetention;
    o.lid_extra = "BC2  REMOVALS  TSS  30  X  30\n";
    o.lid_usage = "S1  BC2  1  4000  50  100  100  0  *  J1\n";
    const Twin t = runTwin("_tw_r7b", o);
    EXPECT_GT(t.drain_seen, 0.0) << "the underdrain never flowed; deck defect";
    expectTwin(t, "R7b");
}

// S3: LID REMOVALS rows survive a save, for pollutants and species alike.
// Before, the writer emitted no REMOVALS row at all, so saving a model lost
// every LID drain removal.
TEST(MsxSurfaceTwin, LidRemovalsRoundTripThroughTheWriter) {
    TwinOpts o; o.lid_controls = kFlowingBioRetention; o.lid_extra = "BC2  REMOVALS  TSS  30  X  40\n";
    o.lid_usage = "S1  BC2  1  4000  50  100  100  0  *  J1\n";
    write("_tw_rm.rxn", rxn_inert());
    write("_tw_rm.inp", twinDeck(o) + "\n[PROCESS_COMPONENTS]\n" + pc("_tw_rm.rxn") + "\n\n[REPORT]\nINPUT NO\n");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, "_tw_rm.inp", "_tw_rm.rpt", "_tw_rm.out", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, "_tw_rm_rt.inp"), SWMM_OK);
    swmm_engine_close(e); swmm_engine_destroy(e);
    std::ifstream f("_tw_rm_rt.inp"); std::stringstream ss; ss << f.rdbuf();
    const std::string text = ss.str();
    const auto at = text.find("REMOVALS");
    ASSERT_NE(at, std::string::npos) << "no REMOVALS row written";
    const std::string row = text.substr(at, text.find('\n', at) - at);
    EXPECT_NE(row.find("TSS"), std::string::npos) << row;
    EXPECT_NE(row.find("X"), std::string::npos) << row;
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, "_tw_rm_rt.inp", "_tw_rm_rt.rpt", "_tw_rm_rt.out", nullptr), SWMM_OK);
    const auto& ms = cpp(e2).context().reactions.surface;
    ASSERT_EQ(ms.lid_rmvl.size(), 1u) << "species removal not resolved after reload";
    EXPECT_DOUBLE_EQ(ms.lid_rmvl[0], 0.40);
    swmm_engine_close(e2); swmm_engine_destroy(e2);
}

// S3 parity: a pollutant's LID REMOVALS row read before [POLLUTANTS] (the
// usual SWMM file order) was dropped, because the name was not yet known.
// Legacy applies it. The same deck in either section order must book the
// same BMP removal. Measured on this deck: base 0.032 lb with the late
// order against 0.348 lb (legacy 0.348).
TEST(MsxSurfaceTwin, PollutantLidRemovalDeclaredBeforePollutantsApplies) {
    TwinOpts o; o.routing = "PERVIOUS"; o.pct_routed = "50"; o.lid_controls = kFlowingBioRetention;
    o.lid_extra = "BC2  REMOVALS  TSS  30\n";
    o.lid_usage = "S1  BC2  1  4000  50  100  100  0  *  J1\n";
    std::string early = twinDeck(o);
    // Pollutant-only: drop the species rows.
    std::string kept;
    for (std::size_t a = 0, b; a < early.size(); a = b + 1) {
        b = early.find('\n', a); if (b == std::string::npos) b = early.size();
        const std::string line = early.substr(a, b - a);
        if (line.rfind("LU1  X  ", 0) == 0) continue;
        kept += line + "\n";
    }
    early = kept + "\n[REPORT]\nINPUT NO\n";
    // Late order: move the [POLLUTANTS] block to the end.
    std::string late = early;
    const auto p0 = late.find("[POLLUTANTS]");
    const auto p1 = late.find("\n[", p0 + 1);
    const std::string block = late.substr(p0, p1 - p0);
    late.erase(p0, p1 - p0);
    late.insert(late.find("[REPORT]"), block + "\n");
    auto bmp = [](const std::string& tag, const std::string& body) {
        write(tag + ".inp", body);
        SWMM_Engine e = swmm_engine_create();
        double v = -1.0;
        if (swmm_engine_open(e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(), (tag + ".out").c_str(), nullptr) == SWMM_OK &&
            swmm_engine_initialize(e) == SWMM_OK && swmm_engine_start(e, 1) == SWMM_OK) {
            double t = 0.0;
            while (swmm_engine_step(e, &t) == SWMM_OK && t > 0.0) {}
            swmm_engine_end(e);
            v = cpp(e).context().mass_balance.qual_bmp_removal[0];
        }
        swmm_engine_close(e); swmm_engine_destroy(e);
        return v;
    };
    const double a = bmp("_tw_rm_early", early), b = bmp("_tw_rm_late", late);
    ASSERT_GT(a, 0.0);
    EXPECT_EQ(b, a) << "REMOVALS before [POLLUTANTS] booked " << b << " against " << a;
}
