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

/// Total Chebyshev coefficients a whole SECTION may pack, across every piece
/// and field, into the shared pool ChebSection::coef.
///
/// @note **Measured, not assumed** (promptperf.md Phase B). The worst case
///       across the full shape catalog this project has built up — every
///       legacy-reconstructed shape (EGG/HORSESHOE/GOTHIC/CATENARY/
///       SEMIELLIPTICAL/BASKETHANDLE/SEMICIRCULAR/ARCH/HORIZ_ELLIPSE/
///       VERT_ELLIPSE), a benched box+semicircle, and a 48-gon polygon that
///       exhausts kMaxPieces — is **648 coefficients** (the 48-gon), counting
///       the padding chebAll()/chebAllOfA() need (see ChebPiece::off_a).
///       2048 carries about 3.2x headroom over that measurement. compile()
///       returns error 9 rather than silently truncating a section that
///       genuinely needs more — the same "fail loudly" contract kMaxPieces
///       already has.
constexpr int kMaxPoolCoeff = 2048;

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

    /// A(y_hi) from this piece's own series, so [a_lo, a_hi] is exactly the
    /// range the fit below covers.
    double a_hi = 0.0;
    double inv_span_a = 0.0;  ///< 1 / (a_hi - a_lo)

    double rho_a = 0.0;  ///< measured Bernstein parameter for A (diagnostic)

    /// Retained coefficient counts. n_u == 0 means this piece has no compiled
    /// inverse and chebYofA must fall back to its Newton solve — a safety
    /// valve for a shape whose inverse does not resolve, never the normal path.
    uint16_t n_a = 0, n_w = 0, n_p = 0, n_i1 = 0, n_u = 0;

    /// Offsets into the section's shared ChebSection::coef pool (promptperf.md
    /// Phase B — packed, variable-length storage in place of seven
    /// kMaxChebCoeff-wide arrays per piece).
    ///
    /// @note off_a/off_w/off_p/off_i1 point into a COMMON per-piece block: each
    ///       of those four fields is packed with trailing zeros out to
    ///       max(n_a,n_w,n_p,n_i1) for THIS piece — not kMaxChebCoeff — so
    ///       chebAll()/chebAllOfA() and ChebSectionBatch.hpp's chebEval2()
    ///       (chebARofY, chebAWofY) can keep reading a field past its OWN
    ///       retained count, exactly the trick they always used, just over a
    ///       per-piece window instead of a fixed 32-wide one. off_da/off_dp/
    ///       off_u need no such padding — nothing reads them in a shared loop
    ///       alongside a different field, so they pack at their own exact
    ///       length. compile() is the only place that must keep this in sync;
    ///       see its packing pass.
    uint16_t off_a = 0, off_w = 0, off_p = 0, off_i1 = 0;
    uint16_t off_da = 0;  ///< d(c_a)/du, n_a-1 terms (chebDeriv's convention)
    uint16_t off_dp = 0;  ///< d(c_p)/du, n_p-1 terms
    uint16_t off_u = 0;   ///< fit variable u as a function of t(A), n_u terms

    // -- the compiled INVERSE, u(A) ----------------------------------------
    //
    // Everything above answers "given depth, what is ...". The solver spends
    // most of its geometry time asking the opposite question, because area is
    // what the routing conserves. Phase 4-5B left that direction as a runtime
    // Newton solve and it measured 419 ns against 15.7 for a forward
    // accessor and 4.9 for the legacy inverse TABLE — 68% of all non-idle
    // samples on a real network. c_u (off_u/n_u above) compiles it away.
    //
    // Only u(A) is fitted, not y(A)/W(A)/P(A)/I1(A) separately. Once u is in
    // hand, y is a closed form (mapSofU, no series at all) and W/P/I1 are the
    // series ALREADY here, evaluated at that u — so one extra series buys the
    // whole A-parametrized family, in exchange for saving one shared basis
    // recurrence on the compound accessors only. Revisit if a future phase
    // measures that recurrence.

    /// Root order of the inverse coordinate: 1 identity, 2 sqrt, 3 cbrt.
    ///
    /// @details A is not generally analytic in the coordinate the FORWARD map
    ///          uses, and the exponent is not the forward tag. Near an end
    ///          where the boundary is tangent to horizontal (forward tag 1.5)
    ///          A vanishes like the CUBE of the fit variable, so u ~ dA^(1/3).
    ///          Near a corner where the width merely goes to zero linearly (a
    ///          V-notch invert, a pointed crown — forward tag 1.0, analytic
    ///          in y) A vanishes like the SQUARE, so u ~ dA^(1/2). Everywhere
    ///          else dA/du is bounded away from zero and u is analytic in A.
    ///          That last case is the common one; the map is the identity and
    ///          costs nothing.
    int8_t inv_k = 1;

    /// Whether the branch point sits at the piece's UPPER end (a_hi) rather
    /// than its lower one. compile() guarantees at most one end is singular,
    /// splitting the piece if both are — the same normalization the forward
    /// map already relies on, applied in the area ordinate.
    bool inv_at_hi = false;
};

