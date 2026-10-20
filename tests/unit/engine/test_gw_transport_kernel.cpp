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
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_reactions.h>
#include <openswmm/engine/openswmm_hotstart.h>   // T7.5

#include <hdf5.h>

#include "core/SWMMEngine.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/subsurface/GroundwaterDispersion.hpp"
#include "2d/subsurface/SubsurfaceSections.hpp"
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
         // Interval <= the series' 10-min spacing (legacy ERROR 159).
         "[RAINGAGES]\nRG1 INTENSITY 0:10 1.0 TIMESERIES RAIN\n\n"
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
         "CELL 1  36.0  1.0  0.45  0.10  2.0  HG0 0.95\n\n";
    s << "[GW_INITIAL_QUALITY]\n;;Scope Zone Species Value\n"
         "*       SAT    TSS  5.0\n"
         "*       UNSAT  TSS  2.0\n"
         "CELL 1  SAT    TSS  25.0\n\n";
    s << extra_gw
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
    int status=SWMM_OK;
    while ((status=swmm_engine_step(r.e, &elapsed)) == SWMM_OK && elapsed > 0.0) {}
    return status == SWMM_OK;
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
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e,1),SWMM_OK);r.started=true;
    // Independent upward forcing: surface receiving capacity now prevents
    // the old forced-infiltration/refund loop from generating this channel.
    r.eng->surfaceRouter2D().subsurface().bookLinkSeepage(0,10.0);
    double elapsed=0.0;
    while(swmm_engine_step(r.e,&elapsed)==SWMM_OK&&elapsed>0.0){}
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
    double pending_down=0.0;
    for(int c=0;c<t.n_cells;++c)pending_down+=t.xacc_from_surface[t.idx(0,c)];
    EXPECT_NEAR(srf.lost_infiltration[0], t.gained_infil[0] + pending_down, 1.0e-12)
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
// ---------------------------------------------------------------------------
// D-A20 — ET carries the AGE row out with the water.
//
// This behaviour was implemented in T7.1 and, until now, gated by nothing:
// `SubsurfaceEtLeavesTheSolutesBehind` asserts only that the SOLUTE row loses
// nothing. So the one thing D-A20 specified for this kernel was the one thing
// no test covered — and reverting it to the GW transport plan's stale §3.5
// text ("ET removes water not age-volume") would have left every gate green.
//
// D-A20: water leaves at the parcel's current age, so the mean age of what
// remains is unchanged — age-volume sunk proportionally, exactly as
// temperature is. Solutes still stay and the column up-concentrates.
//
// **Why this is bounded rather than an exact identity.** The exact statement
// is per firing: `Δlost_et = q_et·dt · (unsat age-volume / unsat water)`, the
// column's mean age at that instant. The cumulative version is an integral of
// a quantity that is not reconstructible from end-of-run state, so an
// `EXPECT_NEAR` against a closed form is not available without instrumenting
// every firing. What IS rigorous: age accrues at `+V·dt`, so a column's mean
// age can never exceed the elapsed simulated time. That bounds the ratio, and
// the LOWER bound is the whole discriminator — the stale specification
// requires exactly zero.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, EtCarriesTheAgeRowOutWithTheWaterD_A20) {
    std::string body = deck(Mesh::Tri, "", /*hg0=*/0.5);
    const std::string anchor =
        "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n";
    const auto at = body.find(anchor);
    ASSERT_NE(at, std::string::npos);
    body.insert(at + anchor.size(), "GW_ET BOUNDARY_ET\n");
    body.insert(body.find("[2D_OPTIONS]"),
                "[EVAPORATION]\nCONSTANT  5.0\nDRY_ONLY  NO\n\n");
    // The age row has to exist for this to test anything.
    // The age row needs TWO switches and the GW one is only a MASK:
    // `TransportPolicy` computes `e.age = ta && ctx.options.water_age`, so
    // `[GW_TRANSPORT_OPTIONS] TRANSPORT_AGE YES` alone leaves `age_row == -1`.
    // The project-level `[OPTIONS] WATER_AGE ON` is what creates the row.
    // (The first version of this gate set only the GW flag and was caught by
    // its own `ASSERT_GE(age_row, 0)` rather than passing vacuously — which
    // is the whole reason that assertion is there.)
    body.insert(body.find("[POLLUTANTS]"), "[OPTIONS]\nWATER_AGE  ON\n\n");
    // Both GW rows named explicitly: assertion (4) needs the solute row to
    // survive, and relying on what a partially-specified
    // [GW_TRANSPORT_OPTIONS] defaults to would make this gate depend on a
    // policy default rather than on the behaviour under test.
    body.insert(body.find("[2D_OPTIONS]"),
                "[GW_TRANSPORT_OPTIONS]\n"
                "TRANSPORT_POLLUTANTS  YES\n"
                "TRANSPORT_AGE         YES\n\n");

    Deck r = open("et_age", body);
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& gw = r.eng->surfaceRouter2D().subsurface();
    const auto& t  = gw.transport();
    ASSERT_TRUE(t.active());
    ASSERT_GE(t.age_row, 0)
        << "no __WATER_AGE__ row, so this gate is measuring nothing";
    const auto a = static_cast<std::size_t>(t.age_row);

    const double et_vol = gw.state().led_et;          // m³ of water ET removed
    ASSERT_GT(et_vol, 0.0) << "the deck did not evaporate from the column";

    // (1) The discriminator. D-A20 says this is positive; the GW transport
    //     plan's stale §3.5 ET row says it is exactly zero.
    EXPECT_GT(t.lost_et[a], 0.0)
        << "ET removed " << et_vol << " m³ of water and no age-volume with it."
           " That is the superseded convention (GW plan §2.4 / §3.5), under"
           " which the mean age of the remaining water rises. D-A20 amends"
           " both sections: age leaves at the supplying layer's age."
        << ledgerDump(t, t.age_row);

    // (2) …and it cannot exceed what elapsed time allows. The deck runs
    //     20 minutes and the aquifer's age row starts at 0 (no
    //     [GW_INITIAL_QUALITY] __WATER_AGE__ seed), so no parcel can be older
    //     than the run. A ratio above that means a unit error or a
    //     double-booking, not physics.
    constexpr double kRunSeconds = 20.0 * 60.0;
    EXPECT_LE(t.lost_et[a], et_vol * kRunSeconds)
        << "the age-volume ET carried out implies a mean age of "
        << (t.lost_et[a] / et_vol) << " s in a " << kRunSeconds
        << " s run" << ledgerDump(t, t.age_row);

    // (3) …and the age row's own books still close, so (1) is a transfer and
    //     not an invention.
    EXPECT_LT(std::fabs(t.residual(t.age_row) - t.inFlight1D(t.age_row)),
              1.0e-10 * std::fabs(t.ledgeredStorage(t.age_row)) + 1.0e-12)
        << ledgerDump(t, t.age_row);

    // (4) The solute row is unchanged by all of this — evapoconcentration is
    //     the half of §3.5 that D-A20 did NOT amend.
    EXPECT_EQ(t.lost_et[0], 0.0)
        << "ET carried solute out of the aquifer" << ledgerDump(t, 0);
    finish(r);
}

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
           bool dispersion = true, double deep = 0.0) {
    std::ostringstream gw;
    gw << "[GW_TRANSPORT_OPTIONS]\nDISPERSION " << (dispersion ? "YES" : "NO")
       << "\n\n" << extra;
    Deck r = open(tag, deck(Mesh::Tri, gw.str(), 0.5, deep));
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
    const T72 plain = runT72("retard_off", params, true, 1e-4);
    const T72 held  = runT72("retard_on",  sorb, true, 1e-4);

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

// The same restart through the deck alone: `[FILES] SAVE HOTSTART … NATIVE`
// writes the V5/V6 blocks, and `USE HOTSTART` reads the file's stamp and
// applies it once the aquifer exists. Without NATIVE the row writes the
// legacy format and this test fails on the stamp — that is the falsifier.
TEST(GwTransportKernel, FilesNativeHotStartCarriesTheAquiferSpeciesAcrossARestart) {
    const fs::path hsf = fs::absolute(kOutDir / "gw_species_files.hsf");
    fs::remove(hsf);

    double sat0 = 0.0, uns0 = 0.0, infil0 = 0.0;
    {
        Deck r = open("hs_files_write", deck(Mesh::Tri,
            "[FILES]\nSAVE HOTSTART \"" + hsf.string() + "\" NATIVE\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        // end() writes the [FILES] save, after the 2D finalize — so the
        // reference values are read after it too.
        ASSERT_EQ(swmm_engine_end(r.e), SWMM_OK);
        r.started = false;
        const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
        sat0   = t.sat_mass[t.idx(0, 0)];
        uns0   = t.unsat_mass[t.idx(0, 0)];
        infil0 = t.gained_infil[0];
        finish(r);
    }
    std::string stamp(14, '\0');
    { std::ifstream f(hsf, std::ios::binary); f.read(stamp.data(), 14); }
    ASSERT_EQ(stamp, "OPENSWMM_HS_V1") << "SAVE HOTSTART … NATIVE did not write a native file";
    EXPECT_GT(sat0, 0.0);

    Deck r2 = open("hs_files_read", deck(Mesh::Tri,
        "[FILES]\nUSE HOTSTART \"" + hsf.string() + "\"\n\n"));
    ASSERT_TRUE(r2.opened);
    ASSERT_EQ(swmm_engine_initialize(r2.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r2.e, 1), SWMM_OK);
    r2.started = true;
    const auto& t2 = r2.eng->surfaceRouter2D().subsurface().transport();
    EXPECT_NEAR(t2.sat_mass[t2.idx(0, 0)], sat0, 1.0e-9 * std::fabs(sat0))
        << "the [FILES] restart did not resume the saturated store";
    EXPECT_NEAR(t2.unsat_mass[t2.idx(0, 0)], uns0, 1.0e-9 * std::fabs(uns0) + 1.0e-12)
        << "the [FILES] restart did not resume the column store";
    EXPECT_NEAR(t2.gained_infil[0], infil0, 1.0e-9 * std::fabs(infil0) + 1.0e-12);
    finish(r2);
}

// The same restart in a model whose storage node carries a LID. The native
// file was applied before init_modules() whenever node LIDs were present,
// so the aquifer, which does not exist yet then, was dropped without a word.
TEST(GwTransportKernel, FilesNativeHotStartWithANodeLidCarriesTheAquifer) {
    const std::string lid =
        "[STORAGE]\nS1 -1.0 0.75 0 FUNCTIONAL 0 0 100 0 0\n\n"   // depth = layer sum
        "[WEIRS]\nW1 S1 J1 TRANSVERSE 0.8 1.84 NO 0 0 NO\n\n"
        "[XSECTIONS]\nW1 RECT_OPEN 0.2 0.2 0 0\n\n"
        "[LID_CONTROLS]\nTrain NODE\nTrain SURFACE 150 .1\n"
        "Train MEDIA 300 .45 .20 .08 125 75 75\nTrain AGGREGATE 300 .40 2500\n"
        "Train BOTTOM 0 0\n\n"
        "[LID_NODES]\nS1 Train 10\n\n"
        "[LID_NODE_OUTLETS]\nW1 1 BOTTOM\n\n";
    const fs::path hsf = fs::absolute(kOutDir / "gw_species_files_lid.hsf");
    fs::remove(hsf);

    double sat0 = 0.0, uns0 = 0.0;
    {
        Deck r = open("hs_files_lid_write", deck(Mesh::Tri, lid +
            "[FILES]\nSAVE HOTSTART \"" + hsf.string() + "\" NATIVE\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        ASSERT_EQ(swmm_engine_end(r.e), SWMM_OK);
        r.started = false;
        const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
        sat0 = t.sat_mass[t.idx(0, 0)];
        uns0 = t.unsat_mass[t.idx(0, 0)];
        finish(r);
    }
    EXPECT_GT(sat0, 0.0);

    Deck r2 = open("hs_files_lid_read", deck(Mesh::Tri, lid +
        "[FILES]\nUSE HOTSTART \"" + hsf.string() + "\"\n\n"));
    ASSERT_TRUE(r2.opened);
    ASSERT_EQ(swmm_engine_initialize(r2.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r2.e, 1), SWMM_OK);
    r2.started = true;
    const auto& t2 = r2.eng->surfaceRouter2D().subsurface().transport();
    EXPECT_NEAR(t2.sat_mass[t2.idx(0, 0)], sat0, 1.0e-9 * std::fabs(sat0))
        << "the [FILES] restart did not resume the saturated store";
    EXPECT_NEAR(t2.unsat_mass[t2.idx(0, 0)], uns0, 1.0e-9 * std::fabs(uns0) + 1.0e-12)
        << "the [FILES] restart did not resume the column store";
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
    std::string body = deck(Mesh::Tri, "[GW_SOURCES]\nW * FLOW 0.0001 TSS CONC 2\n\n");
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
    const double want_source = t.gained_source[0];
    finish(r);

    const fs::path h5 = kOutDir / "h5_species.h5";
    ASSERT_TRUE(fs::exists(h5)) << "no results file was written";
    hid_t fid = H5Fopen(h5.string().c_str(), H5F_ACC_RDONLY, H5P_DEFAULT);
    ASSERT_GE(fid, 0);
    auto has = [&](const char* n) { return H5Lexists(fid, n, H5P_DEFAULT) > 0; };
    EXPECT_TRUE(has("Mesh2_face_gw_sat_conc"));
    EXPECT_TRUE(has("Mesh2_face_gw_unsat_conc"));
    EXPECT_TRUE(has("groundwater_species_ledger"));
    for (const char* field : {"Mesh2_face_gw_sat_conc", "Mesh2_face_gw_unsat_conc"}) {
        const hid_t ds=H5Dopen2(fid,field,H5P_DEFAULT);ASSERT_GE(ds,0);
        const hid_t attr=H5Aopen(ds,"species_units",H5P_DEFAULT);ASSERT_GE(attr,0);
        const hid_t type=H5Aget_type(attr);std::vector<char> unit(H5Tget_size(type)+1,0);
        ASSERT_GE(H5Aread(attr,type,unit.data()),0);EXPECT_STREQ(unit.data(),"MG/L");
        H5Tclose(type);H5Aclose(attr);H5Dclose(ds);
    }

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
    EXPECT_EQ(static_cast<int>(led.second[2]), 15);
    const std::size_t ll = (static_cast<std::size_t>(led.second[0]) - 1) *
                           static_cast<std::size_t>(ns) * 15;
    EXPECT_NEAR(led.first[ll + 2], want_infil, 1.0e-9) << "infil_in term";
    EXPECT_NEAR(led.first[ll + 12], want_resid, 1.0e-12) << "residual term";
    EXPECT_GT(want_source, 0.0);
    EXPECT_NEAR(led.first[ll + 13], want_source, 1.0e-9) << "source term";
    H5Fclose(fid);
}

// ---------------------------------------------------------------------------
// T7.4 — the 1D ⇄ aquifer quality seam.
//
// T7.1 left both 1D seams carrying water with no species: a recharging node
// and a leaking conduit filled their cells with clean water, and a draining
// aquifer handed the node nothing. T7.4 opened them. These gates hold the
// three claims that matter:
//
//   * a leaking conduit's mass ARRIVES (and is not removed twice — the 1D
//     quality solver already debits it as exfiltration);
//   * a recharging node's mass arrives, and the 1D books the loss it never
//     had a row for;
//   * a draining aquifer's mass reaches the node's coupling queue.
//
// The decks below drive each direction on its own so a failure names one.
// ---------------------------------------------------------------------------
namespace {

/// The aquifer deck with a BED under J1, so the node seam is live, plus an
/// inflow that decides the direction: a big one surcharges J1 and pushes
/// water DOWN the bed; none lets the (high) table drain INTO the pipe.
std::string seamDeck(double hg0, double inflow_cms, double seep_mm_hr,
                     double node_tss) {
    std::ostringstream extra;
    extra << "[2D_AQUIFER_NODE]\nJ1  1\n\n";
    std::string body = deck(Mesh::Tri, extra.str(), hg0);
    // The bed needs NODE_ENROLMENT to let a row win; the base deck already
    // says ROWS, which is what [2D_AQUIFER_NODE] above is.
    const std::string rep = "[REPORT]\nINPUT NO\n";
    const auto at = body.find(rep);
    std::ostringstream ins;
    if (inflow_cms != 0.0) {
        ins << "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n";
        if (node_tss > 0.0) ins << "J1  TSS  IN2  CONCEN  1.0  1.0\n";
        ins << "\n[TIMESERIES]\nIN1  0:00  " << inflow_cms
            << "\nIN1  1:00  " << inflow_cms << "\n";
        if (node_tss > 0.0)
            ins << "IN2  0:00  " << node_tss << "\nIN2  1:00  " << node_tss << "\n";
        ins << "\n";
    }
    if (seep_mm_hr > 0.0)
        ins << "[LOSSES]\n;;Link Kentry Kexit Kavg Flap Seepage\nC1  0 0 0 NO  "
            << seep_mm_hr << "\n\n";
    body.insert(at, ins.str());
    return body;
}

struct SeamResult {
    double gained_node = 0.0, lost_node = 0.0;
    double gained_link = 0.0, lost_link = 0.0;
    double residual = 0.0, init = 0.0;
    double in_flight = 0.0;      ///< G-W1: sampled from the 1D, not yet gathered
    double queue_sum = 0.0;      ///< mass parked in the node's coupling queue
    double gw_in_ledger = 0.0;   ///< 1D qual_routing_gw_in
    double seep_ledger = 0.0;    ///< 1D qual_routing_seep
    double ex_in_ledger = 0.0;   ///< 1D qual_routing_ex_in (the coupling row)
    double v12 = 1.0;            ///< vol_1d_to_2d, for the unit check
    double led_node = 0.0;       ///< water ledger, m³, + out of the aquifer
    double led_link = 0.0;       ///< water ledger, m³, + into the aquifer
};

SeamResult runSeam(const std::string& tag, const std::string& body) {
    SeamResult o;
    Deck r = open(tag, body);
    EXPECT_TRUE(r.opened) << tag;
    if (!r.opened) return o;
    EXPECT_TRUE(run(r)) << tag;
    const auto& ctx = r.eng->context();
    const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
    if (t.active()) {
        o.gained_node = t.gained_node[0];
        o.lost_node   = t.lost_node[0];
        o.gained_link = t.gained_link[0];
        o.lost_link   = t.lost_link[0];
        o.residual    = t.residual(0);
        o.in_flight   = t.inFlight1D(0);
        o.init        = t.init_mass[0];
    }
    o.v12 = r.eng->surfaceRouter2D().options().vol_1d_to_2d;
    o.led_node = r.eng->surfaceRouter2D().subsurface().state().led_node;
    o.led_link = r.eng->surfaceRouter2D().subsurface().state().led_link;
    for (double v : ctx.nodes.coupling_qual_queue) o.queue_sum += v;
    if (!ctx.mass_balance.qual_routing_gw_in.empty())
        o.gw_in_ledger = ctx.mass_balance.qual_routing_gw_in[0];
    if (!ctx.mass_balance.qual_routing_seep.empty())
        o.seep_ledger = ctx.mass_balance.qual_routing_seep[0];
    if (!ctx.mass_balance.qual_routing_ex_in.empty())
        o.ex_in_ledger = ctx.mass_balance.qual_routing_ex_in[0];
    finish(r);
    return o;
}

/// G-W1 diagnostic: the WATER side of the same seam, in volume.
struct SeamWater {
    double led_node = 0.0;          ///< aquifer's own books, m³, + out
    double routing_external = 0.0;  ///< 1D inflow row (ft³ / project units)
    double routing_coupling_out = 0.0;
    double coupling_queue = 0.0;    ///< still parked, not yet delivered
    double coupling_volume = 0.0;
    double nacc = 0.0;              ///< sampled but not yet gathered by a cell
    double sy_gap = 0.0;            ///< G-W1b: textbook Sy / the one the column uses
    long   cap_binds = 0;           ///< G-W1b: firings the drain cap clamped
    long   refunds   = 0;           ///< G-W1b: firings a column went short on the node
    double error_pct = 0.0;         ///< the 1D flow continuity error
    double v12 = 1.0;
};

SeamWater runSeamWater(const std::string& tag, const std::string& body) {
    SeamWater o;
    Deck r = open(tag, body);
    EXPECT_TRUE(r.opened) << tag;
    if (!r.opened) return o;
    EXPECT_TRUE(run(r)) << tag;
    const auto& ctx = r.eng->context();
    const auto& led = r.eng->surfaceRouter2D().subsurface().state();
    o.led_node = led.led_node;
    for (double v : led.nacc) o.nacc += v;
    // G-W1b (2026-09-23): the counters that say whether this deck is an
    // instrument, straight from the solver. `sy_gap` is kept for the failure
    // message only — see the gate — and it now asks the solver for the yield
    // the column actually uses instead of re-deriving one branch of it.
    const auto& gw = r.eng->surfaceRouter2D().subsurface();
    o.cap_binds = gw.drainCapBinds();
    o.refunds   = gw.nodeRefunds();
    for (int c = 0; c < led.n_cells; ++c) {
        const auto u = static_cast<std::size_t>(c);
        const double sy_col  = std::max(gw.specificYieldOf(c), 1.0e-300);
        const double sy_text = led.theta_s[u] - led.theta_r[u];
        o.sy_gap = std::max(o.sy_gap, sy_text / sy_col);
    }
    o.routing_external = ctx.mass_balance.routing_external;
    o.routing_coupling_out = ctx.mass_balance.routing_coupling_out;
    // G-W1c (2026-09-23): the promised-but-undelivered tail, scoped to the
    // nodes that actually HAVE a bed — these queues are shared with the 2D
    // surface's drain, so on a deck with both, summing every node would
    // measure the wrong thing.
    //
    // **It is a no-op on the decks in this file** (one junction, so the sum
    // selects the same single entry) and it was added on a hypothesis that
    // turned out to be WRONG: I thought the SIGMA tail mismatch below was
    // the surface drain contaminating the sum. It is not. With the bed
    // switched off the surface still produces Dunne water (2.05 m³) and
    // promises the node nothing at all, so the bed is the only producer and
    // there is nothing an origin tag could separate. The scoping is kept
    // because it is the right quantity to sum, not because it fixes
    // anything. See the handoff for what the SIGMA mismatch actually is.
    for (const auto& b : gw.nodeBeds()) {
        const auto ni = static_cast<std::size_t>(b.node);
        if (ni < ctx.nodes.coupling_queue.size())
            o.coupling_queue += ctx.nodes.coupling_queue[ni];
        if (ni < ctx.nodes.coupling_volume.size())
            o.coupling_volume += ctx.nodes.coupling_volume[ni];
    }
    o.error_pct = ctx.mass_balance.routing_error() * 100.0;
    o.v12 = r.eng->surfaceRouter2D().options().vol_1d_to_2d;
    finish(r);
    return o;
}

}  // namespace

// ---------------------------------------------------------------------------
// G-W1 — the two sides of the bed exchange must agree IN VOLUME.
//
// The validator's control run settled where T7.4's remaining residual lives:
// `bed_only.inp` reports −0.722 % on the 1D flow continuity with the bed on
// and 0.000 % with it off. That is a water defect, not a quality one, and
// quality riding on water 0.7 % out is the ceiling on how well the seam can
// ever report.
//
// The deck is the sharpest instrument available: a high table, NO rain, NO
// [INFLOWS], so the *only* thing that puts water into the 1D network is the
// aquifer draining through J1's bed. Every unit of `led_node` must therefore
// turn up in `routing_external` (the row the coupling inflow folds into),
// modulo whatever is still parked in the delivery queue at the end of the
// run.
// ---------------------------------------------------------------------------
// The base `deck()` bakes in a raingage, and rain on the mesh spills to the
// node through the SURFACE coupling — which folds into the same
// `routing_external` row. That makes it useless as an instrument here (it
// reads 22.06 against the bed's 2.35, and the difference is honest surface
// water). This deck has no raingage at all, so the bed is the only inflow.
std::string bedOnlyDeck(bool bed, const char* closure = "CLOSED_FORM",
                        double zs = 1.0, double hg0 = 0.95) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[JUNCTIONS]\nJ1 -2.0 4.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
         "RAINFALL_MODE SYSTEM\n\n"
         "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  40.0  -  -  -  -\n\n"
         "[2D_AQUIFER_OPTIONS]\nCLOSURE " << closure
      << "\nNODE_ENROLMENT ROWS\n\n"
         "[2D_AQUIFER]\n;;Scope KS ZS THETA_S THETA_R ALPHA\n"
         "*  36.0  " << zs << "  0.45  0.10  2.0  HG0 " << hg0 << "\n\n";
    // The control: the same deck with the bed switched off. The validator's
    // pair — bed on gives −0.722 %, bed off gives 0.000 % — is what
    // established that the residual IS the bed.
    if (bed) s << "[2D_AQUIFER_NODE]\nJ1  1\n\n";
    else     s << "[2D_AQUIFER_NODE]\nJ1  1  EXCHANGE NO\n\n";
    s << "[COORDINATES]\nJ1  5.0  5.0\nO1  40.0  40.0\n\n"
         "[REPORT]\nINPUT NO\n";
    return s.str();
}

void checkDrainingBed(const std::string& tag, const char* closure,
                      double zs, double hg0) {
    SCOPED_TRACE(tag);
    // The control first: with the bed off, nothing enters the 1D at all and
    // its continuity must be exactly closed. If this fails, the deck is
    // wrong and the measurement below means nothing.
    const SeamWater off = runSeamWater(tag + "_none",
                                       bedOnlyDeck(false, closure, zs, hg0));
    ASSERT_LT(std::fabs(off.error_pct), 1.0e-6)
        << "the control deck does not close (" << off.error_pct
        << " %), so it cannot be used to attribute the residual to the bed";

    const SeamWater s = runSeamWater(tag, bedOnlyDeck(true, closure, zs, hg0));
    ASSERT_GT(s.led_node, 0.0) << "the bed did not drain, so nothing is tested";

    // …and the deck must be able to SEE this defect.
    //
    // The first version of this guard thresholded the RATIO between the two
    // specific yields, and it was wrong twice over: no table depth in the
    // deck family fell below it (350 / 96.4 / 38.1 / 21.2 / 14.0 as HG0 goes
    // 0.95 → 0.10, against a threshold of 10), and — the serious half — a
    // deck scoring **56.8×**, nearly six times over, is completely BLIND to
    // the defect: `ZS 8.0 / HG0 7.6` was measured bit-identical against a
    // reverted build. The ratio is not what decides anything. `avail =
    // h_g · Sy · A` binds when it is small in ABSOLUTE terms, and at
    // h_g = 7.6 m the drainable water is enormous under either yield.
    //
    // So the instrument is the clamp itself, counted where it happens. No
    // threshold to tune, and no geometry can make it lie.
    ASSERT_GT(s.cap_binds, 0)
        << "the aquifer→node drain cap never clamped on this deck, so the"
           " assertions below would pass with the defect present. (Specific"
           " yields here differ by " << s.sy_gap << "×, which is NOT the"
           " test — a 56.8× deck sees nothing. What matters is that h_g·Sy·A"
           " is small enough to bind: put the table close under the ground"
           " AND keep the column thin.)";

    // …and then the defect's own signature must be absent. Every refund is
    // water the router already paid the node and the aquifer then disowned.
    EXPECT_EQ(s.refunds, 0)
        << s.refunds << " firings went short on the node's account and were"
           " refunded into the aquifer's books alone — after the router had"
           " already handed that volume over. This was ~120 before G-W1"
           " (124 on Linux, 117 on macOS — the COUNT is platform-dependent,"
           " which is why the assertion is == 0 and not a magnitude).";

    // The aquifer's side of the seam is what its ledger says left plus what
    // is still sitting in the side accumulator, sampled but not yet gathered
    // by a cell firing. (`nacc` is the in-flight tail, not a loss: the run
    // ends between a sample and the firing that consumes it.)
    const double want_1d = (s.led_node + s.nacc) / s.v12;
    const double booked  = s.routing_external + s.coupling_queue + s.coupling_volume;
    EXPECT_NEAR(booked, want_1d, 1.0e-9 * want_1d)
        << "the aquifer gave up " << want_1d << " (1D units) through the bed"
           " — ledgered " << (s.led_node / s.v12) << " plus " << (s.nacc / s.v12)
        << " in flight — but the 1D booked " << booked << ": external "
        << s.routing_external << ", queued " << s.coupling_queue
        << ", unqueued " << s.coupling_volume << ".\n"
           "Too LARGE means the node received water the aquifer never"
           " released — the refund path in fireCell §2 shrinking"
           " `qnode_last` after the router has already handed the volume"
           " over. Too SMALL means the aquifer released water the node never"
           " got.";
    // …and the two ends of the in-flight tail are the same water. The router
    // commits a sample to the node's `coupling_volume` at sampling time,
    // while the aquifer only ledgers it when the cell fires and gathers
    // `nacc`. Between those two moments the volume is legitimately on both
    // sets of books, and at the end of a run one sample is always caught
    // there. Asserting they are EQUAL is what distinguishes that lag from a
    // leak — a leak would make the promised tail bigger than the held one.
    //
    // **This holds to the last bit under CLOSED_FORM and ENSLAVED and fails
    // by 20× under SIGMA**, which is why SIGMA is not yet a case here.
    //
    // G-W4 (2026-09-25) found what that is, and the assertion is RIGHT to
    // fail: whole capped samples are promised to the node and never gathered
    // by the aquifer. On a fully saturated SIGMA column (`hg == zs`, so
    // θ_s − θ_bot → 0) `Sy` sits exactly on `kSyFloor` and the drain cap is
    // pinned to a CONSTANT `kFaceShare·hg·kSyFloor·A` = 0.005 m³ per step —
    // the promise is 0.005 at 10 s and 0.020 at 30 s, every gap an integer
    // multiple. Round numbers out of a floating-point integration mean a
    // limit, not an integral.
    //
    // Two earlier readings of mine are dead: LTS tiering (all three closures
    // sit on tier 0 at the 5 s routing step, so flush and gather DO share a
    // cadence) and "one sample of the live rate" (`qnode_last` moves ±15 %
    // while the promise stays bit-identical). So this is a mis-booking, not a
    // reporting artefact, and stating a weaker invariant here would hide it.
    // It is the drain-side twin of the fill-side defect in G-W2 — see the
    // handoff's `kSyFloor` table.
    EXPECT_NEAR(s.coupling_volume * s.v12, s.nacc, 1.0e-12)
        << "the 1D has been promised " << (s.coupling_volume * s.v12)
        << " m³ that has not been delivered, but the aquifer is only holding "
        << s.nacc << " m³ that it has not yet handed over";

    // The 1D's own continuity error is REPORTED, not asserted against a
    // threshold: on a deck this short the undelivered tail above is a real
    // fraction of a tiny total (it is neither an inflow nor storage until it
    // is delivered), and it shrinks with run length. Picking a tolerance
    // that swallows it would be choosing a number to make a gate pass. The
    // equality assertions above are the falsifiable statement.
    if (std::fabs(s.error_pct) > 1.0e-9)
        std::fprintf(stderr,
                     "[G-W1] 1D flow continuity, %s: %.6f %% (clamped %ld"
                     " firings; undelivered tail %.6g m3 of %.6g m3"
                     " exchanged)\n",
                     tag.c_str(), s.error_pct, s.cap_binds, s.nacc,
                     s.led_node + s.nacc);
}

TEST(GwTransportKernel, DrainingBedAgreesWithTheNodeInVolume) {
    // Closure A. This is the deck the defect was found on.
    checkDrainingBed("gw1_bed_only", "CLOSED_FORM", 1.0, 0.95);
}

TEST(GwTransportKernel, DrainingBedAgreesWithTheNodeUnderEnslavedClosure) {
    // G-W1c (2026-09-23): a SECOND closure, because every fixture in this
    // file was closure A and that is exactly the blind spot the last round
    // found one level up (a guard that re-derived only the CLOSED_FORM
    // branch of `specificYield` and agreed for that reason alone).
    //
    // The geometry is the validator's: on ZS 1.0 / HG0 0.95 the drain cap
    // never clamps under ENSLAVED, so the instrument guard would refuse to
    // certify — correctly. A thinner column binds (228 clamps measured).
    //
    // SIGMA is deliberately NOT here yet: it binds (422 clamps) and the main
    // identity passes with zero refunds, but the in-flight tail assertion
    // fails by 20× for a reason that is NOT a leak — see the handoff. Adding
    // it before that is understood would mean either a red gate or a
    // loosened one.
    checkDrainingBed("gw1_bed_enslaved", "ENSLAVED", 0.5, 0.45);
}

TEST(GwTransportKernel, LeakingConduitCarriesItsQualityIntoTheAquifer) {
    // A seeping conduit over the mesh, no node inflow. The 1D already
    // debits this mass as its exfiltration loss, so the aquifer receiving
    // it completes a transfer rather than creating one.
    std::string body=seamDeck(0.5,0.0,100.0,0.0);
    const std::string anchor="[2D_AQUIFER_OPTIONS]\n";
    body.insert(body.find(anchor)+anchor.size(),"LINK_SEEPAGE ONE_WAY\n");
    const SeamResult s = runSeam("seam_link",body);
    EXPECT_GT(s.gained_link, 0.0)
        << "a leaking conduit delivered no mass — the seam is still closed";
    // …and it did not come out of the aquifer's own books: the residual
    // still closes with the arrival counted as a gain.
    // G-W1 (2026-09-22): the identity is completed, NOT loosened. `nacc_mass`
    // is counted in `ledgeredStorage` (in-flight mass is storage the ledger
    // has not yet named) but is only booked into `gained_node` when the cell
    // gathers it, so between those two moments the balance is short by
    // exactly what is in transit. A run that ends in that window carries the
    // tail. Asserting `residual == inFlight1D` is the whole statement; the
    // bound on it is unchanged. Before this was written the gate passed on
    // luck: tightening the node cap (G-W1) shifted the final sample and the
    // tail jumped to 0.0705 against a 6.1e-8 bound, with `residual` equal to
    // `inFlight1D` to the last digit — which is how the tail was identified
    // rather than mistaken for an 11 % mass leak.
    EXPECT_LT(std::fabs(s.residual - s.in_flight),
              1.0e-10 * std::fabs(s.init) + 1.0e-12)
        << "residual " << s.residual << ", of which " << s.in_flight
        << " is sampled-but-not-yet-gathered; the unexplained part is "
        << (s.residual - s.in_flight);
    // The 1D's own seepage ledger is non-zero: the mass left the pipe. That
    // is the quantity the aquifer received; the two are booked on opposite
    // sides of one transfer, never removed twice from the 1D.
    EXPECT_GT(s.seep_ledger, 0.0) << "the 1D booked no exfiltration";
    // Review F22 (2026-10-15): the mass must arrive in the AQUIFER's units.
    // Per delivered m³ it cannot carry more than the richest water on the
    // deck (cell 1's 25 mg/L seed; rain is 10, the rest of the column 5 and
    // 2). Booking the conduit's conc·ft³ into the conc·m³ store made it
    // carry 35.31× the pipe's concentration.
    ASSERT_GT(s.led_link, 0.0) << "no seepage water arrived; deck defect";
    EXPECT_LE(s.gained_link / s.led_link, 25.0 * (1.0 + 1.0e-9))
        << "the aquifer received " << s.gained_link << " conc·m³ with "
        << s.led_link << " m³ — " << s.gained_link / s.led_link
        << " per m³ (1/vol_1d_to_2d = " << 1.0 / s.v12 << ")";
}

TEST(GwTransportKernel, RechargingNodeSendsItsQualityDownTheBedAndIsLedgered) {
    // J1 surcharged by a 0.5 m³/s inflow carrying 50 mg/L, over a column
    // with room: the node pushes water DOWN its bed.
    const SeamResult s = runSeam("seam_node_in", seamDeck(0.5, 3.0, 0.0, 50.0));
    EXPECT_GT(s.gained_node, 0.0)
        << "a recharging node delivered no mass — the seam is still closed";
    // G-W1 (2026-09-22): the identity is completed, NOT loosened. `nacc_mass`
    // is counted in `ledgeredStorage` (in-flight mass is storage the ledger
    // has not yet named) but is only booked into `gained_node` when the cell
    // gathers it, so between those two moments the balance is short by
    // exactly what is in transit. A run that ends in that window carries the
    // tail. Asserting `residual == inFlight1D` is the whole statement; the
    // bound on it is unchanged. Before this was written the gate passed on
    // luck: tightening the node cap (G-W1) shifted the final sample and the
    // tail jumped to 0.0705 against a 6.1e-8 bound, with `residual` equal to
    // `inFlight1D` to the last digit — which is how the tail was identified
    // rather than mistaken for an 11 % mass leak.
    EXPECT_LT(std::fabs(s.residual - s.in_flight),
              1.0e-10 * std::fabs(s.init) + 1.0e-12)
        << "residual " << s.residual << ", of which " << s.in_flight
        << " is sampled-but-not-yet-gathered; the unexplained part is "
        << (s.residual - s.in_flight);
    // The 1D side books the loss it never had a row for before T7.4 — the
    // reason its quality continuity error used to grow by exactly this.
    EXPECT_GT(s.seep_ledger, 0.0)
        << "the node gave up mass with no ledger entry (the pre-T7.4 defect)";
    // Review F22 (2026-10-15): the node's water is at most 50 mg/L (its only
    // inflow) and the aquifer it could have drawn from is at most 25, so a
    // recharge cannot deliver more than 50 per m³ — in the aquifer's units.
    ASSERT_LT(s.led_node, 0.0) << "the bed did not recharge; deck defect";
    EXPECT_LE(s.gained_node / -s.led_node, 50.0 * (1.0 + 1.0e-9))
        << "the aquifer received " << s.gained_node << " conc·m³ with "
        << -s.led_node << " m³ — " << s.gained_node / -s.led_node
        << " per m³ (1/vol_1d_to_2d = " << 1.0 / s.v12 << ")";
}

TEST(GwTransportKernel, DrainingAquiferHandsItsMassToTheNodeQueue) {
    // A high table and no inflow: the aquifer drains into J1 through its
    // bed, and the mass must reach the node's coupling queue — the same
    // channel the 2D surface's drain uses.
    const SeamResult s = runSeam("seam_node_out", seamDeck(0.95, 0.0, 0.0, 0.0));
    EXPECT_GT(s.lost_node, 0.0)
        << "the aquifer drained no mass to the node" ;
    // THE seam identity, and the one gate that catches both failure modes
    // this seam has produced: every unit of mass the aquifer gave up is
    // booked on the 1D side EXACTLY ONCE — delivered into an inflow row, or
    // still sitting in the queue waiting for its delivery span.
    //
    //   too small  ⇒ mass is being dropped   (the LTS-cadence bug, 75–99.8 %)
    //   too large  ⇒ mass is counted twice   (the queue + gw_in duplicate,
    //                                          a 49.7 % continuity error)
    //
    // Asserting each side is merely non-zero would have caught neither.
    EXPECT_GT(s.ex_in_ledger + s.gw_in_ledger + s.queue_sum, 0.0)
        << "the aquifer's mass never reached the node";
    EXPECT_GE(s.queue_sum, 0.0) << "a negative mass is parked in the queue";
    // The identity, not a band: what the aquifer gave up is what the node
    // received, once the aquifer's conc·m³ is expressed in the 1D's own
    // conc·ft³. (Getting this wrong by a unit factor is how a seam looks
    // fine and loses three quarters of its mass — which is exactly what the
    // first cut of this gate caught.)
    // This deck has no other external inflow and no surface coupling point,
    // so every unit in the 1D's inflow rows came from the aquifer.
    const double want_1d = s.lost_node / s.v12;
    const double booked  = s.ex_in_ledger + s.gw_in_ledger + s.queue_sum;
    EXPECT_NEAR(booked, want_1d, 1.0e-9 * want_1d)
        << "the 1D booked " << booked << " (ex_in " << s.ex_in_ledger
        << " + gw_in " << s.gw_in_ledger << " + queued " << s.queue_sum
        << ") against the aquifer's " << s.lost_node << " = " << want_1d
        << " in 1D units (v12 " << s.v12 << ")";
    // G-W1 (2026-09-22): the identity is completed, NOT loosened. `nacc_mass`
    // is counted in `ledgeredStorage` (in-flight mass is storage the ledger
    // has not yet named) but is only booked into `gained_node` when the cell
    // gathers it, so between those two moments the balance is short by
    // exactly what is in transit. A run that ends in that window carries the
    // tail. Asserting `residual == inFlight1D` is the whole statement; the
    // bound on it is unchanged. Before this was written the gate passed on
    // luck: tightening the node cap (G-W1) shifted the final sample and the
    // tail jumped to 0.0705 against a 6.1e-8 bound, with `residual` equal to
    // `inFlight1D` to the last digit — which is how the tail was identified
    // rather than mistaken for an 11 % mass leak.
    EXPECT_LT(std::fabs(s.residual - s.in_flight),
              1.0e-10 * std::fabs(s.init) + 1.0e-12)
        << "residual " << s.residual << ", of which " << s.in_flight
        << " is sampled-but-not-yet-gathered; the unexplained part is "
        << (s.residual - s.in_flight);
}

// ---------------------------------------------------------------------------
// T7.4 — the reported continuity error must be scaled by a denominator that
// names EVERY inflow route.
//
// T7.4 added two routes (the node bed and the leaking conduit) and the
// `.rpt` block's own copy of the "what came in" sum was not updated. On a
// network deck fed only through its beds that divided a machine-precision
// residual by a machine-precision denominator: **6 198 889.840 %** printed
// beside a balance exact to the last digit.
//
// This is tested against the FORMULA rather than through a deck, because a
// deck cannot isolate it: saturation excess carries the seam's own mass back
// to the surface, which re-infiltrates it, so `gained_infil` is non-zero on
// any deck where a bed recharges hard enough to matter — two successive
// attempts at a deck-based version of this gate passed against the bug for
// that reason. The sum now lives beside the residual it scales, which is
// also what stops the two drifting apart again.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, ContinuityDenominatorNamesEveryInflowRoute) {
    using openswmm::twoD::SubsurfaceTransportState;

    // One species, one cell: enough to ask the question.
    const auto only = [](int route) {
        SubsurfaceTransportState t;
        t.resize(1, 1, 0);
        t.row_names = {"TSS"};
        // Mass arrives by exactly ONE route and is all still there, so the
        // residual is zero and the denominator is the only thing that can
        // make the reported percentage misbehave.
        t.sat_mass[0] = 5.0;
        switch (route) {
            case 0: t.init_mass[0]    = 5.0; break;
            case 1: t.gained_infil[0] = 5.0; break;
            case 2: t.gained_node[0]  = 5.0; break;   // T7.4
            case 3: t.gained_link[0]  = 5.0; break;   // T7.4
            default: t.net_lateral[0] = 5.0; break;
        }
        return t;
    };

    const char* names[5] = {"init_mass", "gained_infil", "gained_node",
                            "gained_link", "net_lateral"};
    for (int route = 0; route < 5; ++route) {
        const SubsurfaceTransportState t = only(route);
        EXPECT_NEAR(t.continuityDenominator(0), 5.0, 1.0e-12)
            << "mass that arrived through " << names[route]
            << " is not counted in the continuity denominator — a deck fed"
               " only that way divides a machine-precision residual by"
               " nothing";
        // …and the residual it scales is zero, so the reported percentage is
        // zero rather than astronomical.
        EXPECT_LT(std::fabs(t.residual(0)), 1.0e-12) << names[route];
    }

    // T7.4b: the other end of the same question. With every route naming
    // itself, a ZERO denominator beside a NON-zero residual is no longer an
    // uninteresting empty aquifer — it is mass that left without any route
    // booking its arrival, which is the signature of the sixth channel
    // nobody has wired up yet. The report must not print 0.000 % there
    // (`n/a` instead), because 0 % is the one answer that would hide it.
    SubsurfaceTransportState t;
    t.resize(1, 1, 0);
    t.row_names = {"TSS"};
    t.lost_deep[0] = 5.0;   // mass left…
    // …and nothing booked its arrival: all five routes stay zero.
    EXPECT_EQ(t.continuityDenominator(0), 0.0)
        << "a state with no inflow on any route must have no scale to divide"
           " by — if this is non-zero the denominator has grown a term that"
           " is not an inflow";
    EXPECT_GT(std::fabs(t.residual(0)), 1.0e-12)
        << "the fixture is meant to hold an UNEXPLAINED residual; if it does"
           " not, it no longer tests the case it was written for";
}

// ---------------------------------------------------------------------------
// T7.4b — and the report must actually SAY it cannot tell.
//
// This is the end-to-end half of the gate above, and its zeros are
// STRUCTURAL rather than arithmetic: there is no raingage at all (so no
// infiltration), the junction sits outside the mesh (so no bed), the column
// is uniform (so no lateral gradient) and there is no `[GW_INITIAL_QUALITY]`
// seed. Every route is zero because nothing in the deck can drive it — not
// because two fluxes happened to cancel. That distinction is the whole
// reason this one is allowed to be a gate: a deck whose zero depends on
// float residue in a face summation can flip to tiny-but-nonzero under
// nothing more than a mesh refinement, and would then pass while the
// defect is present.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, ReportSaysNaWhenThereIsNoScaleToDivideBy) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[POLLUTANTS]\n;;Name Units Rain GW IIflow Kdecay\n"
         "TSS  MG/L  0.0  0.0  0.0  0.0  NO  *  0.0  0.0\n\n"
         "[JUNCTIONS]\nJ1 -2.0 4.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -2.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
         "RAINFALL_MODE SYSTEM\n\n"
         "[2D_VERTICES]\n0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  40.0  -  -  -  -\n\n"
         "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n\n"
         "[2D_AQUIFER]\n;;Scope KS ZS THETA_S THETA_R ALPHA\n"
         "*  36.0  1.0  0.45  0.10  2.0  HG0 0.5\n\n"
         // J1 is deliberately OFF the mesh: an in-mesh junction would be
         // auto-enrolled as a bed and would give `gained_node` a route.
         "[COORDINATES]\nJ1  50.0  50.0\nO1  60.0  60.0\n\n"
         "[REPORT]\nINPUT NO\n";

    Deck r = open("denom_no_scale", s.str());
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    ASSERT_EQ(swmm_engine_end(r.e), SWMM_OK);
    r.started = false;
    ASSERT_EQ(swmm_engine_report(r.e), SWMM_OK);
    finish(r);

    const std::string rpt = [&] {
        std::ifstream f(kOutDir / "denom_no_scale.rpt");
        std::ostringstream ss; ss << f.rdbuf(); return ss.str();
    }();
    const auto blk = rpt.find("2D Aquifer Quality Continuity");
    ASSERT_NE(blk, std::string::npos)
        << "the species block is missing, so this gate is measuring nothing";
    const auto line = rpt.find("Continuity Error", blk);
    ASSERT_NE(line, std::string::npos);
    const std::string reported = rpt.substr(line, 60);
    EXPECT_NE(reported.find("n/a"), std::string::npos)
        << "with every inflow route at zero there is no scale to judge the"
           " residual against, and the report claimed a number anyway: \""
        << reported.substr(0, reported.find('\n')) << "\". Printing 0.000"
           " here reads as a perfect balance when the truth is that the"
           " question is unanswerable — and a zero scale beside a non-zero"
           " residual is exactly how the NEXT unbooked inflow route will"
           " announce itself.";
}

// Phase35: authored wells must affect the actual aquifer and its persisted books.
namespace {
std::string forcingDeck(const std::string& rows, double hg = .3, Mesh mesh = Mesh::Tri) {
    auto text = deck(mesh, rows, hg);
    auto replace = [&](const std::string& a, const std::string& b) {
        const auto at = text.find(a); if (at != std::string::npos) text.replace(at,a.size(),b);
    };
    replace("RAIN  0:00  20.0", "RAIN  0:00  0.0");
    replace("*  CONSTANT  40.0", "*  CONSTANT  0.0");
    replace("*  36.0  1.0", "*  0.000000001  1.0");
    replace("CELL 1  36.0  1.0  0.45  0.10  2.0  HG0 0.95", "");
    return text;
}
}
TEST(GwTransportKernel, NamedSourcesApplyTotalFlowScaleAndNativeMassOnActualCells) {
    for (const auto& units : {"MG/L", "UG/L", "#/L"}) {
        auto body=forcingDeck("[GW_SOURCES]\nW * FLOW 0.001 SCALE 0.5 TSS CONC 10\nM CELL 1 FLOW 0 TSS MASS 2\n\n");
        const auto at=body.find("TSS  MG/L");body.replace(at,9,std::string("TSS  ")+units);
        Deck r=open(std::string("named_sources_")+units[0],body);ASSERT_TRUE(r.opened);
        const auto expectedUnit=units[0]=='#'?openswmm::MassUnits::COUNTS_PER_L:
            units[0]=='U'?openswmm::MassUnits::UG_PER_L:openswmm::MassUnits::MG_PER_L;
        ASSERT_EQ(r.eng->context().pollutants.units[0],expectedUnit);
        ASSERT_TRUE(run(r));
        const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& tr=gw.transport();
        EXPECT_NEAR(gw.state().led_source_in,.6,1e-7);
        EXPECT_NEAR(tr.gained_source[0],8.4,1e-6); // 0.6 m3 × 10 + 2 native-mass/s × 1200 / 1000
        EXPECT_NEAR(gw.state().continuityResidual(),0,1e-7);
        EXPECT_NEAR(tr.residual(0),0,1e-7);
        EXPECT_GT(gw.state().hg[0],.3);EXPECT_GT(gw.state().hg[1],.3);
        double value=0;ASSERT_EQ(swmm_gw2d_get_ledger(r.e,SWMM_GW2D_LED_SOURCE_IN,&value),SWMM_OK);EXPECT_NEAR(value,.6,1e-7);
        ASSERT_EQ(swmm_gw2d_get_species_ledger(r.e,0,SWMM_GW2D_SPL_SOURCE_IN,&value),SWMM_OK);EXPECT_NEAR(value,8.4,1e-6);
        finish(r);
    }
}
TEST(GwTransportKernel, ExtractionIsAvailabilityLimitedAndRemovesDissolvedSpecies) {
    Deck r=open("source_extraction",forcingDeck("[GW_SOURCES]\nP * FLOW -1\n\n",.1));
    ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r));const auto& gw=r.eng->surfaceRouter2D().subsurface();
    EXPECT_GT(gw.state().led_source_out,0);EXPECT_LT(gw.state().led_source_out,20);
    EXPECT_GT(gw.transport().lost_source[0],0);
    EXPECT_NEAR(gw.state().continuityResidual(),0,1e-7);EXPECT_NEAR(gw.transport().residual(0),0,1e-7);
    finish(r);
}
TEST(GwTransportKernel, SourceSeriesUsesNonMidnightStartAndFileValues) {
    fs::create_directories(kOutDir);
    const auto series=kOutDir/"well_nonmidnight.dat";
    {std::ofstream out(series);out<<"01/01/2026 12:00 0\n01/01/2026 12:10 0.002\n01/01/2026 12:20 0\n";}
    auto body=forcingDeck("[TIMESERIES]\nWQ FILE \""+series.string()+"\"\n\n[GW_SOURCES]\nW * FLOW WQ TSS CONC 3\n\n");
    auto at=body.find("START_TIME           00:00:00");body.replace(at,std::string("START_TIME           00:00:00").size(),"START_TIME           12:00:00");
    at=body.find("END_TIME             00:20:00");body.replace(at,std::string("END_TIME             00:20:00").size(),"END_TIME             12:20:00");
    Deck r=open("source_nonmidnight",body);ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r));
    // End flushes the final partial surface/GW synchronization batch.
    ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const double duration=r.eng->context().options.totalDurationMs()/1000.0;
    EXPECT_NEAR(r.eng->context().current_time,duration,1e-8);
    ASSERT_GT(duration,1198);ASSERT_LE(duration,1200);
    // Legacy duration floors separate datetime fractions: this non-midnight
    // interval is 1199 s on this representation, not an assumed 1200 s.
    const double expected=1.2-(1200-duration)*(1200-duration)*.002/(2*600);
    const auto& gw=r.eng->surfaceRouter2D().subsurface();
    EXPECT_NEAR(gw.state().led_source_in,expected,1e-8);
    EXPECT_NEAR(gw.transport().gained_source[0],3*expected,1e-8);finish(r);
}
TEST(GwTransportKernel, UnsupportedGroundwaterForcingFailsRatherThanRunningInertly) {
    const std::vector<std::string> unsupported={
        "[GW_INITIAL_QUALITY]\n* LAYER 1 TSS 3\n\n",
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n\n",
        "[GW_SOURCES]\nW TAG no_such_tag FLOW 0.001\n\n"};
    for(std::size_t i=0;i<unsupported.size();++i){
        SCOPED_TRACE(i);
        Deck r=open("unsupported_forcing_"+std::to_string(i),forcingDeck(unsupported[i]));
        if(i<2)ASSERT_TRUE(r.opened); // Valid authored drafts; the active kernel rejects unsupported semantics.
        if(r.opened)EXPECT_NE(swmm_engine_initialize(r.e),SWMM_OK);
        std::string errors=swmm_get_last_error_msg(r.e)?swmm_get_last_error_msg(r.e):"";
        for(int n=0;n<swmm_get_error_count(r.e);++n)if(const char* e=swmm_get_error_at(r.e,n))errors+=e;
        EXPECT_NE(errors.find(i==0?"LAYER":i==1?"GW_BOUNDARY_QUALITY":"no_such_tag"),std::string::npos)<<errors;
        finish(r);
    }
}
TEST(GwTransportKernel, InitialCellQualityOverridesLaterGlobalRows) {
    Deck r=open("initial_scope_precedence",forcingDeck("[GW_INITIAL_QUALITY]\n* SAT TSS 7\n\n"));ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK);
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& tr=gw.transport();
    EXPECT_NEAR(tr.sat_mass[tr.idx(0,0)]/gw.satVolume(0),25,1e-12);
    EXPECT_NEAR(tr.sat_mass[tr.idx(0,1)]/gw.satVolume(1),7,1e-12);finish(r);
}
TEST(GwTransportKernel, SourceLedgersResumeThroughNativeHotstart) {
    const auto body=forcingDeck("[GW_SOURCES]\nW * FLOW 0.0005 TSS CONC 2\nP CELL 1 FLOW -0.0001\n\n");
    const auto path=kOutDir/"source_restart.hsf";Deck r=open("source_restart_write",body);ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r));
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const double waterIn=gw.state().led_source_in,waterOut=gw.state().led_source_out;
    const double massIn=gw.transport().gained_source[0],massOut=gw.transport().lost_source[0];
    ASSERT_EQ(swmm_hotstart_save(r.e,path.string().c_str()),SWMM_OK);finish(r);
    Deck resumed=open("source_restart_read",body);ASSERT_TRUE(resumed.opened);ASSERT_EQ(swmm_engine_initialize(resumed.e),SWMM_OK);
    SWMM_HotStart hs=nullptr;ASSERT_EQ(swmm_hotstart_open(path.string().c_str(),&hs),SWMM_OK);ASSERT_EQ(swmm_hotstart_apply(resumed.e,hs),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(resumed.e,1),SWMM_OK);resumed.started=true;
    const auto& next=resumed.eng->surfaceRouter2D().subsurface();
    EXPECT_DOUBLE_EQ(next.state().led_source_in,waterIn);EXPECT_DOUBLE_EQ(next.state().led_source_out,waterOut);
    EXPECT_DOUBLE_EQ(next.transport().gained_source[0],massIn);EXPECT_DOUBLE_EQ(next.transport().lost_source[0],massOut);
    EXPECT_NEAR(next.transport().residual(0),0,1e-7);
    double elapsed=0;
    for(int i=0;i<20;++i){ASSERT_EQ(swmm_engine_step(resumed.e,&elapsed),SWMM_OK);if(elapsed<=0)break;}
    resumed.eng->surfaceRouter2D().flushPendingBatch(resumed.eng->context());
    EXPECT_GT(next.state().led_source_in,waterIn);
    EXPECT_GT(next.transport().gained_source[0],massIn);
    EXPECT_NEAR(next.transport().residual(0),0,1e-7);
    swmm_hotstart_close(hs);finish(resumed);
}
TEST(GwTransportKernel, FlowReversalCannotWithdrawFutureInjectionAndClosesAcrossCadences) {
    double firstIn[2]={},firstOut[2]={};
    for(int cadence=0;cadence<2;++cadence) for(int reverse=0;reverse<2;++reverse){
        const std::string values=reverse ? "1\nWQ 0:00:03 -1\nWQ 0:00:06 0" : "-1\nWQ 0:00:03 1\nWQ 0:00:06 0";
        auto body=forcingDeck("[TIMESERIES]\nWQ 0:00:00 "+values+"\n\n[GW_SOURCES]\nW * FLOW WQ TSS CONC 2\n\n",0);
        if(cadence){auto at=body.find("MAX_TIMESTEP 5");body.replace(at,14,"MAX_TIMESTEP 1");}
        Deck r=open("reversal_"+std::to_string(cadence)+"_"+std::to_string(reverse),body);ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r));
        const auto& gw=r.eng->surfaceRouter2D().subsurface();
        EXPECT_NEAR(gw.state().led_source_in,reverse?.75:2.25,1e-6);
        if(!reverse)EXPECT_NEAR(gw.state().led_source_out,0,1e-6);
        else EXPECT_GT(gw.state().led_source_out,.5);
        EXPECT_NEAR(gw.state().continuityResidual(),0,1e-7);
        EXPECT_NEAR(gw.transport().residual(0),0,1e-7);
        if(!cadence){firstIn[reverse]=gw.state().led_source_in;firstOut[reverse]=gw.state().led_source_out;}
        else {EXPECT_NEAR(gw.state().led_source_in,firstIn[reverse],1e-6);EXPECT_NEAR(gw.state().led_source_out,firstOut[reverse],1e-5);}
        finish(r);
    }
}
TEST(GwTransportKernel, CoupledGroundwaterRejectsUnsupportedRk2Route) {
    auto body=forcingDeck("");const auto at=body.find("[2D_OPTIONS]\n");body.insert(at+13,"RECONSTRUCTION_ORDER 2\n");
    Deck r=open("groundwater_rk2",body);ASSERT_TRUE(r.opened);EXPECT_NE(swmm_engine_initialize(r.e),SWMM_OK);finish(r);
}
TEST(GwTransportKernel, AquiferOptionSnapshotsPreserveFullDoublePrecision) {
    Deck r=open("aquifer_option_precision",forcingDeck(""));ASSERT_TRUE(r.opened);
    for(const char* key : {"C_GW","C_COL"}) {
        constexpr double exact=.12345678901234566;
        ASSERT_EQ(swmm_gw2d_option_set(r.e,key,"0.12345678901234566"),SWMM_OK);
        char buffer[128]={};ASSERT_EQ(swmm_gw2d_option_get(r.e,key,buffer,sizeof buffer),SWMM_OK);
        EXPECT_DOUBLE_EQ(std::stod(buffer),exact);
        ASSERT_EQ(swmm_gw2d_option_set(r.e,key,"0.5"),SWMM_OK);
        ASSERT_EQ(swmm_gw2d_option_set(r.e,key,buffer),SWMM_OK);
        ASSERT_EQ(swmm_gw2d_option_get(r.e,key,buffer,sizeof buffer),SWMM_OK);
        EXPECT_DOUBLE_EQ(std::stod(buffer),exact);
    }
    finish(r);
}
TEST(GwTransportKernel, AuthoringTagGettersWorkBeforeRuntimeInitialization) {
    Deck r=open("authoring_tag_snapshot",forcingDeck(""));ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_2d_set_triangle_tag(r.e,0,"selected-groundwater"),SWMM_OK);
    ASSERT_EQ(swmm_2d_set_vertex_tag(r.e,0,"survey"),SWMM_OK);
    char text[128]={};ASSERT_EQ(swmm_2d_get_triangle_tag(r.e,0,text,sizeof text),SWMM_OK);EXPECT_STREQ(text,"selected-groundwater");
    ASSERT_EQ(swmm_2d_get_vertex_tag(r.e,0,text,sizeof text),SWMM_OK);EXPECT_STREQ(text,"survey");
    EXPECT_EQ(swmm_2d_get_triangle_tag(r.e,-1,text,sizeof text),SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_2d_get_vertex_tag(r.e,999,text,sizeof text),SWMM_ERR_BADINDEX);
    finish(r);
}
TEST(GwTransportKernel, GroundwaterHdfUnitsFollowNativeSpeciesEvenWhenSurfaceTransportIsOff) {
    const char* inputUnits[]={"MG/L","UG/L","#/L"};
    const char* outputUnits[]={"MG/L","UG/L","#/L"};
    for(int variant=0;variant<3;++variant) {
        const auto file="gw_native_units_"+std::to_string(variant)+".h5";
        auto body=forcingDeck("[GW_TRANSPORT_OPTIONS]\nTRANSPORT_POLLUTANTS YES\nTRANSPORT_AGE YES\n\n"
            "[GW_INITIAL_QUALITY]\n* SAT __WATER_AGE__ 3600\n* UNSAT __WATER_AGE__ 3600\n\n"
            "[GW_SOURCES]\nW * FLOW 0.0001 __WATER_AGE__ CONC 7200\n\n");
        auto at=body.find("TSS  MG/L");body.replace(at,9,std::string("TSS  ")+inputUnits[variant]);
        body.insert(body.find("[POLLUTANTS]"),"[OPTIONS]\nWATER_AGE ON\n\n");
        at=body.find("REPORT_2D NO");body.replace(at,12,"REPORT_2D YES\nOUTPUT_FILE "+file+"\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES DEPTH GROUNDWATER\nTRANSPORT_POLLUTANTS NO\nTRANSPORT_AGE NO");
        Deck r=open("gw_native_units_"+std::to_string(variant),body);ASSERT_TRUE(r.opened);
        ASSERT_EQ(r.eng->context().pollutants.units[0],static_cast<openswmm::MassUnits>(variant));
        ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK);ASSERT_EQ(swmm_engine_start(r.e,1),SWMM_OK);r.started=true;
        const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& tr=gw.transport();ASSERT_GE(tr.age_row,0);
        EXPECT_NEAR(tr.sat_mass[tr.idx(tr.age_row,0)]/gw.satVolume(0),3600,1e-10);
        double elapsed=0;int status=0;while((status=swmm_engine_step(r.e,&elapsed))==SWMM_OK&&elapsed>0){}
        ASSERT_EQ(status,SWMM_OK);ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
        EXPECT_NEAR(tr.gained_source[static_cast<std::size_t>(tr.age_row)],864,1e-7);
        finish(r);
        const hid_t h5=H5Fopen((kOutDir/file).string().c_str(),H5F_ACC_RDONLY,H5P_DEFAULT);ASSERT_GE(h5,0);
        EXPECT_LE(H5Lexists(h5,"Mesh2_face_species_conc",H5P_DEFAULT),0);
        for(const char* field:{"Mesh2_face_gw_sat_conc","Mesh2_face_gw_unsat_conc"}) {
            const hid_t ds=H5Dopen2(h5,field,H5P_DEFAULT);ASSERT_GE(ds,0);
            auto attr=[&](const char* key){
                const hid_t a=H5Aopen(ds,key,H5P_DEFAULT);if(a<0)return std::string{};
                const hid_t t=H5Aget_type(a);std::vector<char> bytes(H5Tget_size(t)+1,0);
                const auto ok=H5Aread(a,t,bytes.data());H5Tclose(t);H5Aclose(a);
                return ok<0?std::string{}:std::string(bytes.data());
            };
            EXPECT_EQ(attr("species_names"),"TSS,__WATER_AGE__");
            EXPECT_EQ(attr("species_units"),std::string(outputUnits[variant])+",s");
            H5Dclose(ds);
        }
        H5Fclose(h5);
    }
}

