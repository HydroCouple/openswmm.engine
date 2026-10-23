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
 * @file      NameIndex.hpp
 * @brief     O(1) name-to-index lookup for SWMM objects.
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
 * NameIndex replaces that with a hash map so lookup is expected O(1) (hash-collision worst case remains linear).
 *
 * @see Legacy reference: src/solver/project.c — project_findObject()
 * @ingroup engine_data
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    /**
     * @brief Create an empty registry with no deferred-removal scope.
     */
    NameIndex() = default;

    // -----------------------------------------------------------------------
    // Insertion
    // -----------------------------------------------------------------------

    /**
     * @brief Copy a new name and assign the next sequential index.
     *
     * Caller serializes mutation and lookup, and keeps size representable as int. Invalidates
     * vector-backed name views if insertion reallocates.
     * @throws std::invalid_argument When an
     * equivalent name already exists.
     * @throws std::bad_alloc On allocation; insertion is not
     * strongly transactional if the map insert succeeds before the ordered-name append fails.
     *
     * @param[in] name Borrowed name, matched by ASCII-insensitive byte comparison; empty names are
     *     accepted.
     * @return Index equal to the old size().
     */
    int add(const std::string& name) {
        const int idx = try_add(name);
        if (idx < 0) {
            throw std::invalid_argument("NameIndex: duplicate name '" + name + "'");
        }
        return idx;
    }

    /**
     * @brief Copy a new name, returning a duplicate sentinel instead of a duplicate exception.
     *
     * Allocation exceptions still propagate; this function is not noexcept. Map insertion precedes
     * ordered-vector append, so an allocation failure in the latter can leave stores inconsistent.
     * Caller serializes access and keeps size representable as int.
     *
     * @param[in] name Borrowed name, matched by ASCII-insensitive byte comparison; empty names are
     *     accepted.
     * @return New index, or -1 for an equivalent existing name.
     */
    int try_add(const std::string& name) {
        auto [it, inserted] = map_.emplace(name, static_cast<int>(names_.size()));
        if (!inserted)
            return -1;
        names_.push_back(name);
        return it->second;
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    /**
     * @brief Look up a borrowed name without allocating a temporary key.
     *
     * Not valid while a bulk-removal scope has left the hash map stale.
     *
     * @param[in] name Borrowed name, matched by ASCII-insensitive byte comparison; empty names are
     *     accepted.
     * @return Existing zero-based index, or -1 when absent.
     */
    int find(std::string_view name) const noexcept {
        auto it = map_.find(name);
        if (it == map_.end())
            return -1;
        return it->second;
    }

    /**
     * @brief Look up a borrowed name with an optional result.
     *
     * Not valid while bulk removal defers map rebuild.
     *
     * @param[in] name Borrowed name, matched by ASCII-insensitive byte comparison; empty names are
     *     accepted.
     * @return Existing index or std::nullopt when absent.
     */
    std::optional<int> try_find(std::string_view name) const noexcept {
        auto it = map_.find(name);
        if (it == map_.end())
            return std::nullopt;
        return it->second;
    }

    /**
     * @brief Borrow the original spelling of an equivalent registry key.
     *
     * Pointer can be invalidated by name-vector insertion, reserve, rename, erasure or clear. Do not
     * call while a bulk-removal scope has made map indices stale.
     *
     * @param[in] name Borrowed name, matched by ASCII-insensitive byte comparison; empty names are
     *     accepted.
     * @return Pointer into names_, or nullptr for no match; do not free.
     */
    const std::string* canonical(std::string_view name) const noexcept {
        auto it = map_.find(name);
        if (it == map_.end())
            return nullptr;
        return &names_[static_cast<std::size_t>(it->second)];
    }

    /**
     * @brief Borrow a registered name by its ordered index.
     *
     * Works during deferred removal because the ordered vector stays current.
     * @throws
     * std::out_of_range When idx is not an existing row.
     *
     * @param[in] idx Zero-based index in [0,size()); negative values fail the vector bounds check.
     * @return Reference into names_, invalidated by relevant vector mutations or registry destruction.
     */
    const std::string& name_of(int idx) const { return names_.at(static_cast<std::size_t>(idx)); }

    // -----------------------------------------------------------------------
    // Capacity
    // -----------------------------------------------------------------------

    /**
     * @brief Count names in the ordered vector.
     * @return Ordered-name count converted to int; callers keep the count representable.
     */
    int size() const noexcept { return static_cast<int>(names_.size()); }

    /**
     * @brief Test whether the ordered name vector is empty.
     * @return True when no names remain, including during deferred removal.
     */
    bool empty() const noexcept { return names_.empty(); }

    /**
     * @brief Request capacity for the map and ordered name vector.
     *
     * Can rehash the map and reallocate names_, invalidating map iterators/name references. Allocation
     * exceptions propagate.
     *
     * @param[in] n Expected name count; no elements are inserted.
     */
    void reserve(std::size_t n) {
        map_.reserve(n);
        names_.reserve(n);
    }

    /**
     * @brief Erase all names and map entries.
     *
     * Invalidates borrowed names. Does not reset bulk_depth_; callers must still balance any active
     * begin/end scope.
     */
    void clear() noexcept {
        map_.clear();
        names_.clear();
    }

    /**
     * @brief Remove the most recently ordered name without renumbering earlier rows.
     *
     * Empty is a no-op. Caller must not use this while map indices are stale from deferred removal.
     * Copies the tail key before erasing; because this function is noexcept, allocation failure in that
     * copy terminates.
     */
    void pop_back() noexcept {
        if (names_.empty())
            return;
        const std::string tail = names_.back();
        map_.erase(tail);
        names_.pop_back();
    }

    /**
     * @brief Replace one name while retaining its numeric index.
     *
     * Do not call during deferred removal. Borrowed names may be invalidated; no lifecycle validation
     * is performed. This noexcept function can allocate during string/map replacement; allocation
     * failure terminates.
     *
     * @param[in] idx Existing ordered index; invalid values return false without mutation.
     * @param[in] newName Copied replacement token; empty is accepted, case-only respelling allowed,
     *     collision with another row refused.
     * @return True after replacement; false for invalid idx or collision.
     */
    bool rename(int idx, const std::string& newName) noexcept {
        if (idx < 0 || idx >= static_cast<int>(names_.size()))
            return false;
        auto it = map_.find(newName);
        if (it != map_.end() && it->second != idx)
            return false; // duplicate
        map_.erase(names_[static_cast<std::size_t>(idx)]);
        names_[static_cast<std::size_t>(idx)] = newName;
        map_[newName] = idx;
        return true;
    }

    /**
     * @brief Erase an ordered name and renumber following indices.
     *
     * Within a bulk scope erases only names_, leaving the map stale. Outside a scope clears/rebuilds
     * the map. Does not repair any model object references or enforce lifecycle. noexcept means
     * allocation failure during rebuild terminates.
     *
     * @param[in] idx Zero-based ordered index; out-of-range is a no-op.
     */
    void remove_at(int idx) noexcept {
        if (idx < 0 || idx >= static_cast<int>(names_.size()))
            return;
        names_.erase(names_.begin() + idx);
        if (bulk_depth_ > 0)
            return; // map rebuilt once in end_bulk_remove()
        rebuild_map_();
    }

    /**
     * @brief Enter a nestable scope that defers hash-map rebuilding.
     *
     * While any scope is active, only ordered-vector access/removal is valid; do not use find,
     * try_find, canonical, rename, insertion or map-dependent pop_back. Must pair with
     * end_bulk_remove(), including on exceptional paths. Caller serializes access.
     */
    void begin_bulk_remove() noexcept { ++bulk_depth_; }

    /**
     * @brief Close one deferred-removal scope and rebuild on the outermost close.
     *
     * Unbalanced close is ignored. Outermost close rebuilds the entire map; allocation exceptions
     * propagate and can leave a partial map after the nesting depth has returned to zero.
     */
    void end_bulk_remove() {
        if (bulk_depth_ == 0 || --bulk_depth_ > 0)
            return;
        rebuild_map_();
    }

    // -----------------------------------------------------------------------
    // Iteration
    // -----------------------------------------------------------------------

    /**
     * @brief Borrow ordered stored spellings.
     *
     * Ordered access remains current while bulk map rebuilding is deferred.
     * @return Owned vector reference; element references/views require no intervening invalidating
     *     mutation.
     */
    const std::vector<std::string>& names() const noexcept { return names_; }

private:
    /**
     * @brief Rebuild every name-to-index binding from ordered storage.
     *
     * Clears map, reserves capacity and assigns each row in order. Caller ensures unique equivalent
     * names and int-representable size. Allocation exceptions propagate with no rollback of partial map
     * construction.
     */
    void rebuild_map_() {
        map_.clear();
        map_.reserve(names_.size());
        for (int i = 0; i < static_cast<int>(names_.size()); ++i)
            map_[names_[static_cast<std::size_t>(i)]] = i;
    }

    /// name → index; case-insensitive (legacy hash.c parity), original-case keys
    std::unordered_map<std::string, int, CiHash, CiEqual> map_;
    std::vector<std::string> names_; ///< index → name (original spelling)
    int bulk_depth_ = 0;             ///< begin/end_bulk_remove() nesting
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_NAME_INDEX_HPP */
