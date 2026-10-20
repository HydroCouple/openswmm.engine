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
#include <openswmm/engine/openswmm_edit.h>

#include "core/InpWriter.hpp"
#include "core/DateTime.hpp"
#include "hydrology/Inflow.hpp"
#include "transport/components/ReactionModule/ReactionIntegrator.hpp"
#include "transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "transport/components/ReactionModule/MsxLegacyTransport.hpp"
#include <cmath>
#include "core/SWMMEngine.hpp"
#include "transport/components/ReactionModule/ReactionsWriter.hpp"
#include <openswmm/engine/openswmm_reactions.h>

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
        for (const auto& prefix : cleanup_prefixes_)
            for (const auto* suffix : {".inp", ".rxn", ".out", ".rpt"})
                std::remove((prefix + suffix).c_str());
    }
    std::vector<std::string> cleanup_prefixes_;
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
TEST_F(ReactionsConfigTest, SubcatchmentScopeParsesAndRoundTrips) {
    write_rxn("_rx_later.rxn",
              "[REACTION_SPECIES]\nBULK A MG\n"
              "[REACTION_SUBCATCHMENTS]\nRATE A -A\n");
    write_deck("_rx_later.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_later.rxn\"");
    ASSERT_EQ(open_deck("_rx_later.inp", "_rx_later.rpt", "_rx_later.out"), SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    EXPECT_TRUE(ctx.reactions.has_subcatch_reactions());
    const auto text = openswmm::transport::serializeReactionSystem(ctx);
    EXPECT_NE(text.find("[REACTION_SUBCATCHMENTS]"), std::string::npos);
    char error[1024] = {};
    EXPECT_EQ(swmm_reactions_check_text(engine_, text.c_str(), error, sizeof(error)), SWMM_OK) << error;
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

// ---------------------------------------------------------------------------
// E-B1 — [REACTION_QUALITY] NODE|LINK scopes parse into the per-element row
// table; GLOBAL keeps feeding init_global. Falsifier before the round: the
// phase-rejection error fires and open FAILS.
TEST_F(ReactionsConfigTest, NodeLinkQualityRowsParse) {
    write_rxn("_rx_nl.rxn",
              "[REACTION_SPECIES]\nBULK HOCL MG\nBULK NH2CL MG\n"
              "[REACTION_QUALITY]\n"
              "GLOBAL HOCL 0.8\n"
              "NODE   J0  HOCL  1.2\n"
              "LINK   C1  NH2CL 0.05\n");
    write_deck("_rx_nl.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_nl.rxn\"");
    ASSERT_EQ(open_deck("_rx_nl.inp", "_rx_nl.rpt", "_rx_nl.out"), SWMM_OK);

    const auto& ctx = as_cpp_engine(engine_).context();
    const auto& rx  = ctx.reactions;
    EXPECT_DOUBLE_EQ(rx.init_global[0], 0.8);
    ASSERT_EQ(rx.init_elem_idx.size(), 2u);
    EXPECT_EQ(rx.init_elem_is_link[0], 0);
    EXPECT_EQ(rx.init_elem_idx[0], ctx.node_names.find("J0"));
    EXPECT_EQ(rx.init_elem_species[0], 0);          // HOCL
    EXPECT_DOUBLE_EQ(rx.init_elem_value[0], 1.2);
    EXPECT_EQ(rx.init_elem_is_link[1], 1);
    EXPECT_EQ(rx.init_elem_idx[1], ctx.link_names.find("C1"));
    EXPECT_EQ(rx.init_elem_species[1], 1);          // NH2CL
    EXPECT_DOUBLE_EQ(rx.init_elem_value[1], 0.05);

    std::remove("_rx_nl.inp");
    std::remove("_rx_nl.rxn");
}

// ---------------------------------------------------------------------------
// E-B1 — the error matrix stays loud AND transactional: a rejected config
// leaves ctx.reactions fully clean (the R2 lesson re-applied to the new
// vectors) and the species registry at the pollutant block only.
TEST_F(ReactionsConfigTest, NodeLinkQualityErrorsAreLoudAndTransactional) {
    struct Case { const char* tag; const char* rows; const char* needle; };
    const Case cases[] = {
        {"uelem", "NODE NOPE HOCL 1.0\n",   "unknown node 'NOPE'"},
        {"ulink", "LINK NOPE HOCL 1.0\n",   "unknown link 'NOPE'"},
        {"uspec", "NODE J0 NOPE 1.0\n",     "undeclared species 'NOPE'"},
        {"dup",   "NODE J0 HOCL 1.0\nNODE J0 HOCL 2.0\n", "duplicate row"},
        {"neg",   "NODE J0 HOCL -1.0\n",    "bad value for 'HOCL'"},
        {"scope", "CELL J0 HOCL 1.0\n",     "not GLOBAL, NODE, or LINK"},
    };
    for (const auto& c : cases) {
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr) << c.tag;
        std::string rxn = std::string("_rx_nle_") + c.tag + ".rxn";
        std::string inp = std::string("_rx_nle_") + c.tag + ".inp";
        std::string rpt = std::string("_rx_nle_") + c.tag + ".rpt";
        write_rxn(rxn.c_str(),
                  std::string("[REACTION_SPECIES]\nBULK HOCL MG\n"
                              "[REACTION_QUALITY]\n") + c.rows);
        write_deck(inp.c_str(),
                   "org.hydrocouple.openswmm.reactions  config=\"" + rxn +
                       "\"");
        EXPECT_NE(swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr,
                                   nullptr),
                  SWMM_OK) << c.tag;
        const auto& ctx = as_cpp_engine(e).context();
        EXPECT_TRUE(contains(ctx.errors, c.needle)) << c.tag;
        // Transactional: nothing committed.
        EXPECT_FALSE(ctx.reactions.configured) << c.tag;
        EXPECT_TRUE(ctx.reactions.init_elem_idx.empty()) << c.tag;
        EXPECT_EQ(ctx.species_registry.count(),
                  ctx.species_registry.pollutant_count()) << c.tag;
        swmm_engine_destroy(e);
        std::remove(rxn.c_str());
        std::remove(inp.c_str());
    }
}

