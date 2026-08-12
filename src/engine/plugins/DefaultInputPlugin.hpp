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
 * @file DefaultInputPlugin.hpp
 * @brief DefaultInputPlugin — reads/writes SWMM .inp files.
 *
 * @details Wraps the existing InputReader and InpWriter to provide the
 *          IInputPlugin interface for standard SWMM .inp files.
 *
 * @see IInputPlugin.hpp
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
    /**
     * @brief Construct and register all built-in section handlers.
     */
    DefaultInputPlugin();

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;

    int read(const std::string& path, SimulationContext& ctx) override;

    int write(const std::string& path, const SimulationContext& ctx) override;

    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return last_error_.c_str();
    }

    std::vector<std::string> skipped_sections() const override {
        return skipped_sections_;
    }

    /** @brief Access the section registry (for custom section registration). */
    input::SectionRegistry& registry() noexcept { return registry_; }

private:
    /** @brief Register all built-in SWMM .inp section handlers. */
    void register_builtin_handlers();

    input::SectionRegistry   registry_;
    PluginState              state_ = PluginState::UNLOADED;
    std::string              last_error_;
    std::vector<std::string> skipped_sections_;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_INPUT_PLUGIN_HPP */
