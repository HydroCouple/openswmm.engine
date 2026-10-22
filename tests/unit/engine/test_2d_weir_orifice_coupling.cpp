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
 * @file      test_2d_weir_orifice_coupling.cpp
 * @brief     COUPLING_LAW WEIR_ORIFICE in the engine: structure resolution from
 *        [2D_NODE_COUPLING] + presets + map rows, conservation through the live
 *        exchange, COUPLING_HEAD sampling, and the [2D_OPTIONS] round trip.
 *
 * @details plans/1D2D_FINE_SCALE_COUPLING_PLAN_2026-10-19.md §3. Inputs and
 *          outputs land in ./weir_orifice_coupling_out/ (working directory
 *          tests/unit/engine/data) for review.
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

#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_forcing.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>

#include "2d/coupling/NodeCoupling.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "core/SimulationContext.hpp"
#include "platform_test_support.hpp"

namespace fs = std::filesystem;
using namespace openswmm;
using namespace openswmm::twoD;

namespace {

constexpr double kPi = 3.14159265358979323846;

// ---------------------------------------------------------------------------
// resolveExchangeStructure on a one-junction, one-triangle model
// ---------------------------------------------------------------------------

struct Fixture {
    SimulationContext ctx{};
    MeshData mesh;
    SolverOptions2D opts;
    CouplingPoint cp{};
    /**
     * @brief Create the one-node/sloping-triangle geometry-resolution fixture.
     *
     * Owns native node invert2 m/full depth1 m converted to internal feet, a triangle with vertex
     * beds2.9/3.2/3.2 m, len_1d_to_2d=.3048 and a vertex0/cell0/node0 coupling point. Seeds
     * cd=.65/area1 m2 as unauthored defaults; does not initialize a router or run exchange.
     */
    Fixture() {
        ctx.nodes.resize(1);
        ctx.node_names.add("J1");
        ctx.nodes.invert_elev[0] = 2.0 / 0.3048; // 1D internal feet
        ctx.nodes.full_depth[0] = 1.0 / 0.3048;  // rim 3.0 m
        mesh.resize_vertices(3);
        mesh.vx = {0.0, 10.0, 0.0};
        mesh.vy = {0.0, 0.0, 10.0};
        mesh.vz = {2.9, 3.2, 3.2};
        mesh.resize_triangles(1);
        mesh.set_triangle(0, 0, 1, 2);
        buildMeshTopology(mesh);
        opts.len_1d_to_2d = 0.3048;
        cp.cell_idx = 0;
        cp.vertex_idx = 0;
        cp.node_idx = 0;
        cp.cd = 0.65;
        cp.cd_authored = false;
        cp.area = 1.0;
        cp.area_authored = false;
    }
    /**
     * @brief Ensure node-zero coupling metadata and assign selected keyed numeric values.
     *
     * Writes ctx.node_coupling_2d.params[0] directly after ensuring node0; assumes its row is zero in
     * this fresh fixture. Does not validate key bounds/values, resolve geometry or run fluxes.
     *
     * @param[in] t Requested node coupling type.
     * @param[in] kv Key/value pairs with valid enum indices, in each parameter's authored/native
     *     convention expected by resolution.
     * @return Row index returned by ensure_row.
     */
    int row(NodeCoupling2DType t, std::initializer_list<std::pair<NodeCoupling2DKey, double>> kv) {
        const int r = ctx.node_coupling_2d.ensure_row(0, t);
        for (auto [k, v] : kv)
            ctx.node_coupling_2d.params[0][static_cast<std::size_t>(k)] = v;
        return r;
    }
    /**
     * @brief Resolve the fixture coupling point geometry from authored node metadata.
     *
     * Mutates cp through the production resolver using owned mesh/context/options. Does not
     * independently validate the row/type relationship or advance any water state; production errors
     * propagate.
     *
     * @param[in] r Valid metadata row for node0 and the intended type.
     * @param[in] t Coupling type passed to production resolution.
     * @param[in] f Length conversion factor supplied to resolveExchangeStructure, default 1.
     */
    void resolve(int r, NodeCoupling2DType t, double f = 1.0) {
        resolveExchangeStructure(cp, mesh, ctx, opts, r, t, f);
    }
};

} // namespace

