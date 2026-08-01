/**
 * @file test_grid_mapping_weights.cpp
 * @brief Unit tests for SR-4a BILINEAR + AREA_MEAN grid mapping weights.
 *
 * @details Analytic cases from SOFT_RAINFALL_PR_CHECKLIST.md SR-4a:
 *          - square catchment exactly covering 4 pixels ⇒ weights 0.25 each
 *          - catchment inside one pixel ⇒ weight 1.0
 *          - weights row-sum to 1
 *          - BILINEAR at pixel center == CENTROID value
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include "uncertainty/GridMappingWeights.hpp"

#include <cstdint>
#include <numeric>
#include <vector>

using namespace openswmm::uncertainty;

namespace {

// A 4x4 regular grid with unit spacing, centers at 0,1,2,3.
std::vector<double> unit_axis(int n) {
    std::vector<double> a(static_cast<std::size_t>(n));
    for (int i = 0; i < n; ++i) a[static_cast<std::size_t>(i)] = static_cast<double>(i);
    return a;
}

// ---------------------------------------------------------------------------
// BILINEAR
// ---------------------------------------------------------------------------

TEST(BilinearWeights, AtPixelCenterMatchesCentroid) {
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);

    // Query exactly at pixel center (2, 1) → ix=2, iy=1 → flat index 1*4+2 = 6.
    std::uint32_t idx[4];
    double w[4];
    ASSERT_TRUE(bilinearWeights(2.0, 1.0, xc, yc, idx, w));

    // The weight on the containing pixel must be 1, all others 0.
    double wsum = w[0] + w[1] + w[2] + w[3];
    EXPECT_DOUBLE_EQ(wsum, 1.0);

    // Weighted index recovers the centroid pixel (6).
    double eff = 0.0;
    for (int k = 0; k < 4; ++k) eff += w[k] * idx[k];
    EXPECT_DOUBLE_EQ(eff, 6.0);
    // Exactly one weight is 1.0.
    int ones = 0;
    for (int k = 0; k < 4; ++k) if (w[k] == 1.0) ++ones;
    EXPECT_EQ(ones, 1);
}

TEST(BilinearWeights, MidpointBetweenTwoPixelsIsHalfHalf) {
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);

    // Query at (2.5, 1.0): halfway between ix=2 and ix=3 on the y=1 row.
    std::uint32_t idx[4];
    double w[4];
    ASSERT_TRUE(bilinearWeights(2.5, 1.0, xc, yc, idx, w));

    // tx=0.5, ty=0 → w = [0.5, 0.5, 0, 0] on pixels (2,1) and (3,1).
    EXPECT_DOUBLE_EQ(w[0], 0.5);
    EXPECT_DOUBLE_EQ(w[1], 0.5);
    EXPECT_DOUBLE_EQ(w[2], 0.0);
    EXPECT_DOUBLE_EQ(w[3], 0.0);
    EXPECT_EQ(idx[0], 1u * 4 + 2);
    EXPECT_EQ(idx[1], 1u * 4 + 3);
}

TEST(BilinearWeights, CellCenterQuarterQuarterQuarterQuarter) {
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);

    // Query at the exact center of the 4 pixels (2.5, 1.5) → all weights 0.25.
    std::uint32_t idx[4];
    double w[4];
    ASSERT_TRUE(bilinearWeights(2.5, 1.5, xc, yc, idx, w));
    for (int k = 0; k < 4; ++k) EXPECT_DOUBLE_EQ(w[k], 0.25);
    EXPECT_DOUBLE_EQ(w[0] + w[1] + w[2] + w[3], 1.0);
}

TEST(BilinearWeights, ClampsOutsideGridToEdge) {
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);

    // Query far outside (top-right corner). Clamped: full weight on pixel (3,3).
    std::uint32_t idx[4];
    double w[4];
    ASSERT_TRUE(bilinearWeights(100.0, 100.0, xc, yc, idx, w));
    EXPECT_DOUBLE_EQ(w[3], 1.0);   // (ix0+1, iy0+1) = (3,3)
    EXPECT_EQ(idx[3], 3u * 4 + 3);
}

TEST(BilinearWeights, DegenerateGridReturnsFalse) {
    std::vector<double> xc = {0.0};   // nx = 1
    std::vector<double> yc = unit_axis(4);
    std::uint32_t idx[4];
    double w[4];
    EXPECT_FALSE(bilinearWeights(0.0, 1.0, xc, yc, idx, w));
}

// ---------------------------------------------------------------------------
// AREA_MEAN — polygon∩pixel weights
// ---------------------------------------------------------------------------

TEST(AreaMeanWeights, SquareCoveringFourPixelsIsQuarterEach) {
    // 2x2 grid, centers at 0 and 1, so pixel bounds are [-0.5,0.5] and [0.5,1.5].
    std::vector<double> xc = {0.0, 1.0};
    std::vector<double> yc = {0.0, 1.0};

    // Square [0,1]x[0,1] straddles all 4 pixels equally: each pixel gets the
    // 0.5x0.5 quadrant of the square → area 0.25 → weight 0.25.
    std::vector<double> px = {0.0, 1.0, 1.0, 0.0};
    std::vector<double> py = {0.0, 0.0, 1.0, 1.0};

    std::vector<std::uint32_t> opx;
    std::vector<float> ow;
    polygonPixelWeights(px, py, xc, yc, opx, ow);

    ASSERT_EQ(opx.size(), 4u);
    double sum = 0.0;
    for (float w : ow) { EXPECT_NEAR(w, 0.25f, 1e-6); sum += w; }
    EXPECT_NEAR(sum, 1.0, 1e-6);
}

TEST(AreaMeanWeights, PolygonInsideOnePixelIsWeightOne) {
    // 4x4 unit grid; a small square fully inside pixel (2,1) = [1.5,2.5]x[0.5,1.5].
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);
    std::vector<double> px = {1.8, 2.2, 2.2, 1.8};
    std::vector<double> py = {0.8, 0.8, 1.2, 1.2};

    std::vector<std::uint32_t> opx;
    std::vector<float> ow;
    polygonPixelWeights(px, py, xc, yc, opx, ow);

    ASSERT_EQ(opx.size(), 1u);
    EXPECT_EQ(opx[0], 1u * 4 + 2);   // iy=1, ix=2
    EXPECT_NEAR(ow[0], 1.0f, 1e-6);
}

TEST(AreaMeanWeights, WeightsRowSumToOne) {
    // An arbitrary quadrilateral spanning several pixels.
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);
    std::vector<double> px = {0.3, 2.7, 3.1, 0.6};
    std::vector<double> py = {0.4, 0.2, 2.5, 2.9};

    std::vector<std::uint32_t> opx;
    std::vector<float> ow;
    polygonPixelWeights(px, py, xc, yc, opx, ow);

    ASSERT_FALSE(opx.empty());
    double sum = std::accumulate(ow.begin(), ow.end(), 0.0,
                                 [](double a, float b){ return a + b; });
    EXPECT_NEAR(sum, 1.0, 1e-5);
}

TEST(AreaMeanWeights, DegeneratePolygonReturnsEmpty) {
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);
    // Only two vertices — not a polygon.
    std::vector<double> px = {0.0, 1.0};
    std::vector<double> py = {0.0, 1.0};

    std::vector<std::uint32_t> opx;
    std::vector<float> ow;
    polygonPixelWeights(px, py, xc, yc, opx, ow);
    EXPECT_TRUE(opx.empty());
    EXPECT_TRUE(ow.empty());
}

TEST(AreaMeanWeights, ZeroAreaPolygonReturnsEmpty) {
    auto xc = unit_axis(4);
    auto yc = unit_axis(4);
    // Collinear points → zero area.
    std::vector<double> px = {0.0, 1.0, 2.0};
    std::vector<double> py = {0.0, 1.0, 2.0};

    std::vector<std::uint32_t> opx;
    std::vector<float> ow;
    polygonPixelWeights(px, py, xc, yc, opx, ow);
    EXPECT_TRUE(opx.empty());
}

TEST(AreaMeanWeights, AreaWeightedIsProportionalToOverlap) {
    // 2x2 grid, centers 0 and 1 → pixel bounds [-0.5,0.5] and [0.5,1.5].
    std::vector<double> xc = {0.0, 1.0};
    std::vector<double> yc = {0.0, 1.0};

    // Rectangle [0,1]x[0,0.5]: spans the bottom row only, x split evenly.
    // Bottom-left pixel (0,0): x in [0,0.5], y in [0,0.5] → 0.5*0.5 = 0.25
    // Bottom-right pixel (0,1): x in [0.5,1], y in [0,0.5] → 0.25
    // Top row: y in [0.5,...] not covered by the rectangle (y max 0.5).
    std::vector<double> px = {0.0, 1.0, 1.0, 0.0};
    std::vector<double> py = {0.0, 0.0, 0.5, 0.5};

    std::vector<std::uint32_t> opx;
    std::vector<float> ow;
    polygonPixelWeights(px, py, xc, yc, opx, ow);

    ASSERT_EQ(opx.size(), 2u);   // only the bottom row
    double sum = 0.0;
    for (float w : ow) { EXPECT_NEAR(w, 0.5f, 1e-6); sum += w; }
    EXPECT_NEAR(sum, 1.0, 1e-6);
    EXPECT_EQ(opx[0], 0u);       // (iy=0, ix=0)
    EXPECT_EQ(opx[1], 1u);       // (iy=0, ix=1)
}

} // anonymous namespace
