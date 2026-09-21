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
 * @file test_gw_transport_kernel.cpp
 * @brief T7.1 — the transported tuple in the two-zone groundwater kernel.
 *
 * @details The authoring surface has its own suite
 *          (`test_gw_transport_authoring.cpp`); this one is about what the
 *          kernel DOES with the rows once a `[2D_AQUIFER]` is under them:
 *
 *          1. **It conserves.** The GW plan's gate-6 stub — every channel's
 *             ledger closes, on all-triangle, all-quad and mixed meshes.
 *          2. **The surface seam is a transfer, not a loss.** What
 *             infiltration takes off the surface arrives in the aquifer, and
 *             what saturation excess pushes up arrives back on the surface.
 *          3. **ET up-concentrates.** It carries water, age and temperature
 *             and leaves every solute behind.
 *          4. **The matrix tells the truth.** The groundwater row is real
 *             now, and the "authored but inert" warning fires only where
 *             there is genuinely no kernel.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_hotstart.h>   // T7.5

#include <hdf5.h>

#include "core/SWMMEngine.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "transport/TransportPolicy.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_GW_TEST_OUT_DIR) / "gw_transport_kernel";

/// Mesh flavours — the cell-generic claim is only worth anything if the
/// gate actually runs on a quad and on a mixed mesh.
enum class Mesh { Tri, Quad, Mixed };

/**
 * @brief A 10 m pan over one junction with an aquifer under it, a pollutant
 *        seeded in both zones, rain on the mesh and infiltration running.
 *
 * The deck is deliberately busy: rain arrives, infiltrates into the column,
 * recharges the table, drains deep, exchanges with J1's bed and comes back
 * up as saturation excess — so the ledger has something in every row it can
 * reach.
 */
std::string deck(Mesh m, const std::string& extra_gw = "",
                 double hg0 = 0.5, double deep = 0.0) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[POLLUTANTS]\n;;Name Units Rain GW IIflow Kdecay\n"
         "TSS  MG/L  10.0  0.0  0.0  0.0  NO  *  0.0  0.0\n\n"
         "[JUNCTIONS]\nJ1 -2.0 4.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[RAINGAGES]\nRG1 INTENSITY 1:00 1.0 TIMESERIES RAIN\n\n"
         "[TIMESERIES]\nRAIN  0:00  20.0\nRAIN  0:10  0.0\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\nRAINFALL_MODE SYSTEM\n"
         "EVAPORATION CLIMATE\n\n";
    // Ground at 0 m; the aquifer bottom is ZS below it.
    if (m == Mesh::Tri) {
        s << "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
             "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
             "0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n";
    } else if (m == Mesh::Quad) {
        s << "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
             "[2D_QUADS]\n;;V1 V2 V3 V4 N INIT_DEPTH\n"
             "0 1 2 3 0.03 0.0\n\n";
    } else {
        // A quad and two triangles sharing an edge — the mixed case the
        // cell-generic kernel exists for.
        // Cells are numbered triangles first, then quads — the parser
        // requires the sections in that order too.
        s << "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n"
             "20.0 0.0 0.0\n20.0 10.0 0.0\n\n"
             "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
             "1 4 5 0.03 0.0\n1 5 2 0.03 0.0\n\n"
             "[2D_QUADS]\n;;V1 V2 V3 V4 N INIT_DEPTH\n"
             "0 1 2 3 0.03 0.0\n\n";
    }
    s << "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  40.0  -  -  -  -\n\n"
         "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n\n"
         "[2D_AQUIFER]\n;;Scope KS ZS THETA_S THETA_R ALPHA\n"
         "*  36.0  1.0  0.45  0.10  2.0  HG0 " << hg0;
    if (deep > 0.0) s << "  C_LOSS " << deep;
    // Cell 1 starts with its table nearly at the ground: it saturates first,
    // which gives the gate a Dunne channel AND a lateral head gradient to
    // its neighbour — the two channels with their own accumulators.
    s << "\n"
         "CELL 1  36.0  1.0  0.45  0.10  2.0  HG0 0.95\n\n"
         "[GW_INITIAL_QUALITY]\n;;Scope Zone Species Value\n"
         "*       SAT    TSS  5.0\n"
         "*       UNSAT  TSS  2.0\n"
         "CELL 1  SAT    TSS  25.0\n\n"
      << extra_gw
      << "[COORDINATES]\nJ1  5.0  5.0\nO1  40.0  40.0\n\n"
         "[REPORT]\nINPUT NO\n";
    return s.str();
}

