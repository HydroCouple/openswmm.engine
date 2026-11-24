/**
 * @file test_hotstart_legacy_node_volume.cpp
 * @brief A legacy hot start gives junctions and outfalls the volume legacy does.
 *
 * Legacy initNodes sets each node's volume from its restored depth with
 * node_getVolume: fullVolume * (depth / fullDepth), which is 0 for a plain
 * junction or outfall. v6 recomputed it with node::getVolume, which gives them
 * MIN_SURFAREA * depth. The dynamic wave never drains an outfall's volume, so
 * the outfall shed that water as overflow at every step and reported it as
 * flooding, and quality routing lost it: in benchmarks wq-extran1 the outfall
 * flooded 0.443 cfs all run and its BOD5 and TN read 2.5e-4 off legacy. Here
 * legacy saves a hot start file from a short run, then both engines restart
 * from it, stepped together.
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
const char* kOutDir = "hotstart_legacy_node_volume_out";

/// 2 cfs carrying 5 mg/L of P through J1 -> C1 -> J2 -> C2 -> O1 (FREE).
/// @FILES@ becomes the run's SAVE or USE HOTSTART line.
const char* kDeck = R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             01:00:00
REPORT_STEP          00:15:00
ROUTING_STEP         0:00:20

[FILES]
@FILES@

[JUNCTIONS]
J1  101  5  0  0  0
J2  100  5  0  0  0

[OUTFALLS]
O1  99  FREE

[CONDUITS]
C1  J1  J2  400  0.013  0  0  0  0
C2  J2  O1  400  0.013  0  0  0  0

[XSECTIONS]
C1  CIRCULAR  1.5  0  0  0  1
C2  CIRCULAR  1.5  0  0  0  1

[POLLUTANTS]
P  MG/L  0  0  0  0  NO  *  0  0  0

[DWF]
J1  FLOW  2
J1  P     5
)";

std::string deckWith(const std::string& files) {
    std::string deck = kDeck;
    deck.replace(deck.find("@FILES@"), 7, files);
    return deck;
}

} // namespace

TEST(LegacyHotStartNodeVolume, OutfallHoldsNoWaterAsInLegacy) {
    fs::create_directories(kOutDir);
    const fs::path dir = fs::absolute(kOutDir);
    const std::string hsf = (dir / "hotstart_legacy_node_volume.hsf").string();
    const std::string save_inp = (dir / "hotstart_legacy_node_volume.save.inp").string();
    const std::string use_inp = (dir / "hotstart_legacy_node_volume.use.inp").string();
    std::ofstream(save_inp) << deckWith("SAVE HOTSTART \"" + hsf + "\"");
    std::ofstream(use_inp) << deckWith("USE HOTSTART \"" + hsf + "\"");

    // Legacy runs to steady flow and saves a SWMM 5 hot start file.
    fs::remove(hsf);
    ASSERT_EQ(swmm_run(save_inp.c_str(), (save_inp + ".rpt").c_str(), ""), 0);
    ASSERT_TRUE(fs::exists(hsf));

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, use_inp.c_str(), (use_inp + ".v6.rpt").c_str(),
                               (use_inp + ".v6.out").c_str(), nullptr),
              0)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_open(use_inp.c_str(), (use_inp + ".legacy.rpt").c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);

    const int v6_o1 = swmm_node_index(e, "O1");
    const int legacy_o1 = swmm_getIndex(swmm_NODE, "O1");
    const int v6_c2 = swmm_link_index(e, "C2");
    const int legacy_c2 = swmm_getIndex(swmm_LINK, "C2");
    ASSERT_GE(v6_o1, 0);
    ASSERT_GE(legacy_o1, 0);
    ASSERT_GE(v6_c2, 0);
    ASSERT_GE(legacy_c2, 0);

    // The overflow is the defect and is checked tightly. Flows after a hot
    // start already differ from legacy by about 3e-9 (relative) for another
    // reason, below what the .out's float32 can show, so flow and
    // concentration are checked at 1e-6.
    auto close = [](double v6, double legacy, double rel) {
        return std::fabs(v6 - legacy) <= rel * std::max(1e-3, std::fabs(legacy));
    };
    double v6_elapsed = 0.0, legacy_elapsed = 0.0;
    int step = 0;
    do {
        ++step;
        ASSERT_EQ(swmm_engine_step(e, &v6_elapsed), 0);
        ASSERT_EQ(swmm_step(&legacy_elapsed), 0);
        double overflow = 0.0, conc = 0.0, flow = 0.0;
        ASSERT_EQ(swmm_node_get_overflow(e, v6_o1, &overflow), 0);
        ASSERT_EQ(swmm_node_get_quality(e, v6_o1, 0, &conc), 0);
        ASSERT_EQ(swmm_link_get_flow(e, v6_c2, &flow), 0);
        const double legacy_overflow = swmm_getValue(swmm_NODE_OVERFLOW, legacy_o1);
        const double legacy_conc =
            swmm_getValueExpanded(swmm_NODE, swmm_NODE_POLLUTANT_CONCENTRATION, legacy_o1, 0, 0);
        const double legacy_flow = swmm_getValue(swmm_LINK_FLOW, legacy_c2);
        ASSERT_TRUE(close(overflow, legacy_overflow, 1e-9))
            << "O1 overflow at step " << step << ": " << overflow << " vs legacy "
            << legacy_overflow;
        ASSERT_TRUE(close(conc, legacy_conc, 1e-6))
            << "O1 P at step " << step << ": " << conc << " vs legacy " << legacy_conc;
        ASSERT_TRUE(close(flow, legacy_flow, 1e-6))
            << "C2 flow at step " << step << ": " << flow << " vs legacy " << legacy_flow;
    } while (v6_elapsed > 0.0 && legacy_elapsed > 0.0);
    EXPECT_GT(step, 100);

    swmm_end();
    swmm_close();
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
