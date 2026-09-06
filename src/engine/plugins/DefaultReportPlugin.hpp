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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IReportPlugin.hpp"

#include <ctime>
#include <string>

namespace openswmm {

/**
 * @brief Default report plugin: writes SWMM 5.x compatible .rpt report file.
 * @ingroup engine_plugins
 */
class DefaultReportPlugin final : public IReportPlugin {
public:
    explicit DefaultReportPlugin(std::string rpt_path);
    ~DefaultReportPlugin() override;

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

    // Progressive write state
    std::FILE*  file_             = nullptr; ///< Persistent file handle (opened in prepare)
    std::size_t warnings_written_ = 0;      ///< Warnings already written to file
    std::size_t errors_written_   = 0;      ///< Errors already written to file

    /** @brief Write title, input summaries, and analysis options. */
    void write_preamble(std::FILE* f, const SimulationContext& ctx);

    /** @brief Write simulation result sections (continuity, stats, etc.). */
    void write_results(std::FILE* f, const SimulationContext& ctx);

    /** @brief Write analysis timing section. */
    void write_timing(std::FILE* f, const SimulationContext& ctx);
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP */
