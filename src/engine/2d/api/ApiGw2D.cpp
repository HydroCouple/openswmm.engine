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
 * @file ApiGw2D.cpp
 * @brief G-step 19 — implementation of `openswmm_gw2d.h`.
 *
 * @details The authoring half edits `SubsurfaceConfig` (project units, never
 *          converted); the state half reads `SubsurfaceState` (SI). Keeping
 *          the two apart in the header is what keeps them apart here: there
 *          is no place in this file where a project-unit number and an SI one
 *          could be added together.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw2d.h>

#include "../../core/SWMMEngine.hpp"
#include "../subsurface/SubsurfaceSections.hpp"
#include "../subsurface/SubsurfaceSolver.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>

namespace {

using openswmm::twoD::GwAquiferRow;
using openswmm::twoD::GwClosure;
using openswmm::twoD::GwNodeBed;
using openswmm::twoD::SoilChar;
using openswmm::twoD::SubsurfaceConfig;
using openswmm::twoD::SubsurfaceSolver;

void copy_to(const std::string& src, char* buf, int len) {
    if (!buf || len <= 0) return;
    const int n = std::min(static_cast<int>(src.size()), len - 1);
    std::memcpy(buf, src.c_str(), static_cast<std::size_t>(n));
    buf[n] = '\0';
}

bool ieq(const char* a, const char* b) {
    if (!a || !b) return false;
    for (; *a && *b; ++a, ++b)
        if (std::toupper(static_cast<unsigned char>(*a)) !=
            std::toupper(static_cast<unsigned char>(*b))) return false;
    return *a == '\0' && *b == '\0';
}

}  // namespace

// Authoring edits the rows the kernel is BUILT from, so the guard is the
// editable-state one — the same contract as the 2D boundary, infiltration and
// [GW_*] APIs. A mid-run set would be silently dropped at the next
// initialize, and silence is the worst of the three options.
#define AQ_CFG(engine) \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine); \
    if (!eng) return SWMM_ERR_BADHANDLE; \
    SubsurfaceConfig& cfg = eng->surfaceRouter2D().aquiferConfig()

#define AQ_EDITABLE(eng) \
    if (eng->context().state != openswmm::EngineState::OPENED && \
        eng->context().state != openswmm::EngineState::BUILDING) \
        return SWMM_ERR_LIFECYCLE

#define AQ_SOLVER(engine) \
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine); \
    if (!eng) return SWMM_ERR_BADHANDLE; \
    const SubsurfaceSolver& gw = eng->surfaceRouter2D().subsurface(); \
    if (!gw.active()) return SWMM_ERR_LIFECYCLE

