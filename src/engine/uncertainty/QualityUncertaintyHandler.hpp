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

} // namespace openswmm::uncertainty

#endif // OPENSWMM_ENGINE_QUALITY_UNCERTAINTY_HANDLER_HPP