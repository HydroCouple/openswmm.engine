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
 * @file ChebSection.hpp
 * @brief Piecewise-Chebyshev compression of an exact cross-section boundary.
 *
 * @details Turns the exact arc/line boundary of XSectBoundary.hpp into a small
 *          POD table that evaluates A, W, P and I1 at any depth with no
 *          transcendental calls and no geometry traversal. The compression is
 *          done ONCE, at load time (compile()); everything the solver touches
 *          afterwards is the header-inline accessors at the bottom of this
 *          file, which are `OPENSWMM_KERNEL_FN` and read only POD, so the same
 *          bodies compile for the CPU solver and a device backend.
 *
 *          **Why this converges fast.** A(y) is not analytic across the whole
 *          depth range — it kinks at benches and picks up square-root branch
 *          points at smooth tangencies. Splitting at the critical heights
 *          (findCriticalHeights) leaves A analytic on each piece, and applying
 *          the coordinate change in §4b removes the branch point at each end
 *          that still has one. Chebyshev coefficients of an analytic function
 *          decay geometrically, so a handful of them reach machine precision
 *          where a uniform table needs thousands of samples.
 *
 * @see XSectBoundary.hpp for the exact boundary and its critical heights.
 * @ingroup engine_hydraulics
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_CHEB_SECTION_HPP
#define OPENSWMM_CHEB_SECTION_HPP

#include <cmath>
#include <functional>
#include <type_traits>

#include "XSectBoundary.hpp"

// Portable kernel-function marker — same convention as FvKernels.hpp and
// XSectKernels.hpp. Host builds get plain `inline`; a device consumer defines
// this to KOKKOS_INLINE_FUNCTION *before* including.
#ifndef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN inline
#endif

