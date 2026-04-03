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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
        bool                   has_input  = false;
        bool                   has_output = false;
        bool                   has_report = false;
        IPluginComponentInfo*  info = nullptr;
    };
    std::vector<ComponentEntry> discovered_components() const;

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

    const std::vector<IInputPlugin*>&  input_plugins()  const noexcept { return input_plugins_; }
    const std::vector<IOutputPlugin*>& output_plugins() const noexcept { return output_plugins_; }
    const std::vector<IReportPlugin*>& report_plugins() const noexcept { return report_plugins_; }

    int plugin_count() const noexcept {
        return static_cast<int>(input_plugins_.size() + output_plugins_.size() + report_plugins_.size());
    }

    bool empty() const noexcept { return plugin_count() == 0; }

    void unload_all();

    // -----------------------------------------------------------------------
    // Plugin injection (for built-in / programmatic plugins)
    // -----------------------------------------------------------------------

    void add_output_plugin(IOutputPlugin* plugin, std::vector<std::string> args = {});
    void add_report_plugin(IReportPlugin* plugin);
    void add_input_plugin(IInputPlugin* plugin);

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

    std::vector<LibEntry>        libs_;
    std::vector<IInputPlugin*>   input_plugins_;
    std::vector<IOutputPlugin*>  output_plugins_;
    std::vector<IReportPlugin*>  report_plugins_;
    std::vector<std::vector<std::string>> init_args_;

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------

    void scan_directory(const std::string& dir_path,
                        std::function<void(const std::string&)> warn_cb);

    static bool is_file_path(const std::string& str);
    static bool is_shared_library(const std::string& filename);
    static std::string get_library_directory();

    static void* platform_load(const std::string& path);
    static void  platform_unload(void* handle) noexcept;
    static void* platform_sym(void* handle, const char* sym) noexcept;
    static std::string platform_error() noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_PLUGIN_FACTORY_HPP */
