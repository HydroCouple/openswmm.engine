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
#include <openswmm/engine/openswmm_model.h>

#include "core/SWMMEngine.hpp"
#include "2d/SurfaceRouter2D.hpp"          // G1-c item 2 gate reads the state
#include "2d/subsurface/SubsurfaceSolver.hpp"

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

bool warnedAbout(const DeckRun& r, const char* needle) {   // G-X2
    for (const auto& w : r.eng->context().warnings)
        if (w.find(needle) != std::string::npos) return true;
    return false;
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
    // G1-c (2026-09-19): the AQUIFER's own residual closes too. This deck's
    // thin unsaturated zone resolves to the ENSLAVED closure, whose unbounded
    // Newton used to zero the table and lose 10 m³ of the 10 m³ delivered
    // (−32 % here) — the Dunne it returned was 0.001 m³ of the ~8 m³ owed.
    double resid = 0.0, storage = 0.0;
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &storage), SWMM_OK);
    EXPECT_LT(std::fabs(resid), 1.0e-6 * storage + 1.0e-9)
        << "aquifer residual " << resid << "\n" << ledgerDump(r.e);
    EXPECT_GT(dunne, 1.0) << "a full column under 10 m3 of infiltration must return most of it";
    finish(r);
}

// ---------------------------------------------------------------------------
// G1-c item 2 — exfiltration into a DRY cell. Nothing on the surface moves:
// no rain, no ponded water, no coupling. Cell 1's column is full (table at
// the ground), cell 2's sits 1 cm below its ground; lateral Darcy flow lifts
// cell 2's table to the surface and the Dunne excess is owed to a surface
// cell that nothing else would ever activate. The marcher's rebuild must pin
// it active from `pendingSurfaceCells()` (the seed that does not come from
// the surface's own state), the cell must actually receive the water, and
// nothing may be left parked in `xacc_to_surface` at the end.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, ExfiltrationReachesADryCellThroughThePendingSeed) {
    // The helper's mesh is flat, and on a flat mesh two tables equalise
    // BELOW both grounds; cell 1 must sit higher for its table to stand
    // above cell 2's ground. Vertex 1 is raised so cell 1's centroid is at
    // −9.667 m: its full column's table (−9.667) is 0.33 m above cell 2's
    // ground (−10.0), and lateral flow can only leave by exfiltrating there.
    std::string body = deck("[2D_AQUIFER]\n"
                            "CELL 1  36000.0  2.0  0.45  0.10  2.0  HG0 2.0\n"
                            "CELL 2  36000.0  0.5  0.45  0.10  2.0  HG0 0.49\n\n",
                            "", /*init_depth=*/0.0, /*infil_mm_hr=*/0.0);
    const std::string flat = "0.0 0.0 -10.0\n10.0 0.0 -10.0\n";
    const auto at = body.find(flat);
    ASSERT_NE(at, std::string::npos);
    body.replace(at, flat.size(), "0.0 0.0 -10.0\n10.0 0.0 -9.0\n");
    DeckRun r = openDeck("dry_exfil", body);
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));

    const auto& ctx = r.eng->context();
    const auto& mb  = ctx.mass_balance_2d;
    double dunne = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_DUNNE, &dunne), SWMM_OK);
    ASSERT_GT(dunne, 0.0) << "cell 2's table never reached the ground; the gate proves nothing"
                          << ledgerDump(r.e);
    // The surface took everything the aquifer handed over — nothing parked.
    const auto& st = r.eng->surfaceRouter2D().state();
    const auto& gws = r.eng->surfaceRouter2D().subsurface().state();
    double parked = 0.0;
    for (const double v : gws.xacc_to_surface) parked += v;
    EXPECT_NEAR(parked, 0.0, 1.0e-9 * dunne)
        << "exfiltrated water is still waiting for a surface cell to fire";
    EXPECT_NEAR(mb.aquifer_in, dunne, 1.0e-9 * dunne);
    // …and it is on cell 2 (the dry one), not lost: the surface storage grew
    // by the Dunne volume (no rain, no evaporation, no other source).
    const double surf = st.volume[0] + st.volume[1];
    EXPECT_GT(st.volume[1], 0.0) << "the dry cell above the saturated column stayed dry";
    EXPECT_NEAR(surf, dunne, 1.0e-6 * dunne + 1.0e-12)
        << "surface storage " << surf << " vs Dunne " << dunne;
    EXPECT_LT(std::fabs(mb.error()), 1.0e-6) << "2D continuity error " << mb.error();
    finish(r);
}

