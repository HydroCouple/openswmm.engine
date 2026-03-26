/**
 * @file QualityHandler.hpp
 * @brief Section handlers for water quality input sections.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_QUALITY_HANDLER_HPP
#define OPENSWMM_ENGINE_QUALITY_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [POLLUTANTS] into PollutantData + pollutant_names. */
void handle_pollutants(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [LANDUSES] into LanduseData + landuse_names. */
void handle_landuses(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [COVERAGES] — land use coverage fractions per subcatchment. */
void handle_coverages(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [BUILDUP] — buildup functions per (landuse x pollutant). */
void handle_buildup(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [WASHOFF] — washoff functions per (landuse x pollutant). */
void handle_washoff(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [TREATMENT] — treatment expressions per (node x pollutant). */
void handle_treatment(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [LOADINGS] — initial pollutant buildup on subcatchments. */
void handle_loadings(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_QUALITY_HANDLER_HPP */
