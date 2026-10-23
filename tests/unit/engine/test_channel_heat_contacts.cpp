// SPDX-License-Identifier: Apache-2.0

/**
 * @file      test_channel_heat_contacts.cpp
 * @brief     Tests for channel heat contacts.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <cmath>
#include <limits>
#include "2d/subsurface/SubsurfaceSections.hpp"
#include "2d/data/MeshData.hpp"
namespace {
using namespace openswmm::twoD;
/**
 * @brief Build raw cell connectivity for a flat 10 m square contact fixture.
 *
 * Does not build topology, centroids/areas or spatial acceleration; caller performs needed
 * construction before geometric mapping. No solver/state is allocated.
 *
 * @param[in] quads True creates one four-vertex cell; false two triangular cells.
 * @return Owned mesh with SI corner coordinates and zero bed.
 */
MeshData square(bool quads = false) {
    MeshData m;
    m.vx = {0, 10, 10, 0};
    m.vy = {0, 0, 10, 10};
    m.vz = {0, 0, 0, 0};
    m.resize_triangles(quads ? 1 : 2);
    m.cell_v = quads ? std::vector<int>{0, 1, 2, 3} : std::vector<int>{0, 1, 2, -1, 0, 2, 3, -1};
    if (quads)
        m.cell_nv[0] = 4;
    return m;
}
/**
 * @brief Sum mapped interval lengths weighted by their sharing fractions.
 *
 * No sorting, overlap normalization, sign validation or mapping mutation.
 *
 * @param[in] m Stable mapping whose interval endpoints use the same polyline distance units.
 * @return Sum (end-begin)*share, meters for these SI contact fixtures; zero when empty.
 */
double weightedLength(const MeshPolylineMapping& m) {
    double result = 0;
    for (const auto& i : m.intervals)
        result += (i.end - i.begin) * i.share;
    return result;
}
}
TEST(ChannelHeatContacts, FullReachKeepsChainageAndCoverageGapsAcrossMultipleCells) {
    auto r = mapMeshPolyline(square(), {-5, 15}, {2.5, 2.5});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_DOUBLE_EQ(r.length, 20);
    EXPECT_DOUBLE_EQ(r.covered_length, 10);
    ASSERT_EQ(r.intervals.size(), 2);
    EXPECT_EQ(r.intervals[0].cell, 1);
    EXPECT_EQ(r.intervals[1].cell, 0);
    EXPECT_DOUBLE_EQ(r.intervals[0].begin, 5);
    EXPECT_DOUBLE_EQ(r.intervals[0].end, 7.5);
    EXPECT_DOUBLE_EQ(r.intervals[1].begin, 7.5);
    EXPECT_DOUBLE_EQ(r.intervals[1].end, 15);
    EXPECT_DOUBLE_EQ(weightedLength(r), 10);
}
TEST(ChannelHeatContacts, SharedEdgeSplitsOnlyItsOwnIntervalAndPreservesOrdinaryInteriorLengths) {
    auto r = mapMeshPolyline(square(), {0, 5, 10}, {0, 5, 5});
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.intervals.size(), 3);
    EXPECT_NEAR(r.length, 5 * std::sqrt(2.) + 5, 1e-12);
    EXPECT_NEAR(weightedLength(r), r.length, 1e-12);
    EXPECT_DOUBLE_EQ(r.intervals[0].share, .5);
    EXPECT_DOUBLE_EQ(r.intervals[1].share, .5);
    EXPECT_DOUBLE_EQ(r.intervals[2].share, 1);
    EXPECT_NEAR(r.intervals[2].end - r.intervals[2].begin, 5, 1e-12);
    EXPECT_EQ(r.intervals[2].cell, 0);
}
TEST(ChannelHeatContacts, QuadAndReversedRepeatedVertexPolylinePreservePhysicalCoverage) {
    const auto mesh = square(true);
    auto r = mapMeshPolyline(mesh, {15, 5, 5, -5}, {5, 5, 5, 5});
    ASSERT_TRUE(r.error.empty()) << r.error;
    EXPECT_DOUBLE_EQ(r.length, 20);
    EXPECT_DOUBLE_EQ(r.covered_length, 10);
    EXPECT_DOUBLE_EQ(weightedLength(r), 10);
    ASSERT_EQ(r.intervals.size(), 2);
    EXPECT_EQ(r.intervals[0].cell, 0);
    EXPECT_EQ(r.intervals[1].cell, 0);
    EXPECT_DOUBLE_EQ(r.intervals[0].begin, 5);
    EXPECT_DOUBLE_EQ(r.intervals[1].end, 15);
}
TEST(ChannelHeatContacts, RotatingAndTranslatingAReachAndMeshDoesNotChangeItsContactLengths) {
    auto mesh = square();
    std::vector<double> x = {-5, 5, 15}, y = {2.5, 2.5, 8};
    const auto base = mapMeshPolyline(mesh, x, y);
    ASSERT_TRUE(base.error.empty());
    const double a = .731, c = std::cos(a), s = std::sin(a);
    const auto rotate = [&](std::vector<double>& xx, std::vector<double>& yy) {
        for (std::size_t i = 0; i < xx.size(); ++i) {
            const double nx = c * xx[i] - s * yy[i] + 1e4, ny = s * xx[i] + c * yy[i] - 2e4;
            xx[i] = nx;
            yy[i] = ny;
        }
    };
    rotate(mesh.vx, mesh.vy);
    rotate(x, y);
    auto r = mapMeshPolyline(mesh, x, y);
    ASSERT_TRUE(r.error.empty()) << r.error;
    ASSERT_EQ(r.intervals.size(), base.intervals.size());
    EXPECT_NEAR(r.covered_length, base.covered_length, 1e-10);
    for (std::size_t i = 0; i < r.intervals.size(); ++i) {
        EXPECT_EQ(r.intervals[i].cell, base.intervals[i].cell);
        EXPECT_NEAR(r.intervals[i].begin, base.intervals[i].begin, 1e-10);
        EXPECT_NEAR(r.intervals[i].end, base.intervals[i].end, 1e-10);
    }
}
TEST(ChannelHeatContacts, InvalidGeometryRefusesWithoutPublishingPartialContacts) {
    auto mesh = square();
    EXPECT_FALSE(mapMeshPolyline(mesh, {0, 1}, {0}).error.empty());
    EXPECT_FALSE(mapMeshPolyline(mesh, {0, 0}, {0, 0}).error.empty());
    auto r = mapMeshPolyline(mesh, {0, 1, std::numeric_limits<double>::infinity()}, {0, 1, 2});
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(r.intervals.empty());
    // Deliberate duplicate interiors, rather than a legitimate common edge.
    mesh.cell_v = {0, 1, 2, -1, 0, 1, 2, -1};
    r = mapMeshPolyline(mesh, {0, 8}, {1, 1});
    EXPECT_FALSE(r.error.empty());
    EXPECT_TRUE(r.intervals.empty());
    EXPECT_DOUBLE_EQ(r.covered_length, 0);
}
