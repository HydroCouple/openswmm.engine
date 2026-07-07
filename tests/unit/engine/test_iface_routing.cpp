/**
 * @file test_iface_routing.cpp
 * @brief End-to-end tests for routing interface files
 *        ([FILES] USE INFLOWS / SAVE OUTFLOWS).
 *
 * @details Verifies the lifecycle wiring added per
 *          plans/ROUTING_INTERFACE_FILE_INTEGRATION_PLAN_2026-07-01.md:
 *          - USE INFLOWS applies the file's flows as node lateral inflows
 *            (booked as external inflow in the routing mass balance).
 *          - SAVE OUTFLOWS produces a legacy-format interface file with a
 *            header and one row per outlet node per reporting step.
 *          - A missing / malformed inflows file fails swmm_engine_start().
 *          - Chained coupling: model A SAVE OUTFLOWS → model B USE INFLOWS
 *            conserves volume at the shared boundary.
 *
 *          Working directory is tests/unit/engine/data/ at runtime (set by
 *          the test CMakeLists.txt); all scratch files are created there
 *          with unique names and removed on success.
 *
 * @see Legacy: src/legacy/engine/iface.c, routing.c addIfaceInflows()
 * @ingroup engine_tests
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_massbalance.h>
#include <openswmm/engine/openswmm_statistics.h>

namespace fs = std::filesystem;

namespace {

// ---------------------------------------------------------------------------
// Fixture helpers
// ---------------------------------------------------------------------------

// Minimal 1-junction / 1-outfall model. `receiver` names the junction so the
// chained test can receive rows written for another model's outfall.
std::string minimal_model_inp(const std::string& receiver = "J1",
                              const std::string& outfall  = "O1") {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "REPORT_START_DATE    01/01/2026\n"
         "REPORT_START_TIME    00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             01:00:00\n"
         "REPORT_STEP          00:05:00\n"
         "WET_STEP             00:05:00\n"
         "DRY_STEP             00:05:00\n"
         "ROUTING_STEP         0:00:30\n"
         "\n[JUNCTIONS]\n"
         ";;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded\n"
      << receiver << "  100.0  10.0  0.0  0.0  0.0\n"
      << "\n[OUTFALLS]\n"
         ";;Name  Elev  Type\n"
      << outfall << "  95.0  FREE\n"
      << "\n[CONDUITS]\n"
         ";;Name  From  To  Length  N  InOff  OutOff\n"
         "C1  " << receiver << "  " << outfall << "  400.0  0.013  0  0\n"
      << "\n[XSECTIONS]\n"
         ";;Link  Shape  Geom1  Geom2  Geom3  Geom4  Barrels\n"
         "C1  CIRCULAR  1.5  0  0  0  1\n";
    return s.str();
}

// Legacy-format routing interface file: one node, constant flow, two
// periods bracketing the whole simulation (00:00 → 03:00).
std::string constant_inflow_iface(const std::string& node, double flow_cfs) {
    std::ostringstream s;
    s << "SWMM5 Interface File\n"
         "test inflows\n"
         "300  - reporting time step in sec\n"
         "1    - number of constituents as listed below:\n"
         "FLOW CFS\n"
         "1    - number of nodes as listed below:\n"
      << node << "\n"
      << "Node             Year Mon Day Hr  Min Sec FLOW\n"
      << node << "  2026 01 01 00 00 00 " << flow_cfs << "\n"
      << node << "  2026 01 01 03 00 00 " << flow_cfs << "\n";
    return s.str();
}

void write_file(const std::string& path, const std::string& content) {
    std::ofstream f(path);
    ASSERT_TRUE(f.is_open()) << "cannot create " << path;
    f << content;
}

std::string slurp(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

class IfaceRoutingTest : public ::testing::Test {
protected:
    SWMM_Engine engine_ = nullptr;
    std::vector<std::string> scratch_;

    void TearDown() override {
        if (engine_) {
            swmm_engine_close(engine_);
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
        for (const auto& p : scratch_) {
            std::error_code ec;
            fs::remove(p, ec);
        }
    }

    std::string scratch(const std::string& name) {
        auto p = (fs::current_path() / name).string();
        scratch_.push_back(p);
        return p;
    }

    // Open a model from INP text and stop before start().
    void open_model(const std::string& tag, const std::string& inp_text) {
        const auto inp = scratch("iface_" + tag + ".inp");
        const auto rpt = scratch("iface_" + tag + ".rpt");
        const auto out = scratch("iface_" + tag + ".out");
        write_file(inp, inp_text);

        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        ASSERT_EQ(swmm_engine_open(engine_, inp.c_str(), rpt.c_str(),
                                   out.c_str(), nullptr), SWMM_OK)
            << "open failed: " << swmm_get_last_error_msg(engine_);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    }

    void run_to_completion() {
        double elapsed = 0.0;
        for (;;) {
            int rc = swmm_engine_step(engine_, &elapsed);
            ASSERT_EQ(rc, SWMM_OK) << swmm_get_last_error_msg(engine_);
            if (elapsed <= 0.0) break;
        }
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
    }

    void close_engine() {
        ASSERT_EQ(swmm_engine_close(engine_), SWMM_OK);
        swmm_engine_destroy(engine_);
        engine_ = nullptr;
    }
};

// ---------------------------------------------------------------------------
// USE INFLOWS — flows from the file must arrive as external inflows.
// ---------------------------------------------------------------------------

TEST_F(IfaceRoutingTest, UseInflows_AppliesLateralFlow) {
    const double kFlow = 2.0;   // cfs, constant over the 1 h simulation
    const auto iface = scratch("iface_use.txt");
    write_file(iface, constant_inflow_iface("J1", kFlow));

    open_model("use", minimal_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH", iface.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();

    // 2 cfs × 3600 s = 7200 ft³ booked as external inflow.
    double ext_vol = 0.0;
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_EXTERNAL, &ext_vol),
              SWMM_OK);
    EXPECT_NEAR(ext_vol, kFlow * 3600.0, 0.10 * kFlow * 3600.0)
        << "interface inflow volume not delivered";

    // The conduit downstream of the receiving junction must have flowed.
    int c1 = swmm_link_index(engine_, "C1");
    ASSERT_GE(c1, 0);
    double qmax = 0.0;
    ASSERT_EQ(swmm_stat_link_max_flow(engine_, c1, &qmax), SWMM_OK);
    EXPECT_GT(qmax, 0.5 * kFlow);
}

TEST_F(IfaceRoutingTest, UseInflows_ParsedFromFilesSection) {
    // Same scenario, but the path arrives via the [FILES] section instead
    // of the C API — proves the parse → start() wiring end to end.
    const auto iface = scratch("iface_use_sect.txt");
    write_file(iface, constant_inflow_iface("J1", 2.0));

    std::string inp = minimal_model_inp();
    inp += "\n[FILES]\nUSE INFLOWS \"" + iface + "\"\n";
    open_model("use_sect", inp);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();

    double ext_vol = 0.0;
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_EXTERNAL, &ext_vol),
              SWMM_OK);
    EXPECT_GT(ext_vol, 0.0);
}

TEST_F(IfaceRoutingTest, UseInflows_MissingFileFailsStart) {
    open_model("missing", minimal_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH",
                             "no_such_interface_file.txt"), SWMM_OK);
    EXPECT_NE(swmm_engine_start(engine_, 1), SWMM_OK)
        << "start() must fail when the inflows file cannot be opened";
}

TEST_F(IfaceRoutingTest, UseInflows_BadHeaderFailsStart) {
    const auto iface = scratch("iface_bad.txt");
    write_file(iface, "NOT A SWMM5 Interface File\ngarbage\n");
    open_model("bad", minimal_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH", iface.c_str()), SWMM_OK);
    EXPECT_NE(swmm_engine_start(engine_, 1), SWMM_OK)
        << "start() must fail on a malformed interface file header";
}

// ---------------------------------------------------------------------------
// SAVE OUTFLOWS — legacy-format header + one row per outlet per report step.
// ---------------------------------------------------------------------------

TEST_F(IfaceRoutingTest, SaveOutflows_WritesLegacyHeaderAndRows) {
    const auto in_iface  = scratch("iface_chain_in.txt");
    const auto out_iface = scratch("iface_save_out.txt");
    write_file(in_iface, constant_inflow_iface("J1", 2.0));

    open_model("save", minimal_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH",  in_iface.c_str()),
              SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "OUTFLOWS_PATH", out_iface.c_str()),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();
    close_engine();   // flushes + closes the interface file

    ASSERT_TRUE(fs::exists(out_iface));
    const std::string content = slurp(out_iface);

    // Legacy header format.
    EXPECT_EQ(content.rfind("SWMM5 Interface File", 0), 0u)
        << "first line must be the legacy magic string";
    EXPECT_NE(content.find("FLOW CFS"), std::string::npos);
    EXPECT_NE(content.find("number of nodes"), std::string::npos);
    EXPECT_NE(content.find("O1"), std::string::npos)
        << "outfall O1 must be listed as an outlet node";

    // Data rows: 1 h simulation / 5 min reporting → ~12 rows for O1.
    int rows = 0;
    std::istringstream ss(content);
    std::string line;
    bool past_header = false;
    while (std::getline(ss, line)) {
        if (line.rfind("Node", 0) == 0) { past_header = true; continue; }
        if (past_header && line.rfind("O1", 0) == 0) ++rows;
    }
    EXPECT_GE(rows, 10) << "expected ~12 report-step rows for O1";
    EXPECT_LE(rows, 14) << "rows written per routing step instead of per "
                           "reporting step?";
}

TEST_F(IfaceRoutingTest, SameInflowsAndOutflowsFileFailsStart) {
    const auto iface = scratch("iface_same.txt");
    write_file(iface, constant_inflow_iface("J1", 1.0));
    open_model("same", minimal_model_inp());
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH",  iface.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "OUTFLOWS_PATH", iface.c_str()), SWMM_OK);
    EXPECT_NE(swmm_engine_start(engine_, 1), SWMM_OK)
        << "legacy ERR 357: inflows and outflows must not share a file";
}

// ---------------------------------------------------------------------------
// Chained coupling: model A SAVE OUTFLOWS → model B USE INFLOWS.
// ---------------------------------------------------------------------------

TEST_F(IfaceRoutingTest, ChainedModels_ConserveBoundaryVolume) {
    const double kFlow = 2.0;
    const auto a_in  = scratch("iface_a_in.txt");
    const auto a_out = scratch("iface_a_out.txt");
    write_file(a_in, constant_inflow_iface("J1", kFlow));

    // --- Model A: J1 → O1, driven by a constant interface inflow. ---
    open_model("chain_a", minimal_model_inp("J1", "O1"));
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH",  a_in.c_str()),  SWMM_OK);
    ASSERT_EQ(swmm_files_set(engine_, "OUTFLOWS_PATH", a_out.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();

    double a_outflow = 0.0;
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_OUTFLOW, &a_outflow),
              SWMM_OK);
    ASSERT_GT(a_outflow, 0.0);
    close_engine();

    // --- Model B: junction named O1 receives model A's outfall rows. ---
    open_model("chain_b", minimal_model_inp("O1", "OB"));
    ASSERT_EQ(swmm_files_set(engine_, "INFLOWS_PATH", a_out.c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK)
        << swmm_get_last_error_msg(engine_);
    run_to_completion();

    double b_ext = 0.0;
    ASSERT_EQ(swmm_get_routing_total(engine_, SWMM_ROUTING_EXTERNAL, &b_ext),
              SWMM_OK);
    EXPECT_GT(b_ext, 0.0) << "model B received nothing from model A's file";
    // Boundary conservation within routing tolerance. A's outfall volume
    // ramps up over the first report steps, so allow a generous band.
    EXPECT_NEAR(b_ext, a_outflow, 0.15 * a_outflow);
}

// ---------------------------------------------------------------------------
// Parser parity: wrong-mode rows are fatal input errors (legacy ERR_ITEMS).
// ---------------------------------------------------------------------------

TEST_F(IfaceRoutingTest, SaveInflowsRowFailsOpen) {
    std::string inp = minimal_model_inp();
    inp += "\n[FILES]\nSAVE INFLOWS \"whatever.txt\"\n";

    const auto path = scratch("iface_badmode.inp");
    const auto rpt  = scratch("iface_badmode.rpt");
    const auto out  = scratch("iface_badmode.out");
    write_file(path, inp);

    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);
    EXPECT_NE(swmm_engine_open(engine_, path.c_str(), rpt.c_str(),
                               out.c_str(), nullptr), SWMM_OK)
        << "SAVE INFLOWS must be rejected at parse time (legacy ERR_ITEMS)";
}

} // namespace
