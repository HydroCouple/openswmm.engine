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
 * @file test_storage_shapes.cpp
 * @brief Refactored-engine parity for the four geometric storage shapes
 *        (CYLINDRICAL / CONICAL / PARABOLOID / PYRAMIDAL).
 *
 * @details These shapes existed only in the legacy solver (src/legacy/engine/node.c);
 *          the refactored engine ignored the keyword and silently treated the node as
 *          FUNCTIONAL with zeroed coefficients. See plans/STORAGE_SHAPES_PLAN_2026-07-12.md.
 *
 *          The crux the tests below defend: a/b/c are OVERLOADED by shape —
 *            FUNCTIONAL:  A(d) = c + a·d^b     (power law)
 *            geometric:   A(d) = c + a·d + b·d² (quadratic)
 *          so a CYLINDRICAL row (which has b == 0) must NOT fall into the functional
 *          `b == 0 ⇒ d = v/(a+c)` fast path in getDepth().
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>
#include <cmath>

#include "data/NodeData.hpp"
#include "data/NodeSubtypes.hpp"
#include "data/StorageGeometry.hpp"
#include "hydraulics/Node.hpp"

using namespace openswmm;

namespace {

constexpr double kPi = 3.141592653589793;

/** @brief One storage node (index 0) carrying shape @p s with dimensions p1/p2/p3. */
struct ShapeFixture {
    NodeData     nodes;
    NodeSubtypes subs;
    std::size_t  ur = 0;

    ShapeFixture(StorageShape s, double p1, double p2, double p3, double full_depth = 20.0) {
        nodes.resize(1);
        nodes.type[0] = NodeType::STORAGE;
        nodes.full_depth[0] = full_depth;
        nodes.full_volume[0] = 0.0;                 // force the analytic path, not the clamp
        const int sr = subs.set_node_type(nodes, 0, NodeType::STORAGE);
        ur = static_cast<std::size_t>(sr);
        auto& S = subs.storages;
        S.curve[ur] = -1;
        S.shape[ur] = s;
        S.p1[ur] = p1; S.p2[ur] = p2; S.p3[ur] = p3;
        double a = 0.0, b = 0.0, c = 0.0;
        EXPECT_TRUE(storage_shape_coeffs(s, p1, p2, p3, a, b, c));
        S.a[ur] = a; S.b[ur] = b; S.c[ur] = c;
    }

    double a() const { return subs.storages.a[ur]; }
    double b() const { return subs.storages.b[ur]; }
    double c() const { return subs.storages.c[ur]; }

    double area(double d, int unit_sys = 0) {
        return node::getSurfArea(nodes, 0, d, nullptr, unit_sys, &subs);
    }
    double vol(double d, int unit_sys = 0) {
        return node::getVolume(nodes, 0, d, nullptr, unit_sys, &subs);
    }
    double depth(double v, int unit_sys = 0) {
        return node::getDepth(nodes, 0, v, nullptr, unit_sys, &subs);
    }
};

/** @brief The analytic relations, written out independently of the engine. */
double quad_area(double a, double b, double c, double d) { return c + a * d + b * d * d; }
double cubic_vol(double a, double b, double c, double d) {
    return c * d + (a / 2.0) * d * d + (b / 3.0) * d * d * d;
}

}  // namespace

// ===========================================================================
// Coefficient derivation — must reproduce legacy node.c:713-752 exactly
// ===========================================================================

TEST(StorageShapeCoeffs, CylindricalIsConstantArea) {
    double a, b, c;
    ASSERT_TRUE(storage_shape_coeffs(StorageShape::CYLINDRICAL, 30.0, 20.0, 0.0, a, b, c));
    EXPECT_DOUBLE_EQ(a, 0.0);
    EXPECT_DOUBLE_EQ(b, 0.0);
    EXPECT_DOUBLE_EQ(c, kPi * 15.0 * 10.0);   // π·A·B, semi-axes
}

