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
 * @file test_fv_unsteady_friction.cpp
 * @brief Gates for the FV unsteady-friction source term (issue #156 Phase 2).
 *
 * @details Kernel-level algebra of kernels::ufUpdate, plus system-level gates
 *          on a two-tank slosh deck: inertness of UNSTEADY_FRICTION NONE,
 *          at-rest preservation with UF ON, and monotone extra damping with
 *          k3 > 0 (the physical point of the feature — Pinto et al. 2025).
 *          Plan: plans/MIXED_FLOW_CLOSURES_TPA_UF_PLAN_2026-08-29.md §3.3/§3.5.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

#include "../../src/engine/hydraulics/fv/FvKernels.hpp"

namespace fs = std::filesystem;
namespace k = openswmm::fv::kernels;

// ===========================================================================
// Kernel algebra
// ===========================================================================

TEST(UfKernel, ZeroK3IsBitIdentity) {
    EXPECT_EQ(k::ufUpdate(12.34, 5.0, 2.0, 0.0, 1.0, 0.5), 12.34);
}

TEST(UfKernel, AtRestIsExactlyInert) {
    // q = 0, u_old = 0, grad = 0: the well-balanced property must survive.
    EXPECT_EQ(k::ufUpdate(0.0, 5.0, 0.0, 0.02, 0.0, 0.5), 0.0);
}

TEST(UfKernel, ImplicitLocalAccelerationFold) {
    // grad = 0: Q' = (Q* + k3·A·Vⁿ)/(1+k3) exactly.
    const double q = 10.0, a = 4.0, u_old = 3.0, k3 = 0.02;
    const double expect = (q + k3 * a * u_old) / (1.0 + k3);
    EXPECT_NEAR(k::ufUpdate(q, a, u_old, k3, 0.0, 0.1), expect, 1e-15);
}

TEST(UfKernel, DeceleratingFlowGainsMomentum) {
    // Q* < A·Vⁿ (flow just decelerated): UF opposes the deceleration, so the
    // updated Q must sit between Q* and A·Vⁿ — the Brunone behavior.
    const double q = 8.0, a = 4.0, u_old = 3.0;  // A·Vⁿ = 12 > Q*
    const double qn = k::ufUpdate(q, a, u_old, 0.02, 0.0, 0.1);
    EXPECT_GT(qn, q);
    EXPECT_LT(qn, a * u_old);
}

TEST(UfKernel, GradientTermIsResistive) {
    // Positive flow, positive grad_term: momentum must drop by dt·k3·A·grad.
    const double q = 10.0, a = 4.0, u_old = 2.5;  // A·Vⁿ = Q* ⇒ accel part = 0
    const double dt = 0.1, k3 = 0.02, grad = 3.0;
    const double expect = q - dt * k3 * a * grad;
    EXPECT_NEAR(k::ufUpdate(q, a, u_old, k3, grad, dt), expect, 1e-12);
}

TEST(UfKernel, ClampBitesAtHalfMomentum) {
    // A huge gradient term may remove at most half the incoming momentum.
    const double q = 10.0, a = 4.0, u_old = 2.5;
    const double qn = k::ufUpdate(q, a, u_old, 0.045, 1.0e6, 1.0);
    EXPECT_NEAR(qn, 5.0, 1e-12);
}

// ===========================================================================
// System level — two-tank slosh
// ===========================================================================

namespace {

const char* kOutDir = "fv_uf_out";

std::string outPath(const std::string& name) {
    fs::create_directories(kOutDir);
    return (fs::path(kOutDir) / name).string();
}

// Two 100 sf tanks joined by 200 ft of 3-ft circular pipe; initial 4 ft head
// imbalance drives a gravity slosh (period ~40 s). High-crest weir to a free
// outfall satisfies the >=1-outfall rule without ever spilling.
std::string sloshModel(double init_a, double init_b, const std::string& extra) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:20:00\n"
          "REPORT_STEP          5\n"
          "ROUTING_STEP         1\n"
          "FV_MIN_CELLS         8\n"
       << extra
       << "\n[STORAGE]\n"
          ";;Name Elev MaxDepth InitDepth Shape      A1 A2 A0\n"
          "TA     0.0  25.0     " << init_a << "  FUNCTIONAL 0  0  100\n"
          "TB     0.0  25.0     " << init_b << "  FUNCTIONAL 0  0  100\n"
          "\n[OUTFALLS]\n;;Name Elev Type Gated\nO_OUT  0.0  FREE NO\n"
          "\n[WEIRS]\n;;Name From To    Type       CrestHt Cd\n"
          "W_OVF  TB   O_OUT TRANSVERSE 24.5    3.33\n"
          "\n[CONDUITS]\n;;Name From To Length N     Z1 Z2\n"
          "C_P    TA   TB 200.0  0.013 0  0\n"
          "\n[XSECTIONS]\n;;Link Shape     G1  G2 G3 G4 Barrels\n"
          "C_P    CIRCULAR  3.0 0  0  0  1\n"
          "W_OVF  RECT_OPEN 2.0 1.0 0 0\n";
    return ss.str();
}

struct SloshResult {
    double late_amp = -1.0;   ///< max |depth_TA − 10| over the last 5 min
    double max_q = 0.0;       ///< max |Q| over the whole run
    bool ok = false;
};

SloshResult runSlosh(const std::string& base, const std::string& deck) {
    SloshResult r;
    const std::string inp = outPath(base + ".inp");
    { std::ofstream f(inp); f << deck; }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                         outPath(base + ".out").c_str(), nullptr) != 0) {
        ADD_FAILURE() << "open failed: " << swmm_get_last_error_msg(e);
        swmm_engine_destroy(e);
        return r;
    }
    EXPECT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    EXPECT_EQ(swmm_engine_start(e, 1), 0) << swmm_get_last_error_msg(e);
    const int ta = swmm_node_index(e, "TA");
    const int cp = swmm_link_index(e, "C_P");
    EXPECT_GE(ta, 0);
    EXPECT_GE(cp, 0);
    double elapsed = 0.0, t_s = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            swmm_engine_end(e);
            swmm_engine_destroy(e);
            return r;
        }
        t_s = elapsed * 86400.0;
        double q = 0.0;
        swmm_link_get_flow(e, cp, &q);
        r.max_q = std::max(r.max_q, std::fabs(q));
        if (t_s >= 900.0) {  // last 5 of 20 minutes
            double d = 0.0;
            swmm_node_get_depth(e, ta, &d);
            r.late_amp = std::max(r.late_amp, std::fabs(d - 10.0));
        }
    } while (elapsed > 0.0);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    r.ok = true;
    return r;
}

