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
 * @defgroup engine_math Engine numerical utilities
 * @brief Numerical expression evaluation, root finding, ordinary differential equations and portable vector operations used by engine solvers.
 * @ingroup new_engine
 */

/**
 * @file      SIMD.hpp
 * @brief     SIMD abstraction layer for vectorized arithmetic operations.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_SIMD_HPP
#define OPENSWMM_ENGINE_SIMD_HPP

#ifndef OPENSWMM_RESTRICT
#if defined(_MSC_VER)
#define OPENSWMM_RESTRICT __restrict
#else
#define OPENSWMM_RESTRICT __restrict__
#endif
#endif

// ============================================================================
// Cross-compiler vectorisation hint
// ============================================================================

#ifndef OPENSWMM_IVDEP
#if defined(_MSC_VER)
#define OPENSWMM_IVDEP __pragma(loop(ivdep))
#elif defined(__GNUC__) || defined(__clang__)
#define OPENSWMM_IVDEP _Pragma("GCC ivdep")
#else
#define OPENSWMM_IVDEP
#endif
#endif

#include <cstddef>
#include <cmath>
#include <algorithm>
#include <cassert>

// ============================================================================
// Platform detection
// ============================================================================

#if !defined(OPENSWMM_SIMD_SCALAR)
#if defined(__AVX2__)
#include <immintrin.h>
#define OPENSWMM_SIMD_AVX2 1
#define OPENSWMM_SIMD_WIDTH 4 ///< Doubles per AVX2 register
#elif defined(__ARM_NEON) || defined(__aarch64__)
#include <arm_neon.h>
#define OPENSWMM_SIMD_NEON 1
#define OPENSWMM_SIMD_WIDTH 2 ///< Doubles per NEON register
#else
#define OPENSWMM_SIMD_SCALAR 1
#define OPENSWMM_SIMD_WIDTH 1 ///< Scalar fallback
#endif
#else
#if !defined(OPENSWMM_SIMD_WIDTH)
#define OPENSWMM_SIMD_WIDTH 1
#endif
#endif

// Exactly one SIMD backend must be active
static_assert((0
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
              "Exactly one SIMD backend must be active (AVX2, NEON, or SCALAR)");

namespace openswmm::simd {

// ============================================================================
// Element-wise operations (arrays of doubles)
// ============================================================================

/**
 * @brief Compute dst[i] = a[i] + b[i] over n doubles.
 * @param[in] a Borrowed readable array of at least n elements.
 * @param[in] b Borrowed readable array of at least n elements.
 * @param[out] dst Borrowed writable array of at least n elements.
 * @param[in] n Element count; any size, including zero, is supported.
 * @pre Source and destination arrays do not overlap. For n > 0 all pointers
 * are non-null. No SIMD alignment is required.
 * @note No storage is retained. A scalar tail handles partial SIMD lanes.
 */
inline void add(const double* OPENSWMM_RESTRICT a, const double* OPENSWMM_RESTRICT b,
                double* OPENSWMM_RESTRICT dst, std::size_t n) noexcept {
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    const std::size_t nr = n % 4;
    for (std::size_t i = 0; i < nv; ++i) {
        __m256d va = _mm256_loadu_pd(a + i * 4);
        __m256d vb = _mm256_loadu_pd(b + i * 4);
        _mm256_storeu_pd(dst + i * 4, _mm256_add_pd(va, vb));
    }
    for (std::size_t i = nv * 4; i < n; ++i)
        dst[i] = a[i] + b[i];
    (void)nr;
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    for (std::size_t i = 0; i < nv; ++i) {
        float64x2_t va = vld1q_f64(a + i * 2);
        float64x2_t vb = vld1q_f64(b + i * 2);
        vst1q_f64(dst + i * 2, vaddq_f64(va, vb));
    }
    for (std::size_t i = nv * 2; i < n; ++i)
        dst[i] = a[i] + b[i];
#else
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] + b[i];
#endif
}

