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
 * @file test_reaction_legacy_binding.cpp
 * @brief R4 gates: LEGACY quality engine reaction binding
 *        (plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R4).
 *
 * @details Falsifiers, each unique to the defense under test. Measured
 *          separations are in
 *          tests/output/r4_validation_2026-08-16/r4_probe3.log.
 *          - ExactExponential: node decay over N steps must track
 *            exp(−k·N·dt) AND exceed the legacy linearized (1−k·dt)^N by a
 *            wide margin. kdecay is 0.03, not 0.01: at 0.01 the exponential
 *            and the linear product differ by only 3.15%, which left the
 *            discrimination floor 0.15% below the true value — a check with
 *            no headroom. At 0.03 they differ by 35%.
 *          - LinkDecayIsNotDoubleApplied is the ONLY gate on the in-mix
 *            `k = 0.0` zeroing in updateLinkQuality. The node gates cannot
 *            see it: J0 is upstream of C1, so link concentration never
 *            reaches the node. Falsifying the zeroing drives the link/legacy
 *            ratio from 1.215 to 0.197; dropping reactLegacyLinks entirely
 *            drives it to 26.4. The gate brackets both.
 *          - MsxPipeScopeFormulaTracksLinkPollutant covers [REACTION_PIPES]
 *            (tank scope alone leaves half the binding unexercised) and is
 *            falsified by dropping reactLegacyLinks: msx_link_conc stays 0
 *            while 3*links.conc is 16.0.
 *          - MsxFormulaReadsPollutant is falsified by removing PUSH_POLLUT
 *            resolution — the config then fails to open with
 *            "undefined identifier 'TSS'".
 *          - PollutantKineticsRowErrors asserts the R4-specific wording
 *            ("pollutant kinetics"), produced nowhere else.
 *          - ParityGuardWhenNoConfig asserts the LINEAR product — proving
 *            the legacy path is untouched without a reactions component
 *            (the validator's deck-level sha256 is the bitwise version).
 *          - The three bypass gates cover configurations in which NO engine
 *            runs the component. Each was a silent no-reaction run before
 *            R4 validation: MSX-only decks were gated out by the step
 *            loop's `n_pollutants() > 0`, and EULERIAN_ARD / IGNORE_QUALITY
 *            route around QualitySolver::execute() without saying so.
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

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

constexpr double kC0 = 10.0;     ///< initial TSS at the node (Cinit)
/// kdecay (1/s). Sized so exp(−k·dt) and the legacy (1−k·dt) product
/// separate by 35% over the horizon — see the file comment.
constexpr double kK  = 0.03;
constexpr double kDt = 5.0;      ///< ROUTING_STEP (s)

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

/// Isolated-ish node deck: J0 (initial depth, no inflow) drains to OUT.
/// TSS has kdecay and Cinit, so node conc evolves by decay alone (no
/// inflow ⇒ mixing is a no-op on concentration). 2-minute horizon keeps
/// the node wet throughout.
/// `extra_options` appends to [OPTIONS]; `pollutants` false omits the
/// [POLLUTANTS] section entirely (the MSX-only model shape).
void write_deck(const char* path, const std::string& pc_lines,
                const std::string& extra_options = "",
                bool pollutants = true) {
    std::ofstream f(path);
    f << "[TITLE]\nR4 legacy binding gate deck\n\n[OPTIONS]\n"
      << "FLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\n"
      << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\n"
      << "END_DATE 01/01/2026\nEND_TIME 00:02:00\n"
      << "ROUTING_STEP 5\nREPORT_STEP 00:01:00\n"
      << extra_options << "\n"
      << "[JUNCTIONS]\nJ0 10.0 10 0.5 0 0\n\n"
      << "[OUTFALLS]\nOUT 7.0 FREE  NO\n\n"
      << "[CONDUITS]\nC1 J0 OUT 400 0.013 0 0 0\n\n"
      << "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0\n\n";
    if (pollutants)
        f << "[POLLUTANTS]\n"
          << ";;Name Units Crain Cgw Crdii Kdecay SnowOnly CoPollut CoFrac Cdwf Cinit\n"
          << "TSS    MG/L  0     0   0     " << (kK * 86400.0)  // 1/day column (KD1)
          << "    NO       *        0      0    " << kC0 << "\n\n";
    if (!pc_lines.empty())
        f << "[PROCESS_COMPONENTS]\n" << pc_lines << "\n\n";
    f << "[REPORT]\nINPUT NO\n";
}

void write_rxn(const char* path, const std::string& body) {
    std::ofstream c(path);
    c << body;
}

class ReactionLegacyBindingTest : public ::testing::Test {
protected:
    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_NE(engine_, nullptr);
    }
    void TearDown() override {
        if (engine_) { swmm_engine_destroy(engine_); engine_ = nullptr; }
    }
    /// Open + run to completion. ASSERT, not EXPECT, on the open: every
    /// caller reads a concentration array afterwards, and a failed open
    /// leaves those arrays EMPTY — with EXPECT the suite ran on and
    /// segfaulted on nodes.conc[0], turning "the config no longer compiles"
    /// into a crash instead of a reported failure. (Found by running the
    /// handoff's own PUSH_POLLUT falsifier.)
    void run_deck(const char* inp, const char* rpt, const char* out) {
        ASSERT_EQ(swmm_engine_open(engine_, inp, rpt, out, nullptr), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(engine_), SWMM_OK);
        ASSERT_EQ(swmm_engine_start(engine_, 1), SWMM_OK);
        double elapsed = 0.0;
        int guard = 0;
        do {
            ASSERT_EQ(swmm_engine_step(engine_, &elapsed), SWMM_OK);
        } while (elapsed > 0.0 && ++guard < 10000);
        ASSERT_EQ(swmm_engine_end(engine_), SWMM_OK);
    }
    /// Node J0's TSS concentration after a completed run (-1 if unavailable,
    /// which fails any band a caller asserts rather than reading past the end).
    double node_conc() const {
        const auto& c = as_cpp_engine(engine_).context().nodes.conc;
        return c.empty() ? -1.0 : c[0];
    }
    /// Conduit C1's TSS concentration after a completed run.
    double link_conc() const {
        const auto& c = as_cpp_engine(engine_).context().links.conc;
        return c.empty() ? -1.0 : c[0];
    }
    static bool warned(const openswmm::SimulationContext& ctx,
                       const std::string& needle) {
        for (const auto& w : ctx.warnings)
            if (w.find(needle) != std::string::npos) return true;
        return false;
    }
    static bool errored(const openswmm::SimulationContext& ctx,
                        const std::string& needle) {
        for (const auto& e : ctx.errors)
            if (e.find(needle) != std::string::npos) return true;
        return false;
    }
    /// Run a deck on its OWN engine — the link gate compares a with-component
    /// run against a without-component run of the same network, so it needs
    /// two live contexts.
    static double link_conc_of(const char* inp) {
        SWMM_Engine e = swmm_engine_create();
        const std::string rpt = std::string(inp) + ".rpt";
        const std::string out = std::string(inp) + ".out";
        double v = -1.0;
        if (swmm_engine_open(e, inp, rpt.c_str(), out.c_str(), nullptr)
            == SWMM_OK) {
            swmm_engine_initialize(e);
            swmm_engine_start(e, 1);
            double elapsed = 0.0;
            int guard = 0;
            do { swmm_engine_step(e, &elapsed); }
            while (elapsed > 0.0 && ++guard < 10000);
            swmm_engine_end(e);
            const auto& c = as_cpp_engine(e).context().links.conc;
            if (!c.empty()) v = c[0];
        }
        swmm_engine_destroy(e);
        return v;
    }
    SWMM_Engine engine_ = nullptr;
};

