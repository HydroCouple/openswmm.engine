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
 * @file ProcessComponentRegistry.hpp
 * @brief Registry + config-file delivery for [PROCESS_COMPONENTS]
 *        (Unified Transport suite D-UT8; phases IO1–IO2).
 *
 * @details Maps component ids to their config-apply hooks. Built-in engine
 *          components (reactions, water age, heat, transport engines,
 *          integrated2d) register here as their plan phases land; the ids
 *          are pre-declared so a registration line for a planned-but-not-
 *          yet-implemented component produces a precise diagnostic instead
 *          of "unknown id". Library-loaded HydroCouple components
 *          (`hydrocouple_component_info()`) arrive with phase HC2 and will
 *          resolve through PluginFactory discovery before falling back to
 *          this table.
 *
 *          Config files are the component's own `.inp`-dialect file
 *          (bracketed sections, `;;` comments), parsed by
 *          read_component_config() with the [2D_MESH_FILE] path rules:
 *          relative paths resolve against the parent .inp's directory, one
 *          level only (a [PROCESS_COMPONENTS] section inside a component
 *          file is an error), parse failures are fatal.
 *
 * @see plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md §2–§3
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_PLUGINS_PROCESS_COMPONENT_REGISTRY_HPP
#define OPENSWMM_ENGINE_PLUGINS_PROCESS_COMPONENT_REGISTRY_HPP

#include <functional>
#include <map>
#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
struct ProcessComponentSpec;
}

namespace openswmm::components {

/// A component config file split into its bracketed sections, in file order.
/// Section tags are upper-cased; lines are verbatim minus blank and
/// comment-only lines.
struct ComponentConfigSections {
    std::string source_path;  ///< resolved absolute/effective path
    std::vector<std::pair<std::string, std::vector<std::string>>> sections;

    /// First section with this tag, or nullptr.
    const std::vector<std::string>* find(const std::string& upper_tag) const {
        for (const auto& s : sections)
            if (s.first == upper_tag) return &s.second;
        return nullptr;
    }
};

/**
 * @brief Parse a component configuration file (phase IO2).
 *
 * @param path      As given in `config="…"`.
 * @param base_dir  Directory of the parent .inp ("" ⇒ cwd) — relative
 *                  paths resolve against it ([2D_MESH_FILE] §3).
 * @param out       [out] cleared and filled.
 * @return          Empty string on success, else a fatal diagnostic
 *                  (missing file, unreadable, nested [PROCESS_COMPONENTS]).
 */
std::string read_component_config(const std::string& path,
                                  const std::string& base_dir,
                                  ComponentConfigSections& out);

/// Apply hook: consume the parsed config into engine state. Push
/// diagnostics into `errors` (fatal) — never throw.
using ComponentConfigApply =
    std::function<void(SimulationContext& ctx,
                       const ProcessComponentSpec& spec,
                       const ComponentConfigSections& config,
                       std::vector<std::string>& errors)>;

struct ProcessComponentEntry {
    std::string description;      ///< short human-readable description
    std::string pending_phase;    ///< non-empty ⇒ planned, not yet implemented
    ComponentConfigApply apply;   ///< null for planned entries
};

/**
 * @brief Process-global id → entry table.
 *
 * @details Planned built-in ids are pre-seeded (see the .cpp); implemented
 *          components overwrite their entry via register_component() at
 *          engine static-init or test setup. Not thread-safe for
 *          registration (register at startup only).
 */
class ProcessComponentRegistry {
public:
    static ProcessComponentRegistry& instance();

    void register_component(const std::string& id, std::string description,
                            ComponentConfigApply apply);

    const ProcessComponentEntry* find(const std::string& id) const;
    std::vector<std::string> known_ids() const;

private:
    ProcessComponentRegistry();
    std::map<std::string, ProcessComponentEntry> entries_;
};

/**
 * @brief Resolve every [PROCESS_COMPONENTS] spec (phase IO1): look up the
 *        id, read its config file, and invoke the apply hook.
 *
 * @param base_dir Directory of the parent .inp for relative config paths.
 * @return Diagnostics; empty ⇒ all specs resolved and applied. The caller
 *         (SWMMEngine::open) decides fatality/leniency, mirroring the
 *         external-2D-mesh handling.
 */
std::vector<std::string> resolve_process_components(SimulationContext& ctx,
                                                    const std::string& base_dir);

}  // namespace openswmm::components

#endif  // OPENSWMM_ENGINE_PLUGINS_PROCESS_COMPONENT_REGISTRY_HPP
