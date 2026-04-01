/**
 * @file TitleHandler.hpp
 * @brief [TITLE] section handler.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_TITLE_HANDLER_HPP
#define OPENSWMM_ENGINE_TITLE_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/**
 * @brief Parse [TITLE] into ctx.title_notes.
 *
 * @details Format:
 * @code
 * [TITLE]
 * ;;Project Title/Notes
 * First line of project title or notes
 * Second line...
 * @endcode
 *
 * Each non-blank, non-comment line is stored verbatim in
 * ctx.title_notes as a separate entry.
 */
void handle_title(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_TITLE_HANDLER_HPP */
