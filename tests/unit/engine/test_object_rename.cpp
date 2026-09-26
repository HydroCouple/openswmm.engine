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
 * @file test_object_rename.cpp
 * @brief In-place rename for snow packs, aquifers, inlets, streets and LID
 *        controls.
 *
 * @details These five object kinds had no rename API. The GUI's registries
 *          renamed their own copy and then flushed, at which point
 *          `swmm_*_index` reported the new name as unknown and the flush
 *          called `swmm_*_add` — producing a DUPLICATE object and orphaning
 *          the original along with everything referencing it.
 *
 *          The invariants pinned here:
 *            - the object count does not change across a rename;
 *            - `_id(idx)` and `_index(new)` agree afterwards (snow packs,
 *              aquifers and LID controls keep the name in BOTH a names vector
 *              and a NameIndex, so a half-update would desynchronise them);
 *            - the old name no longer resolves;
 *            - duplicate and empty names are rejected, and a pure
 *              case-respelling of the same object is allowed;
 *            - a street rename rewrites the named cross-section reference on
 *              every STREET_XSECT link, which is the one back-reference of the
 *              five that survives parsing.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_subcatchments.h>

namespace {

class ObjectRenameTest : public ::testing::Test {
protected:
    void SetUp() override {
        // swmm_engine_new (not swmm_engine_create) puts the context in
        // BUILDING, which the *_add and *_rename lifecycle guards require.
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
    }

