/**
 * @file test_weir_equal_heads.cpp
 * @brief A weir between two equal heads runs in reverse, as in legacy.
 *
 * Legacy weir_getInflow takes the flow direction as (h1 > h2) ? +1 : -1, so
 * equal heads count as reverse flow and a flap gate closes; its orifices and
 * outlets take h1 >= h2 as forward. v6 used >= for weirs too. Between two
 * nodes flooded to the same rim the forward reading put a gated weir in
 * surcharge at zero flow and reported its full depth, where legacy reports the
 * closed gate's 0: benchmarks 392-nodes differed only in that, on 74 link
 * depth cells of its side-flow weirs. Here J1 and J2 share an invert and a
 * maximum depth and both flood, with a gated weir between them; both engines
 * run the same deck, stepped together.
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

// *_out/ is the gitignored scratch convention
const char* kOutDir = "weir_equal_heads_out";

/// 60 cfs into each of J1 and J2, whose 1 ft outlet pipes carry far less, so
/// both flood to their 5 ft rim; W1 (gated, can surcharge) joins them.
const char* kDeck = R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
ALLOW_PONDING        NO
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             00:30:00
REPORT_STEP          00:01:00
ROUTING_STEP         0:00:05
VARIABLE_STEP        0

[JUNCTIONS]
J1  100  5  0  0  0
J2  100  5  0  0  0

[OUTFALLS]
O1  90  FREE
O2  90  FREE

[CONDUITS]
C1  J1  O1  400  0.013  0  0  0  0
C2  J2  O2  400  0.013  0  0  0  0

[WEIRS]
W1  J1  J2  TRANSVERSE  1  3.33  YES  0  0  YES

[XSECTIONS]
C1  CIRCULAR  1  0  0  0  1
C2  CIRCULAR  1  0  0  0  1
W1  RECT_OPEN  2  10  0  0

[INFLOWS]
J1  FLOW  ""  FLOW  1.0  1.0  60
J2  FLOW  ""  FLOW  1.0  1.0  60
)";

} // namespace

TEST(WeirEqualHeads, GatedWeirBetweenFloodedNodesMatchesLegacy) {
    fs::create_directories(kOutDir);
    const std::string inp = (fs::path(kOutDir) / "weir_equal_heads.inp").string();
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

    const int v6_w1 = swmm_link_index(e, "W1");
    const int legacy_w1 = swmm_getIndex(swmm_LINK, "W1");
    ASSERT_GE(v6_w1, 0);
    ASSERT_GE(legacy_w1, 0);

    double v6_elapsed = 0.0, legacy_elapsed = 0.0;
    int step = 0;
    int flooded_steps = 0;
    do {
        ++step;
        ASSERT_EQ(swmm_engine_step(e, &v6_elapsed), 0);
        ASSERT_EQ(swmm_step(&legacy_elapsed), 0);
        double depth = 0.0, flow = 0.0;
        ASSERT_EQ(swmm_link_get_depth(e, v6_w1, &depth), 0);
        ASSERT_EQ(swmm_link_get_flow(e, v6_w1, &flow), 0);
        const double legacy_depth = swmm_getValue(swmm_LINK_DEPTH, legacy_w1);
        const double legacy_flow = swmm_getValue(swmm_LINK_FLOW, legacy_w1);
        ASSERT_NEAR(depth, legacy_depth, 1e-9 * std::max(1.0, std::fabs(legacy_depth)))
            << "W1 depth at step " << step;
        ASSERT_NEAR(flow, legacy_flow, 1e-9 * std::max(1.0, std::fabs(legacy_flow)))
            << "W1 flow at step " << step;
        const double h1 = swmm_getValue(swmm_NODE_HEAD, swmm_getIndex(swmm_NODE, "J1"));
        const double h2 = swmm_getValue(swmm_NODE_HEAD, swmm_getIndex(swmm_NODE, "J2"));
        if (h1 == h2 && h1 >= 105.0)
            ++flooded_steps;
    } while (v6_elapsed > 0.0 && legacy_elapsed > 0.0);
    EXPECT_GT(flooded_steps, 0) << "both nodes must reach the same full head";

    swmm_end();
    swmm_close();
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
