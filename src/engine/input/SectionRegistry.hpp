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
 * @file      SectionRegistry.hpp
 * @brief     Registry that maps SWMM section tags to handler functions.
 *
 * @details The SectionRegistry enables optional sections (R09): user code
 *          can register custom handler functions for new section tags.
 *          Built-in sections (NODES, LINKS, etc.) are registered at startup.
 *
 * Handler signature:
 * @code{.cpp}
 * using SectionHandler = std::function<void(
 *     SimulationContext&,
 *     const std::vector<std::string>& lines
 * )>;
 * @endcode
 *
 * @see Legacy reference: src/solver/input.c — findSection(), which uses a
 *      static keyword array and switch statement. The new registry replaces
 *      that with a hash map of function pointers.
 *
 * @see tests/unit/test_section_registry.cpp
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_SECTION_REGISTRY_HPP
#define OPENSWMM_ENGINE_SECTION_REGISTRY_HPP

#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <unordered_map>

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::input {

/**
 * @brief Handler function type for input file sections.
 *
 * @details Called for each dispatched section batch, including deferred-row replay.
 *          InputReader can retain leading comment rows for object comments. The handler is responsible for parsing each line
 *          and populating the SimulationContext.
 *
 * @param ctx    Simulation context to populate.
 * @param lines  All non-blank, non-comment lines in the section (after
 *               comment stripping but before tokenization).
 */
using SectionHandler =
    std::function<void(SimulationContext&, const std::vector<std::string>& /*lines*/
                       )>;

/**
 * @brief Registry that maps section tags (e.g., "[JUNCTIONS]") to handlers.
 *
 * @details Two types of sections:
 * - **Built-in sections:** Registered by InputReader at startup for all
 *   standard SWMM 5.x sections.
 * - **Custom sections:** Registered by user code via register_custom() for
 *   new or extension sections. Custom sections can override built-ins.
 *
 * @ingroup engine_input
 */
class SectionRegistry {
public:
    /**
     * @brief Create empty built-in/custom handler maps.
     */
    SectionRegistry() = default;
    /**
     * @brief Destroy owned handler function objects and any captures they own.
     */
    ~SectionRegistry() = default;

    // -----------------------------------------------------------------------
    // Registration
    // -----------------------------------------------------------------------

    /**
     * @brief Store or replace one built-in section callback.
     *
     * Does not remove an overriding custom callback. Empty function objects are accepted and throw
     * std::bad_function_call when invoked. Caller serializes registration and dispatch.
     *
     * @param[in] tag Borrowed tag without brackets; copied and normalized using Tokenizer::to_upper().
     *     No bracket stripping or whitespace trimming occurs.
     * @param[in] handler Function object passed by value and moved into the registry; captured
     *     references must outlive dispatches.
     */
    void register_builtin(std::string_view tag, SectionHandler handler);

    /**
     * @brief Store or replace a callback that takes precedence over a built-in.
     *
     * Empty function objects are accepted; dispatch then throws std::bad_function_call. No internal
     * synchronization.
     *
     * @param[in] tag Borrowed tag without brackets; copied and normalized using Tokenizer::to_upper().
     *     No bracket stripping or whitespace trimming occurs.
     * @param[in] handler Function object passed by value and moved into owned storage; captured
     *     references remain caller-managed.
     */
    void register_custom(std::string_view tag, SectionHandler handler);

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------

    /**
     * @brief Test whether either normalized handler map contains a tag.
     *
     * @param[in] tag Borrowed tag without brackets; copied and normalized using Tokenizer::to_upper().
     *     No bracket stripping or whitespace trimming occurs.
     * @return True when custom or built-in map contains the key, even if its std::function is empty.
     */
    bool has(std::string_view tag) const;

    /**
     * @brief Test whether the normalized custom map contains a tag.
     *
     * @param[in] tag Borrowed tag without brackets; copied and normalized using Tokenizer::to_upper().
     *     No bracket stripping or whitespace trimming occurs.
     * @return True when a custom entry exists; does not compare it with built-in registration.
     */
    bool is_custom(std::string_view tag) const;

    // -----------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------

    /**
     * @brief Invoke a custom callback first, otherwise a built-in, otherwise no-op.
     *
     * Does not catch callback/allocation exceptions or synthesize unknown-section warnings. Callbacks
     * must not retain line references beyond their valid lifetime. Caller serializes registry mutation
     * and dispatch.
     *
     * @param[in] tag Borrowed tag without brackets; copied and normalized using Tokenizer::to_upper().
     *     No bracket stripping or whitespace trimming occurs.
     * @param[in,out] ctx Caller-owned context forwarded by reference; callback controls mutations and
     *     error recording.
     * @param[in] lines Borrowed section lines passed synchronously; may include retained comment rows
     *     according to InputReader policy.
     */
    void dispatch(std::string_view tag, SimulationContext& ctx,
                  const std::vector<std::string>& lines) const;

    // -----------------------------------------------------------------------
    // Enumeration
    // -----------------------------------------------------------------------

    /**
     * @brief Copy the union of normalized built-in/custom tags.
     * @return Owned unsorted tag vector without duplicates; hash-map iteration order is unspecified.
     */
    std::vector<std::string> registered_tags() const;

private:
    std::unordered_map<std::string, SectionHandler> builtin_;
    std::unordered_map<std::string, SectionHandler> custom_;

    /**
     * @brief Copy and uppercase a tag with tokenizer locale rules.
     *
     * @param[in] tag Borrowed tag without brackets; copied and normalized using Tokenizer::to_upper().
     *     No bracket stripping or whitespace trimming occurs.
     * @return Owned normalized tag; no bracket or whitespace cleanup.
     */
    std::string normalize_tag(std::string_view tag) const;
};

} /* namespace openswmm::input */

#endif /* OPENSWMM_ENGINE_SECTION_REGISTRY_HPP */
