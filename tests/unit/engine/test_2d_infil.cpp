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
 * @file test_2d_infil.cpp
 * @brief Track-I gates for per-cell 2D infiltration (plan §5.5, D-I1…D-I6).
 *
 * @details Unit-level gates over Infil2D itself — the adapter, its units, the
 *          D-I3 resolution order and D-I4 validation. The end-to-end gates
 *          (mass balance, continuity residual, lazy path, round-trip) live in
 *          test_2d_infil_integration.cpp, which drives a real model through
 *          the C API.
 *
 *          **These gates deliberately do NOT re-derive the adapter.** Every
 *          expected value here is computed from the user-facing row numbers
 *          and physical unit factors (1 in = 0.0254 m, 1 hr = 3600 s), never
 *          by calling `infil::*` with hand-converted arguments — that would
 *          reproduce the code under test and pass no matter what the adapter
 *          did. §3.2 of the verification handoff names this conversion as the
 *          single most likely place for a silent 3.28x / 25.4x error.
 *
 * @see src/engine/2d/infil/Infil2D.hpp
 * @see plans/TWO_ZONE_GROUNDWATER_EXPLICIT_LTS_PLAN_2026-08-15.md §5.5
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <string>
#include <vector>

#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/infil/Infil2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "core/SimulationOptions.hpp"

using namespace openswmm;
using namespace openswmm::twoD;

namespace {

// ---------------------------------------------------------------------------
// Physical constants used to build EXPECTED values. These are definitions of
// the units themselves, not engine code.
// ---------------------------------------------------------------------------
constexpr double kMetrePerInch = 0.0254;      // exact, by definition
constexpr double kSecPerHour   = 3600.0;

/// A user-typed US rate in in/hr expressed in SI m/s.
constexpr double inhrToMs(double in_per_hr) {
    return in_per_hr * kMetrePerInch / kSecPerHour;
}

/// A user-typed SI rate in mm/hr expressed in SI m/s.
constexpr double mmhrToMs(double mm_per_hr) {
    return mm_per_hr * 0.001 / kSecPerHour;
}

// ---------------------------------------------------------------------------
// A four-triangle strip. Tags let the D-I3 resolution order be exercised:
//   tri 0 -> "LAWN"    tri 1 -> "WOODS"    tri 2 -> "LAWN"    tri 3 -> ""
// ---------------------------------------------------------------------------
MeshData makeTaggedMesh() {
    MeshData mesh;
    mesh.resize_vertices(6);
    mesh.vx = {0.0, 1.0, 2.0, 0.0, 1.0, 2.0};
    mesh.vy = {0.0, 0.0, 0.0, 1.0, 1.0, 1.0};
    mesh.vz = {0.0, 0.0, 0.0, 0.0, 0.0, 0.0};

    mesh.resize_triangles(4);
    const int v0[4] = {0, 0, 1, 1};
    const int v1[4] = {1, 4, 2, 5};
    const int v2[4] = {4, 3, 5, 4};
    for (int t = 0; t < 4; ++t) {
        mesh.tri_v0[t] = v0[t];
        mesh.tri_v1[t] = v1[t];
        mesh.tri_v2[t] = v2[t];
        mesh.mannings_n[t] = 0.03;
    }
    mesh.tri_tag = {"LAWN", "WOODS", "LAWN", ""};

    buildMeshTopology(mesh);
    return mesh;
}

SimulationOptions usOptions() {
    SimulationOptions o;
    o.flow_units = FlowUnits::CFS;   // US: rates are in/hr, depths in
    o.wet_step   = 300.0;
    return o;
}

SimulationOptions siOptions() {
    SimulationOptions o;
    o.flow_units = FlowUnits::CMS;   // SI: rates are mm/hr, depths mm
    o.wet_step   = 300.0;
    return o;
}

SurfaceStateData makeState(const MeshData& mesh, double depth_m, double rain_ms) {
    SurfaceStateData s;
    s.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        s.depth[i]    = depth_m;
        s.rainfall[i] = rain_ms;
    }
    return s;
}

Infil2DRow rowOf(InfilModel m, double p0, double p1 = 0.0, double p2 = 0.0,
                 double p3 = 0.0, double p4 = 0.0) {
    Infil2DRow r;
    r.has_method = true;
    r.method     = m;
    r.p[0] = p0; r.p[1] = p1; r.p[2] = p2; r.p[3] = p3; r.p[4] = p4;
    r.dest = Infil2DDest::LOST;
    return r;
}

/// Build an Infil2D whose every cell carries @p row, resolved against @p mesh.
Infil2D resolvedWith(const MeshData& mesh, const SimulationOptions& opts,
                     const Infil2DRow& row) {
    Infil2D infil;
    Infil2DDefault d;
    d.tag = "*";
    d.row = row;
    infil.defaults().push_back(d);
    std::string err;
    EXPECT_TRUE(infil.resolve(mesh, opts, err)) << err;
    return infil;
}

} // namespace

