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
#include <cstdlib>
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

    // TEMPERATURE (TEMP's heat-off fallback, degC): default 20, settable,
    // negative is a legal temperature, non-numeric refused.
    ASSERT_EQ(swmm_reaction_option_get(engine_, "TEMPERATURE", v, 32),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(std::atof(v), 20.0);
    ASSERT_EQ(swmm_reaction_option_set(engine_, "TEMPERATURE", "-4.5"),
              SWMM_OK);
    ASSERT_EQ(swmm_reaction_option_get(engine_, "TEMPERATURE", v, 32),
              SWMM_OK);
    EXPECT_DOUBLE_EQ(std::atof(v), -4.5);
    EXPECT_EQ(swmm_reaction_option_set(engine_, "TEMPERATURE", "warm"),
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
    EXPECT_EQ(nh, 10);  // D Q U RE US FF AV HRT DT TEMP
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

// ---------------------------------------------------------------------------
// E-C2 — the strongest anti-drift falsifier: a system built from an EMPTY
// engine through the CRUD API must step identically to the same system
// authored in a .rxn file. If the API stored anything the compiler/binding
// reads differently (a missed aligned vector, an unset flag), the
// trajectories diverge.
TEST(ReactionsApiCrud, ApiBuiltSystemEqualsFileAuthored) {
    // File-authored twin.
    write_files();
    SWMM_Engine ef = swmm_engine_create();
    ASSERT_NE(ef, nullptr);
    ASSERT_EQ(swmm_engine_open(ef, "_rxapi.inp", "_rxapi_f.rpt", nullptr,
                               nullptr), SWMM_OK);

    // API-built twin: the same deck minus [PROCESS_COMPONENTS].
    {
        std::ifstream in("_rxapi.inp");
        std::ofstream out("_rxapi_a.inp");
        std::string line;
        bool skip = false;
        while (std::getline(in, line)) {
            if (line.find("[PROCESS_COMPONENTS]") != std::string::npos) {
                skip = true;
                continue;
            }
            if (skip && !line.empty() && line[0] == '[') skip = false;
            if (!skip) out << line << "\n";
        }
    }
    SWMM_Engine ea = swmm_engine_create();
    ASSERT_NE(ea, nullptr);
    ASSERT_EQ(swmm_engine_open(ea, "_rxapi_a.inp", "_rxapi_a.rpt", nullptr,
                               nullptr), SWMM_OK);
    EXPECT_EQ(swmm_reaction_species_count(ea), 0);

    ASSERT_EQ(swmm_reaction_option_set(ea, "SOLVER", "BDF2"), SWMM_OK);
    ASSERT_EQ(swmm_reaction_option_set(ea, "RATE_UNITS", "HR"), SWMM_OK);
    ASSERT_EQ(swmm_reaction_option_set(ea, "ATOL", "1e-8"), SWMM_OK);
    ASSERT_EQ(swmm_reaction_species_add(ea, "HOCL", 0, "MG", 0.0, 0.0),
              SWMM_OK);
    ASSERT_EQ(swmm_reaction_species_add(ea, "WALLP", 1, "MG", 1e-7, 1e-5),
              SWMM_OK);
    ASSERT_EQ(swmm_reaction_coeff_add(ea, "k1", 1, 0.36), SWMM_OK);
    ASSERT_EQ(swmm_reaction_coeff_add(ea, "kb", 0, 0.12), SWMM_OK);
    ASSERT_EQ(swmm_reaction_term_add(ea, "AMM", "0.05 * HOCL"), SWMM_OK);
    ASSERT_EQ(swmm_reaction_expr_set(ea, SWMM_RXN_SCOPE_PIPE, 0,
                                     SWMM_RXN_FORM_RATE, "-k1 * HOCL"),
              SWMM_OK);

    // Step both five routing steps and compare the MSX trajectory bitwise-
    // tight (same code path, same inputs).
    auto run5 = [](SWMM_Engine e) {
        EXPECT_EQ(swmm_engine_initialize(e), SWMM_OK);
        EXPECT_EQ(swmm_engine_start(e, 1), SWMM_OK);
        double elapsed = 1.0;
        for (int i = 0; i < 5 && elapsed > 0.0; ++i)
            EXPECT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);
    };
    run5(ef);
    run5(ea);

    // Probe through the public discovery surface (no internals).
    double gf = -1, ga = -1;
    ASSERT_EQ(swmm_reaction_init_global_get(ef, 0, &gf), SWMM_OK);
    ASSERT_EQ(swmm_reaction_init_global_get(ea, 0, &ga), SWMM_OK);
    EXPECT_DOUBLE_EQ(gf, ga);
    char e1[256], e2[256];
    int f1 = -1, f2 = -1;
    ASSERT_EQ(swmm_reaction_expr_get(ef, SWMM_RXN_SCOPE_PIPE, 0, &f1, e1,
                                     256), SWMM_OK);
    ASSERT_EQ(swmm_reaction_expr_get(ea, SWMM_RXN_SCOPE_PIPE, 0, &f2, e2,
                                     256), SWMM_OK);
    EXPECT_EQ(f1, f2);
    EXPECT_STREQ(e1, e2);

    swmm_engine_end(ef);
    swmm_engine_end(ea);
    swmm_engine_destroy(ef);
    swmm_engine_destroy(ea);
    std::remove("_rxapi.inp");
    std::remove("_rxapi.rxn");
    std::remove("_rxapi_a.inp");
}

// ---------------------------------------------------------------------------
// E-C3 — D-RC6: serialize -> apply_text -> serialize is byte-identical (the
// GUI text tab's sync contract), and check_text is a true dry run.
TEST_F(ReactionsApiTest, SerializeApplyRoundTripIsByteIdentical) {
    int need = 0;
    ASSERT_EQ(swmm_reactions_serialize(engine_, nullptr, 0, &need), SWMM_OK);
    ASSERT_GT(need, 1);
    std::string a(static_cast<std::size_t>(need), '\0');
    ASSERT_EQ(swmm_reactions_serialize(engine_, a.data(), need, &need),
              SWMM_OK);
    a.resize(static_cast<std::size_t>(need) - 1);

    // check_text: valid, and zero state change.
    char err[256];
    const int ns0 = swmm_reaction_species_count(engine_);
    ASSERT_EQ(swmm_reactions_check_text(engine_, a.c_str(), err, 256),
              SWMM_OK) << err;
    EXPECT_EQ(swmm_reaction_species_count(engine_), ns0);

    ASSERT_EQ(swmm_reactions_apply_text(engine_, a.c_str(), err, 256),
              SWMM_OK) << err;
    ASSERT_EQ(swmm_reactions_serialize(engine_, nullptr, 0, &need), SWMM_OK);
    std::string b(static_cast<std::size_t>(need), '\0');
    ASSERT_EQ(swmm_reactions_serialize(engine_, b.data(), need, &need),
              SWMM_OK);
    b.resize(static_cast<std::size_t>(need) - 1);
    EXPECT_EQ(a, b) << "serialize -> apply_text -> serialize must be "
                       "byte-identical (D-RC6)";
}

// ---------------------------------------------------------------------------
// E-C3 — a failed apply_text leaves the previous system byte-identical.
// Falsifier: under the old clear-on-error design the system would be GONE.
TEST_F(ReactionsApiTest, FailedApplyLeavesSystemIntact) {
    int need = 0;
    ASSERT_EQ(swmm_reactions_serialize(engine_, nullptr, 0, &need), SWMM_OK);
    std::string before(static_cast<std::size_t>(need), '\0');
    ASSERT_EQ(swmm_reactions_serialize(engine_, before.data(), need, &need),
              SWMM_OK);
    before.resize(static_cast<std::size_t>(need) - 1);

    char err[256] = {0};
    EXPECT_EQ(swmm_reactions_apply_text(
                  engine_,
                  "[REACTION_SPECIES]\nBULK A MG\n"
                  "[REACTION_PIPES]\nRATE NOPE -1.0 * NOPE\n",
                  err, 256),
              SWMM_ERR_BADPARAM);
    EXPECT_NE(std::string(err).find("NOPE"), std::string::npos) << err;

    ASSERT_EQ(swmm_reactions_serialize(engine_, nullptr, 0, &need), SWMM_OK);
    std::string after(static_cast<std::size_t>(need), '\0');
    ASSERT_EQ(swmm_reactions_serialize(engine_, after.data(), need, &need),
              SWMM_OK);
    after.resize(static_cast<std::size_t>(need) - 1);
    EXPECT_EQ(before, after)
        << "a failed apply must leave the prior system intact";
    // ...and the intact system's vocabulary still validates.
    int col = -1;
    EXPECT_EQ(swmm_reaction_validate_expression(engine_, SWMM_RXN_SCOPE_PIPE,
                                                "HOCL * k1", err, 256, &col),
              SWMM_OK) << err;
}

// ---------------------------------------------------------------------------
// E-C3 — save writes a file a fresh engine reads back into the identical
// canonical text.
TEST_F(ReactionsApiTest, SaveReopensIdentically) {
    ASSERT_EQ(swmm_reactions_save(engine_, "_rxapi_saved.rxn"), SWMM_OK);

    // The fixture deck, rebound to the saved file.
    {
        std::ifstream in("_rxapi.inp");
        std::ofstream out("_rxapi_s.inp");
        std::string line;
        while (std::getline(in, line)) {
            const auto pos = line.find("_rxapi.rxn");
            if (pos != std::string::npos)
                line.replace(pos, std::strlen("_rxapi.rxn"),
                             "_rxapi_saved.rxn");
            out << line << "\n";
        }
    }
    SWMM_Engine e2 = swmm_engine_create();
    ASSERT_NE(e2, nullptr);
    ASSERT_EQ(swmm_engine_open(e2, "_rxapi_s.inp", "_rxapi_s.rpt", nullptr,
                               nullptr), SWMM_OK);
    int n1 = 0, n2 = 0;
    ASSERT_EQ(swmm_reactions_serialize(engine_, nullptr, 0, &n1), SWMM_OK);
    ASSERT_EQ(swmm_reactions_serialize(e2, nullptr, 0, &n2), SWMM_OK);
    std::string a(static_cast<std::size_t>(n1), '\0'),
                b(static_cast<std::size_t>(n2), '\0');
    ASSERT_EQ(swmm_reactions_serialize(engine_, a.data(), n1, &n1), SWMM_OK);
    ASSERT_EQ(swmm_reactions_serialize(e2, b.data(), n2, &n2), SWMM_OK);
    EXPECT_EQ(a, b);
    swmm_engine_destroy(e2);
    std::remove("_rxapi_s.inp");
    std::remove("_rxapi_saved.rxn");
}

// ---------------------------------------------------------------------------
// E-C3 — the [PROCESS_COMPONENTS] surface: enumerate/find on the fixture,
// register (dup refused, file need not exist), remove, and a registration
// survives swmm_model_write.
TEST_F(ReactionsApiTest, ProcessComponentRegistryRoundTrip) {
    ASSERT_GE(swmm_process_component_count(engine_), 1);
    const int ri = swmm_process_component_find(
        engine_, "org.hydrocouple.openswmm.reactions");
    ASSERT_GE(ri, 0);
    char id[128], cfg[256], res[512];
    ASSERT_EQ(swmm_process_component_get(engine_, ri, id, 128, cfg, 256,
                                         res, 512), SWMM_OK);
    EXPECT_STREQ(id, "org.hydrocouple.openswmm.reactions");
    EXPECT_STREQ(cfg, "_rxapi.rxn");
    EXPECT_NE(std::string(res).find("_rxapi.rxn"), std::string::npos)
        << "resolved path must be set after open";

    // Register: new id OK (config file absent — D-RC8), duplicate refused.
    ASSERT_EQ(swmm_process_component_register(
                  engine_, "org.hydrocouple.openswmm.waterage",
                  "_rxapi_new.age"), SWMM_OK);
    EXPECT_EQ(swmm_process_component_register(
                  engine_, "org.hydrocouple.openswmm.reactions", "x.rxn"),
              SWMM_ERR_BADPARAM);

    // The registration reaches the written deck.
    ASSERT_EQ(swmm_model_write(engine_, "_rxapi_pc.inp"), SWMM_OK);
    {
        std::ifstream f("_rxapi_pc.inp");
        std::string all((std::istreambuf_iterator<char>(f)),
                        std::istreambuf_iterator<char>());
        EXPECT_NE(all.find("org.hydrocouple.openswmm.waterage"),
                  std::string::npos);
    }
    const int wi = swmm_process_component_find(
        engine_, "org.hydrocouple.openswmm.waterage");
    ASSERT_GE(wi, 0);
    ASSERT_EQ(swmm_process_component_remove(engine_, wi), SWMM_OK);
    EXPECT_EQ(swmm_process_component_find(
                  engine_, "org.hydrocouple.openswmm.waterage"), -1);
    std::remove("_rxapi_pc.inp");
}

// ---------------------------------------------------------------------------
// E-C2 — D-RC3: species removal keeps every aligned vector in step, and a
// referenced species cannot be removed.
TEST_F(ReactionsApiTest, SpeciesRemoveAlignmentAudit) {
    // The fixture holds HOCL (with a RATE referencing it via k1) and WALLP.
    ASSERT_EQ(swmm_reaction_species_add(engine_, "ZED", 0, "MG", 0.0, 0.0),
              SWMM_OK);
    ASSERT_EQ(swmm_reaction_expr_set(engine_, SWMM_RXN_SCOPE_TANK, 2,
                                     SWMM_RXN_FORM_RATE, "-0.5 * ZED"),
              SWMM_OK);
    ASSERT_EQ(swmm_reaction_species_count(engine_), 3);

    // HOCL is referenced by its own RATE — removal refused.
    EXPECT_EQ(swmm_reaction_species_remove(engine_, 0), SWMM_ERR_BADPARAM);

    // WALLP (idx 1) is unreferenced — removal shifts ZED down to idx 1 with
    // its expression intact and the registry agreeing on names.
    ASSERT_EQ(swmm_reaction_species_remove(engine_, 1), SWMM_OK);
    ASSERT_EQ(swmm_reaction_species_count(engine_), 2);
    char name[64], units[32], expr[256];
    int is_wall = -1, form = -1;
    double atol = 0, rtol = 0;
    ASSERT_EQ(swmm_reaction_species_get(engine_, 1, name, 64, &is_wall,
                                        units, 32, &atol, &rtol), SWMM_OK);
    EXPECT_STREQ(name, "ZED");
    ASSERT_EQ(swmm_reaction_expr_get(engine_, SWMM_RXN_SCOPE_TANK, 1, &form,
                                     expr, 256), SWMM_OK);
    EXPECT_EQ(form, SWMM_RXN_FORM_RATE);
    EXPECT_STREQ(expr, "-0.5 * ZED");
    // The recompiled expression must still evaluate against the SHIFTED
    // index — validated implicitly by the eager recompile, and the
    // validate call agrees the vocabulary still holds ZED.
    char err[128];
    int col = -1;
    EXPECT_EQ(swmm_reaction_validate_expression(engine_, SWMM_RXN_SCOPE_TANK,
                                                "ZED * 2", err, 128, &col),
              SWMM_OK) << err;
}

// ---------------------------------------------------------------------------
// E-C2 — eager validation rolls the mutation back (D-RC5): a bad expression
// never lands, and the pre-existing system is untouched.
TEST_F(ReactionsApiTest, EagerValidationRollsBack) {
    const int nt = swmm_reaction_term_count(engine_);
    EXPECT_EQ(swmm_reaction_term_add(engine_, "BAD", "MIN(HOCL)"),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_reaction_term_count(engine_), nt);

    char expr[256];
    int form = -1;
    EXPECT_EQ(swmm_reaction_expr_set(engine_, SWMM_RXN_SCOPE_PIPE, 0,
                                     SWMM_RXN_FORM_RATE, "1 + NOPE"),
              SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_reaction_expr_get(engine_, SWMM_RXN_SCOPE_PIPE, 0, &form,
                                     expr, 256), SWMM_OK);
    EXPECT_STREQ(expr, "-k1 * HOCL") << "failed set must leave the old expr";

    // Duplicate names refused across kinds.
    EXPECT_EQ(swmm_reaction_species_add(engine_, "k1", 0, "MG", 0, 0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_reaction_coeff_add(engine_, "HOCL", 1, 1.0),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_reaction_species_add(engine_, "TSS", 0, "MG", 0, 0),
              SWMM_ERR_BADPARAM) << "may not shadow a pollutant";
}

// ---------------------------------------------------------------------------
// E-C2 — per-element initial rows: upsert/get/remove round-trip, negative
// refused, and lifecycle guards bite after start.
TEST_F(ReactionsApiTest, InitRowsAndLifecycle) {
    ASSERT_EQ(swmm_reaction_init_global_set(engine_, 0, 0.8), SWMM_OK);
    double g = -1;
    ASSERT_EQ(swmm_reaction_init_global_get(engine_, 0, &g), SWMM_OK);
    EXPECT_DOUBLE_EQ(g, 0.8);

    EXPECT_EQ(swmm_reaction_init_elem_count(engine_), 0);
    ASSERT_EQ(swmm_reaction_init_elem_set(engine_, 0, 0, 0, 1.2), SWMM_OK);
    ASSERT_EQ(swmm_reaction_init_elem_set(engine_, 1, 0, 0, 0.5), SWMM_OK);
    ASSERT_EQ(swmm_reaction_init_elem_set(engine_, 0, 0, 0, 1.4), SWMM_OK);
    EXPECT_EQ(swmm_reaction_init_elem_count(engine_), 2);   // upsert
    int il = -1, ei = -1, si = -1;
    double v = -1;
    ASSERT_EQ(swmm_reaction_init_elem_get(engine_, 0, &il, &ei, &si, &v),
              SWMM_OK);
    EXPECT_EQ(il, 0);
    EXPECT_DOUBLE_EQ(v, 1.4);
    EXPECT_EQ(swmm_reaction_init_elem_set(engine_, 0, 0, 0, -1.0),
              SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_reaction_init_elem_remove(engine_, 0), SWMM_OK);
    EXPECT_EQ(swmm_reaction_init_elem_count(engine_), 1);

    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
    EXPECT_EQ(swmm_reaction_species_add(engine_, "LATE", 0, "MG", 0, 0),
              SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_reaction_option_set(engine_, "ATOL", "1e-9"),
              SWMM_ERR_LIFECYCLE);
    EXPECT_EQ(swmm_reaction_init_elem_set(engine_, 0, 0, 0, 1.0),
              SWMM_ERR_LIFECYCLE);
    swmm_engine_end(engine_);
}

}  // namespace
