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
 * @file      UserFlags.hpp
 * @brief     User-defined model flags (InfoWorks ICM-style, two-section design).
 *
 * @details User flags are a two-section feature:
 *
 *  1. **[USER_FLAGS]** — defines the flag *schema* (name, type, description).
 *     Each entry declares that a flag exists and what type of value it holds.
 *     This is analogous to defining a custom attribute category.
 *
 *  2. **[USER_FLAG_VALUES]** — assigns concrete flag values to specific objects
 *     identified by (object type, object name, flag name).
 *
 * Input file syntax:
 * @code
 * [USER_FLAGS]
 * ;;Name          Type     Description
 * INSPECTED       BOOLEAN  "Has the object been field-inspected?"
 * PRIORITY        INTEGER  "Maintenance priority (1 = highest)"
 * ROUGHNESS_ADJ   REAL     "Site-specific roughness multiplier"
 * ASSET_ID        STRING   "External asset-management system ID"
 *
 * [USER_FLAG_VALUES]
 * ;;ObjectType  ObjectName   FlagName       Value
 * NODE          J1           INSPECTED      YES
 * NODE          J1           PRIORITY       2
 * LINK          C_MAIN       ROUGHNESS_ADJ  1.05
 * LINK          C_MAIN       ASSET_ID       "AM-00341"
 * SUBCATCHMENT  S_WEST       INSPECTED      NO
 * @endcode
 *
 * @see Legacy reference: InfoWorks ICM user-defined flags concept.
 *      No equivalent in legacy SWMM 5.x.
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md R28
 *
 * @ingroup engine_core
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_USER_FLAGS_HPP
#define OPENSWMM_ENGINE_USER_FLAGS_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
#include <optional>
#include <stdexcept>

#include "StringCase.hpp"

namespace openswmm {

// ============================================================================
// Type tag
// ============================================================================

/**
 * @brief Value type for a user flag.
 * @ingroup engine_core
 */
enum class UserFlagType : int {
    BOOLEAN = 0, ///< YES/NO/TRUE/FALSE/1/0
    INTEGER = 1, ///< Signed integer
    REAL = 2,    ///< Double-precision floating-point
    STRING = 3   ///< Arbitrary string
};

// ============================================================================
// Schema definition
// ============================================================================

/**
 * @brief Schema entry for a single user-defined flag.
 *
 * @details Populated from [USER_FLAGS].  No runtime value is stored here —
 *          values are stored separately keyed by (object type, object name,
 *          flag name) in UserFlags::values_.
 *
 * @ingroup engine_core
 */
struct UserFlagDef {
    std::string name;        ///< Flag identifier (uppercase)
    UserFlagType type;       ///< Value type
    std::string description; ///< Human-readable description (may be empty)
};

// ============================================================================
// Per-object value
// ============================================================================

/**
 * @brief A concrete flag value assigned to a specific object.
 * @ingroup engine_core
 */
using UserFlagValue = std::variant<bool, int, double, std::string>;

/**
 * @brief A (object_type, object_name, flag_name) → value assignment.
 *
 * @details Populated from [USER_FLAG_VALUES].
 * @ingroup engine_core
 */
struct UserFlagAssignment {
    std::string object_type; ///< e.g. "NODE", "LINK", "SUBCATCHMENT"
    std::string object_name; ///< Object identifier as it appears in the model
    std::string flag_name;   ///< Must match a name in UserFlags::defs_
    UserFlagValue value;     ///< Typed runtime value
};

// ============================================================================
// Container
// ============================================================================

/**
 * @brief Stores the full user-flags data: schema definitions + per-object values.
 *
 * @details Part of SimulationContext.  Accessible at runtime via the C API
 *          and Python bindings.
 *
 * Lookup key for per-object values is the composite string
 * @c "OBJECTTYPE:OBJECTNAME:FLAGNAME" with original spelling and ASCII-insensitive value lookup.
 * Schema definition lookup remains exact-case; callers normalize schema names when required.
 *
 * @ingroup engine_core
 */
class UserFlags {
public:
    // ------------------------------------------------------------------
    // Schema (from [USER_FLAGS])
    // ------------------------------------------------------------------

    /**
     * @brief Insert or replace an exact-case flag schema.
     *
     * Replacing preserves schema insertion order and existing assignments, including values whose
     * variant no longer matches the schema type. Adding can invalidate borrowed schema references.
     *
     * @param[in] def Definition passed by value and moved into storage; name/type/description are not
     *     normalized or validated.
     */
    void define(UserFlagDef def) {
        const auto it = def_index_.find(def.name);
        if (it != def_index_.end()) {
            defs_[it->second] = std::move(def);
        } else {
            def_index_[def.name] = defs_.size();
            defs_.push_back(std::move(def));
        }
    }

