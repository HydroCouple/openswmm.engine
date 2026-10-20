/*
 *   test_solver_force_main_full_flow.cpp
 *
 *   FORCE_MAIN full flow under Manning (review NUM-07, issue #166).
 *
 *   xsect.c gave a FORCE_MAIN the Hazen-Williams full section factor
 *   A*R^0.63 while every consumer multiplies the factor by the Manning term
 *   1.486*sqrt(S)/n, so a force main's full flow came out 5-8% high. Decks
 *   and checks follow NUM-07_test.c of HydroCouple/swmm-code-review (e13c013):
 *     - dynamic wave: H-W force mains of 1, 0.5 and 4 ft (C = 130, S = 0.1%)
 *       must have the Manning full flow for the equivalent
 *       n = 1.067/C*(D/S)^0.04 (Hydraulics Reference Manual eqs. 7-34/7-35);
 *     - kinematic wave: 2 cfs into a 1 ft FORCE_MAIN and a 1 ft CIRCULAR pipe
 *       (n = 0.013); each passes exactly the Manning full flow.
 *   Tolerance 1%: the fixed engine is within 0.1%, the bug 5-8% off.
 *
 *   Working directory is tests/unit/legacy/engine/data; the decks are shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cmath>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

namespace {

constexpr double kPi = 3.14159265358979;
constexpr double kC = 130.0;
const double kS = 1.0 / std::sqrt(1000.0 * 1000.0 - 1.0);  // 1 ft drop over 1000 ft

double manningFull(double d, double n, double s) {
    const double a = kPi / 4.0 * d * d, r = d / 4.0;
    return 1.486 / n * a * std::pow(r, 2.0 / 3.0) * std::sqrt(s);
}

}  // namespace

TEST(SolverForceMainFullFlow, HazenWilliamsMainsHaveTheManningEquivalentFullFlow) {
    const char* ids[3] = {"FM1", "FM05", "FM4"};
    const double d[3] = {1.0, 0.5, 4.0};
    ASSERT_EQ(swmm_open(XR "NUM-07_dw-hw.inp", XR "_NUM-07_dw-hw_legacy.rpt", XR "_NUM-07_dw-hw_legacy.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);
    double qfull[3];
    for (int i = 0; i < 3; ++i) qfull[i] = swmm_getValue(swmm_LINK_FULLFLOW, swmm_getIndex(swmm_LINK, ids[i]));
    double t = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
    } while (!err && t > 0.0);
    swmm_end();
    swmm_close();
    ASSERT_EQ(err, 0);
    for (int i = 0; i < 3; ++i) {
        const double nEq = 1.067 / kC * std::pow(d[i] / kS, 0.04);
        EXPECT_NEAR(qfull[i] / manningFull(d[i], nEq, kS), 1.0, 0.01) << ids[i];
    }
}

TEST(SolverForceMainFullFlow, KinematicWaveForceMainPassesTheManningFullFlow) {
    ASSERT_EQ(swmm_open(XR "NUM-07_kw.inp", XR "_NUM-07_kw_legacy.rpt", XR "_NUM-07_kw_legacy.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);
    const int fm = swmm_getIndex(swmm_LINK, "FM1"), circ = swmm_getIndex(swmm_LINK, "CIRC1");
    double t = 0.0, qFm = 0.0, qCirc = 0.0;
    int err = 0;
    do {
        err = swmm_step(&t);
        if (t <= 0.0) break;
        qFm = swmm_getValue(swmm_LINK_FLOW, fm);
        qCirc = swmm_getValue(swmm_LINK_FLOW, circ);
    } while (!err);
    swmm_end();
    swmm_close();
    ASSERT_EQ(err, 0);
    const double qFull = manningFull(1.0, 0.013, kS);
    EXPECT_NEAR(qFm / qFull, 1.0, 0.01) << "FORCE_MAIN";
    EXPECT_NEAR(qCirc / qFull, 1.0, 0.01) << "CIRCULAR";
}