// ---------------------------------------------------------------------------
// E-B2 — the NODE/LINK rows are CONSUMED: overridden elements read their row
// after the first step, others read the GLOBAL, under both the legacy CSTR
// binding (msx_* seeded lazily in ensureMsxState) and the ARD engine (cell
// and node-store seeding; the link probe is the volume-weighted published
// value, so a seed that touched only cell 0 of the conduit would drag the
// mean toward the GLOBAL and fail).
TEST_F(ReactionsConfigTest, NodeLinkQualityRowsConsumed) {
    struct Solver { const char* tag; const char* opt; };
    const Solver solvers[] = {
        {"leg", ""},
        {"ard", "[OPTIONS]\nQUALITY_SOLVER EULERIAN_ARD\n\n"},
    };
    for (const auto& s : solvers) {
        std::string rxn = std::string("_rx_nlc_") + s.tag + ".rxn";
        std::string inp = std::string("_rx_nlc_") + s.tag + ".inp";
        std::string rpt = std::string("_rx_nlc_") + s.tag + ".rpt";
        std::string out = std::string("_rx_nlc_") + s.tag + ".out";
        // The node probe targets a STORAGE node: under ARD a junction is a
        // zero-volume passthrough whose published conc is mass/volume — only
        // a node with storage holds initial MSX mass (the E-A2 lesson).
        write_rxn(rxn.c_str(),
                  "[REACTION_SPECIES]\nBULK HOCL MG\n"
                  "[REACTION_QUALITY]\n"
                  "GLOBAL HOCL 0.8\n"
                  "NODE   ST1 HOCL 1.2\n"
                  "LINK   C1  HOCL 0.5\n");
        {
            std::ofstream f(inp);
            f << "[TITLE]\nE-B2 consumption gate deck\n\n"
              << "[OPTIONS]\n"
              << "FLOW_UNITS           CFS\nFLOW_ROUTING         DYNWAVE\n"
              << "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
              << "END_DATE             01/01/2026\nEND_TIME             00:30:00\n"
              << "ROUTING_STEP         5\nREPORT_STEP          00:05:00\n"
              << s.opt
              << "[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth SurDepth Aponded\n"
              << "J0     10.0 10 0.5 0 0\n\n"
              // One outfall per conduit: C1 and C2 both ended at OUT, which
              // legacy refuses (ERROR 141 — an outfall takes at most one
              // connecting link), so the deck could never have run in real
              // SWMM.
              << "[OUTFALLS]\n;;Name Elev Type StageData Gated\n"
              << "OUT  7.0 FREE  NO\nOUT2 7.0 FREE  NO\n\n"
              << "[STORAGE]\n"
              << ";;Name Elev MaxDepth InitDepth Shape     Coeff Expon Const\n"
              << "ST1    8.5  10       0.5       FUNCTIONAL 0    0     1000\n\n"
              << "[CONDUITS]\n;;Name From To Length N Zin Zout Q0\n"
              << "C1 J0  OUT  400 0.013 0 0 0\n"
              << "C2 ST1 OUT2 400 0.013 0 0 0\n\n"
              << "[XSECTIONS]\n;;Link Shape G1 G2 G3 G4\n"
              << "C1 CIRCULAR 1.5 0 0 0\nC2 CIRCULAR 1.5 0 0 0\n\n"
              << "[POLLUTANTS]\n"
              << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut "
                 "CoFrac Cdwf Cinit\n"
              << "TSS    MG/L  0.0   0.0 0.0   0.0    NO       *        "
                 "0.0    0.0  0.0\n\n"
              << "[PROCESS_COMPONENTS]\n"
              << "org.hydrocouple.openswmm.reactions  config=\"" << rxn
              << "\"\n\n"
              << "[REPORT]\nINPUT NO\n";
        }
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr) << s.tag;
        ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(),
                                   nullptr), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK) << s.tag;
        ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK) << s.tag;
        double elapsed = 1.0;
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK) << s.tag;

        const auto& ctx = as_cpp_engine(e).context();
        const auto& rx  = ctx.reactions;
        const auto ns = static_cast<std::size_t>(rx.n_species());
        const auto st = static_cast<std::size_t>(ctx.node_names.find("ST1"));
        const auto c1 = static_cast<std::size_t>(ctx.link_names.find("C1"));
        ASSERT_GE(rx.msx_node_conc.size(), (st + 1) * ns) << s.tag;
        ASSERT_GE(rx.msx_link_conc.size(), (c1 + 1) * ns) << s.tag;
        // One 5 s step of real flow: the tolerance separates the override
        // from the 0.8 global by a wide margin (falsifier: both read 0.8).
        // The LINK band widened 0.05 → 0.15 when R4b's transport landed
        // (2026-09-01): one step now mixes a sliver of the upstream node's
        // state into C1 (measured 0.5846), which is TRANSPORT, not a lost
        // seed. The 0.8 global would read outside the band by 2x — the
        // discriminator this gate exists for is intact.
        EXPECT_NEAR(rx.msx_node_conc[st * ns], 1.2, 0.12)
            << s.tag << ": storage-node override lost";
        EXPECT_NEAR(rx.msx_link_conc[c1 * ns], 0.5, 0.15)
            << s.tag << ": link override lost (or only part of the conduit "
                        "was seeded)";

        swmm_engine_end(e);
        swmm_engine_destroy(e);
        std::remove(rxn.c_str());
        std::remove(inp.c_str());
    }
}

// ---------------------------------------------------------------------------
// E-D1 — an UNQUOTED comma inside a two-argument function survives the row
// capture: the expression payload is the raw remainder of the line, no
// longer tokenize-then-rejoin (which ate the comma and produced "unexpected
// number"). The pre-E-D1 double-quoted form keeps working.
TEST_F(ReactionsConfigTest, UnquotedCommaExpressionsParse) {
    write_rxn("_rx_comma.rxn",
              "[REACTION_SPECIES]\nBULK HOCL MG\n"
              "[REACTION_TERMS]\nCAPPED   MIN(HOCL, 2.0)\n"
              "[REACTION_PIPES]\nRATE HOCL -0.1 * MIN(HOCL, 2.0)\n"
              "[REACTION_TANKS]\nRATE HOCL \"MAX(HOCL, 0.5) * -0.1\"\n");
    write_deck("_rx_comma.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_comma.rxn\"");
    ASSERT_EQ(open_deck("_rx_comma.inp", "_rx_comma.rpt", "_rx_comma.out"),
              SWMM_OK);
    const auto& rx = as_cpp_engine(engine_).context().reactions;
    ASSERT_EQ(rx.term_expr_src.size(), 1u);
    EXPECT_EQ(rx.term_expr_src[0], "MIN(HOCL, 2.0)");   // comma intact
    EXPECT_EQ(rx.pipe_expr_src[0], "-0.1 * MIN(HOCL, 2.0)");
    EXPECT_EQ(rx.tank_expr_src[0], "MAX(HOCL, 0.5) * -0.1");  // unwrapped
    std::remove("_rx_comma.inp");
    std::remove("_rx_comma.rxn");
}

// ---------------------------------------------------------------------------
// E-B1 — the embedded-section path flows through the same apply (D-RQ3), so
// NODE/LINK rows work embedded for free. Pinned so a future split of the
// two paths cannot silently drop it.
TEST_F(ReactionsConfigTest, NodeLinkQualityWorksEmbedded) {
    write_deck("_rx_nlemb.inp", "",
               "[REACTION_SPECIES]\nBULK EMB_Q MG\n\n"
               "[REACTION_QUALITY]\nNODE J0 EMB_Q 2.5\n\n");
    ASSERT_EQ(open_deck("_rx_nlemb.inp", "_rx_nlemb.rpt", "_rx_nlemb.out"),
              SWMM_OK);
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(ctx.reactions.init_elem_idx.size(), 1u);
    EXPECT_EQ(ctx.reactions.init_elem_idx[0], ctx.node_names.find("J0"));
    EXPECT_DOUBLE_EQ(ctx.reactions.init_elem_value[0], 2.5);
    std::remove("_rx_nlemb.inp");
}

}  // namespace

