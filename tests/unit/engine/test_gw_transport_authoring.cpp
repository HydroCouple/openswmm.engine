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
 * @file test_gw_transport_authoring.cpp
 * @brief U4 + U5 (2026-09-07) — the `[GW_*]` subsurface-transport authoring
 *        surface and the 2D groundwater page keys.
 *
 * Gates:
 *   - G1 every section parses, validates and ROUND-TRIPS: author → save →
 *     reopen gives back the same rows. Round-trip fidelity is the whole
 *     observable in this release, since nothing runs the rows.
 *   - G2 cell-generic: `EDGE 0..nv-1` is checked against the cell's OWN
 *     vertex count on a MIXED tri/quad mesh — edge 3 is legal on the quad
 *     and refused on a triangle.
 *   - G3 species resolve through the one registry (pollutants, MSX,
 *     `__WATER_AGE__`, `__TEMPERATURE__`); an unknown name is fatal.
 *   - G4 a run emits exactly ONE "authored but inert" warning and the
 *     results are unchanged (the rows are not consumed).
 *   - G5 the C API writes what the file writes, refuses what the file
 *     refuses, and its rows survive a save.
 *   - G6 (U5) `GROUNDWATER ON` / `GW_ET` round-trip and warn.
 *
 * Artefacts under tests/output/gw_transport (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_model.h>

#include "core/SWMMEngine.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_GW_TEST_OUT_DIR) / "gw_transport";

std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

/// The body of one section. A bare find() over the whole file would match
/// [OPTIONS] IGNORE_GROUNDWATER when asking whether [2D_OPTIONS] carries
/// GROUNDWATER.
std::string section(const std::string& text, const std::string& header) {
    std::istringstream in(text);
    std::string line, out;
    bool in_sec = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            if (in_sec) break;
            in_sec = line.rfind(header, 0) == 0;
            continue;
        }
        if (in_sec) out += line + "\n";
    }
    return out;
}

/// A MIXED mesh: one triangle (cell 0, 3 local edges) and one quad (cell 1,
/// 4 local edges) — cells are numbered triangles first, then quads — so the
/// cell-generic edge check has both to observe.
std::string deck(const std::string& gw_sections,
                 const std::string& twod_options_extra = "") {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:10:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "ROUTING_STEP         5\nALLOW_PONDING        NO\n"
         "WATER_AGE            YES\nHEAT_TRANSPORT       YES\n\n"
         "[POLLUTANTS]\nCu MG/L 0 0 0 0\n\n"
         "[JUNCTIONS]\nJ1 0.0 3.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -1.0 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.5 0 0 0 1\n\n"
         "[TIMESERIES]\n;;Name Date Time Value\n"
         "WellQ  0:00  0.001\nWellQ  1:00  0.002\n"
         "RiverT 0:00  11.0\nRiverT 1:00  12.0\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
      << twod_options_extra << "\n"
         "[2D_VERTICES]\n"
         " 0.0  0.0 -10.0\n10.0  0.0 -10.0\n10.0 10.0 -10.0\n 0.0 10.0 -10.0\n"
         "20.0  0.0 -10.0\n\n"
         // Triangles are numbered before quads, so the triangle is cell 1
         // and the quad is cell 2 (1-based in the file).
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH TAG\n"
         "1 4 2 0.03 0.5 FAR\n\n"
         "[2D_QUADS]\n;;V1 V2 V3 V4 N INIT_DEPTH TAG\n"
         "0 1 2 3 0.03 0.5 PAN\n\n"
      << gw_sections
      << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    fs::path rpt;
    bool opened = false, started = false;
};

DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    DeckRun r;
    const fs::path inp = kOutDir / (tag + ".inp");
    r.rpt = kOutDir / (tag + ".rpt");
    { std::ofstream f(inp); f << body; }
    r.e = swmm_engine_create();
    if (!r.e) return r;
    r.opened = swmm_engine_open(r.e, inp.string().c_str(), r.rpt.string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(),
                                nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

void finish(DeckRun& r) {
    if (!r.e) return;
    if (r.started) swmm_engine_end(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

int warningsMatching(const DeckRun& r, const char* needle) {
    int n = 0;
    for (const auto& w : r.eng->context().warnings)
        if (w.find(needle) != std::string::npos) ++n;
    return n;
}

/// The full authoring set the round-trip test uses. Cell 1 is the QUAD
/// (4 local edges), cell 2 the triangle (3).
const char* kAllSections =
    "[GW_TRANSPORT_OPTIONS]\n"
    "TRANSPORT_POLLUTANTS   YES\n"
    "TRANSPORT_MSX          NO\n"
    "TRANSPORT_AGE          YES\n"
    "TRANSPORT_TEMPERATURE  YES\n"
    "DISPERSION             YES\n"
    "CONDUCTION             NO\n"
    "SURFACE_THERMAL_BC     FIXED 12.5\n"
    "DEEP_THERMAL_BC        GEOTHERMAL_FLUX 0.065 DEPTH 20\n"
    "THERMAL_MIXING         GEOMETRIC\n"
    "C_DIFF                 0.25\n\n"
    "[GW_TRANSPORT_PARAMS]\n"
    "*      2650  880  2.0  0  1.0  0.1  1e-9  1e-9  0.065\n"
    "TAG PAN  2700  900  2.4  0  1.5  0.15 2e-9  1e-9  0.07\n"
    "CELL 2   2600  860  1.8  0  0.8  0.08 1e-9  1e-9  0.06\n\n"
    "[GW_SORPTION]\n"
    "*        Cu  0.5  0.01\n"
    "TAG FAR  Cu  1.2\n\n"
    "[GW_INITIAL_QUALITY]\n"
    "*        SAT     Cu               0.0\n"
    "*        SAT     __TEMPERATURE__  12.0\n"
    "TAG PAN  UNSAT   __TEMPERATURE__  14.0\n"
    "CELL 2   LAYER 3 Cu               1.2\n"
    "*        SAT     __WATER_AGE__    6.0\n\n"
    "[GW_BOUNDARY_QUALITY]\n"
    ";; cell edge species kind value\n"
    "2  3  Cu               CONC 5.0\n"
    "1  0  __TEMPERATURE__  TS   RiverT\n"
    "1  2  Cu               MASSFLUX 0.001\n\n"
    "[GW_SOURCES]\n"
    "W1   CELL 1  FLOW WellQ  Cu CONC 0.0  __TEMPERATURE__ CONC 10.0\n"
    "INJ  CELL 2  FLOW 0.002  Cu MASS 0.5\n\n";

}  // namespace

// ---------------------------------------------------------------------------
// G1 — parse, validate, round-trip
// ---------------------------------------------------------------------------
TEST(GwTransportAuthoring, EverySectionRoundTrips) {
    DeckRun r = openDeck("all", deck(kAllSections));
    ASSERT_TRUE(r.opened) << r.eng->context().error_message;

    EXPECT_EQ(swmm_gw_transport_authored(r.e), 1);
    EXPECT_EQ(swmm_gw_params_count(r.e), 3);
    EXPECT_EQ(swmm_gw_sorption_count(r.e), 2);
    EXPECT_EQ(swmm_gw_init_quality_count(r.e), 5);
    EXPECT_EQ(swmm_gw_boundary_quality_count(r.e), 3);
    EXPECT_EQ(swmm_gw_source_count(r.e), 2);
    EXPECT_EQ(swmm_gw_source_species_count(r.e, 0), 2);

    char buf[256] = {};
    ASSERT_EQ(swmm_gw_transport_option_get(r.e, "TRANSPORT_MSX", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "NO");
    ASSERT_EQ(swmm_gw_transport_option_get(r.e, "SURFACE_THERMAL_BC", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "FIXED 12.5");
    ASSERT_EQ(swmm_gw_transport_option_get(r.e, "DEEP_THERMAL_BC", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "GEOTHERMAL_FLUX 0.065 DEPTH 20");
    ASSERT_EQ(swmm_gw_transport_option_get(r.e, "C_DIFF", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "0.25");
    EXPECT_NE(swmm_gw_transport_option_get(r.e, "NOPE", buf, sizeof buf), SWMM_OK);

    const fs::path saved = kOutDir / "all_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    for (const char* sec : {"[GW_TRANSPORT_OPTIONS]", "[GW_TRANSPORT_PARAMS]",
                            "[GW_SORPTION]", "[GW_INITIAL_QUALITY]",
                            "[GW_BOUNDARY_QUALITY]", "[GW_SOURCES]"})
        EXPECT_NE(text.find(sec), std::string::npos) << sec << " was dropped";
    finish(r);

    // Reopen the saved file: identical row counts and values.
    DeckRun r2 = openDeck("all_reload", text);
    ASSERT_TRUE(r2.opened) << r2.eng->context().error_message;
    EXPECT_EQ(swmm_gw_params_count(r2.e), 3);
    EXPECT_EQ(swmm_gw_sorption_count(r2.e), 2);
    EXPECT_EQ(swmm_gw_init_quality_count(r2.e), 5);
    EXPECT_EQ(swmm_gw_boundary_quality_count(r2.e), 3);
    EXPECT_EQ(swmm_gw_source_count(r2.e), 2);

    SWMM_GwParams p{};
    char tag[64] = {};
    ASSERT_EQ(swmm_gw_params_get(r2.e, 1, &p, tag, sizeof tag), SWMM_OK);
    EXPECT_EQ(p.scope, SWMM_GW_SCOPE_TAG);
    EXPECT_STREQ(tag, "PAN");
    EXPECT_DOUBLE_EQ(p.rho_s, 2700.0);
    EXPECT_DOUBLE_EQ(p.alpha_L, 1.5);

    int scope = -1, cell = -1, zone = -1, layer = -1;
    char species[64] = {};
    double value = 0.0;
    ASSERT_EQ(swmm_gw_init_quality_get(r2.e, 3, &scope, tag, sizeof tag, &cell,
                                       &zone, &layer, species, sizeof species,
                                       &value), SWMM_OK);
    EXPECT_EQ(scope, SWMM_GW_SCOPE_CELL);
    EXPECT_EQ(cell, 1);                     // CELL 2 in the file, 0-based here
    EXPECT_EQ(zone, SWMM_GW_ZONE_LAYER);
    EXPECT_EQ(layer, 3);
    EXPECT_STREQ(species, "Cu");
    EXPECT_DOUBLE_EQ(value, 1.2);

    int bcell = -1, bedge = -1;
    char kind[32] = {}, ts[64] = {};
    ASSERT_EQ(swmm_gw_boundary_quality_get(r2.e, 1, &bcell, &bedge, species,
                                           sizeof species, kind, sizeof kind,
                                           &value, ts, sizeof ts), SWMM_OK);
    EXPECT_EQ(bcell, 0);
    EXPECT_EQ(bedge, 0);
    EXPECT_STREQ(kind, "TS");
    EXPECT_STREQ(ts, "RiverT");

    char name[64] = {}, flow_ts[64] = {};
    double flow = 0.0;
    ASSERT_EQ(swmm_gw_source_get(r2.e, 0, name, sizeof name, &scope, tag,
                                 sizeof tag, &cell, &flow, flow_ts,
                                 sizeof flow_ts), SWMM_OK);
    EXPECT_STREQ(name, "W1");
    EXPECT_EQ(cell, 0);
    EXPECT_STREQ(flow_ts, "WellQ");
    finish(r2);
}

// ---------------------------------------------------------------------------
// G2 — cell-generic edges on a MIXED mesh
// ---------------------------------------------------------------------------
TEST(GwTransportAuthoring, EdgeIsCheckedAgainstItsOwnCellsVertexCount) {
    // Cell 2 is the quad: local edge 3 exists.
    {
        DeckRun r = openDeck("edge_quad_ok",
                         deck("[GW_BOUNDARY_QUALITY]\n2  3  Cu  CONC 5.0\n\n"));
        EXPECT_TRUE(r.opened) << r.eng->context().error_message;
        finish(r);
    }
    // Cell 1 is the triangle: local edge 3 does not.
    {
        DeckRun r = openDeck("edge_tri_bad",
                         deck("[GW_BOUNDARY_QUALITY]\n1  3  Cu  CONC 5.0\n\n"));
        EXPECT_FALSE(r.opened)
            << "EDGE 3 on a 3-vertex cell must be refused (cell-generic check)";
        if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
    }
    // ...and edge 2 does.
    {
        DeckRun r = openDeck("edge_tri_ok",
                         deck("[GW_BOUNDARY_QUALITY]\n1  2  Cu  CONC 5.0\n\n"));
        EXPECT_TRUE(r.opened) << r.eng->context().error_message;
        finish(r);
    }
    // A cell off the mesh is refused.
    {
        DeckRun r = openDeck("edge_cell_bad",
                         deck("[GW_BOUNDARY_QUALITY]\n99  0  Cu  CONC 5.0\n\n"));
        EXPECT_FALSE(r.opened);
        if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
    }
}

// ---------------------------------------------------------------------------
// G3 — species and series resolve through the one registry
// ---------------------------------------------------------------------------
TEST(GwTransportAuthoring, SpeciesAndSeriesAreResolved) {
    {
        DeckRun r = openDeck("species_bad",
                         deck("[GW_INITIAL_QUALITY]\n*  SAT  Nope  1.0\n\n"));
        EXPECT_FALSE(r.opened) << "an unknown species must be fatal";
        if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
    }
    {
        DeckRun r = openDeck("series_bad",
                         deck("[GW_BOUNDARY_QUALITY]\n1 0 Cu TS NoSuchSeries\n\n"));
        EXPECT_FALSE(r.opened) << "an unknown time series must be fatal";
        if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
    }
    {
        // A negative concentration is refused; a negative temperature is not.
        DeckRun r = openDeck("neg_conc",
                         deck("[GW_INITIAL_QUALITY]\n*  SAT  Cu  -1.0\n\n"));
        EXPECT_FALSE(r.opened);
        if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
    }
    {
        DeckRun r = openDeck("neg_temp",
                         deck("[GW_INITIAL_QUALITY]\n"
                              "*  SAT  __TEMPERATURE__  -3.0\n\n"));
        EXPECT_TRUE(r.opened) << r.eng->context().error_message;
        finish(r);
    }
    {
        DeckRun r = openDeck("tag_bad",
                         deck("[GW_SORPTION]\nTAG NOSUCH  Cu  0.5\n\n"));
        EXPECT_FALSE(r.opened) << "a TAG matching no cell must be fatal";
        if (r.e) { swmm_engine_close(r.e); swmm_engine_destroy(r.e); }
    }
}

// ---------------------------------------------------------------------------
// G4 — one warning, results unchanged
// ---------------------------------------------------------------------------
TEST(GwTransportAuthoring, RunIsInertWithExactlyOneWarning) {
    DeckRun r = openDeck("inert", deck(kAllSections));
    ASSERT_TRUE(r.opened) << r.eng->context().error_message;
    EXPECT_EQ(warningsMatching(r, "subsurface transport is AUTHORED but INERT"), 1)
        << "exactly one warning, not one per row";
    ASSERT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r.e, 1), SWMM_OK);
    r.started = true;
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    const double final_2d = r.eng->context().mass_balance_2d.final_storage;
    finish(r);

    // The same deck without any [GW_*] section reaches the same 2D state.
    DeckRun r2 = openDeck("inert_control", deck(""));
    ASSERT_TRUE(r2.opened);
    EXPECT_EQ(warningsMatching(r2, "subsurface transport is AUTHORED"), 0);
    ASSERT_EQ(swmm_engine_initialize(r2.e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(r2.e, 1), SWMM_OK);
    r2.started = true;
    while (swmm_engine_step(r2.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
    EXPECT_DOUBLE_EQ(r2.eng->context().mass_balance_2d.final_storage, final_2d)
        << "authoring the subsurface changed the surface run";
    finish(r2);
}

// ---------------------------------------------------------------------------
// G5 — the C API is the file's twin
// ---------------------------------------------------------------------------
TEST(GwTransportAuthoring, CApiAuthorsAndSaves) {
    DeckRun r = openDeck("api", deck(""));
    ASSERT_TRUE(r.opened);
    EXPECT_EQ(swmm_gw_transport_authored(r.e), 0);

    ASSERT_EQ(swmm_gw_transport_option_set(r.e, "TRANSPORT_MSX", "NO"), SWMM_OK);
    ASSERT_EQ(swmm_gw_transport_option_set(r.e, "SURFACE_THERMAL_BC",
                                           "FIXED 9.5"), SWMM_OK);
    EXPECT_NE(swmm_gw_transport_option_set(r.e, "SURFACE_THERMAL_BC", "FIXED"),
              SWMM_OK) << "FIXED without a value must be refused";
    EXPECT_NE(swmm_gw_transport_option_set(r.e, "C_DIFF", "2.0"), SWMM_OK);
    EXPECT_NE(swmm_gw_transport_option_set(r.e, "NOPE", "1"), SWMM_OK);
    char buf[128] = {};
    ASSERT_EQ(swmm_gw_transport_option_get(r.e, "SURFACE_THERMAL_BC", buf,
                                           sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "FIXED 9.5") << "a refused set must not clobber";

    SWMM_GwParams p{};
    p.scope = SWMM_GW_SCOPE_CELL;
    p.cell  = 0;
    p.rho_s = 2500.0; p.c_s = 850.0; p.lambda_s = 1.9; p.a_s = 0.0;
    p.alpha_L = 2.0; p.alpha_T = 0.2; p.D_m = 1e-9; p.D_v = 1e-9;
    p.geo_flux = 0.05;
    ASSERT_EQ(swmm_gw_params_set(r.e, &p, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_gw_params_count(r.e), 1);
    p.alpha_L = 3.0;
    ASSERT_EQ(swmm_gw_params_set(r.e, &p, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_gw_params_count(r.e), 1) << "same scope+cell must upsert";

    ASSERT_EQ(swmm_gw_init_quality_set(r.e, SWMM_GW_SCOPE_GLOBAL, nullptr, -1,
                                       SWMM_GW_ZONE_SAT, -1, "Cu", 2.5), SWMM_OK);
    EXPECT_NE(swmm_gw_init_quality_set(r.e, SWMM_GW_SCOPE_GLOBAL, nullptr, -1,
                                       SWMM_GW_ZONE_SAT, -1, "Cu", -2.5), SWMM_OK)
        << "a negative concentration must be refused by the API too";

    // The cell-generic edge check: cell 1 is the quad (edge 3 legal), cell 0
    // the triangle (edge 3 out of range) — triangles are numbered first.
    ASSERT_EQ(swmm_gw_boundary_quality_set(r.e, 1, 3, "Cu", "CONC", 4.0, nullptr),
              SWMM_OK);
    EXPECT_NE(swmm_gw_boundary_quality_set(r.e, 0, 3, "Cu", "CONC", 4.0, nullptr),
              SWMM_OK);
    EXPECT_NE(swmm_gw_boundary_quality_set(r.e, 0, 0, "Cu", "TS", 0.0, nullptr),
              SWMM_OK) << "TS needs a series name";

    ASSERT_EQ(swmm_gw_source_set(r.e, "W9", SWMM_GW_SCOPE_CELL, nullptr, 1,
                                 0.003, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_species_set(r.e, 0, "Cu", "CONC", 1.5, nullptr),
              SWMM_OK);
    EXPECT_EQ(swmm_gw_source_species_count(r.e, 0), 1);
    EXPECT_NE(swmm_gw_source_species_set(r.e, 0, "Cu", "NOPE", 1.5, nullptr),
              SWMM_OK);
    EXPECT_EQ(swmm_gw_transport_authored(r.e), 1);

    const fs::path saved = kOutDir / "api_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    finish(r);

    DeckRun r2 = openDeck("api_reload", text);
    ASSERT_TRUE(r2.opened) << r2.eng->context().error_message;
    EXPECT_EQ(swmm_gw_params_count(r2.e), 1);
    EXPECT_EQ(swmm_gw_init_quality_count(r2.e), 1);
    EXPECT_EQ(swmm_gw_boundary_quality_count(r2.e), 1);
    EXPECT_EQ(swmm_gw_source_count(r2.e), 1);
    EXPECT_EQ(swmm_gw_source_species_count(r2.e, 0), 1);
    ASSERT_EQ(swmm_gw_transport_option_get(r2.e, "SURFACE_THERMAL_BC", buf,
                                           sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "FIXED 9.5");
    finish(r2);
}

// ---------------------------------------------------------------------------
// G6 (U5) — the groundwater page keys
// ---------------------------------------------------------------------------
// U5 rewired (2026-09-07): GROUNDWATER is a tri-state PROCESS ENABLE over the
// G1 kernel, and GW_ET is an ALIAS of [2D_AQUIFER_OPTIONS] GW_ET. The claim is
// that ONE setting is stored, resolved and written in exactly ONE place.
TEST(GwPageKeys, GroundwaterEnableIsTriStateAndGwEtIsAnAlias) {
    char buf[64] = {};

    // AUTO is the default, reports AS STORED (the INFILTRATION rule — a host
    // derives the effective state from the row count) and is not written. A
    // deck that never heard of the key is unchanged.
    DeckRun r3 = openDeck("u5_default", deck(""));
    ASSERT_TRUE(r3.opened) << r3.eng->context().error_message;
    ASSERT_EQ(swmm_options_get_ext(r3.e, "GROUNDWATER", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "AUTO") << "the enable round-trips what the deck said";
    const fs::path saved3 = kOutDir / "u5_default_saved.inp";
    ASSERT_EQ(swmm_model_write(r3.e, saved3.string().c_str()), SWMM_OK);
    EXPECT_EQ(section(readAll(saved3), "[2D_OPTIONS]").find("GROUNDWATER"),
              std::string::npos) << "AUTO must not be written";
    finish(r3);

    // GW_ET spelled in [2D_OPTIONS] folds into [2D_AQUIFER_OPTIONS] and is
    // written back THERE, once — never in both sections.
    DeckRun r = openDeck("u5", deck("", "GW_ET BOTH\n"));
    ASSERT_TRUE(r.opened) << r.eng->context().error_message;
    ASSERT_EQ(swmm_options_get_ext(r.e, "GW_ET", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "BOTH") << "the alias reads back through the C API";

    const fs::path saved = kOutDir / "u5_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_EQ(section(text, "[2D_OPTIONS]").find("GW_ET"), std::string::npos)
        << "GW_ET must not be echoed into [2D_OPTIONS] as well";
    EXPECT_NE(section(text, "[2D_AQUIFER_OPTIONS]").find("GW_ET"),
              std::string::npos) << "[2D_AQUIFER_OPTIONS] owns GW_ET";
    finish(r);

    DeckRun r2 = openDeck("u5_reload", text);
    ASSERT_TRUE(r2.opened) << r2.eng->context().error_message;
    ASSERT_EQ(swmm_options_get_ext(r2.e, "GW_ET", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "BOTH") << "and it survives the round trip";
    finish(r2);

    // An explicit YES with nothing authored is the "asked for it, got nothing"
    // case — one warning, exactly as INFILTRATION YES reports it.
    DeckRun r4 = openDeck("u5_on_empty", deck("", "GROUNDWATER YES\n"));
    ASSERT_TRUE(r4.opened) << r4.eng->context().error_message;
    ASSERT_EQ(swmm_options_get_ext(r4.e, "GROUNDWATER", buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "YES");
    EXPECT_EQ(warningsMatching(r4, "no [2D_AQUIFER_OPTIONS] / [2D_AQUIFER] row"), 1);
    const fs::path saved4 = kOutDir / "u5_on_empty_saved.inp";
    ASSERT_EQ(swmm_model_write(r4.e, saved4.string().c_str()), SWMM_OK);
    EXPECT_NE(section(readAll(saved4), "[2D_OPTIONS]").find("GROUNDWATER"),
              std::string::npos) << "a non-default enable IS written";
    finish(r4);

    DeckRun r5 = openDeck("u5_bad", deck("", "GW_ET SOMETIMES\n"));
    EXPECT_FALSE(r5.opened);
    if (r5.e) { swmm_engine_close(r5.e); swmm_engine_destroy(r5.e); }

    DeckRun r6 = openDeck("u5_bad_gw", deck("", "GROUNDWATER SOMETIMES\n"));
    EXPECT_FALSE(r6.opened);
    if (r6.e) { swmm_engine_close(r6.e); swmm_engine_destroy(r6.e); }
}

// ---------------------------------------------------------------------------
// The FILE sidecar
// ---------------------------------------------------------------------------
TEST(GwTransportAuthoring, InitialQualityFileSidecar) {
    fs::create_directories(kOutDir);
    const fs::path csv = kOutDir / "gw_iq.csv";
    {
        std::ofstream f(csv);
        f << "scope,zone,species,value\n"
             "*,SAT,Cu,0.5\n"
             "TAG PAN,UNSAT,__TEMPERATURE__,13.5\n";
    }
    DeckRun r = openDeck("gw_iq_file",
                     deck("[GW_INITIAL_QUALITY]\nFILE gw_iq.csv\n"
                          "*  SAT  __WATER_AGE__  3.0\n\n"));
    ASSERT_TRUE(r.opened) << r.eng->context().error_message;
    EXPECT_EQ(swmm_gw_init_quality_count(r.e), 3) << "2 file rows + 1 inline";
    char buf[256] = {};
    ASSERT_EQ(swmm_gw_init_quality_file_get(r.e, buf, sizeof buf), SWMM_OK);
    EXPECT_STREQ(buf, "gw_iq.csv");

    const fs::path saved = kOutDir / "gw_iq_file_saved.inp";
    ASSERT_EQ(swmm_model_write(r.e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("gw_iq.csv"), std::string::npos);
    // The file's rows are referenced, not inlined: only the inline age row.
    EXPECT_NE(text.find("__WATER_AGE__"), std::string::npos);
    EXPECT_EQ(text.find("13.5"), std::string::npos)
        << "the sidecar's rows must not be expanded into the .inp";
    finish(r);
}
