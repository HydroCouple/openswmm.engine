/**
 * @file PluginDiscovery.cpp
 * @brief Public filter-discovery facade — implementation.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "../../../include/openswmm/plugin_sdk/PluginDiscovery.hpp"

#include "PluginFactory.hpp"

#include <algorithm>
#include <unordered_map>

namespace openswmm {

std::vector<DiscoveredFilter> discover_all_filters() {
    PluginFactory factory;  // constructor scans built-ins + on-disk libraries
    std::vector<DiscoveredFilter> out;

    for (const auto& c : factory.discovered_components()) {
        if (!c.info) continue;
        for (auto& f : c.info->file_filters()) {
            DiscoveredFilter df;
            df.plugin_id      = c.id;
            df.plugin_version = c.version;
            df.plugin_caption = c.info->caption();
            df.filter         = std::move(f);
            out.push_back(std::move(df));
        }
    }

    return out;
}

std::vector<DiscoveredPlugin> discover_plugins_by_id() {
    // Walk discover_all_filters() once and fold entries by plugin_id.
    // Index map keeps O(1) lookup; a parallel vector preserves
    // first-encounter order for callers that want stable iteration.
    std::vector<DiscoveredPlugin> out;
    std::unordered_map<std::string, std::size_t> idx;

    for (auto& df : discover_all_filters()) {
        auto it = idx.find(df.plugin_id);
        DiscoveredPlugin* dp;
        if (it == idx.end()) {
            DiscoveredPlugin entry;
            entry.plugin_id      = df.plugin_id;
            entry.plugin_version = df.plugin_version;
            entry.plugin_caption = df.plugin_caption;
            out.push_back(std::move(entry));
            idx.emplace(df.plugin_id, out.size() - 1);
            dp = &out.back();
        } else {
            dp = &out[it->second];
        }

        // Accumulate role (deduplicated) and the filter itself.
        const PluginRole role = df.filter.role;
        if (std::find(dp->roles.begin(), dp->roles.end(), role) == dp->roles.end())
            dp->roles.push_back(role);
        dp->filters.push_back(std::move(df.filter));
    }

    return out;
}

} /* namespace openswmm */
