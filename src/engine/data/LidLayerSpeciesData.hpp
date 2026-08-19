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
 * @file LidLayerSpeciesData.hpp
 * @brief Phase A4 — per-(LID unit, layer, species) transported state.
 *
 * @details Plan §7 A4 asks for a "generic per-layer species block — heat
 *          reuses it", so this carries a species stride from the start.
 *          Water age is species 0; H5's temperature becomes species 1 by
 *          raising `n_species` and writing its own row, with no second
 *          array and no change to the layer indexing.
 *
 *          LID units live in per-TYPE groups (`LIDManager::group(t)`), not
 *          one flat list, so `group_offset` maps (type, unit-within-group)
 *          to the flat unit index this block is keyed on. It is built once
 *          from the manager and asserted against it on every use.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §7 A4
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_LIDLAYERSPECIESDATA_HPP
#define OPENSWMM_ENGINE_DATA_LIDLAYERSPECIESDATA_HPP

#include <cstddef>
#include <vector>

namespace openswmm {

/// A4: layer index within a LID unit. These are exactly the four
/// `LIDGroupSoA` fields that carry water state (`surf_depth`, `pave_depth`,
/// `soil_moist`, `stor_depth`) and exactly the four terms
/// `LIDSolver::storedVolume()` sums. A drainage mat has thickness and void
/// but no depth field of its own — the green roof stores its mat water in
/// `stor_depth` — so it is not a fifth layer here.
enum class LidLayer : int {
    SURFACE  = 0,
    PAVEMENT = 1,
    SOIL     = 2,
    STORAGE  = 3,
    COUNT_   = 4
};

/// Species rows carried on the LID layers. Age is the only one in A4.
enum class LidSpecies : int {
    AGE    = 0,
    COUNT_ = 1
};

/**
 * @brief Per-(unit, layer, species) state for every LID unit in the model.
 *
 * @details Values are whatever the species means — seconds for AGE. The
 *          update is complete-mix per layer: what is held ages, what
 *          arrives mixes in by volume, and what leaves does so at the
 *          layer's own value, which is why only the INFLOW is needed.
 */
struct LidLayerSpeciesState {
    int n_units   = 0;
    int n_species = 0;

    /// [type] flat index of that group's first unit; size = numGroups() + 1,
    /// so `group_offset.back() == n_units`.
    std::vector<int> group_offset;

    /// [(unit * kLayerCount + layer) * n_species + species]
    std::vector<double> value;

    /// [unit * kLayerCount + layer] — the layer's stored water at the END of
    /// the previous step, as a depth per unit area (ft), void-weighted the
    /// way `LIDSolver::storedVolume()` weights it. Kept because the solver
    /// overwrites its depths in place, so the old volume is otherwise gone
    /// by the time the age update runs.
    std::vector<double> vol_prev;

    /// [unit * n_species + species] — what leaves through the underdrain.
    /// Decision (2026-08-18): the drain draws from the STORAGE layer, so
    /// this is the storage row for every type that has one.
    std::vector<double> drain_value;

    /// [unit * n_species + species] — the value of the water arriving at the
    /// unit from its subcatchment (rainfall, captured subarea runoff, and
    /// run-on where the LID occupies the whole subcatchment), flow-weighted
    /// at the point those terms are assembled.
    std::vector<double> inflow_value;

    static constexpr int kLayerCount = static_cast<int>(LidLayer::COUNT_);

    bool active() const noexcept { return n_units > 0 && n_species > 0; }

    std::size_t layer_index(int unit, LidLayer layer, int species) const noexcept {
        return (static_cast<std::size_t>(unit) * kLayerCount +
                static_cast<std::size_t>(layer)) *
                   static_cast<std::size_t>(n_species) +
               static_cast<std::size_t>(species);
    }

    /// @note No defaulted arguments, deliberately. A3's round removed a
    ///       default from `WaterAgeState::resize` because a short call
    ///       compiled and emptied the new arrays at runtime, where nothing
    ///       could observe it — every call site was guarded by a size
    ///       mismatch that repaired itself on the next step. Requiring both
    ///       counts keeps that failure at the compiler.
    void resize(int units, int species, const std::vector<int>& offsets) {
        n_units   = units;
        n_species = species;
        group_offset = offsets;
        const auto n_layers = static_cast<std::size_t>(units) *
                              static_cast<std::size_t>(kLayerCount);
        value.assign(n_layers * static_cast<std::size_t>(species), 0.0);
        vol_prev.assign(n_layers, 0.0);
        drain_value.assign(static_cast<std::size_t>(units) *
                               static_cast<std::size_t>(species), 0.0);
        inflow_value.assign(static_cast<std::size_t>(units) *
                                static_cast<std::size_t>(species), 0.0);
    }

    void clear() { *this = LidLayerSpeciesState{}; }
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_LIDLAYERSPECIESDATA_HPP
