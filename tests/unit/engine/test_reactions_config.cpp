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
 * @file test_reactions_config.cpp
 * @brief T0a + R1 gates: species registry + reactions component parsing
 *        (plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R1).
 *
 * @details Falsifiers (per the roadmap handoff-authoring rule — each gate
 *          states what would make it fail):
 *          - FullConfigParsesAndPopulates fails if the apply hook never runs
 *            (asserts configured==true AND registry grew past the pollutant
 *            block) or any parsed value is wrong.
 *          - ReferenceValidation/PollutantCollision/LaterPhaseSection/
 *            DuplicateProcessComponentId fail if their validation paths are
 *            removed (each asserts open() FAILS with the specific message).
 *          - EmbeddedFallback uses DIFFERENT species names embedded vs
 *            external, so "external wins" is distinguishable from "embedded
 *            applied" by content, not just by warnings.
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

#include "core/InpWriter.hpp"
#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Minimal deck with one pollutant (TSS). `pc_lines` → [PROCESS_COMPONENTS];
/// `extra_sections` lands verbatim at the end (embedded-fallback tests).
void write_deck(const char* path, const std::string& pc_lines,
                const std::string& extra_sections = "") {
    std::ofstream f(path);
    f << "[TITLE]\nR1 reactions gate deck\n\n"
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
         "0.0  0.0\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << extra_sections;
    f << "[REPORT]\nINPUT NO\n";
}