struct Deck {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    bool opened = false, started = false;
};

Deck open(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    Deck r;
    const fs::path inp = kOutDir / (tag + ".inp");
    { std::ofstream f(inp); f << body; }
    r.e = swmm_engine_create();
    if (!r.e) return r;
    r.opened = swmm_engine_open(r.e, inp.string().c_str(),
                                (kOutDir / (tag + ".rpt")).string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(),
                                nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

bool run(Deck& r) {
    if (!r.opened) return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK) return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK) return false;
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    return true;
}

void finish(Deck& r) {
    if (!r.e) return;
    if (r.started) swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

std::string ledgerDump(const openswmm::twoD::SubsurfaceTransportState& t, int s) {
    const auto u = static_cast<std::size_t>(s);
    std::ostringstream o;
    o << "\n  gw species ledger (" << (u < t.row_names.size() ? t.row_names[u] : "?")
      << "):\n    init      " << t.init_mass[u]
      << "\n    storage   " << t.ledgeredStorage(s)
      << "\n    +infil    " << t.gained_infil[u]
      << "\n    +node     " << t.gained_node[u]
      << "\n    +link     " << t.gained_link[u]
      << "\n    +lateral  " << t.net_lateral[u]
      << "\n    -deep     " << t.lost_deep[u]
      << "\n    -node     " << t.lost_node[u]
      << "\n    -link     " << t.lost_link[u]
      << "\n    -dunne    " << t.lost_dunne[u]
      << "\n    -et       " << t.lost_et[u]
      << "\n    (recharge, internal) " << t.internal_recharge[u]
      << "\n    residual  " << t.residual(s) << "\n";
    return o.str();
}

const char* meshName(Mesh m) {
    return m == Mesh::Tri ? "tri" : (m == Mesh::Quad ? "quad" : "mixed");
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 6 stub — every channel's ledger closes, on every cell shape.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, TupleConservesOnEveryMeshFlavour) {
    for (const Mesh m : {Mesh::Tri, Mesh::Quad, Mesh::Mixed}) {
        const std::string tag = std::string("conserve_") + meshName(m);
        // C_LOSS gives the deep channel something to carry, so the gate is
        // not just testing the channels that happen to be busy.
        Deck r = open(tag, deck(m, "", /*hg0=*/0.5, /*deep=*/1.0e-6));
        ASSERT_TRUE(r.opened) << tag;
        ASSERT_TRUE(run(r)) << tag;
        const auto& gw = r.eng->surfaceRouter2D().subsurface();
        const auto& t  = gw.transport();
        ASSERT_TRUE(t.active()) << tag << ": the aquifer is not transporting";
        ASSERT_GT(t.n_species, 0) << tag;

        for (int s = 0; s < t.n_species; ++s) {
            const auto us = static_cast<std::size_t>(s);
            const double scale = std::max({std::fabs(t.init_mass[us]),
                                           std::fabs(t.ledgeredStorage(s)),
                                           std::fabs(t.gained_infil[us]), 1.0e-30});
            EXPECT_LT(std::fabs(t.residual(s)), 1.0e-10 * scale)
                << tag << " species " << s << ledgerDump(t, s);
        }
        // …and the run actually exercised the channels: mass came down from
        // the surface and moved between the zones.
        EXPECT_GT(t.gained_infil[0], 0.0) << tag << ": nothing infiltrated" << ledgerDump(t, 0);
        EXPECT_NE(t.internal_recharge[0], 0.0) << tag << ": the table never recharged";
        EXPECT_GT(t.init_mass[0], 0.0) << tag << ": [GW_INITIAL_QUALITY] seeded nothing";
        finish(r);
    }
}

// ---------------------------------------------------------------------------
// The surface seam is a TRANSFER: what leaves the surface arrives here.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, InfiltrationHandsMassToTheAquiferAndExcessBringsItBack) {
    Deck r = open("seam", deck(Mesh::Tri));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& gw  = r.eng->surfaceRouter2D().subsurface();
    const auto& t   = gw.transport();
    const auto& srf = r.eng->surfaceRouter2D().state().transport;
    ASSERT_TRUE(t.active());
    ASSERT_TRUE(srf.active());
    // Row 0 is TSS in both domains — the same TransportPolicy layout, which
    // is what makes the seam one number rather than a mapping.
    ASSERT_EQ(t.row_names[0], srf.row_names[0]);

    // The surface has rain-borne TSS only if [POLLUTANTS] gives rain a
    // concentration; here it does not, so the mass that goes DOWN is
    // whatever the surface was carrying — zero — and the interesting
    // direction is the one coming UP out of a seeded aquifer.
    EXPECT_NEAR(srf.lost_infiltration[0], t.gained_infil[0], 1.0e-12)
        << "the surface lost " << srf.lost_infiltration[0]
        << " but the aquifer gained " << t.gained_infil[0];
    // …less whatever is still in flight: `lost_dunne` is booked when the GW
    // cell PUSHES and `gained_exfiltration` when the surface DRAINS, so at
    // the end of a run the two differ by exactly what is sitting in the
    // accumulator — the same one-firing lag the water has.
    double pending = 0.0;
    for (int c = 0; c < t.n_cells; ++c)
        pending += t.xacc_to_surface[t.idx(0, c)];
    EXPECT_NEAR(srf.gained_exfiltration[0] + pending, t.lost_dunne[0], 1.0e-12)
        << "saturation excess carried " << t.lost_dunne[0]
        << " up, the surface received " << srf.gained_exfiltration[0]
        << " and " << pending << " is still in flight";
    EXPECT_GT(t.lost_dunne[0], 0.0)
        << "no saturation excess returned — the seam's upward half is untested"
        << ledgerDump(t, 0);
    EXPECT_LT(std::fabs(t.residual(0)), 1.0e-10 * std::fabs(t.init_mass[0]))
        << ledgerDump(t, 0);
    finish(r);
}

