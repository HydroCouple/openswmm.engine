// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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