// ============================================================================
// G2 — adapter units. The handoff's §3.2 risk, attacked with hand numbers.
// ============================================================================

class Infil2DUnitsTest : public ::testing::Test {
protected:
    MeshData mesh_ = makeTaggedMesh();
};

// CONSTANT is the one method with a closed-form answer that owes nothing to
// the kernel's internal state: with water available, the published rate IS the
// user's rate. 1.0 in/hr must come out as 7.0555...e-6 m/s. A missing ft->m
// conversion reads 2.3148e-5 (3.28x high); a missing in->ft reads 8.47e-5.
TEST_F(Infil2DUnitsTest, ConstantRateUSProjectPublishesTheHandComputedSIRate) {
    Infil2D infil = resolvedWith(mesh_, usOptions(),
                                 rowOf(InfilModel::CONSTANT, 1.0));
    ASSERT_TRUE(infil.active());

    // 1 m of standing water: available-water rate is enormous, so the kernel
    // is capacity-limited, not supply-limited.
    SurfaceStateData st = makeState(mesh_, 1.0, 0.0);
    infil.updateRates(mesh_, st, 60.0);

    const double expected = inhrToMs(1.0);   // 7.0555555...e-6 m/s
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        EXPECT_NEAR(st.infil_rate[i], expected, expected * 1e-12)
            << "cell " << i << ": published " << st.infil_rate[i]
            << " m/s for a 1.0 in/hr CONSTANT row; expected " << expected
            << " (ratio " << st.infil_rate[i] / expected << ")";
    }
}

// The init-units question of §5.5.1 item 2, settled by measurement: the SAME
// physical rate typed in the SI project's units must produce the SAME SI
// output. Nothing about the adapter can be right if this fails.
TEST_F(Infil2DUnitsTest, ConstantRateSIProjectPublishesTheHandComputedSIRate) {
    Infil2D infil = resolvedWith(mesh_, siOptions(),
                                 rowOf(InfilModel::CONSTANT, 25.4));
    ASSERT_TRUE(infil.active());

    SurfaceStateData st = makeState(mesh_, 1.0, 0.0);
    infil.updateRates(mesh_, st, 60.0);

    const double expected = mmhrToMs(25.4);  // == inhrToMs(1.0)
    for (int i = 0; i < mesh_.n_triangles(); ++i) {
        EXPECT_NEAR(st.infil_rate[i], expected, expected * 1e-12)
            << "cell " << i << ": published " << st.infil_rate[i]
            << " m/s for a 25.4 mm/hr CONSTANT row; expected " << expected;
    }
}