// ---------------------------------------------------------------------------
// Review F21 (2026-10-15): the 1D ⇄ aquifer seam for a reaction-component
// species. Seeded like TSS, the species must cross both inbound seams and the
// outbound one exactly as the pollutant does. At base an MSX row left the
// aquifer as `lost_node` and reached nothing, and a leaking conduit delivered
// none (its link concentration was never read).
// ---------------------------------------------------------------------------
namespace {
std::string withSpeciesX(std::string body, const std::string& tag) {
    fs::create_directories(kOutDir);
    { std::ofstream f(kOutDir / (tag + ".rxn"));
      f << "[REACTION_OPTIONS]\nSOLVER RK5\n[REACTION_SPECIES]\nBULK X MG\n"
           "[REACTION_PIPES]\nRATE X 0\n[REACTION_TANKS]\nRATE X 0\n"; }
    const std::string iq = "[GW_INITIAL_QUALITY]\n;;Scope Zone Species Value\n";
    const auto at = body.find(iq);
    if (at != std::string::npos)
        body.insert(at + iq.size(), "*       SAT    X  5.0\n*       UNSAT  X  2.0\nCELL 1  SAT    X  25.0\n");
    // Species rain in clean while TSS rains at 10 mg/L; take rain off the mesh
    // so the two differ only through the seam under test.
    const std::string rain = "RAINFALL_MODE SYSTEM";
    const auto rp = body.find(rain);
    if (rp != std::string::npos) body.replace(rp, rain.size(), "RAINFALL_MODE NONE");
    body.insert(body.find("[REPORT]"),
                "[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"" + tag + ".rxn\"\n\n");
    return body;
}
}  // namespace