// Steps over the 2-minute horizon.
constexpr int kSteps = static_cast<int>(120.0 / kDt);
const double kLinear = std::pow(1.0 - kK * kDt, kSteps);   // legacy product
const double kExp    = std::exp(-kK * kDt * kSteps);       // exact

// ---------------------------------------------------------------------------
// Gate 1 — parity guard: WITHOUT a reactions component the node decays by
// the legacy linear product (proves the untouched path; validator sha256
// is the bitwise version of this claim).
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, ParityGuardWhenNoConfig) {
    write_deck("_r4_plain.inp", "");
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_plain.inp", "_r4_plain.rpt", "_r4_plain.out"));
    EXPECT_NEAR(node_conc(), kC0 * kLinear, 0.01 * kC0 * kLinear)
        << "legacy linear-decay path must be untouched without reactions";
    std::remove("_r4_plain.inp");
}

// ---------------------------------------------------------------------------
// Gate 2 — with a reactions component, decay is the exact exponential —
// and NOT the linear product, and NOT the double-decayed value.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, ExactExponentialReplacesLinearDecay) {
    write_rxn("_r4_inert.rxn",
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_TANKS]\nFORMULA X 0.0 * TSS\n");
    write_deck("_r4_exp.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_inert.rxn\"");
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_exp.inp", "_r4_exp.rpt", "_r4_exp.out"));
    const double c = node_conc();
    EXPECT_NEAR(c, kC0 * kExp, 0.01 * kC0 * kExp);
    // exp/linear = 1.3505 at this kdecay, so the 1.10 floor leaves 18%
    // headroom. (At the kdecay this deck originally used the two products
    // were 3.15% apart and the floor sat 0.15% under the true value — a
    // check that would flip on any small numerical drift.)
    EXPECT_GT(c, kC0 * kLinear * 1.10)
        << "conc matches the LINEAR product — binding did not engage";
    std::remove("_r4_exp.inp");
    std::remove("_r4_inert.rxn");
}

