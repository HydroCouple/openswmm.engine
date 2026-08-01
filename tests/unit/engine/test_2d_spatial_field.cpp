/**
 * @file test_2d_spatial_field.cpp
 * @brief Unit tests for SpatialUncertaintyField and CorrelatedFieldGenerator.
 */

#ifdef OPENSWMM_HAS_2D

#include "2d/uncertainty/SpatialUncertaintyField.hpp"
#include "2d/uncertainty/CorrelatedFieldGenerator.hpp"

#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

using namespace openswmm::twoD;

// ============================================================================
// Helpers
// ============================================================================

namespace {

// Build a regular n×n grid of centroids in [0, L] × [0, L].
void makeGrid(int n, double L, std::vector<double>& cx, std::vector<double>& cy) {
    const double dx = L / (n - 1);
    cx.reserve(static_cast<std::size_t>(n * n));
    cy.reserve(static_cast<std::size_t>(n * n));
    for (int i = 0; i < n; ++i)
        for (int j = 0; j < n; ++j) {
            cx.push_back(i * dx);
            cy.push_back(j * dx);
        }
}

// Compute mean of a vector.
double mean(const std::vector<double>& v) {
    return std::accumulate(v.begin(), v.end(), 0.0) / v.size();
}

// Compute std dev of a vector.
double stddev(const std::vector<double>& v) {
    const double m = mean(v);
    double var = 0.0;
    for (double x : v) var += (x - m) * (x - m);
    return std::sqrt(var / v.size());
}

} // namespace

// ============================================================================
// SpatialUncertaintyField — basic API
// ============================================================================

TEST(SpatialUncertaintyField, DefaultIsNotSpatial) {
    SpatialUncertaintyField f;
    EXPECT_FALSE(f.is_spatial());
    EXPECT_EQ(f.n_members, 0);
    EXPECT_EQ(f.n_cells, 0);
}

TEST(SpatialUncertaintyField, AllocateFillsOnes) {
    SpatialUncertaintyField f;
    f.allocate(3, 5);
    EXPECT_TRUE(f.is_spatial());
    EXPECT_EQ(f.n_members, 3);
    EXPECT_EQ(f.n_cells, 5);
    for (int i = 0; i < 3; ++i)
        for (int t = 0; t < 5; ++t)
            EXPECT_DOUBLE_EQ(f.at(i, t), 1.0);
}

TEST(SpatialUncertaintyField, AtReadWrite) {
    SpatialUncertaintyField f;
    f.allocate(2, 4);
    f.at(0, 2) = 1.35;
    f.at(1, 3) = 0.75;
    EXPECT_DOUBLE_EQ(f.at(0, 2), 1.35);
    EXPECT_DOUBLE_EQ(f.at(1, 3), 0.75);
    EXPECT_DOUBLE_EQ(f.at(0, 0), 1.0);  // untouched
}

TEST(SpatialUncertaintyField, FromScalarExpandsUniformly) {
    SpatialUncertaintyField f;
    const std::vector<double> sc = {0.8, 1.0, 1.2};
    f.fromScalar(sc, 10);
    EXPECT_TRUE(f.is_spatial());
    EXPECT_EQ(f.n_members, 3);
    EXPECT_EQ(f.n_cells, 10);
    for (int i = 0; i < 3; ++i)
        for (int t = 0; t < 10; ++t)
            EXPECT_DOUBLE_EQ(f.at(i, t), sc[static_cast<std::size_t>(i)]);
}

TEST(SpatialUncertaintyField, ClearRevertToScalar) {
    SpatialUncertaintyField f;
    f.allocate(4, 6);
    EXPECT_TRUE(f.is_spatial());
    f.clear();
    EXPECT_FALSE(f.is_spatial());
    EXPECT_EQ(f.n_members, 0);
    EXPECT_EQ(f.n_cells, 0);
}

// ============================================================================
// CorrelatedFieldGenerator — scalar mode (corr_len == 0)
// ============================================================================

TEST(CorrelatedFieldGenerator, ScalarModeReturnsScalarMult) {
    std::vector<double> cx = {0.0, 1.0, 2.0};
    std::vector<double> cy = {0.0, 0.0, 0.0};
    const std::vector<double> sc = {0.85, 1.00, 1.15};
    const double pert = 0.20;

    SpatialUncertaintyField out;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), 3,
                                       sc, pert, /*corr_len=*/0.0, 42, out);
    EXPECT_TRUE(out.is_spatial());
    for (int i = 0; i < 3; ++i)
        for (int t = 0; t < 3; ++t)
            EXPECT_DOUBLE_EQ(out.at(i, t), sc[static_cast<std::size_t>(i)]);
}

TEST(CorrelatedFieldGenerator, ZeroPertReturnsScalarMult) {
    std::vector<double> cx = {0.0, 5.0, 10.0};
    std::vector<double> cy = {0.0, 5.0, 10.0};
    const std::vector<double> sc = {0.9, 1.1};

    SpatialUncertaintyField out;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), 3,
                                       sc, /*pert=*/0.0, /*corr_len=*/2.0, 7, out);
    for (int i = 0; i < 2; ++i)
        for (int t = 0; t < 3; ++t)
            EXPECT_DOUBLE_EQ(out.at(i, t), sc[static_cast<std::size_t>(i)]);
}

// ============================================================================
// CorrelatedFieldGenerator — spatial mode
// ============================================================================

