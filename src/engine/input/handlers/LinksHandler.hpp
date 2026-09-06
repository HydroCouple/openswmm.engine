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
 * @file LinksHandler.hpp
 * @brief Section handlers for link types: CONDUITS, PUMPS, ORIFICES, WEIRS, OUTLETS, XSECTIONS, LOSSES, TRANSECTS.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_LINKS_HANDLER_HPP
#define OPENSWMM_ENGINE_LINKS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

void handle_conduits (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_pumps    (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_orifices (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_weirs    (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_outlets  (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_xsections(SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_losses   (SimulationContext& ctx, const std::vector<std::string>& lines);
void handle_transects(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_LINKS_HANDLER_HPP */
