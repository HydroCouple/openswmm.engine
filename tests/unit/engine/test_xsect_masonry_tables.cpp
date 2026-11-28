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
 * @file      test_xsect_masonry_tables.cpp
 * @brief     Issue #27 (USEPA #160): the masonry shape tables agree with one another.
 *
 * @details EPA SWMM's tables for the seven masonry shapes were digitized from charts, and a
 *          shape's area, hydraulic radius, depth-from-area and section-factor tables did not
 *          follow from its width table. OpenSWMM regenerates them from one geometry
 *          (scripts/gen_xsect_masonry_tables.py). This test holds both engines to that: for
 *          every masonry shape, through each engine's own cross-section functions,
 *            - the area is the integral of the width,
 *            - the hydraulic radius is the area over the perimeter the width describes,
 *            - the depth from an area gives that area back,
 *            - the section factor is A R^(2/3),
 *          and the egg and horseshoe match their circular-arc constructions. The checks are
 *          normalized for the five shapes whose width table and full-section constants are
 *          kept as EPA SWMM gives them (their width table does not integrate to the kept full
 *          area), and absolute for the egg and horseshoe. All of them fail on the EPA tables.
 *
 *          The legacy engine's xsect.c is compiled into this test (its library hides its
 *          symbols), as test_xsect_parity.cpp does, with stub globals for the shapes that read
 *          them.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "../../src/engine/hydraulics/XSectBatch.hpp"

using namespace openswmm;

extern "C" {
// Replica of legacy TXsect (objects.h), layout-compatible with the library's struct.
struct LegacyTXsect {
    int type;
    int culvertCode;
    int transect;
    double yFull;
    double wMax;
    double ywMax;
    double aFull;
    double rFull;
    double sFull;
    double sMax;
    double yBot;
    double aBot;
    double sBot;
    double rBot;
};
int xsect_setParams(LegacyTXsect* xsect, int type, double p[], double ucf);
double xsect_getAofY(LegacyTXsect* xsect, double y);
double xsect_getWofY(LegacyTXsect* xsect, double y);
double xsect_getRofY(LegacyTXsect* xsect, double y);
double xsect_getYofA(LegacyTXsect* xsect, double area);
double xsect_getSofA(LegacyTXsect* xsect, double area);
}

