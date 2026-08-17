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
 *          Engine consumption note: as of E4/R6 the ARD engine carries
 *          pollutants + MSX_BULK species on its mesh (it sizes from
 *          n_pollutants() + ReactionData::n_species(), refusing WALL);
 *          `transported_count()` counts POLLUTANT + MSX_BULK to match.
 *          The LEGACY engine still transports pollutants only (MSX
 *          transport under LEGACY is plan phase R4b).
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

    /// Species the ARD engine carries on its mesh: POLLUTANT + MSX_BULK
    /// (E4/R6) + RESERVED_AGE (A1a). WALL species are element-local;
    /// RESERVED_TEMPERATURE joins with phase H1.
    int transported_count() const noexcept {
        int n = 0;
        for (const auto k : kind_)
            if (k == SpeciesKind::POLLUTANT || k == SpeciesKind::MSX_BULK ||
                k == SpeciesKind::RESERVED_AGE)
                ++n;
        return n;
    }

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