TEST(WeirOrificeResolve, OrificeLawLeavesThePointAlone) {
    Fixture fx;
    fx.resolve(-1, NodeCoupling2DType::MANHOLE);
    EXPECT_EQ(fx.cp.law, 0);
    EXPECT_EQ(fx.cp.geo.l_weir, 0.0) << "ORIFICE points keep the legacy cd/area path";
}

TEST(WeirOrificeResolve, ManholeDefaultsAndDiameter) {
    Fixture fx;
    fx.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    fx.resolve(-1, NodeCoupling2DType::MANHOLE);
    EXPECT_EQ(fx.cp.law, 1);
    EXPECT_NEAR(fx.cp.geo.l_weir, kPi * 1.2, 1e-12);
    EXPECT_NEAR(fx.cp.geo.a_orif, kPi * 1.44 / 4.0, 1e-12);
    EXPECT_NEAR(fx.cp.geo.z_crest, 3.0, 1e-12) << "NODE_RIM";
    EXPECT_DOUBLE_EQ(fx.cp.coef.c_w, 0.540);
    EXPECT_DOUBLE_EQ(fx.cp.coef.c_sw, 2.0 / 3.0 * 0.540) << "DEFAULT keeps the weirs continuous";
    EXPECT_DOUBLE_EQ(fx.cp.coef.c_oi, 0.167);
    EXPECT_DOUBLE_EQ(fx.cp.coef.c_oo, 0.160);

    Fixture us; // US units: authored D in feet, default 4 ft
    us.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    us.resolve(-1, NodeCoupling2DType::MANHOLE, 0.3048);
    EXPECT_NEAR(us.cp.geo.l_weir, kPi * 4.0 * 0.3048, 1e-12);
    Fixture d;
    d.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    d.resolve(d.row(NodeCoupling2DType::MANHOLE,
                    {{NodeCoupling2DKey::D, 2.0}, {NodeCoupling2DKey::LID_OPEN_FRACTION, 0.25}}),
              NodeCoupling2DType::MANHOLE, 0.3048);
    EXPECT_NEAR(d.cp.geo.l_weir, kPi * 2.0 * 0.3048 * 0.25, 1e-12) << "D in feet, lid fraction";
    EXPECT_NEAR(d.cp.geo.a_orif, kPi * std::pow(2.0 * 0.3048, 2) / 4.0 * 0.25, 1e-12);
}

TEST(WeirOrificeResolve, PresetsOverridesAndLawPerNode) {
    Fixture fx; // ORIFICE globally, WEIR_ORIFICE for this node
    fx.resolve(fx.row(NodeCoupling2DType::MANHOLE, {{NodeCoupling2DKey::LAW, 1},
                                                    {NodeCoupling2DKey::COEFFS, 1},
                                                    {NodeCoupling2DKey::CW, 0.6}}),
               NodeCoupling2DType::MANHOLE);
    EXPECT_EQ(fx.cp.law, 1);
    EXPECT_DOUBLE_EQ(fx.cp.coef.c_w, 0.6);
    EXPECT_DOUBLE_EQ(fx.cp.coef.c_sw, 0.056) << "RUBINATO2017 keeps its own C_sw";

    Fixture dflt;
    dflt.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    dflt.resolve(dflt.row(NodeCoupling2DType::MANHOLE, {{NodeCoupling2DKey::CW, 0.6}}),
                 NodeCoupling2DType::MANHOLE);
    EXPECT_DOUBLE_EQ(dflt.cp.coef.c_sw, 0.4) << "DEFAULT: C_sw = ⅔·C_w after the override";

    Fixture upper;
    upper.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    upper.opts.coupling_coeffs = static_cast<int>(NodeCoupling2DCoeffs::RUBINATO2017_UPPER);
    upper.resolve(-1, NodeCoupling2DType::MANHOLE);
    EXPECT_DOUBLE_EQ(upper.cp.coef.c_w, 0.587);
    EXPECT_DOUBLE_EQ(upper.cp.coef.c_oo, 0.161);

    Fixture open;
    open.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    open.resolve(open.row(NodeCoupling2DType::OPEN, {{NodeCoupling2DKey::AREA, 30.0}}),
                 NodeCoupling2DType::OPEN);
    EXPECT_DOUBLE_EQ(open.cp.coef.c_oi, 1.0) << "an open pond tracks the surface";
    EXPECT_NEAR(open.cp.geo.l_weir, std::sqrt(4.0 * kPi * 30.0), 1e-12);
}

