/**
 * @file IOutputPlugin.hpp
 * @brief Interface for output-writing plugins.
 *
 * @details Implement this interface to write SWMM simulation results to custom
 *          formats (HDF5, NetCDF, CSV, database, etc.).
 *
 *          **Threading model:** IOutputPlugin::update() is called from the IO
 *          thread, not the main simulation thread. update() receives a
 *          `const SimulationSnapshot&` — a read-only deep copy of the simulation
 *          state at the output time. Do NOT attempt to access the live
 *          SimulationContext from within update().
 *
 *          All other lifecycle methods (initialize, validate, prepare, finalize)
 *          are called from the main thread before/after the simulation loop.
 *
 * @par Lifecycle order (enforced by PluginFactory)
 * @code
 *   initialize()  →  validate()  →  prepare()
 *      →  update() [N times, IO thread]
 *      →  finalize()
 * @endcode
 *
 * @note Return codes: 0 = success; non-zero = error. The PluginFactory
 *       transitions the plugin to ERROR state on non-zero return.
 *
 * @defgroup engine_plugin_output Output Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IReportPlugin.hpp   — for summary-report plugins
 * @see SimulationSnapshot.hpp — read-only snapshot passed to update()
 * @see IPluginComponentInfo.hpp — factory / metadata interface
 * @see Legacy reference: src/solver/output.c — swmm_step() output writing
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_IOUTPUT_PLUGIN_HPP
#define OPENSWMM_IOUTPUT_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"
#include "SimulationSnapshot.hpp"

/* Forward declaration — avoid pulling in the full SimulationContext */
namespace openswmm { struct SimulationContext; }

namespace openswmm {

/**
 * @brief Interface for output-writing plugins.
 * @ingroup engine_plugin_output
 */
class IOutputPlugin {
public:
    virtual ~IOutputPlugin() = default;

    // -----------------------------------------------------------------------
    // State query
    // -----------------------------------------------------------------------

    /**
     * @brief Query the current plugin state.
     * @returns Current PluginState (thread-safe read).
     */
    virtual PluginState state() const noexcept = 0;

    // -----------------------------------------------------------------------
    // Lifecycle methods (main thread)
    // -----------------------------------------------------------------------

    /**
     * @brief Initialize the plugin from command-line-style arguments.
     *
     * @details Called once after the plugin library is loaded. Parse any
     *          configuration from `init_args` (e.g., output file path, format
     *          options). This is the place to open no files — that happens in
     *          prepare(). Use validate() to check config correctness.
     *
     * @param init_args  Tokenized arguments from the [PLUGINS] section line.
     *                   e.g., {"file=results.h5", "compress=9"}
     * @param info       Back-pointer to the IPluginComponentInfo (for metadata).
     * @returns 0 on success; non-zero on error.
     *
     * @post state() == PluginState::INITIALIZED on success.
     * @post state() == PluginState::ERROR on failure.
     */
    virtual int initialize(
        const std::vector<std::string>& init_args,
        const class IPluginComponentInfo* info
    ) = 0;

    /**
     * @brief Validate plugin configuration against the simulation model.
     *
     * @details Called after the input file is fully parsed and the model is
     *          loaded. Check that configured output paths are writable, that
     *          requested object IDs exist, etc.
     *
     * @param ctx  Read-only view of the simulation context (for model metadata).
     * @returns 0 on success; non-zero on error.
     *
     * @post state() == PluginState::VALIDATED on success.
     */
    virtual int validate(const SimulationContext& ctx) = 0;

    /**
     * @brief Open output file(s) and write headers.
     *
     * @details Called after validate() succeeds, just before the simulation
     *          loop begins. Open file handles, allocate write buffers, and
     *          write any file headers.
     *
     * @param ctx  Read-only simulation context.
     * @returns 0 on success; non-zero on error.
     *
     * @post state() == PluginState::PREPARED on success.
     */
    virtual int prepare(const SimulationContext& ctx) = 0;

    /**
     * @brief Write one output snapshot. Called from the IO thread.
     *
     * @details Called at each output time step with a read-only snapshot of the
     *          simulation state. This method runs on the IO thread — it must
     *          NOT access the live SimulationContext.
     *
     * @param snapshot  Read-only simulation state at this output time step.
     * @returns 0 on success; non-zero on error (transitions to ERROR state).
     *
     * @note This may be called concurrently with the main simulation thread
     *       advancing the next timestep. Use only snapshot data.
     */
    virtual int update(const SimulationSnapshot& snapshot) = 0;

    /**
     * @brief Flush and close output file(s). Called from main thread.
     *
     * @details Called after the simulation loop ends and the IO thread has
     *          been joined. All update() calls have completed at this point.
     *          Flush any write buffers and close file handles.
     *
     * @param ctx  Read-only simulation context (for final state / metadata).
     * @returns 0 on success; non-zero on error.
     *
     * @post state() == PluginState::FINALIZED on success.
     */
    virtual int finalize(const SimulationContext& ctx) = 0;

    // -----------------------------------------------------------------------
    // Optional: error message
    // -----------------------------------------------------------------------

    /**
     * @brief Get the last error message from this plugin instance.
     * @returns Null-terminated error string, or empty string if no error.
     */
    virtual const char* last_error_message() const noexcept { return ""; }
};

} /* namespace openswmm */

#endif /* OPENSWMM_IOUTPUT_PLUGIN_HPP */
