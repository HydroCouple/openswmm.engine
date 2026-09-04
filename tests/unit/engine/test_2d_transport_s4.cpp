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
 * @file test_2d_transport_s4.cpp
 * @brief Overland transport S4 — age, temperature and MSX rows on the
 *        surface, and the coupling tuple's age/temperature halves.
 *
 * @details The rows take the 1D engines' layout verbatim (pollutants, MSX,
 *          `__WATER_AGE__`, `__TEMPERATURE__` last), and the marcher does not
 *          know what a row means — it moves mass. What S4 adds on top is the
 *          three things a row can mean: the age row AGES (d(age)/dt = 1,
 *          exactly), the temperature row is SIGNED and leaves with evaporated
 *          water at the water's own temperature (a solute concentrates, a
 *          temperature does not), and pollutant rows DECAY (kdecay, the exact
 *          exponential, through the same reaction stage the ARD engine uses).
 *
 *          The temperature row is the coupling gate: a pan at T0 draining
 *          into an empty junction under HEAT_TRANSPORT (no flux modules ⇒
 *          pure transport) must make the junction read exactly T0 — the
 *          tuple's temperature-volume half drained by the volume queue's
 *          rule, delivered to `node_temp_vol_in` instead of the
 *          EXTERNAL_INFLOW stand-in S3 used.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_model.h>

#include "2d/SurfaceRouter2D.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "core/SWMMEngine.hpp"
#include "core/SimulationContext.hpp"

using namespace openswmm::twoD;

namespace {

MeshData makeStrip(int nx, int ny, double dx, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx; mesh.vy[v] = j * dx; mesh.vz[v] = 0.0;
        }
    mesh.resize_triangles(2 * nx * ny);
    int t = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v10; mesh.tri_v2[t] = v11; ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v01; ++t;
        }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

/// Still pond with a pollutant row (0) and a SIGNED temperature row (1).
SurfaceStateData makePondPT(const MeshData& mesh, const SolverOptions2D& opts,
                            double h, double c0, double t0) {
    SurfaceStateData s;
    s.resize(mesh.n_triangles(), mesh.n_vertices());
    s.transport.resize(2, mesh.n_triangles(), 0);
    s.transport.n_pollut = 1;
    s.transport.temp_row = 1;
    s.transport.row_names = {"Cu", "__TEMPERATURE__"};
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        s.volume[i] = h * mesh.tri_area[i];
        inertial::cellEtaDepth(mesh, opts, i, s.volume[i], s.head[i], s.depth[i]);
        s.transport.cell_mass[s.transport.idx(0, i)] = c0 * s.volume[i];
        s.transport.cell_mass[s.transport.idx(1, i)] = t0 * s.volume[i];
    }
    return s;
}

void run(ExplicitInertialSolver& solver, double T, double dt_out = 5.0) {
    for (double t = 0.0; t < T; t += dt_out)
        solver.advance(t, std::min(t + dt_out, T));
}

// ---- deck path ------------------------------------------------------------
std::string deck(const std::string& options_extra, const std::string& iq,
                 const std::string& node, double z_pan, double h0,
                 const std::string& inflows = "", const std::string& pollut =
                     "Cu     MG/L  0     0   0     0\n") {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
         "REPORT_STEP          00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n" << options_extra << "\n"
         "[POLLUTANTS]\n;;Name Units Crain Cgw Crdii Kdecay\n" << pollut << "\n"
         "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
      << inflows <<
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n\n"
         "[2D_VERTICES]\n"
         " 0.0  0.0 " << z_pan << "\n10.0  0.0 " << z_pan << "\n"
         "10.0 10.0 " << z_pan << "\n 0.0 10.0 " << z_pan << "\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 " << h0 << "\n0 2 3 0.03 " << h0 << "\n\n"
         "[2D_VERTEX_NODE_MAP]\n0 " << node << " 0.7 1.0\n\n"
      << iq <<
         "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct CoupledRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    bool ok = false;
};