// ---------------------------------------------------------------------------
// G-X1 gate (b) — flooded manhole recharge. J1 is held surcharged by a
// constant inflow it cannot pass; its bed sits on cell 1 whose column starts
// 0.1 m short of the ground. The node recharges the aquifer at the
// conductance rate, the column fills, and the exchange goes to ZERO as
// h_g → z_s — the saturation guard — without the recharge ever flipping
// sign (no ping-pong) and without a drop of Dunne excess (water the column
// accepted and returned in the same firing). Before G-X1 the recharge
// direction was uncapped: the full column kept taking water and handing it
// back as Dunne every firing.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, FloodedManholeRechargesUntilTheColumnIsFullThenStops) {
    std::string body = deck("[2D_AQUIFER]\n"
                            "*  36.0  4.0  0.45  0.10  2.0  HG0 3.9\n\n"
                            "[2D_AQUIFER_NODE]\nJ1  1\n\n",
                            "", /*init_depth=*/0.0, /*infil_mm_hr=*/0.0);
    // a surcharging inflow: 0.5 m³/s into a 0.5 m pipe
    const std::string rep = "[REPORT]\nINPUT NO\n";
    const auto at = body.find(rep);
    ASSERT_NE(at, std::string::npos);
    body.insert(at, "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n\n"
                    "[TIMESERIES]\nIN1  0:00  0.5\nIN1  1:00  0.5\n\n");
    DeckRun r = openDeck("manhole", body);
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    const auto& gw = r.eng->surfaceRouter2D().subsurface();
    double elapsed = 0.0, prev_cum = 0.0;
    bool reversed = false;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
        const double cum = gw.bedExchangeCumulative().empty() ? 0.0 : gw.bedExchangeCumulative()[0];
        if (cum > prev_cum + 1.0e-12) reversed = true;   // + would be aquifer → node
        prev_cum = cum;
    }
    const auto& g = gw.state();
    double dunne = 0.0, node_out = 0.0, resid = 0.0, storage = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_DUNNE, &dunne), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_out), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &storage), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    EXPECT_LT(node_out, -1.0e-3) << "the surcharged node recharged nothing" << ledgerDump(r.e);
    // the recharge is exactly what the aquifer stored (nothing left by any
    // other channel: no deep loss, no ET, no Dunne)
    double init = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INIT_STORAGE, &init), SWMM_OK);
    EXPECT_NEAR(storage - init, -node_out, 1.0e-9 * std::fabs(node_out) + 1.0e-9)
        << "stored " << storage - init << " vs recharged " << -node_out;
    EXPECT_FALSE(reversed) << "the exchange flipped from recharge to drain under a surcharged node";
    EXPECT_NEAR(g.hg[0], g.zs[0], 1.0e-3 * g.zs[0]) << "the column under the node did not fill";
    EXPECT_NEAR(g.qnode_last[0], 0.0, 1.0e-12) << "a full column is still taking water";
    EXPECT_NEAR(dunne, 0.0, 1.0e-9) << "recharge into a saturated column came back as Dunne — ping-pong";
    EXPECT_LT(std::fabs(resid), 1.0e-6 * storage + 1.0e-9) << ledgerDump(r.e);
    // The 1D side booked the same water leaving the node (delivered one
    // routing batch behind the sampling): "2D Coupling Outflow" carries it.
    constexpr double kM3ToFt3 = 1.0 / (0.3048 * 0.3048 * 0.3048);
    const double out_1d_m3 = r.eng->context().mass_balance.routing_coupling_out / kM3ToFt3;
    EXPECT_NEAR(out_1d_m3, -node_out, 0.02 * -node_out + 1.0e-6)
        << "1D coupling outflow " << out_1d_m3 << " vs aquifer node ledger " << -node_out;
    finish(r);
}

