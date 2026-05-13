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
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_NAME_INDEX_HPP
#define OPENSWMM_ENGINE_NAME_INDEX_HPP

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <stdexcept>
#include <optional>

namespace openswmm {

/**
 * @brief Bidirectional name↔index registry for SWMM objects.
 *
 * @details Maintains both a hash map (name→index) and a vector (index→name)
 *          so that either direction of lookup is O(1).
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
     * @param name  Object name (stored as-is; case sensitivity is caller's
     *              responsibility).
     * @returns     Assigned index (== size() before this call).
     *
     * @throws std::invalid_argument if `name` is already registered.
     */
    int add(const std::string& name) {
        auto [it, inserted] = map_.emplace(name, static_cast<int>(names_.size()));
        if (!inserted) {
            throw std::invalid_argument("NameIndex: duplicate name '" + name + "'");
        }
        names_.push_back(name);
        return it->second;
    }

    // -----------------------------------------------------------------------
    // Lookup
    // -----------------------------------------------------------------------

    /**
     * @brief Look up the index for a name.
     *
     * @param name  Object name.
     * @returns     Index, or -1 if not found.
     */
    int find(std::string_view name) const noexcept {
        auto it = map_.find(std::string(name));
        if (it == map_.end()) return -1;
        return it->second;
    }

    /**
     * @brief Look up the index, returning std::optional.
     */
    std::optional<int> try_find(std::string_view name) const noexcept {
        auto it = map_.find(std::string(name));
        if (it == map_.end()) return std::nullopt;
        return it->second;
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
    std::unordered_map<std::string, int> map_;   ///< name → index
    std::vector<std::string>             names_;  ///< index → name
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_NAME_INDEX_HPP */