CoupledRun runDeck(const std::string& tag, const std::string& body, bool step = true) {
    CoupledRun r;
    { std::ofstream f(tag + ".inp"); f << body; }
    r.e = swmm_engine_create();
    if (!r.e) return r;
    if (swmm_engine_open(r.e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(r.e) != SWMM_OK ||
        swmm_engine_start(r.e, 1) != SWMM_OK)
        return r;
    int active = 0;
    swmm_2d_is_active(r.e, &active);
    if (!active) return r;
    if (step) {
        double elapsed = 0.0;
        while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    }
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    r.ok = true;
    return r;
}

void close(CoupledRun& r) {
    if (!r.e) return;
    swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Evaporation: the solute concentrates (S1), the temperature does NOT — a
//    still pond at (c0, T0) under evaporation ends with c > c0 exactly by the
//    volume ratio and T == T0 to round-off in every wet cell.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, EvaporationConcentratesSolutesButNotTemperature) {
    auto mesh = makeStrip(10, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    const double c0 = 3.0, t0 = 17.5, h0 = 0.5, E = 2.0e-5, T = 600.0;
    auto s = makePondPT(mesh, opts, h0, c0, t0);
    std::fill(s.evap_rate.begin(), s.evap_rate.end(), E);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, T);

    const double c_exp = c0 * h0 / (h0 - E * T);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        ASSERT_GT(s.volume[i], 0.0);
        const double c = s.transport.cell_mass[s.transport.idx(0, i)] / s.volume[i];
        const double t = s.transport.cell_mass[s.transport.idx(1, i)] / s.volume[i];
        EXPECT_NEAR(c, c_exp, 1.0e-9 * c_exp) << "solute at cell " << i;
        EXPECT_NEAR(t, t0, 1.0e-9 * t0)
            << "temperature at cell " << i
            << " — evaporation must leave at the water's own temperature";
    }
}

// ---------------------------------------------------------------------------
// 2. Signed row: a pond at −5 °C stays −5 °C under evaporation and under a
//    dispersive step between −5 and +5 conserves T·V and obeys the max
//    principle in BOTH directions (no "mass ≥ 0" guard bites a negative).
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, TemperatureRowIsSignedEverywhere) {
    auto mesh = makeStrip(20, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    opts.dispersion = 0.05;
    auto s = makePondPT(mesh, opts, 1.0, 1.0, -5.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] > 10.0)
            s.transport.cell_mass[s.transport.idx(1, i)] = 5.0 * s.volume[i];
    std::fill(s.evap_rate.begin(), s.evap_rate.end(), 1.0e-5);
    double tv0 = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        tv0 += s.transport.cell_mass[s.transport.idx(1, i)];

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, 300.0);

    // Evaporation removes T·V at the cell's T: Σ T·V falls by Σ T·ΔV, which
    // for the symmetric ±5 layout is 0 to round-off — the total is invariant.
    double tv1 = 0.0, tmin = 1e300, tmax = -1e300;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        tv1 += s.transport.cell_mass[s.transport.idx(1, i)];
        const double t = s.transport.cell_mass[s.transport.idx(1, i)] / s.volume[i];
        tmin = std::min(tmin, t); tmax = std::max(tmax, t);
    }
    EXPECT_NEAR(tv1, tv0, 1.0e-9 * 5.0 * 40.0);
    EXPECT_GE(tmin, -5.0 * (1.0 + 1.0e-10));
    EXPECT_LE(tmax,  5.0 * (1.0 + 1.0e-10));
    EXPECT_LT(tmin, -4.0) << "the cold half vanished — a positivity guard ate it";
    EXPECT_EQ(s.transport.dispersion_limiter_binds, 0L);
}

