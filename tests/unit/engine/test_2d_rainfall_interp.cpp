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
 * @license  Apache-2.0
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

TEST(RainfallInterp, ProjectedCoordinatesPreserveLinearField) {
    for (const double offset : {500000.0, 5000000.0, 100000000.0}) {
        auto gx = kGX, gy = kGY;
        std::vector<double> rain;
        for (int g = 0; g < 5; ++g) {
            rain.push_back(2 * gx[g] + 3 * gy[g] + 1);
            gx[g] += offset; gy[g] += offset;
        }
        for (int x = 2; x < 13; ++x)
            for (int y = 2; y < 13; ++y)
                EXPECT_NEAR(interpAt(offset+x, offset+y, gx, gy, rain),
                            2*x+3*y+1, 1e-9) << offset << " / " << x << "," << y;
    }
}

TEST(RainfallInterp, NearestIncludesOutsideHullAndKeepsDryGage) {
    RainfallInterpolator interp;
    interp.build({2, 7, 11, 100}, {3, 3, 3, 100}, {2, 12}, {3, 3}, 2, 1,
                 RainfallInterpolator::Method::NearestNeighbour);
    std::vector<double> out;
    interp.apply({0, 20}, out);
    ASSERT_EQ(out.size(), 4u);
    EXPECT_DOUBLE_EQ(out[0], 0);   // valid dry reading, not a missing value
    EXPECT_DOUBLE_EQ(out[1], 0);   // distance tie: first gage
    EXPECT_DOUBLE_EQ(out[2], 20);
    EXPECT_DOUBLE_EQ(out[3], 20);
    EXPECT_EQ(interp.diagnostics().idwCells, 0);
}

TEST(RainfallInterp, PerCellMethodAndContributorsAreInspectable) {
    // Cell 0 inside the hull of the five kGX/kGY gages, cell 1 far outside it.
    RainfallInterpolator interp;
    interp.build({7, 100}, {6.5, 100}, kGX, kGY, 5, 1);
    ASSERT_TRUE(interp.ready());
    EXPECT_EQ(interp.cellMethod(0), RainfallInterpolator::CellMethod::NaturalNeighbour);
    EXPECT_EQ(interp.cellMethod(1), RainfallInterpolator::CellMethod::InverseDistance);
    EXPECT_EQ(interp.diagnostics().idwCells, 1);
    std::vector<int> g;
    std::vector<double> w;
    interp.cellWeights(1, g, w);
    ASSERT_EQ(g.size(), 5u);             // IDW reaches every located gage
    double sum = 0;
    for (double x : w) sum += x;
    EXPECT_NEAR(sum, 1.0, 1e-12);

    interp.build({7, 100}, {6.5, 100}, kGX, kGY, 5, 1,
                 RainfallInterpolator::Method::NearestNeighbour);
    EXPECT_EQ(interp.cellMethod(1), RainfallInterpolator::CellMethod::Nearest);
    interp.cellWeights(1, g, w);
    ASSERT_EQ(g.size(), 1u);
    EXPECT_EQ(g[0], 2);                  // (13,13) is the closest gage to (100,100)
    EXPECT_DOUBLE_EQ(w[0], 1.0);
}

TEST(RainfallInterp, InvalidAndDuplicateGagesAreReported) {
    RainfallInterpolator interp;
    const double nan = std::nan("");
    interp.build({5}, {5}, {0, nan, 5, 5}, {0, 8, 5, 5}, 4, 1);
    ASSERT_TRUE(interp.ready());
    std::vector<double> out;
    interp.apply({99, 99, 7, 0}, out);
    EXPECT_DOUBLE_EQ(out[0], 7);
    EXPECT_EQ(interp.diagnostics().unlocated, 1);
    EXPECT_EQ(interp.diagnostics().invalid, 1);
    EXPECT_EQ(interp.diagnostics().duplicates, 1);
    EXPECT_THROW(interp.build({nan}, {0}, {5}, {5}, 1, 1), std::invalid_argument);
}

TEST(RainfallInterp, ConstantRainSurvivesCollinearSitesAndHullBoundary) {
    for (const auto& gy : std::vector<std::vector<double>>{{2,2,2}, {2,2,12}}) {
        RainfallInterpolator interp;
        interp.build({2,7,12,7,-100}, {2,2,2,8,100}, {2,12,7}, gy, 3, 1);
        std::vector<double> out;
        interp.apply({7.5,7.5,7.5},out);
        for (double r : out) EXPECT_NEAR(r,7.5,1e-12);
    }
}

TEST(RainfallMode, NearestParseFormatRoundTrip) {
    SolverOptions2D o;
    for (const auto* token : {"NEAREST_NEIGHBOUR", "nearest_neighbor"}) {
        ASSERT_TRUE(parse2DOptionsLine({"RAINFALL_MODE", token}, o).empty());
        EXPECT_EQ(o.rainfall_mode, RainfallMode::NEAREST_NEIGHBOUR);
        EXPECT_EQ(format2DOptionValue(o, "RAINFALL_MODE"), "NEAREST_NEIGHBOUR");
    }
}