/**
 * @brief Compute dst[i] = a[i] * b[i] over n doubles.
 * @param[in] a Borrowed readable array of at least n elements.
 * @param[in] b Borrowed readable array of at least n elements.
 * @param[out] dst Borrowed writable array of at least n elements.
 * @param[in] n Element count; any size, including zero, is supported.
 * @pre Source and destination arrays do not overlap. For n > 0 all pointers
 * are non-null. No SIMD alignment is required.
 * @note No storage is retained. A scalar tail handles partial SIMD lanes.
 */
inline void multiply(const double* OPENSWMM_RESTRICT a, const double* OPENSWMM_RESTRICT b,
                     double* OPENSWMM_RESTRICT dst, std::size_t n) noexcept {
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    for (std::size_t i = 0; i < nv; ++i) {
        __m256d va = _mm256_loadu_pd(a + i * 4);
        __m256d vb = _mm256_loadu_pd(b + i * 4);
        _mm256_storeu_pd(dst + i * 4, _mm256_mul_pd(va, vb));
    }
    for (std::size_t i = nv * 4; i < n; ++i)
        dst[i] = a[i] * b[i];
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    for (std::size_t i = 0; i < nv; ++i) {
        float64x2_t va = vld1q_f64(a + i * 2);
        float64x2_t vb = vld1q_f64(b + i * 2);
        vst1q_f64(dst + i * 2, vmulq_f64(va, vb));
    }
    for (std::size_t i = nv * 2; i < n; ++i)
        dst[i] = a[i] * b[i];
#else
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] * b[i];
#endif
}

/**
 * @brief Reduce an array to its minimum value.
 * @param[in] a Non-null borrowed array of at least n readable doubles.
 * @param[in] n Element count, at least one; checked by assert in debug builds.
 * @return Minimum element for finite inputs. NaN behavior follows the selected
 * SIMD intrinsics or scalar comparisons and may differ between backends.
 * @note No SIMD alignment is required and no storage is retained.
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
        __m128d hi = _mm256_extractf128_pd(vmin, 1);
        __m128d lo = _mm256_castpd256_pd128(vmin);
        __m128d m2 = _mm_min_pd(lo, hi);
        __m128d m1 = _mm_min_pd(m2, _mm_shuffle_pd(m2, m2, 1));
        result = _mm_cvtsd_f64(m1);
    }
    for (std::size_t i = nv * 4; i < n; ++i)
        result = std::min(result, a[i]);
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
    for (std::size_t i = nv * 2; i < n; ++i)
        result = std::min(result, a[i]);
#else
    for (std::size_t i = 1; i < n; ++i)
        result = std::min(result, a[i]);
#endif
    return result;
}

/**
 * @brief Reduce an array to its maximum value.
 * @param[in] a Non-null borrowed array of at least n readable doubles.
 * @param[in] n Element count, at least one; checked by assert in debug builds.
 * @return Maximum element for finite inputs. NaN behavior follows the selected
 * SIMD intrinsics or scalar comparisons and may differ between backends.
 * @note No SIMD alignment is required and no storage is retained.
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
        __m128d hi = _mm256_extractf128_pd(vmax, 1);
        __m128d lo = _mm256_castpd256_pd128(vmax);
        __m128d m2 = _mm_max_pd(lo, hi);
        __m128d m1 = _mm_max_pd(m2, _mm_shuffle_pd(m2, m2, 1));
        result = _mm_cvtsd_f64(m1);
    }
    for (std::size_t i = nv * 4; i < n; ++i)
        result = std::max(result, a[i]);
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    if (nv > 0) {
        float64x2_t vmax = vld1q_f64(a);
        for (std::size_t i = 1; i < nv; ++i) {
            vmax = vmaxq_f64(vmax, vld1q_f64(a + i * 2));
        }
        result = std::max(vgetq_lane_f64(vmax, 0), vgetq_lane_f64(vmax, 1));
    }
    for (std::size_t i = nv * 2; i < n; ++i)
        result = std::max(result, a[i]);
#else
    for (std::size_t i = 1; i < n; ++i)
        result = std::max(result, a[i]);
#endif
    return result;
}

/**
 * @brief Clamp each array element in place to an inclusive interval.
 * @param[in,out] a Borrowed writable array of at least n doubles; non-null
 * when n > 0. No alignment requirement applies.
 * @param[in] lo Lower bound, in the same units as the array elements.
 * @param[in] hi Upper bound; must be >= lo.
 * @param[in] n Element count; zero leaves the array untouched.
 * @note No storage is retained. NaN behavior follows backend comparisons.
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
 * @brief Sum corresponding products from two arrays.
 * @param[in] a Borrowed readable array of at least n doubles.
 * @param[in] b Borrowed readable array of at least n doubles.
 * @param[in] n Element count; zero returns zero without accessing the arrays.
 * @return Sum of a[i] * b[i], in the product of the inputs' units.
 * @pre Arrays are non-null for n > 0 and satisfy the restrict alias contract.
 * @note No alignment is required. SIMD lanes may use fused multiply-add
 * and a different summation order; scalar tails use ordinary accumulation.
 * No Kahan compensation is performed and no storage is retained.
 */
