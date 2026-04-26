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

} /* namespace openswmm */
