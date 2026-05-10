/**
 * @file InflowsHandler.hpp
 * @brief Section handlers for patterns, inflows, DWF, and RDII.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_INFLOWS_HANDLER_HPP
#define OPENSWMM_ENGINE_INFLOWS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [PATTERNS] into PatternData with continuation-line support. */
void handle_patterns(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [INFLOWS] into ExtInflowData. */
void handle_inflows(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [DWF] into DwfData. */
void handle_dwf(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [RDII] into RDIIAssignData. */
void handle_rdii(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [HYDROGRAPHS] into UnitHydData. */
void handle_hydrographs(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INFLOWS_HANDLER_HPP */
