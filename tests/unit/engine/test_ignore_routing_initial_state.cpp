/**
 * @file test_ignore_routing_initial_state.cpp
 * @brief Under IGNORE_ROUTING a conduit with an initial flow starts as legacy's does.
 *
 * Legacy swmm_start skips routing_open when IgnoreRouting is set, so
 * flowrout_init never runs: a conduit keeps link_initState's state, its
 * initial flow and normal depth (conduit_initState) with a volume of 0, and no
 * node or outfall depth is seeded from the links. v6 gave such a conduit its
 * normal-depth volume, which stayed in the system storage at every report:
 * benchmarks 3525-nodes differed from legacy only there, by 12 ft3. J1 drains
 * through C1, which starts with 1 cfs, to a free outfall; both engines run the
 * same deck, stepped together, under kinematic and dynamic wave.
 */
#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace fs = std::filesystem;

namespace {

// *_out/ is the gitignored scratch convention
const char* kOutDir = "ignore_routing_initial_state_out";

std::string deck(const char* routing) {
    return std::string("[OPTIONS]\n"
                       "FLOW_UNITS           CFS\n"
                       "FLOW_ROUTING         ") +
           routing +
           "\n"
           "IGNORE_ROUTING       YES\n"
           "START_DATE           01/01/2020\n"
           "START_TIME           00:00:00\n"
           "END_DATE             01/01/2020\n"
           "END_TIME             01:00:00\n"
           "REPORT_STEP          00:15:00\n"
           "WET_STEP             00:05:00\n"
           "DRY_STEP             00:05:00\n"
           "ROUTING_STEP         0:00:30\n"
           "\n"
           "[JUNCTIONS]\n"
           "J1  100  5  0  0  0\n"
           "\n"
           "[OUTFALLS]\n"
           "O1  99  FREE\n"
           "\n"
           "[CONDUITS]\n"
           "C1  J1  O1  400  0.013  0  0  1.0  0\n"
           "\n"
           "[XSECTIONS]\n"
           "C1  CIRCULAR  1.5  0  0  0  1\n";
}

void near(double v6, double legacy, const char* what, int step) {
    EXPECT_NEAR(v6, legacy, 1e-9 * std::max(1.0, std::fabs(legacy))) << what << " at step " << step;
}

void runBoth(const char* routing) {
    fs::create_directories(kOutDir);
    const std::string inp =
        (fs::path(kOutDir) / (std::string("ignore_routing_") + routing + ".inp")).string();
    std::ofstream(inp) << deck(routing);

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (inp + ".v6.rpt").c_str(), (inp + ".v6.out").c_str(),
                               nullptr),
              0)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_open(inp.c_str(), (inp + ".legacy.rpt").c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);

    const int v6_c1 = swmm_link_index(e, "C1");
    const int v6_j1 = swmm_node_index(e, "J1");
    const int v6_o1 = swmm_node_index(e, "O1");
    const int legacy_c1 = swmm_getIndex(swmm_LINK, "C1");
    const int legacy_j1 = swmm_getIndex(swmm_NODE, "J1");
    const int legacy_o1 = swmm_getIndex(swmm_NODE, "O1");
    ASSERT_GE(v6_c1, 0);
    ASSERT_GE(v6_j1, 0);
    ASSERT_GE(v6_o1, 0);
    ASSERT_GE(legacy_c1, 0);

    double v6_elapsed = 0.0, legacy_elapsed = 0.0;
    int step = 0;
    do {
        ++step;
        ASSERT_EQ(swmm_engine_step(e, &v6_elapsed), 0);
        ASSERT_EQ(swmm_step(&legacy_elapsed), 0);
        double volume = 0.0, depth = 0.0, flow = 0.0, j1_depth = 0.0, o1_depth = 0.0;
        ASSERT_EQ(swmm_link_get_volume(e, v6_c1, &volume), 0);
        ASSERT_EQ(swmm_link_get_depth(e, v6_c1, &depth), 0);
        ASSERT_EQ(swmm_link_get_flow(e, v6_c1, &flow), 0);
        ASSERT_EQ(swmm_node_get_depth(e, v6_j1, &j1_depth), 0);
        ASSERT_EQ(swmm_node_get_depth(e, v6_o1, &o1_depth), 0);
        near(volume, swmm_getValue(swmm_LINK_VOLUME, legacy_c1), "C1 volume", step);
        near(depth, swmm_getValue(swmm_LINK_DEPTH, legacy_c1), "C1 depth", step);
        near(flow, swmm_getValue(swmm_LINK_FLOW, legacy_c1), "C1 flow", step);
        near(j1_depth, swmm_getValue(swmm_NODE_DEPTH, legacy_j1), "J1 depth", step);
        near(o1_depth, swmm_getValue(swmm_NODE_DEPTH, legacy_o1), "O1 depth", step);
        if (::testing::Test::HasFailure())
            break;
    } while (v6_elapsed > 0.0 && legacy_elapsed > 0.0);
    EXPECT_GT(step, 1);

    swmm_end();
    swmm_close();
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

} // namespace

TEST(IgnoreRoutingInitialState, KinematicWaveMatchesLegacy) {
    runBoth("KINWAVE");
}

TEST(IgnoreRoutingInitialState, DynamicWaveMatchesLegacy) {
    runBoth("DYNWAVE");
}