    /**
     * @brief Remove a schema and matching assignment suffixes.
     *
     * Renumbers following schema slots, then removes values whose suffix after the second colon equals
     * name case-sensitively. Mixed-case assignment spellings can therefore survive; components
     * containing colons are ambiguous to this suffix scan.
     *
     * @param[in] name Exact-case schema key; this schema map does not normalize case.
     * @return True when the exact schema key existed; false leaves all assignments unchanged.
     */
    bool undefine(const std::string& name) {
        const auto it = def_index_.find(name);
        if (it == def_index_.end())
            return false;
        const std::size_t idx = it->second;
        defs_.erase(defs_.begin() + static_cast<std::ptrdiff_t>(idx));
        def_index_.erase(it);
        // Reindex definitions that followed the erased slot.
        for (auto& kv : def_index_)
            if (kv.second > idx)
                --kv.second;
        // Remove orphaned per-object values (key = "TYPE:NAME:FLAG";
        // same left-to-right decomposition as InpWriter).
        for (auto vit = values_.begin(); vit != values_.end();) {
            const auto& key = vit->first;
            const auto p1 = key.find(':');
            const auto p2 = (p1 == std::string::npos) ? std::string::npos : key.find(':', p1 + 1);
            if (p2 != std::string::npos && key.compare(p2 + 1, std::string::npos, name) == 0)
                vit = values_.erase(vit);
            else
                ++vit;
        }
        return true;
    }

    /**
     * @brief Test exact-case schema membership.
     *
     * @param[in] name Exact-case schema key; this schema map does not normalize case.
     * @return True if that exact schema key exists.
     */
    bool is_defined(const std::string& name) const { return def_index_.count(name) != 0; }

    /**
     * @brief Borrow an exact-case schema definition.
     *
     * @param[in] name Exact-case schema key; this schema map does not normalize case.
     * @return Engine-owned definition reference, invalidated by schema vector mutation or container
     *     destruction.
     * @throws std::out_of_range When the exact schema key is absent.
     */
    const UserFlagDef& get_def(const std::string& name) const {
        const auto it = def_index_.find(name);
        if (it == def_index_.end())
            throw std::out_of_range("UserFlags: no definition for flag '" + name + "'");
        return defs_[it->second];
    }

    /**
     * @brief Borrow schema definitions in insertion order.
     * @return Reference to the owned vector; do not retain element references across schema edits.
     */
    const std::vector<UserFlagDef>& all_defs() const noexcept { return defs_; }

    /**
     * @brief Count schema definitions.
     * @return Number of exact-case schema entries.
     */
    std::size_t def_count() const noexcept { return defs_.size(); }

    // ------------------------------------------------------------------
    // Per-object values (from [USER_FLAG_VALUES])
    // ------------------------------------------------------------------

    /**
     * @brief Assign an owned variant value using a case-insensitive composite key.
     *
     * Overwrites an equivalent key while preserving its first stored spelling. Components are not
     * uppercased; the map folds ASCII case. Caller serializes reads/edits; no engine lifecycle check
     * occurs here.
     *
     * @param[in] object_type Borrowed object-category token; copied into the composite key.
     * @param[in] object_name Borrowed object ID; no model-object existence check is performed.
     * @param[in] flag_name Borrowed flag token; no schema/type validation is performed by this
     *     value-map operation.
     * @param[in] value Variant passed by value and moved into the map; no conversion to a declared
     *     schema type.
     */
    void set(const std::string& object_type, const std::string& object_name,
             const std::string& flag_name, UserFlagValue value) {
        values_[make_key(object_type, object_name, flag_name)] = std::move(value);
    }

    /**
     * @brief Remove a case-insensitive composite assignment.
     *
     * @param[in] object_type Borrowed object-category token; copied into the composite key.
     * @param[in] object_name Borrowed object ID; no model-object existence check is performed.
     * @param[in] flag_name Borrowed flag token; no schema/type validation is performed by this
     *     value-map operation.
     * @return True when an equivalent key was erased.
     */
    bool unset(const std::string& object_type, const std::string& object_name,
               const std::string& flag_name) {
        return values_.erase(make_key(object_type, object_name, flag_name)) != 0;
    }