// ---------------------------------------------------------------------------
// ET up-concentrates: it carries water (and the intensive rows) and leaves
// every solute behind.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, SubsurfaceEtLeavesTheSolutesBehind) {
    // GW_ET BOUNDARY_ET with a real evaporation rate.
    std::string body = deck(Mesh::Tri, "", /*hg0=*/0.5);
    const std::string anchor = "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n";
    const auto at = body.find(anchor);
    ASSERT_NE(at, std::string::npos);
    body.insert(at + anchor.size(), "GW_ET BOUNDARY_ET\n");
    const std::string ev = "[EVAPORATION]\nCONSTANT  5.0\nDRY_ONLY  NO\n\n";
    body.insert(body.find("[2D_OPTIONS]"), ev);

    Deck r = open("et", body);
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
    ASSERT_TRUE(t.active());
    const double gw_et = r.eng->surfaceRouter2D().subsurface().state().led_et;
    EXPECT_GT(gw_et, 0.0) << "the deck did not evaporate from the column";
    // TSS is row 0 and is a solute: ET took none of it.
    EXPECT_EQ(t.lost_et[0], 0.0)
        << "ET carried solute out of the aquifer" << ledgerDump(t, 0);
    EXPECT_LT(std::fabs(t.residual(0)), 1.0e-10 * std::fabs(t.init_mass[0]))
        << ledgerDump(t, 0);
    finish(r);
}

