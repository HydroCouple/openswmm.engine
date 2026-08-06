/**
 * @file test_rom_threshold.cpp
 * @brief PR H10 — unit tests for exceedanceFraction() and gapModality().
 *
 * @details Hand-built ensembles, no engine, no ROM: the point of extracting
 *          these as pure functions is that the exact counted fraction and the
 *          gap-statistic thresholds can be pinned against ensembles chosen to
 *          sit precisely on the criteria, which is impractical to arrange
 *          through a live simulation. Engine-level coverage (CSV cadence,
 *          real thresholds) lives in test_engine_1d_rom_lifecycle.cpp.
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <vector>

#include "uncertainty/RomThreshold.hpp"

using openswmm::uncertainty::exceedanceFraction;
using openswmm::uncertainty::gapModality;
using openswmm::uncertainty::ModalityResult;
using openswmm::uncertainty::ThresholdKind;
using openswmm::uncertainty::thresholdKindName;

namespace {

/// Default thresholds from the H10 spec.
constexpr double kGapRatio = 0.5;
constexpr double kMinSide  = 0.20;

/// Two tight clusters of `n_low` + `n_high` members separated by `gap`.
std::vector<double> twoClusters(int n_low, int n_high, double gap,
                                double spread = 0.01) {
    std::vector<double> v;
    for (int i = 0; i < n_low; ++i)
        v.push_back(1.0 + spread * (static_cast<double>(i) / std::max(1, n_low - 1)));
    for (int i = 0; i < n_high; ++i)
        v.push_back(1.0 + gap + spread * (static_cast<double>(i) / std::max(1, n_high - 1)));
    std::sort(v.begin(), v.end());
    return v;
}

/// Evenly spread unimodal ensemble spanning [lo, hi].
std::vector<double> uniformSpread(int n, double lo, double hi) {
    std::vector<double> v(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i)
        v[static_cast<std::size_t>(i)] =
            lo + (hi - lo) * (static_cast<double>(i) / static_cast<double>(n - 1));
    return v;
}

/// Nearest-rank median, matching computeQuantiles()'s convention — used to
/// verify the "q50 lands in the empty gap" claim.
double medianOf(const std::vector<double>& sorted) {
    const int n = static_cast<int>(sorted.size());
    const int idx = static_cast<int>(0.50 * (static_cast<double>(n) - 1.0) + 0.5);
    return sorted[static_cast<std::size_t>(idx)];
}

}  // namespace

// ============================================================================
// exceedanceFraction
// ============================================================================

TEST(ThresholdProbability, ExactCountedFractionStraddlingThreshold) {
    // 10 members at 1..10; threshold 6.5 → members 7,8,9,10 exceed → 0.4.
    const std::vector<double> v = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    EXPECT_DOUBLE_EQ(exceedanceFraction(v.data(), 10, 6.5), 0.4);
    // Threshold 0.5 → all ten exceed.
    EXPECT_DOUBLE_EQ(exceedanceFraction(v.data(), 10, 0.5), 1.0);
    // Threshold 10.5 → none.
    EXPECT_DOUBLE_EQ(exceedanceFraction(v.data(), 10, 10.5), 0.0);
}

TEST(ThresholdProbability, AllBelowIsExactlyZeroAndAllAboveExactlyOne) {
    const std::vector<double> v = uniformSpread(50, 2.0, 3.0);
    EXPECT_EQ(exceedanceFraction(v.data(), 50, 5.0), 0.0);
    EXPECT_EQ(exceedanceFraction(v.data(), 50, 1.0), 1.0);
}

TEST(ThresholdProbability, ExceedanceIsStrictlyAboveNotAtOrAbove) {
    // A member sitting exactly ON the threshold has not crossed it.
    const std::vector<double> v = {1.0, 2.0, 3.0, 4.0};
    EXPECT_DOUBLE_EQ(exceedanceFraction(v.data(), 4, 2.0), 0.5)   // 3,4 exceed
        << "a member exactly at the threshold must not count as exceeding";
    EXPECT_DOUBLE_EQ(exceedanceFraction(v.data(), 4, 4.0), 0.0)
        << "every member at or below the threshold ⇒ exactly 0.0";
}

TEST(ThresholdProbability, IdenticalMembersGiveExactlyZeroOrOne) {
    // The zero-perturbation case: every member identical. There is no
    // intermediate answer — the whole ensemble is on one side.
    const std::vector<double> v(50, 2.5);
    EXPECT_EQ(exceedanceFraction(v.data(), 50, 2.4), 1.0);
    EXPECT_EQ(exceedanceFraction(v.data(), 50, 2.6), 0.0);
    EXPECT_EQ(exceedanceFraction(v.data(), 50, 2.5), 0.0)
        << "exactly at the threshold is not above it";
}

TEST(ThresholdProbability, EmptyOrNullEnsembleIsZeroNotUndefined) {
    const std::vector<double> v = {1.0};
    EXPECT_EQ(exceedanceFraction(nullptr, 0, 1.0), 0.0);
    EXPECT_EQ(exceedanceFraction(v.data(), 0, 1.0), 0.0);
}

// ============================================================================
// gapModality
// ============================================================================

TEST(ThresholdProbability, BimodalEnsembleIsFlaggedAndMedianFallsInTheGap) {
    // THE test that documents why this flag exists: a 50/50 split into two
    // tight clusters. The flag fires, AND the median — the number the sidecar
    // would otherwise report on its own — sits in the empty valley, a value
    // no member actually holds.
    const auto v = twoClusters(25, 25, /*gap=*/1.0);
    const auto r = gapModality(v.data(), static_cast<int>(v.size()),
                               kGapRatio, kMinSide);

    EXPECT_TRUE(r.flagged) << "gap_ratio=" << r.gap_ratio
                           << " minority=" << r.minority_frac;
    EXPECT_NEAR(r.minority_frac, 0.5, 1e-12);

    // The misleading-median regression. Note the sidecar uses NEAREST-RANK
    // quantiles, so q50 is always literally one member's value — it cannot
    // land strictly between the clusters the way an interpolated quantile
    // would. What it does instead is sit on one edge of the gap, and get
    // reported as "the central estimate" while a large share of the ensemble
    // is a full gap away from it. That is the misleading part, and it is what
    // this asserts.
    const double med = medianOf(v);
    const double gap_lo = v[static_cast<std::size_t>(r.gap_index)];
    const double gap_hi = v[static_cast<std::size_t>(r.gap_index + 1)];
    EXPECT_TRUE(med >= gap_lo && med <= gap_hi)
        << "median " << med << " should sit at the gap boundary ["
        << gap_lo << ", " << gap_hi << "]";

    const double half_gap = 0.5 * (gap_hi - gap_lo);
    int n_far = 0;
    for (double x : v) if (std::fabs(x - med) > half_gap) ++n_far;
    EXPECT_GE(n_far, 0.4 * static_cast<double>(v.size()))
        << "at least 40% of members should be more than half a gap away from "
           "the reported median — a q05/q50/q95 summary conveys none of this, "
           "which is exactly why the modality flag exists";
}