extern "C" {

// ---------------------------------------------------------------------------
// [2D_AQUIFER_OPTIONS]
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_option_get(SWMM_Engine engine, const char* key,
                                         char* buf, int buflen) {
    AQ_CFG(engine);
    if (!key || !buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& o = cfg.options;
    auto yn = [](bool b) { return std::string(b ? "YES" : "NO"); };
    auto num = [](double v) {
        char t[32];
        std::snprintf(t, sizeof t, "%.6g", v);
        return std::string(t);
    };
    std::string v;
    if      (ieq(key, "SOIL_CHAR"))       v = soilCharToken(o.soil_char);
    else if (ieq(key, "CLOSURE"))         v = gwClosureToken(o.closure);
    else if (ieq(key, "M_LAYERS"))        v = std::to_string(o.m_layers);
    else if (ieq(key, "CAPILLARY_DIFF"))  v = yn(o.capillary_diff);
    else if (ieq(key, "C_GW"))            v = num(o.c_gw);
    else if (ieq(key, "C_COL"))           v = num(o.c_col);
    else if (ieq(key, "FORCE_CLOSED_FORM")) v = yn(o.force_closed_form);
    else if (ieq(key, "MODE"))            v = o.per_subcatch ? "PER_SUBCATCH" : "MESH";
    else if (ieq(key, "DUNNE"))           v = yn(o.dunne);
    else if (ieq(key, "GW_ET"))           v = o.gw_et;
    else return SWMM_ERR_BADPARAM;
    copy_to(v, buf, buflen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_option_set(SWMM_Engine engine, const char* key,
                                         const char* value) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!key || !value) return SWMM_ERR_BADPARAM;
    // One parser for the file and the API. A second spelling table here is
    // how a host ends up able to set something the .inp cannot express, and
    // then the model does not round-trip.
    const std::vector<std::string> tokens{key, value};
    const std::string err =
        openswmm::twoD::parseAquiferOptionsLine(tokens, cfg.options);
    if (!err.empty()) return SWMM_ERR_BADPARAM;
    cfg.options.authored = true;
    return SWMM_OK;
}

// ---------------------------------------------------------------------------
// [2D_AQUIFER] rows
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_row_count(SWMM_Engine engine, int* count) {
    AQ_CFG(engine);
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(cfg.rows.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_add(SWMM_Engine engine, int scope,
                                      const char* tag, int cell,
                                      double ks, double zs,
                                      double theta_s, double theta_r,
                                      double alpha) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (scope < 0 || scope > 2) return SWMM_ERR_BADPARAM;
    if (scope == SWMM_GW2D_SCOPE_TAG && (!tag || tag[0] == '\0'))
        return SWMM_ERR_BADPARAM;
    if (scope == SWMM_GW2D_SCOPE_CELL && cell < 0) return SWMM_ERR_BADPARAM;
    // The same validity rules the parser enforces, so a host cannot build a
    // model the file would refuse.
    if (!(ks > 0.0) || !(zs > 0.0) || !(alpha > 0.0)) return SWMM_ERR_BADPARAM;
    if (!(theta_s > 0.0) || theta_s > 1.0) return SWMM_ERR_BADPARAM;
    if (theta_r < 0.0 || theta_r >= theta_s) return SWMM_ERR_BADPARAM;

    GwAquiferRow r;
    r.scope   = scope;
    r.tag     = (scope == SWMM_GW2D_SCOPE_TAG) ? tag : "";
    r.cell    = (scope == SWMM_GW2D_SCOPE_CELL) ? cell : -1;
    r.Ks      = ks;
    r.zs      = zs;
    r.theta_s = theta_s;
    r.theta_r = theta_r;
    r.alpha   = alpha;
    cfg.rows.push_back(r);
    cfg.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_get(SWMM_Engine engine, int index,
                                      int* scope, char* tag, int taglen,
                                      int* cell, double* ks, double* zs,
                                      double* theta_s, double* theta_r,
                                      double* alpha) {
    AQ_CFG(engine);
    if (index < 0 || index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = cfg.rows[static_cast<std::size_t>(index)];
    if (scope)   *scope   = r.scope;
    if (cell)    *cell    = r.cell;
    if (ks)      *ks      = r.Ks;
    if (zs)      *zs      = r.zs;
    if (theta_s) *theta_s = r.theta_s;
    if (theta_r) *theta_r = r.theta_r;
    if (alpha)   *alpha   = r.alpha;
    if (tag)     copy_to(r.tag, tag, taglen);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_set_property(SWMM_Engine engine, int index,
                                               const char* key, double value) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!key || index < 0 || index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    auto& r = cfg.rows[static_cast<std::size_t>(index)];
    if      (ieq(key, "PSI_B"))  { if (value < 0.0)  return SWMM_ERR_BADPARAM; r.psi_b = value; }
    else if (ieq(key, "LAMBDA")) { if (value <= 0.0) return SWMM_ERR_BADPARAM; r.lambda = value; }
    else if (ieq(key, "N"))      { if (value <= 1.0) return SWMM_ERR_BADPARAM; r.vg_n = value; }
    else if (ieq(key, "L"))      { r.vg_L = value; }
    else if (ieq(key, "C_LOSS")) { if (value < 0.0)  return SWMM_ERR_BADPARAM; r.c_loss = value; }
    else if (ieq(key, "HG0"))    { r.hg0 = value; }
    else if (ieq(key, "SOIL_CHAR")) {
        const int c = static_cast<int>(std::lround(value));
        if (c < 0 || c > 3) return SWMM_ERR_BADPARAM;
        r.soil_char = static_cast<SoilChar>(c);
        r.soil_char_set = true;
    } else if (ieq(key, "CLOSURE")) {
        const int c = static_cast<int>(std::lround(value));
        if (c < -1 || c > 2) return SWMM_ERR_BADPARAM;
        r.closure = static_cast<GwClosure>(c);
        r.closure_set = true;
    } else if (ieq(key, "M_LAYERS")) {
        const int mlayers = static_cast<int>(std::lround(value));
        if (mlayers < 2 || mlayers > 128) return SWMM_ERR_BADPARAM;
        r.m_layers = mlayers;
    } else {
        return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_row_get_property(SWMM_Engine engine, int index,
                                               const char* key, double* value) {
    AQ_CFG(engine);
    if (!key || !value || index < 0 ||
        index >= static_cast<int>(cfg.rows.size()))
        return SWMM_ERR_BADPARAM;
    const auto& r = cfg.rows[static_cast<std::size_t>(index)];
    if      (ieq(key, "PSI_B"))     *value = r.psi_b;
    else if (ieq(key, "LAMBDA"))    *value = r.lambda;
    else if (ieq(key, "N"))         *value = r.vg_n;
    else if (ieq(key, "L"))         *value = r.vg_L;
    else if (ieq(key, "C_LOSS"))    *value = r.c_loss;
    else if (ieq(key, "HG0"))       *value = r.hg0;
    else if (ieq(key, "SOIL_CHAR")) *value = static_cast<double>(r.soil_char);
    else if (ieq(key, "CLOSURE"))   *value = static_cast<double>(r.closure);
    else if (ieq(key, "M_LAYERS"))  *value = r.m_layers;
    else return SWMM_ERR_BADPARAM;
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
    if (!count) return SWMM_ERR_BADPARAM;
    *count = static_cast<int>(cfg.node_beds.size());
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_add(SWMM_Engine engine, const char* node,
                                       int cell, double kc, double dc,
                                       double area) {
    AQ_CFG(engine);
    AQ_EDITABLE(eng);
    if (!node || node[0] == '\0' || cell < 0) return SWMM_ERR_BADPARAM;
    if (kc < 0.0 || dc < 0.0 || area < 0.0) return SWMM_ERR_BADPARAM;
    if (kc > 0.0 && dc <= 0.0) return SWMM_ERR_BADPARAM;   // a bed needs a thickness
    GwNodeBed b;
    b.cell = cell;
    b.Kc   = kc;
    b.dC   = dc;
    b.area = area;
    cfg.node_beds.push_back(b);
    eng->surfaceRouter2D().aquiferNodeNames().emplace_back(node);
    cfg.options.authored = true;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_node_get(SWMM_Engine engine, int index,
                                       char* node, int nodelen, int* cell,
                                       double* kc, double* dc, double* area) {
    AQ_CFG(engine);
    if (index < 0 || index >= static_cast<int>(cfg.node_beds.size()))
        return SWMM_ERR_BADPARAM;
    const auto& b = cfg.node_beds[static_cast<std::size_t>(index)];
    if (cell) *cell = b.cell;
    if (kc)   *kc   = b.Kc;
    if (dc)   *dc   = b.dC;
    if (area) *area = b.area;
    if (node) {
        const auto& names = eng->surfaceRouter2D().aquiferNodeNames();
        copy_to(index < static_cast<int>(names.size())
                    ? names[static_cast<std::size_t>(index)] : std::string{},
                node, nodelen);
    }
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

// ---------------------------------------------------------------------------
// Running state
// ---------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_gw2d_is_active(SWMM_Engine engine, int* active) {
    auto* eng = reinterpret_cast<openswmm::SWMMEngine*>(engine);
    if (!eng) return SWMM_ERR_BADHANDLE;
    if (!active) return SWMM_ERR_BADPARAM;
    *active = eng->surfaceRouter2D().subsurface().active() ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_dimensions(SWMM_Engine engine,
                                             int* n_cells, int* m_layers) {
    AQ_SOLVER(engine);
    if (n_cells)  *n_cells  = gw.state().n_cells;
    if (m_layers) *m_layers = gw.state().m_layers;
    return SWMM_OK;
}

namespace {

double cellVar(const openswmm::twoD::SubsurfaceState& st, std::size_t u,
               int var, bool& ok) {
    ok = true;
    switch (var) {
        case SWMM_GW2D_VAR_HG:       return st.hg[u];
        case SWMM_GW2D_VAR_HU:       return st.hu[u];
        case SWMM_GW2D_VAR_TABLE_EL: return st.z_bed[u] + st.hg[u];
        case SWMM_GW2D_VAR_Q0:       return st.q0_last[u];
        case SWMM_GW2D_VAR_QLAT:     return st.qlat_last[u];
        case SWMM_GW2D_VAR_QNODE:    return st.qnode_last[u];
        case SWMM_GW2D_VAR_QDEEP:    return st.qdeep_last[u];
        case SWMM_GW2D_VAR_QET:      return st.qet_last[u];
        case SWMM_GW2D_VAR_DUNNE:    return st.dunne_last[u];
        case SWMM_GW2D_VAR_QPLUS:    return st.qplus_last[u];
        case SWMM_GW2D_VAR_DT_CELL:  return st.dt_cell[u];
        case SWMM_GW2D_VAR_TIER:     return static_cast<double>(st.tier[u]);
        case SWMM_GW2D_VAR_CLOSURE:  return static_cast<double>(st.closure[u]);
        default: ok = false; return 0.0;
    }
}

}  // namespace

SWMM_ENGINE_API int swmm_gw2d_get_cell(SWMM_Engine engine, int cell, int var,
                                       double* value) {
    AQ_SOLVER(engine);
    const auto& st = gw.state();
    if (!value || cell < 0 || cell >= st.n_cells) return SWMM_ERR_BADPARAM;
    bool ok = false;
    const double v = cellVar(st, static_cast<std::size_t>(cell), var, ok);
    if (!ok) return SWMM_ERR_BADPARAM;
    *value = v;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_cell_bulk(SWMM_Engine engine, int var,
                                            double* out, int len,
                                            int* written) {
    AQ_SOLVER(engine);
    const auto& st = gw.state();
    if (!out || len < st.n_cells) return SWMM_ERR_BADPARAM;
    bool ok = true;
    for (int i = 0; i < st.n_cells && ok; ++i)
        out[i] = cellVar(st, static_cast<std::size_t>(i), var, ok);
    if (!ok) return SWMM_ERR_BADPARAM;
    if (written) *written = st.n_cells;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_column(SWMM_Engine engine, int cell,
                                         double* theta, int len,
                                         int* written) {
    AQ_SOLVER(engine);
    const auto& st = gw.state();
    if (!theta || cell < 0 || cell >= st.n_cells) return SWMM_ERR_BADPARAM;
    if (len < st.m_layers) return SWMM_ERR_BADPARAM;
    // Refuse rather than return zeros: a caller that reads a closure-A cell's
    // "column" would see a bone-dry profile and have no way to tell it apart
    // from a real one.
    if (st.closure[static_cast<std::size_t>(cell)] !=
        static_cast<int8_t>(GwClosure::SIGMA))
        return SWMM_ERR_BADPARAM;
    for (int j = 0; j < st.m_layers; ++j)
        theta[j] = st.theta_sigma[static_cast<std::size_t>(j) *
                                      static_cast<std::size_t>(st.n_cells) +
                                  static_cast<std::size_t>(cell)];
    if (written) *written = st.m_layers;
    return SWMM_OK;
}

namespace {

/// Everything the aquifer HOLDS right now, including water parked in a side
/// accumulator: in flight between two cells, received from the surface and not
/// yet absorbed, or pushed out and not yet taken. All of it is real water in
/// the aquifer's custody, and this is what SWMM_GW2D_LED_STORAGE reports.
///
/// `nacc` is deliberately NOT subtracted. It holds node exchange that
/// `sampleNodeExchange` has committed but no cell has applied yet, so that
/// water is still inside `state.hg` — subtracting it would report it leaving
/// twice.
double liveStorage(const SubsurfaceSolver& gw) {
    const auto& st = gw.state();
    double s = st.storage();
    for (double v : st.eacc_L) s += v;
    for (double v : st.eacc_R) s += v;
    for (double v : st.xacc_from_surface) s += v;
    for (double v : st.xacc_to_surface)   s += v;
    return s;
}

/// The storage the LEDGER can account for — holdings less the two surface
/// accumulators. See swmm_gw2d_get_continuity_error for why the difference
/// matters.
double ledgeredStorage(const SubsurfaceSolver& gw) {
    const auto& st = gw.state();
    double s = st.storage();
    for (double v : st.eacc_L) s += v;
    for (double v : st.eacc_R) s += v;
    return s;
}

}  // namespace

SWMM_ENGINE_API int swmm_gw2d_get_ledger(SWMM_Engine engine, int term,
                                         double* value) {
    AQ_SOLVER(engine);
    if (!value) return SWMM_ERR_BADPARAM;
    const auto& st = gw.state();
    switch (term) {
        case SWMM_GW2D_LED_RECHARGE:     *value = st.led_recharge; break;
        case SWMM_GW2D_LED_LATERAL:      *value = st.led_lateral; break;
        case SWMM_GW2D_LED_DEEP:         *value = st.led_deep; break;
        case SWMM_GW2D_LED_NODE:         *value = st.led_node; break;
        case SWMM_GW2D_LED_DUNNE:        *value = st.led_dunne; break;
        case SWMM_GW2D_LED_CAPRISE:      *value = st.led_caprise; break;
        case SWMM_GW2D_LED_ET:           *value = st.led_et; break;
        case SWMM_GW2D_LED_INFIL_IN:     *value = st.led_infil_in; break;
        case SWMM_GW2D_LED_INIT_STORAGE: *value = st.led_init_storage; break;
        case SWMM_GW2D_LED_STORAGE:      *value = liveStorage(gw); break;
        default: return SWMM_ERR_BADPARAM;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_continuity_error(SWMM_Engine engine,
                                                   double* value) {
    AQ_SOLVER(engine);
    if (!value) return SWMM_ERR_BADPARAM;
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
    const double in  = st.led_infil_in + st.led_lateral;
    const double out = st.led_deep + st.led_node + st.led_et + st.led_dunne;
    *value = ledgeredStorage(gw) - st.led_init_storage - (in - out);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_gw2d_get_tier_histogram(SWMM_Engine engine, long* out,
                                                 int len, int* written) {
    AQ_SOLVER(engine);
    if (!out || len <= 0) return SWMM_ERR_BADPARAM;
    const auto& f = gw.tierFirings();
    const int n = std::min(len, static_cast<int>(f.size()));
    for (int i = 0; i < n; ++i) out[i] = f[static_cast<std::size_t>(i)];
    if (written) *written = n;
    return SWMM_OK;
}

}  // extern "C"
