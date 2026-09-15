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
 * @file UserFlagValuesHandler.hpp
 * @brief [USER_FLAG_VALUES] section handler (R28).
 *
 * @details Parses per-object flag value assignments.  Each row binds a
 *          (ObjectType, ObjectName, FlagName) triple to a typed value.
 *          The flag must have been defined in [USER_FLAGS] first; if it is
 *          not, the row is stored anyway (with the raw string value as a
 *          STRING) and a warning is emitted.
 *
 * Row format:
 * @code
 * [USER_FLAG_VALUES]
 * ;;ObjectType  ObjectName   FlagName       Value
 * NODE          J1           INSPECTED      YES
 * NODE          J1           PRIORITY       2
 * LINK          C_MAIN       ROUGHNESS_ADJ  1.05
 * LINK          C_MAIN       ASSET_ID       "AM-00341"
 * SUBCATCHMENT  S_WEST       INSPECTED      NO
 * @endcode
 *
 * @see UserFlags.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_USER_FLAG_VALUES_HANDLER_HPP
#define OPENSWMM_ENGINE_USER_FLAG_VALUES_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/**
 * @brief Parse [USER_FLAG_VALUES] into ctx.user_flags (per-object values).
 */
void handle_user_flag_values(SimulationContext& ctx,
                             const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_USER_FLAG_VALUES_HANDLER_HPP */
