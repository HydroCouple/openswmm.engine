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
 * @file test_reactions_api.cpp
 * @brief E-C1 gates: reaction-system validation + discovery C API — the GUI
 *        completer/highlighter contract.
 *
 * @details Falsifiers:
 *          - Discovery round-trip fails while the symbols don't exist (link
 *            failure) or any surfaced value differs from the parsed config.
 *          - Function enumeration cross-checks the compiler by feeding a
 *            wrong-arity call of EVERY enumerated function to
 *            validate_expression — an enumerated arity the compiler doesn't
 *            enforce fails here (the drift guard is exercised, not assumed).
 *          - Validation gates pin OK/arity/undefined-identifier outcomes
 *            with real columns.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_reactions.h>

namespace {

void write_files() {
    {
        std::ofstream c("_rxapi.rxn");
        c << "[REACTION_OPTIONS]\nSOLVER BDF2\nRATE_UNITS HR\nATOL 1e-8\n"
          << "[REACTION_SPECIES]\nBULK HOCL MG\nWALL WALLP MG 1e-7 1e-5\n"
          << "[REACTION_COEFFICIENTS]\nPARAMETER k1 0.36\nCONSTANT kb 0.12\n"
          << "[REACTION_TERMS]\nAMM 0.05 * HOCL\n"
          << "[REACTION_PIPES]\nRATE HOCL -k1 * HOCL\n";
    }
    {
        std::ofstream f("_rxapi.inp");
        f << "[TITLE]\nE-C1 api deck\n\n"
          << "[OPTIONS]\n"
          << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
          << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
          << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n\n"
          << "[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
          << "J0     10.0 10 0.5 0 0\n\n"
          << "[OUTFALLS]\n;;Name Elev Type StageData Gated\nOUT 7.0 FREE  NO\n\n"
          << "[CONDUITS]\n;;Name From To Length N Zin Zout Q0\n"
          << "C1 J0 OUT 400 0.013 0 0 0\n\n"
          << "[XSECTIONS]\n;;Link Shape G1 G2 G3 G4\nC1 CIRCULAR 1.5 0 0 0\n\n"
          << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac "
             "Cdwf Cinit\n"
          << "TSS    MG/L  0.0   0.0 0.0   0.0    NO       *        0.0    "
             "0.0  0.0\n\n"
          << "[PROCESS_COMPONENTS]\n"
          << "org.hydrocouple.openswmm.reactions  config=\"_rxapi.rxn\"\n\n"
          << "[REPORT]\nINPUT NO\n";
    }
}

class ReactionsApiTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
        write_files();
        ASSERT_EQ(swmm_engine_open(engine_, "_rxapi.inp", "_rxapi.rpt",
                                   nullptr, nullptr),
                  SWMM_OK);
    }
    void TearDown() override {
        if (engine_) swmm_engine_destroy(engine_);
        std::remove("_rxapi.inp");
        std::remove("_rxapi.rxn");
    }
    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
