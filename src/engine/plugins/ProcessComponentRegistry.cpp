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
 * @file ProcessComponentRegistry.cpp
 * @brief Registry + config-file delivery for [PROCESS_COMPONENTS] — IO1–IO2.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ProcessComponentRegistry.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "../core/SimulationContext.hpp"

namespace openswmm::components {

// ===========================================================================
// Config-file reader (IO2)
// ===========================================================================

namespace {

std::string upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

/// Strip ';'-style comments (the .inp convention) and trailing whitespace.
std::string strip_comment(const std::string& line) {
    const auto sc = line.find(';');
    std::string s = (sc == std::string::npos) ? line : line.substr(0, sc);
    while (!s.empty() &&
           (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.pop_back();
    return s;
}

/// True when the id token is a shared-library path rather than a registry id
/// (reserved for phase HC2 — HydroCouple component discovery).
bool looks_like_library(const std::string& id) {
    if (id.find('/') != std::string::npos ||
        id.find('\\') != std::string::npos)
        return true;
    for (const char* ext : {".so", ".dylib", ".dll"}) {
        const std::string e(ext);
        if (id.size() > e.size() &&
            id.compare(id.size() - e.size(), e.size(), e) == 0)
            return true;
    }
    return false;
}

}  // namespace

std::string parse_component_config_stream(std::istream& in,
                                          const std::string& label,
                                          ComponentConfigSections& out) {
    std::vector<std::string>* current = nullptr;
    std::string raw;
    while (std::getline(in, raw)) {
        const std::string line = strip_comment(raw);
        if (line.empty()) continue;

        // Section header?
        const auto lb = line.find('[');
        if (lb != std::string::npos && line.find(']', lb) != std::string::npos &&
            line.find_first_not_of(" \t") == lb) {
            const auto rb = line.find(']', lb);
            const std::string tag = upper(line.substr(lb + 1, rb - lb - 1));
            if (tag == "PROCESS_COMPONENTS")
                return "Component config '" + label +
                       "' contains a nested [PROCESS_COMPONENTS] section — "
                       "component files cannot register components (no "
                       "recursion; TRANSPORT_IO_PLUGIN_CONFIG_PLAN §3.3).";
            out.sections.emplace_back(tag, std::vector<std::string>{});
            current = &out.sections.back().second;
            continue;
        }
        if (current == nullptr)
            return "Component config '" + label +
                   "' has content before its first [SECTION] header.";
        current->push_back(line);
    }
    return {};
}

std::string parse_component_config_text(const std::string& text,
                                        const std::string& label,
                                        ComponentConfigSections& out) {
    out.sections.clear();
    out.source_path = label;
    std::istringstream in(text);
    return parse_component_config_stream(in, label, out);
}

std::string read_component_config(const std::string& path,
                                  const std::string& base_dir,
                                  ComponentConfigSections& out) {
    out.sections.clear();

    namespace fs = std::filesystem;
    fs::path p(path);
    if (p.is_relative() && !base_dir.empty()) p = fs::path(base_dir) / p;
    out.source_path = p.string();

    std::ifstream in(p);
    if (!in.is_open())
        return "Process component config file not found or unreadable: '" +
               out.source_path + "'.";
    return parse_component_config_stream(in, out.source_path, out);
}

// ===========================================================================
// Registry (IO1)
// ===========================================================================

ProcessComponentRegistry::ProcessComponentRegistry() {
    // Planned built-in ids (TRANSPORT_IO_PLUGIN_CONFIG_PLAN §2). A
    // registration line for one of these before its phase lands produces a
    // precise diagnostic instead of "unknown component id". Entries are
    // OVERWRITTEN by register_component() when the implementation arrives.
    const struct { const char* id; const char* desc; const char* phase; } planned[] = {
        {"org.hydrocouple.openswmm.reactions",
         "Multispecies reaction system (EPANET-MSX conventions)", "R1 (reactions plan)"},
        {"org.hydrocouple.openswmm.transport.ard",
         "Eulerian ARD transport engine configuration", "E5 (Eulerian ARD plan)"},
        {"org.hydrocouple.openswmm.transport.lard",
         "Lagrangian (LARD) quality engine", "T5 (LARD plan)"},
        {"org.hydrocouple.openswmm.heat",
         "1D heat transport flux modules", "H1 (heat plan)"},
        {"org.hydrocouple.openswmm.waterage",
         "Water age tracking coordinator", "A1 (water age plan)"},
        {"org.hydrocouple.openswmm.integrated2d",
         "Unified 2D surface + two-zone groundwater component", "S1/G1 (2D + GW plans)"},
    };
    for (const auto& pl : planned)
        entries_[pl.id] =
            ProcessComponentEntry{pl.desc, pl.phase, nullptr, nullptr};
}

ProcessComponentRegistry& ProcessComponentRegistry::instance() {
    static ProcessComponentRegistry reg;
    return reg;
}

void ProcessComponentRegistry::register_component(const std::string& id,
                                                  std::string description,
                                                  ComponentConfigApply apply,
                                                  ComponentConfigSave save) {
    entries_[id] =
        ProcessComponentEntry{std::move(description), std::string{},
                              std::move(apply), std::move(save)};
}

const ProcessComponentEntry* ProcessComponentRegistry::find(
    const std::string& id) const {
    const auto it = entries_.find(id);
    return (it == entries_.end()) ? nullptr : &it->second;
}

std::vector<std::string> ProcessComponentRegistry::known_ids() const {
    std::vector<std::string> ids;
    ids.reserve(entries_.size());
    for (const auto& e : entries_) ids.push_back(e.first);
    return ids;
}

// ===========================================================================
// Resolution (IO1) — called from SWMMEngine::open after the input read
// ===========================================================================

std::vector<std::string> resolve_process_components(SimulationContext& ctx,
                                                    const std::string& base_dir) {
    std::vector<std::string> errors;
    auto& reg = ProcessComponentRegistry::instance();

    // Duplicate registrations are refused (IO1 validation finding, resolved
    // per the R1 proposal): two rows for one id would run apply() twice with
    // undefined precedence.
    {
        std::vector<std::string> seen;
        for (const auto& spec : ctx.process_component_specs) {
            for (const auto& s0 : seen)
                if (s0 == spec.id) {
                    errors.push_back(
                        "[PROCESS_COMPONENTS] duplicate registration of '" +
                        spec.id + "' — each component id may appear once.");
                    break;
                }
            seen.push_back(spec.id);
        }
        if (!errors.empty()) return errors;
    }

    for (auto& spec : ctx.process_component_specs) {
        if (looks_like_library(spec.id)) {
            errors.push_back(
                "[PROCESS_COMPONENTS] '" + spec.id +
                "': library-loaded HydroCouple components are not available "
                "yet (arrives with plan phase HC2 — "
                "IMPLEMENTATION_ROADMAP.md).");
            continue;
        }
        const ProcessComponentEntry* entry = reg.find(spec.id);
        if (entry == nullptr) {
            std::string msg = "[PROCESS_COMPONENTS] unknown component id '" +
                              spec.id + "'. Known ids:";
            for (const auto& id : reg.known_ids()) msg += " " + id;
            errors.push_back(msg);
            continue;
        }
        if (!entry->pending_phase.empty()) {
            errors.push_back(
                "[PROCESS_COMPONENTS] '" + spec.id + "' (" +
                entry->description +
                ") is recognized but not yet implemented — arrives with "
                "plan phase " + entry->pending_phase + ".");
            continue;
        }
        if (spec.config_path.empty()) {
            errors.push_back(
                "[PROCESS_COMPONENTS] '" + spec.id +
                "': missing required config=\"…\" argument.");
            continue;
        }
        ComponentConfigSections config;
        const std::string err =
            read_component_config(spec.config_path, base_dir, config);
        if (!err.empty()) {
            errors.push_back(err);
            continue;
        }
        // IO3: remember where the config was actually read from so a
        // save-as can copy the file alongside the written .inp.
        spec.resolved_config_path = config.source_path;
        entry->apply(ctx, spec, config, errors);
    }
    return errors;
}

}  // namespace openswmm::components