namespace {

constexpr double kYFull = 2.5; // ft; any size, the tables are normalized

struct Shape {
    const char* name;
    XSectShape type;
    bool exact;   // its geometry gives every table and constant
    bool r_table; // has an R(Y) table; the others recover R from S(A) as (S/A)^1.5
};

const Shape kShapes[] = {
    {"EGGSHAPED", XSectShape::EGGSHAPED, true, true},
    {"HORSESHOE", XSectShape::HORSESHOE, true, true},
    {"GOTHIC", XSectShape::GOTHIC, false, false},
    {"CATENARY", XSectShape::CATENARY, false, false},
    {"SEMIELLIPTICAL", XSectShape::SEMIELLIPTICAL, false, false},
    {"BASKETHANDLE", XSectShape::BASKETHANDLE, false, true},
    {"SEMICIRCULAR", XSectShape::SEMICIRCULAR, false, false},
};

// Tolerances, as fractions of the full value, chosen between what the regenerated tables
// give (at most about half of each) and what the EPA tables gave (at least 1.5 times each
// for some shape): area 0.54 % against 1.4-3.3 %; R 0.85 % against 2.2-6 % with an R table
// and 2.9 % against 7.5-17 % without one, where (S/A)^1.5 magnifies the S table's
// interpolation; depth 0.8 % against 1.8-2.4 %; section factor 0.09 % against 1.2-3.6 %.
constexpr double kAreaTol = 0.01, kRadiusTol = 0.01, kRadiusViaSTol = 0.04, kDepthTol = 0.01,
                 kSectionTol = 0.01;

// One engine's cross-section functions for one shape.
struct Section {
    std::string engine;
    double yFull = 0, aFull = 0, rFull = 0, sFull = 0, wMax = 0;
    std::function<double(double)> A, W, R, YofA, SofA;
};

Section legacySection(const Shape& s) {
    auto* x = new LegacyTXsect{}; // lives for the test process; a handful of them
    double p[4] = {kYFull, 0, 0, 0};
    if (!xsect_setParams(x, static_cast<int>(s.type), p, 1.0))
        ADD_FAILURE() << "legacy xsect_setParams refused " << s.name;
    Section e;
    e.engine = "5.3.0";
    e.yFull = x->yFull;
    e.aFull = x->aFull;
    e.rFull = x->rFull;
    e.sFull = x->sFull;
    e.wMax = x->wMax;
    e.A = [x](double y) { return xsect_getAofY(x, y); };
    e.W = [x](double y) { return xsect_getWofY(x, y); };
    e.R = [x](double y) { return xsect_getRofY(x, y); };
    e.YofA = [x](double a) { return xsect_getYofA(x, a); };
    e.SofA = [x](double a) { return xsect_getSofA(x, a); };
    return e;
}

Section refactoredSection(const Shape& s) {
    auto* x = new XSectParams{};
    double p[4] = {kYFull, 0, 0, 0};
    if (xsect::setParams(*x, static_cast<int>(s.type), p, 1.0) != 0)
        ADD_FAILURE() << "xsect::setParams refused " << s.name;
    Section e;
    e.engine = "6.0";
    e.yFull = x->y_full;
    e.aFull = x->a_full;
    e.rFull = x->r_full;
    e.sFull = x->s_full;
    e.wMax = x->w_max;
    e.A = [x](double y) { return xsect::getAofY(*x, y); };
    e.W = [x](double y) { return xsect::getWofY(*x, y); };
    e.R = [x](double y) { return xsect::getRofY(*x, y); };
    e.YofA = [x](double a) { return xsect::getYofA(*x, a); };
    e.SofA = [x](double a) { return xsect::getSofA(*x, a); };
    return e;
}

// Area and wetted perimeter implied by the engine's own width function: the trapezoid rule
// on a fine grid, and the polygon through the sampled half-widths (two sides plus any flat
// bottom). Both are what the generator computes, so the tables must reproduce them.
struct Implied {
    std::vector<double> y, area, perim;
};

Implied implied(const Section& e, int n = 4000) {
    Implied m;
    const double dy = e.yFull / n;
    double a = 0.0, p = e.W(0.0), w0 = e.W(0.0);
    m.y.push_back(0.0);
    m.area.push_back(0.0);
    m.perim.push_back(p);
    for (int i = 1; i <= n; ++i) {
        const double w1 = e.W(i * dy);
        a += 0.5 * (w0 + w1) * dy;
        p += 2.0 * std::hypot(dy, 0.5 * (w1 - w0));
        m.y.push_back(i * dy);
        m.area.push_back(a);
        m.perim.push_back(p);
        w0 = w1;
    }
    return m;
}

// Egg: H = 3r; top semicircle r, side arcs 3r centered at (+-2r, 2r), bottom circle r/2.
double eggHalfWidth(double y, double H) {
    const double r = H / 3.0;
    if (y <= 0.2 * r)
        return std::sqrt(std::max(0.0, (r / 2) * (r / 2) - (y - r / 2) * (y - r / 2)));
    if (y <= 2 * r)
        return std::sqrt(9 * r * r - (y - 2 * r) * (y - 2 * r)) - 2 * r;
    return std::sqrt(std::max(0.0, r * r - (y - 2 * r) * (y - 2 * r)));
}

// Horseshoe: height = width = D; top semicircle D/2, side arcs D centered at (+-D/2, D/2),
// invert arc D centered at the crown.
double horseshoeHalfWidth(double y, double D) {
    const double yj = (0.5 - (std::sqrt(7.0) - 1.0) / 4.0) * D;
    if (y <= yj)
        return std::sqrt(std::max(0.0, D * D - (y - D) * (y - D)));
    if (y <= 0.5 * D)
        return std::sqrt(D * D - (y - 0.5 * D) * (y - 0.5 * D)) - 0.5 * D;
    return std::sqrt(std::max(0.0, 0.25 * D * D - (y - 0.5 * D) * (y - 0.5 * D)));
}

std::vector<Section> sections(const Shape& s) {
    return {legacySection(s), refactoredSection(s)};
}

// MASONRY_GATE_VERBOSE=1 prints each check's worst deviation, to set the tolerances from
// what the regenerated tables achieve and what the EPA tables did.
void report(const char* check, const Shape& s, const Section& e, double worst) {
    if (std::getenv("MASONRY_GATE_VERBOSE"))
        std::printf("  %-40s %-15s %-6s worst %.5f\n", check, s.name, e.engine.c_str(), worst);
}

} // namespace

class MasonryTables : public ::testing::TestWithParam<Shape> {};

// A(y) is the integral of W(y): absolutely for the exact shapes, normalized for the kept ones.
TEST_P(MasonryTables, AreaIsTheIntegralOfWidth) {
    const Shape s = GetParam();
    for (const Section& e : sections(s)) {
        const Implied m = implied(e);
        const double scale = s.exact ? 1.0 : e.aFull / m.area.back();
        double worst = 0.0;
        for (std::size_t i = 0; i < m.y.size(); i += 40) {
            const double dif = std::fabs(e.A(m.y[i]) - scale * m.area[i]) / e.aFull;
            worst = std::max(worst, dif);
        }
        report("area vs integral of width", s, e, worst);
        EXPECT_LT(worst, kAreaTol)
            << s.name << " " << e.engine << ": area table against the integral of the width table";
        if (s.exact)
            EXPECT_NEAR(e.aFull, m.area.back(), 0.005 * e.aFull) << s.name << " " << e.engine;
    }
}

