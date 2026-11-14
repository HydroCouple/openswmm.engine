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
 * @file      test_2d_aquifer.cpp
 * @brief     G1 (2026-09-07) — the `[2D_AQUIFER*]` authoring surface, its round
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <bit>
#include <cstdint>

#include <cmath>
#include <cstdlib>
#include <algorithm>
#include <numeric>
#include <limits>
#include "2d/subsurface/LidNodeExchange.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_coupling.h>
#include <openswmm/engine/openswmm_forcing.h> // G-X4
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_sq2d.h>

#include "core/SWMMEngine.hpp"
#include "core/ProcessPerf.hpp"
#include "2d/SurfaceRouter2D.hpp" // G1-c item 2 gate reads the state
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

 * @param[in] infil_mm_hr Constant infiltration capacity in mm/hr, copied into the defaults row without range validation.
 * @return Owned 30-minute CMS deck text; no file IO or parser validation.
 */
std::string deck(const std::string& aquifer_sections, const std::string& twod_options_extra = "",
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
         "[2D_OPTIONS]\nMOMENTUM_EQUATION LOCAL_INERTIAL\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
      << twod_options_extra
      << "\n"
         "[2D_VERTICES]\n"
         "0.0 0.0 -10.0\n10.0 0.0 -10.0\n10.0 10.0 -10.0\n0.0 10.0 -10.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 "
      << init_depth
      << "\n"
         "0 2 3 0.03 "
      << init_depth
      << "\n\n"
         "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  "
      << infil_mm_hr << "  -  -  -  -\n\n"
      << aquifer_sections << "[REPORT]\nINPUT NO\n";
    return m.str();
}

/**
 * @brief Build whole-mesh aquifer rows in the SI project authoring units.
 *
 * Copied verbatim without escaping or parser validation; caller supplies valid row separators. No
 * file IO occurs.
 *
 * @param[in] extra_row_keywords Raw keywords appended after the fixed KS=36 mm/hr, ZS=4 m,
 *     porosities and ALPHA=2 1/m row.
 * @param[in] options_lines Raw aquifer option rows; empty omits that section.
 * @param[in] node_lines Raw node-bed rows; empty omits the node section.
 * @return Owned section text.
 */
std::string aquiferSection(const std::string& extra_row_keywords = "",
                           const std::string& options_lines = "",
                           const std::string& node_lines = "") {
    std::ostringstream m;
    if (!options_lines.empty())
        m << "[2D_AQUIFER_OPTIONS]\n" << options_lines << "\n";
    m << "[2D_AQUIFER]\n"
         ";;Scope KS ZS THETA_S THETA_R ALPHA\n"
         "*  36.0  4.0  0.45  0.10  2.0  "
      << extra_row_keywords << "\n\n";
    if (!node_lines.empty())
        m << "[2D_AQUIFER_NODE]\n" << node_lines << "\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    bool opened = false, started = false;
};

/**
 * @brief Write and open an aquifer fixture, retaining its handle for inspection.
 *
 * Creates kOutDir, truncates the input without checking stream success, and does not
 * initialize/start. Caller must finish even a failed-open non-null handle. Filesystem/allocation
 * exceptions may propagate.
 *
 * @param[in] tag Artifact stem appended below kOutDir; matching .inp/.rpt/.out paths may be
 *     overwritten.
 * @param[in] body Borrowed full input deck text written verbatim.
 * @return Record with owned e handle and matching borrowed eng pointer, or null e on allocation
 *     failure; opened reports only open success.
 */
DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    DeckRun r;
    const fs::path inp = kOutDir / (tag + ".inp");
    {
        std::ofstream f(inp);
        f << body;
    }
    r.e = swmm_engine_create();
    if (!r.e)
        return r;
    r.opened =
        swmm_engine_open(r.e, inp.string().c_str(), (kOutDir / (tag + ".rpt")).string().c_str(),
                         (kOutDir / (tag + ".out")).string().c_str(), nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

/**
 * @brief Open an existing aquifer input without rewriting its contents.
 *
 * Does not create kOutDir, initialize or start. Caller must finish every non-null returned handle;
 * report/result paths use tag independently of inp.
 *
 * @param[in] inp Borrowed existing input path.
 * @param[in] tag Artifact stem appended below kOutDir; matching .inp/.rpt/.out paths may be
 *     overwritten.
 * @return Caller-owned handle record; opened reports open status, with null e on allocation
 *     failure.
 */
DeckRun openFile(const fs::path& inp, const std::string& tag) {
    DeckRun r;
    r.e = swmm_engine_create();
    if (!r.e)
        return r;
    r.opened =
        swmm_engine_open(r.e, inp.string().c_str(), (kOutDir / (tag + ".rpt")).string().c_str(),
                         (kOutDir / (tag + ".out")).string().c_str(), nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

/**
 * @brief Initialize and step an opened aquifer record.
 *
 * Requests saved results and loops until elapsed is nonpositive or a step rejects, with no
 * iteration guard. Does not end, close or destroy; finish remains caller responsibility.
 *
 * @param[in,out] r Owned open record; sets started after start succeeds and mutates its engine
 *     until the first stopping step.
 * @return False on unopened/init/start rejection; true after start even if stepping later fails.
 */
bool run(DeckRun& r) {
    if (!r.opened)
        return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK)
        return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK)
        return false;
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
    return true;
}

/**
 * @brief End and release an aquifer record engine.
 *
 * Ignores end/close statuses. Does not reset eng, opened or started, so eng becomes dangling and
 * must not be reused after this call. Null e is a no-op.
 *
 * @param[in,out] r Record whose non-null e is ended when started, closed and destroyed; e is reset
 *     to null.
 */
void finish(DeckRun& r) {
    if (!r.e)
        return;
    if (r.started)
        swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

/**
 * @brief Format successful public groundwater water-ledger terms for failures.
 *
 * Does not mutate model state or surface query failures separately.
 *
 * @param[in] e Borrowed engine handle passed to each public ledger query.
 * @return Owned text with named cubic-metre terms; rejected queries are omitted.
 */
std::string ledgerDump(SWMM_Engine e) {
    static const struct {
        int term;
        const char* name;
    } kTerms[] = {
        {SWMM_GW2D_LED_INFIL_IN, "infil_in"},
        {SWMM_GW2D_LED_LATERAL, "lateral"},
        {SWMM_GW2D_LED_RECHARGE, "recharge (internal)"},
        {SWMM_GW2D_LED_CAPRISE, "cap_rise (internal)"},
        {SWMM_GW2D_LED_DEEP, "deep_out"},
        {SWMM_GW2D_LED_NODE, "node_out"},
        {SWMM_GW2D_LED_ET, "et_out"},
        {SWMM_GW2D_LED_DUNNE, "dunne_out"},
        {SWMM_GW2D_LED_INIT_STORAGE, "init_storage"},
        {SWMM_GW2D_LED_STORAGE, "storage_now"},
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

/**
 * @brief Search an opened aquifer record warning list.
 *
 * @param[in] r Record with non-null eng and readable model context.
 * @param[in] needle Non-null case-sensitive substring; empty matches any existing warning.
 * @return True on a warning containing needle, false otherwise.
 */
bool warnedAbout(const DeckRun& r, const char* needle) { // G-X2
    for (const auto& w : r.eng->context().warnings)
        if (w.find(needle) != std::string::npos)
            return true;
    return false;
}

/**
 * @brief Collect an entire fixture file into owned text.
 *
 * Uses ordinary text mode and its platform newline rules. The local stream closes on return; no
 * model state is touched and no borrowed buffer is returned. Default stream exception settings
 * apply; allocation exceptions may propagate.
 *
 * @param[in] p Borrowed input filename used for this synchronous open/read.
 * @return Owned string collected from the stream; empty can mean an empty file or failure to open.
 *     Partial read failures have no separate status.
 */
std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

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
        << "residual " << resid << " against storage " << storage << "\n"
        << ledgerDump(r.e);

    // It actually received the surface's infiltration, rather than sitting
    // inert beside it.
    double infil_in = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INFIL_IN, &infil_in), SWMM_OK);
    EXPECT_GT(infil_in, 0.0) << "the aquifer received nothing — the surface is still losing its "
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
    const std::string body = deck(aquiferSection("SOIL_CHAR GARDNER CLOSURE SIGMA M_LAYERS 12 "
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
    ASSERT_EQ(swmm_gw2d_row_get(r.e, 0, nullptr, nullptr, 0, nullptr, &ks0, &zs0, &ts0, &tr0, &al0),
              SWMM_OK);
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
    ASSERT_EQ(
        swmm_gw2d_row_get(r2.e, 0, nullptr, nullptr, 0, nullptr, &ks1, &zs1, &ts1, &tr1, &al1),
        SWMM_OK);
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

// An external mesh file carries geometry and the per-cell mesh sections only;
// the reader takes the aquifer, [GW_*] and mesh land-use rows from the .inp
// alone. The writer used to put them in the .2dm, so the next open dropped
// them and a second save lost them for good (the Bellinge groundwater deck).
TEST(Aquifer2D, ModelSectionsSurviveTwoSavesWithAnExternalMesh) {
    const fs::path dir = kOutDir / "ext_mesh";
    fs::create_directories(dir);
    {
        std::ofstream f(dir / "pan.2dm");
        f << "[2D_VERTICES]\n0.0 0.0 -10.0\n10.0 0.0 -10.0\n10.0 10.0 -10.0\n0.0 10.0 -10.0\n\n"
             "[2D_TRIANGLES]\n0 1 2 0.03 0.0 PAD\n0 2 3 0.03 0.0\n";
    }
    {
        std::ofstream f(dir / "model.inp");
        f << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
             "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\n"
             "END_TIME 00:30:00\nREPORT_STEP 00:01:00\nROUTING_STEP 5\n\n"
             "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
             "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
             "[POLLUTANTS]\nP1 MG/L 0 0 0 0 NO * 0 0 0\n\n[LANDUSES]\nPad 0 0 0\n\n"
             "[WASHOFF]\nPad P1 EXP 0.1 1.0 0 0\n\n"
             "[2D_OPTIONS]\nMOMENTUM_EQUATION LOCAL_INERTIAL\nMAX_TIMESTEP 5\nREPORT_2D NO\n\n[2D_MESH_FILE]\nFILE pan.2dm\n\n"
             "[2D_COVERAGES]\nTAG PAD Pad 100\n\n[2D_LOADINGS]\nTAG PAD P1 50\n\n"
             "[2D_AQUIFER]\n*  36.0  4.0  0.45  0.10  2.0  HG0 1.0\n\n"
             "[2D_AQUIFER_NODE]\nJ1  AUTO  EXCHANGE NO\n\n"
             "[GW_SOURCES]\nW1  CELL 2  FLOW 0.0005  P1 CONC 100\n";
    }
    const auto counts = [](SWMM_Engine e) {
        int rows = -1, beds = -1, cov = -1, load = -1;
        swmm_gw2d_row_count(e, &rows);
        swmm_gw2d_node_count(e, &beds);
        swmm_2d_coverage_count(e, &cov);
        swmm_2d_loading_count(e, &load);
        return std::vector<int>{rows, beds, swmm_gw_source_count(e), cov, load};
    };
    const std::vector<int> want{1, 1, 1, 1, 1}; // aquifer, bed, source, coverage, loading

    fs::path inp = dir / "model.inp";
    for (const char* pass : {"saved1", "saved2"}) {
        SCOPED_TRACE(pass);
        DeckRun r = openFile(inp, std::string("ext_") + pass);
        ASSERT_TRUE(r.opened);
        EXPECT_EQ(counts(r.e), want) << "rows lost on the way into " << pass;
        fs::create_directories(dir / pass);
        inp = dir / pass / "model.inp";
        ASSERT_EQ(swmm_model_write(r.e, inp.string().c_str()), SWMM_OK);
        finish(r);
        const std::string text = readAll(inp), mesh = readAll(dir / pass / "pan.2dm");
        for (const char* sec : {"[2D_AQUIFER]", "[2D_AQUIFER_NODE]", "[GW_SOURCES]",
                                "[2D_COVERAGES]", "[2D_LOADINGS]"}) {
            EXPECT_NE(text.find(sec), std::string::npos) << sec << " missing from the .inp";
            EXPECT_EQ(mesh.find(sec), std::string::npos) << sec << " written to the .2dm";
        }
        EXPECT_NE(mesh.find("PAD"), std::string::npos) << "the cell tag stays with the mesh";
    }
    DeckRun r = openFile(inp, "ext_reopened");
    ASSERT_TRUE(r.opened);
    EXPECT_EQ(counts(r.e), want) << "rows lost after two saves";
    finish(r);
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

    ASSERT_EQ(
        swmm_gw2d_row_add(r.e, SWMM_GW2D_SCOPE_GLOBAL, nullptr, -1, 36.0, 4.0, 0.45, 0.10, 2.0),
        SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_add(r.e, SWMM_GW2D_SCOPE_CELL, nullptr, 1, 18.0, 3.0, 0.40, 0.08, 1.5),
              SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 2);

    int scope = -1, cell = -99;
    double ks = 0.0, zs = 0.0, ts = 0.0, tr = 0.0, al = 0.0;
    ASSERT_EQ(swmm_gw2d_row_get(r.e, 1, &scope, nullptr, 0, &cell, &ks, &zs, &ts, &tr, &al),
              SWMM_OK);
    EXPECT_EQ(scope, SWMM_GW2D_SCOPE_CELL);
    EXPECT_EQ(cell, 1);
    EXPECT_DOUBLE_EQ(ks, 18.0);
    EXPECT_DOUBLE_EQ(al, 1.5);

    // Optional properties, including the two that are enum codes carried as
    // doubles — the shape the dialog's combo boxes write.
    ASSERT_EQ(swmm_gw2d_row_set_property(r.e, 1, "SOIL_CHAR", SWMM_GW2D_SOIL_VAN_GENUCHTEN),
              SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_set_property(r.e, 1, "CLOSURE", SWMM_GW2D_CLOSURE_SIGMA), SWMM_OK);
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
    ASSERT_EQ(swmm_gw2d_node_get(r.e, 0, node, sizeof node, &ncell, &kc, &dc, &area), SWMM_OK);
    EXPECT_STREQ(node, "J1");
    EXPECT_EQ(ncell, 0);
    EXPECT_DOUBLE_EQ(kc, 0.0) << "0 is 'direct Darcy', not 'unset'";

    // Removal, from both tables.
    ASSERT_EQ(swmm_gw2d_row_remove(r.e, 0), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_row_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1);
    ASSERT_EQ(swmm_gw2d_row_get(r.e, 0, &scope, nullptr, 0, &cell, &ks, nullptr, nullptr, nullptr,
                                nullptr),
              SWMM_OK);
    EXPECT_EQ(cell, 1) << "removing row 0 must leave the OTHER row behind";
    ASSERT_EQ(swmm_gw2d_node_remove(r.e, 0), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &nb), SWMM_OK);
    EXPECT_EQ(nb, 0);

    // Out-of-range indices must be refused, not silently clamped: the dialog
    // deletes by row and a clamp would delete the wrong one.
    EXPECT_NE(swmm_gw2d_row_get(r.e, 99, &scope, nullptr, 0, &cell, &ks, &zs, &ts, &tr, &al),
              SWMM_OK);
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
TEST(Aquifer2D, Aquifer2DDestinationRequiresOwnershipMigration) {
    DeckRun r = openDeck("dest_obsolete", deck(aquiferSection(), "INFIL_DESTINATION AQUIFER_2D\n"));
    ASSERT_TRUE(r.opened);
    EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
}

// ---------------------------------------------------------------------------
// A5 — one owner for the infiltrated water
// ---------------------------------------------------------------------------
TEST(Aquifer2D, SurfaceDefaultForLegacyAquiferIsSkippedOnOwnedCells) {
    DeckRun r = openDeck("owned_skip_legacy",
                         deck(aquiferSection(), "INFIL_DESTINATION SUBCATCH_AQUIFER\n"));
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    double accepted = 0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INFIL_IN, &accepted), SWMM_OK);
    EXPECT_GT(accepted, 0);
    finish(r);
}

// ---------------------------------------------------------------------------
// A6 — the surface ledger counts what the aquifer hands back
// ---------------------------------------------------------------------------
TEST(Aquifer2D, ReturnedSaturationExcessIsAnInflowToTheSurfaceLedger) {
    // Receiving limits stop surface infiltration before it needs refunding.
    // A separate link receipt exercises physical saturation excess and the
    // upward surface-ledger transfer, including its pending delivery.
    DeckRun r = openDeck(
        "dunne", deck("[2D_AQUIFER]\n* 36.0 0.5 0.45 0.10 2.0 HG0 0.45\n\n", "", 1.0, 200.0));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    r.eng->surfaceRouter2D().subsurface().bookLinkSeepage(0, 10.0);
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }

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
        << "2D continuity error " << mb.error() << " with aquifer_in = " << mb.aquifer_in;
    // G1-c (2026-09-19): the AQUIFER's own residual closes too. This deck's
    // thin unsaturated zone resolves to the ENSLAVED closure, whose unbounded
    // Newton used to zero the table and lose 10 m³ of the 10 m³ delivered
    // (−32 % here) — the Dunne it returned was 0.001 m³ of the ~8 m³ owed.
    double resid = 0.0, storage = 0.0;
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &storage), SWMM_OK);
    EXPECT_LT(std::fabs(resid), 1.0e-6 * storage + 1.0e-9) << "aquifer residual " << resid << "\n"
                                                           << ledgerDump(r.e);
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
    const auto& mb = ctx.mass_balance_2d;
    double dunne = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_DUNNE, &dunne), SWMM_OK);
    ASSERT_GT(dunne, 0.0) << "cell 2's table never reached the ground; the gate proves nothing"
                          << ledgerDump(r.e);
    // The surface took everything the aquifer handed over — nothing parked.
    const auto& st = r.eng->surfaceRouter2D().state();
    const auto& gws = r.eng->surfaceRouter2D().subsurface().state();
    double parked = 0.0;
    for (const double v : gws.xacc_to_surface)
        parked += v;
    EXPECT_NEAR(parked, 0.0, 1.0e-9 * dunne)
        << "exfiltrated water is still waiting for a surface cell to fire";
    EXPECT_NEAR(mb.aquifer_in, dunne, 1.0e-9 * dunne);
    // …and it is on cell 2 (the dry one), not lost: the surface storage grew
    // by the Dunne volume (no rain, no evaporation, no other source).
    const double surf = st.volume[0] + st.volume[1];
    EXPECT_GT(st.volume[1], 0.0) << "the dry cell above the saturated column stayed dry";
    EXPECT_NEAR(surf + mb.infil_out, dunne, 1.0e-6 * dunne + 1.0e-12)
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
    // Isolate the bed receiver from lateral inflow/exfiltration in the
    // neighboring cell, which has its own physical saturation boundary.
    const std::string neighbor = "0 2 3 0.03 0\n";
    const auto neighbor_at = body.find(neighbor);
    ASSERT_NE(neighbor_at, std::string::npos);
    body.erase(neighbor_at, neighbor.size());
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
        if (cum > prev_cum + 1.0e-12)
            reversed = true; // + would be aquifer → node
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
    EXPECT_NEAR(gw.infiltrationHeadroom(0), 0.0, 1.0e-9)
        << "the column under the node did not fill";
    EXPECT_NEAR(g.qnode_last[0], 0.0, 1.0e-12) << "a full column is still taking water";
    EXPECT_NEAR(dunne, 0.0, 1.0e-9)
        << "recharge into a saturated column came back as Dunne — ping-pong";
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
        const auto at = body.find(a);
        ASSERT_NE(at, std::string::npos) << a;
        body.replace(at, a.size(), b);
    };
    sub("0.0 0.0 -10.0\n10.0 0.0 -10.0\n", "0.0 0.0 -10.0\n10.0 0.0 -9.0\n");    // cell 1 raised
    sub("[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n", "[JUNCTIONS]\nJ1 -12.0 1.9 0 0 0\n"); // rim at -10.1
    sub("[OUTFALLS]\nO1 -1.0 FREE NO\n", "[OUTFALLS]\nO1 -13.0 FREE NO\n");
    sub("[REPORT]\nINPUT NO\n", "[2D_VERTEX_NODE_MAP]\n3 J1 0.7 1.0\n\n[REPORT]\nINPUT NO\n");
    DeckRun r = openDeck("spring", body);
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& ctx = r.eng->context();
    const auto& mb = ctx.mass_balance_2d;
    const auto& st = r.eng->surfaceRouter2D().state();
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
    double surf = 0.0;
    for (double v : st.volume)
        surf += v;
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
/**
 * @brief Build a dry-pan fixture with sustained manhole inflow.
 *
 * Copied verbatim without escaping or parser validation; caller supplies valid row separators. No
 * file IO occurs.
 *
 * @param[in] aquifer_options Raw aquifer option rows after CLOSED_FORM.
 * @param[in] node_rows Raw node enrollment rows; empty omits the node section.
 * @return Owned CMS deck with 0.5 m3/s J1 inflow, HG0=1 m and no surface infiltration.
 */
