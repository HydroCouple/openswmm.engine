/**
 * @file test_rom_surcharge_attenuation.cpp
 * @brief PR H5 — unit tests for the pure computeSurchargeAlpha() function.
 *
 * @details Hand-built fixtures, independent of the real hydraulics: these
 *          pin the exact ramp formula and its degenerate/guard behavior.
 *          Engine-level plumbing coverage (crown_elev/invert_elev/head
 *          reaching the function through the real engine) lives in
 *          test_engine_1d_rom_lifecycle.cpp; the effect on the ROM's
 *          projected sensitivity lives in the SurchargeAttenuation suite of
 *          test_spectral_rom1d.cpp.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <vector>

#include "uncertainty/RomSurchargeAttenuation.hpp"

using openswmm::uncertainty::computeSurchargeAlpha;
using openswmm::uncertainty::SurchargeAttenuationConfig;

namespace {

// Single-node convenience wrapper: builds full-node-space arrays for one
// active node at full index 0, with default ramp band, and returns alpha[0].
double alphaFor(double crown_elev, double invert_elev, double head,
                 SurchargeAttenuationConfig cfg = {}) {
    const std::vector<double> crown  = {crown_elev};
    const std::vector<double> invert = {invert_elev};
    const std::vector<double> heads  = {head};
    const std::vector<int> active_to_full = {0};
    std::vector<double> alpha(1, -1.0);
    computeSurchargeAlpha(1, active_to_full.data(), crown.data(),
                           invert.data(), heads.data(), 1, cfg,
                           alpha.data());
    return alpha[0];
}

}  // namespace

TEST(RomSurchargeAttenuation, WellBelowRampLoIsExactlyOne) {
    // crown_depth = 2.0, depth = 0.5*crown_depth = 1.0 -> ratio 0.5 << 0.9.
    EXPECT_EQ(alphaFor(/*crown=*/2.0, /*invert=*/0.0, /*head=*/1.0), 1.0);
}

TEST(RomSurchargeAttenuation, WellAboveRampHiReachesFloorNotZero) {
    // crown_depth = 2.0, depth = 2*crown_depth = 4.0 -> ratio 2.0 >> 1.1.
    // Deep surcharge floors at alpha_floor (default 0.05), not exactly 0 --
    // a hard 0 claims Manning's n has NO effect once pressurized, which a
    // brute-force MC validation showed collapses the band far below the MC's
    // own (much smaller, but real) spread. See the config field's doc
    // comment and VALIDATION.md, PR H5.
    EXPECT_NEAR(alphaFor(/*crown=*/2.0, /*invert=*/0.0, /*head=*/4.0), 0.05,
                1e-12);
}

TEST(RomSurchargeAttenuation, ExactlyAtRampLoIsOne) {
    // crown_depth = 1.0, depth = 0.9 -> ratio exactly 0.9 == default ramp_lo.
    EXPECT_EQ(alphaFor(/*crown=*/1.0, /*invert=*/0.0, /*head=*/0.9), 1.0);
}

TEST(RomSurchargeAttenuation, ExactlyAtRampHiReachesFloor) {
    // crown_depth = 1.0, depth = 1.1 -> ratio exactly 1.1 == default ramp_hi.
    EXPECT_NEAR(alphaFor(/*crown=*/1.0, /*invert=*/0.0, /*head=*/1.1), 0.05,
                1e-12);
}

TEST(RomSurchargeAttenuation, MidpointRatioGivesExactlyHalf) {
    // crown_depth = 1.0, depth = 1.0 -> ratio exactly 1.0, the midpoint of
    // the default [0.9, 1.1] band: alpha = (1.1-1.0)/0.2 = 0.5.
    EXPECT_NEAR(alphaFor(/*crown=*/1.0, /*invert=*/0.0, /*head=*/1.0), 0.5,
                1e-12);
}

TEST(RomSurchargeAttenuation, DegenerateCrownDepthFailsOpenToOne) {
    // crown_elev <= invert_elev (outfall / no classifiable crown): must not
    // attenuate what can't be classified, regardless of how large head is.
    EXPECT_EQ(alphaFor(/*crown=*/0.0, /*invert=*/0.0, /*head=*/50.0), 1.0);
    EXPECT_EQ(alphaFor(/*crown=*/1.0, /*invert=*/2.0, /*head=*/50.0), 1.0);
}