TEST(WeirOrificeResolve, GrateGeometryAndMapRowPrecedence) {
    Fixture fx;
    fx.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    fx.resolve(fx.row(NodeCoupling2DType::GRATE, {{NodeCoupling2DKey::AREA, 0.18}}),
               NodeCoupling2DType::GRATE);
    EXPECT_DOUBLE_EQ(fx.cp.geo.a_orif, 0.18);
    EXPECT_NEAR(fx.cp.geo.l_weir, 4.0 * std::sqrt(0.18), 1e-12) << "square-grate perimeter";

    Fixture row; // an authored map-row CD / AREA beats the node row
    row.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
    row.cp.cd = 0.5;
    row.cp.cd_authored = true;
    row.cp.area = 0.3;
    row.cp.area_authored = true;
    row.resolve(row.row(NodeCoupling2DType::GRATE,
                        {{NodeCoupling2DKey::AREA, 0.18}, {NodeCoupling2DKey::PERIMETER, 1.8}}),
                NodeCoupling2DType::GRATE);
    EXPECT_DOUBLE_EQ(row.cp.geo.a_orif, 0.3);
    EXPECT_DOUBLE_EQ(row.cp.geo.l_weir, 1.8);
    EXPECT_DOUBLE_EQ(row.cp.coef.c_oi, 0.5);
    EXPECT_DOUBLE_EQ(row.cp.coef.c_oo, 0.5);
    EXPECT_DOUBLE_EQ(row.cp.coef.c_w, 0.540) << "the weir keeps its preset";
}

TEST(WeirOrificeResolve, CrestPolicies) {
    for (auto [crest, z] :
         {std::pair{0, 3.0}, std::pair{1, 2.9}, std::pair{2, 3.0}, std::pair{3, 2.9}}) {
        Fixture fx;
        fx.opts.coupling_law = CouplingLaw2D::WEIR_ORIFICE;
        fx.resolve(fx.row(NodeCoupling2DType::MANHOLE,
                          {{NodeCoupling2DKey::CREST, static_cast<double>(crest)}}),
                   NodeCoupling2DType::MANHOLE);
        EXPECT_NEAR(fx.cp.geo.z_crest, z, 1e-12) << "CREST " << crest;
    }
}

// ---------------------------------------------------------------------------
// Engine runs
// ---------------------------------------------------------------------------

