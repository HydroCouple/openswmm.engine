/**
 * @file      test_fv_junction_root.cpp
 * @brief     The algebraic junction's head root find: how well the head it
 *            returns balances the junction, and how many flux-balance
 *            evaluations one solve costs.
 *
 * @details The junction head is the root of the monotone flux balance R(h)
 *          (ExplicitFvSolver::solveAlgebraicNode), and each evaluation of R
 *          re-solves every incident face. On a surcharged network that solve is
 *          about half of the solver's run time, so its evaluation count is a
 *          first-order cost. The count is read from the engine's own
 *          `[PERF-FV]` line (printed at close when OPENSWMM_PERF is set, which
 *          CMakeLists.txt does for this test): the counters themselves are
 *          header-inline variables, so a test linked against the shared
 *          library sees its own copy, not the solver's.
 *
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "fv_test_support.hpp"

using namespace fvtest;
namespace k = openswmm::fv::kernels;

namespace {

/**
 * @brief Drive one junction feeding a 1000 ft, 3 ft circular pipe with a swinging inflow.
 *
 * Node 0 is a plain (algebraic) junction with the lateral inflow, node 1 a fixed stage. The inflow
 * swings between 5 and 45 cfs with a 60 s period, so the junction head has to be found afresh at
 * every substep.
 *
 * @param[in] stage Fixed downstream head in feet above the flat invert; above 3 the pipe is
 *     pressurized, below it flows with a free surface.
 * @param[out] head_swing Range the junction head moved over, ft.
 * @return Largest |unbalanced volume| the solve left at the junction after any advance, ft3.
 */
double worstJunctionCarry(double stage, double& head_swing) {
    Channel ch = makeNodedChannel(circular(3.0), 40, 25.0, [](double) { return 0.0; }, 0.013);
    for (int i = 0; i < ch.n; ++i)
        ch.state.cell_a[static_cast<std::size_t>(i)] = k::areaOfDepth(ch.mesh.geom[0], stage);
    ch.state.node_head[0] = stage;
    ch.state.node_head[1] = stage;

    ExplicitFvSolver s;
    s.initialize(ch.mesh, ch.state, defaultOptions());
    std::vector<double> lateral = {0.0, 0.0};
    std::vector<double> fixed = {std::numeric_limits<double>::quiet_NaN(), stage};
    FvStepForcing f;
    f.node_lateral = lateral.data();
    f.node_fixed_head = fixed.data();
    f.n_nodes = 2;

    double worst = 0.0, h_min = stage, h_max = stage;
    for (double t = 0.0; t < 300.0; t += 1.0) {
        lateral[0] = 25.0 + 20.0 * std::sin(2.0 * 3.14159265358979323846 * t / 60.0);
        s.advance(t, t + 1.0, f);
        worst = std::max(worst, std::fabs(s.node_continuity_carry()[0]));
        h_min = std::min(h_min, ch.state.node_head[0]);
        h_max = std::max(h_max, ch.state.node_head[0]);
    }
    s.finalize();
    head_swing = h_max - h_min;
    return worst;
}

const char* kOutDir = "fv_junction_root_out";

/**
 * @brief One junction, one 300 ft 3 ft circular pipe, the outfall stage fixed 12 ft above the
 *        crown, fed by an inflow that swings between 5 and 45 cfs every 10 minutes for an hour.
 * @return Owned CFS deck text.
 */
std::string swingingPressurizedDeck() {
    return "[OPTIONS]\n"
           "FLOW_UNITS           CFS\n"
           "FLOW_ROUTING         FV\n"
           "START_DATE           01/01/2026\n"
           "START_TIME           00:00:00\n"
           "END_DATE             01/01/2026\n"
           "END_TIME             01:00:00\n"
           "REPORT_STEP          00:05:00\n"
           "ROUTING_STEP         0.5\n"
           "LENGTHENING_STEP     0\n"
           "\n"
           "[JUNCTIONS]\n"
           ";;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
           "J0     100.0 40.0 15.0 0 0\n"
           "\n"
           "[OUTFALLS]\n"
           ";;Name Elev Type StageData Gated\n"
           "OF     100.0 FIXED 115.0 NO\n"
           "\n"
           "[CONDUITS]\n"
           ";;Name From To Length N Zin Zout Q0\n"
           "C1     J0   OF 300 0.013 0 0 0\n"
           "\n"
           "[XSECTIONS]\n"
           ";;Link Shape G1 G2 G3 G4\n"
           "C1     CIRCULAR 3 0 0 0 1\n"
           "\n"
           "[INFLOWS]\n"
           ";;Node Constituent TimeSeries Type Mfactor Sfactor Baseline\n"
           "J0     FLOW SWING FLOW 1.0 1.0 0\n"
           "\n"
           "[TIMESERIES]\n"
           ";;Name Date Time Value\n"
           "SWING  0:00 5\n"
           "SWING  0:05 45\n"
           "SWING  0:10 5\n"
           "SWING  0:15 45\n"
           "SWING  0:20 5\n"
           "SWING  0:25 45\n"
           "SWING  0:30 5\n"
           "SWING  0:35 45\n"
           "SWING  0:40 5\n"
           "SWING  0:45 45\n"
           "SWING  0:50 5\n"
           "SWING  0:55 45\n"
           "SWING  1:00 5\n"
           "\n"
           "[REPORT]\n"
           "CONTINUITY YES\n";
}

