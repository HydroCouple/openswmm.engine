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
 * @file      PluginFactory.hpp
 * @brief     Plugin loader, auto-discovery, and lifecycle manager.
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
 * update_all() can run on the IO thread. Instance registries/lists have no
 * internal lock: finish/flush outstanding updates before lifecycle changes,
 * injection, moves or unload. Only the process-global component catalogue
 * has its own mutex. Callbacks execute synchronously and exceptions propagate.
 *
 * @see IPluginComponentInfo.hpp
 * @see IInputPlugin.hpp
 * @see IOutputPlugin.hpp
 * @see IReportPlugin.hpp
 * @ingroup engine_plugins
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    /** @brief Register builtin metadata and scan the engine library directory plus plugins/components.
     * Does not construct plugin instances. Discovery can load dynamic-library code and
     * run metadata entry points; filesystem/allocation/plugin exceptions propagate.
     * Directory iteration order is not globally sorted for ordinary IO-plugin discovery. */
    PluginFactory();
    /** @brief Delete owned plugin instances before unloading per-instance IO libraries.
     * Requires no outstanding update/callback/borrowed plugin use; global HydroCouple
     * catalogue handles are retained separately until process exit or explicit test reset. */
    ~PluginFactory();

    // Non-copyable, movable
    /** @brief Disable copying owned plugin pointers and library references. */
    PluginFactory(const PluginFactory&) = delete;
    /** @brief Disable copy assignment of plugin/library ownership. */
    PluginFactory& operator=(const PluginFactory&) = delete;
    /** @brief Move the registry, raw-pointer ownership lists and library entries.
     * @par Source
     * The unnamed source must have no outstanding operations/borrowed factory references.
     * Callers must not continue using moved-from plugin collections as live ownership. */
    PluginFactory(PluginFactory&&) noexcept = default;
    /** @brief Move-assign registry and ownership containers using their default operations.
     * @par Source and destination
     * The unnamed source supplies ownership; destination must first unload its existing
     * plugins/libraries, because default vector assignment does not delete raw plugin
     * pointers or release platform handles stored in discarded entries. No active IO
     * or borrowed pointer use may overlap the move.
     * @return This factory. */
    PluginFactory& operator=(PluginFactory&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Discovery and resolution
    // -----------------------------------------------------------------------

    /** @brief Rescan the host library directory and its plugins/components subdirectories.
     * @param warn_cb Optional synchronous per-load warning sink; absent sinks drop
     * those warnings. Callback exceptions propagate. Existing entries with the exact
     * path spelling are skipped; no instances are created and missing components are
     * not removed. Filesystem scans are shallow and unsorted for ordinary plugins. */
    void discover(std::function<void(const std::string&)> warn_cb = {});

    /** @brief Load/register an IO plugin or offer a non-IO library to the global component catalogue.
     * @param path Platform path spelling, compared literally for per-instance duplicates.
     * Native loader uses LoadLibraryA or dlopen(RTLD_NOW|RTLD_LOCAL); WebAssembly
     * cannot dynamically load. IO metadata entry points are invoked without a separate
     * ABI stamp check; loaded plugins must match the engine toolchain/SDK.
     * @param warn_cb Optional synchronous warning sink; absent means no warning output.
     * @return Borrowed IO metadata, valid until unload/destruction, or null for failure/
     * non-IO components. The latter can still have been adopted by the process-global
     * catalogue. Registering the same id:version replaces that exact key's lookup; the
     * unversioned id keeps its first-loaded alias. Failure is not an exception boundary,
     * and allocation/plugin exceptions can leave partial registrations/loaded handles. */
    IPluginComponentInfo* load_library(const std::string& path,
                                       std::function<void(const std::string&)> warn_cb = {});

    /** @brief Resolve an exact registered ID/version or load an apparent platform library path.
     * @param id_or_path Exact string; empty returns null. Slash/backslash or a recognized
     * platform suffix routes to load_library(); otherwise lookup is case-sensitive.
     * @param warn_cb Optional synchronous warning sink for load/lookup failure.
     * @return Borrowed metadata valid until unload/destruction, or null. An unversioned
     * ID uses its first-loaded alias; no semantic-version comparison is performed. */
    IPluginComponentInfo* find_component(const std::string& id_or_path,
                                         std::function<void(const std::string&)> warn_cb = {});

    /**
     * @brief List all discovered component info entries.
     *
     * @details Returns entries from the component registry. Each entry has
     *          id, version, capabilities, and the IPluginComponentInfo pointer.
     */
    struct ComponentEntry {
        std::string id;
        std::string version;
        bool has_input = false;
        bool has_output = false;
        bool has_report = false;
        bool has_state_io = false;
        IPluginComponentInfo* info = nullptr;

        /// Slice RC.3 — true when this component was registered via
        /// `register_builtin_infos` (statically linked into the engine)
        /// rather than discovered through the on-disk shared-library scan.
        /// Built-ins have no `dlopen` handle and a synthetic `<built-in>`
        /// path; this flag exposes that distinction to public callers
        /// without leaking the internal LibEntry layout.
        bool is_builtin = false;
    };
    /** @brief Snapshot registered metadata/capabilities in retained library order.
     * @return Owned strings/flags with borrowed info pointers. Multiple entries can share
     * an ID/version despite registry aliases; pointers expire on unload. Builtin is
     * detected by synthetic path spelling, not by probing the library handle. */
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
        std::string path; ///< the file, as found
        std::string id;   ///< `IComponentInfo::id()`; empty if refused
        std::string caption;
        std::string version;
        std::string kind;       ///< "model" | "other"; empty if refused
        std::string stamp;      ///< the library's own stamp, or "unstamped(…)"
        std::string load_error; ///< empty on success; names both stamps on refusal
        void* info = nullptr;   ///< HydroCouple::IComponentInfo*, opaque here
    };

    /** @brief Report this build's optional HydroCouple component-loader support.
     * @return Compile-time support flag, not whether components were discovered. */
    static bool hydrocouple_enabled() noexcept;

    /** @brief Copy the engine's HydroCouple component ABI fingerprint.
     * @return Build stamp, or empty when optional support is absent. */
    static std::string component_abi_stamp();

    /** @brief Scan an explicit directory into the process-global component catalogue.
     * @param dir Existing directory; platform path encoding rules apply. Defaults are
     * scanned first so explicit candidates cannot replace default IDs. The path is
     * recorded for bookkeeping; later queries do not automatically rescan it.
     * @return Newly recorded loaded/refused components, or -1 without support/valid
     * directory. Uses the catalogue mutex; sorted candidate paths make first-ID wins
     * deterministic within a directory. May load library code; exceptions propagate. */
    static int component_search_path_add(const std::string& dir);

    /** @brief Snapshot process-global loaded/refused component records under its mutex.
     * @return Owned records with borrowed opaque info pointers, valid until test reset
     * or process exit. First supported call scans default library-relative directories
     * and HYDROCOUPLE_COMPONENT_PATH; ordinary per-factory IO-plugin entries are excluded. */
    static std::vector<ComponentLibraryRecord> component_libraries();

    /** @brief Close globally retained component handles and reset records/default-discovery state.
     * Tests must ensure no component instances, metadata pointers or executing code
     * remain borrowed. Mutex serializes catalogue mutation but cannot make live instance
     * unloading safe. The next query rediscovers defaults/environment. */
    static void component_catalog_reset_for_testing();

    // -----------------------------------------------------------------------
    // Loading (from specs or explicit paths)
    // -----------------------------------------------------------------------

    /** @brief Resolve specs and create every advertised input/output/report/state-IO capability.
     * @param specs Borrowed ordered specs; path selects metadata and init_args are
     * retained with each created capability instance. Explicit IO role selections
     * suppress legacy output/report discovery for that role. Existing instances remain.
     * @param warn_cb Optional synchronous resolution-warning sink.
     * @return Count of specs whose metadata resolved, even if factory methods returned
     * no plugin instances. No lifecycle method is called here. Returned raw plugin
     * objects become owned by this factory; allocation/plugin exceptions propagate. */
    int load_plugins(const std::vector<PluginSpec>& specs,
                     std::function<void(const std::string&)> warn_cb = {},
                     const SimulationContext* io = nullptr);

    // -----------------------------------------------------------------------
    // Lifecycle dispatch (main thread — call in order)
    // -----------------------------------------------------------------------

    /** @brief Initialize output, report and state-IO instances in that order.
     * @param ctx Unused context; input instances are not initialized by this dispatcher.
     * Each instance receives its retained positional arguments and originating
     * component metadata.
     * @return Last nonzero initialize result, or zero; calls continue after returned
     * errors and have no state gate. Plugin exceptions propagate without rollback. */
    int initialize_all(SimulationContext& ctx);
    /** @brief Validate INITIALIZED output/report/state-IO plugins in list order.
     * @param[in,out] ctx Borrowed engine state passed to each eligible plugin; input
     * plugins and all non-INITIALIZED instances are skipped.
     * @return Last nonzero result, or zero; later eligible plugins still run after
     * returned errors. Exceptions propagate and earlier mutations are not rolled back. */
    int validate_all(SimulationContext& ctx);
    /** @brief Prepare VALIDATED output and report plugins.
     * @param[in,out] ctx Borrowed mutable state; input and state-IO plugins are not
     * prepared by this dispatcher, regardless of their state.
     * @return Last nonzero result, or zero; continues after return-code errors.
     * Exceptions propagate and earlier side effects are retained. */
    int prepare_all(SimulationContext& ctx);
    /** @brief Deliver a snapshot to PREPARED/UPDATING output and report plugins.
     * @param snapshot Borrowed immutable packet valid only during each callback;
     * plugins must copy data they retain. Called serially by IOThread in native builds.
     * Input/state-IO plugins and other states are skipped.
     * @return Last nonzero update result, or zero; dispatch continues after returned
     * errors. No exception boundary or collection mutex is supplied. Do not race
     * unload/lifecycle/injection or read mutable context through an unsynchronized alias. */
    int update_all(const SimulationSnapshot& snapshot);
    /** @brief Finalize every output/report/state-IO plugin without a state gate.
     * @param[in,out] ctx Borrowed mutable engine state. Input plugins are skipped.
     * @return Last nonzero result, or zero; continues after returned errors, propagates
     * exceptions and retains earlier effects. Caller first drains outstanding IO updates. */
    int finalize_all(SimulationContext& ctx);
    /** @brief Ask every report plugin to write a summary without a state gate.
     * @param[in,out] ctx Borrowed engine state used by report implementations.
     * @return Last nonzero result, or zero; continues after returned errors.
     * No other capability is called, and exceptions/IO side effects are not rolled back. */
    int write_summary_all(SimulationContext& ctx);

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    /** @brief Borrow the retained capability list without transferring ownership.
     * @return input pointers owned by this factory; collection references/elements
     * can be invalidated by load/injection/move/unload. Do not mutate/delete
     * instances or race collection changes through borrowed access. */
    const std::vector<IInputPlugin*>& input_plugins() const noexcept { return input_plugins_; }
    /** @brief Borrow the retained capability list without transferring ownership.
     * @return output pointers owned by this factory; collection references/elements
     * can be invalidated by load/injection/move/unload. Do not mutate/delete
     * instances or race collection changes through borrowed access. */
    const std::vector<IOutputPlugin*>& output_plugins() const noexcept { return output_plugins_; }
    /** @brief Borrow the retained capability list without transferring ownership.
     * @return report pointers owned by this factory; collection references/elements
     * can be invalidated by load/injection/move/unload. Do not mutate/delete
     * instances or race collection changes through borrowed access. */
    const std::vector<IReportPlugin*>& report_plugins() const noexcept { return report_plugins_; }
    /** @brief Borrow the retained capability list without transferring ownership.
     * @return state_io pointers owned by this factory; collection references/elements
     * can be invalidated by load/injection/move/unload. Do not mutate/delete
     * instances or race collection changes through borrowed access. */
    const std::vector<IStateIOPlugin*>& state_io_plugins() const noexcept {
        return state_io_plugins_;
    }

    /** @brief Count all injected/created capability instances.
     * @return Sum of input/output/report/state-IO list lengths as int, not library/spec count. */
    int plugin_count() const noexcept {
        return static_cast<int>(input_plugins_.size() + output_plugins_.size() +
                                report_plugins_.size() + state_io_plugins_.size());
    }

    /** @brief Test whether any capability instance is retained.
     * @return plugin_count()==0; discovered metadata can still be present. */
    bool empty() const noexcept { return plugin_count() == 0; }

    /** @brief Delete all owned capability instances, clear registries/args and release IO library handles.
     * Does not implicitly finalize plugins or wait for IOThread. Caller must drain
     * callbacks and cease borrowed-pointer use first. Builtin metadata singletons
     * survive; process-global component records/handles are not cleared. */
    void unload_all();

    // -----------------------------------------------------------------------
    // Plugin injection (for built-in / programmatic plugins)
    // -----------------------------------------------------------------------

    /** @brief Adopt a programmatically constructed output plugin without lifecycle calls.
     * @param plugin Required nonnull, uniquely owned compatible instance; no null or
     * duplicate checks occur. Factory deletes it on unload.
     * @param args Initialization strings moved into the output's positional args slot.
     * Caller must avoid concurrent dispatch; allocation failure can leave partially appended lists. */
    void add_output_plugin(IOutputPlugin* plugin, std::vector<std::string> args = {},
                           IPluginComponentInfo* info = nullptr);
    /** @brief Adopt a report plugin without lifecycle calls or validation.
     * @param plugin Required nonnull uniquely owned instance, deleted on unload.
     * Do not insert a duplicate pointer or mutate lists concurrently with dispatch. */
    void add_report_plugin(IReportPlugin* plugin, std::vector<std::string> args = {},
                           IPluginComponentInfo* info = nullptr);
    /** @brief Adopt an input plugin without lifecycle calls or validation.
     * @param plugin Required nonnull uniquely owned instance, deleted on unload.
     * Input lifecycle is managed separately from the all() output/report dispatchers. */
    void add_input_plugin(IInputPlugin* plugin);
    /** @brief Adopt a state-IO plugin without lifecycle calls or validation.
     * @param plugin Required nonnull uniquely owned instance, deleted on unload.
     * Do not insert duplicates or race active dispatch/collection access. */
    void add_state_io_plugin(IStateIOPlugin* plugin);

