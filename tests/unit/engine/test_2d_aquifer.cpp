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
 * @file test_2d_aquifer.cpp
 * @brief G1 (2026-09-07) — the `[2D_AQUIFER*]` authoring surface, its round
 *        trip, and the `swmm_gw2d_*` C API, on a model.
 *
 * @section why What this covers that the gates do not
 *
 * `tests/verification/gw2d_gates` proves the KERNEL conserves: it builds a
 * mesh and a soil in twenty lines and checks conservation identities with no
 * engine at all. Everything between a user's `.inp` and that kernel is
 * invisible to it — the section parser, the unit conversion, the resolution
 * onto cells, the writer, and the C API the GUI reads. A kernel that
 * conserves perfectly is still useless if `[2D_AQUIFER]` never reaches it, so
 * that path is what this file tests.
 *
 * Gates:
 *   - A1 an authored `[2D_AQUIFER]` resolves, the kernel goes live, its
 *     dimensions match the mesh, and it conserves over a run.
 *   - A2 the three sections round-trip through `swmm_model_write`.
 *   - A3 the C API's authoring surface — rows, optional properties and node
 *     beds — reads back what it wrote. This is the exact surface
 *     `Mesh2DAquiferModel` drives, and a silent failure here is a GUI that
 *     shows an empty table.
 *   - A4 `INFIL_DESTINATION AQUIFER_2D` is accepted WITH a `[2D_AQUIFER]`
 *     (and refused without — that half is in test_2d_infil_subcatch_aquifer).
 *   - A5 `SUBCATCH_AQUIFER` and `[2D_AQUIFER]` are refused together: one
 *     owner for the infiltrated water.
 *   - A6 the surface ledger counts water the aquifer hands BACK. Without it
 *     Dunne excess arrives as storage from nowhere and the 2D continuity
 *     error grows by exactly the returned volume.
 *
 * Artefacts under tests/output/aquifer_2d (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw2d.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_AQUIFER_TEST_OUT_DIR) / "aquifer_2d";

/**
 * @brief A two-cell 2D pan over a junction, in SI, with whatever aquifer and
 *        option text the caller wants.
 *
 * @param aquifer_sections the `[2D_AQUIFER*]` text (may be empty)
 * @param twod_options_extra extra `[2D_OPTIONS]` lines
 * @param init_depth ponded depth seeded on both cells (m) — this deck's water
 *        comes from the cells, not from rain, so a test can drive
 *        infiltration without a storm.
 */
std::string deck(const std::string& aquifer_sections,
                 const std::string& twod_options_extra = "",
                 double init_depth = 0.5, double infil_mm_hr = 50.0) {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
      << twod_options_extra << "\n"
         "[2D_VERTICES]\n"
         "0.0 0.0 -10.0\n10.0 0.0 -10.0\n10.0 10.0 -10.0\n0.0 10.0 -10.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 " << init_depth << "\n"
         "0 2 3 0.03 " << init_depth << "\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  " << infil_mm_hr
      << "  -  -  -  -\n\n"
      << aquifer_sections
      << "[REPORT]\nINPUT NO\n";
    return m.str();
}

/// A `*` aquifer under the whole mesh. KS is mm/hr and ALPHA 1/m here: the
/// project is SI, and the rows are authored in the project's own units.
std::string aquiferSection(const std::string& extra_row_keywords = "",
                           const std::string& options_lines = "",
                           const std::string& node_lines = "") {
    std::ostringstream m;
    if (!options_lines.empty())
        m << "[2D_AQUIFER_OPTIONS]\n" << options_lines << "\n";
    m << "[2D_AQUIFER]\n"
         ";;Scope KS ZS THETA_S THETA_R ALPHA\n"
         "*  36.0  4.0  0.45  0.10  2.0  " << extra_row_keywords << "\n\n";
    if (!node_lines.empty())
        m << "[2D_AQUIFER_NODE]\n" << node_lines << "\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    bool opened = false, started = false;
};

DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    DeckRun r;
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

