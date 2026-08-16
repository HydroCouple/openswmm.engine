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
 * @file SpeciesRegistry.hpp
 * @brief Single source of truth for all transported constituents
 *        (Unified Transport master plan §4.1; phase T0a).
 *
 * @details Legacy `[POLLUTANTS]` entries occupy the first slots with kind
 *          POLLUTANT (indices align 1:1 with the legacy pollutant index, so
 *          every existing `[link*np+p]` array stays valid). MSX species
 *          (reactions component, phase R1) append with MSX_BULK/MSX_WALL.
 *          Reserved species (water age A1, temperature H1) append with
 *          their reserved kinds when those phases land.
 *
 *          Engine consumption note (recorded honestly): as of T0a the
 *          transport engines still size their state from
 *          `n_pollutants()` — the registry's `transported_count()` equals
 *          the pollutant count until MSX species transport lands with
 *          R6/E4, at which point the engines re-point here and the E1
 *          shim is fully retired.
 *
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_SPECIES_REGISTRY_HPP
#define OPENSWMM_ENGINE_DATA_SPECIES_REGISTRY_HPP

#include <string>
#include <string_view>
#include <vector>

namespace openswmm {

/// Constituent kinds (master plan §4.1).
enum class SpeciesKind : int {
    POLLUTANT            = 0,  ///< legacy [POLLUTANTS] entry
    RESERVED_AGE         = 1,  ///< __WATER_AGE__ (phase A1)
    RESERVED_TEMPERATURE = 2,  ///< __TEMPERATURE__ (phase H1)
    MSX_BULK             = 3,  ///< reactions component BULK species (R1)
    MSX_WALL             = 4   ///< reactions component WALL species (R1)
};

class SpeciesRegistry {
public:
    void clear() {
        name_.clear();
        kind_.clear();
        units_.clear();
        n_pollutants_ = 0;
    }

    /// Append a species. Returns the new index, or -1 on a name collision
    /// (names are unique across ALL kinds — an MSX species may not shadow a
    /// pollutant).
    int add(std::string name, SpeciesKind kind, std::string units) {
        if (find(name) >= 0) return -1;
        name_.push_back(std::move(name));
        kind_.push_back(kind);
        units_.push_back(std::move(units));
        if (kind == SpeciesKind::POLLUTANT) ++n_pollutants_;
        return static_cast<int>(name_.size()) - 1;
    }

    int find(std::string_view name) const {
        for (std::size_t i = 0; i < name_.size(); ++i)
            if (name_[i] == name) return static_cast<int>(i);
        return -1;
    }

    int count() const noexcept { return static_cast<int>(name_.size()); }
    int pollutant_count() const noexcept { return n_pollutants_; }

    /// Species the transport engines carry. Until R6/E4 this equals the
    /// pollutant count (see the header note).
    int transported_count() const noexcept { return n_pollutants_; }

    const std::string& name(int i) const { return name_[static_cast<std::size_t>(i)]; }
    SpeciesKind kind(int i) const { return kind_[static_cast<std::size_t>(i)]; }
    const std::string& units(int i) const { return units_[static_cast<std::size_t>(i)]; }

private:
    std::vector<std::string> name_;
    std::vector<SpeciesKind> kind_;
    std::vector<std::string> units_;
    int n_pollutants_ = 0;
};

}  // namespace openswmm

#endif  // OPENSWMM_ENGINE_DATA_SPECIES_REGISTRY_HPP