/**
 * @brief A whole cross-section, compressed. POD and trivially copyable so it
 *        can be memcpy'd to device memory.
 *
 * @note **Size (promptperf.md Phase B).** The pre-Phase-B layout gave every
 *       piece its own seven kMaxChebCoeff(32)-wide arrays regardless of
 *       actual content — about 45 kB per section, 43 kB of it coefficients
 *       nobody's compiled boundary ever needed (typical content is 2-12
 *       pieces at well under 32 coefficients per field). Packed, a section
 *       is ~18.5 kB.
 * @note **Dedup (promptperf.md Phase C).** Contrary to an earlier draft of
 *       this note (and of promptperf.md's own Phase C premise text): built-in
 *       shapes under XSECT_GEOMETRY EXACT do NOT each compile their own
 *       section — `PostParseResolver.cpp` has memoized them by
 *       (shape, y_full, w_max) since the original Phase 5 commit
 *       (`6a3ef934`), predating both Phase B and promptperf.md itself.
 *       POLYGON links are separately memoized by (curve, scale, is_open) —
 *       Phase C's own audit found and fixed a real gap in that second key
 *       (it was missing `is_open`, which chebsec::compile() bakes into the
 *       result — see PostParseResolver.cpp's POLYGON block). Measured on
 *       Bellinge (953 CIRCULAR conduits, real network): dedup collapses
 *       them to **46 unique compiled sections** — not 953 — so the total
 *       compiled geometry footprint is ~46 x 18.5 kB ≈ 0.85 MB, comfortably
 *       L2-resident, not the ~18-43 MB an assumed one-section-per-conduit
 *       model would suggest. (Run `[OPTIONS] XSECT_GEOMETRY EXACT` and read
 *       the .rpt's "Compiled Cross-Sections" line for any given network.)
 *       This likely explains why Phase B's own measured network-level
 *       timing delta was muted (~6%, within run-to-run noise) despite a
 *       2.4x per-section size reduction: the working set was already small
 *       before Phase B ran, because dedup — not packing — is what keeps a
 *       real network's compiled geometry off of cache pressure. Phase B
 *       replaces the seven fixed arrays with
 *       offset/length pairs (ChebPiece::off_a etc.) into ONE shared pool
 *       below, sized from a measured worst case across the shape catalog
 *       (kMaxPoolCoeff's own note) rather than the theoretical maximum every
 *       field of every piece could demand. The type stays POD and trivially
 *       copyable — asserted
 *       below — so the device-mirroring contract is unchanged; only the
 *       layout inside the struct did.
 *
 *       Four of the seven series hold the fields themselves; the other three
 *       were each added against a measured profile rather than on principle.
 *       c_u (off_u/n_u) is the compiled inverse u(A), added once profiling
 *       showed the runtime inversion it replaces was 68% of the whole EXACT
 *       solver — and it is deliberately ONE series and not four, see the note
 *       on ChebPiece::a_hi for why y/W/P/I1 in the area ordinate are
 *       compositions rather than fits of their own. c_da and c_dp (off_da/
 *       off_dp) are the derivative series, stored because every consumer was
 *       rebuilding them per call; that was 35 ns of chebRdPdA's 106, and
 *       getdSdA was in turn the largest single item in the profile once the
 *       inversion was gone.
 */