// ---------------------------------------------------------------------------
// Gate 3 — MSX FORMULA species reads the element's pollutant (PUSH_POLLUT).
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, MsxFormulaReadsPollutant) {
    write_rxn("_r4_form.rxn",
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_TANKS]\nFORMULA X 2.0 * TSS\n");
    write_deck("_r4_form.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_form.rxn\"");
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_form.inp", "_r4_form.rpt", "_r4_form.out"));
    const double c = node_conc();
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_FALSE(ctx.reactions.msx_node_conc.empty());
    // X at node 0 == 2 * TSS at node 0, at end of run.
    EXPECT_NEAR(ctx.reactions.msx_node_conc[0], 2.0 * c, 1e-9)
        << "FORMULA over a pollutant reference must track the element value";
    std::remove("_r4_form.inp");
    std::remove("_r4_form.rxn");
}

// ---------------------------------------------------------------------------
// Gate 4 — pollutant kinetics rows are a defined R4b deferral error.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, PollutantKineticsRowErrors) {
    write_rxn("_r4_pk.rxn",
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_TANKS]\nRATE TSS -0.5 * TSS\n");
    write_deck("_r4_pk.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_pk.rxn\"");
    EXPECT_NE(swmm_engine_open(engine_, "_r4_pk.inp", "_r4_pk.rpt",
                               "_r4_pk.out", nullptr), SWMM_OK);
    EXPECT_TRUE(errored(as_cpp_engine(engine_).context(),
                        "pollutant kinetics"));
    std::remove("_r4_pk.inp");
    std::remove("_r4_pk.rxn");
}

// ---------------------------------------------------------------------------
// Gate 5 — RATE MSX species evolve locally, with the R4b transport warning;
// their kinetics may reference pollutants.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, RateMsxEvolvesLocallyWithWarning) {
    write_rxn("_r4_rate.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_TANKS]\nRATE X 0.001 * TSS\n");
    write_deck("_r4_rate.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_rate.rxn\"");
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_rate.inp", "_r4_rate.rpt", "_r4_rate.out"));
    const auto& ctx = as_cpp_engine(engine_).context();
    EXPECT_TRUE(warned(ctx, "not yet transported between elements"));
    ASSERT_FALSE(ctx.reactions.msx_node_conc.empty());
    EXPECT_GT(ctx.reactions.msx_node_conc[0], 0.0)
        << "X accumulates from the TSS-driven source at the node";
    std::remove("_r4_rate.inp");
    std::remove("_r4_rate.rxn");
}