/**
 * @brief Pull one `key=value` number out of the engine's `[PERF-FV]` line.
 * @param[in] text Captured stderr.
 * @param[in] key Key with its trailing '=', e.g. "algresid_per_solve=".
 * @return The number, or NaN when the key is not there.
 */
double perfValue(const std::string& text, const std::string& key) {
    const auto line = text.find("[PERF-FV]");
    if (line == std::string::npos)
        return std::numeric_limits<double>::quiet_NaN();
    const auto pos = text.find(key, line);
    if (pos == std::string::npos)
        return std::numeric_limits<double>::quiet_NaN();
    return std::atof(text.c_str() + pos + key.size());
}

} // namespace

// The head the solve returns balances the junction: what it leaves unbalanced
// is the 1e-6 ft head tolerance times the junction's conductance, per substep.
// Measured 4e-6 ft3 (pressurized) and 8e-6 ft3 (free surface) with the secant
// form; the quasi-Newton loop it replaced ran out its 16-step budget on these
// fixtures and left 1.4e-4 and 4e-3 ft3.
TEST(FvJunctionRoot, TheAcceptedHeadBalancesThePressurizedJunction) {
    double swing = 0.0;
    const double worst = worstJunctionCarry(8.0, swing);
    ASSERT_GT(swing, 1.0) << "the junction head never moved: nothing was solved";
    std::printf("[fv-junction-root] pressurized: worst carry %.3g ft3, head swing %.2f ft\n", worst,
                swing);
    EXPECT_LT(worst, 5.0e-5);
}

TEST(FvJunctionRoot, TheAcceptedHeadBalancesTheFreeSurfaceJunction) {
    double swing = 0.0;
    const double worst = worstJunctionCarry(2.0, swing);
    ASSERT_GT(swing, 0.2) << "the junction head never moved: nothing was solved";
    std::printf("[fv-junction-root] free surface: worst carry %.3g ft3, head swing %.2f ft\n",
                worst, swing);
    EXPECT_LT(worst, 5.0e-5);
}

// A pressurized ghost is where a step on the estimated slope sum(sqrt(g A T))
// converged only linearly: the estimate is off by a constant factor there, and
// the solve cost 8 to 13 residual evaluations on a surcharged sewer network
// and 17.3 on this deck. The secant through the residuals already evaluated
// needs no slope: 4.0 here.
TEST(FvJunctionRoot, APressurizedJunctionIsSolvedInAFewResidualEvaluations) {
    ASSERT_NE(std::getenv("OPENSWMM_PERF"), nullptr)
        << "run with OPENSWMM_PERF=1 (ctest sets it): the count comes from the [PERF-FV] line";
    std::filesystem::create_directories(kOutDir);
    const std::string inp = std::string(kOutDir) + "/swinging_pressurized.inp";
    const std::string rpt = std::string(kOutDir) + "/swinging_pressurized.rpt";
    const std::string out = std::string(kOutDir) + "/swinging_pressurized.out";
    {
        std::ofstream f(inp);
        f << swingingPressurizedDeck();
    }

    testing::internal::CaptureStderr();
    SWMM_Engine e = swmm_engine_create();
    const int opened = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    int ran = -1;
    if (opened == 0 && swmm_engine_initialize(e) == 0 && swmm_engine_start(e, 0) == 0) {
        double elapsed = 0.0;
        do {
            ran = swmm_engine_step(e, &elapsed);
        } while (ran == 0 && elapsed > 0.0);
        swmm_engine_end(e);
        swmm_engine_report(e);
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    const std::string err = testing::internal::GetCapturedStderr();
    ASSERT_EQ(opened, 0) << err;
    ASSERT_EQ(ran, 0) << err;

    const double solves = perfValue(err, "n.algsolve=");
    const double per_solve = perfValue(err, "algresid_per_solve=");
    ASSERT_FALSE(std::isnan(per_solve)) << "no [PERF-FV] line in:\n" << err;
    ASSERT_GT(solves, 1000.0) << "the deck never reached the junction solve";
    std::printf("[fv-junction-root] %g junction solves, %.2f residual evaluations per solve\n",
                solves, per_solve);
    EXPECT_LT(per_solve, 5.0) << "the junction root find is converging linearly again";
}