std::string manholeDeck(const std::string& aquifer_options, const std::string& node_rows) {
    std::string body =
        deck("[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\n" + aquifer_options +
                 "\n[2D_AQUIFER]\n*  36.0  4.0  0.45  0.10  2.0  HG0 1.0\n\n" +
                 (node_rows.empty() ? std::string{} : "[2D_AQUIFER_NODE]\n" + node_rows + "\n"),
             "", /*init_depth=*/0.0, /*infil_mm_hr=*/0.0);
    const std::string rep = "[REPORT]\nINPUT NO\n";
    const auto at = body.find(rep);
    body.insert(at, "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n\n"
                    "[TIMESERIES]\nIN1  0:00  0.5\nIN1  1:00  0.5\n\n"
                    "[COORDINATES]\nJ1  6.0  3.0\nO1  40.0  40.0\n\n");
    return body;
}
} // namespace

TEST(Aquifer2D, NodesInsideTheMeshEnrolByTheirCoordinates) {
    DeckRun r = openDeck("enrol_auto", manholeDeck("", ""));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    int n = -1;
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1) << "J1 sits in cell 1, O1 outside the mesh";
    char name[32] = {0};
    int cell = -1, locate = 0, exchange = 0, automatic = 0;
    ASSERT_EQ(swmm_gw2d_node_get(r.e, 0, name, 32, &cell, nullptr, nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_node_get_flags(r.e, 0, &locate, &exchange, &automatic), SWMM_OK);
    EXPECT_STREQ(name, "J1");
    EXPECT_EQ(cell, 0);
    EXPECT_EQ(locate, 1);
    EXPECT_EQ(exchange, 1);
    EXPECT_EQ(automatic, 1);
    EXPECT_TRUE(warnedAbout(r, "enrolled in the node <-> aquifer exchange"));
    // it exchanges: the surcharged junction recharges the closure-A column
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
    double node_out = 0.0, resid = 0.0, storage = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_out), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &storage), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    EXPECT_LT(node_out, 0.0) << "the auto-enrolled node recharged nothing" << ledgerDump(r.e);
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
    EXPECT_EQ(locate, 1);
    EXPECT_EQ(exchange, 0);
    EXPECT_EQ(automatic, 0);
    EXPECT_FALSE(warnedAbout(r, "enrolled in the node <-> aquifer exchange"));
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
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

// An outfall that discharges onto the mesh is a surface boundary, not a
// manhole: the outfall exchange books the node's net flow as water to or from
// the surface, so an aquifer draw there read as a withdrawal from the dry cell
// and stopped the run ("1D outfall exceeded its reserved surface water
// budget", the Bellinge deck at step 4). The draw is capped by the node's
// inflow and delivered a step later, so a falling trickle (dry weather flow)
// is what overdraws it. Auto-enrolment leaves outfalls out.
TEST(Aquifer2D, OutfallsInsideTheMeshAreNotEnrolled) {
    std::string body = manholeDeck("", "");
    const std::string far = "O1  40.0  40.0\n";
    body.replace(body.find(far), far.size(), "O1  1.0  8.0\n");
    const std::string steady = "IN1  0:00  0.5\nIN1  1:00  0.5\n";
    body.replace(body.find(steady), steady.size(), "IN1  0:00  0.002\nIN1  0:10  0.0\n");
    body.insert(body.find("[REPORT]\nINPUT NO\n"), "[2D_VERTEX_NODE_MAP]\n3 O1 0.65 1\n\n");
    DeckRun r = openDeck("enrol_outfall", body);
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    int n = -1;
    ASSERT_EQ(swmm_gw2d_node_count(r.e, &n), SWMM_OK);
    EXPECT_EQ(n, 1) << "only J1 enrols; O1 sits in cell 2";
    char name[32] = {0};
    ASSERT_EQ(swmm_gw2d_node_get(r.e, 0, name, 32, nullptr, nullptr, nullptr, nullptr), SWMM_OK);
    EXPECT_STREQ(name, "J1");
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    double elapsed = 0.0;
    int rc = SWMM_OK;
    while ((rc = swmm_engine_step(r.e, &elapsed)) == SWMM_OK && elapsed > 0.0) {
    }
    EXPECT_EQ(rc, SWMM_OK) << "the run stopped at " << elapsed << " days";
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
            const auto at = body.find(a);
            if (at != std::string::npos)
                body.replace(at, a.size(), b);
        };
        // ST1 replaces J1: a 50 m² pond, 2 m deep, exfiltrating at 50 mm/h
        sub("[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n",
            "[JUNCTIONS]\n\n[STORAGE]\nST1 0.0 2.0 1.0 FUNCTIONAL 0 0 50 0 0 3.0 50.0 0.3\n");
        sub("C1 J1 O1", "C1 ST1 O1");
        sub("J1  FLOW  IN1", "ST1  FLOW  IN1");
        sub("J1  6.0  3.0", "ST1  6.0  3.0");
        return body;
    };
    // Fevap is zero: storage exfiltration must enter the seepage ledger,
    // with no evaporation booked for either enrollment setting.
    double exfil_enrolled = -1.0, node_enrolled = 0.0, exfil_optout = -1.0, node_optout = 0.0;
    {
        DeckRun r = openDeck("storage_bed", storageDeck(""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        EXPECT_TRUE(warnedAbout(r, "exfiltration is replaced by the node <-> aquifer exchange"));
        exfil_enrolled = r.eng->context().mass_balance.routing_seep_loss;
        EXPECT_DOUBLE_EQ(r.eng->context().mass_balance.routing_evap_loss, 0.0);
        ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_enrolled), SWMM_OK);
        finish(r);
    }
    {
        DeckRun r = openDeck("storage_optout", storageDeck("ST1  AUTO  EXCHANGE NO\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        EXPECT_FALSE(warnedAbout(r, "exfiltration is replaced"));
        exfil_optout = r.eng->context().mass_balance.routing_seep_loss;
        EXPECT_DOUBLE_EQ(r.eng->context().mass_balance.routing_evap_loss, 0.0);
        ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_NODE, &node_optout), SWMM_OK);
        finish(r);
    }
    EXPECT_EQ(exfil_enrolled, 0.0) << "the enrolled pond still exfiltrated on its own";
    EXPECT_LT(node_enrolled, 0.0) << "the enrolled pond did not recharge through the bed";
    EXPECT_GT(exfil_optout, 0.0) << "the opted-out pond lost its legacy exfiltration";
    EXPECT_EQ(node_optout, 0.0);
}

// ---------------------------------------------------------------------------
// A2b — the writer's minimal-deck rule: an option left at its default is not
// written back. This is what makes an untouched load-and-save a no-op, and it
// is why A2 has to author values that differ.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, DefaultOptionsAreNotWrittenBack) {
    DeckRun r = openDeck("rt_defaults", deck(aquiferSection("", "M_LAYERS 8\nDUNNE YES\n")));
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

// ---------------------------------------------------------------------------
// G-X3 gates (2026-09-19) — conduit seepage reaches the aquifer cells the
// conduit crosses. The two-cell pan is split by its diagonal (cell 0 lower-
// right, cell 1 upper-left); C1 runs along y = 5 between the coordinates
// the test gives J1 and O1, seeping at 100 mm/hr under a 0.5 m³/s inflow
// that keeps it full. J1 is kept OUT of the node exchange (NODE_ENROLMENT
// ROWS) so the seepage is the aquifer's only inflow.
// ---------------------------------------------------------------------------
namespace {
/**
 * @brief Build an otherwise dry fixture isolating conduit seepage.
 *
 * Copied verbatim without escaping or parser validation; caller supplies valid row separators. No
 * file IO occurs.
 *
 * @param[in] aquifer_options Raw aquifer options after CLOSED_FORM and NODE_ENROLMENT ROWS.
 * @param[in] x1 J1 x-coordinate in metres, at y=5 m.
 * @param[in] x2 O1 x-coordinate in metres, at y=5 m.
 * @return Owned CMS deck with 100 mm/hr conduit seepage and 0.5 m3/s J1 inflow.
 */
std::string seepDeck(const std::string& aquifer_options, double x1, double x2) {
    std::string body =
        deck("[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n" + aquifer_options +
                 "\n[2D_AQUIFER]\n*  36.0  4.0  0.45  0.10  2.0  HG0 1.0\n\n",
             "", /*init_depth=*/0.0, /*infil_mm_hr=*/0.0);
    const std::string rep = "[REPORT]\nINPUT NO\n";
    const auto at = body.find(rep);
    std::ostringstream extra;
    extra << "[LOSSES]\n;;Link Kentry Kexit Kavg Flap Seepage(mm/hr)\nC1  0  0  0  NO  100\n\n"
             "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n\n"
             "[TIMESERIES]\nIN1  0:00  0.5\nIN1  1:00  0.5\n\n"
             "[COORDINATES]\nJ1  "
          << x1 << "  5.0\nO1  " << x2 << "  5.0\n\n";
    body.insert(at, extra.str());
    return body;
}

struct SeepResult {
    double seep_1d = 0.0; // routing_seep_loss, converted to m³
    double led_link = 0.0, pending = 0.0, storage = 0.0, init = 0.0, dunne = 0.0, resid = 0.0;
    double hg0 = 0.0, hg1 = 0.0, qlink0 = 0.0, qlink1 = 0.0;
    bool warned = false;
};

/**
 * @brief Run a conduit-seepage scenario and copy water transfer diagnostics.
 *
 * Records nonfatal test failures for open/run/public ledger rejection, then samples state and
 * finishes the handle. Open failure returns a zero-initialized result without finishing the
 * allocated failed-open handle. A later run failure does not stop inspection; intended fixture
 * geometry/order is required.
 *
 * @param[in] tag Artifact stem appended below kOutDir; matching .inp/.rpt/.out paths may be
 *     overwritten.
 * @param[in] body Borrowed full input deck text written verbatim.
 * @return Owned result containing cubic-metre ledgers/pending volumes, metre heads, native last
 *     exchange rates and a warning flag.
 */
SeepResult runSeep(const std::string& tag, const std::string& body) {
    SeepResult out;
    DeckRun r = openDeck(tag, body);
    EXPECT_TRUE(r.opened);
    if (!r.opened)
        return out;
    EXPECT_TRUE(run(r));
    out.warned = warnedAbout(r, "seepage is delivered into the aquifer cells");
    const auto& g = r.eng->surfaceRouter2D().subsurface().state();
    // the 1D books in its internal volume unit (ft³); the router hands the
    // aquifer SI through the same factor the node exchange uses
    out.seep_1d = r.eng->context().mass_balance.routing_seep_loss *
                  r.eng->surfaceRouter2D().options().vol_1d_to_2d;
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_LINK, &out.led_link), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &out.storage), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INIT_STORAGE, &out.init), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_DUNNE, &out.dunne), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_continuity_error(r.e, &out.resid), SWMM_OK);
    for (double v : g.lacc)
        out.pending += v;
    if (g.n_cells >= 2) {
        out.hg0 = g.hg[0];
        out.hg1 = g.hg[1];
        out.qlink0 = g.qlink_last[0];
        out.qlink1 = g.qlink_last[1];
    }
    finish(r);
    return out;
}
} // namespace

TEST(Aquifer2D, ConduitSeepageReachesTheAquiferCellsItCrosses) {
    // (d) x = 1 → 9 at y = 5: 4 m in the upper-left cell, 4 m in the
    // lower-right one — every metre of C1 is over the mesh.
    const SeepResult a = runSeep("seep_auto", seepDeck("LINK_SEEPAGE ONE_WAY", 1.0, 9.0));
    EXPECT_TRUE(a.warned) << "the enrolment was not announced";
    EXPECT_GT(a.seep_1d, 1.0e-3) << "the conduit did not seep";
    EXPECT_GT(a.led_link, 0.0);
    // everything the 1D lost is in the aquifer's books: delivered, or booked
    // and not yet gathered (one firing in flight)
    EXPECT_NEAR(a.led_link + a.pending, a.seep_1d, 1.0e-9 * a.seep_1d)
        << "1D seepage " << a.seep_1d << " vs aquifer " << a.led_link << " + " << a.pending;
    // …and it is stored, not lost or returned (no deep loss, no ET, no
    // node, room under the ground)
    EXPECT_EQ(a.dunne, 0.0);
    EXPECT_NEAR(a.storage - a.init, a.led_link + a.pending, 1.0e-9 * a.seep_1d + 1.0e-12)
        << "stored " << a.storage - a.init;
    EXPECT_LT(std::fabs(a.resid), 1.0e-9 * a.storage + 1.0e-12) << "residual " << a.resid;
    // the split follows the length: half each, so the two equal columns
    // rose together (no lateral gradient between them)
    EXPECT_GT(a.hg0, 1.0);
    EXPECT_NEAR(a.hg0, a.hg1, 1.0e-12) << "unequal shares of an evenly split conduit";
    EXPECT_GT(a.qlink0, 0.0);
    EXPECT_NEAR(a.qlink0, a.qlink1, 1.0e-12 * a.qlink0);

    // LINK_SEEPAGE NONE: the aquifer receives nothing, and the 1D is
    // bit-identical — the routing never knew (one-way, pure delivery).
    const SeepResult n = runSeep("seep_none", seepDeck("LINK_SEEPAGE NONE", 1.0, 9.0));
    EXPECT_FALSE(n.warned);
    EXPECT_EQ(n.led_link, 0.0);
    EXPECT_EQ(n.pending, 0.0);
    EXPECT_EQ(n.seep_1d, a.seep_1d) << "LINK_SEEPAGE changed the 1D seepage";
    EXPECT_NEAR(n.storage, n.init, 1.0e-12);
}

TEST(Aquifer2D, ConduitSeepageIsSharedByTheLengthOverTheMesh) {
    // (e) x = 5 → 15: the first 5 m lie in the lower-right cell, the rest
    // is off the mesh — half the seepage arrives, all of it in cell 0.
    const SeepResult h = runSeep("seep_half", seepDeck("LINK_SEEPAGE ONE_WAY", 5.0, 15.0));
    EXPECT_TRUE(h.warned);
    EXPECT_GT(h.seep_1d, 1.0e-3);
    EXPECT_NEAR(h.led_link + h.pending, 0.5 * h.seep_1d, 1.0e-9 * h.seep_1d)
        << "1D seepage " << h.seep_1d << " vs aquifer " << h.led_link << " + " << h.pending;
    EXPECT_GT(h.qlink0, 0.0) << "cell 0 is under the conduit";
    EXPECT_EQ(h.qlink1, 0.0) << "cell 1 is not";
    EXPECT_GT(h.hg0, h.hg1) << "the table under the conduit did not rise first";
    EXPECT_LT(std::fabs(h.resid), 1.0e-9 * h.storage + 1.0e-12) << "residual " << h.resid;

    // entirely off the mesh: nothing enrols, nothing arrives, the 1D still
    // seeps (to nowhere, as before G-X3)
    const SeepResult o = runSeep("seep_outside", seepDeck("LINK_SEEPAGE ONE_WAY", 20.0, 30.0));
    EXPECT_FALSE(o.warned);
    EXPECT_GT(o.seep_1d, 1.0e-3);
    EXPECT_EQ(o.led_link, 0.0);
    EXPECT_EQ(o.pending, 0.0);
}

TEST(Aquifer2D, IndependentLateralLedgersCloseWithOneEndpointStillPending) {
    for (const char* profile : {"CLOSED_FORM", "SIGMA", "CLOSED_FORM_HEAT", "SIGMA_HEAT"}) {
        const bool thermal = std::string(profile).ends_with("_HEAT");
        const char* closure =
            std::string(profile).starts_with("CLOSED_FORM") ? "CLOSED_FORM" : "SIGMA";
        for (const char* transport : {"BULK", "LAYERED"}) {
            if (std::string(closure) == "CLOSED_FORM" && std::string(transport) == "LAYERED")
                continue; // This combination is refused by the transport contract.
            SCOPED_TRACE(std::string(profile) + "/" + transport);
            std::string aquifer =
                "[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\nNODE_ENROLMENT ROWS\n"
                "M_LAYERS 4\nCLOSURE " +
                std::string(closure) +
                "\n[2D_AQUIFER]\nCELL 1 36 4 .45 .1 2 HG0 1\n"
                "CELL 2 36 4 .45 .1 2 HG0 1.2\n"
                "[POLLUTANTS]\nDye MG/L 0 0 0 0\n"
                "[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT " +
                std::string(transport) + "\n[GW_INITIAL_QUALITY]\n* SAT Dye 2\n* UNSAT Dye 2\n";
            if (thermal)
                aquifer +=
                    "[GW_TRANSPORT_OPTIONS]\nTHERMAL_MODEL POROUS_EQUILIBRIUM\n"
                    "CONDUCTION NO\n[GW_TRANSPORT_PARAMS]\n* 2650 880 2 0 .5 0 0 0 .065\n"
                    "[GW_INITIAL_QUALITY]\n* SAT __TEMPERATURE__ 10\n* UNSAT __TEMPERATURE__ 10\n";
            auto body = deck(aquifer, "", 0.0);
            if (thermal)
                body.insert(body.find("[OPTIONS]\n") + std::string("[OPTIONS]\n").size(),
                            "HEAT_TRANSPORT YES\n");
            auto r =
                openDeck("clock-pending-ledger-" + std::string(profile) + "-" + transport, body);
            ASSERT_TRUE(r.opened);
            ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
            ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
            r.started = true;
            auto& surface = r.eng->surfaceRouter2D();
            auto& gw = surface.subsurface();
            auto& state = gw.state();
            gw.prepareClock();
            gw.advanceClock(.125, surface.state(), false);
            ASSERT_EQ(gw.cellCompletedTime(0), 0.0);
            ASSERT_EQ(gw.cellCompletedTime(1), 0.0);
            const int edited[] = {0};
            gw.landClockCells(edited, surface.state());
            EXPECT_EQ(gw.cellCompletedTime(0), .125);
            EXPECT_EQ(gw.cellCompletedTime(1), 0.0);
            const auto sum = [](const auto& a) { return std::accumulate(a.begin(), a.end(), 0.); };
            ASSERT_GT(std::abs(sum(state.eacc_L) + sum(state.eacc_R)), 1e-8);
            EXPECT_NEAR(state.led_lateral, 0., 1e-12);
            EXPECT_NEAR(state.continuityResidual(), 0., 1e-12);
            ASSERT_EQ(gw.transport().n_species, thermal ? 2 : 1);
            EXPECT_NEAR(gw.transport().net_lateral[0], 0., 1e-12);
            EXPECT_NEAR(gw.transport().residual(0), 0., 1e-12);
            const auto snapshot = gw.transportRestartState();
            auto old = snapshot;
            const std::string profile = "OpenSWMM independent groundwater v6";
            const auto position = old.find(profile);
            ASSERT_NE(position, std::string::npos);
            old[position + profile.size() - 1] = '5';
            EXPECT_FALSE(gw.restoreTransportRestart(old, false).empty());
            EXPECT_FALSE(gw.restoreTransportRestart(old, true).empty());
            EXPECT_EQ(gw.transportRestartState(), snapshot);
            ASSERT_TRUE(gw.restoreTransportRestart(snapshot, true).empty());
            EXPECT_EQ(gw.transportRestartState(), snapshot);
            // Read-only accounting must neither gather the neighbour nor
            // consume its pending receipt, and must also close after gather.
            gw.landClockCells(std::array{1}, surface.state());
            EXPECT_NEAR(state.continuityResidual(), 0., 1e-12);
            EXPECT_NEAR(gw.transport().residual(0), 0., 1e-12);
            finish(r);
        }
    }
}

TEST(Aquifer2D, IndependentBulkColumnReturnsOnlyWaterBeyondItsFullStorage) {
    constexpr double end = 1.0 / 256.0;
    for (double inflow : {0.0, 0.001}) {
        auto r = openDeck(
            inflow == 0 ? "clock-internal-recharge" : "clock-real-excess",
            deck("[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\nNODE_ENROLMENT ROWS\nGW_ET NONE\n"
                 "[2D_AQUIFER]\nCELL 1 360 1 .45 .1 2 HG0 .99925 "
                 "SOIL_CHAR GARDNER CLOSURE CLOSED_FORM\n"
                 "[POLLUTANTS]\nDye MG/L 0 0 0 0\n"
                 "[GW_TRANSPORT_OPTIONS]\nTRANSPORT_POLLUTANTS YES\n",
                 "", 0.0));
        ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
        r.started = true;
        auto& surface = r.eng->surfaceRouter2D();
        auto& gw = surface.subsurface();
        auto& cell = gw.state();
        // A full thin column cannot recharge downward. Only the external
        // receipt can add water to the surface or collapse this column.
        cell.hu[0] = cell.theta_s[0] * (cell.zs[0] - cell.hg[0]);
        const double capacity = cell.area[0] * cell.theta_s[0] * cell.zs[0];
        auto& tracer = gw.transport();
        ASSERT_EQ(tracer.n_species, 1);
        constexpr double concentration = 2.0;
        tracer.sat_mass[0] = concentration * gw.satVolume(0);
        tracer.unsat_mass[0] = concentration * gw.unsatVolume(0);
        const double initial_unsat_mass = tracer.unsat_mass[0];
        gw.prepareClock();
        gw.refreshClockAfterEdit(surface.state());
        gw.setProducerEnd(end);
        gw.bookLinkSeepage(0, inflow);
        gw.bookLinkSeepageMass(0, 0, concentration * inflow);
        gw.advanceClock(end, surface.state(), false, true);
        EXPECT_DOUBLE_EQ(cell.led_recharge, 0.);
        EXPECT_NEAR(cell.dunne_cumulative[0], inflow, 1e-12);
        EXPECT_NEAR(cell.xacc_to_surface[0], inflow, 1e-12);
        EXPECT_NEAR(gw.satVolume(0) + gw.unsatVolume(0), capacity, 1e-12);
        EXPECT_NEAR(tracer.xacc_to_surface[0], concentration * inflow, 1e-12);
        EXPECT_NEAR(tracer.unsat_mass[0], inflow > 0 ? 0. : initial_unsat_mass, 1e-12);
        EXPECT_NEAR(tracer.sat_mass[0] + tracer.unsat_mass[0] + tracer.xacc_to_surface[0],
                    concentration * (capacity + inflow), 1e-12);
        finish(r);
    }
}

