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
 * @file PluginsHandler.cpp
 * @brief [PLUGINS] section handler — parses plugin library specs (Phase 4, R12).
 *
 * Row format:
 * @code
 *   LibraryPath   [arg1  arg2  ...]
 *   ./plugins/hdf5_output.so  file="results.h5"  compress=9
 * @endcode
 *
 * First token = shared library path.
 * Remaining tokens = init_args passed to IOutputPlugin::initialize().
 *
 * @see PluginsHandler.hpp
 * @see src/engine/plugins/PluginFactory.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "PluginsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"

namespace openswmm::input {

void handle_plugins(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        PluginSpec spec;
        spec.path = tok[0];  // First token is the library path

        // Remaining tokens are init_args
        for (std::size_t i = 1; i < tok.size(); ++i) {
            spec.init_args.push_back(tok[i]);
        }

        ctx.plugin_specs.push_back(std::move(spec));
    }
}

} /* namespace openswmm::input */
