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
 * @file PluginFactory.hpp
 * @brief Plugin loader, auto-discovery, and lifecycle manager.
 *
 * @details PluginFactory owns all dynamically loaded plugin libraries.
 * It is responsible for:
 *
 *  1. **Auto-discovery** — On construction, scans the engine library directory
 *     and subdirectories (plugins/, components/) for compatible shared libraries
 *     that export `openswmm_plugin_info`. Discovered libraries are registered
 *     in a component registry keyed by `"id:version"`.
 *
 *  2. **Resolution** — Given a string that is either a file path or an
 *     `"id"` / `"id:version"` identifier, resolves it to an IPluginComponentInfo.
 *
 *  3. **Instantiation** — Creates IInputPlugin, IOutputPlugin, or IReportPlugin
 *     instances via factory methods on IPluginComponentInfo.
 *
 *  4. **Lifecycle dispatch** — Calls initialize(), validate(), prepare(),
 *     finalize(), and write_summary() on all loaded plugin instances.
 *
 *  5. **Cleanup** — dlclose() on all library handles at destruction.
 *
 * ### Threading
 *
 * All lifecycle methods except update() run on the main simulation thread.
 * update() is called from the IO thread (via IOThread).
 *
 * @see IPluginComponentInfo.hpp
 * @see IInputPlugin.hpp
 * @see IOutputPlugin.hpp
 * @see IReportPlugin.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_PLUGIN_FACTORY_HPP
#define OPENSWMM_ENGINE_PLUGIN_FACTORY_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <functional>

// Forward declarations
namespace openswmm {
    class IPluginComponentInfo;
    class IInputPlugin;
    class IOutputPlugin;
    class IReportPlugin;
    class IStateIOPlugin;
    struct SimulationContext;
    struct SimulationSnapshot;
    struct PluginSpec;
    enum class PluginType;
}

namespace openswmm {

/**
 * @brief Manages plugin discovery, loading, and lifecycle for one engine instance.
 *
 * @details One PluginFactory per SWMMEngine instance (NOT a singleton).
 *          On construction, auto-discovers compatible plugin libraries in
 *          standard search directories.
 *
 * @ingroup engine_plugins
 */
class PluginFactory {
public:
    /**
     * @brief Construct and auto-discover plugins in standard directories.
     *
     * @details Scans the engine library directory and subdirectories
     *          (plugins/, components/) for shared libraries that export
     *          `openswmm_plugin_info`. Discovered libraries are registered
     *          in the component registry but plugin instances are NOT created.
     */
    PluginFactory();
    ~PluginFactory();