TEST(Aquifer2D, IndependentPartlyFilledColumnSettlesRechargeBeforeReleasingRealExcess) {
    constexpr double end = 1. / 256, concentration = 2.;
    for (bool fill : {false, true}) {
        auto r =
            openDeck(fill ? "clock-partial-overflow" : "clock-partial-recharge",
                     deck("[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\nGW_ET NONE\n"
                          "NODE_ENROLMENT ROWS\n[2D_AQUIFER]\n"
                          "CELL 1 360 1 .45 .1 2 HG0 .9985 SOIL_CHAR GARDNER CLOSURE CLOSED_FORM\n"
                          "[POLLUTANTS]\nDye MG/L 0 0 0 0\n",
                          "", 0.));
        ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
        r.started = true;
        auto& surface = r.eng->surfaceRouter2D();
        auto& gw = surface.subsurface();
        auto& state = gw.state();
        state.hu[0] = .4 * (state.zs[0] - state.hg[0]);
        auto& tr = gw.transport();
        tr.sat_mass[0] = concentration * gw.satVolume(0);
        tr.unsat_mass[0] = concentration * gw.unsatVolume(0);
        const double water = gw.satVolume(0) + gw.unsatVolume(0);
        const double capacity = state.theta_s[0] * state.zs[0] * state.area[0];
        const double inflow = fill ? capacity - water + .001 : 0.;
        gw.prepareClock();
        gw.setProducerEnd(end);
        gw.bookLinkSeepage(0, inflow);
        gw.bookLinkSeepageMass(0, 0, concentration * inflow);
        gw.advanceClock(end, surface.state(), false, true);
        EXPECT_GT(state.led_recharge, 0.);
        EXPECT_NEAR(state.dunne_cumulative[0], fill ? .001 : 0., 1e-12);
        EXPECT_DOUBLE_EQ(state.reject_cumulative[0], 0.);
        EXPECT_NEAR(gw.satVolume(0) + gw.unsatVolume(0) + state.xacc_to_surface[0], water + inflow,
                    1e-12);
        EXPECT_NEAR(tr.sat_mass[0] + tr.unsat_mass[0] + tr.xacc_to_surface[0],
                    concentration * (water + inflow), 1e-12);
        if (fill) {
            EXPECT_DOUBLE_EQ(state.hu[0], 0.);
            EXPECT_NEAR(tr.xacc_to_surface[0], concentration * .001, 1e-12);
        }
        finish(r);
    }
}

TEST(Aquifer2D, IndependentCollapsedColumnKeepsTopRefundsWithTheirDonor) {
    using namespace openswmm::twoD;
    constexpr double end = 1. / 256, intake = .00002, inflow = .001;
    auto r = openDeck("clock-collapse-donor-refund",
                      deck("[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\nGW_ET NONE\n"
                           "NODE_ENROLMENT ROWS\n[2D_AQUIFER]\n"
                           "CELL 1 360 1 .45 .1 2 HG0 .99925 CLOSURE CLOSED_FORM\n"
                           "[POLLUTANTS]\nDye MG/L 0 0 0 0\n",
                           "", 0.));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
    r.started = true;
    auto& surface = r.eng->surfaceRouter2D();
    auto& gw = surface.subsurface();
    auto& state = gw.state();
    ASSERT_TRUE(gw.configureSourceReceipts({0}).empty());
    // Accepted top water may still be pending when another producer fills
    // the receiver. Its numerical refund must retain the original donor.
    SurfaceIntakeReceipt receipt;
    receipt.request.cell = 0;
    receipt.request.source = "pending-donor";
    receipt.end = end;
    receipt.volume = intake;
    receipt.mass = {3. * intake};
    gw.retainSourceReceipt(receipt);
    gw.bookInfiltrationFromSurface(0, intake);
    gw.bookInfiltrationMass(0, 0, receipt.mass[0]);
    state.hu[0] = state.theta_s[0] * (state.zs[0] - state.hg[0]);
    auto& tr = gw.transport();
    tr.sat_mass[0] = 2. * gw.satVolume(0);
    tr.unsat_mass[0] = 2. * gw.unsatVolume(0);
    const double water = gw.satVolume(0) + gw.unsatVolume(0);
    gw.prepareClock();
    gw.setProducerEnd(end);
    gw.bookLinkSeepage(0, inflow);
    gw.bookLinkSeepageMass(0, 0, 2. * inflow);
    gw.advanceClock(end, surface.state(), false, true);
    double refunds = 0., emergence = 0., mass = 0.;
    for (const auto& returned : gw.sourceReturns()) {
        mass += returned.mass[0];
        if (returned.kind == SurfaceReturnKind::NUMERICAL_REFUND) {
            refunds += returned.volume;
            EXPECT_EQ(returned.intake.request.source, receipt.request.source);
            EXPECT_NEAR(returned.mass[0], 3. * returned.volume, 1e-12);
        } else {
            emergence += returned.volume;
            EXPECT_NEAR(returned.mass[0], 2. * returned.volume, 1e-12);
        }
    }
    EXPECT_NEAR(refunds, intake, 1e-12);
    EXPECT_NEAR(emergence, inflow, 1e-12);
    EXPECT_NEAR(gw.satVolume(0) + gw.unsatVolume(0) + refunds + emergence,
                water + intake + inflow, 1e-12);
    EXPECT_NEAR(tr.sat_mass[0] + tr.unsat_mass[0] + mass,
                2. * (water + inflow) + 3. * intake, 1e-12);
    finish(r);
}

TEST(Aquifer2D, ThinRechargePreservesActualEquilibriumAndLegacyRatesAboveTheFloor) {
    using namespace openswmm::twoD;
    for (auto law :
         {SoilChar::GARDNER, SoilChar::RUSSO, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN}) {
        soil::Params p;
        p.law = law;
        for (double length : {1e-7, .0005}) {
            EXPECT_NEAR(soil::rechargeQ0AtLength(p, length, soil::equilibriumStorage(p, length)),
                        0., 1e-12);
            EXPECT_GT(soil::rechargeQ0AtLength(p, length, p.theta_r * length), 0.);
            EXPECT_LE(soil::rechargeQ0AtLength(p, length, p.theta_s * length), 0.);
        }
        for (double length : {.001, .01, 1.})
            for (double theta : {.1, .3, .45})
                EXPECT_DOUBLE_EQ(soil::rechargeQ0AtLength(p, length, theta * length),
                                 soil::rechargeQ0(p, length, theta * length));
    }
}

TEST(Aquifer2D, IndependentThinFullColumnDoesNotDrainIntoAnInventedMillimetre) {
    for (const char* law : {"GARDNER", "RUSSO", "BROOKS_COREY", "VAN_GENUCHTEN"})
        for (double length : {.00075, .0000005}) {
            SCOPED_TRACE(std::string(law) + "/" + std::to_string(length));
            auto r =
                openDeck("clock-thin-full-" + std::string(law) + std::to_string(length),
                         deck("[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\n"
                              "NODE_ENROLMENT ROWS\nGW_ET NONE\n[2D_AQUIFER]\n"
                              "CELL 1 360 1 .45 .1 2 HG0 .99925 CLOSURE CLOSED_FORM SOIL_CHAR " +
                                  std::string(law) + "\n",
                              "", 0.));
            ASSERT_TRUE(r.opened);
            ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
            ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
            r.started = true;
            auto& surface = r.eng->surfaceRouter2D();
            auto& gw = surface.subsurface();
            auto& state = gw.state();
            state.hg[0] = state.zs[0] - length;
            state.hu[0] = state.theta_s[0] * (state.zs[0] - state.hg[0]);
            const double head = state.hg[0], column = state.hu[0], water = state.liveStorage();
            gw.prepareClock();
            gw.advanceClock(1. / 256, surface.state(), false, true);
            // Full storage is above hydrostatic equilibrium. With capillary
            // rise disabled and no forcing there is no downward recharge.
            EXPECT_DOUBLE_EQ(state.q0_last[0], 0.);
            EXPECT_DOUBLE_EQ(state.hg[0], head);
            EXPECT_DOUBLE_EQ(state.hu[0], column);
            EXPECT_DOUBLE_EQ(state.dunne_cumulative[0], 0.);
            EXPECT_DOUBLE_EQ(state.reject_cumulative[0], 0.);
            EXPECT_NEAR(state.liveStorage(), water, 1e-12);
            finish(r);
        }
}

TEST(Aquifer2D, IndependentColumnDrainageWakesWithoutExternalReceipts) {
    auto r = openDeck(
        "clock-column-drainage",
        deck(
            "[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\nGW_HEAD_TOLERANCE .0001\nNODE_ENROLMENT ROWS\nM_LAYERS 4\n"
            "[2D_AQUIFER]\n* 36 1 .45 .1 2 HG0 .5 CLOSURE SIGMA\n",
            "", 0.));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
    r.started = true;
    auto& surface = r.eng->surfaceRouter2D();
    auto& gw = surface.subsurface();
    const auto before = gw.state().hg;
    const double water = gw.state().liveStorage();
    gw.prepareClock();
    EXPECT_EQ(gw.pendingClockWater(0).first, 0.);
    EXPECT_EQ(gw.pendingClockWater(0).second, 0.);
    EXPECT_EQ(gw.clockRung(0), 0);
    gw.advanceClock(1., surface.state(), false);
    EXPECT_EQ(gw.cellCompletedTime(0), 1.);
    EXPECT_GT(gw.state().hg[0], before[0]);
    EXPECT_GT(gw.state().led_recharge, 0.);
    EXPECT_NEAR(gw.state().liveStorage(), water, 1e-12);
    finish(r);
}

TEST(Aquifer2D, IndependentBaseRungCapsWetProducerChecks) {
    for (const char* ceiling : {"3600", "1", ".0625"}) {
        SCOPED_TRACE(ceiling);
        auto r =
            openDeck("clock-base-producer-" + std::string(ceiling),
                     deck("[2D_AQUIFER_OPTIONS]\nGW_CLOCK INDEPENDENT\nGW_HEAD_TOLERANCE .0001\n"
                          "GW_MAX_STEP " +
                              std::string(ceiling) +
                              "\nNODE_ENROLMENT ROWS\nM_LAYERS 4\n[2D_AQUIFER]\n"
                              "* 36 1 .45 .1 2 HG0 .5 CLOSURE SIGMA\n",
                          "INFILTRATION NO\n", .01));
        ASSERT_TRUE(r.opened);
        ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
        r.started = true;
        double actual = 0;
        ASSERT_EQ(swmm_coupling_advance_to(r.e, 5., &actual), SWMM_OK);
        const auto& gw = r.eng->surfaceRouter2D().subsurface();
        ASSERT_NE(gw.clockState(), nullptr);
        const double producer = gw.clockState()->schedule->producerStep();
        EXPECT_EQ(gw.clockState()->firings[2], std::llround(10 / producer));
        EXPECT_LE(gw.clockState()->maximum[2], producer);
        finish(r);
    }
}

TEST(Aquifer2D, IndependentBudgetsIgnoreFutureReceiptsAndKeepOpposingTransfers) {
    auto r = openDeck(
        "clock-directional-budget",
        deck(aquiferSection(
                 "CLOSURE ENSLAVED HG0 1",
                 "GW_CLOCK INDEPENDENT\nGW_HEAD_TOLERANCE 0.000001\nNODE_ENROLMENT ROWS\n"),
             "", 0.));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
    r.started = true;
    auto& surface = r.eng->surfaceRouter2D();
    auto& gw = surface.subsurface();
    gw.prepareClock();
    gw.setProducerEnd(100.);
    gw.bookLinkSeepage(0, .01);
    gw.bookLinkSeepage(0, -.01);
    EXPECT_DOUBLE_EQ(gw.pendingClockWater(0, 1.).first, 0.);
    EXPECT_DOUBLE_EQ(gw.pendingClockWater(0, 1.).second, 0.);
    gw.advanceClock(1., surface.state(), false);
    EXPECT_DOUBLE_EQ(gw.cellCompletedTime(0), 0.);
    gw.setProducerEnd(2.);
    gw.bookLinkSeepage(0, .002);
    gw.bookLinkSeepage(0, -.002);
    EXPECT_NEAR(gw.pendingClockWater(0, 2.).first, .002, 1e-15);
    EXPECT_NEAR(gw.pendingClockWater(0, 2.).second, .002, 1e-15);
    gw.advanceClock(2., surface.state(), false);
    EXPECT_DOUBLE_EQ(gw.cellCompletedTime(0), 2.);
    EXPECT_NEAR(gw.pendingClockWater(0).first, .01, 1e-15);
    EXPECT_NEAR(gw.pendingClockWater(0).second, .01, 1e-15);
    EXPECT_EQ(gw.clockState()->budget_firings[1], 1);
    EXPECT_GT(gw.clockState()->excess_in[1], 0.);
    EXPECT_GT(gw.clockState()->excess_out[1], 0.);
    EXPECT_GE(gw.clockState()->schedule->rung(0), 0); // accuracy alone is never stiff
    finish(r);
}

TEST(Aquifer2D, IndependentLayerContactUsesItsReceivingLayerBudget) {
    auto r = openDeck(
        "clock-layer-budget",
        deck(
            aquiferSection("CLOSURE SIGMA HG0 .5",
                           "GW_CLOCK INDEPENDENT\nNODE_ENROLMENT ROWS\nM_LAYERS 2\n") +
                "[POLLUTANTS]\nDye MG/L 0 0 0 0\n[GW_TRANSPORT_OPTIONS]\nUNSAT_TRANSPORT LAYERED\nTRANSPORT_POLLUTANTS YES\n",
            "", 0.));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
    r.started = true;
    auto& surface = r.eng->surfaceRouter2D();
    auto& gw = surface.subsurface();
    auto& s = gw.state();
    auto& tr = gw.transport();
    ASSERT_EQ(tr.n_layers, 2);
    const double dzA = (s.zs[0] - s.hg[0]) * s.area[0] / tr.n_layers;
    s.theta_sigma[0] = s.theta_s[0] - 1e-6;
    s.hu[0] = 0;
    tr.unsat_mass[0] = 0;
    constexpr double concentration = 2.;
    for (int j = 0; j < tr.n_layers; ++j) {
        const double water = s.theta_sigma[j * s.n_cells] * dzA;
        s.hu[0] += water / s.area[0];
        tr.layer_mass[tr.layerIdx(0, j, 0)] = water * concentration;
        tr.unsat_mass[0] += water * concentration;
    }
    tr.sat_mass[0] = gw.satVolume(0) * concentration;
    const auto mass = [&] {
        const auto sum = [](const auto& values) { return std::accumulate(values.begin(), values.end(), 0.); };
        return sum(tr.sat_mass) + sum(tr.unsat_mass) + sum(tr.sacc_L) + sum(tr.sacc_R) + sum(tr.xacc_to_surface);
    };
    const double water0 = s.liveStorage(), mass0 = mass();
    gw.prepareClock();
    gw.setProducerEnd(1.);
    const double volume = .75e-6 * dzA;
    const double contact = s.z_bed[0] + s.zs[0] - .01;
    gw.bookLinkContact(0, volume, {concentration}, contact, contact);
    gw.advanceClock(1., surface.state(), false);
    EXPECT_EQ(gw.cellCompletedTime(0), 1.);
    EXPECT_EQ(gw.clockState()->budget_firings[2], 1);
    EXPECT_GT(gw.clockState()->excess_in[2], 0.);
    EXPECT_NEAR(s.liveStorage(), water0 + volume, 1e-12);
    EXPECT_NEAR(mass(), mass0 + concentration * volume, 1e-12);
    finish(r);
}

TEST(Aquifer2D, RuntimeBoundaryEditLandsOnlyTheEditedIndependentCell) {
    auto r = openDeck("clock-runtime-edit", deck(aquiferSection(
        "CLOSURE ENSLAVED", "GW_CLOCK INDEPENDENT\nGW_MAX_STEP 1000\nNODE_ENROLMENT ROWS\n")));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 0), SWMM_OK);
    r.started = true;
    double actual = 0;
    ASSERT_EQ(swmm_coupling_advance_to(r.e, 1., &actual), SWMM_OK);
    ASSERT_DOUBLE_EQ(actual, 1.);
    double held = -1.;
    ASSERT_EQ(swmm_gw2d_get_cell(r.e, 0, SWMM_GW2D_VAR_COMPLETED_TIME, &held), SWMM_OK);
    EXPECT_EQ(held, 0.);
    const int ns = r.eng->surfaceRouter2D().subsurface().transport().n_species;
    std::vector<double> concentrations(ns, 0.);
    ASSERT_NE(swmm_gw2d_set_runtime_boundary(r.e, 0, 15, 2, .0001, concentrations.data(),ns), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_cell(r.e, 0, SWMM_GW2D_VAR_COMPLETED_TIME, &held), SWMM_OK);
    EXPECT_EQ(held, 0.);
    ASSERT_EQ(swmm_gw2d_set_runtime_boundary(r.e, 0, 0, 2, .0001, concentrations.data(),ns), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_cell(r.e, 0, SWMM_GW2D_VAR_COMPLETED_TIME, &held), SWMM_OK);
    EXPECT_EQ(held, actual);
    ASSERT_EQ(swmm_gw2d_get_cell(r.e, 1, SWMM_GW2D_VAR_COMPLETED_TIME, &held), SWMM_OK);
    EXPECT_EQ(held, 0.);
    finish(r);
}

TEST(Aquifer2D, ClockOptionsRoundTripAndSharedAlwaysBindsTheSurface) {
    auto r = openDeck("clock-options", deck(aquiferSection("CLOSURE ENSLAVED")));
    ASSERT_TRUE(r.opened);
    char value[64]{};
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_BINDS_SURFACE_DT", "NO"), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "GW_BINDS_SURFACE_DT", value, sizeof(value)), SWMM_OK);
    EXPECT_STREQ(value, "YES");
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_CLOCK", "INDEPENDENT"), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "GW_BINDS_SURFACE_DT", value, sizeof(value)), SWMM_OK);
    EXPECT_STREQ(value, "NO");
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_BASE_STEP", "0.12500001234567891"), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_MAX_STEP", "13.5"), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_BUDGET_FRACTION", "0.12345678912345678"), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_HEAD_TOLERANCE", "0.012345678912345678"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_BUDGET_FRACTION", "1.01"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_BUDGET_FRACTION", "0"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_HEAD_TOLERANCE", "1"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_HEAD_TOLERANCE", "nan"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_MAX_STEP", "0"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_BASE_STEP", "nan"), SWMM_OK);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "GW_CLOCK", "AUTO"), SWMM_OK);
    for (const char* binding : {"NO", "YES"}) {
        ASSERT_EQ(swmm_gw2d_option_set(r.e, "GW_BINDS_SURFACE_DT", binding), SWMM_OK);
        const auto path = kOutDir / (std::string("clock-options-") + binding + "-saved.inp");
        ASSERT_EQ(swmm_model_write(r.e, path.string().c_str()), SWMM_OK);
        auto again = openDeck(std::string("clock-options-reopened-") + binding, readAll(path));
        ASSERT_TRUE(again.opened);
        for (const char* key : {"GW_CLOCK", "GW_BASE_STEP", "GW_MAX_STEP", "GW_BINDS_SURFACE_DT", "GW_BUDGET_FRACTION", "GW_HEAD_TOLERANCE"}) {
            char original[64]{};
            ASSERT_EQ(swmm_gw2d_option_get(r.e, key, original, sizeof(original)), SWMM_OK);
            ASSERT_EQ(swmm_gw2d_option_get(again.e, key, value, sizeof(value)), SWMM_OK);
            EXPECT_STREQ(original, value) << key;
        }
        finish(again);
    }
    finish(r);
}

TEST(Aquifer2D, LinkSeepageOptionRoundTripsAndPreservesLegacyAuto) { // G-X3
    DeckRun r = openDeck("seep_rt", seepDeck("LINK_SEEPAGE NONE", 1.0, 9.0));
    ASSERT_TRUE(r.opened);
    char buf[16] = {0};
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "LINK_SEEPAGE", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "NONE");
    const fs::path saved = kOutDir / "seep_rt_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    {
        const std::string text = readAll(saved);
        const auto k = text.find("LINK_SEEPAGE");
        ASSERT_NE(k, std::string::npos);
        EXPECT_NE(text.find("NONE", k), std::string::npos);
    }
    ASSERT_EQ(swmm_gw2d_option_set(r.e, "LINK_SEEPAGE", "AUTO"), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "LINK_SEEPAGE", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "ONE_WAY");
    EXPECT_NE(swmm_gw2d_option_set(r.e, "LINK_SEEPAGE", "MAYBE"), SWMM_OK);
    finish(r);
}

