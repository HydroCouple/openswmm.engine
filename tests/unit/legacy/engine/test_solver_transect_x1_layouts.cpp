/*
 *   test_solver_transect_x1_layouts.cpp
 *
 *   The two [TRANSECTS] X1 line layouts (review IO-29, issue #173).
 *
 *   The manual documents the X1 line with 11 items (Name Nsta Xleft Xright
 *   0 0 0 Lfactor Wfactor Eoffset) while the GUI writes 10 (one fewer zero);
 *   transect.c read every X1 line in the 10-item layout, so a manual-layout
 *   line had its Lfactor read as Wfactor and the section came out scaled.
 *   Check from IO-29_test.c of HydroCouple/swmm-code-review (e13c013): the
 *   same trapezoid entered in both layouts (Lfactor 2.0) must report full
 *   depth 5.0, full area 150 and max. width 40 ft for both, with equal
 *   hydraulic radius and full flow (report precision 0.01).
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include "openswmm_solver.h"
#include "../../xsect_summary_rpt.hpp"

#define XR "../../../engine/data/xsect_review/"

TEST(SolverTransectX1Layouts, TenAndElevenItemLinesDescribeTheSameSection) {
    ASSERT_EQ(swmm_open(XR "IO-29_x1-layouts.inp", XR "_IO-29_legacy.rpt", XR "_IO-29_legacy.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);
    double t = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
    } while (!err && t > 0.0);
    swmm_end();
    if (!err) err = swmm_report();
    swmm_close();
    ASSERT_EQ(err, 0);

    const auto rows = readXsectSummary(XR "_IO-29_legacy.rpt");
    ASSERT_TRUE(rows.count("C1") && rows.count("C2"));
    const auto& gui = rows.at("C1");     // 10 items
    const auto& manual = rows.at("C2");  // 11 items
    for (const auto* r : {&gui, &manual}) {
        EXPECT_NEAR(r->depth, 5.0, 0.01);
        EXPECT_NEAR(r->area, 150.0, 0.01);
        EXPECT_NEAR(r->wmax, 40.0, 0.01);
    }
    EXPECT_NEAR(manual.rfull, gui.rfull, 0.01);
    EXPECT_NEAR(manual.qfull, gui.qfull, 0.01);
}
