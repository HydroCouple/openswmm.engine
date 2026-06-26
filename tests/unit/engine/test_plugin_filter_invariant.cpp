/**
 * @file test_plugin_filter_invariant.cpp
 * @brief Verifies that every discovered plugin advertises file_filters()
 *        consistent with its capability flags.
 *
 * @details The contract: if `info->has_input()` is true, the plugin must
 *          return at least one FileFilter with role INPUT_READ; same for
 *          OUTPUT, REPORT, and STATE_IO. The PluginFactory logs a warning
 *          when it sees a violation; this test enforces the invariant for
 *          all in-tree plugins so a missed filter declaration breaks CI.
 *
 * @see include/openswmm/plugin_sdk/IPluginComponentInfo.hpp
 * @see src/engine/plugins/PluginFactory.cpp (validate_filter_invariant)
 * @ingroup engine_plugins
 */

#include <gtest/gtest.h>

#include <openswmm/plugin_sdk/IPluginComponentInfo.hpp>

#include "../../src/engine/plugins/PluginFactory.hpp"

using openswmm::FileFilter;
using openswmm::IPluginComponentInfo;
using openswmm::PluginFactory;
using openswmm::PluginRole;

namespace {

bool has_role(const std::vector<FileFilter>& filters, PluginRole role) {
    for (const auto& f : filters) {
        if (f.role == role) return true;
    }
    return false;
}

} // namespace

TEST(PluginFilterInvariant, EveryDiscoveredPluginAdvertisesFiltersForItsCapabilities) {
    PluginFactory factory;  // auto-discovers built-ins + any plugins on disk

    const auto components = factory.discovered_components();
    ASSERT_FALSE(components.empty())
        << "PluginFactory discovered zero components — at minimum the three "
           "built-in defaults (input, output, report) should be registered.";

    for (const auto& c : components) {
        ASSERT_NE(c.info, nullptr) << "ComponentEntry for '" << c.id << "' has null info";
        const auto filters = c.info->file_filters();

        if (c.has_input) {
            EXPECT_TRUE(has_role(filters, PluginRole::INPUT_READ))
                << "Plugin '" << c.id << "' has has_input() == true but no FileFilter "
                   "with role INPUT_READ.";
        }
        if (c.has_output) {
            EXPECT_TRUE(has_role(filters, PluginRole::OUTPUT_WRITE))
                << "Plugin '" << c.id << "' has has_output() == true but no FileFilter "
                   "with role OUTPUT_WRITE.";
        }
        if (c.has_report) {
            EXPECT_TRUE(has_role(filters, PluginRole::REPORT_WRITE))
                << "Plugin '" << c.id << "' has has_report() == true but no FileFilter "
                   "with role REPORT_WRITE.";
        }
        if (c.has_state_io) {
            EXPECT_TRUE(has_role(filters, PluginRole::STATE_READ) ||
                        has_role(filters, PluginRole::STATE_WRITE))
                << "Plugin '" << c.id << "' has has_state_io() == true but no FileFilter "
                   "with role STATE_READ or STATE_WRITE.";
        }
    }
}

TEST(PluginFilterInvariant, BuiltinDefaultsAreRegisteredAndCarryFilters) {
    PluginFactory factory;
    const auto components = factory.discovered_components();

    auto find_id = [&](const std::string& id) -> const PluginFactory::ComponentEntry* {
        for (const auto& c : components) {
            if (c.id == id) return &c;
        }
        return nullptr;
    };

    const auto* in    = find_id("org.hydrocouple.openswmm.builtin.input");
    const auto* out   = find_id("org.hydrocouple.openswmm.builtin.output");
    const auto* rpt   = find_id("org.hydrocouple.openswmm.builtin.report");
    const auto* state = find_id("org.hydrocouple.openswmm.builtin.state_io");

    ASSERT_NE(in,    nullptr) << "Builtin default input plugin info not registered";
    ASSERT_NE(out,   nullptr) << "Builtin default output plugin info not registered";
    ASSERT_NE(rpt,   nullptr) << "Builtin default report plugin info not registered";
    ASSERT_NE(state, nullptr) << "Builtin default state-IO plugin info not registered";

    EXPECT_TRUE(has_role(in->info->file_filters(),    PluginRole::INPUT_READ));
    EXPECT_TRUE(has_role(out->info->file_filters(),   PluginRole::OUTPUT_WRITE));
    EXPECT_TRUE(has_role(rpt->info->file_filters(),   PluginRole::REPORT_WRITE));
    EXPECT_TRUE(has_role(state->info->file_filters(), PluginRole::STATE_READ));
    EXPECT_TRUE(has_role(state->info->file_filters(), PluginRole::STATE_WRITE));
}

TEST(PluginFilterInvariant, FilePatternsAreNonEmptyAndStartWithStarDot) {
    PluginFactory factory;

    for (const auto& c : factory.discovered_components()) {
        for (const auto& f : c.info->file_filters()) {
            EXPECT_FALSE(f.description.empty())
                << "Plugin '" << c.id << "' returned a FileFilter with an empty description";
            ASSERT_FALSE(f.patterns.empty())
                << "Plugin '" << c.id << "' returned a FileFilter with no patterns "
                   "(role=" << openswmm::plugin_role_to_string(f.role) << ")";
            for (const auto& p : f.patterns) {
                EXPECT_GE(p.size(), 3u)
                    << "Plugin '" << c.id << "' has too-short pattern '" << p << "'";
                EXPECT_EQ(p.substr(0, 2), std::string("*."))
                    << "Plugin '" << c.id << "' pattern '" << p
                    << "' does not start with '*.' — hosts expect glob form.";
            }
        }
    }
}