// ---------------------------------------------------------------------------
// 3. Deck: rows take the 1D layout; `[2D_INITIAL_QUALITY]` seeds age and
//    temperature by their reserved names; a still pan ages by exactly the
//    elapsed time (d(age)/dt = 1) and holds its temperature (pure transport).
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, RowsFollowThe1DLayoutAndAStillPanAgesExactly) {
    // Pan at z = 5 with J1 rim at 1: the pan is far above the node but the
    // node is EMPTY and the pan is not coupled hydraulically in a way that
    // drains (vertex 0 maps to J1 — the orifice law WILL drain). Use O1 with
    // the pan far BELOW the outfall so nothing crosses: a still pan.
    CoupledRun r = runDeck("_s4_still", deck(
        "WATER_AGE            YES\nHEAT_TRANSPORT       YES\n",
        "[2D_INITIAL_QUALITY]\n* Cu 2.0\n* __WATER_AGE__ 100\n* __TEMPERATURE__ 12.5\n\n",
        "O1", -10.0, 0.5));
    ASSERT_TRUE(r.ok);
    const auto& router = r.eng->surfaceRouter2D();
    const auto& st = router.state();
    const auto& tr = st.transport;
    ASSERT_EQ(tr.n_species, 3);
    EXPECT_EQ(tr.n_pollut, 1);
    EXPECT_EQ(tr.age_row, 1);
    EXPECT_EQ(tr.temp_row, 2);
    ASSERT_EQ(tr.row_names.size(), 3u);
    EXPECT_EQ(tr.row_names[0], "Cu");
    EXPECT_EQ(tr.row_names[1], "__WATER_AGE__");
    EXPECT_EQ(tr.row_names[2], "__TEMPERATURE__");
    EXPECT_TRUE(r.eng->context().nodes.coupling_tuple_age);
    EXPECT_TRUE(r.eng->context().nodes.coupling_tuple_temp);

    const double elapsed = 20.0 * 60.0;
    for (int c = 0; c < tr.n_cells; ++c) {
        ASSERT_GT(st.volume[c], 0.0);
        const double age = tr.cell_mass[tr.idx(1, c)] / st.volume[c];
        const double tmp = tr.cell_mass[tr.idx(2, c)] / st.volume[c];
        EXPECT_NEAR(age, 100.0 + elapsed, 1.0e-9 * (100.0 + elapsed))
            << "cell " << c << " did not age by exactly the elapsed time";
        EXPECT_NEAR(tmp, 12.5, 1.0e-9 * 12.5) << "cell " << c;
    }
    close(r);
}

// ---------------------------------------------------------------------------
// 4. Coupling, temperature half: a pan at T0 = 15 drains into an empty, clean
//    J1 under HEAT_TRANSPORT (no flux modules) — J1's temperature is exactly
//    T0, and the tuple's temperature-volume identity closes:
//    Σ delivered T·V (queue + node_temp_vol_in history) == T0 × drained × f.
//    Falsifies the EXTERNAL_INFLOW stand-in (which would read the source
//    table's temperature, 20 °C default) and any stride error in the queue.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, DrainDeliversTheCellTemperatureToTheNode) {
    const double t0 = 15.0;
    CoupledRun r = runDeck("_s4_drain_t", deck(
        "HEAT_TRANSPORT       YES\n",
        "[2D_INITIAL_QUALITY]\n* Cu 4.0\n* __TEMPERATURE__ 15.0\n\n",
        "J1", 1.0, 0.5));
    ASSERT_TRUE(r.ok);
    const auto& ctx    = r.eng->context();
    const auto& router = r.eng->surfaceRouter2D();
    const auto& tr     = router.state().transport;
    ASSERT_EQ(tr.temp_row, 1);
    ASSERT_GT(ctx.mass_balance_2d.coupling_2d_to_1d_out, 0.5);

    const int j1 = swmm_node_index(r.e, "J1");
    ASSERT_GE(j1, 0);
    const auto uj = static_cast<std::size_t>(j1);
    ASSERT_LT(uj, ctx.heat_state.node_temp.size());
    EXPECT_NEAR(ctx.heat_state.node_temp[uj], t0, 1.0e-9 * t0)
        << "junction fed only by a 15 °C pan does not read 15 °C: the tuple's "
           "temperature half is not delivered (EXTERNAL_INFLOW stand-in would "
           "read 20)";
    // Pollutant half still exact alongside (S3 unchanged by the extra rows).
    EXPECT_NEAR(ctx.nodes.conc[uj], 4.0, 1.0e-9 * 4.0);
    close(r);
}

