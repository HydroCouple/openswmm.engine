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
 * @file openswmm_initial_quality_impl.cpp
 * @brief C API implementation — [INITIAL_QUALITY] per-element rows (E-A4).
 *
 * @see include/openswmm/engine/openswmm_initial_quality.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_initial_quality.h"

#include <cstring>
#include <string>

namespace {

/// Copy a std::string into a NUL-terminated caller buffer, truncating safely.
inline void copy_to_buf(const std::string& src, char* buf, int buflen) {
    if (!buf || buflen <= 0) return;
    const int copy_len = std::min(static_cast<int>(src.size()), buflen - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
}

/// Classify a constituent name against the current model: pollutant index,
/// the reserved kinds, or kKindUnresolved when it matches nothing.
inline int classify(const openswmm::SimulationContext& ctx,
                    const std::string& cons) {
    if (cons == "__WATER_AGE__")
        return openswmm::InitialQualityData::kKindWaterAge;
    if (cons == "__TEMPERATURE__")
        return openswmm::InitialQualityData::kKindTemperature;
    const int p = ctx.pollutant_names.find(cons);
    return (p >= 0) ? p : openswmm::InitialQualityData::kKindUnresolved;
}

} // namespace

extern "C" {

SWMM_ENGINE_API int swmm_init_quality_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().initial_quality.count();
}

SWMM_ENGINE_API int swmm_init_quality_get(SWMM_Engine engine, int entry_idx,
                                          int* is_link, int* elem_idx,
                                          char* constituent_buf,
                                          int constituent_len, double* value) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.initial_quality.count());
    if (!is_link || !elem_idx || !value ||
        !constituent_buf || constituent_len <= 0)
        return SWMM_ERR_BADPARAM;

    const auto u = static_cast<std::size_t>(entry_idx);
    const auto& iq = ctx.initial_quality;
    *is_link  = iq.is_link[u] ? 1 : 0;
    *elem_idx = iq.elem_idx[u];
    *value    = iq.value[u];
    copy_to_buf(iq.constituent[u], constituent_buf, constituent_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_init_quality_set(SWMM_Engine engine, int is_link,
                                          int elem_idx,
                                          const char* constituent,
                                          double value) {
    CHECK_HANDLE(engine);
    if (!constituent) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    // Rows only seed state at initialize(); a mid-run edit would silently
    // no-op — guard to the editable states (swmm_pollutant_set_init_conc
    // contract).
    CHECK_GEOMETRY(ctx);
    const int n_elems = is_link ? ctx.n_links() : ctx.n_nodes();
    CHECK_INDEX(elem_idx >= 0 && elem_idx < n_elems);

    const std::string cons(constituent);
    const int kind = classify(ctx, cons);
    if (kind == openswmm::InitialQualityData::kKindUnresolved)
        return SWMM_ERR_BADPARAM;
    // Pollutant concentrations may not be negative; the reserved species
    // may (signed age per D-NS1; degC temperatures).
    if (kind >= 0 && value < 0.0) return SWMM_ERR_BADPARAM;

    auto& iq = ctx.initial_quality;
    const std::string& name = is_link
        ? ctx.link_names.name_of(elem_idx)
        : ctx.node_names.name_of(elem_idx);

    // Upsert on (is_link, elem_idx, kind) — the parser rejects duplicates,
    // so at most one row matches.
    for (int r = 0; r < iq.count(); ++r) {
        const auto ur = static_cast<std::size_t>(r);
        if ((iq.is_link[ur] != 0) == (is_link != 0) &&
            iq.elem_idx[ur] == elem_idx && iq.kind[ur] == kind) {
            iq.value[ur]       = value;
            iq.constituent[ur] = cons;
            iq.elem_name[ur]   = name;
            return SWMM_OK;
        }
    }
    iq.add(is_link != 0, name, cons, value, elem_idx, kind);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_init_quality_remove(SWMM_Engine engine,
                                             int entry_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(entry_idx >= 0 && entry_idx < ctx.initial_quality.count());
    ctx.initial_quality.erase(entry_idx);
    return SWMM_OK;
}

} // extern "C"
