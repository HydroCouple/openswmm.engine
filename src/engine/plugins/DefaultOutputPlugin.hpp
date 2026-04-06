/**
 * @file DefaultOutputPlugin.hpp
 * @brief Built-in output plugin that writes the legacy SWMM binary output format.
 *
 * @details DefaultOutputPlugin wraps the legacy `src/output/swmm_output.c` binary
 *          writer via the IOutputPlugin interface. It is automatically registered
 *          by SWMMEngine::start() when save_results != 0 and no [PLUGINS] section
 *          is present (or as an additional plugin alongside user-defined ones).
 *
 *          The binary output file is the standard SWMM 5.x .out format, readable
 *          by EPA SWMM's built-in post-processor and third-party tools.
 *
 * @see IOutputPlugin.hpp
 * @see src/output/swmm_output.c — legacy binary output writer
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 4, R15
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_OUTPUT_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_OUTPUT_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IOutputPlugin.hpp"

#include <string>
#include <atomic>
#include <cstdio>

namespace openswmm {

/**
 * @brief Default output plugin: writes SWMM 5.x compatible binary .out file.
 *
 * @details This plugin is injected automatically when the engine is opened with
 *          a non-empty out_path. It does NOT require a [PLUGINS] entry.
 *
 * @ingroup engine_plugins
 */
class DefaultOutputPlugin final : public IOutputPlugin {
public:
    /**
     * @brief Construct with the output file path.
     * @param out_path  File path for the binary .out file.
     */
    explicit DefaultOutputPlugin(std::string out_path);
    ~DefaultOutputPlugin() override = default;

    // -----------------------------------------------------------------------
    // IOutputPlugin interface
    // -----------------------------------------------------------------------

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;

    int prepare(const SimulationContext& ctx) override;

    int update(const SimulationSnapshot& snapshot) override;

    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return last_error_.c_str();
    }

private:
    std::string         out_path_;
    PluginState         state_      = PluginState::UNLOADED;
    std::string         last_error_;
    int                 step_count_ = 0;

    FILE*               out_file_   = nullptr;
    int                 n_subcatch_ = 0;
    int                 n_nodes_    = 0;
    int                 n_links_    = 0;
    int                 n_polluts_  = 0;
    int                 n_subcatch_vars_ = 0;
    int                 n_node_vars_ = 0;
    int                 n_link_vars_ = 0;
    int                 flow_units_code_ = 0;
    long                id_start_pos_     = 0;
    long                input_start_pos_  = 0;
    long                output_start_pos_ = 0;
    int                 n_periods_  = 0;

    // Unit conversion factors (internal → display), computed once in prepare()
    double ucf_rainfall_  = 1.0;
    double ucf_raindepth_ = 1.0;
    double ucf_evaprate_  = 1.0;
    double ucf_length_    = 1.0;
    double ucf_landarea_  = 1.0;
    double ucf_volume_    = 1.0;
    double ucf_flow_      = 1.0;
    int    unit_system_   = 0;     // 0=US, 1=SI

    static constexpr int MAGIC_NUMBER = 516114522;
    static constexpr int VERSION      = 52001;     // SWMM 5.2.001
    static constexpr int MAX_SYS_RESULTS = 15;

    void writeHeader(const SimulationContext& ctx);
    void writeID(const char* id);
    void writeInt4(int value);
    void writeReal4(float value);
    void writeReal8(double value);
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_OUTPUT_PLUGIN_HPP */
