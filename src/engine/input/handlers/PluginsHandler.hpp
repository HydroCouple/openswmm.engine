/**
 * @file PluginsHandler.hpp
 * @brief [PLUGINS] section handler (Phase 4, R12).
 *
 * @details Parses plugin library paths and per-plugin init arguments.
 *          The parsed specs are stored in SimulationContext and consumed by
 *          PluginFactory during SWMMEngine::open().
 *
 * Row format (whitespace or comma delimited):
 * @code
 * [PLUGINS]
 * ;;Path                              Args
 * ./plugins/hdf5_output.so            file="results.h5" compress=9
 * ./plugins/csv_report.dylib          file="report.csv" delimiter=","
 * @endcode
 *
 * The first token is the shared library path.
 * All remaining tokens are the init_args passed verbatim to
 * IOutputPlugin::initialize() / IReportPlugin::initialize().
 *
 * @see PluginFactory.hpp
 * @see src/engine/plugins/PluginFactory.hpp
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_PLUGINS_HANDLER_HPP
#define OPENSWMM_ENGINE_PLUGINS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/**
 * @brief Parse [PLUGINS] into ctx.plugin_specs.
 */
void handle_plugins(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_PLUGINS_HANDLER_HPP */
