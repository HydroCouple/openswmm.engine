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
 * @file      DefaultInputPlugin.hpp
 * @brief     DefaultInputPlugin — reads/writes SWMM .inp files.
 *
 * @details Wraps the existing InputReader and InpWriter to provide the
 *          IInputPlugin interface for standard SWMM .inp files.
 *
 * @see IInputPlugin.hpp
 * @ingroup engine_plugins
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_INPUT_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_INPUT_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IInputPlugin.hpp"
#include "../input/SectionRegistry.hpp"

#include <string>
#include <vector>

namespace openswmm {

/**
 * @brief Default input plugin that reads/writes SWMM .inp files.
 *
 * @details Owns a SectionRegistry populated with all built-in SWMM section
 *          handlers. The registry maps section tags (JUNCTIONS, CONDUITS, etc.)
 *          to handler functions that populate the SimulationContext.
 *
 * @ingroup engine_plugins
 */
class DefaultInputPlugin : public IInputPlugin {
public:
    /** @brief Construct an UNLOADED plugin and register builtin input/annotation handlers.
     * No file is read; allocation/registration exceptions propagate. */
    DefaultInputPlugin();

    /** @brief Read the last assigned lifecycle marker without synchronization.
     * @return Stored state; methods do not generally enforce an incoming-state transition.
     * Serialize plugin lifecycle/IO with active worker calls. */
    PluginState state() const noexcept override { return state_; }

    /** @brief Mark this plugin INITIALIZED without interpreting arguments.
     * @param init_args Ignored initialization strings; not retained.
     * @param info Ignored metadata pointer; may be null.
     * @return Zero, without clearing an earlier diagnostic or skipped-section list. */
    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    /** @brief Mark this plugin VALIDATED without validating a model.
     * @param ctx Ignored context.
     * @return Zero unconditionally; actual model/input validation is performed elsewhere. */
    int validate(const SimulationContext& ctx) override;

    /** @brief Read an input file through the owned section registry.
     * @param path UTF-8 input filename interpreted by InputReader.
     * @param[in,out] ctx Engine state populated by registered handlers; follows
     * InputReader's initialization/error and partial-mutation contract.
     * @return Zero on accepted read, otherwise context error_code or -1 when it is zero.
     * Always copies the reader's skipped-section list; on failure copies error_message
     * to last_error. Success does not clear older last_error or change lifecycle state.
     * Parser/handler/allocation exceptions are not caught by this plugin. */
    int read(const std::string& path, SimulationContext& ctx) override;

    // Parse native container input using the same handlers as ordinary INP files.
    int readText(const std::string& text, SimulationContext& ctx);
    void registerContextSections(SimulationContext& ctx);

    /** @brief Serialize current engine state through InpWriter and its sidecar rules.
     * @param path UTF-8 destination filename; writing can create/replace the input and
     * component/mesh companions according to InpWriter's staged persistence contract.
     * @param ctx Borrowed model; const interface does not permit appending ctx.warnings.
     * @return Exact writeInpFile status. Nonzero stores a path failure plus collected
     * writer warnings in last_error. On zero, warnings are discarded and an old error
     * can remain stored; success does not update lifecycle state. Exceptions propagate. */
    int write(const std::string& path, const SimulationContext& ctx) override;

    /** @brief Mark the plugin FINALIZED without IO or validation.
     * @param ctx Ignored context.
     * @return Zero; retains registry, diagnostics and skipped sections. */
    int finalize(const SimulationContext& ctx) override;

    /** @brief Borrow the stored diagnostic text without clearing it.
     * @return NUL-terminated member bytes, possibly empty/stale; valid until the next
     * error mutation or destruction. Never free; use the method return code for success. */
    const char* last_error_message() const noexcept override { return last_error_.c_str(); }

    /** @brief Copy the last completed InputReader skipped-section list.
     * @return Independent names, replaced after read() returns; no new file scan occurs. */
    std::vector<std::string> skipped_sections() const override { return skipped_sections_; }

    /** @brief Borrow the owned mutable handler registry for caller-managed extensions.
     * @return Registry valid until plugin destruction; do not mutate it during a read
     * or concurrently with registration/dispatch. Changes affect later input parsing. */
    input::SectionRegistry& registry() noexcept { return registry_; }

private:
    /** @brief Register standard engine sections, passthrough GUI annotations and embedded reaction sections.
     * Runs at construction. LABELS/BACKDROP/PROFILE/PROFILES retain authored lines;
     * reaction sections are captured for later component resolution. Registration does
     * not itself parse/validate models. Caller must avoid racing handler dispatch. */
    void register_builtin_handlers();

    input::SectionRegistry registry_;
    PluginState state_ = PluginState::UNLOADED;
    std::string last_error_;
    std::vector<std::string> skipped_sections_;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_INPUT_PLUGIN_HPP */
