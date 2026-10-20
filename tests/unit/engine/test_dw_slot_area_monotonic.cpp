// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_dw_slot_area_monotonic.cpp
 * @brief Preissmann slot area under SURCHARGE_METHOD SLOT (review CON-18, issue #171).
 *
 * DynamicWave.cpp's slot area was not monotonic in head: between about 1.29
 * and 1.78 pipe diameters the area assigned to a conduit shrank as the head
 * rose, so a closed system filled from one end lost volume (the stored total
 * fell while inflow continued). Check from CON-18_test6.c of
 * HydroCouple/swmm-code-review (e13c013): a full 3 ft pipe between two 20 ft2
 * storage units fed by 0.035 cfs for 6 h. The conduit volume may never drop
 * by more than 1 ft3 from its running maximum, and inflow minus the change in
 * stored volume (pipe + both units) must stay within 2% of the total inflow;
 * the bug gives ~150 ft3 and ~60%.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <cmath>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>

TEST(SlotAreaMonotonic, ClosedPipeFilledFromOneEndKeepsItsVolume) {
    const double D = 3.0, L = 1000.0, kPi = 3.14159265358979;
    const double vFull = kPi * D * D / 4.0 * L;
    const double aStor = 20.0, qIn = 0.035;
    const double v0 = vFull + 2.0 * aStor * D;  // full pipe, both units at 3 ft
    const double inTotal = qIn * 6.0 * 3600.0;

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, "xsect_review/CON-18_closed-pipe.inp", "xsect_review/_CON-18.rpt",
                               "xsect_review/_CON-18.out", nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    const int j1 = swmm_node_index(e, "J1"), j2 = swmm_node_index(e, "J2");
    const int c1 = swmm_link_index(e, "C1");
    double t = 0.0, runMax = -1.0, drop = 0.0, etaDrop = 0.0, worstMiss = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &t), SWMM_OK);  // t in days
        if (t <= 0.0) break;
        double vol = 0.0, h1 = 0.0, h2 = 0.0;
        swmm_link_get_volume(e, c1, &vol);
        swmm_node_get_head(e, j1, &h1);
        swmm_node_get_head(e, j2, &h2);
        runMax = std::max(runMax, vol);
        if (runMax - vol > drop) { drop = runMax - vol; etaDrop = 0.5 * (h1 + h2) / D; }
        const double stored = vol + aStor * (h1 + h2) - v0;
        const double miss = qIn * t * 86400.0 - stored;
        if (std::fabs(miss) > std::fabs(worstMiss)) worstMiss = miss;
    } while (t > 0.0);
    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_report(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    EXPECT_LE(drop, 1.0) << "conduit volume fell " << drop << " ft3 below its running maximum at "
                         << etaDrop << " D";
    EXPECT_LE(std::fabs(worstMiss), 0.02 * inTotal) << worstMiss << " ft3 of the inflow is missing from storage";
}