inline double dot(const double* OPENSWMM_RESTRICT a, const double* OPENSWMM_RESTRICT b,
                  std::size_t n) noexcept {
    double result = 0.0;
#if defined(OPENSWMM_SIMD_AVX2)
    const std::size_t nv = n / 4;
    if (nv > 0) {
        __m256d vsum = _mm256_setzero_pd();
        for (std::size_t i = 0; i < nv; ++i) {
            __m256d va = _mm256_loadu_pd(a + i * 4);
            __m256d vb = _mm256_loadu_pd(b + i * 4);
            vsum = _mm256_fmadd_pd(va, vb, vsum); // FMA: vsum += va * vb
        }
        // Horizontal sum
        __m128d hi = _mm256_extractf128_pd(vsum, 1);
        __m128d lo = _mm256_castpd256_pd128(vsum);
        __m128d s2 = _mm_add_pd(lo, hi);
        __m128d s1 = _mm_add_pd(s2, _mm_shuffle_pd(s2, s2, 1));
        result = _mm_cvtsd_f64(s1);
    }
    for (std::size_t i = nv * 4; i < n; ++i)
        result += a[i] * b[i];
#elif defined(OPENSWMM_SIMD_NEON)
    const std::size_t nv = n / 2;
    if (nv > 0) {
        float64x2_t vsum = vdupq_n_f64(0.0);
        for (std::size_t i = 0; i < nv; ++i) {
            float64x2_t va = vld1q_f64(a + i * 2);
            float64x2_t vb = vld1q_f64(b + i * 2);
            vsum = vfmaq_f64(vsum, va, vb); // FMA: vsum += va * vb
        }
        // Horizontal sum: add both lanes
        result = vgetq_lane_f64(vsum, 0) + vgetq_lane_f64(vsum, 1);
    }
    for (std::size_t i = nv * 2; i < n; ++i)
        result += a[i] * b[i];
#else
    for (std::size_t i = 0; i < n; ++i)
        result += a[i] * b[i];
#endif
    return result;
}

/**
 * @brief Report the compile-time number of doubles per SIMD register.
 * @return Four for AVX2, two for NEON, or one for the scalar backend.
 */
constexpr std::size_t lane_width() noexcept {
    return OPENSWMM_SIMD_WIDTH;
}

/**
 * @brief Compute dst[i] = sqrt(a[i]) over n doubles.
 * @param[in] a Borrowed readable array of at least n doubles.
 * @param[out] dst Borrowed writable array of at least n doubles.
 * @param[in] n Element count; zero performs no reads or writes.
 * @pre Arrays do not overlap and are non-null for n > 0.
 * @note Negative inputs follow std::sqrt domain behavior. No storage is retained;
 * alignment is unrestricted and the compiler may vectorize the loop.
 */
inline void sqrt_array(const double* OPENSWMM_RESTRICT a, double* OPENSWMM_RESTRICT dst,
                       std::size_t n) noexcept {
    OPENSWMM_IVDEP
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = std::sqrt(a[i]);
}

/**
 * @brief Compute dst[i] = fabs(a[i]) over n doubles.
 * @param[in] a Borrowed readable array of at least n doubles.
 * @param[out] dst Borrowed writable array of at least n doubles.
 * @param[in] n Element count; zero performs no reads or writes.
 * @pre Arrays do not overlap and are non-null for n > 0.
 * @note Output values are nonnegative magnitudes. No storage is retained;
 * alignment is unrestricted and the compiler may vectorize the loop.
 */