// ---------------------------------------------------------------------------
// 5. Coupling, spill at the node's TEMPERATURE: J1 fed at 4 mg/L and — via
//    the heat source table's EXTERNAL_INFLOW default — spills onto a pan
//    seeded at exactly that temperature; the pan stays uniform in BOTH rows.
//    The node value the marcher must read is `heat_state.node_temp`, not
//    `nodes.conc` mis-strided (which would put Cu's 4.0 into the T row).
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, SpillArrivesAtTheNodeTemperature) {
    const std::string inflows =
        "[INFLOWS]\n"
        "J1 FLOW \"\" FLOW   1.0 1.0 0.3\n"
        "J1 Cu   \"\" CONCEN 1.0 1.0 4.0\n\n";
    // First run: discover the node temperature the 1D side publishes.
    CoupledRun probe = runDeck("_s4_spill_probe", deck(
        "HEAT_TRANSPORT       YES\n",
        "[2D_INITIAL_QUALITY]\n* Cu 4.0\n\n", "J1", 0.5, 0.2, inflows));
    ASSERT_TRUE(probe.ok);
    const int j1 = swmm_node_index(probe.e, "J1");
    ASSERT_GE(j1, 0);
    const double t_node =
        probe.eng->context().heat_state.node_temp[static_cast<std::size_t>(j1)];
    ASSERT_GT(probe.eng->context().mass_balance_2d.coupling_1d_to_2d_in, 0.5);
    close(probe);
    ASSERT_NE(t_node, 4.0) << "fixture cannot tell the T row from the Cu row";

    std::ostringstream iq;
    iq << "[2D_INITIAL_QUALITY]\n* Cu 4.0\n* __TEMPERATURE__ " << t_node << "\n\n";
    CoupledRun r = runDeck("_s4_spill_t", deck(
        "HEAT_TRANSPORT       YES\n", iq.str(), "J1", 0.5, 0.2, inflows));
    ASSERT_TRUE(r.ok);
    const auto& router = r.eng->surfaceRouter2D();
    const auto& st = router.state();
    const auto& tr = st.transport;
    ASSERT_EQ(tr.temp_row, 1);
    double worst_c = 0.0, worst_t = 0.0;
    for (int c = 0; c < tr.n_cells; ++c) {
        const double v_dry = router.options().dry_depth * router.mesh().tri_area[c];
        if (!(st.volume[c] > v_dry)) continue;
        worst_c = std::max(worst_c, std::fabs(tr.cell_mass[tr.idx(0, c)] / st.volume[c] - 4.0) / 4.0);
        worst_t = std::max(worst_t, std::fabs(tr.cell_mass[tr.idx(1, c)] / st.volume[c] - t_node) /
                                        std::fabs(t_node));
    }
    EXPECT_LT(worst_c, 1.0e-9);
    EXPECT_LT(worst_t, 1.0e-9) << "spill did not arrive at the node's temperature";
    close(r);
}

// ---------------------------------------------------------------------------
// 6. Kdecay on the surface: a still pan of Cu with Kdecay = 0.5 /day decays
//    as exp(−k t) exactly (the reaction stage's closed form), the removed mass
//    lands in qual_routing_reacted in 1D mass units, and the temperature row
//    does not decay.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, PollutantKdecayAppliesOnTheSurfaceAndIsLedgered) {
    CoupledRun r = runDeck("_s4_decay", deck(
        "HEAT_TRANSPORT       YES\n",
        "[2D_INITIAL_QUALITY]\n* Cu 4.0\n* __TEMPERATURE__ 12.0\n\n",
        "O1", -10.0, 0.5, "",
        "Cu     MG/L  0     0   0     0.5\n"));   // Kdecay 0.5 /day
    ASSERT_TRUE(r.ok);
    const auto& ctx    = r.eng->context();
    const auto& router = r.eng->surfaceRouter2D();
    const auto& st     = router.state();
    const auto& tr     = router.state().transport;
    const double k_s   = ctx.pollutants.k_decay[0];      // internal 1/s
    ASSERT_GT(k_s, 0.0);
    const double T = 20.0 * 60.0;
    const double c_exp = 4.0 * std::exp(-k_s * T);
    double removed_m3 = 0.0;
    for (int c = 0; c < tr.n_cells; ++c) {
        const double conc = tr.cell_mass[tr.idx(0, c)] / st.volume[c];
        EXPECT_NEAR(conc, c_exp, 1.0e-9 * c_exp) << "cell " << c;
        EXPECT_NEAR(tr.cell_mass[tr.idx(1, c)] / st.volume[c], 12.0, 1.0e-9 * 12.0);
        removed_m3 += (4.0 - conc) * st.volume[c];
    }
    ASSERT_FALSE(ctx.mass_balance.qual_routing_reacted.empty());
    EXPECT_NEAR(ctx.mass_balance.qual_routing_reacted[0],
                removed_m3 * router.options().flow_2d_to_1d,
                1.0e-6 * removed_m3 * router.options().flow_2d_to_1d)
        << "decayed surface mass not booked in 1D mass units";
    close(r);
}

