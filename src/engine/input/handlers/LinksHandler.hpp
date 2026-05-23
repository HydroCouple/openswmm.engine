/**
 * @file LinksHandler.hpp
 * @brief Section handlers for link types: CONDUITS, PUMPS, ORIFICES, WEIRS, OUTLETS, XSECTIONS, LOSSES, TRANSECTS.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_LINKS_HANDLER_HPP
#define OPENSWMM_ENGINE_LINKS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

void handle_conduits (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_pumps    (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_orifices (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_weirs    (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_outlets  (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_xsections(SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_losses   (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_transects(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_LINKS_HANDLER_HPP */