// ---------------------------------------------------------------------------
// Physics review A1 (2026-10-15): no engine populates the hydraulic variables
// D, Q, U, RE, US, FF or AV, so an expression using one read 0 and the
// kinetics silently did nothing. Until a hydraulics provider fills them, a
// reaction system that references one is refused, naming the variable and
// the expression. HRT, DT and TEMP are populated and stay accepted. Files
// are kept in the test's working directory for review.
// ---------------------------------------------------------------------------
TEST_F(ReactionsConfigTest, UnpopulatedHydraulicVariablesAreRefused) {
    const char* kUnpopulated[] = {"D", "Q", "U", "RE", "US", "FF", "AV"};
    for (const char* v : kUnpopulated) {
        const std::string tag = std::string("_rx_hv_") + v;
        write_rxn((tag + ".rxn").c_str(),
                  std::string("[REACTION_SPECIES]\nBULK A MG\n"
                              "[REACTION_PIPES]\nRATE  A  -0.1 * ") + v + " * A\n");
        write_deck((tag + ".inp").c_str(),
                   "org.hydrocouple.openswmm.reactions  config=\"" + tag + ".rxn\"");
        SWMM_Engine e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        EXPECT_NE(swmm_engine_open(e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                                   (tag + ".out").c_str(), nullptr), SWMM_OK)
            << v << " was accepted and would read 0";
        EXPECT_TRUE(contains(as_cpp_engine(e).context().errors,
                             std::string("hydraulic variable '") + v + "'"))
            << "no error names " << v;
        swmm_engine_destroy(e);
    }
    // A term is refused too, even if only a rate uses it indirectly.
    write_rxn("_rx_hv_term.rxn",
              "[REACTION_SPECIES]\nBULK A MG\n[REACTION_TERMS]\nKW  0.2 * AV\n"
              "[REACTION_TANKS]\nRATE  A  -KW * A\n");
    write_deck("_rx_hv_term.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_hv_term.rxn\"");
    EXPECT_NE(open_deck("_rx_hv_term.inp", "_rx_hv_term.rpt", "_rx_hv_term.out"), SWMM_OK);
    EXPECT_TRUE(contains(as_cpp_engine(engine_).context().errors, "[REACTION_TERMS] 'KW'"));
}

TEST_F(ReactionsConfigTest, PopulatedHydraulicVariablesStayAccepted) {
    write_rxn("_rx_hv_ok.rxn",
              "[REACTION_SPECIES]\nBULK A MG\n"
              "[REACTION_PIPES]\nRATE  A  -0.001 * A * (TEMP / 20) * (1 + 0 * DT)\n"
              "[REACTION_TANKS]\nRATE  A  -0.001 * A * HRT / (HRT + 1)\n");
    write_deck("_rx_hv_ok.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_hv_ok.rxn\"");
    EXPECT_EQ(open_deck("_rx_hv_ok.inp", "_rx_hv_ok.rpt", "_rx_hv_ok.out"), SWMM_OK);
}

// Physics review A3: TIMESTEP and AREA_UNITS are parsed but nothing uses
// them. Say so when a file sets them; the writer's own defaults stay quiet.
TEST_F(ReactionsConfigTest, UnusedTimestepAndAreaUnitsAreReported) {
    write_rxn("_rx_unused.rxn",
              "[REACTION_OPTIONS]\nTIMESTEP 60\nAREA_UNITS M2\n"
              "[REACTION_SPECIES]\nBULK A MG\n[REACTION_PIPES]\nRATE  A  -0.001 * A\n");
    write_deck("_rx_unused.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_unused.rxn\"");
    ASSERT_EQ(open_deck("_rx_unused.inp", "_rx_unused.rpt", "_rx_unused.out"), SWMM_OK);
    const auto& w = as_cpp_engine(engine_).context().warnings;
    EXPECT_TRUE(contains(w, "TIMESTEP 60 is not used")) << "no TIMESTEP warning";
    EXPECT_TRUE(contains(w, "AREA_UNITS M2 has no effect")) << "no AREA_UNITS warning";

    SWMM_Engine e = swmm_engine_create();
    write_rxn("_rx_defaults.rxn",
              "[REACTION_OPTIONS]\nTIMESTEP 0\nAREA_UNITS FT2\n"
              "[REACTION_SPECIES]\nBULK A MG\n[REACTION_PIPES]\nRATE  A  -0.001 * A\n");
    write_deck("_rx_defaults.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_rx_defaults.rxn\"");
    ASSERT_EQ(swmm_engine_open(e, "_rx_defaults.inp", "_rx_defaults.rpt",
                               "_rx_defaults.out", nullptr), SWMM_OK);
    const auto& w2 = as_cpp_engine(e).context().warnings;
    EXPECT_FALSE(contains(w2, "TIMESTEP")) << "the written default warned";
    EXPECT_FALSE(contains(w2, "AREA_UNITS")) << "the written default warned";
    swmm_engine_destroy(e);
}

