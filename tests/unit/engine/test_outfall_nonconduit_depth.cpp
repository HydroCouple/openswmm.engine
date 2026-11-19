/**
 * @file      test_outfall_nonconduit_depth.cpp
 * @brief     A FREE or NORMAL outfall fed by a weir, orifice or outlet has zero depth.
 *
 * @details For a weir, orifice or outlet both link offsets hold the crest height above the
 *          UPSTREAM node's invert. Adding that height to the outfall's own invert (as the
 *          conduit rule "offset + normal/critical depth" does) puts water at the outfall
 *          that is not there: when the outfall invert sits above the upstream invert the
 *          outfall head stands above the crest and the link runs backwards for the whole
 *          run, creating inflow. EPA SWMM 5.2.4 sets the depth to zero in this case; both
 *          engines do the same again. A conduit that enters above the outfall invert keeps
 *          the "offset + depth" rule.
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
#include <tuple>

#include "core/SWMMEngine.hpp"
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {

const std::filesystem::path out = std::filesystem::path(OUTFALL_DEPTH_OUT_DIR);

/**
 * @brief Build the two-hour fixture deck.
 *
 * J1 (invert 0) receives 0.5 cfs and drains through conduit C1, which enters FREE outfall O2
 * 0.5 ft above that outfall's invert. The link under test joins J1 to outfall O1, whose invert
 * (5 ft) is above J1's; its crest is 6 ft above J1's invert, far above the water in J1.
 *
 * @param[in] link "WEIR", "ORIFICE" or "OUTLET".
 * @param[in] outfall_type "FREE" or "NORMAL" for O1.
 * @return Deck text.
 */
std::string deck(const std::string& link, const std::string& outfall_type) {
    std::ostringstream f;
    f << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nLINK_OFFSETS DEPTH\n"
      << "START_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 02:00:00\n"
      << "REPORT_STEP 00:05:00\nROUTING_STEP 5\nVARIABLE_STEP 0\nTHREADS 1\n"
      << "[JUNCTIONS]\nJ1 0 10 0 0 0\n"
      << "[OUTFALLS]\nO1 5 " << outfall_type << " NO\nO2 -1 FREE NO\n"
      << "[CONDUITS]\nC1 J1 O2 200 0.013 0 0.5 0\n";
    if (link == "WEIR")
        f << "[WEIRS]\nL1 J1 O1 TRANSVERSE 6 3.33 NO 0 0 YES\n";
    else if (link == "ORIFICE")
        f << "[ORIFICES]\nL1 J1 O1 SIDE 6 0.65 NO 0\n";
    else
        f << "[OUTLETS]\nL1 J1 O1 6 FUNCTIONAL/DEPTH 10 0.5 NO\n";
    f << "[XSECTIONS]\nC1 CIRCULAR 2 0 0 0\n";
    if (link == "WEIR")
        f << "L1 RECT_OPEN 1 1 0 0\n";
    else if (link == "ORIFICE")
        f << "L1 RECT_CLOSED 1 1 0 0\n";
    f << "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 0.5\n"
      << "[REPORT]\nINPUT NO\n";
    return f.str();
}

struct Result {
    double max_abs_link_flow = 0.0; ///< largest |flow| in L1 over the run (cfs)
    double max_o1_depth = 0.0;      ///< largest depth at O1 over the run (ft)
    double max_o2_depth = 0.0;      ///< largest depth at O2 over the run (ft)
    double max_j1_depth = 0.0;      ///< largest depth at J1 over the run (ft)
};

/**
 * @brief Run the deck to its end in one engine and collect per-step extremes.
 *
 * @param[in] path Deck path; the report is written beside it.
 * @param[in] legacy true runs the in-tree legacy solver, false the engine.
 * @return Extremes over all routing steps.
 */
