/*
 *   test_solver_slot_area_monotonic.cpp
 *
 *   Preissmann slot area under SURCHARGE_METHOD SLOT (review CON-18, issue #171).
 *
 *   dwflow.c's slot area was not monotonic in head: between about 1.29 and
 *   1.78 pipe diameters the area assigned to a conduit shrank as the head
 *   rose, so a closed system filled from one end lost volume. Check from
 *   CON-18_test.c of HydroCouple/swmm-code-review (e13c013): a full 3 ft pipe
 *   between two 20 ft2 storage units fed by 0.035 cfs for 6 h, read back from
 *   the binary output at each 5 min report period. The conduit volume may
 *   never drop by more than 1 ft3 from its running maximum, and inflow minus
 *   the change in stored volume (pipe + both units) must stay within 2% of the
 *   total inflow; the bug gives ~150 ft3 and ~60%.
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>

#include "openswmm_output.h"
#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

TEST(SolverSlotAreaMonotonic, ClosedPipeFilledFromOneEndKeepsItsVolume) {
    const double D = 3.0, L = 1000.0, kPi = 3.14159265358979;
    const double vFull = kPi * D * D / 4.0 * L;
    const double aStor = 20.0, qIn = 0.035;
    const double v0 = vFull + 2.0 * aStor * D;  // full pipe, both units at 3 ft
    const double inTotal = qIn * 6.0 * 3600.0;

    ASSERT_EQ(swmm_open(XR "CON-18_closed-pipe.inp", XR "_CON-18_legacy.rpt", XR "_CON-18_legacy.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);
    const int j1 = swmm_getIndex(swmm_NODE, "J1"), j2 = swmm_getIndex(swmm_NODE, "J2");
    const int c1 = swmm_getIndex(swmm_LINK, "C1");
    double t = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
    } while (!err && t > 0.0);
    swmm_end();
    swmm_close();
    ASSERT_EQ(err, 0);

    SMO_Handle out = nullptr;
    float *vol = nullptr, *h1 = nullptr, *h2 = nullptr;
    int nPer = 0, len = 0;
    ASSERT_EQ(SMO_init(&out), 0);
    ASSERT_EQ(SMO_open(out, XR "_CON-18_legacy.out"), 0);
    ASSERT_EQ(SMO_getTimes(out, SMO_numPeriods, &nPer), 0);
    ASSERT_EQ(SMO_getLinkSeries(out, c1, SMO_flow_volume, 0, nPer, &vol, &len), 0);
    ASSERT_EQ(SMO_getNodeSeries(out, j1, SMO_hydraulic_head, 0, nPer, &h1, &len), 0);
    ASSERT_EQ(SMO_getNodeSeries(out, j2, SMO_hydraulic_head, 0, nPer, &h2, &len), 0);
    ASSERT_GT(len, 0);

    // period i is at (i + 1) report steps of 300 s
    double runMax = -1.0, drop = 0.0, etaDrop = 0.0, worstMiss = 0.0;
    for (int i = 0; i < len; ++i) {
        runMax = std::max(runMax, static_cast<double>(vol[i]));
        if (runMax - vol[i] > drop) { drop = runMax - vol[i]; etaDrop = 0.5 * (h1[i] + h2[i]) / D; }
        const double stored = vol[i] + aStor * (h1[i] + h2[i]) - v0;
        const double miss = qIn * 300.0 * (i + 1) - stored;
        if (std::fabs(miss) > std::fabs(worstMiss)) worstMiss = miss;
    }
    SMO_free(reinterpret_cast<void**>(&vol));
    SMO_free(reinterpret_cast<void**>(&h1));
    SMO_free(reinterpret_cast<void**>(&h2));
    SMO_close(&out);

    EXPECT_LE(drop, 1.0) << "conduit volume fell " << drop << " ft3 below its running maximum at " << etaDrop << " D";
    EXPECT_LE(std::fabs(worstMiss), 0.02 * inTotal) << worstMiss << " ft3 of the inflow is missing from storage";
}