inline void fabs_array(const double* OPENSWMM_RESTRICT a, double* OPENSWMM_RESTRICT dst,
                       std::size_t n) noexcept {
    OPENSWMM_IVDEP
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = std::fabs(a[i]);
}

/**
 * @brief Compute dst[i] = a[i] * b[i] + c[i] over n doubles.
 * @param[in] a Borrowed first-factor array with at least n elements.
 * @param[in] b Borrowed second-factor array with at least n elements.
 * @param[in] c Borrowed addend array with at least n elements, in product units.
 * @param[out] dst Borrowed writable result array with at least n elements.
 * @param[in] n Element count; zero performs no reads or writes.
 * @pre Arrays do not overlap and are non-null for n > 0.
 * @note No storage is retained and no alignment is required. The expression
 * uses ordinary multiply and add; fusion depends on compiler settings.
 */
inline void fma_array(const double* OPENSWMM_RESTRICT a, const double* OPENSWMM_RESTRICT b,
                      const double* OPENSWMM_RESTRICT c, double* OPENSWMM_RESTRICT dst,
                      std::size_t n) noexcept {
    OPENSWMM_IVDEP
    for (std::size_t i = 0; i < n; ++i)
        dst[i] = a[i] * b[i] + c[i];
}

} /* namespace openswmm::simd */

// ============================================================================
// Fast math: fixed-exponent pow() replacements using std::sqrt / std::cbrt
//
// std::pow(x, y) dispatches through exp(y*log(x)) — ~60-80 cycles on ARM.
// std::sqrt / std::cbrt are ~10-15 cycle intrinsics. For exponents that
// factor through half- and third-powers these closed forms are dramatically
// cheaper in the hot loops (Manning friction, weir/orifice equations,
// submergence corrections).
// ============================================================================

