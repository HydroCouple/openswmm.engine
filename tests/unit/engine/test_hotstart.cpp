/**
 * @file test_hotstart.cpp
 * @brief Unit tests for the hot start file API.
 *
 * @details Tests:
 *          - HotStartManager::save() + open() round-trip (binary I/O + CRC32)
 *          - Magic header validation
 *          - CRC32 corruption detection
 *          - Preserved node/link/subcatch values after round-trip
 *          - Missing-object handling (warnings, not errors)
 *          - Modification API (set_node_depth, set_link_flow, etc.)
 *          - Flush-on-modify semantics
 *          - C API wrappers (null-guard, bad-path, close-null)
 *
 * @see src/engine/core/HotStartManager.hpp
 * @see include/openswmm/engine/openswmm_hotstart.h
 * @ingroup engine_hotstart
 */

#include <gtest/gtest.h>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include "../../src/engine/core/HotStartManager.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../include/openswmm/engine/openswmm_hotstart.h"

namespace fs = std::filesystem;
using openswmm::HotStartManager;
using openswmm::HotStartFile;
using openswmm::SimulationContext;
using openswmm::EngineState;

namespace {

// ============================================================================
// Helpers
// ============================================================================

/** @brief Unique temp file path for each test (deleted in TearDown). */
class TempFile {
public:
    explicit TempFile(const std::string& suffix = ".hs") {
        path_ = (fs::temp_directory_path() /
                 ("openswmm_test_" + std::to_string(
                     reinterpret_cast<std::uintptr_t>(this)) + suffix)).string();
    }
    ~TempFile() {
        std::error_code ec;
        fs::remove(path_, ec);
    }
    const std::string& path() const { return path_; }
private:
    std::string path_;
};

/**
 * @brief Build a minimal SimulationContext with a few nodes, links, subcatches.
 *
 * @details Sets state to RUNNING so HotStartManager::save() accepts it.
 */
static SimulationContext make_test_context() {
    SimulationContext ctx;

    // Options
    ctx.options.start_date = 2460000.5;  // arbitrary Julian date
    ctx.options.end_date   = 2460001.5;
    ctx.spatial.crs        = "EPSG:4326";

    // Register nodes
    ctx.node_names.add("J1");
    ctx.node_names.add("J2");
    ctx.node_names.add("OUT1");

    // Register links
    ctx.link_names.add("C1");
    ctx.link_names.add("C2");

    // Register subcatchments
    ctx.subcatch_names.add("S1");

    // Allocate SoA arrays
    ctx.allocate_objects();

    // Set state
    ctx.nodes.depth[0]  = 1.5;   ctx.nodes.head[0]  = 101.5; ctx.nodes.volume[0] = 5.0;
    ctx.nodes.depth[1]  = 0.8;   ctx.nodes.head[1]  = 100.8; ctx.nodes.volume[1] = 2.0;
    ctx.nodes.depth[2]  = 0.0;   ctx.nodes.head[2]  = 100.0; ctx.nodes.volume[2] = 0.0;

    ctx.links.flow[0]   = 3.14;  ctx.links.depth[0]  = 0.9;
    ctx.links.flow[1]   = 1.23;  ctx.links.depth[1]  = 0.4;

    ctx.subcatches.runoff[0] = 0.05;

    ctx.current_time = 86400.0;  // 1 day in seconds
    ctx.state        = EngineState::RUNNING;

    return ctx;
}

// ============================================================================
// Save / open round-trip
// ============================================================================

TEST(HotStartManagerTest, SaveCreatesFile) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr) << HotStartManager::last_io_error();
    delete hs;

    EXPECT_TRUE(fs::exists(tmp.path()));
    EXPECT_GT(fs::file_size(tmp.path()), 16u);  // at least magic + version
}

TEST(HotStartManagerTest, OpenReadsBackSavedFile) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* saved = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(saved, nullptr);
    delete saved;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr) << HotStartManager::last_io_error();
    delete loaded;
}

TEST(HotStartManagerTest, FileHasMagicHeader) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    // Read raw bytes and check magic
    std::ifstream f(tmp.path(), std::ios::binary);
    char magic[16] = {};
    f.read(magic, 16);
    EXPECT_EQ(std::memcmp(magic, "OPENSWMM_HS_V1", 15), 0);
}