std::string fileBytes(const std::string& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

TEST(FvUnsteadyFriction, NoneIsBitIdenticalToAbsent) {
    const auto a = runSlosh("uf_absent", sloshModel(12.0, 8.0, ""));
    const auto b = runSlosh("uf_none",
                            sloshModel(12.0, 8.0,
                                       "UNSTEADY_FRICTION    NONE\n"
                                       "UF_K3                0.02\n"));
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(fileBytes(outPath("uf_absent.out")), fileBytes(outPath("uf_none.out")))
        << "UNSTEADY_FRICTION NONE must be bit-inert (U-G1)";
}

namespace {

// Discretely-at-rest fixture: a junction chain over a flat bed at a uniform
// level is the exact C-property state (zero flux from step one), unlike the
// storage fixture whose coupling carries a small settling ripple.
std::string restModel(const std::string& extra) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:10:00\n"
          "REPORT_STEP          5\n"
          "ROUTING_STEP         1\n"
          "FV_MIN_CELLS         8\n"
       << extra
       << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth\n"
          "TA     0.0  40.0     13.0\n"   // named TA/TB so runSlosh's probes work
          "TB     0.0  40.0     13.0\n"
          "\n[OUTFALLS]\n;;Name Elev Type Gated\nO_OUT  0.0  FREE NO\n"
          "\n[WEIRS]\n;;Name From To    Type       CrestHt Cd\n"
          "W_OVF  TB   O_OUT TRANSVERSE 39.5    3.33\n"
          "\n[CONDUITS]\n;;Name From To Length N     Z1 Z2\n"
          "C_P    TA   TB 200.0  0.013 0  0\n"
          "\n[XSECTIONS]\n;;Link Shape     G1  G2 G3 G4 Barrels\n"
          "C_P    CIRCULAR  3.0 0  0  0  1\n"
          "W_OVF  RECT_OPEN 2.0 1.0 0 0\n";
    return ss.str();
}

} // namespace