// ---------------------------------------------------------------------------
// Gate 6 — integration failure is loud, actionable, and non-fatal.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, FailureIsLoudActionableAndNonFatal) {
    write_rxn("_r4_stiff.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"   // default solver: RK5
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_QUALITY]\nGLOBAL X 1.0\n"
              "[REACTION_TANKS]\nRATE X -1000000.0 * X\n");
    write_deck("_r4_stiff.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_stiff.rxn\"");
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_stiff.inp", "_r4_stiff.rpt", "_r4_stiff.out"));
    const auto& ctx = as_cpp_engine(engine_).context();
    EXPECT_TRUE(warned(ctx, "SOLVER to ROS2 or BDF2"))
        << "the failure warning must carry the integrator's remedy text";
    ASSERT_FALSE(ctx.reactions.msx_node_conc.empty());
    EXPECT_DOUBLE_EQ(ctx.reactions.msx_node_conc[0], 1.0)
        << "failed element must be left at its prior state, not corrupted";
    std::remove("_r4_stiff.inp");
    std::remove("_r4_stiff.rxn");
}

// ---------------------------------------------------------------------------
// Gate 7 — the in-mix linear decay in updateLinkQuality is zeroed exactly
// once when reactions are active. No node gate can see this: J0 is UPSTREAM
// of C1, so link concentration never feeds back into nodes.conc, and the
// handoff's own falsifier (i) left every delivered gate green.
//
// The claim is bracketed rather than pinned to a magic number: reacting
// links must land ABOVE the legacy run (mix-then-exponential is gentler than
// decay-then-mix) but nowhere near the undecayed value.
//   measured  correct 1.215x legacy | double-decay 0.197x | no decay 26.4x
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, LinkDecayIsNotDoubleApplied) {
    write_rxn("_r4_lnk.rxn",
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_PIPES]\nFORMULA X 0.0 * TSS\n");
    write_deck("_r4_lnk_plain.inp", "");
    write_deck("_r4_lnk_rxn.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_lnk.rxn\"");
    const double legacy = link_conc_of("_r4_lnk_plain.inp");
    const double rxn    = link_conc_of("_r4_lnk_rxn.inp");
    ASSERT_GT(legacy, 0.0);
    ASSERT_GT(rxn, 0.0);
    EXPECT_GT(rxn, legacy * 1.10)
        << "link decayed twice — the in-mix k was not zeroed (rxn/legacy = "
        << rxn / legacy << ")";
    EXPECT_LT(rxn, legacy * 1.50)
        << "link never decayed — reactLegacyLinks did not run (rxn/legacy = "
        << rxn / legacy << ")";
    std::remove("_r4_lnk_plain.inp");
    std::remove("_r4_lnk_rxn.inp");
    std::remove("_r4_lnk.rxn");
}

// ---------------------------------------------------------------------------
// Gate 8 — pipe scope. Gates 2-6 are all [REACTION_TANKS]; without this one
// reactLegacyLinks' MSX half, msx_link_conc and the tank=false integrator
// path are never executed.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, MsxPipeScopeFormulaTracksLinkPollutant) {
    write_rxn("_r4_pipe.rxn",
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_PIPES]\nFORMULA X 3.0 * TSS\n"
              "[REACTION_TANKS]\nFORMULA X 2.0 * TSS\n");
    write_deck("_r4_pipe.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_pipe.rxn\"");
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_pipe.inp", "_r4_pipe.rpt", "_r4_pipe.out"));
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_FALSE(ctx.reactions.msx_link_conc.empty());
    ASSERT_FALSE(ctx.reactions.msx_node_conc.empty());
    // Pipe scope uses the LINK coefficient, tank scope the NODE one — a
    // scope mix-up would make one of these track the wrong element.
    EXPECT_NEAR(ctx.reactions.msx_link_conc[0], 3.0 * link_conc(), 1e-9);
    EXPECT_NEAR(ctx.reactions.msx_node_conc[0], 2.0 * node_conc(), 1e-9);
    std::remove("_r4_pipe.inp");
    std::remove("_r4_pipe.rxn");
}

