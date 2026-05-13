/**
 * @file HydrologyHandler.hpp
 * @brief Section handlers for hydrology-related input sections.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_HYDROLOGY_HANDLER_HPP
#define OPENSWMM_ENGINE_HYDROLOGY_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [EVAPORATION] into SimulationOptions evap fields. */
void handle_evaporation(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [TEMPERATURE] into SimulationOptions temperature/wind/snow fields. */
void handle_temperature(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [SNOWPACKS] into SnowpackStore + snowpack_names. */
void handle_snowpacks(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [AQUIFERS] into AquiferStore + aquifer_names. */
void handle_aquifers(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [GROUNDWATER] — link subcatchments to aquifers and nodes. */
void handle_groundwater(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [GWF] — custom groundwater flow expressions (stored as raw text). */
void handle_gwf(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [LID_CONTROLS] into LidControlStore + lid_names. */
void handle_lid_controls(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [LID_USAGE] into LidUsageStore. */
void handle_lid_usage(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_HYDROLOGY_HANDLER_HPP */