// ---------------------------------------------------------------------------
// G-X1 gate (c) — a spring on dry ground reaches the network. The dry
// exfiltration deck of G1-c with J1's rim just under cell 2's ground and an
// orifice coupling at cell 2's own vertex: the water the aquifer pushes up
// onto the dry cell drains into J1. Three ledgers close on the same volume:
// aquifer Dunne == surface aquifer_in; surface 2D→1D drain == what the 1D
// routing received; and the surface balance closes with both terms in.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, SpringOnDryGroundReachesTheNetworkThroughTheOrificeCoupling) {
    std::string body = deck("[2D_AQUIFER]\n"
                            "CELL 1  36000.0  2.0  0.45  0.10  2.0  HG0 2.0\n"
                            "CELL 2  36000.0  0.5  0.45  0.10  2.0  HG0 0.49\n\n",
                            "", /*init_depth=*/0.0, /*infil_mm_hr=*/0.0);
    auto sub = [&](const std::string& a, const std::string& b) {
        const auto at = body.find(a); ASSERT_NE(at, std::string::npos) << a; body.replace(at, a.size(), b);
    };
    sub("0.0 0.0 -10.0\n10.0 0.0 -10.0\n", "0.0 0.0 -10.0\n10.0 0.0 -9.0\n");   // cell 1 raised
    sub("[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n", "[JUNCTIONS]\nJ1 -12.0 1.9 0 0 0\n"); // rim at -10.1
    sub("[OUTFALLS]\nO1 -1.0 FREE NO\n",   "[OUTFALLS]\nO1 -13.0 FREE NO\n");
    sub("[REPORT]\nINPUT NO\n", "[2D_VERTEX_NODE_MAP]\n3 J1 0.7 1.0\n\n[REPORT]\nINPUT NO\n");
    DeckRun r = openDeck("spring", body);
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& ctx = r.eng->context();
    const auto& mb  = ctx.mass_balance_2d;
    const auto& st  = r.eng->surfaceRouter2D().state();
    double dunne = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_DUNNE, &dunne), SWMM_OK);
    ASSERT_GT(dunne, 0.0) << ledgerDump(r.e);
    // ledger 1 ↔ 2: aquifer → surface
    EXPECT_NEAR(mb.aquifer_in, dunne, 1.0e-9 * dunne);
    // the spring drained into J1
    EXPECT_GT(mb.coupling_2d_to_1d_out, 0.5 * dunne)
        << "the exfiltrated water did not reach the node: " << mb.coupling_2d_to_1d_out;
    // ledger 2 ↔ 3: what the surface says it drained is what the 1D received
    // (delivered through the batch queue — the tail still queued at the end
    // is bounded by one batch)
    constexpr double kM3ToFt3 = 1.0 / (0.3048 * 0.3048 * 0.3048);
    const double received_1d = ctx.mass_balance.routing_external / kM3ToFt3;
    EXPECT_NEAR(received_1d, mb.coupling_2d_to_1d_out, 0.02 * mb.coupling_2d_to_1d_out + 1.0e-6)
        << "1D external inflow " << received_1d << " vs 2D drain " << mb.coupling_2d_to_1d_out;
    // the surface balance closes with both terms: init 0 + aquifer_in − drain == storage
    double surf = 0.0; for (double v : st.volume) surf += v;
    EXPECT_NEAR(surf, mb.aquifer_in - mb.coupling_2d_to_1d_out, 1.0e-9 * dunne + 1.0e-12);
    EXPECT_LT(std::fabs(mb.error()), 1.0e-6);
    finish(r);
}

// ---------------------------------------------------------------------------
// G-X2 — auto-enrolment. The helper's decks carry no [COORDINATES], so nothing
// enrols and every earlier gate is untouched. Give J1 coordinates inside cell
// 1 (the triangle 0-1-2 of the 10 × 10 m pan, centroid (6.67, 3.33)) and,
// with no [2D_AQUIFER_NODE] row at all, the node gets a located bed, is
// flagged automatic, exchanges (the surcharged-manhole scenario) and is NOT
// written back; NODE_ENROLMENT ROWS restores the row-only behaviour; an
// EXCHANGE NO row keeps the node out and round-trips.
// ---------------------------------------------------------------------------
namespace {
std::string manholeDeck(const std::string& aquifer_options,
                        const std::string& node_rows) {
    std::string body = deck("[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\n" + aquifer_options +
                            "\n[2D_AQUIFER]\n*  36.0  4.0  0.45  0.10  2.0  HG0 1.0\n\n" +
                            (node_rows.empty() ? std::string{}
                                               : "[2D_AQUIFER_NODE]\n" + node_rows + "\n"),
                            "", /*init_depth=*/0.0, /*infil_mm_hr=*/0.0);
    const std::string rep = "[REPORT]\nINPUT NO\n";
    const auto at = body.find(rep);
    body.insert(at, "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n\n"
                    "[TIMESERIES]\nIN1  0:00  0.5\nIN1  1:00  0.5\n\n"
                    "[COORDINATES]\nJ1  6.0  3.0\nO1  40.0  40.0\n\n");
    return body;
}
}  // namespace

TEST(Aquifer2D, NodesInsideTheMeshEnrolByTheirCoordinates) {
    DeckRun r = openDeck("enrol_auto", manholeDeck("", ""));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    int n = -1;
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1) << "J1 sits in cell 1, O1 outside the mesh";
    char name[32] = {0}; int cell = -1, locate = 0, exchange = 0, automatic = 0;
    ASSERT_EQ(swmm_gw2d_node_get(r.e, 0, name, 32, &cell, nullptr, nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_node_get_flags(r.e, 0, &locate, &exchange, &automatic), SWMM_OK);
    EXPECT_STREQ(name, "J1"); EXPECT_EQ(cell, 0);
    EXPECT_EQ(locate, 1); EXPECT_EQ(exchange, 1); EXPECT_EQ(automatic, 1);
    EXPECT_TRUE(warnedAbout(r, "enrolled in the node <-> aquifer exchange"));
    // it exchanges: the surcharged junction recharges the closure-A column
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    double node_out = 0.0, resid = 0.0, storage = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_out), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &storage), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    EXPECT_LT(node_out, -0.5) << "the auto-enrolled node recharged nothing" << ledgerDump(r.e);
    EXPECT_LT(std::fabs(resid), 1.0e-6 * storage + 1.0e-9);
    // …and the writer does not echo the located bed.
    const fs::path saved = kOutDir / "enrol_auto_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    EXPECT_EQ(readAll(saved).find("[2D_AQUIFER_NODE]"), std::string::npos);
    finish(r);
}

