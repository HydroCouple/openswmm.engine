/**
 * @file test_simd_math.cpp
 * @brief Unit tests for SIMD-accelerated math routines.
 *
 * @details Tests the SIMD abstraction layer which wraps:
 *          - AVX2 intrinsics on x86_64
 *          - ARM NEON on arm64/aarch64
 *          - Scalar fallback on other platforms
 *
 *          Key operations tested:
 *          - Vectorized array addition (used in runoff accumulation)
 *          - Vectorized dot product (used in quality routing)
 *          - Vectorized min/max (used in timestep computation)
 *          - Vectorized clamp (used in depth/flow bounding)
 *          - Vectorized sqrt, fabs, fma
 *
 * @see src/engine/math/SIMD.hpp
 * @ingroup engine_math
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <numeric>
#include <algorithm>
#include <random>
#include <limits>

#include "math/SIMD.hpp"

namespace {

/**
 * @brief Test fixture that generates random test vectors.
 */
class SimdMathTest : public ::testing::Test {
protected:
    static constexpr int N = 1024;
    std::vector<double> a, b, expected, result;

    void SetUp() override {
        a.resize(N);
        b.resize(N);
        expected.resize(N);
        result.resize(N);
        std::mt19937_64 rng(42);
        std::uniform_real_distribution<double> dist(0.0, 100.0);
        for (int i = 0; i < N; i++) {
            a[i] = dist(rng);
            b[i] = dist(rng);
        }
    }
};

TEST_F(SimdMathTest, VectorizedAddMatchesScalar) {
    for (int i = 0; i < N; i++) expected[i] = a[i] + b[i];
    openswmm::simd::add(a.data(), b.data(), result.data(), N);
    for (int i = 0; i < N; i++) EXPECT_DOUBLE_EQ(result[i], expected[i]);
}

TEST_F(SimdMathTest, VectorizedMultiplyMatchesScalar) {
    for (int i = 0; i < N; i++) expected[i] = a[i] * b[i];
    openswmm::simd::multiply(a.data(), b.data(), result.data(), N);
    for (int i = 0; i < N; i++) EXPECT_DOUBLE_EQ(result[i], expected[i]);
}

TEST_F(SimdMathTest, VectorizedMinMatchesStdMin) {
    double std_min = *std::min_element(a.begin(), a.end());
    double simd_min = openswmm::simd::min(a.data(), N);
    EXPECT_DOUBLE_EQ(simd_min, std_min);
}

TEST_F(SimdMathTest, VectorizedMinSingleElement) {
    double val = 42.0;
    EXPECT_DOUBLE_EQ(openswmm::simd::min(&val, 1), 42.0);
}

TEST_F(SimdMathTest, VectorizedMaxMatchesStdMax) {
    double std_max = *std::max_element(a.begin(), a.end());
    double simd_max = openswmm::simd::max(a.data(), N);
    EXPECT_DOUBLE_EQ(simd_max, std_max);
}

TEST_F(SimdMathTest, VectorizedMaxSingleElement) {
    double val = 42.0;
    EXPECT_DOUBLE_EQ(openswmm::simd::max(&val, 1), 42.0);
}

TEST_F(SimdMathTest, VectorizedClamp) {
    std::vector<double> data(a);
    openswmm::simd::clamp(data.data(), 10.0, 50.0, N);
    for (int i = 0; i < N; i++) {
        EXPECT_GE(data[i], 10.0);
        EXPECT_LE(data[i], 50.0);
        double expected_val = std::max(10.0, std::min(50.0, a[i]));
        EXPECT_DOUBLE_EQ(data[i], expected_val);
    }
}

TEST_F(SimdMathTest, VectorizedDotProduct) {
    // Compute reference with Kahan compensation
    double ref = 0.0;
    double comp = 0.0;
    for (int i = 0; i < N; i++) {
        double y = a[i] * b[i] - comp;
        double t = ref + y;
        comp = (t - ref) - y;
        ref = t;
    }
    double simd_dot = openswmm::simd::dot(a.data(), b.data(), N);
    // Allow small relative error due to different accumulation order
    EXPECT_NEAR(simd_dot, ref, std::fabs(ref) * 1e-12);
}

