/**
 * @file      test_delete_record.cpp
 * @brief     Recorded deletes: swmm_*_delete_recorded + swmm_delete_record_restore put the model
 *            back exactly (same indices, order and references).
 *
 * @details Each case writes its input and every .inp it saves under
 *          tests/output/delete_record_2026-11-12/<case>/ for review.
 *
 * @ingroup engine_tests
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_gages.h>
#include <openswmm/engine/openswmm_heat.h>
#include <openswmm/engine/openswmm_subcatchments.h>
#include <openswmm/engine/openswmm_edit.h>
#include <openswmm/engine/openswmm_inflows.h>
#include <openswmm/engine/openswmm_infrastructure.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_reactions.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

const fs::path kOutRoot = fs::path(OPENSWMM_DELETE_RECORD_OUT_DIR);

// Every gage field the .inp carries, and every kind of reference to a gage:
// subcatchments before and after the deleted gage's index, a unit-hydrograph
// group, a user flag and map coordinates. G2 (the middle gage) is deleted.
const char* kGagesInp = R"([TITLE]
Delete-record round trip: every gage field and every reference to a gage

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2024
START_TIME           00:00:00
END_DATE             01/01/2024
END_TIME             06:00:00

[RAINGAGES]
;;Name  Format     Intvl  SCF   Source
G1      INTENSITY  0:15   1.0   TIMESERIES TS1
G2      VOLUME     0:05   0.8   FILE "rain.dat" STA42 MM * 2.5
G3      CUMULATIVE 1:00   1.1   TIMESERIES TS1

[SUBCATCHMENTS]
;;Name  RainGage Outlet Area %Imperv Width %Slope CurbLen
S1      G2       J1     1    50      100   0.5    0
S2      G3       J1     2    40      100   0.5    0
S3      G2       J1     3    30      100   0.5    0

[SUBAREAS]
S1      0.01  0.1  0.05  0.05  25  OUTLET
S2      0.01  0.1  0.05  0.05  25  OUTLET
S3      0.01  0.1  0.05  0.05  25  OUTLET

[INFILTRATION]
S1      3.0  0.5  4  7  0
S2      3.0  0.5  4  7  0
S3      3.0  0.5  4  7  0

[JUNCTIONS]
J1      0  5  0  0  0

[OUTFALLS]
O1      -1  FREE  NO

[CONDUITS]
C1      J1  O1  100  0.01  0  0  0  0

[XSECTIONS]
C1      CIRCULAR  1  0  0  0  1

[HYDROGRAPHS]
UH1     G2
UH1     ALL  SHORT  0.1  1  2

[TIMESERIES]
TS1     0:00  0.5
TS1     1:00  0.0

[USER_FLAGS]
Owner   STRING

[USER_FLAG_VALUES]
GAGE    G2   Owner  city
GAGE    G3   Owner  county

[COORDINATES]
J1      0    0
O1      100  0

[SYMBOLS]
G1      10   10
G2      20   20
G3      30   30
)";

// One link of every kind. GUT1 (a street conduit, index 1) carries every kind of reference a
// link can have in an .inp: a divider's diverted link, an inlet usage, losses, bend vertices, a
// tag, a user flag and initial quality. C3 and Drain sit after it, so their initial quality and
// LID node outlet are renumbered by its delete. Heat overrides are added through the API.
const char* kLinksInp = R"([TITLE]
Delete-record round trip: links of every kind and every reference to a link

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2024
START_TIME           00:00:00
END_DATE             01/01/2024
END_TIME             06:00:00
ROUTING_STEP         0:00:30

[JUNCTIONS]
J3      11  5  0  0  0
J4      10  5  0  0  0
J5      10  5  0  0  0

[OUTFALLS]
O1      8  FREE  *  NO

[STORAGE]
Store   0  2  1  FUNCTIONAL  0  0  100  0  0

[LID_CONTROLS]
Stack   NODE
Stack   SURFACE    6   .1
Stack   MEDIA      12  .45  .2  .08  2  10  3
Stack   AGGREGATE  6   .4   100
Stack   BOTTOM     .5  0

[LID_NODES]
Store   Stack  10  BOTTOM  SEALED

[DIVIDERS]
D1      12  GUT1  CUTOFF  0.5  5  0  0  0

[CONDUITS]
C1      D1  J3  300  0.013  0  0
GUT1    D1  J4  300  0.016  0  0
C3      J4  O1  300  0.013  0  0

[PUMPS]
P1      J3  J4  PC1  ON  0  0

[ORIFICES]
OR1     J3     J5  SIDE    0  0.65  NO  0
Drain   Store  O1  BOTTOM  0  .6    NO  0

[WEIRS]
W1      J3  J4  TRANSVERSE  1.0  3.33  NO  0  0  YES

[OUTLETS]
OL1     J5  O1  0  FUNCTIONAL/DEPTH  10  0.5  NO

[XSECTIONS]
C1      CIRCULAR   1    0  0  0  1
GUT1    STREET     ST_A
C3      CIRCULAR   2    0  0  0  1
OR1     CIRCULAR   1    0  0  0
Drain   CIRCULAR   .1   0  0  0
W1      RECT_OPEN  1.0  2.0  0  0

[LOSSES]
GUT1    0.5  0.5  0.1  NO  0.01

[STREETS]
ST_A    20.0  0.5  4  0.016  0  0  2  20.0  0.04  0.016

[INLETS]
IN_A    GRATE  2.0  2.0  P_BAR-50

[INLET_USAGE]
GUT1    IN_A  J5  1  10  0  0  0  ON_GRADE

[LID_NODE_OUTLETS]
Drain   3  BOTTOM

[POLLUTANTS]
TSS     MG/L  0  0  0  0  NO  *  0  0  0

[INITIAL_QUALITY]
LINK    GUT1  TSS  5
NODE    J3    TSS  2
LINK    C3    TSS  7

[CURVES]
PC1     Pump2  0  0
PC1            5  10

[TAGS]
Link    GUT1   Gutter

[USER_FLAGS]
Owner   STRING

[USER_FLAG_VALUES]
LINK    GUT1  Owner  city

[COORDINATES]
D1      0    0
J3      300  0
J4      300  100
J5      600  0
O1      600  100
Store   0    100

[VERTICES]
GUT1    100  50
GUT1    200  80
)";

std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

class DeleteRecordTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    fs::path dir;

    void TearDown() override {
        if (engine) {
            swmm_engine_close(engine);
            swmm_engine_destroy(engine);
        }
    }

    // Writes the fixture and an empty rain file into this case's output folder and opens it.
    void open_fixture(const char* inp_text) {
        dir = kOutRoot / ::testing::UnitTest::GetInstance()->current_test_info()->name();
        fs::create_directories(dir);
        std::ofstream(dir / "input.inp", std::ios::binary) << inp_text;
        std::ofstream(dir / "rain.dat", std::ios::binary) << "";
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine, (dir / "input.inp").string().c_str(),
                                   (dir / "input.rpt").string().c_str(),
                                   (dir / "input.out").string().c_str(), nullptr),
                  SWMM_OK);
    }

    std::string write(const char* name) {
        const fs::path p = dir / name;
        EXPECT_EQ(swmm_model_write(engine, p.string().c_str()), SWMM_OK) << p;
        return read_file(p);
    }

    int subcatch_gage(const char* subcatch) {
        int g = -2;
        EXPECT_EQ(swmm_subcatch_get_gage(engine, swmm_subcatch_index(engine, subcatch), &g),
                  SWMM_OK);
        return g;
    }

    void expect_original_references() {
        EXPECT_EQ(swmm_gage_count(engine), 3);
        EXPECT_EQ(swmm_gage_index(engine, "G1"), 0);
        EXPECT_EQ(swmm_gage_index(engine, "G2"), 1);
        EXPECT_EQ(swmm_gage_index(engine, "G3"), 2);
        // The writer falls back to a subcatchment's retained gage name when its
        // index is -1, so the indices are checked here, not only in the file.
        EXPECT_EQ(subcatch_gage("S1"), 1);
        EXPECT_EQ(subcatch_gage("S2"), 2);
        EXPECT_EQ(subcatch_gage("S3"), 1);
    }
};

} // namespace

TEST_F(DeleteRecordTest, GageRestoresExactly) {
    open_fixture(kGagesInp);
    const std::string original = write("1_original.inp");
    expect_original_references();

    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_gage_delete_recorded(engine, 1, nullptr, &record), SWMM_OK);
    ASSERT_NE(record, nullptr);
    write("2_deleted.inp");
    EXPECT_EQ(swmm_gage_count(engine), 2);
    EXPECT_EQ(swmm_gage_index(engine, "G2"), -1);
    EXPECT_EQ(subcatch_gage("S1"), -1);
    EXPECT_EQ(subcatch_gage("S2"), 1);

    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    const std::string restored = write("3_restored.inp");
    expect_original_references();
    EXPECT_EQ(restored, original);

    // A record restores once.
    EXPECT_EQ(swmm_delete_record_restore(engine, record), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_gage_count(engine), 3);
    swmm_delete_record_destroy(record);

    // Redo then undo again: a fresh recorded delete restores just as exactly.
    ASSERT_EQ(swmm_gage_delete_recorded(engine, 1, nullptr, &record), SWMM_OK);
    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    const std::string again = write("4_redo_undo.inp");
    expect_original_references();
    EXPECT_EQ(again, original);
}

TEST_F(DeleteRecordTest, GageRestoreRefusesAChangedModel) {
    open_fixture(kGagesInp);
    const std::string original = write("1_original.inp");

    // A different gage added where the deleted one was expected: refuse, change nothing.
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_gage_delete_recorded(engine, 1, nullptr, &record), SWMM_OK);
    ASSERT_EQ(swmm_gage_add(engine, "GX"), SWMM_OK);
    const std::string before = write("2_deleted_then_added.inp");
    EXPECT_EQ(swmm_delete_record_restore(engine, record), SWMM_ERR_BADPARAM);
    EXPECT_EQ(write("3_refused.inp"), before);
    swmm_delete_record_destroy(record);
    ASSERT_EQ(swmm_gage_delete(engine, swmm_gage_index(engine, "GX"), nullptr), SWMM_OK);

    // The deleted gage's name taken by another gage (same count): refuse.
    ASSERT_EQ(swmm_gage_delete_recorded(engine, 0, nullptr, &record), SWMM_OK); // G1
    ASSERT_EQ(swmm_gage_rename(engine, 0, "G1"), SWMM_OK);                      // G3 -> G1
    EXPECT_EQ(swmm_delete_record_restore(engine, record), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_gage_count(engine), 1);
    swmm_delete_record_destroy(record);

    // A record belongs to the engine that made it.
    SWMM_Engine other = swmm_engine_new();
    ASSERT_NE(other, nullptr);
    ASSERT_EQ(swmm_gage_add(other, "Z"), SWMM_OK);
    ASSERT_EQ(swmm_gage_delete_recorded(other, 0, nullptr, &record), SWMM_OK);
    EXPECT_EQ(swmm_delete_record_restore(engine, record), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_delete_record_restore(other, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    swmm_engine_destroy(other);

    EXPECT_EQ(swmm_delete_record_restore(engine, nullptr), SWMM_ERR_BADPARAM);
    swmm_delete_record_destroy(nullptr);
}

TEST_F(DeleteRecordTest, GageRecordIsOptional) {
    open_fixture(kGagesInp);
    ASSERT_EQ(swmm_gage_delete_recorded(engine, 1, nullptr, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_gage_count(engine), 2);
    EXPECT_EQ(swmm_gage_index(engine, "G3"), 1);
    EXPECT_EQ(subcatch_gage("S2"), 1);
}

TEST_F(DeleteRecordTest, GageBatchRestoresExactly) {
    open_fixture(kGagesInp);
    const std::string original = write("1_original.inp");

    // G1 and G3: the first and last gage, so the batch reinserts at both ends.
    const int indices[] = {0, 2};
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_gage_delete_many_recorded(engine, indices, 2, nullptr, &record), SWMM_OK);
    ASSERT_NE(record, nullptr);
    write("2_deleted.inp");
    EXPECT_EQ(swmm_gage_count(engine), 1);
    EXPECT_STREQ(swmm_gage_id(engine, 0), "G2");
    EXPECT_EQ(subcatch_gage("S1"), 0);
    EXPECT_EQ(subcatch_gage("S2"), -1);

    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    const std::string restored = write("3_restored.inp");
    expect_original_references();
    EXPECT_EQ(restored, original);
}

namespace {

int divider_link(SWMM_Engine e) {
    SWMM_DividerParams p{};
    p.diversion_link = -2;
    EXPECT_EQ(swmm_node_get_divider_params(e, swmm_node_index(e, "D1"), &p), SWMM_OK);
    return p.diversion_link;
}

int usage_link(SWMM_Engine e) {
    if (swmm_inlet_usage_count(e) != 1)
        return -2;
    SWMM_InletUsage u{};
    EXPECT_EQ(swmm_inlet_usage_get(e, 0, &u), SWMM_OK);
    return u.host_idx;
}

// Heat override rows as "scope:name", in row order.
std::vector<std::string> heat_rows(SWMM_Engine e) {
    std::vector<std::string> rows;
    int n = 0;
    EXPECT_EQ(swmm_heat_element_override_count(e, &n), SWMM_OK);
    for (int i = 0; i < n; ++i) {
        int scope = 0;
        char name[64] = {0};
        EXPECT_EQ(swmm_heat_get_element_override(e, i, nullptr, &scope, name, sizeof name, nullptr),
                  SWMM_OK);
        rows.push_back(std::to_string(scope) + ":" + name);
    }
    return rows;
}

void add_heat_overrides(SWMM_Engine e) {
    ASSERT_EQ(swmm_heat_set_element_override(e, SWMM_HEAT_ATTR_ALBEDO, SWMM_HEAT_SCOPE_LINK, "GUT1",
                                             0.3),
              SWMM_OK);
    ASSERT_EQ(swmm_heat_set_element_override(e, SWMM_HEAT_ATTR_ALBEDO, SWMM_HEAT_SCOPE_LINK, "C3",
                                             0.4),
              SWMM_OK);
}

std::vector<std::string> link_ids(SWMM_Engine e) {
    std::vector<std::string> ids;
    for (int i = 0; i < swmm_link_count(e); ++i)
        ids.emplace_back(swmm_link_id(e, i));
    return ids;
}

} // namespace

TEST_F(DeleteRecordTest, LinkRestoresExactly) {
    open_fixture(kLinksInp);
    add_heat_overrides(engine);
    const auto heat = heat_rows(engine);
    ASSERT_EQ(heat.size(), 2u);
    const std::string original = write("1_original.inp");
    const auto ids = link_ids(engine);
    const int gut = swmm_link_index(engine, "GUT1");
    ASSERT_EQ(gut, 1);
    // The writer falls back to retained names for both, so check the indices.
    ASSERT_EQ(divider_link(engine), gut);
    ASSERT_EQ(usage_link(engine), gut);

    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_link_delete_recorded(engine, gut, nullptr, &record), SWMM_OK);
    write("2_deleted.inp");
    EXPECT_EQ(swmm_link_index(engine, "GUT1"), -1);
    EXPECT_EQ(divider_link(engine), -1);
    EXPECT_EQ(swmm_inlet_usage_count(engine), 0);
    EXPECT_EQ(heat_rows(engine).size(), 1u);

    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    const std::string restored = write("3_restored.inp");
    EXPECT_EQ(link_ids(engine), ids);
    EXPECT_EQ(divider_link(engine), gut);
    EXPECT_EQ(usage_link(engine), gut);
    EXPECT_EQ(heat_rows(engine), heat);
    EXPECT_EQ(restored, original);

    // Redo then undo again.
    ASSERT_EQ(swmm_link_delete_recorded(engine, gut, nullptr, &record), SWMM_OK);
    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    EXPECT_EQ(write("4_redo_undo.inp"), original);
}

TEST_F(DeleteRecordTest, LinkBatchRestoresExactly) {
    open_fixture(kLinksInp);
    const std::string original = write("1_original.inp");
    const auto ids = link_ids(engine);

    // One link from every subtype table, plus GUT1 and Drain and their references.
    add_heat_overrides(engine);
    const auto heat = heat_rows(engine);
    std::vector<int> indices;
    for (const char* id : {"C1", "GUT1", "P1", "OR1", "Drain", "W1", "OL1"})
        indices.push_back(swmm_link_index(engine, id));
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_link_delete_many_recorded(engine, indices.data(),
                                             static_cast<int>(indices.size()), nullptr, &record),
              SWMM_OK);
    write("2_deleted.inp");
    EXPECT_EQ(link_ids(engine), std::vector<std::string>{"C3"});

    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    const std::string restored = write("3_restored.inp");
    EXPECT_EQ(link_ids(engine), ids);
    EXPECT_EQ(divider_link(engine), 1);
    EXPECT_EQ(usage_link(engine), 1);
    EXPECT_EQ(heat_rows(engine), heat);
    EXPECT_EQ(restored, original);
}

TEST_F(DeleteRecordTest, LinkRestoreRefusesATakenName) {
    open_fixture(kLinksInp);
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_link_delete_recorded(engine, 1, nullptr, &record), SWMM_OK);
    ASSERT_EQ(swmm_link_rename(engine, 0, "GUT1"), SWMM_OK); // C1 takes the name, same count
    const std::string before = write("1_deleted_then_renamed.inp");
    EXPECT_EQ(swmm_delete_record_restore(engine, record), SWMM_ERR_BADPARAM);
    EXPECT_EQ(write("2_refused.inp"), before);
    swmm_delete_record_destroy(record);
}

// Deleting a link BEFORE GUT1 renumbers the divider's diverted link and the inlet usage's link
// rather than clearing them; undo must renumber them back.
TEST_F(DeleteRecordTest, LinkRestoreRenumbersLaterReferences) {
    open_fixture(kLinksInp);
    const std::string original = write("1_original.inp");
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_link_delete_recorded(engine, swmm_link_index(engine, "C1"), nullptr, &record),
              SWMM_OK);
    EXPECT_EQ(divider_link(engine), 0);
    EXPECT_EQ(usage_link(engine), 0);
    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    EXPECT_EQ(divider_link(engine), 1);
    EXPECT_EQ(usage_link(engine), 1);
    EXPECT_EQ(write("2_restored.inp"), original);
}

namespace {

// Reaction rows that name a link: a wall area, a parameter override and an initial quality each
// for GUT1 and for C3 (which sits after GUT1, so a GUT1 delete renumbers its rows), plus node
// rows a link delete must leave alone.
const char* kLinkReactions = R"([REACTION_OPTIONS]
WALL_GEOMETRY YES
COUPLING FULL

[REACTION_SPECIES]
BULK A MG
WALL W MG

[REACTION_COEFFICIENTS]
PARAMETER k 0.1

[REACTION_PARAMETERS]
LINK GUT1 k 0.2
NODE J4 k 0.4
LINK C3 k 0.3

[REACTION_WALLS]
LINK GUT1 5
NODE J4 7
LINK C3 6

[REACTION_QUALITY]
LINK GUT1 A 2
NODE J4 A 4
LINK C3 A 3
)";

std::string reactions_text(SWMM_Engine e) {
    int need = 0;
    EXPECT_EQ(swmm_reactions_serialize(e, nullptr, 0, &need), SWMM_OK);
    std::string text(static_cast<std::size_t>(need), '\0');
    EXPECT_EQ(swmm_reactions_serialize(e, text.data(), need, &need), SWMM_OK);
    text.resize(static_cast<std::size_t>(need) - 1);
    return text;
}

int count_of(const std::string& text, const std::string& word) {
    int n = 0;
    for (auto at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
        ++n;
    return n;
}

} // namespace

TEST_F(DeleteRecordTest, LinkRestoresItsReactionRows) {
    open_fixture(kLinksInp);
    char err[256] = {0};
    ASSERT_EQ(swmm_reactions_apply_text(engine, kLinkReactions, err, sizeof err), SWMM_OK) << err;
    const std::string reactions = reactions_text(engine);
    std::ofstream(dir / "1_original.rxn", std::ios::binary) << reactions;
    ASSERT_EQ(count_of(reactions, "GUT1"), 3) << reactions;
    ASSERT_EQ(count_of(reactions, "C3"), 3) << reactions;
    const std::string original = write("1_original.inp");

    // One link: GUT1's rows go, C3's rows follow C3 to its new index.
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_link_delete_recorded(engine, swmm_link_index(engine, "GUT1"), nullptr, &record),
              SWMM_OK);
    const std::string deleted = reactions_text(engine);
    std::ofstream(dir / "2_deleted.rxn", std::ios::binary) << deleted;
    EXPECT_EQ(count_of(deleted, "GUT1"), 0) << deleted;
    EXPECT_EQ(count_of(deleted, "C3"), 3) << deleted;
    EXPECT_EQ(count_of(deleted, "J4"), 3) << deleted;
    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    std::ofstream(dir / "3_restored.rxn", std::ios::binary) << reactions_text(engine);
    EXPECT_EQ(reactions_text(engine), reactions);
    EXPECT_EQ(write("3_restored.inp"), original);

    // A batch holding both links that own rows and one before them.
    std::vector<int> indices;
    for (const char* id : {"C1", "GUT1", "C3"})
        indices.push_back(swmm_link_index(engine, id));
    ASSERT_EQ(swmm_link_delete_many_recorded(engine, indices.data(),
                                             static_cast<int>(indices.size()), nullptr, &record),
              SWMM_OK);
    const std::string batch_deleted = reactions_text(engine);
    std::ofstream(dir / "4_batch_deleted.rxn", std::ios::binary) << batch_deleted;
    EXPECT_EQ(count_of(batch_deleted, "LINK"), 0) << batch_deleted;
    EXPECT_EQ(count_of(batch_deleted, "J4"), 3) << batch_deleted;
    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    std::ofstream(dir / "5_batch_restored.rxn", std::ios::binary) << reactions_text(engine);
    EXPECT_EQ(reactions_text(engine), reactions);
    EXPECT_EQ(write("5_batch_restored.inp"), original);
}

namespace {

// N2 (a storage node with a LID unit) is referred to by every table that can name a node: links
// at both ends (one of them a divider's diverted link), a subcatchment outlet and groundwater
// node, a LID usage drain-to, an inlet usage capture node, inflow, DWF and RDII rows, treatment,
// initial quality, a 2D coupling row, a tag and a user flag. D1, O1 and the inlet junction IJ1
// come after it and carry the same kinds of rows (S3 drains to O1 with D1 as its groundwater
// node; IJ1 captures to O1), so their indices are renumbered by its delete. Reaction rows and
// heat overrides are added through the API.
const char* kNodesInp = R"([TITLE]
Delete-record round trip: every table that can refer to a node refers to N2

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2024
START_TIME           00:00:00
END_DATE             01/01/2024
END_TIME             06:00:00
ROUTING_STEP         0:00:30

[RAINGAGES]
G1      INTENSITY  0:15  1.0  TIMESERIES TS1

[SUBCATCHMENTS]
S1      G1  N2  1  50  100  0.5  0
S2      G1  J1  1  50  100  0.5  0
S3      G1  O1  1  50  100  0.5  0

[SUBAREAS]
S1      0.01  0.1  0.05  0.05  25  OUTLET
S2      0.01  0.1  0.05  0.05  25  OUTLET
S3      0.01  0.1  0.05  0.05  25  OUTLET

[INFILTRATION]
S1      3.0  0.5  4  7  0
S2      3.0  0.5  4  7  0
S3      3.0  0.5  4  7  0

[AQUIFERS]
A1      0.5  0.15  0.30  0.1  12  15  0.35  14  0  0  4  0.3

[GROUNDWATER]
S1      A1  N2  6  1  1  0  0  0  0  *
S3      A1  D1  6  1  1  0  0  0  0  *

[LID_CONTROLS]
RB      RB
RB      STORAGE  36  0.75  0  0  YES
RB      DRAIN    0   0.5   0  6
Stack   NODE
Stack   SURFACE    6   .1
Stack   MEDIA      12  .45  .2  .08  2  10  3
Stack   AGGREGATE  6   .4   100
Stack   BOTTOM     .5  0

[LID_USAGE]
S1      RB  1  100  0  0  100  0  *  N2

[JUNCTIONS]
J1      12  5  0  0  0
J3      11  5  0  0  0
J4      10  5  0  0  0
J6      9   5  0  0  0

[STORAGE]
N2      9  2  1  FUNCTIONAL  0  0  100  0  0

[LID_NODES]
N2      Stack  10  BOTTOM  SEALED

[DIVIDERS]
D1      8  DV  CUTOFF  0.5  5  0  0  0

[OUTFALLS]
O1      6  FREE  *  NO

[INLET_JUNCTIONS]
IJ1     9.5  0.5  IN_A  O1  2  2.5  0  0  0  ON_SAG

[CONDUITS]
C1      J1  N2  300  0.013  0  0
C2      N2  D1  300  0.013  0  0
DV      D1  N2  300  0.013  0  0
C4      D1  O1  300  0.013  0  0
GUT     J3  J4  300  0.016  0  0
ST3     J4  IJ1 300  0.016  0  0
ST4     IJ1 J6  300  0.016  0  0

[PUMPS]
P1      N2  J3  PC1  ON  0  0

[XSECTIONS]
C1      CIRCULAR  1  0  0  0  1
C2      CIRCULAR  1  0  0  0  1
DV      CIRCULAR  1  0  0  0  1
C4      CIRCULAR  1  0  0  0  1
GUT     STREET    ST_A
ST3     STREET    ST_A
ST4     STREET    ST_A

[STREETS]
ST_A    20.0  0.5  4  0.016  0  0  2  20.0  0.04  0.016

[INLETS]
IN_A    GRATE  2.0  2.0  P_BAR-50

[INLET_USAGE]
GUT     IN_A  N2  1  10  0  0  0  ON_GRADE

[POLLUTANTS]
TSS     MG/L  0  0  0  0  NO  *  0  0  0

[INFLOWS]
J4      FLOW  TS1  FLOW    1.0  1.0  1
N2      FLOW  TS1  FLOW    1.0  1.0  2
D1      FLOW  TS1  FLOW    1.0  1.0  3
N2      TSS   TS1  CONCEN  1.0  1.0  4

[DWF]
N2      FLOW  1.5
D1      FLOW  0.5

[HYDROGRAPHS]
UH1     G1
UH1     ALL  SHORT  0.1  1  2

[RDII]
N2      UH1  100
D1      UH1  50

[TREATMENT]
N2      TSS  R = 0.5
D1      TSS  R = 0.25

[INITIAL_QUALITY]
NODE    N2  TSS  3
NODE    D1  TSS  4

[2D_NODE_COUPLING]
J4      MANHOLE  D=1.2
N2      OPEN
O1      OUTFALL  SPREAD=3

[CURVES]
PC1     Pump2  0  0
PC1            5  10

[TIMESERIES]
TS1     0:00  0.5
TS1     1:00  0.0

[TAGS]
Node    N2  Tank

[USER_FLAGS]
Owner   STRING

[USER_FLAG_VALUES]
NODE    N2  Owner  city
NODE    D1  Owner  county

[COORDINATES]
J1      0    0
J3      100  0
J4      200  0
N2      100  100
D1      200  100
O1      300  100
J6      300  0
IJ1     250  0
)";

const char* kNodeReactions = R"([REACTION_OPTIONS]
WALL_GEOMETRY YES
COUPLING FULL

[REACTION_SPECIES]
BULK A MG
WALL W MG

[REACTION_COEFFICIENTS]
PARAMETER k 0.1

[REACTION_SOURCES]
MASS N2 A 1 *
CONC D1 A 2 *

[REACTION_PARAMETERS]
NODE N2 k 0.2
LINK C1 k 0.3
NODE D1 k 0.4

[REACTION_WALLS]
NODE N2 5
LINK C1 6
NODE D1 7

[REACTION_QUALITY]
NODE N2 A 2
LINK C1 A 3
NODE D1 A 4
)";

void add_node_heat_overrides(SWMM_Engine e) {
    for (const auto& [name, value] : {std::pair{"N2", 0.3}, std::pair{"D1", 0.4}})
        ASSERT_EQ(swmm_heat_set_element_override(e, SWMM_HEAT_ATTR_ALBEDO, SWMM_HEAT_SCOPE_NODE,
                                                 name, value),
                  SWMM_OK);
}

std::vector<std::string> node_ids(SWMM_Engine e) {
    std::vector<std::string> ids;
    for (int i = 0; i < swmm_node_count(e); ++i)
        ids.emplace_back(swmm_node_id(e, i));
    return ids;
}

// Every index-based reference into the node table, as names, so a broken renumbering shows even
// where the writer would fall back to a retained name.
std::vector<std::string> node_refs(SWMM_Engine e) {
    std::vector<std::string> refs;
    auto name = [&](int i) { return i < 0 ? std::string("-") : std::string(swmm_node_id(e, i)); };
    for (int s = 0; s < swmm_subcatch_count(e); ++s) {
        int outlet = -2, gw = -2;
        EXPECT_EQ(swmm_subcatch_get_outlet(e, s, &outlet), SWMM_OK);
        EXPECT_EQ(swmm_subcatch_get_gw_node(e, s, &gw), SWMM_OK);
        refs.push_back("subcatch " + std::to_string(s) + " outlet " + name(outlet) + " gw " +
                       name(gw));
    }
    for (int l = 0; l < swmm_link_count(e); ++l) {
        int n1 = -2, n2 = -2;
        EXPECT_EQ(swmm_link_get_from_node(e, l, &n1), SWMM_OK);
        EXPECT_EQ(swmm_link_get_to_node(e, l, &n2), SWMM_OK);
        refs.push_back(std::string("link ") + swmm_link_id(e, l) + " " + name(n1) + " " + name(n2));
    }
    for (int u = 0; u < swmm_inlet_usage_count(e); ++u) {
        SWMM_InletUsage usage{};
        EXPECT_EQ(swmm_inlet_usage_get(e, u, &usage), SWMM_OK);
        refs.push_back("usage host " +
                       (usage.host_kind == SWMM_INLET_HOST_NODE ? name(usage.host_idx)
                                                                : std::string("link")) +
                       " capture " + name(usage.capture_node_idx));
    }
    return refs;
}

} // namespace

TEST_F(DeleteRecordTest, NodeRestoresExactly) {
    open_fixture(kNodesInp);
    char err[256] = {0};
    ASSERT_EQ(swmm_reactions_apply_text(engine, kNodeReactions, err, sizeof err), SWMM_OK) << err;
    add_node_heat_overrides(engine);
    const auto reactions = reactions_text(engine);
    const auto heat = heat_rows(engine);
    const auto ids = node_ids(engine);
    const auto links = link_ids(engine);
    const auto refs = node_refs(engine);
    const int n2 = swmm_node_index(engine, "N2");
    ASSERT_GE(n2, 0);
    ASSERT_LT(n2, swmm_node_index(engine, "D1"));
    ASSERT_LT(n2, swmm_node_index(engine, "O1"));
    ASSERT_LT(n2, swmm_node_index(engine, "IJ1"));
    ASSERT_EQ(swmm_inlet_usage_count(engine), 2);
    ASSERT_EQ(divider_link(engine), swmm_link_index(engine, "DV"));
    ASSERT_EQ(swmm_ext_inflow_count(engine), 4);
    const std::string original = write("1_original.inp");
    std::ofstream(dir / "1_original.rxn", std::ios::binary) << reactions;

    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_node_delete_recorded(engine, n2, nullptr, &record), SWMM_OK);
    write("2_deleted.inp");
    std::ofstream(dir / "2_deleted.rxn", std::ios::binary) << reactions_text(engine);
    EXPECT_EQ(swmm_node_index(engine, "N2"), -1);
    for (const char* gone : {"C1", "C2", "DV", "P1"})
        EXPECT_EQ(swmm_link_index(engine, gone), -1) << gone;
    EXPECT_EQ(divider_link(engine), -1);
    EXPECT_EQ(swmm_inlet_usage_count(engine), 1);
    EXPECT_EQ(swmm_ext_inflow_count(engine), 2);

    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    std::ofstream(dir / "3_restored.rxn", std::ios::binary) << reactions_text(engine);
    EXPECT_EQ(write("3_restored.inp"), original);
    EXPECT_EQ(node_ids(engine), ids);
    EXPECT_EQ(link_ids(engine), links);
    EXPECT_EQ(node_refs(engine), refs);
    EXPECT_EQ(divider_link(engine), swmm_link_index(engine, "DV"));
    EXPECT_EQ(reactions_text(engine), reactions);
    EXPECT_EQ(heat_rows(engine), heat);

    // Redo then undo again.
    ASSERT_EQ(swmm_node_delete_recorded(engine, n2, nullptr, &record), SWMM_OK);
    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    EXPECT_EQ(write("4_redo_undo.inp"), original);
    EXPECT_EQ(reactions_text(engine), reactions);
}

TEST_F(DeleteRecordTest, NodeBatchRestoresExactly) {
    open_fixture(kNodesInp);
    char err[256] = {0};
    ASSERT_EQ(swmm_reactions_apply_text(engine, kNodeReactions, err, sizeof err), SWMM_OK) << err;
    add_node_heat_overrides(engine);
    const auto reactions = reactions_text(engine);
    const auto heat = heat_rows(engine);
    const auto ids = node_ids(engine);
    const auto refs = node_refs(engine);
    const std::string original = write("1_original.inp");

    // A node from every subtype table (junction, storage, divider, outfall). N2 is deleted before
    // D1 (descending order), so the link D1 diverts to is cascaded before D1's divider row goes.
    std::vector<int> indices;
    for (const char* id : {"J1", "N2", "D1", "O1"})
        indices.push_back(swmm_node_index(engine, id));
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_node_delete_many_recorded(engine, indices.data(),
                                             static_cast<int>(indices.size()), nullptr, &record),
              SWMM_OK);
    write("2_deleted.inp");
    EXPECT_EQ(node_ids(engine), (std::vector<std::string>{"J3", "J4", "J6", "IJ1"}));
    EXPECT_EQ(link_ids(engine), (std::vector<std::string>{"GUT", "ST3", "ST4"}));

    ASSERT_EQ(swmm_delete_record_restore(engine, record), SWMM_OK);
    swmm_delete_record_destroy(record);
    EXPECT_EQ(write("3_restored.inp"), original);
    EXPECT_EQ(node_ids(engine), ids);
    EXPECT_EQ(node_refs(engine), refs);
    EXPECT_EQ(divider_link(engine), swmm_link_index(engine, "DV"));
    EXPECT_EQ(reactions_text(engine), reactions);
    EXPECT_EQ(heat_rows(engine), heat);
}

TEST_F(DeleteRecordTest, NodeRestoreRefusesATakenName) {
    open_fixture(kNodesInp);
    SWMM_DeleteRecord record = nullptr;
    ASSERT_EQ(swmm_node_delete_recorded(engine, swmm_node_index(engine, "N2"), nullptr, &record),
              SWMM_OK);
    ASSERT_EQ(swmm_node_rename(engine, swmm_node_index(engine, "J1"), "N2"), SWMM_OK);
    const std::string before = write("1_deleted_then_renamed.inp");
    EXPECT_EQ(swmm_delete_record_restore(engine, record), SWMM_ERR_BADPARAM);
    EXPECT_EQ(write("2_refused.inp"), before);
    swmm_delete_record_destroy(record);
}
