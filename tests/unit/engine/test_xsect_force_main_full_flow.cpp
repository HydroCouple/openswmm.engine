// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_xsect_force_main_full_flow.cpp
 * @brief FORCE_MAIN full flow under Manning (review NUM-07, issue #166).
 *
 * XSection.cpp gave a FORCE_MAIN the Hazen-Williams full section factor
 * A*R^0.63 while every consumer multiplies the factor by the Manning term
 * 1.486*sqrt(S)/n, so a force main's full flow came out 5-8% high. Decks and
 * checks follow NUM-07_test6.c of HydroCouple/swmm-code-review (e13c013):
 *  - dynamic wave: H-W force mains of 1, 0.5 and 4 ft (C = 130, S = 0.1%)
 *    must have the Manning full flow for the equivalent
 *    n = 1.067/C*(D/S)^0.04 (Hydraulics Reference Manual eqs. 7-34/7-35);
 *    there is no full-flow getter, so q_full = q / (q/q_full).
 *  - kinematic wave: 2 cfs into a 1 ft FORCE_MAIN and a 1 ft CIRCULAR pipe
 *    (n = 0.013); each passes exactly the Manning full flow.
 * Tolerance 1%: the fixed engine is within 0.1%, the bug 5-8% off.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <cmath>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace {

constexpr double kPi = 3.14159265358979;
constexpr double kC = 130.0;
const double kS = 1.0 / std::sqrt(1000.0 * 1000.0 - 1.0);  // 1 ft drop over 1000 ft

double manningFull(double d, double n, double s) {
    const double a = kPi / 4.0 * d * d, r = d / 4.0;
    return 1.486 / n * a * std::pow(r, 2.0 / 3.0) * std::sqrt(s);
}

/// Runs a deck to its end; q[i] and cap[i] hold link i's last flow and q/q_full.
void runDeck(const char* stem, double* q, double* cap, int n) {
    const std::string inp = std::string("xsect_review/") + stem + ".inp";
    const std::string rpt = std::string("xsect_review/_") + stem + ".rpt";
    const std::string out = std::string("xsect_review/_") + stem + ".out";
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    ASSERT_EQ(swmm_link_count(e), n);
    double t = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &t), SWMM_OK);
        swmm_link_get_flows_bulk(e, q, n);       // cfs
        swmm_link_get_capacities_bulk(e, cap, n);  // q / q_full
    } while (t > 0.0);
    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_report(e), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

}  // namespace

TEST(ForceMainFullFlow, HazenWilliamsMainsHaveTheManningEquivalentFullFlow) {
    const double d[3] = {1.0, 0.5, 4.0};  // FM1, FM05, FM4 in deck order
    double q[3] = {}, cap[3] = {};
    runDeck("NUM-07_dw-hw", q, cap, 3);
    for (int i = 0; i < 3; ++i) {
        const double nEq = 1.067 / kC * std::pow(d[i] / kS, 0.04);
        ASSERT_GT(cap[i], 0.0) << "D = " << d[i];
        EXPECT_NEAR(q[i] / cap[i] / manningFull(d[i], nEq, kS), 1.0, 0.01) << "D = " << d[i] << " ft";
    }
}

TEST(ForceMainFullFlow, KinematicWaveForceMainPassesTheManningFullFlow) {
    double q[2] = {}, cap[2] = {};  // FM1, CIRC1 in deck order
    runDeck("NUM-07_kw", q, cap, 2);
    const double qFull = manningFull(1.0, 0.013, kS);
    EXPECT_NEAR(q[0] / qFull, 1.0, 0.01) << "FORCE_MAIN";
    EXPECT_NEAR(q[1] / qFull, 1.0, 0.01) << "CIRCULAR";
}