TEST(GwTransportKernel, DrainingAquiferHandsReactionSpeciesToTheNode) {
    Deck r = open("seam_node_out_msx", withSpeciesX(seamDeck(0.95, 0.0, 0.0, 0.0), "seam_node_out_msx"));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    const auto& ctx = r.eng->context();
    const int j1 = ctx.node_names.find("J1");
    const auto np = static_cast<std::size_t>(ctx.n_pollutants());
    // The junction is fed only by the bed. Seeded identically, the species
    // must track TSS there at every step while the aquifer drains.
    double peak_tss = 0.0, worst = 0.0, elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
        const auto ns = static_cast<std::size_t>(ctx.reactions.n_species());
        const double c_tss = ctx.nodes.conc[static_cast<std::size_t>(j1) * np];
        const double c_x = ns ? ctx.reactions.msx_node_conc[static_cast<std::size_t>(j1) * ns] : 0.0;
        peak_tss = std::max(peak_tss, c_tss);
        worst = std::max(worst, std::fabs(c_x - c_tss));
    }
    const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
    ASSERT_EQ(t.n_msx, 1) << "the aquifer does not transport the species";
    const auto x = static_cast<std::size_t>(t.n_pollut);
    ASSERT_GT(t.lost_node[0], 0.0) << "the aquifer drained no TSS; deck defect";
    ASSERT_GT(peak_tss, 1.0) << "the junction never carried TSS; deck defect";
    EXPECT_NEAR(t.lost_node[x], t.lost_node[0], 1e-9 * t.lost_node[0])
        << "the species left the aquifer differently from TSS";
    EXPECT_LT(worst, 1e-9 * peak_tss)
        << "species and TSS differ by up to " << worst << " at J1 (peak TSS " << peak_tss << ")";
    finish(r);
}