TEST(StorageShapeCoeffs, ConicalMatchesLegacy) {
    double a, b, c;
    ASSERT_TRUE(storage_shape_coeffs(StorageShape::CONICAL, 30.0, 20.0, 2.5, a, b, c));
    const double A = 15.0, B = 10.0, Z = 2.5;
    EXPECT_DOUBLE_EQ(a, 2.0 * kPi * B * Z);
    EXPECT_DOUBLE_EQ(b, kPi * B / A * Z * Z);
    EXPECT_DOUBLE_EQ(c, kPi * A * B);
}

TEST(StorageShapeCoeffs, ParaboloidMatchesLegacy) {
    double a, b, c;
    ASSERT_TRUE(storage_shape_coeffs(StorageShape::PARABOLOID, 30.0, 20.0, 8.0, a, b, c));
    EXPECT_DOUBLE_EQ(a, kPi * 15.0 * 10.0 / 8.0);
    EXPECT_DOUBLE_EQ(b, 0.0);
    EXPECT_DOUBLE_EQ(c, 0.0);                 // comes to a point at the invert
}

TEST(StorageShapeCoeffs, PyramidalUsesFullBaseDims) {
    double a, b, c;
    ASSERT_TRUE(storage_shape_coeffs(StorageShape::PYRAMIDAL, 30.0, 20.0, 2.5, a, b, c));
    EXPECT_DOUBLE_EQ(a, 2.0 * (30.0 + 20.0) * 2.5);   // NOT semi-axes — full L, W
    EXPECT_DOUBLE_EQ(b, 4.0 * 2.5 * 2.5);
    EXPECT_DOUBLE_EQ(c, 30.0 * 20.0);
}

TEST(StorageShapeCoeffs, RejectsInvalidDimensions) {
    double a, b, c;
    EXPECT_FALSE(storage_shape_coeffs(StorageShape::PYRAMIDAL,   0.0, 10.0,  1.0, a, b, c));
    EXPECT_FALSE(storage_shape_coeffs(StorageShape::CONICAL,    10.0,  0.0,  1.0, a, b, c));
    EXPECT_FALSE(storage_shape_coeffs(StorageShape::CONICAL,    10.0, 10.0, -1.0, a, b, c));
    EXPECT_FALSE(storage_shape_coeffs(StorageShape::PARABOLOID, 10.0, 10.0,  0.0, a, b, c));  // Z divides
    EXPECT_TRUE (storage_shape_coeffs(StorageShape::CONICAL,    10.0, 10.0,  0.0, a, b, c));  // Z=0 legal
    // TABULAR/FUNCTIONAL derive nothing.
    EXPECT_FALSE(storage_shape_coeffs(StorageShape::FUNCTIONAL, 10.0, 10.0,  1.0, a, b, c));
    EXPECT_FALSE(storage_shape_coeffs(StorageShape::TABULAR,    10.0, 10.0,  1.0, a, b, c));
}

TEST(StorageShapeCoeffs, KeywordRoundTrip) {
    StorageShape s;
    ASSERT_TRUE(storage_shape_from_keyword("PYRAMIDAL", s));
    EXPECT_EQ(s, StorageShape::PYRAMIDAL);
    // Legacy spells the keyword PARABOLIC; PARABOLOID is accepted as an alias.
    ASSERT_TRUE(storage_shape_from_keyword("PARABOLIC", s));
    EXPECT_EQ(s, StorageShape::PARABOLOID);
    ASSERT_TRUE(storage_shape_from_keyword("PARABOLOID", s));
    EXPECT_EQ(s, StorageShape::PARABOLOID);
    EXPECT_STREQ(storage_shape_keyword(StorageShape::PARABOLOID), "PARABOLIC");
    EXPECT_FALSE(storage_shape_from_keyword("FUNCTIONAL_X", s));
    // Ordinals must match legacy enum StorageType — the C API passes this int through.
    EXPECT_EQ(static_cast<int>(StorageShape::TABULAR),    0);
    EXPECT_EQ(static_cast<int>(StorageShape::FUNCTIONAL), 1);
    EXPECT_EQ(static_cast<int>(StorageShape::PYRAMIDAL),  5);
}

// ===========================================================================
// getSurfArea — the QUADRATIC relation, not the power law
// ===========================================================================

