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
 * @file ChebSection.cpp
 * @ingroup engine_hydraulics
 */

#include "ChebSection.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <vector>

namespace openswmm::chebsec {

using xsboundary::BElem;
using xsboundary::CriticalHeight;
using xsboundary::ExactProps;

// ===========================================================================
// §4a — Chebyshev numerical routines
// ===========================================================================

double chebNode(int n, int j) noexcept {
    const double theta = kPi * (2.0 * static_cast<double>(j) + 1.0) /
                         (2.0 * static_cast<double>(n));
    return 0.5 * (1.0 + std::cos(theta));
}

void chebFitSamples(const double* f, int n, double* c) noexcept {
    for (int k = 0; k < n; ++k) c[k] = 0.0;
    if (n <= 0) return;
    const double scale = 2.0 / static_cast<double>(n);
    for (int k = 0; k < n; ++k) {
        double sum = 0.0;
        for (int j = 0; j < n; ++j) {
            const double theta = kPi * (2.0 * static_cast<double>(j) + 1.0) /
                                 (2.0 * static_cast<double>(n));
            sum += f[j] * std::cos(static_cast<double>(k) * theta);
        }
        c[k] = scale * sum;
    }
    c[0] *= 0.5;
}

void chebFit(const std::function<double(double)>& f, int n, double* c) {
    if (n <= 0) return;
    std::vector<double> samples(static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j) {
        samples[static_cast<std::size_t>(j)] = f(chebNode(n, j));
    }
    chebFitSamples(samples.data(), n, c);
}

int chebChop(double* c, int n, double tol) {
    if (n <= 0) return 0;
    double cmax = 0.0;
    for (int i = 0; i < n; ++i) cmax = std::max(cmax, std::fabs(c[i]));
    if (!(cmax > 0.0)) {
        for (int i = 1; i < n; ++i) c[i] = 0.0;
        return 1;
    }
    const double thr = tol * cmax;
    int m = n;
    while (m > 1 && std::fabs(c[m - 1]) <= thr) --m;
    for (int i = m; i < n; ++i) c[i] = 0.0;
    return m;
}

double bernsteinRho(const double* c, int n) noexcept {
    if (n < 4) return 0.0;
    double cmax = 0.0;
    for (int i = 0; i < n; ++i) cmax = std::max(cmax, std::fabs(c[i]));
    if (!(cmax > 0.0)) return 0.0;

    // Two things live below this floor and both must be excluded: the
    // structural zeros of an even/odd function (log of which is -inf, the
    // documented NaN trap) and the flat numerical noise tail (which would drag
    // the slope toward zero and rho toward 1, forcing endless subdivision).
    const double floor_abs = cmax * 1.0e-12;

    double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0;
    int m = 0;
    for (int k = 1; k < n; ++k) {
        const double a = std::fabs(c[k]);
        if (a <= floor_abs) continue;
        const double x = static_cast<double>(k);
        const double y = std::log(a);
        sx += x; sy += y; sxx += x * x; sxy += x * y;
        ++m;
    }
    if (m < 3) return 0.0;

    const double dm = static_cast<double>(m);
    const double denom = dm * sxx - sx * sx;
    if (!(std::fabs(denom) > 0.0)) return 0.0;
    const double slope = (dm * sxy - sx * sy) / denom;
    if (!std::isfinite(slope)) return 0.0;
    return std::exp(-slope);
}

// ===========================================================================
// §4b — the compiler
// ===========================================================================

namespace {

/// Sample every field once per node — evalExact() is the expensive call, so it
/// is made ONCE per node and the four fields are split out, rather than four
/// times with three results discarded.
bool fitPieceAtDegree(ChebPiece& pc, const BElem* elems, int n, int deg) {
    double sa[kMaxChebCoeff], sw[kMaxChebCoeff];
    double sp[kMaxChebCoeff], si[kMaxChebCoeff];

    const double span = (pc.y_hi - pc.y_lo);
    for (int j = 0; j < deg; ++j) {
        const double u = chebNode(deg, j);
        const double s = mapSofU(u, pc.exp_lo, pc.exp_hi);
        const double y = pc.y_lo + s * span;
        const ExactProps e = xsboundary::evalExact(elems, n, y);
        sa[j] = e.area;
        sw[j] = e.width;
        sp[j] = e.perim;
        si[j] = e.i1;
    }

    chebFitSamples(sa, deg, pc.c_a);
    chebFitSamples(sw, deg, pc.c_w);
    chebFitSamples(sp, deg, pc.c_p);
    chebFitSamples(si, deg, pc.c_i1);

    pc.n_a = chebChop(pc.c_a, deg, kFitTol);
    pc.n_w = chebChop(pc.c_w, deg, kFitTol);
    pc.n_p = chebChop(pc.c_p, deg, kFitTol);
    pc.n_i1 = chebChop(pc.c_i1, deg, kFitTol);

    // Derivative series belong to the piece, so build them once here — every
    // caller used to rebuild them per evaluation. Must come after the chops:
    // differentiating the untruncated series would leave the derivative
    // carrying a tail its own field no longer has.
    chebDeriv(pc.c_a, pc.n_a, pc.c_da);
    chebDeriv(pc.c_p, pc.n_p, pc.c_dp);

    // "Resolved" means the chop found a negligible tail in EVERY field, i.e.
    // the degree was more than enough.
    return pc.n_a < deg && pc.n_w < deg && pc.n_p < deg && pc.n_i1 < deg;
}

/// Fit at 8, then 16, then 32 coefficients, stopping as soon as the series
/// resolves. Returns true when it resolved within kMaxChebCoeff.
bool fitPiece(ChebPiece& pc, const BElem* elems, int n) {
    bool resolved = false;
    for (int deg = 8; deg <= kMaxChebCoeff; deg *= 2) {
        resolved = fitPieceAtDegree(pc, elems, n, deg);
        if (resolved) break;
    }
    pc.rho_a = bernsteinRho(pc.c_a, pc.n_a);
    return resolved;
}

/// A must be non-decreasing. Checked on dA/du, which carries the same sign as
/// dA/dy because every coordinate map here is increasing (ds/du >= 0).
///
/// The threshold is tied to the fit's OWN truncation error rather than being
/// an absolute: the series is deliberately chopped at kFitTol, and
/// differentiating a Chebyshev series amplifies coefficient error by about
/// n^2, so dA/du inherits noise of roughly kFitTol * n^2 * |c|. Demanding
/// strict positivity below that floor would reject fits solely for their own
/// rounding — which is precisely what happened when the design point moved off
/// machine precision. What the guard is actually for is a REAL reversal, orders
/// above this floor, which would make y(A) multi-valued.
bool pieceIsMonotone(const ChebPiece& pc) {
    const double* dc = pc.c_da;   // built by fitPieceAtDegree
    const int nd = pc.n_a - 1;
    if (nd < 1) return true;                    // constant A: flat, not falling
    double scale = 0.0;
    for (int i = 0; i < pc.n_a; ++i) scale = std::max(scale, std::fabs(pc.c_a[i]));
    const double n2 = static_cast<double>(pc.n_a) * static_cast<double>(pc.n_a);
    const double tol = -kFitTol * n2 * std::max(scale, 1.0);
    for (int i = 0; i <= 200; ++i) {
        const double u = static_cast<double>(i) / 200.0;
        if (chebEval(dc, nd, u) < tol) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// The compiled inverse, u(A)
// ---------------------------------------------------------------------------

/// Root order of the inverse coordinate at one end of a piece: 1, 2 or 3.
///
/// @details A is analytic in the fit variable u by construction, but u is NOT
///          generally analytic in A — the inverse acquires its own branch
///          point wherever dA/du vanishes, and the order of that zero sets the
///          root the inverse coordinate needs. Two mechanisms produce one:
///
///          * a horizontal tangency (forward tag 1.5). The map already puts
///            y - y_lo ~ u^2 there and W ~ sqrt(y - y_lo) ~ u, so
///            dA/du = W dy/du ~ u^2 and A vanishes like u^3. Order 3.
///          * a corner where the width simply goes to zero linearly — a
///            V-notch invert, a pointed crown. The forward tag is 1.0 there
///            (A is perfectly analytic in y, which is why Phase 3 tags it so),
///            dy/du is bounded away from zero, and A vanishes like u^2.
///            Order 2.
///
///          Everywhere else W is bounded away from zero and so is dA/du.
///          Order 1, identity map, no transcendental at all.
///
/// @note The second case is why this cannot simply read exp_lo/exp_hi. The
///       forward exponent tag answers "is A analytic in y", and a pointed
///       crown answers yes; the inverse asks a different question and gets a
///       different answer at the same point.
///
/// @note The zero-width test is relative (1e-6 of the piece's own width
///       scale) and therefore approximate. A width that is genuinely tiny but
///       nonzero at an end reads as order 2; the fit then either resolves
///       anyway or fails to, and failing to is safe — see fitPieceInverse.
int inverseRootOrder(const ChebPiece& pc, bool at_hi) {
    const double e = at_hi ? pc.exp_hi : pc.exp_lo;
    if (e > kExpSplit) return 3;

    double w_ref = 0.0;
    for (int i = 0; i < pc.n_w; ++i) w_ref = std::max(w_ref, std::fabs(pc.c_w[i]));
    if (!(w_ref > 0.0)) return 2;

    const double w_end = chebEval(pc.c_w, pc.n_w, at_hi ? 1.0 : 0.0);
    return (w_end > 1.0e-6 * w_ref) ? 1 : 2;
}

/// Solve A(u) = a for u on one piece. Compile-time only — the runtime path is
/// the fitted series this feeds.
///
/// @note Bisection carries the convergence, Newton only accelerates it. Near
///       a singular end dA/du vanishes, so Newton's step is unusable there
///       and the bracket is what actually delivers u; the bracket halves in
///       u regardless of how flat A is, which is exactly the property needed.
double uOfAOnPiece(const ChebPiece& pc, const double* dc, double a) {
    double lo = 0.0, hi = 1.0, u = 0.5;
    for (int it = 0; it < 100; ++it) {
        const double f = chebEval(pc.c_a, pc.n_a, u) - a;
        if (f > 0.0) hi = u; else lo = u;
        if (hi - lo <= 1.0e-16) break;

        const double d = chebEval(dc, pc.n_a - 1, u);
        double un = (d > 0.0) ? (u - f / d) : (0.5 * (lo + hi));
        if (!(un > lo && un < hi)) un = 0.5 * (lo + hi);
        if (std::fabs(un - u) <= 1.0e-16) { u = un; break; }
        u = un;
    }
    return u;
}

double ipow123(double v, int k) {
    return (k == 1) ? v : (k == 2) ? (v * v) : (v * v * v);
}

/// Sampling degree for the inverse fit on an IDENTITY-mapped piece, where the
/// series converges cleanly and the usual chop applies.
constexpr int kInvFitDeg = 24;

/// Sampling degree and chop tolerance on a ROOT-MAPPED piece.
///
/// @note **Both are set by measurement, and more of either makes it worse.**
///       On a root-mapped piece the inverse cannot be more accurate than the
///       forward series it inverts: A is chopped at kFitTol, so near a k-th
///       order zero its zero is only clean to that tolerance, and taking the
///       k-th root magnifies the discrepancy into a localized distortion at
///       the singular end. Measured on a circular pipe's invert piece, |du|
///       against the reference inversion is 2.7e-8 from an 8-node fit, 3.9e-8
///       from 12, 9.3e-8 from 16 and 5.1e-7 from 32 — the extra nodes sample
///       the distortion and the interpolant then carries it everywhere.
///       Chopping at kFitTol simply rejects these pieces outright (the
///       circle's own invert and crown both fell back to Newton), so the
///       tolerance is set at the floor the samples actually support.
///
/// @note An adaptive scheme that measured its own error at check points and
///       spent the fewest coefficients reaching the floor WAS built and
///       measured, and it lost to these two constants both ways: with a
///       uniform check grid it kept 19 coefficients for 4.3e-7 of full depth,
///       and with an end-clustered grid it kept 14 for 6.1e-5, against 12 for
///       4.0e-8 here. The floor near the singular end is unrepresentable
///       rather than merely unresolved, so letting the fit chase it either
///       over-fits the distortion or gives up on the rest of the piece.
constexpr int kInvFitDegRoot = 12;
constexpr double kInvRootTol = 1.0e-7;

/// Fit u(t) for one piece, where t is A normalized and rooted at whichever end
/// carries the branch point. Leaves n_u = 0 if that cannot be done, which makes
/// chebYofA fall back to its Newton solve for this piece — slow but correct, so
/// a shape this does not suit degrades in speed and never in answer.
///
/// @note Accuracy delivered: ~4e-8 of full depth on a root-mapped piece and
///       ~1e-11 on an identity-mapped one. Both are far past what this
///       replaces — the legacy inverse table carries ~1.4e-2, and ~4.1e-1
///       below 5% of full depth.
void fitPieceInverse(ChebPiece& pc) {
    pc.n_u = 0;
    pc.inv_k = 1;
    pc.inv_at_hi = false;

    const double da = pc.a_hi - pc.a_lo;
    if (!(da > 0.0)) return;
    pc.inv_span_a = 1.0 / da;

    const int k_lo = inverseRootOrder(pc, false);
    const int k_hi = inverseRootOrder(pc, true);

    // Both ends singular in the area ordinate cannot be straightened by a
    // single root map, the same way the forward map cannot straighten a
    // 1.5/1.5 piece. The forward compiler answers that by splitting; here the
    // Newton fallback answers it, because the case does not arise for any
    // shape in the catalogue (test_cheb_section asserts full inverse coverage
    // and will say so if that ever stops being true).
    if (k_lo > 1 && k_hi > 1) return;

    pc.inv_at_hi = (k_hi > 1);
    pc.inv_k = pc.inv_at_hi ? k_hi : k_lo;

    const double* dc = pc.c_da;

    const auto sample = [&](int deg, double* su) {
        for (int j = 0; j < deg; ++j) {
            const double t = chebNode(deg, j);
            const double sa = pc.inv_at_hi ? (1.0 - ipow123(1.0 - t, pc.inv_k))
                                           : ipow123(t, pc.inv_k);
            su[j] = uOfAOnPiece(pc, dc, pc.a_lo + sa * da);
        }
    };

    double su[kInvFitDeg], c[kMaxChebCoeff];

    if (pc.inv_k > 1) {
        sample(kInvFitDegRoot, su);
        chebFitSamples(su, kInvFitDegRoot, c);
        const int m = chebChop(c, kInvFitDegRoot, kInvRootTol);
        for (int i = 0; i < kMaxChebCoeff; ++i) {
            pc.c_u[i] = (i < kInvFitDegRoot) ? c[i] : 0.0;
        }
        pc.n_u = m;
        return;
    }

    for (const int d : {8, 16, kInvFitDeg}) {
        sample(d, su);
        chebFitSamples(su, d, c);
        const int m = chebChop(c, d, kFitTol);
        if (m < d) {
            for (int i = 0; i < kMaxChebCoeff; ++i) pc.c_u[i] = (i < d) ? c[i] : 0.0;
            pc.n_u = m;
            return;
        }
    }
}

} // namespace

int compile(ChebSection& out, const BElem* elems, int n, bool is_open) {
    out = ChebSection{};
    if (n < 1) return 1;

    std::vector<CriticalHeight> crit;
    xsboundary::findCriticalHeights(elems, n, crit);
    if (crit.size() < 2) return 3;

    out.is_open = is_open;
    out.y_full = crit.back().y;
    if (!(out.y_full > 0.0)) return 3;

    // ---- pieces, with adaptive subdivision -------------------------------
    //
    // Subdivision fires when the series does not resolve within kMaxChebCoeff,
    // or when it resolves but its measured Bernstein parameter says a
    // singularity is sitting on the interval.
    //
    // @note The rho < 1.2 test alone is NOT a sufficient trigger, contrary to
    //       a literal reading of the design sketch: a piece where A is exactly
    //       linear (vertical walls) retains two coefficients, which is too few
    //       to fit a decay slope through, so bernsteinRho returns 0 — and 0 is
    //       less than 1.2. Keying on rho alone would bisect the best-behaved
    //       pieces in the section until the depth cap stopped it. Hence the
    //       rho test is guarded by rho > 0 and paired with the resolve test.
    int err = 0;
    std::function<void(double, double, double, double, int)> emit =
        [&](double y_lo, double y_hi, double e_lo, double e_hi, int depth) {
            if (err) return;
            if (!(y_hi - y_lo > 1.0e-13 * out.y_full)) return;   // degenerate

            // NORMALIZATION — no piece may be singular at BOTH ends.
            //
            // This is a structural invariant on the decomposition, not a
            // tuning heuristic. A piece with a sqrt branch point at each end
            // can only be straightened by a map that handles both at once
            // (u = acos(1-2s)/pi), and that inverse trig call is ~90% of the
            // evaluation cost — measured 42.7 ns/eval against 17.9 ns for the
            // same series behind a sqrt map, while the degree-14 polynomial
            // itself is 4.2 ns. Splitting at ANY interior point leaves each
            // half with at most one singular end, so `sqrt` always suffices.
            //
            // Enforcing it here removes the 1.5/1.5 row from the coordinate
            // map entirely: after normalization the only maps a compiled
            // section ever uses are identity and sqrt, and WHICH is decided
            // solely by which end of the interval is singular.
            //
            // The recursion terminates immediately — neither half is
            // singular at both ends — and it deliberately does NOT consume
            // subdivision depth, being a normalization rather than a
            // refinement.
            if (e_lo > kExpSplit && e_hi > kExpSplit) {
                const double mid = 0.5 * (y_lo + y_hi);
                emit(y_lo, mid, e_lo, 1.0, depth);
                emit(mid, y_hi, 1.0, e_hi, depth);
                return;
            }

            ChebPiece pc{};
            pc.y_lo = y_lo;
            pc.y_hi = y_hi;
            pc.exp_lo = e_lo;
            pc.exp_hi = e_hi;
            pc.inv_span = 1.0 / (y_hi - y_lo);
            const bool resolved = fitPiece(pc, elems, n);

            const bool poor = (!resolved) ||
                              (pc.rho_a > 0.0 && pc.rho_a < 1.2);
            if (poor && depth < 3) {
                const double mid = 0.5 * (y_lo + y_hi);
                // The midpoint of a piece is a generic depth: analytic, so it
                // takes the identity tag on both new inner ends.
                emit(y_lo, mid, e_lo, 1.0, depth + 1);
                emit(mid, y_hi, 1.0, e_hi, depth + 1);
                return;
            }

            if (out.n_pieces >= kMaxPieces) { err = 5; return; }
            if (!pieceIsMonotone(pc)) { err = 6; return; }
            out.piece[out.n_pieces++] = pc;
        };

    for (std::size_t i = 0; i + 1 < crit.size(); ++i) {
        emit(crit[i].y, crit[i + 1].y,
             crit[i].exp_above, crit[i + 1].exp_below, 0);
        if (err) return err;
    }
    if (out.n_pieces < 1) return 3;

    // ---- refinement: spend spare piece slots on lower degree --------------
    //
    // The pieces above are the minimum needed for CORRECTNESS (analytic on
    // each). Evaluation cost, though, is dominated by the per-coefficient
    // term, so any leftover slot is worth trading for a shorter series. Two
    // separate savings, both measured:
    //   * a 20-coefficient series costs ~4x an 8-coefficient one;
    //   * a piece with sqrt branch points at BOTH ends needs acos to invert
    //     its map, while each half needs only sqrt — about 5x cheaper.
    //
    // Always splitting the piece with the LARGEST series keeps this
    // independent of the order pieces were built in, and the loop is bounded
    // by kMaxPieces so it cannot spin.
    for (int guard = 0; guard < 2 * kMaxPieces && out.n_pieces < kMaxPieces;
         ++guard) {
        int worst = -1;
        int worst_n = kTargetCoeff;
        for (int p = 0; p < out.n_pieces; ++p) {
            const ChebPiece& c = out.piece[p];
            int nmax = c.n_a;
            if (c.n_w > nmax) nmax = c.n_w;
            if (c.n_p > nmax) nmax = c.n_p;
            if (c.n_i1 > nmax) nmax = c.n_i1;
            if (nmax > worst_n) { worst_n = nmax; worst = p; }
        }
        if (worst < 0) break;                      // every piece under target

        const ChebPiece src = out.piece[worst];
        const double mid = 0.5 * (src.y_lo + src.y_hi);
        if (!(mid - src.y_lo > 1.0e-13 * out.y_full)) break;

        ChebPiece lo{}, hi{};
        lo.y_lo = src.y_lo; lo.y_hi = mid;
        lo.exp_lo = src.exp_lo; lo.exp_hi = 1.0;   // the midpoint is generic
        lo.inv_span = 1.0 / (mid - src.y_lo);
        hi.y_lo = mid; hi.y_hi = src.y_hi;
        hi.exp_lo = 1.0; hi.exp_hi = src.exp_hi;
        hi.inv_span = 1.0 / (src.y_hi - mid);
        fitPiece(lo, elems, n);
        fitPiece(hi, elems, n);

        if (!pieceIsMonotone(lo) || !pieceIsMonotone(hi)) break;

        // If neither half came out shorter than the parent, splitting is not
        // buying anything here and would just burn the remaining budget.
        const int lo_n = std::max({lo.n_a, lo.n_w, lo.n_p, lo.n_i1});
        const int hi_n = std::max({hi.n_a, hi.n_w, hi.n_p, hi.n_i1});
        if (std::max(lo_n, hi_n) >= worst_n) break;

        for (int p = out.n_pieces; p > worst + 1; --p) {
            out.piece[p] = out.piece[p - 1];
        }
        out.piece[worst] = lo;
        out.piece[worst + 1] = hi;
        ++out.n_pieces;
    }

    // ---- per-piece area at the lower end ---------------------------------
    // Taken from the piece's OWN series rather than evalExact(y_lo): y_lo is a
    // critical height, and evalExact is only exact at a generic depth. Reading
    // it from the fit also guarantees the area-ordered scan in chebYofA agrees
    // with chebAofY exactly at the piece seams.
    for (int p = 0; p < out.n_pieces; ++p) {
        ChebPiece& pc = out.piece[p];
        pc.a_lo = (p == 0) ? 0.0 : chebEval(pc.c_a, pc.n_a, 0.0);
        pc.a_hi = chebEval(pc.c_a, pc.n_a, 1.0);
    }

    // ---- the compiled inverse, u(A) --------------------------------------
    // Done last, once the piece set is final: the refinement pass above can
    // still split a piece, and a split changes both the area bracket and the
    // series the inverse is fitted against.
    for (int p = 0; p < out.n_pieces; ++p) fitPieceInverse(out.piece[p]);

    // ---- scalars ---------------------------------------------------------
    // a_full and the perimeter at the crown come from the TOP PIECE evaluated
    // at u = 1, not from evalExact(y_full), for the same reason: y_full is a
    // critical height. On a rectangle, evalExact exactly at the rim finds no
    // transversal crossing on the horizontal soffit and reports half the true
    // area — a silent factor-of-two that would propagate into every scalar
    // below it.
    const ChebPiece& top = out.piece[out.n_pieces - 1];
    out.a_full = chebEval(top.c_a, top.n_a, 1.0);

    // Perimeter at the crown: for a CLOSED shape this is NOT the fitted
    // series extrapolated to u=1. That series was built from strictly
    // interior samples and only ever captures the open-channel-below-crown
    // limit; a flat/cornered crown (Puiseux exponent 1.0) has a real jump at
    // y_full between that limit and the pressurized-full perimeter (every
    // boundary element wetted). A smooth ROUND crown (exponent 1.5) has no
    // such jump, which is why this was invisible to the circle-only test
    // suite. Sum every input element's own arcLength instead — well-defined
    // regardless of the crown's smoothness, since it needs no extrapolation
    // at all. An OPEN shape has no crown to pressurize past, so the fitted
    // limit remains correct there (matches legacy RECT_OPEN's own r_full,
    // which likewise excludes the open top).
    if (is_open) {
        out.p_full = chebEval(top.c_p, top.n_p, 1.0);
    } else {
        out.p_full = 0.0;
        for (int i = 0; i < n; ++i) out.p_full += xsboundary::arcLength(elems[i]);
    }
    out.r_full = (out.p_full > 0.0) ? out.a_full / out.p_full : 0.0;
    out.s_full = out.a_full * std::pow(std::max(out.r_full, 0.0), 2.0 / 3.0);

    constexpr int kScan = 1000;
    out.w_max = 0.0;
    out.yw_max = 0.0;
    out.s_max = 0.0;
    out.a_max = out.a_full;
    for (int i = 1; i <= kScan; ++i) {
        const double y = out.y_full * static_cast<double>(i) /
                         static_cast<double>(kScan);
        const double w = chebWofY(out, y);
        if (w > out.w_max) { out.w_max = w; out.yw_max = y; }

        const double a = chebAofY(out, y);
        const double p = chebPofY(out, y);
        if (a > 0.0 && p > 0.0) {
            const double sfac = a * std::pow(a / p, 2.0 / 3.0);
            if (sfac > out.s_max) {
                out.s_max = sfac;
                if (!is_open) out.a_max = a;
            }
        }
    }
    // An open channel's section factor rises all the way to the top, so its
    // conveyance is single-valued and a_max is simply a_full.
    if (is_open) out.a_max = out.a_full;

    return 0;
}

} // namespace openswmm::chebsec