// Cross-unit-system equality, run over EVERY method with a rate-like or
// depth-like parameter. This is the strongest single statement that
// Infil2D reads its rows in project units: 1 in/hr == 25.4 mm/hr,
// 1 in == 25.4 mm, and the published SI series must be indistinguishable.
TEST_F(Infil2DUnitsTest, EveryMethodAgreesAcrossUnitSystems) {
    struct Case {
        const char* name;
        InfilModel  method;
        Infil2DRow  us;
        Infil2DRow  si;
    };

    // US row -> SI row: rates in/hr *25.4 -> mm/hr; depths in *25.4 -> mm;
    // dimensionless columns (decay 1/hr, dry_time days, IMD, CN) unchanged.
    const Case cases[] = {
        {"HORTON",              InfilModel::HORTON,
         rowOf(InfilModel::HORTON,         3.0,       0.5,       4.14, 7.0, 0.0),
         rowOf(InfilModel::HORTON,         3.0*25.4,  0.5*25.4,  4.14, 7.0, 0.0)},
        {"MOD_HORTON",          InfilModel::MOD_HORTON,
         rowOf(InfilModel::MOD_HORTON,     3.0,       0.5,       4.14, 7.0, 0.0),
         rowOf(InfilModel::MOD_HORTON,     3.0*25.4,  0.5*25.4,  4.14, 7.0, 0.0)},
        {"GREEN_AMPT",          InfilModel::GREEN_AMPT,
         rowOf(InfilModel::GREEN_AMPT,     3.5,       0.5,       0.30),
         rowOf(InfilModel::GREEN_AMPT,     3.5*25.4,  0.5*25.4,  0.30)},
        {"MOD_GREEN_AMPT",      InfilModel::MOD_GREEN_AMPT,
         rowOf(InfilModel::MOD_GREEN_AMPT, 3.5,       0.5,       0.30),
         rowOf(InfilModel::MOD_GREEN_AMPT, 3.5*25.4,  0.5*25.4,  0.30)},
        {"CURVE_NUMBER",        InfilModel::CURVE_NUM,
         rowOf(InfilModel::CURVE_NUM,      85.0,      0.0,       7.0),
         rowOf(InfilModel::CURVE_NUM,      85.0,      0.0,       7.0)},
        {"CONSTANT",            InfilModel::CONSTANT,
         rowOf(InfilModel::CONSTANT,       1.2),
         rowOf(InfilModel::CONSTANT,       1.2*25.4)},
    };

    constexpr double kDt      = 300.0;
    constexpr int    kSteps   = 24;      // 2 hours — enough for decay/recovery
    constexpr double kRainMs  = inhrToMs(2.0);
    constexpr double kDepthM  = 0.05;

    for (const Case& c : cases) {
        Infil2D a = resolvedWith(mesh_, usOptions(), c.us);
        Infil2D b = resolvedWith(mesh_, siOptions(), c.si);
        ASSERT_TRUE(a.active()) << c.name;
        ASSERT_TRUE(b.active()) << c.name;

        SurfaceStateData sa = makeState(mesh_, kDepthM, kRainMs);
        SurfaceStateData sb = makeState(mesh_, kDepthM, kRainMs);

        double sum_a = 0.0, sum_b = 0.0;
        for (int n = 0; n < kSteps; ++n) {
            a.updateRates(mesh_, sa, kDt);
            b.updateRates(mesh_, sb, kDt);
            sum_a += sa.infil_rate[0] * kDt;
            sum_b += sb.infil_rate[0] * kDt;
            (void)sum_b;

            // Both must be tracking the same physical process step by step,
            // not merely landing on the same total.
            const double scale = std::max({std::abs(sa.infil_rate[0]),
                                           std::abs(sb.infil_rate[0]), 1e-12});
            EXPECT_NEAR(sa.infil_rate[0], sb.infil_rate[0], scale * 1e-9)
                << c.name << " step " << n << ": US project published "
                << sa.infil_rate[0] << " m/s, SI project " << sb.infil_rate[0]
                << " m/s for the same physical row";
        }

        // The process must actually have run: a gate that compares two zeros
        // is no gate at all.
        EXPECT_GT(sum_a, 0.0) << c.name << " infiltrated nothing in "
                              << kSteps << " steps — gate is vacuous";
    }
}

// The Horton rate the kernel publishes is the step-AVERAGE of the analytic
// capacity curve, F(t) = fmin*t + (df/k)(1 - e^-kt). Compare the published
// series against that closed form evaluated directly in SI from the row's
// user-facing numbers. This anchors f0, fmin AND the 1/hr decay column at
// once, and it is independent of the engine's own conversion path.
TEST_F(Infil2DUnitsTest, HortonSeriesMatchesTheAnalyticCapacityCurveInSI) {
    constexpr double kF0Inhr    = 3.0;
    constexpr double kFminInhr  = 0.5;
    constexpr double kDecayPerHr = 4.14;

    Infil2D infil = resolvedWith(
        mesh_, usOptions(),
        rowOf(InfilModel::HORTON, kF0Inhr, kFminInhr, kDecayPerHr, 7.0, 0.0));
    ASSERT_TRUE(infil.active());

    // Deep ponding so the capacity curve, not the water supply, is what the
    // kernel returns.
    SurfaceStateData st = makeState(mesh_, 1.0, 0.0);

    const double f0   = inhrToMs(kF0Inhr);
    const double fmin = inhrToMs(kFminInhr);
    const double k    = kDecayPerHr / kSecPerHour;      // 1/hr -> 1/s
    const double df   = f0 - fmin;
    auto F = [&](double t) { return fmin * t + df / k * (1.0 - std::exp(-k * t)); };

    constexpr double kDt = 120.0;
    for (int n = 1; n <= 30; ++n) {
        infil.updateRates(mesh_, st, kDt);
        const double expected = (F(n * kDt) - F((n - 1) * kDt)) / kDt;
        EXPECT_NEAR(st.infil_rate[0], expected, expected * 1e-9)
            << "Horton step " << n << " (t=" << n * kDt << " s): published "
            << st.infil_rate[0] << " m/s, analytic step-average " << expected
            << " m/s (ratio " << st.infil_rate[0] / expected << ")";
    }

    // Sanity on the shape: it must actually have decayed toward fmin.
    EXPECT_LT(st.infil_rate[0], 0.55 * f0)
        << "Horton never decayed — decay column may be misread";
    EXPECT_GT(st.infil_rate[0], fmin * 0.999)
        << "Horton fell below its own fmin floor";
}

// §3.2's claim, tested from the other side: if CONSTANT really is a degenerate
// Horton then a HORTON row with f0 == fmin and no decay must publish exactly
// the same series as a CONSTANT row of that rate. Same numbers, two code
// paths (horton_getInfil vs constant_getInfil).
TEST_F(Infil2DUnitsTest, ConstantEqualsDegenerateHorton) {
    constexpr double kRateInhr = 0.85;

    Infil2D k_const = resolvedWith(mesh_, usOptions(),
                                   rowOf(InfilModel::CONSTANT, kRateInhr));
    Infil2D k_horton = resolvedWith(
        mesh_, usOptions(),
        rowOf(InfilModel::HORTON, kRateInhr, kRateInhr, 0.0, 0.0, 0.0));

    SurfaceStateData sc = makeState(mesh_, 0.02, inhrToMs(0.4));
    SurfaceStateData sh = makeState(mesh_, 0.02, inhrToMs(0.4));

    for (int n = 0; n < 20; ++n) {
        k_const.updateRates(mesh_, sc, 300.0);
        k_horton.updateRates(mesh_, sh, 300.0);
        EXPECT_DOUBLE_EQ(sc.infil_rate[0], sh.infil_rate[0])
            << "step " << n << ": CONSTANT " << sc.infil_rate[0]
            << " != degenerate HORTON " << sh.infil_rate[0];
    }
    EXPECT_GT(sc.infil_rate[0], 0.0) << "gate is vacuous — nothing infiltrated";
}

// CONSTANT must be capacity-bounded (plan §5.5.1): offered less water than its
// nominal rate, it takes only what is there.
TEST_F(Infil2DUnitsTest, ConstantIsBoundedByAvailableWater) {
    Infil2D infil = resolvedWith(mesh_, usOptions(),
                                 rowOf(InfilModel::CONSTANT, 10.0));  // 10 in/hr
    // 1 mm of water, no rain, 600 s step: only 1e-3/600 m/s is available.
    SurfaceStateData st = makeState(mesh_, 0.001, 0.0);
    infil.updateRates(mesh_, st, 600.0);

    const double available = 0.001 / 600.0;
    EXPECT_NEAR(st.infil_rate[0], available, available * 1e-12)
        << "CONSTANT published " << st.infil_rate[0]
        << " m/s but only " << available << " m/s was available";
    EXPECT_LT(st.infil_rate[0], inhrToMs(10.0))
        << "CONSTANT ignored the available-water bound";
}

// Green-Ampt is anchored on its own governing equation, evaluated in SI from
// the row's user-facing numbers. Over one saturated step the cumulative depth
// must satisfy
//     F2 - F1 - c1*ln((F2+c1)/(F1+c1)) = Ks*dt,   c1 = (S + depth)*IMD
// This is a RESIDUAL check of the law the kernel solves, not a re-solve of it,
// so it cannot pass by reproducing the adapter: any error in the in/hr -> m/s
// scaling of Ks, or the in -> m scaling of S, throws the residual off by that
// same factor. The wrong-units control at the end proves the gate can fail.
TEST_F(Infil2DUnitsTest, GreenAmptObeysItsGoverningEquationInSI) {
    constexpr double kSInch  = 3.5;    // suction head, inches
    constexpr double kKsInhr = 0.5;    // saturated conductivity, in/hr
    constexpr double kIMD    = 0.30;
    constexpr double kDepthM = 0.5;    // held ponded depth
    constexpr double kDt     = 300.0;

    Infil2D infil = resolvedWith(
        mesh_, usOptions(),
        rowOf(InfilModel::GREEN_AMPT, kSInch, kKsInhr, kIMD));
    ASSERT_TRUE(infil.active());

    SurfaceStateData st = makeState(mesh_, kDepthM, 0.0);   // ponded, no rain

    // Hand-built SI parameters. 1 in = 0.0254 m, 1 hr = 3600 s. No engine call.
    const double ks_si = inhrToMs(kKsInhr);
    const double s_si  = kSInch * kMetrePerInch;
    const double c1    = (s_si + kDepthM) * kIMD;

    auto residual = [&](double F1, double F2, double ks) {
        return F2 - F1 - c1 * std::log((F2 + c1) / (F1 + c1)) - ks * kDt;
    };

    double prev_rate = 1e30;
    double worst_rel = 0.0;
    double worst_rel_wrong_units = 0.0;
    int    saturated_steps = 0;

    for (int n = 0; n < 200; ++n) {
        const double F1 = infil.cumulative()[0];
        infil.updateRates(mesh_, st, kDt);
        const double F2 = infil.cumulative()[0];

        // The physical envelope, which holds in every regime.
        EXPECT_GE(st.infil_rate[0], ks_si * (1.0 - 1e-9))
            << "step " << n << ": rate " << st.infil_rate[0]
            << " fell below Ks " << ks_si;
        EXPECT_LE(st.infil_rate[0], prev_rate * (1.0 + 1e-9))
            << "step " << n << ": rate rose under steady ponding";
        prev_rate = st.infil_rate[0];

        // The governing equation applies once the column has saturated. Before
        // that the kernel is supply-limited and returns ia, so skip those.
        if (st.infil_rate[0] >= ks_si * 1.0000001 && F1 > 0.0) {
            ++saturated_steps;
            const double rel = std::abs(residual(F1, F2, ks_si)) / (ks_si * kDt);
            worst_rel = std::max(worst_rel, rel);
            // Same residual with Ks mis-scaled by the ft/m factor — the exact
            // silent error §3.2 warns about.
            const double rel_bad =
                std::abs(residual(F1, F2, ks_si * 3.280839895013123))
                / (ks_si * kDt);
            worst_rel_wrong_units = std::max(worst_rel_wrong_units, rel_bad);
        }
    }

    ASSERT_GT(saturated_steps, 100)
        << "only " << saturated_steps << " saturated steps — gate is vacuous";

    EXPECT_LT(worst_rel, 0.02)
        << "Green-Ampt cumulative depth violates F2-F1-c1*ln((F2+c1)/(F1+c1))"
           " = Ks*dt by " << (worst_rel * 100.0) << "% of Ks*dt, evaluated in "
           "SI from S=" << kSInch << " in, Ks=" << kKsInhr << " in/hr, IMD="
        << kIMD << " — a parameter is being read in the wrong unit";

    // Non-vacuity: the same residual with a 3.28x error in Ks must fail loudly.
    EXPECT_GT(worst_rel_wrong_units, 1.0)
        << "the residual check cannot distinguish a 3.28x Ks error — it is "
           "not discriminating and must not be trusted";
}

