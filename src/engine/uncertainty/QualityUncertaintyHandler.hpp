/**
 * @file QualityUncertaintyHandler.hpp
 * @brief Input section parser for the QUALITY uncertainty layer.
 *
 * @ingroup engine_uncertainty
 */

#ifndef OPENSWMM_ENGINE_QUALITY_UNCERTAINTY_HANDLER_HPP
#define OPENSWMM_ENGINE_QUALITY_UNCERTAINTY_HANDLER_HPP

#include "UncertaintyConfig.hpp"
#include "../input/SectionRegistry.hpp"

#include <string>
#include <vector>

namespace openswmm { struct SimulationContext; }

namespace openswmm::uncertainty {

/**
 * @brief Parse a single line from the [UNCERTAINTY] section for QUALITY layers.
 *
 * Format: LAYER PARAMETER DISTRIBUTION PERTURBATION
 *
 * This function handles only QUALITY layers.
 *
 * @param tokens  Whitespace-split tokens.
 * @param config  Uncertainty config to append the source spec to.
 * @return Empty string on success, or error description.
 */
std::string parseQualityUncertaintyLine(const std::vector<std::string>& tokens,
                                         UncertaintyConfig& config);

/**
 * @brief Register the UNCERTAINTY input section handler for QUALITY layer.
 *
 * Call during input reader setup regardless of OPENSWMM_HAS_2D.
 * The handler will populate the uncertainty config data in SimulationContext.
 *
 * @param config   Uncertainty config to populate (from [UNCERTAINTY]).
 * @param registry Section registry to register handlers into.
 */
void registerQualityUncertaintySection(UncertaintyConfig& config,
                                       input::SectionRegistry& registry);

/**
 * @brief Parse a single line from [SOFT_RAINFALL_GRID] (SR-2b, design §3.2).
 *
 * Format: Target File Mapping [Options]
 *   Target:   2D | RUNOFF | INFLOWS
 *   File:     quoted path to HDF5 grid file
 *   Mapping:  CENTROID | BILINEAR | AREA_MEAN
 *   Options:  FORCE_LOCATION | NODES <file>
 *
 * @param tokens  Whitespace-split tokens.
 * @param config  Uncertainty config to append the grid source spec to.
 * @return Empty string on success, or error description.
 */
std::string parseSoftRainfallGridLine(const std::vector<std::string>& tokens,
                                       UncertaintyConfig& config);

/**
 * @brief Register the [SOFT_RAINFALL_GRID] input section handler (SR-2b).
 *
 * Call during input reader setup regardless of OPENSWMM_HAS_2D.
 *
 * @param config   Uncertainty config to populate.
 * @param registry Section registry to register handlers into.
 */
void registerSoftRainfallGridSection(UncertaintyConfig& config,
                                     input::SectionRegistry& registry);

// [SOFT_RAINGAGES] (SR-1a, gage-level soft-rainfall input) is not declared
// here: it parses into SimulationContext::soft_rain, which does not exist on
// this base (SR-1b's engine-side state — a separate, still-unstarted
// feature). See QualityUncertaintyHandler.cpp's note at the same spot.

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_QUALITY_UNCERTAINTY_HANDLER_HPP