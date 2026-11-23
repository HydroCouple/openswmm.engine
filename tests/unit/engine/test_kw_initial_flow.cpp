/**
 * @file test_kw_initial_flow.cpp
 * @brief A kinematic-wave conduit with an initial flow starts carrying it.
 *
 * Legacy initLinks (flowrout.c) seeds the kinematic-wave state from each
 * conduit's starting flow (Conduit.q1 = q2 = newFlow / barrels, a1 = a2 =
 * A(newDepth)). KWSolver::init zero-filled it and nothing seeded it, so v6
 * started every conduit empty: in the benchmarks deck
 * 193-h-h-si-units-elements, 25 conduits with InitFlow carried 0 at the first
 * report where legacy carried their flow. Both engines run the same deck here,
 * stepped together.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir = "kw_initial_flow_out"; // *_out/ is the gitignored scratch convention

/// C1 starts at 2 cfs with nothing flowing into J1; C2 starts dry.
const char* kDeck = R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             01:00:00
REPORT_STEP          00:05:00
ROUTING_STEP         00:00:30

[JUNCTIONS]
J1  101  5  0  0  0
J2  100  5  0  0  0

[OUTFALLS]
O1  99  FREE

[CONDUITS]
C1  J1  J2  400  0.013  0  0  2  0
C2  J2  O1  400  0.013  0  0  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1
C2  CIRCULAR  1.5  0  0  0  1
)";

} // namespace

TEST(KinematicWaveInitialFlow, MatchesLegacyFromTheFirstStep) {
    fs::create_directories(kOutDir);
    const std::string inp = (fs::path(kOutDir) / "kw_initial_flow.inp").string();
    std::ofstream(inp) << kDeck;

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (inp + ".v6.rpt").c_str(), (inp + ".v6.out").c_str(),
                               nullptr),
              0)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_open(inp.c_str(), (inp + ".legacy.rpt").c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);

    const char* names[] = {"C1", "C2"};
    int v6_idx[2], legacy_idx[2];
    for (int k = 0; k < 2; ++k) {
        v6_idx[k] = swmm_link_index(e, names[k]);
        legacy_idx[k] = swmm_getIndex(swmm_LINK, names[k]);
        ASSERT_GE(v6_idx[k], 0);
        ASSERT_GE(legacy_idx[k], 0);
    }

    double v6_elapsed = 0.0, legacy_elapsed = 0.0;
    for (int step = 1; step <= 10; ++step) {
        ASSERT_EQ(swmm_engine_step(e, &v6_elapsed), 0);
        ASSERT_EQ(swmm_step(&legacy_elapsed), 0);
        for (int k = 0; k < 2; ++k) {
            double q_v6 = 0.0;
            ASSERT_EQ(swmm_link_get_flow(e, v6_idx[k], &q_v6), 0);
            const double q_legacy = swmm_getValue(swmm_LINK_FLOW, legacy_idx[k]);
            EXPECT_NEAR(q_v6, q_legacy, 1e-6 * std::max(1.0, std::fabs(q_legacy)))
                << names[k] << " at step " << step;
        }
    }
    double q1 = 0.0;
    ASSERT_EQ(swmm_link_get_flow(e, v6_idx[0], &q1), 0);
    EXPECT_GT(q1, 0.0) << "C1 must still be draining its initial flow";

    swmm_end();
    swmm_close();
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
