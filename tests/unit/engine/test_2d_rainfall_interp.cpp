/**
 * @file test_2d_rainfall_interp.cpp
 * @brief Unit tests for 2D rainfall interpolation (RainfallInterpolator) and the
 *        [2D_OPTIONS] RAINFALL_MODE option round-trip.
 *
 * @details Verifies:
 *          - Exactness at a gage site (weight 1 on the coincident gage)
 *          - Partition of unity (a constant rainfall field reproduced everywhere)
 *          - Linear reproduction inside the hull (the defining natural-neighbour
 *            property: a linear rainfall field is reproduced exactly)
 *          - 1-gage → uniform; 2-gage → IDW everywhere
 *          - Outside-hull cells → IDW (bracketed, nearest gage biased)
 *          - No located gages → ready()==false (caller uses the SYSTEM mean)
 *          - RAINFALL_MODE parse / format / key round-trip
 *
 * @see src/engine/2d/mesh/RainfallInterpolator.{hpp,cpp}
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <cmath>
#include <string>
#include <vector>

#include "2d/mesh/RainfallInterpolator.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/input/SectionHandlers2D.hpp"

using namespace openswmm::twoD;

namespace {

// Interpolate the located gages (rain values) onto a single query point.
double interpAt(double px, double py,
                const std::vector<double>& gx, const std::vector<double>& gy,
                const std::vector<double>& rain, double scale = 1.0) {
    RainfallInterpolator interp;
    interp.build({px}, {py}, gx, gy, static_cast<int>(rain.size()), scale);
    EXPECT_TRUE(interp.ready());
    std::vector<double> out;
    interp.apply(rain, out);
    return out.at(0);
}

} // namespace

// ---------------------------------------------------------------------------
// A well-conditioned 5-gage set: square corners + an off-centre interior gage,
// so the Delaunay fan is non-degenerate and queries land strictly inside cells.
// NB: coordinates are kept off the origin — (0,0) is the un-located sentinel
// (a gage with no [SYMBOLS] row), so a gage there would be excluded by design.
// ---------------------------------------------------------------------------
static const std::vector<double> kGX = {1.0, 13.0, 13.0,  1.0, 7.0};
static const std::vector<double> kGY = {1.0,  1.0, 13.0, 13.0, 6.0};

TEST(RainfallInterp, ExactAtGageSite) {
    // Cell centroid coincident with gage 2 → it gets full weight.
    const std::vector<double> rain = {10, 20, 30, 40, 50};
    EXPECT_NEAR(interpAt(kGX[2], kGY[2], kGX, kGY, rain), 30.0, 1e-9);
    EXPECT_NEAR(interpAt(kGX[4], kGY[4], kGX, kGY, rain), 50.0, 1e-9);
}

TEST(RainfallInterp, PartitionOfUnityConstantField) {
    // A constant rainfall field must be reproduced at every cell — inside the
    // hull (natural neighbour) and outside it (IDW).
    const std::vector<double> rain(5, 7.5);
    EXPECT_NEAR(interpAt(6.0, 5.0,    kGX, kGY, rain), 7.5, 1e-9);  // interior
    EXPECT_NEAR(interpAt(110.0, 90.0, kGX, kGY, rain), 7.5, 1e-9);  // far outside
}

TEST(RainfallInterp, LinearReproductionInsideHull) {
    // Natural neighbour reproduces a linear field exactly inside the convex hull.
    auto f = [](double x, double y) { return 2.0 * x + 3.0 * y + 1.0; };
    std::vector<double> rain(5);
    for (int g = 0; g < 5; ++g) rain[g] = f(kGX[g], kGY[g]);

    for (auto q : std::vector<std::pair<double, double>>{{6, 5}, {8, 5}, {5, 9}, {9, 9}}) {
        const double got = interpAt(q.first, q.second, kGX, kGY, rain);
        EXPECT_NEAR(got, f(q.first, q.second), 1e-6)
            << "at (" << q.first << "," << q.second << ")";
    }
}

TEST(RainfallInterp, SingleLocatedGageIsUniform) {
    // Only gage 0 is located; gages 1,2 are at the (0,0) un-located sentinel.
    const std::vector<double> gx = {5.0, 0.0, 0.0};
    const std::vector<double> gy = {5.0, 0.0, 0.0};
    const std::vector<double> rain = {4.0, 99.0, 99.0};
    EXPECT_NEAR(interpAt(5.0, 5.0,   gx, gy, rain), 4.0, 1e-12);
    EXPECT_NEAR(interpAt(50.0, -20.0, gx, gy, rain), 4.0, 1e-12);  // anywhere
}

TEST(RainfallInterp, TwoGagesUseInverseDistance) {
    const std::vector<double> gx = {2.0, 12.0};
    const std::vector<double> gy = {3.0,  3.0};
    const std::vector<double> rain = {10.0, 20.0};
    EXPECT_NEAR(interpAt(2.0, 3.0,  gx, gy, rain), 10.0, 1e-9);  // exact at site
    EXPECT_NEAR(interpAt(12.0, 3.0, gx, gy, rain), 20.0, 1e-9);
    EXPECT_NEAR(interpAt(7.0, 3.0,  gx, gy, rain), 15.0, 1e-9);  // equidistant → mean
    const double near0 = interpAt(3.0, 3.0, gx, gy, rain);       // closer to gage 0
    EXPECT_GT(near0, 10.0);
    EXPECT_LT(near0, 12.0);
}

TEST(RainfallInterp, OutsideHullFallsBackToIdw) {
    // Triangle of 3 gages; query well outside, just beyond gage 0.
    const std::vector<double> gx = {2.0, 12.0, 7.0};
    const std::vector<double> gy = {2.0,  2.0, 12.0};
    const std::vector<double> rain = {1.0, 2.0, 3.0};
    const double out = interpAt(-3.0, -3.0, gx, gy, rain);
    EXPECT_GE(out, 1.0);          // bracketed by the gage values
    EXPECT_LE(out, 3.0);
    EXPECT_LT(out, 1.6);          // nearest gage (gage 0 = 1.0) dominates
}

TEST(RainfallInterp, NoLocatedGagesNotReady) {
    RainfallInterpolator interp;
    // Two gages, both at the (0,0) un-located sentinel.
    interp.build({1.0, 2.0}, {1.0, 2.0}, {0.0, 0.0}, {0.0, 0.0}, 2, 1.0);
    EXPECT_FALSE(interp.ready());
}

TEST(RainfallInterp, GageScaleMatchesFrames) {
    // Gage coords in "feet"; mesh centroid in metres. With scale = 0.3048 a
    // query at gage 0's scaled (metre-frame) location gets that gage's value.
    const double s = 0.3048;
    const std::vector<double> gx = {10.0, 40.0, 25.0};   // feet
    const std::vector<double> gy = {10.0, 10.0, 40.0};
    const std::vector<double> rain = {5.0, 6.0, 7.0};
    EXPECT_NEAR(interpAt(10.0 * s, 10.0 * s, gx, gy, rain, s), 5.0, 1e-9);
}

// ---------------------------------------------------------------------------
// [2D_OPTIONS] RAINFALL_MODE plumbing
// ---------------------------------------------------------------------------

TEST(RainfallMode, DefaultIsNaturalNeighbour) {
    SolverOptions2D o;
    EXPECT_EQ(o.rainfall_mode, RainfallMode::NATURAL_NEIGHBOUR);
}

TEST(RainfallMode, ParseFormatRoundTrip) {
    SolverOptions2D o;
    EXPECT_TRUE(is2DOptionKey("RAINFALL_MODE"));

    EXPECT_TRUE(parse2DOptionsLine({"RAINFALL_MODE", "SYSTEM"}, o).empty());
    EXPECT_EQ(o.rainfall_mode, RainfallMode::SYSTEM);
    EXPECT_EQ(format2DOptionValue(o, "RAINFALL_MODE"), "SYSTEM");

    EXPECT_TRUE(parse2DOptionsLine({"RAINFALL_MODE", "natural_neighbour"}, o).empty());
    EXPECT_EQ(o.rainfall_mode, RainfallMode::NATURAL_NEIGHBOUR);
    EXPECT_EQ(format2DOptionValue(o, "RAINFALL_MODE"), "NATURAL_NEIGHBOUR");

    // American spelling accepted as an alias.
    EXPECT_TRUE(parse2DOptionsLine({"RAINFALL_MODE", "SYSTEM"}, o).empty());
    EXPECT_TRUE(parse2DOptionsLine({"RAINFALL_MODE", "NATURAL_NEIGHBOR"}, o).empty());
    EXPECT_EQ(o.rainfall_mode, RainfallMode::NATURAL_NEIGHBOUR);

    EXPECT_FALSE(parse2DOptionsLine({"RAINFALL_MODE", "bogus"}, o).empty());
}