void write_rxn(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

const char* kGoodRxn =
    ";; nh2cl-style skeleton (structure only; kinetics compile in R2)\n"
    "[REACTION_OPTIONS]\n"
    "SOLVER      BDF2\nRATE_UNITS  HR\nCOUPLING    NONE\n"
    "ATOL        1e-8\nRTOL        1e-5\n\n"
    "[REACTION_SPECIES]\n"
    ";;Kind  Name    Units\n"
    "BULK    HOCL    MG\n"
    "BULK    NH2CL   MG\n"
    "WALL    WALLP   MG\n\n"
    "[REACTION_COEFFICIENTS]\n"
    "PARAMETER  k1   0.36\n"
    "CONSTANT   kb   0.12\n\n"
    "[REACTION_TERMS]\n"
    "AMM   0.05 * NH2CL\n\n"
    "[REACTION_PIPES]\n"
    "RATE     HOCL   -k1 * HOCL * AMM - kb * HOCL\n"
    "FORMULA  NH2CL  0.9 * HOCL\n\n"
    "[REACTION_TANKS]\n"
    "RATE     HOCL   -kb * HOCL\n\n"
    "[REACTION_QUALITY]\n"
    "GLOBAL   HOCL   0.8\n";

class ReactionsConfigTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    void TearDown() override {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
    int open_deck(const char* inp, const char* rpt, const char* out) {
        return swmm_engine_open(engine_, inp, rpt, out, nullptr);
    }
    static bool contains(const std::vector<std::string>& v,
                         const std::string& needle) {
        for (const auto& e : v)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, FullConfigParsesAndPopulates) {
    write_rxn("_rx_good.rxn", kGoodRxn);
    write_deck("_rx_ok.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_good.rxn\"");
    ASSERT_EQ(open_deck("_rx_ok.inp", "_rx_ok.rpt", "_rx_ok.out"), SWMM_OK);

    const auto& ctx = as_cpp_engine(engine_).context();
    const auto& rx  = ctx.reactions;
    // Falsifier: if the apply hook never ran, configured is false and the
    // registry holds only the pollutant block.
    ASSERT_TRUE(rx.configured);
    ASSERT_EQ(ctx.species_registry.count(), 1 + 3);   // TSS + 3 MSX
    EXPECT_EQ(ctx.species_registry.pollutant_count(), 1);
    EXPECT_EQ(static_cast<int>(ctx.species_registry.kind(0)),
              static_cast<int>(openswmm::SpeciesKind::POLLUTANT));
    EXPECT_EQ(rx.registry_base, 1);
    EXPECT_EQ(static_cast<int>(ctx.species_registry.kind(3)),
              static_cast<int>(openswmm::SpeciesKind::MSX_WALL));

    EXPECT_EQ(static_cast<int>(rx.solver),
              static_cast<int>(openswmm::ReactionSolverKind::BDF2));
    ASSERT_EQ(rx.n_species(), 3);
    EXPECT_EQ(rx.species_is_wall[2], 1);
    ASSERT_EQ(rx.coef_name.size(), 2u);
    EXPECT_EQ(rx.coef_is_param[0], 1);
    EXPECT_EQ(rx.coef_is_param[1], 0);
    ASSERT_EQ(rx.term_name.size(), 1u);
    EXPECT_EQ(rx.term_expr_src[0], "0.05 * NH2CL");
    EXPECT_EQ(static_cast<int>(rx.pipe_form[0]),
              static_cast<int>(openswmm::ReactionExprForm::RATE));
    EXPECT_EQ(rx.pipe_expr_src[0], "-k1 * HOCL * AMM - kb * HOCL");
    EXPECT_EQ(static_cast<int>(rx.pipe_form[1]),
              static_cast<int>(openswmm::ReactionExprForm::FORMULA));
    EXPECT_EQ(static_cast<int>(rx.tank_form[0]),
              static_cast<int>(openswmm::ReactionExprForm::RATE));
    EXPECT_DOUBLE_EQ(rx.init_global[0], 0.8);
    EXPECT_DOUBLE_EQ(rx.atol, 1e-8);

    std::remove("_rx_ok.inp");
    std::remove("_rx_good.rxn");
}

// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, UndeclaredSpeciesReferenceFailsOpen) {
    write_rxn("_rx_badref.rxn",
              "[REACTION_SPECIES]\nBULK A MG\n"
              "[REACTION_PIPES]\nRATE  NOPE  -1.0 * NOPE\n");
    write_deck("_rx_badref.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_badref.rxn\"");
    EXPECT_NE(open_deck("_rx_badref.inp", "_rx_badref.rpt", "_rx_badref.out"),
              SWMM_OK);
    EXPECT_TRUE(contains(as_cpp_engine(engine_).context().errors,
                         "undeclared species 'NOPE'"));
    std::remove("_rx_badref.inp");
    std::remove("_rx_badref.rxn");
}

// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, PollutantNameCollisionFailsOpen) {
    write_rxn("_rx_coll.rxn", "[REACTION_SPECIES]\nBULK TSS MG\n");
    write_deck("_rx_coll.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_coll.rxn\"");
    EXPECT_NE(open_deck("_rx_coll.inp", "_rx_coll.rpt", "_rx_coll.out"),
              SWMM_OK);
    EXPECT_TRUE(contains(as_cpp_engine(engine_).context().errors,
                         "collides with an existing pollutant"));
    std::remove("_rx_coll.inp");
    std::remove("_rx_coll.rxn");
}

// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, LaterPhaseSectionFailsWithPhaseName) {
    write_rxn("_rx_later.rxn",
              "[REACTION_SPECIES]\nBULK A MG\n"
              "[REACTION_SOURCES]\nCONC J0 A 1.0\n");
    write_deck("_rx_later.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_later.rxn\"");
    EXPECT_NE(open_deck("_rx_later.inp", "_rx_later.rpt", "_rx_later.out"),
              SWMM_OK);
    // Name the SECTION, not just the phrase: the planned-id diagnostic
    // ("…arrives with plan phase R1…") carries the same phrase, so a bare
    // phrase match passes even when the component never ran at all.
    EXPECT_TRUE(contains(as_cpp_engine(engine_).context().errors,
                         "[REACTION_SOURCES] is recognized but not yet "
                         "supported — arrives with plan phase"));
    std::remove("_rx_later.inp");
    std::remove("_rx_later.rxn");
}

// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, DuplicateProcessComponentIdFailsOpen) {
    write_rxn("_rx_dup.rxn", "[REACTION_SPECIES]\nBULK A MG\n");
    write_deck("_rx_dup.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_dup.rxn\"\n"
               "org.hydrocouple.openswmm.reactions  config=\"_rx_dup.rxn\"");
    EXPECT_NE(open_deck("_rx_dup.inp", "_rx_dup.rpt", "_rx_dup.out"),
              SWMM_OK);
    // Assert the REGISTRY's wording. applyReactionSections has its own
    // "configured twice — duplicate registration or embedded sections…"
    // guard, so matching the bare phrase "duplicate registration" passes
    // even with the registry check removed — one defense masking the other.
    EXPECT_TRUE(contains(as_cpp_engine(engine_).context().errors,
                         "each component id may appear once"));
    std::remove("_rx_dup.inp");
    std::remove("_rx_dup.rxn");
}

// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, EmbeddedFallbackAppliesAndExternalWins) {
    // (a) Embedded only: applied, with the style warning. Species EMB_A
    //     exists ONLY in the embedded sections.
    write_deck("_rx_emb.inp", "",
               "[REACTION_SPECIES]\nBULK EMB_A MG\n\n");
    ASSERT_EQ(open_deck("_rx_emb.inp", "_rx_emb.rpt", "_rx_emb.out"), SWMM_OK);
    {
        const auto& ctx = as_cpp_engine(engine_).context();
        EXPECT_TRUE(ctx.reactions.configured);
        EXPECT_GE(ctx.species_registry.find("EMB_A"), 0);
        EXPECT_TRUE(contains(ctx.warnings, "style warning"));
    }
    std::remove("_rx_emb.inp");

    swmm_engine_destroy(engine_);
    engine_ = swmm_engine_create();
    ASSERT_NE(engine_, nullptr);

    // (b) Both: the external file wins by CONTENT — EXT_B present, EMB_A
    //     absent — and the ignored-embedded warning fires.
    write_rxn("_rx_ext.rxn", "[REACTION_SPECIES]\nBULK EXT_B MG\n");
    write_deck("_rx_both.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_ext.rxn\"",
               "[REACTION_SPECIES]\nBULK EMB_A MG\n\n");
    ASSERT_EQ(open_deck("_rx_both.inp", "_rx_both.rpt", "_rx_both.out"),
              SWMM_OK);
    {
        const auto& ctx = as_cpp_engine(engine_).context();
        EXPECT_TRUE(ctx.reactions.configured);
        EXPECT_GE(ctx.species_registry.find("EXT_B"), 0);
        EXPECT_LT(ctx.species_registry.find("EMB_A"), 0)
            << "embedded sections must NOT apply when the external file wins";
        EXPECT_TRUE(contains(ctx.warnings, "IGNORED"));
    }
    std::remove("_rx_both.inp");
    std::remove("_rx_ext.rxn");
}

// ---------------------------------------------------------------------------
// Embedded [REACTION_*] sections are dropped by InpWriter (no per-component
// serialization until IO3) — the save must SAY so rather than destroying
// user-authored model data silently. Falsifier: remove the writer warning and
// this fails; the sections themselves are expected to be absent either way.
// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, EmbeddedSectionsLostOnSaveAreReported) {
    write_deck("_rx_save.inp", "", "[REACTION_SPECIES]\nBULK EMB_A MG\n\n");
    ASSERT_EQ(open_deck("_rx_save.inp", "_rx_save.rpt", "_rx_save.out"),
              SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_TRUE(ctx.reactions.configured);

    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, "_rx_saved.inp",
                                                 &warnings), 0);
    EXPECT_TRUE(contains(warnings, "NOT written back"))
        << "InpWriter dropped the embedded sections without a word";
    EXPECT_TRUE(contains(warnings, "[REACTION_SPECIES]"))
        << "the warning must name what was lost";

    // The data really is gone — the warning is the whole mitigation.
    std::ifstream in("_rx_saved.inp");
    std::string line;
    bool found = false;
    while (std::getline(in, line))
        if (line.rfind("[REACTION", 0) == 0) found = true;
    EXPECT_FALSE(found);

    std::remove("_rx_save.inp");
    std::remove("_rx_saved.inp");
}

}  // namespace