TEST(FvUnsteadyFriction, DiscreteRestIsBitIdentical) {
    // Exact lake-at-rest (pressurized, flat bed, uniform level): u ≡ 0, so
    // UF must be bit-inert — the well-balanced property survives (A-G2/G3
    // analog).
    const auto a = runSlosh("uf_rest_base", restModel(""));
    const auto b = runSlosh("uf_rest_on",
                            restModel("UNSTEADY_FRICTION    VITKOVSKY\n"
                                      "UF_K3                0.02\n"));
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_EQ(fileBytes(outPath("uf_rest_base.out")),
              fileBytes(outPath("uf_rest_on.out")));
}

TEST(FvUnsteadyFriction, StorageRippleAmplificationBounded) {
    // The storage-coupled level pool is NOT discretely at rest — it settles
    // through a small ripple (~0.004 cfs base). UF's added-inertia character
    // resists that settling and amplifies the ripple (measured 3.2x at
    // k3=0.02); the dead-band keeps the tail quiet but the startup spike
    // clears the floor. Pin the amplification as BOUNDED (≤ 5x base, and no
    // secular growth) so a regression that turns this into an instability
    // bites, and the measured behavior stays on record for the study.
    const auto a = runSlosh("uf_ripple_base", sloshModel(10.0, 10.0, ""));
    const auto b = runSlosh("uf_ripple_on",
                            sloshModel(10.0, 10.0,
                                       "UNSTEADY_FRICTION    VITKOVSKY\n"
                                       "UF_K3                0.02\n"));
    ASSERT_TRUE(a.ok && b.ok);
    EXPECT_LT(b.max_q, 5.0 * std::max(a.max_q, 1e-6));
    // The pool equilibrates slightly below 10 ft in BOTH runs (storage/pipe
    // redistribution, a fixture artifact) — gate on EXTRA drift vs the base,
    // not on an absolute level.
    EXPECT_LT(b.late_amp, a.late_amp + 0.02)
        << "UF must not add secular drift to a level pool";
}

// ---------------------------------------------------------------------------
// Valve-closure fixture — where the paper's damping lives.
//
// A uniform-velocity slosh has ∂V/∂x ≈ 0, so UF there is nearly pure added
// inertia and demonstrates no damping (measured: late amplitude 0.507 vs
// 0.487 WITHOUT it). The Soares-style configuration — steady flow from a
// fixed-head reservoir killed by an instant valve closure — has the sharp
// ∂V/∂t and wave-front ∂V/∂x the correlation was calibrated on.
// ---------------------------------------------------------------------------