// Multispecies Phase 2 (S4): [REACTION_RAINFALL] gives a species a rainfall
// concentration. It parses, round-trips through the writer and the C API,
// and refuses undeclared species, negative values and duplicate rows.
TEST_F(ReactionsConfigTest, RainfallConcentrationParsesRoundTripsAndValidates) {
    write_rxn("_rx_rain.rxn", "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
                              "[REACTION_RAINFALL]\nB  2.5\n");
    write_deck("_rx_rain.inp", "org.hydrocouple.openswmm.reactions  config=\"_rx_rain.rxn\"");
    ASSERT_EQ(open_deck("_rx_rain.inp", "_rx_rain.rpt", "_rx_rain.out"), SWMM_OK);
    const auto& rx = as_cpp_engine(engine_).context().reactions;
    ASSERT_EQ(rx.rain_conc.size(), 2u);
    EXPECT_EQ(rx.rain_conc[0], 0.0);
    EXPECT_EQ(rx.rain_conc[1], 2.5);
    const std::string text = openswmm::transport::serializeReactionSystem(as_cpp_engine(engine_).context());
    EXPECT_NE(text.find("[REACTION_RAINFALL]"), std::string::npos) << text;
    double v = -1.0;
    ASSERT_EQ(swmm_reaction_rain_conc_get(engine_, 1, &v), SWMM_OK);
    EXPECT_EQ(v, 2.5);
    ASSERT_EQ(swmm_reaction_rain_conc_set(engine_, 0, 4.0), SWMM_OK);
    EXPECT_EQ(rx.rain_conc[0], 4.0);
    EXPECT_NE(swmm_reaction_rain_conc_set(engine_, 0, -1.0), SWMM_OK);
    for (const char* bad : {"Z  1\n", "A  -1\n", "A  1\nA  2\n"}) {
        write_rxn("_rx_rain_bad.rxn", std::string("[REACTION_SPECIES]\nBULK A MG\n[REACTION_RAINFALL]\n") + bad);
        write_deck("_rx_rain_bad.inp", "org.hydrocouple.openswmm.reactions  config=\"_rx_rain_bad.rxn\"");
        SWMM_Engine e = swmm_engine_create();
        EXPECT_NE(swmm_engine_open(e, "_rx_rain_bad.inp", "_rx_rain_bad.rpt", "_rx_rain_bad.out", nullptr), SWMM_OK) << bad;
        EXPECT_TRUE(contains(as_cpp_engine(e).context().errors, "[REACTION_RAINFALL]")) << bad;
        swmm_engine_destroy(e);
    }
}

