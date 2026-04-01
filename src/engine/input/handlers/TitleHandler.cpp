/**
 * @file TitleHandler.cpp
 * @brief [TITLE] section handler implementation.
 *
 * @see TitleHandler.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
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