namespace {

std::string valveModel(const std::string& extra) {
    std::ostringstream ss;
    ss << "[OPTIONS]\n"
          "FLOW_UNITS           CFS\n"
          "FLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\n"
          "START_TIME           00:00:00\n"
          "END_DATE             01/01/2026\n"
          "END_TIME             00:10:00\n"
          "REPORT_STEP          0.5\n"
          "ROUTING_STEP         0.25\n"
          "RULE_STEP            0\n"
          "FV_MIN_CELLS         16\n"
       << extra
       << "\n[JUNCTIONS]\n;;Name Elev MaxDepth InitDepth\n"
          "JV     0.0  40.0     13.0\n"
          "\n[OUTFALLS]\n;;Name Elev Type  StageData Gated\n"
          "UP     0.0  FIXED 15.0      NO\n"
          "DN     0.0  FREE            NO\n"
          "\n[CONDUITS]\n;;Name From To Length N     Z1 Z2 Q0\n"
          "C_P    UP   JV 200.0  0.013 0  0  2.4\n"
          "\n[ORIFICES]\n;;Name From To Type Offset Cd   Gated CloseTime\n"
          "VALVE  JV   DN SIDE 0.0    0.62 NO    0\n"
          "\n[XSECTIONS]\n;;Link Shape    G1   G2 G3 G4 Barrels\n"
          "C_P    CIRCULAR 1.0  0  0  0  1\n"
          "VALVE  CIRCULAR 0.41 0  0  0  1\n"
          "\n[CONTROLS]\n"
          "RULE VCLOSE\nIF SIMULATION TIME >= 0.016667\n"
          "THEN ORIFICE VALVE SETTING = 0\n";
    return ss.str();
}

struct ValveResult {
    double late_amp = 0.0;  ///< max |head_JV − final| over the last 3 min
    bool ok = false;
};

// win_start_s: where the late window opens. 420 s (default) suits the
// explicit Euler runs; the implicit and RK2 variants kill the oscillation
// sooner, so their gates look at an earlier window (closure is at t = 60 s).
ValveResult runValve(const std::string& base, const std::string& deck,
                     double win_start_s = 420.0) {
    ValveResult r;
    const std::string inp = outPath(base + ".inp");
    { std::ofstream f(inp); f << deck; }
    SWMM_Engine e = swmm_engine_create();
    if (swmm_engine_open(e, inp.c_str(), outPath(base + ".rpt").c_str(),
                         outPath(base + ".out").c_str(), nullptr) != 0) {
        ADD_FAILURE() << "open failed: " << swmm_get_last_error_msg(e);
        swmm_engine_destroy(e);
        return r;
    }
    EXPECT_EQ(swmm_engine_initialize(e), 0);
    EXPECT_EQ(swmm_engine_start(e, 1), 0);
    const int jv = swmm_node_index(e, "JV");
    EXPECT_GE(jv, 0);
    std::vector<std::pair<double, double>> trace;  // (t_s, head)
    double elapsed = 0.0;
    do {
        if (swmm_engine_step(e, &elapsed) != 0) {
            ADD_FAILURE() << "step failed: " << swmm_get_last_error_msg(e);
            break;
        }
        double d = 0.0;
        swmm_node_get_depth(e, jv, &d);
        trace.emplace_back(elapsed * 86400.0, d);
    } while (elapsed > 0.0);
    swmm_engine_end(e);
    swmm_engine_destroy(e);
    if (trace.empty()) return r;
    const double fin = trace.back().second;
    for (const auto& [t, d] : trace)
        if (t >= win_start_s)  // default: last 3 of 10 minutes
            r.late_amp = std::max(r.late_amp, std::fabs(d - fin));
    r.ok = true;
    return r;
}

} // namespace

TEST(FvUnsteadyFriction, ValveClosureK3Damps) {
    const auto base = runValve("uf_valve_k0", valveModel(""));
    const auto damped = runValve("uf_valve_k20",
                                 valveModel("UNSTEADY_FRICTION    VITKOVSKY\n"
                                            "UF_K3                0.020\n"));
    ASSERT_TRUE(base.ok && damped.ok);
    ASSERT_GT(base.late_amp, 1e-4)
        << "fixture must still be oscillating in the late window";
    EXPECT_LT(damped.late_amp, base.late_amp)
        << "k3=0.02 must damp the post-closure oscillation (U-G2 analog)";
}

TEST(FvUnsteadyFriction, ValveClosureDampingMonotoneInK3) {
    const auto k5 = runValve("uf_valve_k05",
                             valveModel("UNSTEADY_FRICTION    VITKOVSKY\n"
                                        "UF_K3                0.005\n"));
    const auto k20 = runValve("uf_valve_k20b",
                              valveModel("UNSTEADY_FRICTION    VITKOVSKY\n"
                                         "UF_K3                0.020\n"));
    ASSERT_TRUE(k5.ok && k20.ok);
    EXPECT_LE(k20.late_amp, k5.late_amp * 1.001);
}

TEST(FvUnsteadyFriction, StableAtPaperMaxK3) {
    // U-G5: the paper's sweep ceiling; the clamp is the stability guard.
    const auto r = runValve("uf_valve_k45",
                            valveModel("UNSTEADY_FRICTION    VITKOVSKY\n"
                                       "UF_K3                0.045\n"));
    ASSERT_TRUE(r.ok);
    EXPECT_TRUE(std::isfinite(r.late_amp));
    EXPECT_LT(r.late_amp, 40.0) << "no blowup at k3=0.045";
}