// ---------------------------------------------------------------------------
// Gate 9 — an MSX-only model (no [POLLUTANTS]) still reacts. The step loop
// gated the whole quality stage on `n_pollutants() > 0`, so before R4
// validation such a deck ran to completion with nothing reacted and nothing
// reported.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, MsxOnlyModelWithoutPollutantsStillReacts) {
    write_rxn("_r4_only.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_QUALITY]\nGLOBAL X 1.0\n"
              "[REACTION_TANKS]\nRATE X 0.001 * X\n");
    write_deck("_r4_only.inp",
               "org.hydrocouple.openswmm.reactions  config=\"_r4_only.rxn\"",
               "", /*pollutants=*/false);
    ASSERT_NO_FATAL_FAILURE(
        run_deck("_r4_only.inp", "_r4_only.rpt", "_r4_only.out"));
    const auto& ctx = as_cpp_engine(engine_).context();
    ASSERT_EQ(ctx.n_pollutants(), 0);
    ASSERT_FALSE(ctx.reactions.msx_node_conc.empty())
        << "MSX state was never even sized — the quality stage never ran";
    // X' = +0.001*X over 120 s from 1.0 ⇒ exp(0.12) = 1.1275.
    EXPECT_NEAR(ctx.reactions.msx_node_conc[0], std::exp(0.12), 1e-4);
    std::remove("_r4_only.inp");
    std::remove("_r4_only.rxn");
}

// ---------------------------------------------------------------------------
// Gate 10 — configurations in which NO engine runs the component say so.
// Both were silent before R4 validation: the user writes a .rxn file, the
// run completes, and nothing reacted.
// ---------------------------------------------------------------------------
TEST_F(ReactionLegacyBindingTest, BypassedBindingIsAnnounced) {
    write_rxn("_r4_byp.rxn",
              "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"
              "[REACTION_SPECIES]\nBULK X MG\n"
              "[REACTION_TANKS]\nRATE X 0.001 * TSS\n");
    const std::string pc =
        "org.hydrocouple.openswmm.reactions  config=\"_r4_byp.rxn\"";

    write_deck("_r4_byp_ard.inp", pc, "QUALITY_SOLVER EULERIAN_ARD\n");
    ASSERT_EQ(swmm_engine_open(engine_, "_r4_byp_ard.inp", "_r4_byp_ard.rpt",
                               "_r4_byp_ard.out", nullptr), SWMM_OK);
    // E4/R6: EULERIAN_ARD is no longer a bypass — the ARD engine runs its
    // own reaction binding with MSX species transported on the mesh, so the
    // R4-era "does not yet run reactions" warning must be GONE. The
    // positive coverage (reactions actually running under ARD) lives in
    // test_reaction_ard_binding.cpp.
    EXPECT_FALSE(warned(as_cpp_engine(engine_).context(),
                        "does not yet run reactions"))
        << "the retired R4-era EULERIAN_ARD bypass warning still fires";

    // A second engine — the fixture's is already open on the ARD deck.
    SWMM_Engine e2 = swmm_engine_create();
    write_deck("_r4_byp_ign.inp", pc, "IGNORE_QUALITY YES\n");
    ASSERT_EQ(swmm_engine_open(e2, "_r4_byp_ign.inp", "_r4_byp_ign.rpt",
                               "_r4_byp_ign.out", nullptr), SWMM_OK);
    EXPECT_TRUE(warned(as_cpp_engine(e2).context(), "IGNORE_QUALITY is YES"));
    swmm_engine_destroy(e2);

    std::remove("_r4_byp_ard.inp");
    std::remove("_r4_byp_ign.inp");
    std::remove("_r4_byp.rxn");
}

}  // namespace