DeckRun openFile(const fs::path& inp, const std::string& tag) {
    DeckRun r;
    r.e = swmm_engine_create();
    if (!r.e) return r;
    r.opened = swmm_engine_open(r.e, inp.string().c_str(),
                                (kOutDir / (tag + ".rpt")).string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(),
                                nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

bool run(DeckRun& r) {
    if (!r.opened) return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK) return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK) return false;
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    return true;
}

void finish(DeckRun& r) {
    if (!r.e) return;
    if (r.started) swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

/// Every ledger term, for a failure message. A continuity residual on its own
/// says only that something leaked; the terms say which channel.
std::string ledgerDump(SWMM_Engine e) {
    static const struct { int term; const char* name; } kTerms[] = {
        {SWMM_GW2D_LED_INFIL_IN,     "infil_in"},
        {SWMM_GW2D_LED_LATERAL,      "lateral"},
        {SWMM_GW2D_LED_RECHARGE,     "recharge (internal)"},
        {SWMM_GW2D_LED_CAPRISE,      "cap_rise (internal)"},
        {SWMM_GW2D_LED_DEEP,         "deep_out"},
        {SWMM_GW2D_LED_NODE,         "node_out"},
        {SWMM_GW2D_LED_ET,           "et_out"},
        {SWMM_GW2D_LED_DUNNE,        "dunne_out"},
        {SWMM_GW2D_LED_INIT_STORAGE, "init_storage"},
        {SWMM_GW2D_LED_STORAGE,      "storage_now"},
    };
    std::ostringstream s;
    s << "  gw ledger (m3):\n";
    for (const auto& t : kTerms) {
        double v = 0.0;
        if (swmm_gw2d_get_ledger(e, t.term, &v) == SWMM_OK)
            s << "    " << t.name << " = " << v << "\n";
    }
    return s.str();
}

std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

}  // namespace

// ---------------------------------------------------------------------------
// A1 — the kernel goes live from an authored section, and conserves
// ---------------------------------------------------------------------------
TEST(Aquifer2D, AuthoredSectionResolvesAndTheKernelConserves) {
    DeckRun r = openDeck("live", deck(aquiferSection()));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));

    int active = 0;
    ASSERT_EQ(swmm_gw2d_is_active(r.e, &active), SWMM_OK);
    EXPECT_NE(active, 0) << "a [2D_AQUIFER] row was authored but no kernel ran";

    int n_cells = 0, m_layers = 0;
    ASSERT_EQ(swmm_gw2d_get_dimensions(r.e, &n_cells, &m_layers), SWMM_OK);
    EXPECT_EQ(n_cells, 2) << "the kernel must cover every mesh cell";
    EXPECT_GE(m_layers, 1);

    // The kernel's own continuity residual. This is the number the gates
    // check on a bare mesh; here it also exercises the parse, the unit
    // conversion and the resolution onto cells.
    double resid = 0.0, storage = 0.0;
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &storage), SWMM_OK);
    EXPECT_GT(storage, 0.0) << "the aquifer holds no water at all";
    EXPECT_LT(std::fabs(resid), 1.0e-6 * std::fabs(storage) + 1.0e-9)
        << "residual " << resid << " against storage " << storage
        << "\n" << ledgerDump(r.e);

    // It actually received the surface's infiltration, rather than sitting
    // inert beside it.
    double infil_in = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INFIL_IN, &infil_in),
              SWMM_OK);
    EXPECT_GT(infil_in, 0.0)
        << "the aquifer received nothing — the surface is still losing its "
           "infiltration instead of recharging the column";

    // Per-cell reads: the water table must sit inside the soil column.
    for (int c = 0; c < n_cells; ++c) {
        double hg = -1.0;
        ASSERT_EQ(swmm_gw2d_get_cell(r.e, c, SWMM_GW2D_VAR_HG, &hg), SWMM_OK);
        EXPECT_GE(hg, 0.0);
        EXPECT_LE(hg, 4.0 + 1.0e-9) << "table above ZS on cell " << c;
    }
    finish(r);
}