// Curve Number's maximum retention is Smax = (1000/CN - 10)/12 ft, a hardcoded
// inch formula in both unit systems. The cumulative infiltrated depth may not
// exceed it.
TEST_F(Infil2DUnitsTest, CurveNumberCumulativeIsBoundedBySmax) {
    constexpr double kCN = 85.0;
    Infil2D infil = resolvedWith(mesh_, usOptions(),
                                 rowOf(InfilModel::CURVE_NUM, kCN, 0.0, 7.0));
    ASSERT_TRUE(infil.active());

    // Smax in feet -> metres. No engine call involved.
    const double smax_m = (1000.0 / kCN - 10.0) / 12.0 * 0.3048;

    SurfaceStateData st = makeState(mesh_, 0.10, inhrToMs(4.0));
    for (int n = 0; n < 120; ++n) infil.updateRates(mesh_, st, 300.0);

    const double cum = infil.cumulative()[0];
    EXPECT_GT(cum, 0.0) << "CN infiltrated nothing — gate is vacuous";
    EXPECT_LE(cum, smax_m * 1.001)
        << "CN cumulative " << cum << " m exceeded Smax " << smax_m
        << " m — the (1000/CN - 10)/12 inch formula is being read in the "
           "wrong length unit";
}

// The modified variants must be distinguishable from their base variants, in
// the regime the modification targets. If MOD_HORTON silently ran plain Horton
// (or grnampt_getInfil were handed the wrong enum, the §5.5.1 bool trap) the
// two series would coincide.
TEST_F(Infil2DUnitsTest, ModifiedVariantsDifferFromTheirBaseVariants) {
    SurfaceStateData sh = makeState(mesh_, 0.5, 0.0);
    SurfaceStateData sm = makeState(mesh_, 0.5, 0.0);
    Infil2D h = resolvedWith(mesh_, usOptions(),
                             rowOf(InfilModel::HORTON,     3.0, 0.5, 4.14, 7.0, 0.0));
    Infil2D m = resolvedWith(mesh_, usOptions(),
                             rowOf(InfilModel::MOD_HORTON, 3.0, 0.5, 4.14, 7.0, 0.0));

    bool horton_diverged = false;
    for (int n = 0; n < 40; ++n) {
        h.updateRates(mesh_, sh, 300.0);
        m.updateRates(mesh_, sm, 300.0);
        if (std::abs(sh.infil_rate[0] - sm.infil_rate[0]) > 1e-12)
            horton_diverged = true;
    }
    EXPECT_TRUE(horton_diverged)
        << "MOD_HORTON produced the identical series to HORTON over 40 steps "
           "— the modified kernel is not being dispatched";

    // Green-Ampt: the modified variant does not reset F between events. The
    // reset is gated on the inter-event timer T = 5400 / Lu, so the dry window
    // has to OUTLAST it or the two variants are identical by construction and
    // the gate proves nothing. For Ks = 0.5 in/hr, Lu = 4*sqrt(0.5)/12 ft and
    // T is about 22900 s, so 60 dry steps of 600 s (36000 s) clears it.
    SurfaceStateData sg  = makeState(mesh_, 0.0, 0.0);
    SurfaceStateData sgm = makeState(mesh_, 0.0, 0.0);
    Infil2D g  = resolvedWith(mesh_, usOptions(),
                              rowOf(InfilModel::GREEN_AMPT,     3.5, 0.5, 0.30));
    Infil2D gm = resolvedWith(mesh_, usOptions(),
                              rowOf(InfilModel::MOD_GREEN_AMPT, 3.5, 0.5, 0.30));
    bool ga_diverged = false;
    int  wet2_steps  = 0;
    for (int n = 0; n < 140; ++n) {
        const bool wet = (n < 20) || (n >= 80);   // rain, long dry spell, rain
        if (n >= 80) ++wet2_steps;
        for (int i = 0; i < mesh_.n_triangles(); ++i) {
            sg.depth[i]  = wet ? 0.05 : 0.0;
            sgm.depth[i] = wet ? 0.05 : 0.0;
            sg.rainfall[i]  = wet ? inhrToMs(2.0) : 0.0;
            sgm.rainfall[i] = wet ? inhrToMs(2.0) : 0.0;
        }
        g.updateRates(mesh_, sg, 600.0);
        gm.updateRates(mesh_, sgm, 600.0);
        if (std::abs(sg.infil_rate[0] - sgm.infil_rate[0]) > 1e-12)
            ga_diverged = true;
    }
    ASSERT_GT(wet2_steps, 0) << "the second wet window never ran";
    EXPECT_TRUE(ga_diverged)
        << "MOD_GREEN_AMPT produced the identical series to GREEN_AMPT across "
           "a wet / long-dry / wet sequence — grnampt_getInfil is being called "
           "with the wrong model enum (§5.5.1: the variant is selected by an "
           "InfilModel argument, not a bool)";
}

