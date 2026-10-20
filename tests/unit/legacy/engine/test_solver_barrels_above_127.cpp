/*
 *   test_solver_barrels_above_127.cpp
 *
 *   Number of barrels stored in a char (review IO-10, issue #177).
 *
 *   objects.h held TConduit::barrels in a char, so 200 barrels became -56 and
 *   300 became 44: a conduit's full flow and volume were scaled by the wrong
 *   count, silently. Decks and check from IO-10_test.c of
 *   HydroCouple/swmm-code-review (e13c013): a conduit with 300 and one with
 *   200 barrels run and the Cross Section Summary reports the number entered.
 *
 *   Working directory is tests/unit/legacy/engine/data; the decks are shared
 *   with the 6.0.0 regression data under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <string>

#include "openswmm_solver.h"
#include "../../xsect_summary_rpt.hpp"

#define XR "../../../engine/data/xsect_review/"

namespace {

int runDeck(const std::string& stem) {
    const std::string inp = XR + stem + ".inp", rpt = XR "_" + stem + "_legacy.rpt", out = XR "_" + stem + "_legacy.out";
    int err = swmm_open(inp.c_str(), rpt.c_str(), out.c_str());
    if (!err) err = swmm_start(1);
    double t = 0.0;
    while (!err) {
        err = swmm_step(&t);
        if (t <= 0.0) break;
    }
    swmm_end();
    if (!err) err = swmm_report();
    swmm_close();
    return err;
}

}  // namespace

TEST(SolverBarrelsAbove127, TheCountEnteredIsTheCountReported) {
    for (int entered : {300, 200}) {
        const std::string stem = "IO-10_" + std::to_string(entered) + "-barrels";
        ASSERT_EQ(runDeck(stem), 0) << stem;
        const auto rows = readXsectSummary(XR "_" + stem + "_legacy.rpt");
        ASSERT_TRUE(rows.count("C1")) << stem;
        EXPECT_EQ(rows.at("C1").barrels, entered) << stem;
    }
}
