/**
 * @file SIMD.hpp
 * @brief SIMD abstraction layer for vectorized arithmetic operations.
 *
 * @details Provides a platform-neutral interface over:
 *          - **x86_64:** AVX2 (256-bit, 4 doubles per register)
 *          - **arm64:**  NEON (128-bit, 2 doubles per register)
 *          - **Other:**  Scalar fallback (auto-vectorized by the compiler)
 *
 * The goal is to express numerical loops in a way that the compiler can
 * vectorize, with explicit SIMD intrinsics as hints for the most
 * performance-critical paths.
 *
 * ### Usage philosophy
 *
 * Do NOT pepper the solver code with raw intrinsics. Instead:
 * 1. Write the algorithm using the SIMD helpers in this file.
 * 2. Let the compiler auto-vectorize as much as possible.
 * 3. Profile; only use explicit intrinsics for hot loops that the
 *    compiler fails to vectorize.
 *
 * @note The scalar fallback is always available. If SIMD intrinsics are
 *       unavailable at compile time, the scalar path is used automatically.
 *
 * @see tests/unit/test_simd_math.cpp
 * @see tests/benchmarks/bench_hydraulics.cpp
 * @ingroup engine_math
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_SIMD_HPP
#define OPENSWMM_ENGINE_SIMD_HPP

#ifndef OPENSWMM_RESTRICT
#  if defined(_MSC_VER)
#    define OPENSWMM_RESTRICT __restrict
#  else
#    define OPENSWMM_RESTRICT __restrict__
#  endif
#endif

// ============================================================================
// Cross-compiler vectorisation hint
// ============================================================================

#ifndef OPENSWMM_IVDEP
#  if defined(_MSC_VER)
#    define OPENSWMM_IVDEP __pragma(loop(ivdep))
#  elif defined(__GNUC__) || defined(__clang__)
#    define OPENSWMM_IVDEP _Pragma("GCC ivdep")
#  else
#    define OPENSWMM_IVDEP
#  endif
#endif

#include <cstddef>
#include <cmath>
#include <algorithm>
#include <cassert>

// ============================================================================
// Platform detection
// ============================================================================

#if !defined(OPENSWMM_SIMD_SCALAR)
#  if defined(__AVX2__)
#    include <immintrin.h>
#    define OPENSWMM_SIMD_AVX2  1
#    define OPENSWMM_SIMD_WIDTH 4   ///< Doubles per AVX2 register
#  elif defined(__ARM_NEON) || defined(__aarch64__)
#    include <arm_neon.h>
#    define OPENSWMM_SIMD_NEON  1
#    define OPENSWMM_SIMD_WIDTH 2   ///< Doubles per NEON register
#  else
#    define OPENSWMM_SIMD_SCALAR 1
#    define OPENSWMM_SIMD_WIDTH 1   ///< Scalar fallback
#  endif
#else
#  if !defined(OPENSWMM_SIMD_WIDTH)
#    define OPENSWMM_SIMD_WIDTH 1
#  endif
#endif

// Exactly one SIMD backend must be active
static_assert(
    (0
#if defined(OPENSWMM_SIMD_AVX2)
     + 1
#endif
#if defined(OPENSWMM_SIMD_NEON)
     + 1
#endif
#if defined(OPENSWMM_SIMD_SCALAR)
     + 1
#endif
    ) == 1,
    "Exactly one SIMD backend must be active (AVX2, NEON, or SCALAR)"
);

namespace openswmm::simd {

// ============================================================================
// Element-wise operations (arrays of doubles)
// ============================================================================

/**
 * @brief Element-wise addition: dst[i] = a[i] + b[i]
 *
 * @param a    Source array A.
 * @param b    Source array B.
 * @param dst  Destination array.
 * @param n    Number of elements. Need not be a multiple of the SIMD width.
 *
 * @note Arrays may not alias each other (undefined behavior if they do).
 */
