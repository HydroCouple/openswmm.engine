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
 * @file UserFlagsHandler.cpp
 * @brief [USER_FLAGS] section handler — parses flag schema definitions (R28).
 *
 * Each row defines one flag name, its value type, and an optional description.
 * No per-object values are stored here; those come from [USER_FLAG_VALUES].
 *
 * Row format (whitespace or comma delimited):
 * @code
 *   Name          Type     [Description]
 *   INSPECTED     BOOLEAN  "Has the object been field-inspected?"
 *   PRIORITY      INTEGER  "Maintenance priority"
 *   ROUGHNESS_ADJ REAL     "Site-specific roughness multiplier"
 *   ASSET_ID      STRING   "External AM system ID"
 * @endcode
 *
 * @see UserFlags.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "UserFlagsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UserFlags.hpp"

namespace openswmm::input {

void handle_user_flags(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 2) continue;  // Need at least Name + Type

        UserFlagDef def;
        def.name        = Tokenizer::to_upper(tok[0]);
        const std::string type_str = Tokenizer::to_upper(tok[1]);

        if      (type_str == "BOOLEAN") def.type = UserFlagType::BOOLEAN;
        else if (type_str == "INTEGER") def.type = UserFlagType::INTEGER;
        else if (type_str == "REAL")    def.type = UserFlagType::REAL;
        else if (type_str == "STRING")  def.type = UserFlagType::STRING;
        else {
            // Unknown type — default to STRING and emit a warning
            def.type = UserFlagType::STRING;
            if (ctx.warning_code == 0) ctx.warning_code = 102;
        }

        // Optional description in tok[2] (already unquoted by tokenizer)
        if (tok.size() > 2) {
            def.description = tok[2];
        }

        ctx.user_flags.define(std::move(def));
    }
}

} /* namespace openswmm::input */
