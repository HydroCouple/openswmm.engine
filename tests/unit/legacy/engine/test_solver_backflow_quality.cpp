/*!
 * @file test_solver_backflow_quality.cpp
 * @brief Legacy-engine gates for `[OPTIONS] OUTFALL_BACKFLOW_QUALITY
 *        LAST|ZERO` — the quality carried by reverse flow at outfalls.
 *
 * @details Same two-phase deck as the refactored engine's
 *          test_outfall_backflow_quality.cpp: an hour of polluted inflow
 *          (100 mg/L) drains J1 -> C1 -> O1 and loads the outfall's held
 *          state, then the inflow stops and the outfall's TIMESERIES stage
 *          ramps above the junction so boundary water pours back for two
 *          hours. LAST (default, stock qualrout.c behavior) re-injects the
 *          held ~100 mg/L; ZERO delivers a fresh boundary.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>

#include "openswmm_solver.h"

namespace {

void write_deck(const std::string& path, const char* bfq_line,
                bool reversing, bool constant_inflow = false) {
    std::ofstream f(path);
    f << "[TITLE]\nLegacy outfall backflow quality gate deck\n\n"
      << "[OPTIONS]\n"
      << "FLOW_UNITS           CFS\n"
      << "FLOW_ROUTING         DYNWAVE\n"
      << "INFILTRATION         HORTON\n"
      << "LINK_OFFSETS         DEPTH\n"
      << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
      << "REPORT_START_DATE    01/01/2026\nREPORT_START_TIME    00:00:00\n"
      << "END_DATE             01/01/2026\nEND_TIME             03:00:00\n"
      << "ROUTING_STEP         0:00:05\nREPORT_STEP          0:05:00\n";
    if (bfq_line != nullptr) f << bfq_line << "\n";
    // O2 (FREE, low) keeps phase 2 a continuous flushing circuit
    // O1 -> C1 -> J1 -> C2 -> O2 (same shape as the refactored engine's
    // test — without it the backflow stalls once heads equalize).
    // Storage node with an explicit area — same deck shape as the
    // refactored engine's test so the two suites stay comparable.
    f << "\n[STORAGE]\nJ1 0.0 12.0 0 FUNCTIONAL 0 0 100 0 0\n\n"
      << "[OUTFALLS]\nO1 -0.5 TIMESERIES stage_ts NO\n"
      << "O2 -0.5 FREE NO\n\n"
      << "[CONDUITS]\nC1 J1 O1 200 0.013 0 0 0\n"
      << "C2 J1 O2 100 0.013 0.5 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 2.0 0 0 0\nC2 CIRCULAR 1.0 0 0 0\n\n"
      << "[POLLUTANTS]\nTRC MG/L 0 0 0 0 NO * 0 0 0\n\n"
      << "[INFLOWS]\n"
      << "J1 FLOW inflow_ts FLOW 1.0 1.0\n"
      << "J1 TRC \"\" CONCEN 1.0 1.0 100\n\n"
      << "[TIMESERIES]\n";
    if (constant_inflow) {
        f << "inflow_ts 0:00 3.0\ninflow_ts 3:00 3.0\n";
    } else {
        f << "inflow_ts 0:00 3.0\ninflow_ts 0:55 3.0\ninflow_ts 1:00 0.0\n"
          << "inflow_ts 3:00 0.0\n";
    }
    if (reversing) {
        f << "stage_ts 0:00 0.2\nstage_ts 1:00 0.2\nstage_ts 1:10 5.0\n"
          << "stage_ts 3:00 9.0\n";
    } else {
        f << "stage_ts 0:00 0.2\nstage_ts 3:00 0.2\n";
    }
    f << "\n[REPORT]\nINPUT NO\n";
}

struct LegacyRun {
    bool ok = false;
    double j1_conc = -1.0;
    double o1_conc = -1.0;
};

LegacyRun run_deck(const std::string& tag, const char* bfq_line,
                   bool reversing = true, bool constant_inflow = false) {
    LegacyRun r;
    const std::string inp = tag + ".inp";
    write_deck(inp, bfq_line, reversing, constant_inflow);
    if (swmm_open(inp.c_str(), (tag + ".rpt").c_str(),
                  (tag + ".out").c_str()) != 0) {
        ADD_FAILURE() << "swmm_open failed for " << inp;
        return r;
    }
    if (swmm_start(1) != 0) {
        ADD_FAILURE() << "swmm_start failed for " << inp;
        swmm_close();
        return r;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_step(&elapsed) != 0) {
            ADD_FAILURE() << "swmm_step failed for " << inp;
            swmm_end();
            swmm_close();
            return r;
        }
    } while (elapsed > 0.0 && ++guard < 200000);

    const int j1 = swmm_getIndex(swmm_NODE, "J1");
    const int o1 = swmm_getIndex(swmm_NODE, "O1");
    if (j1 < 0 || o1 < 0) {
        ADD_FAILURE() << "node lookup failed (j1=" << j1 << " o1=" << o1
                      << ")";
    } else {
        r.j1_conc = swmm_getValueExpanded(
            swmm_NODE, swmm_NODE_POLLUTANT_CONCENTRATION, j1, 0, 0);
        r.o1_conc = swmm_getValueExpanded(
            swmm_NODE, swmm_NODE_POLLUTANT_CONCENTRATION, o1, 0, 0);
        r.ok = true;
    }
    swmm_end();
    swmm_close();
    return r;
}

// LAST keeps the junction near the loading concentration through two hours
// of backflow; ZERO flushes it with clean boundary water and the supplying
// outfall itself reads zero.
TEST(LegacyOutfallBackflowQuality, LastVsZeroSeparateDecisively) {
    const LegacyRun rl =
        run_deck("_lbq_last", "OUTFALL_BACKFLOW_QUALITY LAST");
    const LegacyRun rz =
        run_deck("_lbq_zero", "OUTFALL_BACKFLOW_QUALITY ZERO");
    ASSERT_TRUE(rl.ok && rz.ok);
    RecordProperty("last_j1_conc", std::to_string(rl.j1_conc));
    RecordProperty("zero_j1_conc", std::to_string(rz.j1_conc));
    EXPECT_GT(rl.j1_conc, 50.0)
        << "LAST: held boundary re-injection should keep J1 polluted";
    EXPECT_LT(rz.j1_conc, 15.0)
        << "ZERO: fresh boundary water should flush J1";
    EXPECT_GT(rl.j1_conc, rz.j1_conc * 4.0);
    EXPECT_LE(rz.o1_conc, 1e-9)
        << "ZERO: a supplying outfall's held state must read zero";
}

// The default (no option line) is the stock LAST behavior, exactly.
TEST(LegacyOutfallBackflowQuality, DefaultIsLast) {
    const LegacyRun rd = run_deck("_lbq_default", nullptr);
    const LegacyRun rl =
        run_deck("_lbq_explicit_last", "OUTFALL_BACKFLOW_QUALITY LAST");
    ASSERT_TRUE(rd.ok && rl.ok);
    EXPECT_EQ(rd.j1_conc, rl.j1_conc);
    EXPECT_EQ(rd.o1_conc, rl.o1_conc);
}

// The flag must be inert while every outfall keeps receiving (constant
// inflow, stage held low); it bites whenever an outfall takes no inflow —
// idle as well as supplying — so the zero-inflow tail is not used here.
TEST(LegacyOutfallBackflowQuality, InertWithoutBackflow) {
    const LegacyRun rl = run_deck("_lbq_inert_last",
                                  "OUTFALL_BACKFLOW_QUALITY LAST",
                                  false, true);
    const LegacyRun rz = run_deck("_lbq_inert_zero",
                                  "OUTFALL_BACKFLOW_QUALITY ZERO",
                                  false, true);
    ASSERT_TRUE(rl.ok && rz.ok);
    EXPECT_EQ(rl.j1_conc, rz.j1_conc);
    EXPECT_EQ(rl.o1_conc, rz.o1_conc);
}

// An unknown value is a keyword error at open, like every other enum option.
TEST(LegacyOutfallBackflowQuality, UnknownValueIsRefused) {
    write_deck("_lbq_bogus.inp", "OUTFALL_BACKFLOW_QUALITY SOMETIMES", true);
    EXPECT_NE(swmm_open("_lbq_bogus.inp", "_lbq_bogus.rpt",
                        "_lbq_bogus.out"), 0);
    swmm_close();
}

}  // namespace