struct ChebSection {
    int  n_pieces = 0;
    int  n_coef = 0;     ///< used length of coef[] below (diagnostic + the
                         ///< pack cursor compile() advances as it fills it)
    bool is_open = false;

    double y_full = 0.0;   ///< full depth (ft)
    double w_max = 0.0;    ///< widest top width (ft)
    double yw_max = 0.0;   ///< depth at which w_max occurs (ft)
    double a_full = 0.0;   ///< area at y_full (ft^2)
    double r_full = 0.0;   ///< hydraulic radius at y_full (ft)
    double s_full = 0.0;   ///< section factor at y_full
    double s_max = 0.0;    ///< maximum section factor
    double a_max = 0.0;    ///< area at which s_max occurs (ft^2)

    /// Wetted perimeter AT y_full (ft).
    /// @note NOT simply "the fitted P(y) series evaluated at its own y_full
    ///       endpoint". For a CLOSED shape with a flat/cornered crown (Puiseux
    ///       exponent 1.0 at the top — see XSectBoundary.hpp), P(y) has a real
    ///       physical jump at y_full: just below the crown the water hasn't
    ///       touched the top wall yet (wetted perimeter = bottom + side walls
    ///       only), while AT y_full the section is pressurized-full (every
    ///       boundary element wetted). The fit is built from strictly-interior
    ///       samples and only ever captures the BELOW-crown limit, so
    ///       extrapolating it to u=1 silently returns the wrong (smaller)
    ///       perimeter — undercounting r_full/s_full by exactly the missing
    ///       crown width. A smooth ROUND crown (exponent 1.5) has no such
    ///       jump — the wetted arc already sweeps almost the entire circle as
    ///       y -> y_full, so the fitted limit is correct there, which is why
    ///       this was never caught by the circle-only Phase 4 test suite.
    ///       compile() sets this to the TRUE closed-loop perimeter (every
    ///       input BElem's arcLength summed, unconditionally) for a closed
    ///       shape, and to the ordinary fitted limit for an open one (an open
    ///       top is never a wall, so there is no discontinuity to correct).
    ///       chebPofY/chebAll read this field directly for y >= y_full rather
    ///       than re-deriving it, so every caller sees the same value.
    double p_full = 0.0;

    ChebPiece piece[kMaxPieces];

