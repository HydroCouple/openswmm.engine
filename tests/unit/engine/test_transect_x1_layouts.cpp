// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_transect_x1_layouts.cpp
 * @brief The two [TRANSECTS] X1 line layouts (review IO-29, issue #173).
 *
 * The manual documents the X1 line with 11 items (Name Nsta Xleft Xright 0 0 0
 * Lfactor Wfactor Eoffset) while the GUI writes 10 (one fewer zero);
 * LinksHandler.cpp read every X1 line in the 10-item layout, so a manual-layout
 * line had its Lfactor read as Wfactor and the section came out scaled. Check
 * from IO-29_test6.c of HydroCouple/swmm-code-review (e13c013): the same
 * trapezoid entered in both layouts (Lfactor 2.0) must report full depth 5.0,
 * full area 150 and max. width 40 ft for both, with equal hydraulic radius and
 * full flow (report precision 0.01).
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>

#include "../xsect_summary_rpt.hpp"

TEST(TransectX1Layouts, TenAndElevenItemLinesDescribeTheSameSection) {
    ASSERT_EQ(swmm_engine_run("xsect_review/IO-29_x1-layouts.inp", "xsect_review/_IO-29.rpt",
                              "xsect_review/_IO-29.out", nullptr),
              SWMM_OK);
    const auto rows = readXsectSummary("xsect_review/_IO-29.rpt");
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
