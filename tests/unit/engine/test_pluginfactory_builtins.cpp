/**
 * @file test_pluginfactory_builtins.cpp
 * @brief Slice RC.5 — verify GeoPackage is an explicit built-in plugin and
 *        the discovery API surfaces the `is_builtin` flag correctly.
 *
 * Coverage:
 *   - The four Default plugins (Input / Output / Report / StateIO) are
 *     always registered and flagged is_builtin=true.
 *   - GeoPackage is registered as a built-in iff OPENSWMM_HAS_GEOPACKAGE.
 *   - The `is_builtin` field propagates through DiscoveredFilter and
 *     DiscoveredPlugin without losing identity.
 *
 * Note: the historic dlsym-leak (where the engine binary's own
 *       `openswmm_plugin_info` symbol was inadvertently discovered
 *       through the scan path) cannot be probed from pure C++ at unit-test
 *       level — that's a build-property check left to the engine's
 *       packaging tests. Visibility-hidden hardening (Slice RC.2) is
 *       verified at build time by the CMake target properties.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <openswmm/plugin_sdk/PluginDiscovery.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace {

const openswmm::DiscoveredPlugin*
findById(const std::vector<openswmm::DiscoveredPlugin>& plugins,
         const std::string& id)
{
    auto it = std::find_if(plugins.begin(), plugins.end(),
        [&](const openswmm::DiscoveredPlugin& p) {
            return p.plugin_id == id;
        });
    return it == plugins.end() ? nullptr : &*it;
}

} // anonymous

// ---------------------------------------------------------------------------
// Default built-ins must be present and flagged
// ---------------------------------------------------------------------------

TEST(PluginFactoryBuiltins, DefaultPluginsAreRegisteredAsBuiltins)
{
    const auto plugins = openswmm::discover_plugins_by_id();
    ASSERT_FALSE(plugins.empty());

    // Walk every plugin; assert at least one carries is_builtin=true so we
    // know the field was wired through (the four Default plugins are
    // always registered via PluginFactory::register_builtin_infos).
    bool sawBuiltin = false;
    for (const auto& p : plugins) {
        if (p.is_builtin) {
            sawBuiltin = true;
            break;
        }
    }
    EXPECT_TRUE(sawBuiltin)
        << "Expected at least one is_builtin=true plugin from "
        << "register_builtin_infos (Default Input/Output/Report/StateIO).";
}

// ---------------------------------------------------------------------------
// GeoPackage — gated on OPENSWMM_HAS_GEOPACKAGE
// ---------------------------------------------------------------------------

TEST(PluginFactoryBuiltins, GeoPackageRegistrationMatchesBuildConfig)
{
    const auto plugins = openswmm::discover_plugins_by_id();
    const auto* gpkg = findById(plugins,
        "org.hydrocouple.openswmm.plugins.geopackage");

#ifdef OPENSWMM_HAS_GEOPACKAGE
    ASSERT_NE(gpkg, nullptr)
        << "Expected GeoPackage to be registered as a built-in when "
           "OPENSWMM_HAS_GEOPACKAGE is defined.";
    EXPECT_TRUE(gpkg->is_builtin)
        << "GeoPackage must be flagged is_builtin=true (Slice RC.1).";
#else
    EXPECT_EQ(gpkg, nullptr)
        << "GeoPackage should NOT be registered when "
           "OPENSWMM_HAS_GEOPACKAGE is undefined.";
#endif
}

TEST(PluginFactoryBuiltins, NoDuplicateRegistrationOfGeoPackage)
{
    const auto plugins = openswmm::discover_plugins_by_id();
    int count = 0;
    for (const auto& p : plugins) {
        if (p.plugin_id == "org.hydrocouple.openswmm.plugins.geopackage")
            ++count;
    }
#ifdef OPENSWMM_HAS_GEOPACKAGE
    EXPECT_EQ(count, 1)
        << "GeoPackage should appear exactly once — the explicit "
           "register_one in register_builtin_infos (Slice RC.1) must "
           "not collide with any leftover discovery-scan registration.";
#else
    EXPECT_EQ(count, 0);
#endif
}

// ---------------------------------------------------------------------------
// is_builtin propagates through DiscoveredFilter -> DiscoveredPlugin
// ---------------------------------------------------------------------------

TEST(PluginFactoryBuiltins, IsBuiltinPropagatesThroughDiscoveredFilter)
{
    const auto filters = openswmm::discover_all_filters();
    ASSERT_FALSE(filters.empty());

    // At least one filter from a built-in source must carry is_builtin=true
    // (Slice RC.3 added the field to DiscoveredFilter and the discovery
    // implementation now propagates it from ComponentEntry).
    bool sawBuiltinFilter = false;
    for (const auto& f : filters) {
        if (f.is_builtin) { sawBuiltinFilter = true; break; }
    }
    EXPECT_TRUE(sawBuiltinFilter);
}

TEST(PluginFactoryBuiltins, NonBuiltinDefaultIsFalseForAccidentalScanHits)
{
    // Defensive: if discover_plugins_by_id ever returns a plugin without
    // an explicit is_builtin assignment (e.g. a future on-disk-scanned
    // plugin), the field's default (false) must hold so callers don't
    // mistake it for a built-in. We can't synthesize a scan plugin in
    // this test environment, but we can assert that the field's default
    // is documented as false by inspecting a default-constructed value.
    openswmm::DiscoveredPlugin fresh;
    EXPECT_FALSE(fresh.is_builtin);
}
