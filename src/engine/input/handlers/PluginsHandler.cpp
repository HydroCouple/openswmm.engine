/**
 * @file PluginsHandler.cpp
 * @brief [PLUGINS] section handler — parses plugin library specs (Phase 4, R12).
 *
 * Row format:
 * @code
 *   LibraryPath   [arg1  arg2  ...]
 *   ./plugins/hdf5_output.so  file="results.h5"  compress=9
 * @endcode
 *
 * First token = shared library path.
 * Remaining tokens = init_args passed to IOutputPlugin::initialize().
 *
 * @see PluginsHandler.hpp
 * @see src/engine/plugins/PluginFactory.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "PluginsHandler.hpp"

#include "../Tokenizer.hpp"
#include "../../core/SimulationContext.hpp"

namespace openswmm::input {

void handle_plugins(SimulationContext& ctx, const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        auto tok = Tokenizer::tokenize(line);
        if (tok.empty()) continue;

        PluginSpec spec;
        spec.path = tok[0];  // First token is the library path

        // Remaining tokens are init_args
        for (std::size_t i = 1; i < tok.size(); ++i) {
            spec.init_args.push_back(tok[i]);
        }

        ctx.plugin_specs.push_back(std::move(spec));
    }
}

} /* namespace openswmm::input */