TEST(GwTransportKernel, LeakingConduitCarriesReactionSpeciesIntoTheAquifer) {
    std::string body = seamDeck(0.5, 0.0, 100.0, 0.0);
    const std::string anchor = "[2D_AQUIFER_OPTIONS]\n";
    body.insert(body.find(anchor) + anchor.size(), "LINK_SEEPAGE ONE_WAY\n");
    Deck r = open("seam_link_msx", withSpeciesX(body, "seam_link_msx"));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& t = r.eng->surfaceRouter2D().subsurface().transport();
    const auto& led = r.eng->surfaceRouter2D().subsurface().state();
    ASSERT_EQ(t.n_msx, 1);
    const auto x = static_cast<std::size_t>(t.n_pollut);
    ASSERT_GT(led.led_link, 0.0) << "no seepage water; deck defect";
    ASSERT_GT(t.gained_link[0], 0.0) << "TSS control delivered nothing";
    EXPECT_GT(t.gained_link[x], 0.0) << "the conduit's species never reached the aquifer";
    EXPECT_LE(t.gained_link[x] / led.led_link, 25.0 * (1.0 + 1e-9));
    finish(r);
}

// ---------------------------------------------------------------------------
// Review (2026-10-15): a GAINING conduit delivers the aquifer's quality.
// High table, no bed, clean pipe inflow, no rain on the mesh: the only
// source of TSS or species X in the network is the water the conduit gains.
// At base the water arrived clean and the aquifer booked the mass as
// `lost_link` with no receiver. The identity: everything the aquifer booked
// out through gaining conduits (debited, still pending, or short) equals
// what the 1D booked in as groundwater inflow plus what is still pending.
// ---------------------------------------------------------------------------
TEST(GwTransportKernel, GainingConduitDeliversTheAquiferQualityToTheNetwork) {
    std::string body = deck(Mesh::Tri, "", 0.95);
    const std::string anchor = "[2D_AQUIFER_OPTIONS]\n";
    body.insert(body.find(anchor) + anchor.size(), "LINK_SEEPAGE TWO_WAY\n");
    body.insert(body.find("[REPORT]\nINPUT NO\n"),
                "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n\n"
                "[TIMESERIES]\nIN1  0:00  0.01\nIN1  1:00  0.01\n\n"
                "[LOSSES]\n;;Link Kentry Kexit Kavg Flap Seepage\nC1  0 0 0 NO  100\n\n");
    Deck r = open("seam_link_gain", withSpeciesX(body, "seam_link_gain"));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    const auto& ctx = r.eng->context();
    const int j1 = ctx.node_names.find("J1");
    ASSERT_GE(j1, 0);
    const auto uj = static_cast<std::size_t>(j1);
    const auto np = static_cast<std::size_t>(ctx.n_pollutants());
    double peak_tss = 0.0, peak_x = 0.0, elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
        const auto ns = static_cast<std::size_t>(ctx.reactions.n_species());
        peak_tss = std::max(peak_tss, ctx.nodes.conc[uj * np]);
        if (ns) peak_x = std::max(peak_x, ctx.reactions.msx_node_conc[uj * ns]);
    }
    const auto& gw = r.eng->surfaceRouter2D().subsurface();
    const auto& t = gw.transport();
    const double led_link = gw.state().led_link;
    ASSERT_LT(led_link, 0.0) << "the conduit never gained; deck defect";
    ASSERT_GT(t.lost_link[0], 0.0) << "no TSS left the aquifer through the conduit";
    EXPECT_GT(peak_tss, 0.1) << "J1 never carried the aquifer's TSS";
    EXPECT_LE(peak_tss, 25.0 * (1.0 + 1e-9));
    EXPECT_GT(peak_x, 0.1) << "J1 never carried the aquifer's species";
    // Cross-domain identity, TSS, in 1D units.
    const double f21 = r.eng->surfaceRouter2D().options().flow_2d_to_1d;
    double pend_2d = 0.0;
    for (int c = 0; c < t.n_cells; ++c) pend_2d += t.lout_mass[t.idx(0, c)];
    const double aq_out = (t.lost_link[0] + pend_2d + t.link_gain_shortfall[0]) * f21;
    double pend_1d = 0.0;
    for (std::size_t i = 0; i < ctx.link_gain_quality.mass.size(); i += np)
        pend_1d += ctx.link_gain_quality.mass[i];
    const double gw_in = ctx.mass_balance.qual_routing_gw_in[0];
    EXPECT_NEAR(gw_in + pend_1d, aq_out, 1e-9 * aq_out)
        << "1D received " << gw_in << " + pending " << pend_1d
        << ", aquifer gave " << aq_out;
    EXPECT_EQ(t.link_gain_shortfall[0], 0.0);
    finish(r);
}

// Multispecies Phase 2 (S4, review D7): reaction species rain onto the mesh
// at their [REACTION_RAINFALL] concentration. They used to rain in clean.
// Seeded and rained exactly like TSS (10 mg/L), X must track TSS on every
// mesh cell at every step.
TEST(GwTransportKernel, SpeciesRainOnTheMeshAtTheirRainfallConcentration) {
    std::string body = deck(Mesh::Tri);
    fs::create_directories(kOutDir);
    { std::ofstream f(kOutDir / "mesh_rain_msx.rxn");
      f << "[REACTION_OPTIONS]\nSOLVER RK5\n[REACTION_SPECIES]\nBULK X MG\n"
           "[REACTION_PIPES]\nRATE X 0\n[REACTION_TANKS]\nRATE X 0\n"
           "[REACTION_RAINFALL]\nX  10.0\n"; }
    const std::string iq = "[GW_INITIAL_QUALITY]\n;;Scope Zone Species Value\n";
    body.insert(body.find(iq) + iq.size(), "*       SAT    X  5.0\n*       UNSAT  X  2.0\nCELL 1  SAT    X  25.0\n");
    body.insert(body.find("[REPORT]"),
                "[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"mesh_rain_msx.rxn\"\n\n");
    Deck r = open("mesh_rain_msx", body);
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    const auto& tr = r.eng->surfaceRouter2D().state().transport;
    ASSERT_EQ(tr.n_msx, 1);
    const int x = tr.n_pollut;
    double worst = 0.0, peak = 0.0, elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0)
        for (int c = 0; c < tr.n_cells; ++c) {
            const double a = tr.cell_mass[tr.idx(0, c)], b = tr.cell_mass[tr.idx(x, c)];
            peak = std::max(peak, std::fabs(a));
            worst = std::max(worst, std::fabs(a - b));
        }
    ASSERT_GT(peak, 0.0) << "no TSS on the mesh; deck defect";
    EXPECT_LE(worst, 1e-12 * peak) << "X and TSS differ by up to " << worst << " (peak " << peak << ")";
    finish(r);
}


// Groundwater completion G0: positive capabilities, not acceptance of inert rows.
TEST(GwCompletion, BoundaryFlowCarriesWaterAndSpeciesWithoutBecomingAWell) {
    for (const auto shape : {Mesh::Tri, Mesh::Quad, Mesh::Mixed}) {
        SCOPED_TRACE(meshName(shape));
        const auto body=forcingDeck(
            "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST 0.0001\n\n"
            "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n\n", .3, shape);
        Deck r=open(std::string("completion_boundary_")+meshName(shape),body);
        ASSERT_TRUE(r.opened) << swmm_get_last_error_msg(r.e);
        ASSERT_TRUE(run(r)) << swmm_get_last_error_msg(r.e);
        ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
        const auto& gw=r.eng->surfaceRouter2D().subsurface();
        const double water=.0001*r.eng->context().current_time;
        EXPECT_NEAR(gw.state().led_lateral,water,1e-9);
        EXPECT_NEAR(gw.transport().net_lateral[0],3*water,1e-9);
        EXPECT_DOUBLE_EQ(gw.state().led_source_in,0);
        EXPECT_DOUBLE_EQ(gw.transport().gained_source[0],0);
        EXPECT_NEAR(gw.state().continuityResidual(),0,1e-9);
        EXPECT_NEAR(gw.transport().residual(0),0,1e-9);
        finish(r);
    }
}