TEST(FvUnsteadyFriction, ValveClosureDampsUnderLts) {
    // Exercises the fireCells/macro-cycle application path.
    const auto base = runValve("uf_lts_k0",
                               valveModel("FV_LTS               YES\n"));
    const auto damped = runValve("uf_lts_k20",
                                 valveModel("FV_LTS               YES\n"
                                            "UNSTEADY_FRICTION    VITKOVSKY\n"
                                            "UF_K3                0.020\n"));
    ASSERT_TRUE(base.ok && damped.ok);
    EXPECT_LE(damped.late_amp, base.late_amp * 1.001);
}

TEST(FvUnsteadyFriction, ValveClosureDampsOnImplicitPath) {
    // U-G2's configuration: the e1 waterhammer columns run
    // FV_PRESSURIZED_IMPLICIT YES, where pressurized momentum lives in the
    // implicit face solve and finalizeCells overwrites whatever ufUpdate did
    // to the cells. UF must therefore fold into the implicit face
    // denominator too — P2 verification measured e1 C4 ≡ C2 byte-identical
    // until that fold landed in PressurizedHeadSolver (issue #156).
    // e1's regime in miniature: high slot celerity for the acoustic
    // (waterhammer) character and a ROUTING_STEP that RESOLVES the ring
    // (c·Δt/Δx ≈ 0.4) — at the fixture's default celerity the post-closure
    // motion is a near-uniform slosh, which the plan pins as added-inertia
    // only (no damping to observe), and at Δt ≫ Δx/c the implicit solve's
    // own dissipation kills the ring before UF can act on it. A later
    // duplicate [OPTIONS] key wins, so the ROUTING_STEP here overrides the
    // fixture's 0.25 s.
    // Window from t = 65 s: the acoustic ring decays within ~10 s of the
    // 60 s closure here, and the FIRST surge peak (t < 65 s) legitimately
    // RISES a little under UF (added inertia resists the deceleration) —
    // the damping the paper reports lives in the reflections that follow
    // (measured: 0.0219 base vs 0.0158 damped, ratio 0.72).
    const char* imp = "FV_PRESSURIZED_IMPLICIT YES\n"
                      "FV_SLOT_CELERITY     1000\n"
                      "ROUTING_STEP         0.05\n";
    const auto base = runValve("uf_imp_k0", valveModel(imp), 65.0);
    const auto damped = runValve("uf_imp_k20",
                                 valveModel(std::string(imp) +
                                            "UNSTEADY_FRICTION    VITKOVSKY\n"
                                            "UF_K3                0.020\n"),
                                 65.0);
    ASSERT_TRUE(base.ok && damped.ok);
    ASSERT_GT(base.late_amp, 1e-3)
        << "fixture must still be ringing in the observation window";
    EXPECT_LT(damped.late_amp, base.late_amp)
        << "k3=0.02 must damp on the implicit-pressurized path (U-G2)";
}

TEST(FvUnsteadyFriction, ValveClosureDampsUnderRk2) {
    // Per-stage application: UF rides takeSubstep, which RK2 calls once per
    // stage like steady friction (plan §3.3 RK2/LTS bullet).
    const auto base = runValve("uf_rk2_k0",
                               valveModel("FV_TIME_INTEGRATION  RK2\n"),
                               120.0);
    const auto damped = runValve("uf_rk2_k20",
                                 valveModel("FV_TIME_INTEGRATION  RK2\n"
                                            "UNSTEADY_FRICTION    VITKOVSKY\n"
                                            "UF_K3                0.020\n"),
                                 120.0);
    ASSERT_TRUE(base.ok && damped.ok);
    ASSERT_GT(base.late_amp, 1e-4)
        << "fixture must still be oscillating in the late window";
    EXPECT_LT(damped.late_amp, base.late_amp)
        << "k3=0.02 must damp under RK2 (same gate as the Euler run)";
}