TEST(StorageShapeArea, QuadraticNotPowerLaw) {
    for (auto s : {StorageShape::CYLINDRICAL, StorageShape::CONICAL,
                   StorageShape::PARABOLOID,  StorageShape::PYRAMIDAL}) {
        ShapeFixture f(s, 30.0, 20.0, 2.5);
        for (double d : {0.0, 0.5, 3.0, 7.25, 15.0}) {
            EXPECT_NEAR(f.area(d), quad_area(f.a(), f.b(), f.c(), d), 1e-9)
                << "shape=" << storage_shape_keyword(s) << " d=" << d;
        }
    }
}

TEST(StorageShapeArea, CylindricalAreaIsDepthInvariant) {
    ShapeFixture f(StorageShape::CYLINDRICAL, 30.0, 20.0, 0.0);
    const double expect = kPi * 15.0 * 10.0;
    EXPECT_NEAR(f.area(0.0),  expect, 1e-9);
    EXPECT_NEAR(f.area(10.0), expect, 1e-9);   // a power-law read would give the same
                                               // here, so the discriminating case is
                                               // getDepth below
}

TEST(StorageShapeArea, ParaboloidHasZeroAreaAtInvert) {
    ShapeFixture f(StorageShape::PARABOLOID, 30.0, 20.0, 8.0);
    EXPECT_NEAR(f.area(0.0), 0.0, 1e-12);
}

// ===========================================================================
// getVolume — the cubic must be the exact integral of the quadratic
// ===========================================================================

TEST(StorageShapeVolume, CubicIsIntegralOfArea) {
    for (auto s : {StorageShape::CYLINDRICAL, StorageShape::CONICAL,
                   StorageShape::PARABOLOID,  StorageShape::PYRAMIDAL}) {
        ShapeFixture f(s, 30.0, 20.0, 2.5);
        for (double d : {0.5, 3.0, 7.25, 15.0}) {
            EXPECT_NEAR(f.vol(d), cubic_vol(f.a(), f.b(), f.c(), d), 1e-8)
                << "shape=" << storage_shape_keyword(s) << " d=" << d;
            // dV/dd == A(d) — central difference against the engine's own area fn.
            const double h = 1e-5;
            const double dv = (f.vol(d + h) - f.vol(d - h)) / (2.0 * h);
            EXPECT_NEAR(dv, f.area(d), 1e-3)
                << "shape=" << storage_shape_keyword(s) << " d=" << d;
        }
    }
}

TEST(StorageShapeVolume, PyramidalAgainstHandComputedValue) {
    // L=30, W=20, Z=2.5 → a=250, b=25, c=600.
    // V(4) = 600*4 + 125*16 + (25/3)*64 = 2400 + 2000 + 533.3333… = 4933.3333…
    ShapeFixture f(StorageShape::PYRAMIDAL, 30.0, 20.0, 2.5);
    EXPECT_NEAR(f.vol(4.0), 2400.0 + 2000.0 + (25.0 / 3.0) * 64.0, 1e-9);
}

// ===========================================================================
// getDepth — the regression that motivated the shape field
// ===========================================================================

TEST(StorageShapeDepth, CylindricalClosedForm) {
    ShapeFixture f(StorageShape::CYLINDRICAL, 30.0, 20.0, 0.0);
    const double d = 6.0;
    EXPECT_NEAR(f.depth(f.vol(d)), d, 1e-9);
}

TEST(StorageShapeDepth, ParaboloidClosedForm) {
    ShapeFixture f(StorageShape::PARABOLOID, 30.0, 20.0, 8.0);
    const double d = 6.0;
    EXPECT_NEAR(f.depth(f.vol(d)), d, 1e-9);
}

TEST(StorageShapeDepth, ConicalAndPyramidalNewton) {
    for (auto s : {StorageShape::CONICAL, StorageShape::PYRAMIDAL}) {
        ShapeFixture f(s, 30.0, 20.0, 2.5);
        for (double d : {1.0, 6.0, 13.5}) {
            EXPECT_NEAR(f.depth(f.vol(d)), d, 1e-3)
                << "shape=" << storage_shape_keyword(s) << " d=" << d;
        }
    }
}

