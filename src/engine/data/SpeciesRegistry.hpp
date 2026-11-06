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
 * @file      SpeciesRegistry.hpp
 * @brief     Single source of truth for all transported constituents
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
 *          The LEGACY engine also maintains its separate MSX state and routing ledger
 *          in ReactionData; this registry does not allocate that runtime storage.
 *
 * @ingroup engine_data
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_DATA_SPECIES_REGISTRY_HPP
#define OPENSWMM_ENGINE_DATA_SPECIES_REGISTRY_HPP

#include <string>
#include <string_view>
#include <vector>

namespace openswmm {

/// Constituent kinds (master plan §4.1).
enum class SpeciesKind : int {
    POLLUTANT = 0,            ///< legacy [POLLUTANTS] entry
    RESERVED_AGE = 1,         ///< __WATER_AGE__ (phase A1)
    RESERVED_TEMPERATURE = 2, ///< __TEMPERATURE__ (phase H1)
    MSX_BULK = 3,             ///< reactions component BULK species (R1)
    MSX_WALL = 4              ///< reactions component WALL species (R1)
};

class SpeciesRegistry {
public:
    /**
     * @brief Erase all species and reset the pollutant count.
     *
     * Retains vector capacities and invalidates borrowed species strings; does not update any external
     * concentration arrays or ReactionData indexes.
     */
    void clear() {
        name_.clear();
        kind_.clear();
        units_.clear();
        n_pollutants_ = 0;
    }

    /**
     * @brief Append a uniquely named species across all kinds.
     *
     * Does not enforce pollutant-first ordering. Independent vector appends may fail without rollback;
     * caller keeps dependent arrays and registry blocks coherent.
     *
     * @param[in] name Owned name moved into storage if accepted; exact-case collision detection.
     * @param[in] kind Kind stored without enum or ordering validation.
     * @param[in] units Owned display/unit label moved verbatim; not parsed.
     * @return New registry index, or -1 on an exact-name collision with no mutation.
     */
    int add(std::string name, SpeciesKind kind, std::string units) {
        if (find(name) >= 0)
            return -1;
        name_.push_back(std::move(name));
        kind_.push_back(kind);
        units_.push_back(std::move(units));
        if (kind == SpeciesKind::POLLUTANT)
            ++n_pollutants_;
        return static_cast<int>(name_.size()) - 1;
    }

    /**
     * @brief Find the first byte-exact species name.
     *
     * @param[in] name Borrowed untrimmed name; case-sensitive and allocation-free lookup.
     * @return Registry index or -1; linear scan over all kinds.
     */
    int find(std::string_view name) const {
        for (std::size_t i = 0; i < name_.size(); ++i)
            if (name_[i] == name)
                return static_cast<int>(i);
        return -1;
    }

    /**
     * @brief Count registered constituents of all kinds.
     * @return Owned name vector length converted to int.
     */
    int count() const noexcept { return static_cast<int>(name_.size()); }
    /**
     * @brief Count registered entries marked POLLUTANT.
     * @return Maintained pollutant-kind count; does not imply those entries occupy the first slots
     *     unless caller enforces ordering.
     */
    int pollutant_count() const noexcept { return n_pollutants_; }

    /**
     * @brief Count mesh-transported constituent kinds.
     *
     * This kind whitelist reports registry membership, not the active solver's supported configuration
     * or allocated mesh dimensions.
     * @return Number of POLLUTANT, MSX_BULK, RESERVED_AGE and RESERVED_TEMPERATURE entries; excludes
     *     MSX_WALL and unknown kinds.
     */
    int transported_count() const noexcept {
        int n = 0;
        for (const auto k : kind_)
            if (k == SpeciesKind::POLLUTANT || k == SpeciesKind::MSX_BULK ||
                k == SpeciesKind::RESERVED_AGE || k == SpeciesKind::RESERVED_TEMPERATURE)
                ++n;
        return n;
    }

    /**
     * @brief Borrow a species identifier.
     *
     * @param[in] i Existing registry index in [0,count()); unchecked.
     * @return Reference to owned string, invalidated by growth, truncation, clear or destruction.
     */
    const std::string& name(int i) const { return name_[static_cast<std::size_t>(i)]; }
    /**
     * @brief Read a species category.
     *
     * @param[in] i Existing registry index; unchecked.
     * @return Stored SpeciesKind value without normalization.
     */
    SpeciesKind kind(int i) const { return kind_[static_cast<std::size_t>(i)]; }
    /**
     * @brief Borrow a species unit label.
     *
     * @param[in] i Existing registry index; unchecked.
     * @return Reference to owned string, invalidated by registry vector mutation/destruction.
     */
    const std::string& units(int i) const { return units_[static_cast<std::size_t>(i)]; }

    /**
     * @brief Drop registry entries from a specified index onward.
     *
     * Shrinks all columns and recounts pollutant kinds. Preserves retained prefix values but
     * invalidates erased strings/references; does not repair dependent species indexes or state arrays.
     * Used when rebuilding reaction-species blocks.
     *
     * @param[in] n First removed index; negative treated as zero, values >=count() leave storage
     *     unchanged.
     */
    void truncate_to(int n) {
        if (n < 0)
            n = 0;
        if (n >= count())
            return;
        const auto un = static_cast<std::size_t>(n);
        name_.resize(un);
        kind_.resize(un);
        units_.resize(un);
        n_pollutants_ = 0;
        for (const auto k : kind_)
            if (k == SpeciesKind::POLLUTANT)
                ++n_pollutants_;
    }

private:
    std::vector<std::string> name_;
    std::vector<SpeciesKind> kind_;
    std::vector<std::string> units_;
    int n_pollutants_ = 0;
};

} // namespace openswmm

#endif // OPENSWMM_ENGINE_DATA_SPECIES_REGISTRY_HPP