namespace {

const fs::path kOut = "weir_orifice_coupling_out";

// The windowless-coupling patch: rain on a 10×10 m patch drains through J1
// (rim at the bed, 1.0 m) to a free outfall.
/**
 * @brief Construct the rainy surface-to-node weir/orifice integration deck.
 *
 * Rain is forced at runtime rather than authored. Sets four tiers, maximum surface timestep 30 s and
 * variable native routing around 6 s; constructs text without validation/simulation.
 *
 * @param[in] opts2d Raw2D_OPTIONS lines with required delimiters.
 * @param[in] node_rows Optional raw2D_NODE_COUPLING rows; nonempty emits the section header.
 * @return Owned forty-minute CMS/DYNWAVE deck with 10-by-10 m bed1 m patch, vertex0-to-J1 mapping
 *     and free downstream outfall.
 */
std::string deck(const std::string& opts2d, const std::string& node_rows) {
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\n"
         "START_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:40:00\nREPORT_STEP 00:01:00\n"
         "ROUTING_STEP 6\nVARIABLE_STEP 0.85\nALLOW_PONDING NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 4\nMAX_TIMESTEP 30\nDRY_DEPTH 0.002\n"
         "REPORT_2D NO\n"
      << opts2d
      << "\n"
         "[2D_VERTICES]\n 0.0 0.0 1.0\n10.0 0.0 1.0\n10.0 10.0 1.0\n 0.0 10.0 1.0\n\n"
         "[2D_TRIANGLES]\n0 1 2 0.03\n0 2 3 0.03\n\n"
         "[2D_VERTEX_NODE_MAP]\n0 J1\n";
    if (!node_rows.empty())
        m << "\n[2D_NODE_COUPLING]\n" << node_rows;
    return m.str();
}

struct LiveRun {
    bool ok = false;
    double received_1d = 0.0, first_2min = 0.0, given_2d_net = 0.0, cont_2d = 0.0;
    std::string backend;
};

/**
 * @brief Run the forced-rain coupling deck and compare native receipts with surface exchange.
 *
 * Applies override rain.001 m/s until prior elapsed time reaches 600 s, then zero; stops at step
 * error or completion and still sets ok=true after cleanup. Thus ok marks startup/cleanup only.
 * Omits terminal elapsed<=0 sample from receipt integration, ignores most getter/end/report/close
 * statuses and destroys the engine; no interpolation at forcing/cutoff times occurs.
 *
 * @param[in] tag Safe artifact basename under kOut for overwritten input/report.
 * @param[in] opts2d Raw surface options passed to deck.
 * @param[in] node_rows Raw coupling rows passed to deck.
 * @return Owned integrated J1 lateral volume m3, first 120 s receipt m3, net2D-to-1D exchange m3,2D
 *     continuity fraction, backend text and ok flag.
 */
LiveRun run(const std::string& tag, const std::string& opts2d, const std::string& node_rows) {
    LiveRun r;
    fs::create_directories(kOut);
    const fs::path inp = kOut / (tag + ".inp");
    {
        std::ofstream f(inp);
        f << deck(opts2d, node_rows);
    }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.string().c_str(), (kOut / (tag + ".rpt")).string().c_str(), "",
                         nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK || swmm_engine_start(e, 0) != SWMM_OK) {
        swmm_engine_destroy(e);
        return r;
    }
    const int j1 = swmm_node_index(e, "J1");
    SWMM_2DRunStats stats{};
    if (swmm_2d_get_run_stats(e, &stats) == SWMM_OK)
        r.backend = stats.backend;
    double elapsed = 0.0, prev = 0.0;
    for (;;) {
        swmm_2d_force_rainfall_uniform(e, elapsed * 86400.0 < 600.0 ? 0.001 : 0.0,
                                       SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST);
        if (swmm_engine_step(e, &elapsed) != SWMM_OK || elapsed <= 0.0)
            break;
        const double dt = (elapsed - prev) * 86400.0;
        prev = elapsed;
        double lat = 0.0;
        if (dt > 0.0 && swmm_node_get_lateral_inflow(e, j1, &lat) == SWMM_OK) {
            r.received_1d += lat * dt;
            if (elapsed * 86400.0 <= 120.0 + 1e-6)
                r.first_2min += lat * dt;
        }
    }
    swmm_engine_end(e);
    double c12 = 0.0, c21 = 0.0;
    swmm_2d_get_mass_balance(e, nullptr, nullptr, nullptr, &c12, &c21, nullptr, nullptr, nullptr,
                             nullptr, nullptr);
    r.given_2d_net = c21 - c12;
    swmm_2d_get_continuity_error(e, &r.cont_2d);
    swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    r.ok = true;
    return r;
}

} // namespace

// Everything the 2D surface gives up through the WEIR_ORIFICE live exchange
// reaches the 1D node, and the 2D ledger closes.
TEST(WeirOrificeCoupling, ConservesThroughTheLiveExchange) {
    LiveRun r = run("weir_orifice_default", "COUPLING_LAW WEIR_ORIFICE\n", "");
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.given_2d_net, 1.0) << "no exchange";
    EXPECT_NEAR(r.received_1d, r.given_2d_net, 5e-4 * r.given_2d_net);
    EXPECT_LT(std::fabs(r.cont_2d), 0.5);
}

// The structure drives the exchange under WEIR_ORIFICE (a smaller manhole
// drains less while the surface is shallow) and is ignored under ORIFICE.
TEST(WeirOrificeCoupling, StructureMattersOnlyUnderTheNewLaw) {
    LiveRun big = run("wo_d12", "COUPLING_LAW WEIR_ORIFICE\n", "J1 MANHOLE D=1.2\n");
    LiveRun small = run("wo_d02", "COUPLING_LAW WEIR_ORIFICE\n", "J1 MANHOLE D=0.2\n");
    ASSERT_TRUE(big.ok && small.ok);
    EXPECT_LT(small.first_2min, 0.9 * big.first_2min);

    LiveRun legacy = run("orifice_plain", "", "");
    LiveRun legacy_d = run("orifice_with_d", "", "J1 MANHOLE D=0.2\n");
    ASSERT_TRUE(legacy.ok && legacy_d.ok);
    EXPECT_EQ(legacy.received_1d, legacy_d.received_1d) << "ORIFICE must not read the structure";
    EXPECT_EQ(legacy.first_2min, legacy_d.first_2min);
}

