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
 * @file ProcessComponentsHandler.hpp
 * @brief [PROCESS_COMPONENTS] section handler (Unified Transport suite, D-UT8).
 *
 * Row format (whitespace delimited, [PLUGINS]-style):
 * @code
 * [PROCESS_COMPONENTS]
 * ;;ComponentId / library                 Arguments (key="value")
 * org.hydrocouple.openswmm.reactions      config="model.rxn"
 * ./components/libmycomponent.so          config="custom.cfg" version="1.2"
 * @endcode
 *
 * The first token is the component id (or, reserved for phase HC2, a shared
 * library path). Remaining tokens are key="value" pairs; `config=` names the
 * component's external configuration file, resolved relative to the parent
 * .inp at open() (the [2D_MESH_FILE] path rules). Registration implies
 * enablement; resolution and config delivery happen in
 * SWMMEngine::open() via plugins/ProcessComponentRegistry.
 *
 * @see plans/transport/TRANSPORT_IO_PLUGIN_CONFIG_PLAN.md §2
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_INPUT_PROCESS_COMPONENTS_HANDLER_HPP
#define OPENSWMM_ENGINE_INPUT_PROCESS_COMPONENTS_HANDLER_HPP

#include <string>
#include <vector>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::input {

void handle_process_components(SimulationContext& ctx,
                               const std::vector<std::string>& lines);

}  // namespace openswmm::input

#endif  // OPENSWMM_ENGINE_INPUT_PROCESS_COMPONENTS_HANDLER_HPP