    // Non-copyable, movable
    PluginFactory(const PluginFactory&) = delete;
    PluginFactory& operator=(const PluginFactory&) = delete;
    PluginFactory(PluginFactory&&) noexcept = default;
    PluginFactory& operator=(PluginFactory&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Discovery and resolution
    // -----------------------------------------------------------------------

    /**
     * @brief Scan standard directories for compatible plugin libraries.
     *
     * @details Called automatically by the constructor. Can be called again
     *          to re-scan (e.g., after plugins are installed at runtime).
     *          Scans: engine library dir, <dir>/plugins/, <dir>/components/.
     *
     * @param warn_cb  Optional callback for per-library load warnings.
     */
    void discover(std::function<void(const std::string&)> warn_cb = {});

    /**
     * @brief Load a single shared library and register it in the component registry.
     *
     * @param path     Path to the shared library.
     * @param warn_cb  Optional warning callback.
     * @returns        Pointer to the IPluginComponentInfo, or nullptr on failure.
     */
    IPluginComponentInfo* load_library(
        const std::string& path,
        std::function<void(const std::string&)> warn_cb = {}
    );

    /**
     * @brief Resolve a plugin identifier to its component info.
     *
     * @details Resolution logic:
     *          1. If the string looks like a file path (contains '/' or '\\',
     *             or ends in .so/.dylib/.dll), load it directly.
     *          2. Otherwise, parse as "id:version" or "id" and look up in
     *             the component registry. If no version is specified, returns
     *             the first match found.
     *
     * @param id_or_path  File path or "id" or "id:version" string.
     * @param warn_cb     Optional warning callback.
     * @returns           Pointer to IPluginComponentInfo, or nullptr if not found.
     */
    IPluginComponentInfo* find_component(
        const std::string& id_or_path,
        std::function<void(const std::string&)> warn_cb = {}
    );

    /**
     * @brief List all discovered component info entries.
     *
     * @details Returns entries from the component registry. Each entry has
     *          id, version, capabilities, and the IPluginComponentInfo pointer.
     */
    struct ComponentEntry {
        std::string            id;
        std::string            version;
        bool                   has_input    = false;
        bool                   has_output   = false;
        bool                   has_report   = false;
        bool                   has_state_io = false;
        IPluginComponentInfo*  info = nullptr;

        /// Slice RC.3 — true when this component was registered via
        /// `register_builtin_infos` (statically linked into the engine)
        /// rather than discovered through the on-disk shared-library scan.
        /// Built-ins have no `dlopen` handle and a synthetic `<built-in>`
        /// path; this flag exposes that distinction to public callers
        /// without leaking the internal LibEntry layout.
        bool                   is_builtin = false;
    };
    std::vector<ComponentEntry> discovered_components() const;

    // -----------------------------------------------------------------------
    // D2 (program plan §B.2.2) — HydroCouple component libraries
    // -----------------------------------------------------------------------
    //
    // A library that exports no `openswmm_plugin_info` is offered to this
    // catalogue before it is unloaded: if it carries the HydroCouple component
    // ABI (`hydrocouple_component_abi_v1`, HydroCouple's
    // `hydrocouplecomponentabi.h`) its stamp is compared against the engine's
    // own, and only on agreement is `hydrocouple_component_info_v1` called.
    //
    // PROCESS-GLOBAL, unlike the rest of this class. The plan's C API for it
    // takes no engine handle (§B.2.5 `swmm_component_library_count(void)`),
    // the same as the built-in `swmm_process_component_known_*` catalogue: a
    // component library is a fact about the installation, not about one
    // model. Every engine's `load_library` feeds the same catalogue, which
    // de-duplicates by path.
    //
    // Libraries stay loaded for the life of the process. The component-info
    // object is a function-local static INSIDE the library, and component
    // instances are host-owned objects whose code lives there too, so
    // unloading while anything could still refer to either is undefined
    // behaviour. Never unloading is the simple correct answer for a discovery
    // catalogue; only `component_catalog_reset_for_testing` closes handles.
    //
    // Without OPENSWMM_WITH_HYDROCOUPLE this compiles and answers "nothing
    // discovered"; the header exposes no HydroCouple type either way.

    /// One library the catalogue looked at and recognised as a component —
    /// loaded, or refused. Libraries that are not components at all are not
    /// recorded (the plan's "skipped silently").
    struct ComponentLibraryRecord {
        std::string path;        ///< the file, as found
        std::string id;          ///< `IComponentInfo::id()`; empty if refused
        std::string caption;
        std::string version;
        std::string kind;        ///< "model" | "other"; empty if refused
        std::string stamp;       ///< the library's own stamp, or "unstamped(…)"
        std::string load_error;  ///< empty on success; names both stamps on refusal
        void*       info = nullptr;  ///< HydroCouple::IComponentInfo*, opaque here
    };

    /// True when built with OPENSWMM_WITH_HYDROCOUPLE.
    static bool hydrocouple_enabled() noexcept;

    /// The engine's own component ABI stamp, or "" when not built with it.
    static std::string component_abi_stamp();

    /// Scan @p dir now and keep it for later discovery. Returns the number of
    /// component libraries newly recorded (loaded or refused), or -1 when
    /// @p dir is not a directory or HydroCouple support is not built.
    static int component_search_path_add(const std::string& dir);

    /// Every recorded library. The first call also runs the default discovery
    /// (§B.2.3): the engine library's directory, its `plugins/` and
    /// `components/` subdirectories, then each entry of the path-list
    /// environment variable `HYDROCOUPLE_COMPONENT_PATH`.
    static std::vector<ComponentLibraryRecord> component_libraries();

    /// Tests only: forget every record and search path, close every handle,
    /// and let the next query run default discovery again (re-reading the
    /// environment). Must not be called while any component instance exists.
    static void component_catalog_reset_for_testing();

    // -----------------------------------------------------------------------
    // Loading (from specs or explicit paths)
    // -----------------------------------------------------------------------

    /**
     * @brief Load plugins from a list of specs (from [PLUGINS] section).
     *
     * @details For each spec, resolves spec.path via find_component(), then
     *          creates plugin instances based on plugin_type.
     *
     * @param specs    Plugin specs (path + init_args).
     * @param warn_cb  Optional warning callback.
     * @returns        Number of successfully loaded plugins.
     */
    int load_plugins(
        const std::vector<PluginSpec>& specs,
        std::function<void(const std::string&)> warn_cb = {}
    );

    // -----------------------------------------------------------------------
    // Lifecycle dispatch (main thread — call in order)
    // -----------------------------------------------------------------------

    int initialize_all(SimulationContext& ctx);
    int validate_all(SimulationContext& ctx);
    int prepare_all(SimulationContext& ctx);
    int update_all(const SimulationSnapshot& snapshot);
    int finalize_all(SimulationContext& ctx);
    int write_summary_all(SimulationContext& ctx);

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    const std::vector<IInputPlugin*>&    input_plugins()    const noexcept { return input_plugins_; }
    const std::vector<IOutputPlugin*>&   output_plugins()   const noexcept { return output_plugins_; }
    const std::vector<IReportPlugin*>&   report_plugins()   const noexcept { return report_plugins_; }
    const std::vector<IStateIOPlugin*>&  state_io_plugins() const noexcept { return state_io_plugins_; }

    int plugin_count() const noexcept {
        return static_cast<int>(input_plugins_.size()
                              + output_plugins_.size()
                              + report_plugins_.size()
                              + state_io_plugins_.size());
    }

    bool empty() const noexcept { return plugin_count() == 0; }

    void unload_all();

    // -----------------------------------------------------------------------
    // Plugin injection (for built-in / programmatic plugins)
    // -----------------------------------------------------------------------

    void add_output_plugin(IOutputPlugin* plugin, std::vector<std::string> args = {});
    void add_report_plugin(IReportPlugin* plugin);
    void add_input_plugin(IInputPlugin* plugin);
    void add_state_io_plugin(IStateIOPlugin* plugin);

private:
    // -----------------------------------------------------------------------
    // Internal types
    // -----------------------------------------------------------------------

    struct LibEntry {
        void*                  handle = nullptr;
        IPluginComponentInfo*  info   = nullptr;
        std::string            path;
    };

    // -----------------------------------------------------------------------
    // Component registry
    // -----------------------------------------------------------------------

    /// Key: "id:version", Value: index into libs_
    std::unordered_map<std::string, std::size_t> registry_;

    // -----------------------------------------------------------------------
    // Plugin storage
    // -----------------------------------------------------------------------

    std::vector<LibEntry>         libs_;
    std::vector<IInputPlugin*>    input_plugins_;
    std::vector<IOutputPlugin*>   output_plugins_;
    std::vector<IReportPlugin*>   report_plugins_;
    std::vector<IStateIOPlugin*>  state_io_plugins_;
    std::vector<std::vector<std::string>> init_args_;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    void scan_directory(const std::string& dir_path,
                        std::function<void(const std::string&)> warn_cb);

    /**
     * @brief Register synthetic LibEntry's for built-in plugin metadata.
     *
     * @details The default Input/Output/Report/State-IO plugin instances are
     *          injected into PluginFactory via add_*_plugin() rather than
     *          loaded via dlopen. Their IPluginComponentInfo singletons are
     *          registered here as synthetic LibEntry's (handle = nullptr) so
     *          their metadata — especially file_filters() — is visible to
     *          discovered_components() and to the GUI FileFilterRegistry.
     */
    void register_builtin_infos();

    /**
     * @brief Verify a plugin's file_filters() match its has_* capabilities.
     *
     * @details Checked once at load time. A capability without a matching
     *          filter triggers a warn_cb call (or stderr fallback) so the
     *          plugin still loads but the GUI can flag missing labels.
     */
    void validate_filter_invariant(IPluginComponentInfo* info,
                                   const std::string& source_path,
                                   std::function<void(const std::string&)> warn_cb);

    static bool is_file_path(const std::string& str);
    static bool is_shared_library(const std::string& filename);
    static std::string get_library_directory();

    static void* platform_load(const std::string& path);
    static void  platform_unload(void* handle) noexcept;
    static void* platform_sym(void* handle, const char* sym) noexcept;
    static std::string platform_error() noexcept;

    // D2: the component catalogue's internals. Private statics because they
    // use platform_* above; the catalogue state itself is file-local in
    // PluginFactory.cpp. Callers must hold the catalogue mutex.
    /// Offer an OPEN handle that exported no `openswmm_plugin_info`. Returns
    /// true if the catalogue recorded it (loaded or refused) and has taken
    /// charge of the handle; false if it is not a component at all, in which
    /// case the caller still owns the handle.
    static bool catalog_adopt_locked(const std::string& path, void* handle);
    /// Scan one directory into the catalogue. Returns records added.
    static int  catalog_scan_locked(const std::string& dir);
    /// Run §B.2.3 default discovery once.
    static void catalog_defaults_locked();
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_PLUGIN_FACTORY_HPP */
