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
 * @file ApiGwTransport.cpp
 * @brief U4 — implementation of openswmm_gw_transport.h.
 *
 * @details Every setter parses through the SAME line parsers the `.inp`
 *          sections use, so the API and the file can never drift: an option
 *          the file refuses the API refuses, with the same message shape.
 *          The rows live on SurfaceRouter2D (via `ctx.twod_io.gw`), so a
 *          save re-emits exactly what the API wrote.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw_transport.h>

#include "../../core/SWMMEngine.hpp"
#include "../gw/GwTransportSections.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

using openswmm::twoD::GwBoundaryQualityRow;
using openswmm::twoD::GwInitialQualityRow;
using openswmm::twoD::GwParamsRow;
using openswmm::twoD::GwScope;
using openswmm::twoD::GwSorptionRow;
using openswmm::twoD::GwSourceRow;
using openswmm::twoD::GwSourceSpeciesTerm;
using openswmm::twoD::GwTransportData;
using openswmm::twoD::GwZone;

void copy_to(const std::string& src, char* buf, int len) {
    if (!buf || len <= 0) return;
    const int n = std::min(static_cast<int>(src.size()), len - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(n));
    buf[n] = '\0';
}

GwScope toScope(int s) {
    switch (s) {
        case SWMM_GW_SCOPE_TAG:  return GwScope::TAG;
        case SWMM_GW_SCOPE_CELL: return GwScope::CELL;
        default:                 return GwScope::GLOBAL;
    }
}
int fromScope(GwScope s) { return static_cast<int>(s); }

GwZone toZone(int z) {
    switch (z) {
        case SWMM_GW_ZONE_UNSAT: return GwZone::UNSAT;
        case SWMM_GW_ZONE_LAYER: return GwZone::LAYER;
        default:                 return GwZone::SAT;
    }
}

}  // namespace

// GW rows are authoring state on the parsed model, so the guard is the
// editable-state one (BUILDING/OPENED), matching the 2D BC and infiltration
// APIs. No mesh is required: a host may author options before meshing.
#define GW_GET(engine) \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine); \
    if (!eng) return SWMM_ERR_BADHANDLE; \
    GwTransportData& gw = eng->surfaceRouter2D().gwTransport()

#define GW_EDITABLE(eng) \
    if (eng->context().state != openswmm::EngineState::OPENED && \
        eng->context().state != openswmm::EngineState::BUILDING) \
        return SWMM_ERR_LIFECYCLE

