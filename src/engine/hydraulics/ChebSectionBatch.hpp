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
 * 4. **Taylor-extrapolating A and W across Picard iterations instead of
 *    re-evaluating** (promptperf.md Phase F). Investigated with a full
 *    instrumented run on Bellinge and dropped without being built, for two
 *    independent reasons, either of which is fatal:
 *
 *    *The premise does not hold.* Phase F assumes DYNWAVE grinds through many
 *    Picard iterations with the depth change shrinking each time. Bellinge
 *    converges in **2.00 iterations per step**: of 159.3 M conduit
 *    evaluations, 79.67 M are at iteration 0, 79.67 M at iteration 1, and
 *    **1,790 — 0.001% — at iteration 2 or beyond.** There is no "later
 *    iteration" to extrapolate into. (The same run shows the legacy
 *    `bypassed_` mask skipping only 0.1% of evaluations, because legacy's own
 *    `Steps > 1` schedule means it is never populated before the step ends.)
 *
 *    *The error bound is unavailable exactly where the traffic is.* Reframing
 *    it as extrapolation from a fixed ANCHOR (never chained — chaining
 *    integrates truncation error over ~157k iterations with no bound) does
 *    work: measured hit rates are 23.3% at dy == 0, 92.5% within 1e-5*y_full
 *    and 99.7% within 1e-3*y_full. But a fast path must decide validity **in
 *    advance**, and the only a-priori bound is sup|A''| / sup|A'''| over the
 *    piece, which is FINITE only where the coordinate map is the identity.
 *    Phase 4's normalization invariant ("no piece may be singular at both
 *    ends") guarantees every other piece has a sqrt-mapped end where du/dy
 *    blows up — and dry-weather depths sit in the invert piece, so only
 *    **7.8% of evaluations land on an identity-mapped piece**. Measuring the
 *    error a 2nd-order Taylor would actually commit confirms it from the
 *    other side: only 56.9% of extrapolations stay inside the compiled fit's
 *    own kFitTol, 99.1% stay inside 1e-7*a_full, and the worst observed is
 *    7.3e-5*a_full — 73,000x the fit tolerance. A rigorously-bounded fast
 *    path therefore fires on ~7.6% of traffic; a tuned-constant radius is
 *    what promptperf.md explicitly forbids.
 *
 *    Ceiling, for anyone tempted to revisit: running the STEP B + STEP D
 *    batch kernels twice (idempotent, so results stay bit-identical) costs
 *    +6.5 to +7.3 s on a 42 s EXACT run, so ONE full forward-geometry pass is
 *    worth ~6.5 s against a ~6 s EXACT-vs-LEGACY gap. At a 7.6% hit rate the
 *    rigorous version is worth ~0.3 s — an order of magnitude below this
 *    machine's run-to-run spread. **Attack the S-family and the compiled
 *    inverse instead:** the same profile puts +2,514 whole-run samples of the
 *    gap in getAofS/getSofA/getdSdA against +2,368 in the entire forward
 *    batch path, and that third needs no accuracy trade at all.
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
    chebEval2(chebCoef(s, pc.off_a), pc.n_a, chebCoef(s, pc.off_p), pc.n_p,
             chebUofY(pc, y), a, p);
    A = a;
    R = (p > 0.0) ? a / p : 0.0;
}

/**
 * @brief Flow area and top width at one depth, in one pass.
 *
 * @details Added for `XSectKernels.hpp`'s `generic_getYcrit` — its
 *          `qCritical` probe called `getAofY` then `getWofY` separately at
 *          the same trial depth on every enumeration/Ridder iteration, which
 *          on a compiled section is two independent piece scans and basis
 *          recurrences for one probe. Same redundant-evaluation shape as
 *          `chebARofY`, one caller over (DYNWAVE's offset/critical-depth
 *          classification path, and `Outfall.cpp`), not the batch layer —
 *          kept here anyway, next to `chebARofY`, so every non-`chebAll`
 *          fused N-series accessor stays in one place.
 *
 * @param s  compiled section.
 * @param y  depth (ft).
 * @param A  flow area (ft^2).
 * @param W  top width (ft).
 *
 * @note Bit-identical to chebAofY() and chebWofY() called separately: both
 *       select the same piece via chebPieceOfY(s, y) and the same u via
 *       chebUofY(pc, y) below the crown; at or above y_full both resolve to
 *       the top piece evaluated at u = 1.0 (chebWofY has no precomputed
 *       "w_full" scalar the way chebRofY has r_full, so this mirrors its
 *       actual code path rather than shortcutting through one).
 */
OPENSWMM_KERNEL_FN void chebAWofY(const ChebSection& s, double y,
                                  double& A, double& W) noexcept {
    if (s.n_pieces <= 0 || y <= 0.0) { A = 0.0; W = 0.0; return; }
    if (y >= s.y_full) {
        const ChebPiece& top = s.piece[s.n_pieces - 1];
        A = s.a_full;
        const double w = chebEval(chebCoef(s, top.off_w), top.n_w, 1.0);
        W = (w > 0.0) ? w : 0.0;
        return;
    }
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    double a = 0.0, w = 0.0;
    chebEval2(chebCoef(s, pc.off_a), pc.n_a, chebCoef(s, pc.off_w), pc.n_w,
             chebUofY(pc, y), a, w);
    A = a;
    W = (w > 0.0) ? w : 0.0;
}

/**
 * @brief Three Chebyshev series sharing one basis recurrence.
 *
 * @details The three-field sibling of chebEval2, for callers that need A, W
 *          and P together. Same reasoning: the `T_k` recurrence is a serial
 *          dependency chain and the per-field multiply-adds are not, so a
 *          third field is very nearly free where a third CALL is not.
 *
 * @param ca,na  first field's coefficients and retained count.
 * @param cb,nb  second field's coefficients and retained count.
 * @param cc,nc  third field's coefficients and retained count.
 * @param u      fit variable in [0,1], from chebUofY().
 * @param a_out,b_out,c_out  the three sums.
 *
 * @note All three fields run to `max(na, nb, nc)` terms — safe and
 *       result-preserving for exactly the reason chebEval2 and chebAll
 *       document: the surplus coefficients are zero, so the extra terms add
 *       `0.0 * T_k`.
 */
OPENSWMM_KERNEL_FN void chebEval3(const double* ca, int na,
                                  const double* cb, int nb,
                                  const double* cc, int nc, double u,
                                  double& a_out, double& b_out,
                                  double& c_out) noexcept {
    const double x = 2.0 * u - 1.0;
    const double x2 = 2.0 * x;
    int n = (na > nb) ? na : nb;
    if (nc > n) n = nc;

    double a = ca[0], b = cb[0], c = cc[0];
    if (n > 1) { a += ca[1] * x; b += cb[1] * x; c += cc[1] * x; }
    double t_prev = 1.0, t_cur = x;
    for (int k = 2; k < n; ++k) {
        const double t = x2 * t_cur - t_prev;
        t_prev = t_cur;
        t_cur = t;
        a += ca[k] * t;
        b += cb[k] * t;
        c += cc[k] * t;
    }
    a_out = a;
    b_out = b;
    c_out = c;
}

/**
 * @brief Flow area, top width and hydraulic radius at one depth, in one pass.
 *
 * @details Exists for the FV closure (`FvKernels.hpp::closureAll`), which
 *          needs exactly these three at one depth for every cell on every
 *          timestep. Through the ordinary accessors that costs FOUR series
 *          evaluations and four piece scans, not three: `chebRofY` evaluates
 *          the area series a second time to form A/P (see its body). One
 *          piece scan and one basis recurrence replace all of it.
 *
 * @param s  compiled section.
 * @param y  depth (ft).
 * @param A  flow area (ft^2).
 * @param W  top width (ft), clamped at 0 like chebWofY.
 * @param R  hydraulic radius (ft).
 *
 * @note Bit-identical to chebAofY/chebWofY/chebRofY called separately. Below
 *       the crown all three select the same piece and the same u, and R is
 *       formed as `a / p` from the very coefficients chebRofY would have
 *       re-evaluated. At and above y_full each field takes the scalar its own
 *       accessor takes (a_full, the top piece's width at u = 1, r_full) —
 *       note in particular that R uses `r_full`, which for a closed shape
 *       carries the crown perimeter JUMP that extrapolating the fitted P
 *       series would miss (ChebSection::p_full).
 */
OPENSWMM_KERNEL_FN void chebAWRofY(const ChebSection& s, double y,
                                   double& A, double& W, double& R) noexcept {
    if (s.n_pieces <= 0 || y <= 0.0) { A = 0.0; W = 0.0; R = 0.0; return; }
    if (y >= s.y_full) {
        const ChebPiece& top = s.piece[s.n_pieces - 1];
        A = s.a_full;
        const double w = chebEval(chebCoef(s, top.off_w), top.n_w, 1.0);
        W = (w > 0.0) ? w : 0.0;
        R = s.r_full;
        return;
    }
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    double a = 0.0, w = 0.0, p = 0.0;
    chebEval3(chebCoef(s, pc.off_a), pc.n_a,
              chebCoef(s, pc.off_w), pc.n_w,
              chebCoef(s, pc.off_p), pc.n_p,
              chebUofY(pc, y), a, w, p);
    A = a;
    W = (w > 0.0) ? w : 0.0;
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