// ---------------------------------------------------------------------------
// G-X4 gates (2026-09-20) — the SIGNED conduit ⇄ aquifer conductance.
// `LINK_SEEPAGE TWO_WAY` replaces the fixed [LOSSES] loss with
// `K·W·L·min((h_link − max(h_gw, z_inv))/d_c, 1)`: the legacy rate exactly
// when the pipe is full over a table at or below its invert, throttled as
// the table rises, and NEGATIVE — a gaining reach, water the aquifer hands
// to the pipe — once the table passes the water surface. The deck is the
// seepage deck of G-X3 with the table moved: `HG0` decides the direction.
// ---------------------------------------------------------------------------
namespace {
/// A deck whose CONDUIT RUNS INSIDE THE AQUIFER — which the two-cell pan of
/// the decks above does not (its mesh sits 10 m below the network, so no
/// table it can hold ever reaches the pipe). Here the cells' ground is at
/// 0 m, `ZS 8` puts the aquifer bottom at −8 m, and C1 runs from J1 (invert
/// −5.00) to O1 (invert −5.05) at y = 5, wholly over the mesh — mid invert
/// −5.025 m, crown −4.55 m. `HG0` alone decides the direction:
///
///   HG0 6.0 → table at −2.0 m, 3.0 m ABOVE the invert → the pipe gains
///   HG0 2.0 → table at −6.0 m, 1.0 m BELOW it        → disconnected, and a
///             FULL pipe then loses exactly the legacy rate.
///
/// Both tables leave metres of genuinely unsaturated column above them. That
/// is not decoration: closure A's specific yield is `θ_s − hu/L`, so a table
/// parked just under the ground (the G-X1 manhole deck's regime) has almost
/// no yield and moves metres for a litre — conserving, but useless as a
/// gauge of anything.
///
/// @param drowned FIXED outfall stage above the crown, so the reach is
///        pressurized and `depth == d_c` for the whole run — the regime in
///        which the signed law must reproduce the legacy rate to the bit.
/// @brief Build a conduit fixture whose water table can drive signed exchange.
/// @param[in] hg0 Initial saturated height above the aquifer bottom in metres.
/// @param[in] link_rows Raw 2D_AQUIFER_LINKS rows; empty omits the section.
/// @param[in] extra_options Raw aquifer option rows appended after NODE_ENROLMENT ROWS.
/// @return Owned CMS deck text; rows are copied without validation or file IO.
std::string twoWayDeck(double hg0, const std::string& link_rows = "",
                       const std::string& extra_options = "", bool drowned = false) {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "DRY_STEP             00:01:00\nROUTING_STEP         5\n"
         "ALLOW_PONDING        NO\n\n"
         "[JUNCTIONS]\nJ1 -5.00 6.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -5.05 "
      << (drowned ? "FIXED -4.0 NO" : "FREE NO")
      << "\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[LOSSES]\n;;Link Kentry Kexit Kavg Flap Seepage(mm/hr)\nC1  0  0  0  NO  100\n\n"
         "[INFLOWS]\nJ1  FLOW  IN1  FLOW  1.0  1.0\n\n"
         "[TIMESERIES]\nIN1  0:00  0.5\nIN1  1:00  0.5\n\n"
         "[COORDINATES]\nJ1  1.0  5.0\nO1  9.0  5.0\n\n"
         "[2D_OPTIONS]\nMOMENTUM_EQUATION LOCAL_INERTIAL\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n\n"
         "[2D_VERTICES]\n"
         "0.0 0.0 0.0\n10.0 0.0 0.0\n10.0 10.0 0.0\n0.0 10.0 0.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 0.0\n0 2 3 0.03 0.0\n\n"
         "[2D_AQUIFER_OPTIONS]\nCLOSURE CLOSED_FORM\nNODE_ENROLMENT ROWS\n"
      << extra_options
      << "\n"
         "[2D_AQUIFER]\n*  36.0  8.0  0.45  0.10  2.0  HG0 "
      << hg0 << "\n\n";
    if (!link_rows.empty())
        m << "[2D_AQUIFER_LINKS]\n" << link_rows << "\n";
    m << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct TwoWayResult {
    double seep_loss = 0.0, gw_inflow = 0.0; // 1D ledger (m³)
    double led_link = 0.0, pending = 0.0, storage = 0.0, init = 0.0, resid = 0.0;
    double hg0 = 0.0, rate_last = 0.0;
    bool warned = false;
};

/**
 * @brief Run a bidirectional conduit/aquifer scenario and copy diagnostics.
 *
 * Records nonfatal test failures for open/run/public ledger rejection, then samples state and
 * finishes the handle. Open failure returns a zero-initialized result without finishing the
 * allocated failed-open handle. A later run failure does not stop inspection; intended fixture
 * geometry/order is required.
 *
 * @param[in] tag Artifact stem appended below kOutDir; matching .inp/.rpt/.out paths may be
 *     overwritten.
 * @param[in] body Borrowed full input deck text written verbatim.
 * @return Owned result containing cubic-metre ledgers/pending volumes, metre head, native last
 *     conduit seep-loss rate and warning flag.
 */
TwoWayResult runTwoWay(const std::string& tag, const std::string& body) {
    TwoWayResult out;
    DeckRun r = openDeck(tag, body);
    EXPECT_TRUE(r.opened);
    if (!r.opened)
        return out;
    EXPECT_TRUE(run(r));
    out.warned = warnedAbout(r, "a reach under the water table GAINS");
    const auto& ctx = r.eng->context();
    const auto& g = r.eng->surfaceRouter2D().subsurface().state();
    const double v12 = r.eng->surfaceRouter2D().options().vol_1d_to_2d;
    out.seep_loss = ctx.mass_balance.routing_seep_loss * v12;
    out.gw_inflow = ctx.mass_balance.routing_link_gw_inflow * v12;
    const int cr = ctx.link_subtypes.conduit_row(0);
    if (cr >= 0)
        out.rate_last = ctx.link_subtypes.conduits.seep_loss_rate[static_cast<std::size_t>(cr)];
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_LINK, &out.led_link), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_STORAGE, &out.storage), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_INIT_STORAGE, &out.init), SWMM_OK);
    EXPECT_EQ(swmm_gw2d_get_continuity_error(r.e, &out.resid), SWMM_OK);
    for (double v : g.lacc)
        out.pending += v;
    if (g.n_cells > 0)
        out.hg0 = g.hg[0];
    finish(r);
    return out;
}
} // namespace

TEST(Aquifer2D, ConduitUnderTheWaterTableGainsFromTheAquifer) { // gate (f)
    // HG0 3.9 puts the table 0.1 m under the ground (−10 + 3.9 = −6.1 m),
    // far above the conduit (invert −0.5 m mean). Every term reverses.
    const TwoWayResult up =
        runTwoWay("gain_two_way", twoWayDeck(6.0, "", "LINK_SEEPAGE TWO_WAY\n"));
    EXPECT_TRUE(up.warned) << "the signed law was not announced";
    EXPECT_GT(up.gw_inflow, 1.0e-3) << "the drowned conduit gained nothing";
    EXPECT_EQ(up.seep_loss, 0.0) << "a gaining reach also booked a loss";
    EXPECT_LT(up.rate_last, 0.0) << "the seepage rate never went negative";
    // the aquifer paid for it, to the drop: led_link is the NET (negative)
    EXPECT_NEAR(up.led_link + up.pending, -up.gw_inflow, 1.0e-9 * up.gw_inflow)
        << "1D gained " << up.gw_inflow << " but the aquifer lost " << -(up.led_link + up.pending);
    EXPECT_NEAR(up.storage - up.init, up.led_link + up.pending, 1.0e-9 * up.gw_inflow + 1.0e-12);
    EXPECT_LT(std::fabs(up.resid), 1.0e-9 * up.storage + 1.0e-12) << "residual " << up.resid;
    EXPECT_LT(up.hg0, 6.0) << "the table did not draw down under the gaining reach";

    // The same deck with the table at the bottom (HG0 0.0, well below the
    // conduit invert) is the DISCONNECTED limit: the pipe loses, and — the
    // property the whole law is built around — a full pipe loses exactly
    // what LINK_SEEPAGE AUTO's legacy rate does.
    const TwoWayResult down = runTwoWay(
        "gain_two_way_low", twoWayDeck(2.0, "", "LINK_SEEPAGE TWO_WAY\n", /*drowned=*/true));
    const TwoWayResult legacy =
        runTwoWay("gain_auto_low", twoWayDeck(2.0, "", "LINK_SEEPAGE ONE_WAY\n", /*drowned=*/true));
    EXPECT_GT(down.seep_loss, 1.0e-3);
    EXPECT_EQ(down.gw_inflow, 0.0);
    // The RATE is the invariant, not the cumulative volume: the run starts
    // with an empty pipe, where `depth < d_c` and the signed law correctly
    // throttles while the legacy law — which assumes a unit gradient
    // whatever the depth — does not. Once the pipe is full the two are the
    // same number.
    EXPECT_NEAR(down.rate_last, legacy.rate_last, 1.0e-12 * legacy.rate_last)
        << "TWO_WAY changed the loss of a FULL pipe over a disconnected table";
    EXPECT_LE(down.seep_loss, legacy.seep_loss * (1.0 + 1.0e-9))
        << "the signed law lost MORE than the unit-gradient law it caps at";
}

TEST(Aquifer2D, LinkSeepageAutoIsUnchangedByTheSignedLaw) { // gate (g)
    // AUTO is G-X3: one-way, the legacy rate, whatever the table does. The
    // high-table deck that gains under TWO_WAY must still LOSE under AUTO.
    const TwoWayResult a = runTwoWay("gain_auto_high", twoWayDeck(6.0, "", "LINK_SEEPAGE AUTO\n"));
    EXPECT_FALSE(a.warned);
    EXPECT_GT(a.seep_loss, 1.0e-3) << "AUTO stopped losing";
    EXPECT_EQ(a.gw_inflow, 0.0) << "AUTO gained — the signed law leaked into it";
    EXPECT_GT(a.led_link + a.pending, 0.0);
}

TEST(Aquifer2D, AquiferLinksRowOverridesAndOptsOut) { // gate (h)
    // EXCHANGE NO: the conduit is not in the share table at all, so it keeps
    // its legacy loss and the aquifer receives nothing.
    const TwoWayResult off =
        runTwoWay("gain_row_off", twoWayDeck(6.0, "C1  EXCHANGE NO\n", "LINK_SEEPAGE TWO_WAY\n"));
    EXPECT_FALSE(off.warned) << "an opted-out conduit was still enrolled";
    EXPECT_EQ(off.led_link, 0.0);
    EXPECT_EQ(off.pending, 0.0);
    EXPECT_EQ(off.gw_inflow, 0.0);
    EXPECT_GT(off.seep_loss, 1.0e-3) << "the 1D stopped seeping as well";

    // KC scales the conductance: ten times the conductivity, ten times the
    // gain (the law is linear in K and nothing else changed — the table
    // draws down more, so the check is a band, not an equality).
    const TwoWayResult k1 =
        runTwoWay("gain_row_k1", twoWayDeck(6.0, "C1  KC 100\n", "LINK_SEEPAGE TWO_WAY\n"));
    const TwoWayResult k10 =
        runTwoWay("gain_row_k10", twoWayDeck(6.0, "C1  KC 1000\n", "LINK_SEEPAGE TWO_WAY\n"));
    EXPECT_GT(k1.gw_inflow, 1.0e-3);
    // The law is linear in K, so ten times the conductivity would be ten
    // times the gain — at a FIXED head. It is not fixed: 17 m³ out of two
    // 50 m² cells draws the table down ~0.5 m, which cuts the driving head,
    // so the ratio lands between the linear bound and 1. Both ends matter —
    // above 10 would mean the law is not linear in K, at 1 it would mean KC
    // is not reaching the law at all (the bug this gate first caught: KC
    // was multiplied by the unit factor instead of divided, and every
    // conductance came out 10^9 times too large).
    EXPECT_GT(k10.gw_inflow, 3.0 * k1.gw_inflow)
        << "KC 1000 gained " << k10.gw_inflow << " vs KC 100 " << k1.gw_inflow;
    EXPECT_LT(k10.gw_inflow, 10.0 * k1.gw_inflow)
        << "the gain outran the conductance it is linear in";
    // …and the aquifer still pays exactly, at either conductance.
    EXPECT_NEAR(k10.led_link + k10.pending, -k10.gw_inflow, 1.0e-9 * k10.gw_inflow);
    EXPECT_LT(std::fabs(k10.resid), 1.0e-9 * k10.storage + 1.0e-12);
}

