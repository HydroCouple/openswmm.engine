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
 * @file SectionRegistry.cpp
 * @brief Implementation of the section-handler registry.
 *
 * @see SectionRegistry.hpp for interface documentation.
 * @ingroup engine_input
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "SectionRegistry.hpp"
#include "Tokenizer.hpp"

namespace openswmm::input {

// ============================================================================
// Private helpers
// ============================================================================

std::string SectionRegistry::normalize_tag(std::string_view tag) const {
    return Tokenizer::to_upper(tag);
}

// ============================================================================
// Registration
// ============================================================================

void SectionRegistry::register_builtin(std::string_view tag, SectionHandler handler) {
    builtin_[normalize_tag(tag)] = std::move(handler);
}

void SectionRegistry::register_custom(std::string_view tag, SectionHandler handler) {
    custom_[normalize_tag(tag)] = std::move(handler);
}

// ============================================================================
// Query
// ============================================================================

bool SectionRegistry::has(std::string_view tag) const {
    const std::string key = normalize_tag(tag);
    return custom_.count(key) > 0 || builtin_.count(key) > 0;
}

bool SectionRegistry::is_custom(std::string_view tag) const {
    return custom_.count(normalize_tag(tag)) > 0;
}

// ============================================================================
// Dispatch
// ============================================================================

void SectionRegistry::dispatch(
    std::string_view                tag,
    SimulationContext&              ctx,
    const std::vector<std::string>& lines
) const {
    const std::string key = normalize_tag(tag);

    // Custom handlers override built-ins with the same tag
    {
        auto it = custom_.find(key);
        if (it != custom_.end()) {
            it->second(ctx, lines);
            return;
        }
    }
    {
        auto it = builtin_.find(key);
        if (it != builtin_.end()) {
            it->second(ctx, lines);
            return;
        }
    }
    // Unknown section — silently ignored (caller should warn if desired)
}

// ============================================================================
// Enumeration
// ============================================================================

std::vector<std::string> SectionRegistry::registered_tags() const {
    std::vector<std::string> tags;
    tags.reserve(builtin_.size() + custom_.size());
    for (const auto& [tag, _] : builtin_) tags.push_back(tag);
    for (const auto& [tag, _] : custom_) {
        // Avoid duplicates (custom overrides same-named builtin)
        bool already = false;
        for (const auto& existing : tags) {
            if (existing == tag) { already = true; break; }
        }
        if (!already) tags.push_back(tag);
    }
    return tags;
}

} /* namespace openswmm::input */
