/**
 * @file OptionsHandler.hpp
 * @brief [OPTIONS] section handler declaration.
 * @see OptionsHandler.cpp for implementation and key mapping documentation.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_OPTIONS_HANDLER_HPP
#define OPENSWMM_ENGINE_OPTIONS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::input {

/**
 * @brief Parse the [OPTIONS] section into ctx.options.
 *
 * @details Built-in handler registered in SWMMEngine::register_builtin_handlers().
 *          Unknown keys are stored in ctx.options.ext_options (R05).
 *          The CRS key also populates ctx.spatial.crs (R06).
 *
 * @param ctx    Simulation context to populate.
 * @param lines  Non-blank, comment-stripped lines from the [OPTIONS] section.
 */
void handle_options(SimulationContext& ctx,
                    const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_OPTIONS_HANDLER_HPP */
