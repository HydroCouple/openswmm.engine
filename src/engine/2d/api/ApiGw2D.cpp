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
 * @file      ApiGw2D.cpp
 * @brief     G-step 19 — implementation of `openswmm_gw2d.h`.
 *
 * @details The authoring half edits `SubsurfaceConfig` (project units, never
 *          converted); the state half reads `SubsurfaceState` (SI). Keeping
 *          the two apart in the header is what keeps them apart here: there
 *          is no place in this file where a project-unit number and an SI one
 *          could be added together.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw2d.h>

#include "../../core/SWMMEngine.hpp"
#include "../subsurface/SubsurfaceSections.hpp"
#include "../subsurface/GroundwaterConcentration.hpp"
#include "../subsurface/SubsurfaceSolver.hpp"
#include "../../core/UnitConversion.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <set>
#include <unordered_map>

namespace {

using openswmm::twoD::GwAquiferRow;
using openswmm::twoD::GwClosure;
using openswmm::twoD::GwLinkMode; // G-X4
using openswmm::twoD::GwLinkRow;
using openswmm::twoD::GwNodeBed;
using openswmm::twoD::SoilChar;
using openswmm::twoD::SubsurfaceConfig;
using openswmm::twoD::SubsurfaceSolver;

/**
 * @brief Copy an optional string result with bounded NUL termination.
 * @param[in] src Borrowed source string; not retained.
 * @param[out] buf Optional writable len-byte buffer; NULL skips the copy.
 * @param[in] len Byte capacity including NUL; nonpositive skips copying.
 * @post Copies at most len-1 bytes and terminates when storage/capacity are usable. No
 * allocation or validation of caller capacity.
 */
void copy_to(const std::string& src, char* buf, int len) {
    if (!buf || len <= 0)
        return;
    const int n = std::min(static_cast<int>(src.size()), len - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(n));
    buf[n] = '\0';
}

/**
 * @brief Compare two C strings using bytewise case-insensitive option matching.
 * @param[in] a Optional NUL-terminated first string.
 * @param[in] b Optional NUL-terminated second string.
 * @return True for equal strings ignoring case; false if either pointer is NULL.
 * @note Casts bytes to unsigned char before std::toupper; no allocation or retained input.
 */
bool ieq(const char* a, const char* b) {
    if (!a || !b)
        return false;
    for (; *a && *b; ++a, ++b)
        if (std::toupper(static_cast<unsigned char>(*a)) !=
            std::toupper(static_cast<unsigned char>(*b)))
            return false;
    return *a == '\0' && *b == '\0';
}

} // namespace

// Authoring edits the rows the kernel is BUILT from, so the guard is the
// editable-state one — the same contract as the 2D boundary, infiltration and
// [GW_*] APIs. A mid-run set would be silently dropped at the next
// initialize, and silence is the worst of the three options.
#define AQ_CFG(engine)                                                                             \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);                                   \
    if (!eng)                                                                                      \
        return SWMM_ERR_BADHANDLE;                                                                 \
    SubsurfaceConfig& cfg = eng->surfaceRouter2D().aquiferConfig()

#define AQ_EDITABLE(eng)                                                                           \
    if (eng->context().state != openswmm::EngineState::OPENED &&                                   \
        eng->context().state != openswmm::EngineState::BUILDING)                                   \
    return SWMM_ERR_LIFECYCLE

#define AQ_SOLVER(engine)                                                                          \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);                                   \
    if (!eng)                                                                                      \
        return SWMM_ERR_BADHANDLE;                                                                 \
    const SubsurfaceSolver& gw = eng->surfaceRouter2D().subsurface();                              \
    if (!gw.active())                                                                              \
    return SWMM_ERR_LIFECYCLE

