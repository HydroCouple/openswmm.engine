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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_PLUGIN_DISCOVERY_HPP
#define OPENSWMM_PLUGIN_DISCOVERY_HPP

#include <string>
#include <vector>

#include "IPluginComponentInfo.hpp"

/* =========================================================================
 * Export macro
 *
 * The engine target sets WINDOWS_EXPORT_ALL_SYMBOLS OFF and relies on explicit
 * export macros (see src/engine/CMakeLists.txt). On ELF/Mach-O the target's
 * CXX_VISIBILITY_PRESET is `default`, so omitting the macro still exports the
 * symbol — which is why the unannotated declarations below linked fine on
 * Linux/macOS while failing on Windows with:
 *     error LNK2001: unresolved external symbol openswmm::discover_all_filters
 * Block is byte-identical to the one in openswmm_engine.h / openswmm_2d.h;
 * identical macro redefinition is well-defined, so including those alongside
 * this header is safe.
 * ========================================================================= */

#ifdef OPENSWMM_ENGINE_STATIC
#  define SWMM_ENGINE_API
#else
#  ifdef _WIN32
#    ifdef openswmm_engine_EXPORTS
#      define SWMM_ENGINE_API __declspec(dllexport)
#    else
#      define SWMM_ENGINE_API __declspec(dllimport)
#    endif
#  else
#    define SWMM_ENGINE_API __attribute__((visibility("default")))
#  endif
#endif

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

    /// Slice RC.3 — true when the source plugin is an engine built-in
    /// (statically linked into the engine, registered via
    /// PluginFactory::register_builtin_infos). False for plugins
    /// discovered through the on-disk shared-library scan. Propagated
    /// up to DiscoveredPlugin's matching field by discover_plugins_by_id.
    bool         is_builtin = false;
};

/**
 * @brief Enumerate every FileFilter advertised by every discovered plugin.
 *
 * @details Constructs a transient PluginFactory, runs auto-discovery, then
 *          flattens IPluginComponentInfo::file_filters() across all components.
 *          The returned list preserves the order of (plugin, filter-within-plugin).
 *
 * @returns Flat snapshot of all advertised filters.
 *
 * @note Returns std::vector by value across the library boundary. Safe for
 *       consumers built with the same toolchain and C++ runtime as the engine
 *       (which is what the GUI and the in-tree CI do). A consumer built with a
 *       different MSVC runtime/STL must not call this directly.
 */
SWMM_ENGINE_API std::vector<DiscoveredFilter> discover_all_filters();

/**
 * @brief A plugin grouped by id with all the roles and filters it advertises.
 *
 * @details Companion view to `DiscoveredFilter` for hosts that need to
 *          reason about plugin capabilities at the *plugin* level rather
 *          than per-filter.  The driving use case is detecting tri-role
 *          plugins (e.g., the GeoPackage trio sharing a single
 *          `plugin_id`) so a GUI can offer a "single container" toggle
 *          when one plugin handles INPUT_READ + REPORT_WRITE +
 *          OUTPUT_WRITE for the same extension.
 *
 *          A plugin appears once per `plugin_id`, with `roles` and
 *          `filters` accumulated across every IPluginComponentInfo
 *          registered under that id.
 *
 * @ingroup engine_plugin_sdk
 */
struct DiscoveredPlugin {
    std::string                plugin_id;       ///< IPluginComponentInfo::id()
    std::string                plugin_version;  ///< IPluginComponentInfo::version()
    std::string                plugin_caption;  ///< IPluginComponentInfo::caption()
    std::vector<PluginRole>    roles;           ///< Distinct roles advertised across all filters
    std::vector<FileFilter>    filters;         ///< Every filter the plugin advertises

    /**
     * @brief True when the plugin was registered as an engine built-in
     *        (via PluginFactory::register_builtin_infos) rather than
     *        discovered through the on-disk shared-library scan.
     *
     * @details Slice RC.3 (APPROVED 2026-05-25). Hosts use this to gate
     *          UI affordances that don't make sense for built-ins — e.g.
     *          the Simulation Options Plugins-tab Remove button cannot
     *          dlclose a statically-linked plugin, so it's greyed out
     *          when `is_builtin == true`. The default (`false`) keeps
     *          older callers' behavior unchanged: any plugin discovered
     *          via the directory scan is treated as a non-builtin.
     */
    bool                       is_builtin = false;
};

/**
 * @brief Enumerate every discovered plugin, grouped by `plugin_id`.
 *
 * @details Walks `discover_all_filters()` and folds entries that share
 *          a `plugin_id` into a single `DiscoveredPlugin`.  `roles` is
 *          de-duplicated.  Order of plugins follows the order in which
 *          their first filter is encountered (stable across calls).
 *
 *          Use this when you need "what does plugin X support overall"
 *          rather than "give me every (plugin, filter) pair".
 *
 * @returns Flat snapshot of plugins, one entry per `plugin_id`.
 *
 * @note Same cross-boundary std::vector caveat as discover_all_filters().
 */
SWMM_ENGINE_API std::vector<DiscoveredPlugin> discover_plugins_by_id();

} /* namespace openswmm */

#endif /* OPENSWMM_PLUGIN_DISCOVERY_HPP */
