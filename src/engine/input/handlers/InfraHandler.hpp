/**
 * @file InfraHandler.hpp
 * @brief Section handlers for [STREETS], [INLETS], [INLET_USAGE].
 * @see InfraHandler.cpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_INFRA_HANDLER_HPP
#define OPENSWMM_ENGINE_INFRA_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [STREETS] — fills ctx.streets. */
void handle_streets(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [INLETS] — fills ctx.inlets. */
void handle_inlets(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [INLET_USAGE] — fills ctx.inlet_usages. */
void handle_inlet_usage(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [ADJUSTMENTS] — fills climate adjustment arrays. */
void handle_adjustments(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [EVENTS] — fills ctx.events. */
void handle_events(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INFRA_HANDLER_HPP */