    /// Shared packed coefficient pool for every piece's c_a/c_w/c_p/c_i1/
    /// c_da/c_dp/c_u — see ChebPiece::off_a and kMaxPoolCoeff.
    double coef[kMaxPoolCoeff]{};
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

/// Evaluate sum c_k T_k(2u-1) for u in [0,1] by the FORWARD T_k recurrence.
///
/// @note **Not Clenshaw**, deliberately, and this is worth 2x. Clenshaw's step
///       `b1 = x2*b1 - b2 + c[k]` puts two dependent arithmetic ops on the
///       critical path per coefficient and carries the accumulation IN that
///       chain, so a degree-n series costs n times the latency of both. The
///       forward form advances `T_k = x2*T_{k-1} - T_{k-2}` with ONE fused
///       op and accumulates `a += c[k]*T_k` OFF the chain, where it overlaps
///       freely. Measured on a compiled circular pipe (9 coefficients,
///       Phase 5B): Clenshaw 9.44 ns/eval against 4.76 ns forward.
///
///       Clenshaw is normally preferred for stability, and that preference
///       does not apply here: |T_k(x)| <= 1 for |x| <= 1, so the forward
///       recurrence cannot amplify, and u is confined to [0,1] by chebUofY.
///       chebAll() already used the forward form for exactly this reason
///       (see its own note); this makes the single-field accessors agree
///       with it instead of reaching the same value by a different rounding.
OPENSWMM_KERNEL_FN double chebEval(const double* c, int n, double u) noexcept {
    if (n <= 0) return 0.0;
    const double x = 2.0 * u - 1.0;
    const double x2 = 2.0 * x;
    double a = c[0];
    if (n > 1) a += c[1] * x;
    double t_prev = 1.0, t_cur = x;
    for (int k = 2; k < n; ++k) {
        const double t = x2 * t_cur - t_prev;
        t_prev = t_cur;
        t_cur = t;
        a += c[k] * t;
    }
    return a;
}

/// Pointer to the start of one packed coefficient run in the section's
/// shared pool. @p off is one of ChebPiece's off_* fields.
/// @note Phase B's one new indirection: every accessor below reads a piece's
///       series through this instead of a `pc.c_x` array member.
OPENSWMM_KERNEL_FN const double* chebCoef(const ChebSection& s, int off) noexcept {
    return &s.coef[off];
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
///
/// @note Linear scan over y_lo, exiting at the first piece above @p y. With
///       at most 24 pieces this is 1-3 compares; a binary search and a
///       per-cell cached piece index were both measured SLOWER during
///       Phase 4 and must not be reintroduced.
///
/// @note Phase 5B also measured, and REJECTED, two further variants of this
///       scan: mirroring the piece lower bounds into a contiguous array in
///       the section header (so the scan stops striding by sizeof(ChebPiece))
///       and replacing the early exit with a branch-free `p += (y >= ...)`
///       count. In ISOLATION the branch-free contiguous form looks like a
///       clear win — 2.7 ns against 7.6 for a 5-piece section, because the
///       early exit is a data-dependent branch. In CONTEXT it lost in every
///       case measured: 37.4 ns/eval against this loop's 26.2 inside
///       chebAll, and 17.6 against 15.8 inside chebAofY. The early exit's
///       saved iterations are worth more than the mispredictions cost, since
///       the surrounding evaluation gives the processor plenty to do while
///       the branch resolves — and counting always pays for every piece.
///       Keep the early exit.
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

/// Index of the piece containing area @p a.
///
/// @note Same linear scan as chebPieceOfY, in the area ordinate. The two stay
///       consistent because ChebPiece::a_lo is read off the piece's own
///       fitted series rather than from evalExact(y_lo), so the seams agree
///       exactly with what chebAofY returns there.
OPENSWMM_KERNEL_FN int chebPieceOfA(const ChebSection& s, double a) noexcept {
    int p = 0;
    for (int i = 1; i < s.n_pieces; ++i) {
        if (a >= s.piece[i].a_lo) p = i;
        else break;
    }
    return p;
}

/// Fit variable u for area @p a within piece @p pc, from the COMPILED
/// inverse. Undefined unless pc.n_u > 0 — callers check.
///
/// @details Two coordinate changes, both cheap. First A is normalized onto
///          [0,1] and taken to the inv_k-th root at whichever end carries the
///          branch point, which is what makes u analytic in the result.
///          Then one Chebyshev series. The root is a hardware sqrt or cbrt,
///          never an inverse trig call: compile() guarantees at most one end
///          of a piece is singular, exactly as it does for the forward map.
OPENSWMM_KERNEL_FN double chebUofA(const ChebSection& s, const ChebPiece& pc,
                                   double a) noexcept {
    double sa = (a - pc.a_lo) * pc.inv_span_a;
    if (sa < 0.0) sa = 0.0;
    else if (sa > 1.0) sa = 1.0;

    double t;
    if (pc.inv_at_hi) {
        const double v = 1.0 - sa;
        t = 1.0 - ((pc.inv_k == 1) ? v
                                   : (pc.inv_k == 2) ? std::sqrt(v)
                                                     : std::cbrt(v));
    } else {
        t = (pc.inv_k == 1) ? sa
                            : (pc.inv_k == 2) ? std::sqrt(sa)
                                              : std::cbrt(sa);
    }

    double u = chebEval(chebCoef(s, pc.off_u), pc.n_u, t);
    if (u < 0.0) u = 0.0;
    else if (u > 1.0) u = 1.0;
    return u;
}

/// Depth from the fit variable — closed form, no series.
/// @note This is why only u(A) needs fitting and y(A) does not.
OPENSWMM_KERNEL_FN double chebYofU(const ChebPiece& pc, double u) noexcept {
    return pc.y_lo + mapSofU(u, pc.exp_lo, pc.exp_hi) / pc.inv_span;
}

OPENSWMM_KERNEL_FN double chebAofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (y >= s.y_full) return s.a_full;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    return chebEval(chebCoef(s, pc.off_a), pc.n_a, chebUofY(pc, y));
}

OPENSWMM_KERNEL_FN double chebWofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    const double yy = (y >= s.y_full) ? s.y_full : y;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, yy)];
    const double w = chebEval(chebCoef(s, pc.off_w), pc.n_w, chebUofY(pc, yy));
    return (w > 0.0) ? w : 0.0;
}