TEST(Aquifer2D, TwoWayRoundTripsAndTheRowsEcho) { // G-X4 authoring
    DeckRun r =
        openDeck("two_way_rt", twoWayDeck(6.0, "C1  KC 50 DC 2.5\n", "LINK_SEEPAGE TWO_WAY\n"));
    ASSERT_TRUE(r.opened);
    char buf[16] = {0};
    ASSERT_EQ(swmm_gw2d_option_get(r.e, "LINK_SEEPAGE", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "TWO_WAY");
    const fs::path saved = kOutDir / "two_way_rt_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("[2D_AQUIFER_LINKS]"), std::string::npos);
    const auto at = text.find("[2D_AQUIFER_LINKS]");
    EXPECT_NE(text.find("KC", at), std::string::npos);
    EXPECT_NE(text.find("DC", at), std::string::npos);
    EXPECT_NE(swmm_gw2d_option_set(r.e, "LINK_SEEPAGE", "SOMETIMES"), SWMM_OK);
    finish(r);
}

// ---------------------------------------------------------------------------
// G-X4 — `swmm_forcing_link_seepage`: the HydroCouple inlet. A host's
// groundwater component computes the exchange and the conduit routes it,
// over the top of whatever the deck's own law produced. Driven here with
// the internal aquifer coupled, so the gate also pins the accounting: the
// forced rate is what the 2D aquifer is debited, and a negative one lands
// in the 1D as a gain, not as a negative loss.
// ---------------------------------------------------------------------------
TEST(Aquifer2D, HostForcedConduitExchangeIsRoutedAndBooked) {
    DeckRun r = openDeck("force_seep", twoWayDeck(2.0, "", "LINK_SEEPAGE TWO_WAY\n"));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    // −0.35 out of the pipe = a gain, held for the run. The forcing API
    // takes the engine's INTERNAL flow unit (ft³/s) whatever FLOW_UNITS
    // says — the convention swmm_forcing_link_flow already set — so the
    // expectation below converts with the router's own factor rather than
    // assuming the deck's CMS.
    const double forced_cfs = -0.35;
    ASSERT_EQ(
        swmm_forcing_link_seepage(r.e, 0, forced_cfs, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
        SWMM_OK);
    double elapsed = 0.0, seconds = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0)
        seconds = elapsed * 86400.0;
    const auto& ctx = r.eng->context();
    const double v12 = r.eng->surfaceRouter2D().options().vol_1d_to_2d;
    const double gw_in = ctx.mass_balance.routing_link_gw_inflow * v12;
    const double seep = ctx.mass_balance.routing_seep_loss * v12;
    double led_link = 0.0, pending = 0.0, resid = 0.0;
    ASSERT_EQ(swmm_gw2d_get_ledger(r.e, SWMM_GW2D_LED_LINK, &led_link), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_continuity_error(r.e, &resid), SWMM_OK);
    for (double v : r.eng->surfaceRouter2D().subsurface().state().lacc)
        pending += v;
    // the whole run at the forced rate — the deck's own law, which was
    // LOSING here (table 1 m under the invert), never got a say
    EXPECT_EQ(seep, 0.0) << "the forced gain still booked a loss";
    const double expect_m3 = -forced_cfs * seconds * v12;
    EXPECT_NEAR(gw_in, expect_m3, 0.02 * expect_m3)
        << "routed " << gw_in << " m3 over " << seconds << " s, wanted " << expect_m3;
    // …and the aquifer paid for exactly what the pipe received
    EXPECT_NEAR(led_link + pending, -gw_in, 1.0e-9 * gw_in);
    EXPECT_LT(std::fabs(resid), 1.0e-6) << "residual " << resid;
    finish(r);
}

TEST(Aquifer2D, AutomaticLinkSeepageUsesTheSignedLaw) {
    const auto automatic = runTwoWay("default_signed", twoWayDeck(6.0));
    const auto explicitMode =
        runTwoWay("explicit_signed", twoWayDeck(6.0, "", "LINK_SEEPAGE TWO_WAY\n"));
    EXPECT_GT(automatic.gw_inflow, 0.0);
    EXPECT_EQ(automatic.seep_loss, explicitMode.seep_loss);
    EXPECT_EQ(automatic.gw_inflow, explicitMode.gw_inflow);
    EXPECT_EQ(automatic.storage, explicitMode.storage);
    EXPECT_EQ(automatic.led_link, explicitMode.led_link);
    EXPECT_EQ(automatic.pending, explicitMode.pending);
    EXPECT_LT(std::fabs(automatic.resid), 1e-9 * automatic.storage + 1e-12);
}

namespace {
using namespace openswmm::twoD;
using openswmm::richards::Column;
struct NodeTrialRig {
    MeshData mesh;
    InertialEdges edges;
    SolverOptions2D options = [] { // LOCAL_INERTIAL: not the default since 2026-11-13
        SolverOptions2D o;
        o.momentum = Momentum2D::LOCAL_INERTIAL;
        return o;
    }();
    SubsurfaceConfig config;
    SubsurfaceSolver gw;
    /**
     * @brief Initialize a minimal aquifer solver with requested node-bed contacts.
     *
     * Uses default GwUnitFactors and fixed aquifer properties. Owns all mesh/options/config/solver
     * members, throws runtime_error on solver initialization rejection, and discards collected
     * warnings.
     *
     * @param[in] nodes Nonnegative node/contact count; one bed per node is mapped to node modulo cells.
     * @param[in] closure Groundwater closure selected for initialized state.
     * @param[in] hg Initial saturated height in metres in the fixed 4 m column.
     * @param[in] area Mesh cell plan area in m2, assigned to each cell.
     * @param[in] contact Bed contact area in m2 per node.
     * @param[in] kc Bed conductivity passed unchanged to each GwNodeBed.
     * @param[in] dc Bed thickness in metres passed unchanged to each GwNodeBed.
     * @param[in] cells Strictly positive mesh cell count; required by modulo mapping.
     * @param[in] et Raw groundwater ET option string passed to initialization.
     * @param[in] dunne Enable saturation-excess return when true.
     */
    explicit NodeTrialRig(int nodes = 1, GwClosure closure = GwClosure::CLOSED_FORM, double hg = 2,
                          double area = 1, double contact = 40, double kc = 0, double dc = 0,
                          int cells = 1, const std::string& et = "NONE", bool dunne = true) {
        mesh.resize_triangles(cells);
        mesh.size_neighbours();
        mesh.size_geometry();
        for (int i = 0; i < cells; ++i) {
            mesh.tri_area[i] = area;
            mesh.tri_cz[i] = 0;
        }
        edges.build(mesh);
        config.options.closure = closure;
        config.options.force_closed_form = true;
        config.options.gw_et = et;
        config.options.dunne = dunne;
        GwAquiferRow row;
        row.Ks = 1.e-3;
        row.zs = 4;
        row.theta_s = .45;
        row.theta_r = .1;
        row.alpha = 2;
        row.hg0 = hg;
        config.rows.push_back(row);
        for (int n = 0; n < nodes; ++n) {
            GwNodeBed b;
            b.node = n;
            b.cell = n % cells;
            b.area = contact;
            b.Kc = kc;
            b.dC = dc;
            config.node_beds.push_back(b);
        }
        std::vector<std::string> warnings;
        GwUnitFactors units;
        const auto error = gw.initialize(mesh, edges, options, units, nodes, config, warnings);
        if (!error.empty())
            throw std::runtime_error(error);
    }
};

TEST(AquiferInfiltration, PublishedHeadroomRetainsLiveSignedReceiptSummation) {
    using openswmm::detail::CompletedSourceArchive;
    CompletedSourceArchive archive;
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (double pending : {-1., -0., 0., 1e-17, .1, 1.})
            for (bool prescribed : {false, true})
                for (double capacity : {0., .05}) {
                    NodeTrialRig rig(3, closure);
                    auto& s = rig.gw.state();
                    ASSERT_EQ(static_cast<GwClosure>(s.closure[0]), closure);
                    if (closure == GwClosure::SIGMA) {
                        std::fill(s.theta_sigma.begin(), s.theta_sigma.end(), .2);
                        s.hu[0] = .2 * (s.zs[0] - s.hg[0]);
                    }
                    s.xacc_from_surface[0] = pending;
                    s.lacc[0] = .00037;
                    s.nacc = {-.0001, .0002, -.0003};
                    s.infil_capacity[0] = capacity;
                    s.infil_remaining[0] = -7;
                    if (prescribed && pending < 0 && capacity > 0)
                        ASSERT_GT(rig.gw.infiltrationHeadroom(0), 0);
                    for (double requested : {-1., -0., 0., 1e-300, 1e-17, .1, 1.,
                                              std::numeric_limits<double>::infinity(),
                                              std::numeric_limits<double>::quiet_NaN()}) {
                        const double room = rig.gw.infiltrationHeadroom(0);
                        const double old_remaining = s.infil_remaining[0];
                        const double old_pending = s.xacc_from_surface[0];
                        const double old_front = s.wetting_front[0];
                        const bool admitted = requested > 0 && std::isfinite(requested) &&
                                              (prescribed || capacity > 0);
                        const double expected = admitted ? std::min(requested, room) : 0.;
                        double taken = rig.gw.acceptSurfaceInfiltration(0, requested, prescribed);
                        EXPECT_EQ(std::bit_cast<std::uint64_t>(taken),
                                  std::bit_cast<std::uint64_t>(expected));
                        if (admitted) {
                            // Re-read canonical storage and the updated receipt sequence.
                            const double fresh = rig.gw.infiltrationHeadroom(0);
                            EXPECT_EQ(std::bit_cast<std::uint64_t>(s.infil_remaining[0]),
                                      std::bit_cast<std::uint64_t>(fresh));
                        } else {
                            EXPECT_EQ(s.infil_remaining[0], old_remaining);
                            EXPECT_EQ(s.xacc_from_surface[0], old_pending);
                            EXPECT_EQ(s.wetting_front[0], old_front);
                        }
                        archive(taken, s.xacc_from_surface, s.wetting_front, s.infil_remaining,
                                s.lacc, s.nacc, s.hg, s.hu, s.theta_sigma);
                    }
                }
    const char* output = std::getenv("OPENSWMM_HEADROOM_TEST_OUT");
    const auto folder = output ? fs::path(output) : kOutDir;
    fs::create_directories(folder);
    const auto bytes = archive.bytes();
    std::ofstream(folder / "headroom-bits.bin", std::ios::binary).write(bytes.data(), bytes.size());
}
/**
 * @brief Build an owned Richards column for aquifer exchange trials.
 *
 * Does not solve or validate the column. Nonpositive count adds no material cells; out-of-range
 * saturation is retained for caller-selected rejection tests.
 *
 * @param[in] count Number of unit-total-depth material cells; positive for a meaningful one-metre
 *     profile.
 * @param[in] se Effective material saturation used as theta=0.05+0.4*se, normally in [0,1].
 * @return Owned enabled column with one surface cell holding 0.02 m3 and count material cells over
 *     1 m2.
 */
Column trialColumn(int count = 4, double se = .8) {
    Column c;
    c.options.enabled = true;
    c.options.max_step = 10;
    c.cells.push_back({.1, 1, 1, .1});
    c.water.push_back(.02);
    for (int n = 0; n < count; ++n) {
        openswmm::richards::Cell cell;
        cell.volume = cell.dz = 1. / count;
        cell.area = 1;
        cell.z = 1 - (n + .5) / count;
        cell.theta_s = .45;
        cell.Ks = 1.e-3;
        cell.wilting = .1;
        cell.material = {.05, 2, 1.6, .5, 1.e-4};
        c.cells.push_back(cell);
        c.water.push_back(cell.volume * (.05 + .4 * se));
    }
    return c;
}
/**
 * @brief Sum stored water across a trial column.
 *
 * @param[in] c Borrowed column whose water entries are summed unchanged.
 * @return Total water in m3, including its surface entry; zero for an empty vector.
 */
double columnWater(const Column& c) {
    return std::accumulate(c.water.begin(), c.water.end(), 0.);
}
}
TEST(RichardsAquiferTrial, BottomHeadAndInvertDatumAreUsedInsteadOfVisibleSurfaceHead) {
    NodeTrialRig rig;
    auto column = trialColumn(4, .5);
    const auto original = column.water;
    auto r = trialRichardsBeds(rig.gw, 20, {{0, &column, -2.5, .2, 1}});
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.receipts.size(), 1u);
    const auto& receipt = r.receipts[0];
    EXPECT_GT(-2.5 + column.cells[0].z + column.water[0] / column.cells[0].area,
              receipt.aquifer_head);
    EXPECT_LT(receipt.lid_head, receipt.aquifer_head);
    EXPECT_GT(receipt.accepted_up, 0);
    EXPECT_DOUBLE_EQ(r.columns[0].bottom_head, receipt.aquifer_head + 2.5);
    EXPECT_EQ(column.water, original);
    EXPECT_NEAR(columnWater(r.columns[0]) - columnWater(column),
                receipt.accepted_up - receipt.accepted_down, 1.e-10);
}
TEST(RichardsAquiferTrial, ManyContactsShareOneReceiverWithoutAreaClampOrOrderDependence) {
    NodeTrialRig rig(131);
    rig.gw.state().hu[0] = .45 * 2 - 1.e-6; // real finite remaining pore capacity
    std::vector<Column> columns;
    for (int n = 0; n < 131; ++n)
        columns.push_back(trialColumn(4 * (1 + n % 2), .8));
    std::vector<RichardsBedBinding> bindings;
    for (int n = 0; n < 131; ++n)
        bindings.push_back({n, &columns[n], 10, .5, 1});
    const auto before = rig.gw.state();
    auto a = trialRichardsBeds(rig.gw, 20, bindings);
    ASSERT_TRUE(a.error.empty()) << a.error;
    std::reverse(bindings.begin(), bindings.end());
    auto b = trialRichardsBeds(rig.gw, 20, bindings);
    ASSERT_TRUE(b.error.empty()) << b.error;
    long double sum = 0;
    int cap_hits = 0;
    for (std::size_t n = 0; n < a.receipts.size(); ++n) {
        const auto& r = a.receipts[n];
        sum += r.accepted_down;
        cap_hits += a.reports[n].bottom_out_limited;
        EXPECT_EQ(a.columns[n].water, b.columns[n].water);
        EXPECT_DOUBLE_EQ(r.accepted_down, b.receipts[n].accepted_down);
        EXPECT_EQ(r.node, int(n));
        EXPECT_LE(r.accepted_down, r.reserved_down);
        EXPECT_NEAR(columnWater(columns[n]) - columnWater(a.columns[n]),
                    r.accepted_down - r.accepted_up + a.reports[n].evaporation, 1.e-10);
    }
    ASSERT_EQ(a.budgets.size(), 1u);
    // Receipts are doubles; allow one representable step when their sum is
    // accumulated using the platform's long-double representation.
    EXPECT_LE(
        sum, std::nextafter(a.budgets[0].second.receiver, std::numeric_limits<double>::infinity()));
    EXPECT_GT(cap_hits, 0);
    EXPECT_EQ(rig.gw.state().hg, before.hg);
    EXPECT_EQ(rig.gw.state().hu, before.hu);
    EXPECT_EQ(rig.gw.state().nacc, before.nacc);
}
TEST(RichardsAquiferTrial, SharedDryDonorCapsUpwardFlowAcrossEveryClosureAndEtClaim) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA}) {
        NodeTrialRig rig(5, closure, 1.e-5);
        std::vector<Column> columns(5, trialColumn(4, .5));
        std::vector<RichardsBedBinding> bindings;
        for (int n = 0; n < 5; ++n)
            bindings.push_back({n, &columns[n], -10, .5, 1});
        auto a = trialRichardsBeds(rig.gw, 20, bindings);
        ASSERT_TRUE(a.error.empty()) << a.error;
        double sum = 0;
        int cap = 0;
        for (std::size_t n = 0; n < a.receipts.size(); ++n) {
            sum += a.receipts[n].accepted_up;
            cap += a.reports[n].bottom_in_limited;
        }
        EXPECT_LE(sum, a.budgets[0].second.donor);
        EXPECT_GT(cap, 0);
        rig.gw.state().et_pending[0] = a.budgets[0].second.donor;
        auto b = trialRichardsBeds(rig.gw, 20, bindings);
        ASSERT_TRUE(b.error.empty()) << b.error;
        EXPECT_DOUBLE_EQ(b.budgets[0].second.donor, 0);
        for (const auto& r : b.receipts)
            EXPECT_DOUBLE_EQ(r.accepted_up, 0);
    }
}
TEST(RichardsAquiferTrial, PendingInflowsCannotFinanceAnotherNodesUpwardAward) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 0);
    auto down = trialColumn(), up = trialColumn();
    rig.gw.state().nacc[0] = -0.1; // another already pending inward receipt
    auto r = trialRichardsBeds(rig.gw, 20, {{0, &down, 10, .5, 1}, {1, &up, -10, .5, 1}});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_GT(r.receipts[0].accepted_down, 0);
    EXPECT_DOUBLE_EQ(r.receipts[1].accepted_up, 0);
    EXPECT_DOUBLE_EQ(r.budgets[0].second.donor, 0);
    EXPECT_DOUBLE_EQ(rig.gw.state().nacc[0], -.1);
}
TEST(RichardsAquiferTrial, ExistingPendingWithdrawalsAndIntakesConsumeCapacityOnce) {
    NodeTrialRig rig;
    SubsurfaceSolver::NodeWaterBudget old, next;
    ASSERT_TRUE(rig.gw.nodeWaterBudget(0, 20, old).empty());
    rig.gw.state().nacc[0] = old.donor * .25;
    rig.gw.state().et_pending[0] = old.donor * .25;
    ASSERT_TRUE(rig.gw.nodeWaterBudget(0, 20, next).empty());
    EXPECT_NEAR(next.donor, old.donor * .5, 1.e-12);
    rig.gw.state().nacc[0] = -old.receiver * .25;
    rig.gw.state().xacc_from_surface[0] = old.receiver * .25;
    ASSERT_TRUE(rig.gw.nodeWaterBudget(0, 20, next).empty());
    EXPECT_NEAR(next.receiver, old.receiver * .5, 1.e-12);
}
TEST(RichardsAquiferTrial, ZeroClogAndHydraulicReceivingCeilingDoNotFabricateWater) {
    NodeTrialRig rig;
    auto col = trialColumn(4, .5);
    auto r = trialRichardsBeds(rig.gw, 20, {{0, &col, -10, col.water.back(), 1}});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_GT(r.receipts[0].requested_up, 0);
    EXPECT_DOUBLE_EQ(r.receipts[0].reserved_up, 0);
    EXPECT_DOUBLE_EQ(r.receipts[0].accepted_up, 0);
    r = trialRichardsBeds(rig.gw, 20, {{0, &col, -10, .5, 0}});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_DOUBLE_EQ(r.receipts[0].requested_up, 0);
    EXPECT_DOUBLE_EQ(r.receipts[0].accepted_up, 0);
    EXPECT_DOUBLE_EQ(r.columns[0].bottom_factor, 0);
}
TEST(RichardsAquiferTrial, InvalidLastLaneAliasingAndPendingCorruptionLeaveAllOwnersUnchanged) {
    NodeTrialRig rig(2);
    auto a = trialColumn(), b = a;
    const auto water = a.water;
    const auto hg = rig.gw.state().hg;
    b.cells.back().material.specific_storage = 0;
    auto bad = trialRichardsBeds(rig.gw, 20, {{0, &a, -10, .5, 1}, {1, &b, -10, .5, 1}});
    EXPECT_FALSE(bad.error.empty());
    EXPECT_TRUE(bad.columns.empty());
    EXPECT_EQ(a.water, water);
    EXPECT_EQ(rig.gw.state().hg, hg);
    bad = trialRichardsBeds(rig.gw, 20, {{0, &a, -10, .5, 1}, {1, &a, -10, .5, 1}});
    EXPECT_FALSE(bad.error.empty());
    rig.gw.state().nacc[0] = std::numeric_limits<double>::quiet_NaN();
    bad = trialRichardsBeds(rig.gw, 20, {{0, &a, -10, .5, 1}});
    EXPECT_FALSE(bad.error.empty());
    EXPECT_EQ(a.water, water);
}

TEST(RichardsAquiferTrial, EqualAbsoluteHeadsAreRecordedAsKnownZero) {
    NodeTrialRig rig(1, GwClosure::CLOSED_FORM, 4);
    auto c = trialColumn();
    c.water[0] = 0;
    for (std::size_t k = 1; k < c.cells.size(); ++k) {
        auto& cell = c.cells[k];
        cell.theta_s = .5;
        cell.material.specific_storage = .125;
        c.water[k] =
            cell.volume * openswmm::richards::storage(cell.material, cell.theta_s, 1 - cell.z);
    }
    auto r = trialRichardsBeds(rig.gw, 20, {{0, &c, -1, .2, 1}});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_DOUBLE_EQ(r.receipts[0].lid_head, r.receipts[0].aquifer_head);
    EXPECT_DOUBLE_EQ(r.receipts[0].requested_down, 0);
    EXPECT_DOUBLE_EQ(r.receipts[0].requested_up, 0);
    EXPECT_DOUBLE_EQ(r.receipts[0].accepted_down, 0);
    EXPECT_DOUBLE_EQ(r.receipts[0].accepted_up, 0);
    EXPECT_EQ(c.water, r.columns[0].water);
}
TEST(RichardsAquiferTrial, DirectAndSemiConfiningPathsReuseTheirSeriesResistance) {
    NodeTrialRig direct(1, GwClosure::CLOSED_FORM, 2, 1, 40),
        bed(1, GwClosure::CLOSED_FORM, 2, 1, 40, 1.e-4, .5);
    auto c = trialColumn(4, .8);
    auto a = trialRichardsBeds(direct.gw, 1.e-7, {{0, &c, -2.5, .2, 1}});
    auto b = trialRichardsBeds(bed.gw, 1.e-7, {{0, &c, -2.5, .2, 1}});
    ASSERT_TRUE(a.error.empty()) << a.error;
    ASSERT_TRUE(b.error.empty()) << b.error;
    const auto& cell = c.cells.back();
    const double p =
        openswmm::richards::pressure(cell.material, cell.theta_s, c.water.back() / cell.volume);
    const double half = .5 * cell.dz / openswmm::richards::conductivity(cell.material, cell.Ks, p);
    EXPECT_NEAR(a.receipts[0].requested_up / b.receipts[0].requested_up,
                (half + .5 / 1.e-4) / (half + 2 / 1.e-3), 1.e-6);
}
TEST(RichardsAquiferTrial, UnavailableAndCorruptBudgetsRemainErrorsRatherThanZero) {
    SubsurfaceSolver absent;
    SubsurfaceSolver::NodeWaterBudget out{9, 9, 9};
    EXPECT_FALSE(absent.nodeWaterBudget(0, 20, out).empty());
    EXPECT_DOUBLE_EQ(out.donor, 9);
    NodeTrialRig rig(1, GwClosure::SIGMA);
    auto c = trialColumn();
    rig.gw.state().theta_sigma[0] = std::numeric_limits<double>::quiet_NaN();
    auto r = trialRichardsBeds(rig.gw, 20, {{0, &c, -10, .5, 1}});
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(r.receipts.empty());
}