// ---------------------------------------------------------------------------
// A2 — the three sections survive a save/reload
// ---------------------------------------------------------------------------
TEST(Aquifer2D, SectionsRoundTripThroughTheWriter) {
    // Every authored option here DIFFERS from its default. The writer omits
    // options that match the default — the same minimal-deck rule the rest of
    // the file uses — so authoring M_LAYERS 8 / DUNNE YES would produce no
    // section at all and prove nothing about the round trip.
    const std::string body =
        deck(aquiferSection("SOIL_CHAR GARDNER CLOSURE SIGMA M_LAYERS 12 "
                            "C_LOSS 0.5 HG0 1.5",
                            "M_LAYERS 12\nDUNNE NO\nC_GW 0.4\n",
                            // NODE CELL then keyword/value pairs — the bed's
                            // three numbers are optional and named, so a
                            // future fourth cannot renumber a saved file.
                            "J1  1  KC 0.36  DC 0.5  AREA 25.0\n"));
    DeckRun r = openDeck("rt", body);
    ASSERT_TRUE(r.opened);

    const fs::path saved = kOutDir / "rt_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("[2D_AQUIFER]"), std::string::npos);
    EXPECT_NE(text.find("[2D_AQUIFER_OPTIONS]"), std::string::npos);
    EXPECT_NE(text.find("[2D_AQUIFER_NODE]"), std::string::npos);
    // The keywords are what a positional-only writer would silently drop.
    EXPECT_NE(text.find("GARDNER"), std::string::npos);
    EXPECT_NE(text.find("SIGMA"), std::string::npos);
    // …and the node bed's named columns.
    EXPECT_NE(text.find("KC"), std::string::npos);
    EXPECT_NE(text.find("AREA"), std::string::npos);

    int n0 = 0, nb0 = 0;
    ASSERT_EQ(swmm_gw2d_row_count(r.e, &n0), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &nb0), SWMM_OK);
    double ks0 = 0.0, zs0 = 0.0, ts0 = 0.0, tr0 = 0.0, al0 = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get(r.e, 0, nullptr, nullptr, 0, nullptr,
                                &ks0, &zs0, &ts0, &tr0, &al0), SWMM_OK);
    double hg0_0 = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get_property(r.e, 0, "HG0", &hg0_0), SWMM_OK);
    finish(r);

    // Reopen the saved file: same counts, same numbers, in the same units.
    DeckRun r2 = openFile(saved, "rt2");
    ASSERT_TRUE(r2.opened) << "the file this engine wrote does not reopen";
    int n1 = 0, nb1 = 0;
    ASSERT_EQ(swmm_gw2d_row_count(r2.e, &n1), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_node_count(r2.e, &nb1), SWMM_OK);
    EXPECT_EQ(n1, n0);
    EXPECT_EQ(nb1, nb0);
    double ks1 = 0.0, zs1 = 0.0, ts1 = 0.0, tr1 = 0.0, al1 = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get(r2.e, 0, nullptr, nullptr, 0, nullptr,
                                &ks1, &zs1, &ts1, &tr1, &al1), SWMM_OK);
    EXPECT_DOUBLE_EQ(ks1, ks0) << "KS was converted on the way through";
    EXPECT_DOUBLE_EQ(zs1, zs0);
    EXPECT_DOUBLE_EQ(ts1, ts0);
    EXPECT_DOUBLE_EQ(tr1, tr0);
    EXPECT_DOUBLE_EQ(al1, al0);
    double hg0_1 = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get_property(r2.e, 0, "HG0", &hg0_1), SWMM_OK);
    EXPECT_DOUBLE_EQ(hg0_1, hg0_0);
    finish(r2);
}