    /**
     * @brief Test case-insensitive composite assignment membership.
     *
     * @param[in] object_type Borrowed object-category token; copied into the composite key.
     * @param[in] object_name Borrowed object ID; no model-object existence check is performed.
     * @param[in] flag_name Borrowed flag token; no schema/type validation is performed by this
     *     value-map operation.
     * @return True when the composite key is present.
     */
    bool has_value(const std::string& object_type, const std::string& object_name,
                   const std::string& flag_name) const {
        return values_.count(make_key(object_type, object_name, flag_name)) != 0;
    }

    /**
     * @brief Borrow a case-insensitive composite assignment value.
     *
     * @param[in] object_type Borrowed object-category token; copied into the composite key.
     * @param[in] object_name Borrowed object ID; no model-object existence check is performed.
     * @param[in] flag_name Borrowed flag token; no schema/type validation is performed by this
     *     value-map operation.
     * @return Reference to the owned variant, valid until that entry is replaced/erased or the
     *     container is destroyed.
     */
    const UserFlagValue& get_value(const std::string& object_type, const std::string& object_name,
                                   const std::string& flag_name) const {
        const auto it = values_.find(make_key(object_type, object_name, flag_name));
        if (it == values_.end())
            throw std::out_of_range("UserFlags: no value for " + object_type + ":" + object_name +
                                    ":" + flag_name);
        return it->second;
    }

    /**
     * @brief Copy a case-insensitive composite assignment when present.
     *
     * @param[in] object_type Borrowed object-category token; copied into the composite key.
     * @param[in] object_name Borrowed object ID; no model-object existence check is performed.
     * @param[in] flag_name Borrowed flag token; no schema/type validation is performed by this
     *     value-map operation.
     * @return Independent value copy, or std::nullopt for an absent key. String values may allocate
     *     while copying.
     */
    std::optional<UserFlagValue> try_get_value(const std::string& object_type,
                                               const std::string& object_name,
                                               const std::string& flag_name) const {
        const auto it = values_.find(make_key(object_type, object_name, flag_name));
        if (it == values_.end())
            return std::nullopt;
        return it->second;
    }

    /// Composite-key value map; case-insensitive so object-name lookups match
    /// the engine's legacy-parity name semantics, while the stored key keeps
    /// the first-seen spelling for faithful [USER_FLAG_VALUES] round-trips.
    using ValueMap = std::unordered_map<std::string, UserFlagValue, CiHash, CiEqual>;

    /**
     * @brief Borrow the complete case-insensitive assignment map.
     * @return Owned map reference; edits can invalidate iterators, and replacing/erasing entries
     *     invalidates borrowed values.
     */
    const ValueMap& all_values() const noexcept { return values_; }

    /**
     * @brief Count stored composite assignments.
     * @return Number of case-insensitively distinct composite keys.
     */
    std::size_t value_count() const noexcept { return values_.size(); }

    // ------------------------------------------------------------------
    // Reset
    // ------------------------------------------------------------------

    /**
     * @brief Erase all schemas and assignments.
     *
     * Invalidates borrowed schema/value references and iterators; vector/map capacity is not explicitly
     * released.
     */
    void clear() noexcept {
        defs_.clear();
        def_index_.clear();
        values_.clear();
    }

    // ------------------------------------------------------------------
    // Composite-key helper (public for tests)
    // ------------------------------------------------------------------

    /**
     * @brief Join three unmodified components with colon delimiters.
     *
     * Performs no escaping, normalization or validation. Callers needing an unambiguous composite
     * identity must ensure components contain no colons.
     *
     * @param[in] object_type Borrowed object-category token; copied into the composite key.
     * @param[in] object_name Borrowed object ID; no model-object existence check is performed.
     * @param[in] flag_name Borrowed flag token; no schema/type validation is performed by this
     *     value-map operation.
     * @return New owned string object_type + colon + object_name + colon + flag_name.
     */
    static std::string make_key(const std::string& object_type, const std::string& object_name,
                                const std::string& flag_name) {
        std::string key;
        key.reserve(object_type.size() + 1 + object_name.size() + 1 + flag_name.size());
        key += object_type;
        key += ':';
        key += object_name;
        key += ':';
        key += flag_name;
        return key;
    }

private:
    // Schema
    std::vector<UserFlagDef> defs_;
    std::unordered_map<std::string, std::size_t> def_index_; ///< name → defs_ index

    // Per-object values
    ValueMap values_; ///< composite key → value (case-insensitive)
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_USER_FLAGS_HPP */
