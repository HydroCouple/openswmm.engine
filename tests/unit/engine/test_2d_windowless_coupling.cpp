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
 * @file      test_2d_windowless_coupling.cpp
 * @brief     Phase-3 gates for windowless co-advance coupling (explicit marcher):
 *        cross-domain conservation through the live in-marcher exchange, the
 *        exchange-conductance sign/magnitude contract, and the COUPLING_AREA
 *        AUTO derivation.
 *
 * @details Reuses the coupling-conservation harness (rain a coupled 2D patch,
 *          drain through junction J1 whose ONLY lateral inflow is the
 *          coupling) with INTEGRATOR EXPLICIT: the volume the 1D receives must
 *          equal the volume the 2D gave up through the batch queue delivery.
 *          Outputs land in ./windowless_coupling_out/ for review.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_forcing.h>

#include "2d/coupling/NodeCoupling.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "data/NodeData.hpp"

namespace fs = std::filesystem;
using namespace openswmm::twoD;

namespace {

// Same physical fixture as test_2d_coupling_conservation, on the marcher path.
// `area_token`: the authored exchange area; `auto_area`: adds COUPLING_AREA AUTO;
// `sur_depth`: J1's surcharge depth; `coupling_cd`: [2D_OPTIONS] COUPLING_CD;
// `row_tail`: replaces the map row's "CD AREA" tokens when given.
/**
 * @brief Build a windowless explicit-coupling parameter fixture.
 *
 * Uses 6 s routing maximum, 30 s surface maximum and fixed geometry; raw tokens may intentionally
 * be invalid for refusal cases. No file writing, validation or run.
 *
 * @param[in] area_token Authored coupling area m2 when row_tail is null.
 * @param[in] auto_area True emits global COUPLING_AREA AUTO.
 * @param[in] sur_depth J1 surcharge depth meters.
 * @param[in] coupling_cd Raw global coupling coefficient token.
 * @param[in] row_tail Optional raw map-row suffix replacing coefficient and area; null uses 0.7 and
 *     area_token.
 * @return Owned forty-minute CMS deck with 100 m2 pan, four LTS tiers and J1 vertex map.
 */
std::string build_model(double area_token, bool auto_area, double sur_depth = 0.0,
                        const std::string& coupling_cd = "0.7", const char* row_tail = nullptr) {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             00:40:00\n"
         "REPORT_STEP          00:01:00\n"
         "ROUTING_STEP         6\n"
         "VARIABLE_STEP        0.85\n"
         "ALLOW_PONDING        NO\n"
         "\n"
         "[JUNCTIONS]\n"
         "J1      0.0   1.0       0          "
      << sur_depth
      << "         0\n"
         "\n"
         "[OUTFALLS]\n"
         "O1     -0.5    FREE  NO\n"
         "\n"
         "[CONDUITS]\n"
         "C1      J1    O1  30.0    0.013      0         0          0\n"
         "\n"
         "[XSECTIONS]\n"
         "C1      CIRCULAR  0.3    0      0      0      1\n"
         "\n"
         "[2D_OPTIONS]\n"
         "INTEGRATOR       EXPLICIT\n"
         "LTS_TIERS        4\n"
         "MAX_TIMESTEP     30\n"
         "DRY_DEPTH        0.002\n"
         "COUPLING_CD      "
      << coupling_cd
      << "\n"
         "REPORT_2D        NO\n";
    if (auto_area)
        m << "COUPLING_AREA    AUTO\n";
    m << "\n"
         "[2D_VERTICES]\n"
         " 0.0    0.0   1.0\n"
         "10.0    0.0   1.0\n"
         "10.0   10.0   1.0\n"
         " 0.0   10.0   1.0\n"
         "\n"
         "[2D_TRIANGLES]\n"
         "0     1   2   0.03\n"
         "0     2   3   0.03\n"
         "\n"
         "[2D_VERTEX_NODE_MAP]\n"
         "0         J1    ";
    if (row_tail)
        m << row_tail << "\n";
    else
        m << "0.7  " << area_token << "\n";
    return m.str();
}

struct RunResult {
    bool ok = false;
    double received_1d = 0.0;
    double received_first_2min = 0.0; ///< 1D receipt while the drain is area-limited
    double given_2d_net = 0.0;
    double cont_2d = 0.0;
    std::string rpt_text;
};

/**
 * @brief Run the windowless coupling fixture and collect receipts/report text.
 *
 * Forces 0.001 m/s rainfall until prior elapsed reaches 600 s, integrates positive-time J1 lateral
 * flow and separately first-two-minute receipts. Steps without a guard; step error can still yield
 * ok=true. Owns/destroys engine and ignores stream/forcing/getter/end/report/close/read statuses;
 * partial runs/reports can be returned.
 *
 * @param[in] dir Existing writable artifact directory.
 * @param[in] tag Safe basename for overwritten .inp/.rpt/.out.
 * @param[in] area_token Authored coupling area m2 when row_tail is null.
 * @param[in] auto_area True emits global COUPLING_AREA AUTO.
 * @param[in] sur_depth J1 surcharge depth meters.
 * @param[in] coupling_cd Raw global coupling coefficient token.
 * @param[in] row_tail Optional raw map-row suffix replacing coefficient and area; null uses 0.7 and
 *     area_token.
 * @return Owned receipt volumes m3, dimensionless continuity and report text; ok=false on
 *     setup/inactive/node/start refusal.
 */
RunResult run(const fs::path& dir, const std::string& tag, double area_token, bool auto_area,
              double sur_depth = 0.0, const std::string& coupling_cd = "0.7",
              const char* row_tail = nullptr) {
    RunResult r;
    const fs::path inp = dir / (tag + ".inp");
    const fs::path rpt = dir / (tag + ".rpt");
    const fs::path out = dir / (tag + ".out");
    {
        std::ofstream f(inp);
        f << build_model(area_token, auto_area, sur_depth, coupling_cd, row_tail);
    }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                         nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    const int j1 = swmm_node_index(eng, "J1");
    if (!active || j1 < 0) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }

    constexpr double RAIN_OFF_S = 600.0;
    constexpr double RAIN_RATE = 0.001;
    double elapsed = 0.0, prev = 0.0;
    while (true) {
        const double t_s = elapsed * 86400.0;
        swmm_2d_force_rainfall_uniform(eng, (t_s < RAIN_OFF_S) ? RAIN_RATE : 0.0,
                                       SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
        const double dt_s = (elapsed - prev) * 86400.0;
        prev = elapsed;
        double lat = 0.0;
        if (dt_s > 0.0 && swmm_node_get_lateral_inflow(eng, j1, &lat) == SWMM_OK) {
            r.received_1d += lat * dt_s;
            if (elapsed * 86400.0 <= 120.0 + 1.0e-6)
                r.received_first_2min += lat * dt_s;
        }
    }
    swmm_engine_end(eng);

    double c12 = 0.0, c21 = 0.0;
    swmm_2d_get_mass_balance(eng, nullptr, nullptr, nullptr, &c12, &c21, nullptr, nullptr, nullptr,
                             nullptr, nullptr);
    r.given_2d_net = c21 - c12;
    swmm_2d_get_continuity_error(eng, &r.cont_2d);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    {
        std::ifstream f(rpt);
        std::ostringstream ss;
        ss << f.rdbuf();
        r.rpt_text = ss.str();
    }
    r.ok = true;
    return r;
}

} // namespace

class WindowlessCoupling2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    /**
     * @brief Create the reviewable windowless_coupling_out artifact directory.
     *
     * Sets dir_ relative to the test working directory and creates missing directories with the
     * throwing filesystem overload. Existing files are retained; no engine or input file is created
     * here.
     */
    void SetUp() override {
        dir_ = fs::path("windowless_coupling_out");
        fs::create_directories(dir_);
    }
};

// Cross-domain conservation on the marcher path: the volume the 1D receives
// through the batch queue delivery equals the volume the marcher's live
// exchange gave up — VARIABLE_STEP on, with rainfall ending well before run end
// to reduce the final pending delivery volume.
TEST_F(WindowlessCoupling2DTest, ReceivedEqualsGivenWindowless) {
    RunResult r = run(dir_, "windowless", 1.0, false);
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.given_2d_net, 1.0) << "fixture produced no exchange";
    // This fixture still integrates legacy display CMS (0.02832 per cfs),
    // whereas the surface ledger uses geometric SI (0.3048³ per ft³). The
    // ~1.11e-4 relative unit offset and final pending delivery are absorbed by
    // this 5e-4 bound, not elapsed-time roundoff. The stricter, unit-aligned
    // transfer ledger is checked in test_2d_coupling_conservation.cpp (#194).
    EXPECT_NEAR(r.received_1d, r.given_2d_net, 5.0e-4 * std::fabs(r.given_2d_net))
        << "1D received " << r.received_1d << " m³ vs 2D gave " << r.given_2d_net << " m³";
    EXPECT_LT(std::fabs(r.cont_2d), 0.5) << "2D continuity (%)";
}

// COUPLING_AREA AUTO derives the exchange area from the connected conduit:
// an absurd authored area (25 m² on a 0.07 m² pipe) trips the oversize
// warning without AUTO and is silenced (rederived) with it.
TEST_F(WindowlessCoupling2DTest, AutoAreaRederivesOversizedDefaults) {
    RunResult big = run(dir_, "area_big", 25.0, false);
    ASSERT_TRUE(big.ok);
    EXPECT_NE(big.rpt_text.find("has exchange AREA"), std::string::npos)
        << "oversize warning expected without AUTO";
    RunResult drv = run(dir_, "area_auto", 25.0, true);
    ASSERT_TRUE(drv.ok);
    EXPECT_EQ(drv.rpt_text.find("has exchange AREA"), std::string::npos)
        << "AUTO should rederive the baked-in oversized area";
    ASSERT_GT(drv.given_2d_net, 1.0) << "AUTO run produced no exchange";
}

// The AUTO-derived area must reach the LIVE exchange, not just silence the
// warning: the AUTO run drains like an explicit row authored at the derived
// area (1.25 × the 0.3 m pipe's 0.0707 m²) and unlike the 0.001 m² token it
// replaced — a token small enough that its orifice cannot keep up with the
// rain, so the two-minute receipt separates the areas.
TEST_F(WindowlessCoupling2DTest, AutoAreaReachesLiveExchange) {
    const double a_auto = 1.25 * 0.25 * 3.14159265358979323846 * 0.3 * 0.3;
    RunResult tiny = run(dir_, "auto_live_tiny", 0.001, false);
    RunResult drv = run(dir_, "auto_live_auto", 0.001, true);
    RunResult ref = run(dir_, "auto_live_ref", a_auto, false);
    ASSERT_TRUE(tiny.ok && drv.ok && ref.ok);
    ASSERT_GT(ref.received_first_2min, 0.1) << "fixture produced no early exchange";
    EXPECT_GT(std::fabs(tiny.received_first_2min - ref.received_first_2min),
              0.5 * ref.received_first_2min)
        << "fixture does not discriminate the 0.001 m² token from the derived area";
    EXPECT_NEAR(drv.received_first_2min, ref.received_first_2min, 0.01 * ref.received_first_2min)
        << "AUTO run should drain like an explicit derived-area row";
}

// A map row without a CD token takes [2D_OPTIONS] COUPLING_CD: it must run
// bit-identically to the same row authoring that CD, and differently from
// the 0.65 every unauthored row used to get. (AUTO sizes the area, since a
// row cannot author AREA without authoring CD.)
TEST_F(WindowlessCoupling2DTest, CouplingCdAppliesToRowsWithoutCd) {
    RunResult dflt = run(dir_, "cd_from_option", 1.0, true, 0.0, "0.9", "");
    RunResult expl = run(dir_, "cd_authored_09", 1.0, true, 0.0, "0.9", "0.9");
    RunResult old = run(dir_, "cd_authored_065", 1.0, true, 0.0, "0.9", "0.65");
    ASSERT_TRUE(dflt.ok && expl.ok && old.ok);
    ASSERT_GT(expl.received_1d, 1.0) << "fixture produced no exchange";
    EXPECT_EQ(dflt.received_first_2min, expl.received_first_2min)
        << "a row without CD must use COUPLING_CD";
    EXPECT_EQ(dflt.received_1d, expl.received_1d);
    EXPECT_NE(dflt.received_first_2min, old.received_first_2min)
        << "fixture does not discriminate CD 0.9 from 0.65";
}