// ---------------------------------------------------------------------------
// The transport matrix, and the warning that used to be unconditional.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, MatrixReportsTheGroundwaterRowAndTheWarningIsScoped) {
    using openswmm::transport::Domain;
    using openswmm::transport::SpeciesClass;
    using openswmm::transport::CellState;
    {
        Deck r = open("matrix_live", deck(Mesh::Tri));
        ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        const auto m = openswmm::transport::resolve(r.eng->context());
        EXPECT_EQ(m.at(Domain::GROUNDWATER, SpeciesClass::POLLUTANTS).state,
                  CellState::ENABLED)
            << "reason: " << m.at(Domain::GROUNDWATER, SpeciesClass::POLLUTANTS).reason;
        EXPECT_EQ(m.at(Domain::GROUNDWATER, SpeciesClass::POLLUTANTS).count, 1);
        bool inert = false;
        for (const auto& w : r.eng->context().warnings)
            if (w.find("AUTHORED but INERT") != std::string::npos) inert = true;
        EXPECT_FALSE(inert) << "the inert warning fired on a deck WITH a kernel";
        finish(r);
    }
    {
        // The same [GW_*] rows with no [2D_AQUIFER] under them: still inert,
        // and still said so.
        std::string body = deck(Mesh::Tri);
        const auto a = body.find("[2D_AQUIFER_OPTIONS]");
        const auto b = body.find("[GW_INITIAL_QUALITY]");
        ASSERT_NE(a, std::string::npos);
        ASSERT_NE(b, std::string::npos);
        body.erase(a, b - a);   // drop the aquifer, keep the [GW_*] rows
        Deck r = open("matrix_inert", body);
        ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        const auto m = openswmm::transport::resolve(r.eng->context());
        EXPECT_EQ(m.at(Domain::GROUNDWATER, SpeciesClass::POLLUTANTS).state,
                  CellState::UNAVAILABLE);
        bool inert = false;
        for (const auto& w : r.eng->context().warnings)
            if (w.find("AUTHORED but INERT") != std::string::npos) inert = true;
        EXPECT_TRUE(inert) << "a deck with no kernel was not told its rows are inert";
        finish(r);
    }
}

// ---------------------------------------------------------------------------
// T7.2 — dispersion, retardation and decay.
//
// The deck below seeds cell 1 at 25 mg/L against 5 mg/L everywhere else and
// lets the two neighbours exchange: with dispersion ON the gap closes, and
// the max principle says neither cell may ever pass the other's start.
// ---------------------------------------------------------------------------
namespace {

struct T72 {
    double sat_c0 = 0.0, sat_c1 = 0.0;   ///< end-of-run saturated concentrations
    double stored = 0.0, reacted = 0.0, residual = 0.0, init = 0.0;
    double exported = 0.0;   ///< everything that LEFT the aquifer as solute
    long   binds = 0;
};

/// @param extra rows appended to the `[GW_*]` block (sorption, params…)
/// @param dispersion the `[GW_TRANSPORT_OPTIONS] DISPERSION` switch
T72 runT72(const std::string& tag, const std::string& extra,
           bool dispersion = true) {
    std::ostringstream gw;
    gw << "[GW_TRANSPORT_OPTIONS]\nDISPERSION " << (dispersion ? "YES" : "NO")
       << "\n\n" << extra;
    Deck r = open(tag, deck(Mesh::Tri, gw.str()));
    T72 o;
    EXPECT_TRUE(r.opened) << tag;
    if (!r.opened) return o;
    EXPECT_TRUE(run(r)) << tag;
    const auto& gws = r.eng->surfaceRouter2D().subsurface();
    const auto& t   = gws.transport();
    EXPECT_TRUE(t.active()) << tag;
    if (!t.active()) { finish(r); return o; }
    const double v0 = gws.satVolume(0), v1 = gws.satVolume(1);
    o.sat_c0 = (v0 > 0.0) ? t.sat_mass[t.idx(0, 0)] / v0 : 0.0;
    o.sat_c1 = (v1 > 0.0) ? t.sat_mass[t.idx(0, 1)] / v1 : 0.0;
    o.stored = t.ledgeredStorage(0);
    o.exported = t.lost_deep[0] + t.lost_node[0] + t.lost_link[0] + t.lost_dunne[0];
    o.reacted = t.lost_reaction[0];
    o.residual = t.residual(0);
    o.init = t.init_mass[0];
    o.binds = t.dispersion_limiter_binds;
    finish(r);
    return o;
}

}  // namespace

