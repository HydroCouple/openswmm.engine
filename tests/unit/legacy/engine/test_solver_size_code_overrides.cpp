/*
 *   test_solver_size_code_overrides.cpp
 *
 *   Ellipse and arch size codes vs. dimensions (review IO-12, issue #172).
 *
 *   For HORIZ_ELLIPSE, VERT_ELLIPSE and ARCH, xsect_setParams() took a zero
 *   Geom2 to mean "Geom1 is the size code" (the SWMM 5.0 form) even when
 *   Geom3 held the size code the manual documents, and truncated a fractional
 *   size code without a message. Decks and checks follow IO-12_test.c of
 *   HydroCouple/swmm-code-review (e13c013); the standard dimensions are those
 *   of Appendix A12/A13 (ellipse code 4: 24 x 38 in, 5.10 ft2; arch code 12:
 *   62 x 102 in, 34.60 ft2):
 *     - IO-12_size-codes.inp runs; C1 "HORIZ_ELLIPSE 2 0 4", C2 "VERT_ELLIPSE
 *       3.17 0 4", C3 "ARCH 5.17 0 12" and C4 "HORIZ_ELLIPSE 4 0 0" (the 5.0
 *       form) all get their standard sizes;
 *     - IO-12_height-as-code.inp is rejected ("HORIZ_ELLIPSE 1.2 0 0 0" in
 *       metres has no width and 1.2 is not a size code);
 *     - IO-12_fractional-code.inp is rejected ("ARCH 4.5 7.0 12.5 0").
 *   The report carries two decimals, so the tolerance is 0.01.
 *
 *   Working directory is tests/unit/legacy/engine/data; the decks are shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
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

TEST(SolverSizeCodeOverrides, SizeCodeInGeom3WinsOverAZeroWidth) {
    ASSERT_EQ(runDeck("IO-12_size-codes"), 0);
    const auto rows = readXsectSummary(XR "_IO-12_size-codes_legacy.rpt");
    const char* ids[4] = {"C1", "C2", "C3", "C4"};
    const double expect[4][3] = {{2.00, 5.10, 3.17}, {3.17, 5.10, 2.00}, {5.17, 34.60, 8.50}, {2.00, 5.10, 3.17}};
    for (int k = 0; k < 4; ++k) {
        ASSERT_TRUE(rows.count(ids[k])) << ids[k];
        const auto& r = rows.at(ids[k]);
        EXPECT_NEAR(r.depth, expect[k][0], 0.01) << ids[k] << " full depth";
        EXPECT_NEAR(r.area, expect[k][1], 0.01) << ids[k] << " full area";
        EXPECT_NEAR(r.wmax, expect[k][2], 0.01) << ids[k] << " max. width";
    }
}

TEST(SolverSizeCodeOverrides, AHeightIsNotASizeCode) {
    EXPECT_NE(runDeck("IO-12_height-as-code"), 0);
}

TEST(SolverSizeCodeOverrides, AFractionalSizeCodeIsRejected) {
    EXPECT_NE(runDeck("IO-12_fractional-code"), 0);
}