TEST(WeirOrificeCoupling, StencilWetMeanHeadRunsAndConserves) {
    LiveRun r =
        run("wo_stencil", "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_HEAD STENCIL_WET_MEAN\n", "");
    ASSERT_TRUE(r.ok);
    ASSERT_GT(r.given_2d_net, 1.0);
    EXPECT_NEAR(r.received_1d, r.given_2d_net, 5e-4 * r.given_2d_net);
}

TEST(WeirOrificeCoupling, OptionsRoundTripThroughInp) {
    fs::create_directories(kOut);
    const fs::path inp = kOut / "options_in.inp", out = kOut / "options_out.inp";
    {
        std::ofstream f(inp);
        f << deck("COUPLING_LAW WEIR_ORIFICE\nCOUPLING_HEAD STENCIL_WET_MEAN\n"
                  "COUPLING_COEFFS rubinato2017\nCOUPLING_POND_MODE TRACKING\n"
                  "COUPLING_POND_RELAX 20\nCOUPLING_RIM_BAND 0.03\nCOUPLING_EQUALIZE NO\n",
                  "");
    }
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), "", "", nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write(e, out.string().c_str()), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e2, out.string().c_str(), "", "", nullptr), SWMM_OK);
    char buf[64];
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_LAW", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "WEIR_ORIFICE");
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_HEAD", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "STENCIL_WET_MEAN");
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_COEFFS", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "RUBINATO2017");
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_POND_MODE", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "TRACKING");
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_POND_RELAX", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "20");
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_RIM_BAND", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "0.03");
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_EQUALIZE", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "NO");
    EXPECT_EQ(swmm_options_set_ext(e2, "COUPLING_EQUALIZE", "DEFAULT"), SWMM_OK);
    ASSERT_EQ(swmm_options_get_ext(e2, "COUPLING_EQUALIZE", buf, 64), SWMM_OK);
    EXPECT_STREQ(buf, "DEFAULT");
    EXPECT_EQ(swmm_options_set_ext(e2, "COUPLING_LAW", "WEIR"), SWMM_ERR_BADPARAM);
    swmm_engine_close(e2);
    swmm_engine_destroy(e2);
}

// The Kokkos marcher evaluates the same law (ExchangeLaw.hpp on device) and
// the same head sampling: the OpenMP plugin agrees with the CPU marcher to
// round-off. Skipped where the plugin is not built or not found (set
// OPENSWMM_GPU_PLUGIN_PATH to the plugin directory).
TEST(WeirOrificeCoupling, KokkosBackendMatchesCpu) {
    const std::string opts = "COUPLING_LAW WEIR_ORIFICE\nCOUPLING_HEAD STENCIL_WET_MEAN\n"
                             "COUPLING_POND_MODE TRACKING\nCOUPLING_EQUALIZE YES\n";
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "cpu");
    LiveRun cpu = run("wo_backend_cpu", opts, "J1 MANHOLE D=0.6\n");
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "omp");
    LiveRun omp = run("wo_backend_omp", opts, "J1 MANHOLE D=0.6\n");
    plattest::setEnvVar("OPENSWMM_2D_BACKEND", "cpu");
    ASSERT_TRUE(cpu.ok && omp.ok);
    if (omp.backend.rfind("omp", 0) != 0)
        GTEST_SKIP() << "omp plugin not loaded (backend: " << omp.backend << ")";
    ASSERT_GT(cpu.received_1d, 1.0);
    EXPECT_NEAR(omp.received_1d, cpu.received_1d, 1e-9 * cpu.received_1d);
    EXPECT_NEAR(omp.first_2min, cpu.first_2min, 1e-9 * std::max(1.0, cpu.first_2min));
}
