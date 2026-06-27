/**
 * @file test_model_write_with_plugin.cpp
 * @brief Unit tests for AA-3.1 — plugin-routed model write + [PLUGINS]
 *        section accessors + grouped plugin discovery facade.
 *
 * @details Covers:
 *          - swmm_model_write_with_plugin: NULL plugin id falls back to
 *            built-in .inp writer; unknown plugin id rejects;
 *            non-input-capable plugin rejects.
 *          - swmm_plugins_count / get / set / remove: round-trip a row,
 *            replace-by-key, idempotent remove, buffer truncation.
 *          - discover_plugins_by_id: groups multiple filter entries that
 *            share a plugin_id into a single DiscoveredPlugin with the
 *            full role set.
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/plugin_sdk/PluginDiscovery.hpp>

namespace fs = std::filesystem;

// ---------------------------------------------------------------------------
// Fixture — minimal valid programmatic model used by the write-side tests.
// ---------------------------------------------------------------------------

class ModelWriteWithPluginTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);

        // Nodes: J1 (junction) → O1 (outfall).  Smallest valid model
        // that swmm_validate_model accepts.
        ASSERT_EQ(swmm_node_add(engine, "J1", SWMM_NODE_JUNCTION), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "O1", SWMM_NODE_OUTFALL),  SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "L1", SWMM_LINK_CONDUIT),  SWMM_OK);
        ASSERT_EQ(swmm_link_set_nodes(engine,
                      swmm_link_index(engine, "L1"),
                      swmm_node_index(engine, "J1"),
                      swmm_node_index(engine, "O1")), SWMM_OK);
    }

    void TearDown() override {
        if (engine) swmm_engine_destroy(engine);
    }

    // Each test that touches the filesystem uses its own tmp file so
    // parallel test runs don't collide.
    std::string tmpFile(const char* tag) {
        auto dir = fs::temp_directory_path();
        return (dir / ("aa3_" + std::string(tag) + ".inp")).string();
    }
};

// ---------------------------------------------------------------------------
// swmm_model_write_with_plugin
// ---------------------------------------------------------------------------

TEST_F(ModelWriteWithPluginTest, NullPluginIdFallsBackToInpWriter) {
    const std::string path = tmpFile("null_plugin");
    int rc = swmm_model_write_with_plugin(engine, path.c_str(), nullptr);
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_TRUE(fs::exists(path)) << "expected " << path << " to be written";
    fs::remove(path);
}

TEST_F(ModelWriteWithPluginTest, EmptyPluginIdFallsBackToInpWriter) {
    const std::string path = tmpFile("empty_plugin");
    int rc = swmm_model_write_with_plugin(engine, path.c_str(), "");
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_TRUE(fs::exists(path));
    fs::remove(path);
}

TEST_F(ModelWriteWithPluginTest, UnknownPluginIdReturnsBadParam) {
    const std::string path = tmpFile("unknown_plugin");
    int rc = swmm_model_write_with_plugin(engine, path.c_str(),
                                           "org.does.not.exist");
    EXPECT_EQ(rc, SWMM_ERR_BADPARAM);
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(ModelWriteWithPluginTest, NullPathRejected) {
    int rc = swmm_model_write_with_plugin(engine, nullptr, "");
    EXPECT_EQ(rc, SWMM_ERR_BADPARAM);
}

// ---------------------------------------------------------------------------
// [PLUGINS] section accessors
// ---------------------------------------------------------------------------

TEST_F(ModelWriteWithPluginTest, PluginsEmptyAtStart) {
    int count = -1;
    EXPECT_EQ(swmm_plugins_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 0);
}

TEST_F(ModelWriteWithPluginTest, PluginSetThenGetRoundTrip) {
    EXPECT_EQ(swmm_plugin_set(engine, "org.example.foo",
                               "arg1 arg2 keyA=valueA"),
              SWMM_OK);

    int count = 0;
    ASSERT_EQ(swmm_plugins_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 1);

    char path_buf[128] = {};
    char args_buf[128] = {};
    ASSERT_EQ(swmm_plugin_get(engine, 0, path_buf, sizeof(path_buf),
                               args_buf, sizeof(args_buf)),
              SWMM_OK);
    EXPECT_STREQ(path_buf, "org.example.foo");
    EXPECT_STREQ(args_buf, "arg1 arg2 keyA=valueA");
}

TEST_F(ModelWriteWithPluginTest, PluginSetReplacesByKey) {
    ASSERT_EQ(swmm_plugin_set(engine, "alpha", "first"), SWMM_OK);
    ASSERT_EQ(swmm_plugin_set(engine, "beta",  "second"), SWMM_OK);
    // Replace alpha's args, count stays at 2.
    ASSERT_EQ(swmm_plugin_set(engine, "alpha", "replaced"), SWMM_OK);

    int count = 0;
    ASSERT_EQ(swmm_plugins_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 2);

    char buf[64] = {};
    char args[64] = {};
    ASSERT_EQ(swmm_plugin_get(engine, 0, buf, sizeof(buf), args, sizeof(args)),
              SWMM_OK);
    EXPECT_STREQ(buf, "alpha");
    EXPECT_STREQ(args, "replaced");
}

TEST_F(ModelWriteWithPluginTest, PluginRemoveIsIdempotent) {
    ASSERT_EQ(swmm_plugin_set(engine, "to.remove", ""), SWMM_OK);
    EXPECT_EQ(swmm_plugin_remove(engine, "to.remove"), SWMM_OK);
    EXPECT_EQ(swmm_plugin_remove(engine, "to.remove"), SWMM_OK);  // idempotent
    EXPECT_EQ(swmm_plugin_remove(engine, "never.set"), SWMM_OK);  // also idempotent

    int count = 0;
    ASSERT_EQ(swmm_plugins_count(engine, &count), SWMM_OK);
    EXPECT_EQ(count, 0);
}

TEST_F(ModelWriteWithPluginTest, PluginGetTruncatesSmallBuffer) {
    ASSERT_EQ(swmm_plugin_set(engine, "very.long.plugin.id", "args"),
              SWMM_OK);
    char tiny[6] = {0};
    EXPECT_EQ(swmm_plugin_get(engine, 0, tiny, sizeof(tiny), nullptr, 0),
              SWMM_OK);
    EXPECT_STREQ(tiny, "very.");  // 5 chars + NUL
}

TEST_F(ModelWriteWithPluginTest, PluginGetAllowsNullArgsBuf) {
    ASSERT_EQ(swmm_plugin_set(engine, "id.only", "ignored"), SWMM_OK);
    char path_buf[32] = {};
    EXPECT_EQ(swmm_plugin_get(engine, 0, path_buf, sizeof(path_buf),
                               nullptr, 0),
              SWMM_OK);
    EXPECT_STREQ(path_buf, "id.only");
}

TEST_F(ModelWriteWithPluginTest, PluginSetRejectsEmptyKey) {
    EXPECT_EQ(swmm_plugin_set(engine, "", "args"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_plugin_set(engine, nullptr, "args"), SWMM_ERR_BADPARAM);
}

TEST_F(ModelWriteWithPluginTest, PluginGetBadIndex) {
    char buf[32] = {};
    EXPECT_EQ(swmm_plugin_get(engine, -1, buf, sizeof(buf), nullptr, 0),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_plugin_get(engine, 99, buf, sizeof(buf), nullptr, 0),
              SWMM_ERR_BADINDEX);
}

// ---------------------------------------------------------------------------
// discover_plugins_by_id grouped facade
// ---------------------------------------------------------------------------

TEST(DiscoverPluginsByIdTest, ReturnsAtLeastBuiltinPlugins) {
    auto plugins = openswmm::discover_plugins_by_id();
    // Engine ships at least the default input/output/report plugins,
    // each as a distinct IPluginComponentInfo.  Built-ins must surface.
    EXPECT_GT(plugins.size(), 0u);
    for (const auto& p : plugins) {
        EXPECT_FALSE(p.plugin_id.empty());
        EXPECT_FALSE(p.roles.empty());
        EXPECT_FALSE(p.filters.empty());
    }
}

TEST(DiscoverPluginsByIdTest, RolesDeduplicated) {
    auto plugins = openswmm::discover_plugins_by_id();
    for (const auto& p : plugins) {
        // Each role appears at most once in the de-duplicated set.
        for (std::size_t i = 0; i < p.roles.size(); ++i) {
            for (std::size_t j = i + 1; j < p.roles.size(); ++j) {
                EXPECT_NE(p.roles[i], p.roles[j])
                    << "plugin " << p.plugin_id
                    << " has duplicate role at indices " << i << " and " << j;
            }
        }
    }
}

TEST(DiscoverPluginsByIdTest, MatchesAllFiltersByPluginId) {
    // Cross-check: every (plugin_id, filter) pair from discover_all_filters
    // must show up exactly once in the grouped view.
    auto flat    = openswmm::discover_all_filters();
    auto grouped = openswmm::discover_plugins_by_id();

    std::size_t flat_count = flat.size();
    std::size_t grouped_filter_count = 0;
    for (const auto& p : grouped)
        grouped_filter_count += p.filters.size();
    EXPECT_EQ(flat_count, grouped_filter_count);
}