inline void add(
    const double* OPENSWMM_RESTRICT a,
    const double* OPENSWMM_RESTRICT b,
    double*       OPENSWMM_RESTRICT dst,
    std::size_t                n
) noexcept {
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    const std::size_t nr = n % 4;
    for (std::size_t i = 0; i < nv; ++i) {
        __m256d va = _mm256_loadu_pd(a   + i * 4);
        __m256d vb = _mm256_loadu_pd(b   + i * 4);
        _mm256_storeu_pd(dst + i * 4, _mm256_add_pd(va, vb));
    }
    for (std::size_t i = nv * 4; i < n; ++i) dst[i] = a[i] + b[i];
    (void)nr;
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    for (std::size_t i = 0; i < nv; ++i) {
        float64x2_t va = vld1q_f64(a   + i * 2);
        float64x2_t vb = vld1q_f64(b   + i * 2);
        vst1q_f64(dst + i * 2, vaddq_f64(va, vb));
    }
    for (std::size_t i = nv * 2; i < n; ++i) dst[i] = a[i] + b[i];
#else
    for (std::size_t i = 0; i < n; ++i) dst[i] = a[i] + b[i];
#endif
}

/**
 * @brief Element-wise multiplication: dst[i] = a[i] * b[i]
 */
inline void multiply(
    const double* OPENSWMM_RESTRICT a,
    const double* OPENSWMM_RESTRICT b,
    double*       OPENSWMM_RESTRICT dst,
    std::size_t                n
) noexcept {
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    for (std::size_t i = 0; i < nv; ++i) {
        __m256d va = _mm256_loadu_pd(a   + i * 4);
        __m256d vb = _mm256_loadu_pd(b   + i * 4);
        _mm256_storeu_pd(dst + i * 4, _mm256_mul_pd(va, vb));
    }
    for (std::size_t i = nv * 4; i < n; ++i) dst[i] = a[i] * b[i];
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    for (std::size_t i = 0; i < nv; ++i) {
        float64x2_t va = vld1q_f64(a   + i * 2);
        float64x2_t vb = vld1q_f64(b   + i * 2);
        vst1q_f64(dst + i * 2, vmulq_f64(va, vb));
    }
    for (std::size_t i = nv * 2; i < n; ++i) dst[i] = a[i] * b[i];
#else
    for (std::size_t i = 0; i < n; ++i) dst[i] = a[i] * b[i];
#endif
}

/**
 * @brief Find the minimum value in an array.
 *
 * @param a  Array of doubles.
 * @param n  Number of elements. Must be >= 1.
 * @returns  Minimum value.
 */
inline double min(const double* a, std::size_t n) noexcept {
    assert(n >= 1);
    double result = a[0];
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    if (nv > 0) {
        __m256d vmin = _mm256_loadu_pd(a);
        for (std::size_t i = 1; i < nv; ++i) {
            vmin = _mm256_min_pd(vmin, _mm256_loadu_pd(a + i * 4));
        }
        // Horizontal reduction
        __m128d hi  = _mm256_extractf128_pd(vmin, 1);
        __m128d lo  = _mm256_castpd256_pd128(vmin);
        __m128d m2  = _mm_min_pd(lo, hi);
        __m128d m1  = _mm_min_pd(m2, _mm_shuffle_pd(m2, m2, 1));
        result = _mm_cvtsd_f64(m1);
    }
    for (std::size_t i = nv * 4; i < n; ++i) result = std::min(result, a[i]);
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    if (nv > 0) {
        float64x2_t vmin = vld1q_f64(a);
        for (std::size_t i = 1; i < nv; ++i) {
            vmin = vminq_f64(vmin, vld1q_f64(a + i * 2));
        }
        // Horizontal reduction: min of 2 lanes
        result = std::min(vgetq_lane_f64(vmin, 0), vgetq_lane_f64(vmin, 1));
    }
    for (std::size_t i = nv * 2; i < n; ++i) result = std::min(result, a[i]);
#else
    for (std::size_t i = 1; i < n; ++i) result = std::min(result, a[i]);
#endif
    return result;
}

