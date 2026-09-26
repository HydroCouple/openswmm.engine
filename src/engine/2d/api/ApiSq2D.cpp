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
 * @file ApiSq2D.cpp
 * @brief S7 (2026-09-19) — C API over the 2D surface-quality rows and the
 *        cell buildup store (openswmm_sq2d.h).
 *
 * @details Row edits go through the SAME line parsers the `.inp` sections
 *          use (`parse2DCoveragesLine` & co.), so the API and the file share
 *          one grammar, one replace-by-scope rule and one percent check. The
 *          names a row refers to are validated here against the model's
 *          land uses and species, which the parsers defer to initialize —
 *          a host must not be able to build a model the file would refuse.
 */

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_sq2d.h>

#include "../../core/SWMMEngine.hpp"
#include "../quality/SurfaceQuality2D.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace {

using openswmm::twoD::SqScope;
using openswmm::twoD::SqScopeKey;
using openswmm::twoD::SurfaceQuality2D;

void copy_to(const std::string& src, char* buf, int len) {
    if (!buf || len <= 0) return;
    const int n = std::min(static_cast<int>(src.size()), len - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(n));
    buf[n] = '\0';
}

bool scopeTokens(int scope, const char* tag, int cell, std::vector<std::string>& t) {
    if (scope == SWMM_SQ2D_SCOPE_GLOBAL) { t.push_back("*"); return true; }
    if (scope == SWMM_SQ2D_SCOPE_TAG) {
        if (!tag || tag[0] == '\0') return false;
        t.push_back("TAG"); t.push_back(tag); return true;
    }
    if (scope == SWMM_SQ2D_SCOPE_CELL) {
        if (cell < 0) return false;
        t.push_back("CELL"); t.push_back(std::to_string(cell + 1)); return true;
    }
    return false;
}

bool sameKey(const SqScopeKey& a, const SqScopeKey& b) {
    return a.scope == b.scope && a.tag == b.tag && a.cell == b.cell;
}

bool isSurfaceSpecies(const openswmm::SimulationContext& ctx, const std::string& name) {
    if (ctx.pollutant_names.find(name) >= 0) return true;
    for (const auto& s : ctx.reactions.species_name)
        if (s == name) return true;
    return false;
}

}  // namespace

#define SQ_STORE(engine) \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine); \
    if (!eng) return SWMM_ERR_BADHANDLE; \
    SurfaceQuality2D& sq = eng->surfaceRouter2D().surfaceQuality()

#define SQ_EDITABLE(eng) \
    if (eng->context().state != openswmm::EngineState::OPENED && \
        eng->context().state != openswmm::EngineState::BUILDING) \
        return SWMM_ERR_LIFECYCLE