TEST(RomSurchargeAttenuation, OutOfRangeActiveIndexFailsOpenToOne) {
    // A full-node index outside [0, n_nodes_full) must not read out of
    // bounds and must fail open (alpha = 1.0), same convention as
    // RomDiagTrust's out-of-range defensiveness.
    const std::vector<double> crown  = {2.0};
    const std::vector<double> invert = {0.0};
    const std::vector<double> heads  = {10.0};  // would be ratio 5.0 if read
    const std::vector<int> active_to_full = {99};
    std::vector<double> alpha(1, -1.0);
    computeSurchargeAlpha(1, active_to_full.data(), crown.data(),
                           invert.data(), heads.data(), 1,
                           SurchargeAttenuationConfig{}, alpha.data());
    EXPECT_EQ(alpha[0], 1.0);
}

TEST(RomSurchargeAttenuation, CustomRampBandChangesOutput) {
    // Same physical state (ratio = 1.0) under two different configured
    // bands must give two different results -- proves the formula reads
    // cfg.ramp_lo/ramp_hi, not hardcoded 0.9/1.1.
    SurchargeAttenuationConfig narrow{/*ramp_lo=*/0.95, /*ramp_hi=*/1.05};
    SurchargeAttenuationConfig wide{/*ramp_lo=*/0.5, /*ramp_hi=*/1.5};
    const double a_narrow = alphaFor(1.0, 0.0, 1.0, narrow);
    const double a_wide   = alphaFor(1.0, 0.0, 1.0, wide);
    EXPECT_NEAR(a_narrow, 0.5, 1e-12);  // still the midpoint of its own band
    EXPECT_NEAR(a_wide, 0.5, 1e-12);
    // Push ratio to 1.1: narrow band has already reached its floor, wide
    // band's ramp has not (still strictly above the floor).
    const double a_narrow_at_1p1 = alphaFor(1.0, 0.0, 1.1, narrow);
    const double a_wide_at_1p1   = alphaFor(1.0, 0.0, 1.1, wide);
    EXPECT_NEAR(a_narrow_at_1p1, narrow.alpha_floor, 1e-12);
    EXPECT_GT(a_wide_at_1p1, wide.alpha_floor);
}

TEST(RomSurchargeAttenuation, FloorIsConfigurable) {
    // Deep surcharge (ratio >> ramp_hi) with a custom, non-default floor
    // must return exactly that floor, not the hardcoded 0.05 default --
    // proves the formula reads cfg.alpha_floor.
    SurchargeAttenuationConfig cfg;
    cfg.alpha_floor = 0.20;
    EXPECT_NEAR(alphaFor(/*crown=*/2.0, /*invert=*/0.0, /*head=*/4.0, cfg),
                0.20, 1e-12);
}

TEST(RomSurchargeAttenuation, ZeroFloorReproducesOriginalHardZeroBehavior) {
    // A caller that explicitly wants the checklist's literal candidate (ramp
    // all the way to 0) can still get it -- the floor is additive, not a
    // behavior removal.
    SurchargeAttenuationConfig cfg;
    cfg.alpha_floor = 0.0;
    EXPECT_EQ(alphaFor(/*crown=*/2.0, /*invert=*/0.0, /*head=*/4.0, cfg), 0.0);
}

TEST(RomSurchargeAttenuation, MultipleNodesAreIndependent) {
    // Three active nodes in one call, mixed regimes: free-surface,
    // surcharged, and mid-ramp. Each slot must reflect only its own node's
    // state -- no cross-contamination between array slots.
    const std::vector<double> crown  = {2.0, 2.0, 2.0};
    const std::vector<double> invert = {0.0, 0.0, 0.0};
    // ratios: 0.5 (free surface), 2.0 (surcharged), 1.0 (midpoint).
    const std::vector<double> heads  = {1.0, 4.0, 2.0};
    const std::vector<int> active_to_full = {0, 1, 2};
    std::vector<double> alpha(3, -1.0);
    computeSurchargeAlpha(3, active_to_full.data(), crown.data(),
                           invert.data(), heads.data(), 3,
                           SurchargeAttenuationConfig{}, alpha.data());
    EXPECT_EQ(alpha[0], 1.0);
    EXPECT_NEAR(alpha[1], 0.05, 1e-12);  // deep surcharge -> the floor, not 0
    EXPECT_NEAR(alpha[2], 0.5, 1e-12);
}
