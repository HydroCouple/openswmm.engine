/*
 *   test_solver_irregular_barrels.cpp
 *
 *   Barrels of IRREGULAR and STREET cross sections (review IO-11, issue #176).
 *
 *   link.c returned from the IRREGULAR / STREET branch of the [XSECTIONS]
 *   reader before reading the Barrels item, so every transect conduit had one
 *   barrel whatever the deck said. Deck and check from IO-11_test.c of
 *   HydroCouple/swmm-code-review (e13c013): two identical channels on the same
 *   transect, C1 with 2 barrels under 20 cfs and C2 with 1 barrel under 10 cfs,
 *   must report 2 and 1 barrels and reach the same upstream depth (to 0.005 ft).
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 regression data under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "openswmm_solver.h"
#include "../../xsect_summary_rpt.hpp"

#define XR "../../../engine/data/xsect_review/"

TEST(SolverIrregularBarrels, TwoBarrelsUnderTwiceTheFlowMatchOneBarrel) {
    ASSERT_EQ(swmm_open(XR "IO-11_irregular-barrels.inp", XR "_IO-11_legacy.rpt", XR "_IO-11_legacy.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);
    const int j1 = swmm_getIndex(swmm_NODE, "J1"), j2 = swmm_getIndex(swmm_NODE, "J2");
    double t = 0.0, y1 = 0.0, y2 = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
        if (t <= 0.0) break;
        y1 = swmm_getValue(swmm_NODE_DEPTH, j1);
        y2 = swmm_getValue(swmm_NODE_DEPTH, j2);
    } while (!err);
    swmm_end();
    if (!err) err = swmm_report();
    swmm_close();
    ASSERT_EQ(err, 0);

    const auto rows = readXsectSummary(XR "_IO-11_legacy.rpt");
    ASSERT_TRUE(rows.count("C1") && rows.count("C2"));
    EXPECT_EQ(rows.at("C1").barrels, 2);
    EXPECT_EQ(rows.at("C2").barrels, 1);
    EXPECT_NEAR(y1, y2, 0.005) << "upstream depths of the 2-barrel and 1-barrel channels";
}
