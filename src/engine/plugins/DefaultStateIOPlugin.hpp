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
 * @file DefaultStateIOPlugin.hpp
 * @brief Built-in state IO plugin — wraps HotStartManager for read/write.
 *
 * @details Provides the default `IStateIOPlugin` implementation. Reads and
 *          writes the OpenSWMM `OPENSWMM_HS_V1` / `V2` binary hot-start format
 *          (and recognises legacy `SWMM5-HOTSTART*` files for read).
 *
 *          Solver-internal state (infiltration, groundwater) flows through
 *          `SimulationContext::state_accessors`, wired by SWMMEngine at
 *          `open()` time. The plugin itself holds no references to engine
 *          subsystems and is fully self-contained.
 *
 * @see IStateIOPlugin.hpp
 * @see HotStartManager.hpp
 * @see SimulationContext::state_accessors
 * @see docs/STATE_IO_PLUGIN_PLAN.md
 * @ingroup engine_plugins
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DEFAULT_STATE_IO_PLUGIN_HPP
#define OPENSWMM_ENGINE_DEFAULT_STATE_IO_PLUGIN_HPP

#include "../../../include/openswmm/plugin_sdk/IStateIOPlugin.hpp"

#include <string>
#include <vector>

namespace openswmm {

/**
 * @brief Default state-IO plugin: OpenSWMM binary + legacy SWMM5 hot-start.
 *
 * @ingroup engine_plugins
 */
class DefaultStateIOPlugin : public IStateIOPlugin {
public:
    DefaultStateIOPlugin() = default;

    PluginState state() const noexcept override { return state_; }

    int initialize(const std::vector<std::string>& init_args,
                   const IPluginComponentInfo* info) override;

    int validate(const SimulationContext& ctx) override;

    bool can_read(const std::string& path) const override;

    int read_state(const std::string& path, SimulationContext& ctx) override;

    int write_state(const std::string& path, const SimulationContext& ctx) override;

    std::vector<std::string> warnings() const override { return warnings_; }

    int finalize(const SimulationContext& ctx) override;

    const char* last_error_message() const noexcept override {
        return last_error_.c_str();
    }

private:
    PluginState                 state_   = PluginState::UNLOADED;
    std::string                 last_error_;
    std::vector<std::string>    warnings_;
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_DEFAULT_STATE_IO_PLUGIN_HPP */