OPENSWMM_KERNEL_FN double chebPofY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || s.n_pieces <= 0) return 0.0;
    // At or above the crown, P jumps for a closed shape (see ChebSection::
    // p_full) — read the compile()-computed true value rather than
    // extrapolating the fit, which only ever captures the below-crown limit.
    if (y >= s.y_full) return s.p_full;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    const double p = chebEval(chebCoef(s, pc.off_p), pc.n_p, chebUofY(pc, y));
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
        const double i1_full = chebEval(chebCoef(s, top.off_i1), top.n_i1, 1.0);
        return i1_full + s.a_full * (y - s.y_full);
    }
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    return chebEval(chebCoef(s, pc.off_i1), pc.n_i1, chebUofY(pc, y));
}

/// dA/dy from the A series' exact derivative — the same quantity chebWofY
/// returns, reached by an independent route. Agreement between the two is what
/// proves the map Jacobian below is the one the fit actually used.
/// dA/dy on ONE piece, from a derivative series the caller already has.
///
/// @param pc  the piece containing @p y.
/// @param dc  d/du coefficients of the area series — pass
///            chebCoef(s, pc.off_da), which compile() builds once per piece.
/// @param y   depth (ft), which must lie inside @p pc.
///
/// @note Split out of chebdAdY so a caller that evaluates the derivative
///       REPEATEDLY ON ONE PIECE — chebYofA's Newton loop is the only one, and
///       it dominated the whole EXACT profile — can lift the chebDeriv call
///       out of its loop. `dc` depends on the piece alone, never on y, so
///       recomputing it per iteration was pure waste: measured 966 ns/eval
///       against 419 for the same inversion, bit-for-bit identical output.
OPENSWMM_KERNEL_FN double chebdAdYOnPiece(const ChebPiece& pc, const double* dc,
                                          double y) noexcept {
    const double u = chebUofY(pc, y);
    const double dAdu = chebEval(dc, pc.n_a - 1, u);

    // dy/du = span * ds/du. At an end tagged 1.5 the Jacobian vanishes, and so
    // does dA/du (A goes like u^3 there) — the true width is zero at a smooth
    // tangency, so returning 0 is the correct limit rather than a guard.
    const double dydu = mapDsDu(u, pc.exp_lo, pc.exp_hi) / pc.inv_span;
    if (!(dydu > 1.0e-300)) return 0.0;
    return dAdu / dydu;
}

OPENSWMM_KERNEL_FN double chebdAdY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || y >= s.y_full || s.n_pieces <= 0) return 0.0;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    return chebdAdYOnPiece(pc, chebCoef(s, pc.off_da), y);
}

/// dP/dy from the P series' exact derivative — same construction as
/// chebdAdY, over c_p/n_p instead of c_a/n_a.
/// @note Not in the original Phase 4 design; added for Phase 5's getdSdA,
///       which needs dR/dA = 1/P - (A/P^2)*dP/dA analytically (no finite
///       differences) and dP/dA = (dP/dy)/(dA/dy).
OPENSWMM_KERNEL_FN double chebdPdY(const ChebSection& s, double y) noexcept {
    if (y <= 0.0 || y >= s.y_full || s.n_pieces <= 0) return 0.0;
    const ChebPiece& pc = s.piece[chebPieceOfY(s, y)];
    const double u = chebUofY(pc, y);
    const double dPdu = chebEval(chebCoef(s, pc.off_dp), pc.n_p - 1, u);

    const double dydu = mapDsDu(u, pc.exp_lo, pc.exp_hi) / pc.inv_span;
    if (!(dydu > 1.0e-300)) return 0.0;
    return dPdu / dydu;
}

