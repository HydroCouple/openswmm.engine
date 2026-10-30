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
 * @file      test_transport_policy.cpp
 * @brief     E2 (2026-09-07) — TransportPolicy, swmm_get_transport_matrix, the
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

/**
 * @brief Read an extended option through a fixed local buffer.
 *
 * Uses a 512-byte zero-initialized buffer; does not expose distinct error codes or retry with a
 * larger allocation.
 *
 * @param[in] e Borrowed engine handle queried without ownership transfer.
 * @param[in] key Non-null option name accepted by swmm_options_get_ext.
 * @return Owned option text on success, or the literal &lt;err&gt; on any rejection.
 */
std::string ext(SWMM_Engine e, const char* key) {
    char buf[512] = {};
    if (swmm_options_get_ext(e, key, buf, sizeof buf) != SWMM_OK)
        return "<err>";
    return buf;
}

/**
 * @brief Build the two-triangle surface transport policy fixture.
 *
 * Raw rows are copied without escaping or validation; caller supplies valid line separators. No
 * parser or file IO is involved.
 *
 * @param[in] options_extra Raw rows appended inside OPTIONS.
 * @param[in] twod_options_extra Raw rows appended inside 2D_OPTIONS.
 * @param[in] extra_sections Raw complete sections appended before REPORT.
 * @param[in] pollut Raw pollutant section text; default declares Cu in mg/L.
 * @return Owned CMS deck with fixed geometry and simulation dates.
 */
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
      << twod_options_extra
      << "\n"
         "[2D_VERTICES]\n"
         " 0.0  0.0 -10.0\n10.0  0.0 -10.0\n10.0 10.0 -10.0\n 0.0 10.0 -10.0\n\n"
         "[2D_TRIANGLES]\n;;V1 V2 V3 N INIT_DEPTH\n"
         "0 1 2 0.03 0.5\n0 2 3 0.03 0.5\n\n"
         "[2D_VERTEX_NODE_MAP]\n0 O1 0.7 1.0\n\n"
      << extra_sections << "[REPORT]\nINPUT NO\n";
    return m.str();
}

struct DeckRun {
    SWMM_Engine e = nullptr;
    openswmm::SWMMEngine* eng = nullptr;
    fs::path rpt;
    bool opened = false, started = false;
};

/**
 * @brief Write and open a transport policy input for later inspection.
 *
 * Creates kOutDir, truncates input, and leaves init/start to caller. Stream failures are unchecked;
 * filesystem exceptions propagate. Null e reports allocation failure; every non-null e requires
 * finish.
 *
 * @param[in] tag Artifact stem below kOutDir; matching input/report/result files may be
 *     overwritten.
 * @param[in] body Borrowed full input text, copied verbatim.
 * @return Record owning e even after open rejection; eng borrows e, and opened reports only open
 *     success.
 */
DeckRun openDeck(const std::string& tag, const std::string& body) {
    fs::create_directories(kOutDir);
    DeckRun r;
    const fs::path inp = kOutDir / (tag + ".inp");
    r.rpt = kOutDir / (tag + ".rpt");
    {
        std::ofstream f(inp);
        f << body;
    }
    r.e = swmm_engine_create();
    if (!r.e)
        return r;
    r.opened = swmm_engine_open(r.e, inp.string().c_str(), r.rpt.string().c_str(),
                                (kOutDir / (tag + ".out")).string().c_str(), nullptr) == SWMM_OK;
    r.eng = static_cast<openswmm::SWMMEngine*>(r.e);
    return r;
}

/**
 * @brief Initialize and start an opened fixture with saved results.
 *
 * Does not step/end or release the handle. Failed setup can leave partial state requiring finish.
 *
 * @param[in,out] r Caller-owned record; engine is mutated and started is set only after successful
 *     start.
 * @return True on successful open/init/start; false on first rejection.
 */
bool startRun(DeckRun& r) {
    if (!r.opened)
        return false;
    if (swmm_engine_initialize(r.e) != SWMM_OK)
        return false;
    if (swmm_engine_start(r.e, 1) != SWMM_OK)
        return false;
    r.started = true;
    return true;
}

/**
 * @brief Step the started fixture until completion or the first rejected step.
 *
 * Loops while the step status is SWMM_OK and elapsed is positive. Does not assert or return
 * rejection status and has no iteration guard; callers must not infer success from return alone.
 * Does not end or release.
 *
 * @param[in,out] r Caller-owned started record whose engine is advanced in place.
 */
