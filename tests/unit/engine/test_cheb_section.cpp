/**
 * @file test_cheb_section.cpp
 * @brief Piecewise-Chebyshev section compiler (Phase 4).
 *
 * @details Covers the numerical routines, the coordinate map, and the two
 *          trap-setters that stop a future refactor from quietly undoing the
 *          things that make this converge: splitting at critical heights, and
 *          stretching the coordinate at a square-root end.
 *
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>

#include <chrono>
#include <cmath>
#include <vector>

#include "hydraulics/ChebSection.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "hydraulics/XSectBoundary.hpp"
#include "hydraulics/XSectKernels.hpp"

using namespace openswmm;
using namespace openswmm::chebsec;
using openswmm::xsboundary::BElem;
using openswmm::xsboundary::CriticalHeight;
using openswmm::xsboundary::evalExact;
using openswmm::xsboundary::findCriticalHeights;

namespace {

constexpr double kPiL = 3.14159265358979323846;

BElem arcOf(double cx, double cy, double r, double a0, double a1) {
    BElem e{};
    e.cx = cx; e.cy = cy; e.radius = r; e.a0 = a0; e.a1 = a1;
    e.x0 = cx + r * std::cos(a0); e.y0 = cy + r * std::sin(a0);
    e.x1 = cx + r * std::cos(a1); e.y1 = cy + r * std::sin(a1);
    return e;
}

BElem segOf(double x0, double y0, double x1, double y1) {
    BElem e{};
    e.x0 = x0; e.y0 = y0; e.x1 = x1; e.y1 = y1;
    return e;
}

/// Full circle of diameter d, invert at y = 0.
std::vector<BElem> circleOf(double d) {
    return {arcOf(0.0, 0.5 * d, 0.5 * d, -0.5 * kPiL, 1.5 * kPiL)};
}

/// Box `w` x `h` sitting on a semicircular low-flow channel of radius r.
/// Invert at y = 0; the box floor (springing line) is at y = r.
std::vector<BElem> benchedOf(double w, double h, double r) {
    const double hw = 0.5 * w;
    const double top = r + h;
    return {
        arcOf(0.0, r, r, kPiL, 2.0 * kPiL),
        segOf(r, r, hw, r),
        segOf(hw, r, hw, top),
        segOf(hw, top, -hw, top),
        segOf(-hw, top, -hw, r),
        segOf(-hw, r, -r, r),
    };
}

int totalCoeffs(const ChebSection& s) {
    int t = 0;
    for (int p = 0; p < s.n_pieces; ++p) t += s.piece[p].n_a;
    return t;
}

/// Worst |A_fit - A_exact| over the interior of the depth range.
double maxAreaError(const ChebSection& s, const std::vector<BElem>& b,
                    double lo_frac, double hi_frac, int samples = 400) {
    double worst = 0.0;
    for (int i = 0; i <= samples; ++i) {
        const double f = lo_frac + (hi_frac - lo_frac) *
                                       static_cast<double>(i) /
                                       static_cast<double>(samples);
        const double y = f * s.y_full;
        const double got = chebAofY(s, y);
        const double ref = evalExact(b.data(), static_cast<int>(b.size()), y).area;
        worst = std::max(worst, std::fabs(got - ref));
    }
    return worst;
}

} // namespace

// ---------------------------------------------------------------------------
// §4a numerical routines
// ---------------------------------------------------------------------------

TEST(ChebSection, EvalReproducesKnownPolynomials) {
    // c = {0,1,0,...} is T_1(2u-1) = 2u-1.
    double c[4] = {0.0, 1.0, 0.0, 0.0};
    EXPECT_NEAR(chebEval(c, 4, 0.0), -1.0, 1e-15);
    EXPECT_NEAR(chebEval(c, 4, 0.5), 0.0, 1e-15);
    EXPECT_NEAR(chebEval(c, 4, 1.0), 1.0, 1e-15);
}

TEST(ChebSection, FitThenEvalIsAccurateForASmoothFunction) {
    double c[24];
    chebFit([](double u) { return std::exp(u) * std::sin(3.0 * u); }, 24, c);
    for (int i = 0; i <= 50; ++i) {
        const double u = static_cast<double>(i) / 50.0;
        EXPECT_NEAR(chebEval(c, 24, u), std::exp(u) * std::sin(3.0 * u), 1e-13);
    }
}

TEST(ChebSection, DerivCoefficientsMatchAFiniteDifference) {
    double c[20], dc[20];
    chebFit([](double u) { return std::exp(u) * std::sin(3.0 * u); }, 20, c);
    chebDeriv(c, 20, dc);
    for (double u : {0.15, 0.4, 0.62, 0.88}) {
        const double h = 1e-6;
        const double num = (chebEval(c, 20, u + h) - chebEval(c, 20, u - h)) / (2 * h);
        EXPECT_NEAR(chebEval(dc, 19, u), num, 1e-7) << "at u=" << u;
    }
}

TEST(ChebSection, ChopKeepsThreeOrFewerForAnExactlyLinearField) {
    // Test 10: a piece with vertical walls has A exactly linear in depth, so
    // the series must collapse to a constant + slope.
    double c[32];
    chebFit([](double u) { return 3.0 + 7.0 * u; }, 32, c);
    EXPECT_LE(chebChop(c, 32), 3);
}

TEST(ChebSection, BernsteinRhoMatchesTheAnalyticEllipseParameter) {
    // Test 8. 1/(x^2+a^2) has poles at x = +-ia, so rho = a + sqrt(a^2+1).
    // This function is EVEN, so every odd coefficient is a structural zero —
    // this doubles as the regression test for the log-of-zero NaN trap.
    const double as[4] = {1.0, 0.5, 0.2, 0.1};
    const double want[4] = {2.4142, 1.6180, 1.2198, 1.1050};
    for (int i = 0; i < 4; ++i) {
        const double a = as[i];
        double c[64];
        chebFit([a](double u) {
            const double x = 2.0 * u - 1.0;
            return 1.0 / (x * x + a * a);
        }, 64, c);
        const double rho = bernsteinRho(c, 64);
        ASSERT_TRUE(std::isfinite(rho)) << "NaN trap for a=" << a;
        EXPECT_NEAR(rho, want[i], 0.01 * want[i]) << "a=" << a;
    }
}

// ---------------------------------------------------------------------------
// §4b compile()
// ---------------------------------------------------------------------------

TEST(ChebSection, CircularPipeCompilesToAFewCoefficientsAtTheDesignTolerance) {
    // Test 5. In the two-sided (boundary-angle) coordinate a circle's area is
    // entire, so the coefficients decay faster than geometrically — which is
    // what lets the series be chopped this short at kFitTol.
    const auto circ = circleOf(4.0);
    ChebSection s;
    ASSERT_EQ(compile(s, circ.data(), static_cast<int>(circ.size())), 0);

    // Pieces are cheap and the normalization below deliberately spends them;
    // what matters is that the section stays compact, not that it is one piece.
    EXPECT_LE(s.n_pieces, 8);
    EXPECT_NEAR(s.y_full, 4.0, 1e-12);
    EXPECT_NEAR(s.a_full, kPiL * 4.0, 20.0 * kFitTol * kPiL * 4.0);

    // Accuracy is asserted against the DESIGN POINT (kFitTol), not against
    // machine precision: the series is deliberately chopped early because the
    // last orders cost more than they are worth. See kFitTol's trade table.
    const double err = maxAreaError(s, circ, 1e-4, 1.0 - 1e-4) / s.a_full;
    EXPECT_LT(err, 20.0 * kFitTol)
        << "relative |dA| = " << err << " with " << totalCoeffs(s)
        << " coefficients over " << s.n_pieces << " pieces";
    // ...and still vastly better than the 51-point table it replaces (1.4e-2).
    EXPECT_LT(err, 1.0e-5);
    std::printf("[cheb] circle: %d piece(s), %d A-coefficients, max|dA| = %.3e\n",
                s.n_pieces, totalCoeffs(s), err);
}

TEST(ChebSection, StretchedCoordinateBeatsIdentityByOrders_TrapSetter) {
    // Test 7. A circle is tangent to horizontal at BOTH invert and crown. Fit
    // the same field over the same interval with the identity map and the
    // accuracy must collapse — if this ever stops failing, the coordinate
    // change has been silently removed.
    const auto circ = circleOf(4.0);
    ChebSection s;
    ASSERT_EQ(compile(s, circ.data(), static_cast<int>(circ.size())), 0);
    ASSERT_GT(s.piece[0].exp_lo, 1.25) << "invert must carry the sqrt tag";
    ASSERT_GT(s.piece[s.n_pieces - 1].exp_hi, 1.25) << "crown must carry it too";
    const double good = maxAreaError(s, circ, 1e-4, 1.0 - 1e-4);

    // Deliberately wrong: identity map, full budget.
    double c[kMaxChebCoeff];
    chebFit([&](double u) {
        return evalExact(circ.data(), static_cast<int>(circ.size()), u * 4.0).area;
    }, kMaxChebCoeff, c);
    double bad = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double u = 1e-4 + (1.0 - 2e-4) * static_cast<double>(i) / 400.0;
        const double ref =
            evalExact(circ.data(), static_cast<int>(circ.size()), u * 4.0).area;
        bad = std::max(bad, std::fabs(chebEval(c, kMaxChebCoeff, u) - ref));
    }

    std::printf("[cheb] circle stretched = %.3e, identity = %.3e (%.0f x worse)\n",
                good, bad, bad / std::max(good, 1e-300));
    EXPECT_GT(bad, 1000.0 * good) << "stretched " << good << " vs identity " << bad;
}

TEST(ChebSection, BenchedSectionNeedsTheCriticalHeightSplit_TrapSetter) {
    // Test 6. Box on a semicircular low-flow channel. Split at the springing
    // line it is near-exact; fitted as ONE piece with the same total budget it
    // must be far worse, because the bench kink and the round invert both sit
    // inside the interval.
    const auto b = benchedOf(6.0, 4.0, 1.0);
    ChebSection s;
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
    EXPECT_GE(s.n_pieces, 2) << "must split at the springing line";

    const double split_err = maxAreaError(s, b, 1e-4, 1.0 - 1e-4) / s.a_full;
    EXPECT_LT(split_err, 20.0 * kFitTol)
        << "with " << totalCoeffs(s) << " coefficients";

    double c[kMaxChebCoeff];
    chebFit([&](double u) {
        return evalExact(b.data(), static_cast<int>(b.size()), u * s.y_full).area;
    }, kMaxChebCoeff, c);
    double one_err = 0.0;
    for (int i = 0; i <= 400; ++i) {
        const double u = 1e-4 + (1.0 - 2e-4) * static_cast<double>(i) / 400.0;
        const double ref =
            evalExact(b.data(), static_cast<int>(b.size()), u * s.y_full).area;
        one_err = std::max(one_err, std::fabs(chebEval(c, kMaxChebCoeff, u) - ref));
    }

    std::printf("[cheb] benched split = %.3e (%d coeff, %d pieces), "
                "unsplit = %.3e\n",
                split_err, totalCoeffs(s), s.n_pieces, one_err);
    EXPECT_GT(one_err, 1e-4) << "un-split fit was suspiciously good";
}

namespace {

/// Box `w` x `h` with all four corners rounded by a quarter-circle fillet of
/// radius `r`, CCW from the bottom-left. Every straight-to-arc transition is
/// tangent (a true fillet, not a sharp corner), so the invert and crown are
/// smooth like a circle's (exp = 1.5) while the two mid-height joints, where
/// curvature jumps from 0 to 1/r, are critical but analytic (exp = 1.0) —
/// the same "tangential but curvature-discontinuous" case Phase 3 already
/// documented for a round low-flow channel under a box.
std::vector<BElem> filletedBoxOf(double w, double h, double r) {
    return {
        segOf(r, 0.0, w - r, 0.0),
        arcOf(w - r, r, r, -0.5 * kPiL, 0.0),
        segOf(w, r, w, h - r),
        arcOf(w - r, h - r, r, 0.0, 0.5 * kPiL),
        segOf(w - r, h, r, h),
        arcOf(r, h - r, r, 0.5 * kPiL, kPiL),
        segOf(0.0, h - r, 0.0, r),
        arcOf(r, r, r, kPiL, 1.5 * kPiL),
    };
}

} // namespace

TEST(ChebSection, FilletedCornersTriggerAdaptiveSubdivision) {
    // Test 9. findCriticalHeights sees exactly 4 heights here (0, r, h-r, h —
    // every element endpoint), so a compiler with NO adaptive subdivision
    // would stop at 3 pieces (one per interval) no matter how poorly any of
    // them fit. A 1x1 box with a fillet as large as a twentieth of its own
    // side forces real curvature into the corner pieces relative to their
    // size, which is what should push the adaptive bisection (bernsteinRho
    // < 1.2 after resolving) to split at least one of them further —
    // measured to split the corner pieces to 6 total, confirmed empirically
    // before writing this assertion, not assumed from the geometry alone.
    const double w = 1.0, h = 1.0, r = 0.05;
    const auto b = filletedBoxOf(w, h, r);

    std::vector<CriticalHeight> crit;
    findCriticalHeights(b.data(), static_cast<int>(b.size()), crit);
    const int baseline_pieces = static_cast<int>(crit.size()) - 1;
    ASSERT_EQ(baseline_pieces, 3) << "critical-height count changed — re-check the fixture";

    ChebSection s;
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
    EXPECT_GT(s.n_pieces, baseline_pieces)
        << "expected adaptive subdivision beyond the " << baseline_pieces
        << " critical-height-bounded pieces, got " << s.n_pieces;

    // Accuracy is judged against the design point (kFitTol), the same bar
    // test 5/6 use — not the spec's original 1e-10, which predates kFitTol
    // being relaxed to 1e-9 for speed (see current.md's Option A entry).
    const double err = maxAreaError(s, b, 1e-4, 1.0 - 1e-4) / s.a_full;
    EXPECT_LT(err, 20.0 * kFitTol)
        << "relative |dA| = " << err << " with " << s.n_pieces << " pieces";
    std::printf("[cheb] filleted box: %d piece(s) (baseline %d), max|dA|/A = %.3e\n",
                s.n_pieces, baseline_pieces, err);
}

TEST(ChebSection, NoCompiledPieceIsSingularAtBothEnds_Invariant) {
    // THE NORMALIZATION INVARIANT. A piece singular at both ends can only be
    // straightened by u = acos(1-2s)/pi, and that inverse trig call dominates
    // evaluation cost — about 90% of it, against a polynomial evaluation that
    // is nearly free. Splitting at any interior point leaves each half with at
    // most one singular end, where a hardware sqrt suffices.
    //
    // Holding this invariant is what collapsed the circular pipe from 2.9x
    // slower than the legacy table to parity, while IMPROVING its accuracy.
    // If it is ever violated, the acos returns and the regression comes with
    // it — silently, since results stay correct.
    for (const auto& b : {circleOf(3.0), circleOf(0.75), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int p = 0; p < s.n_pieces; ++p) {
            const bool lo_singular = s.piece[p].exp_lo > kExpSplit;
            const bool hi_singular = s.piece[p].exp_hi > kExpSplit;
            EXPECT_FALSE(lo_singular && hi_singular)
                << "piece " << p << " of " << s.n_pieces
                << " is singular at both ends — the acos map is back";
        }
    }
}

TEST(ChebSection, ExactDerivativeAgreesWithTheFittedWidth) {
    // Test 11 — catches a wrong coordinate-change Jacobian in chebdAdY, which
    // is the easiest bug here: A stays right and only the derivative is wrong.
    for (const auto& b : {circleOf(3.0), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int i = 1; i < 200; ++i) {
            const double y = s.y_full * static_cast<double>(i) / 200.0;
            EXPECT_NEAR(chebdAdY(s, y), chebWofY(s, y), 1e-4 * std::max(1.0, s.w_max))
                << "at y=" << y << " (y_full=" << s.y_full << ")";
        }
    }
}

TEST(ChebSection, AreaInvertsBackToDepth) {
    // Test 12. The spec's original 1e-12*y_full and "Newton in <=4 iterations
    // for 95%" both predate Phase 5C's compiled inverse (2026-08-22): chebYofA
    // no longer runs Newton at all on the hot path for any piece with a
    // compiled n_u > 0 (see EveryPieceOfEveryShapeCompilesAnInverse), so an
    // iteration count has nothing left to assert, and the achievable accuracy
    // is now bounded by the FORWARD series' own kFitTol chop magnified by the
    // inverse root order — measured worst case ~4e-8*y_full on a root-mapped
    // (singular-end) piece, ~1e-11 on an identity-mapped one (current.md,
    // Phase 5C). 1e-7*y_full below gives that measurement a real margin while
    // still being 10x tighter than this test's original tolerance, so a
    // future regression back toward the old Newton-only accuracy would be
    // caught here rather than passing silently.
    for (const auto& b : {circleOf(3.0), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int i = 1; i < 300; ++i) {
            const double y = s.y_full * static_cast<double>(i) / 300.0;
            const double a = chebAofY(s, y);
            if (a / s.a_full < 1e-6) continue;
            EXPECT_NEAR(chebYofA(s, a), y, 1e-7 * s.y_full) << "at y=" << y;
        }
    }
}

TEST(ChebSection, I1MatchesAFineQuadratureWithoutDoingOne) {
    // Test 13 — licenses replacing the Simpson I1 table.
    const auto circ = circleOf(3.0);
    ChebSection s;
    ASSERT_EQ(compile(s, circ.data(), static_cast<int>(circ.size())), 0);

    for (double f : {0.2, 0.5, 0.8, 0.97}) {
        const double y = f * s.y_full;
        const int m = 50001;
        const double step = y / static_cast<double>(m);
        double ref = 0.0;
        for (int i = 0; i < m; ++i) {
            const double y0 = static_cast<double>(i) * step;
            ref += 0.5 * (chebAofY(s, y0) + chebAofY(s, y0 + step)) * step;
        }
        EXPECT_NEAR(chebI1ofY(s, y), ref, 1e-6 * std::max(1.0, ref))
            << "at y/y_full=" << f;
    }
}

TEST(ChebSection, MonotoneAreaOnRealSections) {
    // The error-6 guard exists for a fitting failure, not for bad input: a
    // valid closed boundary always has A non-decreasing. Assert the positive.
    //
    // Test 17 (Phase 8) asks for a dedicated "synthetic non-monotone A -> 6"
    // negative case. Investigated directly rather than assumed: evalExact's
    // own width(y) is the sum, over crossings SORTED then paired adjacent
    // (xs[i], xs[i+1]), of xr - xl -- which is non-negative by construction
    // for ANY input, since sorting guarantees xr >= xl in every pair,
    // regardless of how the source elements wind or how many disjoint or
    // nested loops feed the array. Empirically (three constructions tried: a
    // disjoint CW "hole" rectangle, a CW hole nested inside the outer box's
    // own x-range, and a self-intersecting "bowtie" quadrilateral built the
    // same way RejectsSelfIntersectingBowtie's fixture is, just fed directly
    // to compile() instead of through fromPolyline's validation) dA/dy always
    // came out equal to that same non-negative width, never negative --
    // consistent with the co-area/Fubini identity Area(y) = integral of
    // width(t) dt over t in [0,y] holding for ANY closed (possibly self-
    // intersecting) chain once "inside at height t" is defined by the same
    // even-odd sort-and-pair rule width(y) already uses, which is exactly
    // what the synthetic "cap" segment in evalExact's own header comment
    // constructs. That makes error 6 unreachable through the public
    // compile(BElem*, n) API by ANY array of elements, not merely by ones
    // that already passed fromPolyline/fromArcSpec's validation -- it is a
    // pure defense against a hypothetical bug in chebFit/chebDeriv
    // misrepresenting a function that is mathematically guaranteed
    // monotonic, not a gate on any geometric input. Reaching it would need a
    // seam into the anonymous-namespace RawPiece/pieceIsMonotone machinery
    // this file cannot see, which is the same category as the already-
    // accepted untested error-9 pool-exhaustion case in ChebSection.hpp.
    for (const auto& b : {circleOf(3.0), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        double prev = -1.0;
        for (int i = 0; i <= 2000; ++i) {
            const double a = chebAofY(s, s.y_full * static_cast<double>(i) / 2000.0);
            EXPECT_GE(a, prev - 1e-6 * s.a_full);
            prev = a;
        }
    }
}

TEST(ChebSection, ChebAllAgreesWithTheIndividualAccessors) {
    // chebAll() is documented as the hot-loop entry point and re-implements,
    // rather than delegates to, each single-field accessor: the y >= y_full
    // continuation, the W/P non-negativity clamps, the I1 linear extension
    // above the crown, and a hand-unrolled forward T_k recurrence in place of
    // chebEval's Clenshaw. Nothing enforced the two paths staying in sync — if
    // one were edited and not the other, results would silently diverge. This
    // sweeps depth across, at, and above y_full (chebAll's own branch there)
    // and checks all four fields against chebAofY/chebWofY/chebPofY/chebI1ofY.
    for (const auto& b : {circleOf(3.0), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int i = 0; i <= 450; ++i) {
            const double y = s.y_full * 1.5 * static_cast<double>(i) / 450.0;
            double A, W, P, I1;
            chebAll(s, y, &A, &W, &P, &I1);
            const double tol_a = 1e-12 * std::max(1.0, s.a_full);
            const double tol_w = 1e-12 * std::max(1.0, s.w_max);
            const double tol_i = 1e-12 * std::max(1.0, s.a_full * s.y_full);
            EXPECT_NEAR(A, chebAofY(s, y), tol_a) << "A at y=" << y;
            EXPECT_NEAR(W, chebWofY(s, y), tol_w) << "W at y=" << y;
            EXPECT_NEAR(P, chebPofY(s, y), tol_w) << "P at y=" << y;
            EXPECT_NEAR(I1, chebI1ofY(s, y), tol_i) << "I1 at y=" << y;
        }
    }
}

TEST(ChebSection, PerimeterAndHydraulicRadiusMatchTheExactBoundary) {
    // chebPofY/chebRofY are otherwise reached only indirectly, through
    // compile()'s own scalar scan for r_full/s_max — never asserted directly
    // against the exact boundary the way area and I1 already are.
    for (const auto& b : {circleOf(3.0), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int i = 1; i < 400; ++i) {
            // Interior, generic depths only — evalExact() is not claimed exact
            // at a critical height (see XSectBoundary.hpp), and this test is
            // about chebPofY/chebRofY, not that documented scope limit.
            const double f = 1e-4 + (1.0 - 2e-4) * static_cast<double>(i) / 400.0;
            const double y = f * s.y_full;
            const auto ref = evalExact(b.data(), static_cast<int>(b.size()), y);
            if (ref.perim <= 0.0) continue;

            EXPECT_NEAR(chebPofY(s, y), ref.perim,
                       20.0 * kFitTol * std::max(1.0, ref.perim))
                << "P at y=" << y;
            // Independent of chebAofY/chebPofY's own division: reference R is
            // computed straight from evalExact's area and perimeter.
            EXPECT_NEAR(chebRofY(s, y), ref.area / ref.perim,
                       1e-6 * std::max(1.0, ref.area / ref.perim))
                << "R at y=" << y;
        }
    }
}

// ---------------------------------------------------------------------------
// Topology: disjoint wetted components (test 18)
// ---------------------------------------------------------------------------

namespace {

/// Two low-flow channels in the floor of a wide box, separated by a raised
/// island whose top is at `y_merge`. Below that height the wetted region is
/// TWO disjoint components; at and above it, one. Every wall is vertical or
/// horizontal, so A is exactly piecewise-linear and the compiled fit should
/// be near-exact — which is what makes this a clean topology test rather
/// than a fitting-accuracy one.
///
///   y=box_top  +-------------------------------+
///              |                               |
///   y=y_merge  +--+   +-----------------+   +--+
///                 |   |                 |   |
///   y=0           +---+                 +---+
///              (left channel)      (right channel)
std::vector<BElem> twinChannelsOf(double y_merge, double box_top) {
    const double x[12] = {-2.5, -1.0, -1.0, 1.0, 1.0, 2.5,
                           2.5,  3.0,  3.0, -3.0, -3.0, -2.5};
    const double y[12] = {0.0, 0.0, y_merge, y_merge, 0.0, 0.0,
                          y_merge, y_merge, box_top, box_top, y_merge, y_merge};
    std::vector<BElem> out;
    EXPECT_EQ(openswmm::xsboundary::fromPolyline(x, y, 12, out), 0);
    return out;
}

} // namespace

TEST(ChebSection, TwinChannelsMergeIntoOneComponentAndCompileExactly) {
    // Test 18. Two independent findings live here, and they are separable:
    // evalExact must REPORT the topology change (ncomp 2 -> 1) and sum both
    // spans into the top width while it lasts; and compile() must carry A
    // smoothly and monotonically THROUGH the merge, where the top width
    // jumps discontinuously (3.0 -> 6.0) but A itself only gains a slope
    // kink. That kink is a critical height, so this also exercises the
    // Phase 3 tagging on a genuinely multi-component section.
    const double y_merge = 1.0, box_top = 4.0;
    const auto tw = twinChannelsOf(y_merge, box_top);
    const int n = static_cast<int>(tw.size());

    // --- topology, below the merge: two channels, each 1.5 wide ---
    for (double f : {0.001, 0.25, 0.5, 0.75, 0.999}) {
        const double y = f * y_merge;
        const auto p = evalExact(tw.data(), n, y);
        EXPECT_EQ(p.ncomp, 2) << "expected two disjoint channels at y=" << y;
        EXPECT_NEAR(p.width, 3.0, 1e-12) << "top width must SUM both spans at y=" << y;
        EXPECT_NEAR(p.area, 3.0 * y, 1e-12) << "at y=" << y;
    }

    // --- and above it: one component spanning the full box ---
    // At y == y_merge EXACTLY the half-open crossing convention
    // (XSectBoundary.hpp) already reports the ABOVE-side limit: the island's
    // top is horizontal so contributes no transversal crossing, while the
    // outer walls' [y_merge, box_top) spans do. That is the documented
    // generic-depth caveat landing on the physically useful side here, not a
    // separate behaviour — so the merge height is grouped with "above".
    for (double f : {1.0, 1.0001, 1.5, 2.5, 3.999}) {
        const double y = f * y_merge;
        const auto p = evalExact(tw.data(), n, y);
        EXPECT_EQ(p.ncomp, 1) << "channels must have merged by y=" << y;
        EXPECT_NEAR(p.width, 6.0, 1e-12) << "at y=" << y;
    }

    // --- compiled: monotone through the merge, and near-exact ---
    ChebSection s;
    ASSERT_EQ(compile(s, tw.data(), n, false), 0);
    EXPECT_NEAR(s.y_full, box_top, 1e-12);
    EXPECT_NEAR(s.a_full, 21.0, 1e-9);   // 2*(1.5*1) box-channels + 6*3 upper box

    double worst_a = 0.0, worst_w = 0.0;
    for (int i = 0; i <= 4000; ++i) {
        const double f = 1e-4 + (1.0 - 2e-4) * static_cast<double>(i) / 4000.0;
        const double y = f * s.y_full;
        const auto ref = evalExact(tw.data(), n, y);
        worst_a = std::max(worst_a, std::fabs(chebAofY(s, y) - ref.area));
        worst_w = std::max(worst_w, std::fabs(chebWofY(s, y) - ref.width));
    }
    // The spec asks for <= 1e-10; A is piecewise LINEAR on each side of the
    // merge, so two coefficients per piece represent it EXACTLY and the
    // measured error is pure round-off (~7e-15). Asserting near the measured
    // floor rather than the spec's loose bound is what would actually catch
    // the split being lost — at 1e-10 a fit that straddled the kink could
    // still slip through.
    EXPECT_LT(worst_a, 1e-12) << "worst |dA| = " << worst_a;
    EXPECT_LT(worst_w, 1e-12) << "worst |dW| = " << worst_w;

    double prev = -1.0;
    for (int i = 0; i <= 5000; ++i) {
        const double a = chebAofY(s, s.y_full * static_cast<double>(i) / 5000.0);
        ASSERT_GE(a, prev) << "A fell through the merge";
        prev = a;
    }
    std::printf("[cheb] twin channels: %d piece(s), max|dA| = %.3e\n",
                s.n_pieces, worst_a);
}

// ---------------------------------------------------------------------------
// Degenerate top width at the crown (test 19, geometry half)
// ---------------------------------------------------------------------------

namespace {

/// Box `w` wide and `h_wall` tall, closed by a triangular roof rising to a
/// POINT at `h_apex`. The top width goes to zero LINEARLY at the crown —
/// the sharpest approach to B = 0 a straight-sided section can make.
std::vector<BElem> pointedRoofOf(double w, double h_wall, double h_apex) {
    const double hw = 0.5 * w;
    const double x[5] = {-hw, hw, hw, 0.0, -hw};
    const double y[5] = {0.0, 0.0, h_wall, h_apex, h_wall};
    std::vector<BElem> out;
    EXPECT_EQ(openswmm::xsboundary::fromPolyline(x, y, 5, out), 0);
    return out;
}

} // namespace

TEST(ChebSection, WidthStaysNonNegativeWhereItCollapsesToZero) {
    // Test 19, geometry half (the FV half is in test_fv_solver_closure.cpp).
    // A fitted series approaching a zero is the classic place to pick up a
    // small NEGATIVE overshoot from truncation — and a negative top width is
    // not merely inaccurate, it puts a negative under the celerity's square
    // root and produces a NaN that propagates through the whole solver.
    //
    // Both ways a section can close are covered, because they are different
    // analytically: a POINTED crown closes linearly (W ~ delta, forward tag
    // 1.0 — analytic, per Phase 3) while a ROUND crown closes like a square
    // root (W ~ sqrt(delta), tag 1.5). The round case is the one whose fit
    // is built on a stretched coordinate, so it is the one where an
    // overshoot would be least obvious.
    struct Case { const char* name; std::vector<BElem> b; };
    const Case cases[] = {
        {"pointed crown", pointedRoofOf(4.0, 2.0, 3.0)},
        {"round crown",   circleOf(3.0)},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ChebSection s;
        ASSERT_EQ(compile(s, c.b.data(), static_cast<int>(c.b.size()), false), 0);
        for (int i = 0; i <= 20000; ++i) {
            const double y = s.y_full * static_cast<double>(i) / 20000.0;
            const double w = chebWofY(s, y);
            ASSERT_TRUE(std::isfinite(w)) << "non-finite width at y=" << y;
            ASSERT_GE(w, 0.0) << "NEGATIVE width at y=" << y;
        }
        // ...and it really does collapse, so the check above is not vacuous.
        EXPECT_LT(chebWofY(s, s.y_full), 1e-9 * std::max(1.0, s.w_max));
    }
}

// ---------------------------------------------------------------------------
// Critical depth and non-monotone conveyance (tests 14, 15)
// ---------------------------------------------------------------------------

namespace {

/// Wrap a compiled section into an XSectParams the way PostParseResolver
/// actually does for a POLYGON link (PostParseResolver.cpp's POLYGON case:
/// every scalar comes from the ChebSection, not from setParams()).
XSectParams paramsFor(const ChebSection& s) {
    XSectParams xs{};
    xs.type = static_cast<int>(XSectShape::POLYGON);
    xs.y_full = s.y_full; xs.a_full = s.a_full; xs.r_full = s.r_full;
    xs.w_max = s.w_max; xs.s_full = s.s_full; xs.s_max = s.s_max;
    xs.cheb = &s;
    return xs;
}

/// True critical depth from Q^2*B(y) = g*A(y)^3, bisected directly on
/// evalExact — independent of both the compiled series AND getYcrit's own
/// solver, so this is genuine outside ground truth, not a self-check.
double analyticYcrit(const std::vector<BElem>& b, double y_full, double q) {
    constexpr double g = 32.2;
    double lo = 1e-6 * y_full, hi = 0.999 * y_full;
    for (int it = 0; it < 200; ++it) {
        const double mid = 0.5 * (lo + hi);
        const auto p = evalExact(b.data(), static_cast<int>(b.size()), mid);
        const double f = p.area * std::sqrt(g * p.area / p.width) - q;
        if (f > 0.0) hi = mid; else lo = mid;
    }
    return 0.5 * (lo + hi);
}

} // namespace

TEST(ChebSection, CriticalDepthOnACompiledCircleMatchesTheAnalyticFormula) {
    // Test 14. getYcrit for a compiled circle vs the TRUE analytic circular
    // critical depth (not the LEGACY table, which is the comparison
    // test_cheb_section_batch.cpp's GetYcritOnACompiledSectionMatchesThe
    // AnalyticCircle already makes, deliberately loosely, since LEGACY's own
    // table carries up to ~1.4% error and that test is about the fused-vs-
    // unfused accessor path, not accuracy).
    //
    // The spec's original target was 1e-6 ft. It was originally UNREACHABLE:
    // getYcrit's enumeration branch (taken whenever a_full/(pi/4*y_full^2) is
    // in [0.5, 2.0], which a circle always is) closes with ONE linear
    // interpolation across a y_full/25 bracket, giving an O(dy)
    // discretization error — worst observed 8.6e-3 ft on this circle, three
    // orders ABOVE the target and nothing to do with the geometry backend.
    //
    // generic_getYcrit now polishes that bracket with Ridder for a COMPILED
    // boundary only (kYcritTolCheb), because chebAWofY resolves A and W far
    // more sharply than the bracket does. Measured after: worst 4.1e-8 ft
    // over q in [0.25, 100], a ~2e5x improvement, so the original 1e-6 ft
    // target is now met with two decades to spare and is asserted directly.
    // LEGACY keeps the coarse enumeration by design (its own tables are only
    // good to ~1e-2, so refining inside one of their brackets would be false
    // precision) — the second half of this test pins that it was untouched.
    const double d = 4.0;
    const auto circ = circleOf(d);
    ChebSection s;
    ASSERT_EQ(compile(s, circ.data(), static_cast<int>(circ.size())), 0);
    const XSectParams xs = paramsFor(s);

    const double dy_step = s.y_full / 25.0;
    for (double q : {1.0, 10.0, 40.0, 80.0}) {
        const double yc_code = xsect::getYcrit(xs, q);
        const double yc_true = analyticYcrit(circ, s.y_full, q);
        EXPECT_NEAR(yc_code, yc_true, 1.0e-6)
            << "q=" << q << " code=" << yc_code << " analytic=" << yc_true;
    }

    // The polish is gated on xs.cheb, so a plain tabulated CIRCULAR must still
    // show the coarse enumeration floor. Comparing legacy against the ANALYTIC
    // root cannot show that — legacy's ~1.4% table error swamps the O(dy)
    // solver error, so the comparison passes with or without the gate
    // (confirmed by mutation: dropping `xs.cheb &&` left it green). The
    // discriminator has to compare legacy's getYcrit against the root of
    // legacy's OWN table-based Qc, which cancels the table error and leaves
    // only what the solver itself contributes.
    double p[4] = {d, 0.0, 0.0, 0.0};
    XSectParams legacy{};
    ASSERT_EQ(xsect::setParams(legacy, static_cast<int>(XSectShape::CIRCULAR),
                               p, 1.0), 0);
    ASSERT_EQ(legacy.cheb, nullptr);

    auto legacySelfYcrit = [&](double q) {
        double lo = 1.0e-9 * legacy.y_full, hi = 0.999999 * legacy.y_full;
        for (int it = 0; it < 200; ++it) {
            const double mid = 0.5 * (lo + hi);
            const double a = xsect::getAofY(legacy, mid);
            const double w = xsect::getWofY(legacy, mid);
            const double f = (w > 0.0)
                                 ? a * std::sqrt(32.2 * a / w) - q
                                 : -q;
            if (f > 0.0) hi = mid; else lo = mid;
        }
        return 0.5 * (lo + hi);
    };

    double worst_legacy = 0.0;
    for (double q : {1.0, 10.0, 40.0, 80.0}) {
        const double yc_legacy = xsect::getYcrit(legacy, q);
        worst_legacy = std::max(worst_legacy,
                                std::fabs(yc_legacy - legacySelfYcrit(q)));
    }
    EXPECT_GT(worst_legacy, 1.0e-4)
        << "LEGACY getYcrit resolved its own Qc to " << worst_legacy
        << " ft — far sharper than the 25-step enumeration can manage, so the "
           "compiled-only polish has leaked into the legacy path";
    EXPECT_LT(worst_legacy, dy_step)
        << "LEGACY getYcrit worse than one enumeration step";
}

TEST(ChebSection, NonMonotoneConveyanceRoundTripsOnBothSidesOfAMax) {
    // Test 15. A closed circular pipe's section factor S = A*(A/P)^(2/3)
    // rises to a maximum BEFORE the pipe runs full, then falls back down to
    // S_full at y_full — the classic peak-flow-before-full behavior. Measured
    // on a D=4 circle: y(a_max)/D = 0.938, inside the spec's [0.93, 0.95].
    const double d = 4.0;
    const auto circ = circleOf(d);
    ChebSection s;
    ASSERT_EQ(compile(s, circ.data(), static_cast<int>(circ.size())), 0);
    const XSectParams xs = paramsFor(s);

    ASSERT_GT(s.s_max, s.s_full) << "conveyance must actually be non-monotone";
    ASSERT_LT(s.a_max, s.a_full) << "the peak must occur before the pipe runs full";
    const double y_at_amax = chebYofA(s, s.a_max) / s.y_full;
    EXPECT_GE(y_at_amax, 0.93);
    EXPECT_LE(y_at_amax, 0.95);

    // Non-monotonicity itself: S rises to the peak, then falls back down.
    EXPECT_LT(xsect::getSofA(xs, 0.5 * s.a_max), s.s_max);
    EXPECT_LT(xsect::getSofA(xs, s.a_full), s.s_max);

    // generic_getAofS resolves an ambiguous S (S_full <= S <= S_max, which
    // has TWO valid area preimages, one per branch) onto the FALLING branch
    // near a_full — this is a real, pre-existing property of the shared
    // (LEGACY-inherited) algorithm, not a POLYGON/EXACT-specific defect, and
    // it means the round-trip identity is only guaranteed away from the
    // narrow rising-branch band where S already exceeds S_full. Found by
    // measuring, not assumed: that crossing point a* is located directly
    // below, the same way a real caller never would, to keep the two
    // well-defined regions the test actually asserts honest about where
    // they stop.
    double lo = 0.0, hi = s.a_max;
    for (int it = 0; it < 100; ++it) {
        const double mid = 0.5 * (lo + hi);
        if (xsect::getSofA(xs, mid) < s.s_full) lo = mid; else hi = mid;
    }
    const double a_star = 0.5 * (lo + hi);

    for (int i = 1; i < 200; ++i) {
        const double a = 0.9 * a_star * static_cast<double>(i) / 200.0;
        const double back = xsect::getAofS(xs, xsect::getSofA(xs, a));
        EXPECT_NEAR(back, a, 1e-4 * s.a_full) << "rising branch, a=" << a;
    }
    for (int i = 0; i <= 200; ++i) {
        const double a = s.a_max + (s.a_full - s.a_max) * static_cast<double>(i) / 200.0;
        const double back = xsect::getAofS(xs, xsect::getSofA(xs, a));
        EXPECT_NEAR(back, a, 1e-2 * s.a_full) << "falling branch, a=" << a;
    }
}

// ---------------------------------------------------------------------------
// Budget
// ---------------------------------------------------------------------------

namespace {

/// Regular n-gon approximating a circle of radius 2, invert at y = 0.
std::vector<BElem> ngonOf(int nv) {
    std::vector<double> px, py;
    for (int i = 0; i < nv; ++i) {
        const double t = 2.0 * kPiL * static_cast<double>(i) / static_cast<double>(nv);
        px.push_back(2.0 * std::cos(t));
        py.push_back(2.0 + 2.0 * std::sin(t));
    }
    std::vector<BElem> b;
    EXPECT_EQ(openswmm::xsboundary::fromPolyline(px.data(), py.data(), nv, b), 0);
    return b;
}

} // namespace

TEST(ChebSection, CompileOfALargeBoundaryIsFastEnoughForMidRunUpdates) {
    // Test 25: bounds the cost of recompiling a section during a simulation.
    // 48 vertices, not the design sketch's 64 — see the next test for why.
    const auto b = ngonOf(48);
    const auto t0 = std::chrono::steady_clock::now();
    ChebSection s;
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    std::printf("[cheb] compile(48-element boundary) = %.2f ms, %d pieces\n",
                ms, s.n_pieces);
    EXPECT_LT(ms, 50.0);
}

TEST(ChebSection, TooManyVerticesIsRejectedLoudlyNotDegradedQuietly) {
    // The piece budget is a sharp limit on polyline detail, and it is reported
    // rather than worked around. A symmetric n-gon pairs its vertex heights,
    // so it needs n/2 pieces: 48 vertices exactly fills kMaxPieces = 24, and
    // 64 needs 32 and must fail with code 5.
    //
    // This is where the design sketch is self-inconsistent — its performance
    // case compiles a 64-element boundary, which its own kMaxPieces forbids
    // for a generic polygon. Failing loudly is the safe half of that conflict:
    // the alternative is a section that silently converges algebraically.
    ChebSection ok;
    const auto b48 = ngonOf(48);
    EXPECT_EQ(compile(ok, b48.data(), static_cast<int>(b48.size())), 0);
    EXPECT_EQ(ok.n_pieces, 24);

    ChebSection over;
    const auto b64 = ngonOf(64);
    EXPECT_EQ(compile(over, b64.data(), static_cast<int>(b64.size())), 5);
}

TEST(ChebSection, EvaluationCostAgainstTableLookupAndTheAnalyticPath_Informational) {
    // Test 26. INFORMATIONAL ONLY, per the spec's own text: prints the
    // numbers a design decision was made from, and asserts nothing but a
    // loose sanity bound wide enough to survive machine-to-machine variance
    // while still catching a catastrophic regression (an accidental
    // quadratic piece scan, a lost fast path, etc.). The ~25-30% design-time
    // advantage over a 512-point table is NOT re-asserted here — it was
    // measured on one core with synthetic coefficients (see "Why this
    // design" at the top of prompt.md) and is a design expectation, not a
    // CI contract; this machine's own numbers are printed instead of judged
    // against that figure. Measured here: chebAll ~29 ns/depth vs a real
    // 4x512-table lookup ~27 ns and the circular analytic path ~34 ns — a
    // circle is documented project-wide (current.md, Phase 5B/5C) as the
    // one shape where cheb reaches near-PARITY rather than exceeding a
    // table, because its normalization-invariant sqrt maps carry real cost
    // a straight-line table lookup never pays; ~1.08x here is consistent
    // with that, not a regression.
    const auto circ = circleOf(4.0);
    ChebSection s;
    ASSERT_EQ(compile(s, circ.data(), static_cast<int>(circ.size())), 0);

    // Four independent 512-entry tables, one per chebAll field, sampled from
    // the SAME compiled section — this isolates the cost of "one fused
    // evaluation" vs "four independent table lookups" from any difference
    // in what is being represented, which is the comparison the design
    // document's own benchmark made.
    constexpr int kTableN = 512;
    std::vector<double> tA(kTableN), tW(kTableN), tP(kTableN), tI1(kTableN);
    for (int i = 0; i < kTableN; ++i) {
        const double y = s.y_full * static_cast<double>(i) / (kTableN - 1);
        tA[static_cast<std::size_t>(i)]  = chebAofY(s, y);
        tW[static_cast<std::size_t>(i)]  = chebWofY(s, y);
        tP[static_cast<std::size_t>(i)]  = chebPofY(s, y);
        tI1[static_cast<std::size_t>(i)] = chebI1ofY(s, y);
    }

    // A depth sweep, not a single repeated value: a hot loop evaluates many
    // different depths, and a fixed depth would let the branch predictor
    // and cache trivialize the piece lookup in a way real use never gets.
    constexpr int kSamples = 4096;
    std::vector<double> ys(kSamples);
    for (int i = 0; i < kSamples; ++i)
        ys[static_cast<std::size_t>(i)] = s.y_full * static_cast<double>(i) / kSamples;

    constexpr int kReps = 200;   // kSamples * kReps ~= 819200 evaluations each

    // --- fused chebAll: one piece scan, one basis recurrence, four fields ---
    double sink = 0.0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int r = 0; r < kReps; ++r) {
        for (double y : ys) {
            double A, W, P, I1;
            chebAll(s, y, &A, &W, &P, &I1);
            sink += A + W + P + I1;
        }
    }
    const auto t1 = std::chrono::steady_clock::now();

    // --- four independent 512-entry table lookups, same depths ---
    // Uses xsect::lookup — the ACTUAL production accessor every legacy
    // tabulated shape calls (bisection locate() + linear interpolation,
    // with the non-finite/out-of-range guards real callers rely on) — not
    // a hand-rolled stand-in. A minimal hand-rolled lerp was tried first and
    // is why this matters: it measured ~5x FASTER than xsect::lookup on the
    // same tables, entirely from omitting locate()'s bisection and lookup()'s
    // guard branches, which made the "3x" bound below unmeetable by any
    // realistic chebAll — not because chebAll regressed, but because the
    // baseline was unrealistically cheap. xsect::lookup is what the
    // comparison actually means to measure against.
    for (int r = 0; r < kReps; ++r) {
        for (double y : ys) {
            const double f = y / s.y_full;
            sink += xsect::lookup(f, tA.data(), kTableN) +
                    xsect::lookup(f, tW.data(), kTableN) +
                    xsect::lookup(f, tP.data(), kTableN) +
                    xsect::lookup(f, tI1.data(), kTableN);
        }
    }
    const auto t2 = std::chrono::steady_clock::now();

    // --- the CIRCULAR analytic path: one acos + one sin per depth for A,
    //     plus the sin/cos this shares for W and P. "One field" in the
    //     design document's own 91 ms/4M-eval figure is the acos itself. ---
    const double d = s.y_full;
    for (int r = 0; r < kReps; ++r) {
        for (double y : ys) {
            double c = 1.0 - 2.0 * y / d;
            if (c < -1.0) c = -1.0;
            if (c > 1.0) c = 1.0;
            const double th = 2.0 * std::acos(c);
            const double A  = (d * d / 8.0) * (th - std::sin(th));
            const double W  = d * std::sin(0.5 * th);
            const double P  = 0.5 * d * th;
            sink += A + W + P;
        }
    }
    const auto t3 = std::chrono::steady_clock::now();

    const double n_evals = static_cast<double>(kSamples) * kReps;
    const double ns_cheb  = std::chrono::duration<double, std::nano>(t1 - t0).count() / n_evals;
    const double ns_table = std::chrono::duration<double, std::nano>(t2 - t1).count() / n_evals;
    const double ns_circ  = std::chrono::duration<double, std::nano>(t3 - t2).count() / n_evals;

    std::printf("[cheb] eval cost/depth: chebAll(4 fields)=%.2f ns, "
                "4x512-table=%.2f ns, circular-analytic(3 fields)=%.2f ns\n",
                ns_cheb, ns_table, ns_circ);
    std::printf("[cheb] pieces=%d, total A-coefficients=%d (%.1f/piece), sink=%.6e\n",
                s.n_pieces, totalCoeffs(s),
                static_cast<double>(totalCoeffs(s)) / s.n_pieces, sink);

    // The one assertion: loose enough to survive real machine variance
    // (this project's own perf tests elsewhere use similarly wide margins —
    // see test 25's 50 ms bound against a ~0.6 ms measurement) while still
    // catching the class of regression that matters, e.g. an accidental
    // linear-or-worse piece rescan per field instead of one shared pass.
    EXPECT_LT(ns_cheb, 3.0 * ns_table)
        << "fused chebAll (" << ns_cheb << " ns) is more than 3x a naive "
           "four-table lookup (" << ns_table << " ns) — investigate before "
           "assuming this is just a slow machine";
}

TEST(ChebSection, IsTriviallyCopyableAndBounded) {
    // Test 27. Trivial copyability is the load-bearing claim (device mirroring)
    // and is unchanged by Phase B (promptperf.md) — packing moved WHERE the
    // coefficients live, not whether the type stays POD/memcpy-able.
    //
    // The size bound itself, though, is the thing Phase B exists to shrink.
    // Pre-Phase-B, every piece reserved seven kMaxChebCoeff(32)-wide arrays
    // regardless of actual content: ~45 kB per section, of which ~43 kB was
    // coefficients no real compiled boundary ever needed (typical content is
    // 1-12 pieces at well under 32 coefficients per field). Built-in shapes
    // under XSECT_GEOMETRY EXACT each compile their own section — no dedup
    // by (shape, dimensions) yet (promptperf.md Phase C, unstarted) — so on
    // a network where one compiled shape dominates (Bellinge, 953 conduits,
    // ~70% CIRCULAR), that put the compiled geometry data at ~43 MB
    // (45 kB x 953), several times larger than L3, so every EXACT evaluation
    // missed cache before doing any arithmetic. Packed, the same count is
    // ~18 MB (~18.5 kB x 953). Phase B replaces the fixed arrays with
    // offset/length pairs into one shared pool (kMaxPoolCoeff), sized from a
    // MEASURED worst case across the shape catalog — see kMaxPoolCoeff's own
    // note — rather than the arithmetic maximum. Asserting close to the real
    // measured size, rather than well above it, is deliberate: this is the
    // regression test that would catch a change silently reintroducing the
    // old per-piece reservation.
    static_assert(std::is_trivially_copyable_v<ChebSection>);
    std::printf("[cheb] sizeof(ChebSection) = %zu bytes (%.1f kB)\n",
                sizeof(ChebSection), sizeof(ChebSection) / 1024.0);
    EXPECT_LE(sizeof(ChebSection), 20u * 1024u);
}

// ===========================================================================
// The compiled inverse, u(A)
// ===========================================================================

namespace {

/// Gothic arch — round invert, POINTED crown. The two ends want different
/// inverse root orders, which is the case a single tag could not describe.
std::vector<BElem> gothicOf(double d) {
    const double r = 0.5 * d;
    const double cy = r;
    const double R = 2.0 * r;
    const double crown_y = cy + std::sqrt(R * R - r * r);
    return {arcOf(0.0, cy, r, kPiL, 2.0 * kPiL),
            arcOf(-r, cy, R, 0.0, std::atan2(crown_y - cy, r)),
            arcOf(r, cy, R, std::atan2(crown_y - cy, -r), kPiL)};
}

/// Closed box — flat invert, flat crown. Nothing singular anywhere.
std::vector<BElem> boxOf(double w, double h) {
    const double hw = 0.5 * w;
    return {segOf(-hw, 0.0, hw, 0.0), segOf(hw, 0.0, hw, h),
            segOf(hw, h, -hw, h), segOf(-hw, h, -hw, 0.0)};
}

/// V-notch — the width goes to zero LINEARLY at the invert, so A ~ y^2 there.
/// The forward exponent tag is 1.0 (A is analytic in y); the inverse is not.
std::vector<BElem> vNotchOf(double w, double h) {
    const double hw = 0.5 * w;
    return {segOf(0.0, 0.0, hw, h), segOf(hw, h, -hw, h), segOf(-hw, h, 0.0, 0.0)};
}

} // namespace

TEST(ChebSection, EveryPieceOfEveryShapeCompilesAnInverse) {
    // Test 28. fitPieceInverse leaves n_u = 0 when it cannot straighten a
    // piece, and chebYofA then falls back to its Newton solve — correct but
    // ~25x slower, and silently so. This is the test the fallback's own
    // comment points at: it asserts the fallback is dead code for every shape
    // the compiler is actually asked to handle, so a future change that starts
    // tripping it says so here rather than as an unexplained slowdown.
    const struct { const char* name; std::vector<BElem> b; bool open; } cases[] = {
        {"circle", circleOf(4.0), false},
        {"benched", benchedOf(6.0, 3.0, 1.0), false},
        {"gothic", gothicOf(4.0), false},
        {"box", boxOf(5.0, 3.0), false},
        {"v-notch", vNotchOf(4.0, 2.0), true},
        {"48-gon", ngonOf(48), false},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ChebSection s;
        ASSERT_EQ(compile(s, c.b.data(), static_cast<int>(c.b.size()), c.open), 0);
        ASSERT_GT(s.n_pieces, 0);
        for (int p = 0; p < s.n_pieces; ++p) {
            EXPECT_GT(s.piece[p].n_u, 0) << "piece " << p << " fell back to Newton";
        }
    }
}

TEST(ChebSection, InverseRootOrderFollowsTheGeometryNotTheForwardTag) {
    // Test 29 — pins inverseRootOrder, the one piece of judgement in the
    // inverse that a plausible-looking shortcut gets wrong. Reading
    // exp_lo/exp_hi would give the right answer for a round invert and the
    // WRONG one for a V-notch, whose forward tag is 1.0 (A really is analytic
    // in y there) while A still vanishes quadratically.

    // Round invert, round crown: cube root at both outer ends.
    ChebSection circ;
    const auto cb = circleOf(4.0);
    ASSERT_EQ(compile(circ, cb.data(), static_cast<int>(cb.size())), 0);
    EXPECT_EQ(circ.piece[0].inv_k, 3);
    EXPECT_FALSE(circ.piece[0].inv_at_hi);
    EXPECT_EQ(circ.piece[circ.n_pieces - 1].inv_k, 3);
    EXPECT_TRUE(circ.piece[circ.n_pieces - 1].inv_at_hi);

    // Flat invert, flat crown: nothing singular, identity map, no root at all.
    ChebSection box;
    const auto bb = boxOf(5.0, 3.0);
    ASSERT_EQ(compile(box, bb.data(), static_cast<int>(bb.size())), 0);
    for (int p = 0; p < box.n_pieces; ++p) EXPECT_EQ(box.piece[p].inv_k, 1);

    // V-notch invert: forward tag 1.0, inverse root order 2.
    ChebSection vee;
    const auto vb = vNotchOf(4.0, 2.0);
    ASSERT_EQ(compile(vee, vb.data(), static_cast<int>(vb.size()), true), 0);
    EXPECT_LT(vee.piece[0].exp_lo, kExpSplit) << "forward tag should be analytic";
    EXPECT_EQ(vee.piece[0].inv_k, 2) << "inverse still has a square-root branch";
    EXPECT_FALSE(vee.piece[0].inv_at_hi);
}

TEST(ChebSection, CompiledInverseAgreesWithTheNewtonSolveItReplaces) {
    // Test 30. chebYofASolve is the reference the compiler itself fits
    // against, so this is the direct statement of what compiling the inverse
    // costs in accuracy. The tolerance is split because the two piece kinds
    // have genuinely different ceilings, and the reason is structural: on a
    // root-mapped piece the forward series' own kFitTol chop leaves its k-th
    // order zero imperfect, and the k-th root magnifies that. Neither figure
    // is close to mattering — the legacy inverse TABLE this replaces carries
    // ~1.4e-2, and ~4.1e-1 below 5% of full depth.
    const struct { const char* name; std::vector<BElem> b; bool open; double tol; }
    cases[] = {
        {"circle", circleOf(4.0), false, 1e-7},
        {"benched", benchedOf(6.0, 3.0, 1.0), false, 1e-7},
        {"gothic", gothicOf(4.0), false, 1e-7},
        {"box", boxOf(5.0, 3.0), false, 1e-9},
        {"v-notch", vNotchOf(4.0, 2.0), true, 1e-9},
    };
    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ChebSection s;
        ASSERT_EQ(compile(s, c.b.data(), static_cast<int>(c.b.size()), c.open), 0);
        for (int i = 1; i < 4000; ++i) {
            const double a = s.a_full * static_cast<double>(i) / 4000.0;
            const ChebPiece& pc = s.piece[chebPieceOfA(s, a)];
            EXPECT_NEAR(chebYofA(s, a), chebYofASolve(s, pc, a), c.tol * s.y_full)
                << "at a/a_full = " << (a / s.a_full);
        }
    }
}

TEST(ChebSection, TheAreaAccessorsAgreeWithGoingThroughDepth) {
    // Test 31. chebWofA/chebPofA/chebRofA/chebI1ofA/chebAllOfA all evaluate at
    // the SAME u the inverse produced rather than re-entering through
    // chebYofA, which is the whole point of having them — one piece scan
    // instead of two. That makes them a second implementation of the same
    // quantity, so pin them against the composition they stand in for.
    for (const auto& b : {circleOf(4.0), benchedOf(6.0, 3.0, 1.0), gothicOf(4.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int i = 0; i <= 500; ++i) {
            const double a = s.a_full * static_cast<double>(i) / 500.0;
            SCOPED_TRACE(::testing::Message() << "a/a_full = " << (a / s.a_full));
            const double y = chebYofA(s, a);

            // NOT exact, and the reason is worth stating: the A-domain
            // accessors evaluate at the u the inverse produced, while going
            // through depth recovers u from y with the forward map. u -> y is
            // a squaring on a root-mapped piece and y -> u the matching
            // square root, so the round trip is faithful to within an ulp or
            // two, not to the bit. Requiring exact equality here would be
            // asserting that sqrt(x*x) == x.
            const double scale = std::max(1.0, s.a_full);
            EXPECT_NEAR(chebWofA(s, a), chebWofY(s, y), 1e-12 * scale);
            EXPECT_NEAR(chebPofA(s, a), chebPofY(s, y), 1e-12 * scale);
            EXPECT_NEAR(chebI1ofA(s, a), chebI1ofY(s, y), 1e-12 * scale * s.y_full);

            double yy = 0.0, w = 0.0, p = 0.0, i1 = 0.0;
            chebAllOfA(s, a, &yy, &w, &p, &i1);
            // These four DO have to be bit-exact: chebAllOfA shares one piece
            // scan and one u with the single-field A accessors, so the only
            // difference is the shared basis recurrence running to the longest
            // requested field instead of each field's own count — and the
            // surplus terms multiply coefficients chebChop already zeroed.
            EXPECT_EQ(yy, y);
            EXPECT_EQ(w, chebWofA(s, a));
            EXPECT_EQ(p, chebPofA(s, a));
            EXPECT_EQ(i1, chebI1ofA(s, a));

            // R is A/P by construction, not a fitted quantity of its own.
            EXPECT_EQ(chebRofA(s, a), (p > 0.0) ? a / p : 0.0);
        }
    }
}

TEST(ChebSection, RAndDPdAMatchAFiniteDifferenceOfTheSeries) {
    // Test 32 — chebRdPdA forms dP/dA as (dP/du)/(dA/du) so the coordinate
    // map's Jacobian cancels instead of being divided out twice. That is an
    // algebraic identity, so a wrong one is silent; a finite difference in A
    // is an independent construction and catches it.
    for (const auto& b : {circleOf(4.0), benchedOf(6.0, 3.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (double f : {0.15, 0.3, 0.45, 0.6, 0.75, 0.9}) {
            const double a = f * s.a_full;
            double r = 0.0, dpda = 0.0;
            ASSERT_TRUE(chebRdPdA(s, a, &r, &dpda)) << "at f = " << f;
            EXPECT_NEAR(r, chebRofA(s, a), 1e-12 * std::max(1.0, r));

            const double h = 1e-6 * s.a_full;
            const double fd = (chebPofA(s, a + h) - chebPofA(s, a - h)) / (2.0 * h);
            EXPECT_NEAR(dpda, fd, 1e-4 * std::max(std::fabs(fd), 1.0))
                << "at f = " << f;
        }
    }
}

TEST(ChebSection, TheNewtonFallbackStillAnswersWhenAPieceHasNoInverse) {
    // Test 33. n_u == 0 is the safety valve, and safety valves rot unless
    // something exercises them. Clear the compiled inverse off one piece and
    // check the answer is unchanged to the accuracy the fit claims — which
    // also demonstrates that the fallback and the fitted path are inverting
    // the same function rather than two subtly different ones.
    const auto b = circleOf(4.0);
    ChebSection s;
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
    ASSERT_GE(s.n_pieces, 3);

    ChebSection maimed = s;
    maimed.piece[1].n_u = 0;
    ASSERT_GT(s.piece[1].n_u, 0) << "piece 1 must have had an inverse to remove";

    for (int i = 1; i < 2000; ++i) {
        const double a = s.a_full * static_cast<double>(i) / 2000.0;
        EXPECT_NEAR(chebYofA(maimed, a), chebYofA(s, a), 1e-7 * s.y_full)
            << "at a/a_full = " << (a / s.a_full);
    }
}
