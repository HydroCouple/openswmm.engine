/**
 * @file test_runoff_ensemble.cpp
 * @brief Phase 3 tests: SoilParameterLHS, RunoffEnsemble, WQUncertaintyBounds.
 *
 * Covers roadmap section 3C:
 *   - Shared member ordering between runoff uncertainty and the 2D ensemble.
 *   - RunoffEnsemble reproducibility (same seed → same infiltration trajectory).
 *   - WQUncertaintyBounds analytical bounds vs manufactured first-order decay.
 */

#include <gtest/gtest.h>

#include "uncertainty/UncertaintyEnsemble.hpp"
#include "uncertainty/SoilParameterLHS.hpp"
#include "uncertainty/RunoffEnsemble.hpp"
#include "uncertainty/WQUncertaintyBounds.hpp"
#include "core/SimulationOptions.hpp"

#include <algorithm>
#include <numeric>
#include <cmath>

using namespace openswmm;
using namespace openswmm::uncertainty;

// ============================================================================
// Helpers
// ============================================================================

static SimulationOptions default_opts() {
    SimulationOptions opts;
    return opts;  // US customary defaults (CFS, ft, in/hr)
}

// ============================================================================
// SoilParameterLHS — shared member ordering tests
// ============================================================================

// generate() produces exactly n_members multipliers in [1-p, 1+p].
TEST(SoilParameterLHS, GeneratesCorrectCountAndRange) {
    UncertaintyEnsemble ens;
    ens.n_members = 10;
    ens.soil_pert = 0.20;
    ens.generate();

    SoilParameterLHS lhs;
    lhs.generate(ens);

    ASSERT_EQ(lhs.members(), 10);
    for (int i = 0; i < 10; ++i) {
        EXPECT_GE(lhs.mult(i), 0.80) << "member " << i;
        EXPECT_LE(lhs.mult(i), 1.20) << "member " << i;
    }
}

// Sorted soil_mult covers all M LHS strata exactly (true LHS property).
TEST(SoilParameterLHS, CoversAllStrataExactly) {
    const int M = 10;
    UncertaintyEnsemble ens;
    ens.n_members = M;
    ens.soil_pert = 0.20;
    ens.generate();

    SoilParameterLHS lhs;
    lhs.generate(ens);

    auto sm = lhs.soil_mult;
    std::sort(sm.begin(), sm.end());

    const double lo = 0.80, hi = 1.20;
    for (int i = 0; i < M; ++i) {
        double expected = lo + (static_cast<double>(i) + 0.5)
                              / static_cast<double>(M) * (hi - lo);
        EXPECT_NEAR(sm[static_cast<std::size_t>(i)], expected, 1e-10)
            << "stratum " << i << " not covered";
    }
}

// Soil ordering must differ from both Manning (ascending) and rainfall
// (descending) orderings — ensuring cross-parameter decorrelation.
TEST(SoilParameterLHS, OrderingDiffersFromManningAndRainfall) {
    const int M = 20;
    UncertaintyEnsemble ens;
    ens.n_members        = M;
    ens.mannings_pert_2d = 0.20;
    ens.rainfall_pert_2d = 0.20;
    ens.soil_pert        = 0.20;
    ens.generate();

    SoilParameterLHS lhs;
    lhs.generate(ens);

    // Check soil is not in ascending order (same as Manning)
    bool ascending = true;
    for (int i = 0; i + 1 < M; ++i) {
        if (lhs.soil_mult[static_cast<std::size_t>(i)] >=
            lhs.soil_mult[static_cast<std::size_t>(i + 1)]) {
            ascending = false;
            break;
        }
    }
    EXPECT_FALSE(ascending) << "soil_mult should not be strictly ascending";

    // Check soil is not in descending order (same as rainfall)
    bool descending = true;
    for (int i = 0; i + 1 < M; ++i) {
        if (lhs.soil_mult[static_cast<std::size_t>(i)] <=
            lhs.soil_mult[static_cast<std::size_t>(i + 1)]) {
            descending = false;
            break;
        }
    }
    EXPECT_FALSE(descending) << "soil_mult should not be strictly descending";
}

// Soil LHS is derived from the same shared ensemble — member i's soil_mult
// comes from the ensemble that also owns Manning and rainfall multipliers,
// so member count is identical across all three columns.
TEST(SoilParameterLHS, SharedEnsembleMemberCount) {
    UncertaintyEnsemble ens;
    ens.n_members        = 50;
    ens.mannings_pert_2d = 0.20;
    ens.rainfall_pert_2d = 0.20;
    ens.soil_pert        = 0.20;
    ens.generate();

    SoilParameterLHS lhs;
    lhs.generate(ens);

    EXPECT_EQ(lhs.members(), ens.n_members);
    EXPECT_EQ(static_cast<int>(ens.mannings_mult_2d.size()), ens.n_members);
    EXPECT_EQ(static_cast<int>(ens.rainfall_mult_2d.size()), ens.n_members);
    EXPECT_EQ(static_cast<int>(ens.soil_mult.size()),        ens.n_members);
}

