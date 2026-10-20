/*
 *   test_solver_arch_size_codes.cpp
 *
 *   Standard ARCH size code 31 (review NUM-20, issue #167).
 *
 *   xsect.dat carried a full hydraulic radius for corrugated-steel arch code
 *   31 (36 x 46 in) far below the value its shape implies, so that one
 *   standard pipe routed less than its neighbours. Check from NUM-20_test.c
 *   of HydroCouple/swmm-code-review (e13c013): codes 30, 31 and 32 are
 *   geometrically similar (31x40, 36x46, 41x53 in), so Rfull/rise must be the
 *   same for all three. The toolkit has no Rfull getter, so it is recovered
 *   from the full flow with Manning's equation and the tabulated full areas
 *   (7.0, 9.4, 12.3 ft2); code 31's ratio is compared with the mean of its
 *   neighbours' to 3% (the bad entry is ~25% off).
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

TEST(SolverArchSizeCodes, Code31HasTheFullRadiusItsNeighboursImply) {
    const char* ids[3] = {"A30", "A31", "A32"};
    const double rise[3] = {31.0, 36.0, 41.0}, afull[3] = {7.0, 9.4, 12.3}, n = 0.024;
    ASSERT_EQ(swmm_open(XR "NUM-20_arch-codes.inp", XR "_NUM-20_legacy.rpt", XR "_NUM-20_legacy.out"), 0);
    ASSERT_EQ(swmm_start(0), 0);
    double ratio[3];
    for (int i = 0; i < 3; ++i) {
        const int k = swmm_getIndex(swmm_LINK, ids[i]);
        const double qfull = swmm_getValue(swmm_LINK_FULLFLOW, k);
        const double slope = swmm_getValue(swmm_LINK_SLOPE, k);
        const double rfull = std::pow(qfull * n / (1.486 * afull[i] * std::sqrt(slope)), 1.5);
        ratio[i] = rfull / (rise[i] / 12.0);
    }
    swmm_end();
    swmm_close();
    const double ref = 0.5 * (ratio[0] + ratio[2]);
    EXPECT_NEAR(ratio[1] / ref, 1.0, 0.03) << "code 31 Rfull/rise " << ratio[1] << ", codes 30/32 give " << ref;
}