TEST(HotStartManagerTest, NodeCountPreservedAfterRoundTrip) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->nodes.size(), 3u);
    EXPECT_EQ(loaded->links.size(), 2u);
    EXPECT_EQ(loaded->subcatches.size(), 1u);
    delete loaded;
}

TEST(HotStartManagerTest, NodeDepthPreservedAfterRoundTrip) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);

    EXPECT_DOUBLE_EQ(loaded->nodes[0].depth, 1.5);
    EXPECT_DOUBLE_EQ(loaded->nodes[1].depth, 0.8);
    EXPECT_EQ(loaded->nodes[0].id, "J1");
    EXPECT_EQ(loaded->nodes[2].id, "OUT1");
    delete loaded;
}

TEST(HotStartManagerTest, NodeHeadPreservedAfterRoundTrip) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);
    EXPECT_DOUBLE_EQ(loaded->nodes[0].head, 101.5);
    EXPECT_DOUBLE_EQ(loaded->nodes[1].head, 100.8);
    delete loaded;
}

TEST(HotStartManagerTest, LinkFlowPreservedAfterRoundTrip) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);
    EXPECT_DOUBLE_EQ(loaded->links[0].flow,  3.14);
    EXPECT_DOUBLE_EQ(loaded->links[1].flow,  1.23);
    EXPECT_EQ(loaded->links[0].id, "C1");
    delete loaded;
}

TEST(HotStartManagerTest, SubcatchRunoffPreservedAfterRoundTrip) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);
    EXPECT_DOUBLE_EQ(loaded->subcatches[0].runoff, 0.05);
    EXPECT_EQ(loaded->subcatches[0].id, "S1");
    delete loaded;
}

TEST(HotStartManagerTest, HeaderMetadataPreservedAfterRoundTrip) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);
    EXPECT_EQ(loaded->header.version, 1u);
    EXPECT_DOUBLE_EQ(loaded->header.sim_time,   86400.0);
    EXPECT_DOUBLE_EQ(loaded->header.start_date, ctx.options.start_date);
    EXPECT_DOUBLE_EQ(loaded->header.end_date,   ctx.options.end_date);
    EXPECT_EQ(loaded->header.crs, "EPSG:4326");
    delete loaded;
}

// ============================================================================
// CRC32 corruption detection
// ============================================================================

TEST(HotStartManagerTest, CorruptedFileFailsOpen) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    // Flip a byte in the middle of the file
    {
        std::fstream f(tmp.path(), std::ios::in | std::ios::out | std::ios::binary);
        f.seekp(20);
        char b = 0;
        f.read(&b, 1);
        b = static_cast<char>(~b);
        f.seekp(20);
        f.write(&b, 1);
    }

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    EXPECT_EQ(loaded, nullptr);  // CRC mismatch
    EXPECT_FALSE(HotStartManager::last_io_error().empty());
    delete loaded;
}

TEST(HotStartManagerTest, OpenNonexistentFileReturnsNull) {
    HotStartFile* hs = HotStartManager::open("/nonexistent/path.hs");
    EXPECT_EQ(hs, nullptr);
    EXPECT_FALSE(HotStartManager::last_io_error().empty());
}

// ============================================================================
// Missing-object handling
// ============================================================================

TEST(HotStartManagerTest, ApplyAllObjectsPresentZeroWarnings) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);

    // Apply to same context — all objects present
    SimulationContext ctx2 = make_test_context();
    ctx2.state = EngineState::INITIALIZED;
    int warnings = HotStartManager::apply(*loaded, ctx2);
    EXPECT_EQ(warnings, 0);
    EXPECT_EQ(loaded->warnings.size(), 0u);
    delete loaded;
}