namespace openswmm::chebsec {

/// Maximum Chebyshev coefficients retained per field per piece.
constexpr int kMaxChebCoeff = 32;
/// Maximum smooth depth intervals a section may be split into.
///
/// @note **This caps how detailed a POLYGON boundary may be, and the limit is
///       sharp.** A section needs (distinct critical heights - 1) pieces, and
///       every polyline vertex contributes a critical height. A left-right
///       symmetric polygon pairs its vertex heights, so it needs n/2 pieces
///       for n vertices — 24 pieces admits a 48-gon. An asymmetric one needs
///       up to n-1, so about 25 vertices. Beyond that compile() returns 5
///       rather than degrading quietly.
///
///       That is a real limit on dense point clouds, and it is the intended
///       trade: the shapes POLYGON exists for (egg, arch, gothic, horseshoe)
///       are a handful of ARCS, which are exact and cost one piece each.
///       A surveyed channel with dozens of offsets is what IRREGULAR
///       transects are for. The design sketch's own performance case asks for
///       a 64-element boundary to compile, which these constants cannot
///       satisfy for a generic polygon; raising kMaxPieces is the lever if
///       that case matters, at ~1.1 kB per additional piece.
constexpr int kMaxPieces = 24;

/// Relative tolerance the fit is chopped at — **the accuracy/speed design
/// point**, and the single most important number in this file.
///
/// @note Chasing machine precision here is a bad trade, and the measurements
///       say so plainly. Evaluation cost is roughly linear in the retained
///       coefficient count, and the last few orders of accuracy are the
///       expensive ones. Measured on a circular pipe split into 8 pieces:
///
///       | chop  | coefficients | relative error | vs legacy table |
///       |-------|--------------|----------------|-----------------|
///       | 1e-14 |      15      |     6e-15      | ~2x slower      |
///       | 1e-10 |      11      |     8e-11      | ~parity         |
///       | 1e-9  |      10      |     1e-9       | ~15% faster     |
///       | 1e-8  |       9      |     6e-9       | ~25% faster     |
///
///       The thing being replaced — a 51-point table with linear
///       interpolation — carries about **1.4e-2** relative error, and about
///       **4.1e-1** below 5% of full depth. So 1e-9 is roughly SEVEN ORDERS
///       better than the status quo while also being faster; spending 5 more
///       coefficients to reach 1e-14 buys accuracy far below anything
///       hydraulically meaningful and gives the speed back.
constexpr double kFitTol = 1.0e-9;

/// Coefficient count per piece that compile() will spend spare piece slots to
/// get under.
///
/// @note **This is a speed knob, not an accuracy knob** — every piece is fitted
///       to machine precision either way. Evaluation cost is roughly
///       `fixed + n * per-coefficient`, and the per-coefficient term dominates:
///       measured on a circular pipe, a 20-coefficient series costs 109 ms per
///       4M evaluations against 28 ms at 8 coefficients. Splitting a piece in
///       two lowers the degree each half needs, so trading spare slots for
///       degree is close to a pure win — bounded by kMaxPieces, and costing
///       ~1.1 kB plus one predictable compare per extra piece.
///
///       Splitting also REMOVES the expensive coordinate map: a piece with a
///       sqrt branch point at both ends needs `acos` (38 ms/4M), while its two
///       halves each have a branch point at one end only and need `sqrt`
///       (7 ms/4M). So the refinement pass pays for itself twice.
constexpr int kTargetCoeff = 8;

/// Compile-time pi, spelled locally so the header stays dependency-free.
constexpr double kPi = 3.14159265358979323846;

/// Exponent tags above this are treated as the 3/2 (square-root branch) case.
/// Only 1.0 and 1.5 are ever emitted by findCriticalHeights, so the midpoint
/// is a safe discriminator that cannot be upset by float representation.
constexpr double kExpSplit = 1.25;

/**
 * @brief One smooth depth interval and its Chebyshev series.
 *
 * @details Fields are fitted in a coordinate `u` in [0,1] chosen so that A is
 *          ANALYTIC on the piece. The map depends on the exponent tag at BOTH
 *          ends — see mapSofU(). Tag 1.0 at both ends means the identity map.
 *
 * @warning A one-sided stretch is NOT sufficient. A closed conduit is tangent
 *          to horizontal at the CROWN as well as the invert (W goes to zero
 *          like sqrt(y_full - y), so A approaches its limit like a 3/2 power
 *          from below). Stretching only the bottom leaves the crown branch
 *          point in place and the fit stalls several orders short.
 */
struct ChebPiece {
    double y_lo = 0.0;      ///< lower depth of the interval (ft)
    double y_hi = 0.0;      ///< upper depth of the interval (ft)
    double exp_lo = 1.0;    ///< map tag at y_lo (1.0 analytic / 1.5 sqrt-branch)
    double exp_hi = 1.0;    ///< map tag at y_hi
    double inv_span = 0.0;  ///< 1 / (y_hi - y_lo)

    /// A(y_lo), taken from this piece's own fitted series so the area-ordered
    /// piece scan in chebYofA agrees exactly with chebAofY.
    /// @note Not in the original design sketch; added so chebYofA can locate
    ///       its piece by the same cheap linear compare the depth path uses,
    ///       instead of evaluating a Clenshaw per piece to find its bracket.
    double a_lo = 0.0;

    int n_a = 0, n_w = 0, n_p = 0, n_i1 = 0;   ///< retained coefficient counts

    double c_a[kMaxChebCoeff]{};   ///< flow area
    double c_w[kMaxChebCoeff]{};   ///< top width
    double c_p[kMaxChebCoeff]{};   ///< wetted perimeter
    double c_i1[kMaxChebCoeff]{};  ///< hydrostatic first moment

    double rho_a = 0.0;  ///< measured Bernstein parameter for A (diagnostic)
};

/**
 * @brief A whole cross-section, compressed. POD and trivially copyable so it
 *        can be memcpy'd to device memory.
 *
 * @note **Size.** With kMaxPieces = 24 and kMaxChebCoeff = 32 this is about
 *       26 kB: 24 pieces x 4 fields x 32 coefficients x 8 bytes is already
 *       24 kB of coefficients alone. The design sketch's "sizeof <= 8 kB"
 *       acceptance figure is arithmetically unreachable with those two
 *       constants and has been reported rather than met by shrinking them —
 *       both are set by accuracy requirements (a benched section needs ~33
 *       coefficients in total, and subdivision needs the piece headroom).
 *       The load-bearing part of that requirement, trivial copyability for the
 *       device backend, IS asserted below.
 */
struct ChebSection {
    int  n_pieces = 0;
    bool is_open = false;

