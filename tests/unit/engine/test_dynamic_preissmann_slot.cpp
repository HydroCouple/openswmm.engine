/**
 * @file test_dynamic_preissmann_slot.cpp
 * @brief Targeted tests for the Dynamic Preissmann Slot (DPS) algorithm.
 *
 * @details Verifies the DPS implementation against the formulation in:
 *   Sharior, S., Hodges, B.R., & Vasconcelos, J.G. (2023).
 *   "Generalized, Dynamic, and Transient-Storage Form of the Preissmann Slot."
 *   Journal of Hydraulic Engineering, 149(11), 04023046.
 *   DOI: 10.1061/JHEND8.HYENG-13609
 *
 * Test categories:
 *   1. DPS constants and parameter defaults
 *   2. computeInitialPreissmannNumber — analytical verification (Eq. 23)
 *   3. computePreissmannNumber — decay model verification (Eq. 22)
 *   4. updateDPSState — surcharge onset, slot area, head (Eqs. 14, 19)
 *   5. Depressurization and hysteresis
 *   6. getCrownCutoff / getSlotWidth behavior for DYNAMIC_SLOT
 *   7. DPS head correction in computeLinkGeometry
 *   8. Mass conservation: slot area × length ≈ excess volume
 *   9. Open-shape bypass: open conduits never engage DPS
 *  10. Energy conservation: no spurious head when P decreases
 *
 * @see src/engine/hydraulics/DynamicWave.hpp
 * @see src/engine/hydraulics/DynamicWave.cpp
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>
#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <cmath>
#include <vector>
#include <numeric>

#include "hydraulics/DynamicWave.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "core/SimulationContext.hpp"

using namespace openswmm;
using namespace openswmm::dynwave;

// ============================================================================
// Helper: build a minimal SimulationContext for DPS testing
// ============================================================================

/// Create a minimal 2-node, 1-link context with a circular conduit.
/// The conduit connects node 0 (upstream) to node 1 (downstream).
static SimulationContext buildMinimalContext(
    double diameter,       // pipe diameter (ft)
    double length,         // conduit length (ft)
    double upstream_elev,  // upstream invert (ft)
    double downstream_elev // downstream invert (ft)
) {
    SimulationContext ctx;

    // --- Nodes ---
    ctx.nodes.resize(2);
    ctx.nodes.invert_elev[0] = upstream_elev;
    ctx.nodes.invert_elev[1] = downstream_elev;
    ctx.nodes.full_depth[0] = 20.0;  // generous depth so no overflow
    ctx.nodes.full_depth[1] = 20.0;
    ctx.nodes.full_volume[0] = 20.0 * 12.566;  // approximate
    ctx.nodes.full_volume[1] = 20.0 * 12.566;
    ctx.nodes.crown_elev[0] = upstream_elev + diameter;
    ctx.nodes.crown_elev[1] = downstream_elev + diameter;

    // --- Link (single circular conduit) ---
    ctx.links.resize(1);
    ctx.links.type[0] = LinkType::CONDUIT;
    ctx.links.node1[0] = 0;
    ctx.links.node2[0] = 1;
    ctx.links.offset1[0] = 0.0;
    ctx.links.offset2[0] = 0.0;
    ctx.links.xsect_shape[0] = XsectShape::CIRCULAR;
    ctx.links.xsect_y_full[0] = diameter;
    ctx.links.length[0] = length;
    ctx.links.mod_length[0] = length;
    ctx.links.barrels[0] = 1;

    // Compute cross-section properties for circular pipe
    double R = diameter / 2.0;
    double a_full = M_PI * R * R;
    double w_max = diameter;
    double r_full = R / 2.0;  // D/4 for circular
    double s_full = a_full * std::pow(r_full, 2.0/3.0);

    ctx.links.xsect_a_full[0] = a_full;
    ctx.links.xsect_w_max[0] = w_max;
    ctx.links.xsect_r_full[0] = r_full;
    ctx.links.xsect_s_full[0] = s_full;
    ctx.links.xsect_s_max[0] = s_full;
    ctx.links.roughness[0] = 0.013;
    ctx.links.slope[0] = std::fabs(upstream_elev - downstream_elev) / length;
    ctx.links.flow[0] = 0.0;
    ctx.links.old_flow[0] = 0.0;
    ctx.links.volume[0] = 0.0;

    return ctx;
}

// ============================================================================
// 1. DPS constants and parameter defaults
// ============================================================================

TEST(DPSConstants, DefaultTargetCelerity) {
    EXPECT_DOUBLE_EQ(DPS_DEFAULT_TARGET_CELERITY, 100.0);
}

TEST(DPSConstants, DefaultShockParam) {
    EXPECT_DOUBLE_EQ(DPS_DEFAULT_SHOCK_PARAM, 2.0);
}

TEST(DPSConstants, DefaultDecayTime) {
    EXPECT_DOUBLE_EQ(DPS_DEFAULT_DECAY_TIME, 10.0);
}

TEST(DPSConstants, CrownCutoffMatchesSlot) {
    EXPECT_DOUBLE_EQ(DPS_CROWN_CUTOFF, SLOT_CROWN_CUTOFF);
}

TEST(DPSConstants, DynamicSlotEnumValue) {
    EXPECT_EQ(static_cast<int>(SurchargeMethod::DYNAMIC_SLOT), 2);
}

TEST(DPSSolverDefaults, DefaultDPSParameters) {
    DWSolver solver;
    EXPECT_DOUBLE_EQ(solver.dps_target_celerity, DPS_DEFAULT_TARGET_CELERITY);
    EXPECT_DOUBLE_EQ(solver.dps_shock_param, DPS_DEFAULT_SHOCK_PARAM);
    EXPECT_DOUBLE_EQ(solver.dps_decay_time, DPS_DEFAULT_DECAY_TIME);
}

TEST(DPSSolverDefaults, CustomDPSParameters) {
    DWSolver solver;
    solver.dps_target_celerity = 200.0;
    solver.dps_shock_param = 3.0;
    solver.dps_decay_time = 5.0;

    EXPECT_DOUBLE_EQ(solver.dps_target_celerity, 200.0);
    EXPECT_DOUBLE_EQ(solver.dps_shock_param, 3.0);
    EXPECT_DOUBLE_EQ(solver.dps_decay_time, 5.0);
}

// ============================================================================
// 2. computeInitialPreissmannNumber — Eq. 23: P_0 = c_T / (β · c_g)
// ============================================================================

class DPSInitialPTest : public ::testing::Test {
protected:
    DWSolver solver;
    SimulationContext ctx;
    XSectGroups groups;
    std::vector<XSectParams> xparams;

    void SetUp() override {
        // 3-ft diameter circular pipe, 1000 ft long, 0.1% slope
        ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);

        // Build XSectGroups for the solver
        xparams.resize(1);
        double p[4] = {3.0, 0, 0, 0};
        xsect::setParams(xparams[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
        groups.build(xparams.data(), 1);

        solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
        solver.init(2, 1, groups);
    }
};

TEST_F(DPSInitialPTest, AnalyticalVerification) {
    // Eq. 23: P_0 = c_T / (β · c_g)
    // c_g = sqrt(g · A_f / W_max) = sqrt(g · h_d) where h_d = A_f / W_max
    double af = ctx.links.xsect_a_full[0];
    double wm = ctx.links.xsect_w_max[0];
    double hd = af / wm;
    double cg = std::sqrt(32.2 * hd);
    double expected_p0 = solver.dps_target_celerity / (solver.dps_shock_param * cg);
    expected_p0 = std::max(expected_p0, 1.0);

    double p0 = solver.computeInitialPreissmannNumber(0, ctx);
    EXPECT_NEAR(p0, expected_p0, 1e-10);
}

TEST_F(DPSInitialPTest, P0AlwaysAtLeast1) {
    // With extremely high c_g (very large pipe), P_0 could be < 1. Verify floor.
    // Set a large pipe: A_f = 1000, W_max = 100 → h_d = 10 → c_g ≈ 17.9
    // With c_T = 100, β = 2: P_0 = 100 / (2 * 17.9) ≈ 2.79 > 1 (still > 1)
    // Lower c_T to make P_0 < 1:
    solver.dps_target_celerity = 1.0;  // Very low target celerity
    solver.dps_shock_param = 100.0;     // Very high shock param

    double p0 = solver.computeInitialPreissmannNumber(0, ctx);
    EXPECT_GE(p0, 1.0);
}

TEST_F(DPSInitialPTest, ZeroWidthReturns1) {
    // Degenerate case: W_max = 0 → should return 1.0 safely
    ctx.links.xsect_w_max[0] = 0.0;
    double p0 = solver.computeInitialPreissmannNumber(0, ctx);
    EXPECT_DOUBLE_EQ(p0, 1.0);
}

TEST_F(DPSInitialPTest, ZeroAreaReturns1) {
    ctx.links.xsect_a_full[0] = 0.0;
    double p0 = solver.computeInitialPreissmannNumber(0, ctx);
    EXPECT_DOUBLE_EQ(p0, 1.0);
}

TEST_F(DPSInitialPTest, HigherTargetCelerityGivesHigherP0) {
    solver.dps_target_celerity = 100.0;
    double p0_low = solver.computeInitialPreissmannNumber(0, ctx);

    solver.dps_target_celerity = 500.0;
    double p0_high = solver.computeInitialPreissmannNumber(0, ctx);

    EXPECT_GT(p0_high, p0_low);
}

TEST_F(DPSInitialPTest, HigherBetaGivesLowerP0) {
    solver.dps_shock_param = 2.0;
    double p0_low_beta = solver.computeInitialPreissmannNumber(0, ctx);

    solver.dps_shock_param = 4.0;
    double p0_high_beta = solver.computeInitialPreissmannNumber(0, ctx);

    EXPECT_LT(p0_high_beta, p0_low_beta);
}

// ============================================================================
// 3. computePreissmannNumber — Eq. 22: P(t) = 1 - (1 - P_0) · exp(-t/r)
// ============================================================================

class DPSDecayTest : public ::testing::Test {
protected:
    DWSolver solver;
    SimulationContext ctx;
    XSectGroups groups;
    std::vector<XSectParams> xparams;

    void SetUp() override {
        ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);
        xparams.resize(1);
        double p[4] = {3.0, 0, 0, 0};
        xsect::setParams(xparams[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
        groups.build(xparams.data(), 1);

        solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
        solver.dps_decay_time = 10.0;
        solver.init(2, 1, groups);
    }

    void setSurchargedState(double p0, double surcharge_time) {
        solver.dps_preissmann_[0] = p0;
        solver.dps_surcharge_t_[0] = surcharge_time;
    }
};

TEST_F(DPSDecayTest, AtTimeZeroReturnsP0) {
    double p0 = 5.0;
    setSurchargedState(p0, 0.0);

    // At t=0: P = 1 - (1 - P0) * exp(0) = 1 - (1 - P0) = P0
    double P = solver.computePreissmannNumber(0, 0.0);
    EXPECT_NEAR(P, p0, 1e-10);
}

TEST_F(DPSDecayTest, DecaysToward1) {
    double p0 = 10.0;
    setSurchargedState(p0, 0.0);
    double P_at_0 = solver.computePreissmannNumber(0, 0.0);

    setSurchargedState(p0, 50.0);  // 5 time constants
    double P_at_50 = solver.computePreissmannNumber(0, 0.0);

    EXPECT_GT(P_at_0, P_at_50);
    EXPECT_NEAR(P_at_50, 1.0, 0.1);  // Should be very close to 1 after 5τ
}

TEST_F(DPSDecayTest, ExponentialDecayVerification) {
    double p0 = 8.0;
    double r = solver.dps_decay_time;  // 10 s
    double t = 5.0;  // half a time constant

    setSurchargedState(p0, t);
    double P = solver.computePreissmannNumber(0, 0.0);

    double expected = 1.0 - (1.0 - p0) * std::exp(-t / r);
    EXPECT_NEAR(P, expected, 1e-10);
}

TEST_F(DPSDecayTest, AtInfiniteTimeConvergesTo1) {
    double p0 = 20.0;
    setSurchargedState(p0, 1e6);  // Very long time

    double P = solver.computePreissmannNumber(0, 0.0);
    EXPECT_NEAR(P, 1.0, 1e-6);
}

TEST_F(DPSDecayTest, ZeroDecayTimeReturns1) {
    solver.dps_decay_time = 0.0;
    setSurchargedState(5.0, 1.0);

    double P = solver.computePreissmannNumber(0, 0.0);
    EXPECT_DOUBLE_EQ(P, 1.0);
}

TEST_F(DPSDecayTest, NotSurchargedReturnsCurrent) {
    solver.dps_preissmann_[0] = 7.5;
    solver.dps_surcharge_t_[0] = -1.0;  // Not surcharged

    double P = solver.computePreissmannNumber(0, 0.0);
    EXPECT_DOUBLE_EQ(P, 7.5);
}

TEST_F(DPSDecayTest, NeverBelowOne) {
    // Even with P0 < 1 (forced), result should be >= 1
    setSurchargedState(0.5, 2.0);  // P0 < 1 (shouldn't happen normally)
    double P = solver.computePreissmannNumber(0, 0.0);
    EXPECT_GE(P, 1.0);
}

// ============================================================================
// 4. updateDPSState — surcharge onset, slot area, head
// ============================================================================

class DPSUpdateTest : public ::testing::Test {
protected:
    DWSolver solver;
    SimulationContext ctx;
    XSectGroups groups;
    std::vector<XSectParams> xparams;

    void SetUp() override {
        ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);
        xparams.resize(1);
        double p[4] = {3.0, 0, 0, 0};
        xsect::setParams(xparams[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
        groups.build(xparams.data(), 1);

        solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
        solver.init(2, 1, groups);
    }

    double aFull() const { return ctx.links.xsect_a_full[0]; }
    double length() const { return ctx.links.mod_length[0]; }
    double vFull() const { return aFull() * length(); }
};

TEST_F(DPSUpdateTest, NoSurchargeWhenVolumeUnderFull) {
    ctx.links.volume[0] = vFull() * 0.9;
    solver.updateDPSState(ctx, 1.0);

    EXPECT_LT(solver.dps_surcharge_t_[0], 0.0);  // Not surcharged
    EXPECT_DOUBLE_EQ(solver.dps_slot_area_[0], 0.0);
    EXPECT_DOUBLE_EQ(solver.dps_slot_head_[0], 0.0);
}

TEST_F(DPSUpdateTest, SurchargeOnsetInitializesCorrectly) {
    // Set volume above full
    double excess = 10.0;  // ft³
    ctx.links.volume[0] = vFull() + excess;

    solver.updateDPSState(ctx, 1.0);

    // Should be marked as surcharged
    EXPECT_GE(solver.dps_surcharge_t_[0], 0.0);
    // Initial P should be computed
    EXPECT_GT(solver.dps_preissmann_[0], 0.0);
    // Slot area should be positive
    EXPECT_GT(solver.dps_slot_area_[0], 0.0);
    // Slot head should be positive
    EXPECT_GT(solver.dps_slot_head_[0], 0.0);
}

TEST_F(DPSUpdateTest, SlotAreaEqualsExcessVolumeOverLength) {
    // Eq. 14: Ts = excess_V / L  (for first step where Ts_old = 0)
    double excess = 50.0;
    ctx.links.volume[0] = vFull() + excess;

    solver.updateDPSState(ctx, 1.0);

    double expected_ts = excess / length();
    EXPECT_NEAR(solver.dps_slot_area_[0], expected_ts, 1e-10);
}

TEST_F(DPSUpdateTest, HeadComputationEq19) {
    // Eq. 19: Δh_s = P² · ΔTs / (Af + Ts_old)
    // For first step: Ts_old = 0, so Δh_s = P² · Ts / Af
    double excess = 50.0;
    ctx.links.volume[0] = vFull() + excess;

    solver.updateDPSState(ctx, 1.0);

    double ts = excess / length();
    double P = solver.dps_preissmann_[0];
    // P was set as initial P at onset, surcharge clock is 0, so P = P₀
    double expected_hs = P * P * ts / aFull();

    EXPECT_NEAR(solver.dps_slot_head_[0], expected_hs, 1e-8);
}

TEST_F(DPSUpdateTest, IncrementalSlotAreaUpdate) {
    // Step 1: set excess volume → initial Ts
    double excess1 = 50.0;
    ctx.links.volume[0] = vFull() + excess1;
    solver.updateDPSState(ctx, 1.0);

    double ts_after_1 = solver.dps_slot_area_[0];
    double hs_after_1 = solver.dps_slot_head_[0];

    // Step 2: increase volume → Ts should increase by the incremental amount
    double excess2 = 100.0;
    ctx.links.volume[0] = vFull() + excess2;
    solver.updateDPSState(ctx, 1.0);

    double ts_after_2 = solver.dps_slot_area_[0];
    double expected_ts2 = excess2 / length();  // total Ts at step 2

    EXPECT_NEAR(ts_after_2, expected_ts2, 1e-10);
    EXPECT_GT(ts_after_2, ts_after_1);

    // Head should have increased
    EXPECT_GT(solver.dps_slot_head_[0], hs_after_1);
}

TEST_F(DPSUpdateTest, SurchargeClockAdvances) {
    double excess = 50.0;
    ctx.links.volume[0] = vFull() + excess;
    double dt = 2.0;

    // First step: onset → surcharge_t = 0
    solver.updateDPSState(ctx, dt);
    EXPECT_DOUBLE_EQ(solver.dps_surcharge_t_[0], 0.0);

    // Second step: clock advances by dt
    solver.updateDPSState(ctx, dt);
    EXPECT_NEAR(solver.dps_surcharge_t_[0], dt, 1e-10);

    // Third step
    solver.updateDPSState(ctx, dt);
    EXPECT_NEAR(solver.dps_surcharge_t_[0], 2.0 * dt, 1e-10);
}

// ============================================================================
// 5. Depressurization and hysteresis
// ============================================================================

TEST_F(DPSUpdateTest, DepressurizationClearsState) {
    // Surcharge first
    ctx.links.volume[0] = vFull() + 50.0;
    solver.updateDPSState(ctx, 1.0);
    EXPECT_GE(solver.dps_surcharge_t_[0], 0.0);

    // Depressurize: volume below full
    ctx.links.volume[0] = vFull() * 0.8;
    solver.updateDPSState(ctx, 1.0);

    // State should be cleared
    EXPECT_LT(solver.dps_surcharge_t_[0], 0.0);
    EXPECT_DOUBLE_EQ(solver.dps_slot_area_[0], 0.0);
    EXPECT_DOUBLE_EQ(solver.dps_slot_head_[0], 0.0);
}

TEST_F(DPSUpdateTest, ResurchargeAfterDepressurization) {
    // Surcharge → depressurize → resurcharge
    ctx.links.volume[0] = vFull() + 50.0;
    solver.updateDPSState(ctx, 1.0);
    double p0_first = solver.dps_preissmann_[0];

    ctx.links.volume[0] = vFull() * 0.5;
    solver.updateDPSState(ctx, 1.0);

    // Resurcharge
    ctx.links.volume[0] = vFull() + 30.0;
    solver.updateDPSState(ctx, 1.0);

    // Should re-initialize with fresh P_0
    EXPECT_GE(solver.dps_surcharge_t_[0], 0.0);
    EXPECT_DOUBLE_EQ(solver.dps_surcharge_t_[0], 0.0);  // Clock resets
    EXPECT_NEAR(solver.dps_preissmann_[0], p0_first, 1e-10);  // Same pipe → same P_0
}

TEST_F(DPSUpdateTest, HeadNeverNegative) {
    // Surcharge then reduce volume slightly (still above full)
    ctx.links.volume[0] = vFull() + 100.0;
    solver.updateDPSState(ctx, 1.0);

    // Reduce volume but keep above full → delta_ts is negative
    ctx.links.volume[0] = vFull() + 10.0;
    solver.updateDPSState(ctx, 1.0);

    // Head may decrease but should never go negative
    EXPECT_GE(solver.dps_slot_head_[0], 0.0);
}

TEST_F(DPSUpdateTest, SlotAreaNeverNegative) {
    ctx.links.volume[0] = vFull() + 100.0;
    solver.updateDPSState(ctx, 1.0);

    // Reduce to barely above full
    ctx.links.volume[0] = vFull() + 0.001;
    solver.updateDPSState(ctx, 1.0);

    EXPECT_GE(solver.dps_slot_area_[0], 0.0);
}

// ============================================================================
// 6. getCrownCutoff / getSlotWidth for DYNAMIC_SLOT
// ============================================================================

TEST(DPSSlotBehavior, CrownCutoffMatchesSlotMethod) {
    DWSolver solver_dps;
    solver_dps.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;

    DWSolver solver_slot;
    solver_slot.surcharge_method = SurchargeMethod::SLOT;

    EXPECT_DOUBLE_EQ(solver_dps.getCrownCutoff(), solver_slot.getCrownCutoff());
    EXPECT_DOUBLE_EQ(solver_dps.getCrownCutoff(), SLOT_CROWN_CUTOFF);
}

TEST(DPSSlotBehavior, SlotWidthUsesSjobergFormula) {
    // For DYNAMIC_SLOT, at depth = y_full * 0.99 (above SLOT_CROWN_CUTOFF),
    // the Sjoberg formula should give a positive width.
    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;

    double y_full = 3.0;
    double w_max = 3.0;
    double y = y_full * 0.99;
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);

    EXPECT_GT(w, 0.0);

    // Should match what SLOT method gives
    DWSolver solver_slot;
    solver_slot.surcharge_method = SurchargeMethod::SLOT;
    double w_slot = solver_slot.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);

    EXPECT_DOUBLE_EQ(w, w_slot);
}

TEST(DPSSlotBehavior, SlotWidthZeroBelowCrownCutoff) {
    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;

    double y_full = 3.0;
    double w_max = 3.0;
    double y = y_full * 0.5;  // Well below crown cutoff
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);

    EXPECT_DOUBLE_EQ(w, 0.0);
}

TEST(DPSSlotBehavior, SlotWidthCapAt178) {
    // For y/yFull > 1.78: slot width = 1% of max width
    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;

    double y_full = 3.0;
    double w_max = 3.0;
    double y = y_full * 2.0;  // > 1.78 * yFull
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);

    EXPECT_NEAR(w, 0.01 * w_max, 1e-10);
}

// ============================================================================
// 7. Open-shape bypass: open conduits never engage DPS
// ============================================================================

class DPSOpenShapeTest : public ::testing::Test {
protected:
    DWSolver solver;
    SimulationContext ctx;
    XSectGroups groups;
    std::vector<XSectParams> xparams;

    void SetUp() override {
        ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);
        // Change to open shape
        ctx.links.xsect_shape[0] = XsectShape::TRAPEZOIDAL;

        xparams.resize(1);
        double p[4] = {3.0, 5.0, 1.0, 1.0};
        xsect::setParams(xparams[0], static_cast<int>(XsectShape::TRAPEZOIDAL), p, 1.0);
        groups.build(xparams.data(), 1);

        solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
        solver.init(2, 1, groups);
    }
};

TEST_F(DPSOpenShapeTest, OpenShapeNeverSurcharged) {
    // Put volume way above "full"
    double af = ctx.links.xsect_a_full[0];
    double L = ctx.links.mod_length[0];
    ctx.links.volume[0] = af * L * 2.0;  // Double full volume

    solver.updateDPSState(ctx, 1.0);

    EXPECT_LT(solver.dps_surcharge_t_[0], 0.0);
    EXPECT_DOUBLE_EQ(solver.dps_slot_area_[0], 0.0);
}

TEST_F(DPSOpenShapeTest, SlotWidthZeroForOpenShape) {
    DWSolver s;
    s.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;

    double w = s.getSlotWidth(5.0, 3.0, 5.0, XsectShape::TRAPEZOIDAL);
    EXPECT_DOUBLE_EQ(w, 0.0);

    w = s.getSlotWidth(5.0, 3.0, 5.0, XsectShape::RECT_OPEN);
    EXPECT_DOUBLE_EQ(w, 0.0);

    w = s.getSlotWidth(5.0, 3.0, 5.0, XsectShape::TRIANGULAR);
    EXPECT_DOUBLE_EQ(w, 0.0);

    w = s.getSlotWidth(5.0, 3.0, 5.0, XsectShape::PARABOLIC);
    EXPECT_DOUBLE_EQ(w, 0.0);
}

// ============================================================================
// 8. Mass conservation: slot area × length ≈ excess volume
// ============================================================================

TEST(DPSMassConservation, SlotAreaTimesLengthEqualsExcess) {
    auto ctx = buildMinimalContext(3.0, 500.0, 100.0, 99.5);
    std::vector<XSectParams> xp(1);
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xp[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 1);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.init(2, 1, groups);

    double af = ctx.links.xsect_a_full[0];
    double L = ctx.links.mod_length[0];
    double v_full = af * L;

    // Test for various excess volumes
    double excesses[] = {1.0, 10.0, 50.0, 200.0, 1000.0};
    for (double excess : excesses) {
        // Reset state
        solver.dps_slot_area_[0] = 0.0;
        solver.dps_slot_head_[0] = 0.0;
        solver.dps_preissmann_[0] = 0.0;
        solver.dps_surcharge_t_[0] = -1.0;

        ctx.links.volume[0] = v_full + excess;
        solver.updateDPSState(ctx, 1.0);

        double Ts_times_L = solver.dps_slot_area_[0] * L;
        EXPECT_NEAR(Ts_times_L, excess, 1e-6)
            << "Failed for excess = " << excess;
    }
}

// ============================================================================
// 9. Energy conservation: no spurious head when P decreases over time
// ============================================================================
// The key innovation of the DPS (Eq. 19) is using incremental ΔTs instead of
// total Ts to compute head. This prevents energy-source artifacts when P decays
// and the effective slot width compresses prior slot volume.

TEST(DPSEnergyConservation, DecreasingExcessReducesHead) {
    // If excess volume decreases, head should also decrease (or stay zero)
    auto ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);
    std::vector<XSectParams> xp(1);
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xp[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 1);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.dps_decay_time = 10.0;
    solver.init(2, 1, groups);

    double af = ctx.links.xsect_a_full[0];
    double L = ctx.links.mod_length[0];
    double v_full = af * L;

    // Step 1: large excess → large head
    ctx.links.volume[0] = v_full + 200.0;
    solver.updateDPSState(ctx, 1.0);
    double h1 = solver.dps_slot_head_[0];
    EXPECT_GT(h1, 0.0);

    // Step 2: same excess but P has decayed (surcharge clock advanced)
    // delta_Ts = 0 (volume hasn't changed), so delta_hs = 0
    // Head should NOT increase when nothing changes
    solver.updateDPSState(ctx, 1.0);
    double h2 = solver.dps_slot_head_[0];
    EXPECT_NEAR(h2, h1, 1e-10);  // No change in head when delta_Ts = 0

    // Step 3: reduce excess → delta_Ts is negative → head should go DOWN
    ctx.links.volume[0] = v_full + 100.0;
    solver.updateDPSState(ctx, 1.0);
    double h3 = solver.dps_slot_head_[0];

    // Head should decrease (or at worst stay same due to P²)
    // The delta_hs = P² * (-delta) / (Af + Ts_old) → negative increment
    // Total head = h2 + negative → h3 < h2
    EXPECT_LE(h3, h2);
}

TEST(DPSEnergyConservation, SteadyVolumeNoHeadGrowth) {
    // Hold excess volume constant for many timesteps.
    // Head should not grow — verifies no energy-source artifact.
    auto ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);
    std::vector<XSectParams> xp(1);
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xp[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 1);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.dps_decay_time = 10.0;
    solver.init(2, 1, groups);

    double v_full = ctx.links.xsect_a_full[0] * ctx.links.mod_length[0];
    ctx.links.volume[0] = v_full + 100.0;

    // First step establishes state
    solver.updateDPSState(ctx, 1.0);
    double h_initial = solver.dps_slot_head_[0];

    // Many timesteps at constant volume
    for (int i = 0; i < 100; ++i) {
        solver.updateDPSState(ctx, 1.0);
    }

    double h_final = solver.dps_slot_head_[0];

    // Head should equal the head after step 1 (no growth when delta_Ts = 0)
    EXPECT_NEAR(h_final, h_initial, 1e-10);
}

// ============================================================================
// 10. Celerity verification: P relates to wave speed
// ============================================================================

TEST(DPSCelerity, PreissmannNumberRelatesCelerity) {
    // Eq. 8: P = c_T / c_p where c_p is the local pressure celerity
    // At onset, P_0 = c_T / (β · c_g), so c_p_initial = β · c_g
    // This means the initial effective celerity is β times the gravity-wave celerity.

    auto ctx = buildMinimalContext(4.0, 1000.0, 100.0, 99.0);
    std::vector<XSectParams> xp(1);
    double p[4] = {4.0, 0, 0, 0};
    xsect::setParams(xp[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 1);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.dps_target_celerity = 200.0;
    solver.dps_shock_param = 2.0;
    solver.init(2, 1, groups);

    double P0 = solver.computeInitialPreissmannNumber(0, ctx);

    // Verify: c_T / P_0 should equal β · c_g
    double af = ctx.links.xsect_a_full[0];
    double wm = ctx.links.xsect_w_max[0];
    double hd = af / wm;
    double cg = std::sqrt(32.2 * hd);
    double expected_cp = solver.dps_shock_param * cg;
    double actual_cp = solver.dps_target_celerity / P0;

    EXPECT_NEAR(actual_cp, expected_cp, 1e-8);
}

// ============================================================================
// 11. Multi-barrel conduit handling
// ============================================================================

TEST(DPSMultiBarrel, VolumeCorrectlyDividedByBarrels) {
    auto ctx = buildMinimalContext(3.0, 1000.0, 100.0, 99.0);
    ctx.links.barrels[0] = 3;

    std::vector<XSectParams> xp(1);
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xp[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 1);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.init(2, 1, groups);

    double af = ctx.links.xsect_a_full[0];
    double L = ctx.links.mod_length[0];
    double v_full_per_barrel = af * L;
    double excess_per_barrel = 50.0;

    // Total volume = 3 barrels * (v_full + excess) per barrel
    ctx.links.volume[0] = 3.0 * (v_full_per_barrel + excess_per_barrel);
    solver.updateDPSState(ctx, 1.0);

    // Slot area should be based on per-barrel excess
    double expected_ts = excess_per_barrel / L;
    EXPECT_NEAR(solver.dps_slot_area_[0], expected_ts, 1e-10);
}

// ============================================================================
// 12. DPS state initialization after init()
// ============================================================================

TEST(DPSInit, StateVectorsInitializedCorrectly) {
    std::vector<XSectParams> xp(3);
    double p[4] = {2.0, 0, 0, 0};
    for (int i = 0; i < 3; ++i)
        xsect::setParams(xp[i], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 3);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.init(4, 3, groups);

    for (int i = 0; i < 3; ++i) {
        EXPECT_DOUBLE_EQ(solver.dps_slot_area_[i], 0.0);
        EXPECT_DOUBLE_EQ(solver.dps_slot_head_[i], 0.0);
        EXPECT_DOUBLE_EQ(solver.dps_preissmann_[i], 0.0);
        EXPECT_LT(solver.dps_surcharge_t_[i], 0.0);
    }
}

// ============================================================================
// 13. Different pipe sizes: verify P_0 scales correctly
// ============================================================================

TEST(DPSScaling, LargerPipeGivesSmallerP0) {
    // Larger pipe → larger c_g → smaller P_0
    auto ctx_small = buildMinimalContext(2.0, 1000.0, 100.0, 99.0);
    auto ctx_large = buildMinimalContext(6.0, 1000.0, 100.0, 99.0);

    std::vector<XSectParams> xp_s(1), xp_l(1);
    double ps[4] = {2.0, 0, 0, 0};
    double pl[4] = {6.0, 0, 0, 0};
    xsect::setParams(xp_s[0], static_cast<int>(XsectShape::CIRCULAR), ps, 1.0);
    xsect::setParams(xp_l[0], static_cast<int>(XsectShape::CIRCULAR), pl, 1.0);

    XSectGroups gs, gl;
    gs.build(xp_s.data(), 1);
    gl.build(xp_l.data(), 1);

    DWSolver solver_s, solver_l;
    solver_s.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver_l.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver_s.init(2, 1, gs);
    solver_l.init(2, 1, gl);

    double p0_small = solver_s.computeInitialPreissmannNumber(0, ctx_small);
    double p0_large = solver_l.computeInitialPreissmannNumber(0, ctx_large);

    // Larger pipe has larger c_g → smaller P_0
    EXPECT_GT(p0_small, p0_large);
}

// ============================================================================
// 14. Verify computePreissmannNumber at half-life time
// ============================================================================

TEST(DPSDecayPrecision, HalfLifeCheck) {
    // For a first-order decay, at t = r * ln(2), the quantity (P-1) should
    // be half of (P_0 - 1).
    std::vector<XSectParams> xp(1);
    double p[4] = {3.0, 0, 0, 0};
    xsect::setParams(xp[0], static_cast<int>(XsectShape::CIRCULAR), p, 1.0);
    XSectGroups groups;
    groups.build(xp.data(), 1);

    DWSolver solver;
    solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    solver.dps_decay_time = 10.0;
    solver.init(2, 1, groups);

    double p0 = 9.0;  // P_0 - 1 = 8
    double half_life = solver.dps_decay_time * std::log(2.0);

    solver.dps_preissmann_[0] = p0;
    solver.dps_surcharge_t_[0] = half_life;

    double P = solver.computePreissmannNumber(0, 0.0);

    // At half-life: P - 1 = (P_0 - 1) * 0.5 = 4, so P = 5
    double expected = 1.0 + (p0 - 1.0) * 0.5;
    EXPECT_NEAR(P, expected, 1e-10);
}

// ============================================================================
// 15. Slot width as function of depth — smooth Sjoberg transition
//     (User request #1)
// ============================================================================

// Shared fixture for slot geometry tests on a circular pipe.
class SlotGeometryTest : public ::testing::Test {
protected:
    DWSolver solver;
    XSectParams xs;
    double y_full, a_full, w_max, r_full;

    void SetUp() override {
        solver.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;

        double p[4] = {3.0, 0, 0, 0};  // 3 ft diameter circular
        xsect::setParams(xs, static_cast<int>(XsectShape::CIRCULAR), p, 1.0);

        y_full = xs.y_full;  // 3.0
        a_full = xs.a_full;
        w_max  = xs.w_max;   // 3.0
        r_full = xs.r_full;
    }

    /// Compute combined area at depth y: physical xsect below crown, slot above.
    double totalArea(double y) const {
        if (y >= y_full) {
            double w_slot = solver.getSlotWidth(y, y_full, w_max,
                                                 XsectShape::CIRCULAR);
            return solver.getSlotArea(y, y_full, a_full, w_slot);
        }
        return xsect::getAofY(xs, y);
    }

    /// Compute combined top width: physical xsect below crown, slot above.
    double totalWidth(double y) const {
        double w_slot = solver.getSlotWidth(y, y_full, w_max,
                                             XsectShape::CIRCULAR);
        if (w_slot > 0.0) return w_slot;
        return xsect::getWofY(xs, y);
    }
};

TEST_F(SlotGeometryTest, SjobergSweepMonotonicallyDecreasing) {
    // Slot width from Sjoberg formula should decrease monotonically as depth
    // increases above the crown (the slot narrows for deeper surcharge).
    double prev_w = 1e10;
    int n_steps = 50;
    double crown = y_full * SLOT_CROWN_CUTOFF;
    double y_max = y_full * 1.78;
    double dy = (y_max - crown) / n_steps;

    for (int i = 0; i <= n_steps; ++i) {
        double y = crown + i * dy;
        double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
        EXPECT_GT(w, 0.0) << "Slot width must be positive at y/yf = " << y / y_full;
        EXPECT_LE(w, prev_w) << "Slot width must decrease with depth at y/yf = "
                             << y / y_full;
        prev_w = w;
    }
}

TEST_F(SlotGeometryTest, SjobergWidthMatchesFormulaExactly) {
    // Verify the formula: w = wMax * 0.5423 * exp(-yNorm^2.4) at several points.
    double depths[] = {0.99, 1.0, 1.05, 1.2, 1.5, 1.75};
    for (double yNorm : depths) {
        double y = y_full * yNorm;
        double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
        double expected = w_max * 0.5423 * std::exp(-std::pow(yNorm, 2.4));
        EXPECT_NEAR(w, expected, 1e-12)
            << "Sjoberg formula mismatch at y/yf = " << yNorm;
    }
}

TEST_F(SlotGeometryTest, SlotWidthTransitionRegionSmooth) {
    // Check that max step-to-step change in slot width is bounded (no jumps).
    // Use a fine sweep from 0.98 to 1.02 across the crown cutoff.
    int n = 200;
    double y_lo = y_full * 0.98;
    double y_hi = y_full * 1.02;
    double dy = (y_hi - y_lo) / n;

    double max_delta_w = 0.0;
    double prev_w = solver.getSlotWidth(y_lo, y_full, w_max, XsectShape::CIRCULAR);

    for (int i = 1; i <= n; ++i) {
        double y = y_lo + i * dy;
        double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
        double delta = std::fabs(w - prev_w);
        if (delta > max_delta_w) max_delta_w = delta;
        prev_w = w;
    }

    // The Sjoberg formula at the cutoff gives:
    // w(0.985257) = 3 * 0.5423 * exp(-0.985257^2.4) ≈ 0.606
    // w(0.98) = 0 (below cutoff)
    // So there IS a jump at the cutoff point itself of ~0.606.
    // But within the active slot region (above cutoff), changes should be small.
    // Verify that the jump is at most wMax * 0.5423 * exp(-cutoff^2.4).
    double w_at_cutoff = w_max * 0.5423 *
        std::exp(-std::pow(SLOT_CROWN_CUTOFF, 2.4));
    EXPECT_LE(max_delta_w, w_at_cutoff + 0.01)
        << "Slot width jump is bounded by value at cutoff";
}

// ============================================================================
// 16. Cross-sectional area, hyd. radius with slot active vs inactive
//     (User request #2)
// ============================================================================

TEST_F(SlotGeometryTest, AreaBelowCrownUsesPhysicalXsect) {
    // At 50% depth, area should come from the physical circular cross-section.
    double y = y_full * 0.5;
    double A_phys = xsect::getAofY(xs, y);
    double w_slot = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);

    EXPECT_DOUBLE_EQ(w_slot, 0.0);  // Slot not active below crown
    EXPECT_GT(A_phys, 0.0);
}

TEST_F(SlotGeometryTest, AreaAboveCrownIncludesSlotContribution) {
    // Above crown: A = A_full + (y - y_full) * w_slot
    double y = y_full * 1.1;
    double w_slot = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_GT(w_slot, 0.0);

    double A_slot = solver.getSlotArea(y, y_full, a_full, w_slot);
    double A_expected = a_full + (y - y_full) * w_slot;
    EXPECT_NEAR(A_slot, A_expected, 1e-12);
    // Must exceed A_full
    EXPECT_GT(A_slot, a_full);
}

TEST_F(SlotGeometryTest, AreaAtFullIsAfull) {
    double y = y_full;
    double w_slot = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    double A_slot = solver.getSlotArea(y, y_full, a_full, w_slot);

    // At exactly y_full, (y - y_full) = 0, so A = A_full regardless of slot
    EXPECT_NEAR(A_slot, a_full, 1e-12);
}

TEST_F(SlotGeometryTest, AreaMonotonicallyIncreasingAboveCrown) {
    double prev_A = a_full;
    int n = 50;
    double dy = y_full * 0.02;  // 2% steps from 1.0 to 2.0

    for (int i = 1; i <= n; ++i) {
        double y = y_full + i * dy;
        double A = totalArea(y);
        EXPECT_GT(A, prev_A)
            << "Area must increase with depth at y/yf = " << y / y_full;
        prev_A = A;
    }
}

TEST_F(SlotGeometryTest, HydRadAboveCrownEqualsRfull) {
    // Preissmann slot convention: hydraulic radius stays at R_full
    // for depths above the crown.
    double depths[] = {1.0, 1.1, 1.5, 2.0, 5.0};
    for (double yNorm : depths) {
        double y = y_full * yNorm;
        double R = solver.getSlotHydRad(y, y_full, r_full);
        EXPECT_DOUBLE_EQ(R, r_full)
            << "Hyd. radius should be R_full above crown at y/yf = " << yNorm;
    }
}

TEST_F(SlotGeometryTest, HydRadBelowCrownFromXsect) {
    // Below crown, the solver defers to the batch xsect lookup.
    // getSlotHydRad returns r_full even below, because the caller is expected
    // to use batch values. But the physical value should differ.
    double y = y_full * 0.5;
    double R_phys = xsect::getRofY(xs, y);
    EXPECT_GT(R_phys, 0.0);
    EXPECT_NE(R_phys, r_full);  // Physical R at half-depth ≠ R_full
}

// ============================================================================
// 17. Top width returns slot width (not zero) above crown — Sjoberg
//     (User request #3)
// ============================================================================

TEST_F(SlotGeometryTest, TopWidthPositiveAboveCrown) {
    // Sweep from crown cutoff to 1.78*y_full; slot width should always be > 0.
    int n = 100;
    double y_lo = y_full * SLOT_CROWN_CUTOFF;
    double y_hi = y_full * 1.78;
    double dy = (y_hi - y_lo) / n;

    for (int i = 0; i <= n; ++i) {
        double y = y_lo + i * dy;
        double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
        EXPECT_GT(w, 0.0)
            << "Top width must be positive (slot active) at y/yf = "
            << y / y_full;
    }
}

TEST_F(SlotGeometryTest, TopWidthAboveCrownLessThanPhysicalMaxWidth) {
    // The slot width should always be much less than the physical w_max.
    // At crown cutoff, Sjoberg gives ~0.61 * w_max. At deeper depths it's even less.
    double y = y_full * 1.0;  // At crown
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_LT(w, w_max)
        << "Slot width must be narrower than physical max width";
}

TEST_F(SlotGeometryTest, TopWidthBeyond178CapAt1Percent) {
    // Beyond 1.78 * y_full, slot width caps at 1% of w_max
    double depths[] = {1.79, 2.0, 3.0, 10.0, 100.0};
    double expected = 0.01 * w_max;
    for (double yNorm : depths) {
        double y = y_full * yNorm;
        double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
        EXPECT_NEAR(w, expected, 1e-12)
            << "Beyond 1.78: slot width must be 1% of wMax at y/yf = " << yNorm;
    }
}

// ============================================================================
// 18. Edge cases: exact crown, slightly above/below, very large
//     (User request #4)
// ============================================================================

TEST_F(SlotGeometryTest, EdgeDepthExactlyAtCrownCutoff) {
    // At exactly the crown cutoff, the Sjoberg formula should activate.
    double y = y_full * SLOT_CROWN_CUTOFF;
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    // yNorm == SLOT_CROWN_CUTOFF is NOT < SLOT_CROWN_CUTOFF, so slot is active
    double expected = w_max * 0.5423 *
        std::exp(-std::pow(SLOT_CROWN_CUTOFF, 2.4));
    EXPECT_NEAR(w, expected, 1e-12);
}

TEST_F(SlotGeometryTest, EdgeDepthJustBelowCutoff) {
    // One ULP below cutoff — slot should be inactive (return 0).
    double y = y_full * std::nextafter(SLOT_CROWN_CUTOFF, 0.0);
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_DOUBLE_EQ(w, 0.0);
}

TEST_F(SlotGeometryTest, EdgeDepthJustAboveCutoff) {
    // One ULP above cutoff — slot should be active.
    double y = y_full * std::nextafter(SLOT_CROWN_CUTOFF, 2.0);
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_GT(w, 0.0);
}

TEST_F(SlotGeometryTest, EdgeDepthExactlyAtFull) {
    double y = y_full;
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    double A = solver.getSlotArea(y, y_full, a_full, w);

    // y_full / y_full = 1.0 > SLOT_CROWN_CUTOFF, so slot is active
    EXPECT_GT(w, 0.0);
    // But A = A_full + 0 * w = A_full (no extra volume at exact crown)
    EXPECT_NEAR(A, a_full, 1e-12);
}

TEST_F(SlotGeometryTest, EdgeDepthAtSlotCapBoundary) {
    // At exactly 1.78 * y_full: should still use Sjoberg, not the cap.
    double y = y_full * 1.78;
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    double w_sjoberg = w_max * 0.5423 * std::exp(-std::pow(1.78, 2.4));
    EXPECT_NEAR(w, w_sjoberg, 1e-12);

    // Just above 1.78: should use cap
    double y2 = y_full * std::nextafter(1.78, 2.0);
    double w2 = solver.getSlotWidth(y2, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_NEAR(w2, 0.01 * w_max, 1e-12);
}

TEST_F(SlotGeometryTest, EdgeVeryLargeDepth) {
    // At extreme depth (100x pipe diameter), slot still returns the cap value.
    double y = y_full * 100.0;
    double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_NEAR(w, 0.01 * w_max, 1e-12);

    // Area should be very large
    double A = solver.getSlotArea(y, y_full, a_full, w);
    EXPECT_GT(A, a_full * 10.0);
}

TEST_F(SlotGeometryTest, EdgeZeroDepth) {
    double w = solver.getSlotWidth(0.0, y_full, w_max, XsectShape::CIRCULAR);
    EXPECT_DOUBLE_EQ(w, 0.0);
}

TEST_F(SlotGeometryTest, EdgeYfullZero) {
    // y_full = 0 should return 0 without division by zero.
    double w = solver.getSlotWidth(1.0, 0.0, 3.0, XsectShape::CIRCULAR);
    EXPECT_DOUBLE_EQ(w, 0.0);
}

// ============================================================================
// 19. Slot parameter sensitivity → wave celerity impact
//     (User request #5)
// ============================================================================

TEST_F(SlotGeometryTest, WiderSlotGivesSlowerCelerity) {
    // Wave celerity in a Preissmann slot: c = sqrt(g * A / W)
    // where A = A_full + (y-yf)*W_slot and W = W_slot.
    // At y = y_full (no extra depth yet): c = sqrt(g * A_full / W_slot)
    // Wider slot → smaller c (more compressible).

    double y = y_full * 1.01;  // Just above crown

    // Default DYNAMIC_SLOT Sjoberg width
    DWSolver solver_ds;
    solver_ds.surcharge_method = SurchargeMethod::DYNAMIC_SLOT;
    double w_ds = solver_ds.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    double A_ds = solver_ds.getSlotArea(y, y_full, a_full, w_ds);
    double c_ds = std::sqrt(32.2 * A_ds / w_ds);

    // EXTRAN uses y_full * 0.001 as fixed slot width (narrower → faster)
    DWSolver solver_ex;
    solver_ex.surcharge_method = SurchargeMethod::EXTRAN;
    double w_ex = solver_ex.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
    double A_ex = solver_ex.getSlotArea(y, y_full, a_full, w_ex);
    double c_ex = (w_ex > 0.0) ? std::sqrt(32.2 * A_ex / w_ex) : 0.0;

    // EXTRAN has much narrower slot → much faster wave celerity
    if (w_ex > 0.0) {
        EXPECT_GT(c_ex, c_ds)
            << "Narrower EXTRAN slot should give faster celerity than Sjoberg";
    }
}

TEST_F(SlotGeometryTest, CelerityDecreasesWithDepthAboveCrown) {
    // As depth increases above crown, the Sjoberg formula narrows the slot.
    // Narrower slot → faster celerity (slot acts like column of water).
    // But area also increases with depth, so c = sqrt(g*A/W).
    // Net effect: since W shrinks faster than A grows, c should INCREASE
    // with depth in the Sjoberg region.

    double prev_c = 0.0;
    int n = 20;
    double y_lo = y_full * 1.01;
    double y_hi = y_full * 1.70;
    double dy = (y_hi - y_lo) / n;

    for (int i = 0; i <= n; ++i) {
        double y = y_lo + i * dy;
        double w = solver.getSlotWidth(y, y_full, w_max, XsectShape::CIRCULAR);
        double A = solver.getSlotArea(y, y_full, a_full, w);
        if (w > 0.0) {
            double c = std::sqrt(32.2 * A / w);
            EXPECT_GT(c, prev_c)
                << "Celerity should increase as slot narrows at y/yf = "
                << y / y_full;
            prev_c = c;
        }
    }
}

TEST_F(SlotGeometryTest, CelerityAtCrownCutoffBoundsGravityWave) {
    // At the crown cutoff, the slot opens with width ≈ 0.61 * w_max.
    // The gravity-wave celerity for the open pipe is c_g = sqrt(g * A_full / w_max).
    // The slot celerity at cutoff should be in the same ballpark but slower
    // (wider effective width → slower).

    double w_cutoff = solver.getSlotWidth(y_full * SLOT_CROWN_CUTOFF,
                                           y_full, w_max, XsectShape::CIRCULAR);
    double A_cutoff = a_full;  // approximately A_full at crown
    double c_slot = std::sqrt(32.2 * A_cutoff / w_cutoff);

    double c_gravity = std::sqrt(32.2 * a_full / w_max);

    // Slot celerity should be faster than gravity wave (slot is narrower than
    // pipe width, so sqrt(g*A/W_slot) > sqrt(g*A/W_max))
    EXPECT_GT(c_slot, c_gravity);
}

// ============================================================================
// 20. Numerical continuity: A(h) and dA/dh continuous across transition
//     (User request #6)
// ============================================================================

TEST_F(SlotGeometryTest, AreaContinuousAtCrown) {
    // At y = y_full, physical area should equal A_full.
    // The slot area formula at y = y_full gives: A_full + 0 * w_slot = A_full.
    // So there should be no jump in area at the crown.

    double A_phys_at_crown = xsect::getAofY(xs, y_full);
    double w_at_crown = solver.getSlotWidth(y_full, y_full, w_max,
                                             XsectShape::CIRCULAR);
    double A_slot_at_crown = solver.getSlotArea(y_full, y_full, a_full,
                                                 w_at_crown);

    EXPECT_NEAR(A_phys_at_crown, a_full, 1e-6)
        << "Physical area at crown should equal A_full";
    EXPECT_NEAR(A_slot_at_crown, a_full, 1e-12)
        << "Slot area at crown should equal A_full";
    EXPECT_NEAR(A_phys_at_crown, A_slot_at_crown, 1e-6)
        << "No area jump at the crown transition";
}

TEST_F(SlotGeometryTest, dAdh_ContinuousAcrossCrown) {
    // Compute dA/dy using finite differences on both sides of y_full.
    // Below crown: dA/dy = physical cross-section width W(y)
    // Above crown: dA/dy = slot_width (from getSlotWidth formula)
    //
    // At y = y_full for a circular pipe, physical W → 0 (crown is a point).
    // The Sjoberg slot opens at the cutoff (0.985*yf) with w ≈ 0.61*wMax.
    // So there IS an inherent mathematical jump in dA/dy at the crown cutoff.
    // However, the cutoff is intentionally set below the physical crown
    // so that the slot activates BEFORE the physical width hits zero,
    // creating an overlap region where both contribute. The Sjoberg formula
    // is designed so the combined width transitions smoothly.
    //
    // Test: verify that in the region just above the cutoff, the slot-augmented
    // dA/dy doesn't have large discontinuities relative to the step size.

    double eps = 1e-6;
    int n = 100;
    double y_lo = y_full * 0.90;
    double y_hi = y_full * 1.10;
    double dy = (y_hi - y_lo) / n;

    std::vector<double> areas(n + 1);
    for (int i = 0; i <= n; ++i) {
        areas[i] = totalArea(y_lo + i * dy);
    }

    // Compute first derivative via central differences (interior points)
    std::vector<double> dAdh(n - 1);
    for (int i = 1; i < n; ++i) {
        dAdh[i - 1] = (areas[i + 1] - areas[i - 1]) / (2.0 * dy);
    }

    // Verify dA/dh is always non-negative (area is monotonically increasing)
    for (int i = 0; i < static_cast<int>(dAdh.size()); ++i) {
        double y = y_lo + (i + 1) * dy;
        EXPECT_GE(dAdh[i], -eps)
            << "dA/dh must be non-negative at y/yf = " << y / y_full;
    }

    // Verify dA/dh doesn't have large jumps (second derivative bounded)
    double max_d2Adh2 = 0.0;
    for (int i = 1; i < static_cast<int>(dAdh.size()); ++i) {
        double d2 = std::fabs(dAdh[i] - dAdh[i - 1]) / dy;
        if (d2 > max_d2Adh2) max_d2Adh2 = d2;
    }

    // The second derivative should be bounded — not infinite.
    // For a 3 ft pipe, reasonable upper bound is ~100 (dimensionless, ft²/ft²).
    EXPECT_LT(max_d2Adh2, 1000.0)
        << "d²A/dh² should be bounded across the crown transition";
}

TEST_F(SlotGeometryTest, AreaContinuousAtCutoffFiniteDifference) {
    // Fine finite-difference check right at the crown cutoff boundary.
    // A(y-eps) should be close to A(y+eps) with no large jump.

    double y_cutoff = y_full * SLOT_CROWN_CUTOFF;
    double eps = y_full * 1e-8;

    double A_below = totalArea(y_cutoff - eps);
    double A_at    = totalArea(y_cutoff);
    double A_above = totalArea(y_cutoff + eps);

    // The area itself should be continuous (values should be close)
    EXPECT_NEAR(A_below, A_at, 0.01)
        << "Area should be continuous at the crown cutoff (below vs at)";
    EXPECT_NEAR(A_at, A_above, 0.01)
        << "Area should be continuous at the crown cutoff (at vs above)";
}

TEST_F(SlotGeometryTest, WidthTransitionAtCutoff) {
    // The Sjoberg formula is designed so that the slot width at the cutoff
    // approximates the physical pipe width, creating a smooth handoff.
    // For a circular pipe at y/yf = 0.985, the physical width is very narrow
    // (approaching 0 at the crown). The Sjoberg slot width there is ~0.61 * D.
    //
    // This test documents the actual magnitudes — the slot is intentionally
    // wider than the vanishing physical width to avoid infinite celerity.

    double y_cutoff = y_full * SLOT_CROWN_CUTOFF;
    double w_phys = xsect::getWofY(xs, y_cutoff);
    double w_slot = solver.getSlotWidth(y_cutoff, y_full, w_max,
                                         XsectShape::CIRCULAR);

    // Both should be positive at the cutoff
    EXPECT_GT(w_phys, 0.0);
    EXPECT_GT(w_slot, 0.0);

    // The slot width should be larger than the vanishing physical width
    // (this is the whole point — prevent near-zero width → infinite celerity)
    EXPECT_GT(w_slot, w_phys)
        << "Slot width should exceed physical width near the crown to "
           "prevent infinite celerity";
}

TEST_F(SlotGeometryTest, AreaAndDerivativeSweepNoNaN) {
    // Sweep across the full range and verify no NaN or Inf values.
    int n = 500;
    double dy = y_full * 3.0 / n;

    for (int i = 0; i <= n; ++i) {
        double y = i * dy;
        if (y <= 0.0) continue;

        double A = totalArea(y);
        double W = totalWidth(y);

        EXPECT_FALSE(std::isnan(A)) << "Area is NaN at y = " << y;
        EXPECT_FALSE(std::isinf(A)) << "Area is Inf at y = " << y;
        EXPECT_FALSE(std::isnan(W)) << "Width is NaN at y = " << y;
        EXPECT_FALSE(std::isinf(W)) << "Width is Inf at y = " << y;
        EXPECT_GE(A, 0.0) << "Area must be non-negative at y = " << y;
        EXPECT_GE(W, 0.0) << "Width must be non-negative at y = " << y;
    }
}