// R(y) is A(y)/P(y) with P the perimeter the width describes.
TEST_P(MasonryTables, HydraulicRadiusFollowsTheWidth) {
    const Shape s = GetParam();
    for (const Section& e : sections(s)) {
        const Implied m = implied(e);
        const double rFullImplied = m.area.back() / m.perim.back();
        const double scale = s.exact ? 1.0 : e.rFull / rFullImplied;
        // From 5 % to 95 % of the depth. Below that R is a few percent of rFull; above it
        // the section factor peaks and falls within one table interval, which a 51-point
        // table only brackets, and the shapes without an R table recover R from the section
        // factor as (S/A)^1.5. EPA SWMM's tables share both limits.
        double worst = 0.0;
        for (std::size_t i = 200; i <= 3800; i += 40) {
            const double r = scale * m.area[i] / m.perim[i];
            worst = std::max(worst, std::fabs(e.R(m.y[i]) - r) / e.rFull);
        }
        report("hydraulic radius vs A/P", s, e, worst);
        EXPECT_LT(worst, s.r_table ? kRadiusTol : kRadiusViaSTol)
            << s.name << " " << e.engine << ": hydraulic radius against area over perimeter";
        if (s.exact)
            EXPECT_NEAR(e.rFull, rFullImplied, 0.005 * e.rFull) << s.name << " " << e.engine;
    }
}

// Y(A(y)) gives y back, from 10 % to 95 % of the depth: the depth-from-area table's first
// and last intervals hold the root-like rise at the invert and the crown, which they can
// only bracket.
TEST_P(MasonryTables, DepthFromAreaInvertsAreaFromDepth) {
    const Shape s = GetParam();
    for (const Section& e : sections(s)) {
        double worst = 0.0;
        for (int i = 5; i <= 47; ++i) {
            const double y = e.yFull * i / 50.0;
            worst = std::max(worst, std::fabs(e.YofA(e.A(y)) - y) / e.yFull);
        }
        report("depth from area round trip", s, e, worst);
        EXPECT_LT(worst, kDepthTol) << s.name << " " << e.engine << ": depth from area";
    }
}

// S(A) is A R^(2/3) at the depth of that area, up to 90 % of the depth: above it the
// section factor peaks and falls within the last table interval.
TEST_P(MasonryTables, SectionFactorIsAreaTimesRadiusToTwoThirds) {
    const Shape s = GetParam();
    for (const Section& e : sections(s)) {
        double worst = 0.0;
        for (int i = 2; i <= 45; ++i) {
            const double y = e.yFull * i / 50.0;
            const double a = e.A(y);
            const double sf = a * std::pow(e.R(y), 2.0 / 3.0);
            worst = std::max(worst, std::fabs(e.SofA(a) - sf) / e.sFull);
        }
        report("section factor vs A R^(2/3)", s, e, worst);
        EXPECT_LT(worst, kSectionTol) << s.name << " " << e.engine << ": section factor";
    }
}

INSTANTIATE_TEST_SUITE_P(Shapes, MasonryTables, ::testing::ValuesIn(kShapes),
                         [](const ::testing::TestParamInfo<Shape>& i) { return i.param.name; });

// The egg's and horseshoe's width and full-section constants come from their geometry.
TEST(MasonryTablesGeometry, EggAndHorseshoeMatchTheirConstructions) {
    struct Case {
        Shape shape;
        double (*half)(double, double);
    };
    for (const Case& c : {Case{kShapes[0], eggHalfWidth}, Case{kShapes[1], horseshoeHalfWidth}}) {
        // exact full area and perimeter by fine integration of the construction
        const int n = 200000;
        double a = 0.0, p = 2.0 * c.half(0.0, kYFull), x0 = c.half(0.0, kYFull), wmax = 0.0;
        for (int i = 1; i <= n; ++i) {
            const double y = kYFull * i / n, x1 = c.half(y, kYFull);
            a += (x0 + x1) * (kYFull / n);
            p += 2.0 * std::hypot(kYFull / n, x1 - x0);
            wmax = std::max(wmax, 2.0 * x1);
            x0 = x1;
        }
        for (const Section& e : sections(c.shape)) {
            EXPECT_NEAR(e.aFull, a, 1e-4 * a) << c.shape.name << " " << e.engine << " aFull";
            EXPECT_NEAR(e.rFull, a / p, 1e-4 * a / p)
                << c.shape.name << " " << e.engine << " rFull";
            EXPECT_NEAR(e.wMax, wmax, 1e-4 * wmax) << c.shape.name << " " << e.engine << " wMax";
            // At the 51-point table's own nodes, where the table holds the geometry's value
            // to its five decimals (between nodes it interpolates the arcs linearly).
            double worst = 0.0;
            for (int i = 1; i < 50; ++i) {
                const double y = kYFull * i / 50.0;
                worst = std::max(worst, std::fabs(e.W(y) - 2.0 * c.half(y, kYFull)) / wmax);
            }
            report("width vs geometry", c.shape, e, worst);
            EXPECT_LT(worst, 1e-4) << c.shape.name << " " << e.engine << ": width against geometry";
        }
    }
}
