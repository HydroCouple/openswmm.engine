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
 * @file CatchmentHandler.hpp
 * @brief Section handlers for subcatchments and rain gages.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_CATCHMENT_HANDLER_HPP
#define OPENSWMM_ENGINE_CATCHMENT_HANDLER_HPP

#include <vector>
#include <string>

namespace openswmm { struct SimulationContext; }

namespace openswmm::input {

/** @brief Parse [SUBCATCHMENTS] into SubcatchData + subcatch_names. */
void handle_subcatchments(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [SUBAREAS] — Manning's n + depression storage for each subcatch. */
void handle_subareas(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [INFILTRATION] — Horton/Green-Ampt/CN params per subcatch. */
void handle_infiltration(SimulationContext& ctx, const std::vector<std::string>& lines);

/** @brief Parse [RAINGAGES] into GageData + gage_names. */
void handle_raingages(SimulationContext& ctx, const std::vector<std::string>& lines);

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_CATCHMENT_HANDLER_HPP */