    double y_full = 0.0;   ///< full depth (ft)
    double w_max = 0.0;    ///< widest top width (ft)
    double yw_max = 0.0;   ///< depth at which w_max occurs (ft)
    double a_full = 0.0;   ///< area at y_full (ft^2)
    double r_full = 0.0;   ///< hydraulic radius at y_full (ft)
    double s_full = 0.0;   ///< section factor at y_full
    double s_max = 0.0;    ///< maximum section factor
    double a_max = 0.0;    ///< area at which s_max occurs (ft^2)

    ChebPiece piece[kMaxPieces];
};

static_assert(std::is_trivially_copyable_v<ChebSection>,
              "ChebSection must be trivially copyable to mirror to device memory");

// ===========================================================================
// §4a — Chebyshev numerical routines (build-time)
// ===========================================================================

/// The j-th Chebyshev node of the first kind, mapped to [0,1].
/// @note First-kind (Gauss) nodes rather than Lobatto ones, deliberately:
///       every node is strictly INTERIOR to the piece. Piece boundaries are
///       critical heights, and evalExact() is only exact at a generic depth —
///       at a flat rim or a multi-vertex height it under-reports width and
///       area. Interior-only sampling means the fit never sees those values.
double chebNode(int n, int j) noexcept;

/// Chebyshev coefficients on [0,1] from samples already taken at chebNode().
/// @param f samples f(chebNode(n,j)), j = 0..n-1
void chebFitSamples(const double* f, int n, double* c) noexcept;

/// Chebyshev coefficients of @p f on [0,1].
/// @note Euler's formula at work: T_k(cos t) = cos(k t), so this is a Fourier
///       cosine transform of f composed with cos. The naive O(n^2) sum is used
///       throughout — n never exceeds kMaxChebCoeff here, where an FFT loses.
void chebFit(const std::function<double(double)>& f, int n, double* c);

/// Largest retained coefficient count after zeroing a negligible tail.
/// @returns the number of leading coefficients kept (at least 1).
int chebChop(double* c, int n, double tol = 1.0e-14);

/**
 * @brief Bernstein ellipse parameter rho from coefficient decay.
 * @details |c_k| ~ rho^-k, so rho = exp(-slope) of a least-squares line
 *          through log|c_k|. rho > 1 means geometric convergence; rho close to
 *          1 means a singularity sits on or near the interval and the piece
 *          must be subdivided.
 * @warning If the function is even or odd, alternate coefficients are ZERO and
 *          a naive log-fit returns NaN. Only indices above the noise floor are
 *          fitted, which excludes both the structural zeros and the numerical
 *          tail (including the tail matters just as much — its flat noise
 *          floor otherwise drags the fitted slope toward zero and rho toward
 *          1, which would trigger endless spurious subdivision).
 * @returns rho, or 0.0 when there is too little clean decay to fit.
 */
double bernsteinRho(const double* c, int n) noexcept;

// ===========================================================================
// §4b — the compiler (build-time)
// ===========================================================================

/**
 * @brief Compile an exact boundary into a piecewise-Chebyshev section.
 *
 * @param out      destination, fully overwritten.
 * @param elems    boundary chain from fromPolyline()/fromArcSpec().
 * @param n        element count.
 * @param is_open  true for an open channel: no crown, and the section factor
 *                 rises monotonically so a_max is a_full rather than the
 *                 interior peak a closed conduit has.
 *
 * @returns 0 on success, or a code shared with xsboundary::BoundaryError —
 *          1 too few elements, 3 zero height, 5 kMaxPieces exhausted,
 *          6 A(y) not monotone.
 *
 * @note **A(y) is never silently clamped to restore monotonicity.** A
 *       non-monotone A makes y(A) ill-posed, and the FV solver inverts it
 *       every step to get depth from conserved area; a clamp would turn a
 *       loud build-time failure into cells that quietly reconstruct the wrong
 *       free surface. Error 6 is returned instead.
 */
int compile(ChebSection& out, const xsboundary::BElem* elems, int n,
            bool is_open = false);

// ===========================================================================
// Coordinate map — shared by the compiler and the accessors
// ===========================================================================
//
// s is the normalized depth (y - y_lo)/(y_hi - y_lo); u is the fit variable.
// Squaring s near an end with a 3/2 branch point turns |ds|^{3/2} into |du|^3,
// which is analytic — that is the whole trick.

/// s(u) — fit variable to normalized depth.
OPENSWMM_KERNEL_FN double mapSofU(double u, double e_lo, double e_hi) noexcept {
    const bool lo = (e_lo > kExpSplit);
    const bool hi = (e_hi > kExpSplit);
    if (lo && hi) return 0.5 * (1.0 - std::cos(kPi * u));
    if (lo)       return u * u;
    if (hi)       { const double v = 1.0 - u; return 1.0 - v * v; }
    return u;
}

/// u(s) — normalized depth back to fit variable.
OPENSWMM_KERNEL_FN double mapUofS(double s, double e_lo, double e_hi) noexcept {
    const bool lo = (e_lo > kExpSplit);
    const bool hi = (e_hi > kExpSplit);
    const double sc = (s < 0.0) ? 0.0 : ((s > 1.0) ? 1.0 : s);
    if (lo && hi) {
        double t = 1.0 - 2.0 * sc;
        if (t < -1.0) t = -1.0;
        if (t > 1.0) t = 1.0;
        return std::acos(t) / kPi;
    }
    if (lo) return std::sqrt(sc);
    if (hi) return 1.0 - std::sqrt(1.0 - sc);
    return sc;
}

/// ds/du — the Jacobian of the map actually selected.
/// @warning chebdAdY MUST divide by this (times the span). Using the identity
///          Jacobian while fitting in a stretched coordinate is the easiest
///          bug in this file, and it is silent: A itself stays correct and
///          only the derivative is wrong.
OPENSWMM_KERNEL_FN double mapDsDu(double u, double e_lo, double e_hi) noexcept {
    const bool lo = (e_lo > kExpSplit);
    const bool hi = (e_hi > kExpSplit);
    if (lo && hi) return 0.5 * kPi * std::sin(kPi * u);
    if (lo)       return 2.0 * u;
    if (hi)       return 2.0 * (1.0 - u);
    return 1.0;
}

// ===========================================================================
// Accessors — header-inline, POD-only, device-safe
// ===========================================================================

/// Clenshaw evaluation of sum c_k T_k(2u-1) for u in [0,1].
OPENSWMM_KERNEL_FN double chebEval(const double* c, int n, double u) noexcept {
    if (n <= 0) return 0.0;
    const double x = 2.0 * u - 1.0;
    const double x2 = 2.0 * x;
    double b1 = 0.0, b2 = 0.0;
    for (int k = n - 1; k >= 1; --k) {
        const double t = x2 * b1 - b2 + c[k];
        b2 = b1;
        b1 = t;
    }
    return x * b1 - b2 + c[0];
}

/// Exact derivative coefficients, d/du on [0,1]. @p dc holds n entries; the
/// derivative series has degree n-2, so evaluate it with n-1 coefficients.
OPENSWMM_KERNEL_FN void chebDeriv(const double* c, int n, double* dc) noexcept {
    for (int i = 0; i < n; ++i) dc[i] = 0.0;
    if (n < 2) return;
    dc[n - 2] = 2.0 * static_cast<double>(n - 1) * c[n - 1];
    for (int k = n - 2; k >= 1; --k) {
        const double nxt = (k + 1 < n) ? dc[k + 1] : 0.0;
        dc[k - 1] = nxt + 2.0 * static_cast<double>(k) * c[k];
    }
    dc[0] *= 0.5;
    // d/dx -> d/du, since x = 2u - 1.
    for (int i = 0; i < n; ++i) dc[i] *= 2.0;
}

/// Index of the piece containing depth @p y.
/// @note Linear scan over y_lo. With at most 24 pieces this is 1-3 predictable
///       compares; a binary search and a per-cell cached piece index were both
///       measured SLOWER during design and must not be reintroduced.
OPENSWMM_KERNEL_FN int chebPieceOfY(const ChebSection& s, double y) noexcept {
    int p = 0;
    for (int i = 1; i < s.n_pieces; ++i) {
        if (y >= s.piece[i].y_lo) p = i;
        else break;
    }
    return p;
}

/// Fit variable for depth @p y within piece @p pc.
OPENSWMM_KERNEL_FN double chebUofY(const ChebPiece& pc, double y) noexcept {
    return mapUofS((y - pc.y_lo) * pc.inv_span, pc.exp_lo, pc.exp_hi);
}

OPENSWMM_KERNEL_FN double chebAofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (y >= s.y_full) return s.a_full;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    return chebEval(pc.c_a, pc.n_a, chebUofY(pc, y));
}