TEST(GwTransportKernel, DispersionMixesTheNeighboursAndRespectsTheMaxPrinciple) {
    // A big dispersivity, so the term is unmistakably active.
    const std::string params =
        "[GW_TRANSPORT_PARAMS]\n;;Scope rho_s c_s lambda_s a_s alpha_L alpha_T D_m D_v geo\n"
        "*  2650  880  2.0  0.0  50.0  5.0  1.0e-9  1.0e-9  0.065\n\n";
    const T72 on  = runT72("disp_on",  params, /*dispersion=*/true);
    const T72 off = runT72("disp_off", params, /*dispersion=*/false);

    // Cell 1 starts rich (25 mg/L) and cell 0 lean (5 mg/L). Dispersion
    // narrows that gap; without it only advection mixes them.
    const double gap_on  = std::fabs(on.sat_c1  - on.sat_c0);
    const double gap_off = std::fabs(off.sat_c1 - off.sat_c0);
    EXPECT_LT(gap_on, gap_off)
        << "dispersion did not mix: gap " << gap_on << " with it, "
        << gap_off << " without";
    // The max principle, pairwise: no cell may end outside the range the
    // two started in. 25 and 5 are the seeded saturated concentrations;
    // infiltration brings 10 mg/L rain water in, so the upper bound is the
    // seeded maximum and the lower bound is the smallest source.
    EXPECT_LE(on.sat_c0, 25.0 + 1.0e-9) << "cell 0 ended above every source";
    EXPECT_LE(on.sat_c1, 25.0 + 1.0e-9) << "cell 1 ended above every source";
    EXPECT_GE(on.sat_c0, 0.0);
    EXPECT_GE(on.sat_c1, 0.0);
    // …and it conserves: dispersion is an exchange, not a source.
    EXPECT_LT(std::fabs(on.residual), 1.0e-10 * std::fabs(on.init))
        << "residual " << on.residual;
}

TEST(GwTransportKernel, RetardationHoldsMassBackAndStillConserves) {
    const std::string params =
        "[GW_TRANSPORT_PARAMS]\n"
        "*  2650  880  2.0  0.0  50.0  5.0  1.0e-9  1.0e-9  0.065\n\n";
    // K_d = 2 L/kg over ρ_b ≈ 2650(1 − 0.45) ≈ 1458 kg/m³ at θ = 0.45 gives
    // R ≈ 1 + 1458·0.002/0.45 ≈ 7.5: most of the mass is on the grains and
    // only a seventh of it travels.
    const std::string sorb = params +
        "[GW_SORPTION]\n;;Scope Species Kd Decay\n*  TSS  2.0  -\n\n";
    const T72 plain = runT72("retard_off", params);
    const T72 held  = runT72("retard_on",  sorb);

    // The defining property, and the one that does not depend on which way
    // a particular cell's net flux happens to run: every EXIT from the
    // aquifer carries dissolved solute only, so sorbing most of the mass on
    // the grains must leave less of it able to go anywhere. (Asserting that
    // a chosen cell ends richer is NOT that property — on this deck the
    // rich cell is a net importer, and retardation throttles what reaches
    // it slightly more than what leaves it.)
    EXPECT_LT(held.exported, plain.exported)
        << "retarded export " << held.exported << " vs free " << plain.exported;
    EXPECT_GT(held.stored, plain.stored)
        << "…and what did not leave must still be here: " << held.stored
        << " vs " << plain.stored;
    // Sorption moves nothing out of the aquifer, so the books are untouched.
    EXPECT_EQ(held.reacted, 0.0) << "K_d alone should not remove mass";
    EXPECT_LT(std::fabs(held.residual), 1.0e-10 * std::fabs(held.init))
        << "residual " << held.residual;
}

TEST(GwTransportKernel, DecayRemovesTheAnalyticFractionAndIsLedgered) {
    const std::string params =
        "[GW_TRANSPORT_PARAMS]\n"
        "*  2650  880  2.0  0.0  1.0  0.1  1.0e-9  1.0e-9  0.065\n\n";
    // 10 /day over the deck's 20 minutes: exp(−10 × 20/1440) ≈ 0.871, so
    // ~13 % of whatever the aquifer held should be gone.
    const std::string decay = params +
        "[GW_SORPTION]\n;;Scope Species Kd Decay\n*  TSS  0.0  10.0\n\n";
    const T72 without = runT72("decay_off", params);
    const T72 with    = runT72("decay_on",  decay);

    EXPECT_GT(with.reacted, 0.0) << "nothing decayed";
    EXPECT_EQ(without.reacted, 0.0) << "mass decayed with no DECAY authored";
    EXPECT_LT(with.stored, without.stored) << "decay left the storage untouched";
    // The ledger is the whole difference: what decay removed is exactly what
    // the two runs' storage differs by, net of what left by other channels.
    EXPECT_LT(std::fabs(with.residual), 1.0e-10 * std::fabs(with.init))
        << "residual " << with.residual << " — decay is ledgered as an outflow";
    // The fraction is the analytic one, to the accuracy of an explicit
    // kernel that applies it per firing: a 20-minute run at 10 /day.
    const double expect_frac = 1.0 - std::exp(-10.0 * (20.0 / 1440.0));
    const double got_frac = with.reacted / std::max(with.init, 1.0e-30);
    EXPECT_NEAR(got_frac, expect_frac, 0.25 * expect_frac)
        << "decayed " << got_frac << " of the initial mass, analytic "
        << expect_frac;
}

