/*
 *   test_solver_xsect_before_link.cpp
 *
 *   An [XSECTIONS] row read before its link's own row (review CRASH-02, issue #178).
 *
 *   link.c's xsect reader indexed Conduit[] by the link's conduit sub-index
 *   before the link's own section had set it, so a model with [XSECTIONS]
 *   above [WEIRS] (or an orifice whose node is undefined, which leaves the
 *   link half-built) wrote past the conduit array. Decks and checks from
 *   CRASH-02_test.c of HydroCouple/swmm-code-review (e13c013):
 *     - CRASH-02_orifice-bad-node.inp is rejected with "ERROR 209: undefined
 *       object O1x" (and nothing worse);
 *     - CRASH-02_xsect-before-weir.inp runs, and its weir passes the 5 cfs
 *       inflow (to 0.05 cfs) at the end of the run.
 *
 *   Working directory is tests/unit/legacy/engine/data; the decks are shared
 *   with the 6.0.0 regression data under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <fstream>
#include <string>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

namespace {

bool reportHas(const char* rpt, const std::string& text) {
    std::ifstream f(rpt);
    for (std::string line; std::getline(f, line);)
        if (line.find(text) != std::string::npos) return true;
    return false;
}

}  // namespace

TEST(SolverXsectBeforeLink, OrificeWithAnUndefinedNodeIsError209) {
    EXPECT_NE(swmm_open(XR "CRASH-02_orifice-bad-node.inp", XR "_CRASH-02_badnode_legacy.rpt",
                        XR "_CRASH-02_badnode_legacy.out"),
              0);
    swmm_close();
    EXPECT_TRUE(reportHas(XR "_CRASH-02_badnode_legacy.rpt", "ERROR 209: undefined object O1x"));
}

TEST(SolverXsectBeforeLink, XsectionsAboveWeirsRunsAndPassesTheInflow) {
    ASSERT_EQ(swmm_open(XR "CRASH-02_xsect-before-weir.inp", XR "_CRASH-02_weir_legacy.rpt",
                        XR "_CRASH-02_weir_legacy.out"),
              0);
    const int w1 = swmm_getIndex(swmm_LINK, "W1");
    ASSERT_EQ(swmm_start(1), 0);
    double t = 0.0, q = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
        if (t <= 0.0) break;
        q = swmm_getValue(swmm_LINK_FLOW, w1);
    } while (!err);
    swmm_end();
    swmm_close();
    ASSERT_EQ(err, 0);
    EXPECT_NEAR(q, 5.0, 0.05) << "weir flow at the end of the run";
}