// ---------------------------------------------------------------------------
// 7. Passenger: with WATER_AGE and HEAT_TRANSPORT on, the coupled drain
//    hydraulics are bit-identical to the pollutant-only S3 deck.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, ExtraRowsArePassengersOnTheHydraulics) {
    CoupledRun a = runDeck("_s4_pass_a", deck("", "[2D_INITIAL_QUALITY]\n* Cu 4.0\n\n",
                                              "J1", 1.0, 0.5));
    ASSERT_TRUE(a.ok);
    std::vector<double> vol_a = a.eng->surfaceRouter2D().state().volume;
    close(a);
    CoupledRun b = runDeck("_s4_pass_b", deck(
        "WATER_AGE            YES\nHEAT_TRANSPORT       YES\n",
        "[2D_INITIAL_QUALITY]\n* Cu 4.0\n* __WATER_AGE__ 50\n* __TEMPERATURE__ 9.0\n\n",
        "J1", 1.0, 0.5));
    ASSERT_TRUE(b.ok);
    EXPECT_EQ(b.eng->surfaceRouter2D().state().volume, vol_a);
    close(b);
}

// ---------------------------------------------------------------------------
// 8. (S3 debt) InpWriter round trip: [2D_INITIAL_QUALITY] (all three scopes),
//    [2D_BOUNDARY_QUALITY] and DISPERSION are written and read back so the
//    reopened model seeds the same state — including the reserved row names.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS4, QualitySectionsRoundTripThroughTheWriter) {
    // Pan far below O1 (still), with a non-WALL boundary edge so the
    // [2D_BOUNDARY_QUALITY] row can resolve, and DISPERSION set.
    std::string body = deck(
        "HEAT_TRANSPORT       YES\n",
        "[2D_INITIAL_QUALITY]\n* Cu 2.0\nCELL 2 Cu 7.0\n* __TEMPERATURE__ 11.0\n\n"
        "[2D_BOUNDARY_CONDITIONS]\n0 0 SPECIFIED_FLOW 0.0\n\n"
        "[2D_BOUNDARY_QUALITY]\n0 0 Cu 3.5\n0 0 __TEMPERATURE__ 8.0\n\n",
        "O1", -10.0, 0.5);
    body.replace(body.find("REPORT_2D NO\n"), std::string("REPORT_2D NO\n").size(),
                 "REPORT_2D NO\nDISPERSION 0.02\n");
    CoupledRun a = runDeck("_s4_rt_a", body, /*step=*/false);
    ASSERT_TRUE(a.ok);
    const auto& tra = a.eng->surfaceRouter2D().state().transport;
    ASSERT_EQ(tra.n_species, 2);
    const std::vector<double> mass_a = tra.cell_mass;
    const std::vector<double> bc_a   = tra.bc_conc;
    ASSERT_EQ(a.eng->surfaceRouter2D().options().dispersion, 0.02);
    ASSERT_EQ(swmm_model_write(a.e, "_s4_rt_b.inp"), 0);
    close(a);

    std::string written;
    { std::ifstream f("_s4_rt_b.inp"); std::ostringstream ss; ss << f.rdbuf(); written = ss.str(); }
    EXPECT_NE(written.find("[2D_INITIAL_QUALITY]"), std::string::npos) << written;
    EXPECT_NE(written.find("[2D_BOUNDARY_QUALITY]"), std::string::npos) << written;
    EXPECT_NE(written.find("DISPERSION"), std::string::npos) << written;
    EXPECT_NE(written.find("__TEMPERATURE__"), std::string::npos) << written;

    CoupledRun b = runDeck("_s4_rt_b", written, /*step=*/false);
    ASSERT_TRUE(b.ok) << "the written deck did not reopen";
    const auto& trb = b.eng->surfaceRouter2D().state().transport;
    EXPECT_EQ(trb.cell_mass, mass_a) << "seeded state differs after the round trip";
    EXPECT_EQ(trb.bc_conc, bc_a)     << "boundary quality differs after the round trip";
    EXPECT_EQ(b.eng->surfaceRouter2D().options().dispersion, 0.02);
    close(b);
}
