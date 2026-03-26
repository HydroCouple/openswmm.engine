/**
 * @file UserFlags.hpp
 * @brief User-defined model flags (InfoWorks ICM-style, two-section design).
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#ifndef OPENSWMM_ENGINE_USER_FLAGS_HPP
#define OPENSWMM_ENGINE_USER_FLAGS_HPP

#include <string>
#include <unordered_map>
#include <vector>
#include <variant>
#include <optional>
#include <stdexcept>

namespace openswmm {

// ============================================================================
// Type tag
// ============================================================================

/**
 * @brief Value type for a user flag.
 * @ingroup engine_core
 */
enum class UserFlagType : int {
    BOOLEAN = 0,  ///< YES/NO/TRUE/FALSE/1/0
    INTEGER = 1,  ///< Signed integer
    REAL    = 2,  ///< Double-precision floating-point
    STRING  = 3   ///< Arbitrary string
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
    std::string  name;         ///< Flag identifier (uppercase)
    UserFlagType type;         ///< Value type
    std::string  description;  ///< Human-readable description (may be empty)
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
    std::string    object_type;  ///< e.g. "NODE", "LINK", "SUBCATCHMENT"
    std::string    object_name;  ///< Object identifier as it appears in the model
    std::string    flag_name;    ///< Must match a name in UserFlags::defs_
    UserFlagValue  value;        ///< Typed runtime value
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
 * @c "OBJECTTYPE:OBJECTNAME:FLAGNAME" (all uppercase).
 *
 * @ingroup engine_core
 */
class UserFlags {
public:

    // ------------------------------------------------------------------
    // Schema (from [USER_FLAGS])
    // ------------------------------------------------------------------

    /**
     * @brief Register a flag definition.
     * @details Overwrites any existing definition with the same name.
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
     * @brief Check whether a flag name has been defined (schema).
     */
    bool is_defined(const std::string& name) const {
        return def_index_.count(name) != 0;
    }

    /**
     * @brief Retrieve a flag definition by name.
     * @throws std::out_of_range if not defined.
     */
    const UserFlagDef& get_def(const std::string& name) const {
        const auto it = def_index_.find(name);
        if (it == def_index_.end())
            throw std::out_of_range("UserFlags: no definition for flag '" + name + "'");
        return defs_[it->second];
    }

    /**
     * @brief All flag definitions, in insertion order.
     */
    const std::vector<UserFlagDef>& all_defs() const noexcept { return defs_; }

    /**
     * @brief Number of defined flag schemas.
     */
    std::size_t def_count() const noexcept { return defs_.size(); }

    // ------------------------------------------------------------------
    // Per-object values (from [USER_FLAG_VALUES])
    // ------------------------------------------------------------------

    /**
     * @brief Assign a value to (object_type, object_name, flag_name).
     * @details Overwrites any existing assignment for the same triple.
     *          @p object_type and @p flag_name are stored uppercase.
     */
    void set(const std::string& object_type,
             const std::string& object_name,
             const std::string& flag_name,
             UserFlagValue      value) {
        values_[make_key(object_type, object_name, flag_name)] = std::move(value);
    }

    /**
     * @brief Check whether a value has been assigned for a given triple.
     */
    bool has_value(const std::string& object_type,
                   const std::string& object_name,
                   const std::string& flag_name) const {
        return values_.count(make_key(object_type, object_name, flag_name)) != 0;
    }

    /**
     * @brief Retrieve the value for (object_type, object_name, flag_name).
     * @throws std::out_of_range if not found.
     */
    const UserFlagValue& get_value(const std::string& object_type,
                                   const std::string& object_name,
                                   const std::string& flag_name) const {
        const auto it = values_.find(make_key(object_type, object_name, flag_name));
        if (it == values_.end())
            throw std::out_of_range(
                "UserFlags: no value for " + object_type + ":" +
                object_name + ":" + flag_name);
        return it->second;
    }

    /**
     * @brief Retrieve the value, or nullopt if not assigned.
     */
    std::optional<UserFlagValue> try_get_value(const std::string& object_type,
                                               const std::string& object_name,
                                               const std::string& flag_name) const {
        const auto it = values_.find(make_key(object_type, object_name, flag_name));
        if (it == values_.end()) return std::nullopt;
        return it->second;
    }

    /**
     * @brief All per-object value assignments, keyed by composite string.
     */
    const std::unordered_map<std::string, UserFlagValue>& all_values() const noexcept {
        return values_;
    }

    /**
     * @brief Number of per-object value assignments.
     */
    std::size_t value_count() const noexcept { return values_.size(); }

    // ------------------------------------------------------------------
    // Reset
    // ------------------------------------------------------------------

    /**
     * @brief Clear both schema definitions and per-object value assignments.
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
     * @brief Build the composite lookup key "OBJECTTYPE:OBJECTNAME:FLAGNAME".
     * @details @p object_type and @p flag_name are uppercased; @p object_name
     *          preserves its original case (object names may be mixed-case in
     *          the input file).
     */
    static std::string make_key(const std::string& object_type,
                                const std::string& object_name,
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
    std::vector<UserFlagDef>                  defs_;
    std::unordered_map<std::string, std::size_t> def_index_;  ///< name → defs_ index

    // Per-object values
    std::unordered_map<std::string, UserFlagValue> values_;   ///< composite key → value
};

} /* namespace openswmm */

#endif /* OPENSWMM_ENGINE_USER_FLAGS_HPP */
