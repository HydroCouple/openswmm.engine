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
 * @file test_xsect_kernels_parity.cpp
 * @brief Plan §5.1 gate 1/3 — the portable cross-section kernels agree with the
 *        public accessors, and agree with themselves when their tables are
 *        rebound to different memory.
 *
 * @details The geometry layer moved out of XSection.cpp into XSectKernels.hpp so
 *          the identical bodies can compile for a device backend. Two things
 *          have to hold for that move to be safe, and they are different claims:
 *
 *          1. **The public accessors still route through the moved bodies.**
 *             `xsect::getAofY` and friends are now one-line forwarders, so this
 *             is structural rather than numerical — but it is what the whole
 *             regression suite's byte-identical result rests on, and asserting
 *             it here localizes a break to this file instead of to a .rpt diff.
 *
 *          2. **Rebinding the tables changes nothing.** This is the claim that
 *             matters for the device path: an evaluator built over a COPY of
 *             the geometry tables, at different addresses, must reproduce the
 *             host evaluator exactly. A device backend does precisely this,
 *             with the copies in device memory. If indexing, table sizes or
 *             pointer wiring were wrong anywhere, this is where it shows —
 *             on the host, in milliseconds, instead of on a GPU.
 *
 *          Both are swept over the full shape catalog and the full depth range,
 *          and asserted at ULP zero. Sampling error is not in play: the same
 *          arithmetic reads the same numbers from a different address.
 *
 * @see plans/EXPLICIT_FV_KOKKOS_1D_SOLVER_PLAN.md §5.1, §6.8
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>
#include <vector>

#include "hydraulics/XSectBatch.hpp"
#include "hydraulics/XSectKernels.hpp"
#include "hydraulics/xsect_tables.hpp"

using namespace openswmm;
using openswmm::xsect::XsectEval;
using openswmm::xsect::XsectTables;