private:
    // -----------------------------------------------------------------------
    // Internal types
    // -----------------------------------------------------------------------

    struct LibEntry {
        void* handle = nullptr;
        IPluginComponentInfo* info = nullptr;
        std::string path;
    };

    // -----------------------------------------------------------------------
    // Component registry
    // -----------------------------------------------------------------------

    /// Key: "id:version", Value: index into libs_
    std::unordered_map<std::string, std::size_t> registry_;

    // -----------------------------------------------------------------------
    // Plugin storage
    // -----------------------------------------------------------------------

    std::vector<LibEntry> libs_;
    std::vector<IInputPlugin*> input_plugins_;
    std::vector<IOutputPlugin*> output_plugins_;
    std::vector<IReportPlugin*> report_plugins_;
    std::vector<IStateIOPlugin*> state_io_plugins_;
    std::vector<std::vector<std::string>> init_args_;
    std::unordered_map<void*, std::vector<std::string>> instance_args_;
    std::unordered_map<void*, IPluginComponentInfo*> instance_info_;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    /** @brief Shallow-scan ordinary plugin candidates in one directory.
     * @param dir_path Platform directory spelling; iterator construction uses error_code,
     * but other filesystem/allocation/plugin operations can still throw.
     * @param warn_cb Optional synchronous load warning sink. Candidate regular files
     * use platform suffix matching, skip literal paths already retained and call
     * load_library(). No sort, canonical path normalization or recursive scan is applied. */
    void scan_directory(const std::string& dir_path,
                        std::function<void(const std::string&)> warn_cb);

    /** @brief Register default and enabled GeoPackage/HDF5 metadata as synthetic library entries.
     * Idempotent for existing exact id:version keys. Does not create actual capability
     * instances; builtin info factory methods can deliberately return null. */
    void register_builtin_infos();

    /** @brief Warn about advertised capabilities lacking matching dialog filter roles.
     * @param info Optional borrowed metadata; null is a no-op.
     * @param source_path Source spelling copied into diagnostics.
     * @param warn_cb Optional synchronous warning sink; absent means warnings are dropped
     * (no stderr fallback). A state-IO capability needs at least one read/write role.
     * Does not reject registration or validate filter patterns; plugin/callback exceptions propagate. */
    void validate_filter_invariant(IPluginComponentInfo* info, const std::string& source_path,
                                   std::function<void(const std::string&)> warn_cb);

    /** @brief Recognize a possible direct library path for component lookup.
     * @param str String inspected without trimming; slash/backslash or a platform
     * shared-library suffix qualifies, and empty does not.
     * @return Heuristic result, without filesystem existence/validity checks. */
    static bool is_file_path(const std::string& str);
    /** @brief Check a case-sensitive platform suffix without probing file content.
     * @param filename Filename: Windows .dll, macOS .dylib/.so, other builds .so.
     * @return True for the exact recognized trailing suffix; versioned .so.N and
     * uppercase suffixes are not recognized by this helper. */
    static bool is_shared_library(const std::string& filename);
    /** @brief Locate the module containing this helper using platform loader metadata.
     * @return Its parent directory, or empty on lookup failure/WebAssembly. Path encoding
     * follows platform narrow APIs/filesystem conversion; no canonicalization is promised. */
    static std::string get_library_directory();

    /** @brief Open one platform shared library without registering metadata.
     * @param path NUL-free platform path; Windows uses its narrow loader API.
     * @return Owned loader handle, or null on failure/WebAssembly. Caller releases via
     * platform_unload() or transfers to a retained library/catalogue entry. */
    static void* platform_load(const std::string& path);
    /** @brief Release one loader reference, ignoring platform close failure.
     * @param handle Owned open handle, or null for a no-op. WebAssembly is a no-op.
     * Caller ensures no remaining instance/metadata/code use depends on this reference. */
    static void platform_unload(void* handle) noexcept;
    /** @brief Resolve a symbol address without calling it.
     * @param handle Borrowed loaded library; null returns null.
     * @param sym Required NUL-terminated symbol name for a nonnull native handle.
     * @return Borrowed address or null on failure/WebAssembly; lifetime follows library. */
    static void* platform_sym(void* handle, const char* sym) noexcept;
    /** @brief Copy the current platform loader diagnostic.
     * @return Formatted GetLastError/dlerror text, or fixed unsupported text for WebAssembly.
     * Native error state can be consumed by the underlying API; no operation context is retained. */
    static std::string platform_error() noexcept;

    // D2: the component catalogue's internals. Private statics because they
    // use platform_* above; the catalogue state itself is file-local in
    // PluginFactory.cpp. Callers must hold the catalogue mutex.
    /** @brief Offer an opened non-IO library to the process-global component catalogue.
     * @param path Platform path compared using weak canonicalization.
     * @param handle Owned loader reference transferred only when true is returned.
     * Caller holds the catalogue mutex. Stamped components require exact ABI agreement;
     * unrecognized unstamped libraries are ignored, while the legacy component entry
     * point can be called best-effort and recorded as unstamped. Duplicate successful
     * IDs are refused; matching canonical paths release the redundant loader reference.
     * @return True if recorded/adopted/refused and the handle is now retained or closed;
     * false for noncomponents/no support, leaving ownership with caller. Exceptions can
     * interrupt bookkeeping; this is not an exception-safe ownership wrapper. */
    static bool catalog_adopt_locked(const std::string& path, void* handle);
    /** @brief Scan sorted shallow library candidates into the global catalogue.
     * @param dir Directory spelling; invalid directory yields zero. Caller holds the
     * catalogue mutex. Unloadable/noncomponent files are silently skipped.
     * @return Increase in recorded loaded/refused entries, not successfully instantiated models. */
    static int catalog_scan_locked(const std::string& dir);
    /** @brief Perform default global discovery once under the catalogue mutex.
     * Marks defaults_done before scanning library-relative directories and the platform
     * path-list environment variable. An exception can therefore leave a partial scan
     * marked done. Explicit paths are recorded by their own add operation, not rescanned here. */
    static void catalog_defaults_locked();
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_PLUGIN_FACTORY_HPP */