// ---------------------------------------------------------------------------
// T7.5 — the tuple becomes visible and restartable.
//
// A state nobody can read and a restart cannot keep is not finished work,
// so these gates check the three surfaces against the kernel's own numbers
// rather than against each other: the C API, the `.h5`, the `.rpt`, and a
// hotstart round trip.
// ---------------------------------------------------------------------------

TEST(GwTransportKernel, CApiReportsTheSpeciesRowsConcentrationsAndLedger) {
    Deck r = open("api_species", deck(Mesh::Tri));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& gw = r.eng->surfaceRouter2D().subsurface();
    const auto& t  = gw.transport();

    int n = -1;
    ASSERT_EQ(swmm_gw2d_species_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, t.n_species);
    ASSERT_GT(n, 0);
    char nm[64] = {0};
    ASSERT_EQ(swmm_gw2d_species_name(r.e, 0, nm, sizeof nm), SWMM_OK);
    EXPECT_STREQ(nm, t.row_names[0].c_str());
    EXPECT_NE(swmm_gw2d_species_name(r.e, n, nm, sizeof nm), SWMM_OK)
        << "an out-of-range row was accepted";

    // Concentrations are mass over the ZONE's water volume, and the API
    // must agree with the state it is reporting.
    std::vector<double> c(static_cast<std::size_t>(t.n_cells), -1.0);
    int written = 0;
    ASSERT_EQ(swmm_gw2d_get_cell_conc(r.e, SWMM_GW2D_ZONE_SAT, 0, c.data(),
                                      static_cast<int>(c.size()), &written),
              SWMM_OK);
    EXPECT_EQ(written, t.n_cells);
    for (int i = 0; i < t.n_cells; ++i) {
        const double v = gw.satVolume(i);
        const double want = (v > 0.0) ? t.sat_mass[t.idx(0, i)] / v : 0.0;
        EXPECT_NEAR(c[static_cast<std::size_t>(i)], want, 1.0e-12) << "cell " << i;
        EXPECT_GT(want, 0.0) << "cell " << i << " carries nothing to report";
    }
    ASSERT_EQ(swmm_gw2d_get_cell_conc(r.e, SWMM_GW2D_ZONE_UNSAT, 0, c.data(),
                                      static_cast<int>(c.size()), &written),
              SWMM_OK);
    EXPECT_NE(swmm_gw2d_get_cell_conc(r.e, 42, 0, c.data(),
                                      static_cast<int>(c.size()), &written),
              SWMM_OK) << "an unknown zone was accepted";

    // …and the ledger terms are the kernel's own.
    double v = 0.0;
    ASSERT_EQ(swmm_gw2d_get_species_ledger(r.e, 0, SWMM_GW2D_SPL_INFIL_IN, &v), SWMM_OK);
    EXPECT_NEAR(v, t.gained_infil[0], 1.0e-12);
    ASSERT_EQ(swmm_gw2d_get_species_ledger(r.e, 0, SWMM_GW2D_SPL_RESIDUAL, &v), SWMM_OK);
    EXPECT_LT(std::fabs(v), 1.0e-10 * std::fabs(t.init_mass[0]));
    EXPECT_NE(swmm_gw2d_get_species_ledger(r.e, 0, 99, &v), SWMM_OK);
    finish(r);
}

