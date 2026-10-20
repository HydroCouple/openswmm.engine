// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_transect_right_wall.cpp
 * @brief Transect right end wall in the wetted perimeter (review NUM-24, issue #169).
 *
 * Transect.cpp added the vertical wall at a transect's left end to the wetted
 * perimeter when the left bank is lower than the section's full height, but
 * not the one at its right end, so a section and its mirror image routed
 * differently. Check from NUM-24_test6.c of HydroCouple/swmm-code-review
 * (e13c013): T1 (left end 1 ft above the bed, right end at full height) and
 * T2 (its reflection) must have the same full flow to 0.5% and the same depth
 * under 60 cfs to 1%, and both the analytic one (A = 44 ft2,
 * P = 4 + sqrt 5 + 6 + sqrt 29 ft) to 1%.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <cmath>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "../xsect_summary_rpt.hpp"

TEST(TransectRightWall, MirrorImageSectionsRouteAlike) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "xsect_review/NUM-24_mirror.inp", "xsect_review/_NUM-24.rpt",
                               "xsect_review/_NUM-24.out", nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    const int nd[2] = {swmm_node_index(e, "J1"), swmm_node_index(e, "J2")};
    double slope = 0.0;
    ASSERT_EQ(swmm_link_get_slope(e, swmm_link_index(e, "C1"), &slope), SWMM_OK);
    double depth[2] = {}, t = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &t), SWMM_OK);
        for (int i = 0; i < 2; ++i) swmm_node_get_depth(e, nd[i], &depth[i]);
    } while (t > 0.0);
    ASSERT_EQ(swmm_engine_end(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_report(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    const auto rows = readXsectSummary("xsect_review/_NUM-24.rpt");
    ASSERT_TRUE(rows.count("C1") && rows.count("C2"));
    const double q1 = rows.at("C1").qfull, q2 = rows.at("C2").qfull;
    const double a = 44.0, p = 4.0 + std::sqrt(5.0) + 6.0 + std::sqrt(29.0);
    const double expect = 1.486 / 0.03 * a * std::pow(a / p, 2.0 / 3.0) * std::sqrt(slope);
    EXPECT_NEAR(q2 / q1, 1.0, 0.005) << "mirror full flows " << q1 << " vs " << q2;
    EXPECT_NEAR(depth[1] / depth[0], 1.0, 0.01) << "depths " << depth[0] << " vs " << depth[1];
    EXPECT_NEAR(q1 / expect, 1.0, 0.01) << "T1 vs analytic";
    EXPECT_NEAR(q2 / expect, 1.0, 0.01) << "T2 vs analytic";
}
