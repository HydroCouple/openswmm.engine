/**
 * @file test_cheb_section_batch.cpp
 * @brief Batched compiled-section evaluation (Phase 5B).
 *
 * @details The batch path exists purely for speed, so the only thing that
 *          makes it safe is that it computes the SAME NUMBER as the scalar
 *          accessor it replaces. This suite asserts that exactly — `EXPECT_EQ`
 *          on doubles, not `EXPECT_NEAR` — because the equivalence argument in
 *          ChebSectionBatch.hpp is a bit-level one (a shorter field's surplus
 *          terms are `0.0 * T_k`, and adding a signed zero leaves a double
 *          unchanged), and a tolerance would let a real drift hide inside it.
 *
 *          The depth grid deliberately includes the three places the compiled
 *          representation changes character, because those are where a fused
 *          rewrite is most likely to diverge from the accessor it copies:
 *            - **dry** (y <= 0), where every field short-circuits to zero;
 *            - **at the crown exactly** (y == y_full), where a closed shape's
 *              perimeter genuinely JUMPS (see ChebSection::p_full) and the
 *              fitted series must not be extrapolated;
 *            - **above the crown**, where A is pinned and only I1 keeps rising.
 *          Between them the grid sweeps every piece, including the seams,
 *          since the piece scan is the part the batch entry points share.
 *
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "hydraulics/ChebSection.hpp"
#include "hydraulics/ChebSectionBatch.hpp"
#include "hydraulics/LegacyShapeBoundary.hpp"
#include "hydraulics/XSectBatch.hpp"
#include "hydraulics/XSectBoundary.hpp"
#include "hydraulics/XSectKernels.hpp"

using namespace openswmm;
using namespace openswmm::chebsec;
using openswmm::xsboundary::BElem;

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

/// Box `w` x `h` on a semicircular low-flow channel of radius r — the benched
/// section Phase 4 used, whose bench kink is a real critical height.
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

/// Pointed (gothic) crown over a semicircular invert: the crown is ANALYTIC,
/// not a smooth tangency, so P jumps there. Phase 5 found a real bug in
/// exactly this configuration that the circle-only suite could not see.
std::vector<BElem> gothicOf(double d) {
    const double r = 0.5 * d;
    const double cy = r;
    const double R = 2.0 * r;
    const double crown_y = cy + std::sqrt(R * R - r * r);
    std::vector<BElem> b;
    b.push_back(arcOf(0.0, cy, r, kPiL, 2.0 * kPiL));
    b.push_back(arcOf(-r, cy, R, 0.0, std::atan2(crown_y - cy, r)));
    b.push_back(arcOf(r, cy, R, std::atan2(crown_y - cy, -r), kPiL));
    return b;
}

/// An OPEN rectangular channel — p_full comes from the fitted limit there,
/// not from the closed-loop perimeter, so the crown case differs in kind.
std::vector<BElem> openBoxOf(double w, double h) {
    const double hw = 0.5 * w;
    return {segOf(-hw, 0.0, hw, 0.0), segOf(hw, 0.0, hw, h),
            segOf(hw, h, -hw, h), segOf(-hw, h, -hw, 0.0)};
}

/// A built-in shape reconstructed the way XSECT_GEOMETRY EXACT does it.
bool compileLegacyShape(XSectShape shape, const double geom[4],
                        ChebSection& cs) {
    XSectParams xs{};
    double p[4] = {geom[0], geom[1], geom[2], geom[3]};
    xsect::setParams(xs, static_cast<int>(shape), p, 1.0);
    std::vector<BElem> elems;
    if (!xsboundary::buildLegacyBoundary(shape, xs, elems)) return false;
    cs = ChebSection{};
    return compile(cs, elems.data(), static_cast<int>(elems.size()), false) == 0;
}

/// Depths spanning dry, every piece and seam, the crown exactly, and above it.
std::vector<double> depthGrid(const ChebSection& s) {
    std::vector<double> y{-1.0, -1e-30, 0.0};
    for (int i = 0; i <= 60; ++i) {
        y.push_back(s.y_full * static_cast<double>(i) / 60.0);
    }
    for (int p = 0; p < s.n_pieces; ++p) {           // the seams themselves
        y.push_back(s.piece[p].y_lo);
        y.push_back(s.piece[p].y_hi);
    }
    y.push_back(s.y_full);                            // crown, exactly
    y.push_back(std::nextafter(s.y_full, 0.0));       // one ulp below it
    y.push_back(s.y_full * 1.0000001);
    y.push_back(s.y_full * 1.5);
    y.push_back(s.y_full * 10.0);
    return y;
}

/// Every batch entry point against the scalar accessor it stands in for,
/// element by element, at exact equality.
void expectBatchMatchesScalar(const char* what, const ChebSection& s) {
    SCOPED_TRACE(what);
    const std::vector<double> y = depthGrid(s);
    const std::size_t n = y.size();
    std::vector<const ChebSection*> secs(n, &s);

    // Prefill with a value nothing can legitimately produce, so an entry the
    // batch call failed to write shows up as a mismatch rather than as a
    // stale zero that happens to be right.
    const double kUnset = -12345.0;
    std::vector<double> bA(n, kUnset), bW(n, kUnset), bP(n, kUnset),
                        bI(n, kUnset), bR(n, kUnset), bA2(n, kUnset),
                        bR2(n, kUnset);

    chebAofYBatch(secs.data(), y.data(), bA.data(), n);
    chebWofYBatch(secs.data(), y.data(), bW.data(), n);
    chebRofYBatch(secs.data(), y.data(), bR.data(), n);
    chebARofYBatch(secs.data(), y.data(), bA2.data(), bR2.data(), n);
    chebAllBatch(secs.data(), y.data(), bA.data(), bW.data(), bP.data(),
                 bI.data(), n);

    for (std::size_t i = 0; i < n; ++i) {
        SCOPED_TRACE(::testing::Message() << "y = " << y[i]
                                          << "  (y_full = " << s.y_full << ")");
        double a = 0.0, w = 0.0, p = 0.0, i1 = 0.0;
        chebAll(s, y[i], &a, &w, &p, &i1);

        EXPECT_EQ(bA[i], a);
        EXPECT_EQ(bW[i], w);
        EXPECT_EQ(bP[i], p);
        EXPECT_EQ(bI[i], i1);

        // The single-field batches must agree with the single-field
        // accessors, which are the things they actually replace.
        EXPECT_EQ(bR[i], chebRofY(s, y[i]));
        EXPECT_EQ(bA2[i], chebAofY(s, y[i]));
        EXPECT_EQ(bR2[i], chebRofY(s, y[i]));

        // ...and the fused two-field form must agree with the four-field one
        // it replaced in XSectBatch.cpp, which is the substitution that
        // actually shipped.
        EXPECT_EQ(bR2[i], (p > 0.0) ? a / p : 0.0);
    }
}

} // namespace

// ===========================================================================
// The shape catalog: every family Phases 4 and 5 built a claim on.
// ===========================================================================

TEST(ChebSectionBatch, MatchesScalarOnACircle) {
    ChebSection s{};
    const auto b = circleOf(4.0);
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size()), false), 0);
    expectBatchMatchesScalar("circle D=4", s);
}

TEST(ChebSectionBatch, MatchesScalarOnABenchedSection) {
    ChebSection s{};
    const auto b = benchedOf(6.0, 4.0, 1.0);
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size()), false), 0);
    expectBatchMatchesScalar("benched box + semicircle", s);
}

TEST(ChebSectionBatch, MatchesScalarOnAPointedCrown) {
    ChebSection s{};
    const auto b = gothicOf(4.0);
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size()), false), 0);
    expectBatchMatchesScalar("gothic (analytic crown, P jumps)", s);
}

TEST(ChebSectionBatch, MatchesScalarOnAnOpenChannel) {
    ChebSection s{};
    const auto b = openBoxOf(5.0, 3.0);
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size()), true), 0);
    expectBatchMatchesScalar("open rectangular channel", s);
}

TEST(ChebSectionBatch, MatchesScalarOnEggAndArch) {
    ChebSection egg{};
    const double g_egg[4] = {3.0, 0.0, 0.0, 0.0};
    ASSERT_TRUE(compileLegacyShape(XSectShape::EGGSHAPED, g_egg, egg));
    expectBatchMatchesScalar("EGGSHAPED via buildLegacyBoundary", egg);

    ChebSection arch{};
    const double g_arch[4] = {3.0, 0.0, 0.0, 0.0};
    ASSERT_TRUE(compileLegacyShape(XSectShape::ARCH, g_arch, arch));
    expectBatchMatchesScalar("ARCH via buildLegacyBoundary", arch);
}

// ===========================================================================
// A group can legitimately mix compiled and uncompiled links.
// ===========================================================================

TEST(ChebSectionBatch, NullSectionEntriesAreLeftUntouched) {
    // PostParseResolver's EXACT-mode compilation can fail for one degenerate
    // link while its group-mates succeed, so XSectBatch.cpp relies on the
    // batch calls SKIPPING those elements rather than zeroing them — it fills
    // them itself from the legacy dispatch afterwards. If a batch call ever
    // started writing over them, that fix-up would be silently overwritten
    // in one order and silently redundant in the other.
    ChebSection s{};
    const auto b = circleOf(3.0);
    ASSERT_EQ(compile(s, b.data(), static_cast<int>(b.size()), false), 0);

    const std::size_t n = 7;
    const double y[n] = {0.5, 1.0, 1.5, 2.0, 2.5, 0.25, 2.9};
    const ChebSection* secs[n] = {&s, nullptr, &s, nullptr, &s, &s, nullptr};

    const double kSentinel = -777.25;
    std::vector<double> A(n, kSentinel), W(n, kSentinel), P(n, kSentinel),
                        I(n, kSentinel), R(n, kSentinel);

    chebAofYBatch(secs, y, A.data(), n);
    chebWofYBatch(secs, y, W.data(), n);
    chebRofYBatch(secs, y, R.data(), n);
    chebAllBatch(secs, y, A.data(), W.data(), P.data(), I.data(), n);

    for (std::size_t i = 0; i < n; ++i) {
        SCOPED_TRACE(::testing::Message() << "element " << i);
        if (secs[i] == nullptr) {
            EXPECT_EQ(A[i], kSentinel);
            EXPECT_EQ(W[i], kSentinel);
            EXPECT_EQ(P[i], kSentinel);
            EXPECT_EQ(I[i], kSentinel);
            EXPECT_EQ(R[i], kSentinel);
        } else {
            double a = 0.0, w = 0.0, p = 0.0, i1 = 0.0;
            chebAll(s, y[i], &a, &w, &p, &i1);
            EXPECT_EQ(A[i], a);
            EXPECT_EQ(W[i], w);
            EXPECT_EQ(P[i], p);
            EXPECT_EQ(I[i], i1);
            EXPECT_EQ(R[i], chebRofY(s, y[i]));
        }
    }
}

// ===========================================================================
// A batch spans MANY sections, not one repeated — that is the case where a
// per-element piece pointer is genuinely different every iteration.
// ===========================================================================

TEST(ChebSectionBatch, MatchesScalarAcrossAMixedPoolOfSections) {
    std::vector<ChebSection> pool(6);
    std::vector<std::vector<BElem>> bs;
    bs.push_back(circleOf(1.5));
    bs.push_back(circleOf(4.0));
    bs.push_back(benchedOf(6.0, 4.0, 1.0));
    bs.push_back(gothicOf(3.0));
    bs.push_back(openBoxOf(5.0, 3.0));
    bs.push_back(circleOf(9.0));
    for (std::size_t k = 0; k < pool.size(); ++k) {
        const bool open = (k == 4);
        ASSERT_EQ(compile(pool[k], bs[k].data(),
                          static_cast<int>(bs[k].size()), open), 0)
            << "pool section " << k;
    }

    const std::size_t n = 512;
    std::vector<const ChebSection*> secs(n);
    std::vector<double> y(n);
    for (std::size_t i = 0; i < n; ++i) {
        const ChebSection& s = pool[i % pool.size()];
        secs[i] = &s;
        // Sweep past the crown so the crown/above-crown branches are hit for
        // every section in the pool, not just the shallow ones.
        y[i] = s.y_full * 1.2 * static_cast<double>(i % 97) / 96.0;
    }

    std::vector<double> A(n), W(n), P(n), I(n), A2(n), R2(n);
    chebAllBatch(secs.data(), y.data(), A.data(), W.data(), P.data(), I.data(), n);
    chebARofYBatch(secs.data(), y.data(), A2.data(), R2.data(), n);

    for (std::size_t i = 0; i < n; ++i) {
        SCOPED_TRACE(::testing::Message() << "element " << i << " y = " << y[i]);
        double a = 0.0, w = 0.0, p = 0.0, i1 = 0.0;
        chebAll(*secs[i], y[i], &a, &w, &p, &i1);
        EXPECT_EQ(A[i], a);
        EXPECT_EQ(W[i], w);
        EXPECT_EQ(P[i], p);
        EXPECT_EQ(I[i], i1);
        EXPECT_EQ(A2[i], a);
        EXPECT_EQ(R2[i], (p > 0.0) ? a / p : 0.0);
    }
}

// ===========================================================================
// chebEval's forward recurrence — the other half of this phase's change.
// ===========================================================================

TEST(ChebSectionBatch, ForwardRecurrenceAgreesWithClenshaw) {
    // chebEval was changed from Clenshaw to the forward T_k recurrence for
    // speed (9.44 -> 4.76 ns/eval on a 9-coefficient series). The two are
    // mathematically identical, so the only thing worth pinning is that the
    // rewrite did not introduce an indexing slip and that the forward form
    // does not amplify — which it cannot, since |T_k(x)| <= 1 on [-1,1]. A
    // coefficient set with a deliberately nasty alternating tail exercises
    // any cancellation the forward form would be blamed for.
    double c[kMaxChebCoeff]{};
    for (int k = 0; k < 20; ++k) {
        c[k] = ((k % 2) ? -1.0 : 1.0) * std::exp(-0.35 * k) * (1.0 + 0.5 * k);
    }
    for (int i = 0; i <= 200; ++i) {
        const double u = static_cast<double>(i) / 200.0;
        const double x = 2.0 * u - 1.0;
        // Reference: Clenshaw, spelled out here so the comparison does not
        // depend on the implementation under test.
        double b1 = 0.0, b2 = 0.0;
        for (int k = 19; k >= 1; --k) {
            const double t = 2.0 * x * b1 - b2 + c[k];
            b2 = b1;
            b1 = t;
        }
        const double ref = x * b1 - b2 + c[0];
        EXPECT_NEAR(chebEval(c, 20, u), ref, 1.0e-14 * (1.0 + std::fabs(ref)))
            << "u = " << u;
    }
}

// ===========================================================================
// The bypass-mask packed view must carry the compiled boundaries with it.
// ===========================================================================

TEST(ChebSectionBatch, BypassMaskPackedViewKeepsCompiledBoundaries) {
    // Regression test for a silent EXACT bypass found in Phase 5B.
    //
    // DYNWAVE masks out links that have already converged within a Picard
    // step, and XSectGroups answers that by packing the surviving links into
    // a mirror group (setBypassMask). Every batch kernel then decides
    // "compiled boundary or legacy table?" on `g.cheb.empty()` — where `g` is
    // whatever maskedGroup() handed back. The mirror copied the per-link
    // transect tables but NOT the compiled-boundary array, so any partially
    // bypassed group looked like a LEGACY-only group and quietly routed
    // itself back to the tables. That is the ordinary case rather than a
    // corner: with roughly nine Picard iterations per step and most links
    // converged, most groups are partially bypassed most of the time.
    //
    // The check is that masking changes only WHICH links get written, never
    // WHAT is written for the ones that survive — which is the contract
    // setBypassMask already documents, and which the missing mirror broke.
    constexpr int kN = 4;
    std::vector<XSectParams> params(kN);
    const double diam[kN] = {1.0, 2.0, 3.0, 4.0};
    for (int i = 0; i < kN; ++i) {
        double p[4] = {diam[i], 0.0, 0.0, 0.0};
        xsect::setParams(params[static_cast<std::size_t>(i)],
                         static_cast<int>(XSectShape::CIRCULAR), p, 1.0);
    }

    XSectGroups groups;
    groups.build(params.data(), kN);
    const ShapeGroup* found = groups.findGroup(XSectShape::CIRCULAR);
    ASSERT_NE(found, nullptr);
    ASSERT_EQ(found->count, kN);

    // Attach compiled boundaries the way attachChebSections() does. The
    // const_cast is white-box on purpose: reaching this through a real
    // SimulationContext would need a whole parsed model, and the thing under
    // test is a data-mirroring bug one level below that.
    std::vector<chebsec::ChebSection> sections(kN);
    auto& g = *const_cast<ShapeGroup*>(found);
    g.cheb.assign(static_cast<std::size_t>(kN), nullptr);
    for (int k = 0; k < kN; ++k) {
        const int li = g.link_idx[static_cast<std::size_t>(k)];
        std::vector<BElem> elems;
        ASSERT_TRUE(xsboundary::buildLegacyBoundary(
            XSectShape::CIRCULAR, params[static_cast<std::size_t>(li)], elems));
        ASSERT_EQ(compile(sections[static_cast<std::size_t>(k)], elems.data(),
                          static_cast<int>(elems.size()), false), 0);
        g.cheb[static_cast<std::size_t>(k)] = &sections[static_cast<std::size_t>(k)];
    }

    double d1[kN], d2[kN], dm[kN];
    for (int i = 0; i < kN; ++i) {
        d1[i] = 0.35 * diam[i];
        d2[i] = 0.60 * diam[i];
        dm[i] = 0.50 * diam[i];
    }

    // Unmasked reference.
    double a1[kN]{}, a2[kN]{}, am[kN]{}, h1[kN]{}, hm[kN]{};
    groups.setBypassMask(nullptr);
    groups.computeAreaHydRadTriple(d1, d2, dm, a1, a2, am, h1, hm, kN);

    // The compiled boundary must actually be in play, or this test would
    // pass just as happily with EXACT switched off everywhere.
    EXPECT_NE(a1[0], xsect::getAofY(params[0], d1[0]))
        << "compiled boundary is not engaged even unmasked — test is vacuous";
    for (int i = 0; i < kN; ++i) {
        double ea = 0.0, er = 0.0;
        chebARofY(sections[static_cast<std::size_t>(i)], d1[i], ea, er);
        EXPECT_EQ(a1[i], ea) << "link " << i;
        EXPECT_EQ(h1[i], er) << "link " << i;
    }

    // Now bypass ONE link, so the group is packed rather than passed whole.
    const std::uint8_t mask[kN] = {0, 1, 0, 0};
    double ma1[kN]{}, ma2[kN]{}, mam[kN]{}, mh1[kN]{}, mhm[kN]{};
    groups.setBypassMask(mask);
    groups.computeAreaHydRadTriple(d1, d2, dm, ma1, ma2, mam, mh1, mhm, kN);
    groups.setBypassMask(nullptr);

    for (int i = 0; i < kN; ++i) {
        if (mask[i]) continue;                    // not written; nothing to check
        SCOPED_TRACE(::testing::Message() << "surviving link " << i);
        EXPECT_EQ(ma1[i], a1[i]);
        EXPECT_EQ(ma2[i], a2[i]);
        EXPECT_EQ(mam[i], am[i]);
        EXPECT_EQ(mh1[i], h1[i]);
        EXPECT_EQ(mhm[i], hm[i]);
    }
}

// ===========================================================================
// The inverse path — where the EXACT profile actually lives.
// ===========================================================================

TEST(ChebSectionBatch, PieceLocalDerivativeMatchesTheSectionLevelOne) {
    // chebYofA's Newton loop used to call chebdAdY, which recomputed the
    // piece's derivative coefficient series on EVERY iteration even though
    // that series depends only on the piece. Hoisting it out is worth 2.3x on
    // what was then the single most expensive function in the header
    // (966 -> 419 ns/eval, and 68% of all non-idle samples in a profiled
    // Bellinge EXACT run). The series is now built once per piece by
    // compile() and stored as ChebPiece::c_da, which removed the same
    // recompute from chebdAdY, chebdPdY and chebRdPdA as well.
    //
    // Two separate claims below, and they need different strengths:
    //
    //   * Fed the SAME series, chebdAdYOnPiece and chebdAdY must agree to the
    //     bit — that is one expression on one set of inputs.
    //   * The stored series matches one recomputed here only to about an ulp.
    //     chebDeriv is an inline function, so the library and this test
    //     compile their own instantiations, and the two are free to contract
    //     `nxt + 2*k*c[k]` into an FMA differently. Measured: one coefficient
    //     of one circle piece differs by 2.0e-16 relative, the rest by
    //     exactly zero. Storing the series once is what makes the first claim
    //     hold everywhere it matters, rather than leaving each call site to
    //     rebuild it and drift.
    struct Case { const char* name; std::vector<BElem> b; bool open; };
    std::vector<Case> cases;
    cases.push_back({"circle", circleOf(4.0), false});
    cases.push_back({"benched", benchedOf(6.0, 4.0, 1.0), false});
    cases.push_back({"gothic", gothicOf(4.0), false});
    cases.push_back({"open box", openBoxOf(5.0, 3.0), true});

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ChebSection s{};
        ASSERT_EQ(compile(s, c.b.data(), static_cast<int>(c.b.size()), c.open), 0);

        for (int p = 0; p < s.n_pieces; ++p) {
            const ChebPiece& pc = s.piece[p];
            double dc[kMaxChebCoeff];
            chebDeriv(pc.c_a, pc.n_a, dc);

            // The stored series IS the derivative series, to an ulp.
            double cmax = 0.0;
            for (int i = 0; i < pc.n_a; ++i) cmax = std::max(cmax, std::fabs(dc[i]));
            for (int i = 0; i < pc.n_a; ++i) {
                EXPECT_NEAR(pc.c_da[i], dc[i], 1e-14 * std::max(cmax, 1.0))
                    << "piece " << p << " coefficient " << i;
            }

            for (int i = 1; i < 40; ++i) {          // strictly inside the piece
                const double y = pc.y_lo +
                                 (pc.y_hi - pc.y_lo) * static_cast<double>(i) / 40.0;
                EXPECT_EQ(chebdAdYOnPiece(pc, pc.c_da, y), chebdAdY(s, y))
                    << "piece " << p << " at y = " << y;
                EXPECT_NEAR(chebdAdYOnPiece(pc, dc, y), chebdAdY(s, y),
                            1e-13 * std::max(1.0, std::fabs(chebdAdY(s, y))))
                    << "piece " << p << " at y = " << y;
            }
        }
    }
}

TEST(ChebSectionBatch, InvertingAreaRoundTripsThroughTheForwardSeries) {
    // chebYofA is the function the hoist changed, so pin what it must still
    // do: y(A(y)) == y to the fit's own accuracy, on every piece, for shapes
    // whose inverse behaves differently (a smooth circle, a benched section
    // with a real kink, an open channel with no crown).
    struct Case { const char* name; std::vector<BElem> b; bool open; };
    std::vector<Case> cases;
    cases.push_back({"circle", circleOf(4.0), false});
    cases.push_back({"benched", benchedOf(6.0, 4.0, 1.0), false});
    cases.push_back({"open box", openBoxOf(5.0, 3.0), true});

    for (const auto& c : cases) {
        SCOPED_TRACE(c.name);
        ChebSection s{};
        ASSERT_EQ(compile(s, c.b.data(), static_cast<int>(c.b.size()), c.open), 0);

        double worst = 0.0;
        for (int i = 1; i < 400; ++i) {
            const double y = s.y_full * static_cast<double>(i) / 400.0;
            const double back = chebYofA(s, chebAofY(s, y));
            worst = std::max(worst, std::fabs(back - y) / s.y_full);
        }
        // Newton is safeguarded to 1e-15 * y_full, so the round trip is
        // limited by the AREA fit rather than by the inversion; kFitTol with
        // room for the derivative's own n^2 amplification is the honest bar.
        EXPECT_LT(worst, 1.0e-7) << "worst relative round-trip error " << worst;
    }
}