TEST_F(ReactionsApiTest, DiscoveryRoundTrip) {
    ASSERT_EQ(swmm_reaction_species_count(engine_), 2);
    char name[64], units[32], expr[256];
    int is_wall = -1, is_param = -1, form = -1;
    double atol = 0, rtol = 0, value = 0;

    ASSERT_EQ(swmm_reaction_species_get(engine_, 0, name, 64, &is_wall,
                                        units, 32, &atol, &rtol), SWMM_OK);
    EXPECT_STREQ(name, "HOCL");
    EXPECT_EQ(is_wall, 0);
    EXPECT_STREQ(units, "MG");
    ASSERT_EQ(swmm_reaction_species_get(engine_, 1, name, 64, &is_wall,
                                        units, 32, &atol, &rtol), SWMM_OK);
    EXPECT_STREQ(name, "WALLP");
    EXPECT_EQ(is_wall, 1);
    EXPECT_DOUBLE_EQ(atol, 1e-7);

    ASSERT_EQ(swmm_reaction_coeff_count(engine_), 2);
    ASSERT_EQ(swmm_reaction_coeff_get(engine_, 0, name, 64, &is_param,
                                      &value), SWMM_OK);
    EXPECT_STREQ(name, "k1");
    EXPECT_EQ(is_param, 1);
    EXPECT_DOUBLE_EQ(value, 0.36);

    ASSERT_EQ(swmm_reaction_term_count(engine_), 1);
    ASSERT_EQ(swmm_reaction_term_get(engine_, 0, name, 64, expr, 256),
              SWMM_OK);
    EXPECT_STREQ(name, "AMM");
    EXPECT_STREQ(expr, "0.05 * HOCL");

    ASSERT_EQ(swmm_reaction_expr_get(engine_, SWMM_RXN_SCOPE_PIPE, 0, &form,
                                     expr, 256), SWMM_OK);
    EXPECT_EQ(form, SWMM_RXN_FORM_RATE);
    EXPECT_STREQ(expr, "-k1 * HOCL");
    ASSERT_EQ(swmm_reaction_expr_get(engine_, SWMM_RXN_SCOPE_TANK, 0, &form,
                                     expr, 256), SWMM_OK);
    EXPECT_EQ(form, SWMM_RXN_FORM_NONE);

    char v[32];
    ASSERT_EQ(swmm_reaction_option_get(engine_, "SOLVER", v, 32), SWMM_OK);
    EXPECT_STREQ(v, "BDF2");
    ASSERT_EQ(swmm_reaction_option_get(engine_, "RATE_UNITS", v, 32),
              SWMM_OK);
    EXPECT_STREQ(v, "HR");
    EXPECT_EQ(swmm_reaction_option_get(engine_, "NOPE", v, 32),
              SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// The function vocabulary IS what the compiler enforces: for every
// enumerated function, a call with arity+1 arguments must fail validation
// with a message naming that function. An enumerated function the compiler
// does not know (or an arity it does not enforce) breaks this loop.
TEST_F(ReactionsApiTest, FunctionVocabularyMatchesTheCompiler) {
    const int nf = swmm_reaction_function_count();
    EXPECT_EQ(nf, 13);
    int n_unary = 0, n_binary = 0;
    for (int i = 0; i < nf; ++i) {
        char name[32];
        int arity = 0;
        ASSERT_EQ(swmm_reaction_function_get(i, name, 32, &arity), SWMM_OK);
        (arity == 1 ? n_unary : n_binary)++;

        std::string call = std::string(name) + "(1";
        for (int a = 1; a < arity + 1; ++a) call += ", 1";
        call += ")";
        char err[256];
        int col = -1;
        EXPECT_EQ(swmm_reaction_validate_expression(
                      engine_, SWMM_RXN_SCOPE_PIPE, call.c_str(), err, 256,
                      &col),
                  SWMM_ERR_BADPARAM)
            << call << " must over-feed " << name;
        EXPECT_NE(std::string(err).find(name), std::string::npos)
            << call << " → '" << err << "'";
    }
    EXPECT_EQ(n_unary, 10);
    EXPECT_EQ(n_binary, 3);

    const int nh = swmm_reaction_hydvar_count();
    EXPECT_EQ(nh, 9);
    char hname[16], hdesc[128];
    ASSERT_EQ(swmm_reaction_hydvar_get(0, hname, 16, hdesc, 128), SWMM_OK);
    EXPECT_STREQ(hname, "D");
    EXPECT_NE(std::string(hdesc).find("Depth"), std::string::npos);
    EXPECT_EQ(swmm_reaction_hydvar_get(nh, hname, 16, hdesc, 128),
              SWMM_ERR_BADINDEX);
}

// ---------------------------------------------------------------------------
TEST_F(ReactionsApiTest, ValidationOutcomes) {
    char err[256];
    int col = -1;

    // OK: species, coefficient, pollutant (read-only), hydvar, 2-arg func.
    EXPECT_EQ(swmm_reaction_validate_expression(
                  engine_, SWMM_RXN_SCOPE_PIPE,
                  "MIN(HOCL, 2) * k1 + TSS * D", err, 256, &col),
              SWMM_OK) << err;

    // Term scope accepts references to ALL terms (D-RC1).
    EXPECT_EQ(swmm_reaction_validate_expression(
                  engine_, SWMM_RXN_SCOPE_TERM, "AMM * 2", err, 256, &col),
              SWMM_OK) << err;

    // Arity error names the function and carries a column (E-D1 wiring).
    EXPECT_EQ(swmm_reaction_validate_expression(
                  engine_, SWMM_RXN_SCOPE_PIPE, "MIN(HOCL)", err, 256, &col),
              SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("MIN"), std::string::npos) << err;
    EXPECT_GT(col, 0);

    // Undefined identifier points at itself.
    EXPECT_EQ(swmm_reaction_validate_expression(
                  engine_, SWMM_RXN_SCOPE_PIPE, "1 + NOPE", err, 256, &col),
              SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("NOPE"), std::string::npos) << err;
    EXPECT_EQ(col, 5);

    // Bad scope refused.
    EXPECT_EQ(swmm_reaction_validate_expression(engine_, 99, "1", err, 256,
                                                &col),
              SWMM_ERR_BADPARAM);
}

}  // namespace
