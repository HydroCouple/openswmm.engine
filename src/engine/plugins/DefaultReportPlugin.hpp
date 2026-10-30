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
 * @file      DefaultReportPlugin.hpp
 * @brief     Built-in report plugin that writes the legacy SWMM .rpt summary.
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    /** @brief Store a report destination and start in LOADED state.
     * @param rpt_path UTF-8 filename moved into storage; empty disables actual report IO.
     * No file is opened and no path is validated until prepare()/write_summary(). */
    explicit DefaultReportPlugin(std::string rpt_path);
    /** @brief Write an interruption marker, flush and close a report still open at destruction.
     * No summary is generated; fprintf/flush/close errors are ignored. Caller first
     * serializes all active report use and releases borrowed diagnostic pointers. */
    ~DefaultReportPlugin() override;

    /** @brief Read the last assigned lifecycle marker without synchronization.
     * @return Stored state; methods do not generally enforce an incoming-state transition.
     * Serialize plugin lifecycle/IO with active worker calls. */
    PluginState state() const noexcept override { return state_; }

    /** @brief Mark INITIALIZED without opening the report.
     * @param init_args Ignored strings.
     * @param info Ignored metadata pointer.
     * @return Zero, retaining previous diagnostics/counters. */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /** @brief Mark VALIDATED without checking path or model data.
     * @param ctx Ignored context.
     * @return Zero unconditionally. */
    int validate(const SimulationContext& ctx) override;
    /** @brief Open a progressive report, write its preamble and mark PREPARED.
     * @param ctx Stable compatible model/options; rpt_disabled skips opening.
     * A nonempty enabled destination uses truncate mode normally, exclusive creation
     * after requireFreshOutput(). Call only once before final close; no guard closes
     * an already open stream on repeated prepare.
     * @return -1 plus ERROR/last_error only for failed exclusive managed creation;
     * ordinary open failure still returns zero and marks PREPARED with no file.
     * Preamble/flush errors are not checked; exceptions propagate. */
    int prepare(const SimulationContext& ctx) override;
    /** @brief Mark UPDATING without writing per-step report content.
     * @param snapshot Ignored immutable packet.
     * @return Zero; report summaries use SimulationContext later. */
    int update(const SimulationSnapshot& snapshot) override;
    /** @brief Append outstanding errors/warnings, flush and mark FINALIZED.
     * @param ctx Stable model diagnostics; errors are capped at one hundred plus
     * a maximum-count notice, warnings are not capped.
     * @return Zero even with no open file or unchecked fprintf/flush failure. Stream
     * remains open for write_summary(); this is not the final file-close operation. */
    int finalize(const SimulationContext& ctx) override;

    /** @brief Write result/snow/timing sections and close the report stream.
     * @param ctx Stable aligned model/statistics/options. Uses progressive preamble
     * when prepared; otherwise opens/truncates (or creates exclusively) and writes the
     * whole report including diagnostics. Run finalize() first to flush new diagnostics
     * into an already open progressive stream.
     * @return Zero for empty/disabled report or after writing/closing, -1 for fallback
     * open failure. Does not detect later write/close errors, set last_error for that
     * fallback failure, or change lifecycle state. Repeated calls after close can
     * rewrite the report (or fail exclusive creation). Exceptions propagate. */
    int write_summary(const SimulationContext& ctx) override;
    // Managed sessions create a new report and never truncate a late collision.
    /** @brief Require exclusive file creation for managed-session report paths.
     * Sets a retained flag only; existing streams are not reopened and no IO occurs. */
    void requireFreshOutput() noexcept { checkpoint_fresh_ = true; }

    /** @brief Borrow the stored diagnostic text without clearing it.
     * @return NUL-terminated member bytes, possibly empty/stale; valid until the next
     * error mutation or destruction. Never free; use the method return code for success. */
    const char* last_error_message() const noexcept override { return last_error_.c_str(); }

private:
    std::string rpt_path_;
    PluginState state_ = PluginState::UNLOADED;
    std::string last_error_;
    bool checkpoint_fresh_ = false;

    // Progressive write state
    std::FILE* file_ = nullptr;        ///< Persistent file handle (opened in prepare)
    std::size_t warnings_written_ = 0; ///< Warnings already written to file
    std::size_t errors_written_ = 0;   ///< Errors already written to file

    /** @brief Append title, authored model/input summaries and analysis options.
     * @param f Required writable borrowed report stream, not closed here.
     * @param ctx Stable compatible model with aligned name/data arrays and valid options.
     * Prints project display units, formats serial dates and initializes progressive
     * warning/error counters as applicable. fprintf errors are not reported. */
    void write_preamble(std::FILE* f, const SimulationContext& ctx);

    /** @brief Append enabled continuity, subsystem and object statistics report tables.
     * @param f Required writable borrowed report stream, not closed here.
     * @param ctx Stable aligned model/statistics/options. Converts native feet/cfs/ft3
     * statistics through report-specific display factors and respects report/ignore
     * options and available arrays; no solver step or accumulation occurs. Summary
     * rounding/legacy constants can differ from raw API values. fprintf errors are ignored. */
    void write_results(std::FILE* f, const SimulationContext& ctx);

    /** @brief Append wall-clock begin/end and elapsed analysis timing.
     * @param f Required writable borrowed stream, not closed here.
     * @param ctx Supplies wall_start (including input/initialization time); zero uses
     * current end time, resulting in less-than-one-second display. Uses local ctime()
     * text and integer duration formatting; serialize callers relying on shared C
     * time-conversion storage. Write failures are not reported. */
    void write_timing(std::FILE* f, const SimulationContext& ctx);
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_REPORT_PLUGIN_HPP */
