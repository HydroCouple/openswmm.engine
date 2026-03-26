/**
 * @file CatchmentHandler.hpp
 * @brief Section handlers for subcatchments and rain gages.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_CATCHMENT_HANDLER_HPP
#define OPENSWMM_ENGINE_CATCHMENT_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [SUBCATCHMENTS] into SubcatchData + subcatch_names. */
void handle_subcatchments(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [SUBAREAS] — Manning's n + depression storage for each subcatch. */
void handle_subareas(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [INFILTRATION] — Horton/Green-Ampt/CN params per subcatch. */
void handle_infiltration(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [RAINGAGES] into GageData + gage_names. */
void handle_raingages(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_CATCHMENT_HANDLER_HPP */