OPENSWMM_KERNEL_FN double chebWofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    const double yy = (y >= s.y_full) ? s.y_full : y;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, yy)];
    const double w = chebEval(pc.c_w, pc.n_w, chebUofY(pc, yy));
    return (w > 0.0) ? w : 0.0;
}

OPENSWMM_KERNEL_FN double chebPofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    const double yy = (y >= s.y_full) ? s.y_full : y;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, yy)];
    const double p = chebEval(pc.c_p, pc.n_p, chebUofY(pc, yy));
    return (p > 0.0) ? p : 0.0;
}

OPENSWMM_KERNEL_FN double chebRofY(const ChebSection& s, double y) noexcept {
    if (y >= s.y_full) return s.r_full;
    const double p = chebPofY(s, y);
    return (p > 0.0) ? chebAofY(s, y) / p : 0.0;
}

OPENSWMM_KERNEL_FN double chebI1ofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (y >= s.y_full) {
        // Above the crown A is constant at a_full, so I1 continues linearly.
        const ChebPiece& top = s.piece[s.n_pieces - 1];
        const double i1_full = chebEval(top.c_i1, top.n_i1, 1.0);
        return i1_full + s.a_full * (y - s.y_full);
    }
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    return chebEval(pc.c_i1, pc.n_i1, chebUofY(pc, y));
}