TEST(GwCompletion, HydraulicBoundaryRejectsAnInteriorEdge) {
    Deck r=open("completion_interior_boundary",forcingDeck(
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 1 FLOW CONST 0.0001\n\n[GW_BOUNDARY_QUALITY]\n1 1 TSS CONC 3\n\n"));
    ASSERT_TRUE(r.opened) << swmm_get_last_error_msg(r.e);
    EXPECT_NE(swmm_engine_initialize(r.e),SWMM_OK);
    EXPECT_NE(std::string(swmm_get_last_error_msg(r.e)).find("exterior"),std::string::npos);
    finish(r);
}

TEST(GwCompletion, LayeredInitialConditionsRequireActualSigmaTransport) {
    auto body=forcingDeck(
        "[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT LAYERED\n\n"
        "[GW_INITIAL_QUALITY]\n* LAYER 1 TSS 7\n* LAYER 2 TSS 11\n\n");
    auto at=body.find("CLOSURE CLOSED_FORM");
    ASSERT_NE(at,std::string::npos);body.replace(at,19,"CLOSURE SIGMA");
    Deck r=open("completion_layered_initial",body);
    ASSERT_TRUE(r.opened) << swmm_get_last_error_msg(r.e);
    EXPECT_EQ(swmm_engine_initialize(r.e),SWMM_OK) << swmm_get_last_error_msg(r.e);
    finish(r);
}

TEST(GwCompletion, BoundaryWithdrawalIsLimitedAndItsNativeReceiptsArePublic) {
    Deck r=open("completion_boundary_drain",forcingDeck(
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST -1\n\n",.1));
    ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r))<<swmm_get_last_error_msg(r.e);
    ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    double ri=-1,ro=-1,ai=-1,ao=-1;
    ASSERT_EQ(swmm_gw2d_boundary_water(r.e,0,&ri,&ro,&ai,&ao),SWMM_OK);
    EXPECT_DOUBLE_EQ(ri,0);EXPECT_DOUBLE_EQ(ai,0);
    EXPECT_GT(ro,1000);EXPECT_GT(ao,0);EXPECT_LT(ao,20);
    const auto& gw=r.eng->surfaceRouter2D().subsurface();
    EXPECT_NEAR(gw.state().led_lateral,-ao,1e-10);
    EXPECT_NEAR(gw.state().continuityResidual(),0,1e-9);
    EXPECT_NEAR(gw.transport().residual(0),0,1e-9);
    double mi=-1,mo=-1,fi=-1,fo=-1;
    ASSERT_EQ(swmm_gw2d_boundary_species(r.e,0,0,&mi,&mo,&fi,&fo),SWMM_OK);
    EXPECT_DOUBLE_EQ(mi,0);EXPECT_GT(mo,0);EXPECT_DOUBLE_EQ(fi,0);EXPECT_DOUBLE_EQ(fo,0);
    EXPECT_NEAR(gw.transport().net_lateral[0],-mo,1e-9);
    finish(r);
}
TEST(GwCompletion, EdgeMassFluxUsesPhysicalLengthWithoutInventingWater) {
    Deck r=open("completion_boundary_mass",forcingDeck(
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST 0\n\n"
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS MASSFLUX 2\n\n"));
    ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r))<<swmm_get_last_error_msg(r.e);
    ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& gw=r.eng->surfaceRouter2D().subsurface();
    // Two native amount/s/metre on the 10 metre edge, with 1000 L per m3.
    const double expected=.001*2*10*r.eng->context().current_time;
    EXPECT_DOUBLE_EQ(gw.state().led_lateral,0);
    EXPECT_NEAR(gw.transport().net_lateral[0],expected,1e-9);
    EXPECT_DOUBLE_EQ(gw.transport().gained_source[0],0);
    EXPECT_NEAR(gw.transport().residual(0),0,1e-9);
    finish(r);
}
TEST(GwCompletion, BoundaryReversalKeepsGrossTransfersAndChronologicalDonors) {
    for(int cadence:{1,5})for(int positive_first:{0,1}) {
        const std::string values=positive_first?"1\nBQ 0:00:03 -1\nBQ 0:00:06 0":"-1\nBQ 0:00:03 1\nBQ 0:00:06 0";
        auto body=forcingDeck("[TIMESERIES]\nBQ 0:00:00 "+values+"\n\n"
            "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW SERIES BQ\n\n"
            "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n\n",0);
        if(cadence==1){auto at=body.find("MAX_TIMESTEP 5");body.replace(at,14,"MAX_TIMESTEP 1");}
        Deck r=open("completion_boundary_reversal_"+std::to_string(cadence)+"_"+std::to_string(positive_first),body);
        ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r))<<swmm_get_last_error_msg(r.e);
        ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
        const auto& gw=r.eng->surfaceRouter2D().subsurface();
        EXPECT_NEAR(gw.state().led_boundary_in,positive_first?.75:2.25,1e-7);
        if(!positive_first)EXPECT_NEAR(gw.state().led_boundary_out,0,1e-8);
        else EXPECT_GT(gw.state().led_boundary_out,.1);
        EXPECT_NEAR(gw.transport().boundary_in[0],3*gw.state().led_boundary_in,1e-8);
        EXPECT_NEAR(gw.state().continuityResidual(),0,1e-8);
        EXPECT_NEAR(gw.transport().residual(0),0,1e-8);
        finish(r);
    }
}
TEST(GwCompletion, BoundaryHeadAtRestAndMissingCompositionHaveDistinctBehavior) {
    Deck r=open("completion_boundary_head",forcingDeck(
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 HEAD CONST -0.7\n\n"
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n\n",.3));
    ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r))<<swmm_get_last_error_msg(r.e);
    const auto& gw=r.eng->surfaceRouter2D().subsurface();
    EXPECT_NEAR(gw.state().led_boundary_in,0,1e-12);
    EXPECT_NEAR(gw.state().led_boundary_out,0,1e-12);finish(r);
    Deck missing=open("completion_boundary_composition",forcingDeck(
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 HEAD CONST -0.6\n\n"));
    ASSERT_TRUE(missing.opened);EXPECT_NE(swmm_engine_initialize(missing.e),SWMM_OK);
    EXPECT_NE(std::string(swmm_get_last_error_msg(missing.e)).find("composition"),std::string::npos);
    finish(missing);
}

namespace {
std::string completionErrors(const Deck& r) {
    std::string text=swmm_get_last_error_msg(r.e)?swmm_get_last_error_msg(r.e):"";
    for(int n=0;n<swmm_get_error_count(r.e);++n)if(const char* error=swmm_get_error_at(r.e,n))text+=error;
    return text;
}
std::string layeredDeck(const std::string& extra,Mesh mesh=Mesh::Tri,double hg=.3) {
    auto body=forcingDeck("[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT LAYERED\nDISPERSION NO\n\n"+extra,hg,mesh);
    const auto at=body.find("CLOSURE CLOSED_FORM");
    if(at!=std::string::npos)body.replace(at,19,"CLOSURE SIGMA");
    const auto seed=body.find("[GW_INITIAL_QUALITY]");
    const auto next=body.find('[',seed+1);
    body.erase(seed,next-seed);
    return body;
}
}
TEST(GwCompletion, LayerScopesAndSorptionSeedDissolvedConcentrations) {
    Deck r=open("layer_scope_sorption",layeredDeck(
        "[GW_INITIAL_QUALITY]\n* LAYER 1 TSS 7\nCELL 1 UNSAT TSS 3\nCELL 1 LAYER 2 TSS 11\n\n"
        "[GW_SORPTION]\n* TSS 2 0\n\n"));
    ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();const auto& w=gw.state();
    ASSERT_GT(t.n_layers,1);
    for(int i=0;i<t.n_cells;++i)for(int j=0;j<t.n_layers;++j) {
        const double capacity=w.area[i]*(w.zs[i]-w.hg[i])/t.n_layers*
            (w.theta_sigma[std::size_t(j)*t.n_cells+i]+t.rho_b[i]*.002);
        const double expected=i==0?(j==1?11:3):(j==0?7:0);
        EXPECT_NEAR(t.layer_mass[t.layerIdx(0,j,i)]/capacity,expected,1e-12);
    }
    finish(r);
}
TEST(GwCompletion, LayeredMovingTableKeepsUniformDissolvedQualityAndMass) {
    for(const Mesh mesh:{Mesh::Tri,Mesh::Quad,Mesh::Mixed})for(const std::string rate:{"0.001","-0.001"}) {
        Deck r=open(std::string("layer_uniform_")+meshName(mesh)+(rate[0]=='-'?"_out":"_in"),layeredDeck(
            "[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n\n"
            "[GW_SORPTION]\n* TSS 2 0\n\n"
            "[GW_SOURCES]\nW * FLOW "+rate+" TSS CONC 7\n\n",mesh));
        ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_TRUE(run(r))<<completionErrors(r);
        ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
        const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();const auto& w=gw.state();
        std::vector<double> bulk(t.n_cells);int written=0;
        ASSERT_EQ(swmm_gw2d_get_cell_conc(r.e,SWMM_GW2D_ZONE_SAT,0,bulk.data(),t.n_cells,&written),SWMM_OK);
        for(int i=0;i<t.n_cells;++i)if(gw.satVolume(i)>1e-10)EXPECT_NEAR(bulk[i],7,1e-8);
        ASSERT_EQ(swmm_gw2d_get_cell_conc(r.e,SWMM_GW2D_ZONE_UNSAT,0,bulk.data(),t.n_cells,&written),SWMM_OK);
        for(int i=0;i<t.n_cells;++i)if(gw.unsatVolume(i)>1e-10)EXPECT_NEAR(bulk[i],7,1e-8);
        for(int i=0;i<t.n_cells;++i) {
            const double capacity=w.area[i]*w.hg[i]*(w.theta_s[i]+t.rho_b[i]*.002);
            if(capacity>1e-10)EXPECT_NEAR(t.sat_mass[t.idx(0,i)]/capacity,7,1e-8);
            double sum=0;
            for(int j=0;j<t.n_layers;++j) {
                const double cap=w.area[i]*(w.zs[i]-w.hg[i])/t.n_layers*
                    (w.theta_sigma[std::size_t(j)*t.n_cells+i]+t.rho_b[i]*.002);
                const double mass=t.layer_mass[t.layerIdx(0,j,i)];sum+=mass;
                if(cap>1e-10)EXPECT_NEAR(mass/cap,7,1e-8);
            }
            EXPECT_DOUBLE_EQ(sum,t.unsat_mass[t.idx(0,i)]);
        }
        EXPECT_NEAR(t.residual(0),0,1e-8)<<ledgerDump(t,0);
        EXPECT_NEAR(w.continuityResidual(),0,1e-9);finish(r);
    }
}
TEST(GwCompletion, NativeRestartRestoresLayersAndBoundaryReceiptsOrRefusesIdentityMismatch) {
    const auto body=layeredDeck("[GW_INITIAL_QUALITY]\n* LAYER 1 TSS 7\n* LAYER 2 TSS 11\n\n"
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST 0.0001\n\n"
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n\n");
    const auto path=kOutDir/"completion_layers.hsf";
    Deck first=open("completion_restart_write",body);ASSERT_TRUE(first.opened)<<completionErrors(first);ASSERT_TRUE(run(first))<<completionErrors(first);
    const auto expected=first.eng->surfaceRouter2D().subsurface().transportRestartState();
    ASSERT_FALSE(expected.empty());ASSERT_EQ(swmm_hotstart_save(first.e,path.string().c_str()),SWMM_OK);
    finish(first);
    SWMM_HotStart hs=nullptr;ASSERT_EQ(swmm_hotstart_open(path.string().c_str(),&hs),SWMM_OK);
    Deck next=open("completion_restart_read",body);ASSERT_TRUE(next.opened);
    ASSERT_EQ(swmm_engine_initialize(next.e),SWMM_OK);
    ASSERT_EQ(swmm_hotstart_apply(next.e,hs),SWMM_OK)<<swmm_get_last_error_msg(next.e);
    ASSERT_EQ(swmm_engine_start(next.e,1),SWMM_OK);next.started=true;
    EXPECT_EQ(next.eng->surfaceRouter2D().subsurface().transportRestartState(),expected);
    finish(next);
    auto changed=body;const auto at=changed.find("FLOW CONST 0.0001");changed.replace(at,17,"FLOW CONST 0.0002");
    Deck wrong=open("completion_restart_wrong",changed);ASSERT_TRUE(wrong.opened);
    ASSERT_EQ(swmm_engine_initialize(wrong.e),SWMM_OK);
    const auto original=wrong.eng->surfaceRouter2D().subsurface().transportRestartState();
    EXPECT_NE(swmm_hotstart_apply(wrong.e,hs),SWMM_OK);
    EXPECT_EQ(wrong.eng->surfaceRouter2D().subsurface().transportRestartState(),original);
    finish(wrong);swmm_hotstart_close(hs);
}

TEST(GwCompletion, ExplicitGroundwaterReactorsConvertAIntoBAndCloseEachLedger) {
    const std::string reactions=
        "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-12\nRTOL 1e-10\n\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n\n"
        "[REACTION_SUBSURFACE]\n* SAT A RATE -0.001*A\n* SAT B RATE 0.001*A\n"
        "* UNSAT A RATE -0.001*A\n* UNSAT B RATE 0.001*A\n\n"
        "[GW_INITIAL_QUALITY]\n* SAT A 7\n* UNSAT A 7\n\n";
    for(bool layers:{false,true}) {
        Deck r=open(layers?"reactor_layers":"reactor_bulk",layers?layeredDeck(reactions):forcingDeck(reactions));
        ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_TRUE(run(r))<<completionErrors(r);
        ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
        const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();
        const int a=t.rowIndex("A"),b=t.rowIndex("B");ASSERT_GE(a,0);ASSERT_GE(b,0);
        const double initial=t.init_mass[a],remaining=initial*std::exp(-.001*r.eng->context().current_time);
        EXPECT_NEAR(t.ledgeredStorage(a),remaining,initial*1e-8);
        EXPECT_NEAR(t.ledgeredStorage(a)+t.ledgeredStorage(b),initial,initial*1e-10);
        EXPECT_NEAR(t.residual(a),0,1e-9);EXPECT_NEAR(t.residual(b),0,1e-9);
        EXPECT_GT(t.lost_reaction[a],0);EXPECT_LT(t.lost_reaction[b],0);
        finish(r);
    }
}
TEST(GwCompletion, GroundwaterVariablesWorkThroughTermsAndPipeAndWallDependenciesAreRefused) {
    const std::string prefix="[REACTION_SPECIES]\nBULK A MG\nWALL W MG/FT2\n\n"
        "[REACTION_TERMS]\nsoil GW_THETA/GW_POROSITY+GW_SE*0+GW_DEPTH_M*0\n\n";
    for(const std::string expression:{"-0.001*A*soil","-A*HRT","-A*W","-A*Q"}) {
        Deck r=open("reactor_context_"+std::to_string(expression.size()),forcingDeck(prefix+
            "[REACTION_SUBSURFACE]\n* SAT A RATE "+expression+"\n\n"));
        if(expression=="-0.001*A*soil") {
            ASSERT_TRUE(r.opened)<<completionErrors(r);
            char message[256];int column=0;
            EXPECT_EQ(swmm_reaction_validate_expression(r.e,SWMM_RXN_SCOPE_SUBSURFACE,expression.c_str(),message,sizeof message,&column),SWMM_OK)<<message;
            EXPECT_NE(swmm_reaction_validate_expression(r.e,SWMM_RXN_SCOPE_PIPE,expression.c_str(),message,sizeof message,&column),SWMM_OK);

            EXPECT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        } else EXPECT_FALSE(r.opened)<<expression;
        finish(r);
    }
    Deck network=open("reactor_ground_variable_in_pipe",forcingDeck(prefix+
        "[REACTION_PIPES]\nRATE A -A*soil\n\n"));
    EXPECT_FALSE(network.opened);finish(network);
}

TEST(GwCompletion, MolecularDiffusionRunsAtHydrostaticRest) {
    Deck r=open("completion_diffusion_at_rest",forcingDeck(
        "[GW_INITIAL_QUALITY]\nCELL 1 SAT TSS 10\nCELL 2 SAT TSS 0\n\n"
        "[GW_TRANSPORT_PARAMS]\n* - - - - 0 0 0.01 - -\n\n"));
    ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_TRUE(run(r))<<completionErrors(r);
    ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();
    const double a=t.sat_mass[t.idx(0,0)]/gw.satVolume(0),b=t.sat_mass[t.idx(0,1)]/gw.satVolume(1);
    EXPECT_LT(a,9.9);EXPECT_GT(b,.1);EXPECT_GE(b,0);EXPECT_LE(a,10);
    EXPECT_NEAR(t.residual(0),0,1e-9);EXPECT_NEAR(gw.state().continuityResidual(),0,1e-9);finish(r);
}

TEST(GwCompletion, LayerAndEdgeResultsPersistCanonicalInventoriesAndGrossReceipts) {
    auto body=layeredDeck("[GW_INITIAL_QUALITY]\n* UNSAT TSS 7\n\n"
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST 0.0001\n\n"
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n\n");
    const auto at=body.find("REPORT_2D NO");ASSERT_NE(at,std::string::npos);
    body.replace(at,12,"REPORT_2D YES\nOUTPUT_FILE completion_layers.h5\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES DEPTH GROUNDWATER");
    Deck r=open("completion_layer_results",body);ASSERT_TRUE(r.opened)<<completionErrors(r);
    ASSERT_TRUE(run(r))<<completionErrors(r);
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto expected=gw.transport().layer_mass;
    const int ns=gw.transport().n_species,nl=gw.transport().n_layers,nc=gw.transport().n_cells;
    double water=-1,mass=-2,conc=-3;
    ASSERT_EQ(swmm_gw2d_get_layer_quality(r.e,0,0,0,&water,&mass,&conc),SWMM_OK);
    EXPECT_DOUBLE_EQ(mass,expected[0]);EXPECT_GE(water,0);EXPECT_GE(conc,0);
    EXPECT_NE(swmm_gw2d_get_layer_quality(r.e,-1,0,0,&water,&mass,&conc),SWMM_OK);
    EXPECT_DOUBLE_EQ(mass,expected[0]);
    finish(r);
    hid_t file=H5Fopen((kOutDir/"completion_layers.h5").string().c_str(),H5F_ACC_RDONLY,H5P_DEFAULT);
    ASSERT_GE(file,0);
    for(const char* name:{"Mesh2_face_gw_layer_mass","Mesh2_face_gw_layer_conc","groundwater_boundary_species"}) {
        hid_t dataset=H5Dopen2(file,name,H5P_DEFAULT);ASSERT_GE(dataset,0)<<name;
        hid_t space=H5Dget_space(dataset);EXPECT_EQ(H5Sget_simple_extent_ndims(space),4);
        hsize_t dims[4]={};H5Sget_simple_extent_dims(space,dims,nullptr);EXPECT_GT(dims[0],0);
        EXPECT_EQ(dims[1],ns);
        if(std::string(name)=="Mesh2_face_gw_layer_mass") {
            EXPECT_EQ(dims[2],nl);EXPECT_EQ(dims[3],nc);
            std::vector<double> values(std::size_t(dims[0])*ns*nl*nc);
            ASSERT_GE(H5Dread(dataset,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,values.data()),0);
            const auto offset=values.size()-expected.size();
            for(std::size_t k=0;k<expected.size();++k)EXPECT_NEAR(values[offset+k],expected[k],1e-9);
        }
        H5Sclose(space);H5Dclose(dataset);
    }
    EXPECT_GT(H5Lexists(file,"groundwater_boundary_water",H5P_DEFAULT),0);
    EXPECT_GT(H5Lexists(file,"groundwater_transport_ledger",H5P_DEFAULT),0);H5Fclose(file);
}

TEST(GwCompletion, PorousOwnerReceivesBoundarySpeciesAndPhysicalHeatExactlyOnce) {
    auto body=forcingDeck("[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION NO\n\n"
        "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n\n"
        "[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST 0.0001\n\n"
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n1 0 TSS MASSFLUX 2\n"
        "1 0 __TEMPERATURE__ CONC 20\n1 0 __TEMPERATURE__ HEATFLUX 100\n\n");
    body.insert(body.find("FLOW_UNITS"),"HEAT_TRANSPORT YES\n");
    Deck r=open("completion_porous_boundary",body);ASSERT_TRUE(r.opened)<<completionErrors(r);
    ASSERT_TRUE(run(r))<<completionErrors(r);ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();
    const double duration=r.eng->context().current_time,water=.0001*duration;
    EXPECT_NEAR(t.boundary_in[0],3*water,1e-9);EXPECT_NEAR(t.boundary_flux_in[0],.001*2*10*duration,1e-9);
    EXPECT_DOUBLE_EQ(t.gained_source[0],0);EXPECT_NEAR(t.residual(0),0,1e-9);
    double initial=0,final=0,advection=0,heating=0;
    for(const auto& cell:gw.heat().cells) {
        initial+=cell.initial_j;advection+=cell.advective_j;heating+=cell.source_j;
        for(double energy:cell.column.energy)final+=energy;
        EXPECT_FALSE(cell.failed)<<cell.error;
    }
    EXPECT_NEAR(heating,100*10*duration,1e-6);
    EXPECT_NEAR(advection,gw.heat().water_capacity*20*water,1e-5);
    EXPECT_NEAR(final-initial,advection+heating,1e-4);
    double rejected=-1;ASSERT_EQ(swmm_gw2d_get_ledger(r.e,SWMM_GW2D_LED_REJECT,&rejected),SWMM_OK);
    EXPECT_DOUBLE_EQ(rejected,gw.state().led_reject);finish(r);
}

TEST(GwCompletion, LayeredStrongReversalCollapseAndRewetStayAdmissibleAndConserve) {
    auto body=layeredDeck("[GW_INITIAL_QUALITY]\n* LAYER 1 TSS 7\n* LAYER 2 TSS 11\n\n"
        "[TIMESERIES]\nWQ 0:00:00 -0.5\nWQ 0:05:00 -0.5\nWQ 0:10:00 0.5\n"
        "WQ 0:15:00 0.5\nWQ 0:20:00 -0.5\n\n"
        "[GW_SOURCES]\nW * FLOW WQ TSS CONC 0\n\n",Mesh::Tri,.02);
    Deck r=open("completion_layer_strong_reversal",body);ASSERT_TRUE(r.opened)<<completionErrors(r);
    ASSERT_TRUE(run(r))<<completionErrors(r);ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();
    for(double mass:t.layer_mass){EXPECT_TRUE(std::isfinite(mass));EXPECT_GE(mass,0);}
    for(double mass:t.sat_mass){EXPECT_TRUE(std::isfinite(mass));EXPECT_GE(mass,0);}
    EXPECT_GT(gw.state().led_source_in,1);EXPECT_GT(gw.state().led_source_out,.01);
    EXPECT_NEAR(t.residual(0),0,1e-8)<<ledgerDump(t,0);
    EXPECT_NEAR(gw.state().continuityResidual(),0,1e-8);finish(r);
}


TEST(GwCompletion, TensorFluxMatchesLinearSkewedFieldsUnderRotationAndReflection) {
    using namespace openswmm::twoD::dispersion;
    for(double angle:{0.,.37,1.2})for(double reflection:{-1.,1.}) {
        const auto rotate=[&](std::array<double,2> a) {
            return std::array<double,2>{std::cos(angle)*a[0]-std::sin(angle)*reflection*a[1],
                std::sin(angle)*a[0]+std::cos(angle)*reflection*a[1]};
        };
        const auto v=rotate({2.,0.}),g=rotate({.3,.7}),n=rotate({1.,0.});
        const auto l=rotate({.4,.3}),r=rotate({-.6,-.2});
        const auto d=tensor(v[0],v[1],3,.5,.1);
        const double left=5.,right=left+g[0]*(l[0]-r[0])+g[1]*(l[1]-r[1]);
        const double measured=faceFlux(d,d,n[0],n[1],.4,.6,l,r,g,g,left,right,2.);
        const auto dg=d.apply(g);const double expected=-2*(n[0]*dg[0]+n[1]*dg[1]);
        EXPECT_NEAR(measured,expected,1e-13);
        const auto transverse=rotate({0.,1.});
        EXPECT_NEAR(transverse[0]*dg[0]+transverse[1]*dg[1],1.1*.7,1e-13);
    }
    GradientFit fit;fit.add(1,.3,.3+.7*.3);fit.add(-.2,1.,-.3*.2+.7);
    EXPECT_NEAR(fit.value()[0],.3,1e-14);EXPECT_NEAR(fit.value()[1],.7,1e-14);
}

TEST(GwCompletion, HeterogeneousTensorResistancePreservesAContinuousInterfaceFlux) {
    using namespace openswmm::twoD::dispersion;
    const Tensor a{4,1,3},b{2,.5,2};
    const double q=1.2,gy=.7,gxL=(-q-a.xy*gy)/a.xx,gxR=(-q-b.xy*gy)/b.xx;
    const std::array<double,2> armL{.4,.3},armR{-.6,-.2};
    const double left=5-gxL*armL[0]-gy*armL[1],right=5-gxR*armR[0]-gy*armR[1];
    EXPECT_NEAR(faceFlux(a,b,1,0,.4,.6,armL,armR,{gxL,gy},{gxR,gy},left,right,1),q,1e-14);
}
TEST(GwCompletion, SoluteDiffusionSafetyFactorChangesTheActualCellTimeLimit) {
    double limits[2]{};
    for(int i=0;i<2;++i) {
        Deck r=open("completion_cdiff_"+std::to_string(i),forcingDeck(
            "[GW_TRANSPORT_OPTIONS]\nDISPERSION YES\nC_DIFF "+std::string(i?"0.1":"0.4")+
            "\n[GW_TRANSPORT_PARAMS]\n* - - - - 0 0 100 - -\n"));
        ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& gw=r.eng->surfaceRouter2D().subsurface();limits[i]=gw.state().dt_cell[0];
        EXPECT_GT(limits[i],0);finish(r);
    }
    EXPECT_NEAR(limits[1]/limits[0],.25,1e-12);
}

TEST(GwCompletion, TransverseDispersivitySpreadsATracerAcrossAnOrthogonalDarcyFlow) {
    double transverse[2]{};
    for(int mode=0;mode<2;++mode) {
        auto body=forcingDeck("[GW_TRANSPORT_PARAMS]\n* - - - - 1 "+std::to_string(mode)+" 0 - -\n");
        std::ostringstream mesh;mesh<<"[2D_VERTICES]\n";
        for(int y=0;y<=3;++y)for(int x=0;x<=3;++x)mesh<<x*10<<" "<<y*10<<" 0\n";
        mesh<<"[2D_QUADS]\n";
        for(int y=0;y<3;++y)for(int x=0;x<3;++x) {
            const int v=y*4+x;mesh<<v<<" "<<v+1<<" "<<v+5<<" "<<v+4<<" .03 0\n";
        }
        const auto begin=body.find("[2D_VERTICES]"),end=body.find("[2D_INFILTRATION_DEFAULTS]");
        ASSERT_NE(begin,std::string::npos);ASSERT_NE(end,std::string::npos);body.replace(begin,end-begin,mesh.str());
        Deck r=open("completion_transverse_"+std::to_string(mode),body);ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& gw=r.eng->surfaceRouter2D().subsurface();auto& state=gw.state();auto& transport=gw.transport();
        for(int y=0;y<3;++y)for(int x=0;x<3;++x) {
            const int cell=y*3+x;state.hg[cell]=.5-.01*x;state.Ks[cell]=.001;
            transport.sat_mass[transport.idx(0,cell)]=(y==1?10:0)*gw.satVolume(cell);
        }
        gw.assignTiers(1,1);gw.fireGwFaces(0,1);
        // Faces with zero hydraulic transfer are transverse to the x flow.
        // Their paired species packets expose diffusion before cell gathering.
        for(std::size_t e=0;e<state.eacc_L.size();++e)if(std::abs(state.eacc_L[e])<1e-15)
            transverse[mode]+=std::abs(transport.sacc_L[e]);
        finish(r);
    }
    EXPECT_NEAR(transverse[0],0,1e-15);EXPECT_GT(transverse[1],1e-8);
}

TEST(GwCompletion, PendingFaceAndLayerRestartContinuesExactlyAndTruncationIsAtomic) {
    const auto body=layeredDeck("[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST .0001\n"
        "[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n"
        "[GW_INITIAL_QUALITY]\n* LAYER 1 TSS 7\n* LAYER 2 TSS 11\n");
    Deck first=open("completion_pending_reference",body),second=open("completion_pending_restart",body);
    ASSERT_TRUE(first.opened);ASSERT_TRUE(second.opened);
    ASSERT_EQ(swmm_engine_initialize(first.e),SWMM_OK);ASSERT_EQ(swmm_engine_initialize(second.e),SWMM_OK);
    auto& a=first.eng->surfaceRouter2D();auto& b=second.eng->surfaceRouter2D();
    auto& ga=a.subsurface();auto& gb=b.subsurface();
    ga.assignTiers(.1,1);gb.assignTiers(.1,1);
    for(int step=0;step<40;++step) {
        ga.fireGwFaces(0,.1);
        if(step==17) {
            const auto saved=ga.transportRestartState();ASSERT_FALSE(saved.empty());
            const auto before=gb.transportRestartState();
            EXPECT_FALSE(gb.restoreTransportRestart(saved.substr(0,saved.size()/2),true).empty());
            EXPECT_EQ(gb.transportRestartState(),before);
            ASSERT_TRUE(gb.restoreTransportRestart(saved,true).empty());
            EXPECT_EQ(gb.transportRestartState(),saved);
        } else if(step>17)gb.fireGwFaces(0,.1);
        ga.fireGwCells(0,.1,a.state(),step*.1);
        if(step>=17) {
            gb.fireGwCells(0,.1,b.state(),step*.1);
            EXPECT_EQ(gb.transportRestartState(),ga.transportRestartState())<<step;
        }
    }
    for(int s=0;s<ga.transport().n_species;++s)EXPECT_NEAR(ga.transport().residual(s),0,1e-9);
    finish(first);finish(second);
}

TEST(GwCompletion, DisabledGroundwaterRetainsItsChildBoundariesWithoutExecutingThem) {
    auto body=forcingDeck("[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 FLOW CONST .0001\n");
    body.insert(body.find("[2D_OPTIONS]\n")+13,"GROUNDWATER NO\n");
    Deck r=open("completion_disabled_parent",body);ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    EXPECT_FALSE(r.eng->surfaceRouter2D().subsurface().active());
    EXPECT_EQ(swmm_gw2d_boundary_count(r.e),1);finish(r);
}

TEST(GwCompletion, ManufacturedNeumannDiffusionConvergesOnThreeSpatialTimeRefinements) {
    std::ofstream evidence(kOutDir/"manufactured_diffusion_refinement.csv");
    ASSERT_TRUE(evidence.good());evidence.precision(17);
    evidence<<"cells_per_axis,steps,dt,l2_error,refinement_ratio,water_residual,species_residual\n";
    double previous=0;
    for(int n:{8,16,32}) {
        constexpr double diffusion=.01,finalTime=1.;
        const double pi=std::acos(-1.),dx=1./n;
        auto body=forcingDeck("[GW_TRANSPORT_PARAMS]\n* - - - - 0 0 .01 - -\n");
        std::ostringstream mesh;mesh.precision(17);mesh<<"[2D_VERTICES]\n";
        for(int y=0;y<=n;++y)for(int x=0;x<=n;++x)mesh<<x*dx<<" "<<y*dx<<" 0\n";
        mesh<<"[2D_QUADS]\n";
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            const int v=y*(n+1)+x;mesh<<v<<" "<<v+1<<" "<<v+n+2<<" "<<v+n+1<<" .03 0\n";
        }
        const auto begin=body.find("[2D_VERTICES]"),end=body.find("[2D_INFILTRATION_DEFAULTS]");
        ASSERT_NE(begin,std::string::npos);ASSERT_NE(end,std::string::npos);body.replace(begin,end-begin,mesh.str());
        Deck r=open("completion_manufactured_diffusion_"+std::to_string(n),body);ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();auto& tr=gw.transport();
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            const int cell=y*n+x;const double c=1+.2*std::cos(pi*(x+.5)*dx)*std::cos(pi*(y+.5)*dx);
            tr.sat_mass[tr.idx(0,cell)]=c*gw.satVolume(cell);
        }
        tr.init_mass[0]=tr.storage(0);
        const int steps=int(std::ceil(finalTime/(.05*dx*dx/diffusion)));const double dt=finalTime/steps;
        gw.assignTiers(dt,1);
        for(int step=0;step<steps;++step) {
            gw.fireGwFaces(0,dt);gw.fireGwCells(0,dt,router.state(),step*dt);
        }
        double square=0;
        for(int y=0;y<n;++y)for(int x=0;x<n;++x) {
            const int cell=y*n+x;
            const double exact=1+.2*std::exp(-2*diffusion*pi*pi*finalTime)*
                std::cos(pi*(x+.5)*dx)*std::cos(pi*(y+.5)*dx);
            const double difference=tr.sat_mass[tr.idx(0,cell)]/gw.satVolume(cell)-exact;
            square+=difference*difference;
        }
        const double error=std::sqrt(square/(n*n));
        evidence<<n<<","<<steps<<","<<dt<<","<<error<<","<<(previous>0?previous/error:0)
            <<","<<gw.state().continuityResidual()<<","<<tr.residual(0)<<"\n";
        EXPECT_LT(error,.002);if(previous>0)EXPECT_GT(previous/error,3.5);
        EXPECT_NEAR(tr.residual(0),0,1e-10);EXPECT_NEAR(gw.state().continuityResidual(),0,1e-10);
        previous=error;finish(r);
    }
}

TEST(GwCompletion, HydraulicHeadAndTotalFlowBoundariesAgreeInUSAndSIProjects) {
    for(bool head:{false,true}) {
        std::vector<double> reference;
        for(bool us:{false,true}) {
            const double length=us?1/.3048:1,conductivity=us?1/25.4:1;
            std::ostringstream input;input.precision(17);
            input<<"[OPTIONS]\nFLOW_UNITS "<<(us?"CFS":"CMS")
                <<"\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2026\nEND_DATE 01/01/2026\nEND_TIME 00:00:10\nROUTING_STEP .5\nREPORT_STEP 00:00:01\n"
                <<"[POLLUTANTS]\nTSS MG/L 0 0 0 0\n[JUNCTIONS]\nJ1 "<<-2*length<<" "<<2*length
                <<"\n[OUTFALLS]\nO1 "<<-3*length<<" FREE NO\n[CONDUITS]\nC1 J1 O1 "<<30*length
                <<" .013 0 0 0\n[XSECTIONS]\nC1 CIRCULAR "<<.5*length<<" 0 0 0 1\n"
                <<"[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP .5\nREPORT_2D NO\nRAINFALL_MODE NONE\n"
                <<"[2D_VERTICES]\n0 0 0\n"<<10*length<<" 0 0\n"<<10*length<<" "<<10*length<<" 0\n0 "<<10*length<<" 0\n"
                <<"[2D_QUADS]\n0 1 2 3 .03 0\n[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nSOIL_CHAR GARDNER\nNODE_ENROLMENT ROWS\n"
                <<"[2D_AQUIFER]\n* "<<36*conductivity<<" "<<length<<" .45 .1 "<<2/length<<" HG0 "<<.3*length
                <<"\n[GW_TRANSPORT_OPTIONS]\nDISPERSION NO\n[2D_AQUIFER_BOUNDARIES]\nCELL 1 EDGE 0 "
                <<(head?"HEAD":"FLOW")<<" CONST "<<(head?-.6*length:.0001)
                <<"\n[GW_BOUNDARY_QUALITY]\n1 0 TSS CONC 3\n[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 2\n";
            Deck r=open(std::string("completion_units_")+(head?"head_":"flow_")+(us?"us":"si"),input.str());
            ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_TRUE(run(r))<<completionErrors(r);
            ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
            const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& tr=gw.transport();
            const std::vector<double> measured{gw.state().led_boundary_in,gw.state().led_boundary_out,
                gw.state().storage(),tr.ledgeredStorage(0),tr.boundary_in[0],tr.boundary_out[0]};
            EXPECT_GT(measured[0],0);EXPECT_NEAR(tr.residual(0),0,1e-10);
            if(!us)reference=measured;
            else for(std::size_t i=0;i<reference.size();++i)EXPECT_NEAR(measured[i],reference[i],1e-10*std::max(1.,std::abs(reference[i])))<<i;
            finish(r);
        }
    }
}


TEST(GwCompletion, AboveTableConduitContactPairsLayerWaterAndMassAcrossRestart) {
    for(const Mesh shape:{Mesh::Tri,Mesh::Quad,Mesh::Mixed}) {
        const auto body=layeredDeck("",shape);
        Deck first=open(std::string("contact_pending_")+meshName(shape),body);
        Deck second=open(std::string("contact_restored_")+meshName(shape),body);
        ASSERT_TRUE(first.opened);ASSERT_TRUE(second.opened);
        ASSERT_EQ(swmm_engine_initialize(first.e),SWMM_OK)<<completionErrors(first);
        ASSERT_EQ(swmm_engine_initialize(second.e),SWMM_OK)<<completionErrors(second);
        auto& a=first.eng->surfaceRouter2D();auto& b=second.eng->surfaceRouter2D();
        auto& ga=a.subsurface();auto& gb=b.subsurface();
        ga.assignTiers(.1,1);gb.assignTiers(.1,1);
        const auto& w=ga.state();const auto& t=ga.transport();const int layer=t.n_layers/2;
        const double elevation=w.z_bed[0]+w.zs[0]-(w.zs[0]-w.hg[0])*(layer+.5)/t.n_layers;
        const double old_water=w.hu[0]*w.area[0];
        std::vector<double> concentration(t.n_species,0);concentration[0]=7;
        ga.bookLinkContact(0,.02,concentration,elevation,elevation);
        EXPECT_NEAR(t.inFlight1D(0),.14,1e-14);
        EXPECT_NEAR(t.contact_link_mass[t.layerIdx(0,layer,0)],.14,1e-14);
        const auto saved=ga.transportRestartState();
        ASSERT_TRUE(gb.restoreTransportRestart(saved,true).empty());
        EXPECT_EQ(gb.transportRestartState(),saved);
        ga.fireGwCells(0,.1,a.state(),0);gb.fireGwCells(0,.1,b.state(),0);
        EXPECT_EQ(ga.transportRestartState(),gb.transportRestartState());
        EXPECT_NEAR(t.layer_mass[t.layerIdx(0,layer,0)],.14,1e-10);
        EXPECT_NEAR(t.sat_mass[t.idx(0,0)],0,1e-10);
        EXPECT_NEAR(w.hu[0]*w.area[0]-old_water,.02,1e-10);
        EXPECT_NEAR(t.gained_link[0],.14,1e-14);
        EXPECT_NEAR(t.residual(0),0,1e-10);EXPECT_NEAR(w.continuityResidual(),0,1e-10);
        EXPECT_DOUBLE_EQ(t.inFlight1D(0),0);
        finish(first);finish(second);
    }
}

TEST(GwCompletion, SlopingConduitCrossesLayersAndSaturatedContactWithoutMidpointLumping) {
    for(bool reverse:{false,true}) {
        Deck r=open(reverse?"contact_slope_reverse":"contact_slope",layeredDeck("",Mesh::Quad));
        ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();
        const auto& t=gw.transport();const auto& w=gw.state();
        const double ground=w.z_bed[0]+w.zs[0],length=w.zs[0]-w.hg[0];
        std::vector<double> concentration(t.n_species,0);concentration[0]=7;
        const double a=ground,b=ground-2*length;
        gw.bookLinkContact(0,.08,concentration,reverse?b:a,reverse?a:b);
        for(int j=0;j<t.n_layers;++j) {
            EXPECT_NEAR(t.contact_water[j*t.n_cells],.04/t.n_layers,1e-14);
            EXPECT_NEAR(t.contact_link_mass[t.layerIdx(0,j,0)],.28/t.n_layers,1e-14);
        }
        EXPECT_NEAR(t.lacc_mass[t.idx(0,0)],.28,1e-14);
        gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
        EXPECT_NEAR(t.gained_link[0],.56,1e-13);
        EXPECT_NEAR(t.residual(0),0,1e-10);EXPECT_NEAR(w.continuityResidual(),0,1e-10);
        finish(r);
    }
}

TEST(GwCompletion, AboveTableNodeBedReceivesItsQualityInTheIntersectedLayer) {
    auto body=layeredDeck("[2D_AQUIFER_NODE]\nJ1 1 KC 360 DC .05 AREA 1\n",Mesh::Quad);
    const auto at=body.find("J1 -2.0 4.0 0 0 0");ASSERT_NE(at,std::string::npos);
    body.replace(at,std::string("J1 -2.0 4.0 0 0 0").size(),"J1 -.2 4.0 .4 0 0");
    Deck r=open("contact_node",body);ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();auto nodes=r.eng->context().nodes;
    const auto& w=gw.state();const auto& t=gw.transport();
    const double l12=router.options().len_1d_to_2d;
    nodes.depth[0]=.4/l12;nodes.volume[0]=10/router.options().vol_1d_to_2d;
    std::vector<double> concentration(nodes.invert_elev.size()*t.n_species,0);concentration[0]=7;
    gw.setNodeRowConc(concentration.data(),t.n_species);
    gw.sampleNodeExchange(&nodes,.1,.1);
    const int layer=gw.contactLayer(0,-.25);ASSERT_GE(layer,0);
    const double incoming=-w.nacc[0];ASSERT_GT(incoming,0);
    EXPECT_NEAR(t.contact_water[layer*t.n_cells],incoming,1e-15);
    EXPECT_NEAR(t.contact_node_mass[t.layerIdx(0,layer,0)],7*incoming,1e-15);
    EXPECT_DOUBLE_EQ(t.nacc_mass[0],0);
    gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
    EXPECT_NEAR(t.layer_mass[t.layerIdx(0,layer,0)],7*incoming,1e-10);
    EXPECT_NEAR(t.gained_node[0],7*incoming,1e-14);
    EXPECT_NEAR(t.residual(0),0,1e-10);EXPECT_NEAR(w.continuityResidual(),0,1e-10);
    nodes.depth[0]=0;nodes.volume[0]=0;nodes.inflow[0]=0;gw.resetNodeExchangeVolumes();gw.sampleNodeExchange(&nodes,.1,.2);
    EXPECT_DOUBLE_EQ(w.nacc[0],0); // disconnected table cannot lift water into an empty bed
    finish(r);
}

TEST(GwCompletion, ConduitResolverRetainsClippedStationElevations) {
    auto body=layeredDeck("[LOSSES]\nC1 0 0 0 NO 100\n",Mesh::Quad);
    Deck r=open("contact_resolver",body);ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& ctx=r.eng->context();
    ctx.spatial.node_x[0]=-5;ctx.spatial.node_y[0]=5;
    ctx.spatial.node_x[1]=15;ctx.spatial.node_y[1]=5;
    ctx.nodes.invert_elev[0]=0;ctx.nodes.invert_elev[1]=-1/.3048;
    openswmm::twoD::SubsurfaceConfig cfg;std::vector<std::string> names;int count=0;
    const auto shares=openswmm::twoD::resolveLinkSeepage(ctx,r.eng->surfaceRouter2D().mesh(),cfg,names,1,count);
    ASSERT_EQ(shares.size(),1);EXPECT_NEAR(shares[0].weight,.5,1e-14);
    ASSERT_EQ(shares[0].contacts.size(),1);
    EXPECT_NEAR(shares[0].contacts[0].weight,.5,1e-14);
    EXPECT_NEAR(shares[0].contacts[0].begin,-.25,1e-14);
    EXPECT_NEAR(shares[0].contacts[0].end,-.75,1e-14);
    finish(r);
}

TEST(GwCompletion, LayerContactPreservesUniformQualityDuringCompressionAndOverflow) {
    for(const double volume:{.02,30.}) {
        Deck r=open(volume<1?"contact_uniform":"contact_overflow",layeredDeck(
            "[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n",Mesh::Quad));
        ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();
        const auto& t=gw.transport();const auto& w=gw.state();
        const double elevation=w.z_bed[0]+w.zs[0]-.01;
        std::vector<double> concentration(t.n_species,0);concentration[0]=7;
        gw.bookLinkContact(0,volume,concentration,elevation,elevation);
        gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
        for(int j=0;j<t.n_layers;++j) {
            const double water=w.area[0]*(w.zs[0]-w.hg[0])/t.n_layers*w.theta_sigma[j*t.n_cells];
            EXPECT_NEAR(t.layer_mass[t.layerIdx(0,j,0)],7*water,1e-9);
        }
        EXPECT_NEAR(t.sat_mass[t.idx(0,0)],7*w.area[0]*w.hg[0]*w.theta_s[0],1e-9);
        EXPECT_NEAR(t.residual(0),0,1e-9);EXPECT_NEAR(w.continuityResidual(),0,1e-9);
        finish(r);
    }
}

namespace {
std::string porousReactorDeck(const std::string& laws,const std::string& extra="") {
    auto body=forcingDeck("[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION NO\nTHERMAL_LAYERS 4\nDISPERSION NO\n\n"
        "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-13\nRTOL 1e-11\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
        "[REACTION_SUBSURFACE]\n"+laws+
        "[GW_INITIAL_QUALITY]\n* SAT A 7\n* UNSAT A 7\n* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n"+extra);
    body.insert(body.find("FLOW_UNITS"),"HEAT_TRANSPORT YES\n");return body;
}
}
TEST(GwCompletion, PorousCanonicalReactorsConvertAIntoBWithoutChangingThermalEnergy) {
    Deck r=open("porous_reactor",porousReactorDeck(
        "* SAT A RATE -.001*A\n* SAT B RATE .001*A\n* UNSAT A RATE -.001*A\n* UNSAT B RATE .001*A\n"));
    ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& gw=r.eng->surfaceRouter2D().subsurface();double initial_energy=0;
    for(const auto& c:gw.heat().cells)for(double e:c.column.energy)initial_energy+=e;
    ASSERT_TRUE(run(r))<<completionErrors(r);ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& t=gw.transport();const int a=t.rowIndex("A"),b=t.rowIndex("B");ASSERT_GE(a,0);ASSERT_GE(b,0);
    const double initial=t.init_mass[a],expected=initial*std::exp(-.001*r.eng->context().current_time);
    EXPECT_NEAR(t.ledgeredStorage(a),expected,initial*1e-8);
    EXPECT_NEAR(t.ledgeredStorage(a)+t.ledgeredStorage(b),initial,initial*1e-10);
    EXPECT_NEAR(t.residual(a),0,1e-9);EXPECT_NEAR(t.residual(b),0,1e-9);
    double final_energy=0;
    for(const auto& c:gw.heat().cells)for(double e:c.column.energy)final_energy+=e;
    EXPECT_NEAR(final_energy,initial_energy,1e-4);
    for(int i=0;i<t.n_cells;++i)for(int row:{a,b}) {
        const auto& c=gw.heat().cells[i];const auto view=c.porewater.zoneMass(c.column,c.saturated_water,row);
        EXPECT_DOUBLE_EQ(t.sat_mass[t.idx(row,i)],view.first);
        EXPECT_DOUBLE_EQ(t.unsat_mass[t.idx(row,i)],view.second);
    }
    finish(r);
}

TEST(GwCompletion, PorousPhaseReactorsUseActualCapacityTemperatureAndDepth) {
    for(bool sorption:{false,true}) {
        Deck r=open(sorption?"porous_reactor_sorbed":"porous_reactor_phases",porousReactorDeck(
            "* SAT A RATE -.01*A*TEMP/20\n* SAT B RATE .01*A*TEMP/20\n"
            "* UNSAT A RATE -.02*A*(GW_DEPTH_M*0+1)\n* UNSAT B RATE .02*A*(GW_DEPTH_M*0+1)\n",
            sorption?"[GW_SORPTION]\n* A 2 0\n* B 2 0\n":""));
        ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();const auto& t=gw.transport();
        const int a=t.rowIndex("A"),b=t.rowIndex("B");ASSERT_GE(a,0);ASSERT_GE(b,0);
        std::vector<double> expected;
        for(int i=0;i<t.n_cells;++i) {
            const auto& c=gw.heat().cells[i];const auto& col=c.column;const auto nl=col.theta.size();
            for(std::size_t j=0;j<nl;++j) {
                const double bulk=col.area*(col.depth_edges[j+1]-col.depth_edges[j]);
                const double water=col.theta[j]*bulk,sat=c.saturated_water[j];
                const double fraction=sat/(col.material.porosity*bulk);
                const double retention=c.porewater.retention[std::size_t(a)*nl+j];
                const double capacity=water+retention;
                const double satcap=sat+retention*fraction,unsatcap=capacity-satcap;
                const double original=c.porewater.mass[std::size_t(a)*nl+j];
                expected.push_back(original*(satcap/capacity*std::exp(satcap>0?-.01*.1*sat/satcap:0)+
                    unsatcap/capacity*std::exp(unsatcap>0?-.02*.1*(water-sat)/unsatcap:0)));
            }
        }
        gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
        std::size_t index=0;
        for(const auto& c:gw.heat().cells)for(std::size_t j=0;j<c.column.theta.size();++j) {
            EXPECT_NEAR(c.porewater.mass[std::size_t(a)*c.column.theta.size()+j],expected[index++],1e-8);
        }
        EXPECT_NEAR(t.residual(a),0,1e-9);EXPECT_NEAR(t.residual(b),0,1e-9);
        const auto saved=gw.transportRestartState();
        ASSERT_TRUE(gw.restoreTransportRestart(saved,true).empty());EXPECT_EQ(gw.transportRestartState(),saved);
        finish(r);
    }
}

TEST(GwCompletion, PorousReactorFailurePublishesNoPartialReactionInventory) {
    Deck r=open("porous_reactor_refusal",porousReactorDeck(
        "* SAT A RATE -.1*A\n* SAT B RATE .1*A\n* UNSAT A RATE 1/(GW_THETA-GW_THETA)\n"));
    ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();const auto& t=gw.transport();
    const int a=t.rowIndex("A"),b=t.rowIndex("B");ASSERT_GE(a,0);ASSERT_GE(b,0);
    const auto before=gw.heat().cells[0].porewater.mass;
    gw.assignTiers(.1,1);EXPECT_THROW(gw.fireGwCells(0,.1,router.state(),0),std::runtime_error);
    const auto& after=gw.heat().cells[0].porewater.mass;ASSERT_EQ(after.size(),before.size());
    for(std::size_t j=0;j<before.size();++j)EXPECT_NEAR(after[j],before[j],1e-9);
    EXPECT_DOUBLE_EQ(t.lost_reaction[a],0);EXPECT_DOUBLE_EQ(t.lost_reaction[b],0);
    finish(r);
}

TEST(GwCompletion, ContactReceiptsCarrySignedTemperatureAndAgeWithTheirWater) {
    auto body=layeredDeck("",Mesh::Quad);
    body.insert(body.find("FLOW_UNITS"),"HEAT_TRANSPORT YES\nWATER_AGE ON\n");
    Deck r=open("contact_intensive_rows",body);ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();const auto& t=gw.transport();
    ASSERT_GE(t.age_row,0);ASSERT_GE(t.temp_row,0);
    std::vector<double> concentration(t.n_species,0);concentration[0]=7;
    concentration[t.age_row]=4;concentration[t.temp_row]=-5;
    gw.bookLinkContact(0,.02,concentration,-.25,-.25);
    EXPECT_NEAR(t.inFlight1D(t.temp_row),-.1,1e-15);
    const auto saved=gw.transportRestartState();ASSERT_TRUE(gw.restoreTransportRestart(saved,true).empty());
    gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
    EXPECT_NEAR(t.gained_link[t.temp_row],-.1,1e-15);
    EXPECT_NEAR(t.gained_link[t.age_row],.08,1e-15);
    EXPECT_NEAR(t.residual(t.temp_row),0,1e-10);EXPECT_NEAR(t.residual(t.age_row),0,1e-10);
    finish(r);
}

TEST(GwCompletion, ZeroNetConduitReversalKeepsGrossDonorReservation) {
    Deck r=open("contact_gross_reservation",layeredDeck("",Mesh::Quad));
    ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();
    const double before=gw.unreservedDrainableWater(0),room=gw.infiltrationHeadroom(0);
    const double v=std::min(.01,before/10);ASSERT_GT(v,0);
    gw.bookLinkSeepage(0,-v);
    std::vector<double> concentration(gw.transport().n_species,0);concentration[0]=7;
    gw.bookLinkContact(0,v,concentration,-.25,-.25);
    EXPECT_DOUBLE_EQ(gw.state().lacc[0],0);
    EXPECT_NEAR(gw.unreservedDrainableWater(0),before-v,1e-14);
    EXPECT_NEAR(gw.infiltrationHeadroom(0),room-v,1e-14);
    const auto saved=gw.transportRestartState();ASSERT_TRUE(gw.restoreTransportRestart(saved,true).empty());
    EXPECT_NEAR(gw.unreservedDrainableWater(0),before-v,1e-14);
    gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
    EXPECT_NEAR(gw.transport().gained_link[0],7*v,1e-14);
    EXPECT_NEAR(gw.transport().residual(0),0,1e-10);
    EXPECT_NEAR(gw.state().continuityResidual(),0,1e-10);finish(r);
}

TEST(GwCompletion, ActualRoutingDeliversAboveTableConduitQualityToLayers) {
    auto body=seamDeck(.3,.02,100,50);
    auto replace=[&](const std::string& a,const std::string& b){const auto p=body.find(a);ASSERT_NE(p,std::string::npos);body.replace(p,a.size(),b);};
    replace("CLOSURE CLOSED_FORM","CLOSURE SIGMA\nLINK_SEEPAGE ONE_WAY");
    replace("J1 -2.0 4.0 0 0 0","J1 -.2 4.0 .1 0 0");
    replace("O1 -2.5 FREE NO","O1 -.3 FREE NO");
    replace("*  36.0  1.0","*  0.000000001  1.0");
    replace("CELL 1  36.0  1.0  0.45  0.10  2.0  HG0 0.95","");
    replace("J1  1\n","J1 1 EXCHANGE NO\n");
    const auto seed=body.find("[GW_INITIAL_QUALITY]");const auto next=body.find('[',seed+1);
    body.erase(seed,next-seed);
    body.insert(body.find("[REPORT]"),"[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT LAYERED\nDISPERSION NO\n");
    Deck r=open("contact_routing",body);ASSERT_TRUE(r.opened)<<completionErrors(r);
    ASSERT_TRUE(run(r))<<completionErrors(r);ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();
    ASSERT_GT(t.gained_link[0],0);EXPECT_GT(t.unsat_mass[0],0);
    EXPECT_NEAR(t.sat_mass[0],0,1e-7);
    EXPECT_NEAR(t.residual(0),t.inFlight1D(0),1e-8);
    EXPECT_NEAR(gw.state().continuityResidual(),0,1e-8);finish(r);
}

namespace {
std::string mappedPorousDeck(const std::string& extra="",Mesh shape=Mesh::Tri,int sigma=5,int thermal=3) {
    auto body=layeredDeck("[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\nCONDUCTION NO\nTHERMAL_LAYERS "+
        std::to_string(thermal)+"\n\n"+extra,shape);
    body.insert(body.find("FLOW_UNITS"),"HEAT_TRANSPORT YES\n");
    const std::string anchor="[2D_AQUIFER_OPTIONS]\n";
    body.insert(body.find(anchor)+anchor.size(),"M_LAYERS "+std::to_string(sigma)+"\n");return body;
}
double totalGroundEnergy(const openswmm::twoD::SubsurfaceSolver& gw) {
    double total=0;for(const auto& c:gw.heat().cells)for(double e:c.column.energy)total+=e;return total;
}
}
TEST(GwSigmaPorous, UnequalGridsPreserveCanonicalLayerInitialMassAndThermalEnergy) {
    for(const auto counts:{std::pair<int,int>{5,3},{3,11},{8,2}}) {
        Deck r=open("mapped_initial_"+std::to_string(counts.first)+"_"+std::to_string(counts.second),mappedPorousDeck(
            "[GW_INITIAL_QUALITY]\n* SAT TSS 5\n* UNSAT TSS 7\n* LAYER 2 TSS 11\n"
            "* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n* LAYER "+std::to_string(counts.first)+" __TEMPERATURE__ -5\n"
            "[GW_SORPTION]\n* TSS 2 0\n",Mesh::Quad,counts.first,counts.second));
        ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
        auto& gw=r.eng->surfaceRouter2D().subsurface();auto& tr=gw.transport();const auto& w=gw.state();
        ASSERT_EQ(tr.n_layers,counts.first);ASSERT_EQ(gw.heat().cells[0].column.theta.size(),counts.second);
        const auto original=tr.layer_mass;double expected_energy=0;
        for(int cell=0;cell<tr.n_cells;++cell) {
            const auto& c=gw.heat().cells[cell];const double dz=(w.zs[cell]-w.hg[cell])/tr.n_layers;
            for(int layer=0;layer<tr.n_layers;++layer) {
                const double theta=w.theta_sigma[layer*tr.n_cells+cell];
                const double capacity=w.area[cell]*dz*(theta+tr.kd[tr.idx(0,cell)]*tr.rho_b[cell]);
                EXPECT_NEAR(tr.layer_mass[tr.layerIdx(0,layer,cell)],capacity*(layer==1?11:7),1e-12);
                expected_energy+=w.area[cell]*dz*c.column.material.volumetricCapacity(theta)*(layer==counts.first-1?-5:20);
            }
            expected_energy+=w.area[cell]*w.hg[cell]*c.column.material.volumetricCapacity(w.theta_s[cell])*20;
            EXPECT_LE(c.porewater.sigma.overlaps.size(),std::size_t(counts.first+counts.second+1));
            for(int repeat=0;repeat<30;++repeat) {
                ASSERT_TRUE(gw.refreshSigmaPorewater(cell).empty());gw.heat().refreshView(cell,tr);
            }
            double physical=0;for(std::size_t j=0;j<c.column.theta.size();++j)physical+=c.porewater.mass[j];
            EXPECT_NEAR(physical,tr.sat_mass[tr.idx(0,cell)]+tr.unsat_mass[tr.idx(0,cell)],1e-10);
        }
        EXPECT_NEAR(totalGroundEnergy(gw),expected_energy,1e-5);
        for(int cell=0;cell<tr.n_cells;++cell)for(int layer=0;layer<tr.n_layers;++layer)
            EXPECT_DOUBLE_EQ(tr.layer_mass[tr.layerIdx(0,layer,cell)],original[tr.layerIdx(0,layer,cell)]);
        EXPECT_NEAR(tr.residual(0),0,1e-10);finish(r);
    }
}

TEST(GwSigmaPorous, MovingTableConservesUniformSorbingChemistryAndThermalEnergy) {
    for(const Mesh shape:{Mesh::Tri,Mesh::Quad,Mesh::Mixed})for(const std::string rate:{"0.001","-0.001"}) {
        Deck r=open(std::string("mapped_uniform_")+meshName(shape)+(rate[0]=='-'?"_out":"_in"),mappedPorousDeck(
            "[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n"
            "* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n"
            "[GW_SORPTION]\n* TSS 2 0\n"
            "[GW_SOURCES]\nW * FLOW "+rate+" TSS CONC 7 __TEMPERATURE__ CONC 20\n",shape));
        ASSERT_TRUE(r.opened)<<completionErrors(r);ASSERT_TRUE(run(r))<<completionErrors(r);
        ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
        const auto& gw=r.eng->surfaceRouter2D().subsurface();const auto& t=gw.transport();const auto& w=gw.state();
        for(int i=0;i<t.n_cells;++i)for(int layer=0;layer<t.n_layers;++layer) {
            const double dz=(w.zs[i]-w.hg[i])/t.n_layers,theta=w.theta_sigma[layer*t.n_cells+i];
            const double capacity=w.area[i]*dz*(theta+t.kd[t.idx(0,i)]*t.rho_b[i]);
            EXPECT_NEAR(t.layer_mass[t.layerIdx(0,layer,i)],7*capacity,1e-8);
        }
        double initial=0,external=0;for(const auto& c:gw.heat().cells) {
            EXPECT_FALSE(c.failed)<<c.error;initial+=c.initial_j;
            external+=c.advective_j+c.source_j+c.surface_j+c.deep_j+c.latent_j+c.lateral_j+c.water_contact_j+c.channel_contact_j+c.lid_contact_j;
            for(std::size_t j=0;j<c.column.energy.size();++j)EXPECT_NEAR(c.column.energy[j]/c.column.capacity[j],20,1e-8);
        }
        EXPECT_NEAR(totalGroundEnergy(gw)-initial,external,1e-3);
        EXPECT_NEAR(t.residual(0),0,1e-8);EXPECT_NEAR(w.continuityResidual(),0,1e-9);finish(r);
    }
}

TEST(GwSigmaPorous, PendingFiniteContactMapsOnceAndRestartsWithoutASecondInventory) {
    const auto body=mappedPorousDeck("[GW_INITIAL_QUALITY]\n* SAT TSS 5\n* UNSAT TSS 7\n",Mesh::Quad);
    Deck a=open("mapped_contact",body),b=open("mapped_contact_restart",body);
    ASSERT_TRUE(a.opened);ASSERT_TRUE(b.opened);
    ASSERT_EQ(swmm_engine_initialize(a.e),SWMM_OK)<<completionErrors(a);
    ASSERT_EQ(swmm_engine_initialize(b.e),SWMM_OK)<<completionErrors(b);
    auto& ga=a.eng->surfaceRouter2D().subsurface();auto& gb=b.eng->surfaceRouter2D().subsurface();
    auto& tr=ga.transport();auto& pore=ga.heat().cells[0].porewater;
    const double before=tr.storage(0),energy=totalGroundEnergy(ga);
    ASSERT_TRUE(pore.applyContactMassIncrement(0,0,.14));
    EXPECT_DOUBLE_EQ(tr.storage(0),before); // accepted receipt is not a second writable store
    const auto saved=ga.transportRestartState();ASSERT_FALSE(saved.empty());
    ASSERT_TRUE(gb.restoreTransportRestart(saved,true).empty());EXPECT_EQ(gb.transportRestartState(),saved);
    ga.heat().refreshView(0,tr);gb.heat().refreshView(0,gb.transport());
    ASSERT_TRUE(ga.refreshPorewaterContactLedger().empty());ASSERT_TRUE(gb.refreshPorewaterContactLedger().empty());
    EXPECT_EQ(ga.transportRestartState(),gb.transportRestartState());
    EXPECT_NEAR(tr.storage(0)-before,.14,1e-12);EXPECT_NEAR(tr.gained_contact[0],.14,1e-12);
    const auto settled=ga.transportRestartState();ga.heat().refreshView(0,tr);EXPECT_EQ(ga.transportRestartState(),settled);
    EXPECT_DOUBLE_EQ(totalGroundEnergy(ga),energy);EXPECT_NEAR(tr.residual(0),0,1e-10);
    const auto no_change=ga.transportRestartState();
    EXPECT_FALSE(pore.applyContactMassIncrement(0,0,-1e9));EXPECT_EQ(ga.transportRestartState(),no_change);
    finish(a);finish(b);
}

TEST(GwSigmaPorous, ColdLayerContactCarriesAgeAndPhysicalJoulesOnce) {
    auto body=mappedPorousDeck("[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n",Mesh::Quad);
    body.insert(body.find("FLOW_UNITS"),"WATER_AGE ON\n");
    Deck r=open("mapped_cold_contact",body);ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();const auto& t=gw.transport();
    const double energy=totalGroundEnergy(gw);std::vector<double> concentration(t.n_species,0);
    concentration[0]=7;concentration[t.temp_row]=-5;concentration[t.age_row]=4;
    gw.bookLinkContact(0,.02,concentration,-.25,-.25);
    gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
    EXPECT_NEAR(totalGroundEnergy(gw)-energy,gw.heat().water_capacity*(-5)*.02,1e-5);
    EXPECT_NEAR(t.gained_link[t.temp_row],-.1,1e-14);EXPECT_NEAR(t.gained_link[t.age_row],.08,1e-14);
    EXPECT_NEAR(t.residual(0),0,1e-10);EXPECT_NEAR(t.residual(t.age_row),0,1e-10);
    for(int layer=0;layer<t.n_layers;++layer)EXPECT_TRUE(std::isfinite(t.layer_mass[t.layerIdx(t.temp_row,layer,0)]));
    finish(r);
}

TEST(GwSigmaPorous, LayerReactorsUseMappedThermalTemperatureWithAqueousSorptionCapacity) {
    const std::string chemistry="[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-12\nRTOL 1e-10\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
        "[REACTION_SUBSURFACE]\n* SAT A RATE -.001*A*(TEMP+10)/30\n* SAT B RATE .001*A*(TEMP+10)/30\n"
        "* UNSAT A RATE -.001*A*(TEMP+10)/30\n* UNSAT B RATE .001*A*(TEMP+10)/30\n"
        "[GW_INITIAL_QUALITY]\n* SAT A 7\n* UNSAT A 7\n* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n";
    Deck r=open("mapped_reactors",mappedPorousDeck(chemistry));ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r))<<completionErrors(r);ASSERT_EQ(swmm_engine_end(r.e),SWMM_OK);r.started=false;
    const auto& t=r.eng->surfaceRouter2D().subsurface().transport();const int a=t.rowIndex("A"),b=t.rowIndex("B");ASSERT_GE(a,0);ASSERT_GE(b,0);
    EXPECT_NEAR(t.ledgeredStorage(a),t.init_mass[a]*std::exp(-.001*r.eng->context().current_time),t.init_mass[a]*1e-8);
    EXPECT_NEAR(t.ledgeredStorage(a)+t.ledgeredStorage(b),t.init_mass[a],t.init_mass[a]*1e-10);
    EXPECT_NEAR(t.residual(a),0,1e-9);EXPECT_NEAR(t.residual(b),0,1e-9);finish(r);
}

