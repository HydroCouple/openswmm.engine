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
 * @file UserFlagsHandler.hpp
 * @brief [USER_FLAGS] section handler (R28).
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_USER_FLAGS_HANDLER_HPP
#define OPENSWMM_ENGINE_USER_FLAGS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/**
 * @brief Parse [USER_FLAGS] into ctx.user_flags.
 *
 * @details Format:
 * @code
 * [USER_FLAGS]
 * ;;Name            Type      Default   Description
 * ENABLE_DEBUG_OUT  BOOLEAN   NO        "Write extra debug output"
 * MAX_ITERATIONS    INTEGER   10        "Override Newton iterations"
 * STABILITY_FACTOR  REAL      1.0       "Global stability multiplier"
 * LABEL_PREFIX      STRING    "SIM"     "Prefix for output labels"
 * @endcode
 */
void handle_user_flags(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_USER_FLAGS_HANDLER_HPP */