/// Invert A -> y on a KNOWN piece by Newton, safeguarded by bisection.
///
/// @warning **Not the hot path any more, and must not become it again.** This
///          is the fallback for a piece whose inverse did not compile
///          (ChebPiece::n_u == 0) and the reference the compiler itself uses
///          to generate the samples it fits. It costs 419 ns against roughly
///          16 for the compiled inverse; when it dominated the whole EXACT
///          solver it was 68% of all non-idle samples on a real network.
///
/// @note The derivative series is a loop invariant — it belongs to the PIECE
///       and the bracket never leaves it — so it is computed once here rather
///       than inside chebdAdY on every iteration. That alone was 2.3x on this
///       function, bit for bit identical.
OPENSWMM_KERNEL_FN double chebYofASolve(const ChebSection& s,
                                        const ChebPiece& pc, double a) noexcept {
    double lo = pc.y_lo, hi = pc.y_hi;
    double y = 0.5 * (lo + hi);
    const double* ca = chebCoef(s, pc.off_a);
    const double* cda = chebCoef(s, pc.off_da);
    for (int it = 0; it < 40; ++it) {
        const double f = chebEval(ca, pc.n_a, chebUofY(pc, y)) - a;
        if (f > 0.0) hi = y; else lo = y;
        if (hi - lo <= 1.0e-15 * s.y_full) break;

        const double d = chebdAdYOnPiece(pc, cda, y);
        double y_next = (d > 0.0) ? (y - f / d) : (0.5 * (lo + hi));
        if (!(y_next > lo && y_next < hi)) y_next = 0.5 * (lo + hi);
        if (std::fabs(y_next - y) <= 1.0e-15 * s.y_full) { y = y_next; break; }
        y = y_next;
    }
    return y;
}

/// Depth from area — one series evaluation, no iteration.
///
/// @details Area is what the routing conserves, so this is the question the
///          solver asks most; profiling Bellinge under XSECT_GEOMETRY EXACT
///          put this function and its derivative at 68% of all non-idle
///          samples while it was still a Newton solve. It is now the same
///          shape as the forward accessors: locate the piece, change
///          coordinate, evaluate one Chebyshev series.
///
/// @note The legacy engine never inverts anything at runtime either — EPA
///       SWMM ships a SECOND table per shape (Y_Circ beside A_Circ) and reads
///       it. Compiling u(A) is the same trick in a representation that is
///       seven orders more accurate.
OPENSWMM_KERNEL_FN double chebYofA(const ChebSection& s, double a) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (a >= s.a_full) return s.y_full;

    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    if (pc.n_u > 0) return chebYofU(pc, chebUofA(s, pc, a));
    return chebYofASolve(s, pc, a);
}

/// Top width from area.
OPENSWMM_KERNEL_FN double chebWofA(const ChebSection& s, double a) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (a >= s.a_full) return chebWofY(s, s.y_full);

    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    const double u = (pc.n_u > 0) ? chebUofA(s, pc, a)
                                  : chebUofY(pc, chebYofASolve(s, pc, a));
    const double w = chebEval(chebCoef(s, pc.off_w), pc.n_w, u);
    return (w > 0.0) ? w : 0.0;
}

/// Wetted perimeter from area.
OPENSWMM_KERNEL_FN double chebPofA(const ChebSection& s, double a) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0) return 0.0;
    // At and above the crown the section is full: p_full, not the fitted
    // limit. See ChebSection::p_full for why those differ.
    if (a >= s.a_full) return s.p_full;

    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    const double u = (pc.n_u > 0) ? chebUofA(s, pc, a)
                                  : chebUofY(pc, chebYofASolve(s, pc, a));
    const double p = chebEval(chebCoef(s, pc.off_p), pc.n_p, u);
    return (p > 0.0) ? p : 0.0;
}

