/**
 * @file IReportPlugin.hpp
 * @brief Interface for report-writing plugins.
 *
 * @details Implement this interface to write SWMM summary reports to custom
 *          formats (CSV, Excel, database, etc.).
 *
 *          Report plugins are similar to output plugins but are also called
 *          at the end of the simulation to write summary statistics. The
 *          update() method receives time-series data as output steps progress,
 *          and write_summary() is called once after finalize().
 *
 *          **Threading model:** Same as IOutputPlugin — update() is called
 *          from the IO thread with a read-only SimulationSnapshot.
 *
 * @defgroup engine_plugin_report Report Plugin Interface
 * @ingroup  engine_plugin_sdk
 *
 * @see IOutputPlugin.hpp    — for time-series output plugins
 * @see SimulationSnapshot.hpp
 * @see IPluginComponentInfo.hpp
 * @see Legacy reference: src/solver/report.c — SWMM 5.x report functions
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_IREPORT_PLUGIN_HPP
#define OPENSWMM_IREPORT_PLUGIN_HPP

#include <string>
#include <vector>
#include "PluginState.hpp"
#include "SimulationSnapshot.hpp"

namespace openswmm {

struct SimulationContext;

/**
 * @brief Interface for report-writing plugins.
 * @ingroup engine_plugin_report
 */
class IReportPlugin {
public:
    virtual ~IReportPlugin() = default;

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
        const class IPluginComponentInfo* info
    ) = 0;

    /**
     * @brief Validate configuration against the loaded model.
     * @param ctx  Simulation context (after input parsing).
     * @returns 0 on success.
     * @post state() == PluginState::VALIDATED on success.
     */
    virtual int validate(const SimulationContext& ctx) = 0;

    /**
     * @brief Open report file(s) and write any headers.
     * @param ctx  Simulation context.
     * @returns 0 on success.
     * @post state() == PluginState::PREPARED on success.
     */
    virtual int prepare(const SimulationContext& ctx) = 0;

    /**
     * @brief Accumulate data from one output step. Called from IO thread.
     *
     * @details This is optional — some report plugins only use write_summary()
     *          and do nothing in update(). The default implementation is a no-op.
     *
     * @param snapshot  Read-only simulation state at this output time step.
     * @returns 0 on success.
     */
    virtual int update(const SimulationSnapshot& snapshot) { (void)snapshot; return 0; }

    /**
     * @brief Write the final summary report. Called from main thread.
     *
     * @details Called after all update() calls have completed and finalize()
     *          has returned. Write summary statistics (peak flows, volumes,
     *          mass balance errors, etc.).
     *
     * @param ctx  Final simulation context (contains statistics).
     * @returns 0 on success.
     */
    virtual int write_summary(const SimulationContext& ctx) = 0;

    /**
     * @brief Flush and close report file(s).
     * @param ctx  Final simulation context.
     * @returns 0 on success.
     * @post state() == PluginState::FINALIZED on success.
     */
    virtual int finalize(const SimulationContext& ctx) = 0;

    /** @brief Get the last error message. */
    virtual const char* last_error_message() const noexcept { return ""; }
};

} /* namespace openswmm */

#endif /* OPENSWMM_IREPORT_PLUGIN_HPP */
