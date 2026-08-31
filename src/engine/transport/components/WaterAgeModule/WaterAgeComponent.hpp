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
 * @file WaterAgeComponent.hpp
 * @brief The `org.hydrocouple.openswmm.waterage` process component — phase
 *        A1a: parse `[WATER_AGE_SOURCES]` (`model.age`) into
 *        SimulationContext::water_age_config.
 *
 * @details The coordinator of the water age plan's packaging note: age
 *          TRANSPORT is embedded in the engines (a reserved species row on
 *          the ARD mesh in A1a); this component only carries the
 *          per-source initial-age table so every engine agrees when
 *          QUALITY_SOLVER flips. `[OPTIONS] WATER_AGE ON` enables tracking
 *          with all-zero source ages even without this component.
 *
 *          A1a scope: GLOBAL rows for all seven sources + NODE overrides
 *          for DWF/EXTERNAL_INFLOW, VALUE (hours) only. TIMESERIES ages,
 *          SUBCATCH/EDGE_BC scopes, snow/GW state toggles are later phases
 *          and refuse with precise deferral errors.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §1–§2, §7 A1
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_COMPONENT_HPP
#define OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_COMPONENT_HPP

#include <string>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// Register `org.hydrocouple.openswmm.waterage` with the process-component
/// registry (idempotent; called from SWMMEngine::open before resolution).
void registerWaterAgeComponent();

/// Render [WATER_AGE_SOURCES] from the live config in FILE units
/// (HOURS, shortest-exact) — the single spelling shared by the
/// ComponentConfigSave hook and swmm_water_age_save (IO3c), the same
/// sharing the reactions component has with serializeReactionSystem.
/// Empty when the component never applied (save DECLINES → the
/// carry-alongside copy runs).
std::string serializeWaterAgeConfig(const SimulationContext& ctx);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_COMPONENT_HPP