TEST(ThresholdProbability, UnimodalEnsembleAtSameSpreadIsNotFlagged) {
    // Same overall spread as the bimodal case above, but evenly filled in —
    // no false positive.
    const auto v = uniformSpread(50, 1.0, 2.01);
    const auto r = gapModality(v.data(), static_cast<int>(v.size()),
                               kGapRatio, kMinSide);
    EXPECT_FALSE(r.flagged) << "gap_ratio=" << r.gap_ratio;
}

TEST(ThresholdProbability, EightyTwentySplitFires) {
    // Minority share exactly at the 20% floor must fire (the criterion is
    // "at least 20%", not "more than").
    const auto v = twoClusters(40, 10, /*gap=*/1.0);
    const auto r = gapModality(v.data(), static_cast<int>(v.size()),
                               kGapRatio, kMinSide);
    EXPECT_NEAR(r.minority_frac, 0.20, 1e-12);
    EXPECT_TRUE(r.flagged)
        << "a minority share exactly at the floor must still flag";
}

TEST(ThresholdProbability, NinetyFiveFiveSplitDoesNotFire) {
    // Same large gap, but only 5% on the minority side: this is an outlier
    // cluster, not a second mode. The minority-fraction floor is what
    // suppresses it — note the gap ratio alone would have fired.
    const auto v = twoClusters(475, 25, /*gap=*/1.0);
    const auto r = gapModality(v.data(), static_cast<int>(v.size()),
                               kGapRatio, kMinSide);
    EXPECT_NEAR(r.minority_frac, 0.05, 1e-12);
    EXPECT_GT(r.gap_ratio, kGapRatio)
        << "sanity: the gap itself is large — only the minority floor should "
           "be suppressing this";
    EXPECT_FALSE(r.flagged);
}

