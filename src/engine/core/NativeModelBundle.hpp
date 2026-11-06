// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "AtomicOutputFile.hpp"
#include "FileIO.hpp"
#include "SimulationContext.hpp"
#include "../plugins/ProcessComponentRegistry.hpp"
#include <fstream>
#include <map>
#include <iterator>

namespace openswmm::native_model {
inline std::map<std::string, std::string> configurations(const SimulationContext& ctx) {
    std::map<std::string, std::string> result;
    for (const auto& component : ctx.process_component_specs) {
        const auto* entry = components::ProcessComponentRegistry::instance().find(component.id);
        std::string text;
        if (entry && entry->save) text = entry->save(ctx, component);
        if (text.empty() && !component.config_path.empty()) {
            auto path = io::utf8_path(component.resolved_config_path.empty()
                ? component.config_path : component.resolved_config_path);
            if (path.is_relative()) path = io::utf8_path(ctx.inp_file_path).parent_path() / path;
            std::ifstream file(path, std::ios::binary);
            if (!file) throw std::runtime_error("Cannot preserve component configuration: " + io::path_utf8(path));
            text.assign(std::istreambuf_iterator<char>(file), {});
            if (file.bad()) throw std::runtime_error("Cannot finish reading component configuration");
        }
        if (!text.empty()) result[component.id] = std::move(text);
    }
    return result;
}

inline void restoreConfigurations(SimulationContext& ctx,
                                   const std::map<std::string, std::string>& content) {
    if (content.empty()) return;
    auto folder = io::utf8_path(ctx.inp_file_path);
    folder += ".resources";
    folder /= "components";
    std::filesystem::create_directories(folder);
    int index = 0;
    for (auto& component : ctx.process_component_specs) {
        const auto found = content.find(component.id);
        if (found == content.end()) continue;
        const auto path = std::filesystem::absolute(folder / (std::to_string(index++) + ".cfg"));
        io::AtomicOutputFile file(path, true);
        if (!file.stream() || std::fwrite(found->second.data(), 1, found->second.size(), file.stream()) != found->second.size()
            || !file.commit()) throw std::runtime_error("Cannot restore component configuration: " + file.error());
        component.config_path = component.resolved_config_path = io::path_utf8(path);
    }
}
}
