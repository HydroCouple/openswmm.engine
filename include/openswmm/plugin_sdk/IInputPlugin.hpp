/**
 * @file IInputPlugin.hpp
 * @brief Interface for input file reader/writer plugins.
 *
 * @details Implement this interface to support reading and writing SWMM model
 *          data from/to different file formats (e.g., .inp, GeoJSON, HDF5,
 *          database).
 *
 *          Input plugins are responsible for:
 *          - Reading model data from a file into a SimulationContext
 *          - Writing model data from a SimulationContext to a file
 *
 *          **Threading model:** All methods are called from the main thread.
 *
 * @defgroup engine_plugin_input Input Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IOutputPlugin.hpp    — for time-series output plugins
 * @see IReportPlugin.hpp    — for summary report plugins
 * @see IPluginComponentInfo.hpp
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_IINPUT_PLUGIN_HPP
#define OPENSWMM_IINPUT_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"

namespace openswmm {

struct SimulationContext;
class IPluginComponentInfo;

/**
 * @brief Interface for input file reader/writer plugins.
 * @ingroup engine_plugin_input
 */
class IInputPlugin {
public:
    virtual ~IInputPlugin() = default;

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
     * @brief Read model data from a file into the simulation context.
     *
     * @details Populates all SoA arrays, name indices, options, and tables
     *          in the SimulationContext. The caller (SWMMEngine::open()) is
     *          responsible for calling cross-reference resolution after this
     *          method returns.
     *
     *          Implementations should also populate `ctx.plugin_specs` if the
     *          input format supports plugin declarations, so the engine can
     *          load output/report plugins after reading.
     *
     *          Input plugins are injected into the engine before open() is
     *          called — they cannot be discovered via [PLUGINS] because the
     *          input file has not been read yet at that point.
     *
     * @param path  Path to the input file.
     * @param ctx   Simulation context to populate.
     * @returns 0 on success, non-zero error code on failure.
     */
    virtual int read(const std::string& path, SimulationContext& ctx) = 0;

    /**
     * @brief Write model data from the simulation context to a file.
     *
     * @details Serializes the full model state to the specified file path
     *          in the format supported by this plugin.
     *
     * @param path  Path to the output file.
     * @param ctx   Simulation context to serialize.
     * @returns 0 on success, non-zero error code on failure.
     */
    virtual int write(const std::string& path, const SimulationContext& ctx) = 0;

    /**
     * @brief Sections or elements that were skipped during the last read().
     *
     * @details Returns identifiers for any input sections or elements that
     *          had no handler and were ignored. For .inp files these are
     *          section tags like "MY_UNKNOWN_SECTION". Other formats may
     *          return their own identifiers (e.g., JSON keys).
     *
     * @returns Vector of skipped section/element identifiers (empty by default).
     */
    virtual std::vector<std::string> skipped_sections() const { return {}; }

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

#endif /* OPENSWMM_IINPUT_PLUGIN_HPP */