// ---------------------------------------------------------------------------
// A3 — the authoring API the GUI models drive
// ---------------------------------------------------------------------------
TEST(Aquifer2D, CApiAuthorsRowsOptionsAndNodeBeds) {
    // Start from a deck with NO aquifer: the API must be able to create one
    // from nothing, which is what "New row" in the dialog does.
    DeckRun r = openDeck("api", deck(""));
    ASSERT_TRUE(r.opened);

    int n = -1;
    ASSERT_EQ(swmm_gw2d_row_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 0);

    ASSERT_EQ(swmm_gw2d_row_add(r.e, SWMM_GW2D_SCOPE_GLOBAL, nullptr, -1,
                                36.0, 4.0, 0.45, 0.10, 2.0), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_add(r.e, SWMM_GW2D_SCOPE_CELL, nullptr, 1,
                                18.0, 3.0, 0.40, 0.08, 1.5), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 2);

    int scope = -1, cell = -99;
    double ks = 0.0, zs = 0.0, ts = 0.0, tr = 0.0, al = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get(r.e, 1, &scope, nullptr, 0, &cell,
                                &ks, &zs, &ts, &tr, &al), SWMM_OK);
    EXPECT_EQ(scope, SWMM_GW2D_SCOPE_CELL);
    EXPECT_EQ(cell, 1);
    EXPECT_DOUBLE_EQ(ks, 18.0);
    EXPECT_DOUBLE_EQ(al, 1.5);

    // Optional properties, including the two that are enum codes carried as
    // doubles — the shape the dialog's combo boxes write.
    ASSERT_EQ(swmm_gw2d_row_set_property(r.e, 1, "SOIL_CHAR",
                                         SWMM_GW2D_SOIL_VAN_GENUCHTEN), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_set_property(r.e, 1, "CLOSURE",
                                         SWMM_GW2D_CLOSURE_SIGMA), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_set_property(r.e, 1, "HG0", 1.25), SWMM_OK);
    double v = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get_property(r.e, 1, "SOIL_CHAR", &v), SWMM_OK);
    EXPECT_EQ(static_cast<int>(v), SWMM_GW2D_SOIL_VAN_GENUCHTEN);
    ASSERT_EQ(swmm_gw2d_row_get_property(r.e, 1, "CLOSURE", &v), SWMM_OK);
    EXPECT_EQ(static_cast<int>(v), SWMM_GW2D_CLOSURE_SIGMA);
    ASSERT_EQ(swmm_gw2d_row_get_property(r.e, 1, "HG0", &v), SWMM_OK);
    EXPECT_DOUBLE_EQ(v, 1.25);

    // Options go through the same token spellings the .inp uses.
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "M_LAYERS", "16"), SWMM_OK);
    char buf[64] = {0};
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "M_LAYERS", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "16");

    // Node beds.
    ASSERT_EQ(swmm_gw2d_node_add(r.e, "J1", 0, 0.0, 0.0, 0.0), SWMM_OK);
    int nb = 0;
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &nb), SWMM_OK);
    EXPECT_EQ(nb, 1);
    char node[32] = {0};
    int ncell = -1;
    double kc = -1.0, dc = -1.0, area = -1.0;
    ASSERT_EQ(swmm_gw2d_node_get(r.e, 0, node, sizeof node, &ncell,
                                 &kc, &dc, &area), SWMM_OK);
    EXPECT_STREQ(node, "J1");
    EXPECT_EQ(ncell, 0);
    EXPECT_DOUBLE_EQ(kc, 0.0) << "0 is 'direct Darcy', not 'unset'";

    // Removal, from both tables.
    ASSERT_EQ(swmm_gw2d_row_remove(r.e, 0), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
    ASSERT_EQ(swmm_gw2d_row_get(r.e, 0, &scope, nullptr, 0, &cell,
                                &ks, nullptr, nullptr, nullptr, nullptr),
              SWMM_OK);
    EXPECT_EQ(cell, 1) << "removing row 0 must leave the OTHER row behind";
    ASSERT_EQ(swmm_gw2d_node_remove(r.e, 0), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &nb), SWMM_OK);
    EXPECT_EQ(nb, 0);

    // Out-of-range indices must be refused, not silently clamped: the dialog
    // deletes by row and a clamp would delete the wrong one.
    EXPECT_NE(swmm_gw2d_row_get(r.e, 99, &scope, nullptr, 0, &cell,
                                &ks, &zs, &ts, &tr, &al), SWMM_OK);
    EXPECT_NE(swmm_gw2d_row_remove(r.e, 99), SWMM_OK);

    // What was authored through the API must run.
    ASSERT_TRUE(run(r));
    int active = 0;
    ASSERT_EQ(swmm_gw2d_is_active(r.e, &active), SWMM_OK);
    EXPECT_NE(active, 0) << "rows added through the API never reached the kernel";
    finish(r);
}

