/*
 *   test_solver_custom_ellipse.cpp
 *
 *   Custom elliptical pipes (review CON-17, issue #170).
 *
 *   xsect.c scaled a HORIZ_ELLIPSE / VERT_ELLIPSE with custom axes (size code
 *   0) by the standard ellipse's area and radius factors of its height alone,
 *   so a 2 x 6 ft ellipse got the area of a 2 x 3.17 ft one and routing lost
 *   the difference. Check from CON-17_test.c of HydroCouple/swmm-code-review
 *   (e13c013): a horizontal 2 x 6 and a vertical 6 x 2 ellipse must each
 *   report the full area of their own axes (pi/4 * 2 * 6 = 9.42 ft2) to 5%
 *   and run with a flow continuity error under 1%.
 *
 *   Working directory is tests/unit/legacy/engine/data; the decks are shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>

#include "openswmm_solver.h"
#include "../../xsect_summary_rpt.hpp"

#define XR "../../../engine/data/xsect_review/"

namespace {

/// Full run of one deck; returns the flow continuity error in percent.
double runDeck(const std::string& stem) {
    const std::string inp = XR + stem + ".inp", rpt = XR "_" + stem + "_legacy.rpt", out = XR "_" + stem + "_legacy.out";
    EXPECT_EQ(swmm_open(inp.c_str(), rpt.c_str(), out.c_str()), 0);
    EXPECT_EQ(swmm_start(1), 0);
    double t = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
    } while (!err && t > 0.0);
    EXPECT_EQ(err, 0);
    swmm_end();
    float runoffErr = 0, flowErr = 0, qualErr = 0;
    swmm_getMassBalErr(&runoffErr, &flowErr, &qualErr);
    EXPECT_EQ(swmm_report(), 0);
    swmm_close();
    return flowErr;
}

}  // namespace

TEST(SolverCustomEllipse, BothOrientationsGetTheAreaOfTheirOwnAxes) {
    const double aEllipse = 0.25 * 3.14159265358979 * 2.0 * 6.0;  // 9.42 ft2
    for (const char* stem : {"CON-17_horiz-2x6", "CON-17_vert-6x2"}) {
        const double ce = runDeck(stem);
        const auto rows = readXsectSummary(std::string(XR "_") + stem + "_legacy.rpt");
        ASSERT_TRUE(rows.count("C1")) << stem;
        EXPECT_NEAR(rows.at("C1").area / aEllipse, 1.0, 0.05) << stem << " full area " << rows.at("C1").area;
        EXPECT_LE(std::fabs(ce), 1.0) << stem << " continuity error " << ce << " %";
    }
}
