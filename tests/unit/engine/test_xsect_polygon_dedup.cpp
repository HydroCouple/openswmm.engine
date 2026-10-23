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
 * @file test_xsect_polygon_dedup.cpp
 * @brief promptperf.md Phase C — compiled-section deduplication in
 *        PostParseResolver.cpp's POLYGON block.
 *
 * @details Phase C asks every compiled `ChebSection` to be keyed by its
 *          full geometric identity, with exact bit equality (not a
 *          tolerance) so "near-equal geometries are different geometries."
 *          Two dedup mechanisms already existed before this phase (both
 *          shipped in the original Phase 5 commit, predating promptperf.md):
 *          POLYGON links memoized by (curve, scale), and built-in shapes
 *          under `XSECT_GEOMETRY EXACT` memoized by (shape, y_full, w_max).
 *          Auditing the POLYGON key against `chebsec::compile()`'s actual
 *          parameters found it incomplete: `compile()` takes an `is_open`
 *          argument that changes the compiled result
 *          (`ChebSection::is_open`, and the `a_max`/`s_max` branch —
 *          `ChebSection.cpp`, `compile()`), but the memo key was only
 *          `(curve, scale)`. Two POLYGON links referencing the same curve
 *          and scale, one with Geom2 (the open/closed flag) set and one
 *          not, would silently share a single compiled section carrying
 *          only the FIRST link's open/closed classification — exactly the
 *          class of under-inclusive-memoization bug this project has found
 *          and fixed repeatedly elsewhere (the FV `geom_key()` not hashing
 *          `xs.cheb`, the packed-view `cheb` mirror in `setBypassMask`; see
 *          SWMM_dev/.memory/current.md's Phase 5/5B entries).
 *
 * @see PostParseResolver.cpp's POLYGON block (`polygon_memo`).
 * @ingroup engine_hydraulics
 */

#include <gtest/gtest.h>

#include "core/SimulationContext.hpp"
#include "data/TableData.hpp"
#include "input/PostParseResolver.hpp"

using openswmm::LinkType;
using openswmm::SimulationContext;
using openswmm::TableType;
using openswmm::XsectShape;

namespace {

/// A simple, valid, non-self-intersecting 4-point square boundary — CCW,
/// straight edges only (no bulge column). Big enough to compile to a
/// non-degenerate ChebSection; the exact shape is otherwise irrelevant to
/// this test, which is about which links SHARE a compiled section, not
/// about the section's own geometry (that is XSectBoundary/ChebSection's
/// own test coverage).
int addSquareCurve(SimulationContext& ctx, const std::string& name) {
    const int ci = ctx.tables.add(name, TableType::CURVE_XPOLYGON);
    auto& tbl = ctx.tables[ci];
    tbl.x = {-1.0, 1.0, 1.0, -1.0};
    tbl.y = {-1.0, -1.0, 1.0, 1.0};
    return ci;
}

/// Registers one CONDUIT link with a POLYGON cross-section referencing
/// curve `curve_name`, scale `scale` (Geom1) and open/closed flag `is_open`
/// (Geom2) — mirrors PostParseResolver's own reading of those two fields.
void addPolygonLink(SimulationContext& ctx, int idx, const std::string& curve_name,
                     double scale, bool is_open) {
    auto uj = static_cast<std::size_t>(idx);
    const auto cr = static_cast<std::size_t>(
        ctx.link_subtypes.set_link_type(ctx.links, idx, LinkType::CONDUIT));
    ctx.links.direction[uj] = 1;
    ctx.link_subtypes.conduits.barrels[cr] = 1;
    ctx.link_subtypes.conduits.length[cr] = 100.0;

    ctx.links.xsect_shape[uj] = XsectShape::POLYGON;
    ctx.links.pump_curve_name[uj] = curve_name;
    ctx.links.xsect_geom1[uj] = scale;
    ctx.links.xsect_geom2[uj] = is_open ? 1.0 : 0.0;
}

/// Minimal two-node, N-link network with every link a POLYGON conduit.
void buildModel(SimulationContext& ctx, int n_links) {
    ctx.node_names.add("N0");
    ctx.node_names.add("N1");
    ctx.nodes.resize(2);
    for (int i = 0; i < 2; ++i) {
        auto ui = static_cast<std::size_t>(i);
        ctx.nodes.type[ui] = openswmm::NodeType::JUNCTION;
        ctx.nodes.invert_elev[ui] = 0.0;
        ctx.nodes.full_depth[ui] = 10.0;
    }
    for (int i = 0; i < n_links; ++i)
        ctx.link_names.add("L" + std::to_string(i));
    ctx.links.resize(n_links);
}

} // namespace

// ===========================================================================
// The bug: same curve + scale, different open/closed flag, MUST NOT share.
// ===========================================================================

TEST(PolygonSectionDedup, DifferingOpenFlagCompilesSeparateSections) {
    SimulationContext ctx;
    buildModel(ctx, 2);
    addSquareCurve(ctx, "SQ");

    addPolygonLink(ctx, 0, "SQ", /*scale=*/1.0, /*is_open=*/false);
    addPolygonLink(ctx, 1, "SQ", /*scale=*/1.0, /*is_open=*/true);

    openswmm::input::resolve_cross_references(ctx);

    ASSERT_GE(ctx.links.xsect_cheb_idx[0], 0);
    ASSERT_GE(ctx.links.xsect_cheb_idx[1], 0);

    // The whole point of Phase C dedup is index-sharing when the geometric
    // identity truly matches. Here it must NOT match: is_open differs.
    EXPECT_NE(ctx.links.xsect_cheb_idx[0], ctx.links.xsect_cheb_idx[1])
        << "closed and open POLYGON links sharing a curve+scale must not "
           "alias one compiled ChebSection";

    const auto& closed = ctx.cheb_sections[static_cast<std::size_t>(ctx.links.xsect_cheb_idx[0])];
    const auto& open    = ctx.cheb_sections[static_cast<std::size_t>(ctx.links.xsect_cheb_idx[1])];
    EXPECT_FALSE(closed.is_open);
    EXPECT_TRUE(open.is_open);

    // Mutation check: if the memo key regresses to (curve, scale) only,
    // both links resolve to the SAME index and this equality would flip to
    // true while is_open on link 1 would silently read false (the closed
    // section's value) — this is precisely the bug being pinned.
}

// ===========================================================================
// The feature this bug sits inside of: true duplicates DO still share.
// ===========================================================================

TEST(PolygonSectionDedup, IdenticalCurveScaleAndOpenFlagShareOneSection) {
    SimulationContext ctx;
    buildModel(ctx, 3);
    addSquareCurve(ctx, "SQ");

    addPolygonLink(ctx, 0, "SQ", /*scale=*/2.0, /*is_open=*/false);
    addPolygonLink(ctx, 1, "SQ", /*scale=*/2.0, /*is_open=*/false);
    addPolygonLink(ctx, 2, "SQ", /*scale=*/2.0, /*is_open=*/false);

    openswmm::input::resolve_cross_references(ctx);

    ASSERT_GE(ctx.links.xsect_cheb_idx[0], 0);
    EXPECT_EQ(ctx.links.xsect_cheb_idx[0], ctx.links.xsect_cheb_idx[1]);
    EXPECT_EQ(ctx.links.xsect_cheb_idx[0], ctx.links.xsect_cheb_idx[2]);

    // Only one section should have been compiled for these three identical
    // links — the dedup pool, not one-per-link.
    EXPECT_EQ(static_cast<int>(ctx.cheb_sections.size()), 1);
}

TEST(PolygonSectionDedup, DifferingScaleCompilesSeparateSections) {
    SimulationContext ctx;
    buildModel(ctx, 2);
    addSquareCurve(ctx, "SQ");

    addPolygonLink(ctx, 0, "SQ", /*scale=*/1.0, /*is_open=*/false);
    addPolygonLink(ctx, 1, "SQ", /*scale=*/2.0, /*is_open=*/false);

    openswmm::input::resolve_cross_references(ctx);

    ASSERT_GE(ctx.links.xsect_cheb_idx[0], 0);
    ASSERT_GE(ctx.links.xsect_cheb_idx[1], 0);
    EXPECT_NE(ctx.links.xsect_cheb_idx[0], ctx.links.xsect_cheb_idx[1]);
    EXPECT_EQ(static_cast<int>(ctx.cheb_sections.size()), 2);

    const auto& small = ctx.cheb_sections[static_cast<std::size_t>(ctx.links.xsect_cheb_idx[0])];
    const auto& big    = ctx.cheb_sections[static_cast<std::size_t>(ctx.links.xsect_cheb_idx[1])];
    EXPECT_LT(small.y_full, big.y_full);
}
