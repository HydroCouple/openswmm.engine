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
 * @file test_gwf_api.cpp
 * @brief [GWF] custom groundwater flow expression C API: set/get/clear,
 *        InpWriter round-trip, the non-mutating validator, the parser's
 *        LAT alias + case-insensitive subcatchment lookup, load/initialize
 *        rejection of malformed expressions, and the editor vocabulary.
 *
 * @details Phase E1 of
 *          openswmm.gui/workplans/AQUIFER_GROUNDWATER_EXCHANGE_GUI_PLAN_2026-09-05.md.
 *          Fixture models are written next to the other fixtures under
 *          tests/unit/engine/data/gwf_api/ with the `_` prefix .gitignore
 *          excludes, never to a temporary directory (CLAUDE.md 4.1).
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace fs = std::filesystem;

namespace {

const fs::path kArtifactDir = "gwf_api";

void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    f << text;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

/// Two groundwater subcatchments, one with a mixed-case name so the [GWF]
/// lookup / writer keying is observable. `gwf` is appended verbatim.
std::string gw_model(const std::string& gwf = "") {
    return
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         KINWAVE\n"
        "INFILTRATION         HORTON\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
        "REPORT_STEP          00:05:00\n"
        "WET_STEP             00:05:00\n"
        "DRY_STEP             00:05:00\n"
        "ROUTING_STEP         0:00:30\n"
        "\n[RAINGAGES]\n"
        "RG1  INTENSITY 1:00 1.0 TIMESERIES TS1\n"
        "\n[SUBCATCHMENTS]\n"
        "S1    RG1  J1  2.0  25  400  1.0  0\n"
        "Sub2  RG1  J1  3.0  40  500  1.0  0\n"
        "\n[SUBAREAS]\n"
        "S1    0.01  0.1  0.05  0.05  25  OUTLET\n"
        "Sub2  0.01  0.1  0.05  0.05  25  OUTLET\n"
        "\n[INFILTRATION]\n"
        "S1    3.0  0.5  4.0  7  0\n"
        "Sub2  3.0  0.5  4.0  7  0\n"
        "\n[AQUIFERS]\n"
        ";;Name Por  WP    FC    Ksat Kslope Tslope ETu  ETs Seep  Ebot Egw  Umc\n"
        "AQ1    0.5  0.15  0.30  5.0  10.0   15.0   0.35 0.0 0.002 0.0  10.0 0.30\n"
        "\n[GROUNDWATER]\n"
        ";;Subcatch Aquifer Node SurfElev A1     B1  A2  B2  A3  Tw  Hstar\n"
        "S1         AQ1     J1   12.0     0.001  1.0 0.0 0.0 0.0 0.0 0.0\n"
        "Sub2       AQ1     J1   12.0     0.002  1.0 0.0 0.0 0.0 0.0 0.0\n"
        + gwf +
        "\n[JUNCTIONS]\n"
        "J1  10.0  5.0  0  0  0\n"
        "\n[OUTFALLS]\n"
        "O1  8.0  FREE\n"
        "\n[CONDUITS]\n"
        "C1  J1  O1  400.0  0.013  0  0\n"
        "\n[XSECTIONS]\n"
        "C1  CIRCULAR  1.5  0  0  0  1\n"
        "\n[TIMESERIES]\n"
        "TS1  01/01/2026 00:00 0.5\n"
        "TS1  01/01/2026 01:00 0.0\n";
}

std::string get_expr(SWMM_Engine e, int idx, int type) {
    char buf[256] = {};
    EXPECT_EQ(swmm_subcatch_get_gwf_expression(e, idx, type, buf, sizeof(buf)), SWMM_OK);
    return buf;
}

} // namespace

class GwfApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    /// Writes `text` as <tag>.inp and opens it; `expect_rc` lets a test assert
    /// on a rejected open without the fixture failing first.
    void open_model(const std::string& tag, const std::string& text = gw_model(),
                    int expect_rc = SWMM_OK) {
        const fs::path inp = kArtifactDir / ("_" + tag + ".inp");
        write_file(inp, text);
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, inp.string().c_str(),
                                   (kArtifactDir / ("_" + tag + ".rpt")).string().c_str(),
                                   (kArtifactDir / ("_" + tag + ".out")).string().c_str(),
                                   nullptr), expect_rc)
            << swmm_get_last_error_msg(engine);
    }

    std::string written_model(const std::string& tag) {
        const fs::path p = kArtifactDir / ("_" + tag + "_gwf_out.inp");
        EXPECT_EQ(swmm_model_write(engine, p.string().c_str()), SWMM_OK);
        return read_file(p);
    }

    void reopen_from(const std::string& tag) {
        const fs::path p = kArtifactDir / ("_" + tag + "_gwf_out.inp");
        swmm_engine_destroy(engine);
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, p.string().c_str(),
                                   (kArtifactDir / ("_" + tag + "_gwf_r.rpt")).string().c_str(),
                                   (kArtifactDir / ("_" + tag + "_gwf_r.out")).string().c_str(),
                                   nullptr), SWMM_OK)
            << swmm_get_last_error_msg(engine);
    }

    void TearDown() override {
        if (engine) swmm_engine_destroy(engine);
    }
};

// ============================================================================
// set / get / clear
// ============================================================================

TEST_F(GwfApiTest, SetGetClearRoundTrip) {
    open_model("set_get");
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_GE(s1, 0);

    // No [GWF] section: both slots read back empty.
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_LATERAL), "");
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_DEEP), "");

    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_LATERAL,
                                               "0.001*(Hgw-10)"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_DEEP,
                                               "0.0005 * HGW"), SWMM_OK);
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_LATERAL), "0.001*(Hgw-10)");   // stored as typed
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_DEEP), "0.0005 * HGW");

    // Truncation: NUL-terminated within buflen.
    char small[6] = {};
    ASSERT_EQ(swmm_subcatch_get_gwf_expression(engine, s1, SWMM_GWF_LATERAL,
                                               small, sizeof(small)), SWMM_OK);
    EXPECT_STREQ(small, "0.001");

    // NULL clears one slot, "" clears the other; the untouched slot is kept.
    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_LATERAL, nullptr), SWMM_OK);
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_LATERAL), "");
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_DEEP), "0.0005 * HGW");
    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_DEEP, ""), SWMM_OK);
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_DEEP), "");

    // set() does not validate — an editor validates first, the engine
    // rejects at initialize() (see InvalidExpressionSetViaApiFailsInitialize).
    EXPECT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_LATERAL, "Hgww"), SWMM_OK);
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_LATERAL), "Hgww");

    // Argument checking.
    char buf[16] = {};
    EXPECT_EQ(swmm_subcatch_get_gwf_expression(engine, s1, 2, buf, sizeof(buf)), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, -1, "HGW"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_gwf_expression(engine, s1, SWMM_GWF_DEEP, nullptr, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_subcatch_get_gwf_expression(engine, 99, SWMM_GWF_DEEP, buf, sizeof(buf)), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_subcatch_set_gwf_expression(engine, 99, SWMM_GWF_DEEP, "HGW"), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_subcatch_get_gwf_expression(nullptr, s1, SWMM_GWF_DEEP, buf, sizeof(buf)), SWMM_ERR_BADHANDLE);
}

TEST_F(GwfApiTest, SetIsPreStartOnly) {
    open_model("prestart");
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_GE(s1, 0);
    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK) << swmm_get_last_error_msg(engine);
    EXPECT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_LATERAL, "HGW"),
              SWMM_ERR_LIFECYCLE);
    // Reads stay available.
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_LATERAL), "");
}

// ============================================================================
// InpWriter round-trip
// ============================================================================