// ---------------------------------------------------------------------------
// [2D_COVERAGES]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_2d_coverage_count(SWMM_Engine engine, int* count) {
    SQ_STORE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(sq.coverage_rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_coverage_set(SWMM_Engine engine, int scope,
                                         const char* tag, int cell,
                                         const char* const* landuses,
                                         const double* percents, int n) {
    SQ_STORE(engine);
    SQ_EDITABLE(eng);
    if (n <= 0 || !landuses || !percents) return SWMM_ERR_BADPARAM;
    std::vector<std::string> t;
    if (!scopeTokens(scope, tag, cell, t)) return SWMM_ERR_BADPARAM;
    for (int i = 0; i < n; ++i) {
        if (!landuses[i] || eng->context().landuse_names.find(landuses[i]) < 0)
            return SWMM_ERR_BADPARAM;
        t.emplace_back(landuses[i]);
        t.push_back(std::to_string(percents[i]));
    }
    return openswmm::twoD::parse2DCoveragesLine(t, sq.coverage_rows).empty()
               ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_2d_coverage_row_size(SWMM_Engine engine, int index, int* n) {
    SQ_STORE(engine);
    if (!n || index < 0 || index >= static_cast<int>(sq.coverage_rows.size()))
        return SWMM_ERR_BADPARAM;
    *n = static_cast<int>(sq.coverage_rows[static_cast<std::size_t>(index)].uses.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_coverage_get(SWMM_Engine engine, int index, int k,
                                         int* scope, char* tag, int taglen,
                                         int* cell, char* landuse, int lulen,
                                         double* percent) {
    SQ_STORE(engine);
    if (index < 0 || index >= static_cast<int>(sq.coverage_rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = sq.coverage_rows[static_cast<std::size_t>(index)];
    if (k < 0 || k >= static_cast<int>(r.uses.size())) return SWMM_ERR_BADPARAM;
    if (scope)   *scope   = static_cast<int>(r.key.scope);
    if (cell)    *cell    = r.key.cell;
    if (tag)     copy_to(r.key.tag, tag, taglen);
    if (landuse) copy_to(r.uses[static_cast<std::size_t>(k)].first, landuse, lulen);
    if (percent) *percent = r.uses[static_cast<std::size_t>(k)].second;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_coverage_remove(SWMM_Engine engine, int index) {
    SQ_STORE(engine);
    SQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(sq.coverage_rows.size()))
        return SWMM_ERR_BADPARAM;
    sq.coverage_rows.erase(sq.coverage_rows.begin() + index);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [2D_LOADINGS]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_2d_loading_count(SWMM_Engine engine, int* count) {
    SQ_STORE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(sq.loading_rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_loading_set(SWMM_Engine engine, int scope,
                                        const char* tag, int cell,
                                        const char* species, double value) {
    SQ_STORE(engine);
    SQ_EDITABLE(eng);
    if (!species || !isSurfaceSpecies(eng->context(), species)) return SWMM_ERR_BADPARAM;
    std::vector<std::string> t;
    if (!scopeTokens(scope, tag, cell, t)) return SWMM_ERR_BADPARAM;
    t.emplace_back(species);
    t.push_back(std::to_string(value));
    std::vector<openswmm::twoD::LoadingRow2D> one;
    if (!openswmm::twoD::parse2DLoadingsLine(t, one).empty()) return SWMM_ERR_BADPARAM;
    one.front().value = value;   // the exact double, not its text
    auto& rows = sq.loading_rows;
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const openswmm::twoD::LoadingRow2D& o) {
        return sameKey(o.key, one.front().key) && o.species == one.front().species;
    }), rows.end());
    rows.push_back(std::move(one.front()));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_loading_get(SWMM_Engine engine, int index,
                                        int* scope, char* tag, int taglen,
                                        int* cell, char* species, int splen,
                                        double* value) {
    SQ_STORE(engine);
    if (index < 0 || index >= static_cast<int>(sq.loading_rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = sq.loading_rows[static_cast<std::size_t>(index)];
    if (scope)   *scope = static_cast<int>(r.key.scope);
    if (cell)    *cell  = r.key.cell;
    if (tag)     copy_to(r.key.tag, tag, taglen);
    if (species) copy_to(r.species, species, splen);
    if (value)   *value = r.value;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_loading_remove(SWMM_Engine engine, int index) {
    SQ_STORE(engine);
    SQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(sq.loading_rows.size()))
        return SWMM_ERR_BADPARAM;
    sq.loading_rows.erase(sq.loading_rows.begin() + index);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [2D_CURB_LENGTH]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_2d_curb_length_count(SWMM_Engine engine, int* count) {
    SQ_STORE(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(sq.curb_rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_curb_length_set(SWMM_Engine engine, int scope,
                                            const char* tag, int cell,
                                            double length) {
    SQ_STORE(engine);
    SQ_EDITABLE(eng);
    std::vector<std::string> t;
    if (!scopeTokens(scope, tag, cell, t)) return SWMM_ERR_BADPARAM;
    t.push_back(std::to_string(length));
    std::vector<openswmm::twoD::CurbRow2D> one;
    if (!openswmm::twoD::parse2DCurbLengthLine(t, one).empty()) return SWMM_ERR_BADPARAM;
    one.front().length = length;
    auto& rows = sq.curb_rows;
    rows.erase(std::remove_if(rows.begin(), rows.end(), [&](const openswmm::twoD::CurbRow2D& o) {
        return sameKey(o.key, one.front().key);
    }), rows.end());
    rows.push_back(std::move(one.front()));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_curb_length_get(SWMM_Engine engine, int index,
                                            int* scope, char* tag, int taglen,
                                            int* cell, double* length) {
    SQ_STORE(engine);
    if (index < 0 || index >= static_cast<int>(sq.curb_rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = sq.curb_rows[static_cast<std::size_t>(index)];
    if (scope)  *scope  = static_cast<int>(r.key.scope);
    if (cell)   *cell   = r.key.cell;
    if (tag)    copy_to(r.key.tag, tag, taglen);
    if (length) *length = r.length;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_2d_curb_length_remove(SWMM_Engine engine, int index) {
    SQ_STORE(engine);
    SQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(sq.curb_rows.size()))
        return SWMM_ERR_BADPARAM;
    sq.curb_rows.erase(sq.curb_rows.begin() + index);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// Buildup store
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_2d_get_buildup_bulk(SWMM_Engine engine,
                                             const char* species,
                                             double* out, int n) {
    SQ_STORE(engine);
    if (!species || !out) return SWMM_ERR_BADPARAM;
    if (!sq.active()) return SWMM_ERR_LIFECYCLE;
    if (n < sq.nCells()) return SWMM_ERR_BADPARAM;
    int s = -1;
    for (int i = 0; i < sq.nSpecies(); ++i)
        if (sq.speciesName(i) == species) { s = i; break; }
    if (s < 0) return SWMM_ERR_BADPARAM;
    for (int c = 0; c < sq.nCells(); ++c) out[c] = sq.buildupPerArea(c, s);
    return SWMM_OK;
}
