/**
 * @file IStateIOPlugin.hpp
 * @brief Interface for state / hot-start IO plugins.
 *
 * @details Implement this interface to support reading and writing simulation
 *          state (also called "hot-start") to and from different file formats
 *          (e.g., the built-in OpenSWMM binary, legacy SWMM5 binary, HDF5,
 *          NetCDF, cloud blob storage).
 *
 *          State plugins are responsible for:
 *          - Reading a persisted simulation state into a SimulationContext
 *            BEFORE simulation starts (engine in INITIALIZED state).
 *          - Writing the current simulation state to a path during a run
 *            (scheduled checkpoints) or once at the end (final snapshot).
 *
 *          **Threading model:** All methods are called from the main thread.
 *
 *          The engine resolves which plugin handles a given path on read by
 *          calling can_read() on each registered state plugin in turn; the
 *          first match wins, with the built-in default plugin acting as
 *          fallback so legacy formats keep working when no external plugin
 *          claims them.
 *
 * @defgroup engine_plugin_state_io State IO Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IInputPlugin.hpp     — for model-data input plugins
 * @see IOutputPlugin.hpp    — for time-series output plugins
 * @see IReportPlugin.hpp    — for summary report plugins
 * @see IPluginComponentInfo.hpp
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ISTATE_IO_PLUGIN_HPP
#define OPENSWMM_ISTATE_IO_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"

namespace openswmm {

struct SimulationContext;
class IPluginComponentInfo;

/**
 * @brief Interface for state / hot-start IO plugins.
 * @ingroup engine_plugin_state_io
 */
class IStateIOPlugin {
public:
    virtual ~IStateIOPlugin() = default;

    /** @brief Query the current plugin state. */
    virtual PluginState state() const noexcept = 0;

    /**
     * @brief Initialize from [PLUGINS] arguments.
     * @param init_args  Tokenized arguments from the [PLUGINS] line.
     * @param info       Back-pointer to IPluginComponentInfo.
     * @returns 0 on success.
     * @post state() == PluginState::INITIALIZED on success.
     */
    virtual int initialize(
        const std::vector<std::string>& init_args,
        const IPluginComponentInfo* info
    ) = 0;

    /**
     * @brief Validate configuration against the loaded model.
     * @param ctx  Simulation context (after input parsing).
     * @returns 0 on success.
     * @post state() == PluginState::VALIDATED on success.
     */
    virtual int validate(const SimulationContext& ctx) = 0;

    /**
     * @brief Cheap format sniff used to dispatch a path to the right plugin.
     *
     * @details Implementations should peek at file extension and/or a small
     *          fixed-size header (magic number) and return true if this plugin
     *          can read the file. Must NOT perform a full parse — the engine
     *          calls this on every registered state plugin in registration
     *          order to pick the first match.
     *
     * @param path  Path to a candidate state file.
     * @returns true if this plugin recognizes the file format.
     */
    virtual bool can_read(const std::string& path) const { return false; }

    /**
     * @brief Read state from a file and apply it to the simulation context.
     *
     * @details Called BEFORE simulation start (engine in INITIALIZED state).
     *          Restores node depths/heads/volumes, link flows/depths/volumes,
     *          subcatchment runoff, and any solver-internal state (infiltration,
     *          groundwater, snowpack) exposed via SimulationContext accessors.
     *
     *          Missing objects (present in the file but not in the model, or
     *          vice versa) are reported via warnings() and do NOT cause a
     *          non-zero return — the caller decides how strict to be.
     *
     * @param path  Path to the state file.
     * @param ctx   Simulation context to populate.
     * @returns 0 on success, non-zero error code on failure.
     */
    virtual int read_state(const std::string& path, SimulationContext& ctx) = 0;

    /**
     * @brief Write the current simulation state to a file.
     *
     * @details Safe to call repeatedly during a run for scheduled checkpoints
     *          and once at the end for the final snapshot. Implementations
     *          should NOT hold the file open between calls — each call
     *          opens, writes, fsyncs, and closes.
     *
     * @param path  Destination path.
     * @param ctx   Simulation context to serialize.
     * @returns 0 on success, non-zero error code on failure.
     */
    virtual int write_state(const std::string& path, const SimulationContext& ctx) = 0;

    /**
     * @brief Non-fatal warnings accumulated during the last call.
     *
     * @details Typical contents: "node 'J3' present in hot-start file but not
     *          in model — skipped". Cleared at the start of each
     *          read_state()/write_state() call.
     *
     * @returns Vector of warning messages (empty by default).
     */
    virtual std::vector<std::string> warnings() const { return {}; }

    /**
     * @brief Finalize and release resources.
     * @param ctx  Simulation context.
     * @returns 0 on success.
     * @post state() == PluginState::FINALIZED on success.
     */
    virtual int finalize(const SimulationContext& ctx) = 0;

    /** @brief Get the last error message. */
    virtual const char* last_error_message() const noexcept { return ""; }
};

} /* namespace openswmm */

#endif /* OPENSWMM_ISTATE_IO_PLUGIN_HPP */