// COUPLING_CD 0 (which the GUI could write) must not shut the rows without a
// CD: they keep the 0.65 they ran at before the option was applied, with a
// warning.
TEST_F(WindowlessCoupling2DTest, NonPositiveCouplingCdKeepsTheDefault) {
    RunResult zero = run(dir_, "cd_option_zero", 1.0, true, 0.0, "0", "");
    RunResult ref = run(dir_, "cd_option_zero_ref", 1.0, true, 0.0, "0.9", "0.65");
    ASSERT_TRUE(zero.ok && ref.ok);
    ASSERT_GT(ref.received_1d, 1.0) << "fixture produced no exchange";
    EXPECT_EQ(zero.received_1d, ref.received_1d) << "rows without CD must fall back to 0.65";
    EXPECT_NE(zero.rpt_text.find("COUPLING_CD 0 is not a discharge coefficient"), std::string::npos)
        << "fallback warning missing";
}

// A coupled node's SurDepth is ignored (it ponds from the rim and the
// exchange gate opens at the rim), and the warning has to say so rather than
// describe a sur_depth-raised spill threshold that does not exist.
TEST_F(WindowlessCoupling2DTest, SurDepthWarningDescribesTheRim) {
    RunResult r = run(dir_, "sur_depth_warning", 1.0, false, 0.5);
    ASSERT_TRUE(r.ok);
    EXPECT_NE(r.rpt_text.find("has sur_depth > 0 — ignored while the node is 2D-coupled"),
              std::string::npos)
        << "sur_depth warning missing or stale";
    EXPECT_EQ(r.rpt_text.find("full_depth + sur_depth as the spill threshold"), std::string::npos);
}

// The exchange conductance is non-negative everywhere and matches −∂Q/∂h_1d
// (central FD) where the crown gate and wet ramps are saturated.
TEST(WindowlessCouplingConductance, SignAndFdParity) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vx = {0.0, 10.0, 0.0};
    mesh.vy = {0.0, 0.0, 10.0};
    mesh.vz = {1.0, 1.0, 1.0};
    mesh.resize_triangles(1);
    mesh.set_triangle(0, 0, 1, 2);
    buildMeshTopology(mesh);

    SurfaceStateData state;
    state.resize(1, 3);
    state.volume[0] = 0.5 * mesh.tri_area[0]; // 0.5 m of ponded water
    state.depth[0] = 0.5;
    state.head[0] = 1.5;

    openswmm::NodeData nodes;
    nodes.resize(1);
    nodes.invert_elev[0] = 0.0;
    nodes.full_depth[0] = 1.0; // crown at 1.0 (2D-frame metres: len factors 1)
    nodes.depth[0] = 1.2;
    nodes.head[0] = 1.2;
    nodes.volume[0] = 5.0;

    SolverOptions2D opts; // len/flow factors default 1.0 in unit tests

    CouplingPoint cp{};
    cp.cell_idx = 0;
    cp.vertex_idx = -1;
    cp.node_idx = 0;
    cp.cd = 0.7;
    cp.area = 0.5;
    cp.is_outfall = false;

    for (double h1d = 0.2; h1d <= 2.6; h1d += 0.1) {
        nodes.head[0] = h1d;
        nodes.depth[0] = h1d; // invert 0
        const double G = computeNodeCouplingDQdh1d(cp, mesh, state, nodes, opts);
        ASSERT_GE(G, 0.0) << "conductance must never be negative (h1d=" << h1d << ")";
        // FD parity in the saturated regime: both sides well above crown+band
        // and clear of the |Δh| regularization knee.
        const double dh = std::fabs(state.head[0] - h1d);
        if (h1d > 1.1 && dh > 0.05) {
            const double eps = 1.0e-4;
            nodes.head[0] = h1d + eps;
            const double qp = computeNodeCouplingQ(cp, mesh, state, nodes, opts, nullptr);
            nodes.head[0] = h1d - eps;
            const double qm = computeNodeCouplingQ(cp, mesh, state, nodes, opts, nullptr);
            nodes.head[0] = h1d;
            const double g_fd = -(qp - qm) / (2.0 * eps);
            EXPECT_NEAR(G, g_fd, 0.05 * std::max(g_fd, 1.0e-9)) << "at h1d=" << h1d;
        }
    }
}
