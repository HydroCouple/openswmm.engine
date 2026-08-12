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
 * @file InflowsHandler.hpp
 * @brief Section handlers for patterns, inflows, DWF, and RDII.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_INFLOWS_HANDLER_HPP
#define OPENSWMM_ENGINE_INFLOWS_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [PATTERNS] into PatternData with continuation-line support. */
void handle_patterns(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [INFLOWS] into ExtInflowData. */
void handle_inflows(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [DWF] into DwfData. */
void handle_dwf(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [RDII] into RDIIAssignData. */
void handle_rdii(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [HYDROGRAPHS] into UnitHydData. */
void handle_hydrographs(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [RDII_DECAY] into RDIIDecayData (exponential IA model). */
void handle_rdii_decay(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_INFLOWS_HANDLER_HPP */