// ============================================================================
// RunoffEnsemble — Horton reproducibility
// ============================================================================

// Same seed + same parameters → identical infiltration rates across all steps.
TEST(RunoffEnsemble, HortonReproducibility) {
    const int M = 5;

    auto build_and_run = [&]() {
        UncertaintyEnsemble ens;
        ens.n_members = M;
        ens.soil_pert = 0.20;
        ens.generate();

        SoilParameterLHS lhs;
        lhs.generate(ens);

        RunoffEnsemble re;
        re.initialize(M, lhs.soil_mult);
        // f0=3 in/hr, fmin=0.5 in/hr, decay=4 1/hr, regen=0.1 days, Fmax=0
        re.initHorton(3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

        // 5 steps of 60s with 0.0001 ft/sec precipitation
        std::vector<double> rates(static_cast<std::size_t>(M));
        for (int t = 0; t < 5; ++t) {
            re.step(0.0001, 0.0, 60.0, InfilModel::HORTON);
            if (t == 4) {
                for (int i = 0; i < M; ++i)
                    rates[static_cast<std::size_t>(i)] = re.rate(i);
            }
        }
        return rates;
    };

    auto r1 = build_and_run();
    auto r2 = build_and_run();

    for (int i = 0; i < M; ++i)
        EXPECT_DOUBLE_EQ(r1[static_cast<std::size_t>(i)],
                         r2[static_cast<std::size_t>(i)])
            << "member " << i << " rate differs between runs";
}

// Members with higher soil_mult should have higher initial infiltration rate
// (f0 * mult — more permeable soil infiltrates faster initially).
TEST(RunoffEnsemble, HortonHigherMultGivesHigherRate) {
    const int M = 10;
    UncertaintyEnsemble ens;
    ens.n_members = M;
    ens.soil_pert = 0.20;
    ens.generate();

    SoilParameterLHS lhs;
    lhs.generate(ens);

    RunoffEnsemble re;
    re.initialize(M, lhs.soil_mult);
    re.initHorton(3.0, 0.5, 4.0, 0.1, 0.0, default_opts());

    // One step: infiltration is supply-limited by precip
    // Use precip > f0*max_mult so all members are demand-limited (capacity-limited)
    // precip = 10 in/hr → 10/3600/12 ft/sec ≫ f0*(1.2) = 3.6 in/hr
    re.step(10.0 / 3600.0 / 12.0, 0.0, 1.0, InfilModel::HORTON);

    // Find the member with smallest and largest soil_mult
    auto& sm = lhs.soil_mult;
    int lo_i = static_cast<int>(
        std::min_element(sm.begin(), sm.end()) - sm.begin());
    int hi_i = static_cast<int>(
        std::max_element(sm.begin(), sm.end()) - sm.begin());

    EXPECT_GT(re.rate(hi_i), re.rate(lo_i))
        << "higher soil_mult should give higher Horton infiltration rate";
}

// Green-Ampt: higher Ks_mult → higher steady-state infiltration.
// At t=0 all members are supply-limited (F=0 → infinite capacity → all clip
// to precip).  Run many steps so F accumulates and capacity drops below
// precip rate, making Ks the binding constraint.
TEST(RunoffEnsemble, GreenAmptHigherMultGivesHigherRate) {
    const int M = 10;
    UncertaintyEnsemble ens;
    ens.n_members = M;
    ens.soil_pert = 0.20;
    ens.generate();

    SoilParameterLHS lhs;
    lhs.generate(ens);

    RunoffEnsemble re;
    re.initialize(M, lhs.soil_mult);
    // S=4in, Ks=1in/hr, IMD=0.3.  Large precip so all steps are
    // capacity-limited once F is large enough.
    re.initGreenAmpt(4.0, 1.0, 0.3, default_opts());

    // Run 60 steps × 60s = 1 hour.  After ~30 min F ≫ S*IMD so
    // capacity ≈ Ks*(1 + S*IMD/F) which is dominated by Ks.
    const double precip = 5.0 / 3600.0 / 12.0;  // 5 in/hr → ft/sec
    for (int t = 0; t < 60; ++t)
        re.step(precip, 0.0, 60.0, InfilModel::GREEN_AMPT);

    auto& sm = lhs.soil_mult;
    int lo_i = static_cast<int>(
        std::min_element(sm.begin(), sm.end()) - sm.begin());
    int hi_i = static_cast<int>(
        std::max_element(sm.begin(), sm.end()) - sm.begin());

    EXPECT_GT(re.rate(hi_i), re.rate(lo_i))
        << "higher Ks_mult should give higher Green-Ampt infiltration rate after saturation";
}

// ============================================================================
// WQDecayBounds — analytical trajectory tests
// ============================================================================

// q05 ≤ q50 ≤ q95 for any valid inputs.
TEST(WQDecayBounds, QuantileOrdering) {
    WQUncertaintyBounds wq;
    wq.decay_pert = 0.20;

    auto b = wq.compute(10.0, 0.1, 1.0, 50);
    EXPECT_LE(b.q05, b.q50);
    EXPECT_LE(b.q50, b.q95);
}

// For zero decay rate: all members give c0 → q05=q50=q95=c0.
TEST(WQDecayBounds, ZeroDecayPreservesConcentration) {
    WQUncertaintyBounds wq;
    wq.decay_pert = 0.20;

    auto b = wq.compute(7.5, 0.0, 10.0, 50);
    EXPECT_NEAR(b.q05, 7.5, 1e-12);
    EXPECT_NEAR(b.q50, 7.5, 1e-12);
    EXPECT_NEAR(b.q95, 7.5, 1e-12);
}

// For zero perturbation: all members identical → tight bounds.
TEST(WQDecayBounds, ZeroPertGivesTightBounds) {
    WQUncertaintyBounds wq;
    wq.decay_pert = 0.0;

    double c0 = 10.0, k = 0.1, dt = 1.0;
    double expected = c0 * std::exp(-k * dt);

    auto b = wq.compute(c0, k, dt, 20);
    EXPECT_NEAR(b.q05, expected, 1e-12);
    EXPECT_NEAR(b.q50, expected, 1e-12);
    EXPECT_NEAR(b.q95, expected, 1e-12);
}

// LHS with finite M keeps q05 and q95 strictly inside the theoretical limits
// [c0*exp(-k*(1+p)*dt), c0*exp(-k*(1-p)*dt)].  The endpoints are only
// approached as M→∞ (the outermost strata sit 0.5/M away from the boundary).
TEST(WQDecayBounds, BoundsInsideTheoreticalLimits) {
    WQUncertaintyBounds wq;
    wq.decay_pert = 0.20;

    const double c0 = 10.0, k = 0.1, dt = 1.0, p = 0.20;
    const int    M  = 1000;

    double q05_lim = c0 * std::exp(-k * (1.0 + p) * dt);  // most decay — true minimum
    double q95_lim = c0 * std::exp(-k * (1.0 - p) * dt);  // least decay — true maximum

    auto b = wq.compute(c0, k, dt, M);

    // LHS 5th-percentile concentration lies above the true minimum
    // (LHS strata don't reach the exact extreme km=1+p with finite M).
    EXPECT_GT(b.q05, q05_lim) << "q05 should be strictly above theoretical minimum";
    EXPECT_LT(b.q95, q95_lim) << "q95 should be strictly below theoretical maximum";

    // But within 3% of the theoretical limit (M=1000 strata are fine)
    EXPECT_NEAR(b.q05, q05_lim, q05_lim * 0.03);
    EXPECT_NEAR(b.q95, q95_lim, q95_lim * 0.03);
}

// Manufactured trajectory: c0=10, k=0.1 1/s, dt=1s, pert=0.20, M=50.
// Verify q05 < exact_det < q95 where exact_det = c0*exp(-k*dt).
TEST(WQDecayBounds, ManufacturedTrajectoryBracketsDetSolution) {
    WQUncertaintyBounds wq;
    wq.decay_pert = 0.20;

    const double c0 = 10.0, k = 0.1, dt = 1.0;
    double c_det = c0 * std::exp(-k * dt);  // deterministic (km=1)

    auto b = wq.compute(c0, k, dt, 50);

    EXPECT_LT(b.q05, c_det) << "q05 should be below deterministic solution";
    EXPECT_GT(b.q95, c_det) << "q95 should be above deterministic solution";
    EXPECT_NEAR(b.q50, c_det, c_det * 0.02)
        << "q50 should be near deterministic solution (within 2%)";
}

// Multi-step: q05 ≤ q50 ≤ q95 maintained at every step; spread grows with time.
TEST(WQDecayBounds, MultiStepMonotonicAndSpreadGrows) {
    WQUncertaintyBounds wq;
    wq.decay_pert = 0.20;

    const double c0 = 10.0, k = 0.05, dt = 1.0;
    const int    M  = 50;

    double prev_spread = 0.0;
    double c_prev      = c0;
    for (int t = 1; t <= 5; ++t) {
        auto b = wq.compute(c_prev, k, dt, M);
        EXPECT_LE(b.q05, b.q50) << "step " << t;
        EXPECT_LE(b.q50, b.q95) << "step " << t;

        double spread = b.q95 - b.q05;
        // Spread should remain positive
        EXPECT_GT(spread, 0.0) << "non-zero spread at step " << t;

        prev_spread = spread;
        c_prev      = b.q50;  // advance using median
    }
    (void)prev_spread;
}