extern "C" {

SWMM_ENGINE_API int swmm_gw2d_get_execution_status(SWMM_Engine engine,
                                                   SWMM_GwExecutionStatus* status) {
    AQ_SOLVER(engine);
    if (!status || status->struct_size != sizeof(*status))
        return SWMM_ERR_BADPARAM;
    SWMM_GwExecutionStatus result{};
    result.struct_size = sizeof(result);
    result.active = 1;
    const auto& backend = gw.rechargeBackend();
    result.available_operators = backend.evaluate ? backend.capabilities.operators : 0;
    result.executed_operators = gw.rechargeCells() > 0 ? SWMM_GW2D_OP_CLOSED_FORM_RECHARGE : 0;
    result.host_owned_operations = 15u;
    result.effective_threads = backend.evaluate ? gw.rechargeThreads() : 0;
    result.accepted_batches = gw.rechargeBatches();
    result.accepted_cells = gw.rechargeCells();
    copy_to(backend.evaluate ? "omp" : "cpu", result.recharge_backend,
            sizeof(result.recharge_backend));
    copy_to(backend.evaluate ? backend.capabilities.label : "CPU groundwater operators",
            result.profile, sizeof(result.profile));
    copy_to(backend.diagnostic, result.detail, sizeof(result.detail));
    *status = result;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_boundary_count(SWMM_Engine engine) {
    AQ_CFG(engine);
    return static_cast<int>(cfg.boundaries.size());
}
SWMM_ENGINE_API int swmm_gw2d_boundary_get(SWMM_Engine engine, int index, SWMM_GwBoundary* row,
                                           char* series, int length) {
    AQ_CFG(engine);
    if (index < 0 || index >= int(cfg.boundaries.size()))
        return SWMM_ERR_BADINDEX;
    if (!row || !series || length <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& b = cfg.boundaries[index];
    if (b.series.size() >= std::size_t(length))
        return SWMM_ERR_BADPARAM;
    *row = {b.cell, b.edge, b.head ? SWMM_GW2D_BOUNDARY_HEAD : SWMM_GW2D_BOUNDARY_FLOW, b.value};
    copy_to(b.series, series, length);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_gw2d_boundary_set(SWMM_Engine engine, const SWMM_GwBoundary* row,
                                           const char* series) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!row || row->cell < 0 || row->edge < 0 || row->edge > 3 || !std::isfinite(row->value) ||
        (row->kind != SWMM_GW2D_BOUNDARY_HEAD && row->kind != SWMM_GW2D_BOUNDARY_FLOW))
        return SWMM_ERR_BADPARAM;
    const std::string name = series ? series : "";
    if (name.find_first_of("\r\n\"") != std::string::npos)
        return SWMM_ERR_BADPARAM;
    openswmm::twoD::GwBoundaryRow replacement{
        row->cell, row->edge, row->kind == SWMM_GW2D_BOUNDARY_HEAD, row->value, name};
    auto next = cfg.boundaries;
    const auto old = std::find_if(next.begin(), next.end(), [&](const auto& b) {
        return b.cell == row->cell && b.edge == row->edge;
    });
    if (old == next.end())
        next.push_back(std::move(replacement));
    else
        *old = std::move(replacement);
    cfg.boundaries.swap(next);
    cfg.options.authored = true;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_gw2d_boundary_remove(SWMM_Engine engine, int index) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (index < 0 || index >= int(cfg.boundaries.size()))
        return SWMM_ERR_BADINDEX;
    cfg.boundaries.erase(cfg.boundaries.begin() + index);
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_gw2d_boundary_water(SWMM_Engine engine, int index, double* requested_in,
                                             double* requested_out, double* applied_in,
                                             double* applied_out) {
    AQ_SOLVER(engine);
    if (index < 0 || index >= int(gw.boundaries().size()))
        return SWMM_ERR_BADINDEX;
    if (!requested_in || !requested_out || !applied_in || !applied_out)
        return SWMM_ERR_BADPARAM;
    const auto& b = gw.boundaries()[index];
    *requested_in = b.requested_in;
    *requested_out = b.requested_out;
    *applied_in = b.applied_in;
    *applied_out = b.applied_out;
    return SWMM_OK;
}
SWMM_ENGINE_API int swmm_gw2d_boundary_species(SWMM_Engine engine, int index, int species,
                                               double* advective_in, double* advective_out,
                                               double* imposed_in, double* imposed_out) {
    AQ_SOLVER(engine);
    if (index < 0 || index >= int(gw.boundaries().size()) || species < 0 ||
        species >= gw.transport().n_species)
        return SWMM_ERR_BADINDEX;
    if (!advective_in || !advective_out || !imposed_in || !imposed_out)
        return SWMM_ERR_BADPARAM;
    const auto& b = gw.boundaries()[index];
    *advective_in = b.mass_in[species];
    *advective_out = b.mass_out[species];
    *imposed_in = b.flux_in[species];
    *imposed_out = b.flux_out[species];
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER_OPTIONS]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_option_get(SWMM_Engine engine, const char* key, char* buf,
                                         int buflen) {
    AQ_CFG(engine);
    if (!key || !buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& o = cfg.options;
    auto yn = [](bool b) { return std::string(b ? "YES" : "NO"); };
    auto num = [](double v) {
        char t[32];
        std::snprintf(t, sizeof t, "%.17g", v);
        return std::string(t);
    };
    std::string v;
    if (ieq(key, "SOIL_CHAR"))
        v = soilCharToken(o.soil_char);
    else if (ieq(key, "CLOSURE"))
        v = gwClosureToken(o.closure);
    else if (ieq(key, "M_LAYERS"))
        v = std::to_string(o.m_layers);
    else if (ieq(key, "CAPILLARY_DIFF"))
        v = yn(o.capillary_diff);
    else if (ieq(key, "C_GW"))
        v = num(o.c_gw);
    else if (ieq(key, "C_COL"))
        v = num(o.c_col);
    else if (ieq(key, "FORCE_CLOSED_FORM"))
        v = yn(o.force_closed_form);
    else if (ieq(key, "MODE"))
        v = o.per_subcatch ? "PER_SUBCATCH" : "MESH";
    else if (ieq(key, "DUNNE"))
        v = yn(o.dunne);
    else if (ieq(key, "GW_ET"))
        v = o.gw_et;
    else if (ieq(key, "WILTING_SUCTION"))
        v = o.wilting_suction_set ? num(o.wilting_suction) : "AUTO";
    else if (ieq(key, "OPTIONS_AUTHORED"))
        v = yn(o.authored);
    else if (ieq(key, "CONFIGURED"))
        v = yn(!cfg.empty());
    else if (ieq(key, "GW_ET_EFFECTIVE"))
        v = eng->surfaceRouter2D().options().groundwater == 0 || cfg.empty() ? "NONE"
            : o.gw_et == "AUTO" ? (o.per_subcatch ? "NONE" : "BOTH")
                                : o.gw_et;
    else if (ieq(key, "LINK_SEEPAGE_EFFECTIVE"))
        v = eng->surfaceRouter2D().options().groundwater == 0 || cfg.empty() ? "NONE"
            : o.link_seepage == GwLinkMode::NONE                             ? "NONE"
            : o.link_seepage == GwLinkMode::TWO_WAY ||
                    (o.link_seepage == GwLinkMode::DEFAULT && !o.per_subcatch)
                ? "TWO_WAY"
                : "ONE_WAY";
    else if (ieq(key, "WILTING_SUCTION_EFFECTIVE"))
        v = num(o.wilting_suction_set ? o.wilting_suction
                                      : 150.0 / (openswmm::ucf::getUnitSystem(static_cast<int>(
                                                     eng->context().options.flow_units)) == 0
                                                     ? .3048
                                                     : 1.0));
    else if (ieq(key, "NODE_ENROLMENT"))
        v = o.node_auto ? "AUTO" : "ROWS"; // G-X2
    else if (ieq(key, "LINK_SEEPAGE"))     // G-X3/G-X4
        v = (o.link_seepage == GwLinkMode::NONE) ? "NONE"
            : (o.link_seepage == GwLinkMode::TWO_WAY)
                ? "TWO_WAY"
                : (o.link_seepage == GwLinkMode::DEFAULT ? "DEFAULT" : "ONE_WAY");
    else
        return SWMM_ERR_BADPARAM;
    copy_to(v, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_process_options_set(SWMM_Engine engine, const char* et,
                                                  const char* link, const char* wilting,
                                                  int authored) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!et || !link || !wilting || (authored != 0 && authored != 1))
        return SWMM_ERR_BADPARAM;
    auto candidate = cfg.options;
    for (const auto& kv : std::vector<std::pair<std::string, std::string>>{
             {"GW_ET", et}, {"LINK_SEEPAGE", link}, {"WILTING_SUCTION", wilting}})
        if (!openswmm::twoD::parseAquiferOptionsLine({kv.first, kv.second}, candidate).empty())
            return SWMM_ERR_BADPARAM;
    candidate.authored = authored != 0;
    cfg.options = std::move(candidate);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_option_set(SWMM_Engine engine, const char* key, const char* value) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!key || !value)
        return SWMM_ERR_BADPARAM;
    // One parser for the file and the API. A second spelling table here is
    // how a host ends up able to set something the .inp cannot express, and
    // then the model does not round-trip.
    const std::vector<std::string> tokens{key, value};
    const std::string err = openswmm::twoD::parseAquiferOptionsLine(tokens, cfg.options);
    if (!err.empty())
        return SWMM_ERR_BADPARAM;
    cfg.options.authored = true;
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER] rows
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_row_count(SWMM_Engine engine, int* count) {
    AQ_CFG(engine);
    if (!count)
        return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(cfg.rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_add(SWMM_Engine engine, int scope, const char* tag, int cell,
                                      double ks, double zs, double theta_s, double theta_r,
                                      double alpha) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (scope < 0 || scope > 2)
        return SWMM_ERR_BADPARAM;
    if (scope == SWMM_GW2D_SCOPE_TAG && (!tag || tag[0] == '\0'))
        return SWMM_ERR_BADPARAM;
    if (scope == SWMM_GW2D_SCOPE_CELL && cell < 0)
        return SWMM_ERR_BADPARAM;
    // The same validity rules the parser enforces, so a host cannot build a
    // model the file would refuse.
    if (!(ks > 0.0) || !(zs > 0.0) || !(alpha > 0.0))
        return SWMM_ERR_BADPARAM;
    if (!(theta_s > 0.0) || theta_s > 1.0)
        return SWMM_ERR_BADPARAM;
    if (theta_r < 0.0 || theta_r >= theta_s)
        return SWMM_ERR_BADPARAM;

    GwAquiferRow r;
    r.scope = scope;
    r.tag = (scope == SWMM_GW2D_SCOPE_TAG) ? tag : "";
    r.cell = (scope == SWMM_GW2D_SCOPE_CELL) ? cell : -1;
    r.Ks = ks;
    r.zs = zs;
    r.theta_s = theta_s;
    r.theta_r = theta_r;
    r.alpha = alpha;
    cfg.rows.push_back(r);
    cfg.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_get(SWMM_Engine engine, int index, int* scope, char* tag,
                                      int taglen, int* cell, double* ks, double* zs,
                                      double* theta_s, double* theta_r, double* alpha) {
    AQ_CFG(engine);
    if (index < 0 || index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = cfg.rows[static_cast<std::size_t>(index)];
    if (scope)
        *scope = r.scope;
    if (cell)
        *cell = r.cell;
    if (ks)
        *ks = r.Ks;
    if (zs)
        *zs = r.zs;
    if (theta_s)
        *theta_s = r.theta_s;
    if (theta_r)
        *theta_r = r.theta_r;
    if (alpha)
        *alpha = r.alpha;
    if (tag)
        copy_to(r.tag, tag, taglen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_set_property(SWMM_Engine engine, int index, const char* key,
                                               double value) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!key || index < 0 || index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    auto& r = cfg.rows[static_cast<std::size_t>(index)];
    if (ieq(key, "PSI_B")) {
        if (value < 0.0)
            return SWMM_ERR_BADPARAM;
        r.psi_b = value;
    } else if (ieq(key, "LAMBDA")) {
        if (value <= 0.0)
            return SWMM_ERR_BADPARAM;
        r.lambda = value;
    } else if (ieq(key, "N")) {
        if (value <= 1.0)
            return SWMM_ERR_BADPARAM;
        r.vg_n = value;
    } else if (ieq(key, "L")) {
        r.vg_L = value;
    } else if (ieq(key, "C_LOSS")) {
        if (value < 0.0)
            return SWMM_ERR_BADPARAM;
        r.c_loss = value;
    } else if (ieq(key, "HG0")) {
        r.hg0 = value;
    } else if (ieq(key, "SOIL_CHAR")) {
        const int c = static_cast<int>(std::lround(value));
        if (c < 0 || c > 3)
            return SWMM_ERR_BADPARAM;
        r.soil_char = static_cast<SoilChar>(c);
        r.soil_char_set = true;
    } else if (ieq(key, "CLOSURE")) {
        const int c = static_cast<int>(std::lround(value));
        if (c < -1 || c > 2)
            return SWMM_ERR_BADPARAM;
        r.closure = static_cast<GwClosure>(c);
        r.closure_set = true;
    } else if (ieq(key, "M_LAYERS")) {
        const int mlayers = static_cast<int>(std::lround(value));
        if (mlayers < 2 || mlayers > 128)
            return SWMM_ERR_BADPARAM;
        r.m_layers = mlayers;
    } else {
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_get_property(SWMM_Engine engine, int index, const char* key,
                                               double* value) {
    AQ_CFG(engine);
    if (!key || !value || index < 0 || index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = cfg.rows[static_cast<std::size_t>(index)];
    if (ieq(key, "PSI_B"))
        *value = r.psi_b;
    else if (ieq(key, "LAMBDA"))
        *value = r.lambda;
    else if (ieq(key, "N"))
        *value = r.vg_n;
    else if (ieq(key, "L"))
        *value = r.vg_L;
    else if (ieq(key, "C_LOSS"))
        *value = r.c_loss;
    else if (ieq(key, "HG0"))
        *value = r.hg0;
    else if (ieq(key, "SOIL_CHAR"))
        *value = static_cast<double>(r.soil_char);
    else if (ieq(key, "CLOSURE"))
        *value = static_cast<double>(r.closure);
    else if (ieq(key, "M_LAYERS"))
        *value = r.m_layers;
    else if (ieq(key, "SOIL_CHAR_SET"))
        *value = r.soil_char_set ? 1.0 : 0.0;
    else if (ieq(key, "CLOSURE_SET"))
        *value = r.closure_set ? 1.0 : 0.0;
    else
        return SWMM_ERR_BADPARAM;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_remove(SWMM_Engine engine, int index) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    cfg.rows.erase(cfg.rows.begin() + index);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER_NODE]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_node_count(SWMM_Engine engine, int* count) {
    AQ_CFG(engine);
    if (!count)
        return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(cfg.node_beds.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_add(SWMM_Engine engine, const char* node, int cell, double kc,
                                       double dc, double area) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!node || node[0] == '\0' || cell < -1)
        return SWMM_ERR_BADPARAM;
    if (!std::isfinite(kc) || !std::isfinite(dc) || !std::isfinite(area) || kc < 0.0 || dc < 0.0 ||
        area < 0.0)
        return SWMM_ERR_BADPARAM;
    if (kc > 0.0 && dc <= 0.0)
        return SWMM_ERR_BADPARAM; // a bed needs a thickness
    GwNodeBed b;
    b.cell = cell;
    b.locate = (cell < 0); // G-X2: −1 = CELL AUTO, located at initialize
    b.Kc = kc;
    b.dC = dc;
    b.area = area;
    cfg.node_beds.push_back(b);
    eng->surfaceRouter2D().aquiferNodeNames().emplace_back(node);
    cfg.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_get(SWMM_Engine engine, int index, char* node, int nodelen,
                                       int* cell, double* kc, double* dc, double* area) {
    AQ_CFG(engine);
    if (index < 0 || index >= static_cast<int>(cfg.node_beds.size()))
        return SWMM_ERR_BADPARAM;
    const auto& b = cfg.node_beds[static_cast<std::size_t>(index)];
    if (cell)
        *cell = b.cell;
    if (kc)
        *kc = b.Kc;
    if (dc)
        *dc = b.dC;
    if (area)
        *area = b.area;
    if (node) {
        const auto& names = eng->surfaceRouter2D().aquiferNodeNames();
        copy_to(index < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(index)]
                                                       : std::string{},
                node, nodelen);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_get_flags(SWMM_Engine engine, int index, int* locate,
                                             int* exchange, int* automatic) {
    AQ_CFG(engine); // G-X2
    if (index < 0 || index >= static_cast<int>(cfg.node_beds.size()))
        return SWMM_ERR_BADPARAM;
    const auto& b = cfg.node_beds[static_cast<std::size_t>(index)];
    if (locate)
        *locate = b.locate ? 1 : 0;
    if (exchange)
        *exchange = b.exchange ? 1 : 0;
    if (automatic)
        *automatic = b.automatic ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_set_exchange(SWMM_Engine engine, int index, int exchange) {
    AQ_CFG(engine); // G-X2
    AQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(cfg.node_beds.size()))
        return SWMM_ERR_BADPARAM;
    cfg.node_beds[static_cast<std::size_t>(index)].exchange = (exchange != 0);
    cfg.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_remove(SWMM_Engine engine, int index) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(cfg.node_beds.size()))
        return SWMM_ERR_BADPARAM;
    cfg.node_beds.erase(cfg.node_beds.begin() + index);
    auto& names = eng->surfaceRouter2D().aquiferNodeNames();
    if (index < static_cast<int>(names.size()))
        names.erase(names.begin() + index);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_replace(SWMM_Engine engine, const SWMM_GwNodeBed* expected,
                                           int expected_count, const SWMM_GwNodeBed* replacement,
                                           int count, const SWMM_LidNodeBottomEdit* bottoms,
                                           int bottom_count, int apply) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (expected_count < 0 || count < 0 || bottom_count < 0 || apply < 0 || apply > 1 ||
        (expected_count && !expected) || (count && !replacement) || (bottom_count && !bottoms))
        return SWMM_ERR_BADPARAM;
    const auto& names = eng->surfaceRouter2D().aquiferNodeNames();
    auto matches = [&](const SWMM_GwNodeBed& r, std::size_t i) {
        const auto& b = cfg.node_beds[i];
        return r.node && i < names.size() && names[i] == r.node && r.cell == b.cell &&
               r.locate == int(b.locate) && r.exchange == int(b.exchange) &&
               r.automatic == int(b.automatic) && r.kc == b.Kc && r.dc == b.dC && r.area == b.area;
    };
    if (expected_count != static_cast<int>(cfg.node_beds.size()))
        return SWMM_ERR_BADPARAM;
    for (int i = 0; i < expected_count; ++i)
        if (!matches(expected[i], i))
            return SWMM_ERR_BADPARAM;
    try {
        auto& c = eng->context();
        std::vector<GwNodeBed> beds;
        std::vector<std::string> next_names;
        std::set<int> edited;
        std::unordered_map<int, std::size_t> bed_index;
        std::unordered_map<std::string, std::size_t> automatic_index;
        for (std::size_t i = 0; i < cfg.node_beds.size(); ++i)
            if (cfg.node_beds[i].automatic && i < names.size())
                automatic_index.emplace(names[i], i);
        std::unordered_map<int, openswmm::LidNodeBottom> modes;
        beds.reserve(count);
        next_names.reserve(count);
        modes.reserve(bottom_count);
        for (int i = 0; i < count; ++i) {
            const auto& r = replacement[i];
            if (!r.node || !r.node[0] || r.cell < -1 || (!r.locate && r.cell < 0) || r.locate < 0 ||
                r.locate > 1 || r.exchange < 0 || r.exchange > 1 || r.automatic < 0 ||
                r.automatic > 1 || !std::isfinite(r.kc) || !std::isfinite(r.dc) ||
                !std::isfinite(r.area) || r.kc < 0 || r.dc < 0 || r.area < 0 ||
                (r.kc > 0 && r.dc <= 0))
                return SWMM_ERR_BADPARAM;
            const int node = c.node_names.find(r.node);
            if (node < 0 || !bed_index.emplace(node, beds.size()).second)
                return SWMM_ERR_BADPARAM;
            if (r.automatic) {
                const auto old = automatic_index.find(r.node);
                if (old == automatic_index.end() || !matches(r, old->second))
                    return SWMM_ERR_BADPARAM;
            }
            GwNodeBed b;
            b.node = node;
            b.cell = r.cell;
            b.locate = r.locate;
            b.exchange = r.exchange;
            b.automatic = r.automatic;
            b.Kc = r.kc;
            b.dC = r.dc;
            b.area = r.area;
            beds.push_back(b);
            next_names.emplace_back(r.node);
        }
        for (int i = 0; i < bottom_count; ++i) {
            const auto& e = bottoms[i];
            if (e.node < 0 || e.node >= c.n_nodes() || e.mode < 0 || e.mode > 2 ||
                !edited.insert(e.node).second)
                return SWMM_ERR_BADPARAM;
            const int row = c.node_subtypes.storage_row(e.node);
            if (row < 0 || c.node_subtypes.storages.lid[row].control < 0 ||
                static_cast<int>(c.node_subtypes.storages.lid[row].bottom) != e.expected_mode)
                return SWMM_ERR_BADPARAM;
            modes.emplace(row, static_cast<openswmm::LidNodeBottom>(e.mode));
        }
        const auto& st = c.node_subtypes.storages;
        for (int row = 0; row < st.count(); ++row) {
            auto mode = st.lid[row].bottom;
            const auto edit = modes.find(row);
            if (edit != modes.end())
                mode = edit->second;
            if (st.lid[row].control < 0 || mode != openswmm::LidNodeBottom::MeshAquifer)
                continue;
            if (cfg.empty() || cfg.options.per_subcatch ||
                eng->surfaceRouter2D().options().groundwater == 0 || c.options.ignore_2d ||
                c.options.ignore_routing || eng->surfaceRouter2D().mesh().n_cells() == 0)
                return SWMM_ERR_BADPARAM;
            const auto binding = bed_index.find(st.node_idx[row]);
            if (binding == bed_index.end())
                return SWMM_ERR_BADPARAM;
            const auto& b = beds[binding->second];
            if (!b.exchange || b.automatic || b.area <= 0)
                return SWMM_ERR_BADPARAM;
        }
        bool changed = count != expected_count;
        for (int i = 0; !changed && i < count; ++i)
            changed = !matches(replacement[i], i);
        if (!apply)
            return SWMM_OK;
        // All fallible validation/allocation precedes this publication seam.
        cfg.node_beds.swap(beds);
        eng->surfaceRouter2D().aquiferNodeNames().swap(next_names);
        for (const auto& edit : modes)
            c.node_subtypes.storages.lid[edit.first].bottom = edit.second;
        if (changed)
            cfg.options.authored = true;
        return SWMM_OK;
    } catch (...) {
        return SWMM_ERR_INTERNAL;
    }
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER_LINKS]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_link_count(SWMM_Engine engine, int* count) {
    AQ_CFG(engine);
    if (!count)
        return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(cfg.link_rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_link_add(SWMM_Engine engine, const char* link, double kc, double dc,
                                       int exchange) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!link || link[0] == '\0')
        return SWMM_ERR_BADPARAM;
    if (!(kc >= 0.0) || !(dc >= 0.0))
        return SWMM_ERR_BADPARAM; // parser rule
    GwLinkRow r;
    r.Kc = kc;
    r.dC = dc;
    r.exchange = (exchange != 0);
    cfg.link_rows.push_back(r);
    eng->surfaceRouter2D().aquiferLinkNames().emplace_back(link);
    cfg.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_link_get(SWMM_Engine engine, int index, char* link, int linklen,
                                       double* kc, double* dc, int* exchange) {
    AQ_CFG(engine);
    if (index < 0 || index >= static_cast<int>(cfg.link_rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = cfg.link_rows[static_cast<std::size_t>(index)];
    if (kc)
        *kc = r.Kc;
    if (dc)
        *dc = r.dC;
    if (exchange)
        *exchange = r.exchange ? 1 : 0;
    if (link) {
        const auto& names = eng->surfaceRouter2D().aquiferLinkNames();
        copy_to(index < static_cast<int>(names.size()) ? names[static_cast<std::size_t>(index)]
                                                       : std::string{},
                link, linklen);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_link_remove(SWMM_Engine engine, int index) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (index < 0 || index >= static_cast<int>(cfg.link_rows.size()))
        return SWMM_ERR_BADPARAM;
    cfg.link_rows.erase(cfg.link_rows.begin() + index);
    auto& names = eng->surfaceRouter2D().aquiferLinkNames();
    if (index < static_cast<int>(names.size()))
        names.erase(names.begin() + index);
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// Running state
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_is_active(SWMM_Engine engine, int* active) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng)
        return SWMM_ERR_BADHANDLE;
    if (!active)
        return SWMM_ERR_BADPARAM;
    *active = eng->surfaceRouter2D().subsurface().active() ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_dimensions(SWMM_Engine engine, int* n_cells, int* m_layers) {
    AQ_SOLVER(engine);
    if (n_cells)
        *n_cells = gw.state().n_cells;
    if (m_layers)
        *m_layers = gw.state().m_layers;
    return SWMM_OK;
}

namespace {

/**
 * @brief Read one unchecked scalar groundwater state selector.
 * @param[in] st Borrowed initialized state with all selected arrays sized for u.
 * @param[in] u Zero-based valid covered-cell offset; bounds are not checked here.
 * @param[in] var SWMM_GW2D_VAR_* selector; units/signs follow the public enum definitions.
 * @param[out] ok Set true for a recognized selector, false for an unknown code.
 * @return Raw state value, including NaN if present; zero for an unknown selector.
 * @note Caller validates the cell and decides whether nonfinite output is permitted. Read-only.
 */
double cellVar(const openswmm::twoD::SubsurfaceState& st, std::size_t u, int var, bool& ok) {
    ok = true;
    switch (var) {
    case SWMM_GW2D_VAR_HG:
        return st.hg[u];
    case SWMM_GW2D_VAR_HU:
        return st.hu[u];
    case SWMM_GW2D_VAR_TABLE_EL:
        return st.z_bed[u] + st.hg[u];
    case SWMM_GW2D_VAR_Q0:
        return st.q0_last[u];
    case SWMM_GW2D_VAR_QLAT:
        return st.qlat_last[u];
    case SWMM_GW2D_VAR_QNODE:
        return st.qnode_last[u];
    case SWMM_GW2D_VAR_QDEEP:
        return st.qdeep_last[u];
    case SWMM_GW2D_VAR_QET:
        return st.qet_last[u];
    case SWMM_GW2D_VAR_DUNNE:
        return st.dunne_last[u];
    case SWMM_GW2D_VAR_QPLUS:
        return st.qplus_last[u];
    case SWMM_GW2D_VAR_DT_CELL:
        return st.dt_cell[u];
    case SWMM_GW2D_VAR_TIER:
        return static_cast<double>(st.tier[u]);
    case SWMM_GW2D_VAR_CLOSURE:
        return static_cast<double>(st.closure[u]);
    case SWMM_GW2D_VAR_QLINK:
        return st.qlink_last[u]; // G-X3
    case SWMM_GW2D_VAR_INFIL_CAPACITY:
        return st.infil_capacity[u];
    case SWMM_GW2D_VAR_INFIL_REMAINING:
        return st.infil_remaining[u];
    case SWMM_GW2D_VAR_INFIL_REFRESH:
        return st.infil_refresh[u];
    case SWMM_GW2D_VAR_INFIL_PENDING:
        return st.xacc_from_surface[u];
    case SWMM_GW2D_VAR_REJECT:
        return st.reject_last[u];
    case SWMM_GW2D_VAR_REJECT_CUM:
        return st.reject_cumulative[u];
    case SWMM_GW2D_VAR_DUNNE_CUM:
        return st.dunne_cumulative[u];
    case SWMM_GW2D_VAR_ET_PENDING:
        return st.et_pending[u];
    case SWMM_GW2D_VAR_ET_POTENTIAL_CUM:
        return st.et_potential_cumulative[u];
    case SWMM_GW2D_VAR_ET_SURFACE_CUM:
        return st.et_surface_cumulative[u];
    case SWMM_GW2D_VAR_ET_SOIL_CUM:
        return st.et_soil_cumulative[u];
    case SWMM_GW2D_VAR_ET_UNUSED_CUM:
        return st.et_unused_cumulative[u];
    case SWMM_GW2D_VAR_ET_SURFACE:
        return st.et_surface_last[u];
    case SWMM_GW2D_VAR_ET_POTENTIAL:
        return st.et_potential_last[u];
    case SWMM_GW2D_VAR_ET_STRESS:
        return st.et_stress[u];
    case SWMM_GW2D_VAR_ET_REFRESH:
        return st.et_refresh[u];
    default:
        ok = false;
        return 0.0;
    }
}

} // namespace

SWMM_ENGINE_API int swmm_gw2d_get_cell(SWMM_Engine engine, int cell, int var, double* value) {
    AQ_SOLVER(engine);
    const auto& st = gw.state();
    if (!value || cell < 0 || cell >= st.n_cells)
        return SWMM_ERR_BADPARAM;
    bool ok = false;
    const double v = cellVar(st, static_cast<std::size_t>(cell), var, ok);
    if (!ok)
        return SWMM_ERR_BADPARAM;
    if (!std::isfinite(v))
        return SWMM_ERR_LIFECYCLE;
    *value = v;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_cell_bulk(SWMM_Engine engine, int var, double* out, int len,
                                            int* written) {
    AQ_SOLVER(engine);
    const auto& st = gw.state();
    if (!out || len < st.n_cells)
        return SWMM_ERR_BADPARAM;
    bool ok = true;
    for (int i = 0; i < st.n_cells && ok; ++i)
        out[i] = cellVar(st, static_cast<std::size_t>(i), var, ok);
    if (!ok)
        return SWMM_ERR_BADPARAM;
    if (written)
        *written = st.n_cells;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_column(SWMM_Engine engine, int cell, double* theta, int len,
                                         int* written) {
    AQ_SOLVER(engine);
    const auto& st = gw.state();
    if (!theta || cell < 0 || cell >= st.n_cells)
        return SWMM_ERR_BADPARAM;
    if (len < st.m_layers)
        return SWMM_ERR_BADPARAM;
    // Refuse rather than return zeros: a caller that reads a closure-A cell's
    // "column" would see a bone-dry profile and have no way to tell it apart
    // from a real one.
    if (st.closure[static_cast<std::size_t>(cell)] != static_cast<int8_t>(GwClosure::SIGMA))
        return SWMM_ERR_BADPARAM;
    for (int j = 0; j < st.m_layers; ++j)
        theta[j] =
            st.theta_sigma[static_cast<std::size_t>(j) * static_cast<std::size_t>(st.n_cells) +
                           static_cast<std::size_t>(cell)];
    if (written)
        *written = st.m_layers;
    return SWMM_OK;
}

namespace {

// G-O: the storage views live on SubsurfaceState (the results file and the
// report read the same numbers); the reasoning is on the methods there and
// on swmm_gw2d_get_continuity_error below.
/**
 * @brief Read the canonical live water holdings.
 * @param[in] gw Borrowed initialized subsurface solver with valid state.
 * @return Domain storage in m3; delegates to SubsurfaceState::liveStorage(). No mutation or
 * allocation.
 */
double liveStorage(const SubsurfaceSolver& gw) {
    return gw.state().liveStorage();
}
/**
 * @brief Read the canonical water storage matched to cumulative firing ledgers.
 * @param[in] gw Borrowed initialized subsurface solver with valid state.
 * @return Domain storage in m3; delegates to SubsurfaceState::ledgeredStorage(). No mutation or
 * allocation.
 */
double ledgeredStorage(const SubsurfaceSolver& gw) {
    return gw.state().ledgeredStorage();
}

} // namespace

SWMM_ENGINE_API int swmm_gw2d_get_ledger(SWMM_Engine engine, int term, double* value) {
    AQ_SOLVER(engine);
    if (!value)
        return SWMM_ERR_BADPARAM;
    const auto& st = gw.state();
    switch (term) {
    case SWMM_GW2D_LED_RECHARGE:
        *value = st.led_recharge;
        break;
    case SWMM_GW2D_LED_LATERAL:
        *value = st.led_lateral;
        break;
    case SWMM_GW2D_LED_DEEP:
        *value = st.led_deep;
        break;
    case SWMM_GW2D_LED_NODE:
        *value = st.led_node;
        break;
    case SWMM_GW2D_LED_REJECT:
        *value = st.led_reject;
        break;
    case SWMM_GW2D_LED_DUNNE:
        *value = st.led_dunne;
        break;
    case SWMM_GW2D_LED_CAPRISE:
        *value = st.led_caprise;
        break;
    case SWMM_GW2D_LED_ET:
        *value = st.led_et;
        break;
    case SWMM_GW2D_LED_INFIL_IN:
        *value = st.led_infil_in;
        break;
    case SWMM_GW2D_LED_INIT_STORAGE:
        *value = st.led_init_storage;
        break;
    case SWMM_GW2D_LED_STORAGE:
        *value = liveStorage(gw);
        break;
    case SWMM_GW2D_LED_SOURCE_IN:
        *value = st.led_source_in;
        break;
    case SWMM_GW2D_LED_SOURCE_OUT:
        *value = st.led_source_out;
        break;
    case SWMM_GW2D_LED_BOUNDARY_IN:
        *value = st.led_boundary_in;
        break;
    case SWMM_GW2D_LED_BOUNDARY_OUT:
        *value = st.led_boundary_out;
        break;
    case SWMM_GW2D_LED_LINK:
        *value = st.led_link;
        break; // G-X3
    default:
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// T7.5 — the transported tuple, read-only
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_species_count(SWMM_Engine engine, int* count) {
    AQ_SOLVER(engine);
    if (!count)
        return SWMM_ERR_BADPARAM;
    const auto& tr = gw.transport();
    *count = tr.active() ? tr.n_species : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_species_name(SWMM_Engine engine, int species, char* buf, int buflen) {
    AQ_SOLVER(engine);
    if (!buf || buflen <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& tr = gw.transport();
    if (species < 0 || species >= tr.n_species)
        return SWMM_ERR_BADINDEX;
    copy_to(tr.row_names[static_cast<std::size_t>(species)], buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_cell_conc(SWMM_Engine engine, int zone, int species, double* out,
                                            int len, int* written) {
    AQ_SOLVER(engine);
    if (!out || len <= 0)
        return SWMM_ERR_BADPARAM;
    if (zone != SWMM_GW2D_ZONE_SAT && zone != SWMM_GW2D_ZONE_UNSAT)
        return SWMM_ERR_BADPARAM;
    const auto& tr = gw.transport();
    if (!tr.active() || species < 0 || species >= tr.n_species)
        return SWMM_ERR_BADINDEX;
    const int n = std::min(len, tr.n_cells);
    for (int c = 0; c < n; ++c) {
        out[c] = openswmm::twoD::groundwaterZoneConcentration(gw, c, species,
                                                              zone == SWMM_GW2D_ZONE_SAT);
    }
    if (written)
        *written = n;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_layer_quality(SWMM_Engine engine, int cell, int layer,
                                                int species, double* water, double* mass,
                                                double* concentration) {
    AQ_SOLVER(engine);
    if (!water || !mass || !concentration)
        return SWMM_ERR_BADPARAM;
    const auto& tr = gw.transport();
    const auto& st = gw.state();
    if (tr.n_layers == 0)
        return SWMM_ERR_BADPARAM;
    if (!gw.covers(cell) || layer < 0 || layer >= tr.n_layers || species < 0 ||
        species >= tr.n_species)
        return SWMM_ERR_BADINDEX;
    const double bulk = st.area[cell] * (st.zs[cell] - st.hg[cell]) / tr.n_layers;
    const double volume = bulk * st.theta_sigma[std::size_t(layer) * st.n_cells + cell];
    const double stored = tr.layer_mass[tr.layerIdx(species, layer, cell)];
    const double partition = (species == tr.age_row || species == tr.temp_row)
                                 ? 0
                                 : tr.kd[tr.idx(species, cell)] * tr.rho_b[cell];
    const double capacity = volume + partition * bulk;
    *water = volume;
    *mass = stored;
    *concentration = capacity > 0 ? stored / capacity : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_species_ledger(SWMM_Engine engine, int species, int term,
                                                 double* value) {
    AQ_SOLVER(engine);
    if (!value)
        return SWMM_ERR_BADPARAM;
    const auto& tr = gw.transport();
    if (!tr.active() || species < 0 || species >= tr.n_species)
        return SWMM_ERR_BADINDEX;
    const auto u = static_cast<std::size_t>(species);
    switch (term) {
    case SWMM_GW2D_SPL_INIT:
        *value = tr.init_mass[u];
        break;
    case SWMM_GW2D_SPL_STORAGE:
        *value = tr.ledgeredStorage(species);
        break;
    case SWMM_GW2D_SPL_INFIL_IN:
        *value = tr.gained_infil[u];
        break;
    case SWMM_GW2D_SPL_NODE_IN:
        *value = tr.gained_node[u];
        break;
    case SWMM_GW2D_SPL_LINK_IN:
        *value = tr.gained_link[u];
        break;
    case SWMM_GW2D_SPL_LATERAL_NET:
        *value = tr.net_lateral[u];
        break;
    case SWMM_GW2D_SPL_DEEP_OUT:
        *value = tr.lost_deep[u];
        break;
    case SWMM_GW2D_SPL_NODE_OUT:
        *value = tr.lost_node[u];
        break;
    case SWMM_GW2D_SPL_LINK_OUT:
        *value = tr.lost_link[u];
        break;
    case SWMM_GW2D_SPL_DUNNE_OUT:
        *value = tr.lost_dunne[u];
        break;
    case SWMM_GW2D_SPL_ET_OUT:
        *value = tr.lost_et[u];
        break;
    case SWMM_GW2D_SPL_REACTED:
        *value = tr.lost_reaction[u];
        break;
    case SWMM_GW2D_SPL_SOURCE_IN:
        *value = tr.gained_source[u];
        break;
    case SWMM_GW2D_SPL_SOURCE_OUT:
        *value = tr.lost_source[u];
        break;
    case SWMM_GW2D_SPL_BOUNDARY_IN:
        *value = tr.boundary_in[u];
        break;
    case SWMM_GW2D_SPL_BOUNDARY_OUT:
        *value = tr.boundary_out[u];
        break;
    case SWMM_GW2D_SPL_BOUNDARY_FLUX_IN:
        *value = tr.boundary_flux_in[u];
        break;
    case SWMM_GW2D_SPL_BOUNDARY_FLUX_OUT:
        *value = tr.boundary_flux_out[u];
        break;
    case SWMM_GW2D_SPL_AGE_GENERATED:
        *value = tr.generated_age.empty() ? 0 : tr.generated_age[u];
        break;
    case SWMM_GW2D_SPL_RESIDUAL:
        *value = tr.residual(species);
        break;
    default:
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_continuity_error(SWMM_Engine engine, double* value) {
    AQ_SOLVER(engine);
    if (!value)
        return SWMM_ERR_BADPARAM;
    const auto& st = gw.state();
    // In: infiltration from the surface, and lateral inflow across the domain
    // boundary (net, already signed). Out: deep loss, node exchange, ET and
    // saturation excess handed back to the surface. Recharge and capillary
    // rise are INTERNAL — they move water between the two zones of the same
    // cell — so they do not appear here, and a residual that scales with them
    // means the handover is wrong, not the boundary.
    //
    // The storage term is `ledgeredStorage`, NOT `liveStorage`, and the
    // difference is the whole correctness of this number. Every ledger term is
    // written at the cell's own firing, so the two surface accumulators are
    // always a step out of phase with it:
    //
    //   xacc_from_surface  water the surface has handed over, which
    //                      `led_infil_in` does not count until the cell
    //                      gathers it;
    //   xacc_to_surface    water `led_dunne` counted as OUT the moment it was
    //                      pushed, which the surface has not drained yet.
    //
    // Write out storage_now = state + eacc + P_in + P_out and the true
    // inflow = led_infil_in + P_in, true outflow = led_dunne − P_out, and both
    // pending terms cancel exactly — leaving state + eacc against the ledger
    // as written. Using holdings here instead reports a leak of P_in + P_out,
    // which on a steadily infiltrating deck is a permanent non-zero residual
    // for a kernel that is conserving to machine precision.
    (void)st;
    *value = gw.state().continuityResidual(); // G-O: one definition, shared
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_tier_histogram(SWMM_Engine engine, long* out, int len,
                                                 int* written) {
    AQ_SOLVER(engine);
    if (!out || len <= 0)
        return SWMM_ERR_BADPARAM;
    const auto& f = gw.tierFirings();
    const int n = std::min(len, static_cast<int>(f.size()));
    for (int i = 0; i < n; ++i)
        out[i] = f[static_cast<std::size_t>(i)];
    if (written)
        *written = n;
    return SWMM_OK;
}

} // extern "C"