namespace openswmm::fastmath {

/**
 * @brief Compute the real 3/2 power using square or cube roots.
 * @param[in] x Nonnegative finite input. Units are chosen by the caller.
 * @return x * sqrt(x); standard-library floating-point domain and
 * range behavior applies. The result is returned by value.
 */
inline double pow3_2(double x) noexcept {
    return x * std::sqrt(x);
}

/**
 * @brief Compute the real 5/2 power using square or cube roots.
 * @param[in] x Nonnegative finite input. Units are chosen by the caller.
 * @return x * x * sqrt(x); standard-library floating-point domain and
 * range behavior applies. The result is returned by value.
 */
inline double pow5_2(double x) noexcept {
    return x * x * std::sqrt(x);
}

/**
 * @brief Compute the real 5/3 power using square or cube roots.
 * @param[in] x Finite input; x * x must remain representable for a finite result. Units are chosen by the caller.
 * @return x * cbrt(x * x); standard-library floating-point domain and
 * range behavior applies. The result is returned by value.
 */
inline double pow5_3(double x) noexcept {
    return x * std::cbrt(x * x);
}

/**
 * @brief Compute the real 4/3 power using square or cube roots.
 * @param[in] x Finite input. Units are chosen by the caller.
 * @return x * cbrt(x); standard-library floating-point domain and
 * range behavior applies. The result is returned by value.
 */
inline double pow4_3(double x) noexcept {
    return x * std::cbrt(x);
}

/**
 * @brief Compute the real 2/3 power using square or cube roots.
 * @param[in] x Finite input; x * x must remain representable for a finite result. Units are chosen by the caller.
 * @return cbrt(x * x); standard-library floating-point domain and
 * range behavior applies. The result is returned by value.
 */
inline double pow2_3(double x) noexcept {
    return std::cbrt(x * x);
}

#if defined(OPENSWMM_SIMD_NEON)
// ============================================================================
// NEON 2-wide vectorised cube root via Newton-Raphson refinement.
//
// Seeding strategy: cast to float32x2, use single-precision cbrt via
// arithmetic refinement, then widen back to float64x2 for three
// double-precision refinement passes. This implementation has no formal
// error bound; callers must qualify its accuracy over their input range.
//
// Used by batch_pow4_3 and batch_pow2_3 to compute Manning exponents on
// pairs of hydraulic-radius values without calling std::cbrt twice.
// ============================================================================

/**
 * @brief Approximate two cube roots with float seeding and refinement.
 * @param[in] x Two packed nonnegative finite doubles, also representable
 * as finite floats for the seeding calculation.
 * @return Packed results of the implemented Newton refinement sequence.
 * @note No exceptional-lane handling or accuracy guarantee is supplied;
 * callers must validate the approximation over their input range.
 */
inline float64x2_t vcbrtq_f64(float64x2_t x) noexcept {
    // --- Step 1: float32 seed ---
    // Narrow to float32, compute a rough cbrt via bit-manipulation seed
    // (the classic Kahan integer cbrt trick generalised to float):
    //   ieee754(float) cbrt seed: reinterpret as int32, scale exponent by 1/3.
    float32x2_t xf = vcvt_f32_f64(x); // narrow to float32
    int32x2_t xi = vreinterpret_s32_f32(xf);
    // Magic constant for float cbrt seed: (1/3) * (2^23) * (2 - 127/3 + bias)
    // = 0x2A2A2A2B (approx). Gives ~5-bit accurate seed.
    xi = vadd_s32(vshr_n_s32(vsub_s32(xi, vdup_n_s32(0x3F800000)), 2),
                  vdup_n_s32(0x3F800000 + (0x3F800000 / 3)));
    float32x2_t cr = vreinterpret_f32_s32(xi);

    // --- Step 2: two float32 NR steps: cr = cr - (cr - x/cr²) / 3 ---
    float32x2_t cr2 = vmul_f32(cr, cr);
    cr = vadd_f32(cr, vmul_f32(vdup_n_f32(0.333333333f), vsub_f32(vdiv_f32(xf, cr2), cr)));
    cr2 = vmul_f32(cr, cr);
    cr = vadd_f32(cr, vmul_f32(vdup_n_f32(0.333333333f), vsub_f32(vdiv_f32(xf, cr2), cr)));

    // --- Step 3: widen seed to double and run three double-precision NR ---
    float64x2_t c = vcvt_f64_f32(cr); // widen to float64

    // NR: c_next = c - (c³ - x) / (3 c²)
    //           = (2c³ + x) / (3c²)      [Horner-friendly form]
    float64x2_t three = vdupq_n_f64(3.0);
    float64x2_t onethird = vdupq_n_f64(0.333333333333333333);
    for (int i = 0; i < 3; ++i) {
        float64x2_t c2 = vmulq_f64(c, c);
        float64x2_t c3 = vmulq_f64(c2, c);
        // c = c - (c³ - x) / (3 * c²)
        c = vsubq_f64(c, vmulq_f64(onethird, vdivq_f64(vsubq_f64(c3, x), vmulq_f64(three, c2))));
    }
    return c;
}

/**
 * @brief Approximate the 4/3 power of two packed double values.
 * @param[in] x Packed nonnegative finite values; each input to vcbrtq_f64()
 * must satisfy that helper's float-seeding representability requirements.
 * @return Packed x * vcbrtq_f64(x), with the approximation limitations of
 * vcbrtq_f64(). No input validation or exceptional-lane handling is performed.
 */
inline float64x2_t batch_pow4_3(float64x2_t x) noexcept {
    return vmulq_f64(x, vcbrtq_f64(x));
}

/**
 * @brief Approximate the 2/3 power of two packed double values.
 * @param[in] x Packed nonnegative finite values; each input to vcbrtq_f64()
 * must satisfy that helper's float-seeding representability requirements.
 * @return Packed vcbrtq_f64(x * x), with the approximation limitations of
 * vcbrtq_f64(). No input validation or exceptional-lane handling is performed.
 */
inline float64x2_t batch_pow2_3(float64x2_t x) noexcept {
    return vcbrtq_f64(vmulq_f64(x, x));
}

#endif /* OPENSWMM_SIMD_NEON */

} /* namespace openswmm::fastmath */

#endif /* OPENSWMM_ENGINE_SIMD_HPP */
