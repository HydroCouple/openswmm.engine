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
 * @file ChebSectionBatch.hpp
 * @brief Batched evaluation of compiled cross-sections — one call per group of
 *        conduits instead of one accessor call per conduit per field.
 *
 * @details The win here is **field fusion, not lane parallelism**, which is
 *          the opposite of what this phase set out to build. The measurements
 *          that redirected it are recorded below so nobody repeats them.
 *
 *          What a batch call buys is the chance to answer a caller's WHOLE
 *          question in one pass over one piece. chebAll() already does that
 *          for callers wanting all four closure fields. The dominant batch
 *          caller does not want four: DYNWAVE wants area and hydraulic
 *          radius, which is A and P, and reaching those through chebAll()
 *          computes W and I1 as well and throws them away. Evaluating the two
 *          series actually wanted, sharing one basis recurrence between them,
 *          is worth **about 1.6x** on that path for the circular sections
 *          that dominate real networks, and more for simpler ones.
 *
 * ### Measured on this machine (AVX2, Phase 5B), ns per evaluation
 *
 * Medians of repeated interleaved runs, not best-of. A compiled circular pipe
 * (5 pieces), a benched box on a semicircular invert (4 pieces), a one-piece
 * closed box, and a "mixed" pool of 24 distinct circular sections standing in
 * for a real network, where every diameter compiles its own section:
 *
 * | path                        | before | after | what changed           |
 * |-----------------------------|--------|-------|------------------------|
 * | A + R, circular             |  27.3  | 17.3  | chebEval2 field fusion |
 * | A + R, mixed pool           |  32.2  | 19.2  | chebEval2 field fusion |
 * | A + R, benched              |  15.7  |  9.0  | chebEval2 field fusion |
 * | A + R, one-piece box        |  10.5  |  4.4  | chebEval2 field fusion |
 * | A alone, circular           |  18.7  | 15.4  | forward recurrence     |
 * | W alone, circular           |  21.4  | 16.6  | forward recurrence     |
 * | all four (chebAll)          |  24.8  | 24.8  | already fused          |
 *
 * End to end on Bellinge (953 conduits, ~70% circular, dry-weather flow,
 * DYNWAVE): EXACT 203 s -> 154 s against an unchanged 36 s LEGACY, so the
 * EXACT/LEGACY ratio moved 5.6x -> 4.2x.
 *
 * ### Three designs measured and REJECTED — do not reintroduce
 *
 * 1. **Gathering W lanes' coefficients** (`_mm256_i64gather_pd`) and running
 *    the recurrence across lanes — the shape this phase was specified to
 *    build. Implemented and measured at **105 ns/eval against 25 scalar**,
 *    four times SLOWER. AVX2's gather is microcoded, roughly one 4-lane
 *    gather per 4 cycles, and this kernel needs four of them per coefficient
 *    index (one per field) to replace four ordinary loads. It also forces
 *    every lane in a group to run to the WORST lane's coefficient count,
 *    which the per-element form avoids for free.
 * 2. **Vectorizing the recurrence over COEFFICIENT INDEX** instead
 *    (contiguous loads, `T_{k+4} = 2 T_4 T_k - T_{k-4}`). This works and is
 *    never slower, but measured only 1.1x-1.3x on the four-field path and
 *    nothing at all on single-field, while making results depend on the SIMD
 *    width the engine happened to be built for. A platform-dependent answer
 *    is not worth 1.1x.
 * 3. **Unrolling the batch loops by two or four** to overlap consecutive
 *    elements' dependent-load chains. No measurable gain, and it made simple
 *    sections slower (a one-piece box went 3.1 -> 4.3 ns/eval) once the
 *    per-lane piece pointers stopped fitting in registers. The compiler
 *    already interleaves these loops; hand-unrolling got in its way.
 *
 * @note Every function here returns **bit-identical** results to the
 *       ChebSection.hpp accessor it replaces — not merely close. Sharing the
 *       basis recurrence between two fields changes nothing numerically,
 *       because the extra terms a shorter field picks up are `c[k] * T_k`
 *       with `c[k]` exactly zero (chebChop zeroes the tail), and adding a
 *       signed zero leaves a double unchanged. test_cheb_section_batch.cpp
 *       asserts exact equality rather than a tolerance, which is what makes
 *       that argument checkable rather than merely plausible.
 *
 * @warning A null entry in the section-pointer array is SKIPPED, leaving that
 *          element's outputs untouched. Callers mixing compiled and
 *          uncompiled links (XSectBatch.cpp does — one degenerate link can
 *          fail to compile while its group-mates succeed) must fill those
 *          elements themselves. Skipping rather than defaulting keeps this
 *          header free of any dependency on XSectParams and the legacy shape
 *          dispatch.
 *
 * @see ChebSection.hpp for the compiled representation and scalar accessors.
 * @see XSectBatch.cpp for the batch kernels that call these.
 * @ingroup engine_hydraulics
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_CHEB_SECTION_BATCH_HPP
#define OPENSWMM_CHEB_SECTION_BATCH_HPP

#include <cstddef>

#include "ChebSection.hpp"