TEST(HotStartManagerTest, ApplyMissingNodeIssuesWarningNotError) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);

    // Target context is missing "J2" and "OUT1"
    SimulationContext ctx2;
    ctx2.options = ctx.options;
    ctx2.node_names.add("J1");
    ctx2.link_names.add("C1");
    ctx2.link_names.add("C2");
    ctx2.subcatch_names.add("S1");
    ctx2.allocate_objects();
    ctx2.state = EngineState::INITIALIZED;

    int warnings = HotStartManager::apply(*loaded, ctx2);
    EXPECT_GE(warnings, 2);  // at least J2 and OUT1 missing
    EXPECT_GE(loaded->warnings.size(), 2u);
    delete loaded;
}

TEST(HotStartManagerTest, ApplyMissingObjectsDoNotPreventMatchingObjects) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);

    // Target has only J1; J2 and OUT1 are missing
    SimulationContext ctx2;
    ctx2.options = ctx.options;
    ctx2.node_names.add("J1");
    ctx2.link_names.add("C1");
    ctx2.link_names.add("C2");
    ctx2.subcatch_names.add("S1");
    ctx2.allocate_objects();
    ctx2.state = EngineState::INITIALIZED;

    HotStartManager::apply(*loaded, ctx2);

    // J1 depth should still be applied correctly
    EXPECT_DOUBLE_EQ(ctx2.nodes.depth[0], 1.5);
    delete loaded;
}

TEST(HotStartManagerTest, ApplyWarningCallbackFiredPerMissingObject) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);

    SimulationContext ctx2;
    ctx2.options = ctx.options;
    ctx2.node_names.add("J1");
    ctx2.link_names.add("C1");
    ctx2.link_names.add("C2");
    ctx2.subcatch_names.add("S1");
    ctx2.allocate_objects();
    ctx2.state = EngineState::INITIALIZED;

    std::vector<std::string> callback_warnings;
    HotStartManager::apply(*loaded, ctx2,
        [&](const std::string& msg) { callback_warnings.push_back(msg); });

    EXPECT_EQ(callback_warnings.size(), loaded->warnings.size());
    for (const auto& msg : callback_warnings) {
        EXPECT_FALSE(msg.empty());
    }
    delete loaded;
}

// ============================================================================
// Modification API
// ============================================================================

TEST(HotStartFileTest, SetNodeDepthModifiesRecord) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    EXPECT_TRUE(hs->set_node_depth("J1", 9.99));
    EXPECT_DOUBLE_EQ(hs->nodes[0].depth, 9.99);
    EXPECT_TRUE(hs->dirty);
    delete hs;
}

TEST(HotStartFileTest, SetNodeDepthUnknownIdReturnsFalse) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    EXPECT_FALSE(hs->set_node_depth("NONEXISTENT", 1.0));
    EXPECT_FALSE(hs->dirty);
    delete hs;
}

TEST(HotStartFileTest, SetNodeHeadModifiesRecord) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    EXPECT_TRUE(hs->set_node_head("J2", 200.0));
    EXPECT_DOUBLE_EQ(hs->nodes[1].head, 200.0);
    delete hs;
}

TEST(HotStartFileTest, SetLinkFlowModifiesRecord) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    EXPECT_TRUE(hs->set_link_flow("C1", 7.77));
    EXPECT_DOUBLE_EQ(hs->links[0].flow, 7.77);
    delete hs;
}

TEST(HotStartFileTest, SetLinkDepthModifiesRecord) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    EXPECT_TRUE(hs->set_link_depth("C2", 0.55));
    EXPECT_DOUBLE_EQ(hs->links[1].depth, 0.55);
    delete hs;
}

TEST(HotStartFileTest, SetSubcatchRunoffModifiesRecord) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    EXPECT_TRUE(hs->set_subcatch_runoff("S1", 0.42));
    EXPECT_DOUBLE_EQ(hs->subcatches[0].runoff, 0.42);
    delete hs;
}

TEST(HotStartFileTest, FlushWritesModifiedDepthToDisk) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);

    hs->set_node_depth("J1", 5.55);
    EXPECT_TRUE(HotStartManager::flush(*hs));
    EXPECT_FALSE(hs->dirty);
    delete hs;

    // Reload and verify the modified value persisted
    HotStartFile* loaded = HotStartManager::open(tmp.path());
    ASSERT_NE(loaded, nullptr);
    EXPECT_DOUBLE_EQ(loaded->nodes[0].depth, 5.55);
    delete loaded;
}