TEST(ThresholdProbability, SingleOutlierDoesNotCountAsASecondMode) {
    // One stray member far away: enormous gap ratio, 1/50 minority share.
    auto v = uniformSpread(49, 1.0, 2.0);
    v.push_back(20.0);
    std::sort(v.begin(), v.end());
    const auto r = gapModality(v.data(), static_cast<int>(v.size()),
                               kGapRatio, kMinSide);
    EXPECT_GT(r.gap_ratio, 10.0) << "sanity: the outlier makes a huge gap";
    EXPECT_FALSE(r.flagged)
        << "a lone outlier must not be reported as a second mode";
}

TEST(ThresholdProbability, ZeroSpreadEnsembleIsNeverFlagged) {
    // Zero-perturbation run: every member identical. IQR is degenerate, so
    // the ratio has no meaningful denominator and there are no modes to
    // separate.
    const std::vector<double> v(50, 3.25);
    const auto r = gapModality(v.data(), 50, kGapRatio, kMinSide);
    EXPECT_FALSE(r.flagged);
    EXPECT_EQ(r.gap_ratio, 0.0);
}

TEST(ThresholdProbability, TooFewMembersIsNotFlagged) {
    const std::vector<double> v = {1.0, 1.0, 5.0};
    const auto r = gapModality(v.data(), 3, kGapRatio, kMinSide);
    EXPECT_FALSE(r.flagged) << "below 4 members the quartile span is meaningless";
}

TEST(ThresholdProbability, ThresholdsAreConfigurableNotHardCoded) {
    // The 95/5 split that the default floor suppresses must fire once the
    // floor is lowered — proving the criterion is the config field and not a
    // baked-in constant.
    const auto v = twoClusters(475, 25, /*gap=*/1.0);
    EXPECT_FALSE(gapModality(v.data(), static_cast<int>(v.size()),
                             kGapRatio, 0.20).flagged);
    EXPECT_TRUE(gapModality(v.data(), static_cast<int>(v.size()),
                            kGapRatio, 0.04).flagged);

    // Likewise the gap ratio. A two-cluster fixture is no good here: for a
    // 50/50 split the IQR is itself ≈ the gap, so the ratio is ≈ 1 and always
    // fires. Instead take an otherwise-continuous ensemble and open a MODEST
    // step in the middle — gap small relative to the IQR, so the default 0.5
    // suppresses it while a loosened threshold catches it.
    auto w = uniformSpread(50, 1.0, 2.0);
    for (int i = 25; i < 50; ++i) w[static_cast<std::size_t>(i)] += 0.03;
    const auto strict = gapModality(w.data(), static_cast<int>(w.size()), 0.5, kMinSide);
    const auto loose  = gapModality(w.data(), static_cast<int>(w.size()), 0.05, kMinSide);
    EXPECT_FALSE(strict.flagged) << "gap_ratio=" << strict.gap_ratio;
    EXPECT_TRUE(loose.flagged)   << "gap_ratio=" << loose.gap_ratio;
}

// ============================================================================
// ThresholdKind naming (CSV stability)
// ============================================================================

TEST(ThresholdProbability, ThresholdKindNamesAreStable) {
    EXPECT_STREQ(thresholdKindName(ThresholdKind::CROWN),     "CROWN");
    EXPECT_STREQ(thresholdKindName(ThresholdKind::MAX_DEPTH), "MAX_DEPTH");
    EXPECT_STREQ(thresholdKindName(ThresholdKind::NONE),      "NONE");
}
