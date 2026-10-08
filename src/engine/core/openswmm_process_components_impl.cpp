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
 * @file openswmm_process_components_impl.cpp
 * @brief C API implementation — [PROCESS_COMPONENTS] registrations (E-C3).
 *
 * @see include/openswmm/engine/openswmm_process_components.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_process_components.h"
#include "../plugins/ProcessComponentRegistry.hpp"   // U1: built-in catalogue
#include "../plugins/PluginFactory.hpp"             // D2: component-library catalogue

#include <cstring>
#include <string>

namespace {

inline void copy_to_buf(const std::string& src, char* buf, int buflen) {
    if (!buf || buflen <= 0) return;
    const int copy_len = std::min(static_cast<int>(src.size()), buflen - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
}

// D2: fixed-size struct fields, sized by the array itself so a field can
// never be copied with the wrong length.
template <std::size_t N>
inline void copy_to_field(const std::string& src, char (&field)[N]) {
    copy_to_buf(src, field, static_cast<int>(N));
}

} // namespace

extern "C" {

SWMM_ENGINE_API int swmm_process_component_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return static_cast<int>(
        to_engine(engine)->context().process_component_specs.size());
}

SWMM_ENGINE_API int swmm_process_component_get(SWMM_Engine engine, int idx,
        char* id_buf, int id_len, char* config_buf, int config_len,
        char* resolved_buf, int resolved_len) {
    CHECK_HANDLE(engine);
    const auto& specs =
        to_engine(engine)->context().process_component_specs;
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(specs.size()));
    if (!id_buf || id_len <= 0 || !config_buf || config_len <= 0 ||
        !resolved_buf || resolved_len <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& s = specs[static_cast<std::size_t>(idx)];
    copy_to_buf(s.id, id_buf, id_len);
    copy_to_buf(s.config_path, config_buf, config_len);
    copy_to_buf(s.resolved_config_path, resolved_buf, resolved_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_process_component_known_count(void) {
    return static_cast<int>(
        openswmm::components::ProcessComponentRegistry::instance().known_ids().size());
}

SWMM_ENGINE_API int swmm_process_component_known_get(int idx,
        char* id_buf, int id_len, char* desc_buf, int desc_len,
        int* implemented) {
    const auto& reg = openswmm::components::ProcessComponentRegistry::instance();
    const auto ids = reg.known_ids();
    if (idx < 0 || idx >= static_cast<int>(ids.size())) return SWMM_ERR_BADINDEX;
    const std::string& id = ids[static_cast<std::size_t>(idx)];
    const auto* entry = reg.find(id);
    if (!entry) return SWMM_ERR_INTERNAL;
    copy_to_buf(id, id_buf, id_len);
    copy_to_buf(entry->description, desc_buf, desc_len);
    if (implemented) *implemented = entry->apply ? 1 : 0;
    return SWMM_OK;
}

// ---- D2 (program plan §B.2.5): discovered component libraries -------------

SWMM_ENGINE_API int swmm_component_search_path_add(const char* dir) {
    if (!dir || !*dir) return SWMM_ERR_BADPARAM;
    if (!openswmm::PluginFactory::hydrocouple_enabled()) return SWMM_ERR_PLUGIN;
    return openswmm::PluginFactory::component_search_path_add(dir) < 0
        ? SWMM_ERR_BADPARAM : SWMM_OK;
}

SWMM_ENGINE_API int swmm_component_library_count(void) {
    return static_cast<int>(openswmm::PluginFactory::component_libraries().size());
}

SWMM_ENGINE_API int swmm_component_library_get(int idx,
        SWMM_ComponentLibraryInfo* info) {
    if (!info) return SWMM_ERR_BADPARAM;
    const auto recs = openswmm::PluginFactory::component_libraries();
    if (idx < 0 || idx >= static_cast<int>(recs.size())) return SWMM_ERR_BADINDEX;
    const auto& r = recs[static_cast<std::size_t>(idx)];
    copy_to_field(r.id,         info->id);
    copy_to_field(r.caption,    info->caption);
    copy_to_field(r.version,    info->version);
    copy_to_field(r.path,       info->path);
    copy_to_field(r.kind,       info->kind);
    copy_to_field(r.stamp,      info->stamp);
    copy_to_field(r.load_error, info->load_error);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_process_component_find(SWMM_Engine engine,
        const char* id) {
    if (!engine || !id) return -1;
    const auto& specs =
        to_engine(engine)->context().process_component_specs;
    for (std::size_t i = 0; i < specs.size(); ++i)
        if (specs[i].id == id) return static_cast<int>(i);
    return -1;
}

SWMM_ENGINE_API int swmm_process_component_register(SWMM_Engine engine,
        const char* id, const char* config_path) {
    CHECK_HANDLE(engine);
    if (!id || !*id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    if (swmm_process_component_find(engine, id) >= 0)
        return SWMM_ERR_BADPARAM;   // duplicate id — the parser's rule
    openswmm::ProcessComponentSpec spec;
    spec.id = id;
    if (config_path) spec.config_path = config_path;
    // The file need not exist yet: the GUI's "create component + config
    // file" flow registers first, then swmm_reactions_save writes it;
    // resolve_process_components reads it at the next open (D-RC8).
    ctx.process_component_specs.push_back(std::move(spec));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_process_component_remove(SWMM_Engine engine,
        int idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    auto& specs = ctx.process_component_specs;
    CHECK_INDEX(idx >= 0 && idx < static_cast<int>(specs.size()));
    specs.erase(specs.begin() + static_cast<std::size_t>(idx));
    return SWMM_OK;
}

} // extern "C"
