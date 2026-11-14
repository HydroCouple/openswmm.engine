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
 * @file      test_object_rename.cpp
 * @brief     In-place rename for snow packs, aquifers, inlets, streets and LID
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <string>
#include <filesystem>
#include <fstream>
#include <openswmm/engine/openswmm_inflows.h>
#include <openswmm/engine/openswmm_quality.h>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_tables.h>
#include <openswmm/engine/openswmm_controls.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_subcatchments.h>

namespace {

class ObjectRenameTest : public ::testing::Test {
protected:
    /**
     * @brief Create an editable BUILDING engine for object rename tests.
     *
     * Uses swmm_engine_new and performs no model open or simulation. Fatal GoogleTest assertions return
     * from this hook on failure; any created handle remains owned by the fixture for teardown.
     */
    void SetUp() override {
        // swmm_engine_new (not swmm_engine_create) puts the context in
        // BUILDING, which the *_add and *_rename lifecycle guards require.
        engine_ = swmm_engine_new();
        ASSERT_NE(engine_, nullptr);
    }

    /**
     * @brief Destroy a non-null fixture handle and clear its member slot.
     *
     * Engine destruction supplies remaining model cleanup; safe after setup failed before allocation.
     */
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

TEST_F(ObjectRenameTest, SubcatchmentRenamePreservesSnowRemovalDestination) {
    ASSERT_EQ(swmm_subcatch_add(engine_, "CatchA"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_add(engine_, "CatchB"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_add(engine_, "Selected"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_add(engine_, "Control"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_set_removal_subcatch(engine_, 0, "catcha"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_set_removal_subcatch(engine_, 1, "CatchB"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_set_removal(engine_, 0, 2, .1, .1, .1, .1, .2), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_rename(engine_, 0, "Renamed"), SWMM_OK);
    char destination[128]{};
    ASSERT_EQ(swmm_snowpack_get_removal_subcatch(engine_, 0, destination, sizeof(destination)), SWMM_OK);
    EXPECT_STREQ(destination, "Renamed");
    double threshold, out, imp, perv, melt, subcatch;
    ASSERT_EQ(swmm_snowpack_get_removal(engine_, 0, &threshold, &out, &imp, &perv, &melt, &subcatch), SWMM_OK);
    EXPECT_DOUBLE_EQ(threshold, 2);
    EXPECT_DOUBLE_EQ(subcatch, .2);
    ASSERT_EQ(swmm_subcatch_rename(engine_, 0, "RENAMED"), SWMM_OK);
    ASSERT_EQ(swmm_snowpack_get_removal_subcatch(engine_, 0, destination, sizeof(destination)), SWMM_OK);
    EXPECT_STREQ(destination, "RENAMED");
    EXPECT_EQ(swmm_subcatch_rename(engine_, 0, "CatchB"), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_snowpack_get_removal_subcatch(engine_, 0, destination, sizeof(destination)), SWMM_OK);
    EXPECT_STREQ(destination, "RENAMED");
    ASSERT_EQ(swmm_snowpack_get_removal_subcatch(engine_, 1, destination, sizeof(destination)), SWMM_OK);
    EXPECT_STREQ(destination, "CatchB");
}

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
    ASSERT_EQ(swmm_link_set_nodes(engine_, link, swmm_node_index(engine_, "J1"),
                                  swmm_node_index(engine_, "O1")),
              SWMM_OK);
    const int street = swmm_street_index(engine_, "ST1");
    ASSERT_GE(street, 0);

    ASSERT_EQ(swmm_link_set_xsect(engine_, link, SWMM_XSECT_STREET, static_cast<double>(street),
                                  0.0, 0.0, 0.0),
              SWMM_OK);

    ASSERT_EQ(swmm_street_rename(engine_, street, "Main"), SWMM_OK);

    // The link still resolves, and it resolves to the SAME street.
    int shape = -1;
    double g1 = -1.0, g2 = 0.0, g3 = 0.0, g4 = 0.0;
    ASSERT_EQ(swmm_link_get_xsect(engine_, link, &shape, &g1, &g2, &g3, &g4), SWMM_OK);
    EXPECT_EQ(shape, SWMM_XSECT_STREET);
    EXPECT_EQ(static_cast<int>(g1), street)
        << "street rename left the link's named cross-section reference stale";
}

} // namespace

TEST_F(ObjectRenameTest, AtomicTablePayloadValidatesBeforePublication) {
    ASSERT_EQ(swmm_timeseries_add(engine_,"Series"),SWMM_OK);
    const int idx=swmm_table_index(engine_,"Series");
    const double x[]{46023.,46023.5},y[]{1.,2.};
    ASSERT_EQ(swmm_table_replace_points(engine_,idx,x,y,2,2,46023.,""),SWMM_OK);
    const double bad[]{46023.,46023.};
    EXPECT_EQ(swmm_table_replace_points(engine_,idx,bad,y,2,0,0.,""),SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_table_validate_points(engine_,idx,x,y,2,3,46023.),SWMM_ERR_BADPARAM);
    double xx=0,yy=0,anchor=0;int count=0,relative=0;
    ASSERT_EQ(swmm_table_get_point_count(engine_,idx,&count),SWMM_OK);EXPECT_EQ(count,2);
    ASSERT_EQ(swmm_table_get_point(engine_,idx,1,&xx,&yy),SWMM_OK);EXPECT_DOUBLE_EQ(xx,x[1]);EXPECT_DOUBLE_EQ(yy,y[1]);
    ASSERT_EQ(swmm_timeseries_get_relative_info(engine_,idx,&relative,&anchor),SWMM_OK);EXPECT_EQ(relative,2);EXPECT_DOUBLE_EQ(anchor,46023.);
    const double changed[]{3.,4.};ASSERT_EQ(swmm_table_validate_points(engine_,idx,x,changed,2,0,0.),SWMM_OK);
    ASSERT_EQ(swmm_table_get_point(engine_,idx,0,&xx,&yy),SWMM_OK);EXPECT_DOUBLE_EQ(yy,1.);
}
TEST_F(ObjectRenameTest, WholeRuleProgramSharesDeclarationsAndRejectsWithoutMutation) {
    ASSERT_EQ(swmm_node_add(engine_,"J1",SWMM_NODE_JUNCTION),SWMM_OK);
    ASSERT_EQ(swmm_node_add(engine_,"J2",SWMM_NODE_JUNCTION),SWMM_OK);
    ASSERT_EQ(swmm_link_add(engine_,"L",SWMM_LINK_CONDUIT),SWMM_OK);
    ASSERT_EQ(swmm_link_set_nodes(engine_,0,0,1),SWMM_OK);
    const char* blocks[]{"VARIABLE Depth = NODE J1 DEPTH\nRULE First\nIF Depth > 1\nTHEN CONDUIT L STATUS = CLOSED\n",
        "RULE Second\nIF Depth <= 1\nTHEN CONDUIT L STATUS = OPEN\n"};
    char error[4096]{};ASSERT_EQ(swmm_control_replace_program(engine_,blocks,2,0,error,sizeof(error)),SWMM_OK)<<error;
    EXPECT_EQ(swmm_control_count(engine_),0);
    ASSERT_EQ(swmm_control_replace_program(engine_,blocks,2,1,error,sizeof(error)),SWMM_OK)<<error;
    char id[128]{};ASSERT_EQ(swmm_control_get_id(engine_,0,id,sizeof(id)),SWMM_OK);EXPECT_STREQ(id,"First");
    const char* invalid[]{blocks[0],"RULE Second\nIF NODE Missing DEPTH > 1\nTHEN CONDUIT L STATUS = CLOSED\n"};
    EXPECT_EQ(swmm_control_replace_program(engine_,invalid,2,1,error,sizeof(error)),SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_control_count(engine_),2);char body[4096]{};
    ASSERT_EQ(swmm_control_get_rule(engine_,1,body,sizeof(body)),SWMM_OK);EXPECT_STREQ(body,blocks[1]);
}

TEST_F(ObjectRenameTest, SaveReopenRetainsInactiveSnowAndForwardExternalBuildup) {
    const auto folder=std::filesystem::path("test-output/bulk-record-persistence");
    std::filesystem::create_directories(folder);
    const auto source=folder/"source.inp",saved=folder/"saved.inp";
    {std::ofstream file(source);file<<R"([OPTIONS]
FLOW_UNITS CFS
START_DATE 01/01/2026
END_DATE 01/02/2026
[JUNCTIONS]
J1 0 10
[OUTFALLS]
O1 0 FREE NO
[CONDUITS]
C1 J1 O1 100 .013 0 0
[XSECTIONS]
C1 CIRCULAR 1 0 0 0 1
[POLLUTANTS]
TSS MG/L 0 0 0 0 NO
[LANDUSES]
LU 1 .2 0
[BUILDUP]
LU TSS EXT 2 .3 TS AREA
[TIMESERIES]
TS 01/01/2026 0:00 1
TS 01/01/2026 1:00 2
[HYDROGRAPHS]
H ALL SHORT .1 1 2 .5 .2 .1
[RDII_DECAY]
H SHORT .2 .1 .3 10 .02 -1 NOSNOW 3.5 1.25
)";}
    swmm_engine_destroy(engine_);engine_=swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(engine_,source.string().c_str(),nullptr,nullptr,nullptr),SWMM_OK) << swmm_get_last_error_msg(engine_);
    ASSERT_EQ(swmm_model_write(engine_,saved.string().c_str()),SWMM_OK);
    auto reopened=swmm_engine_create();ASSERT_NE(reopened,nullptr);
    ASSERT_EQ(swmm_engine_open(reopened,saved.string().c_str(),nullptr,nullptr,nullptr),SWMM_OK);
    char name[128]{};int response=0,on=1;double d[8]{};
    ASSERT_EQ(swmm_rdii_decay_get(reopened,0,name,sizeof(name),&response,&d[0],&d[1],&d[2],&d[3],&d[4],&d[5],&on,&d[6],&d[7]),SWMM_OK);
    EXPECT_EQ(on,0);EXPECT_DOUBLE_EQ(d[6],3.5);EXPECT_DOUBLE_EQ(d[7],1.25);
    int type=0,normalizer=0;double c1=0,c2=0,c3=-1;
    ASSERT_EQ(swmm_buildup_get(reopened,0,0,&type,&c1,&c2,&c3,&normalizer),SWMM_OK);
    EXPECT_EQ(type,4);ASSERT_GE(c3,0);EXPECT_STREQ(swmm_table_id(reopened,int(c3)),"TS");
    swmm_engine_destroy(reopened);
}