    void TearDown() override {
        if (engine_ != nullptr) {
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }

    SWMM_Engine engine_ = nullptr;
};

// ---------------------------------------------------------------------------
// Snow packs — names vector + NameIndex must stay in lockstep
// ---------------------------------------------------------------------------

TEST_F(ObjectRenameTest, SnowpackRenameKeepsBothNameStoresInSync) {
    ASSERT_EQ(swmm_snowpack_add(engine_, "SP1"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_add(engine_, "SP2"), SWMM_OK);
    const int idx = swmm_snowpack_index(engine_, "SP1");
    ASSERT_GE(idx, 0);

    ASSERT_EQ(swmm_snowpack_rename(engine_, idx, "Residential"), SWMM_OK);

    // No duplicate was created.
    EXPECT_EQ(swmm_snowpack_count(engine_), 2);
    // The names vector (swmm_snowpack_id) and the registry (swmm_snowpack_index)
    // agree — this is what a half-update would break.
    EXPECT_STREQ(swmm_snowpack_id(engine_, idx), "Residential");
    EXPECT_EQ(swmm_snowpack_index(engine_, "Residential"), idx);
    EXPECT_LT(swmm_snowpack_index(engine_, "SP1"), 0);
    // The untouched neighbour keeps its slot.
    EXPECT_EQ(swmm_snowpack_index(engine_, "SP2"), 1);
}

TEST_F(ObjectRenameTest, SnowpackRenameRejectsDuplicateAndEmpty) {
    ASSERT_EQ(swmm_snowpack_add(engine_, "SP1"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_add(engine_, "SP2"), SWMM_OK);

    EXPECT_EQ(swmm_snowpack_rename(engine_, 0, "SP2"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_snowpack_rename(engine_, 0, "sp2"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_snowpack_rename(engine_, 0, ""), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_snowpack_rename(engine_, 0, nullptr), SWMM_ERR_BADPARAM);
    // Rejected renames leave the object untouched.
    EXPECT_STREQ(swmm_snowpack_id(engine_, 0), "SP1");
}

TEST_F(ObjectRenameTest, SnowpackRenameAllowsCaseRespelling) {
    ASSERT_EQ(swmm_snowpack_add(engine_, "sp1"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_rename(engine_, 0, "SP1"), SWMM_OK);
    EXPECT_STREQ(swmm_snowpack_id(engine_, 0), "SP1");
    // Lookup is case-insensitive, so both spellings still resolve to slot 0.
    EXPECT_EQ(swmm_snowpack_index(engine_, "sp1"), 0);
}

// ---------------------------------------------------------------------------
// Aquifers — same dual-store shape, no back-references
// ---------------------------------------------------------------------------

TEST_F(ObjectRenameTest, AquiferRenameKeepsBothNameStoresInSync) {
    ASSERT_EQ(swmm_aquifer_add(engine_, "AQ1"), SWMM_OK);
    const int idx = swmm_aquifer_index(engine_, "AQ1");
    ASSERT_GE(idx, 0);

    ASSERT_EQ(swmm_aquifer_rename(engine_, idx, "Upper"), SWMM_OK);

    EXPECT_EQ(swmm_aquifer_count(engine_), 1);
    EXPECT_STREQ(swmm_aquifer_id(engine_, idx), "Upper");
    EXPECT_EQ(swmm_aquifer_index(engine_, "Upper"), idx);
    EXPECT_LT(swmm_aquifer_index(engine_, "AQ1"), 0);
}

// ---------------------------------------------------------------------------
// LID controls — dual store, usage rows are index-based
// ---------------------------------------------------------------------------

TEST_F(ObjectRenameTest, LidRenameKeepsBothNameStoresInSync) {
    ASSERT_EQ(swmm_lid_add(engine_, "LID1", 0), SWMM_OK);
    const int idx = swmm_lid_index(engine_, "LID1");
    ASSERT_GE(idx, 0);

    ASSERT_EQ(swmm_lid_rename(engine_, idx, "BioRetention"), SWMM_OK);

    EXPECT_EQ(swmm_lid_count(engine_), 1);
    EXPECT_STREQ(swmm_lid_id(engine_, idx), "BioRetention");
    EXPECT_EQ(swmm_lid_index(engine_, "BioRetention"), idx);
    EXPECT_LT(swmm_lid_index(engine_, "LID1"), 0);
}

// ---------------------------------------------------------------------------
// Inlets — single names vector, linear case-insensitive lookup
// ---------------------------------------------------------------------------

TEST_F(ObjectRenameTest, InletRenameUpdatesNameAndRejectsDuplicate) {
    ASSERT_EQ(swmm_inlet_add(engine_, "IN1", "GRATE"), SWMM_OK);
    ASSERT_EQ(swmm_inlet_add(engine_, "IN2", "CURB"), SWMM_OK);

    ASSERT_EQ(swmm_inlet_rename(engine_, 0, "Grate_A"), SWMM_OK);
    EXPECT_EQ(swmm_inlet_count(engine_), 2);
    EXPECT_STREQ(swmm_inlet_id(engine_, 0), "Grate_A");
    EXPECT_EQ(swmm_inlet_index(engine_, "Grate_A"), 0);
    EXPECT_LT(swmm_inlet_index(engine_, "IN1"), 0);

    EXPECT_EQ(swmm_inlet_rename(engine_, 0, "in2"), SWMM_ERR_BADPARAM);
    EXPECT_STREQ(swmm_inlet_id(engine_, 0), "Grate_A");
}

// ---------------------------------------------------------------------------
// Streets — the one kind with a name reference that survives parsing
// ---------------------------------------------------------------------------

TEST_F(ObjectRenameTest, StreetRenameUpdatesNameAndRejectsDuplicate) {
    ASSERT_EQ(swmm_street_add(engine_, "ST1"), SWMM_OK);
    ASSERT_EQ(swmm_street_add(engine_, "ST2"), SWMM_OK);

    ASSERT_EQ(swmm_street_rename(engine_, 0, "Main"), SWMM_OK);
    EXPECT_EQ(swmm_street_count(engine_), 2);
    EXPECT_STREQ(swmm_street_id(engine_, 0), "Main");
    EXPECT_EQ(swmm_street_index(engine_, "Main"), 0);
    EXPECT_LT(swmm_street_index(engine_, "ST1"), 0);

    EXPECT_EQ(swmm_street_rename(engine_, 0, "st2"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_street_rename(engine_, 0, ""), SWMM_ERR_BADPARAM);
    EXPECT_STREQ(swmm_street_id(engine_, 0), "Main");
}

TEST_F(ObjectRenameTest, StreetRenameRewritesLinkCrossSectionReference) {
    // A street cross-section is referenced BY NAME from the link, and that
    // reference is what InpWriter emits as [XSECTIONS] Geom1. If the rename
    // does not rewrite it, swmm_link_get_xsect can no longer resolve the
    // street and the written .inp points at a name that does not exist.
    ASSERT_EQ(swmm_node_add(engine_, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine_, "O1", SWMM_NODE_OUTFALL), SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine_, "C1", SWMM_LINK_CONDUIT), SWMM_OK);
    ASSERT_EQ(swmm_street_add(engine_, "ST1"), SWMM_OK);

    const int link = swmm_link_index(engine_, "C1");
    ASSERT_GE(link, 0);
    ASSERT_EQ(swmm_link_set_nodes(engine_, link,
                                  swmm_node_index(engine_, "J1"),
                                  swmm_node_index(engine_, "O1")),
              SWMM_OK);
    const int street = swmm_street_index(engine_, "ST1");
    ASSERT_GE(street, 0);

    ASSERT_EQ(swmm_link_set_xsect(engine_, link, SWMM_XSECT_STREET,
                                  static_cast<double>(street), 0.0, 0.0, 0.0),
              SWMM_OK);

    ASSERT_EQ(swmm_street_rename(engine_, street, "Main"), SWMM_OK);

    // The link still resolves, and it resolves to the SAME street.
    int shape = -1;
    double g1 = -1.0, g2 = 0.0, g3 = 0.0, g4 = 0.0;
    ASSERT_EQ(swmm_link_get_xsect(engine_, link, &shape, &g1, &g2, &g3, &g4),
              SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_STREET);
    EXPECT_EQ(static_cast<int>(g1), street)
        << "street rename left the link's named cross-section reference stale";
}

}  // namespace
