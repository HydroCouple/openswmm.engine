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
 * @file test_reaction_integrators.cpp
 * @brief R3 gates: EUL/RK5/ROS2/BDF2 + EQUIL Newton + FORMULA + rate units
 *        against ANALYTIC batch-reactor references
 *        (plans/transport/MULTISPECIES_REACTIONS_MSX_PLAN.md §5 R3).
 *
 * @details Falsifiers (unique artifacts per gate — no aliasing):
 *          - Decay/chain gates compare against closed-form exponentials at
 *            stated tolerances; loosening a solver breaks its own row only.
 *          - The STIFFNESS LADDER is the empirical check on D-R7's
 *            no-CVODE position: λ_fast/λ_slow = 1e6; ROS2 and BDF2 must
 *            deliver the slow-mode solution accurately in ONE reaction
 *            step (dt ≫ 1/λ_fast), where explicit EUL at the same step is
 *            unstable (asserted to diverge — a deliberately failing
 *            configuration proving the gate can tell the solvers apart).
 *          - RateUnitsGate: identical kinetics under HR vs SEC with k
 *            scaled by 3600 must agree to machine-level tolerance —
 *            falsified by removing the unitFactor scaling.
 *          - EQUIL/FORMULA gates assert exact algebra.
 *
 *          Reaction systems are built through the REAL config path (engine
 *          open with model.rxn) so gates cover parse→compile→integrate
 *          end-to-end; the integrator is then driven directly on a local
 *          species block (batch reactor: zero flow, tank scope).
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
#include "transport/components/ReactionModule/ReactionIntegrator.hpp"

