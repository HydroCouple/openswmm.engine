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
 * @file ProcessComponentsHandler.cpp
 * @brief [PROCESS_COMPONENTS] section handler — parses component
 *        registrations (Unified Transport suite, D-UT8; phase IO1).
 *
 * @see ProcessComponentsHandler.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ProcessComponentsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"

namespace openswmm::input {

namespace {

/// Split `key="value"` / `key=value` (quotes stripped). Returns false when
/// the token carries no '='.
bool split_kv(const std::string& tok, std::string& key, std::string& val) {
    const auto eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    key = tok.substr(0, eq);
    val = tok.substr(eq + 1);
    if (val.size() >= 2 && val.front() == '"' && val.back() == '"')
        val = val.substr(1, val.size() - 2);
    return true;
}

}  // namespace

void handle_process_components(SimulationContext& ctx,
                               const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        ProcessComponentSpec spec;
        spec.id = tok[0];

        for (std::size_t i = 1; i < tok.size(); ++i) {
            std::string key, val;
            if (!split_kv(tok[i], key, val)) {
                ctx.errors.push_back(
                    "[PROCESS_COMPONENTS] '" + spec.id +
                    "': argument '" + tok[i] +
                    "' is not a key=\"value\" pair.");
                continue;
            }
            if (Tokenizer::to_upper(key) == "CONFIG") {
                spec.config_path = val;
            } else {
                spec.args.emplace_back(std::move(key), std::move(val));
            }
        }

        ctx.process_component_specs.push_back(std::move(spec));
    }
}

}  // namespace openswmm::input
