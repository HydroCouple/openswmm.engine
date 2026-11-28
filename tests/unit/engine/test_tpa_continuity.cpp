/**
 * @file      test_tpa_continuity.cpp
 * @brief     SURCHARGE_METHOD TPA conserves water on a pumped force main at least as well as EXTRAN.
 *
 * @details A pump lifts a constant 600 gpm inflow from a wet well into a 20 x 500 ft, 12-in force
 *          main that rises 50 ft to a high point and falls to a vented manhole and a gravity pipe to
 *          a free outfall; the interior junctions are sealed. Before the fix TPA destroyed 43 % of
 *          the water (46 % with the pump cycling): a junction beside a full-but-unengaged conduit end
 *          absorbed its inflow over the crown-cutoff half-pipe surface while the link end area stayed
 *          at A_full, and a junction above its crown stored MIN_SURFAREA x head with zero booked
 *          volume. The fix rides TPA's slot line above the crown (no crown clamp, A = A_full +
 *          w (y - y_full), width w) and gives a junction above its crown EXTRAN's zero-storage
 *          dQ/dH update. Both the always-on and the pump-cycling case are run against EXTRAN on the
 *          same deck.
 *
 * @ingroup engine_hydraulics
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "tpa_continuity_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

// The plan's force-main deck: WW (wet well) -> PMP -> WWD, P1..P12 rising to FM12 at 60 ft,
// P13..P20 falling to the vented DMH at 40 ft, G1 to a free outfall. Sealed interior junctions.
std::string forceMainDeck(const std::string& method, bool cycling,
                          const char* node_continuity = nullptr) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS GPM\nFLOW_ROUTING DYNWAVE\nLINK_OFFSETS ELEVATION\n"
          "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2026\n"
          "REPORT_START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 06:00:00\nREPORT_STEP 00:05:00\n"
          "ROUTING_STEP 30\nVARIABLE_STEP 0.75\nMINIMUM_STEP 0.5\nMIN_SURFAREA 12.5664\nMAX_TRIALS 20\n"
          "HEAD_TOLERANCE 0.00492\nFORCE_MAIN_EQUATION H-W\nSURCHARGE_METHOD "
       << method
       << "\nINERTIAL_DAMPING PARTIAL\nNORMAL_FLOW_LIMITED BOTH\nMIN_SLOPE 0\nTHREADS 1\n";
    if (method == "TPA")
        ss << "TPA_CELERITY 159\n";
    if (node_continuity)
        ss << "NODE_CONTINUITY " << node_continuity << "\n";
    ss << "\n[JUNCTIONS]\nWWD 10.000 3 0 984.252 0\n";
    const double rise[] = {14.167, 18.333, 22.500, 26.667, 30.833, 35.000,
                           39.167, 43.333, 47.500, 51.667, 55.833, 60.000};
    const double fall[] = {57.5, 55.0, 52.5, 50.0, 47.5, 45.0, 42.5};
    for (int i = 0; i < 12; ++i)
        ss << "FM" << (i + 1) << " " << rise[i] << " 3 0 984.252 0\n";
    for (int i = 0; i < 7; ++i)
        ss << "FM" << (i + 13) << " " << fall[i] << " 3 0 984.252 0\n";
    ss << "DMH 40.000 8 0 0 0\n\n[OUTFALLS]\nOUT 30 FREE NO\n\n[STORAGE]\n"
          "WW 0 20 5 FUNCTIONAL 0 0 200 0 0\n\n[CONDUITS]\n";
    std::vector<std::pair<std::string, double>> nodes = {{"WWD", 10.0}};
    for (int i = 0; i < 12; ++i)
        nodes.emplace_back("FM" + std::to_string(i + 1), rise[i]);
    for (int i = 0; i < 7; ++i)
        nodes.emplace_back("FM" + std::to_string(i + 13), fall[i]);
    nodes.emplace_back("DMH", 40.0);
    for (std::size_t i = 0; i + 1 < nodes.size(); ++i)
        ss << "P" << (i + 1) << " " << nodes[i].first << " " << nodes[i + 1].first << " 500 0.012 "
           << nodes[i].second << " " << nodes[i + 1].second << " 0 0\n";
    ss << "G1 DMH OUT 400 0.013 * 30.5 0 0\n\n[PUMPS]\nPMP WW WWD PC ON 0 0\n\n[XSECTIONS]\n";
    for (int i = 1; i <= 20; ++i)
        ss << "P" << i << " FORCE_MAIN 1 120 0 0 1\n";
    ss << "G1 CIRCULAR 2 0 0 0 1\n\n[CURVES]\nPC PUMP3 0 1800\nPC 40 1500\nPC 60 1200\nPC 80 700\n"
          "PC 100 0\n";
    if (cycling)
        ss << "\n[CONTROLS]\nRULE ON\nIF NODE WW DEPTH >= 8\nTHEN PUMP PMP STATUS = ON\nPRIORITY 1\n"
              "\nRULE OFF\nIF NODE WW DEPTH <= 3\nTHEN PUMP PMP STATUS = OFF\nPRIORITY 2\n";
    ss << "\n[INFLOWS]\nWW FLOW \"\" FLOW 1.0 1.0 600\n\n[REPORT]\nNODES ALL\nLINKS ALL\n";
    return ss.str();
}

struct DeckRun {
    double cont_err = 1e9; ///< routing continuity, fraction (0.01 = 1 %)
    double out_volume = 0; ///< integral of the G1 flow, project units x seconds
    double peak_flow = 0;  ///< max G1 flow, project units
    double t_half = -1.0;  ///< first time (s) G1 reaches half its peak
    bool ok = false;
};

DeckRun runDeck(const std::string& base, const std::string& deck) {
    DeckRun r;
    const std::string inp = outPath(base + ".inp");
    {
        std::ofstream f(inp);
        f << deck;
    }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                         outPath(base + ".out").c_str(), nullptr) != 0) {
        ADD_FAILURE() << "open failed: " << swmm_get_last_error_msg(e);
        swmm_engine_destroy(e);
        return r;
    }
    EXPECT_EQ(swmm_engine_initialize(e), 0);
    EXPECT_EQ(swmm_engine_start(e, 1), 0);
    const int g1 = swmm_link_index(e, "G1");
    EXPECT_GE(g1, 0);
    std::vector<std::pair<double, double>> series; // (seconds, flow)
    double elapsed_days = 0.0, prev_s = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed_days) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        const double now_s = elapsed_days * 86400.0;
        double q = 0.0;
        swmm_link_get_flow(e, g1, &q);
        if (now_s > prev_s) {
            r.out_volume += q * (now_s - prev_s);
            series.emplace_back(now_s, q);
            prev_s = now_s;
        }
        r.peak_flow = std::max(r.peak_flow, q);
    } while (elapsed_days > 0.0);
    for (const auto& [t, q] : series) {
        if (q >= 0.5 * r.peak_flow) {
            r.t_half = t;
            break;
        }
    }
    swmm_get_routing_continuity_error(e, &r.cont_err);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    r.ok = true;
    return r;
}

// At least as good as EXTRAN on the same deck, and never worse than the plan's 1 % line.
void expectConservesLikeExtran(const char* label, const DeckRun& tpa, const DeckRun& extran) {
    ASSERT_TRUE(tpa.ok && extran.ok);
    const double bound = std::max(0.01, std::fabs(extran.cont_err) + 0.005);
    EXPECT_LE(std::fabs(tpa.cont_err), bound)
        << label << ": TPA continuity " << 100.0 * tpa.cont_err << " % vs EXTRAN "
        << 100.0 * extran.cont_err << " %";
    ASSERT_GT(extran.out_volume, 0.0);
    EXPECT_NEAR(tpa.out_volume / extran.out_volume, 1.0, 0.02)
        << label << ": outfall volume ratio TPA/EXTRAN";
    EXPECT_NEAR(tpa.peak_flow / extran.peak_flow, 1.0, 0.10) << label << ": peak G1 flow ratio";
    ::testing::Test::RecordProperty(std::string(label) + "_tpa_continuity_pct",
                                    std::to_string(100.0 * tpa.cont_err));
    ::testing::Test::RecordProperty(std::string(label) + "_tpa_t_half_s",
                                    std::to_string(tpa.t_half));
    ::testing::Test::RecordProperty(std::string(label) + "_extran_t_half_s",
                                    std::to_string(extran.t_half));
}

} // namespace

TEST(TpaContinuity, PumpAlwaysOn) {
    const DeckRun tpa = runDeck("tpa_on", forceMainDeck("TPA", false));
    const DeckRun extran = runDeck("extran_on", forceMainDeck("EXTRAN", false));
    expectConservesLikeExtran("always_on", tpa, extran);
}

TEST(TpaContinuity, PumpCycling) {
    const DeckRun tpa = runDeck("tpa_cycling", forceMainDeck("TPA", true));
    const DeckRun extran = runDeck("extran_cycling", forceMainDeck("EXTRAN", true));
    expectConservesLikeExtran("cycling", tpa, extran);
    // Routing continuity on its own, the plan's line.
    EXPECT_LE(std::fabs(tpa.cont_err), 0.01);
}

// NODE_CONTINUITY SEMI_IMPLICIT: a surcharged EXTRAN junction must not store water over the
// crown-cutoff half-pipe area. Before the fix the 21 sealed junctions of this main held the pumped
// water as 2,800 ft² of phantom manhole (booked as held volume, so the ledger read 0.17 %), the
// wet well filled in 5.5 h instead of 7.5 min, and nothing reached the outfall in 6 h.
TEST(SemiImplicitSurcharge, ExtranForceMainDeliversLikeExplicit) {
    const DeckRun semi =
        runDeck("extran_cycling_semi", forceMainDeck("EXTRAN", true, "SEMI_IMPLICIT"));
    const DeckRun expl =
        runDeck("extran_cycling_explicit", forceMainDeck("EXTRAN", true, "EXPLICIT"));
    ASSERT_TRUE(semi.ok && expl.ok);
    ASSERT_GT(expl.out_volume, 0.0);
    EXPECT_NEAR(semi.out_volume / expl.out_volume, 1.0, 0.05)
        << "outfall volume ratio semi/explicit";
    EXPECT_NEAR(semi.peak_flow / expl.peak_flow, 1.0, 0.10) << "peak G1 flow ratio semi/explicit";
    EXPECT_LE(std::fabs(semi.cont_err), 0.03)
        << "semi-implicit continuity " << 100.0 * semi.cont_err << " %";
}
