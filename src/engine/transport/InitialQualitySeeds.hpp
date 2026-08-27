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
 * @file InitialQualitySeeds.hpp
 * @brief [INITIAL_QUALITY] reserved-species (__WATER_AGE__/__TEMPERATURE__)
 *        helpers for the per-engine seed sites (E-A3).
 *
 * @details Precedence (D-IQ7): hotstart > [INITIAL_QUALITY] per-element >
 *          sidecar INITIAL_STATE GLOBAL. The age appliers no-op when a
 *          hotstart state is loaded so no call site can get the order wrong.
 *          The age/heat state arrays are wiped by resize() at engine-specific
 *          moments, so callers apply these AT their seed site rather than
 *          relying on a write surviving a later resize (D-IQ8).
 *
 *          Units: [INITIAL_QUALITY] __WATER_AGE__ rows are HOURS in the file
 *          (the [WATER_AGE_SOURCES] convention) and SECONDS internally;
 *          __TEMPERATURE__ rows are degC both places.
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_INITIAL_QUALITY_SEEDS_HPP
#define OPENSWMM_ENGINE_INITIAL_QUALITY_SEEDS_HPP

#include "../core/SimulationContext.hpp"
#include "../data/InitialQualityData.hpp"

namespace openswmm::transport {

/**
 * @brief Per-element initial age (SECONDS) for one node/link, falling back
 *        to @p fallback (normally the INITIAL_STATE global) when no
 *        [INITIAL_QUALITY] row targets the element. Later rows win.
 */
inline double initialAgeSecondsFor(const SimulationContext& ctx, bool is_link,
                                   int idx, double fallback) {
    double v = fallback;
    const auto& iq = ctx.initial_quality;
    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if (iq.kind[ur] != InitialQualityData::kKindWaterAge) continue;
        if ((iq.is_link[ur] != 0) != is_link) continue;
        if (iq.elem_idx[ur] != idx) continue;
        v = iq.value[ur] * 3600.0;   // hours -> seconds
    }
    return v;
}

/**
 * @brief Per-element initial temperature (degC) for one node/link, falling
 *        back to @p fallback (normally the INITIAL_STATE global).
 */
inline double initialTempFor(const SimulationContext& ctx, bool is_link,
                             int idx, double fallback) {
    double v = fallback;
    const auto& iq = ctx.initial_quality;
    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if (iq.kind[ur] != InitialQualityData::kKindTemperature) continue;
        if ((iq.is_link[ur] != 0) != is_link) continue;
        if (iq.elem_idx[ur] != idx) continue;
        v = iq.value[ur];
    }
    return v;
}

/**
 * @brief Apply __WATER_AGE__ rows onto water_age_state.node_age/link_age
 *        (hours -> seconds). No-op when a hotstart state is loaded (D-IQ7)
 *        or the arrays are unsized.
 */
inline void applyInitialAgeOverrides(SimulationContext& ctx) {
    auto& ws = ctx.water_age_state;
    if (ws.hotstart_loaded) return;
    const auto& iq = ctx.initial_quality;
    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if (iq.kind[ur] != InitialQualityData::kKindWaterAge) continue;
        const int ei = iq.elem_idx[ur];
        if (ei < 0) continue;
        auto& arr = iq.is_link[ur] ? ws.link_age : ws.node_age;
        if (static_cast<std::size_t>(ei) < arr.size())
            arr[static_cast<std::size_t>(ei)] = iq.value[ur] * 3600.0;
    }
}

/**
 * @brief Apply __TEMPERATURE__ rows onto heat_state.node_temp/link_temp
 *        (degC). No-op on unsized arrays.
 */
inline void applyInitialTempOverrides(SimulationContext& ctx) {
    auto& hs = ctx.heat_state;
    const auto& iq = ctx.initial_quality;
    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if (iq.kind[ur] != InitialQualityData::kKindTemperature) continue;
        const int ei = iq.elem_idx[ur];
        if (ei < 0) continue;
        auto& arr = iq.is_link[ur] ? hs.link_temp : hs.node_temp;
        if (static_cast<std::size_t>(ei) < arr.size())
            arr[static_cast<std::size_t>(ei)] = iq.value[ur];
    }
}

} // namespace openswmm::transport

#endif // OPENSWMM_ENGINE_INITIAL_QUALITY_SEEDS_HPP
