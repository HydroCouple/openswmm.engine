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
 * @file HeatLegacy.hpp
 * @brief Phase H1 — the LEGACY (CSTR) engine's temperature transport.
 *
 * @details A single-scalar rerun of the CSTR quality stages for
 *          `__TEMPERATURE__`, in the same order the pollutant arrays take
 *          — structurally the A1b water-age mirror
 *          (`WaterAgeModule/WaterAgeLegacy.cpp`) with three deliberate
 *          differences, each of which is a way a verbatim copy would have
 *          been wrong:
 *
 *          1. **No aging step.** Age grows at 1 s/s in every parcel;
 *             temperature does not grow at all in H1. Heat SOURCES (the
 *             W/m² surface, radiative and sediment fluxes of plan §2)
 *             arrive with H2–H4 and will enter here, at the stage the age
 *             mirror uses for aging.
 *          2. **No zero floor.** The age mirror ends each stage with
 *             `max(value, 0)` because a negative age is meaningless.
 *             Temperatures below 0 °C are ordinary, and flooring them
 *             would silently warm every cold-weather model to freezing.
 *             The bound here is the physical one: a volume-weighted mean
 *             lies between its inputs, so the clamp is two-sided,
 *             `[min(T_old, T_in), max(T_old, T_in)]`.
 *          3. **Initial state is a temperature, not a zero.** An unseeded
 *             network starts at the configured INITIAL_STATE temperature
 *             (default 20 °C), not at 0.
 *
 *          Like the age mirror this runs at the END of
 *          QualitySolver::execute — it reads the fully accumulated
 *          `qual_vol_in` as its mixing denominator — and writes ONLY
 *          `heat_state`, so HEAT_TRANSPORT ON leaves every pollutant and
 *          water-age trajectory bit-identical under LEGACY.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §1, §6 H1
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_HEAT_LEGACY_HPP
#define OPENSWMM_ENGINE_TRANSPORT_HEAT_LEGACY_HPP

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

/// One LEGACY temperature routing step (call at the END of
/// QualitySolver::execute, after qual_vol_in is fully accumulated).
/// No-op when HEAT_TRANSPORT is off.
void routeLegacyHeat(SimulationContext& ctx, double dt);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_HEAT_LEGACY_HPP
