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
 * @file test_object_deletion_many.cpp
 * @brief swmm_*_delete_many (perf plan Phase A1): the batch APIs must be
 *        observably IDENTICAL to sequential per-object deletes in descending
 *        index order — final state, name index, cross-references and cascade
 *        entry set — while rebuilding the name→index map once per batch.
 */

#include <gtest/gtest.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_edit.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <set>
#include <string>
#include <tuple>
#include <vector>

namespace {

// Full observable identity of the network topology: every name in index
// order per type, plus link endpoints and subcatch outlets by NAME.
struct NetworkState {
    std::vector<std::string> nodes, links, subcatches, gages;
    std::vector<std::pair<std::string, std::string>> link_ends;
    std::vector<std::string> subcatch_outlets;   // "" = none

    bool operator==(const NetworkState& o) const {
        return nodes == o.nodes && links == o.links
            && subcatches == o.subcatches && gages == o.gages
            && link_ends == o.link_ends
            && subcatch_outlets == o.subcatch_outlets;
    }
};

NetworkState capture(SWMM_Engine e)
{
    NetworkState s;
    const int nn = swmm_node_count(e);
    for (int i = 0; i < nn; ++i) s.nodes.emplace_back(swmm_node_id(e, i));
    const int nl = swmm_link_count(e);
    for (int i = 0; i < nl; ++i) {
        s.links.emplace_back(swmm_link_id(e, i));
        int a = -1, b = -1;
        swmm_link_get_from_node(e, i, &a);
        swmm_link_get_to_node(e, i, &b);
        s.link_ends.emplace_back(a >= 0 ? swmm_node_id(e, a) : "",
                                 b >= 0 ? swmm_node_id(e, b) : "");
    }
    const int nsc = swmm_subcatch_count(e);
    for (int i = 0; i < nsc; ++i) {
        s.subcatches.emplace_back(swmm_subcatch_id(e, i));
        int out = -1;
        swmm_subcatch_get_outlet(e, i, &out);
        s.subcatch_outlets.emplace_back(out >= 0 ? swmm_node_id(e, out) : "");
    }
    const int ng = swmm_gage_count(e);
    for (int i = 0; i < ng; ++i) s.gages.emplace_back(swmm_gage_id(e, i));
    return s;
}

// Cascade entries as an order-independent multiset of (type, idx, field,
// cascaded) — the batch aggregates per-object reports, whose relative order
// within one delete is fixed but whose concatenation order across deletes is
// the descending sequence.
std::multiset<std::tuple<int, int, std::string, int>>
entrySet(const SWMM_ImpactReport& r)
{
    std::multiset<std::tuple<int, int, std::string, int>> out;
    for (int i = 0; i < r.n_entries; ++i)
        out.insert({r.entries[i].obj_type, r.entries[i].obj_idx,
                    r.entries[i].field ? r.entries[i].field : "",
                    r.entries[i].cascaded});
    return out;
}

/// Chain of `n` junctions J0..J(n-1) linked in sequence, one subcatchment
/// draining to every 4th node, one gage per 8 nodes.
void buildChain(SWMM_Engine e, int n)
{
    for (int i = 0; i < n; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "J%d", i);
        ASSERT_EQ(swmm_node_add(e, id, SWMM_NODE_JUNCTION), SWMM_OK);
    }
    for (int i = 0; i + 1 < n; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "C%d", i);
        ASSERT_EQ(swmm_link_add(e, id, SWMM_LINK_CONDUIT), SWMM_OK);
    }
    for (int i = 0; i < n / 4; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "S%d", i);
        ASSERT_EQ(swmm_subcatch_add(e, id), SWMM_OK);
    }
    for (int i = 0; i < n / 8; ++i) {
        char id[32];
        std::snprintf(id, sizeof(id), "G%d", i);
        ASSERT_EQ(swmm_gage_add(e, id), SWMM_OK);
    }
    // Wiring AFTER all adds (add's resize re-initialises SoA fields).
    for (int i = 0; i + 1 < n; ++i)
        ASSERT_EQ(swmm_link_set_nodes(e, i, i, i + 1), SWMM_OK);
    for (int i = 0; i < n / 4; ++i)
        ASSERT_EQ(swmm_subcatch_set_outlet(e, i, i * 4), SWMM_OK);
}

} // namespace

// ---------------------------------------------------------------------------

TEST(DeleteManyTest, NodesMatchSequentialDescending)
{
    // Batch on engine A, sequential DESCENDING per-object on engine B; the
    // observable state and the cascade entry sets must be identical.
    SWMM_Engine a = swmm_engine_new();
    SWMM_Engine b = swmm_engine_new();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    buildChain(a, 24);
    buildChain(b, 24);

    // Unsorted, with a duplicate — the batch dedupes and sorts internally.
    const std::vector<int> victims{3, 17, 8, 3, 12, 0};

    SWMM_ImpactReport batchRep{};
    ASSERT_EQ(swmm_node_delete_many(a, victims.data(),
                                    static_cast<int>(victims.size()),
                                    &batchRep),
              SWMM_OK);

    std::vector<int> seq = victims;
    std::sort(seq.begin(), seq.end(), std::greater<int>());
    seq.erase(std::unique(seq.begin(), seq.end()), seq.end());
    std::multiset<std::tuple<int, int, std::string, int>> seqEntries;
    for (int idx : seq) {
        SWMM_ImpactReport r{};
        ASSERT_EQ(swmm_node_delete(b, idx, &r), SWMM_OK);
        for (auto& t : entrySet(r)) seqEntries.insert(t);
        swmm_impact_report_free(&r);
    }

    EXPECT_TRUE(capture(a) == capture(b));
    EXPECT_EQ(entrySet(batchRep), seqEntries);
    swmm_impact_report_free(&batchRep);

    // The deferred name→index map really was rebuilt: every survivor
    // resolves to its post-batch index by NAME.
    const int nn = swmm_node_count(a);
    for (int i = 0; i < nn; ++i)
        EXPECT_EQ(swmm_node_index(a, swmm_node_id(a, i)), i);
    const int nl = swmm_link_count(a);
    for (int i = 0; i < nl; ++i)
        EXPECT_EQ(swmm_link_index(a, swmm_link_id(a, i)), i);

    swmm_engine_destroy(a);
    swmm_engine_destroy(b);
}