Result run(const std::string& path, bool legacy) {
    Result r;
    double elapsed = 0.0;
    if (legacy) {
        EXPECT_EQ(swmm_open(path.c_str(), (path + ".legacy.rpt").c_str(), ""), 0);
        EXPECT_EQ(swmm_start(0), 0);
        const int l1 = swmm_getIndex(swmm_LINK, "L1");
        const int o1 = swmm_getIndex(swmm_NODE, "O1");
        const int o2 = swmm_getIndex(swmm_NODE, "O2");
        const int j1 = swmm_getIndex(swmm_NODE, "J1");
        do {
            EXPECT_EQ(swmm_step(&elapsed), 0);
            r.max_abs_link_flow =
                std::max(r.max_abs_link_flow, std::fabs(swmm_getValue(swmm_LINK_FLOW, l1)));
            r.max_o1_depth = std::max(r.max_o1_depth, swmm_getValue(swmm_NODE_DEPTH, o1));
            r.max_o2_depth = std::max(r.max_o2_depth, swmm_getValue(swmm_NODE_DEPTH, o2));
            r.max_j1_depth = std::max(r.max_j1_depth, swmm_getValue(swmm_NODE_DEPTH, j1));
        } while (elapsed > 0.0);
        EXPECT_EQ(swmm_end(), 0);
        EXPECT_EQ(swmm_close(), 0);
    } else {
        openswmm::SWMMEngine engine;
        EXPECT_EQ(engine.open(path.c_str(), (path + ".v6.rpt").c_str(), ""), 0);
        EXPECT_EQ(engine.initialize(), 0);
        EXPECT_EQ(engine.start(0), 0);
        const auto& ctx = engine.context();
        const auto l1 = static_cast<std::size_t>(ctx.link_names.find("L1"));
        const auto o1 = static_cast<std::size_t>(ctx.node_names.find("O1"));
        const auto o2 = static_cast<std::size_t>(ctx.node_names.find("O2"));
        const auto j1 = static_cast<std::size_t>(ctx.node_names.find("J1"));
        do {
            EXPECT_EQ(engine.step(&elapsed), 0);
            r.max_abs_link_flow = std::max(r.max_abs_link_flow, std::fabs(ctx.links.flow[l1]));
            r.max_o1_depth = std::max(r.max_o1_depth, ctx.nodes.depth[o1]);
            r.max_o2_depth = std::max(r.max_o2_depth, ctx.nodes.depth[o2]);
            r.max_j1_depth = std::max(r.max_j1_depth, ctx.nodes.depth[j1]);
        } while (elapsed > 0.0);
        EXPECT_EQ(engine.end(), 0);
    }
    return r;
}

using Case = std::tuple<std::string, std::string, bool>; // link, O1 type, legacy
class OutfallNonConduitDepth : public ::testing::TestWithParam<Case> {};

TEST_P(OutfallNonConduitDepth, DryCrestGivesZeroDepthAndNoFlow) {
    const auto& [link, type, legacy] = GetParam();
    std::filesystem::create_directories(out);
    const auto path = out / (link + "_" + type + ".inp");
    std::ofstream(path) << deck(link, type);
    const Result r = run(path.string(), legacy);

    // The water in J1 never reaches the crest 6 ft above its invert ...
    EXPECT_GT(r.max_j1_depth, 0.05);
    EXPECT_LT(r.max_j1_depth, 2.0);
    // ... so nothing passes the link in either direction and the outfall is dry.
    EXPECT_EQ(r.max_abs_link_flow, 0.0);
    EXPECT_EQ(r.max_o1_depth, 0.0);
    // A conduit entering above the outfall invert keeps "offset + depth".
    EXPECT_GT(r.max_o2_depth, 0.5);
}

INSTANTIATE_TEST_SUITE_P(BothEngines, OutfallNonConduitDepth,
                         ::testing::Combine(::testing::Values("WEIR", "ORIFICE", "OUTLET"),
                                            ::testing::Values("FREE", "NORMAL"), ::testing::Bool()),
                         [](const ::testing::TestParamInfo<Case>& info) {
                             return std::get<0>(info.param) + "_" + std::get<1>(info.param) +
                                    (std::get<2>(info.param) ? "_legacy" : "_engine");
                         });

} // namespace
