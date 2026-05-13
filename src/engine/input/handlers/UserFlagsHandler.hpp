/**
 * @file UserFlagsHandler.hpp
 * @brief [USER_FLAGS] section handler (R28).
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
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
