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
 * @file      SectionRegistry.cpp
 * @brief     Implementation of the section-handler registry.
 *
 * @see SectionRegistry.hpp for interface documentation.
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
    std::string key = normalize_tag(tag);
    custom_views_.erase(key);
    custom_[std::move(key)] = std::move(handler);
}

void SectionRegistry::register_custom_views(std::string_view tag, SectionViewHandler handler) {
    std::string key = normalize_tag(tag);
    custom_.erase(key);
    custom_views_[std::move(key)] = std::move(handler);
}

// ============================================================================
// Query
// ============================================================================

bool SectionRegistry::has(std::string_view tag) const {
    const std::string key = normalize_tag(tag);
    return custom_.count(key) > 0 || custom_views_.count(key) > 0 || builtin_.count(key) > 0;
}

bool SectionRegistry::is_custom(std::string_view tag) const {
    const std::string key = normalize_tag(tag);
    return custom_.count(key) > 0 || custom_views_.count(key) > 0;
}

bool SectionRegistry::takes_views(std::string_view tag) const {
    return custom_views_.count(normalize_tag(tag)) > 0;
}

// ============================================================================
// Dispatch
// ============================================================================

void SectionRegistry::dispatch(std::string_view tag, SimulationContext& ctx,
                               const std::vector<std::string>& lines) const {
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
        auto it = custom_views_.find(key);
        if (it != custom_views_.end()) {
            const std::vector<std::string_view> views(lines.begin(), lines.end());
            it->second(ctx, views);
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

void SectionRegistry::dispatch_views(std::string_view tag, SimulationContext& ctx,
                                     const std::vector<std::string_view>& lines) const {
    {
        auto it = custom_views_.find(normalize_tag(tag));
        if (it != custom_views_.end()) {
            it->second(ctx, lines);
            return;
        }
    }
    if (!has(tag))
        return; // unknown section, as dispatch()
    const std::vector<std::string> copies(lines.begin(), lines.end());
    dispatch(tag, ctx, copies);
}

// ============================================================================
// Enumeration
// ============================================================================

std::vector<std::string> SectionRegistry::registered_tags() const {
    std::vector<std::string> tags;
    tags.reserve(builtin_.size() + custom_.size());
    for (const auto& [tag, _] : builtin_)
        tags.push_back(tag);
    auto add_unique = [&tags](const std::string& tag) {
        // Avoid duplicates (custom overrides same-named builtin)
        for (const auto& existing : tags) {
            if (existing == tag)
                return;
        }
        tags.push_back(tag);
    };
    for (const auto& [tag, _] : custom_)
        add_unique(tag);
    for (const auto& [tag, _] : custom_views_)
        add_unique(tag);
    return tags;
}

} /* namespace openswmm::input */
