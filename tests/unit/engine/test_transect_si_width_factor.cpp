// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_transect_si_width_factor.cpp
 * @brief Transect bank stations with a width factor in SI (review NUM-23, issue #168).
 *
 * Transect.cpp located the left and right bank stations by comparing the
 * scaled station values for exact equality; in SI units with a station width
 * factor the product does not round the same way twice, the banks were not
 * found, and the whole section took the channel roughness. Check from
 * NUM-23_test6.c of HydroCouple/swmm-code-review (e13c013): T1 is a 0/10/30/40 m
 * section, banks at 10/30, with Wfactor 1.5; T2 is the same section with the
 * stations already multiplied (0/15/45/60, banks 15/45). Both must give the
 * analytic composite Manning full flow (overbanks n = 0.10, channel 0.03) to
 * 2%, and the same depth under 5 m3/s to 1%.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <cmath>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "../xsect_summary_rpt.hpp"

TEST(TransectSiWidthFactor, ScaledStationsMatchTheSectionEnteredDirectly) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "xsect_review/NUM-23_si-wfactor.inp", "xsect_review/_NUM-23.rpt",
                               "xsect_review/_NUM-23.out", nullptr),
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

    const auto rows = readXsectSummary("xsect_review/_NUM-23.rpt");
    ASSERT_TRUE(rows.count("C1") && rows.count("C2"));
    // two 15 m overbanks 2 m deep on 1:7.5 slopes plus a 30 m channel 2 m deep
    const double expect = std::sqrt(slope) * (2.0 / 0.10 * 15.0 * std::pow(15.0 / std::sqrt(229.0), 2.0 / 3.0)
                                              + 1.0 / 0.03 * 60.0 * std::pow(60.0 / 30.0, 2.0 / 3.0));
    EXPECT_NEAR(rows.at("C1").qfull / expect, 1.0, 0.02) << "T1 (Wfactor 1.5)";
    EXPECT_NEAR(rows.at("C2").qfull / expect, 1.0, 0.02) << "T2 (stations entered scaled)";
    EXPECT_NEAR(depth[0] / depth[1], 1.0, 0.01) << "depths " << depth[0] << " vs " << depth[1];
}