namespace {
/**
 * @brief Create a borrowed bed-owner view with uniform trial temperatures.
 *
 * @param[in] bed Bed index passed unchanged; caller ensures it identifies the intended contact.
 * @param[in,out] c Caller-owned column borrowed by pointer; must outlive use of the returned view.
 * @param[in] invert Owner invert elevation in metres.
 * @param[in] clogging Clogging factor passed unchanged to the acceptance trial.
 * @return Owner record with an owned 10 C temperature vector matching c.water length and a borrowed
 *     column pointer.
 */
RichardsBedOwner owner(int bed, Column& c, double invert, double clogging = 1) {
    return {bed, &c, invert, clogging, std::vector<double>(c.water.size(), 10)};
}
/**
 * @brief Sum live groundwater and owner-column water storage.
 *
 * @param[in] gw Borrowed initialized aquifer solver.
 * @param[in] columns Borrowed columns, summed in their existing order.
 * @return Total m3 including live aquifer storage and every column water entry.
 */
double ownedWater(const SubsurfaceSolver& gw, const std::vector<Column>& columns) {
    double total = gw.state().liveStorage();
    for (const auto& c : columns)
        total += columnWater(c);
    return total;
}
/**
 * @brief Assert selected aquifer water state and ledgers were preserved.
 *
 * Uses nonfatal exact vector/double assertions on hg, hu, sigma and exchange accumulators plus
 * node/ET/deep ledgers. This is a selected-field immutability check rather than a full
 * SubsurfaceState comparison.
 *
 * @param[in] gw Borrowed solver containing the candidate state.
 * @param[in] before Borrowed baseline state with expected arrays and ledger values.
 */
void unchanged(const SubsurfaceSolver& gw, const SubsurfaceState& before) {
    const auto& s = gw.state();
    EXPECT_EQ(s.hg, before.hg);
    EXPECT_EQ(s.hu, before.hu);
    EXPECT_EQ(s.theta_sigma, before.theta_sigma);
    EXPECT_EQ(s.nacc, before.nacc);
    EXPECT_EQ(s.lacc, before.lacc);
    EXPECT_EQ(s.eacc_L, before.eacc_L);
    EXPECT_EQ(s.eacc_R, before.eacc_R);
    EXPECT_EQ(s.xacc_from_surface, before.xacc_from_surface);
    EXPECT_EQ(s.xacc_to_surface, before.xacc_to_surface);
    EXPECT_DOUBLE_EQ(s.led_node, before.led_node);
    EXPECT_DOUBLE_EQ(s.led_et, before.led_et);
    EXPECT_DOUBLE_EQ(s.led_deep, before.led_deep);
}
}
TEST(RichardsAquiferAcceptance,
     OppositeDirectionsPublishActualClosureAndBothOwnersAcrossAllClosures) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA}) {
        NodeTrialRig rig(2, closure);
        SurfaceStateData surf;
        surf.resize(1, 0);
        std::vector<Column> columns(2, trialColumn());
        const double initial = ownedWater(rig.gw, columns);
        double down = 0, up = 0;
        for (int step = 0; step < 5; ++step) {
            auto accepted =
                acceptRichardsBedsAtFiring(rig.gw, surf, step, step + 1,
                                           {owner(0, columns[0], 10), owner(1, columns[1], -10)});
            ASSERT_TRUE(accepted.error.empty()) << accepted.error;
            ASSERT_EQ(accepted.trial.receipts.size(), 2u);
            for (std::size_t n = 0; n < 2; ++n) {
                const auto& receipt = accepted.trial.receipts[n];
                EXPECT_EQ(columns[n].water, accepted.trial.columns[n].water);
                EXPECT_EQ(columns[n].bottom,
                          openswmm::richards::Bottom::
                              Sealed); // publish water, not ephemeral quotas/boundary
                EXPECT_TRUE(std::isinf(columns[n].bottom_out_limit));
                down += receipt.accepted_down;
                up += receipt.accepted_up;
            }
            EXPECT_NEAR(accepted.balance, 0, 1e-10);
            EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), step + 1);
            EXPECT_EQ(rig.gw.nodeRefunds(), 0);
            EXPECT_EQ(rig.gw.state().nacc, std::vector<double>(2, 0));
        }
        EXPECT_GT(down, 0);
        EXPECT_GT(up, 0);
        EXPECT_NEAR(std::accumulate(rig.gw.richardsBedDownCumulative().begin(),
                                    rig.gw.richardsBedDownCumulative().end(), 0.),
                    down, 1e-14);
        EXPECT_NEAR(std::accumulate(rig.gw.richardsBedUpCumulative().begin(),
                                    rig.gw.richardsBedUpCumulative().end(), 0.),
                    up, 1e-14);
        EXPECT_NEAR(rig.gw.state().led_node, up - down, 1e-12);
        EXPECT_EQ(rig.gw.nodeExchangeVolumes(), std::vector<double>(2, 0));
        EXPECT_NEAR(initial - ownedWater(rig.gw, columns) - rig.gw.state().led_deep -
                        rig.gw.state().led_et,
                    0, 1e-10);
    }
}
TEST(RichardsAquiferAcceptance, ExhaustedSharedDonorSettlesWithoutUnilateralRefunds) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA}) {
        NodeTrialRig rig(5, closure, 1e-5);
        SurfaceStateData surf;
        surf.resize(1, 0);
        std::vector<Column> columns(5, trialColumn(4, .5));
        std::vector<RichardsBedOwner> owners;
        for (int n = 0; n < 5; ++n)
            owners.push_back(owner(n, columns[n], -10));
        const double initial = ownedWater(rig.gw, columns);
        auto accepted = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, owners);
        ASSERT_TRUE(accepted.error.empty()) << accepted.error;
        double paid = 0;
        int capped = 0;
        for (std::size_t n = 0; n < 5; ++n) {
            paid += accepted.trial.receipts[n].accepted_up;
            capped += accepted.trial.reports[n].bottom_in_limited;
        }
        EXPECT_GT(paid, 0);
        EXPECT_GT(capped, 0);
        EXPECT_LE(paid, accepted.trial.budgets[0].second.donor);
        EXPECT_EQ(rig.gw.nodeRefunds(), 0);
        EXPECT_NEAR(initial - ownedWater(rig.gw, columns) - rig.gw.state().led_deep -
                        rig.gw.state().led_et,
                    0, 1e-10);
    }
}
TEST(RichardsAquiferAcceptance, MixedVectorContactsSettleOnceAndArePermutationIndependent) {
    NodeTrialRig a(131), b(131);
    SurfaceStateData sa, sb;
    sa.resize(1, 0);
    sb = sa;
    a.gw.state().hu[0] = b.gw.state().hu[0] = .9 - 1e-6;
    std::vector<Column> ca;
    for (int n = 0; n < 131; ++n)
        ca.push_back(trialColumn(4 * (1 + n % 2)));
    auto cb = ca;
    std::vector<RichardsBedOwner> oa, ob;
    for (int n = 0; n < 131; ++n) {
        oa.push_back(owner(n, ca[n], 10));
        ob.push_back(owner(n, cb[n], 10));
    }
    std::reverse(ob.begin(), ob.end());
    const double initial = ownedWater(a.gw, ca);
    auto x = acceptRichardsBedsAtFiring(a.gw, sa, 0, 20, oa),
         y = acceptRichardsBedsAtFiring(b.gw, sb, 0, 20, ob);
    ASSERT_TRUE(x.error.empty()) << x.error;
    ASSERT_TRUE(y.error.empty()) << y.error;
    for (int n = 0; n < 131; ++n) {
        EXPECT_EQ(ca[n].water, cb[n].water);
        EXPECT_DOUBLE_EQ(x.trial.receipts[n].accepted_down, y.trial.receipts[n].accepted_down);
    }
    EXPECT_EQ(a.gw.state().hg, b.gw.state().hg);
    EXPECT_EQ(a.gw.state().hu, b.gw.state().hu);
    EXPECT_EQ(a.gw.richardsBedDownCumulative(), b.gw.richardsBedDownCumulative());
    EXPECT_NEAR(initial - ownedWater(a.gw, ca) - a.gw.state().led_deep - a.gw.state().led_et, 0,
                1e-10);
}
TEST(RichardsAquiferAcceptance, FinalHydraulicBoundsRejectInternalRedistributionWithoutPublishing) {
    NodeTrialRig rig;
    SurfaceStateData surf;
    surf.resize(1, 0);
    surf.infil_rate[0] = .023;
    auto c = trialColumn(4, .5);
    const auto water = c.water;
    auto proposed = trialRichardsBeds(rig.gw, 20, {{0, &c, 10, 10, 1}});
    ASSERT_TRUE(proposed.error.empty()) << proposed.error;
    ASSERT_GT(proposed.columns[0].water[1], c.water[1]);
    auto o = owner(0, c, 10);
    o.max_water[1] = c.water[1];
    const auto snapshot = rig.gw.state();
    auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {o});
    EXPECT_NE(failed.error.find("final hydraulic"), std::string::npos);
    unchanged(rig.gw, snapshot);
    EXPECT_EQ(c.water, water);
    EXPECT_DOUBLE_EQ(surf.infil_rate[0], .023);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
    EXPECT_DOUBLE_EQ(rig.gw.richardsBedDownCumulative()[0], 0);
}
TEST(RichardsAquiferAcceptance, RefundingAquiferTrialLeavesAllOwnersAndPendingClaimsUnchanged) {
    NodeTrialRig rig(2);
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn();
    rig.gw.state().nacc[1] = 10;
    rig.gw.assignTiers(20, 1);
    auto witness = rig.gw;
    auto private_surface = surf;
    witness.fireGwCells(0, 20, private_surface);
    ASSERT_GT(witness.nodeRefunds(), 0);
    const auto snapshot = rig.gw.state();
    const auto water = c.water;
    auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {owner(0, c, 10, 0)});
    EXPECT_NE(failed.error.find("matching Richards receipt"), std::string::npos);
    unchanged(rig.gw, snapshot);
    EXPECT_EQ(c.water, water);
    EXPECT_EQ(rig.gw.nodeRefunds(), 0);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
}
TEST(RichardsAquiferAcceptance, InvalidLastLaneAndUnpairedLegacyReceiptPreserveTheWholeBatch) {
    NodeTrialRig rig(2);
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto a = trialColumn(), b = a;
    const auto water = a.water;
    auto oa = owner(0, a, 10), ob = owner(1, b, -10);
    ob.max_water.pop_back();
    auto snapshot = rig.gw.state();
    auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {oa, ob});
    EXPECT_FALSE(failed.error.empty());
    unchanged(rig.gw, snapshot);
    EXPECT_EQ(a.water, water);
    EXPECT_EQ(b.water, water);
    ob = owner(1, b, -10);
    b.cells.back().material.specific_storage = 0;
    failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {oa, ob});
    EXPECT_FALSE(failed.error.empty());
    unchanged(rig.gw, snapshot);
    rig.gw.state().nacc[0] = .01;
    snapshot = rig.gw.state();
    failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {oa});
    EXPECT_NE(failed.error.find("unpaired"), std::string::npos);
    unchanged(rig.gw, snapshot);
    EXPECT_EQ(a.water, water);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
}
TEST(RichardsAquiferAcceptance, OwnerClockRejectsReplayGapsAndMissingSharedBeds) {
    NodeTrialRig rig(2);
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto a = trialColumn(), b = a;
    auto accepted =
        acceptRichardsBedsAtFiring(rig.gw, surf, 0, 1, {owner(0, a, 10, 0), owner(1, b, -10, 0)});
    ASSERT_TRUE(accepted.error.empty()) << accepted.error;
    const auto snapshot = rig.gw.state();
    const auto water = a.water;
    for (const auto times : {std::pair{0., 1.}, std::pair{2., 3.}}) {
        auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, times.first, times.second,
                                                 {owner(0, a, 10, 0), owner(1, b, -10, 0)});
        EXPECT_FALSE(failed.error.empty());
        unchanged(rig.gw, snapshot);
        EXPECT_EQ(a.water, water);
    }
    auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, 1, 2, {owner(0, a, 10, 0)});
    EXPECT_NE(failed.error.find("omitted"), std::string::npos);
    unchanged(rig.gw, snapshot);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 1);
    accepted =
        acceptRichardsBedsAtFiring(rig.gw, surf, 1, 2, {owner(1, b, -10, 0), owner(0, a, 10, 0)});
    EXPECT_TRUE(accepted.error.empty()) << accepted.error;
}
TEST(RichardsAquiferAcceptance,
     DifferentCellClocksRemainIndependentAndGenericChannelsExcludePorousOwners) {
    NodeTrialRig rig(3, GwClosure::CLOSED_FORM, 2, 1, 40, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    auto a = trialColumn(), b = a;
    const double other_h = rig.gw.state().hg[1];
    auto x = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 10, {owner(0, a, 10, 0)});
    ASSERT_TRUE(x.error.empty()) << x.error;
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 0);
    EXPECT_DOUBLE_EQ(rig.gw.state().hg[1], other_h);
    auto y = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {owner(1, b, -10, 0)});
    ASSERT_TRUE(y.error.empty()) << y.error;
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 10);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 20);
    openswmm::NodeData nodes;
    nodes.resize(3);
    nodes.invert_elev.assign(3, -10);
    nodes.depth.assign(3, 0);
    nodes.volume.assign(3, 1);
    rig.gw.sampleNodeExchange(&nodes, 1);
    EXPECT_DOUBLE_EQ(rig.gw.nodeExchangeVolumes()[0], 0);
    EXPECT_DOUBLE_EQ(rig.gw.nodeExchangeVolumes()[1], 0);
    EXPECT_GT(rig.gw.nodeExchangeVolumes()[2], 0);
    rig.gw.assignTiers(20, 1);
    const auto snapshot = rig.gw.state();
    EXPECT_THROW(rig.gw.fireGwCells(0, 20, surf, 20), std::runtime_error);
    unchanged(rig.gw, snapshot);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 10);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 20);
}
TEST(RichardsAquiferAcceptance,
     JointCheckpointGuardsCannotSilentlyLoseOwnershipOrRestoreUnknownClocks) {
    using openswmm::detail::CompletedSourceArchive;
    NodeTrialRig rig;
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn();
    rig.gw.assignTiers(20, 1);
    CompletedSourceArchive old;
    rig.gw.checkpoint(old, false);
    auto restored = rig.gw;
    CompletedSourceArchive read(old.bytes());
    restored.checkpoint(read, false);
    read.finish();
    auto rejected = acceptRichardsBedsAtFiring(restored, surf, 0, 1, {owner(0, c, 10, 0)});
    EXPECT_NE(rejected.error.find("restart extension"), std::string::npos);
    auto accepted = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 1, {owner(0, c, 10, 0)});
    ASSERT_TRUE(accepted.error.empty()) << accepted.error;
    for (bool model : {false, true}) {
        CompletedSourceArchive blocked;
        EXPECT_THROW(rig.gw.checkpoint(blocked, model), std::runtime_error);
        EXPECT_TRUE(blocked.bytes().empty());
    }
}
TEST(RichardsAquiferAcceptance, CoupledOwnerIntervalRefinementConvergesWithClosedWaterBalance) {
    const auto run = [](double dt) {
        NodeTrialRig rig(1, GwClosure::CLOSED_FORM, 2, 1, 1, 1e-6, .15);
        SurfaceStateData surf;
        surf.resize(1, 0);
        auto c = trialColumn();
        const double initial = rig.gw.state().liveStorage() + columnWater(c);
        for (int n = 0; n < int(4 / dt); ++n) {
            auto r =
                acceptRichardsBedsAtFiring(rig.gw, surf, n * dt, (n + 1) * dt, {owner(0, c, -2.5)});
            if (!r.error.empty())
                throw std::runtime_error(r.error);
        }
        EXPECT_NEAR(initial - rig.gw.state().liveStorage() - columnWater(c) -
                        rig.gw.state().led_deep - rig.gw.state().led_et,
                    0, 1e-10);
        c.water.push_back(rig.gw.state().hg[0]);
        return c.water;
    };
    const auto reference = run(.125), coarse = run(1), fine = run(.5), finer = run(.25);
    const auto error = [&](const auto& values) {
        double sum = 0;
        for (std::size_t n = 0; n < values.size(); ++n)
            sum += std::abs(values[n] - reference[n]);
        return sum;
    };
    EXPECT_LT(error(fine), error(coarse));
    EXPECT_LT(error(finer), error(fine));
}
TEST(RichardsAquiferAcceptance, PendingOtherChannelsAndEtSettleOnTheirActualWaterAcrossClosures) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA}) {
        NodeTrialRig rig(3, closure, 2, 1, 1, 1e-5, .15, 1, "BOTH");
        SurfaceStateData surf;
        surf.resize(1, 0);
        std::vector<Column> columns(2, trialColumn());
        for (auto& c : columns)
            for (auto& cell : c.cells)
                cell.potential_et = 1e-7;
        rig.gw.bookInfiltrationFromSurface(0, .003);
        rig.gw.bookLinkSeepage(0, .002);
        rig.gw.state().nacc[2] = .001;
        rig.gw.bookAreaEt(0, 2, .00004, 0, .00004);
        const double initial = ownedWater(rig.gw, columns);
        auto accepted = acceptRichardsBedsAtFiring(
            rig.gw, surf, 0, 2, {owner(0, columns[0], 10), owner(1, columns[1], -10)});
        ASSERT_TRUE(accepted.error.empty()) << accepted.error;
        double lid_et = 0;
        for (const auto& r : accepted.trial.reports)
            lid_et += r.evaporation;
        EXPECT_GT(lid_et, 0);
        EXPECT_GT(rig.gw.state().led_et, 0);
        EXPECT_EQ(rig.gw.nodeRefunds(), 0);
        EXPECT_DOUBLE_EQ(rig.gw.state().lacc[0], 0);
        EXPECT_DOUBLE_EQ(rig.gw.state().xacc_from_surface[0], 0);
        EXPECT_DOUBLE_EQ(rig.gw.state().et_pending[0], 0);
        EXPECT_NEAR(initial - ownedWater(rig.gw, columns) - .001 - rig.gw.state().led_deep -
                        rig.gw.state().led_et - lid_et,
                    0, 1e-10);
    }
}
TEST(RichardsAquiferAcceptance, UnpairedSourceIntakesAndLiveDriversAreExplicitlyUnqualified) {
    NodeTrialRig rig;
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn();
    ASSERT_TRUE(rig.gw.configureSourceReceipts({0}).empty());
    rig.gw.bookInfiltrationFromSurface(0, .001);
    const auto snapshot = rig.gw.state();
    auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 1, {owner(0, c, 10)});
    EXPECT_NE(failed.error.find("Unpaired source water"), std::string::npos);
    unchanged(rig.gw, snapshot);
    NodeTrialRig live;
    live.gw.runtimeSources().rows.resize(1);
    failed = acceptRichardsBedsAtFiring(live.gw, surf, 0, 1, {owner(0, c, 10)});
    EXPECT_NE(failed.error.find("live drivers"), std::string::npos);
    EXPECT_DOUBLE_EQ(live.gw.cellCompletedTime(0), 0);
}
TEST(RichardsAquiferAcceptance, LaterCellRefundRollsBackEarlierPrivateFiringAndOwnership) {
    using openswmm::detail::CompletedSourceArchive;
    NodeTrialRig rig(4, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    rig.gw.publishInfiltration(surf, 20, 0);
    rig.gw.state().nacc[3] = 10;
    rig.gw.assignTiers(20, 1);
    CompletedSourceArchive before;
    rig.gw.checkpoint(before, false);
    const auto rates = surf.infil_rate;
    auto a = trialColumn(), b = a;
    const auto water = a.water;
    auto failed =
        acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {owner(0, a, 10, 0), owner(1, b, -10, 0)});
    EXPECT_NE(failed.error.find("matching Richards receipt"), std::string::npos);
    CompletedSourceArchive after;
    rig.gw.checkpoint(after, false);
    EXPECT_EQ(before.bytes(), after.bytes());
    EXPECT_EQ(a.water, water);
    EXPECT_EQ(b.water, water);
    EXPECT_EQ(surf.infil_rate, rates);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 0);
    EXPECT_EQ(rig.gw.richardsBedDownCumulative(), std::vector<double>(4, 0));
}
TEST(RichardsAquiferAcceptance, UnaccountedAquiferOverflowRejectsRatherThanHidingAContinuityGap) {
    NodeTrialRig rig(1, GwClosure::CLOSED_FORM, 4, 1, 1, 0, 0, 1, "NONE", false);
    SurfaceStateData surf;
    surf.resize(1, 0);
    // Saturated-zone inflow drives Dunne excess; top infiltration instead
    // reaches column rejection, which is correctly returned even with Dunne off.
    rig.gw.bookLinkSeepage(0, 1);
    const auto snapshot = rig.gw.state();
    auto c = trialColumn();
    const auto water = c.water;
    auto failed = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 20, {owner(0, c, 10, 0)});
    EXPECT_NE(failed.error.find("water balance"), std::string::npos);
    unchanged(rig.gw, snapshot);
    EXPECT_EQ(c.water, water);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
}

TEST(RichardsAquiferTier, NaturalTiersKeepIndependentClocksAndSpendOnlyDuePendingWater) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    rig.gw.state().dt_cell = {1, 2};
    rig.gw.assignTiers(1, 2);
    ASSERT_EQ(rig.gw.state().tier[0], 0);
    ASSERT_EQ(rig.gw.state().tier[1], 1);
    auto a = trialColumn(), b = a;
    rig.gw.bookLinkSeepage(1, .001);
    const auto slow = rig.gw.state().hg[1];
    auto fast = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(0, a, -2.5)});
    ASSERT_TRUE(fast.error.empty()) << fast.error;
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 1);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 0);
    EXPECT_DOUBLE_EQ(rig.gw.state().hg[1], slow);
    EXPECT_DOUBLE_EQ(rig.gw.state().lacc[1], .001);
    EXPECT_EQ(rig.gw.tierFirings(), (std::vector<long>{1, 0}));
    fast = acceptRichardsBedsForTier(rig.gw, surf, 0, 1, 2, {owner(0, a, -2.5)});
    ASSERT_TRUE(fast.error.empty()) << fast.error;
    auto coarse = acceptRichardsBedsForTier(rig.gw, surf, 1, 0, 2, {owner(1, b, -2.5)});
    ASSERT_TRUE(coarse.error.empty()) << coarse.error;
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 2);
    EXPECT_DOUBLE_EQ(rig.gw.state().lacc[1], 0);
    EXPECT_EQ(rig.gw.tierFirings(), (std::vector<long>{2, 1}));
    EXPECT_EQ(rig.gw.state().tier, (std::vector<uint8_t>{0, 1}));
}
TEST(RichardsAquiferTier, OrdinaryCellsOnTheSameTierFireOnceAlongsidePorousOwners) {
    NodeTrialRig rig(1, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2),
        reference(1, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf, expected;
    surf.resize(2, 0);
    expected.resize(2, 0);
    for (auto* gw : {&rig.gw, &reference.gw}) {
        gw->state().dt_cell = {1, 1};
        gw->assignTiers(1, 1);
        gw->bookLinkSeepage(1, .001);
    }
    auto c = trialColumn();
    auto accepted = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(0, c, -2.5)});
    ASSERT_TRUE(accepted.error.empty()) << accepted.error;
    reference.gw.fireGwCells(0, 1, expected, 0);
    EXPECT_DOUBLE_EQ(rig.gw.state().hg[1], reference.gw.state().hg[1]);
    EXPECT_DOUBLE_EQ(rig.gw.state().hu[1], reference.gw.state().hu[1]);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 1);
    EXPECT_DOUBLE_EQ(rig.gw.state().lacc[1], 0);
    EXPECT_EQ(rig.gw.tierFirings(), std::vector<long>{1});
    EXPECT_NEAR(accepted.balance, 0, 1e-10);
}
TEST(RichardsAquiferTier, LateOrdinaryRefundRollsBackColumnsTierAndAllWaterOwners) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    rig.gw.state().dt_cell = {1, 1};
    rig.gw.assignTiers(1, 1);
    rig.gw.state().nacc[1] = 10;
    auto c = trialColumn();
    const auto water = c.water;
    openswmm::detail::CompletedSourceArchive before;
    rig.gw.checkpoint(before, false);
    auto refused = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(0, c, -2.5)});
    EXPECT_NE(refused.error.find("refunded"), std::string::npos);
    EXPECT_EQ(c.water, water);
    openswmm::detail::CompletedSourceArchive after;
    rig.gw.checkpoint(after, false);
    EXPECT_EQ(before.bytes(), after.bytes());
    EXPECT_EQ(rig.gw.tierFirings(), std::vector<long>{0});
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
    rig.gw.state().nacc[1] = 0;
    refused = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(0, c, -2.5)});
    EXPECT_TRUE(refused.error.empty()) << refused.error;
}
TEST(RichardsAquiferTier, WrongTierAliasedColumnsAndMissingBoundOwnerCannotAdvance) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    rig.gw.state().dt_cell = {1, 2};
    rig.gw.assignTiers(1, 2);
    auto a = trialColumn(), b = a;
    const auto water = a.water;
    auto bad = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(1, b, -2.5)});
    EXPECT_NE(bad.error.find("outside"), std::string::npos);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
    EXPECT_EQ(a.water, water);
    auto good = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(0, a, -2.5)});
    ASSERT_TRUE(good.error.empty()) << good.error;
    bad = acceptRichardsBedsForTier(rig.gw, surf, 0, 1, 2, {});
    EXPECT_NE(bad.error.find("omitted"), std::string::npos);
    bad = acceptRichardsBedsForTier(rig.gw, surf, 0, 0, 1, {owner(0, a, -2.5)});
    EXPECT_NE(bad.error.find("clocks"), std::string::npos);
    NodeTrialRig shared(2);
    shared.gw.assignTiers(1, 1);
    surf.resize(1, 0);
    bad =
        acceptRichardsBedsForTier(shared.gw, surf, 0, 0, 1, {owner(0, b, -2.5), owner(1, b, -2.5)});
    EXPECT_NE(bad.error.find("duplicate"), std::string::npos);
    EXPECT_DOUBLE_EQ(shared.gw.cellCompletedTime(0), 0);
}
TEST(RichardsAquiferTier, DueTierWithoutPorousOwnersPreservesOrdinaryFiringResults) {
    NodeTrialRig a(1), b(1);
    SurfaceStateData x, y;
    x.resize(1, 0);
    y.resize(1, 0);
    a.gw.assignTiers(1, 1);
    b.gw.assignTiers(1, 1);
    a.gw.bookLinkSeepage(0, .001);
    b.gw.bookLinkSeepage(0, .001);
    auto accepted = acceptRichardsBedsForTier(a.gw, x, 0, 0, 1, {});
    ASSERT_TRUE(accepted.error.empty()) << accepted.error;
    b.gw.fireGwCells(0, 1, y, 0);
    EXPECT_EQ(a.gw.state().hg, b.gw.state().hg);
    EXPECT_EQ(a.gw.state().hu, b.gw.state().hu);
    EXPECT_EQ(a.gw.tierFirings(), b.gw.tierFirings());
    EXPECT_TRUE(accepted.trial.receipts.empty());
}
TEST(RichardsAquiferAcceptance, InconsistentEnslavedStorageCannotEnterAConservativeTransaction) {
    NodeTrialRig rig(1, GwClosure::ENSLAVED);
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn();
    const double equilibrium = rig.gw.state().hu[0];
    rig.gw.state().hu[0] -= .001;
    const auto state = rig.gw.state();
    const auto water = c.water;
    auto refused = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 1, {owner(0, c, -2.5)});
    EXPECT_NE(refused.error.find("actual equilibrium storage"), std::string::npos);
    unchanged(rig.gw, state);
    EXPECT_EQ(c.water, water);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
    rig.gw.state().hu[0] = equilibrium;
    auto accepted = acceptRichardsBedsAtFiring(rig.gw, surf, 0, 1, {owner(0, c, -2.5)});
    EXPECT_TRUE(accepted.error.empty()) << accepted.error;
}

