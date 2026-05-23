/**
 * @file ControlsHandler.hpp
 * @brief Section handlers for [CONTROLS] and [REPORT].
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_CONTROLS_HANDLER_HPP
#define OPENSWMM_ENGINE_CONTROLS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

void handle_controls(SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_report  (SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_CONTROLS_HANDLER_HPP */
