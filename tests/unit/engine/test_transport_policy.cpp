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
 * @file test_transport_policy.cpp
 * @brief E2 (2026-09-07) — TransportPolicy, swmm_get_transport_matrix, the
 *        .rpt matrix block and the [2D_OPTIONS] process keys.
 *
 * Gates (EXECUTION_PLAN_V2 §E2):
 *   - G1 grammar: every key round-trips through swmm_options_set_ext /
 *     get_ext with the engine's spelling; bad values are refused.
 *   - G2 default-preserving: a deck without the keys sizes the 2D transport
 *     state exactly as before (pollutants, age, temperature rows).
 *   - G3 TRANSPORT_POLLUTANTS NO ⇒ n_pollut == 0 with age/temperature intact;
 *     the matrix says DISABLED_BY_USER / TRANSPORT_POLLUTANTS.
 *   - G4 INFILTRATION NO ⇒ zero infiltrated volume on a deck whose '*' row
 *     would otherwise infiltrate; AUTO reports the effective YES.
 *   - G5 EVAPORATION NO zeroes a forced sink; CLIMATE evaporates at the
 *     project [EVAPORATION] rate with no forcing.
 *   - G6 the .rpt carries the matrix block for a species deck and not for a
 *     hydraulics-only deck.
 *   - G7 INFIL_DESTINATION fills rows without a DEST column (refused at
 *     resolve for the reserved destinations, exactly as a spelled DEST is).
 *
 * All files land under tests/output/transport_policy (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_infil2d.h>
#include <openswmm/engine/openswmm_model.h>

#include "2d/SurfaceRouter2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "core/SWMMEngine.hpp"
#include "transport/TransportPolicy.hpp"

namespace fs = std::filesystem;

namespace {

const fs::path kOutDir = fs::path(OPENSWMM_TRANSPORT_POLICY_TEST_OUT_DIR) / "transport_policy";

std::string readAll(const fs::path& p) {
    std::ifstream f(p);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

std::string ext(SWMM_Engine e, const char* key) {
    char buf[512] = {};
    if (swmm_options_get_ext(e, key, buf, sizeof buf) != SWMM_OK) return "<err>";
    return buf;
}

/// A 10 m pan (2 triangles) draining to J1 → O1, one pollutant, optional
/// extra [OPTIONS] lines and extra 2D sections.
std::string deck(const std::string& options_extra, const std::string& twod_options_extra,
                 const std::string& extra_sections,
                 const std::string& pollut = "[POLLUTANTS]\nCu MG/L 0 0 0 0\n\n") {
    std::ostringstream m;
    m << "[OPTIONS]\n"
         "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
         "END_DATE             01/01/2026\nEND_TIME             00:20:00\n"
         "REPORT_STEP          00:01:00\nWET_STEP             00:01:00\n"
         "ROUTING_STEP         5\nALLOW_PONDING        NO\n"
      << options_extra << "\n"
      << pollut
      << "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n"
         "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
         "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
         "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 5\n"
         "DRY_DEPTH 0.001\nCOUPLING_CD 0.7\nREPORT_2D NO\n"
      << twod_options_extra << "\n"
         "[2D_VERTICES]\n"
         " 0.0  0.0 -10.0\n10.0  0.0 -10.0\n10.0 10.0 -10.0\n 0.0 10.0 -10.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n"
         "[2D_VERTEX_NODE_MAP]\n0 O1 0.7 1.0\n\n"
      << extra_sections
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

bool startRun(DeckRun& r) {
    if (!r.opened) return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK) return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK) return false;
    r.started = true;
    return true;
}

void stepAll(DeckRun& r) {
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {}
}

void finish(DeckRun& r) {
    if (!r.e) return;
    if (r.started) swmm_engine_end(r.e);
    if (r.started) swmm_engine_report(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

const SWMM_TransportCell& cell(const SWMM_TransportMatrix& m, int d, int c) {
    return m.cell[d][c];
}

/// True when some line of @p text tokenizes to exactly {key, value}.
bool hasKeyValue(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k, v, rest;
        if (!(ls >> k >> v)) continue;
        if (ls >> rest) continue;
        if (k == key && v == value) return true;
    }
    return false;
}

bool hasKey(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k;
        if ((ls >> k) && k == key) return true;
    }
    return false;
}

/// The body of one section, so a 2D key is not confused with the 1D [OPTIONS]
/// key of the same name (INFILTRATION is spelled in both).
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

}  // namespace

// ---------------------------------------------------------------------------
// G1 — grammar
// ---------------------------------------------------------------------------
TEST(TransportPolicyKeys, GrammarRoundTripsAndRefusesBadValues) {
    DeckRun r = openDeck("keys", deck("", "", ""));
    ASSERT_TRUE(r.opened);
    SWMM_Engine e = r.e;

    // Defaults.
    EXPECT_EQ(ext(e, "TRANSPORT_POLLUTANTS"), "YES");
    EXPECT_EQ(ext(e, "TRANSPORT_MSX"), "YES");
    EXPECT_EQ(ext(e, "TRANSPORT_AGE"), "YES");
    EXPECT_EQ(ext(e, "TRANSPORT_TEMPERATURE"), "YES");
    EXPECT_EQ(ext(e, "EVAPORATION"), "YES");
    EXPECT_EQ(ext(e, "INFILTRATION"), "AUTO");       // unset ⇒ AUTO (on iff rows resolve)
    EXPECT_EQ(ext(e, "INFIL_DEFAULT_METHOD"), "NONE");
    EXPECT_EQ(ext(e, "INFIL_DESTINATION"), "LOST");
    EXPECT_EQ(ext(e, "INFIL_STEP"), "00:00:00");

    EXPECT_EQ(swmm_options_set_ext(e, "TRANSPORT_POLLUTANTS", "NO"), SWMM_OK);
    EXPECT_EQ(ext(e, "TRANSPORT_POLLUTANTS"), "NO");
    EXPECT_EQ(swmm_options_set_ext(e, "TRANSPORT_AGE", "off"), SWMM_OK);
    EXPECT_EQ(ext(e, "TRANSPORT_AGE"), "NO");
    EXPECT_NE(swmm_options_set_ext(e, "TRANSPORT_MSX", "MAYBE"), SWMM_OK);
    EXPECT_EQ(ext(e, "TRANSPORT_MSX"), "YES") << "a refused set must not clobber";

    EXPECT_EQ(swmm_options_set_ext(e, "EVAPORATION", "CLIMATE"), SWMM_OK);
    EXPECT_EQ(ext(e, "EVAPORATION"), "CLIMATE");
    EXPECT_EQ(swmm_options_set_ext(e, "EVAPORATION", "NO"), SWMM_OK);
    EXPECT_EQ(ext(e, "EVAPORATION"), "NO");
    EXPECT_NE(swmm_options_set_ext(e, "EVAPORATION", "SOMETIMES"), SWMM_OK);

    EXPECT_EQ(swmm_options_set_ext(e, "INFILTRATION", "YES"), SWMM_OK);
    EXPECT_EQ(ext(e, "INFILTRATION"), "YES");
    EXPECT_EQ(swmm_options_set_ext(e, "INFILTRATION", "AUTO"), SWMM_OK);
    EXPECT_EQ(ext(e, "INFILTRATION"), "AUTO");

    EXPECT_EQ(swmm_options_set_ext(e, "INFIL_DEFAULT_METHOD", "MODIFIED_HORTON"), SWMM_OK);
    EXPECT_EQ(ext(e, "INFIL_DEFAULT_METHOD"), "MOD_HORTON");
    EXPECT_NE(swmm_options_set_ext(e, "INFIL_DEFAULT_METHOD", "MAGIC"), SWMM_OK);

    EXPECT_EQ(swmm_options_set_ext(e, "INFIL_DESTINATION", "SUBCATCH_AQUIFER"), SWMM_OK);
    EXPECT_EQ(ext(e, "INFIL_DESTINATION"), "SUBCATCH_AQUIFER");
    EXPECT_NE(swmm_options_set_ext(e, "INFIL_DESTINATION", "MARS"), SWMM_OK);

    // INFIL_STEP is an alias of [2D_INFILTRATION_OPTIONS] INFIL_STEP: the
    // write lands in the Infil2D options the C API reads.
    EXPECT_EQ(swmm_options_set_ext(e, "INFIL_STEP", "00:02:00"), SWMM_OK);
    EXPECT_EQ(ext(e, "INFIL_STEP"), "00:02:00");
    SWMM_Infil2DOptions io{};
    ASSERT_EQ(swmm_infil2d_get_options(e, &io), SWMM_OK);
    EXPECT_DOUBLE_EQ(io.infil_step, 120.0);

    // The writer emits the non-defaults into [2D_OPTIONS] and they reload.
    const fs::path saved = kOutDir / "keys_saved.inp";
    ASSERT_EQ(swmm_model_write(e, saved.string().c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_TRUE(hasKeyValue(text, "TRANSPORT_POLLUTANTS", "NO")) << text;
    EXPECT_TRUE(hasKeyValue(text, "TRANSPORT_AGE", "NO"));
    EXPECT_TRUE(hasKeyValue(text, "EVAPORATION", "NO"));
    EXPECT_TRUE(hasKeyValue(text, "INFIL_DESTINATION", "SUBCATCH_AQUIFER"));
    EXPECT_TRUE(hasKeyValue(text, "INFIL_DEFAULT_METHOD", "MOD_HORTON"));
    const std::string opts2d = section(text, "[2D_OPTIONS]");
    EXPECT_FALSE(hasKey(opts2d, "INFILTRATION")) << "AUTO must not be written";
    EXPECT_FALSE(hasKey(opts2d, "TRANSPORT_MSX")) << "defaults must not be written";
    // The alias is written where it always was, beside the rows.
    EXPECT_TRUE(hasKeyValue(text, "INFIL_STEP", "0:02:00")) << text;   // fmt_step spelling
    finish(r);

    DeckRun r2 = openDeck("keys_reload", text);
    ASSERT_TRUE(r2.opened);
    EXPECT_EQ(ext(r2.e, "TRANSPORT_POLLUTANTS"), "NO");
    EXPECT_EQ(ext(r2.e, "EVAPORATION"), "NO");
    EXPECT_EQ(ext(r2.e, "INFIL_DESTINATION"), "SUBCATCH_AQUIFER");
    EXPECT_EQ(ext(r2.e, "INFIL_STEP"), "00:02:00");
    finish(r2);
}

// ---------------------------------------------------------------------------
// G2 / G3 — row layout with and without the opt-out
// ---------------------------------------------------------------------------
TEST(TransportPolicyRows, DefaultLayoutIsUnchangedAndPollutantOptOutKeepsAgeAndHeat) {
    {
        DeckRun r = openDeck("rows_default",
                         deck("WATER_AGE YES\nHEAT_TRANSPORT YES\n", "", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        const auto& tr = r.eng->surfaceRouter2D().state().transport;
        EXPECT_EQ(tr.n_pollut, 1);
        EXPECT_EQ(tr.n_msx, 0);
        ASSERT_EQ(tr.row_names.size(), 3u);
        EXPECT_EQ(tr.row_names[0], "Cu");
        EXPECT_EQ(tr.row_names[1], "__WATER_AGE__");
        EXPECT_EQ(tr.row_names[2], "__TEMPERATURE__");
        EXPECT_EQ(tr.age_row, 1);
        EXPECT_EQ(tr.temp_row, 2);

        SWMM_TransportMatrix m{};
        ASSERT_EQ(swmm_get_transport_matrix(r.e, &m), SWMM_OK);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_POLLUTANTS).state,
                  SWMM_TRANSPORT_ENABLED);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_POLLUTANTS).count, 1);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_NETWORK_1D, SWMM_TRANSPORT_CLASS_AGE).state,
                  SWMM_TRANSPORT_ENABLED);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_MSX).state,
                  SWMM_TRANSPORT_UNAVAILABLE);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_GROUNDWATER, SWMM_TRANSPORT_CLASS_POLLUTANTS).state,
                  SWMM_TRANSPORT_UNAVAILABLE);
        EXPECT_STREQ(cell(m, SWMM_TRANSPORT_DOMAIN_GROUNDWATER, SWMM_TRANSPORT_CLASS_POLLUTANTS).reason,
                     "no [AQUIFERS]");
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_RUNOFF, SWMM_TRANSPORT_CLASS_POLLUTANTS).state,
                  SWMM_TRANSPORT_UNAVAILABLE) << "no subcatchments in this deck";
        EXPECT_STREQ(swmm_transport_domain_name(SWMM_TRANSPORT_DOMAIN_SURFACE_2D), "2D surface");
        EXPECT_STREQ(swmm_transport_class_name(SWMM_TRANSPORT_CLASS_TEMPERATURE), "Temperature");
        EXPECT_STREQ(swmm_transport_domain_name(99), "");
        finish(r);
    }
    {
        DeckRun r = openDeck("rows_optout",
                         deck("WATER_AGE YES\nHEAT_TRANSPORT YES\n",
                              "TRANSPORT_POLLUTANTS NO\n", ""));
        ASSERT_TRUE(r.opened);
        SWMM_TransportMatrix m{};
        ASSERT_EQ(swmm_get_transport_matrix(r.e, &m), SWMM_OK);
        const auto& c2d = cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_POLLUTANTS);
        EXPECT_EQ(c2d.state, SWMM_TRANSPORT_DISABLED_BY_USER);
        EXPECT_STREQ(c2d.reason, "TRANSPORT_POLLUTANTS");
        // 1D is untouched by a 2D opt-out.
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_NETWORK_1D, SWMM_TRANSPORT_CLASS_POLLUTANTS).state,
                  SWMM_TRANSPORT_ENABLED);

        ASSERT_TRUE(startRun(r));
        const auto& tr = r.eng->surfaceRouter2D().state().transport;
        EXPECT_EQ(tr.n_pollut, 0);
        ASSERT_EQ(tr.row_names.size(), 2u);
        EXPECT_EQ(tr.row_names[0], "__WATER_AGE__");
        EXPECT_EQ(tr.row_names[1], "__TEMPERATURE__");
        EXPECT_EQ(tr.age_row, 0);
        EXPECT_EQ(tr.temp_row, 1);
        finish(r);
    }
    {
        // Everything off on the surface: no transport state at all.
        DeckRun r = openDeck("rows_alloff",
                         deck("WATER_AGE YES\n",
                              "TRANSPORT_POLLUTANTS NO\nTRANSPORT_AGE NO\n", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        const auto& tr = r.eng->surfaceRouter2D().state().transport;
        EXPECT_FALSE(tr.active());
        EXPECT_EQ(tr.row_names.size(), 0u);
        finish(r);
    }
}

// ---------------------------------------------------------------------------
// G4 — INFILTRATION switch
// ---------------------------------------------------------------------------
TEST(TransportPolicyProcesses, InfiltrationSwitch) {
    const std::string rows =
        "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n";
    double vol_on = 0.0, vol_off = 0.0;
    {
        DeckRun r = openDeck("infil_auto", deck("", "", rows));
        ASSERT_TRUE(r.opened);
        EXPECT_EQ(ext(r.e, "INFILTRATION"), "AUTO");
        EXPECT_EQ(ext(r.e, "INFIL_DEFAULT_METHOD"), "CONSTANT") << "derived from the '*' row";
        int ndef = 0;
        ASSERT_EQ(swmm_infil2d_defaults_count(r.e, &ndef), SWMM_OK);
        EXPECT_EQ(ndef, 1) << "AUTO resolves against these rows";
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        ASSERT_EQ(swmm_infil2d_get_total_volume(r.e, &vol_on), SWMM_OK);
        EXPECT_GT(vol_on, 0.0);
        finish(r);
    }
    {
        DeckRun r = openDeck("infil_no", deck("", "INFILTRATION NO\n", rows));
        ASSERT_TRUE(r.opened);
        EXPECT_EQ(ext(r.e, "INFILTRATION"), "NO");
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        ASSERT_EQ(swmm_infil2d_get_total_volume(r.e, &vol_off), SWMM_OK);
        EXPECT_DOUBLE_EQ(vol_off, 0.0);
        // The rows are still in the model (they save).
        SWMM_Infil2DRow row{};
        int is_override = -1;
        ASSERT_EQ(swmm_infil2d_get_cell(r.e, 0, &row, &is_override), SWMM_OK);
        EXPECT_EQ(row.has_method, 1);
        finish(r);
    }
    {
        // INFIL_DEFAULT_METHOD NONE drops the '*' row for the run.
        DeckRun r = openDeck("infil_none", deck("", "INFIL_DEFAULT_METHOD NONE\n", rows));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        double v = -1.0;
        ASSERT_EQ(swmm_infil2d_get_total_volume(r.e, &v), SWMM_OK);
        EXPECT_DOUBLE_EQ(v, 0.0);
        finish(r);
    }
    {
        // A conflicting method is an initialize error, not a silent switch.
        DeckRun r = openDeck("infil_conflict", deck("", "INFIL_DEFAULT_METHOD HORTON\n", rows));
        ASSERT_TRUE(r.opened);
        EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
}

// ---------------------------------------------------------------------------
// G5 — EVAPORATION switch
// ---------------------------------------------------------------------------
TEST(TransportPolicyProcesses, EvaporationSwitch) {
    auto evapOut = [](DeckRun& r) {
        double evap = -1.0;
        EXPECT_EQ(swmm_2d_get_mass_balance(r.e, nullptr, nullptr, nullptr, nullptr, nullptr,
                                           nullptr, nullptr, nullptr, nullptr, &evap), SWMM_OK);
        return evap;
    };
    {
        // YES (default): a forced uniform sink evaporates.
        DeckRun r = openDeck("evap_yes", deck("", "", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        ASSERT_EQ(swmm_2d_force_evap_uniform(r.e, 1.0e-5, SWMM_FORCING_OVERRIDE,
                                             SWMM_FORCING_PERSIST), SWMM_OK);
        stepAll(r);
        EXPECT_GT(evapOut(r), 0.0);
        finish(r);
    }
    {
        // NO: the same forcing is ignored — zero ledger row.
        DeckRun r = openDeck("evap_no", deck("", "EVAPORATION NO\n", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        ASSERT_EQ(swmm_2d_force_evap_uniform(r.e, 1.0e-5, SWMM_FORCING_OVERRIDE,
                                             SWMM_FORCING_PERSIST), SWMM_OK);
        stepAll(r);
        EXPECT_DOUBLE_EQ(evapOut(r), 0.0);
        finish(r);
    }
    {
        // CLIMATE: the project [EVAPORATION] rate drives the sink unforced.
        DeckRun r = openDeck("evap_climate",
                         deck("", "EVAPORATION CLIMATE\n",
                              "[EVAPORATION]\nCONSTANT 10.0\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        EXPECT_GT(evapOut(r), 0.0);
        finish(r);
    }
    {
        // YES without forcing and with a project rate: nothing (pre-E2 sink).
        DeckRun r = openDeck("evap_yes_climate_ignored",
                         deck("", "", "[EVAPORATION]\nCONSTANT 10.0\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        EXPECT_DOUBLE_EQ(evapOut(r), 0.0);
        finish(r);
    }
}

// ---------------------------------------------------------------------------
// G6 — the .rpt block
// ---------------------------------------------------------------------------
TEST(TransportPolicyReport, MatrixBlockPrintedForSpeciesDecksOnly) {
    {
        DeckRun r = openDeck("rpt_species", deck("WATER_AGE YES\n", "TRANSPORT_AGE NO\n", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        const fs::path rpt = r.rpt;
        finish(r);
        const std::string text = readAll(rpt);
        EXPECT_NE(text.find("Transport by domain"), std::string::npos) << text;
        EXPECT_NE(text.find("off:TRANSPORT_AGE"), std::string::npos) << text;
        EXPECT_NE(text.find("2D surface"), std::string::npos);
    }
    {
        DeckRun r = openDeck("rpt_hydraulics", deck("", "", "", /*pollut*/ ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        const fs::path rpt = r.rpt;
        finish(r);
        EXPECT_EQ(readAll(rpt).find("Transport by domain"), std::string::npos)
            << "a hydraulics-only report must be unchanged";
    }
}

// ---------------------------------------------------------------------------
// G7 — INFIL_DESTINATION fills unspelled rows
// ---------------------------------------------------------------------------
TEST(TransportPolicyProcesses, InfilDestinationAppliesToUnspelledRowsOnly) {
    const std::string rows =
        "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n";
    {
        // AQUIFER_2D is authoring-only: the row takes it and resolve refuses
        // it with the reserved-destination message, exactly like a spelled
        // DEST would.
        DeckRun r = openDeck("dest_aq2d", deck("", "INFIL_DESTINATION AQUIFER_2D\n", rows));
        ASSERT_TRUE(r.opened);
        EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
    {
        // A row that spells LOST keeps it.
        const std::string spelled =
            "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -  LOST\n\n";
        DeckRun r = openDeck("dest_spelled", deck("", "INFIL_DESTINATION AQUIFER_2D\n", spelled));
        ASSERT_TRUE(r.opened);
        EXPECT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
}

// ---------------------------------------------------------------------------
// The policy's canonical row helper agrees with the engines' enables.
// ---------------------------------------------------------------------------
TEST(TransportPolicyUnit, CanonicalRowsMatchEnables) {
    DeckRun r = openDeck("unit_rows", deck("WATER_AGE YES\nHEAT_TRANSPORT YES\n", "", ""));
    ASSERT_TRUE(r.opened);
    const auto& ctx = r.eng->context();
    const auto e1 = openswmm::transport::network1DEnables(ctx);
    EXPECT_EQ(e1.n_pollut, 1);
    EXPECT_TRUE(e1.age);
    EXPECT_TRUE(e1.temperature);
    const auto L = openswmm::transport::canonicalRows(ctx, e1);
    EXPECT_EQ(L.ns, 3);
    EXPECT_EQ(L.age_row, 1);
    EXPECT_EQ(L.temp_row, 2);
    ASSERT_EQ(L.names.size(), 3u);
    EXPECT_EQ(L.names[2], "__TEMPERATURE__");
    finish(r);
}
