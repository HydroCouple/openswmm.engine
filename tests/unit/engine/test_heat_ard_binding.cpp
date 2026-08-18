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
 * @file test_heat_ard_binding.cpp
 * @brief Phase H4 gates — `__TEMPERATURE__` on the Eulerian ARD mesh.
 *
 * @details H4 makes temperature a mesh row (after pollutants, MSX and age),
 *          so it inherits advection, FCT, node mixing, structure
 *          passthrough and dispersion from the shared kernels, and adds a
 *          PER-CELL surface-flux stage that the LEGACY mirror can only do
 *          per link.
 *
 *          The gate this suite exists for is the first one. `publish()`
 *          dispatches rows as `if (age) … else if (s < np) … else MSX`, and
 *          the MSX row count is `ns − np − na`. A temperature row without
 *          its own branch falls into the MSX arm and writes past
 *          `msx_*_conc`; an uncorrected `nm` reports one MSX row too many.
 *          Neither shows up as a wrong temperature — they corrupt the
 *          POLLUTANT and MSX arrays instead, which is why the first gate
 *          asserts pollutants and MSX rows on a heat deck rather than the
 *          temperature. That is lesson 14's shape (a stride/row-count change
 *          swept a neighbour) and lesson 51's (a category gaining a second
 *          member).
 *
 *          The last gate is the other half of the phase. Binding the row to
 *          the mesh buys per-cell fluxes, and "per cell" is an area claim:
 *          each cell's free surface is its own top width times its own
 *          length, barrel-scaled once. Asking only whether the water got
 *          cooler leaves that claim untested, so the area is compared
 *          against the LEGACY mirror's, on a deck with two barrels.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H4
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

constexpr double kQ      = 5.0;
constexpr double kTIn    = 30.0;   ///< °C, external inflow
constexpr double kTInit  = 5.0;    ///< °C, INITIAL_STATE — distinct from all
constexpr double kCinit  = 42.0;   ///< mg/L TSS

void write_file(const char* path, const std::string& body) {
    std::ofstream f(path);
    f << body;
}

