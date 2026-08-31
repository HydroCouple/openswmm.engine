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
 * @file test_reaction_expressions.cpp
 * @brief R2 gates: expression compiler + Tier-1 VM + transactional registry
 *        (plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R2).
 *
 * @details Falsifiers, each asserting wording/values UNIQUE to the defense
 *          under test (defense-in-depth aliasing lesson, roadmap):
 *          - VM goldens compare EXACT doubles computed by hand — any
 *            change to precedence, associativity, or an op body fails.
 *          - UndefinedIdentifier asserts the compiler's message AND the
 *            1-based column — the R1 parser has no message with a column.
 *          - ForwardOnlyTermRule asserts "references later term — term
 *            references are forward-only", produced only by the compiler.
 *          - TransactionalRegistryOnRejectedConfig (the R2 carried
 *            obligation): LENIENT open of a config that fails COMPILATION
 *            (parses fine structurally) must leave the registry at exactly
 *            the pollutant count and reactions unconfigured. Probe that
 *            falsifies it: re-add direct registry insertion in
 *            parseSpecies — the gate then fails.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"
#include "transport/components/ReactionModule/ReactionExpression.hpp"

namespace {

using openswmm::RxExprSpan;
using openswmm::RxHydVar;
using openswmm::RxToken;
using openswmm::transport::compileReactionExpression;
using openswmm::transport::evalReactionExpression;
using openswmm::transport::RxEvalEnv;
using openswmm::transport::RxSymbols;

// ---------------------------------------------------------------------------
// Direct VM gates
// ---------------------------------------------------------------------------

class RxVmTest : public ::testing::Test {
protected:
    std::vector<std::string> species{"HOCL", "NH2CL"};
    std::vector<std::string> coefs{"k1", "kb"};
    std::vector<std::string> terms{"AMM"};
    std::vector<RxToken>     pool;