namespace {

/// Every shape `setParams` can build from raw geometry, with parameters that
/// produce a valid section. Transect-backed shapes (IRREGULAR, CUSTOM,
/// STREET_XSECT) are excluded: their tables hang off XSectParams rather than
/// off the shared table set, so they exercise a different pointer path (one a
/// device backend rebinds separately).
struct ShapeCase {
    const char*   name;
    XSectShape    shape;
    double        p[4];
};

const std::vector<ShapeCase>& shapeCatalog() {
    static const std::vector<ShapeCase> cases = {
        {"CIRCULAR",        XSectShape::CIRCULAR,        {3.0, 0, 0, 0}},
        {"FORCE_MAIN",      XSectShape::FORCE_MAIN,      {3.0, 120.0, 0, 0}},
        {"FILLED_CIRCULAR", XSectShape::FILLED_CIRCULAR, {3.0, 0.5, 0, 0}},
        {"EGGSHAPED",       XSectShape::EGGSHAPED,       {3.0, 0, 0, 0}},
        {"HORSESHOE",       XSectShape::HORSESHOE,       {3.0, 0, 0, 0}},
        {"GOTHIC",          XSectShape::GOTHIC,          {3.0, 0, 0, 0}},
        {"CATENARY",        XSectShape::CATENARY,        {3.0, 0, 0, 0}},
        {"SEMIELLIPTICAL",  XSectShape::SEMIELLIPTICAL,  {3.0, 0, 0, 0}},
        {"BASKETHANDLE",    XSectShape::BASKETHANDLE,    {3.0, 0, 0, 0}},
        {"SEMICIRCULAR",    XSectShape::SEMICIRCULAR,    {3.0, 0, 0, 0}},
        {"HORIZ_ELLIPSE",   XSectShape::HORIZ_ELLIPSE,   {3.0, 4.0, 0, 0}},
        {"VERT_ELLIPSE",    XSectShape::VERT_ELLIPSE,    {4.0, 3.0, 0, 0}},
        {"ARCH",            XSectShape::ARCH,            {3.0, 4.0, 0, 0}},
        {"RECT_CLOSED",     XSectShape::RECT_CLOSED,     {3.0, 4.0, 0, 0}},
        {"RECT_OPEN",       XSectShape::RECT_OPEN,       {3.0, 4.0, 0, 0}},
        {"RECT_TRIANG",     XSectShape::RECT_TRIANG,     {3.0, 4.0, 1.0, 0}},
        {"RECT_ROUND",      XSectShape::RECT_ROUND,      {3.0, 4.0, 2.0, 0}},
        {"MOD_BASKET",      XSectShape::MOD_BASKET,      {3.0, 4.0, 2.0, 0}},
        {"TRAPEZOIDAL",     XSectShape::TRAPEZOIDAL,     {3.0, 4.0, 1.0, 1.5}},
        {"TRIANGULAR",      XSectShape::TRIANGULAR,      {3.0, 4.0, 0, 0}},
        {"PARABOLIC",       XSectShape::PARABOLIC,       {3.0, 4.0, 0, 0}},
        {"POWERFUNC",       XSectShape::POWERFUNC,       {3.0, 4.0, 2.0, 0}},
    };
    return cases;
}

/// A table set whose pointers address INDEPENDENT copies of the geometry data.
/// The copies are owned by a function-local static so they outlive every use,
/// and they are deliberately allocated (not aliased) so a stale pointer or a
/// wrong length cannot pass by accident.
const XsectTables& rebound() {
    static std::vector<std::vector<double>> owned;
    static const XsectTables t = [] {
        const XsectTables& h = xsect::hostTables();
        XsectTables r = h;
        auto clone = [&](const double* src, int n) -> const double* {
            if (!src || n <= 0) return src;
            owned.emplace_back(src, src + n);
            return owned.back().data();
        };
        // Every shared table, cloned through its own recorded length. Amax is
        // indexed by shape id (26 entries), not by a paired N_ count.
        r.A_Circ          = clone(h.A_Circ,          h.N_A_Circ);
        r.A_Egg           = clone(h.A_Egg,           h.N_A_Egg);
        r.A_Horseshoe     = clone(h.A_Horseshoe,     h.N_A_Horseshoe);
        r.A_Baskethandle  = clone(h.A_Baskethandle,  h.N_A_Baskethandle);
        r.A_HorizEllipse  = clone(h.A_HorizEllipse,  h.N_A_HorizEllipse);
        r.A_VertEllipse   = clone(h.A_VertEllipse,   h.N_A_VertEllipse);
        r.A_Arch          = clone(h.A_Arch,          h.N_A_Arch);

        r.R_Circ          = clone(h.R_Circ,          h.N_R_Circ);
        r.R_Egg           = clone(h.R_Egg,           h.N_R_Egg);
        r.R_Horseshoe     = clone(h.R_Horseshoe,     h.N_R_Horseshoe);
        r.R_Baskethandle  = clone(h.R_Baskethandle,  h.N_R_Baskethandle);
        r.R_HorizEllipse  = clone(h.R_HorizEllipse,  h.N_R_HorizEllipse);
        r.R_VertEllipse   = clone(h.R_VertEllipse,   h.N_R_VertEllipse);
        r.R_Arch          = clone(h.R_Arch,          h.N_R_Arch);

        r.W_Circ          = clone(h.W_Circ,          h.N_W_Circ);
        r.W_Egg           = clone(h.W_Egg,           h.N_W_Egg);
        r.W_Horseshoe     = clone(h.W_Horseshoe,     h.N_W_Horseshoe);
        r.W_BasketHandle  = clone(h.W_BasketHandle,  h.N_W_BasketHandle);
        r.W_Gothic        = clone(h.W_Gothic,        h.N_W_Gothic);
        r.W_Catenary      = clone(h.W_Catenary,      h.N_W_Catenary);
        r.W_SemiEllip     = clone(h.W_SemiEllip,     h.N_W_SemiEllip);
        r.W_SemiCirc      = clone(h.W_SemiCirc,      h.N_W_SemiCirc);
        r.W_HorizEllipse  = clone(h.W_HorizEllipse,  h.N_W_HorizEllipse);
        r.W_VertEllipse   = clone(h.W_VertEllipse,   h.N_W_VertEllipse);
        r.W_Arch          = clone(h.W_Arch,          h.N_W_Arch);

        r.Y_Circ          = clone(h.Y_Circ,          h.N_Y_Circ);
        r.Y_Egg           = clone(h.Y_Egg,           h.N_Y_Egg);
        r.Y_Horseshoe     = clone(h.Y_Horseshoe,     h.N_Y_Horseshoe);
        r.Y_BasketHandle  = clone(h.Y_BasketHandle,  h.N_Y_BasketHandle);
        r.Y_Gothic        = clone(h.Y_Gothic,        h.N_Y_Gothic);
        r.Y_Catenary      = clone(h.Y_Catenary,      h.N_Y_Catenary);
        r.Y_SemiEllip     = clone(h.Y_SemiEllip,     h.N_Y_SemiEllip);
        r.Y_SemiCirc      = clone(h.Y_SemiCirc,      h.N_Y_SemiCirc);

        r.S_Circ          = clone(h.S_Circ,          h.N_S_Circ);
        r.S_Egg           = clone(h.S_Egg,           h.N_S_Egg);
        r.S_Horseshoe     = clone(h.S_Horseshoe,     h.N_S_Horseshoe);
        r.S_BasketHandle  = clone(h.S_BasketHandle,  h.N_S_BasketHandle);
        r.S_Gothic        = clone(h.S_Gothic,        h.N_S_Gothic);
        r.S_Catenary      = clone(h.S_Catenary,      h.N_S_Catenary);
        r.S_SemiEllip     = clone(h.S_SemiEllip,     h.N_S_SemiEllip);
        r.S_SemiCirc      = clone(h.S_SemiCirc,      h.N_S_SemiCirc);

        r.Amax            = clone(h.Amax, 26);
        return r;
    }();
    return t;
}

/// 0 → just past full, so the depth sweep covers the dry end, the interior and
/// the crown. 401 stations resolves the low-x quadratic-refinement branch of
/// lookup_exact, which is the part most sensitive to indexing.
std::vector<double> depthStations(double y_full) {
    std::vector<double> ys;
    ys.reserve(401);
    for (int i = 0; i <= 400; ++i)
        ys.push_back(y_full * static_cast<double>(i) / 400.0);
    return ys;
}

}  // namespace