// ============================================================================
// G7 (part 1) — D-I3 resolution order, in memory.
// ============================================================================

class Infil2DResolutionTest : public ::testing::Test {
protected:
    MeshData mesh_ = makeTaggedMesh();   // tags: LAWN, WOODS, LAWN, ""
};

// override > tag > '*' > none, on a mesh that carries all four cases at once.
TEST_F(Infil2DResolutionTest, OverrideBeatsTagBeatsStarBeatsNothing) {
    Infil2D infil;

    Infil2DDefault star;  star.tag  = "*";
    star.row = rowOf(InfilModel::CONSTANT, 1.0);
    Infil2DDefault lawn;  lawn.tag  = "LAWN";
    lawn.row = rowOf(InfilModel::HORTON, 3.0, 0.5, 4.14, 7.0, 0.0);
    infil.defaults() = {star, lawn};

    Infil2DOverride ov;  ov.tri = 2;      // a LAWN cell, overridden
    ov.row = rowOf(InfilModel::CURVE_NUM, 85.0, 0.0, 7.0);
    infil.overrides() = {ov};

    std::string err;
    ASSERT_TRUE(infil.resolve(mesh_, usOptions(), err)) << err;

    const auto& rows = infil.resolvedRows();
    const auto& prov = infil.provenance();
    ASSERT_EQ(rows.size(), 4u);

    EXPECT_EQ(rows[0].method, InfilModel::HORTON)     << "tri 0 (LAWN) should take the tag row";
    EXPECT_EQ(prov[0], Infil2DProvenance::TAG);
    EXPECT_EQ(rows[1].method, InfilModel::CONSTANT)   << "tri 1 (WOODS, untagged row) should fall back to '*'";
    EXPECT_EQ(prov[1], Infil2DProvenance::STAR);
    EXPECT_EQ(rows[2].method, InfilModel::CURVE_NUM)  << "tri 2 override must beat its LAWN tag";
    EXPECT_EQ(prov[2], Infil2DProvenance::OVERRIDE);
    EXPECT_EQ(rows[3].method, InfilModel::CONSTANT)   << "tri 3 (no tag) should take '*'";
    EXPECT_EQ(prov[3], Infil2DProvenance::STAR);
}

// A tag row spelled NONE must CLEAR the '*' default for its cells, and the
// engine's Infil2DProvenance collapses that to NONE (handoff §3.8).
TEST_F(Infil2DResolutionTest, TagNoneClearsTheStarDefault) {
    Infil2D infil;
    Infil2DDefault star;  star.tag = "*";
    star.row = rowOf(InfilModel::CONSTANT, 1.0);
    Infil2DDefault woods; woods.tag = "WOODS";
    woods.row = Infil2DRow{};             // has_method == false, i.e. NONE
    infil.defaults() = {star, woods};

    std::string err;
    ASSERT_TRUE(infil.resolve(mesh_, usOptions(), err)) << err;

    EXPECT_FALSE(infil.resolvedRows()[1].has_method)
        << "a WOODS row spelling NONE did not clear the '*' default";
    EXPECT_EQ(infil.provenance()[1], Infil2DProvenance::NONE);
    EXPECT_TRUE(infil.resolvedRows()[0].has_method) << "NONE leaked onto LAWN";
}

