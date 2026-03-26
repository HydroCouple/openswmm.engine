/**
 * @file DefaultReportPlugin.hpp
 * @brief Built-in report plugin that writes the legacy SWMM .rpt summary.
 *
 * @details DefaultReportPlugin wraps the legacy `src/solver/report.c` summary
 *          report writer via the IReportPlugin interface. It is automatically
 *          registered by SWMMEngine::start() when a non-empty rpt_path is given.
 *
 * @see IReportPlugin.hpp
 * @see src/solver/report.c — legacy report writer
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 4, R16
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IReportPlugin.hpp"

#include <string>

namespace openswmm {

/**
 * @brief Default report plugin: writes SWMM 5.x compatible .rpt report file.
 * @ingroup engine_plugins
 */
class DefaultReportPlugin final : public IReportPlugin {
public:
    explicit DefaultReportPlugin(std::string rpt_path);
    ~DefaultReportPlugin() override = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate (const SimulationContext& ctx) override;
    int prepare  (const SimulationContext& ctx) override;
    int update   (const SimulationSnapshot& snapshot) override;
    int finalize (const SimulationContext& ctx) override;

    /** @brief Write the summary statistics table to the .rpt file. */
    int write_summary(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return last_error_.c_str();
    }

private:
    std::string rpt_path_;
    PluginState state_      = PluginState::UNLOADED;
    std::string last_error_;
    std::time_t wall_start_ = 0;  ///< Wall-clock time when prepare() was called
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP */
