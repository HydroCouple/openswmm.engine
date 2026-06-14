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
 * @license  MIT License
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