namespace {

using openswmm::ReactionData;
using openswmm::RxHydVar;
using openswmm::transport::ReactionIntegrator;
using openswmm::transport::RxStepReport;
using openswmm::transport::RxWorkspace;

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

void write_deck(const char* path, const char* rxn_path) {
    std::ofstream f(path);
    f << "[TITLE]\nR3 gate deck\n\n[OPTIONS]\n"
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

/// Open a deck with the given model.rxn body; returns the engine (caller
/// destroys). The reaction system arrives fully parsed + compiled.
SWMM_Engine open_with_rxn(const std::string& rxn_body) {
    {
        std::ofstream c("_r3_sys.rxn");
        c << rxn_body;
    }
    write_deck("_r3_sys.inp", "_r3_sys.rxn");
    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    EXPECT_EQ(swmm_engine_open(e, "_r3_sys.inp", "_r3_sys.rpt", "_r3_sys.out",
                               nullptr), SWMM_OK);
    std::remove("_r3_sys.inp");
    std::remove("_r3_sys.rxn");
    return e;
}

RxStepReport run_step(const ReactionData& rx, double dt, double* species) {
    RxWorkspace ws;
    ws.init(rx);
    double hv[static_cast<int>(RxHydVar::COUNT_)] = {};
    return ReactionIntegrator::step(rx, /*tank=*/true, dt, species, hv, ws);
}

// ---------------------------------------------------------------------------
// Gate 1 — first-order decay vs exp() for all four solvers.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, FirstOrderDecayMatchesClosedForm) {
    const double k = 0.25;    // 1/s
    const double dt = 10.0;   // s
    const double c0 = 5.0;
    const double exact = c0 * std::exp(-k * dt);

    // EUL is NOT in this table. By MSX semantics it takes one explicit step
    // over dt, so at k*dt = 2.5 it returns c0*(1 - 2.5) = -7.5 — no relative
    // band around a positive exact value can express that, and asserting one
    // only hides the real behavior. Its exact value is pinned below, and
    // gate 3 asserts the loud failure when the same step blows up.
    const struct { const char* solver; double tol; } rows[] = {
        {"RK5",  1e-6},
        {"ROS2", 1e-3},
        {"BDF2", 5e-2},
    };
    for (const auto& r : rows) {
        SWMM_Engine e = open_with_rxn(
            std::string("[REACTION_OPTIONS]\nSOLVER ") + r.solver +
            "\nRATE_UNITS SEC\nATOL 1e-10\nRTOL 1e-8\n"
            "[REACTION_SPECIES]\nBULK A MG\n"
            "[REACTION_TANKS]\nRATE A -0.25 * A\n");
        const auto& rx = as_cpp_engine(e).context().reactions;
        ASSERT_TRUE(rx.compiled) << r.solver;
        double species[1] = {c0};
        const auto rep = run_step(rx, dt, species);
        // EXPECT, not ASSERT: an ASSERT here aborts the whole test function,
        // so a failure in one solver silently skips every solver after it.
        // (That is how BDF2's 248% error hid behind ROS2 during R3
        // validation.)
        EXPECT_TRUE(rep.ok) << r.solver << ": " << rep.error;
        if (rep.ok)
            EXPECT_NEAR(species[0], exact, r.tol * exact)
                << r.solver << " vs exact " << exact;
        swmm_engine_destroy(e);
    }

    // EUL: exactly one explicit step, c0*(1 - k*dt). Pinned exactly — this
    // is the documented MSX EUL contract, not an accuracy claim.
    {
        SWMM_Engine e = open_with_rxn(
            "[REACTION_OPTIONS]\nSOLVER EUL\nRATE_UNITS SEC\n"
            "ATOL 1e-10\nRTOL 1e-8\n"
            "[REACTION_SPECIES]\nBULK A MG\n"
            "[REACTION_TANKS]\nRATE A -0.25 * A\n");
        double species[1] = {c0};
        const auto rep = run_step(as_cpp_engine(e).context().reactions, dt,
                                  species);
        EXPECT_TRUE(rep.ok) << rep.error;
        EXPECT_EQ(rep.substeps, 1) << "EUL must take exactly one step";
        EXPECT_DOUBLE_EQ(species[0], c0 * (1.0 - k * dt));
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 1b — NONLINEAR reference: second-order decay A' = -k A², whose exact
// solution is A(t) = A0 / (1 + k A0 t).
//
// This gate exists for the cached FD Jacobian. Every other analytic gate here
// is LINEAR, and for a linear system a cached Jacobian is exactly equal to a
// freshly computed one — so they cannot detect a stale-Jacobian defect at
// all. Here J = -2kA genuinely varies along the trajectory: BDF2 iterates to
// a residual tolerance so a stale J may only cost iterations, but ROS2 has no
// corrector and a stale J moves its answer. Falsifier: raise kJacMaxAge far
// enough (or drop the rejection/Newton-failure invalidation) and ROS2 drifts
// off this reference.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, NonlinearDecayMatchesClosedFormWithCachedJacobian) {
    const double k = 0.5, dt = 20.0, a0 = 3.0;
    const double exact = a0 / (1.0 + k * a0 * dt);
    for (const char* solver : {"ROS2", "BDF2", "RK5"}) {
        SWMM_Engine e = open_with_rxn(
            std::string("[REACTION_OPTIONS]\nSOLVER ") + solver +
            "\nRATE_UNITS SEC\nATOL 1e-10\nRTOL 1e-8\n"
            "[REACTION_SPECIES]\nBULK A MG\n"
            "[REACTION_TANKS]\nRATE A -0.5 * A * A\n");
        const auto& rx = as_cpp_engine(e).context().reactions;
        ASSERT_TRUE(rx.compiled) << solver;
        double species[1] = {a0};
        const auto rep = run_step(rx, dt, species);
        EXPECT_TRUE(rep.ok) << solver << ": " << rep.error;
        if (rep.ok)
            EXPECT_NEAR(species[0], exact, 1e-5 * exact)
                << solver << " vs exact " << exact;
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 1c — the DEFAULT integrator is RK5, and a config that omits SOLVER
// gets it. Pinned because the choice is evidence-based and easy to flip back
// by accident: RK5 costs ~27x fewer substeps than ROS2 on ordinary
// (non-stiff) kinetics and, being stability- rather than accuracy-limited,
// also beats both implicit solvers on STIFF systems once tolerances tighten.
// See the table on ReactionData::solver. The trade is the cliff that gate 1d
// covers.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, DefaultSolverIsExplicitRK5) {
    SWMM_Engine e = open_with_rxn(
        "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"        // no SOLVER row
        "[REACTION_SPECIES]\nBULK A MG\n"
        "[REACTION_TANKS]\nRATE A -0.25 * A\n");
    const auto& rx = as_cpp_engine(e).context().reactions;
    ASSERT_TRUE(rx.compiled);
    EXPECT_EQ(static_cast<int>(rx.solver),
              static_cast<int>(openswmm::ReactionSolverKind::RK5));

    // …and it is cheap on the ordinary case at the shipping tolerances.
    double species[1] = {5.0};
    const auto rep = run_step(rx, 10.0, species);
    ASSERT_TRUE(rep.ok) << rep.error;
    EXPECT_NEAR(species[0], 5.0 * std::exp(-2.5), 1e-4 * 5.0 * std::exp(-2.5));
    EXPECT_LT(rep.substeps, 50) << "default-tolerance cost regressed";
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 1d — the cliff an explicit default buys, and its diagnostic. Past
// roughly lambda_fast*dt/3.3 substeps RK5 hits the cap. That is acceptable
// ONLY because it fails loudly AND names the remedy; "exceeded the substep
// cap" alone leaves the user with no action.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, ExplicitDefaultOnStiffKineticsFailsWithActionableMessage) {
    SWMM_Engine e = open_with_rxn(
        "[REACTION_OPTIONS]\nRATE_UNITS SEC\n"        // default solver
        "[REACTION_SPECIES]\nBULK A MG\n"
        "[REACTION_TANKS]\nRATE A -1000000.0 * A\n"); // lambda*dt = 1e7
    const auto& rx = as_cpp_engine(e).context().reactions;
    ASSERT_TRUE(rx.compiled);
    double species[1] = {1.0};
    const auto rep = run_step(rx, 10.0, species);
    EXPECT_FALSE(rep.ok) << "stiffness beyond the explicit solver must fail";
    EXPECT_NE(rep.error.find("SOLVER to ROS2 or BDF2"), std::string::npos)
        << "the failure must name the remedy, got: " << rep.error;
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — coupled chain A -> B (COUPLING FULL) vs the analytic solution.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, CoupledChainMatchesAnalyticSolution) {
    const double ka = 0.30, kb = 0.10, dt = 8.0, a0 = 4.0;
    const double a_exact = a0 * std::exp(-ka * dt);
    // b(0) = 0 ⇒ b(t) = a0·ka/(kb − ka)·(e^(−ka·t) − e^(−kb·t)); with
    // ka > kb both factors are negative, so b_ref > 0.
    const double b_ref = a0 * ka / (kb - ka) *
                         (std::exp(-ka * dt) - std::exp(-kb * dt));

    SWMM_Engine e = open_with_rxn(
        "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING FULL\n"
        "ATOL 1e-10\nRTOL 1e-8\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
        "[REACTION_TANKS]\nRATE A -0.30 * A\nRATE B 0.30 * A - 0.10 * B\n");
    const auto& rx = as_cpp_engine(e).context().reactions;
    double species[2] = {a0, 0.0};
    const auto rep = run_step(rx, dt, species);
    ASSERT_TRUE(rep.ok) << rep.error;
    EXPECT_NEAR(species[0], a_exact, 1e-6 * a_exact);
    // b(t) = a0 ka/(kb−ka) (e^−ka t − e^−kb t); with ka>kb the sign works out
    // negative/negative → positive.
    EXPECT_NEAR(species[1], b_ref, 1e-5 * std::fabs(b_ref));
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — STIFFNESS LADDER (the D-R7 empirical check). λ ratio 1e6:
// implicit solvers deliver the slow mode in one big step; explicit EUL at
// the same step size must blow up (proves the gate discriminates).
// ---------------------------------------------------------------------------
TEST(RxIntegrators, StiffnessLadderImplicitSolversHoldExplicitDiverges) {
    const double k_fast = 100.0;    // 1/s  (fast mode, 1/λ = 10 ms)
    const double k_slow = 1e-4;     // 1/s
    const double dt     = 100.0;    // s — 10,000× the fast time constant
    const double f0 = 1.0, s0 = 1.0;
    const double f_exact = f0 * std::exp(-k_fast * dt);   // ~0 (underflows to ~0)
    const double s_exact = s0 * std::exp(-k_slow * dt);   // 0.99005

    const std::string body_common =
        "\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-8\nRTOL 1e-6\n"
        "[REACTION_SPECIES]\nBULK F MG\nBULK S MG\n"
        "[REACTION_TANKS]\nRATE F -100.0 * F\nRATE S -0.0001 * S\n";

    for (const char* solver : {"ROS2", "BDF2"}) {
        SWMM_Engine e = open_with_rxn(
            std::string("[REACTION_OPTIONS]\nSOLVER ") + solver + body_common);
        const auto& rx = as_cpp_engine(e).context().reactions;
        double species[2] = {f0, s0};
        const auto rep = run_step(rx, dt, species);
        ASSERT_TRUE(rep.ok) << solver << ": " << rep.error;
        EXPECT_NEAR(species[1], s_exact, 2e-3 * s_exact)
            << solver << " slow-mode accuracy at dt >> 1/lambda_fast";
        EXPECT_LT(std::fabs(species[0]), 1e-2)
            << solver << " fast mode must be damped (L-stability), exact "
            << f_exact;
        // Record the cost: implicit solvers should NOT need ~k_fast*dt
        // substeps. (RK5 would; that asymmetry is the ladder's point.)
        // Economy is informative, not a physical claim (handoff §3a). The
        // premise "one big step" does not survive contact with an ACCURACY
        // controller: L-stability buys stability, not permission to skip an
        // unresolved transient, and atol 1e-10 forces the fast mode to be
        // resolved before h can grow. Measured during R3 validation:
        // ROS2 2707, BDF2 3979. Bound = measured x2, per §3a.
        EXPECT_LT(rep.substeps, 8000) << solver << " substep economy";
        swmm_engine_destroy(e);
    }

    // Explicit EUL at the same single step: |1 − h k_fast| >> 1 ⇒ the fast
    // mode explodes. The integrator must FAIL LOUDLY (non-finite detection),
    // not return a silently wrong state.
    {
        SWMM_Engine e = open_with_rxn(
            std::string("[REACTION_OPTIONS]\nSOLVER EUL") + body_common);
        const auto& rx = as_cpp_engine(e).context().reactions;
        double species[2] = {f0, s0};
        const auto rep = run_step(rx, dt, species);
        const bool diverged =
            !rep.ok || std::fabs(species[0]) > 1e3;
        EXPECT_TRUE(diverged)
            << "EUL at h*k=1e4 cannot be accurate; if this passes the "
               "ladder cannot discriminate and the gate is broken";
        swmm_engine_destroy(e);
    }
}

// ---------------------------------------------------------------------------
// Gate 4 — EQUIL Newton + FORMULA order + terms in kinetics.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, EquilibriumAndFormulaAlgebra) {
    SWMM_Engine e = open_with_rxn(
        "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nATOL 1e-10\nRTOL 1e-8\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\nBULK TOT MG\n"
        "[REACTION_TERMS]\nHALF 0.5 * A\n"
        "[REACTION_TANKS]\n"
        "RATE    A   -0.1 * A + HALF * 0.0\n"   // decay, term referenced
        "EQUIL   B   B - 2.0 * A\n"             // B = 2A after solve
        "FORMULA TOT A + B\n");
    const auto& rx = as_cpp_engine(e).context().reactions;
    double species[3] = {3.0, 0.0, 0.0};
    const double dt = 5.0;
    const auto rep = run_step(rx, dt, species);
    ASSERT_TRUE(rep.ok) << rep.error;
    const double a = 3.0 * std::exp(-0.1 * dt);
    EXPECT_NEAR(species[0], a, 1e-6 * a);
    EXPECT_NEAR(species[1], 2.0 * species[0], 1e-8);   // EQUIL algebra
    EXPECT_NEAR(species[2], species[0] + species[1], 1e-12);  // FORMULA
    EXPECT_GT(rep.newton_iters, 0);
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 5 — rate-unit scaling: k[1/hr] over dt seconds == k/3600 in SEC.
// Falsifier: remove unitFactor — the HR run decays 3600× too fast.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, RateUnitsScaleExactly) {
    const double dt = 600.0;  // s
    double a_sec = 0.0, a_hr = 0.0;
    {
        SWMM_Engine e = open_with_rxn(
            "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nATOL 1e-12\nRTOL 1e-10\n"
            "[REACTION_SPECIES]\nBULK A MG\n"
            "[REACTION_TANKS]\nRATE A -0.0001 * A\n");   // 1e-4 1/s
        double sp[1] = {2.0};
        ASSERT_TRUE(run_step(as_cpp_engine(e).context().reactions, dt, sp).ok);
        a_sec = sp[0];
        swmm_engine_destroy(e);
    }
    {
        SWMM_Engine e = open_with_rxn(
            "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS HR\nATOL 1e-12\nRTOL 1e-10\n"
            "[REACTION_SPECIES]\nBULK A MG\n"
            "[REACTION_TANKS]\nRATE A -0.36 * A\n");     // 0.36 1/hr = 1e-4 1/s
        double sp[1] = {2.0};
        ASSERT_TRUE(run_step(as_cpp_engine(e).context().reactions, dt, sp).ok);
        a_hr = sp[0];
        swmm_engine_destroy(e);
    }
    EXPECT_NEAR(a_sec, a_hr, 1e-9 * a_sec);
    EXPECT_NEAR(a_sec, 2.0 * std::exp(-0.0001 * dt), 1e-8);
}

// ---------------------------------------------------------------------------
// Gate 6 — COUPLING NONE freezes other species (MSX semantics).
// A feeds B, but under NONE, B integrates with A frozen at its start value.
// ---------------------------------------------------------------------------
TEST(RxIntegrators, CouplingNoneFreezesOtherSpecies) {
    const double dt = 4.0, a0 = 2.0;
    SWMM_Engine e = open_with_rxn(
        "[REACTION_OPTIONS]\nSOLVER RK5\nRATE_UNITS SEC\nCOUPLING NONE\n"
        "ATOL 1e-12\nRTOL 1e-10\n"
        "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n"
        "[REACTION_TANKS]\nRATE A -0.5 * A\nRATE B 0.5 * A\n");
    const auto& rx = as_cpp_engine(e).context().reactions;
    double species[2] = {a0, 0.0};
    const auto rep = run_step(rx, dt, species);
    ASSERT_TRUE(rep.ok) << rep.error;
    // A decays exactly; B integrates 0.5*A with A FROZEN at a0:
    // b = 0.5*a0*dt — distinguishable from the coupled answer
    // a0(1 − e^{−0.5 dt}) = 1.729…, frozen answer = 4.0.
    EXPECT_NEAR(species[0], a0 * std::exp(-0.5 * dt), 1e-6);
    EXPECT_NEAR(species[1], 0.5 * a0 * dt, 1e-6)
        << "COUPLING NONE must freeze A for B's integration";
    swmm_engine_destroy(e);
}

}  // namespace