TEST(GwSigmaPorous, PendingFaceHeatAndMappedContactContinueExactlyAfterRestore) {
    const auto body=mappedPorousDeck("[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n"
        "* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 20\n"
        "[2D_AQUIFER]\nCELL 1 36 1 .45 .1 2 HG0 .8\n");
    Deck first=open("mapped_pending_face",body),second=open("mapped_pending_face_restore",body);
    ASSERT_TRUE(first.opened);ASSERT_TRUE(second.opened);
    ASSERT_EQ(swmm_engine_initialize(first.e),SWMM_OK)<<completionErrors(first);
    ASSERT_EQ(swmm_engine_initialize(second.e),SWMM_OK)<<completionErrors(second);
    auto& a=first.eng->surfaceRouter2D();auto& b=second.eng->surfaceRouter2D();auto& ga=a.subsurface();auto& gb=b.subsurface();
    ga.assignTiers(.1,1);gb.assignTiers(.1,1);ga.fireGwFaces(0,.1);
    double promised=0;for(const auto& c:ga.heat().cells)for(double amount:c.porewater.reserved_out)promised+=amount;
    ASSERT_GT(promised,0);ASSERT_TRUE(ga.heat().cells[0].porewater.applyContactMassIncrement(0,0,.1));
    const auto saved=ga.transportRestartState();
    {std::ofstream file(kOutDir/"mapped_pending_face.bin",std::ios::binary);file.write(saved.data(),saved.size());}
    ASSERT_TRUE(gb.restoreTransportRestart(saved,true).empty());EXPECT_EQ(gb.transportRestartState(),saved);
    for(int step=0;step<12;++step) {
        if(step){ga.fireGwFaces(0,.1);gb.fireGwFaces(0,.1);}
        ga.fireGwCells(0,.1,a.state(),step*.1);gb.fireGwCells(0,.1,b.state(),step*.1);
        ASSERT_TRUE(ga.refreshPorewaterContactLedger().empty());ASSERT_TRUE(gb.refreshPorewaterContactLedger().empty());
        EXPECT_EQ(ga.transportRestartState(),gb.transportRestartState())<<step;
    }
    EXPECT_NEAR(ga.transport().residual(0),0,1e-9);EXPECT_NEAR(ga.state().continuityResidual(),0,1e-9);
    Deck fresh=open("mapped_moving_restore_fresh",body);ASSERT_TRUE(fresh.opened);
    ASSERT_EQ(swmm_engine_initialize(fresh.e),SWMM_OK);
    auto& gf=fresh.eng->surfaceRouter2D().subsurface();
    ASSERT_TRUE(gf.restoreTransportRestart(ga.transportRestartState(),true).empty());
    EXPECT_EQ(gf.transportRestartState(),ga.transportRestartState());
    finish(first);finish(second);finish(fresh);
}

