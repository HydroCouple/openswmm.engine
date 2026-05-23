/**
 * @file test_report_section.cpp
 * @brief Unit and regression tests for the [REPORT] section implementation.
 *
 * @details Tests cover:
 *   - Parsing all [REPORT] keywords (handle_report)
 *   - Default-ALL behavior (subcatch/nodes/links default to ALL when omitted)
 *   - PostParseResolver propagation of ALL/SOME/NONE to per-object rpt_flag
 *   - InpWriter [REPORT] round-trip
 *   - DefaultOutputPlugin rpt_flag filtering (header counts + update skips)
 *   - DefaultReportPlugin disabled gate
 *
 * @see src/engine/input/handlers/ControlsHandler.cpp — handle_report()
 * @see src/engine/input/PostParseResolver.cpp
 * @see src/engine/core/InpWriter.cpp
 * @see src/engine/plugins/DefaultOutputPlugin.hpp
 * @see src/engine/plugins/DefaultReportPlugin.hpp
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#include <cstdio>

#include "../../src/engine/input/handlers/ControlsHandler.hpp"
#include "../../src/engine/input/PostParseResolver.hpp"
#include "../../src/engine/core/InpWriter.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/plugins/DefaultOutputPlugin.hpp"
#include "../../src/engine/plugins/DefaultReportPlugin.hpp"
#include "../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"

using openswmm::SimulationContext;
using openswmm::SimulationSnapshot;
using openswmm::DefaultOutputPlugin;
using openswmm::DefaultReportPlugin;

namespace fs = std::filesystem;

namespace {

static const std::string TMP = (fs::temp_directory_path() / "").string();

// Helper: call handle_report with a list of raw lines
static void parse_report(SimulationContext& ctx, std::vector<std::string> lines) {
    openswmm::input::handle_report(ctx, lines);
}

// Helper: build a minimal context with named objects for resolver testing
static void build_small_model(SimulationContext& ctx, int n_subcatch, int n_nodes, int n_links) {
    // Subcatchments
    for (int i = 0; i < n_subcatch; ++i) {
        ctx.subcatch_names.add("S" + std::to_string(i));
    }
    ctx.subcatches.resize(n_subcatch);

    // Nodes
    for (int i = 0; i < n_nodes; ++i) {
        ctx.node_names.add("N" + std::to_string(i));
    }
    ctx.nodes.resize(n_nodes);
    // Set required node fields so output plugin doesn't crash
    for (int i = 0; i < n_nodes; ++i) {
        auto ui = static_cast<std::size_t>(i);
        ctx.nodes.type[ui] = openswmm::NodeType::JUNCTION;
        ctx.nodes.invert_elev[ui] = 0.0;
        ctx.nodes.full_depth[ui] = 10.0;
    }

    // Links
    for (int i = 0; i < n_links; ++i) {
        ctx.link_names.add("L" + std::to_string(i));
    }
    ctx.links.resize(n_links);
    for (int i = 0; i < n_links; ++i) {
        auto ui = static_cast<std::size_t>(i);
        ctx.links.type[ui] = openswmm::LinkType::CONDUIT;
        ctx.links.direction[ui] = 1;
        ctx.links.barrels[ui] = 1;
        ctx.links.length[ui] = 100.0;
    }
}

// ============================================================================
// 1. [REPORT] Keyword Parsing
// ============================================================================

TEST(ReportParserTest, DefaultsAreALL) {
    SimulationContext ctx;
    // No parsing — verify factory defaults
    EXPECT_EQ(ctx.options.rpt_subcatchments, 1);  // ALL
    EXPECT_EQ(ctx.options.rpt_nodes, 1);           // ALL
    EXPECT_EQ(ctx.options.rpt_links, 1);           // ALL
    EXPECT_FALSE(ctx.options.rpt_disabled);
    EXPECT_FALSE(ctx.options.rpt_input);
    EXPECT_TRUE(ctx.options.rpt_continuity);
    EXPECT_TRUE(ctx.options.rpt_flowstats);
    EXPECT_FALSE(ctx.options.rpt_controls);
    EXPECT_FALSE(ctx.options.rpt_averages);
}

TEST(ReportParserTest, DisabledYes) {
    SimulationContext ctx;
    parse_report(ctx, {"DISABLED  YES"});
    EXPECT_TRUE(ctx.options.rpt_disabled);
}

TEST(ReportParserTest, DisabledNo) {
    SimulationContext ctx;
    ctx.options.rpt_disabled = true;  // set then unset
    parse_report(ctx, {"DISABLED  NO"});
    EXPECT_FALSE(ctx.options.rpt_disabled);
}

TEST(ReportParserTest, InputYes) {
    SimulationContext ctx;
    parse_report(ctx, {"INPUT  YES"});
    EXPECT_TRUE(ctx.options.rpt_input);
}

TEST(ReportParserTest, ContinuityNo) {
    SimulationContext ctx;
    parse_report(ctx, {"CONTINUITY  NO"});
    EXPECT_FALSE(ctx.options.rpt_continuity);
}

TEST(ReportParserTest, FlowStatsNo) {
    SimulationContext ctx;
    parse_report(ctx, {"FLOWSTATS  NO"});
    EXPECT_FALSE(ctx.options.rpt_flowstats);
}

TEST(ReportParserTest, ControlsYes) {
    SimulationContext ctx;
    parse_report(ctx, {"CONTROLS  YES"});
    EXPECT_TRUE(ctx.options.rpt_controls);
}

TEST(ReportParserTest, AveragesYes) {
    SimulationContext ctx;
    parse_report(ctx, {"AVERAGES  YES"});
    EXPECT_TRUE(ctx.options.rpt_averages);
}

TEST(ReportParserTest, SubcatchmentsAll) {
    SimulationContext ctx;
    ctx.options.rpt_subcatchments = 0;  // start NONE
    parse_report(ctx, {"SUBCATCHMENTS  ALL"});
    EXPECT_EQ(ctx.options.rpt_subcatchments, 1);
}

TEST(ReportParserTest, SubcatchmentsNONE) {
    SimulationContext ctx;
    parse_report(ctx, {"SUBCATCHMENTS  NONE"});
    EXPECT_EQ(ctx.options.rpt_subcatchments, 0);
}

TEST(ReportParserTest, SubcatchmentsSOME) {
    SimulationContext ctx;
    parse_report(ctx, {"SUBCATCHMENTS  S1", "SUBCATCHMENTS  S2"});
    EXPECT_EQ(ctx.options.rpt_subcatchments, 2);  // SOME
    ASSERT_EQ(ctx.options.rpt_subcatch_names.size(), 2u);
    EXPECT_EQ(ctx.options.rpt_subcatch_names[0], "S1");
    EXPECT_EQ(ctx.options.rpt_subcatch_names[1], "S2");
}

TEST(ReportParserTest, NodesAll) {
    SimulationContext ctx;
    ctx.options.rpt_nodes = 0;
    parse_report(ctx, {"NODES  ALL"});
    EXPECT_EQ(ctx.options.rpt_nodes, 1);
}

TEST(ReportParserTest, NodesNONE) {
    SimulationContext ctx;
    parse_report(ctx, {"NODES  NONE"});
    EXPECT_EQ(ctx.options.rpt_nodes, 0);
}

TEST(ReportParserTest, NodesSOME) {
    SimulationContext ctx;
    parse_report(ctx, {"NODES  N1", "NODES  N2", "NODES  N3"});
    EXPECT_EQ(ctx.options.rpt_nodes, 2);
    EXPECT_EQ(ctx.options.rpt_node_names.size(), 3u);
}

TEST(ReportParserTest, LinksAll) {
    SimulationContext ctx;
    ctx.options.rpt_links = 0;
    parse_report(ctx, {"LINKS  ALL"});
    EXPECT_EQ(ctx.options.rpt_links, 1);
}

TEST(ReportParserTest, LinksNONE) {
    SimulationContext ctx;
    parse_report(ctx, {"LINKS  NONE"});
    EXPECT_EQ(ctx.options.rpt_links, 0);
}

TEST(ReportParserTest, LinksSOME) {
    SimulationContext ctx;
    parse_report(ctx, {"LINKS  C1"});
    EXPECT_EQ(ctx.options.rpt_links, 2);
    ASSERT_EQ(ctx.options.rpt_link_names.size(), 1u);
    EXPECT_EQ(ctx.options.rpt_link_names[0], "C1");
}

TEST(ReportParserTest, MultipleKeywordsInBlock) {
    SimulationContext ctx;
    parse_report(ctx, {
        "DISABLED  NO",
        "INPUT     YES",
        "CONTINUITY YES",
        "FLOWSTATS  YES",
        "CONTROLS   NO",
        "AVERAGES   YES",
        "SUBCATCHMENTS  ALL",
        "NODES          ALL",
        "LINKS          ALL",
    });
    EXPECT_FALSE(ctx.options.rpt_disabled);
    EXPECT_TRUE(ctx.options.rpt_input);
    EXPECT_TRUE(ctx.options.rpt_continuity);
    EXPECT_TRUE(ctx.options.rpt_flowstats);
    EXPECT_FALSE(ctx.options.rpt_controls);
    EXPECT_TRUE(ctx.options.rpt_averages);
    EXPECT_EQ(ctx.options.rpt_subcatchments, 1);
    EXPECT_EQ(ctx.options.rpt_nodes, 1);
    EXPECT_EQ(ctx.options.rpt_links, 1);
}

TEST(ReportParserTest, CaseInsensitive) {
    SimulationContext ctx;
    parse_report(ctx, {"disabled  yes", "averages  YES"});
    EXPECT_TRUE(ctx.options.rpt_disabled);
    EXPECT_TRUE(ctx.options.rpt_averages);
}

// ============================================================================
// 2. PostParseResolver — rpt_flag propagation
// ============================================================================

TEST(ReportResolverTest, ALL_SetsAllFlags) {
    SimulationContext ctx;
    build_small_model(ctx, 3, 4, 2);
    ctx.options.rpt_subcatchments = 1;  // ALL
    ctx.options.rpt_nodes = 1;
    ctx.options.rpt_links = 1;

    openswmm::input::resolve_cross_references(ctx);

    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(ctx.subcatches.rpt_flag[static_cast<std::size_t>(i)], 1)
            << "subcatch " << i;
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(ctx.nodes.rpt_flag[static_cast<std::size_t>(i)], 1)
            << "node " << i;
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(ctx.links.rpt_flag[static_cast<std::size_t>(i)], 1)
            << "link " << i;
}

TEST(ReportResolverTest, NONE_LeavesAllFlagsZero) {
    SimulationContext ctx;
    build_small_model(ctx, 3, 4, 2);
    ctx.options.rpt_subcatchments = 0;  // NONE
    ctx.options.rpt_nodes = 0;
    ctx.options.rpt_links = 0;

    openswmm::input::resolve_cross_references(ctx);

    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(ctx.subcatches.rpt_flag[static_cast<std::size_t>(i)], 0);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(ctx.nodes.rpt_flag[static_cast<std::size_t>(i)], 0);
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(ctx.links.rpt_flag[static_cast<std::size_t>(i)], 0);
}

TEST(ReportResolverTest, SOME_SetsOnlyNamedObjects) {
    SimulationContext ctx;
    build_small_model(ctx, 3, 4, 2);

    // SOME subcatchments: only S0 and S2
    ctx.options.rpt_subcatchments = 2;
    ctx.options.rpt_subcatch_names = {"S0", "S2"};

    // SOME nodes: only N1
    ctx.options.rpt_nodes = 2;
    ctx.options.rpt_node_names = {"N1"};

    // ALL links
    ctx.options.rpt_links = 1;

    openswmm::input::resolve_cross_references(ctx);

    EXPECT_EQ(ctx.subcatches.rpt_flag[0], 1);  // S0 — named
    EXPECT_EQ(ctx.subcatches.rpt_flag[1], 0);  // S1 — not named
    EXPECT_EQ(ctx.subcatches.rpt_flag[2], 1);  // S2 — named

    EXPECT_EQ(ctx.nodes.rpt_flag[0], 0);  // N0
    EXPECT_EQ(ctx.nodes.rpt_flag[1], 1);  // N1 — named
    EXPECT_EQ(ctx.nodes.rpt_flag[2], 0);  // N2
    EXPECT_EQ(ctx.nodes.rpt_flag[3], 0);  // N3

    EXPECT_EQ(ctx.links.rpt_flag[0], 1);  // ALL
    EXPECT_EQ(ctx.links.rpt_flag[1], 1);
}

TEST(ReportResolverTest, SOME_UnknownNameIgnored) {
    SimulationContext ctx;
    build_small_model(ctx, 2, 2, 0);
    ctx.options.rpt_subcatchments = 2;
    ctx.options.rpt_subcatch_names = {"S0", "NONEXISTENT"};

    openswmm::input::resolve_cross_references(ctx);

    EXPECT_EQ(ctx.subcatches.rpt_flag[0], 1);  // S0
    EXPECT_EQ(ctx.subcatches.rpt_flag[1], 0);  // S1 (NONEXISTENT not found)
}

// ============================================================================
// 3. InpWriter round-trip — [REPORT] section
// ============================================================================

class ReportInpRoundTripTest : public ::testing::Test {
protected:
    std::string tmp_path_;

    void SetUp() override {
        tmp_path_ = TMP + "test_report_roundtrip.inp";
    }

    void TearDown() override {
        std::remove(tmp_path_.c_str());
    }

    // Read the written .inp file back as text and extract [REPORT] section lines
    std::vector<std::string> extract_report_section(const std::string& path) {
        std::vector<std::string> lines;
        std::ifstream f(path);
        bool in_section = false;
        std::string line;
        while (std::getline(f, line)) {
            if (line.rfind("[REPORT]", 0) == 0) {
                in_section = true;
                continue;
            }
            if (in_section && !line.empty() && line[0] == '[') break;
            if (in_section && !line.empty() && line[0] != ';')
                lines.push_back(line);
        }
        return lines;
    }
};

TEST_F(ReportInpRoundTripTest, AllKeywordsWritten) {
    SimulationContext ctx;
    ctx.options.rpt_disabled = true;
    ctx.options.rpt_input = true;
    ctx.options.rpt_continuity = false;
    ctx.options.rpt_flowstats = false;
    ctx.options.rpt_controls = true;
    ctx.options.rpt_averages = true;
    ctx.options.rpt_subcatchments = 1;  // ALL
    ctx.options.rpt_nodes = 0;          // NONE
    ctx.options.rpt_links = 2;          // SOME
    ctx.options.rpt_link_names = {"C1", "C2"};

    int rc = openswmm::inp_writer::writeInpFile(ctx, tmp_path_);
    ASSERT_EQ(rc, 0);

    auto lines = extract_report_section(tmp_path_);
    // Flatten to single string for easy searching
    std::string text;
    for (const auto& l : lines) text += l + "\n";

    EXPECT_NE(text.find("YES"), std::string::npos);  // DISABLED YES
    EXPECT_NE(text.find("SUBCATCHMENTS"), std::string::npos);
    EXPECT_NE(text.find("ALL"), std::string::npos);
    EXPECT_NE(text.find("NONE"), std::string::npos);
    EXPECT_NE(text.find("C1"), std::string::npos);
    EXPECT_NE(text.find("C2"), std::string::npos);
}

TEST_F(ReportInpRoundTripTest, ParseWrittenReportSection) {
    // Build, write, then parse the [REPORT] section
    SimulationContext ctx_write;
    ctx_write.options.rpt_disabled = true;
    ctx_write.options.rpt_input = true;
    ctx_write.options.rpt_continuity = false;
    ctx_write.options.rpt_flowstats = false;
    ctx_write.options.rpt_controls = true;
    ctx_write.options.rpt_averages = true;
    ctx_write.options.rpt_subcatchments = 0;  // NONE
    ctx_write.options.rpt_nodes = 1;          // ALL
    ctx_write.options.rpt_links = 2;          // SOME
    ctx_write.options.rpt_link_names = {"Link1", "Link2"};

    int rc = openswmm::inp_writer::writeInpFile(ctx_write, tmp_path_);
    ASSERT_EQ(rc, 0);

    auto lines = extract_report_section(tmp_path_);

    // Parse extracted lines through handle_report
    SimulationContext ctx_read;
    parse_report(ctx_read, lines);

    EXPECT_EQ(ctx_read.options.rpt_disabled, true);
    EXPECT_EQ(ctx_read.options.rpt_input, true);
    EXPECT_EQ(ctx_read.options.rpt_continuity, false);
    EXPECT_EQ(ctx_read.options.rpt_flowstats, false);
    EXPECT_EQ(ctx_read.options.rpt_controls, true);
    EXPECT_EQ(ctx_read.options.rpt_averages, true);
    EXPECT_EQ(ctx_read.options.rpt_subcatchments, 0);  // NONE
    EXPECT_EQ(ctx_read.options.rpt_nodes, 1);           // ALL
    EXPECT_EQ(ctx_read.options.rpt_links, 2);            // SOME
    ASSERT_EQ(ctx_read.options.rpt_link_names.size(), 2u);
    EXPECT_EQ(ctx_read.options.rpt_link_names[0], "Link1");
    EXPECT_EQ(ctx_read.options.rpt_link_names[1], "Link2");
}

// ============================================================================
// 4. DefaultOutputPlugin — rpt_flag filtering
// ============================================================================

class OutputPluginRptFlagTest : public ::testing::Test {
protected:
    std::string out_path_;

    void SetUp() override {
        out_path_ = TMP + "test_rpt_flag.out";
    }

    void TearDown() override {
        std::remove(out_path_.c_str());
    }
};

TEST_F(OutputPluginRptFlagTest, OnlyFlaggedObjectsWrittenToHeader) {
    // Build a model with 3 nodes, but only 2 flagged for reporting
    SimulationContext ctx;
    build_small_model(ctx, 2, 3, 2);

    // Flag S0 only, N0 and N2 only, L1 only
    ctx.subcatches.rpt_flag[0] = 1;
    ctx.subcatches.rpt_flag[1] = 0;
    ctx.nodes.rpt_flag[0] = 1;
    ctx.nodes.rpt_flag[1] = 0;
    ctx.nodes.rpt_flag[2] = 1;
    ctx.links.rpt_flag[0] = 0;
    ctx.links.rpt_flag[1] = 1;

    DefaultOutputPlugin plug(out_path_);
    ASSERT_EQ(plug.initialize({}, nullptr), 0);
    ASSERT_EQ(plug.validate(ctx), 0);
    ASSERT_EQ(plug.prepare(ctx), 0);

    // Write one snapshot
    SimulationSnapshot snap;
    snap.sim_time = 2460001.0;
    snap.subcatch_count = 2;
    snap.node_count = 3;
    snap.link_count = 2;
    snap.pollut_count = 0;
    snap.subcatch.rainfall.assign(2, 0.001);
    snap.subcatch.snow_depth.assign(2, 0.0);
    snap.subcatch.evap.assign(2, 0.0);
    snap.subcatch.infil.assign(2, 0.0);
    snap.subcatch.runoff.assign(2, 0.0);
    snap.subcatch.gw_flow.assign(2, 0.0);
    snap.subcatch.gw_elev.assign(2, 0.0);
    snap.subcatch.soil_moist.assign(2, 0.0);
    snap.nodes.depth.assign(3, 1.0);
    snap.nodes.head.assign(3, 2.0);
    snap.nodes.volume.assign(3, 3.0);
    snap.nodes.lateral_inflow.assign(3, 0.0);
    snap.nodes.total_inflow.assign(3, 0.0);
    snap.nodes.overflow.assign(3, 0.0);
    snap.links.flow.assign(2, 5.0);
    snap.links.depth.assign(2, 1.0);
    snap.links.velocity.assign(2, 2.0);
    snap.links.volume.assign(2, 3.0);
    snap.links.capacity.assign(2, 0.5);

    ASSERT_EQ(plug.update(snap), 0);
    ASSERT_EQ(plug.finalize(ctx), 0);

    // Read back the binary file and verify header counts
    FILE* f = std::fopen(out_path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);

    int magic, version, flow_units, n_sc, n_nd, n_lk, n_pol;
    std::fread(&magic, sizeof(int), 1, f);
    std::fread(&version, sizeof(int), 1, f);
    std::fread(&flow_units, sizeof(int), 1, f);
    std::fread(&n_sc, sizeof(int), 1, f);
    std::fread(&n_nd, sizeof(int), 1, f);
    std::fread(&n_lk, sizeof(int), 1, f);
    std::fread(&n_pol, sizeof(int), 1, f);

    EXPECT_EQ(magic, 516114522);
    EXPECT_EQ(n_sc, 1);   // Only S0 flagged
    EXPECT_EQ(n_nd, 2);   // N0 and N2 flagged
    EXPECT_EQ(n_lk, 1);   // Only L1 flagged
    EXPECT_EQ(n_pol, 0);

    std::fclose(f);
}

TEST_F(OutputPluginRptFlagTest, AllFlaggedWritesAllCounts) {
    SimulationContext ctx;
    build_small_model(ctx, 2, 3, 2);

    // All flagged
    std::fill(ctx.subcatches.rpt_flag.begin(), ctx.subcatches.rpt_flag.end(), 1);
    std::fill(ctx.nodes.rpt_flag.begin(), ctx.nodes.rpt_flag.end(), 1);
    std::fill(ctx.links.rpt_flag.begin(), ctx.links.rpt_flag.end(), 1);

    DefaultOutputPlugin plug(out_path_);
    plug.initialize({}, nullptr);
    plug.validate(ctx);
    plug.prepare(ctx);
    plug.finalize(ctx);

    FILE* f = std::fopen(out_path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);

    int hdr[7];
    std::fread(hdr, sizeof(int), 7, f);
    EXPECT_EQ(hdr[3], 2);  // n_subcatch
    EXPECT_EQ(hdr[4], 3);  // n_nodes
    EXPECT_EQ(hdr[5], 2);  // n_links
    std::fclose(f);
}

TEST_F(OutputPluginRptFlagTest, NoneFlaggedWritesZeroCounts) {
    SimulationContext ctx;
    build_small_model(ctx, 2, 3, 2);
    // All flags remain 0 (from resize)

    DefaultOutputPlugin plug(out_path_);
    plug.initialize({}, nullptr);
    plug.validate(ctx);
    plug.prepare(ctx);
    plug.finalize(ctx);

    FILE* f = std::fopen(out_path_.c_str(), "rb");
    ASSERT_NE(f, nullptr);

    int hdr[7];
    std::fread(hdr, sizeof(int), 7, f);
    EXPECT_EQ(hdr[3], 0);  // n_subcatch
    EXPECT_EQ(hdr[4], 0);  // n_nodes
    EXPECT_EQ(hdr[5], 0);  // n_links
    std::fclose(f);
}

// ============================================================================
// 5. DefaultReportPlugin — DISABLED gate
// ============================================================================

TEST(ReportPluginDisabledTest, DisabledWritesSummaryOk) {
    std::string rpt_path = TMP + "test_report_disabled.rpt";
    SimulationContext ctx;
    ctx.options.rpt_disabled = true;

    DefaultReportPlugin rp(rpt_path.c_str());
    ASSERT_EQ(rp.initialize({}, nullptr), 0);
    ASSERT_EQ(rp.validate(ctx), 0);
    ASSERT_EQ(rp.prepare(ctx), 0);
    ASSERT_EQ(rp.finalize(ctx), 0);
    ASSERT_EQ(rp.write_summary(ctx), 0);

    // Verify the rpt file is minimal (empty or very short — no full summaries)
    std::ifstream f(rpt_path);
    std::string content((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());

    // With DISABLED=YES, continuity/flow stats/node/link/subcatch summaries skipped.
    // The file should not contain "Node Depth Summary" or similar.
    EXPECT_EQ(content.find("Node Depth Summary"), std::string::npos);
    EXPECT_EQ(content.find("Subcatchment Runoff Summary"), std::string::npos);

    std::remove(rpt_path.c_str());
}

// ============================================================================
// 6. End-to-end: parse + resolve + output plugin integration
// ============================================================================

TEST(ReportEndToEndTest, ParseResolveAndOutputPlugin) {
    SimulationContext ctx;
    build_small_model(ctx, 3, 4, 2);

    // Parse a [REPORT] section
    parse_report(ctx, {
        "SUBCATCHMENTS  S0",
        "SUBCATCHMENTS  S2",
        "NODES          ALL",
        "LINKS          NONE",
    });

    EXPECT_EQ(ctx.options.rpt_subcatchments, 2);  // SOME
    EXPECT_EQ(ctx.options.rpt_nodes, 1);           // ALL
    EXPECT_EQ(ctx.options.rpt_links, 0);           // NONE

    // Resolve to set per-object flags
    openswmm::input::resolve_cross_references(ctx);

    // Verify flags
    EXPECT_EQ(ctx.subcatches.rpt_flag[0], 1);
    EXPECT_EQ(ctx.subcatches.rpt_flag[1], 0);
    EXPECT_EQ(ctx.subcatches.rpt_flag[2], 1);
    for (int i = 0; i < 4; ++i)
        EXPECT_EQ(ctx.nodes.rpt_flag[static_cast<std::size_t>(i)], 1);
    for (int i = 0; i < 2; ++i)
        EXPECT_EQ(ctx.links.rpt_flag[static_cast<std::size_t>(i)], 0);

    // Drive through DefaultOutputPlugin
    std::string out_path = TMP + "test_e2e_report.out";
    DefaultOutputPlugin plug(out_path);
    plug.initialize({}, nullptr);
    plug.validate(ctx);
    plug.prepare(ctx);
    plug.finalize(ctx);

    // Read header: 2 subcatch, 4 nodes, 0 links
    FILE* f = std::fopen(out_path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    int hdr[7];
    std::fread(hdr, sizeof(int), 7, f);
    EXPECT_EQ(hdr[3], 2);  // 2 subcatchments (S0, S2)
    EXPECT_EQ(hdr[4], 4);  // 4 nodes (ALL)
    EXPECT_EQ(hdr[5], 0);  // 0 links (NONE)
    std::fclose(f);
    std::remove(out_path.c_str());
}

// ============================================================================
// 7. Binary output file footer validation
// ============================================================================

TEST(ReportOutputFooterTest, FooterContainsMagicAndPeriods) {
    SimulationContext ctx;
    build_small_model(ctx, 1, 1, 1);
    std::fill(ctx.subcatches.rpt_flag.begin(), ctx.subcatches.rpt_flag.end(), 1);
    std::fill(ctx.nodes.rpt_flag.begin(), ctx.nodes.rpt_flag.end(), 1);
    std::fill(ctx.links.rpt_flag.begin(), ctx.links.rpt_flag.end(), 1);

    std::string out_path = TMP + "test_footer.out";
    DefaultOutputPlugin plug(out_path);
    plug.initialize({}, nullptr);
    plug.validate(ctx);
    plug.prepare(ctx);

    // Write 3 update periods
    SimulationSnapshot snap;
    snap.sim_time = 2460001.0;
    snap.subcatch_count = 1;
    snap.node_count = 1;
    snap.link_count = 1;
    snap.pollut_count = 0;
    snap.subcatch.rainfall.assign(1, 0.0);
    snap.subcatch.snow_depth.assign(1, 0.0);
    snap.subcatch.evap.assign(1, 0.0);
    snap.subcatch.infil.assign(1, 0.0);
    snap.subcatch.runoff.assign(1, 0.0);
    snap.subcatch.gw_flow.assign(1, 0.0);
    snap.subcatch.gw_elev.assign(1, 0.0);
    snap.subcatch.soil_moist.assign(1, 0.0);
    snap.nodes.depth.assign(1, 0.0);
    snap.nodes.head.assign(1, 0.0);
    snap.nodes.volume.assign(1, 0.0);
    snap.nodes.lateral_inflow.assign(1, 0.0);
    snap.nodes.total_inflow.assign(1, 0.0);
    snap.nodes.overflow.assign(1, 0.0);
    snap.links.flow.assign(1, 0.0);
    snap.links.depth.assign(1, 0.0);
    snap.links.velocity.assign(1, 0.0);
    snap.links.volume.assign(1, 0.0);
    snap.links.capacity.assign(1, 0.0);

    for (int i = 0; i < 3; ++i) {
        snap.sim_time = 2460001.0 + i * 0.01;
        plug.update(snap);
    }
    plug.finalize(ctx);

    // Read last 6 ints from the file (footer)
    FILE* f = std::fopen(out_path.c_str(), "rb");
    ASSERT_NE(f, nullptr);
    std::fseek(f, -6 * static_cast<long>(sizeof(int)), SEEK_END);

    int footer[6];
    std::fread(footer, sizeof(int), 6, f);
    std::fclose(f);

    // footer[3] = n_periods, footer[4] = error_code, footer[5] = magic
    EXPECT_EQ(footer[3], 3);          // 3 periods written
    EXPECT_EQ(footer[4], 0);          // no error
    EXPECT_EQ(footer[5], 516114522);  // magic number
    std::remove(out_path.c_str());
}

} /* anonymous namespace */
