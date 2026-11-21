/**
 * @file      test_semi_implicit_continuity.cpp
 * @brief     Converged SEMI_IMPLICIT node depths satisfy the same continuity equation as EXPLICIT.
 *
 * @details At a free-surface junction the routing step solves
 *          A * (y - y_old) = dV, with dV the trapezoidal net inflow volume. NODE_CONTINUITY
 *          EXPLICIT iterates y = y_old + dV / A; SEMI_IMPLICIT adds the head response of the
 *          connected links to the denominator so the iteration converges faster. That is a
 *          change of path, not of destination: when both iterations are run to convergence
 *          they must land on the same depths.
 *
 *          The net inflow is evaluated at the latest iterate, so the head-response term has to
 *          multiply the change from that iterate. Multiplying the change from the old depth
 *          instead counts the response twice and converges to a different, damped answer, as
 *          if every node carried extra surface area 0.5 * dt * sum(dQ/dH). This test fails on
 *          that form.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "core/SWMMEngine.hpp"

namespace {

const std::filesystem::path out = std::filesystem::path(SEMI_IMPLICIT_OUT_DIR);

/**
 * @brief Build a one-hour free-surface fixture: three junctions in series to a free outfall.
 *
 * A ramped inflow (0 to 8 cfs in 30 minutes) keeps the 2 ft pipes part full, so no node
 * surcharges and both continuity options solve the same equation. The step is fixed at 10 s and
 * the iteration is allowed 500 trials at a 1e-10 ft tolerance. Normal-flow limiting is off and
 * inertial terms are fully damped: with them on, about a quarter of the steps settle into a
 * switching cycle that never meets the tolerance under either option, and an unconverged step
 * has no single answer to compare.
 *
 * @param[in] continuity "EXPLICIT" or "SEMI_IMPLICIT".
 * @return Deck text.
 */
std::string deck(const std::string& continuity) {
    std::ostringstream f;
    f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nLINK_OFFSETS DEPTH\n"
      << "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 01:00:00\n"
      << "REPORT_STEP 00:05:00\nROUTING_STEP 10\nVARIABLE_STEP 0\nLENGTHENING_STEP 0\n"
      << "INERTIAL_DAMPING FULL\nNORMAL_FLOW_LIMITED NONE\n"
      << "MAX_TRIALS 500\nHEAD_TOLERANCE 1e-10\nTHREADS 1\n"
      << "NODE_CONTINUITY " << continuity << "\n"
      << "[JUNCTIONS]\nJ1 3 10 0 0 0\nJ2 2 10 0 0 0\nJ3 1 10 0 0 0\n"
      << "[OUTFALLS]\nO1 0 FREE NO\n"
      << "[CONDUITS]\nC1 J1 J2 200 0.013 0 0 0\nC2 J2 J3 200 0.013 0 0 0\n"
      << "C3 J3 O1 200 0.013 0 0 0\n"
      << "[XSECTIONS]\nC1 CIRCULAR 2 0 0 0\nC2 CIRCULAR 2 0 0 0\nC3 CIRCULAR 2 0 0 0\n"
      << "[INFLOWS]\nJ1 FLOW ramp FLOW 1.0 1.0\n"
      << "[TIMESERIES]\nramp 0:00 0\nramp 0:30 8\nramp 1:00 8\n"
      << "[REPORT]\nINPUT NO\n";
    return f.str();
}

struct Trace {
    std::vector<double> depth; ///< J1, J2, J3 depth after every routing step (ft)
    long steps = 0;
    long non_converged = 0;
    double max_depth = 0.0;
};

/**
 * @brief Run the fixture and record the three junction depths after every routing step.
 *
 * @param[in] continuity "EXPLICIT" or "SEMI_IMPLICIT".
 * @return Depth trace and convergence counts.
 */
Trace run(const std::string& continuity) {
    Trace t;
    std::filesystem::create_directories(out);
    const auto path = (out / (continuity + ".inp")).string();
    std::ofstream(path) << deck(continuity);
    openswmm::SWMMEngine engine;
    EXPECT_EQ(engine.open(path.c_str(), (path + ".rpt").c_str(), ""), 0);
    EXPECT_EQ(engine.initialize(), 0);
    EXPECT_EQ(engine.start(0), 0);
    const auto& ctx = engine.context();
    double elapsed = 0.0;
    do {
        EXPECT_EQ(engine.step(&elapsed), 0);
        for (std::size_t i = 0; i < 3; ++i) {
            t.depth.push_back(ctx.nodes.depth[i]);
            t.max_depth = std::max(t.max_depth, ctx.nodes.depth[i]);
        }
        ++t.steps;
    } while (elapsed > 0.0);
    t.non_converged = ctx.routing_stats.n_non_converged;
    EXPECT_EQ(engine.end(), 0);
    return t;
}

TEST(SemiImplicitContinuity, ConvergedDepthsMatchExplicitOnFreeSurface) {
    const Trace ex = run("EXPLICIT");
    const Trace si = run("SEMI_IMPLICIT");

    // The fixture does what it claims: both runs converge on every step, the pipes carry
    // water, and nothing reaches the 2 ft crown.
    EXPECT_EQ(ex.non_converged, 0);
    EXPECT_EQ(si.non_converged, 0);
    ASSERT_EQ(ex.steps, si.steps);
    ASSERT_EQ(ex.depth.size(), si.depth.size());
    EXPECT_GT(ex.max_depth, 0.5);
    EXPECT_LT(ex.max_depth, 1.9);

    double worst = 0.0;
    for (std::size_t i = 0; i < ex.depth.size(); ++i)
        worst = std::max(worst, std::fabs(ex.depth[i] - si.depth[i]));
    // Same equation, two iteration paths, both converged to 1e-10 ft per step. The
    // double-counted form differs by hundredths of a foot on this fixture.
    EXPECT_LT(worst, 1e-6);
}

} // namespace