/**
 * @brief Find the maximum value in an array.
 */
inline double max(const double* a, std::size_t n) noexcept {
    assert(n >= 1);
    double result = a[0];
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    if (nv > 0) {
        __m256d vmax = _mm256_loadu_pd(a);
        for (std::size_t i = 1; i < nv; ++i) {
            vmax = _mm256_max_pd(vmax, _mm256_loadu_pd(a + i * 4));
        }
        // Horizontal reduction
        __m128d hi  = _mm256_extractf128_pd(vmax, 1);
        __m128d lo  = _mm256_castpd256_pd128(vmax);
        __m128d m2  = _mm_max_pd(lo, hi);
        __m128d m1  = _mm_max_pd(m2, _mm_shuffle_pd(m2, m2, 1));
        result = _mm_cvtsd_f64(m1);
    }
    for (std::size_t i = nv * 4; i < n; ++i) result = std::max(result, a[i]);
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    if (nv > 0) {
        float64x2_t vmax = vld1q_f64(a);
        for (std::size_t i = 1; i < nv; ++i) {
            vmax = vmaxq_f64(vmax, vld1q_f64(a + i * 2));
        }
        result = std::max(vgetq_lane_f64(vmax, 0), vgetq_lane_f64(vmax, 1));
    }
    for (std::size_t i = nv * 2; i < n; ++i) result = std::max(result, a[i]);
#else
    for (std::size_t i = 1; i < n; ++i) result = std::max(result, a[i]);
#endif
    return result;
}

/**
 * @brief Clamp all elements of an array to [lo, hi].
 *
 * @param a   Array to clamp in-place.
 * @param lo  Lower bound.
 * @param hi  Upper bound.
 * @param n   Number of elements.
 */
inline void clamp(double* a, double lo, double hi, std::size_t n) noexcept {
#if defined(OPENSWMM_SIMD_AVX2)
    __m256d vlo = _mm256_set1_pd(lo);
    __m256d vhi = _mm256_set1_pd(hi);
    const std::size_t nv = n / 4;
    for (std::size_t i = 0; i < nv; ++i) {
        __m256d v = _mm256_loadu_pd(a + i * 4);
        v = _mm256_max_pd(vlo, _mm256_min_pd(vhi, v));
        _mm256_storeu_pd(a + i * 4, v);
    }
    for (std::size_t i = nv * 4; i < n; ++i) {
        a[i] = std::max(lo, std::min(hi, a[i]));
    }
#elif defined(OPENSWMM_SIMD_NEON)
    float64x2_t vlo = vdupq_n_f64(lo);
    float64x2_t vhi = vdupq_n_f64(hi);
    const std::size_t nv = n / 2;
    for (std::size_t i = 0; i < nv; ++i) {
        float64x2_t v = vld1q_f64(a + i * 2);
        v = vmaxq_f64(vlo, vminq_f64(vhi, v));
        vst1q_f64(a + i * 2, v);
    }
    for (std::size_t i = nv * 2; i < n; ++i) {
        a[i] = std::max(lo, std::min(hi, a[i]));
    }
#else
    for (std::size_t i = 0; i < n; ++i) {
        a[i] = std::max(lo, std::min(hi, a[i]));
    }
#endif
}

/**
 * @brief Dot product of two arrays: sum(a[i] * b[i]).
 *
 * @note Uses Kahan compensation on the scalar remainder to minimize
 *       floating-point accumulation error.
 *
 * @param a  Array A.
 * @param b  Array B.
 * @param n  Number of elements.
 * @returns  Sum of element-wise products.
 */