void stepAll(DeckRun& r) {
    double elapsed = 0.0;
    while (swmm_engine_step(r.e, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
}

/**
 * @brief Release the fixture engine after optional end/report.
 *
 * When started, calls end and then report; ignores statuses, closes and destroys. eng becomes
 * dangling while opened/started remain unchanged; no artifact deletion occurs.
 *
 * @param[in,out] r Record owning e; null e is a no-op and released e is reset to null.
 */
void finish(DeckRun& r) {
    if (!r.e)
        return;
    if (r.started)
        swmm_engine_end(r.e);
    if (r.started)
        swmm_engine_report(r.e);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
    r.e = nullptr;
}

/**
 * @brief Borrow one unchecked transport-matrix entry.
 *
 * No bounds checking or copy is performed.
 *
 * @param[in] m Borrowed matrix whose lifetime must cover use of the returned reference.
 * @param[in] d Valid domain index in m.cell.
 * @param[in] c Valid constituent index in the selected row.
 * @return Const reference to m.cell[d][c], valid until m is destroyed or modified.
 */
const SWMM_TransportCell& cell(const SWMM_TransportMatrix& m, int d, int c) {
    return m.cell[d][c];
}

/**
 * @brief Find a line containing exactly two whitespace-separated expected tokens.
 *
 * Does not strip comments, normalize case or accept a trailing third token. Input text is
 * unchanged.
 *
 * @param[in] text Borrowed multiline text.
 * @param[in] key Exact case-sensitive first token.
 * @param[in] value Exact case-sensitive second token.
 * @return True for a line matching both tokens with no third token; false otherwise.
 */
bool hasKeyValue(const std::string& text, const std::string& key, const std::string& value) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k, v, rest;
        if (!(ls >> k >> v))
            continue;
        if (ls >> rest)
            continue;
        if (k == key && v == value)
            return true;
    }
    return false;
}

/**
 * @brief Find a line whose first whitespace-separated token matches a key.
 *
 * Does not interpret section boundaries or strip comments; input text is unchanged.
 *
 * @param[in] text Borrowed multiline text.
 * @param[in] key Exact case-sensitive first token.
 * @return True when a line begins with key after whitespace; subsequent tokens are ignored.
 */
bool hasKey(const std::string& text, const std::string& key) {
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream ls(line);
        std::string k;
        if ((ls >> k) && k == key)
            return true;
    }
    return false;
}

/**
 * @brief Copy the first matching section body for option assertions.
 *
 * Only lines beginning with [ in column zero are headers. Stops at the next such header after a
 * match; matching uses prefix comparison, not complete-line equality. No input mutation occurs.
 *
 * @param[in] text Borrowed input text, split into lines.
 * @param[in] header Exact case-sensitive section-header prefix, such as [2D_OPTIONS].
 * @return Owned body with newline appended per retained line, excluding the header; empty when
 *     unmatched.
 */