/// Hydraulic radius from area — A/P with one piece scan, not chebRofY of
/// chebYofA, which walks the piece twice.
///
/// @note The numerator is the CALLER's area, not the area series re-evaluated
///       at the recovered depth. The two agree to the fit's own accuracy, and
///       using the caller's is both cheaper and the more faithful answer: a
///       routing scheme that conserves A is asking what the wetted radius is
///       for the A it is holding.
OPENSWMM_KERNEL_FN double chebRofA(const ChebSection& s, double a) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (a >= s.a_full) return s.r_full;

    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    const double u = (pc.n_u > 0) ? chebUofA(s, pc, a)
                                  : chebUofY(pc, chebYofASolve(s, pc, a));
    const double p = chebEval(chebCoef(s, pc.off_p), pc.n_p, u);
    return (p > 0.0) ? a / p : 0.0;
}

/// Hydrostatic first moment from area.
OPENSWMM_KERNEL_FN double chebI1ofA(const ChebSection& s, double a) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0) return 0.0;
    if (a >= s.a_full) {
        const ChebPiece& top = s.piece[s.n_pieces - 1];
        return chebEval(chebCoef(s, top.off_i1), top.n_i1, 1.0);
    }
    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    const double u = (pc.n_u > 0) ? chebUofA(s, pc, a)
                                  : chebUofY(pc, chebYofASolve(s, pc, a));
    return chebEval(chebCoef(s, pc.off_i1), pc.n_i1, u);
}

/// Hydraulic radius and dP/dA at area @p a, from ONE piece scan.
///
/// @details Everything getdSdA needs. Returns false when the piece cannot
///          support the derivative (full section, zero perimeter, or a
///          vanishing dA/du at a tangency), leaving the caller to fall back
///          to its finite-difference form.
///
/// @note dP/dA is formed as (dP/du)/(dA/du), NOT as (dP/dy)/(dA/dy). The map
///       Jacobian cancels identically between numerator and denominator, so
///       taking the ratio in the fit variable skips mapDsDu entirely — and,
///       more usefully, avoids dividing each derivative separately by a
///       Jacobian that vanishes at a horizontal tangency. Same value, better
///       conditioned exactly where the old form was worst.
OPENSWMM_KERNEL_FN bool chebRdPdA(const ChebSection& s, double a,
                                  double* R, double* dPdA) noexcept {
    if (a <= 0.0 || s.n_pieces <= 0 || a >= s.a_full) return false;

    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    const double u = (pc.n_u > 0) ? chebUofA(s, pc, a)
                                  : chebUofY(pc, chebYofASolve(s, pc, a));

    const double p = chebEval(chebCoef(s, pc.off_p), pc.n_p, u);
    if (!(p > 0.0)) return false;

    const double dAdu = chebEval(chebCoef(s, pc.off_da), pc.n_a - 1, u);
    if (!(dAdu > 0.0)) return false;
    const double dPdu = chebEval(chebCoef(s, pc.off_dp), pc.n_p - 1, u);

    *R = a / p;
    *dPdA = dPdu / dAdu;
    return true;
}

/**
 * @brief FUSED inverse evaluation — depth, width, perimeter and first moment
 *        from area in one piece scan, one coordinate change and one shared
 *        basis recurrence.
 *
 * @details The mirror of chebAll, and the entry point for any solver that
 *          carries area as its state variable (the finite-volume path does).
 *          Calling chebYofA and then chebAll would walk the piece twice and
 *          run two basis recurrences; this runs one of each. Every
 *          redundant-evaluation bug found on this project has had that shape.
 *
 * @note Pass nullptr for any output not wanted — the cost of a field is its
 *       dot product, and skipping it skips that.
 */
