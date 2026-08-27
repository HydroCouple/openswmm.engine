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
 * @file openswmm_water_age_impl.cpp
 * @brief Water-age source-table CRUD implementation (subplan X5).
 *
 * @details Values cross the boundary in HOURS (the config file's unit);
 *          the engine stores SECONDS. Negative values are legal and pass
 *          straight through — a negative source age extracts age-volume
 *          (D-NS1), clamped at consumption by the engines. There is no
 *          `ucf` conversion here: hours are hours in every unit system.
 *
 * @see include/openswmm/engine/openswmm_water_age.h
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_water_age.h"

#include <cstdio>
#include <string>

namespace {

constexpr double kSecPerHour = 3600.0;

bool valid_source(int s) {
    return s >= 0 && s < static_cast<int>(openswmm::WaterAgeSource::COUNT_);
}

/// The A1a scope rule the parser enforces: only DWF and EXTERNAL_INFLOW
/// take per-node overrides.
bool node_scoped(int s) {
    return s == static_cast<int>(openswmm::WaterAgeSource::DWF) ||
           s == static_cast<int>(openswmm::WaterAgeSource::EXTERNAL_INFLOW);
}

/// Row index of (source, node) in the override arrays, or -1.
int find_override(const openswmm::WaterAgeConfigData& cfg, int source,
                  int node) {
    for (std::size_t i = 0; i < cfg.node_over_source.size(); ++i)
        if (cfg.node_over_source[i] == source &&
            cfg.node_over_node[i] == node)
            return static_cast<int>(i);
    return -1;
}

const char* source_name(int s) {
    switch (static_cast<openswmm::WaterAgeSource>(s)) {
        case openswmm::WaterAgeSource::RAINFALL:        return "RAINFALL";
        case openswmm::WaterAgeSource::DWF:             return "DWF";
        case openswmm::WaterAgeSource::GW:              return "GW";
        case openswmm::WaterAgeSource::RDII:            return "RDII";
        case openswmm::WaterAgeSource::EXTERNAL_INFLOW: return "EXTERNAL_INFLOW";
        case openswmm::WaterAgeSource::IFACE:           return "IFACE";
        case openswmm::WaterAgeSource::INITIAL_STATE:   return "INITIAL_STATE";
        default:                                        return "UNKNOWN";
    }
}

}  // namespace

SWMM_ENGINE_API int swmm_water_age_get_enabled(SWMM_Engine engine,
                                               int* enabled) {
    CHECK_HANDLE(engine);
    if (enabled == nullptr) return SWMM_ERR_BADPARAM;
    *enabled = to_engine(engine)->context().options.water_age ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_get_global_source(SWMM_Engine engine,
                                                     int source,
                                                     double* hours) {
    CHECK_HANDLE(engine);
    if (hours == nullptr) return SWMM_ERR_BADPARAM;
    CHECK_INDEX(valid_source(source));
    const auto& cfg = to_engine(engine)->context().water_age_config;
    *hours = cfg.global_age[source] / kSecPerHour;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_set_global_source(SWMM_Engine engine,
                                                     int source,
                                                     double hours) {
    CHECK_HANDLE(engine);
    CHECK_INDEX(valid_source(source));
    auto& cfg = to_engine(engine)->context().water_age_config;
    // Negative hours are legal — extraction (D-NS1). No floor here: the
    // clamp lives at consumption, where the held age is known.
    cfg.global_age[source] = hours * kSecPerHour;
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_override_count(SWMM_Engine engine,
                                                  int* count) {
    CHECK_HANDLE(engine);
    if (count == nullptr) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(
        to_engine(engine)->context().water_age_config.node_over_source.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_get_override(SWMM_Engine engine, int index,
                                                int* source, int* node_index,
                                                double* hours) {
    CHECK_HANDLE(engine);
    const auto& cfg = to_engine(engine)->context().water_age_config;
    CHECK_INDEX(index >= 0 &&
                index < static_cast<int>(cfg.node_over_source.size()));
    const auto ui = static_cast<std::size_t>(index);
    if (source != nullptr)     *source = cfg.node_over_source[ui];
    if (node_index != nullptr) *node_index = cfg.node_over_node[ui];
    if (hours != nullptr)      *hours = cfg.node_over_age[ui] / kSecPerHour;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_set_override(SWMM_Engine engine,
                                                int source, int node_index,
                                                double hours) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(valid_source(source));
    CHECK_INDEX(node_index >= 0 && node_index < ctx.n_nodes());
    // The parser's A1a scope rule, enforced identically here — an editor
    // must not be able to author a table the file parser would refuse.
    if (!node_scoped(source)) return SWMM_ERR_BADPARAM;

    auto& cfg = ctx.water_age_config;
    const int row = find_override(cfg, source, node_index);
    if (row >= 0) {
        cfg.node_over_age[static_cast<std::size_t>(row)] =
            hours * kSecPerHour;
    } else {
        cfg.node_over_source.push_back(source);
        cfg.node_over_node.push_back(node_index);
        cfg.node_over_age.push_back(hours * kSecPerHour);
    }
    cfg.configured = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_remove_override(SWMM_Engine engine,
                                                   int source,
                                                   int node_index) {
    CHECK_HANDLE(engine);
    auto& cfg = to_engine(engine)->context().water_age_config;
    const int row = find_override(cfg, source, node_index);
    CHECK_INDEX(row >= 0);
    const auto ui = static_cast<std::size_t>(row);
    cfg.node_over_source.erase(cfg.node_over_source.begin() + ui);
    cfg.node_over_node.erase(cfg.node_over_node.begin() + ui);
    cfg.node_over_age.erase(cfg.node_over_age.begin() + ui);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_water_age_save(SWMM_Engine engine,
                                        const char* path) {
    CHECK_HANDLE(engine);
    if (path == nullptr || *path == '\0') return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const auto& cfg = ctx.water_age_config;

    std::FILE* f = std::fopen(path, "w");
    if (f == nullptr) return SWMM_ERR_IO;
    std::fprintf(f, "[WATER_AGE_SOURCES]\n");
    for (int s = 0; s < static_cast<int>(openswmm::WaterAgeSource::COUNT_);
         ++s) {
        // Zero is the default — writing every row would make a save-as of
        // an untouched model look configured. Negative rows DO write.
        if (cfg.global_age[s] == 0.0) continue;
        std::fprintf(f, "%-16s GLOBAL %g\n", source_name(s),
                     cfg.global_age[s] / kSecPerHour);
    }
    for (std::size_t i = 0; i < cfg.node_over_source.size(); ++i) {
        const int nd = cfg.node_over_node[i];
        if (nd < 0 || nd >= ctx.n_nodes()) continue;
        std::fprintf(f, "%-16s NODE %s %g\n",
                     source_name(cfg.node_over_source[i]),
                     ctx.node_names.name_of(nd).c_str(),
                     cfg.node_over_age[i] / kSecPerHour);
    }
    std::fclose(f);
    return SWMM_OK;
}