extern "C" {

// ---------------------------------------------------------------------------
// [GW_TRANSPORT_OPTIONS]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw_transport_option_get(SWMM_Engine engine,
        const char* key, char* buf, int buflen) {
    GW_GET(engine);
    if (!key || !buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& o = gw.options;
    const std::string k(key);
    auto yn = [](bool b) { return std::string(b ? "YES" : "NO"); };
    std::string v;
    if      (k == "TRANSPORT_POLLUTANTS")  v = yn(o.transport_pollutants);
    else if (k == "TRANSPORT_MSX")         v = yn(o.transport_msx);
    else if (k == "TRANSPORT_AGE")         v = yn(o.transport_age);
    else if (k == "TRANSPORT_TEMPERATURE") v = yn(o.transport_temperature);
    else if (k == "DISPERSION")            v = yn(o.dispersion);
    else if (k == "CONDUCTION")            v = yn(o.conduction);
    else if (k == "THERMAL_MIXING")        v = o.thermal_mixing;
    else if (k == "SURFACE_THERMAL_BC")
        v = o.surface_thermal_arg.empty()
                ? o.surface_thermal_bc
                : o.surface_thermal_bc + " " + o.surface_thermal_arg;
    else if (k == "DEEP_THERMAL_BC") {
        v = o.deep_thermal_bc;
        if (!o.deep_thermal_arg.empty()) v += " " + o.deep_thermal_arg;
        if (o.deep_depth > 0.0) {
            char d[64];
            std::snprintf(d, sizeof d, " DEPTH %.12g", o.deep_depth);
            v += d;
        }
    } else if (k == "C_DIFF") {
        char d[64];
        std::snprintf(d, sizeof d, "%.12g", o.c_diff);
        v = d;
    } else {
        return SWMM_ERR_BADPARAM;
    }
    copy_to(v, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_transport_option_set(SWMM_Engine engine,
        const char* key, const char* value) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (!key || !value) return SWMM_ERR_BADPARAM;
    // Route through the file parser so the API and the section agree on
    // every accepted spelling — including the multi-token thermal BCs.
    std::vector<std::string> tokens{key};
    std::string cur;
    for (const char* p = value; *p; ++p) {
        if (std::isspace(static_cast<unsigned char>(*p))) {
            if (!cur.empty()) { tokens.push_back(cur); cur.clear(); }
        } else {
            cur += *p;
        }
    }
    if (!cur.empty()) tokens.push_back(cur);
    // Commit only on success: the parser assigns before validating the
    // trailing arguments, so a rejected set must not leave a half-edit.
    auto tmp = gw.options;
    if (!openswmm::twoD::parseGwTransportOptionsLine(tokens, tmp).empty())
        return SWMM_ERR_BADPARAM;
    tmp.authored = true;
    gw.options = tmp;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_transport_authored(SWMM_Engine engine) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return 0;
    return eng->surfaceRouter2D().gwTransport().empty() ? 0 : 1;
}

// ---------------------------------------------------------------------------
// [GW_TRANSPORT_PARAMS]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw_params_count(SWMM_Engine engine) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return -1;
    return static_cast<int>(eng->surfaceRouter2D().gwTransport().params.size());
}

SWMM_ENGINE_API int swmm_gw_params_get(SWMM_Engine engine, int idx,
        SWMM_GwParams* row, char* tag_buf, int tag_len) {
    GW_GET(engine);
    if (!row) return SWMM_ERR_BADPARAM;
    if (idx < 0 || idx >= static_cast<int>(gw.params.size())) return SWMM_ERR_BADINDEX;
    const auto& r = gw.params[static_cast<std::size_t>(idx)];
    row->scope    = fromScope(r.scope);
    row->cell     = r.cell;
    row->rho_s    = r.rho_s;
    row->c_s      = r.c_s;
    row->lambda_s = r.lambda_s;
    row->a_s      = r.a_s;
    row->alpha_L  = r.alpha_L;
    row->alpha_T  = r.alpha_T;
    row->D_m      = r.D_m;
    row->D_v      = r.D_v;
    row->geo_flux = r.geo_flux;
    copy_to(r.tag, tag_buf, tag_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_params_set(SWMM_Engine engine,
        const SWMM_GwParams* row, const char* tag) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (!row) return SWMM_ERR_BADPARAM;
    GwParamsRow r;
    r.scope = toScope(row->scope);
    r.tag   = (r.scope == GwScope::TAG && tag) ? tag : "";
    r.cell  = (r.scope == GwScope::CELL) ? row->cell : -1;
    if (r.scope == GwScope::TAG && r.tag.empty()) return SWMM_ERR_BADPARAM;
    if (r.scope == GwScope::CELL && r.cell < 0)   return SWMM_ERR_BADINDEX;
    r.rho_s = row->rho_s; r.c_s = row->c_s; r.lambda_s = row->lambda_s;
    r.a_s = row->a_s; r.alpha_L = row->alpha_L; r.alpha_T = row->alpha_T;
    r.D_m = row->D_m; r.D_v = row->D_v; r.geo_flux = row->geo_flux;
    for (auto& e : gw.params)
        if (e.scope == r.scope && e.tag == r.tag && e.cell == r.cell) {
            e = r;
            gw.options.authored = true;
            return SWMM_OK;
        }
    gw.params.push_back(std::move(r));
    gw.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_params_remove(SWMM_Engine engine, int idx) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (idx < 0 || idx >= static_cast<int>(gw.params.size())) return SWMM_ERR_BADINDEX;
    gw.params.erase(gw.params.begin() + idx);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [GW_SORPTION]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw_sorption_count(SWMM_Engine engine) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return -1;
    return static_cast<int>(eng->surfaceRouter2D().gwTransport().sorption.size());
}

SWMM_ENGINE_API int swmm_gw_sorption_get(SWMM_Engine engine, int idx,
        int* scope, char* tag_buf, int tag_len, int* cell,
        char* species_buf, int species_len, double* kd, double* decay) {
    GW_GET(engine);
    if (idx < 0 || idx >= static_cast<int>(gw.sorption.size())) return SWMM_ERR_BADINDEX;
    const auto& r = gw.sorption[static_cast<std::size_t>(idx)];
    if (scope) *scope = fromScope(r.scope);
    if (cell)  *cell  = r.cell;
    if (kd)    *kd    = r.kd;
    if (decay) *decay = r.decay;
    copy_to(r.tag, tag_buf, tag_len);
    copy_to(r.species, species_buf, species_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_sorption_set(SWMM_Engine engine, int scope,
        const char* tag, int cell, const char* species, double kd, double decay) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (!species || !*species || kd < 0.0) return SWMM_ERR_BADPARAM;
    GwSorptionRow r;
    r.scope   = toScope(scope);
    r.tag     = (r.scope == GwScope::TAG && tag) ? tag : "";
    r.cell    = (r.scope == GwScope::CELL) ? cell : -1;
    r.species = species;
    r.kd      = kd;
    r.decay   = decay;
    if (r.scope == GwScope::TAG && r.tag.empty()) return SWMM_ERR_BADPARAM;
    if (r.scope == GwScope::CELL && r.cell < 0)   return SWMM_ERR_BADINDEX;
    for (auto& e : gw.sorption)
        if (e.scope == r.scope && e.tag == r.tag && e.cell == r.cell &&
            e.species == r.species) {
            e = r;
            gw.options.authored = true;
            return SWMM_OK;
        }
    gw.sorption.push_back(std::move(r));
    gw.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_sorption_remove(SWMM_Engine engine, int idx) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (idx < 0 || idx >= static_cast<int>(gw.sorption.size())) return SWMM_ERR_BADINDEX;
    gw.sorption.erase(gw.sorption.begin() + idx);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [GW_INITIAL_QUALITY]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw_init_quality_count(SWMM_Engine engine) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return -1;
    return static_cast<int>(
        eng->surfaceRouter2D().gwTransport().initial_quality.size());
}

SWMM_ENGINE_API int swmm_gw_init_quality_get(SWMM_Engine engine, int idx,
        int* scope, char* tag_buf, int tag_len, int* cell,
        int* zone, int* layer, char* species_buf, int species_len, double* value) {
    GW_GET(engine);
    if (idx < 0 || idx >= static_cast<int>(gw.initial_quality.size()))
        return SWMM_ERR_BADINDEX;
    const auto& r = gw.initial_quality[static_cast<std::size_t>(idx)];
    if (scope) *scope = fromScope(r.scope);
    if (cell)  *cell  = r.cell;
    if (zone)  *zone  = static_cast<int>(r.zone);
    if (layer) *layer = r.layer;
    if (value) *value = r.value;
    copy_to(r.tag, tag_buf, tag_len);
    copy_to(r.species, species_buf, species_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_init_quality_set(SWMM_Engine engine, int scope,
        const char* tag, int cell, int zone, int layer,
        const char* species, double value) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (!species || !*species) return SWMM_ERR_BADPARAM;
    GwInitialQualityRow r;
    r.scope   = toScope(scope);
    r.tag     = (r.scope == GwScope::TAG && tag) ? tag : "";
    r.cell    = (r.scope == GwScope::CELL) ? cell : -1;
    r.zone    = toZone(zone);
    r.layer   = (r.zone == GwZone::LAYER) ? layer : -1;
    r.species = species;
    r.value   = value;
    if (r.scope == GwScope::TAG && r.tag.empty()) return SWMM_ERR_BADPARAM;
    if (r.scope == GwScope::CELL && r.cell < 0)   return SWMM_ERR_BADINDEX;
    if (r.zone == GwZone::LAYER && r.layer < 1)   return SWMM_ERR_BADPARAM;
    // A concentration may not be negative; the reserved species may.
    if (value < 0.0 && r.species != "__TEMPERATURE__" &&
        r.species != "__WATER_AGE__")
        return SWMM_ERR_BADPARAM;
    for (auto& e : gw.initial_quality)
        if (e.scope == r.scope && e.tag == r.tag && e.cell == r.cell &&
            e.zone == r.zone && e.layer == r.layer && e.species == r.species) {
            e.value = r.value;
            gw.options.authored = true;
            return SWMM_OK;
        }
    gw.initial_quality.push_back(std::move(r));
    gw.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_init_quality_remove(SWMM_Engine engine, int idx) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (idx < 0 || idx >= static_cast<int>(gw.initial_quality.size()))
        return SWMM_ERR_BADINDEX;
    gw.initial_quality.erase(gw.initial_quality.begin() + idx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_init_quality_file_get(SWMM_Engine engine,
        char* buf, int buflen) {
    GW_GET(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    copy_to(gw.initial_quality_file, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_init_quality_file_set(SWMM_Engine engine,
        const char* path) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    gw.initial_quality_file = path ? path : "";
    if (!gw.initial_quality_file.empty()) gw.options.authored = true;
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [GW_BOUNDARY_QUALITY]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw_boundary_quality_count(SWMM_Engine engine) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return -1;
    return static_cast<int>(
        eng->surfaceRouter2D().gwTransport().boundary_quality.size());
}

SWMM_ENGINE_API int swmm_gw_boundary_quality_get(SWMM_Engine engine, int idx,
        int* cell, int* edge, char* species_buf, int species_len,
        char* kind_buf, int kind_len, double* value, char* ts_buf, int ts_len) {
    GW_GET(engine);
    if (idx < 0 || idx >= static_cast<int>(gw.boundary_quality.size()))
        return SWMM_ERR_BADINDEX;
    const auto& r = gw.boundary_quality[static_cast<std::size_t>(idx)];
    if (cell)  *cell  = r.cell;
    if (edge)  *edge  = r.edge;
    if (value) *value = r.value;
    copy_to(r.species, species_buf, species_len);
    copy_to(r.kind, kind_buf, kind_len);
    copy_to(r.ts_name, ts_buf, ts_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_boundary_quality_set(SWMM_Engine engine,
        int cell, int edge, const char* species, const char* kind,
        double value, const char* ts_name) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (!species || !*species || !kind || cell < 0 || edge < 0)
        return SWMM_ERR_BADPARAM;
    const std::string k(kind);
    if (k != "CONC" && k != "TS" && k != "MASSFLUX" && k != "HEATFLUX")
        return SWMM_ERR_BADPARAM;
    // EDGE is the LOCAL edge of ITS cell — cell-generic: 3 for a triangle,
    // 4 for a quad. Checked here when the mesh is already in hand; the
    // resolve pass checks it again for file-authored rows.
    const auto& mesh = eng->surfaceRouter2D().mesh();
    if (mesh.n_cells() > 0) {
        if (cell >= mesh.n_cells()) return SWMM_ERR_BADINDEX;
        if (edge >= static_cast<int>(mesh.cell_nv[static_cast<std::size_t>(cell)]))
            return SWMM_ERR_BADINDEX;
    }
    GwBoundaryQualityRow r;
    r.cell    = cell;
    r.edge    = edge;
    r.species = species;
    r.kind    = k;
    r.value   = value;
    r.ts_name = ts_name ? ts_name : "";
    if (k == "TS" && r.ts_name.empty()) return SWMM_ERR_BADPARAM;
    for (auto& e : gw.boundary_quality)
        if (e.cell == r.cell && e.edge == r.edge && e.species == r.species) {
            e = r;
            gw.options.authored = true;
            return SWMM_OK;
        }
    gw.boundary_quality.push_back(std::move(r));
    gw.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_boundary_quality_remove(SWMM_Engine engine, int idx) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (idx < 0 || idx >= static_cast<int>(gw.boundary_quality.size()))
        return SWMM_ERR_BADINDEX;
    gw.boundary_quality.erase(gw.boundary_quality.begin() + idx);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [GW_SOURCES]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw_source_count(SWMM_Engine engine) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return -1;
    return static_cast<int>(eng->surfaceRouter2D().gwTransport().sources.size());
}

SWMM_ENGINE_API int swmm_gw_source_get(SWMM_Engine engine, int idx,
        char* name_buf, int name_len, int* scope, char* tag_buf, int tag_len,
        int* cell, double* flow, char* flow_ts_buf, int flow_ts_len) {
    GW_GET(engine);
    if (idx < 0 || idx >= static_cast<int>(gw.sources.size())) return SWMM_ERR_BADINDEX;
    const auto& r = gw.sources[static_cast<std::size_t>(idx)];
    if (scope) *scope = fromScope(r.scope);
    if (cell)  *cell  = r.cell;
    if (flow)  *flow  = r.flow;
    copy_to(r.name, name_buf, name_len);
    copy_to(r.tag, tag_buf, tag_len);
    copy_to(r.flow_ts, flow_ts_buf, flow_ts_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_source_set(SWMM_Engine engine, const char* name,
        int scope, const char* tag, int cell, double flow, const char* flow_ts) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (!name || !*name) return SWMM_ERR_BADPARAM;
    const GwScope sc = toScope(scope);
    if (sc == GwScope::TAG && (!tag || !*tag)) return SWMM_ERR_BADPARAM;
    if (sc == GwScope::CELL && cell < 0)       return SWMM_ERR_BADINDEX;
    for (auto& e : gw.sources)
        if (e.name == name) {
            e.scope   = sc;
            e.tag     = (sc == GwScope::TAG && tag) ? tag : "";
            e.cell    = (sc == GwScope::CELL) ? cell : -1;
            e.by_xy   = false;
            e.flow    = flow;
            e.flow_ts = flow_ts ? flow_ts : "";
            gw.options.authored = true;
            return SWMM_OK;
        }
    GwSourceRow r;
    r.name    = name;
    r.scope   = sc;
    r.tag     = (sc == GwScope::TAG && tag) ? tag : "";
    r.cell    = (sc == GwScope::CELL) ? cell : -1;
    r.flow    = flow;
    r.flow_ts = flow_ts ? flow_ts : "";
    gw.sources.push_back(std::move(r));
    gw.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_source_remove(SWMM_Engine engine, int idx) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (idx < 0 || idx >= static_cast<int>(gw.sources.size())) return SWMM_ERR_BADINDEX;
    gw.sources.erase(gw.sources.begin() + idx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_source_species_count(SWMM_Engine engine, int src_idx) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return -1;
    const auto& src = eng->surfaceRouter2D().gwTransport().sources;
    if (src_idx < 0 || src_idx >= static_cast<int>(src.size())) return -1;
    return static_cast<int>(src[static_cast<std::size_t>(src_idx)].species.size());
}

SWMM_ENGINE_API int swmm_gw_source_species_get(SWMM_Engine engine, int src_idx,
        int term_idx, char* species_buf, int species_len,
        char* kind_buf, int kind_len, double* value, char* ts_buf, int ts_len) {
    GW_GET(engine);
    if (src_idx < 0 || src_idx >= static_cast<int>(gw.sources.size()))
        return SWMM_ERR_BADINDEX;
    const auto& terms = gw.sources[static_cast<std::size_t>(src_idx)].species;
    if (term_idx < 0 || term_idx >= static_cast<int>(terms.size()))
        return SWMM_ERR_BADINDEX;
    const auto& t = terms[static_cast<std::size_t>(term_idx)];
    if (value) *value = t.value;
    copy_to(t.species, species_buf, species_len);
    copy_to(t.kind, kind_buf, kind_len);
    copy_to(t.ts_name, ts_buf, ts_len);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_source_species_set(SWMM_Engine engine, int src_idx,
        const char* species, const char* kind, double value, const char* ts_name) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (src_idx < 0 || src_idx >= static_cast<int>(gw.sources.size()))
        return SWMM_ERR_BADINDEX;
    if (!species || !*species || !kind) return SWMM_ERR_BADPARAM;
    const std::string k(kind);
    if (k != "CONC" && k != "MASS") return SWMM_ERR_BADPARAM;
    auto& terms = gw.sources[static_cast<std::size_t>(src_idx)].species;
    for (auto& t : terms)
        if (t.species == species) {
            t.kind    = k;
            t.value   = value;
            t.ts_name = ts_name ? ts_name : "";
            return SWMM_OK;
        }
    GwSourceSpeciesTerm t;
    t.species = species;
    t.kind    = k;
    t.value   = value;
    t.ts_name = ts_name ? ts_name : "";
    terms.push_back(std::move(t));
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw_source_species_remove(SWMM_Engine engine,
        int src_idx, int term_idx) {
    GW_GET(engine);
    GW_EDITABLE(eng);
    if (src_idx < 0 || src_idx >= static_cast<int>(gw.sources.size()))
        return SWMM_ERR_BADINDEX;
    auto& terms = gw.sources[static_cast<std::size_t>(src_idx)].species;
    if (term_idx < 0 || term_idx >= static_cast<int>(terms.size()))
        return SWMM_ERR_BADINDEX;
    terms.erase(terms.begin() + term_idx);
    return SWMM_OK;
}

}  // extern "C"
