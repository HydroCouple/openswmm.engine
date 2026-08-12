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
 * @file NameIndex.hpp
 * @brief O(1) name-to-index lookup for SWMM objects.
 *
 * @details Wraps an unordered_map<string,int> to provide fast lookup of object
 *          indices by name. Used by SimulationContext for nodes, links,
 *          subcatchments, rain gages, tables, pollutants, and user flags.
 *
 * ### Why a separate class?
 *
 * The legacy SWMM engine uses project_findObject() in src/solver/project.c,
 * which does a linear scan through the object name table. For models with
 * hundreds of conduits this becomes noticeable during input parsing.
 *
 * NameIndex replaces that with a hash map so every lookup is O(1).
 *
 * @see Legacy reference: src/solver/project.c — project_findObject()
 * @ingroup engine_data
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_NAME_INDEX_HPP
#define OPENSWMM_ENGINE_NAME_INDEX_HPP

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <optional>

#include "../core/StringCase.hpp"

namespace openswmm {

/**
 * @brief Bidirectional name↔index registry for SWMM objects.
 *
 * @details Maintains both a hash map (name→index) and a vector (index→name)
 *          so that either direction of lookup is O(1).
 *
 *          Lookups are CASE-INSENSITIVE (ASCII fold), matching the legacy
 *          engine's hash table (src/legacy/engine/hash.c UCHAR). Names are
 *          stored with their original spelling — name_of()/names() feed the
 *          .out ID tables, interface files, and the .inp/GeoPackage writers,
 *          which must round-trip the user's spelling byte-identically. Two
 *          names differing only in case are therefore the SAME entry; add()
 *          treats them as duplicates, exactly as legacy does.
 *
 * @ingroup engine_data
 */
class NameIndex {
public:
    NameIndex() = default;

    // -----------------------------------------------------------------------
    // Insertion
    // -----------------------------------------------------------------------

    /**
     * @brief Add a new name and assign the next sequential index.
     *
     * @param name  Object name (stored as-is; matched case-insensitively).
     * @returns     Assigned index (== size() before this call).
     *
     * @throws std::invalid_argument if `name` is already registered (any
     *         case spelling). Callers on user-input paths should prefer
     *         try_add() and report the duplicate themselves.
     */
    int add(const std::string& name) {
        const int idx = try_add(name);
        if (idx < 0) {
            throw std::invalid_argument("NameIndex: duplicate name '" + name + "'");
        }
        return idx;
    }

    /**
     * @brief Non-throwing add.
     *
     * @param name  Object name (stored as-is; matched case-insensitively).
     * @returns     Assigned index, or -1 if `name` (in any case spelling) is
     *              already registered.
     */
    int try_add(const std::string& name) {
        auto [it, inserted] = map_.emplace(name, static_cast<int>(names_.size()));
        if (!inserted) return -1;
        names_.push_back(name);
        return it->second;
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    /**
     * @brief Look up the index for a name (case-insensitive).
     *
     * @param name  Object name.
     * @returns     Index, or -1 if not found.
     */
    int find(std::string_view name) const noexcept {
        auto it = map_.find(name);
        if (it == map_.end()) return -1;
        return it->second;
    }

    /**
     * @brief Look up the index (case-insensitive), returning std::optional.
     */
    std::optional<int> try_find(std::string_view name) const noexcept {
        auto it = map_.find(name);
        if (it == map_.end()) return std::nullopt;
        return it->second;
    }

    /**
     * @brief Stored (original) spelling for a case-insensitive match.
     *
     * @param name  Object name in any case spelling.
     * @returns     Pointer to the registered spelling, or nullptr if the name
     *              is not registered. Used by duplicate-ID diagnostics to
     *              report both spellings.
     */
    const std::string* canonical(std::string_view name) const noexcept {
        auto it = map_.find(name);
        if (it == map_.end()) return nullptr;
        return &names_[static_cast<std::size_t>(it->second)];
    }

    /**
     * @brief Return the name for a given index.
     * @throws std::out_of_range if idx is out of bounds.
     */
    const std::string& name_of(int idx) const {
        return names_.at(static_cast<std::size_t>(idx));
    }

    // -----------------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------------

    /** @brief Number of registered names. */
    int size() const noexcept { return static_cast<int>(names_.size()); }

    /** @brief True if no names are registered. */
    bool empty() const noexcept { return names_.empty(); }

    /**
     * @brief Pre-allocate for a known count (avoids rehash during input).
     */
    void reserve(std::size_t n) {
        map_.reserve(n);
        names_.reserve(n);
    }

    /** @brief Remove all entries. */
    void clear() noexcept {
        map_.clear();
        names_.clear();
    }

    /**
     * @brief Pop the tail entry — the name added most recently.
     * @details Used by the C ABI's `*_pop_last` family to undo a just-added
     *          object without requiring the full renumbering that a
     *          general-purpose `remove_at(idx)` would need. No-op when
     *          empty.
     */
    void pop_back() noexcept {
        if (names_.empty()) return;
        const std::string tail = names_.back();
        map_.erase(tail);
        names_.pop_back();
    }

    /**
     * @brief Rename the entry at `idx` to `newName`.
     *
     * @param idx     Index of the entry to rename.
     * @param newName New name (must not collide, case-insensitively, with a
     *                DIFFERENT entry; a pure case-respelling of the same
     *                entry is allowed).
     * @returns       true on success; false if idx is out of range or newName
     *                is already in use by another entry.
     */
    bool rename(int idx, const std::string& newName) noexcept {
        if (idx < 0 || idx >= static_cast<int>(names_.size())) return false;
        auto it = map_.find(newName);
        if (it != map_.end() && it->second != idx) return false; // duplicate
        map_.erase(names_[static_cast<std::size_t>(idx)]);
        names_[static_cast<std::size_t>(idx)] = newName;
        map_[newName] = idx;
        return true;
    }

    /**
     * @brief Remove the entry at `idx` and rebuild the name→index map.
     *
     * @details All entries at indices > idx are shifted down by one.
     *          The map is rebuilt from scratch — O(n) — which is acceptable
     *          since this is only called in BUILDING or OPENED state.
     *          No-op if idx is out of range.
     */
    void remove_at(int idx) noexcept {
        if (idx < 0 || idx >= static_cast<int>(names_.size())) return;
        names_.erase(names_.begin() + idx);
        map_.clear();
        map_.reserve(names_.size());
        for (int i = 0; i < static_cast<int>(names_.size()); ++i)
            map_[names_[i]] = i;
    }

    // -----------------------------------------------------------------------
    // Iteration
    // -----------------------------------------------------------------------

    /** @brief Read-only access to the ordered name list. */
    const std::vector<std::string>& names() const noexcept { return names_; }

private:
    /// name → index; case-insensitive (legacy hash.c parity), original-case keys
    std::unordered_map<std::string, int, CiHash, CiEqual> map_;
    std::vector<std::string> names_;  ///< index → name (original spelling)
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_NAME_INDEX_HPP */