TEST_F(GwfApiTest, ExpressionSurvivesInpWriterRoundTrip) {
    open_model("writer_rt");
    const int s1 = swmm_subcatch_index(engine, "S1");
    const int s2 = swmm_subcatch_index(engine, "Sub2");
    ASSERT_GE(s1, 0); ASSERT_GE(s2, 0);

    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_LATERAL,
                                               "0.001 * (Hgw - 10.0)"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s2, SWMM_GWF_DEEP,
                                               "MIN(HGW, HCB) * KS"), SWMM_OK);

    const std::string text = written_model("writer_rt");
    EXPECT_NE(text.find("[GWF]"), std::string::npos);
    EXPECT_NE(text.find("0.001 * (Hgw - 10.0)"), std::string::npos);
    EXPECT_NE(text.find("MIN(HGW, HCB) * KS"), std::string::npos);
    // The key was built from the registry spelling, so the writer finds it.
    EXPECT_NE(text.find("Sub2"), std::string::npos);
    EXPECT_EQ(text.find("GWF:"), std::string::npos) << "GWF key leaked into [OPTIONS]";

    reopen_from("writer_rt");
    const int r1 = swmm_subcatch_index(engine, "S1");
    const int r2 = swmm_subcatch_index(engine, "Sub2");
    ASSERT_GE(r1, 0); ASSERT_GE(r2, 0);
    EXPECT_EQ(get_expr(engine, r1, SWMM_GWF_LATERAL), "0.001 * (Hgw - 10.0)");
    EXPECT_EQ(get_expr(engine, r1, SWMM_GWF_DEEP), "");
    EXPECT_EQ(get_expr(engine, r2, SWMM_GWF_LATERAL), "");
    EXPECT_EQ(get_expr(engine, r2, SWMM_GWF_DEEP), "MIN(HGW, HCB) * KS");
}

// ============================================================================
// Validator
// ============================================================================

TEST_F(GwfApiTest, ValidateExpressionAcceptsAndRejects) {
    open_model("validate");
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_GE(s1, 0);

    char err[256] = {};
    int col = -2;

    // Valid: manual example, 2-arg function with upper-case name, unary
    // minus + power + step().
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "0.001*(Hgw-10)", err, sizeof(err), &col), SWMM_OK);
    EXPECT_STREQ(err, "");
    EXPECT_EQ(col, -1);
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "MIN(HGW, HCB) * KS", err, sizeof(err), &col), SWMM_OK);
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "-HGW^2 + step(HGW-HCB)", err, sizeof(err), &col), SWMM_OK);

    // Unknown identifier, column at the identifier.
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "Hgww", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("Hgww"), std::string::npos) << err;
    EXPECT_EQ(col, 0);
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "2 * Hgww", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_EQ(col, 4);

    // Unbalanced parenthesis.
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "log(", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_NE(std::strlen(err), 0u);
    EXPECT_GE(col, 0);

    // Wrong arity.
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "min(HGW)", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("min"), std::string::npos) << err;
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "sqrt(HGW, 2)", err, sizeof(err), &col), SWMM_ERR_BADPARAM);

    // Missing operator, column at the second operand.
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "2 HGW", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_EQ(col, 2);

    // Unknown character (the runtime tokenizer would silently skip it).
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "HGW $ 2", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_EQ(col, 4);

    // Empty / whitespace-only is invalid — callers treat empty as "clear".
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("empty"), std::string::npos) << err;
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "   ", err, sizeof(err), &col), SWMM_ERR_BADPARAM);

    // Trailing operator / NULL expression / optional outputs.
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "HGW +", err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_gwf_validate_expression(engine, nullptr, err, sizeof(err), &col), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "Hgww", nullptr, 0, nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_gwf_validate_expression(engine, "HGW", nullptr, 0, nullptr), SWMM_OK);

    // Non-mutating: nothing was stored.
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_LATERAL), "");
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_DEEP), "");
}

// ============================================================================
// Parser: LAT alias, case-insensitive subcatchment, rejection on load
// ============================================================================

