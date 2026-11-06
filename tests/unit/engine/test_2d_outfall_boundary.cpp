// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      test_2d_outfall_boundary.cpp
 * @brief     Outfalls on the 2D surface (plan §6): a flap gate no longer hides a
 *        drowned outfall's tailwater (O1), TAILWATER_RELAX lags the stage
 *        (O2), COUPLING_HEAD samples the tailwater (O3), SPREAD delivers the
 *        discharge over a radius (O4), and the report lists each outfall's
 *        tailwater record (O5/O8).
 *
 * @details A J1 → O1 pipe discharging onto a 20 × 20 m patch of 5 m squares
 *          (32 triangles) whose centre vertex — six cells around it — is
 *          O1's coupling point; the bed rises `slope` away from it. Inputs
 *          and reports land in ./outfall_boundary_out/ for review.
 *
 * @ingroup engine_2d
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

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

const fs::path kOut = "outfall_boundary_out";

// O1 sits at the patch centre (vertex 12) on a bed at 0.2 m rising `slope`
// away from it; the patch starts `init_depth` deep (a drowned outfall) or
// dry. J1 drains to O1 through a 0.3 m pipe and receives a steady 0.05 m³/s.
/**
 * @brief Build a mapped-outfall radial-bowl boundary fixture.
 *
 * Base mesh elevation is 0.2 m, network inflow 0.05 m3/s and routing 2 s. Tokens are inserted
 * without validation; no filesystem or solver operation.
 *
 * @param[in] opts2d Raw extra 2D_OPTIONS rows.
 * @param[in] node_rows Raw 2D_NODE_COUPLING rows; empty omits section.
 * @param[in] gated True authors a gated O1 outfall.
 * @param[in] init_depth Uniform initial mesh depth meters.
 * @param[in] slope Dimensionless radial rise away from the central vertex.
 * @return Owned twenty-minute CMS deck with 20 m square 32-triangle mesh and center vertex mapped
 *     to O1.
 */
std::string deck(const std::string& opts2d, const std::string& node_rows, bool gated,
                 double init_depth, double slope) {
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\n"
         "START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:20:00\nREPORT_STEP 00:01:00\n"
         "ROUTING_STEP 2\n\n"
         "[JUNCTIONS]\nJ1 0.5 2.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 0.0 FREE "
      << (gated ? "YES" : "NO")
      << "\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
         "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 0.05\n\n"
         "[2D_OPTIONS]\nMAX_TIMESTEP 1\nDRY_DEPTH 0.002\nREPORT_2D NO\n"
      << opts2d
      << "\n"
         "[2D_VERTICES]\n";
    for (int j = 0; j <= 4; ++j)
        for (int i = 0; i <= 4; ++i)
            m << i * 5.0 << " " << j * 5.0 << " "
              << 0.2 + slope * std::hypot(i * 5.0 - 10.0, j * 5.0 - 10.0) << "\n";
    m << "\n[2D_TRIANGLES]\n";
    auto v = [](int i, int j) { return j * 5 + i; };
    for (int j = 0; j < 4; ++j)
        for (int i = 0; i < 4; ++i) {
            m << v(i, j) << " " << v(i + 1, j) << " " << v(i + 1, j + 1) << " 0.03 " << init_depth
              << "\n";
            m << v(i, j) << " " << v(i + 1, j + 1) << " " << v(i, j + 1) << " 0.03 " << init_depth
              << "\n";
        }
    m << "\n[2D_VERTEX_NODE_MAP]\n12 O1\n";
    if (!node_rows.empty())
        m << "\n[2D_NODE_COUPLING]\n" << node_rows;
    return m.str();
}

struct OutfallRun {
    bool ok = false;
    double first_stage = 0.0, max_stage = 0.0; ///< O1 head after step 1 / overall, m
    double stage_5min = 0.0;                   ///< O1 head at 5 min, m
    double min_link_flow = 1e300;              ///< C1, m³/s (negative = reverse)
    int receiving_cells = 0;                   ///< cells the outfall exchanged with
    std::string rpt;
};

/**
 * @brief Run the outfall boundary fixture and collect stage/flow/spatial diagnostics.
 *
 * ok=false on lifecycle setup refusal, otherwise true even after step/getter/report failure.
 * Samples first/first-after-five-minute stage and peak with no iteration cap. Counts absolute
 * coupling volumes above 1e-12 m3 before end. Owns/destroys engine; assumes O1/C1 lookup succeeds
 * and ignores most output statuses.
 *
 * @param[in] tag Safe retained artifact basename under kOut.
 * @param[in] opts2d Raw extra options passed to deck.
 * @param[in] rows Raw node-coupling rows passed to deck.
 * @param[in] gated Gated outfall switch.
 * @param[in] init_depth Initial surface depth meters.
 * @param[in] slope Radial bowl slope, default 0.02.
 * @return Owned stages meters, minimum C1 flow m3/s, count of cells with nontrivial applied
 *     coupling and report text.
 */