TEST(CorrelatedFieldGenerator, SpatialValuesInValidRange) {
    // 10×10 grid, corr_len = 3 m, L = 20 m.
    std::vector<double> cx, cy;
    makeGrid(10, 20.0, cx, cy);
    const int n_tri = static_cast<int>(cx.size());
    const int M = 20;
    std::vector<double> sc(static_cast<std::size_t>(M));
    const double pert = 0.25;
    // Linear LHS: sc[i] = 1 - pert + (2*pert*(i+0.5)/M)
    for (int i = 0; i < M; ++i)
        sc[static_cast<std::size_t>(i)] = 1.0 - pert + (2.0 * pert * (i + 0.5) / M);

    SpatialUncertaintyField out;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, pert, /*corr_len=*/3.0, 42, out);

    EXPECT_EQ(out.n_members, M);
    EXPECT_EQ(out.n_cells, n_tri);

    for (int i = 0; i < M; ++i)
        for (int t = 0; t < n_tri; ++t) {
            EXPECT_GT(out.at(i, t), 0.0) << "i=" << i << " t=" << t;
            EXPECT_LT(out.at(i, t), 2.0) << "i=" << i << " t=" << t;
        }
}

TEST(CorrelatedFieldGenerator, SpatialVarianceExistsWithCorrelation) {
    // corr_len small relative to domain → clear spatial variation.
    std::vector<double> cx, cy;
    makeGrid(8, 16.0, cx, cy);
    const int n_tri = static_cast<int>(cx.size());
    const std::vector<double> sc(20, 1.0);  // all at 1.0 globally
    const double pert = 0.20;

    SpatialUncertaintyField out;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, pert, /*corr_len=*/1.5, 99, out);

    // For each member, check spatial std dev > 0.
    for (int i = 0; i < 20; ++i) {
        std::vector<double> row(static_cast<std::size_t>(n_tri));
        for (int t = 0; t < n_tri; ++t)
            row[static_cast<std::size_t>(t)] = out.at(i, t);
        EXPECT_GT(stddev(row), 0.01) << "member " << i << " has no spatial variation";
    }
}

TEST(CorrelatedFieldGenerator, LargeCorrelationLengthIsNearlyUniform) {
    // corr_len >> domain size → smoothed field is nearly constant per member.
    std::vector<double> cx, cy;
    makeGrid(5, 10.0, cx, cy);
    const int n_tri = static_cast<int>(cx.size());
    const double pert = 0.20;
    std::vector<double> sc = {0.85, 1.05, 1.15};

    SpatialUncertaintyField out;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, pert, /*corr_len=*/1000.0, 1, out);

    for (int i = 0; i < 3; ++i) {
        std::vector<double> row(static_cast<std::size_t>(n_tri));
        for (int t = 0; t < n_tri; ++t)
            row[static_cast<std::size_t>(t)] = out.at(i, t);
        // Spatial std dev should be much smaller than pert when corr_len >> domain.
        EXPECT_LT(stddev(row), 0.05)
            << "member " << i << " has unexpectedly high spatial variation";
    }
}

TEST(CorrelatedFieldGenerator, ReproducibleFromSameSeed) {
    std::vector<double> cx, cy;
    makeGrid(6, 12.0, cx, cy);
    const int n_tri = static_cast<int>(cx.size());
    const std::vector<double> sc(10, 1.0);

    SpatialUncertaintyField out1, out2;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, 0.20, 2.0, /*seed=*/12345, out1);
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, 0.20, 2.0, /*seed=*/12345, out2);

    ASSERT_EQ(out1.values.size(), out2.values.size());
    for (std::size_t k = 0; k < out1.values.size(); ++k)
        EXPECT_DOUBLE_EQ(out1.values[k], out2.values[k]) << "k=" << k;
}

TEST(CorrelatedFieldGenerator, DifferentSeedsProduceDifferentFields) {
    std::vector<double> cx, cy;
    makeGrid(5, 10.0, cx, cy);
    const int n_tri = static_cast<int>(cx.size());
    const std::vector<double> sc(5, 1.0);

    SpatialUncertaintyField out1, out2;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, 0.20, 2.0, /*seed=*/1, out1);
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, 0.20, 2.0, /*seed=*/2, out2);

    int n_different = 0;
    for (std::size_t k = 0; k < out1.values.size(); ++k)
        if (out1.values[k] != out2.values[k]) ++n_different;
    EXPECT_GT(n_different, 0) << "different seeds produced identical fields";
}

TEST(CorrelatedFieldGenerator, GlobalLevelCentresEachMember) {
    // Each member's mean across cells should be close to its scalar_mult level.
    std::vector<double> cx, cy;
    makeGrid(8, 16.0, cx, cy);
    const int n_tri = static_cast<int>(cx.size());
    const double pert = 0.15;
    const int M = 10;
    std::vector<double> sc(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        sc[static_cast<std::size_t>(i)] = 1.0 - pert + (2.0 * pert * (i + 0.5) / M);

    SpatialUncertaintyField out;
    CorrelatedFieldGenerator::generate(cx.data(), cy.data(), n_tri,
                                       sc, pert, /*corr_len=*/2.0, 77, out);

    for (int i = 0; i < M; ++i) {
        std::vector<double> row(static_cast<std::size_t>(n_tri));
        for (int t = 0; t < n_tri; ++t)
            row[static_cast<std::size_t>(t)] = out.at(i, t);
        const double row_mean = mean(row);
        // Mean of the spatial field should be within ±pert of the global level.
        EXPECT_NEAR(row_mean, sc[static_cast<std::size_t>(i)], pert)
            << "member " << i;
    }
}

#endif // OPENSWMM_HAS_2D