TEST_F(SimdMathTest, VectorizedSqrtArray) {
    openswmm::simd::sqrt_array(a.data(), result.data(), N);
    for (int i = 0; i < N; i++) {
        EXPECT_DOUBLE_EQ(result[i], std::sqrt(a[i]));
    }
}

TEST_F(SimdMathTest, VectorizedFabsArray) {
    // Mix in some negative values
    for (int i = 0; i < N; i += 2) a[i] = -a[i];
    openswmm::simd::fabs_array(a.data(), result.data(), N);
    for (int i = 0; i < N; i++) {
        EXPECT_DOUBLE_EQ(result[i], std::fabs(a[i]));
    }
}

TEST_F(SimdMathTest, VectorizedFmaArray) {
    std::vector<double> c(N);
    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> dist(0.0, 50.0);
    for (int i = 0; i < N; i++) c[i] = dist(rng);

    openswmm::simd::fma_array(a.data(), b.data(), c.data(), result.data(), N);
    for (int i = 0; i < N; i++) {
        EXPECT_DOUBLE_EQ(result[i], a[i] * b[i] + c[i]);
    }
}

TEST_F(SimdMathTest, NonMultipleOfLaneWidthHandledCorrectly) {
    // Test with N = 13 (not a multiple of any SIMD lane width)
    constexpr int M = 13;
    std::vector<double> sa(M), sb(M), sr(M);
    for (int i = 0; i < M; i++) { sa[i] = i + 1.0; sb[i] = 2.0; }

    openswmm::simd::add(sa.data(), sb.data(), sr.data(), M);
    for (int i = 0; i < M; i++) EXPECT_DOUBLE_EQ(sr[i], sa[i] + sb[i]);

    openswmm::simd::multiply(sa.data(), sb.data(), sr.data(), M);
    for (int i = 0; i < M; i++) EXPECT_DOUBLE_EQ(sr[i], sa[i] * sb[i]);

    double min_val = openswmm::simd::min(sa.data(), M);
    EXPECT_DOUBLE_EQ(min_val, 1.0);

    double max_val = openswmm::simd::max(sa.data(), M);
    EXPECT_DOUBLE_EQ(max_val, 13.0);

    double dot_val = openswmm::simd::dot(sa.data(), sb.data(), M);
    double ref_dot = 0.0;
    for (int i = 0; i < M; i++) ref_dot += sa[i] * sb[i];
    EXPECT_DOUBLE_EQ(dot_val, ref_dot);
}

TEST_F(SimdMathTest, UnalignedBuffersHandledCorrectly) {
    // Offset by 1 double (8 bytes) to test unaligned access
    std::vector<double> buf(N + 1);
    for (int i = 0; i < N; i++) buf[i + 1] = a[i];

    double* unaligned = buf.data() + 1;
    std::vector<double> res(N);

    openswmm::simd::add(unaligned, b.data(), res.data(), N);
    for (int i = 0; i < N; i++) EXPECT_DOUBLE_EQ(res[i], a[i] + b[i]);
}

TEST_F(SimdMathTest, LaneWidthIsPositive) {
    EXPECT_GE(openswmm::simd::lane_width(), 1u);
}

TEST_F(SimdMathTest, FastMathPow4_3) {
    for (int i = 1; i < 100; i++) {
        double x = static_cast<double>(i);
        double fast = openswmm::fastmath::pow4_3(x);
        double ref = std::pow(x, 4.0 / 3.0);
        EXPECT_NEAR(fast, ref, std::fabs(ref) * 1e-14);
    }
}

TEST_F(SimdMathTest, FastMathPow2_3) {
    for (int i = 1; i < 100; i++) {
        double x = static_cast<double>(i);
        double fast = openswmm::fastmath::pow2_3(x);
        double ref = std::pow(x, 2.0 / 3.0);
        EXPECT_NEAR(fast, ref, std::fabs(ref) * 1e-14);
    }
}

} /* anonymous namespace */