std::string section(const std::string& text, const std::string& header) {
    std::istringstream in(text);
    std::string line, out;
    bool in_sec = false;
    while (std::getline(in, line)) {
        if (!line.empty() && line[0] == '[') {
            if (in_sec)
                break;
            in_sec = line.rfind(header, 0) == 0;
            continue;
        }
        if (in_sec)
            out += line + "\n";
    }
    return out;
}

} // namespace

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
    EXPECT_EQ(ext(e, "INFILTRATION"), "AUTO"); // unset ⇒ AUTO (on iff rows resolve)
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
    EXPECT_TRUE(hasKeyValue(text, "INFIL_STEP", "0:02:00")) << text; // fmt_step spelling
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
        DeckRun r = openDeck("rows_default", deck("WATER_AGE YES\nHEAT_TRANSPORT YES\n", "", ""));
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
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_POLLUTANTS).count,
                  1);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_NETWORK_1D, SWMM_TRANSPORT_CLASS_AGE).state,
                  SWMM_TRANSPORT_ENABLED);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_MSX).state,
                  SWMM_TRANSPORT_UNAVAILABLE);
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_GROUNDWATER, SWMM_TRANSPORT_CLASS_POLLUTANTS).state,
                  SWMM_TRANSPORT_UNAVAILABLE);
        // T7.1: either section can give this deck a groundwater row now —
        // the legacy subcatchment [AQUIFERS] (which still transports
        // nothing) or the two-zone [2D_AQUIFER] (which does), so the
        // "there is none" reason names both.
        EXPECT_STREQ(
            cell(m, SWMM_TRANSPORT_DOMAIN_GROUNDWATER, SWMM_TRANSPORT_CLASS_POLLUTANTS).reason,
            "no [AQUIFERS] or [2D_AQUIFER]");
        EXPECT_EQ(cell(m, SWMM_TRANSPORT_DOMAIN_RUNOFF, SWMM_TRANSPORT_CLASS_POLLUTANTS).state,
                  SWMM_TRANSPORT_UNAVAILABLE)
            << "no subcatchments in this deck";
        EXPECT_STREQ(swmm_transport_domain_name(SWMM_TRANSPORT_DOMAIN_SURFACE_2D), "2D surface");
        EXPECT_STREQ(swmm_transport_class_name(SWMM_TRANSPORT_CLASS_TEMPERATURE), "Temperature");
        EXPECT_STREQ(swmm_transport_domain_name(99), "");
        finish(r);
    }
    {
        DeckRun r = openDeck("rows_optout", deck("WATER_AGE YES\nHEAT_TRANSPORT YES\n",
                                                 "TRANSPORT_POLLUTANTS NO\n", ""));
        ASSERT_TRUE(r.opened);
        SWMM_TransportMatrix m{};
        ASSERT_EQ(swmm_get_transport_matrix(r.e, &m), SWMM_OK);
        const auto& c2d =
            cell(m, SWMM_TRANSPORT_DOMAIN_SURFACE_2D, SWMM_TRANSPORT_CLASS_POLLUTANTS);
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
        DeckRun r =
            openDeck("rows_alloff",
                     deck("WATER_AGE YES\n", "TRANSPORT_POLLUTANTS NO\nTRANSPORT_AGE NO\n", ""));
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
    const std::string rows = "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n";
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
                                           nullptr, nullptr, nullptr, nullptr, &evap),
                  SWMM_OK);
        return evap;
    };
    {
        // YES (default): a forced uniform sink evaporates.
        DeckRun r = openDeck("evap_yes", deck("", "", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        ASSERT_EQ(
            swmm_2d_force_evap_uniform(r.e, 1.0e-5, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
            SWMM_OK);
        stepAll(r);
        EXPECT_GT(evapOut(r), 0.0);
        finish(r);
    }
    {
        // NO: the same forcing is ignored — zero ledger row.
        DeckRun r = openDeck("evap_no", deck("", "EVAPORATION NO\n", ""));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        ASSERT_EQ(
            swmm_2d_force_evap_uniform(r.e, 1.0e-5, SWMM_FORCING_OVERRIDE, SWMM_FORCING_PERSIST),
            SWMM_OK);
        stepAll(r);
        EXPECT_DOUBLE_EQ(evapOut(r), 0.0);
        finish(r);
    }
    {
        // CLIMATE: the project [EVAPORATION] rate drives the sink unforced.
        DeckRun r = openDeck("evap_climate",
                             deck("", "EVAPORATION CLIMATE\n", "[EVAPORATION]\nCONSTANT 10.0\n\n"));
        ASSERT_TRUE(r.opened);
        ASSERT_TRUE(startRun(r));
        stepAll(r);
        EXPECT_GT(evapOut(r), 0.0);
        finish(r);
    }
    {
        // YES without forcing and with a project rate: nothing (pre-E2 sink).
        DeckRun r =
            openDeck("evap_yes_climate_ignored", deck("", "", "[EVAPORATION]\nCONSTANT 10.0\n\n"));
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
    const std::string rows = "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -\n\n";
    {
        // The default reaches unspelled rows. This deck has no subcatchment
        // aquifer, so resolving that destination must fail.
        DeckRun r = openDeck("dest_aq2d", deck("", "INFIL_DESTINATION SUBCATCH_AQUIFER\n", rows));
        ASSERT_TRUE(r.opened);
        EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
    {
        // A row that spells LOST keeps it.
        const std::string spelled =
            "[2D_INFILTRATION_DEFAULTS]\n*  CONSTANT  50.0  -  -  -  -  LOST\n\n";
        DeckRun r =
            openDeck("dest_spelled", deck("", "INFIL_DESTINATION SUBCATCH_AQUIFER\n", spelled));
        ASSERT_TRUE(r.opened);
        EXPECT_EQ(swmm_engine_initialize(r.e), SWMM_OK);
        swmm_engine_close(r.e);
        swmm_engine_destroy(r.e);
    }
}

TEST(TransportPolicyProcesses, ObsoleteAquiferDestinationIsRejectedEvenWithExplicitLostRows) {
    const std::string spelled = "[2D_INFILTRATION_DEFAULTS]\n* CONSTANT 50.0 - - - - LOST\n\n";
    DeckRun r = openDeck("dest_obsolete", deck("", "INFIL_DESTINATION AQUIFER_2D\n", spelled));
    ASSERT_TRUE(r.opened);
    EXPECT_NE(swmm_engine_initialize(r.e), SWMM_OK);
    EXPECT_NE(r.eng->context().error_message.find("obsolete"), std::string::npos);
    swmm_engine_close(r.e);
    swmm_engine_destroy(r.e);
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