TEST(DeleteManyTest, LinksSubcatchesGagesMatchSequential)
{
    SWMM_Engine a = swmm_engine_new();
    SWMM_Engine b = swmm_engine_new();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    buildChain(a, 24);
    buildChain(b, 24);

    const std::vector<int> linkVictims{1, 20, 7};
    const std::vector<int> scVictims{0, 4};
    const std::vector<int> gageVictims{2, 0};

    ASSERT_EQ(swmm_link_delete_many(a, linkVictims.data(), 3, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_delete_many(a, scVictims.data(), 2, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_gage_delete_many(a, gageVictims.data(), 2, nullptr), SWMM_OK);

    for (int idx : {20, 7, 1})
        ASSERT_EQ(swmm_link_delete(b, idx, nullptr), SWMM_OK);
    for (int idx : {4, 0})
        ASSERT_EQ(swmm_subcatch_delete(b, idx, nullptr), SWMM_OK);
    for (int idx : {2, 0})
        ASSERT_EQ(swmm_gage_delete(b, idx, nullptr), SWMM_OK);

    EXPECT_TRUE(capture(a) == capture(b));
    for (int i = 0; i < swmm_subcatch_count(a); ++i)
        EXPECT_EQ(swmm_subcatch_index(a, swmm_subcatch_id(a, i)), i);
    for (int i = 0; i < swmm_gage_count(a); ++i)
        EXPECT_EQ(swmm_gage_index(a, swmm_gage_id(a, i)), i);

    swmm_engine_destroy(a);
    swmm_engine_destroy(b);
}

TEST(DeleteManyTest, ValidatesEveryIndexBeforeMutating)
{
    SWMM_Engine e = swmm_engine_new();
    ASSERT_NE(e, nullptr);
    buildChain(e, 8);
    const NetworkState before = capture(e);

    // One bad index anywhere fails the whole call with no changes.
    const std::vector<int> bad{2, 5, 99};
    EXPECT_EQ(swmm_node_delete_many(e, bad.data(), 3, nullptr),
              SWMM_ERR_BADINDEX);
    EXPECT_TRUE(capture(e) == before);

    const std::vector<int> neg{2, -1};
    EXPECT_EQ(swmm_node_delete_many(e, neg.data(), 2, nullptr),
              SWMM_ERR_BADINDEX);
    EXPECT_TRUE(capture(e) == before);

    // NULL array with n > 0 is a parameter error; n == 0 is a clean no-op.
    EXPECT_EQ(swmm_node_delete_many(e, nullptr, 3, nullptr),
              SWMM_ERR_BADPARAM);
    SWMM_ImpactReport rep{};
    EXPECT_EQ(swmm_node_delete_many(e, nullptr, 0, &rep), SWMM_OK);
    EXPECT_EQ(rep.n_entries, 0);
    swmm_impact_report_free(&rep);
    EXPECT_TRUE(capture(e) == before);

    swmm_engine_destroy(e);
}

TEST(DeleteManyTest, BatchScalesWhereSequentialRehashes)
{
    // Not a timing assertion (CI noise) — a scale smoke test that also
    // PRINTS the batch-vs-sequential wall times so perf regressions are
    // visible in the log. 2000 deletes from a 6000-node chain.
    constexpr int kNodes  = 6000;
    constexpr int kDelete = 2000;

    SWMM_Engine a = swmm_engine_new();
    SWMM_Engine b = swmm_engine_new();
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    buildChain(a, kNodes);
    buildChain(b, kNodes);

    std::vector<int> victims;
    victims.reserve(kDelete);
    for (int i = 0; i < kDelete; ++i) victims.push_back(i * 3 + 1);

    using clock = std::chrono::steady_clock;
    const auto t0 = clock::now();
    ASSERT_EQ(swmm_node_delete_many(a, victims.data(), kDelete, nullptr),
              SWMM_OK);
    const auto t1 = clock::now();

    std::vector<int> seq = victims;
    std::sort(seq.begin(), seq.end(), std::greater<int>());
    for (int idx : seq)
        ASSERT_EQ(swmm_node_delete(b, idx, nullptr), SWMM_OK);
    const auto t2 = clock::now();

    const auto ms = [](auto d) {
        return std::chrono::duration_cast<std::chrono::milliseconds>(d).count();
    };
    std::printf("[delete_many perf] batch %lld ms, sequential %lld ms "
                "(%d of %d nodes)\n",
                static_cast<long long>(ms(t1 - t0)),
                static_cast<long long>(ms(t2 - t1)), kDelete, kNodes);

    EXPECT_TRUE(capture(a) == capture(b));
    for (int i = 0; i < swmm_node_count(a); ++i)
        EXPECT_EQ(swmm_node_index(a, swmm_node_id(a, i)), i);

    swmm_engine_destroy(a);
    swmm_engine_destroy(b);
}