TEST(RichardsPendingSteps, OppositeTransfersForcingEtAndOtherClaimsCloseAcrossAllClosures) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA}) {
        NodeTrialRig rig(3, closure, 2, 1, 1, 1e-5, .15, 1, "BOTH");
        SurfaceStateData surf;
        surf.resize(1, 0);
        std::vector<Column> columns(2, trialColumn());
        for (auto& c : columns) {
            c.inflow = 1e-5;
            for (auto& cell : c.cells)
                cell.potential_et = 1e-7;
        }
        rig.gw.bookInfiltrationFromSurface(0, .003);
        rig.gw.bookLinkSeepage(0, .002);
        rig.gw.state().nacc[2] = .001;
        rig.gw.bookAreaEt(0, 2, .00004, 0, .00004);
        const double initial = ownedWater(rig.gw, columns),
                     head = rig.gw.state().z_bed[0] + rig.gw.state().hg[0];
        auto r =
            acceptRichardsBedsAcrossSteps(rig.gw, surf, 0, 2, {.25, .5, 1, 2},
                                          {owner(0, columns[0], 10), owner(1, columns[1], -10)});
        ASSERT_TRUE(r.error.empty()) << r.error;
        ASSERT_EQ(r.steps.size(), 4u);
        double down = 0, up = 0, in = 0, et = 0;
        for (std::size_t k = 0; k < r.steps.size(); ++k) {
            EXPECT_DOUBLE_EQ(r.steps[k].start, k ? r.steps[k - 1].end : 0);
            for (const auto& receipt : r.steps[k].receipts) {
                EXPECT_DOUBLE_EQ(receipt.aquifer_head, head);
                down += receipt.accepted_down;
                up += receipt.accepted_up;
            }
        }
        for (const auto& report : r.trial.reports) {
            in += report.inflow;
            et += report.evaporation;
        }
        EXPECT_GT(down, 0);
        EXPECT_GT(up, 0);
        EXPECT_GT(et, 0);
        EXPECT_NEAR(initial + in - ownedWater(rig.gw, columns) - .001 - et -
                        rig.gw.state().led_deep - rig.gw.state().led_et,
                    0, 1e-10);
        EXPECT_NEAR(down,
                    rig.gw.richardsBedDownCumulative()[0] + rig.gw.richardsBedDownCumulative()[1],
                    1e-14);
        EXPECT_NEAR(up, rig.gw.richardsBedUpCumulative()[0] + rig.gw.richardsBedUpCumulative()[1],
                    1e-14);
        EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 2);
        EXPECT_EQ(rig.gw.nodeExchangeVolumes(), std::vector<double>(3, 0));
    }
}
TEST(RichardsPendingSteps, PackedColumnsShareTheWholeWindowReceiverWithoutRenewingIt) {
    NodeTrialRig a(65), b(65);
    SurfaceStateData sa, sb;
    sa.resize(1, 0);
    sb.resize(1, 0);
    a.gw.state().hu[0] = b.gw.state().hu[0] = .45 * 2 - 1e-6;
    std::vector<Column> ca;
    for (int n = 0; n < 65; ++n)
        ca.push_back(trialColumn(4 * (1 + n % 2)));
    auto cb = ca;
    std::vector<RichardsBedOwner> oa, ob;
    for (int n = 0; n < 65; ++n) {
        oa.push_back(owner(n, ca[n], 10));
        ob.push_back(owner(n, cb[n], 10));
    }
    std::reverse(ob.begin(), ob.end());
    const double initial = ownedWater(a.gw, ca);
    auto x = acceptRichardsBedsAcrossSteps(a.gw, sa, 0, 20, {5, 10, 20}, oa),
         y = acceptRichardsBedsAcrossSteps(b.gw, sb, 0, 20, {5, 10, 20}, ob);
    ASSERT_TRUE(x.error.empty()) << x.error;
    ASSERT_TRUE(y.error.empty()) << y.error;
    long double spent = 0;
    const double ceiling = x.trial.budgets[0].second.receiver;
    // The same shared ceiling applies to every prefix of completed steps;
    // one double ulp covers rounding across contacts and step boundaries.
    for (const auto& step : x.steps) {
        for (const auto& r : step.receipts)
            spent += r.accepted_down;
        EXPECT_LE(spent, std::nextafter(ceiling, std::numeric_limits<double>::infinity()));
    }
    EXPECT_GT(spent, 0);
    EXPECT_NEAR(initial - ownedWater(a.gw, ca) - a.gw.state().led_et - a.gw.state().led_deep, 0,
                1e-10);
    for (int n = 0; n < 65; ++n) {
        EXPECT_EQ(ca[n].water, cb[n].water);
        EXPECT_DOUBLE_EQ(x.trial.receipts[n].accepted_down, y.trial.receipts[n].accepted_down);
    }
    EXPECT_EQ(a.gw.state().hg, b.gw.state().hg);
    EXPECT_EQ(a.gw.richardsBedDownCumulative(), b.gw.richardsBedDownCumulative());
}
TEST(RichardsPendingSteps, PendingIntakesAndEarlierDownwardStepsNeverFundUpwardAwards) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA}) {
        NodeTrialRig rig(2, closure, 0);
        SurfaceStateData surf;
        surf.resize(1, 0);
        rig.gw.bookInfiltrationFromSurface(0, .02);
        auto a = trialColumn(), b = a;
        auto r = acceptRichardsBedsAcrossSteps(rig.gw, surf, 0, 3, {1, 2, 3},
                                               {owner(0, a, 10), owner(1, b, -10)});
        ASSERT_TRUE(r.error.empty()) << r.error;
        double down = 0;
        for (const auto& step : r.steps)
            for (const auto& receipt : step.receipts) {
                down += receipt.accepted_down;
                EXPECT_DOUBLE_EQ(receipt.accepted_up, 0);
            }
        EXPECT_GT(down, 0);
        EXPECT_GT(rig.gw.state().hg[0], 0);
        EXPECT_EQ(rig.gw.richardsBedUpCumulative(), std::vector<double>(2, 0));
    }
}
TEST(RichardsPendingSteps, FutureSurfaceSupplyCannotRenewTheInitialPorousDonor) {
    NodeTrialRig rig;
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn();
    c.inflow = .2;
    // Forecast ET reserves the entire initial porous donor. New surface supply
    // may feed storage/ET, but cannot renew that window's bottom donor quota.
    for (std::size_t k = 1; k < c.cells.size(); ++k)
        c.cells[k].potential_et = .03;
    const double initial = columnWater(c);
    auto r = acceptRichardsBedsAcrossSteps(rig.gw, surf, 0, 3, {1, 2, 3}, {owner(0, c, 10)});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_GT(columnWater(c), initial);
    for (const auto& step : r.steps)
        EXPECT_DOUBLE_EQ(step.receipts[0].accepted_down, 0);
    EXPECT_DOUBLE_EQ(rig.gw.richardsBedDownCumulative()[0], 0);
    EXPECT_NEAR(r.trial.reports[0].inflow, .6, 1e-14);
}
TEST(RichardsPendingSteps, InvalidCoverageAndAnIntermediateHydraulicFailurePublishNothing) {
    NodeTrialRig rig;
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn(4, .5);
    const auto water = c.water;
    const auto before = rig.gw.state();
    for (const auto& ends :
         std::vector<std::vector<double>>{{},
                                          {1},
                                          {1, 1, 2},
                                          {-1, 2},
                                          {3, 2},
                                          {1, std::numeric_limits<double>::quiet_NaN(), 2}}) {
        auto r = acceptRichardsBedsAcrossSteps(rig.gw, surf, 0, 2, ends, {owner(0, c, 10)});
        EXPECT_FALSE(r.error.empty());
        EXPECT_TRUE(r.steps.empty());
        unchanged(rig.gw, before);
        EXPECT_EQ(c.water, water);
    }
    auto o = owner(0, c, 10);
    o.max_water[1] = c.water[1];
    auto bad = acceptRichardsBedsAcrossSteps(rig.gw, surf, 0, 20, {5, 10, 20}, {o});
    EXPECT_NE(bad.error.find("hydraulic"), std::string::npos);
    unchanged(rig.gw, before);
    EXPECT_EQ(c.water, water);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(0), 0);
}
TEST(RichardsPendingSteps, PrivatePackingCannotHideAliasedOriginalColumns) {
    NodeTrialRig rig(2);
    SurfaceStateData surf;
    surf.resize(1, 0);
    auto c = trialColumn();
    const auto water = c.water;
    const auto before = rig.gw.state();
    auto bad = acceptRichardsBedsAcrossSteps(rig.gw, surf, 0, 2, {1, 2},
                                             {owner(0, c, 10), owner(1, c, -10)});
    EXPECT_NE(bad.error.find("duplicate"), std::string::npos);
    EXPECT_TRUE(bad.steps.empty());
    unchanged(rig.gw, before);
    EXPECT_EQ(c.water, water);
}
TEST(RichardsPendingSteps, LateOrdinaryRefundDiscardsAllStagesAndAllowsCorrectedRetry) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    rig.gw.state().dt_cell = {2, 2};
    rig.gw.assignTiers(2, 1);
    rig.gw.state().nacc[1] = 10;
    auto c = trialColumn();
    const auto water = c.water;
    openswmm::detail::CompletedSourceArchive before;
    rig.gw.checkpoint(before, false);
    auto bad = acceptRichardsBedsForTierAcrossSteps(rig.gw, surf, 0, 0, 2, {.5, 1, 2},
                                                    {owner(0, c, -2.5)});
    EXPECT_NE(bad.error.find("refunded"), std::string::npos);
    EXPECT_TRUE(bad.steps.empty());
    EXPECT_EQ(c.water, water);
    openswmm::detail::CompletedSourceArchive after;
    rig.gw.checkpoint(after, false);
    EXPECT_EQ(before.bytes(), after.bytes());
    EXPECT_EQ(rig.gw.tierFirings(), std::vector<long>{0});
    rig.gw.state().nacc[1] = 0;
    auto good = acceptRichardsBedsForTierAcrossSteps(rig.gw, surf, 0, 0, 2, {.5, 1, 2},
                                                     {owner(0, c, -2.5)});
    ASSERT_TRUE(good.error.empty()) << good.error;
    EXPECT_EQ(rig.gw.tierFirings(), std::vector<long>{1});
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 2);
}
TEST(RichardsPendingSteps, ShortColumnStepsPreserveNaturalFastAndSlowGroundwaterFirings) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surf;
    surf.resize(2, 0);
    rig.gw.state().dt_cell = {1, 2};
    rig.gw.assignTiers(1, 2);
    rig.gw.bookLinkSeepage(1, .001);
    auto a = trialColumn(), b = a;
    const auto slow = b.water;
    const double hg = rig.gw.state().hg[1];
    auto first = acceptRichardsBedsForTierAcrossSteps(rig.gw, surf, 0, 0, 1, {.25, .5, .75, 1},
                                                      {owner(0, a, -2.5)});
    ASSERT_TRUE(first.error.empty()) << first.error;
    EXPECT_EQ(b.water, slow);
    EXPECT_DOUBLE_EQ(rig.gw.state().hg[1], hg);
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 0);
    EXPECT_DOUBLE_EQ(rig.gw.state().lacc[1], .001);
    auto second = acceptRichardsBedsForTierAcrossSteps(rig.gw, surf, 0, 1, 2, {1.25, 1.5, 1.75, 2},
                                                       {owner(0, a, -2.5)});
    ASSERT_TRUE(second.error.empty()) << second.error;
    auto last = acceptRichardsBedsForTierAcrossSteps(
        rig.gw, surf, 1, 0, 2, {.25, .5, .75, 1, 1.25, 1.5, 1.75, 2}, {owner(1, b, -2.5)});
    ASSERT_TRUE(last.error.empty()) << last.error;
    EXPECT_EQ(last.steps.size(), 8u);
    EXPECT_EQ(rig.gw.tierFirings(), (std::vector<long>{2, 1}));
    EXPECT_EQ(rig.gw.state().tier, (std::vector<uint8_t>{0, 1}));
    EXPECT_DOUBLE_EQ(rig.gw.cellCompletedTime(1), 2);
}

namespace {
std::string bedState(SubsurfaceSolver& gw) {
    openswmm::detail::CompletedSourceArchive archive;
    gw.checkpoint(archive, false);
    return archive.bytes();
}

void saveBedState(const std::string& name, SubsurfaceSolver& gw) {
    const auto directory = kOutDir / "bed_lookup";
    fs::create_directories(directory);
    const auto bytes = bedState(gw);
    std::ofstream(directory / (name + ".chk"), std::ios::binary).write(bytes.data(), bytes.size());
}
}

TEST(AquiferBedLookup, InterleavedInactiveAndEmptyCellBedsSurviveRestoreAndRepeatedFirings) {
    for (bool species : {false, true}) {
        SCOPED_TRACE(species);
        NodeTrialRig rig(12, GwClosure::CLOSED_FORM, 2, 10, .1, 1e-5, .1, 5);
        // Cell 4 has no active beds; other cells have nonadjacent bed IDs.
        rig.config.node_beds[4].exchange = false;
        rig.config.node_beds[9].cell = -1;
        std::vector<std::string> warnings;
        ASSERT_EQ(rig.gw.initialize(rig.mesh, rig.edges, rig.options, {}, 12, rig.config,
                                    warnings), "");
        std::vector<double> concentrations(24, 3);
        if (species) {
            RowLayoutLite rows;
            rows.n_species = rows.n_pollut = 2;
            rows.names = {"A", "B"};
            rig.gw.initTransport(rows, nullptr, {}, warnings);
            auto& tr = rig.gw.transport();
            for (int s = 0; s < 2; ++s)
                for (int c = 0; c < 5; ++c) {
                    tr.sat_mass[tr.idx(s, c)] = 2 * rig.gw.satVolume(c);
                    tr.unsat_mass[tr.idx(s, c)] = 2 * rig.gw.unsatVolume(c);
                    tr.init_mass[s] += tr.sat_mass[tr.idx(s, c)] + tr.unsat_mass[tr.idx(s, c)];
                }
            rig.gw.setNodeRowConc(concentrations.data(), 2);
        }
        openswmm::NodeData nodes;
        nodes.resize(12);
        nodes.volume.assign(12, 100);
        for (int n = 0; n < 12; ++n)
            nodes.invert_elev[n] = n % 2 ? -1 : -3;
        SurfaceStateData surface;
        surface.resize(5, 0);
        rig.gw.state().dt_cell = {.25, .5, 1., .25, .5};
        rig.gw.assignTiers(.25, 3);
        rig.gw.sampleNodeExchange(&nodes, .25);
        rig.gw.fireGwCells(0, .25, surface);
        const auto checkpoint = bedState(rig.gw);
        saveBedState(species ? "species_partial" : "water_partial", rig.gw);
        auto reference = rig.gw;
        const auto saved_surface = surface;
        // Firing tier 1 ends on the bed-free cell, replacing the last gather.
        rig.gw.fireGwCells(1, .5, surface);
        openswmm::detail::CompletedSourceArchive restore(checkpoint);
        ASSERT_NO_THROW(rig.gw.checkpoint(restore, false));
        ASSERT_NO_THROW(restore.finish());
        EXPECT_EQ(bedState(rig.gw), checkpoint);
        surface = saved_surface;
        auto reference_surface = saved_surface;
        for (int tier : {2, 1, 0}) {
            rig.gw.fireGwCells(tier, .25 * (1 << tier), surface);
            reference.fireGwCells(tier, .25 * (1 << tier), reference_surface);
            EXPECT_EQ(bedState(rig.gw), bedState(reference));
        }
        for (double pending : rig.gw.state().nacc)
            EXPECT_DOUBLE_EQ(pending, 0);
        if (species) {
            const auto& tr = rig.gw.transport();
            for (int s = 0; s < 2; ++s) {
                const auto begin = tr.node_out_mass.begin() + s * 12;
                EXPECT_NEAR(std::accumulate(begin, begin + 12, 0.), tr.lost_node[s], 1e-12);
                EXPECT_DOUBLE_EQ(tr.node_out_mass[s * 12 + 4], 0);
                EXPECT_DOUBLE_EQ(tr.node_out_mass[s * 12 + 9], 0);
                EXPECT_GT(tr.gained_node[s], 0);
                EXPECT_GT(tr.lost_node[s], 0);
            }
        }
        saveBedState(species ? "species_complete" : "water_complete", rig.gw);
    }
}

TEST(AquiferBedLookup, PerformanceWorkload) {
    const char* mode = std::getenv("OPENSWMM_SOA_S1_BENCH");
    if (!mode)
        GTEST_SKIP() << "set OPENSWMM_SOA_S1_BENCH=dt or fire";
    const bool dt_only = std::string(mode) == "dt";
    ASSERT_TRUE(dt_only || std::string(mode) == "fire");
    int cells = dt_only ? 2048 : 1024, repeats = 64;
    if (const char* text = std::getenv("OPENSWMM_SOA_S1_REPEATS"))
        repeats = std::stoi(text);
    ASSERT_GT(repeats, 0);
    NodeTrialRig rig(4 * cells, GwClosure::CLOSED_FORM, 2, 10, .01, 1e-6, .1, cells);
    SurfaceStateData surface;
    surface.resize(cells, 0);
    rig.gw.assignTiers(.25, 1);
    openswmm::NodeData nodes;
    nodes.resize(4 * cells);
    nodes.invert_elev.assign(4 * cells, -3);
    nodes.volume.assign(4 * cells, 100);
    openswmm::perf::reset_process();
    for (int repeat = 0; repeat < repeats; ++repeat) {
        if (dt_only)
            rig.gw.refreshDtCell(rig.mesh, rig.edges);
        else {
            rig.gw.sampleNodeExchange(&nodes, .25);
            rig.gw.fireGwCells(0, .25, surface, repeat * .25);
        }
    }
    saveBedState(dt_only ? "bench_dt" : "bench_fire", rig.gw);
    if (openswmm::perf::enabled())
        openswmm::perf::dump_process();
}

TEST(AquiferBedLookup, RestoredNegativeZeroGatherIsClearedByTheNextCell) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 10, .1, 1e-5, .1, 2);
    SurfaceStateData surface;
    surface.resize(2, 0);
    rig.gw.state().dt_cell = {.25, .5};
    rig.gw.assignTiers(.25, 2);
    rig.gw.state().nacc[1] = -0.;
    rig.gw.fireGwCells(1, .5, surface);
    const auto checkpoint = bedState(rig.gw);
    saveBedState("negative_zero_partial", rig.gw);
    auto reference = rig.gw;
    rig.gw.fireGwCells(0, .25, surface);
    openswmm::detail::CompletedSourceArchive restore(checkpoint);
    ASSERT_NO_THROW(rig.gw.checkpoint(restore, false));
    ASSERT_NO_THROW(restore.finish());
    rig.gw.fireGwCells(0, .25, surface);
    reference.fireGwCells(0, .25, surface);
    EXPECT_EQ(bedState(rig.gw), bedState(reference));
    saveBedState("negative_zero_complete", rig.gw);
}

TEST(AquiferBedLookup, RetieringGrowShrinkPreservesOrderedCheckpointLists) {
    NodeTrialRig rig(12, GwClosure::CLOSED_FORM, 2, 10, .1, 1e-5, .1, 5);
    rig.gw.state().dt_cell = {.25, 4., 1., .5, 2.};
    int step = 0;
    for (int count : {1, 8, 2, 5, 1, 8}) {
        rig.gw.assignTiers(.25, count);
        EXPECT_EQ(rig.gw.tierCount(), count);
        EXPECT_EQ(rig.gw.tierFirings(), std::vector<long>(count, 0));
        std::vector<uint8_t> expected;
        for (int tier : {0, 4, 2, 1, 3})
            expected.push_back(static_cast<uint8_t>(std::min(tier, count - 1)));
        EXPECT_EQ(rig.gw.state().tier, expected);
        const auto saved = bedState(rig.gw);
        rig.gw.assignTiers(.125, 3);
        openswmm::detail::CompletedSourceArchive restore(saved);
        ASSERT_NO_THROW(rig.gw.checkpoint(restore, false));
        ASSERT_NO_THROW(restore.finish());
        EXPECT_EQ(bedState(rig.gw), saved);
        EXPECT_EQ(rig.gw.state().tier, expected);
        saveBedState("retier_grow_shrink_" + std::to_string(step++), rig.gw);
    }
}