/// dA/dy from the A series' exact derivative — the same quantity chebWofY
/// returns, reached by an independent route. Agreement between the two is what
/// proves the map Jacobian below is the one the fit actually used.
OPENSWMM_KERNEL_FN double chebdAdY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || y >= s.y_full || s.n_pieces <= 0) return 0.0;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    const double u = chebUofY(pc, y);

    double dc[kMaxChebCoeff];
    chebDeriv(pc.c_a, pc.n_a, dc);
    const double dAdu = chebEval(dc, pc.n_a - 1, u);

    // dy/du = span * ds/du. At an end tagged 1.5 the Jacobian vanishes, and so
    // does dA/du (A goes like u^3 there) — the true width is zero at a smooth
    // tangency, so returning 0 is the correct limit rather than a guard.
    const double dydu = mapDsDu(u, pc.exp_lo, pc.exp_hi) / pc.inv_span;
    if (!(dydu > 1.0e-300)) return 0.0;
    return dAdu / dydu;
}

/// Invert A -> y. Newton on the exact derivative, safeguarded by bisection.
OPENSWMM_KERNEL_FN double chebYofA(const ChebSection& s, double a) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (a >= s.a_full) return s.y_full;

    // Same cheap linear scan as the depth path, in the area ordinate.
    int p = 0;
    for (int i = 1; i < s.n_pieces; ++i) {
        if (a >= s.piece[i].a_lo) p = i;
        else break;
    }
    const ChebPiece& pc = s.piece[p];

    double lo = pc.y_lo, hi = pc.y_hi;
    double y = 0.5 * (lo + hi);
    for (int it = 0; it < 40; ++it) {
        const double f = chebEval(pc.c_a, pc.n_a, chebUofY(pc, y)) - a;
        if (f > 0.0) hi = y; else lo = y;
        if (hi - lo <= 1.0e-15 * s.y_full) break;

        const double d = chebdAdY(s, y);
        double y_next = (d > 0.0) ? (y - f / d) : (0.5 * (lo + hi));
        if (!(y_next > lo && y_next < hi)) y_next = 0.5 * (lo + hi);
        if (std::fabs(y_next - y) <= 1.0e-15 * s.y_full) { y = y_next; break; }
        y = y_next;
    }
    return y;
}