// With no section at all the object must stay on the unallocated fast path —
// this is what makes the bitwise-regression gate (I7) structurally true.
TEST_F(Infil2DResolutionTest, NoRowsMeansNoAllocationAndInactive) {
    Infil2D infil;
    std::string err;
    ASSERT_TRUE(infil.resolve(mesh_, usOptions(), err)) << err;
    EXPECT_FALSE(infil.active());
    EXPECT_TRUE(infil.resolvedRows().empty());
    EXPECT_TRUE(infil.provenance().empty());
    EXPECT_TRUE(infil.cumulative().empty());

    // updateRates on an inactive model must not touch the published array.
    SurfaceStateData st = makeState(mesh_, 0.1, inhrToMs(2.0));
    infil.updateRates(mesh_, st, 300.0);
    for (int i = 0; i < mesh_.n_triangles(); ++i)
        EXPECT_DOUBLE_EQ(st.infil_rate[i], 0.0);
}

// A '*' row spelling NONE is a no-op, not an activation.
TEST_F(Infil2DResolutionTest, StarNoneLeavesTheModelInactive) {
    Infil2D infil;
    Infil2DDefault star;  star.tag = "*";
    star.row = Infil2DRow{};
    infil.defaults().push_back(star);
    std::string err;
    ASSERT_TRUE(infil.resolve(mesh_, usOptions(), err)) << err;
    EXPECT_FALSE(infil.active());
}

// INFIL_STEP defaults to the project WET_STEP (D-I1) and is otherwise honoured.
TEST_F(Infil2DResolutionTest, InfilStepDefaultsToWetStep) {
    SimulationOptions o = usOptions();
    o.wet_step = 900.0;

    Infil2D a = resolvedWith(mesh_, o, rowOf(InfilModel::CONSTANT, 1.0));
    EXPECT_DOUBLE_EQ(a.stepSeconds(), 900.0);

    Infil2D b;
    Infil2DDefault star; star.tag = "*"; star.row = rowOf(InfilModel::CONSTANT, 1.0);
    b.defaults().push_back(star);
    b.options().infil_step = 120.0;
    std::string err;
    ASSERT_TRUE(b.resolve(mesh_, o, err)) << err;
    EXPECT_DOUBLE_EQ(b.stepSeconds(), 120.0);
}

// ============================================================================
// G11 — validation (D-I4 destinations, ranges, indices).
// ============================================================================

class Infil2DValidationTest : public ::testing::Test {
protected:
    MeshData mesh_ = makeTaggedMesh();
};

TEST_F(Infil2DValidationTest, UnsupportedDestinationsAreRejectedByName) {
    for (auto dest : {Infil2DDest::SUBCATCH_AQUIFER, Infil2DDest::AQUIFER_2D}) {
        Infil2D infil;
        Infil2DDefault d; d.tag = "LAWN";
        d.row = rowOf(InfilModel::CONSTANT, 1.0);
        d.row.dest = dest;
        infil.defaults().push_back(d);

        std::string err;
        EXPECT_FALSE(infil.resolve(mesh_, usOptions(), err))
            << "destination " << infil2DDestToken(dest) << " was accepted";
        EXPECT_NE(err.find(infil2DDestToken(dest)), std::string::npos)
            << "message does not name the destination: " << err;
        EXPECT_NE(err.find("LAWN"), std::string::npos)
            << "message does not name the offending tag: " << err;
        EXPECT_NE(err.find("not supported in this release"), std::string::npos)
            << "message is not the §5.5.4 wording: " << err;
    }
}

TEST_F(Infil2DValidationTest, OutOfRangeCellIndexIsRejected) {
    Infil2D infil;
    Infil2DOverride ov; ov.tri = 9;    // mesh has 4 triangles
    ov.row = rowOf(InfilModel::CONSTANT, 1.0);
    infil.overrides().push_back(ov);

    std::string err;
    EXPECT_FALSE(infil.resolve(mesh_, usOptions(), err));
    EXPECT_NE(err.find("out of range"), std::string::npos) << err;
    EXPECT_NE(err.find("10"), std::string::npos)
        << "message should name the 1-based cell the user typed: " << err;
}