OPENSWMM_KERNEL_FN void chebAllOfA(const ChebSection& s, double a,
                                   double* y, double* W, double* P,
                                   double* I1) noexcept {
    if (s.n_pieces <= 0 || a <= 0.0) {
        if (y) *y = 0.0;
        if (W) *W = 0.0;
        if (P) *P = 0.0;
        if (I1) *I1 = 0.0;
        return;
    }
    if (a >= s.a_full) {
        const ChebPiece& top = s.piece[s.n_pieces - 1];
        if (y) *y = s.y_full;
        if (W) { const double w = chebEval(chebCoef(s, top.off_w), top.n_w, 1.0);
                 *W = (w > 0.0) ? w : 0.0; }
        if (P) *P = s.p_full;
        if (I1) *I1 = chebEval(chebCoef(s, top.off_i1), top.n_i1, 1.0);
        return;
    }

    const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
    const double u = (pc.n_u > 0) ? chebUofA(s, pc, a)
                                  : chebUofY(pc, chebYofASolve(s, pc, a));
    if (y) *y = chebYofU(pc, u);

    const double x = 2.0 * u - 1.0;
    const double x2 = 2.0 * x;

    // Trip count is the longest REQUESTED field. Reading past a field's own
    // retained count is safe and deliberate — off_w/off_p/off_i1 are packed
    // as one common per-piece block, zero-padded out to max(n_a,n_w,n_p,n_i1)
    // for THIS piece (see ChebPiece::off_a) — so the loop carries no
    // per-field length test, exactly as chebAll does.
    int nmax = 0;
    if (W && pc.n_w > nmax) nmax = pc.n_w;
    if (P && pc.n_p > nmax) nmax = pc.n_p;
    if (I1 && pc.n_i1 > nmax) nmax = pc.n_i1;

    const double* cw = chebCoef(s, pc.off_w);
    const double* cp = chebCoef(s, pc.off_p);
    const double* ci1 = chebCoef(s, pc.off_i1);
    double aw = cw[0], ap = cp[0], ai = ci1[0];
    if (nmax > 1) {
        aw += cw[1] * x;
        ap += cp[1] * x;
        ai += ci1[1] * x;
    }
    double t_prev = 1.0, t_cur = x;
    for (int k = 2; k < nmax; ++k) {
        const double t = x2 * t_cur - t_prev;
        t_prev = t_cur;
        t_cur = t;
        aw += cw[k] * t;
        ap += cp[k] * t;
        ai += ci1[k] * t;
    }

    if (W) *W = (aw > 0.0) ? aw : 0.0;
    if (P) *P = (ap > 0.0) ? ap : 0.0;
    if (I1) *I1 = ai;
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
        *W = chebEval(chebCoef(s, top.off_w), top.n_w, 1.0);
        // s.p_full, not the fitted extrapolation — see ChebSection::p_full.
        *P = s.p_full;
        *I1 = chebEval(chebCoef(s, top.off_i1), top.n_i1, 1.0) +
              s.a_full * (y - s.y_full);
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
    // off_a/off_w/off_p/off_i1 are packed as one common per-piece block,
    // zero-padded out to exactly this max (see ChebPiece::off_a) — n here IS
    // that pad boundary, computed the same way compile() computed it.

    // Interleaved: the basis term is consumed by all four fields the moment it
    // is produced, so it stays in a register. Materializing T_k into an array
    // first and dotting each field over its own (shorter) length was MEASURED
    // SLOWER — 279 ms against 243 — because the spill and reload cost more than
    // the surplus multiply-adds it saves. Surplus terms are harmless: the pool
    // is zero-initialized and every field's tail out to n is a packed zero.
    const double* ca = chebCoef(s, pc.off_a);
    const double* cw = chebCoef(s, pc.off_w);
    const double* cp = chebCoef(s, pc.off_p);
    const double* ci1 = chebCoef(s, pc.off_i1);
    double a = ca[0], w = cw[0], p = cp[0], i1 = ci1[0];
    if (n > 1) {
        a += ca[1] * x;
        w += cw[1] * x;
        p += cp[1] * x;
        i1 += ci1[1] * x;
    }
    double t_prev = 1.0, t_cur = x;
    for (int k = 2; k < n; ++k) {
        const double t = x2 * t_cur - t_prev;
        t_prev = t_cur;
        t_cur = t;
        a += ca[k] * t;
        w += cw[k] * t;
        p += cp[k] * t;
        i1 += ci1[k] * t;
    }

    *A = a;
    *W = (w > 0.0) ? w : 0.0;
    *P = (p > 0.0) ? p : 0.0;
    *I1 = i1;
}

} // namespace openswmm::chebsec

#endif // OPENSWMM_CHEB_SECTION_HPP