TEST_F(GwfApiTest, LoadAcceptsLatKeywordAndMixedCaseSubcatchName) {
    open_model("lat_alias", gw_model(
        "\n[GWF]\n"
        "SUB2  LAT   0.5*HGW\n"
        "s1    DEEP  0.0005 * Hgw\n"));

    const int s1 = swmm_subcatch_index(engine, "S1");
    const int s2 = swmm_subcatch_index(engine, "Sub2");
    ASSERT_GE(s1, 0); ASSERT_GE(s2, 0);
    EXPECT_EQ(get_expr(engine, s2, SWMM_GWF_LATERAL), "0.5*HGW");
    EXPECT_EQ(get_expr(engine, s1, SWMM_GWF_DEEP), "0.0005 * Hgw");

    // Written back with the canonical keyword and the registry spelling.
    const std::string text = written_model("lat_alias");
    EXPECT_NE(text.find("Sub2"), std::string::npos);
    EXPECT_NE(text.find("LATERAL"), std::string::npos);
    EXPECT_EQ(text.find("SUB2"), std::string::npos);
}

TEST_F(GwfApiTest, LoadRejectsMalformedExpression) {
    open_model("bad_load", gw_model("\n[GWF]\nS1  LATERAL  Hgww * 2\n"), SWMM_ERR_PARSE);
    const std::string msg = swmm_get_last_error_msg(engine);
    EXPECT_NE(msg.find("233"), std::string::npos) << msg;
    EXPECT_NE(msg.find("Hgww"), std::string::npos) << msg;
}

TEST_F(GwfApiTest, InvalidExpressionSetViaApiFailsInitialize) {
    open_model("bad_init");
    const int s1 = swmm_subcatch_index(engine, "S1");
    ASSERT_GE(s1, 0);
    ASSERT_EQ(swmm_subcatch_set_gwf_expression(engine, s1, SWMM_GWF_DEEP, "log("), SWMM_OK);
    EXPECT_EQ(swmm_engine_initialize(engine), SWMM_ERR_PARSE);
    const std::string msg = swmm_get_last_error_msg(engine);
    EXPECT_NE(msg.find("233"), std::string::npos) << msg;
    EXPECT_NE(msg.find("DEEP"), std::string::npos) << msg;
}

// ============================================================================
// Editor vocabulary
// ============================================================================

TEST_F(GwfApiTest, VocabularyListsVariablesAndFunctions) {
    open_model("vocab");
    char buf[128] = {};

    ASSERT_EQ(swmm_gwf_variable_count(engine), 11);
    ASSERT_EQ(swmm_gwf_variable_name(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "HGW");
    ASSERT_EQ(swmm_gwf_variable_name(engine, 10, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "A");
    for (int i = 0; i < 11; ++i) {
        ASSERT_EQ(swmm_gwf_variable_description(engine, i, buf, sizeof(buf)), SWMM_OK) << i;
        EXPECT_NE(std::strlen(buf), 0u) << "variable " << i << " has no description";
    }
    EXPECT_EQ(swmm_gwf_variable_name(engine, 11, buf, sizeof(buf)), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_gwf_variable_description(engine, -1, buf, sizeof(buf)), SWMM_ERR_BADINDEX);

    ASSERT_EQ(swmm_gwf_function_count(engine), 21);
    ASSERT_EQ(swmm_gwf_function_name(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "abs");
    ASSERT_EQ(swmm_gwf_function_name(engine, 12, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "min");
    ASSERT_EQ(swmm_gwf_function_name(engine, 20, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "acot");
    EXPECT_EQ(swmm_gwf_function_name(engine, 21, buf, sizeof(buf)), SWMM_ERR_BADINDEX);

    EXPECT_EQ(swmm_gwf_variable_count(nullptr), -1);
    EXPECT_EQ(swmm_gwf_function_count(nullptr), -1);
}
