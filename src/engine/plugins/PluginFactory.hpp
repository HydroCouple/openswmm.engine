/**
 * @file PluginFactory.hpp
 * @brief Plugin loader and lifecycle manager (Phase 4, R14).
 *
 * @details PluginFactory owns all dynamically loaded plugin libraries.
 * It is responsible for:
 *
 *  1. **Loading** shared libraries via dlopen/LoadLibrary and resolving
 *     the `openswmm_plugin_info` symbol to get an IPluginComponentInfo*.
 *
 *  2. **Instantiating** IOutputPlugin and/or IReportPlugin via the factory
 *     methods on IPluginComponentInfo.
 *
 *  3. **Lifecycle dispatch** — calling initialize(), validate(), prepare(),
 *     finalize(), and (for IReportPlugin) write_summary() on all loaded
 *     plugin instances in registration order.
 *
 *  4. **Cleanup** — dlclose() on all library handles at destruction.
 *
 * ### Threading
 *
 * All lifecycle methods except update() run on the main simulation thread.
 * update() is called from the IO thread (via IOThread). PluginFactory does
 * NOT directly call update() — that is done by the IOThread.
 *
 * ### Error handling
 *
 * If a plugin's lifecycle method returns non-zero, the plugin is transitioned
 * to PluginState::ERROR and a warning is emitted. Non-error plugins continue.
 * If ALL plugins error, the factory returns the last error code.
 *
 * @see IPluginComponentInfo.hpp
 * @see IOutputPlugin.hpp
 * @see IReportPlugin.hpp
 * @see src/engine/output/IOThread.hpp
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 4
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
#include <functional>

// Forward declarations — avoid pulling in full plugin SDK in every TU
namespace openswmm {
    class IPluginComponentInfo;
    class IOutputPlugin;
    class IReportPlugin;
    struct SimulationContext;
    struct SimulationSnapshot;
    struct PluginSpec;
}

namespace openswmm {

/**
 * @brief Manages all dynamically loaded plugins for one engine instance.
 *
 * @details One PluginFactory per SWMMEngine instance (NOT a singleton).
 *          This allows multiple concurrent engine instances (e.g. Monte Carlo
 *          runs) to each have their own independent plugin set.
 *
 * @ingroup engine_plugins
 */
class PluginFactory {
public:
    PluginFactory() = default;
    ~PluginFactory();

    // Non-copyable, movable
    PluginFactory(const PluginFactory&) = delete;
    PluginFactory& operator=(const PluginFactory&) = delete;
    PluginFactory(PluginFactory&&) noexcept = default;
    PluginFactory& operator=(PluginFactory&&) noexcept = default;

    // -----------------------------------------------------------------------
    // Loading
    // -----------------------------------------------------------------------

    /**
     * @brief Load all plugins listed in the given specs.
     *
     * @details For each spec: dlopen(spec.path), resolve
     *          `openswmm_plugin_info`, call create_output_plugin() /
     *          create_report_plugin(). Specs with load errors are skipped
     *          with a warning; they do not abort the whole load.
     *
     * @param specs       Plugin specs parsed from [PLUGINS].
     * @param warn_cb     Optional callback to report per-plugin load errors.
     *                    Signature: void(const std::string& message).
     * @returns Number of successfully loaded plugins.
     */
    int load_plugins(
        const std::vector<PluginSpec>& specs,
        std::function<void(const std::string&)> warn_cb = {}
    );

    // -----------------------------------------------------------------------
    // Lifecycle dispatch (main thread — call in order)
    // -----------------------------------------------------------------------

    /**
     * @brief Call initialize() on all output and report plugins.
     * @param ctx  Simulation context (for metadata).
     * @returns 0 on success; last non-zero error code if any plugin failed.
     */
    int initialize_all(SimulationContext& ctx);

    /**
     * @brief Call validate() on all plugins.
     */
    int validate_all(SimulationContext& ctx);

    /**
     * @brief Call prepare() on all plugins (open output files, write headers).
     */
    int prepare_all(SimulationContext& ctx);

    /**
     * @brief Deliver a snapshot to all output AND report plugins.
     *
     * @details Called from the IO thread at each output boundary.
     *          Only plugins in PREPARED or UPDATING state receive the update.
     *
     * @param snapshot  Read-only simulation state snapshot.
     * @returns 0 on success; last non-zero error code if any plugin failed.
     */
    int update_all(const SimulationSnapshot& snapshot);

    /**
     * @brief Call finalize() on all plugins.
     *
     * @details Called from the main thread after the IO thread has been joined.
     */
    int finalize_all(SimulationContext& ctx);

    /**
     * @brief Call write_summary() on all report plugins.
     *
     * @details Called after finalize_all(), during SWMMEngine::report().
     */
    int write_summary_all(SimulationContext& ctx);

    // -----------------------------------------------------------------------
    // Introspection
    // -----------------------------------------------------------------------

    /** @brief All loaded output plugin instances. */
    const std::vector<IOutputPlugin*>& output_plugins() const noexcept {
        return output_plugins_;
    }

    /** @brief All loaded report plugin instances. */
    const std::vector<IReportPlugin*>& report_plugins() const noexcept {
        return report_plugins_;
    }

    /** @brief Total number of loaded plugins (output + report). */
    int plugin_count() const noexcept {
        return static_cast<int>(output_plugins_.size() + report_plugins_.size());
    }

    /** @brief True if no plugins have been loaded. */
    bool empty() const noexcept { return plugin_count() == 0; }

    /** @brief Unload all plugins and close library handles. */
    void unload_all();

    /**
     * @brief Inject a pre-created output plugin instance (takes ownership).
     *
     * @details Used by SWMMEngine to register the DefaultOutputPlugin without
     *          going through the [PLUGINS] / dlopen path.
     * @param plugin  Heap-allocated instance; PluginFactory will delete it.
     * @param args    Init args (empty for built-in plugins).
     */
    void add_output_plugin(IOutputPlugin* plugin,
                           std::vector<std::string> args = {});

    /**
     * @brief Inject a pre-created report plugin instance (takes ownership).
     */
    void add_report_plugin(IReportPlugin* plugin);

private:
    // -----------------------------------------------------------------------
    // Internal library handle record
    // -----------------------------------------------------------------------

    struct LibEntry {
        void*                  handle = nullptr;  ///< dlopen / LoadLibrary handle
        IPluginComponentInfo*  info   = nullptr;  ///< Component info (not owned — lives in lib)
        std::string            path;              ///< Original library path (for error messages)
    };

    std::vector<LibEntry>        libs_;            ///< One per loaded shared library
    std::vector<IOutputPlugin*>  output_plugins_;  ///< Owned instances
    std::vector<IReportPlugin*>  report_plugins_;  ///< Owned instances
    std::vector<std::vector<std::string>> init_args_; ///< init_args[i] matches output_plugins_[i]

    // -----------------------------------------------------------------------
    // Platform-specific helpers
    // -----------------------------------------------------------------------

    static void* platform_load(const std::string& path);
    static void  platform_unload(void* handle) noexcept;
    static void* platform_sym(void* handle, const char* sym) noexcept;
    static std::string platform_error() noexcept;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_PLUGIN_FACTORY_HPP */