// ---------------------------------------------------------------------------
// A4 — AQUIFER_2D is accepted once there is an aquifer to receive it
// ---------------------------------------------------------------------------
TEST(Aquifer2D, Aquifer2DDestinationIsAcceptedWithAnAquifer) {
    DeckRun r = openDeck("dest_ok",
                         deck(aquiferSection(), "INFIL_DESTINATION AQUIFER_2D\n"));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r))
        << "AQUIFER_2D with a [2D_AQUIFER] present must be accepted";
    double infil_in = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INFIL_IN, &infil_in),
              SWMM_OK);
    EXPECT_GT(infil_in, 0.0);
    finish(r);
}

// ---------------------------------------------------------------------------
// A5 — one owner for the infiltrated water
// ---------------------------------------------------------------------------
TEST(Aquifer2D, SubcatchAquiferAndA2DAquiferAreRefusedTogether) {
    DeckRun r = openDeck(
        "two_owners",
        deck(aquiferSection(), "INFIL_DESTINATION SUBCATCH_AQUIFER\n"));
    ASSERT_TRUE(r.opened);
    EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK)
        << "the same infiltration would be delivered to both aquifers";
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
}

// ---------------------------------------------------------------------------
// A6 — the surface ledger counts what the aquifer hands back
// ---------------------------------------------------------------------------
TEST(Aquifer2D, ReturnedSaturationExcessIsAnInflowToTheSurfaceLedger) {
    // A shallow soil under a deep pond: the column fills, the table reaches
    // the ground, and everything after that comes straight back up as Dunne
    // excess. HG0 starts the table 0.1 m below the surface so this happens
    // inside the half hour the deck runs.
    DeckRun r = openDeck(
        "dunne",
        deck("[2D_AQUIFER]\n*  36.0  0.5  0.45  0.10  2.0  HG0 0.45\n\n",
             "", /*init_depth=*/1.0, /*infil_mm_hr=*/200.0));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));

    const auto& mb = r.eng->context().mass_balance_2d;
    double dunne = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_DUNNE, &dunne), SWMM_OK);
    ASSERT_GT(dunne, 0.0) << "the deck never saturated; the gate proves nothing";

    // The surface's inflow term must account for the water it actually took
    // back — everything handed over, less anything still parked in the
    // accumulator waiting for its cell to fire.
    EXPECT_GT(mb.aquifer_in, 0.0)
        << "saturation excess reached the surface but no inflow was booked; "
           "the 2D continuity check is reading it as storage from nowhere";
    EXPECT_LE(mb.aquifer_in, dunne + 1.0e-9)
        << "the surface cannot have taken more than the aquifer handed over";

    // …and with the term in place, the surface balance closes.
    EXPECT_LT(std::fabs(mb.error()), 0.01)
        << "2D continuity error " << mb.error()
        << " with aquifer_in = " << mb.aquifer_in;
    finish(r);
}

// ---------------------------------------------------------------------------
// A2b — the writer's minimal-deck rule: an option left at its default is not
// written back. This is what makes an untouched load-and-save a no-op, and it
// is why A2 has to author values that differ.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, DefaultOptionsAreNotWrittenBack) {
    DeckRun r = openDeck("rt_defaults",
                         deck(aquiferSection("", "M_LAYERS 8\nDUNNE YES\n")));
    ASSERT_TRUE(r.opened);
    const fs::path saved = kOutDir / "rt_defaults_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("[2D_AQUIFER]"), std::string::npos)
        << "the rows themselves are never optional";
    EXPECT_EQ(text.find("[2D_AQUIFER_OPTIONS]"), std::string::npos)
        << "every authored option equalled its default; writing the section "
           "back would grow the deck on every save";
    finish(r);
}
