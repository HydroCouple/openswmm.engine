/**
 * @file test_rom_diag_trust.cpp
 * @brief PR H3 — unit tests for the pure fr_trust/surcharge_frac functions.
 *
 * @details Hand-built fixtures, independent of the real hydraulics: these
 *          pin the exact closed-form weighted mean and the clamp behavior
 *          that would be impractical to force through a live DYNWAVE solve
 *          (Froude is derived from real momentum physics, not directly
 *          settable). Engine-level integration coverage (still water,
 *          CSV row cadence) lives in test_engine_1d_rom_lifecycle.cpp.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <vector>

#include "uncertainty/RomDiagTrust.hpp"

using openswmm::uncertainty::computeFrTrust;
using openswmm::uncertainty::computeSurchargeFrac;

// ============================================================================
// computeFrTrust
// ============================================================================

TEST(RomDiagTrust, StillWaterGivesExactZero) {
    // Zero flow everywhere -> zero weighted denominator -> exactly 0.0, not NaN.
    const std::vector<double> flow(5, 0.0);
    const std::vector<double> froude(5, 0.0);
    EXPECT_EQ(computeFrTrust(flow.data(), froude.data(), nullptr,
                            static_cast<int>(flow.size())),
              0.0);
}

TEST(RomDiagTrust, TwoConduitClosedFormMatchesTo1e12) {
    // Hand-built: Q = [2.0, 3.0], Fr = [0.4, 0.6].
    // fr_trust = (2*0.4^2 + 3*0.6^2) / (2+3) = (0.32 + 1.08) / 5 = 0.28
    const std::vector<double> flow   = {2.0, 3.0};
    const std::vector<double> froude = {0.4, 0.6};
    const double expected = (2.0 * 0.4 * 0.4 + 3.0 * 0.6 * 0.6) / 5.0;
    const double actual = computeFrTrust(flow.data(), froude.data(), nullptr, 2);
    EXPECT_NEAR(actual, expected, 1e-12);
    EXPECT_NEAR(actual, 0.28, 1e-12);
}

TEST(RomDiagTrust, NegativeFlowContributesByMagnitude) {
    // Flow direction must not matter -- only |Q| weights the sum.
    const std::vector<double> flow_pos = {2.0, 3.0};
    const std::vector<double> flow_neg = {-2.0, -3.0};
    const std::vector<double> froude   = {0.4, 0.6};
    EXPECT_EQ(computeFrTrust(flow_pos.data(), froude.data(), nullptr, 2),
              computeFrTrust(flow_neg.data(), froude.data(), nullptr, 2));
}

TEST(RomDiagTrust, FroudeClampedAtUpperBound) {
    // A supercritical trickle (Fr=5.0 on a near-zero flow) must clamp to 1.5
    // before squaring, not blow up the weighted mean.
    const std::vector<double> flow   = {100.0, 1e-6};
    const std::vector<double> froude = {0.0,   5.0};
    const double actual = computeFrTrust(flow.data(), froude.data(), nullptr, 2);
    // Clamped contribution: 1e-6 * 1.5^2 = 2.25e-6, over a denominator of
    // ~100 -> a vanishingly small trust value, NOT dominated by Fr=5.
    EXPECT_LT(actual, 1e-6);
    EXPECT_GE(actual, 0.0);
}

TEST(RomDiagTrust, FroudeClampedAtLowerBound) {
    // A negative Froude (shouldn't occur physically, but the clamp must be
    // defensive) clamps to 0, not squared-negative or left as-is.
    const std::vector<double> flow   = {1.0};
    const std::vector<double> froude = {-2.0};
    EXPECT_EQ(computeFrTrust(flow.data(), froude.data(), nullptr, 1), 0.0);
}

TEST(RomDiagTrust, IneligibleLinksExcluded) {
    // Three links; only link 1 (middle) is eligible. The result must equal
    // the single-link closed form, NOT include links 0/2's contribution.
    const std::vector<double>  flow      = {10.0, 2.0, 10.0};
    const std::vector<double>  froude    = {0.9,  0.4, 0.9};
    const std::vector<uint8_t> eligible  = {0,    1,   0};
    const double actual = computeFrTrust(flow.data(), froude.data(),
                                          eligible.data(), 3);
    EXPECT_NEAR(actual, 0.4 * 0.4, 1e-12);  // Fr^2 of the sole eligible link
}

TEST(RomDiagTrust, AllIneligibleGivesExactZero) {
    const std::vector<double>  flow     = {10.0, 20.0};
    const std::vector<double>  froude   = {0.9,  0.9};
    const std::vector<uint8_t> eligible = {0,    0};
    EXPECT_EQ(computeFrTrust(flow.data(), froude.data(), eligible.data(), 2), 0.0);
}

// ============================================================================
// computeSurchargeFrac
// ============================================================================

TEST(RomDiagTrust, SurchargeFracMatchesHandSetFlagPattern) {
    // Full node space: 6 nodes, only nodes 1 and 4 surcharged.
    const std::vector<uint8_t> node_surcharged = {0, 1, 0, 0, 1, 0};
    // Active nodes are a subset (excludes outfall at full-index 5).
    const std::vector<int> active_full_idx = {0, 1, 2, 3, 4};
    const double frac = computeSurchargeFrac(
        node_surcharged.data(), static_cast<int>(node_surcharged.size()),
        active_full_idx.data(), static_cast<int>(active_full_idx.size()));
    EXPECT_NEAR(frac, 2.0 / 5.0, 1e-12);
}

TEST(RomDiagTrust, SurchargeFracZeroWhenNoneSurcharged) {
    const std::vector<uint8_t> node_surcharged = {0, 0, 0, 0};
    const std::vector<int> active_full_idx = {0, 1, 2, 3};
    EXPECT_EQ(computeSurchargeFrac(node_surcharged.data(), 4,
                                   active_full_idx.data(), 4),
              0.0);
}

TEST(RomDiagTrust, SurchargeFracOneWhenAllSurcharged) {
    const std::vector<uint8_t> node_surcharged = {1, 1, 1};
    const std::vector<int> active_full_idx = {0, 1, 2};
    EXPECT_EQ(computeSurchargeFrac(node_surcharged.data(), 3,
                                   active_full_idx.data(), 3),
              1.0);
}

TEST(RomDiagTrust, SurchargeFracZeroWhenNoActiveNodes) {
    const std::vector<uint8_t> node_surcharged = {1, 1};
    EXPECT_EQ(computeSurchargeFrac(node_surcharged.data(), 2, nullptr, 0), 0.0);
}

TEST(RomDiagTrust, SurchargeFracIgnoresOutOfRangeFullIndex) {
    // Defensive: an out-of-range full index (shouldn't happen with a correct
    // active map) must not read out of bounds or count spuriously.
    const std::vector<uint8_t> node_surcharged = {1, 0};
    const std::vector<int> active_full_idx = {0, 99};
    const double frac = computeSurchargeFrac(node_surcharged.data(), 2,
                                              active_full_idx.data(), 2);
    EXPECT_NEAR(frac, 0.5, 1e-12);  // only index 0 counted (surcharged)
}
