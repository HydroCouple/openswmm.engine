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
#include "hydraulics/XSectBoundary.hpp"

using namespace openswmm::chebsec;
using openswmm::xsboundary::BElem;
using openswmm::xsboundary::evalExact;

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
    // Test 12.
    for (const auto& b : {circleOf(3.0), benchedOf(6.0, 4.0, 1.0)}) {
        ChebSection s;
        ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size())), 0);
        for (int i = 1; i < 300; ++i) {
            const double y = s.y_full * static_cast<double>(i) / 300.0;
            const double a = chebAofY(s, y);
            if (a / s.a_full < 1e-6) continue;
            EXPECT_NEAR(chebYofA(s, a), y, 1e-6 * s.y_full) << "at y=" << y;
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

TEST(ChebSection, IsTriviallyCopyableAndBounded) {
    // Test 27. Trivial copyability is the load-bearing claim (device mirroring).
    // The size figure is reported rather than asserted at the design sketch's
    // 8 kB, which is arithmetically impossible: 24 pieces x 5 fields x 32
    // coefficients x 8 bytes is 30 kB of coefficients before anything else.
    //
    // The bound moved 32 -> 48 kB when the compiled inverse u(A) added the
    // fifth coefficient array. That was a deliberate trade, not drift: the
    // runtime inversion it replaced measured 419 ns against ~16 for a fitted
    // series and was 68% of all non-idle samples on a real network. One array
    // and not four — y(A) is closed-form once u is known, and W/P/I1 reuse the
    // forward series at that same u — so the fifth array buys the entire
    // area-parametrized family for 8 kB rather than 24.
    static_assert(std::is_trivially_copyable_v<ChebSection>);
    std::printf("[cheb] sizeof(ChebSection) = %zu bytes (%.1f kB)\n",
                sizeof(ChebSection), sizeof(ChebSection) / 1024.0);
    EXPECT_LE(sizeof(ChebSection), 48u * 1024u);
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