/**
 * @brief FUSED evaluation — the hot-loop entry point.
 * @details One piece scan, one coordinate change, four Clenshaw recurrences.
 *          This is the measured win over four separate accessor calls; do not
 *          split it back up.
 */
OPENSWMM_KERNEL_FN void chebAll(const ChebSection& s, double y,
                                double* A, double* W, double* P,
                                double* I1) noexcept {
    if (s.n_pieces <= 0 || y <= 0.0) {
        *A = 0.0; *W = 0.0; *P = 0.0; *I1 = 0.0;
        return;
    }
    if (y >= s.y_full) {
        const ChebPiece& top = s.piece[s.n_pieces - 1];
        *A = s.a_full;
        *W = chebEval(top.c_w, top.n_w, 1.0);
        *P = chebEval(top.c_p, top.n_p, 1.0);
        *I1 = chebEval(top.c_i1, top.n_i1, 1.0) + s.a_full * (y - s.y_full);
        if (*W < 0.0) *W = 0.0;
        if (*P < 0.0) *P = 0.0;
        return;
    }

    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    const double u = chebUofY(pc, y);
    const double x = 2.0 * u - 1.0;
    const double x2 = 2.0 * x;

    // ONE basis recurrence, four dot products — not four Clenshaws. Clenshaw
    // is a serial dependency chain, so running it four times costs four times
    // its latency; sharing the T_k recurrence pays that latency once and lets
    // the four accumulations proceed alongside it. Measured 1.83x on a
    // circular pipe. The forward recurrence is safe here where Clenshaw would
    // normally be preferred: |T_k(x)| <= 1 for |x| <= 1, so it cannot grow.
    int n = pc.n_a;
    if (pc.n_w > n) n = pc.n_w;
    if (pc.n_p > n) n = pc.n_p;
    if (pc.n_i1 > n) n = pc.n_i1;
    // Reading past a field's own retained count is safe and deliberate:
    // chebChop zeroes the tail, and the arrays are zero-initialized.

    // Interleaved: the basis term is consumed by all four fields the moment it
    // is produced, so it stays in a register. Materializing T_k into an array
    // first and dotting each field over its own (shorter) length was MEASURED
    // SLOWER — 279 ms against 243 — because the spill and reload cost more than
    // the surplus multiply-adds it saves. Surplus terms are harmless: chebChop
    // zeroes the tail and the arrays are zero-initialized.
    double a = pc.c_a[0], w = pc.c_w[0], p = pc.c_p[0], i1 = pc.c_i1[0];
    if (n > 1) {
        a += pc.c_a[1] * x;
        w += pc.c_w[1] * x;
        p += pc.c_p[1] * x;
        i1 += pc.c_i1[1] * x;
    }
    double t_prev = 1.0, t_cur = x;
    for (int k = 2; k < n; ++k) {
        const double t = x2 * t_cur - t_prev;
        t_prev = t_cur;
        t_cur = t;
        a += pc.c_a[k] * t;
        w += pc.c_w[k] * t;
        p += pc.c_p[k] * t;
        i1 += pc.c_i1[k] * t;
    }

    *A = a;
    *W = (w > 0.0) ? w : 0.0;
    *P = (p > 0.0) ? p : 0.0;
    *I1 = i1;
}

} // namespace openswmm::chebsec

#endif // OPENSWMM_CHEB_SECTION_HPP