namespace openswmm::chebsec {

/**
 * @brief Two series at one fit variable, sharing the basis recurrence.
 *
 * @details The same trick chebAll() uses across four fields — one `T_k` chain
 *          with the accumulations riding alongside it — for callers that want
 *          exactly two. The recurrence is the serial part; the accumulations
 *          are not, so a second field is very nearly free where a second CALL
 *          is not.
 *
 * @param ca      first field's coefficients.
 * @param na      first field's retained coefficient count.
 * @param cb      second field's coefficients.
 * @param nb      second field's retained coefficient count.
 * @param u       fit variable in [0,1], from chebUofY().
 * @param a_out   first field's sum.
 * @param b_out   second field's sum.
 *
 * @note Both fields run to `max(na, nb)` terms. That is safe and changes no
 *       result: chebChop zeroes each field's tail and ChebPiece's arrays are
 *       zero-initialized, so the surplus terms are `0.0 * T_k`, and adding a
 *       (possibly negatively signed) zero leaves a double bit-unchanged.
 *       chebAll() relies on the same property across four fields.
 */
OPENSWMM_KERNEL_FN void chebEval2(const double* ca, int na,
                                  const double* cb, int nb, double u,
                                  double& a_out, double& b_out) noexcept {
    const double x = 2.0 * u - 1.0;
    const double x2 = 2.0 * x;
    const int n = (na > nb) ? na : nb;

    double a = ca[0], b = cb[0];
    if (n > 1) { a += ca[1] * x; b += cb[1] * x; }
    double t_prev = 1.0, t_cur = x;
    for (int k = 2; k < n; ++k) {
        const double t = x2 * t_cur - t_prev;
        t_prev = t_cur;
        t_cur = t;
        a += ca[k] * t;
        b += cb[k] * t;
    }
    a_out = a;
    b_out = b;
}

/**
 * @brief Flow area and hydraulic radius at one depth, in one pass.
 *
 * @param s  compiled section.
 * @param y  depth (ft).
 * @param A  flow area (ft^2).
 * @param R  hydraulic radius (ft).
 *
 * @note Bit-identical to chebAofY() and chebRofY() called separately, and to
 *       the chebAll()-then-divide route XSectBatch.cpp took before, while
 *       evaluating two series instead of four.
 *
 * @note R is formed from the RAW perimeter sum, which matches chebRofY:
 *       that function divides by chebPofY's already-clamped value, and
 *       clamping a positive number changes nothing while a non-positive one
 *       takes the zero branch either way.
 */
OPENSWMM_KERNEL_FN void chebARofY(const ChebSection& s, double y,
                                  double& A, double& R) noexcept {
    if (s.n_pieces <= 0 || y <= 0.0) { A = 0.0; R = 0.0; return; }
    if (y >= s.y_full) { A = s.a_full; R = s.r_full; return; }
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    double a = 0.0, p = 0.0;
    chebEval2(pc.c_a, pc.n_a, pc.c_p, pc.n_p, chebUofY(pc, y), a, p);
    A = a;
    R = (p > 0.0) ? a / p : 0.0;
}

// ===========================================================================
// Batch accessors
// ===========================================================================
//
// Plain loops, deliberately — hand-unrolling them was measured and rejected
// (see the file header). What these exist for is to give each XSectBatch.cpp
// kernel ONE call shaped like what that kernel actually needs, which is how
// the fused two-field path becomes reachable at all.

/// Flow area at each depth. Bit-identical to chebAofY() per element.
inline void chebAofYBatch(const ChebSection* const* cheb, const double* y,
                          double* A, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (cheb[i]) A[i] = chebAofY(*cheb[i], y[i]);
    }
}

/// Top width at each depth. Bit-identical to chebWofY() per element.
inline void chebWofYBatch(const ChebSection* const* cheb, const double* y,
                          double* W, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (cheb[i]) W[i] = chebWofY(*cheb[i], y[i]);
    }
}

/// Hydraulic radius at each depth. Bit-identical to chebRofY() per element,
/// reached through chebARofY so that A and P share one basis recurrence.
inline void chebRofYBatch(const ChebSection* const* cheb, const double* y,
                          double* R, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (!cheb[i]) continue;
        double a = 0.0, r = 0.0;
        chebARofY(*cheb[i], y[i], a, r);
        R[i] = r;
    }
}

/// Flow area and hydraulic radius together — **the hot path**, and the reason
/// this header exists. One piece scan and one basis recurrence for both.
inline void chebARofYBatch(const ChebSection* const* cheb, const double* y,
                           double* A, double* R, std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (!cheb[i]) continue;
        chebARofY(*cheb[i], y[i], A[i], R[i]);
    }
}

/// All four closure fields at each depth. Bit-identical to chebAll() per
/// element.
/// @note This is the entry point Phase 6's FV wiring wants: closureAll()
///       needs A, W, R and I1 at one depth, which is exactly one call here.
inline void chebAllBatch(const ChebSection* const* cheb, const double* y,
                         double* A, double* W, double* P, double* I1,
                         std::size_t n) noexcept {
    for (std::size_t i = 0; i < n; ++i) {
        if (cheb[i]) chebAll(*cheb[i], y[i], &A[i], &W[i], &P[i], &I1[i]);
    }
}

} // namespace openswmm::chebsec

#endif // OPENSWMM_CHEB_SECTION_BATCH_HPP
