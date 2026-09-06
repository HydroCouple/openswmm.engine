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
 * @file TitleHandler.cpp
 * @brief [TITLE] section handler implementation.
 *
 * @see TitleHandler.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "TitleHandler.hpp"
#include "../../core/SimulationContext.hpp"

namespace openswmm::input {

void handle_title(SimulationContext& ctx, const std::vector<std::string>& lines) {
    ctx.title_notes.clear();
    ctx.title_notes.reserve(lines.size());
    for (const auto& line : lines) {
        ctx.title_notes.push_back(line);
    }
}

} /* namespace openswmm::input */
