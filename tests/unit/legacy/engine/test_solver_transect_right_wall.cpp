/*
 *   test_solver_transect_right_wall.cpp
 *
 *   Transect right end wall in the wetted perimeter (review NUM-24, issue #169).
 *
 *   transect.c added the vertical wall at a transect's left end to the wetted
 *   perimeter when the left bank is lower than the full height, but not the
 *   one at its right end, so a section and its mirror image routed
 *   differently. Check from NUM-24_test.c of HydroCouple/swmm-code-review
 *   (e13c013): T1 (left end 1 ft above the bed, right end at full height) and
 *   T2 (its reflection) must have the same full flow to 0.5%, the same depth
 *   under 60 cfs to 1%, and both the analytic one (A = 44 ft2,
 *   P = 4 + sqrt 5 + 6 + sqrt 29 ft) to 1%.
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

TEST(SolverTransectRightWall, MirrorImageSectionsRouteAlike) {
    ASSERT_EQ(swmm_open(XR "NUM-24_mirror.inp", XR "_NUM-24_legacy.rpt", XR "_NUM-24_legacy.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);
    const int ln[2] = {swmm_getIndex(swmm_LINK, "C1"), swmm_getIndex(swmm_LINK, "C2")};
    const int nd[2] = {swmm_getIndex(swmm_NODE, "J1"), swmm_getIndex(swmm_NODE, "J2")};
    double qfull[2], depth[2] = {0, 0};
    for (int i = 0; i < 2; ++i) qfull[i] = swmm_getValue(swmm_LINK_FULLFLOW, ln[i]);
    const double slope = swmm_getValue(swmm_LINK_SLOPE, ln[0]);
    double t = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
        if (t <= 0.0) break;
        for (int i = 0; i < 2; ++i) depth[i] = swmm_getValue(swmm_NODE_DEPTH, nd[i]);
    } while (!err);
    swmm_end();
    swmm_close();
    ASSERT_EQ(err, 0);
    const double a = 44.0, p = 4.0 + std::sqrt(5.0) + 6.0 + std::sqrt(29.0);
    const double expect = 1.486 / 0.03 * a * std::pow(a / p, 2.0 / 3.0) * std::sqrt(slope);
    EXPECT_NEAR(qfull[1] / qfull[0], 1.0, 0.005) << "mirror full flows " << qfull[0] << " vs " << qfull[1];
    EXPECT_NEAR(depth[1] / depth[0], 1.0, 0.01) << "depths " << depth[0] << " vs " << depth[1];
    EXPECT_NEAR(qfull[0] / expect, 1.0, 0.01) << "T1 vs analytic";
    EXPECT_NEAR(qfull[1] / expect, 1.0, 0.01) << "T2 vs analytic";
}
