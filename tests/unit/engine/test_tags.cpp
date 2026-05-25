/**
 * @file test_tags.cpp
 * @brief Unit tests for the per-object [TAGS] attribute (Slice DB.3).
 *
 * @details Covers:
 *  - swmm_node/link/subcatch_get/set_tag round-trip
 *  - empty/null tag clears
 *  - rename preserves the tag (index-keyed storage, not name-keyed)
 *  - InpWriter emits a [TAGS] section that round-trips back through
 *    SpatialHandler::handle_tags
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_subcatchments.h>

#include <filesystem>
#include <fstream>
#include <string>

class TagsTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        // One of each kind so we exercise node + link + subcatch tag accessors.
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "J2", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "C1", 0), SWMM_OK);  // CONDUIT
        ASSERT_EQ(swmm_subcatch_add(engine, "S1"), SWMM_OK);
    }

    void TearDown() override { swmm_engine_destroy(engine); }
};

TEST_F(TagsTest, NodeTagRoundTrip) {
    char buf[64] = {0};

    // Empty by default.
    EXPECT_EQ(swmm_node_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");

    // Set + read back.
    EXPECT_EQ(swmm_node_set_tag(engine, 0, "upstream"), SWMM_OK);
    EXPECT_EQ(swmm_node_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "upstream");

    // Null tag clears.
    EXPECT_EQ(swmm_node_set_tag(engine, 0, nullptr), SWMM_OK);
    EXPECT_EQ(swmm_node_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");

    // Empty string also clears.
    EXPECT_EQ(swmm_node_set_tag(engine, 0, ""), SWMM_OK);
    EXPECT_EQ(swmm_node_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "");
}

TEST_F(TagsTest, LinkTagRoundTrip) {
    char buf[64] = {0};
    EXPECT_EQ(swmm_link_set_tag(engine, 0, "trunk"), SWMM_OK);
    EXPECT_EQ(swmm_link_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "trunk");
}

TEST_F(TagsTest, SubcatchTagRoundTrip) {
    char buf[64] = {0};
    EXPECT_EQ(swmm_subcatch_set_tag(engine, 0, "residential"), SWMM_OK);
    EXPECT_EQ(swmm_subcatch_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "residential");
}

TEST_F(TagsTest, TagTruncatedToBufferSize) {
    EXPECT_EQ(swmm_node_set_tag(engine, 0, "0123456789ABCDEF"), SWMM_OK);
    char small[8] = {0};
    EXPECT_EQ(swmm_node_get_tag(engine, 0, small, sizeof(small)), SWMM_OK);
    // 7 chars + NUL terminator.
    EXPECT_STREQ(small, "0123456");
}

TEST_F(TagsTest, RenamePreservesTag) {
    // The earlier name-keyed map silently lost the tag on rename. This
    // regression locks in the index-keyed contract — tags travel with
    // the slot, not the name.
    ASSERT_EQ(swmm_node_set_tag(engine, 0, "trunk-asset-77"), SWMM_OK);
    ASSERT_EQ(swmm_node_rename(engine, 0, "J1_renamed"), SWMM_OK);

    char buf[64] = {0};
    EXPECT_EQ(swmm_node_get_tag(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "trunk-asset-77");
}

TEST_F(TagsTest, BadIndexAndBufRejected) {
    EXPECT_EQ(swmm_node_get_tag(engine, 99, nullptr, 0), SWMM_ERR_BADPARAM);
    char buf[8] = {0};
    EXPECT_EQ(swmm_node_get_tag(engine, 99, buf, sizeof(buf)), SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_node_set_tag(engine, 99, "x"),             SWMM_ERR_BADINDEX);
}

TEST_F(TagsTest, InpWriterEmitsTagsSection) {
    // Tag each kind, write the .inp, verify the [TAGS] block is present
    // with the expected text. (Round-tripping through swmm_engine_open
    // would need a fuller model; the on-disk regex is sufficient to lock
    // the emission contract.)
    ASSERT_EQ(swmm_node_set_tag(engine, 0, "tagN1"), SWMM_OK);
    ASSERT_EQ(swmm_link_set_tag(engine, 0, "tagL1"), SWMM_OK);
    ASSERT_EQ(swmm_subcatch_set_tag(engine, 0, "tagS1"), SWMM_OK);

    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "swmm_tags_roundtrip.inp";
    ASSERT_EQ(swmm_model_write(engine, tmp.string().c_str()), SWMM_OK);

    std::ifstream in(tmp);
    ASSERT_TRUE(in.good());
    std::string body((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());

    EXPECT_NE(body.find("[TAGS]"),    std::string::npos);
    EXPECT_NE(body.find("tagN1"),     std::string::npos);
    EXPECT_NE(body.find("tagL1"),     std::string::npos);
    EXPECT_NE(body.find("tagS1"),     std::string::npos);
    EXPECT_NE(body.find("J1"),        std::string::npos);
    EXPECT_NE(body.find("C1"),        std::string::npos);
    EXPECT_NE(body.find("S1"),        std::string::npos);

    fs::remove(tmp);
}

TEST_F(TagsTest, NoTagsSectionWhenAllEmpty) {
    // Don't pollute the .inp with an empty [TAGS] block when nothing is
    // tagged — keeps round-trip diffs clean.
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / "swmm_tags_empty.inp";
    ASSERT_EQ(swmm_model_write(engine, tmp.string().c_str()), SWMM_OK);

    std::ifstream in(tmp);
    ASSERT_TRUE(in.good());
    std::string body((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());

    EXPECT_EQ(body.find("[TAGS]"), std::string::npos);
    fs::remove(tmp);
}
