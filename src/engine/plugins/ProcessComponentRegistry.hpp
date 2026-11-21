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
 * @file      ProcessComponentRegistry.hpp
 * @brief     Registry + config-file delivery for [PROCESS_COMPONENTS]
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
/// Section tags are upper-cased; semicolon comments and trailing space/tab/CR
/// are stripped, while leading content indentation is retained.
struct ComponentConfigSections {
    std::string source_path; ///< resolved absolute/effective path
    std::vector<std::pair<std::string, std::vector<std::string>>> sections;

    /** @brief Borrow the first exact section-tag match.
     * @param upper_tag Already uppercase tag with exact interior spacing; this helper
     * does not normalize it. Repeated sections remain separate.
     * @return Owned line-vector pointer, or null; invalidated when sections is replaced/grows. */
    const std::vector<std::string>* find(const std::string& upper_tag) const {
        for (const auto& s : sections)
            if (s.first == upper_tag)
                return &s.second;
        return nullptr;
    }
};

/** @brief Clear sections, resolve a UTF-8 config path and parse the component dialect.
 * @param path Config filename; relative paths join base_dir when nonempty.
 * @param base_dir Parent-input directory, or empty to use process working directory.
 * Resolution does not force absolute/canonical form.
 * @param[out] out Sections cleared immediately and source_path set to effective
 * UTF-8 spelling, even on failure. Parsing errors can leave a retained prefix.
 * @return Empty on success, otherwise missing/unreadable/section/nesting diagnostic.
 * No recursive component registration is permitted. Stream read failure after
 * opening is not separately checked; filesystem/allocation exceptions propagate. */
std::string read_component_config(const std::string& path, const std::string& base_dir,
                                  ComponentConfigSections& out);

/** @brief Parse component configuration text using the same section/comment rules as files.
 * @param text Borrowed complete text copied into an input stream.
 * @param label Diagnostic source label also stored as out.source_path.
 * @param[out] out Sections cleared before parsing; a rejected document can retain
 * its successfully parsed prefix.
 * @return Empty on accepted syntax, otherwise a fatal section/nesting diagnostic.
 * Semicolon comments are stripped without quote awareness; blank/comment-only
 * lines are skipped, tags uppercased, repeated sections retained and pre-section
 * content rejected. General unknown tags are accepted for component-specific validation. */
std::string parse_component_config_text(const std::string& text, const std::string& label,
                                        ComponentConfigSections& out);

/// Apply hook: consume the parsed config into engine state. Push
/// diagnostics into `errors` (fatal) — never throw.
using ComponentConfigApply =
    std::function<void(SimulationContext& ctx, const ProcessComponentSpec& spec,
                       const ComponentConfigSections& config, std::vector<std::string>& errors)>;

/**
 * @brief Save hook (IO3a): render this component's CURRENT engine state back
 *        to config-file text — the inverse of ::ComponentConfigApply.
 *
 * @details Until this existed, `swmm_model_write` emitted the
 *          `[PROCESS_COMPONENTS]` `config=` PATH and copied the file the model
 *          was read from, but nothing rewrote its CONTENT. So every edit made
 *          through a C API or the GUI — a `[HEAT_SOURCES]` temperature, a
 *          reaction expression — was **silently lost on save**, while a
 *          hand-edit of the same file persisted. `InpWriter`'s own comment
 *          states the intended division ("the component config FILES are each
 *          component's own to write, never ours"); this is the hook that lets
 *          a component honour it.
 *
 * @return  the complete config-file text, or **empty to decline** — an empty
 *          return means "I have nothing to serialize", and the writer falls
 *          back to the IO3 carry-alongside copy. Declining is how a component
 *          that has not implemented saving yet stays correct rather than
 *          truncating its own file to nothing.
 *
 * @note    `ctx` is CONST. A save must not mutate the model it is describing;
 *          that is the property that makes writing twice give the same file.
 */
using ComponentConfigSave =
    std::function<std::string(const SimulationContext& ctx, const ProcessComponentSpec& spec)>;

struct ProcessComponentEntry {
    std::string description;    ///< short human-readable description
    std::string pending_phase;  ///< non-empty ⇒ planned, not yet implemented
    ComponentConfigApply apply; ///< null for planned entries
    /// IO3a. Null until a component implements serialization; the writer then
    /// falls back to copying, so components can adopt this ONE AT A TIME
    /// without any intermediate state losing data.
    ComponentConfigSave save;
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
    /** @brief Access the process-global lazily constructed builtin hook registry.
     * @return Borrowed singleton; caller serializes registration with all lookup/use.
     * Initial construction is thread-safe, subsequent map mutation is not. */
    static ProcessComponentRegistry& instance();

    /** @brief Insert or replace a complete component-hook entry and clear its pending phase.
     * @param id Exact case-sensitive key, copied without empty/duplicate validation.
     * @param description Human-readable description moved into storage.
     * @param apply Required callable for an implemented entry; no nonnull check occurs.
     * Hooks receive mutable engine state and are expected to append diagnostics rather
     * than throw, though this registry installs no exception boundary.
     * @param save Optional serialization hook moved into storage; null declines authored
     * serialization and lets the writer use its file-copy fallback.
     * Register at startup with no concurrent readers; replacement invalidates prior hook assumptions. */
    void register_component(const std::string& id, std::string description,
                            ComponentConfigApply apply, ComponentConfigSave save = nullptr);

    /** @brief Borrow one exact component registry entry.
     * @param id Case-sensitive key; no path or version parsing is applied.
     * @return Owned map-entry pointer, or null. Other map insertions preserve addresses,
     * but replacement changes that entry's contents; serialize registration/use. */
    const ProcessComponentEntry* find(const std::string& id) const;
    /** @brief Copy every registered key, including planned entries without apply hooks.
     * @return Independent lexicographically map-ordered keys; does not load libraries. */
    std::vector<std::string> known_ids() const;

private:
    /** @brief Seed the planned builtin IDs with descriptions/phases and null hooks.
     * Later register_component() replaces planned entries with implemented callbacks. */
    ProcessComponentRegistry();
    std::map<std::string, ProcessComponentEntry> entries_;
};

/** @brief Validate specs, parse each config and apply implemented builtin hooks in order.
 * @param[in,out] ctx Engine state holding process_component_specs and hook targets.
 * Duplicate exact IDs are detected before any hook; then specs are handled in order.
 * Unknown/planned/path-like IDs, absent config and parser errors append diagnostics
 * and continue. Successfully parsed specs retain resolved_config_path before apply.
 * @param base_dir Parent input directory for relative config paths; empty uses cwd.
 * @return Collected errors, empty when no errors were appended. Earlier successful
 * hook mutations are retained after later errors; no rollback or exception catch
 * is provided. A registered implemented entry must have a nonnull apply callable.
 * This resolver still refuses library-path specs independently of the discovery catalogue. */
std::vector<std::string> resolve_process_components(SimulationContext& ctx,
                                                    const std::string& base_dir);

} // namespace openswmm::components

#endif // OPENSWMM_ENGINE_PLUGINS_PROCESS_COMPONENT_REGISTRY_HPP