TEST_F(Infil2DValidationTest, OutOfRangeParametersAreRejected) {
    struct Bad { const char* what; Infil2DRow row; };
    const Bad bad[] = {
        {"negative Horton f0",  rowOf(InfilModel::HORTON, -1.0, 0.5, 4.14, 7.0, 0.0)},
        {"negative Ks",         rowOf(InfilModel::GREEN_AMPT, 3.5, -0.5, 0.3)},
        {"IMD above 1",         rowOf(InfilModel::GREEN_AMPT, 3.5, 0.5, 1.7)},
        {"CN of 0",             rowOf(InfilModel::CURVE_NUM, 0.0, 0.0, 7.0)},
        {"CN of 120",           rowOf(InfilModel::CURVE_NUM, 120.0, 0.0, 7.0)},
        {"negative constant",   rowOf(InfilModel::CONSTANT, -2.0)},
    };
    for (const Bad& b : bad) {
        Infil2D infil;
        Infil2DDefault d; d.tag = "*"; d.row = b.row;
        infil.defaults().push_back(d);
        std::string err;
        EXPECT_FALSE(infil.resolve(mesh_, usOptions(), err))
            << b.what << " was accepted";
    }
}

// The token grammar must round-trip through both directions.
TEST(Infil2DTokensTest, MethodAndDestTokensRoundTrip) {
    const char* methods[] = {"HORTON", "MODIFIED_HORTON", "GREEN_AMPT",
                             "MODIFIED_GREEN_AMPT", "CURVE_NUMBER", "CONSTANT"};
    for (const char* tok : methods) {
        Infil2DRow r;
        ASSERT_TRUE(parseInfil2DMethod(tok, r.method, r.has_method)) << tok;
        ASSERT_TRUE(r.has_method) << tok;
        EXPECT_STREQ(infil2DMethodToken(r), tok);
        // Case-insensitivity is part of the grammar.
        std::string lower(tok);
        for (char& c : lower) c = static_cast<char>(std::tolower(c));
        Infil2DRow r2;
        EXPECT_TRUE(parseInfil2DMethod(lower, r2.method, r2.has_method)) << lower;
        EXPECT_EQ(r2.method, r.method);
    }

    Infil2DRow none;
    EXPECT_TRUE(parseInfil2DMethod("NONE", none.method, none.has_method));
    EXPECT_FALSE(none.has_method);
    EXPECT_STREQ(infil2DMethodToken(none), "NONE");

    InfilModel m; bool has = true;
    EXPECT_FALSE(parseInfil2DMethod("SPONGE", m, has));

    for (const char* tok : {"LOST", "SUBCATCH_AQUIFER", "AQUIFER_2D"}) {
        Infil2DDest d;
        ASSERT_TRUE(parseInfil2DDest(tok, d)) << tok;
        EXPECT_STREQ(infil2DDestToken(d), tok);
    }
    Infil2DDest d;
    EXPECT_FALSE(parseInfil2DDest("THE_SEA", d));
}

// ============================================================================
// G6 (unit half) — §5.5.2's dry-cell rule, isolated from the router.
// ============================================================================

// Drive updateRates directly across wet / dry / wet. With a drying time set,
// Horton recovery must restore capacity across the dry window; with it zeroed,
// it must not. This says nothing about whether SurfaceRouter2D actually KEEPS
// CALLING updateRates on a mesh that has gone dry — that is the integration
// half of the gate. It isolates which of the two is at fault when the
// end-to-end run shows no recovery.
TEST(Infil2DDryCellTest, HortonRecoversCapacityAcrossADryWindow) {
    MeshData mesh = makeTaggedMesh();

    auto runSequence = [&](double dry_time_days) {
        Infil2D infil = resolvedWith(
            mesh, usOptions(),
            rowOf(InfilModel::HORTON, 3.0, 0.5, 4.14, dry_time_days, 0.0));
        SurfaceStateData st = makeState(mesh, 0.0, 0.0);
        constexpr double kDt = 300.0;
        // Event 1: 10 min of 2 in/hr rain on a bare surface.
        for (int n = 0; n < 2; ++n) {
            for (int i = 0; i < mesh.n_triangles(); ++i) {
                st.rainfall[i] = inhrToMs(2.0);
                st.depth[i]    = 0.0;
            }
            infil.updateRates(mesh, st, kDt);
        }
        // Gap: 2h20m bone dry.
        for (int n = 0; n < 28; ++n) {
            for (int i = 0; i < mesh.n_triangles(); ++i) {
                st.rainfall[i] = 0.0;
                st.depth[i]    = 0.0;
            }
            infil.updateRates(mesh, st, kDt);
        }
        // Event 2: the rain returns. Report the capacity it presents.
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            st.rainfall[i] = inhrToMs(2.0);
            st.depth[i]    = 0.0;
        }
        infil.updateRates(mesh, st, kDt);
        return st.infil_rate[0];
    };

    const double recovered = runSequence(0.05);   // ~72 min drying time
    const double stale     = runSequence(0.0);    // recovery disabled

    EXPECT_GT(recovered, stale * 1.05)
        << "after a 2h20m dry window a cell with a 0.05 d drying time presented "
        << recovered << " m/s and one with recovery disabled " << stale
        << " m/s — Infil2D::updateRates is not advancing Horton recovery on a "
           "dry cell";
}
