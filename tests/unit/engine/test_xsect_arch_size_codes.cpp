// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_xsect_arch_size_codes.cpp
 * @brief Standard ARCH size code 31 (review NUM-20, issue #167).
 *
 * xsect_tables.hpp (and legacy xsect.dat) carried a full hydraulic radius for
 * corrugated-steel arch code 31 (36 x 46 in) that is far below the value its
 * shape implies, so that one standard pipe routed less than its neighbours.
 * Check from NUM-20_test6.c of HydroCouple/swmm-code-review (e13c013):
 * codes 30, 31 and 32 are geometrically similar (31x40, 36x46, 41x53 in), so
 * Rfull/rise must be the same for all three; code 31's is compared with the
 * mean of its neighbours' to 3% (the bad entry is ~25% off).
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_xsect.h>

TEST(ArchSizeCodes, Code31HasTheFullRadiusItsNeighboursImply) {
    const char* ids[3] = {"A30", "A31", "A32"};
    double ratio[3] = {};
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "xsect_review/NUM-20_arch-codes.inp", "xsect_review/_NUM-20.rpt",
                               "xsect_review/_NUM-20.out", nullptr),
              SWMM_OK);
    for (int i = 0; i < 3; ++i) {
        SWMM_XSect xs = nullptr;
        ASSERT_EQ(swmm_link_create_xsect(e, swmm_link_index(e, ids[i]), &xs), SWMM_OK) << ids[i];
        double yfull = 0, afull = 0, rfull = 0, wmax = 0;
        ASSERT_EQ(swmm_xsect_full_properties(xs, &yfull, &afull, &rfull, &wmax, nullptr, nullptr), SWMM_OK);
        swmm_xsect_free(xs);
        ratio[i] = rfull / yfull;
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    const double ref = 0.5 * (ratio[0] + ratio[2]);
    EXPECT_NEAR(ratio[1] / ref, 1.0, 0.03)
        << "code 31 Rfull/rise " << ratio[1] << ", codes 30/32 give " << ref;
}