// ---------------------------------------------------------------------------
// The public accessors are the moved bodies.
// ---------------------------------------------------------------------------
TEST(XsectKernels, PublicAccessorsRouteThroughTheSharedBodies) {
    const XsectEval& ev = xsect::hostEval();
    for (const ShapeCase& c : shapeCatalog()) {
        XSectParams xs{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_EQ(xsect::setParams(xs, static_cast<int>(c.shape), p, 1.0), 0)
            << c.name;
        for (double y : depthStations(xs.y_full)) {
            EXPECT_EQ(ev.getAofY(xs, y), xsect::getAofY(xs, y)) << c.name << " y=" << y;
            EXPECT_EQ(ev.getWofY(xs, y), xsect::getWofY(xs, y)) << c.name << " y=" << y;
            EXPECT_EQ(ev.getRofY(xs, y), xsect::getRofY(xs, y)) << c.name << " y=" << y;
        }
    }
}

// ---------------------------------------------------------------------------
// The device path's central claim, checked on the host.
// ---------------------------------------------------------------------------
TEST(XsectKernels, ReboundTablesReproduceTheHostEvaluatorExactly) {
    const XsectEval& host = xsect::hostEval();
    const XsectEval  dev{rebound()};

    for (const ShapeCase& c : shapeCatalog()) {
        XSectParams xs{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_EQ(xsect::setParams(xs, static_cast<int>(c.shape), p, 1.0), 0)
            << c.name;

        for (double y : depthStations(xs.y_full)) {
            EXPECT_EQ(dev.getAofY(xs, y), host.getAofY(xs, y)) << c.name << " A y=" << y;
            EXPECT_EQ(dev.getWofY(xs, y), host.getWofY(xs, y)) << c.name << " W y=" << y;
            EXPECT_EQ(dev.getRofY(xs, y), host.getRofY(xs, y)) << c.name << " R y=" << y;
        }
        // The area-argument family too: getYofA and getSofA reach different
        // tables (Y_*, S_*) than the depth family, and a device backend has to
        // rebind those as well.
        const double a_full = xs.a_full;
        for (int i = 0; i <= 400; ++i) {
            const double a = a_full * static_cast<double>(i) / 400.0;
            EXPECT_EQ(dev.getYofA(xs, a), host.getYofA(xs, a)) << c.name << " Y a=" << a;
            EXPECT_EQ(dev.getSofA(xs, a), host.getSofA(xs, a)) << c.name << " S a=" << a;
            EXPECT_EQ(dev.getRofA(xs, a), host.getRofA(xs, a)) << c.name << " R a=" << a;
        }
        EXPECT_EQ(dev.getAmax(xs), host.getAmax(xs)) << c.name << " Amax";
    }
}

// ---------------------------------------------------------------------------
// The evaluator is trivially copyable — the property that lets it be captured
// by value into a kernel. Asserted rather than assumed, because adding an
// owning member to XsectTables would break the device path silently.
// ---------------------------------------------------------------------------
TEST(XsectKernels, EvaluatorIsTriviallyCopyableForKernelCapture) {
    EXPECT_TRUE(std::is_trivially_copyable_v<XsectTables>);
    EXPECT_TRUE(std::is_trivially_copyable_v<XsectEval>);
}

// ---------------------------------------------------------------------------
// Phase 4b: the batch path DW runs on IS the shared bodies.
//
// `XSectGroups` is what DWSolver's computeLinkGeometry STEP B (widths) and
// STEP D (areas + hydraulic radii) call, through the same triple kernels used
// here. Every analytic shape formula now has exactly one definition — the
// `xsect::shape` leaves — so this asserts the property that makes the two
// solvers' geometry structurally identical rather than coincidentally equal.
//
// In the bit-exact configuration the gate is ULP zero, not a tolerance. Two
// things it catches that a tolerance would not:
//   - operand order. IEEE multiplication is not associative, so the old batch
//     spelling of the triangular area, `s_bot*y*y`, is a different computation
//     from legacy's `y*y*s_bot`. No deck in either corpus has a TRIANGULAR
//     conduit, so the regression suite could never have seen it.
//   - the fused circular kernel, which computes the A_Circ and R_Circ
//     interpolations from one shared segment index. It carries a comment
//     claiming bit-identity with two separate lookup_exact calls; this turns
//     that claim into a gate.
//
// The DEFAULT build enables SWMM_XSECT_FAST_LOOKUP (§6), where the batch path
// deliberately normalizes by a precomputed reciprocal and interpolates with
// `* inv_delta`, while XsectEval always divides. There the two paths are
// SUPPOSED to differ, and the contract is the §6 tolerance rather than the
// last bit — so that is what is asserted. The strict form runs in the
// bit-exact configuration (`-DOPENSWMM_FAST_XSECT_LOOKUP=OFF`), which is the
// one under the legacy parity contract.
// ---------------------------------------------------------------------------
namespace {
#ifdef SWMM_XSECT_FAST_LOOKUP
// §6's published envelope for the fast lookup, same figures as the tolerance
// gate in test_xsect_parity.cpp.
::testing::AssertionResult Agrees(double batch, double shared) {
    const double tol = 1e-8 + 1e-7 * std::fabs(shared);
    if (std::fabs(batch - shared) <= tol) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
        << "batch=" << batch << " shared=" << shared
        << " |diff|=" << std::fabs(batch - shared) << " tol=" << tol;
}
#else
::testing::AssertionResult Agrees(double batch, double shared) {
    if (batch == shared) return ::testing::AssertionSuccess();
    return ::testing::AssertionFailure()
        << "batch=" << batch << " shared=" << shared << " (ULP != 0)";
}
#endif
}  // namespace

TEST(XSectSharedFormulas, BatchPathMatchesTheSharedKernels) {
    const XsectEval& ev = xsect::hostEval();

    for (const ShapeCase& c : shapeCatalog()) {
        XSectParams xs{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_EQ(xsect::setParams(xs, static_cast<int>(c.shape), p, 1.0), 0)
            << c.name;

        XSectGroups groups;
        groups.build(&xs, 1);

        for (double y : depthStations(xs.y_full)) {
            const double d[1] = {y};
            double a1 = 0, a2 = 0, am = 0, h1 = 0, hm = 0;
            double w1 = 0, w2 = 0, wm = 0;
            groups.computeAreaHydRadTriple(d, d, d, &a1, &a2, &am, &h1, &hm, 1);
            groups.computeWidthsTriple(d, d, d, &w1, &w2, &wm, 1);

            EXPECT_TRUE(Agrees(a1, ev.getAofY(xs, y))) << c.name << " A y=" << y;
            EXPECT_TRUE(Agrees(h1, ev.getRofY(xs, y))) << c.name << " R y=" << y;
            EXPECT_TRUE(Agrees(w1, ev.getWofY(xs, y))) << c.name << " W y=" << y;

            // The triple's slots must agree with each other. a2 comes from
            // computeAreas and a1/am from computeAreaAndHydRad — for CIRCULAR
            // those are two different kernels (the fused one reuses a single
            // segment index), so this is a real check, not a tautology.
            EXPECT_TRUE(Agrees(a2, a1)) << c.name << " a2 y=" << y;
            EXPECT_EQ(am, a1) << c.name << " am y=" << y;
            EXPECT_EQ(hm, h1) << c.name << " hm y=" << y;
            EXPECT_EQ(w2, w1) << c.name << " w2 y=" << y;
            EXPECT_EQ(wm, w1) << c.name << " wm y=" << y;
        }
    }
}

// ---------------------------------------------------------------------------
// The analytic shapes, held to ULP zero in EVERY build mode.
//
// SWMM_XSECT_FAST_LOOKUP changes the normalize/interpolate layer, which these
// shapes never touch — their whole geometry is the closed-form leaf. So the
// unification Phase 4b performed is asserted here without a mode branch, and
// TRIANGULAR (which no deck in either corpus contains) is covered by name.
// ---------------------------------------------------------------------------
TEST(XSectSharedFormulas, AnalyticShapeLeavesAreBitExactInEveryMode) {
    struct Case { const char* name; XSectShape shape; double p[4]; };
    const std::vector<Case> analytic = {
        {"TRAPEZOIDAL", XSectShape::TRAPEZOIDAL, {3.0, 4.0, 1.0, 1.5}},
        {"TRIANGULAR",  XSectShape::TRIANGULAR,  {3.0, 4.0, 0, 0}},
    };

    const XsectEval& ev = xsect::hostEval();
    for (const Case& c : analytic) {
        XSectParams xs{};
        double p[4] = {c.p[0], c.p[1], c.p[2], c.p[3]};
        ASSERT_EQ(xsect::setParams(xs, static_cast<int>(c.shape), p, 1.0), 0)
            << c.name;

        XSectGroups groups;
        groups.build(&xs, 1);

        for (double y : depthStations(xs.y_full)) {
            const double d[1] = {y};
            double a1 = 0, a2 = 0, am = 0, h1 = 0, hm = 0;
            double w1 = 0, w2 = 0, wm = 0;
            groups.computeAreaHydRadTriple(d, d, d, &a1, &a2, &am, &h1, &hm, 1);
            groups.computeWidthsTriple(d, d, d, &w1, &w2, &wm, 1);

            EXPECT_EQ(a1, ev.getAofY(xs, y)) << c.name << " A y=" << y;
            EXPECT_EQ(h1, ev.getRofY(xs, y)) << c.name << " R y=" << y;
            EXPECT_EQ(w1, ev.getWofY(xs, y)) << c.name << " W y=" << y;
        }
    }
}

// ---------------------------------------------------------------------------
// The one formula the batch path does NOT route through a shared leaf: the
// rectangular area, which stays on an explicit SIMD multiply for the vector
// width. `_mm256_mul_pd` / `vmulq_f64` are element-wise IEEE multiplies, so it
// is bit-identical to shape::rectAofY — pinned here so the claim in
// XSectBatch.cpp is checked rather than trusted.
// ---------------------------------------------------------------------------
TEST(XSectSharedFormulas, RectAreaMatchesSimdPath) {
    constexpr int kN = 257;   // not a multiple of any vector width
    std::vector<double> depth(kN), w_max(kN), simd_a(kN);
    for (int k = 0; k < kN; ++k) {
        depth[k] = 0.01 * static_cast<double>(k) + 1e-7;
        w_max[k] = 0.75 + 0.013 * static_cast<double>(k);
    }
    xsect_batch::area_rect(depth.data(), w_max.data(), simd_a.data(), kN);

    for (int k = 0; k < kN; ++k)
        EXPECT_EQ(simd_a[k], xsect::shape::rectAofY(depth[k], w_max[k]))
            << "k=" << k;
}