TEST(GwSigmaPorous, IncompatibleThermalGridAndUnpairedMappingReceiptsRefuseAtomically) {
    const std::string quality="[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n";
    Deck a=open("mapped_identity",mappedPorousDeck(quality,Mesh::Quad,5,3));
    Deck b=open("mapped_identity_target",mappedPorousDeck(quality,Mesh::Quad,5,3));
    Deck c=open("mapped_identity_wrong",mappedPorousDeck(quality,Mesh::Quad,5,4));
    ASSERT_TRUE(a.opened);ASSERT_TRUE(b.opened);ASSERT_TRUE(c.opened);
    ASSERT_EQ(swmm_engine_initialize(a.e),SWMM_OK);ASSERT_EQ(swmm_engine_initialize(b.e),SWMM_OK);ASSERT_EQ(swmm_engine_initialize(c.e),SWMM_OK);
    auto& ga=a.eng->surfaceRouter2D().subsurface();auto& gb=b.eng->surfaceRouter2D().subsurface();auto& gc=c.eng->surfaceRouter2D().subsurface();
    const auto saved=ga.transportRestartState(),target=gb.transportRestartState(),wrong=gc.transportRestartState();
    EXPECT_FALSE(gc.restoreTransportRestart(saved,true).empty());EXPECT_EQ(gc.transportRestartState(),wrong);
    auto& pending=ga.heat().cells[0].porewater.sigma.pending;pending[0]+=.1;
    const auto corrupt=ga.transportRestartState();pending[0]-=.1;
    EXPECT_FALSE(gb.restoreTransportRestart(corrupt,true).empty());EXPECT_EQ(gb.transportRestartState(),target);
    auto& cached=ga.heat().cells[0].porewater.mass;const double old=cached[0];cached[0]+=.1;
    const auto corrupt_view=ga.transportRestartState();cached[0]=old;
    EXPECT_FALSE(gb.restoreTransportRestart(corrupt_view,true).empty());EXPECT_EQ(gb.transportRestartState(),target);
    EXPECT_FALSE(gb.restoreTransportRestart(saved.substr(0,saved.size()/2),true).empty());EXPECT_EQ(gb.transportRestartState(),target);
    finish(a);finish(b);finish(c);
}

