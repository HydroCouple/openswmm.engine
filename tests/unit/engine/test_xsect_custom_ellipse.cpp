// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_xsect_custom_ellipse.cpp
 * @brief Custom elliptical pipes (review CON-17, issue #170).
 *
 * XSection.cpp scaled a HORIZ_ELLIPSE / VERT_ELLIPSE with custom axes (size
 * code 0) by the standard ellipse's area and radius factors of its height
 * alone, so a 2 x 6 ft ellipse got the area of a 2 x 3.17 ft one and routing
 * lost the difference. Check from CON-17_test6.c of HydroCouple/swmm-code-review
 * (e13c013): a horizontal 2 x 6 and a vertical 6 x 2 ellipse must each report
 * the full area of their own axes (pi/4 * 2 * 6 = 9.42 ft2) to 5% and run with
 * a flow continuity error under 1%.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_massbalance.h>

#include "../xsect_summary_rpt.hpp"

namespace {

/// Full run of one deck; returns the routing continuity error in percent.
double runDeck(const std::string& stem) {
    const std::string inp = "xsect_review/" + stem + ".inp";
    const std::string rpt = "xsect_review/_" + stem + ".rpt";
    const std::string out = "xsect_review/_" + stem + ".out";
    SWMM_Engine e = swmm_engine_create();
    EXPECT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double t = 0.0;
    do {
        EXPECT_EQ(swmm_engine_step(e, &t), SWMM_OK);
    } while (t > 0.0);
    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    double frac = 0.0;
    swmm_get_routing_continuity_error(e, &frac);
    EXPECT_EQ(swmm_engine_report(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return 100.0 * frac;
}

}  // namespace

TEST(CustomEllipse, BothOrientationsGetTheAreaOfTheirOwnAxes) {
    const double aEllipse = 0.25 * 3.14159265358979 * 2.0 * 6.0;  // 9.42 ft2
    for (const char* stem : {"CON-17_horiz-2x6", "CON-17_vert-6x2"}) {
        const double ce = runDeck(stem);
        const auto rows = readXsectSummary(std::string("xsect_review/_") + stem + ".rpt");
        ASSERT_TRUE(rows.count("C1")) << stem;
        EXPECT_NEAR(rows.at("C1").area / aEllipse, 1.0, 0.05) << stem << " full area " << rows.at("C1").area;
        EXPECT_LE(std::fabs(ce), 1.0) << stem << " continuity error " << ce << " %";
    }
}