TEST(GwTransportKernel, ReportCarriesTheAquiferQualityContinuity) {
    Deck r = open("rpt_species", deck(Mesh::Tri));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    // The continuity blocks are written by `report()`, not by `end()` — the
    // CLI calls both and so must anything that reads the .rpt (without it
    // the plugin's destructor stamps "[Report interrupted]" instead).
    ASSERT_EQ(swmm_engine_end(r.e), SWMM_OK);
    r.started = false;
    ASSERT_EQ(swmm_engine_report(r.e), SWMM_OK);
    finish(r);
    const std::string rpt = [&] {
        std::ifstream f(kOutDir / "rpt_species.rpt");
        std::ostringstream ss; ss << f.rdbuf(); return ss.str();
    }();
    EXPECT_NE(rpt.find("2D Aquifer Quality Continuity"), std::string::npos)
        << "the species continuity block is missing from the report";
    EXPECT_NE(rpt.find("Initial Stored Mass"), std::string::npos);
    EXPECT_NE(rpt.find("Saturation Excess Return"), std::string::npos);
    // The block is per species and names the row it is reporting.
    EXPECT_NE(rpt.find("TSS"), std::string::npos);
    // …and the water block is still there beside it.
    EXPECT_NE(rpt.find("2D Aquifer Continuity"), std::string::npos);
}

TEST(GwTransportKernel, HotStartCarriesTheAquiferSpeciesAcrossARestart) {
    const fs::path hsf = kOutDir / "gw_species.hsf";
    fs::remove(hsf);

    // `swmm_hotstart_save`, not `[FILES] SAVE HOTSTART`: the latter writes
    // the LEGACY SWMM5 routing format, which has no versioned blocks at all
    // (and so no aquifer, and no species). The native writer is the one that
    // carries V5's water table and V6's tuple.
    double sat0 = 0.0, uns0 = 0.0, infil0 = 0.0;
    {
        Deck r = open("hs_write", deck(Mesh::Tri));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
        sat0   = t.sat_mass[t.idx(0, 0)];
        uns0   = t.unsat_mass[t.idx(0, 0)];
        infil0 = t.gained_infil[0];
        ASSERT_EQ(swmm_hotstart_save(r.e, hsf.string().c_str()), SWMM_OK);
        finish(r);
    }
    ASSERT_TRUE(fs::exists(hsf)) << "no hotstart file was written";
    EXPECT_GT(sat0, 0.0);
    EXPECT_GT(infil0, 0.0);

    // A second model reads it and must RESUME — not restart from the
    // [GW_INITIAL_QUALITY] seed, which for cell 0's saturated zone is a
    // different (and larger) number.
    Deck r2 = open("hs_read", deck(Mesh::Tri));
    ASSERT_TRUE(r2.opened);
    ASSERT_EQ(swmm_engine_initialize(r2.e), SWMM_OK);
    // apply() wants SWMM_STATE_INITIALIZED — before start(), which is also
    // the only point at which resuming a state means anything.
    SWMM_HotStart hs = nullptr;
    ASSERT_EQ(swmm_hotstart_open(hsf.string().c_str(), &hs), SWMM_OK);
    ASSERT_EQ(swmm_hotstart_apply(r2.e, hs), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r2.e, 1), SWMM_OK);
    r2.started = true;
    const auto& t2 = r2.eng->surfaceRouter2D().subsurface().transport();
    EXPECT_NEAR(t2.sat_mass[t2.idx(0, 0)], sat0, 1.0e-9 * std::fabs(sat0))
        << "the restart did not resume the saturated store";
    EXPECT_NEAR(t2.unsat_mass[t2.idx(0, 0)], uns0, 1.0e-9 * std::fabs(uns0) + 1.0e-12)
        << "the restart did not resume the column store";
    // The ledger continues rather than restarting at zero, so the restarted
    // run's continuity statement is a continuation of the original's.
    EXPECT_NEAR(t2.gained_infil[0], infil0, 1.0e-9 * std::fabs(infil0) + 1.0e-12);
    swmm_hotstart_close(hs);
    finish(r2);
}