TEST(Aquifer2D, NodeEnrolmentRowsKeepsOnlyAuthoredBeds) {
    DeckRun r = openDeck("enrol_rows", manholeDeck("NODE_ENROLMENT ROWS\n", ""));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    int n = -1;
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 0);
    EXPECT_FALSE(warnedAbout(r, "enrolled in the node <-> aquifer exchange"));
    char buf[16] = {0};
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "NODE_ENROLMENT", buf, 16), SWMM_OK);
    EXPECT_STREQ(buf, "ROWS");
    finish(r);
}

TEST(Aquifer2D, ExchangeNoOptsANodeOutAndRoundTrips) {
    DeckRun r = openDeck("enrol_optout", manholeDeck("", "J1  AUTO  EXCHANGE NO\n"));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    int n = -1;
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1) << "the opt-out row is the only bed row";
    int locate = 0, exchange = 1, automatic = 1;
    ASSERT_EQ(swmm_gw2d_node_get_flags(r.e, 0, &locate, &exchange, &automatic), SWMM_OK);
    EXPECT_EQ(locate, 1); EXPECT_EQ(exchange, 0); EXPECT_EQ(automatic, 0);
    EXPECT_FALSE(warnedAbout(r, "enrolled in the node <-> aquifer exchange"));
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    double node_out = 1.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_out), SWMM_OK);
    EXPECT_EQ(node_out, 0.0) << "an opted-out node exchanged";
    const fs::path saved = kOutDir / "enrol_optout_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("[2D_AQUIFER_NODE]"), std::string::npos);
    EXPECT_NE(text.find("AUTO"), std::string::npos);
    EXPECT_NE(text.find("EXCHANGE NO"), std::string::npos);
    finish(r);
}

// ---------------------------------------------------------------------------
// G-X2 — the one-owner rule. A storage unit with Green-Ampt exfiltration
// (Ksat 50 mm/h) inside a cell: enrolled, its exfiltration is silenced and
// the aquifer receives through the conductance channel instead; opted out,
// the legacy exfiltration runs and the aquifer sees nothing.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, StorageExfiltrationIsReplacedByTheBedWhenEnrolled) {
    auto storageDeck = [&](const std::string& node_rows) {
        std::string body = manholeDeck("", node_rows);
        auto sub = [&](const std::string& a, const std::string& b) {
            const auto at = body.find(a); if (at != std::string::npos) body.replace(at, a.size(), b);
        };
        // ST1 replaces J1: a 50 m² pond, 2 m deep, exfiltrating at 50 mm/h
        sub("[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n",
            "[JUNCTIONS]\n\n[STORAGE]\nST1 0.0 2.0 1.0 FUNCTIONAL 0 0 50 0 0 3.0 50.0 0.3\n");
        sub("C1 J1 O1", "C1 ST1 O1");
        sub("J1  FLOW  IN1", "ST1  FLOW  IN1");
        sub("J1  6.0  3.0", "ST1  6.0  3.0");
        return body;
    };
    // storage exfiltration rides `nodes.losses`, booked to routing_evap_loss
    // (Fevap is 0 on the deck, so the row is exfiltration alone)
    double exfil_enrolled = -1.0, node_enrolled = 0.0, exfil_optout = -1.0, node_optout = 0.0;
    {
        DeckRun r = openDeck("storage_bed", storageDeck(""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        EXPECT_TRUE(warnedAbout(r, "exfiltration is replaced by the node <-> aquifer exchange"));
        exfil_enrolled = r.eng->context().mass_balance.routing_evap_loss;
        ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_enrolled), SWMM_OK);
        finish(r);
    }
    {
        DeckRun r = openDeck("storage_optout", storageDeck("ST1  AUTO  EXCHANGE NO\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        EXPECT_FALSE(warnedAbout(r, "exfiltration is replaced"));
        exfil_optout = r.eng->context().mass_balance.routing_evap_loss;
        ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_optout), SWMM_OK);
        finish(r);
    }
    EXPECT_EQ(exfil_enrolled, 0.0) << "the enrolled pond still exfiltrated on its own";
    EXPECT_LT(node_enrolled, -0.5)  << "the enrolled pond did not recharge through the bed";
    EXPECT_GT(exfil_optout, 0.0)    << "the opted-out pond lost its legacy exfiltration";
    EXPECT_EQ(node_optout, 0.0);
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