OutfallRun run(const std::string& tag, const std::string& opts2d, const std::string& rows,
               bool gated, double init_depth, double slope = 0.02) {
    OutfallRun r;
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp"), rpt = kOut / (tag + ".rpt");
    {
        std::ofstream f(inp);
        f << deck(opts2d, rows, gated, init_depth, slope);
    }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.string().c_str(), rpt.string().c_str(), "", nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    const int o1 = swmm_node_index(e, "O1"), c1 = swmm_link_index(e, "C1");
    int nt = 0;
    swmm_2d_triangle_count(e, &nt);
    std::vector<double> vol(static_cast<std::size_t>(nt));
    double elapsed = 0.0;
    for (int step = 0;; ++step) {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
        double head = 0.0, q = 0.0;
        swmm_node_get_head(e, o1, &head);
        swmm_link_get_flow(e, c1, &q);
        if (step == 0)
            r.first_stage = head;
        if (r.stage_5min == 0.0 && elapsed * 86400.0 >= 300.0)
            r.stage_5min = head;
        r.max_stage = std::max(r.max_stage, head);
        r.min_link_flow = std::min(r.min_link_flow, q);
    }
    swmm_2d_get_coupling_volume_bulk(e, vol.data());
    r.receiving_cells = static_cast<int>(
        std::count_if(vol.begin(), vol.end(), [](double x) { return std::fabs(x) > 1e-12; }));
    swmm_engine_end(e);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    std::ifstream f(rpt);
    std::stringstream ss;
    ss << f.rdbuf();
    r.rpt = ss.str();
    r.ok = true;
    return r;
}

} // namespace

// O1: a gated outfall under 0.5 m of standing water takes the 2D tailwater as
// its stage (the gate then blocks backflow through the link); before, the
// gate suppressed the tailwater and the outfall discharged as if free.
TEST(OutfallBoundary, GatedOutfallSeesTheTailwater) {
    const OutfallRun gated = run("gated_drowned", "", "", true, 0.5);
    const OutfallRun open = run("open_drowned", "", "", false, 0.5);
    ASSERT_TRUE(gated.ok && open.ok);
    EXPECT_GT(open.first_stage, 0.5) << "fixture: the ungated outfall is drowned";
    EXPECT_NEAR(gated.first_stage, open.first_stage, 1e-6)
        << "the gate must not hide the tailwater";
    EXPECT_GE(gated.min_link_flow, 0.0) << "the gate blocks reverse flow";
    EXPECT_LT(open.min_link_flow, 0.0)
        << "fixture: without the gate the tailwater backs up the pipe";
}

// O2: TAILWATER_RELAX makes the stage follow the rising 2D surface with a lag.
TEST(OutfallBoundary, TailwaterRelaxLagsTheStage) {
    const OutfallRun now = run("relax_none", "", "", false, 0.0);
    const OutfallRun slow = run("relax_600", "", "O1 OUTFALL TAILWATER_RELAX=600\n", false, 0.0);
    ASSERT_TRUE(now.ok && slow.ok);
    EXPECT_GT(now.stage_5min, 0.2) << "fixture: by 5 min the surface sets the stage";
    EXPECT_LT(slow.stage_5min, now.stage_5min - 0.01) << "a 600 s relaxation should lag";
    EXPECT_GT(slow.max_stage, slow.stage_5min) << "and still approach the surface";
}

// O4: SPREAD delivers the discharge over every cell within the radius, not
// only the cells around the outfall vertex (flat bed: all are eligible).
TEST(OutfallBoundary, SpreadDeliversOverTheRadius) {
    const OutfallRun local = run("spread_none", "", "", false, 0.0, 0.0);
    const OutfallRun spread = run("spread_8", "", "O1 OUTFALL SPREAD=8\n", false, 0.0, 0.0);
    ASSERT_TRUE(local.ok && spread.ok);
    EXPECT_EQ(local.receiving_cells, 6) << "fixture: the six cells around the outfall vertex";
    EXPECT_GT(spread.receiving_cells, 12) << "SPREAD=8 m should reach well past the stencil";
    // Only onto cells the water could reach: on a bed rising 50 % away from
    // the outfall, the dry uphill cells in the radius receive nothing.
    const OutfallRun uphill = run("spread_8_uphill", "", "O1 OUTFALL SPREAD=8\n", false, 0.0, 0.5);
    ASSERT_TRUE(uphill.ok);
    EXPECT_EQ(uphill.receiving_cells, 6);
}

// O3: COUPLING_HEAD VERTEX reads the tailwater as the wet-depth-weighted mean
// over the outfall's cells instead of the deepest cell's head.
TEST(OutfallBoundary, CouplingHeadSamplesTheTailwater) {
    const OutfallRun deepest = run("head_deepest", "", "", false, 0.03);
    const OutfallRun mean = run("head_vertex", "COUPLING_HEAD VERTEX\n", "", false, 0.03);
    ASSERT_TRUE(deepest.ok && mean.ok);
    EXPECT_GT(std::fabs(deepest.first_stage - mean.first_stage), 1e-4);
}

// O5/O8: the report lists each coupled outfall's tailwater record.
TEST(OutfallBoundary, ReportListsTheOutfallTailwater) {
    const OutfallRun r = run("report", "", "", true, 0.5);
    ASSERT_TRUE(r.ok);
    const auto at = r.rpt.find("2D Outfall Tailwater Summary");
    ASSERT_NE(at, std::string::npos) << r.rpt;
    const auto row = r.rpt.find("\n  O1 ", at);
    ASSERT_NE(row, std::string::npos);
    std::istringstream line(r.rpt.substr(row + 5, r.rpt.find('\n', row + 1) - row - 5));
    double engaged = 0, submerged = 0;
    line >> engaged >> submerged;
    EXPECT_NEAR(engaged, 20.0 / 60.0, 0.01) << "drowned for the whole 20 min run";
    EXPECT_GT(submerged, 0.0) << "0.5 m over a 0.3 m outlet crown";
}
