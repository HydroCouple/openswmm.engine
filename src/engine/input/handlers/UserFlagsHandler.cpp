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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