// Exercise the full gage state machine -> router path in both unit families.
// No subcatchment references the gage: it must still advance for mesh rainfall.
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <iterator>

TEST(RainfallMode, MeshOnlyGageUnitsFormatsAndStormEnd) {
    // Kept beside the test's working directory so the decks can be reviewed.
    const auto folder = std::filesystem::current_path() / "test_2d_rainfall_interp_out";
    std::filesystem::create_directories(folder);
    for (const auto* mode : {"NATURAL_NEIGHBOUR", "NEAREST_NEIGHBOUR"}) {
        for (const auto* units : {"CFS", "CMS"}) {
            for (const auto* format : {"INTENSITY", "VOLUME", "CUMULATIVE"}) {
                SCOPED_TRACE(std::string(mode)+" / "+units+" / "+format);
                const double intensity = std::string(units) == "CMS" ? 25.4 : 1.0;
                const double raw = std::string(format) == "INTENSITY" ? intensity : intensity/60;
                std::ostringstream inp;
                inp << std::setprecision(17)
                    << "[OPTIONS]\nFLOW_UNITS " << units << "\nFLOW_ROUTING DYNWAVE\n"
                    << "START_DATE 01/01/2026\nSTART_TIME 00:00:00\nEND_DATE 01/01/2026\nEND_TIME 00:03:00\n"
                    << "REPORT_STEP 00:00:10\nWET_STEP 00:00:01\nDRY_STEP 00:00:01\nROUTING_STEP 1\n"
                    << "[RAINGAGES]\nRG " << format << " 0:01 1 TIMESERIES TS\n"
                    << "[TIMESERIES]\nTS 01/01/2026 00:00 " << raw
                    << "\nTS 01/01/2026 00:01 0\nTS 01/01/2026 00:02 0\n"
                    << "[SYMBOLS]\nRG 5 5\n[JUNCTIONS]\nJ 0 1 0 0 0\n"
                    << "[OUTFALLS]\nO -0.5 FREE NO\n[CONDUITS]\nC J O 30 0.013 0 0 0\n"
                    << "[XSECTIONS]\nC CIRCULAR 0.3 0 0 0 1\n"
                    << "[2D_OPTIONS]\nINTEGRATOR EXPLICIT\nLTS_TIERS 1\nMAX_TIMESTEP 1\nREPORT_2D NO\nRAINFALL_MODE " << mode
                    << "\n[2D_VERTICES]\n0 0 0\n10 0 0\n10 10 0\n0 10 0\n"
                    << "[2D_TRIANGLES]\n0 1 2 0.03 0 pan\n0 2 3 0.03 0 pan\n[REPORT]\nINPUT NO\n";
                const auto path = (folder/"rain.inp").string(), rpt = (folder/"rain.rpt").string();
                { std::ofstream f(path); f << inp.str(); }
                struct Engine {
                    SWMM_Engine e = swmm_engine_create();
                    ~Engine() { swmm_engine_end(e); swmm_engine_close(e); swmm_engine_destroy(e); }
                } engine;
                ASSERT_NE(engine.e, nullptr);
                ASSERT_EQ(swmm_engine_open(engine.e,path.c_str(),rpt.c_str(),nullptr,nullptr),0);
                // The .inp writer must keep the mode, not fall back to the default.
                const auto written = (folder/"rain_written.inp").string();
                ASSERT_EQ(swmm_model_write(engine.e, written.c_str()), 0);
                {
                    std::ifstream f(written);
                    const std::string text((std::istreambuf_iterator<char>(f)), {});
                    EXPECT_NE(text.find(mode), std::string::npos);
                }
                ASSERT_EQ(swmm_engine_initialize(engine.e),0);
                // One located gage: every cell takes it at weight 1.
                int method = -9, count = -1, gage = -1;
                double weight = 0;
                ASSERT_EQ(swmm_2d_get_rainfall_weights(engine.e, 1, &method, &gage, &weight, 1, &count), 0);
                EXPECT_EQ(method, 2);
                EXPECT_EQ(count, 1);
                EXPECT_EQ(gage, 0);
                EXPECT_DOUBLE_EQ(weight, 1.0);
                ASSERT_EQ(swmm_engine_start(engine.e,0),0);
                int wet = 0, dry = 0;
                for (int i = 0; i < 1000; ++i) {
                    double days = 0;
                    ASSERT_EQ(swmm_engine_step(engine.e,&days),0);
                    if (days <= 0) break;
                    const double sec = days * 86400;
                    for (int cell = 0; cell < 2; ++cell) {
                        double rain = -1;
                        ASSERT_EQ(swmm_2d_get_rainfall(engine.e,cell,&rain),0);
                        if (sec > 5 && sec < 50) { EXPECT_NEAR(rain,0.0254/3600,1e-15); ++wet; }
                        if (sec > 80 && sec < 150) { EXPECT_DOUBLE_EQ(rain,0); ++dry; }
                    }
                }
                EXPECT_GT(wet,0); EXPECT_GT(dry,0);
            }
        }
    }
}
