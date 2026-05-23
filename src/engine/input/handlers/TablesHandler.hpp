/**
 * @file TablesHandler.hpp
 * @brief Section handlers for [TIMESERIES] and [CURVES].
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_TABLES_HANDLER_HPP
#define OPENSWMM_ENGINE_TABLES_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [TIMESERIES] into TableData + table_names. */
void handle_timeseries(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [CURVES] into TableData + table_names. */
void handle_curves(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_TABLES_HANDLER_HPP */