TEST_F(ReactionsConfigTest, SourcePatternsParametersAndReportRoundTrip) {
    cleanup_prefixes_.push_back("_rx_extended");
    const std::string body =
        "[REACTION_SPECIES]\nBULK A MG\nBULK B UG\n"
        "[REACTION_COEFFICIENTS]\nPARAMETER k 0.1\n"
        "[REACTION_PARAMETERS]\nNODE J0 k 0.2\nLINK C1 k 0.3\n"
        "[REACTION_PATTERNS]\npulse 1 0\npulse 2\n"
        "[REACTION_SOURCES]\nMASS J0 A 28.317 pulse\nCONC OUT B 3\n"
        "[REACTION_REPORT]\nSPECIES ALL NO\nSPECIES B YES\n";
    write_rxn("_rx_extended.rxn", body);
    write_deck("_rx_extended.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_extended.rxn\"");
    ASSERT_EQ(open_deck("_rx_extended.inp", "_rx_extended.rpt", "_rx_extended.out"), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    const auto first = openswmm::transport::serializeReactionSystem(ctx);
    EXPECT_EQ(ctx.reactions.source_patterns[0].factors, (std::vector<double>{1, 0, 2}));
    EXPECT_EQ(ctx.reactions.report_enabled, (std::vector<uint8_t>{0, 1}));
    char error[1024] = {};
    ASSERT_EQ(swmm_reactions_apply_text(engine_, first.c_str(), error, sizeof(error)), SWMM_OK) << error;
    EXPECT_EQ(openswmm::transport::serializeReactionSystem(ctx), first);
    EXPECT_NE(swmm_reaction_species_remove(engine_, 0), SWMM_OK); // source still owns A
    EXPECT_NE(swmm_reaction_coeff_remove(engine_, 0), SWMM_OK); // override still owns k
    EXPECT_EQ(openswmm::transport::serializeReactionSystem(ctx), first);
    std::vector<double> gathered;
    EXPECT_DOUBLE_EQ(ctx.reactions.coefficients(false, 0, gathered)[0], 0.2);
    EXPECT_DOUBLE_EQ(ctx.reactions.coefficients(true, 0, gathered)[0], 0.3);
    EXPECT_DOUBLE_EQ(ctx.reactions.coef_value[0], 0.1);
}

TEST_F(ReactionsConfigTest, ExtendedSectionsValidateTransactionally) {
    cleanup_prefixes_.push_back("_rx_validate");
    write_rxn("_rx_validate.rxn", "[REACTION_SPECIES]\nBULK A MG\nWALL W MG\n"
        "[REACTION_COEFFICIENTS]\nPARAMETER p 1\nCONSTANT k 1\n");
    write_deck("_rx_validate.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_validate.rxn\"");
    ASSERT_EQ(open_deck("_rx_validate.inp", "_rx_validate.rpt", "_rx_validate.out"), SWMM_OK);
    const auto before = openswmm::transport::serializeReactionSystem(as_cpp_engine(engine_).context());
    const std::vector<std::string> invalid = {
        "[REACTION_PARAMETERS]\nNODE J0 k 2\n", // constants cannot be overridden
        "[REACTION_PARAMETERS]\nNODE missing p 2\n",
        "[REACTION_PARAMETERS]\nLINK C1 p nan\n",
        "[REACTION_PARAMETERS]\nNODE J0 p 2\nNODE J0 p 3\n",
        "[REACTION_PATTERNS]\npulse -1\n",
        "[REACTION_PATTERNS]\npulse inf\n",
        "[REACTION_SOURCES]\nMASS J0 W 1\n",
        "[REACTION_SOURCES]\nMASS J0 A 1 missing\n",
        "[REACTION_SOURCES]\nSETPOINT J0 A 1\n",
        "[REACTION_SOURCES]\nCONC J0 A 1\nMASS J0 A 2\n",
        "[REACTION_REPORT]\nSPECIES missing YES\n",
        "[REACTION_REPORT]\nSPECIES A MAYBE\n",
        "[REACTION_REPORT]\nSPECIES A NO\nSPECIES ALL YES\n"};
    for (const auto& suffix : invalid) {
        SCOPED_TRACE(suffix);
        char error[1024] = {};
        EXPECT_NE(swmm_reactions_apply_text(engine_, (before + suffix).c_str(), error, sizeof(error)), SWMM_OK);
        EXPECT_NE(error[0], '\0');
        EXPECT_EQ(openswmm::transport::serializeReactionSystem(as_cpp_engine(engine_).context()), before);
    }
}

TEST_F(ReactionsConfigTest, SourceMassUnitsAndPatternClockDoNotAccumulateAcrossSteps) {
    cleanup_prefixes_.push_back("_rx_source");
    write_rxn("_rx_source.rxn", "[REACTION_OPTIONS]\nPATTERN_TIMESTEP 60\nRATE_UNITS DAY\n"
        "[REACTION_SPECIES]\nBULK A UG\n"
        "[REACTION_PATTERNS]\npulse 1 0 2\n"
        "[REACTION_SOURCES]\nMASS J0 A 28.317 pulse\n");
    write_deck("_rx_source.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_source.rxn\"");
    ASSERT_EQ(open_deck("_rx_source.inp", "_rx_source.rpt", "_rx_source.out"), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    openswmm::inflow::InflowSolver inflow;
    inflow.init(ctx);
    for (int minute = 0; minute < 7; ++minute) {
        inflow.computeAll(ctx, openswmm::datetime::addSeconds(ctx.options.start_date, minute * 60), 5);
        EXPECT_NEAR(ctx.reactions.msx_ext_mass_in[0], (std::vector<double>{1, 0, 2})[minute % 3], 1.e-12);
    }
    ctx.reactions.sources[0].mass = false;
    ctx.reactions.sources[0].value = 7;
    // FLOW rows in the model are absent: a concentration adds no phantom mass.
    inflow.computeAll(ctx, ctx.options.start_date, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_ext_mass_in[0], 0.0);
}

TEST_F(ReactionsConfigTest, PerElementParameterFeedsTermsRatesEquilibriaAndFormula) {
    cleanup_prefixes_.push_back("_rx_coef");
    write_rxn("_rx_coef.rxn", "[REACTION_OPTIONS]\nRATE_UNITS SEC\nATOL 1e-10\nRTOL 1e-8\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\nBULK C MG\n"
        "[REACTION_COEFFICIENTS]\nPARAMETER k 0.01\n"
        "[REACTION_PARAMETERS]\nNODE J0 k 0.02\nLINK C1 k 0.03\n"
        "[REACTION_TERMS]\nloss k * A\n"
        "[REACTION_TANKS]\nRATE A -loss\nEQUIL B B - k * 100\nFORMULA C k * 1000\n"
        "[REACTION_PIPES]\nRATE A -loss\nEQUIL B B - k * 100\nFORMULA C k * 1000\n");
    write_deck("_rx_coef.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_coef.rxn\"");
    ASSERT_EQ(open_deck("_rx_coef.inp", "_rx_coef.rpt", "_rx_coef.out"), SWMM_OK);
    auto& rx = as_cpp_engine(engine_).context().reactions;
    for (int scope = 0; scope < 2; ++scope) {
        openswmm::transport::RxWorkspace ws; ws.init(rx);
        double species[] = {10, 0, 0}, hyd[static_cast<int>(openswmm::RxHydVar::COUNT_)] = {};
        std::vector<double> coefficients;
        const auto rep = openswmm::transport::ReactionIntegrator::step(rx, scope == 0, 5, species, hyd, ws,
            nullptr, rx.coefficients(scope != 0, 0, coefficients));
        ASSERT_TRUE(rep.ok) << rep.error;
        const double k = scope == 0 ? .02 : .03;
        EXPECT_NEAR(species[0], 10 * std::exp(-5 * k), 1.e-6);
        EXPECT_NEAR(species[1], 100 * k, 1.e-7);
        EXPECT_NEAR(species[2], 1000 * k, 1.e-7);
    }
    EXPECT_DOUBLE_EQ(rx.coef_value[0], .01);
}

TEST_F(ReactionsConfigTest, DryStorageDoesNotExecuteTankFormula) {
    cleanup_prefixes_.push_back("_rx_dry");
    write_rxn("_rx_dry.rxn", "[REACTION_SPECIES]\nBULK A MG\n"
        "[REACTION_TANKS]\nFORMULA A 99\n[REACTION_QUALITY]\nGLOBAL A 7\n");
    write_deck("_rx_dry.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_dry.rxn\"");
    ASSERT_EQ(open_deck("_rx_dry.inp", "_rx_dry.rpt", "_rx_dry.out"), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    ctx.nodes.type[0] = openswmm::NodeType::STORAGE;
    ctx.nodes.volume[0] = 0;
    ctx.nodes.old_volume[0] = 0;
    openswmm::transport::reactLegacyNodes(ctx, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_node_conc[0], 7);
    ctx.nodes.volume[0] = 0.01; // below the quality reactor's one-litre threshold
    openswmm::transport::reactLegacyNodes(ctx, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_node_conc[0], 7);
    ctx.nodes.volume[0] = 1;
    openswmm::transport::reactLegacyNodes(ctx, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_node_conc[0], 99);
}

TEST_F(ReactionsConfigTest, WallStateStaysAttachedWhileBulkAdvects) {
    cleanup_prefixes_.push_back("_rx_wall");
    write_rxn("_rx_wall.rxn", "[REACTION_SPECIES]\nBULK A MG\nWALL W MG\n"
        "[REACTION_QUALITY]\nGLOBAL A 2\nGLOBAL W 3\nNODE J0 W 11\nLINK C1 W 17\n");
    write_deck("_rx_wall.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_wall.rxn\"");
    ASSERT_EQ(open_deck("_rx_wall.inp", "_rx_wall.rpt", "_rx_wall.out"), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
    openswmm::transport::ensureMsxState(ctx);
    const int node = ctx.node_names.find("J0"), link = ctx.link_names.find("C1");
    ctx.links.flow[link] = 2;
    ctx.nodes.qual_vol_in[node] = 10;
    ctx.reactions.msx_ext_mass_in.assign(ctx.n_nodes() * 2, 0);
    ctx.reactions.msx_ext_mass_in[node * 2] = 100;
    for (int step = 0; step < 3; ++step) openswmm::transport::routeLegacyMsx(ctx, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_node_conc[node * 2 + 1], 11);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_link_conc[link * 2 + 1], 17);
    EXPECT_GT(ctx.reactions.msx_node_conc[node * 2], 2);
    openswmm::transport::bookMsxSystemOutflow(ctx, node, 2, false, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_ledger.outflow[1], 0);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_ledger.init[1], 0);
}

TEST_F(ReactionsConfigTest, ConcentrationSourceUsesExternalWaterAndDryWeatherFlow) {
    cleanup_prefixes_.push_back("_rx_sourcewater");
    write_rxn("_rx_sourcewater.rxn", "[REACTION_SPECIES]\nBULK A MG\n"
        "[REACTION_SOURCES]\nCONC J0 A 7\n");
    write_deck("_rx_sourcewater.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_sourcewater.rxn\"",
               "[DWF]\nJ0 FLOW 2\n[INFLOWS]\nJ0 FLOW * FLOW 1 1 3\n");
    ASSERT_EQ(open_deck("_rx_sourcewater.inp", "_rx_sourcewater.rpt", "_rx_sourcewater.out"), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    openswmm::inflow::InflowSolver inflow;
    inflow.init(ctx);
    inflow.computeAll(ctx, ctx.options.start_date, 5);
    EXPECT_DOUBLE_EQ(ctx.nodes.ext_inflow[0], 3);
    EXPECT_DOUBLE_EQ(ctx.nodes.dwf_inflow[0], 2);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_ext_mass_in[0], 35);
    ctx.reactions.sources[0].mass = true;
    ctx.reactions.sources[0].value = 28.317;
    // The routing assembler clears water rates before each computeAll call.
    std::fill(ctx.nodes.ext_inflow.begin(), ctx.nodes.ext_inflow.end(), 0.0);
    std::fill(ctx.nodes.dwf_inflow.begin(), ctx.nodes.dwf_inflow.end(), 0.0);
    inflow.computeAll(ctx, ctx.options.start_date, 5);
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_ext_mass_in[0], 1);
    EXPECT_DOUBLE_EQ(ctx.nodes.ext_inflow[0], 3); // a mass source adds no water
}

TEST_F(ReactionsConfigTest, PatternTimestepOptionUsesSecondsAndRejectsNonfiniteValues) {
    cleanup_prefixes_.push_back("_rx_patternoption");
    write_rxn("_rx_patternoption.rxn", "[REACTION_SPECIES]\nBULK A MG\n");
    write_deck("_rx_patternoption.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_patternoption.rxn\"");
    ASSERT_EQ(open_deck("_rx_patternoption.inp", "_rx_patternoption.rpt", "_rx_patternoption.out"), SWMM_OK);
    EXPECT_EQ(swmm_reaction_option_set(engine_, "PATTERN_TIMESTEP", "90"), SWMM_OK);
    char value[64];
    ASSERT_EQ(swmm_reaction_option_get(engine_, "PATTERN_TIMESTEP", value, sizeof(value)), SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(value), 90);
    for (const auto* invalid : {"0", "-1", "nan", "inf"})
        EXPECT_NE(swmm_reaction_option_set(engine_, "PATTERN_TIMESTEP", invalid), SWMM_OK);
}

TEST_F(ReactionsConfigTest, DeletingGeometryDropsOwnedRowsAndPreservesSurvivingIdentities) {
    cleanup_prefixes_.push_back("_rx_delete");
    write_rxn("_rx_delete.rxn", "[REACTION_SPECIES]\nBULK A MG\n"
        "[REACTION_COEFFICIENTS]\nPARAMETER k 1\n"
        "[REACTION_PARAMETERS]\nNODE J0 k 2\nNODE OUT k 3\nLINK C1 k 4\n"
        "[REACTION_SOURCES]\nCONC J0 A 1\nMASS OUT A 28.317\n"
        "[REACTION_QUALITY]\nNODE J0 A 2\nNODE OUT A 3\nLINK C1 A 4\n");
    write_deck("_rx_delete.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_delete.rxn\"");
    ASSERT_EQ(open_deck("_rx_delete.inp", "_rx_delete.rpt", "_rx_delete.out"), SWMM_OK);
    auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(swmm_node_delete(engine_, ctx.node_names.find("J0"), nullptr), SWMM_OK);
    ASSERT_EQ(ctx.reactions.sources.size(), 1u);
    EXPECT_EQ(ctx.reactions.sources[0].node, ctx.node_names.find("OUT"));
    ASSERT_EQ(ctx.reactions.parameter_overrides.size(), 1u);
    EXPECT_EQ(ctx.reactions.parameter_overrides[0].element, ctx.node_names.find("OUT"));
    EXPECT_DOUBLE_EQ(ctx.reactions.parameter_overrides[0].value, 3);
    ASSERT_EQ(ctx.reactions.init_elem_idx.size(), 1u);
    EXPECT_EQ(ctx.reactions.init_elem_idx[0], ctx.node_names.find("OUT"));
    const auto text = openswmm::transport::serializeReactionSystem(ctx);
    EXPECT_EQ(text.find("J0"), std::string::npos);
    EXPECT_EQ(text.find("C1"), std::string::npos);
    EXPECT_NE(text.find("OUT"), std::string::npos);
}

TEST_F(ReactionsConfigTest, SubcatchmentExpressionsRejectUnavailableContextTransactionally) {
    cleanup_prefixes_.push_back("_rx_wet_context");
    write_rxn("_rx_wet_context.rxn", "[REACTION_SPECIES]\nBULK A MG\nWALL W MG\n"
        "[REACTION_TERMS]\nwall_term W\nhrt_term HRT\n");
    write_deck("_rx_wet_context.inp", "org.hydrocouple.openswmm.reactions config=\"_rx_wet_context.rxn\"");
    ASSERT_EQ(open_deck("_rx_wet_context.inp", "_rx_wet_context.rpt", "_rx_wet_context.out"), SWMM_OK);
    const auto before = openswmm::transport::serializeReactionSystem(as_cpp_engine(engine_).context());
    for (const auto* expression : {"W", "wall_term", "HRT", "hrt_term", "D", "GW_THETA", "TSS"}) {
        SCOPED_TRACE(expression);
        char error[1024] = {}; int column = -1;
        EXPECT_NE(swmm_reaction_validate_expression(engine_, SWMM_RXN_SCOPE_SUBCATCHMENT, expression, error, sizeof(error), &column), SWMM_OK);
        EXPECT_NE(swmm_reaction_expr_set(engine_, SWMM_RXN_SCOPE_SUBCATCHMENT, 0, SWMM_RXN_FORM_RATE, expression), SWMM_OK);
        EXPECT_EQ(openswmm::transport::serializeReactionSystem(as_cpp_engine(engine_).context()), before);
    }
    EXPECT_NE(swmm_reaction_expr_set(engine_, SWMM_RXN_SCOPE_SUBCATCHMENT, 1, SWMM_RXN_FORM_RATE, "-W"), SWMM_OK);
    ASSERT_EQ(swmm_reaction_expr_set(engine_, SWMM_RXN_SCOPE_SUBCATCHMENT, 0, SWMM_RXN_FORM_RATE, "-A * DT / 100"), SWMM_OK);
    int form = 0; char expr[128];
    ASSERT_EQ(swmm_reaction_expr_get(engine_, SWMM_RXN_SCOPE_SUBCATCHMENT, 0, &form, expr, sizeof(expr)), SWMM_OK);
    EXPECT_EQ(form, SWMM_RXN_FORM_RATE);
    EXPECT_STREQ(expr, "-A * DT / 100");
    ASSERT_EQ(swmm_reaction_species_add(engine_, "extra", 0, "MG", 0, 0), SWMM_OK);
    ASSERT_EQ(swmm_reaction_species_remove(engine_, 2), SWMM_OK);
    EXPECT_EQ(as_cpp_engine(engine_).context().reactions.subcatch_form.size(), 2u);
}