TEST(AquiferClockTelemetry, HistogramsSeparateUnboundedLimitsAndUseExactBinEdges) {
    GwClockTelemetry profile;
    profile.reset(4, true);
    profile.bounds(0, 1, 16);
    profile.bounds(1, 4, 1);
    profile.bounds(2, 1e30, 1e30);
    profile.bounds(3, 1, 1e30);
    profile.rebuilt({1, 1, 0, 1}, {0, 2, 0, 1});
    EXPECT_EQ(profile.rebuilds, 1);
    EXPECT_EQ(profile.closures[0].zone_ratio[1], 1);
    EXPECT_EQ(profile.closures[2].zone_ratio[4], 1);
    EXPECT_EQ(profile.closures[1].u_unbounded, 1);
    EXPECT_EQ(profile.closures[0].both_unbounded, 0); // uncovered cells excluded
    profile.fired(0, 0, 0, .5, 1, GwFiringOrigin::macro);
    profile.fired(0, 0, .5, .25, 1, GwFiringOrigin::tail);
    profile.fired(1, 2, 0, .75, 1e30, GwFiringOrigin::direct);
    EXPECT_EQ(profile.closures[0].overfire[2], 2);
    EXPECT_EQ(profile.closures[2].unbounded, 1);
    EXPECT_DOUBLE_EQ(profile.closures[0].elapsed, .75);
    EXPECT_DOUBLE_EQ(profile.closures[0].max_elapsed, .5);
    EXPECT_EQ(profile.cells[0].firings, 2);
    EXPECT_EQ(profile.origins, (std::array<GwClockTelemetry::Count, 4>{1, 1, 1, 0}));
    EXPECT_DOUBLE_EQ(profile.end - profile.start, .75);
    profile.reset(4, false);
    profile.bounds(0, 1, 2);
    profile.rebuilt({1, 1, 0, 1}, {0, 2, 0, 1});
    profile.fired(0, 0, 0, 1, 2, GwFiringOrigin::tail);
    profile.batch(0);
    EXPECT_TRUE(profile.cells.empty());
    EXPECT_EQ(profile.rebuilds, 0);
    EXPECT_EQ(profile.closures[0].firings, 0);
    EXPECT_EQ(profile.tier_batches[0], 0);
}

TEST(AquiferClockTelemetry, CountsSurviveRetieringAndDoNotEnterCheckpointBytes) {
    NodeTrialRig rig(0, GwClosure::CLOSED_FORM, 2, 10, .1, 1e-5, .1, 2);
    SurfaceStateData surface;
    surface.resize(2, 0);
    rig.gw.state().dt_cell = {1., 4.};
    rig.gw.assignTiers(1, 3);
    rig.gw.fireGwCells(0, 1, surface, 0, -1, GwFiringOrigin::macro);
    rig.gw.fireGwCells(2, 4, surface, 0, -1, GwFiringOrigin::macro);
    rig.gw.fireGwCells(0, .25, surface, 4, -1, GwFiringOrigin::tail);
    const auto before = bedState(rig.gw);
    const auto& profile = rig.gw.clockTelemetry();
    if (openswmm::perf::enabled()) {
        EXPECT_EQ(profile.cells[0].firings, 2);
        EXPECT_EQ(profile.cells[1].firings, 1);
        EXPECT_EQ(profile.tier_batches[0], 2);
        EXPECT_EQ(profile.tier_batches[2], 1);
        EXPECT_EQ(profile.origins[static_cast<int>(GwFiringOrigin::tail)], 1);
    } else {
        EXPECT_FALSE(profile.active());
    }
    rig.gw.assignTiers(.5, 2);
    EXPECT_EQ(rig.gw.tierFirings(), (std::vector<long>{0, 0}));
    if (openswmm::perf::enabled()) {
        EXPECT_EQ(profile.cells[0].firings, 2);
        EXPECT_EQ(profile.tier_batches[2], 1);
    }
    openswmm::detail::CompletedSourceArchive restore(before);
    rig.gw.checkpoint(restore, false);
    restore.finish();
    EXPECT_EQ(bedState(rig.gw), before);
    saveBedState("clock_telemetry_checkpoint", rig.gw);
}

TEST(AquiferBedLookup, ParallelDtAndIntakeMatchSerialAcrossClosuresAndSoils) {
    constexpr int cells = 1027;
    const SoilChar laws[] = {SoilChar::GARDNER, SoilChar::RUSSO, SoilChar::BROOKS_COREY,
                             SoilChar::VAN_GENUCHTEN};
    for (int threads : {1, 4, 8}) {
        NodeTrialRig serial(3 * cells, GwClosure::SIGMA, 2, 10, .1, 1e-5, .1, cells),
            parallel(3 * cells, GwClosure::SIGMA, 2, 10, .1, 1e-5, .1, cells);
        parallel.options.num_threads = threads;
        SurfaceStateData a, b;
        a.resize(cells, 0);
        b.resize(cells, 0);
        for (auto* rig : {&serial, &parallel}) {
            auto& state = rig->gw.state();
            for (int i = 0; i < cells; ++i) {
                state.closure[i] = static_cast<int8_t>(i % 3 == 0 ? GwClosure::SIGMA
                                                      : i % 3 == 1 ? GwClosure::CLOSED_FORM
                                                                   : GwClosure::ENSLAVED);
                state.soil_char[i] = static_cast<int8_t>(laws[i % 4]);
                state.psi_b[i] = .2;
                state.lambda[i] = .5;
                state.vg_n[i] = 2;
                state.vg_L[i] = .5;
                state.hg[i] = .5 + .1 * (i % 12);
                const double theta = .2 + .005 * (i % 7);
                state.hu[i] = theta * (state.zs[i] - state.hg[i]);
                for (int j = 0; j < state.m_layers; ++j)
                    state.theta_sigma[std::size_t(j) * cells + i] = theta;
                state.xacc_from_surface[i] = i % 2 ? 1e-7 : -1e-7;
            }
            for (std::size_t n = 0; n < state.nacc.size(); ++n)
                state.nacc[n] = n % 2 ? 1e-8 : -1e-8;
        }
        for (int step = 0; step < 3; ++step) {
            for (int i = 0; i < cells; ++i)
                a.depth[i] = b.depth[i] = i % 3 == step ? 0. : .01 * (i % 5);
            serial.gw.refreshDtCell(serial.mesh, serial.edges);
            parallel.gw.refreshDtCell(parallel.mesh, parallel.edges);
            serial.gw.publishInfiltration(a, .25 * (step + 1), step);
            parallel.gw.publishInfiltration(b, .25 * (step + 1), step);
            EXPECT_EQ(a.infil_rate, b.infil_rate);
            EXPECT_EQ(bedState(serial.gw), bedState(parallel.gw));
            saveBedState("parallel_dt_intake_" + std::to_string(threads) + "_" +
                             std::to_string(step), parallel.gw);
        }
    }
}

TEST(RichardsSurfaceSnapshots, PreservesUnusedFieldsAndUnselectedInfiltrationSlots) {
    for (auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (int depth_count : {0, 1, 3}) {
            NodeTrialRig rig(2, closure, 2, 1, 1, 0, 0, 3);
            SurfaceStateData surface;
            surface.resize(3, 0);
            surface.depth.assign(depth_count, .25);
            surface.infil_rate = {.12345, .23456, .34567};
            surface.head = {11, 12, 13};
            surface.volume = {4, 5, 6};
            surface.grad_hx = {7, 8, 9};
            surface.face_vx = {1, 2, 3};
            surface.face_vy = {-1, -2, -3};
            surface.rainfall = {.1, .2, .3};
            surface.evap_rate = {.01, .02, .03};
            surface.coupling_flux = {-4, 0, 4};
            surface.infil_applied = {14, 15, 16};
            const auto before = surface;
            rig.gw.state().infil_interval[0] = 1;
            auto column = trialColumn();
            auto result = acceptRichardsBedsAtFiring(rig.gw, surface, 0, 1,
                                                      {owner(0, column, -2.5)});
            ASSERT_TRUE(result.error.empty()) << result.error;
            EXPECT_EQ(surface.depth, before.depth);
            EXPECT_EQ(surface.head, before.head);
            EXPECT_EQ(surface.volume, before.volume);
            EXPECT_EQ(surface.grad_hx, before.grad_hx);
            EXPECT_EQ(surface.face_vx, before.face_vx);
            EXPECT_EQ(surface.face_vy, before.face_vy);
            EXPECT_EQ(surface.rainfall, before.rainfall);
            EXPECT_EQ(surface.evap_rate, before.evap_rate);
            EXPECT_EQ(surface.coupling_flux, before.coupling_flux);
            EXPECT_EQ(surface.infil_applied, before.infil_applied);
            EXPECT_EQ(surface.edge_flux, before.edge_flux);
            EXPECT_DOUBLE_EQ(surface.infil_rate[0], rig.gw.state().infil_capacity[0]);
            EXPECT_NE(surface.infil_rate[0], before.infil_rate[0]);
            EXPECT_DOUBLE_EQ(surface.infil_rate[1], before.infil_rate[1]);
            EXPECT_DOUBLE_EQ(surface.infil_rate[2], before.infil_rate[2]);
        }
}

TEST(RichardsSurfaceSnapshots, NestedLateRefusalPreservesSurfaceAndRetryPublishesRates) {
    NodeTrialRig rig(2, GwClosure::CLOSED_FORM, 2, 1, 1, 0, 0, 2);
    SurfaceStateData surface;
    surface.resize(2, 0);
    surface.depth = {.25, .5};
    surface.head = {11, 12};
    surface.volume = {4, 5};
    surface.rainfall = {.1, .2};
    surface.infil_rate = {.12345, .23456};
    rig.gw.state().dt_cell = {2, 2};
    rig.gw.assignTiers(2, 1);
    rig.gw.state().infil_interval = {1, 1};
    rig.gw.state().nacc[1] = 10;
    auto column = trialColumn();
    const auto water = column.water;
    const auto before_surface = surface;
    openswmm::detail::CompletedSourceArchive before;
    rig.gw.checkpoint(before, false);
    auto bad = acceptRichardsBedsForTierAcrossSteps(rig.gw, surface, 0, 0, 2,
        {.5, 1, 2}, {owner(0, column, -2.5)});
    ASSERT_NE(bad.error.find("refunded"), std::string::npos) << bad.error;
    EXPECT_EQ(column.water, water);
    EXPECT_EQ(surface.depth, before_surface.depth);
    EXPECT_EQ(surface.head, before_surface.head);
    EXPECT_EQ(surface.volume, before_surface.volume);
    EXPECT_EQ(surface.rainfall, before_surface.rainfall);
    EXPECT_EQ(surface.infil_rate, before_surface.infil_rate);
    openswmm::detail::CompletedSourceArchive after;
    rig.gw.checkpoint(after, false);
    EXPECT_EQ(before.bytes(), after.bytes());
    rig.gw.state().nacc[1] = 0;
    auto accepted = acceptRichardsBedsForTierAcrossSteps(rig.gw, surface, 0, 0, 2,
        {.5, 1, 2}, {owner(0, column, -2.5)});
    ASSERT_TRUE(accepted.error.empty()) << accepted.error;
    EXPECT_EQ(surface.depth, before_surface.depth);
    EXPECT_EQ(surface.head, before_surface.head);
    EXPECT_EQ(surface.volume, before_surface.volume);
    EXPECT_EQ(surface.rainfall, before_surface.rainfall);
    EXPECT_EQ(surface.infil_rate, rig.gw.state().infil_capacity);
}

// Darcy velocity display output: on a 4 x 3 grid of 10 m quads with a planar
// water table, every interior cell's reconstructed specific discharge is
// q = −K·∇h (K = KS = 36 mm/hr = 1e-5 m/s). Interior cells have a face on
// every side, so the Perot reconstruction is exact up to the upwind
// transmissivity (a fraction of a percent here). Edge cells have a no-flow
// side and are only checked for direction.
TEST(Aquifer2D, DarcyVelocityReconstructsAPlanarWaterTable) {
    constexpr int nx = 4, ny = 3;
    constexpr double sx = 0.002, sy = 0.001, K = 36.0 / 1000.0 / 3600.0;
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
         "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
         "END_DATE 01/01/2026\nEND_TIME 00:05:00\n"
         "REPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 5\n"
         "ALLOW_PONDING NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nMOMENTUM_EQUATION LOCAL_INERTIAL\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n\n[2D_VERTICES]\n";
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            m << 10.0 * i << " " << 10.0 * j << " -10.0\n";
    m << "\n[2D_QUADS]\n";
    const auto v = [](int i, int j) { return j * (nx + 1) + i; };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            m << v(i, j) << " " << v(i + 1, j) << " " << v(i + 1, j + 1) << " " << v(i, j + 1)
              << " 0.03 0\n";
    m << "\n[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n"
         "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\n\n[2D_AQUIFER]\n";
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i)
            m << "CELL " << j * nx + i + 1 << " 36 10 .45 .1 2 HG0 "
              << 5.0 - sx * (10.0 * i + 5.0) - sy * (10.0 * j + 5.0) << "\n";
    m << "\n[REPORT]\nINPUT NO\n";

    DeckRun r = openDeck("darcy_velocity_planar", m.str());
    ASSERT_TRUE(r.opened);
    ASSERT_TRUE(run(r));
    const auto& g = r.eng->surfaceRouter2D().subsurface().state();
    ASSERT_EQ(g.n_cells, nx * ny);
    const auto head = [&](int c) { return g.z_bed[c] + g.hg[c]; };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int c = j * nx + i;
            SCOPED_TRACE("cell " + std::to_string(c));
            double qx = 0, qy = 0;
            ASSERT_EQ(swmm_gw2d_get_cell(r.e, c, SWMM_GW2D_VAR_QX, &qx), SWMM_OK);
            ASSERT_EQ(swmm_gw2d_get_cell(r.e, c, SWMM_GW2D_VAR_QY, &qy), SWMM_OK);
            EXPECT_EQ(qx, g.qx_last[c]);
            EXPECT_EQ(qy, g.qy_last[c]);
            EXPECT_GT(qx, 0.0) << "flow runs down the +x slope";
            EXPECT_GT(qy, 0.0) << "flow runs down the +y slope";
            if (i == 0 || j == 0 || i == nx - 1 || j == ny - 1)
                continue;
            // −K·∇h from the heads as they are now (the table relaxes slightly).
            const double ex = -K * (head(c + 1) - head(c - 1)) / 20.0;
            const double ey = -K * (head(c + nx) - head(c - nx)) / 20.0;
            EXPECT_NEAR(qx, ex, 0.02 * std::abs(ex)) << "qx " << qx << " vs " << ex;
            EXPECT_NEAR(qy, ey, 0.02 * std::abs(ey)) << "qy " << qy << " vs " << ey;
            EXPECT_NEAR(ex, K * sx, 0.05 * K * sx) << "the planar table did not persist";
        }
    finish(r);
}

namespace {

/**
 * @brief Five-minute deck of three 10 m groundwater quads in a row on a flat water table.
 * @param[in] zs Aquifer thickness of every cell (m).
 * @param[in] hg0 Initial saturated thickness of every cell (m).
 * @param[in] boundaries [2D_AQUIFER_BOUNDARIES] rows, or empty for none.
 */
std::string darcyStripDeck(double zs, double hg0, const std::string& boundaries) {
    constexpr int nx = 3;
    std::ostringstream m;
    m << "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\n"
         "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
         "END_DATE 01/01/2026\nEND_TIME 00:05:00\n"
         "REPORT_STEP 00:01:00\nWET_STEP 00:01:00\nDRY_STEP 00:01:00\nROUTING_STEP 5\n"
         "ALLOW_PONDING NO\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[2D_OPTIONS]\nMOMENTUM_EQUATION LOCAL_INERTIAL\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n\n[2D_VERTICES]\n";
    for (int j = 0; j <= 1; ++j)
        for (int i = 0; i <= nx; ++i)
            m << 10.0 * i << " " << 10.0 * j << " -10.0\n";
    m << "\n[2D_QUADS]\n";
    for (int i = 0; i < nx; ++i)
        m << i << " " << i + 1 << " " << nx + 2 + i << " " << nx + 1 + i << " 0.03 0\n";
    m << "\n[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n"
         "[2D_AQUIFER_OPTIONS]\nNODE_ENROLMENT ROWS\n\n[2D_AQUIFER]\n";
    for (int i = 0; i < nx; ++i)
        m << "CELL " << i + 1 << " 36 " << zs << " .45 .1 2 HG0 " << hg0 << "\n";
    if (!boundaries.empty())
        m << "\n[2D_AQUIFER_BOUNDARIES]\n" << boundaries;
    m << "\n[REPORT]\nINPUT NO\n";
    return m.str();
}

} // namespace

// Darcy velocity display output at a FLOW boundary and in thin cells: a row of
// three 10 m quads on a flat water table takes Q through the west edge of the
// first cell. The boundary face is part of that cell's Perot moment, so it
// shows q = Q·(dx/2) / (A·hg) (the east face carries almost nothing yet). A
// column that cannot reach 1 cm (ZS = 8 mm) reports zero instead of a spike.
TEST(Aquifer2D, DarcyVelocityCountsBoundaryEdgesAndSkipsThinCells) {
    constexpr double Q = 1.0e-6; // m3/s through the west edge of cell 1
    const auto deck = [&](double zs, double hg0) {
        // Local edge k runs from vertex k + 1 to k + 2: edge 2 (v3 -> v0) is the west side.
        std::ostringstream boundary;
        boundary << "CELL 1 EDGE 2 FLOW CONST " << Q << "\n";
        return darcyStripDeck(zs, hg0, boundary.str());
    };
    const auto darcy = [](DeckRun& r, int c, double& qx, double& qy) {
        ASSERT_EQ(swmm_gw2d_get_cell(r.e, c, SWMM_GW2D_VAR_QX, &qx), SWMM_OK);
        ASSERT_EQ(swmm_gw2d_get_cell(r.e, c, SWMM_GW2D_VAR_QY, &qy), SWMM_OK);
    };
    {
        DeckRun r = openDeck("darcy_velocity_boundary", deck(10.0, 4.9));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        const auto& g = r.eng->surfaceRouter2D().subsurface().state();
        double qx = 0, qy = 0;
        darcy(r, 0, qx, qy);
        const double expected = Q * 5.0 / (100.0 * g.hg[0]);
        EXPECT_NEAR(qx, expected, 0.02 * expected) << "the boundary face is in the moment";
        EXPECT_NEAR(qy, 0.0, 1e-3 * expected);
        finish(r);
    }
    {
        DeckRun r = openDeck("darcy_velocity_thin", deck(0.008, 0.005));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(run(r));
        const auto& g = r.eng->surfaceRouter2D().subsurface().state();
        ASSERT_LT(g.hg[0], 0.01) << "the first cell stayed thinner than 1 cm";
        ASSERT_GT(g.hg[0], 0.005) << "the boundary fed the first cell";
        double qx = 1, qy = 1;
        darcy(r, 0, qx, qy);
        EXPECT_EQ(qx, 0.0);
        EXPECT_EQ(qy, 0.0);
        finish(r);
    }
}

// A coupler's runtime FLOW prescription (kind 2, outward m3/s) on the same west
// edge is part of the Perot moment too, like an authored FLOW boundary: the
// cell drains westward at q = -Q·(dx/2) / (A·hg).
TEST(Aquifer2D, DarcyVelocityCountsRuntimeFlowBoundaryEdges) {
    constexpr double Q = 1.0e-6; // m3/s out through the west edge of cell 1
    DeckRun r = openDeck("darcy_velocity_runtime_flow", darcyStripDeck(10.0, 4.9, ""));
    ASSERT_TRUE(r.opened);
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    ASSERT_EQ(r.eng->surfaceRouter2D().subsurface().transport().n_species, 0);
    // Local edge 2 (v3 -> v0) is the west side.
    ASSERT_EQ(swmm_gw2d_set_runtime_boundary(r.e, 0, 2, 2, Q, nullptr, 0), SWMM_OK);
    double elapsed = 0.0;
    int steps = 0, rc = SWMM_OK;
    while ((rc = swmm_engine_step(r.e, &elapsed)) == SWMM_OK && elapsed > 0.0)
        ++steps;
    ASSERT_EQ(rc, SWMM_OK) << "the run stopped early";
    ASSERT_GT(steps, 1);
    const auto& g = r.eng->surfaceRouter2D().subsurface().state();
    double qx = 0, qy = 0;
    ASSERT_EQ(swmm_gw2d_get_cell(r.e, 0, SWMM_GW2D_VAR_QX, &qx), SWMM_OK);
    ASSERT_EQ(swmm_gw2d_get_cell(r.e, 0, SWMM_GW2D_VAR_QY, &qy), SWMM_OK);
    const double expected = -Q * 5.0 / (100.0 * g.hg[0]);
    EXPECT_NEAR(qx, expected, 0.02 * std::abs(expected)) << "the runtime FLOW face is in the moment";
    EXPECT_NEAR(qy, 0.0, 1e-3 * std::abs(expected));
    finish(r);
}

TEST(AquiferReview, UnsaturatedNodeBedUsesDisconnectedHeadWithoutTransport) {
    NodeTrialRig water(1, GwClosure::CLOSED_FORM, 1, 10, 1, 1e-5, .1);
    openswmm::NodeData nodes;
    nodes.resize(1);
    nodes.invert_elev[0] = 0;
    nodes.depth[0] = .2;
    nodes.volume[0] = 1;
    water.gw.sampleNodeExchange(&nodes, 1);
    const double expected = -(1e-5 / .1) * (.2 + .1);
    EXPECT_NEAR(water.gw.nodeExchangeVolumes()[0], expected, 1e-12);
}