/// Flow-through chain so a thermal signal has somewhere to travel.
/// `solver` selects the engine; `open_channel` selects RECT_OPEN vs CIRCULAR
/// so the mesh's `is_open` gate can be exercised by geometry; `end_time`
/// selects whether the chain is caught mid-transient or fully flushed, which
/// is the difference between a gate that can see the initial state and one
/// that cannot; `barrels` is what tells a correct exchange area from one
/// that applies the barrel count twice.
void write_deck(const char* path, const char* solver,
                const std::string& pc_lines, bool pollutants = true,
                bool open_channel = false,
                const std::string& extra_options = "",
                const char* end_time = "01:00:00", int barrels = 1,
                const char* routing_step = "5") {
    std::ofstream f(path);
    const std::string nb = " " + std::to_string(barrels);
    f << "[TITLE]\nH4 ARD heat binding gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "HEAT_TRANSPORT ON\nQUALITY_SOLVER " << solver << "\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME " << end_time << "\n"
      << "ROUTING_STEP " << routing_step << "\nREPORT_STEP 00:05:00\n"
      << extra_options << "\n"
      << "[TEMPERATURE]\nTIMESERIES air_ts\nHUMIDITY 50\n\n"
      << "[TIMESERIES]\nair_ts 01/01/2026 00:00 50.0\n"
      << "air_ts 01/02/2026 00:00 50.0\n\n"
      << "[JUNCTIONS]\n"
      << "J0 10.0 10 1.5 0 0\nJ1 9.4 10 1.5 0 0\nJ2 8.8 10 1.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 8.0 FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1 J0 J1  500 0.013 0 0 0\nC2 J1 J2 500 0.013 0 0 0\n"
      << "C3 J2 OUT 500 0.013 0 0 0\n\n"
      << "[XSECTIONS]\n"
      << (open_channel
              ? "C1 RECT_OPEN 3.0 4.0 0 0" + nb + "\nC2 RECT_OPEN 3.0 4.0 0 0" +
                    nb + "\nC3 RECT_OPEN 3.0 4.0 0 0" + nb + "\n\n"
              : "C1 CIRCULAR 3.0 0 0 0" + nb + "\nC2 CIRCULAR 3.0 0 0 0" + nb +
                    "\nC3 CIRCULAR 3.0 0 0 0" + nb + "\n\n")
      << "[INFLOWS]\nJ0 FLOW \"\" FLOW 1.0 1.0 " << kQ << "\n\n";
    if (pollutants)
        f << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
             "Cdwf Cinit\n"
          << "TSS    MG/L  0     0   0     0      NO       *        0      0    "
          << kCinit << "\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

/// Two MSX bulk species. `publish()` derives the MSX row count by
/// SUBTRACTING the reserved rows from `ns`, and dispatches anything that is
/// neither a reserved row nor a pollutant into the MSX arm — so neither the
/// subtraction nor the dispatch is exercised at all unless a deck actually
/// carries MSX rows.
void write_rxn(const char* path) {
    write_file(path,
               "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
               "[REACTION_SPECIES]\nBULK X MG\nBULK Y MG\n"
               "[REACTION_PIPES]\nRATE X 0\nRATE Y 0\n"
               "[REACTION_TANKS]\nRATE X 0\nRATE Y 0\n"
               "[REACTION_QUALITY]\nGLOBAL X 8\nGLOBAL Y 3\n");
}

std::string heat_cfg(const std::string& flux_rows = "") {
    return "[HEAT_SOURCES]\n"
           "EXTERNAL_INFLOW GLOBAL " + std::to_string(kTIn) + "\n"
           "INITIAL_STATE   GLOBAL " + std::to_string(kTInit) + "\n" +
           (flux_rows.empty() ? "" : "\n[HEAT_FLUXES]\n" + flux_rows);
}

SWMM_Engine run_and_hold(const char* inp, const char* rpt, const char* out) {
    SWMM_Engine e = swmm_engine_create();
    if (e == nullptr) return nullptr;
    if (swmm_engine_open(e, inp, rpt, out, nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    double elapsed = 0.0;
    int guard = 0;
    do {
        if (swmm_engine_step(e, &elapsed) != SWMM_OK) break;
    } while (elapsed > 0.0 && ++guard < 20000);
    swmm_engine_end(e);
    return e;
}

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — adding the temperature row must not disturb the pollutant or MSX
// rows.
//
// THE defect gate. A missing `s == temp_row_` branch in publish() sends the
// row into the MSX arm; an uncorrected `nm = ns - np - na` counts one MSX row
// too many. Both corrupt POLLUTANTS and MSX, not temperature — so this gate
// reads them on a heat deck and compares against the identical deck with heat
// off. A differential, so it cannot be fooled by a plausible-looking value.
//
// Two properties of the DECK do the work here, and the gate is blind without
// either:
//
//   * It carries MSX species. The dispatch arm and the subtraction that
//     derive the MSX row count are unreachable on a pollutant-only deck —
//     `s` is either a pollutant or a reserved row and never falls through.
//     Reverting `nm` to the pre-H4 expression changes nothing observable
//     until a deck has rows for it to miscount.
//   * It stops at five minutes. Over an hour the clean inflow flushes every
//     row to ~1e-22, and a differential over near-zeros is a comparison
//     between two kinds of nothing. Five minutes leaves TSS at 8.8/34.2/40.0
//     and the MSX rows at 1.7/0.6, so a misindexed write has somewhere
//     visible to land.
// ---------------------------------------------------------------------------
TEST(HeatArdBindingTest, AddingTheTemperatureRowLeavesPollutantsAndMsxIntact) {
    write_file("_h4.heat", heat_cfg());
    write_rxn("_h4.rxn");
    const std::string rxn_pc =
        "org.hydrocouple.openswmm.reactions config=\"_h4.rxn\"";

    write_deck("_h4_off.inp", "EULERIAN_ARD", rxn_pc, /*pollutants=*/true,
               /*open_channel=*/false, /*extra_options=*/"",
               /*end_time=*/"00:05:00");
    // Heat off: strip the option by rewriting the one line.
    {
        std::ifstream in("_h4_off.inp");
        std::string body((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
        const auto at = body.find("HEAT_TRANSPORT ON\n");
        ASSERT_NE(at, std::string::npos);
        body.erase(at, std::string("HEAT_TRANSPORT ON\n").size());
        write_file("_h4_off.inp", body);
    }
    SWMM_Engine off = run_and_hold("_h4_off.inp", "_h4_off.rpt", "_h4_off.out");
    ASSERT_NE(off, nullptr);
    const auto& off_ctx  = as_cpp_engine(off).context();
    const auto conc_ref  = off_ctx.nodes.conc;
    const auto lconc_ref = off_ctx.links.conc;
    const auto mnode_ref = off_ctx.reactions.msx_node_conc;
    const auto mlink_ref = off_ctx.reactions.msx_link_conc;
    swmm_engine_destroy(off);

    write_deck("_h4_on.inp", "EULERIAN_ARD",
               rxn_pc + "\norg.hydrocouple.openswmm.heat config=\"_h4.heat\"",
               /*pollutants=*/true, /*open_channel=*/false,
               /*extra_options=*/"", /*end_time=*/"00:05:00");
    SWMM_Engine on = run_and_hold("_h4_on.inp", "_h4_on.rpt", "_h4_on.out");
    ASSERT_NE(on, nullptr);
    const auto& ctx = as_cpp_engine(on).context();

    // SETUP: the heat row really is on the mesh, or this comparison is
    // trivially true (lesson 36).
    ASSERT_FALSE(ctx.heat_state.node_temp.empty())
        << "heat_state never sized — the ARD row was not created, so an "
           "unchanged pollutant array proves nothing";
    // SETUP: and the reference fields are worth comparing. An hour-long run
    // flushes them to ~1e-22, at which point every EXPECT_NEAR below passes
    // no matter where a stray write lands.
    ASSERT_FALSE(mnode_ref.empty())
        << "no MSX rows on the mesh — the dispatch arm and the row-count "
           "subtraction this gate exists to protect are both unreachable";
    ASSERT_GT(*std::max_element(conc_ref.begin(), conc_ref.end()), 1.0)
        << "the pollutant field flushed before the comparison";
    ASSERT_GT(*std::max_element(mnode_ref.begin(), mnode_ref.end()), 0.1)
        << "the MSX field flushed before the comparison";

    // The SIZES first. Leaving temperature out of the `nm` subtraction does
    // not move a value — it widens the stride, so every consumer that
    // computes its own index reads a neighbouring species.
    ASSERT_EQ(mnode_ref.size(), ctx.reactions.msx_node_conc.size())
        << "the MSX node array changed stride when the temperature row was "
           "added: " << mnode_ref.size() << " -> "
        << ctx.reactions.msx_node_conc.size()
        << ". publish() derives the MSX row count by subtracting the reserved "
           "rows from ns; a reserved row left out of that subtraction inflates "
           "the count by one.";
    ASSERT_EQ(mlink_ref.size(), ctx.reactions.msx_link_conc.size());
    ASSERT_EQ(conc_ref.size(), ctx.nodes.conc.size());
    ASSERT_EQ(lconc_ref.size(), ctx.links.conc.size());

    const auto same = [](const char* what, const std::vector<double>& ref,
                         const std::vector<double>& got) {
        for (std::size_t i = 0; i < ref.size(); ++i)
            EXPECT_NEAR(ref[i], got[i], 1.0e-12)
                << what << " slot " << i << " moved when the temperature row "
                   "was added to the mesh: " << ref[i] << " -> " << got[i]
                << ". The row is published by a dispatch chain that ends in an "
                   "MSX else-branch, and the MSX row count is derived by "
                   "subtraction — either can corrupt this array without any "
                   "temperature looking wrong.";
    };
    same("pollutant node", conc_ref, ctx.nodes.conc);
    same("pollutant link", lconc_ref, ctx.links.conc);
    same("MSX node", mnode_ref, ctx.reactions.msx_node_conc);
    same("MSX link", mlink_ref, ctx.reactions.msx_link_conc);
    swmm_engine_destroy(on);
}

// ---------------------------------------------------------------------------
// Gate 2 — the mesh transports temperature, and agrees with LEGACY.
//
// The cross-engine check (the G-UT2 analogue). Both engines carry the same
// physics on the same deck; they discretise it differently, so they agree to
// scheme tolerance rather than exactly — which is the honest claim.
// ---------------------------------------------------------------------------
TEST(HeatArdBindingTest, ArdAndLegacyAgreeOnTransportedTemperature) {
    write_file("_h4x.heat", heat_cfg());

    write_deck("_h4_leg.inp", "LEGACY",
               "org.hydrocouple.openswmm.heat config=\"_h4x.heat\"");
    SWMM_Engine l = run_and_hold("_h4_leg.inp", "_h4_leg.rpt", "_h4_leg.out");
    ASSERT_NE(l, nullptr);
    const auto leg = as_cpp_engine(l).context().heat_state.node_temp;
    swmm_engine_destroy(l);

    write_deck("_h4_ard.inp", "EULERIAN_ARD",
               "org.hydrocouple.openswmm.heat config=\"_h4x.heat\"");
    SWMM_Engine a = run_and_hold("_h4_ard.inp", "_h4_ard.rpt", "_h4_ard.out");
    ASSERT_NE(a, nullptr);
    const auto ard = as_cpp_engine(a).context().heat_state.node_temp;
    swmm_engine_destroy(a);

    ASSERT_FALSE(leg.empty());
    ASSERT_EQ(leg.size(), ard.size());

    // SETUP: both engines must have moved OFF the initial state, or an
    // agreement at 5 degC would just mean neither transported anything.
    EXPECT_GT(leg[0], kTInit + 1.0)
        << "LEGACY did not warm J0 above its 5 degC seed — nothing was "
           "transported, so agreement below is vacuous";
    EXPECT_GT(ard[0], kTInit + 1.0)
        << "ARD did not warm J0 above its 5 degC seed — the mesh row is "
           "inert";

    for (std::size_t i = 0; i < leg.size(); ++i)
        EXPECT_NEAR(leg[i], ard[i], 2.0)
            << "node " << i << ": LEGACY " << leg[i] << " degC vs ARD "
            << ard[i] << ". A gap far larger than scheme tolerance means one "
               "engine is not transporting heat at all — check which is at "
               "the 5 degC seed.";

    // The comparison above is made at the END of the hour, by which point
    // both engines have flushed to the 30 degC inflow — which means it cannot
    // see the INITIAL_STATE at all: seeding the mesh cells from a zeroed
    // array instead of the heat config produces the identical 30 degC field
    // and the identical agreement. Five minutes puts the thermal front
    // mid-chain, where the last conduit is still holding its seed.
    write_deck("_h4_seed.inp", "EULERIAN_ARD",
               "org.hydrocouple.openswmm.heat config=\"_h4x.heat\"",
               /*pollutants=*/true, /*open_channel=*/false,
               /*extra_options=*/"", /*end_time=*/"00:05:00");
    SWMM_Engine sd = run_and_hold("_h4_seed.inp", "_h4_seed.rpt",
                                  "_h4_seed.out");
    ASSERT_NE(sd, nullptr);
    const auto& sctx = as_cpp_engine(sd).context();
    ASSERT_FALSE(sctx.heat_state.link_temp.empty());
    // SETUP: the front must be mid-chain, or "the tail still holds the seed"
    // is just "nothing moved".
    EXPECT_GT(sctx.heat_state.link_temp.front(), kTInit + 5.0)
        << "C1 reads " << sctx.heat_state.link_temp.front()
        << " degC five minutes in — the 30 degC inflow has not entered the "
           "mesh, so the tail below proves nothing about the seed";
    EXPECT_NEAR(sctx.heat_state.link_temp.back(), kTInit, 1.0)
        << "C3 reads " << sctx.heat_state.link_temp.back()
        << " degC five minutes in, with the front still upstream of it. Its "
           "cells were seeded from INITIAL_STATE (" << kTInit
        << " degC); a reading near 0 means the seed came from a zeroed array "
           "rather than the heat config.";
    swmm_engine_destroy(sd);
}

// ---------------------------------------------------------------------------
// Gate 3 — per-cell surface fluxes act on the mesh.
// ---------------------------------------------------------------------------
TEST(HeatArdBindingTest, SurfaceFluxesActPerCellUnderArd) {
    // Open channel so the cells have a free surface at all.
    write_file("_h4f_on.heat", heat_cfg("SURFACE_EXCHANGE ON\n"));
    write_deck("_h4f_on.inp", "EULERIAN_ARD",
               "org.hydrocouple.openswmm.heat config=\"_h4f_on.heat\"",
               /*pollutants=*/false, /*open_channel=*/true);
    SWMM_Engine on = run_and_hold("_h4f_on.inp", "_h4f_on.rpt", "_h4f_on.out");
    ASSERT_NE(on, nullptr);
    const auto t_on = as_cpp_engine(on).context().heat_state.link_temp;
    swmm_engine_destroy(on);

    write_file("_h4f_off.heat", heat_cfg("SURFACE_EXCHANGE OFF\n"));
    write_deck("_h4f_off.inp", "EULERIAN_ARD",
               "org.hydrocouple.openswmm.heat config=\"_h4f_off.heat\"",
               /*pollutants=*/false, /*open_channel=*/true);
    SWMM_Engine off = run_and_hold("_h4f_off.inp", "_h4f_off.rpt",
                                   "_h4f_off.out");
    ASSERT_NE(off, nullptr);
    const auto t_off = as_cpp_engine(off).context().heat_state.link_temp;
    swmm_engine_destroy(off);

    ASSERT_FALSE(t_on.empty());
    ASSERT_EQ(t_on.size(), t_off.size());
    EXPECT_LT(t_on[0], t_off[0] - 1.0e-9)
        << "C1 reads " << t_on[0] << " degC with per-cell surface exchange on "
           "and " << t_off[0] << " off. 30 degC water under 10 degC air at "
           "50 % RH evaporates and cools; identical values mean the per-cell "
           "stage never ran.";
}

// ---------------------------------------------------------------------------
// Gate 4 — closed conduits have no free surface on the mesh either.
//
// The `FvGeometry::is_open` gate, the mesh twin of the LEGACY path's
// `xsect::isOpen`. It also keeps the Preissmann slot out of the area: a
// surcharged closed pipe reports a slot width that is numerical, not a water
// surface, and would otherwise be treated as one.
// ---------------------------------------------------------------------------
TEST(HeatArdBindingTest, ClosedConduitCellsDoNotExchange) {
    write_file("_h4c.heat", heat_cfg("SURFACE_EXCHANGE ON\n"));
    write_deck("_h4c_closed.inp", "EULERIAN_ARD",
               "org.hydrocouple.openswmm.heat config=\"_h4c.heat\"",
               /*pollutants=*/false, /*open_channel=*/false);
    SWMM_Engine c = run_and_hold("_h4c_closed.inp", "_h4c_closed.rpt",
                                 "_h4c_closed.out");
    ASSERT_NE(c, nullptr);
    const auto closed = as_cpp_engine(c).context().heat_state.link_temp;
    swmm_engine_destroy(c);

    write_file("_h4c_off.heat", heat_cfg("SURFACE_EXCHANGE OFF\n"));
    write_deck("_h4c_off.inp", "EULERIAN_ARD",
               "org.hydrocouple.openswmm.heat config=\"_h4c_off.heat\"",
               /*pollutants=*/false, /*open_channel=*/false);
    SWMM_Engine o = run_and_hold("_h4c_off.inp", "_h4c_off.rpt",
                                 "_h4c_off.out");
    ASSERT_NE(o, nullptr);
    const auto ref = as_cpp_engine(o).context().heat_state.link_temp;
    swmm_engine_destroy(o);

    ASSERT_FALSE(closed.empty());
    ASSERT_EQ(closed.size(), ref.size());
    for (std::size_t i = 0; i < closed.size(); ++i)
        EXPECT_NEAR(closed[i], ref[i], 1.0e-12)
            << "closed conduit " << i << " exchanged heat with the "
               "atmosphere: " << closed[i] << " vs " << ref[i]
            << ". A CIRCULAR section has no free surface, and its "
               "Preissmann slot width is a numerical device, not water.";
}

// ---------------------------------------------------------------------------
// Gate 5 — the per-cell exchange AREA is the LEGACY area, cell by cell.
//
// Gates 3 and 4 ask whether the flux stage runs and whether it is gated on
// `is_open`. Neither asks how BIG it is: gate 3's assertion is "cooler than
// with the module off, by more than 1e-9", which an area that is double, or
// half, or barrel-blind satisfies just as well. So the design decision this
// phase turns on — free surface = top width(depth) x cell_dx, barrel-scaled
// exactly once by FvGeometry — has no observer.
//
// This is that observer. Run the SAME deck through both engines for the full
// hour: by then the transport difference between a CSTR chain and an advected
// mesh has washed out (both sit at the inflow temperature), so the only thing
// still separating the two node fields is the surface-flux term. The LEGACY
// side computes its area as `getWofY(depth) x length x barrels` (H2), which
// makes this a direct comparison of the two area expressions.
//
// TWO BARRELS deliberately: `widthOfDepth` already multiplies by
// `FvGeometry::barrel_scale`, so on a single-barrel deck an implementation
// that applies `barrels` a second time is indistinguishable from a correct
// one.
// ---------------------------------------------------------------------------
TEST(HeatArdBindingTest, PerCellFluxAreaMatchesTheLegacyPath) {
    write_file("_h4a.heat", heat_cfg("SURFACE_EXCHANGE ON\n"));
    const std::string pc =
        "org.hydrocouple.openswmm.heat config=\"_h4a.heat\"";

    write_deck("_h4a_leg.inp", "LEGACY", pc, /*pollutants=*/false,
               /*open_channel=*/true, /*extra_options=*/"",
               /*end_time=*/"01:00:00", /*barrels=*/2);
    SWMM_Engine l = run_and_hold("_h4a_leg.inp", "_h4a_leg.rpt",
                                 "_h4a_leg.out");
    ASSERT_NE(l, nullptr);
    const auto leg = as_cpp_engine(l).context().heat_state.node_temp;
    swmm_engine_destroy(l);

    write_deck("_h4a_ard.inp", "EULERIAN_ARD", pc, /*pollutants=*/false,
               /*open_channel=*/true, /*extra_options=*/"",
               /*end_time=*/"01:00:00", /*barrels=*/2);
    SWMM_Engine a = run_and_hold("_h4a_ard.inp", "_h4a_ard.rpt",
                                 "_h4a_ard.out");
    ASSERT_NE(a, nullptr);
    const auto ard = as_cpp_engine(a).context().heat_state.node_temp;
    swmm_engine_destroy(a);

    ASSERT_FALSE(leg.empty());
    ASSERT_EQ(leg.size(), ard.size());

    // SETUP: there must BE a flux to compare. With the module inert both
    // fields sit at 30 degC and agree perfectly for the wrong reason.
    const double cooling = kTIn - leg.back();
    ASSERT_GT(cooling, 0.1)
        << "the LEGACY reference cooled only " << cooling
        << " degC over the hour — too little signal to size an area against";

    // 0.05 degC against a 0.34 degC signal: the two schemes place the same
    // total area differently along the chain, which is worth about 0.01 degC
    // at the outfall. Applying barrels twice, or dropping the cell length,
    // moves it by the whole signal.
    for (std::size_t i = 0; i < leg.size(); ++i)
        EXPECT_NEAR(leg[i], ard[i], 0.05)
            << "node " << i << ": LEGACY " << leg[i] << " degC vs ARD "
            << ard[i] << ", against a total cooling of " << cooling
            << " degC. The two paths must build the same exchange area — "
               "LEGACY as top width x length x barrels, the mesh as the sum "
               "of width(h_c) x dx_c with barrels already inside width().";
}

// ---------------------------------------------------------------------------
// Gate 6 — the flux is applied ONCE PER ROUTING STEP, not once per substep.
//
// §4.3's Lie split puts the surface-flux stage in the aging slot, at routing
// cadence, after the advection–dispersion subcycle. Moving it inside the
// subcycle at the full `dt` would apply it `nsub` times over — and nothing
// else in this suite can see that, because every deck here meshes to 12 cells
// and never subcycles at all (nsub == 1 at ROUTING_STEP 5, so "per substep"
// and "per routing step" are the same thing).
//
// So this gate runs the deck at a routing step coarse enough to force real
// subcycling (nsub == 4 at 60 s) and requires the hour's total cooling to
// match the finely-stepped run. A source term applied once per routing step
// is step-independent to the accuracy of the split; one applied per substep
// scales with the substep count.
// ---------------------------------------------------------------------------
TEST(HeatArdBindingTest, SurfaceFluxIsAppliedOncePerRoutingStep) {
    write_file("_h4s.heat", heat_cfg("SURFACE_EXCHANGE ON\n"));
    const std::string pc =
        "org.hydrocouple.openswmm.heat config=\"_h4s.heat\"";

    write_deck("_h4s_fine.inp", "EULERIAN_ARD", pc, /*pollutants=*/false,
               /*open_channel=*/true, /*extra_options=*/"",
               /*end_time=*/"01:00:00", /*barrels=*/2, /*routing_step=*/"5");
    SWMM_Engine f = run_and_hold("_h4s_fine.inp", "_h4s_fine.rpt",
                                 "_h4s_fine.out");
    ASSERT_NE(f, nullptr);
    const auto fine = as_cpp_engine(f).context().heat_state.node_temp;
    swmm_engine_destroy(f);

    write_deck("_h4s_coarse.inp", "EULERIAN_ARD", pc, /*pollutants=*/false,
               /*open_channel=*/true, /*extra_options=*/"",
               /*end_time=*/"01:00:00", /*barrels=*/2, /*routing_step=*/"60");
    SWMM_Engine c = run_and_hold("_h4s_coarse.inp", "_h4s_coarse.rpt",
                                 "_h4s_coarse.out");
    ASSERT_NE(c, nullptr);
    const auto coarse = as_cpp_engine(c).context().heat_state.node_temp;
    swmm_engine_destroy(c);

    ASSERT_FALSE(fine.empty());
    ASSERT_EQ(fine.size(), coarse.size());

    // SETUP: without a flux both runs sit at 30 degC and agree for the wrong
    // reason.
    ASSERT_GT(kTIn - fine.back(), 0.1)
        << "no cooling at the fine step — nothing to be step-independent "
           "about";

    for (std::size_t i = 0; i < fine.size(); ++i)
        EXPECT_NEAR(fine[i], coarse[i], 0.05)
            << "node " << i << ": " << fine[i] << " degC at a 5 s routing "
               "step vs " << coarse[i] << " degC at 60 s. The coarse run "
               "subcycles the transport four ways; the surface flux must "
               "still be applied once for the whole routing step, so the "
               "hour's cooling cannot depend on the step.";
}
