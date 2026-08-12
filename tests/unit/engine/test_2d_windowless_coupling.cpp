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
 * @file test_2d_windowless_coupling.cpp
 * @brief Phase-3 gates for windowless co-advance coupling (explicit marcher):
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
// `area_token`: the authored exchange area; `auto_area`: adds COUPLING_AREA AUTO.
std::string build_model(double area_token, bool auto_area) {
    std::ostringstream m;
    m <<
        "[OPTIONS]\n"
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
        "J1      0.0   1.0       0          0         0\n"
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
        "COUPLING_CD      0.7\n"
        "REPORT_2D        NO\n";
    if (auto_area) m << "COUPLING_AREA    AUTO\n";
    m <<
        "\n"
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
        "0         J1    0.7  " << area_token << "\n";
    return m.str();
}

struct RunResult {
    bool   ok = false;
    double received_1d = 0.0;
    double given_2d_net = 0.0;
    double cont_2d = 0.0;
    std::string rpt_text;
};

RunResult run(const fs::path& dir, const std::string& tag, double area_token,
              bool auto_area) {
    RunResult r;
    const fs::path inp = dir / (tag + ".inp");
    const fs::path rpt = dir / (tag + ".rpt");
    const fs::path out = dir / (tag + ".out");
    { std::ofstream f(inp); f << build_model(area_token, auto_area); }

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                         out.string().c_str(), nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng); return r;
    }
    if (swmm_engine_initialize(eng) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }
    int active = 0;
    swmm_2d_is_active(eng, &active);
    const int j1 = swmm_node_index(eng, "J1");
    if (!active || j1 < 0) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }
    if (swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng); swmm_engine_destroy(eng); return r;
    }

    constexpr double RAIN_OFF_S = 600.0;
    constexpr double RAIN_RATE  = 0.001;
    double elapsed = 0.0, prev = 0.0;
    while (true) {
        const double t_s = elapsed * 86400.0;
        swmm_2d_force_rainfall_uniform(eng,
                                       (t_s < RAIN_OFF_S) ? RAIN_RATE : 0.0,
                                       SWMM_FORCING_OVERRIDE,
                                       SWMM_FORCING_PERSIST);
        if (swmm_engine_step(eng, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
        const double dt_s = (elapsed - prev) * 86400.0;
        prev = elapsed;
        double lat = 0.0;
        if (dt_s > 0.0 &&
            swmm_node_get_lateral_inflow(eng, j1, &lat) == SWMM_OK)
            r.received_1d += lat * dt_s;
    }
    swmm_engine_end(eng);

    double c12 = 0.0, c21 = 0.0;
    swmm_2d_get_mass_balance(eng, nullptr, nullptr, nullptr, &c12, &c21,
                             nullptr, nullptr, nullptr, nullptr, nullptr);
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

}  // namespace

class WindowlessCoupling2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    void SetUp() override {
        dir_ = fs::path("windowless_coupling_out");
        fs::create_directories(dir_);
    }
};

// Cross-domain conservation on the marcher path: the volume the 1D receives
// through the batch queue delivery equals the volume the marcher's live
// exchange gave up — VARIABLE_STEP on, drain completed well before run end so
// no queued volume is stranded at the final step.
TEST_F(WindowlessCoupling2DTest, ReceivedEqualsGivenWindowless) {
    RunResult r = run(dir_, "windowless", 1.0, false);
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.given_2d_net, 1.0) << "fixture produced no exchange";
    // 10× tighter than the window-path contract (0.5%): the two ledgers are
    // integrated through independently-rounded C-API elapsed-time round-trips,
    // which bounds how closely they can be compared.
    EXPECT_NEAR(r.received_1d, r.given_2d_net,
                5.0e-4 * std::fabs(r.given_2d_net))
        << "1D received " << r.received_1d << " m³ vs 2D gave "
        << r.given_2d_net << " m³";
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

// The exchange conductance is non-negative everywhere and matches −∂Q/∂h_1d
// (central FD) where the crown gate and wet ramps are saturated.
TEST(WindowlessCouplingConductance, SignAndFdParity) {
    MeshData mesh;
    mesh.resize_vertices(3);
    mesh.vx = {0.0, 10.0, 0.0};
    mesh.vy = {0.0, 0.0, 10.0};
    mesh.vz = {1.0, 1.0, 1.0};
    mesh.resize_triangles(1);
    mesh.tri_v0[0] = 0; mesh.tri_v1[0] = 1; mesh.tri_v2[0] = 2;
    buildMeshTopology(mesh);

    SurfaceStateData state;
    state.resize(1, 3);
    state.volume[0] = 0.5 * mesh.tri_area[0];   // 0.5 m of ponded water
    state.depth[0]  = 0.5;
    state.head[0]   = 1.5;

    openswmm::NodeData nodes;
    nodes.resize(1);
    nodes.invert_elev[0] = 0.0;
    nodes.full_depth[0]  = 1.0;   // crown at 1.0 (2D-frame metres: len factors 1)
    nodes.depth[0]       = 1.2;
    nodes.head[0]        = 1.2;
    nodes.volume[0]      = 5.0;

    SolverOptions2D opts;   // len/flow factors default 1.0 in unit tests

    CouplingPoint cp{};
    cp.cell_idx = 0;
    cp.vertex_idx = -1;
    cp.node_idx = 0;
    cp.cd = 0.7;
    cp.area = 0.5;
    cp.is_outfall = false;

    for (double h1d = 0.2; h1d <= 2.6; h1d += 0.1) {
        nodes.head[0]  = h1d;
        nodes.depth[0] = h1d;   // invert 0
        const double G =
            computeNodeCouplingDQdh1d(cp, mesh, state, nodes, opts);
        ASSERT_GE(G, 0.0) << "conductance must never be negative (h1d=" << h1d
                          << ")";
        // FD parity in the saturated regime: both sides well above crown+band
        // and clear of the |Δh| regularization knee.
        const double dh = std::fabs(state.head[0] - h1d);
        if (h1d > 1.1 && dh > 0.05) {
            const double eps = 1.0e-4;
            nodes.head[0] = h1d + eps;
            const double qp = computeNodeCouplingQ(cp, mesh, state, nodes,
                                                   opts, nullptr);
            nodes.head[0] = h1d - eps;
            const double qm = computeNodeCouplingQ(cp, mesh, state, nodes,
                                                   opts, nullptr);
            nodes.head[0] = h1d;
            const double g_fd = -(qp - qm) / (2.0 * eps);
            EXPECT_NEAR(G, g_fd, 0.05 * std::max(g_fd, 1.0e-9))
                << "at h1d=" << h1d;
        }
    }
}
