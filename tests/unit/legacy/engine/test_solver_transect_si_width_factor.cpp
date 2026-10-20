/*
 *   test_solver_transect_si_width_factor.cpp
 *
 *   Transect bank stations with a width factor in SI (review NUM-23, issue #168).
 *
 *   transect.c located the bank stations by comparing the scaled station
 *   values for exact equality; in SI with a station width factor the product
 *   does not round the same way twice, the banks were not found, and the whole
 *   section took the channel roughness. Check from NUM-23_test.c of
 *   HydroCouple/swmm-code-review (e13c013): T1 (0/10/30/40 m, banks 10/30,
 *   Wfactor 1.5) and T2 (the same stations entered scaled) must both give the
 *   analytic composite Manning full flow to 2% and the same depth under
 *   5 m3/s to 1%.
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

TEST(SolverTransectSiWidthFactor, ScaledStationsMatchTheSectionEnteredDirectly) {
    ASSERT_EQ(swmm_open(XR "NUM-23_si-wfactor.inp", XR "_NUM-23_legacy.rpt", XR "_NUM-23_legacy.out"), 0);
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
    const double expect = std::sqrt(slope) * (2.0 / 0.10 * 15.0 * std::pow(15.0 / std::sqrt(229.0), 2.0 / 3.0)
                                              + 1.0 / 0.03 * 60.0 * std::pow(60.0 / 30.0, 2.0 / 3.0));
    EXPECT_NEAR(qfull[0] / expect, 1.0, 0.02) << "T1 (Wfactor 1.5)";
    EXPECT_NEAR(qfull[1] / expect, 1.0, 0.02) << "T2 (stations entered scaled)";
    EXPECT_NEAR(depth[0] / depth[1], 1.0, 0.01) << "depths " << depth[0] << " vs " << depth[1];
}