// ---------------------------------------------------------------------------
// T7.5 — the results file carries the tuple.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, ResultsFileCarriesTheSpeciesFieldsAndLedger) {
    // REPORT_2D YES with the GROUNDWATER variable: the species datasets ride
    // the same mask the water fields do. FLOAT64 so the comparison tests the
    // PLUMBING rather than float32's last digit — the default precision
    // agrees to ~6e-9 relative, which is the storage class, not the writer.
    std::string body = deck(Mesh::Tri);
    const std::string old_rep = "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n";
    const auto at = body.find(old_rep);
    ASSERT_NE(at, std::string::npos);
    body.replace(at, old_rep.size(),
                 "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D YES\n"
                 "OUTPUT_FILE h5_species.h5\nOUTPUT_PRECISION FLOAT64\n"
                 "REPORT_2D_VARIABLES DEPTH GROUNDWATER\n");
    Deck r = open("h5_species", body);
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
    const int ns = t.n_species;
    const int nc = t.n_cells;
    std::vector<double> want_sat(static_cast<std::size_t>(nc), 0.0);
    {   // the concentrations the file should hold at the last record
        const auto& gw = r.eng->surfaceRouter2D().subsurface();
        for (int c = 0; c < nc; ++c) {
            const double v = gw.satVolume(c);
            want_sat[static_cast<std::size_t>(c)] =
                (v > 0.0) ? t.sat_mass[t.idx(0, c)] / v : 0.0;
        }
    }
    const double want_resid = t.residual(0);
    const double want_infil = t.gained_infil[0];
    finish(r);

    const fs::path h5 = kOutDir / "h5_species.h5";
    ASSERT_TRUE(fs::exists(h5)) << "no results file was written";
    hid_t fid = H5Fopen(h5.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    ASSERT_GE(fid, 0);
    auto has = [&](const char* n) { return H5Lexists(fid, n, H5P_DEFAULT) > 0; };
    EXPECT_TRUE(has("Mesh2_face_gw_sat_conc"));
    EXPECT_TRUE(has("Mesh2_face_gw_unsat_conc"));
    EXPECT_TRUE(has("groundwater_species_ledger"));

    auto readAll = [&](const char* n) {
        hid_t ds = H5Dopen2(fid, n, H5P_DEFAULT);
        hid_t sp = H5Dget_space(ds);
        const int rank = H5Sget_simple_extent_ndims(sp);
        std::vector<hsize_t> d(static_cast<std::size_t>(rank));
        H5Sget_simple_extent_dims(sp, d.data(), nullptr);
        hsize_t total = 1;
        for (auto x : d) total *= x;
        std::vector<double> v(static_cast<std::size_t>(total));
        H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, v.data());
        H5Sclose(sp); H5Dclose(ds);
        return std::make_pair(v, d);
    };
    // [time, species, face] — the last record must be the state the run
    // ended in, cell for cell.
    const auto sat = readAll("Mesh2_face_gw_sat_conc");
    ASSERT_EQ(sat.second.size(), 3u);
    EXPECT_EQ(static_cast<int>(sat.second[1]), ns);
    EXPECT_EQ(static_cast<int>(sat.second[2]), nc);
    ASSERT_GT(sat.second[0], 0u);
    const std::size_t last = (static_cast<std::size_t>(sat.second[0]) - 1) *
                             static_cast<std::size_t>(ns) *
                             static_cast<std::size_t>(nc);
    for (int c = 0; c < nc; ++c)
        EXPECT_NEAR(sat.first[last + static_cast<std::size_t>(c)],
                    want_sat[static_cast<std::size_t>(c)], 1.0e-9)
            << "cell " << c << " differs between the file and the kernel";

    // …and the ledger's last record, including the residual the writer
    // derives rather than stores.
    const auto led = readAll("groundwater_species_ledger");
    ASSERT_EQ(led.second.size(), 3u);
    EXPECT_EQ(static_cast<int>(led.second[2]), 13);
    const std::size_t ll = (static_cast<std::size_t>(led.second[0]) - 1) *
                           static_cast<std::size_t>(ns) * 13;
    EXPECT_NEAR(led.first[ll + 2], want_infil, 1.0e-9) << "infil_in term";
    EXPECT_NEAR(led.first[ll + 12], want_resid, 1.0e-12) << "residual term";
    H5Fclose(fid);
}
