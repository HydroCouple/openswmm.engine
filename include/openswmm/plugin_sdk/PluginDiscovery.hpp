/**
 * @file PluginDiscovery.hpp
 * @brief Public, framework-free facade for enumerating plugin file filters.
 *
 * @details Hosts (Qt GUIs, CLI tools, Python bindings) typically only need to
 *          know what file formats the engine and its plugins handle, not the
 *          full lifecycle. This header exposes a one-shot discovery facade
 *          that walks every built-in default plugin plus every on-disk
 *          plugin shared library, and returns the flat list of FileFilter
 *          entries each plugin advertises.
 *
 *          Internally this constructs a transient `PluginFactory`, so calling
 *          `discover_all_filters()` triggers a directory scan. Callers should
 *          cache the result.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_PLUGIN_DISCOVERY_HPP
#define OPENSWMM_PLUGIN_DISCOVERY_HPP

#include <string>
#include <vector>

#include "IPluginComponentInfo.hpp"

namespace openswmm {

/**
 * @brief A FileFilter paired with the plugin id and version that emitted it.
 *
 * @details The id/version are useful for hosts that want to label or sort
 *          filters by their providing plugin (e.g., grouping all GeoPackage
 *          filters together regardless of role).
 *
 * @ingroup engine_plugin_sdk
 */
struct DiscoveredFilter {
    std::string  plugin_id;       ///< IPluginComponentInfo::id() of the source plugin
    std::string  plugin_version;  ///< IPluginComponentInfo::version() of the source plugin
    std::string  plugin_caption;  ///< IPluginComponentInfo::caption() — human-readable
    FileFilter   filter;          ///< The advertised filter
};

/**
 * @brief Enumerate every FileFilter advertised by every discovered plugin.
 *
 * @details Constructs a transient PluginFactory, runs auto-discovery, then
 *          flattens IPluginComponentInfo::file_filters() across all components.
 *          The returned list preserves the order of (plugin, filter-within-plugin).
 *
 * @returns Flat snapshot of all advertised filters.
 */
std::vector<DiscoveredFilter> discover_all_filters();

} /* namespace openswmm */

#endif /* OPENSWMM_PLUGIN_DISCOVERY_HPP */
