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
 * @file UserFlagValuesHandler.cpp
 * @brief [USER_FLAG_VALUES] section handler — implements R28 per-object values.
 *
 * @see UserFlagValuesHandler.hpp
 * @see UserFlags.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "UserFlagValuesHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"
#include "../../core/UserFlags.hpp"

#include "../../core/charconv_compat.hpp"

#include <charconv>

namespace openswmm::input {

void handle_user_flag_values(SimulationContext& ctx,
                             const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.size() < 4) continue;  // ObjectType ObjectName FlagName Value

        const std::string obj_type  = Tokenizer::to_upper(tok[0]);
        const std::string& obj_name = tok[1];              // preserve case
        const std::string flag_name = Tokenizer::to_upper(tok[2]);
        const std::string& raw_val  = tok[3];

        // Determine the type from the schema definition (if registered).
        // If not defined yet, default to STRING and warn.
        UserFlagType type = UserFlagType::STRING;
        bool defined = ctx.user_flags.is_defined(flag_name);
        if (defined) {
            type = ctx.user_flags.get_def(flag_name).type;
        } else {
            if (ctx.warning_code == 0) ctx.warning_code = 103;
        }

        UserFlagValue value;
        switch (type) {
            case UserFlagType::BOOLEAN:
                value = Tokenizer::parse_boolean(raw_val);
                break;

            case UserFlagType::INTEGER: {
                int v = 0;
                std::from_chars(raw_val.data(), raw_val.data() + raw_val.size(), v);
                value = v;
                break;
            }

            case UserFlagType::REAL: {
                double v = 0.0;
                openswmm::from_chars_double(raw_val.data(), raw_val.data() + raw_val.size(), v);
                value = v;
                break;
            }

            case UserFlagType::STRING:
            default:
                value = raw_val;
                break;
        }

        ctx.user_flags.set(obj_type, obj_name, flag_name, std::move(value));
    }
}

} /* namespace openswmm::input */