TEST(HotStartFileTest, CleanFileFlushIsNoOp) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    EXPECT_FALSE(hs->dirty);
    EXPECT_TRUE(HotStartManager::flush(*hs));  // should succeed without writing
    delete hs;
}

// ============================================================================
// C API wrappers
// ============================================================================

TEST(HotStartCApiTest, OpenNullPathReturnsError) {
    SWMM_HotStart hs = nullptr;
    int rc = swmm_hotstart_open(nullptr, &hs);
    EXPECT_NE(rc, SWMM_OK);
    EXPECT_EQ(hs, nullptr);
}

TEST(HotStartCApiTest, OpenNullOutParamReturnsError) {
    int rc = swmm_hotstart_open("/some/path.hs", nullptr);
    EXPECT_NE(rc, SWMM_OK);
}

TEST(HotStartCApiTest, OpenNonexistentFileReturnsError) {
    SWMM_HotStart hs = nullptr;
    int rc = swmm_hotstart_open("/nonexistent/path/file.hs", &hs);
    EXPECT_EQ(rc, SWMM_ERR_HOTSTART);
    EXPECT_EQ(hs, nullptr);
}

TEST(HotStartCApiTest, CloseNullHandleIsNoOp) {
    int rc = swmm_hotstart_close(nullptr);
    EXPECT_EQ(rc, SWMM_OK);
}

TEST(HotStartCApiTest, NodeCountMatchesSavedModel) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    SWMM_HotStart h = nullptr;
    ASSERT_EQ(swmm_hotstart_open(tmp.path().c_str(), &h), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_node_count(h), 3);
    EXPECT_EQ(swmm_hotstart_link_count(h), 2);
    swmm_hotstart_close(h);
}

TEST(HotStartCApiTest, GetSimTimeMatchesSaved) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    SWMM_HotStart h = nullptr;
    ASSERT_EQ(swmm_hotstart_open(tmp.path().c_str(), &h), SWMM_OK);
    double t = -1.0;
    EXPECT_EQ(swmm_hotstart_get_sim_time(h, &t), SWMM_OK);
    EXPECT_DOUBLE_EQ(t, 86400.0);
    swmm_hotstart_close(h);
}

TEST(HotStartCApiTest, GetCrsMatchesSaved) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    SWMM_HotStart h = nullptr;
    ASSERT_EQ(swmm_hotstart_open(tmp.path().c_str(), &h), SWMM_OK);
    char buf[64] = {};
    EXPECT_EQ(swmm_hotstart_get_crs(h, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "EPSG:4326");
    swmm_hotstart_close(h);
}

TEST(HotStartCApiTest, SetNodeDepthViaApiBadIdReturnsError) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    SWMM_HotStart h = nullptr;
    ASSERT_EQ(swmm_hotstart_open(tmp.path().c_str(), &h), SWMM_OK);
    int rc = swmm_hotstart_set_node_depth(h, "DOES_NOT_EXIST", 1.0);
    EXPECT_EQ(rc, SWMM_ERR_BADPARAM);
    swmm_hotstart_close(h);
}

TEST(HotStartCApiTest, WarningCountZeroBeforeApply) {
    TempFile tmp;
    SimulationContext ctx = make_test_context();

    HotStartFile* hs = HotStartManager::save(ctx, tmp.path());
    ASSERT_NE(hs, nullptr);
    delete hs;

    SWMM_HotStart h = nullptr;
    ASSERT_EQ(swmm_hotstart_open(tmp.path().c_str(), &h), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_warning_count(h), 0);
    swmm_hotstart_close(h);
}

TEST(HotStartCApiTest, WarningAccessNullHandleIsSafe) {
    EXPECT_EQ(swmm_hotstart_warning_count(nullptr), 0);
    EXPECT_EQ(swmm_hotstart_warning(nullptr, 0), nullptr);
    EXPECT_EQ(swmm_hotstart_node_count(nullptr), -1);
    EXPECT_EQ(swmm_hotstart_link_count(nullptr), -1);
}

} /* anonymous namespace */