TEST(StorageShapeDepth, RoundTripsUnderSIUnits) {
    // unit_sys = 1 (SI) is where the coefficients' user-unit regime and the truncated
    // SI VOLUME factor interact — the exact spot the last storage bug hid in.
    for (auto s : {StorageShape::CYLINDRICAL, StorageShape::CONICAL,
                   StorageShape::PARABOLOID,  StorageShape::PYRAMIDAL}) {
        ShapeFixture f(s, 30.0, 20.0, 2.5);
        for (double d : {1.0, 6.0, 13.5}) {
            const double v = f.vol(d, /*unit_sys=*/1);
            EXPECT_NEAR(f.depth(v, /*unit_sys=*/1), d, 1e-3)
                << "shape=" << storage_shape_keyword(s) << " d=" << d;
        }
    }
}

TEST(StorageShapeDepth, CylindricalDoesNotHitFunctionalFastPath) {
    // THE regression. A CYLINDRICAL row has b == 0, which is also the trigger for the
    // FUNCTIONAL closed form `d = v / (a + c)`. With a == 0 that happens to agree, so
    // make a and c BOTH non-zero by using CONICAL with a deliberately chosen Z: b != 0
    // there, so instead assert the inverse — that a shape row never reproduces the
    // power-law answer when the two differ.
    ShapeFixture cone(StorageShape::CONICAL, 30.0, 20.0, 2.5);
    const double d = 8.0;
    const double v = cone.vol(d);
    // What the power-law (FUNCTIONAL) reading of the same a/b/c would have produced:
    const double power_law_vol =
        cone.c() * d + cone.a() / (cone.b() + 1.0) * std::pow(d, cone.b() + 1.0);
    ASSERT_GT(std::fabs(v - power_law_vol), 1.0) << "test is not discriminating";
    EXPECT_NEAR(cone.depth(v), d, 1e-3);
}

TEST(StorageShapeDepth, ClampsAtFullDepth) {
    ShapeFixture f(StorageShape::PYRAMIDAL, 30.0, 20.0, 2.5, /*full_depth=*/10.0);
    // The EXACT clamp is getDepth's early-out (legacy node.c:801-802):
    //   if (fullVolume > 0 && v >= fullVolume) return fullDepth;
    // It only fires when fullVolume has been computed. The fixture defaults
    // fullVolume to 0 so the OTHER cases take the analytic path; give this row
    // its real full volume so the clamp path is the one under test. Without it
    // the CONICAL/PYRAMIDAL Newton (bounded to [0, fullDepth], xacc 0.001) would
    // converge to just under fullDepth — which is exactly what legacy returns
    // too, so the point of this test is the early-out, not Newton's tail.
    f.nodes.full_volume[0] = f.vol(10.0);            // V(fullDepth)
    EXPECT_NEAR(f.depth(f.vol(50.0)), 10.0, 1e-9);   // way over the top → clamps
    EXPECT_DOUBLE_EQ(f.depth(0.0), 0.0);
}

// ===========================================================================
// The FUNCTIONAL path must be untouched by all of the above
// ===========================================================================

TEST(StorageShapeDepth, FunctionalPathUnchanged) {
    NodeData nodes;
    nodes.resize(1);
    nodes.type[0] = NodeType::STORAGE;
    nodes.full_depth[0] = 20.0;
    nodes.full_volume[0] = 0.0;
    NodeSubtypes subs;
    const auto ur = static_cast<std::size_t>(subs.set_node_type(nodes, 0, NodeType::STORAGE));
    auto& S = subs.storages;
    S.curve[ur] = -1;
    S.shape[ur] = StorageShape::FUNCTIONAL;   // the default
    S.a[ur] = 100.0; S.b[ur] = 1.0; S.c[ur] = 50.0;

    // A = 50 + 100·d ⇒ V = 50d + 50d²; at d=5, V=1500 (same numbers as the pre-existing
    // NodeGetDepth.StorageFunctionalNonlinear, which must keep passing).
    EXPECT_NEAR(node::getVolume(nodes, 0, 5.0, nullptr, 0, &subs), 1500.0, 1e-6);
    EXPECT_NEAR(node::getDepth(nodes, 0, 1500.0, nullptr, 0, &subs), 5.0, 0.01);
    EXPECT_NEAR(node::getSurfArea(nodes, 0, 5.0, nullptr, 0, &subs), 550.0, 1e-6);
}