    double compile_eval(const std::string& src, const RxEvalEnv& env,
                        int max_term = 1) {
        RxSymbols sym;
        sym.species = &species;
        sym.coefs   = &coefs;
        sym.terms   = &terms;
        sym.max_term = max_term;
        RxExprSpan span;
        int col = 0;
        const std::string err =
            compileReactionExpression(src, sym, pool, span, col);
        EXPECT_TRUE(err.empty()) << src << " → " << err << " (col " << col << ")";
        return evalReactionExpression(pool, span, env);
    }
};

TEST_F(RxVmTest, PrecedenceAndAssociativityGoldens) {
    RxEvalEnv env{};
    // 2 + 3*4^2 - -5 = 2 + 48 + 5 = 55  (^ binds tighter than *, unary minus)
    EXPECT_DOUBLE_EQ(compile_eval("2 + 3 * 4 ^ 2 - -5", env), 55.0);
    // Right-associative ^: 2^3^2 = 2^9 = 512, NOT (2^3)^2 = 64.
    EXPECT_DOUBLE_EQ(compile_eval("2 ^ 3 ^ 2", env), 512.0);
    // Division/multiplication left-assoc: 24 / 4 * 2 = 12, not 3.
    EXPECT_DOUBLE_EQ(compile_eval("24 / 4 * 2", env), 12.0);
    // Functions incl. binary MIN/MAX/POW and nested calls.
    EXPECT_DOUBLE_EQ(compile_eval("MIN(3, MAX(1, 2)) + POW(2, 5)", env), 34.0);
    EXPECT_DOUBLE_EQ(compile_eval("STEP(-2) + STEP(3) + SGN(-9)", env), 0.0);
    EXPECT_DOUBLE_EQ(compile_eval("SQRT(ABS(-16))", env), 4.0);

    // Unary minus vs '^'. HISTORY (R2 validation finding): R2 shipped the
    // Excel convention ((-2)^2 = +4) unpinned; the engine's own legacy
    // parser (src/legacy/engine/mathexpr.c, which EPANET-MSX shares) cannot
    // arbitrate — it returns 0 for BOTH "-2^2" and "(-2)^2" while getting
    // "0-2^2" and "-(2^2)" right, so there is no MSX-conventional answer to
    // inherit. RESOLUTION (D-R8, 2026-08-16): unary minus binds BELOW '^' —
    // the Python/Fortran/MATLAB convention, -2^2 = -(2^2) = -4 — because
    // the R5 authoring path round-trips through sympy, which parses -k**2
    // as -(k**2); any other choice makes Python-authored kinetics silently
    // disagree with the same text in model.rxn. Note -2^3 agrees either
    // way (odd exponent), which is exactly why these need EVEN-exponent
    // goldens on both spellings.
    EXPECT_DOUBLE_EQ(compile_eval("-2 ^ 2", env), -4.0);     // D-R8
    EXPECT_DOUBLE_EQ(compile_eval("(-2) ^ 2", env), 4.0);    // explicit parens
    EXPECT_DOUBLE_EQ(compile_eval("-2 ^ 3", env), -8.0);
    EXPECT_DOUBLE_EQ(compile_eval("2 ^ -2", env), 0.25);     // NEG above '^' rhs
    EXPECT_DOUBLE_EQ(compile_eval("0 - 2 ^ 2", env), -4.0);  // binary minus
    EXPECT_DOUBLE_EQ(compile_eval("-2 * 3", env), -6.0);     // unchanged by D-R8
}

TEST_F(RxVmTest, KineticsWithResolvedOperandsEvaluateExactly) {
    const double sp[] = {0.8, 0.2};        // HOCL, NH2CL
    const double cf[] = {0.36, 0.12};      // k1, kb
    const double tm[] = {0.05 * 0.2};      // AMM = 0.05*NH2CL = 0.01
    double hv[static_cast<int>(RxHydVar::COUNT_)] = {};
    hv[static_cast<int>(RxHydVar::U)] = 2.5;
    RxEvalEnv env{sp, cf, tm, hv};
    // -k1*HOCL*AMM - kb*HOCL = -0.36*0.8*0.01 - 0.12*0.8 = -0.09888
    EXPECT_DOUBLE_EQ(compile_eval("-k1 * HOCL * AMM - kb * HOCL", env),
                     -0.36 * 0.8 * 0.01 - 0.12 * 0.8);
    // Hydraulic variable resolution (U) alongside species.
    EXPECT_DOUBLE_EQ(compile_eval("U * NH2CL", env), 2.5 * 0.2);

    // TEMP resolves as a hydraulic variable — the Arrhenius shape the slot
    // exists for. theta^(T-20) with theta=1.07, T=25.
    hv[static_cast<int>(RxHydVar::TEMP)] = 25.0;
    EXPECT_DOUBLE_EQ(compile_eval("1.07 ^ (TEMP - 20)", env),
                     std::pow(1.07, 5.0));
}

TEST_F(RxVmTest, UndefinedIdentifierReportsNameAndColumn) {
    RxSymbols sym;
    sym.species = &species; sym.coefs = &coefs; sym.terms = &terms;
    sym.max_term = 1;
    RxExprSpan span;
    int col = 0;
    const std::string err =
        compileReactionExpression("kb * zzz + 1", sym, pool, span, col);
    EXPECT_NE(err.find("undefined identifier 'zzz'"), std::string::npos) << err;
    EXPECT_EQ(col, 6);   // 1-based column of 'zzz'
}

TEST_F(RxVmTest, EmptySpanEvaluatesToZeroByContract) {
    // D-R9: len == 0 is the documented "no expression" encoding. Falsifier:
    // remove the guard in evalReactionExpression — this read becomes an
    // uninitialized stack slot (and fails under sanitizers deterministically
    // rather than "returning 0.0 by luck", the R2 validator's phrase).
    RxExprSpan empty{};
    RxEvalEnv env{};
    EXPECT_DOUBLE_EQ(evalReactionExpression(pool, empty, env), 0.0);
}

TEST_F(RxVmTest, DepthGuardRejectsTooDeepExpressions) {
    std::string deep;
    for (int i = 0; i < 40; ++i) deep += "(1 + ";
    deep += "1";
    for (int i = 0; i < 40; ++i) deep += ")";
    RxSymbols sym;
    RxExprSpan span;
    int col = 0;
    const std::string err =
        compileReactionExpression(deep, sym, pool, span, col);
    EXPECT_NE(err.find("expression too deep"), std::string::npos) << err;
}

// E-D1 (D-R9): arity errors name the function, its arity, and the actual
// count, at the CALL-SITE column — not "malformed expression" col 1.
// Falsifier: before the round each of these compiled into the RPN discipline
// check and reported the generic message.
TEST_F(RxVmTest, ArityErrorsNameTheFunction) {
    struct Case { const char* src; const char* needle; };
    const Case cases[] = {
        {"MIN(HOCL)",       "function 'MIN' expects 2 arguments, got 1"},
        {"MAX(1, 2, 3)",    "function 'MAX' expects 2 arguments, got 3"},
        {"EXP(1, 2)",       "function 'EXP' expects 1 argument, got 2"},
        {"1 + POW(2)",      "function 'POW' expects 2 arguments, got 1"},
    };
    for (const auto& c : cases) {
        RxSymbols sym;
        sym.species = &species;
        RxExprSpan span;
        int col = 0;
        const std::string err =
            compileReactionExpression(c.src, sym, pool, span, col);
        EXPECT_NE(err.find(c.needle), std::string::npos)
            << c.src << " → '" << err << "'";
        EXPECT_GT(col, 0) << c.src << ": arity error must carry a column";
    }
}

// ---------------------------------------------------------------------------
// Engine-level gates
// ---------------------------------------------------------------------------

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

void write_deck(const char* path, const char* rxn_path) {
    std::ofstream f(path);
    f << "[TITLE]\nR2 gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 00:30:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:05:00\n\n"
      << "[JUNCTIONS]\nJ0 10.0 10 0.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n"
      << "[POLLUTANTS]\nTSS MG/L 0 0 0 0 NO * 0 0 0\n\n"
      << "[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions  config=\""
      << rxn_path << "\"\n\n[REPORT]\nINPUT NO\n";
}

class RxCompileEngineTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    void TearDown() override {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
    SWMM_Engine engine_ = nullptr;
};

TEST_F(RxCompileEngineTest, GoodConfigCompilesToPool) {
    {
        std::ofstream c("_r2_good.rxn");
        c << "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
          << "[REACTION_TERMS]\nT1 0.5 * A\n"
          << "[REACTION_PIPES]\nRATE A -T1 * B\n";
    }
    write_deck("_r2_good.inp", "_r2_good.rxn");
    ASSERT_EQ(swmm_engine_open(engine_, "_r2_good.inp", "_r2_good.rpt",
                               "_r2_good.out", nullptr), SWMM_OK);
    const auto& rx = as_cpp_engine(engine_).context().reactions;
    EXPECT_TRUE(rx.compiled);
    EXPECT_FALSE(rx.token_pool.empty());
    ASSERT_EQ(rx.term_expr.size(), 1u);
    EXPECT_GT(rx.term_expr[0].len, 0);
    ASSERT_EQ(rx.pipe_expr.size(), 2u);
    EXPECT_GT(rx.pipe_expr[0].len, 0);
    EXPECT_EQ(rx.pipe_expr[1].len, 0);     // B has no expression
    std::remove("_r2_good.inp");
    std::remove("_r2_good.rxn");
}

TEST_F(RxCompileEngineTest, ForwardOnlyTermRuleFailsOpen) {
    {
        std::ofstream c("_r2_fwd.rxn");
        c << "[REACTION_SPECIES]\nBULK A MG\n"
          << "[REACTION_TERMS]\nT1 T2 + 1\nT2 2 * A\n";
    }
    write_deck("_r2_fwd.inp", "_r2_fwd.rxn");
    EXPECT_NE(swmm_engine_open(engine_, "_r2_fwd.inp", "_r2_fwd.rpt",
                               "_r2_fwd.out", nullptr), SWMM_OK);
    bool found = false;
    for (const auto& e : as_cpp_engine(engine_).context().errors)
        if (e.find("references later term — term references are "
                   "forward-only") != std::string::npos)
            found = true;
    EXPECT_TRUE(found);
    std::remove("_r2_fwd.inp");
    std::remove("_r2_fwd.rxn");
}

// The R2 carried obligation (R1 validation finding): a config that parses
// structurally but FAILS COMPILATION must leave no registry entries behind
// under a lenient (editor) open.
TEST_F(RxCompileEngineTest, TransactionalRegistryOnRejectedConfig) {
    {
        std::ofstream c("_r2_txn.rxn");
        c << "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
          << "[REACTION_PIPES]\nRATE A -zzz * A\n";   // compile-time failure
    }
    write_deck("_r2_txn.inp", "_r2_txn.rxn");
    swmm_engine_set_lenient_open(engine_, 1);
    ASSERT_EQ(swmm_engine_open(engine_, "_r2_txn.inp", "_r2_txn.rpt",
                               "_r2_txn.out", nullptr), SWMM_OK)
        << "lenient open must survive the bad config";
    const auto& ctx = as_cpp_engine(engine_).context();
    // Unique to this defense: the compiler's undefined-identifier wording.
    bool found = false;
    for (const auto& e : ctx.errors)
        if (e.find("undefined identifier 'zzz'") != std::string::npos)
            found = true;
    EXPECT_TRUE(found);
    // The registry holds ONLY the pollutant block — no A, no B.
    EXPECT_EQ(ctx.species_registry.count(), 1);
    EXPECT_LT(ctx.species_registry.find("A"), 0);
    EXPECT_FALSE(ctx.reactions.configured);
    EXPECT_EQ(ctx.reactions.n_species(), 0)
        << "rejected config must clear reaction state, not half-hold it";
    std::remove("_r2_txn.inp");
    std::remove("_r2_txn.rxn");
}

}  // namespace
