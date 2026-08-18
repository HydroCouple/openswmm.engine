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
 * @file HeatComponent.hpp
 * @brief The `org.hydrocouple.openswmm.heat` process component — phase H1:
 *        parse `[HEAT_SOURCES]` (`model.heat`) into
 *        SimulationContext::heat_config.
 *
 * @details Heat TRANSPORT is embedded in the engines (the LEGACY CSTR
 *          mirror in H1); this component carries only the per-source inlet
 *          temperature table, so every engine agrees when QUALITY_SOLVER
 *          flips. `[OPTIONS] HEAT_TRANSPORT ON` enables tracking with
 *          default source temperatures even without this component.
 *
 *          H1 scope: GLOBAL rows for all seven sources + NODE overrides
 *          for DWF/EXTERNAL_INFLOW, VALUE (°C) only. TIMESERIES
 *          temperatures, SUBCATCH/EDGE_BC scopes and the flux modules of
 *          plan §2 are later phases and refuse with precise deferral
 *          errors.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §3, §6 H1
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_HEAT_COMPONENT_HPP
#define OPENSWMM_ENGINE_TRANSPORT_HEAT_COMPONENT_HPP

namespace openswmm::transport {

/// Register `org.hydrocouple.openswmm.heat` with the process-component
/// registry (idempotent; called from SWMMEngine::open before resolution).
void registerHeatComponent();

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_HEAT_COMPONENT_HPP
