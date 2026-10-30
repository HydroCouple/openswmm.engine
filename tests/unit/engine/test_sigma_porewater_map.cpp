// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/heat/SigmaPorewaterMap.hpp"
#include "2d/subsurface/SubsurfaceTransportState.hpp"
#include "core/CompletedSourceArchive.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>

using namespace openswmm::twoD;
namespace {
std::string snapshot(SigmaPorewaterMap& map) {
    openswmm::detail::CompletedSourceArchive a;
    a(map.layers, map.species, map.capacity, map.mass, map.reserved, map.pending);
    a.sequence(map.overlaps, [&](auto& o) { a(o.physical, o.reactor, o.water, o.bulk); });
    return a.bytes();
}
void retain(const std::string& name, SigmaPorewaterMap& map) {
    if (const char* root = std::getenv("OPENSWMM_SIGMA_MAP_TEST_OUT")) {
        std::filesystem::create_directories(root);
        const auto bytes = snapshot(map);
        std::ofstream(std::filesystem::path(root) / (name + ".bin"), std::ios::binary)
            .write(bytes.data(), bytes.size());
    }
}
}

TEST(SigmaPorewaterMap, ReusedBuffersMatchFreshMapsAndRefusalsKeepEveryField) {
    SigmaPorewaterMap reused, other;
    for (int iteration = 0; iteration < 24; ++iteration) {
        const int m = 1 + iteration % 4, ns = 1 + iteration % 3;
        std::vector<SigmaPorewaterMap::Overlap> entries;
        for (int r = 0; r <= m; ++r)
            for (int j = 0; j < 1 + iteration % 3; ++j)
                entries.push_back({j, r, .2 + .03 * j, .5 + .01 * r});
        std::vector<double> partition(ns, .3), canonical(ns * (m + 1)), held(ns, .01);
        for (std::size_t k = 0; k < canonical.size(); ++k)
            canonical[k] = 1 + .1 * k + iteration;
        SigmaPorewaterMap fresh;
        ASSERT_TRUE(fresh.configure(m, ns, entries, partition, canonical, held).empty());
        ASSERT_TRUE(reused.configure(m, ns, entries, partition, canonical, held).empty());
        EXPECT_EQ(snapshot(reused), snapshot(fresh));
        // Interleave another map to exercise shared per-thread staging storage.
        ASSERT_TRUE(other.configure(m, ns, {}, partition, canonical, std::vector<double>(ns)).empty());
        ASSERT_TRUE(reused.apply(0, 0, .001));
        const auto before = snapshot(reused);
        auto invalid = canonical;
        invalid.back() = -1;
        EXPECT_FALSE(reused.rebase(invalid, held).empty());
        EXPECT_EQ(snapshot(reused), before);
        EXPECT_FALSE(reused.configure(m, ns, entries, partition, invalid, held).empty());
        EXPECT_EQ(snapshot(reused), before);
        auto bad_geometry = entries;
        bad_geometry.back().bulk = -1;
        EXPECT_FALSE(reused.configure(m, ns, bad_geometry, partition, canonical, held).empty());
        EXPECT_EQ(snapshot(reused), before);
        ASSERT_TRUE(reused.rebase(canonical, held).empty());
        retain("rebase-" + std::to_string(iteration), reused);
    }
}

TEST(SigmaPorewaterMap, PublishCommitsOnceAndInvalidOwnerLeavesMapAndInventoryUnchanged) {
    SubsurfaceTransportState tr;
    tr.resize(2, 1, 0);
    tr.n_layers = 2;
    tr.layer_mass = {1, 2, 3, 4};
    tr.sat_mass = {5, 6};
    SigmaPorewaterMap map;
    ASSERT_TRUE(map.configure(2, 2, {{0, 0, .2, .4}, {0, 1, .3, .4}, {1, 2, .4, .6}},
                              {0., .1}, {1, 2, 5, 3, 4, 6}, {0., 0.}).empty());
    ASSERT_TRUE(map.apply(0, 0, .5));
    const auto before = snapshot(map);
    const auto layers = tr.layer_mass, sat = tr.sat_mass;
    map.pending.back() = -100;
    const auto invalid = snapshot(map);
    EXPECT_FALSE(map.publish(tr, 0).empty());
    EXPECT_EQ(snapshot(map), invalid);
    EXPECT_EQ(tr.layer_mass, layers);
    EXPECT_EQ(tr.sat_mass, sat);
    map.pending.back() = 0;
    EXPECT_EQ(snapshot(map), before);
    ASSERT_TRUE(map.publish(tr, 0).empty());
    const auto accepted = snapshot(map);
    const auto accepted_layers = tr.layer_mass;
    ASSERT_TRUE(map.publish(tr, 0).empty());
    EXPECT_EQ(snapshot(map), accepted);
    EXPECT_EQ(tr.layer_mass, accepted_layers);
    EXPECT_NEAR(tr.layer_mass[0] + tr.layer_mass[1], 3.5, 1e-15);
    retain("published", map);
}

TEST(SigmaPorewaterMap, DryUnsortedOverlapsOverwriteStagingAndKeepCanonicalTemperature) {
    const std::vector<double> canonical{1, 2, 3, 4, 0, 0, 0, 0};
    const std::vector<SigmaPorewaterMap::Overlap> wet{
        {2, 2, .1, .4}, {0, 0, .2, .2}, {1, 2, .3, .3}};
    auto dry = wet;
    for (auto& entry : dry)
        entry.water = 0;
    SigmaPorewaterMap map;
    ASSERT_TRUE(map.configure(3, 2, wet, {0., 0.}, canonical, {0., 0.}).empty());
    ASSERT_TRUE(map.configure(3, 2, dry, {0., 0.}, canonical, {0., 0.}).empty());
    EXPECT_EQ(map.mass, std::vector<double>(6, 0));
    EXPECT_EQ(map.reserved, std::vector<double>(6, 0));
    const auto before = snapshot(map);
    EXPECT_FALSE(map.rebase(canonical, {1., 0.}).empty());
    EXPECT_EQ(snapshot(map), before);
    SubsurfaceTransportState tr;
    tr.resize(2, 1, 0);
    tr.n_layers = 3;
    tr.temp_row = 1;
    tr.layer_mass = {1, 2, 3, -40, 50, -60};
    tr.sat_mass = {4, -70};
    const auto layers = tr.layer_mass, sat = tr.sat_mass;
    ASSERT_TRUE(map.publish(tr, 0).empty());
    EXPECT_EQ(tr.layer_mass, layers);
    EXPECT_EQ(tr.sat_mass, sat);
    retain("dry-temperature", map);
}