TEST(GwSigmaPorous, FiniteContactsCannotSpendAnAlreadyPromisedConduitExport) {
    Deck r=open("mapped_protected_export",mappedPorousDeck("[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n",Mesh::Quad));
    ASSERT_TRUE(r.opened);ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();auto& tr=gw.transport();
    const double volume=.01,amount=gw.bookLinkGain(0,0,volume);ASSERT_GT(amount,0);
    gw.bookLinkSeepage(0,-volume);
    auto& pore=gw.heat().cells[0].porewater;const auto nl=gw.heat().cells[0].column.theta.size();
    double reserved=0;for(std::size_t j=0;j<nl;++j)reserved+=pore.reserved_out[j];
    EXPECT_NEAR(reserved,amount,1e-14);
    for(std::size_t j=0;j<nl;++j) {
        const double available=pore.contactAvailableMass(0,j);
        ASSERT_TRUE(pore.applyContactMassIncrement(0,j,-.9*available));
    }
    gw.heat().refreshView(0,tr);ASSERT_TRUE(gw.refreshPorewaterContactLedger().empty());
    EXPECT_GE(tr.sat_mass[0],amount);
    const auto saved=gw.transportRestartState();ASSERT_TRUE(gw.restoreTransportRestart(saved,true).empty());
    gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
    EXPECT_NEAR(tr.lost_link[0],amount,1e-14);EXPECT_NEAR(tr.residual(0),0,1e-9);
    for(double mass:tr.layer_mass)EXPECT_GE(mass,0);finish(r);
}

TEST(GwSigmaPorous, LayerSpecificKineticsReadProjectedTemperatureAndRetardation) {
    const auto body=mappedPorousDeck("[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-13\nRTOL 1e-11\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
        "[REACTION_SUBSURFACE]\n* UNSAT A RATE -.01*A*TEMP/20\n* UNSAT B RATE .01*A*TEMP/20\n"
        "[GW_INITIAL_QUALITY]\n* UNSAT A 7\n* SAT A 7\n* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 10\n* LAYER 2 __TEMPERATURE__ 30\n"
        "[GW_SORPTION]\n* A 2 0\n* B 2 0\n",Mesh::Quad,5,3);
    Deck r=open("mapped_local_kinetics",body);ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    auto& router=r.eng->surfaceRouter2D();auto& gw=router.subsurface();const auto& tr=gw.transport();const auto& w=gw.state();
    const int a=tr.rowIndex("A"),b=tr.rowIndex("B");ASSERT_GE(a,0);ASSERT_GE(b,0);
    std::vector<double> expected(tr.n_layers);double lo=1e30,hi=-1e30;
    for(int layer=0;layer<tr.n_layers;++layer) {
        double volume=0,mass=0,temperature=0;
        ASSERT_EQ(swmm_gw2d_get_layer_quality(r.e,0,layer,tr.temp_row,&volume,&mass,&temperature),SWMM_OK);
        lo=std::min(lo,temperature);hi=std::max(hi,temperature);
        const double theta=w.theta_sigma[layer*tr.n_cells];
        const double scale=theta/(theta+tr.kd[tr.idx(a,0)]*tr.rho_b[0]);
        expected[layer]=tr.layer_mass[tr.layerIdx(a,layer,0)]*std::exp(-.01*.1*temperature/20*scale);
    }
    ASSERT_GT(hi-lo,1);
    gw.assignTiers(.1,1);gw.fireGwCells(0,.1,router.state(),0);
    for(int layer=0;layer<tr.n_layers;++layer)EXPECT_NEAR(tr.layer_mass[tr.layerIdx(a,layer,0)],expected[layer],1e-8);
    EXPECT_NEAR(tr.residual(a),0,1e-9);EXPECT_NEAR(tr.residual(b),0,1e-9);finish(r);
}

TEST(GwSigmaPorous, UnspecifiedTemperatureUsesTheConfiguredThermalInitialState) {
    Deck r=open("mapped_thermal_default",mappedPorousDeck("",Mesh::Quad));ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e),SWMM_OK)<<completionErrors(r);
    const auto& ctx=r.eng->context();const auto& gw=r.eng->surfaceRouter2D().subsurface();
    const double expected=ctx.heat_config.global_temp[int(openswmm::HeatSource::INITIAL_STATE)];
    for(const auto& c:gw.heat().cells)for(std::size_t j=0;j<c.column.energy.size();++j)
        EXPECT_NEAR(c.column.energy[j]/c.column.capacity[j],expected,1e-12);
    finish(r);
}

TEST(GwSigmaPorous, ResultsKeepChemicalAndThermalLayerAxesIndependent) {
    auto body=mappedPorousDeck("[GW_INITIAL_QUALITY]\n* SAT TSS 7\n* UNSAT TSS 7\n"
        "* SAT __TEMPERATURE__ 20\n* UNSAT __TEMPERATURE__ 10\n",Mesh::Quad,5,3);
    const auto at=body.find("REPORT_2D NO");ASSERT_NE(at,std::string::npos);
    body.replace(at,12,"REPORT_2D YES\nOUTPUT_FILE mapped_layers.h5\nOUTPUT_PRECISION FLOAT64\nREPORT_2D_VARIABLES DEPTH GROUNDWATER");
    Deck r=open("mapped_results",body);ASSERT_TRUE(r.opened);ASSERT_TRUE(run(r))<<completionErrors(r);
    const auto expected=r.eng->surfaceRouter2D().subsurface().transport().layer_mass;
    const auto& tr=r.eng->surfaceRouter2D().subsurface().transport();const int ns=tr.n_species,nc=tr.n_cells;
    finish(r);
    hid_t file=H5Fopen((kOutDir/"mapped_layers.h5").string().c_str(),H5F_ACC_RDONLY,H5P_DEFAULT);ASSERT_GE(file,0);
    hid_t dataset=H5Dopen2(file,"Mesh2_face_gw_layer_mass",H5P_DEFAULT);ASSERT_GE(dataset,0);
    hid_t space=H5Dget_space(dataset);ASSERT_EQ(H5Sget_simple_extent_ndims(space),4);
    hsize_t dimensions[4]={};H5Sget_simple_extent_dims(space,dimensions,nullptr);
    EXPECT_EQ(dimensions[1],ns);EXPECT_EQ(dimensions[2],5);EXPECT_EQ(dimensions[3],nc);
    std::vector<double> values(std::size_t(dimensions[0])*ns*5*nc);
    ASSERT_GE(H5Dread(dataset,H5T_NATIVE_DOUBLE,H5S_ALL,H5S_ALL,H5P_DEFAULT,values.data()),0);
    const auto offset=values.size()-expected.size();
    for(std::size_t k=0;k<expected.size();++k)EXPECT_NEAR(values[offset+k],expected[k],1e-9);
    H5Sclose(space);H5Dclose(dataset);H5Fclose(file);
}
