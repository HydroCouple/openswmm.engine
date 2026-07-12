/*
 *   test_solver_storage_shapes.cpp
 *
 *   Created: 2026-07-12
 *
 *   Regression tests for storage_getDepth()/storage_getVolDiff() (node.c),
 *   covering two bugs found in a nonlinear (Newton-solved) STORAGE shape:
 *
 *     1. storage_getVolDiff() passed the storage unit's subIndex to
 *        storage_getVolume()/storage_getSurfArea(), which both expect a
 *        NODE index and re-derive the subIndex themselves via
 *        Node[j].subIndex. Whenever a storage node isn't at the same
 *        position in Node[] as its own subIndex (i.e. any storage node
 *        preceded by another node type — the normal case), the Newton
 *        solve read the wrong node's geometry.
 *
 *     2. storage_getDepth() runs its Newton solve in USER (display) units,
 *        but storage_getVolDiff()'s callback passed the trial depth
 *        straight into storage_getVolume()/storage_getSurfArea(), which
 *        expect INTERNAL (ft) units and return INTERNAL (ft3/ft2) results.
 *        The residual then mixed the two unit systems. This was silent
 *        under US units (UCF(LENGTH) == UCF(VOLUME) == 1.0) but wrong
 *        under SI.
 *
 *   Both fixtures below place STOR1 at node index 1 (preceded by J1 at
 *   index 0, so STOR1's storage subIndex is 0 != its node index 1 —
 *   triggering bug #1 pre-fix) with a FUNCTIONAL shape (A0=50, A1=10,
 *   A2=2) that forces storage_getDepth() into its Newton branch (bug #2's
 *   code path). storage_shapes_us.inp is US units (CFS); storage_shapes_si
 *   .inp is identical except for FLOW_UNITS (CMS), which is what exposes
 *   bug #2.
 *
 *   Rather than predicting the exact depth/volume trajectory produced by
 *   KINWAVE's iterative storage routing (which would require re-deriving
 *   the routing solver by hand), these tests check a routing-independent
 *   invariant: at every reporting step, the node's reported NODE_VOLUME
 *   must equal the FUNCTIONAL-shape volume integral evaluated at the
 *   reported NODE_DEPTH. That invariant is exactly what both bugs break,
 *   and it must hold regardless of how KINWAVE arrived at that depth.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "openswmm_solver.h"

#define US_MODEL_INP "./storage_shapes_us.inp"
#define US_RPT       "./_storage_shapes_us.rpt"
#define US_OUT       "./_storage_shapes_us.out"

#define SI_MODEL_INP "./storage_shapes_si.inp"
#define SI_RPT       "./_storage_shapes_si.rpt"
#define SI_OUT       "./_storage_shapes_si.out"

// STOR1's FUNCTIONAL coefficients, matching [STORAGE] in both fixtures:
// area(d) = A0 + A1*d^A2;  volume(d) = A0*d + A1/(A2+1)*d^(A2+1)
namespace {
constexpr double A0 = 50.0;
constexpr double A1 = 10.0;
constexpr double A2 = 2.0;

double expectedVolume(double depth) {
    return A0 * depth + (A1 / (A2 + 1.0)) * std::pow(depth, A2 + 1.0);
}

// Runs the model to completion, sampling STOR1's depth/volume at each
// routing step and asserting NODE_VOLUME == expectedVolume(NODE_DEPTH)
// whenever depth is strictly inside (0, fullDepth) — i.e. not clamped at
// the empty or full boundary, where the geometric relation isn't the
// thing under test. Returns the number of samples actually checked, so
// the caller can assert the test wasn't vacuous.
int runAndCheckConsistency(const char* inp, const char* rpt, const char* out,
                            double relTol) {
    EXPECT_EQ(swmm_open(inp, rpt, out), 0);

    int idxJ1    = swmm_getIndex(swmm_NODE, "J1");
    int idxStor1 = swmm_getIndex(swmm_NODE, "STOR1");
    EXPECT_GE(idxJ1, 0);
    EXPECT_GE(idxStor1, 0);

    // STOR1 must NOT be at node index 0 (its own storage subIndex), or
    // this fixture no longer exercises the subIndex-vs-node-index bug.
    EXPECT_EQ(idxJ1, 0);
    EXPECT_EQ(idxStor1, 1);

    double fullDepth = swmm_getValue(swmm_NODE_MAXDEPTH, idxStor1);

    int checked = 0;
    if (swmm_start(1) == 0) {
        double elapsed = 0.0;
        do {
            if (swmm_step(&elapsed) != 0) break;

            double d = swmm_getValue(swmm_NODE_DEPTH, idxStor1);
            double v = swmm_getValue(swmm_NODE_VOLUME, idxStor1);

            if (d > 1e-6 && d < fullDepth - 1e-6) {
                double vExpected = expectedVolume(d);
                double tol = relTol * std::max(1.0, std::fabs(vExpected));
                EXPECT_NEAR(v, vExpected, tol)
                    << "STOR1 NODE_VOLUME inconsistent with its own FUNCTIONAL "
                       "shape at depth=" << d;
                ++checked;
            }
        } while (elapsed > 0.0);
        swmm_end();
    }
    swmm_close();
    return checked;
}
}  // namespace

TEST(StorageShapeConsistency, FunctionalNewton_USUnits) {
    int checked = runAndCheckConsistency(US_MODEL_INP, US_RPT, US_OUT, 0.01);
    EXPECT_GT(checked, 0) << "No mid-range depth samples were taken — "
                              "adjust the US fixture's inflow/duration";
}

TEST(StorageShapeConsistency, FunctionalNewton_SIUnits) {
    int checked = runAndCheckConsistency(SI_MODEL_INP, SI_RPT, SI_OUT, 0.01);
    EXPECT_GT(checked, 0) << "No mid-range depth samples were taken — "
                              "adjust the SI fixture's inflow/duration";
}