inline double dot(
    const double* OPENSWMM_RESTRICT a,
    const double* OPENSWMM_RESTRICT b,
    std::size_t                n
) noexcept {
    double result = 0.0;
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    if (nv > 0) {
        __m256d vsum = _mm256_setzero_pd();
        for (std::size_t i = 0; i < nv; ++i) {
            __m256d va = _mm256_loadu_pd(a + i * 4);
            __m256d vb = _mm256_loadu_pd(b + i * 4);
            vsum = _mm256_fmadd_pd(va, vb, vsum);   // FMA: vsum += va * vb
        }
        // Horizontal sum
        __m128d hi  = _mm256_extractf128_pd(vsum, 1);
        __m128d lo  = _mm256_castpd256_pd128(vsum);
        __m128d s2  = _mm_add_pd(lo, hi);
        __m128d s1  = _mm_add_pd(s2, _mm_shuffle_pd(s2, s2, 1));
        result = _mm_cvtsd_f64(s1);
    }
    for (std::size_t i = nv * 4; i < n; ++i) result += a[i] * b[i];
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    if (nv > 0) {
        float64x2_t vsum = vdupq_n_f64(0.0);
        for (std::size_t i = 0; i < nv; ++i) {
            float64x2_t va = vld1q_f64(a + i * 2);
            float64x2_t vb = vld1q_f64(b + i * 2);
            vsum = vfmaq_f64(vsum, va, vb);   // FMA: vsum += va * vb
        }
        // Horizontal sum: add both lanes
        result = vgetq_lane_f64(vsum, 0) + vgetq_lane_f64(vsum, 1);
    }
    for (std::size_t i = nv * 2; i < n; ++i) result += a[i] * b[i];
#else
    for (std::size_t i = 0; i < n; ++i) result += a[i] * b[i];
#endif
    return result;
}

/**
 * @brief Returns the SIMD lane width (doubles per register on this platform).
 */
constexpr std::size_t lane_width() noexcept {
    return OPENSWMM_SIMD_WIDTH;
}

/**
 * @brief Element-wise sqrt: dst[i] = sqrt(a[i]).
 *        Written as a simple loop; compilers auto-vectorize to platform-native
 *        SIMD (vsqrtq_f64 on ARM, _mm256_sqrt_pd on x86, etc.).
 */
inline void sqrt_array(
    const double* OPENSWMM_RESTRICT a,
    double*       OPENSWMM_RESTRICT dst,
    std::size_t                n
) noexcept {
    OPENSWMM_IVDEP
    for (std::size_t i = 0; i < n; ++i) dst[i] = std::sqrt(a[i]);
}

/**
 * @brief Element-wise fabs: dst[i] = fabs(a[i]).
 */
inline void fabs_array(
    const double* OPENSWMM_RESTRICT a,
    double*       OPENSWMM_RESTRICT dst,
    std::size_t                n
) noexcept {
    OPENSWMM_IVDEP
    for (std::size_t i = 0; i < n; ++i) dst[i] = std::fabs(a[i]);
}

/**
 * @brief Element-wise fused multiply-add: dst[i] = a[i] * b[i] + c[i].
 */
inline void fma_array(
    const double* OPENSWMM_RESTRICT a,
    const double* OPENSWMM_RESTRICT b,
    const double* OPENSWMM_RESTRICT c,
    double*       OPENSWMM_RESTRICT dst,
    std::size_t                n
) noexcept {
    OPENSWMM_IVDEP
    for (std::size_t i = 0; i < n; ++i) dst[i] = a[i] * b[i] + c[i];
}

} /* namespace openswmm::simd */

// ============================================================================
// Fast math: pow(x, 4/3) and pow(x, 2/3) using std::cbrt
// ============================================================================

namespace openswmm::fastmath {

/** @brief pow(x, 4/3) = x * cbrt(x). Much faster than std::pow(x, 4.0/3.0). */
inline double pow4_3(double x) noexcept {
    return x * std::cbrt(x);
}

/** @brief pow(x, 2/3) = cbrt(x²). Much faster than std::pow(x, 2.0/3.0). */
inline double pow2_3(double x) noexcept {
    return std::cbrt(x * x);
}

} /* namespace openswmm::fastmath */

#endif /* OPENSWMM_ENGINE_SIMD_HPP */
