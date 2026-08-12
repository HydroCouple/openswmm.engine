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
 * @file openswmm_subcatchments_impl.cpp
 * @brief C API implementation — subcatchment identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_subcatchments.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_subcatchments.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_subcatches();
}

SWMM_ENGINE_API int swmm_subcatch_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().subcatch_names.find(id);
}

SWMM_ENGINE_API const char* swmm_subcatch_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_subcatches()) return nullptr;
    return ctx.subcatch_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING or OPENED — "editable" states)
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    if (ctx.subcatch_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    ctx.subcatch_names.add(id);
    int n = ctx.subcatch_names.size();
    ctx.subcatches.grow_to(n);
    const auto un = static_cast<std::size_t>(n);
    if (ctx.spatial.subcatch_x.size() < un)          ctx.spatial.subcatch_x.resize(un, 0.0);
    if (ctx.spatial.subcatch_y.size() < un)          ctx.spatial.subcatch_y.resize(un, 0.0);
    if (ctx.spatial.subcatch_polygon_x.size() < un)  ctx.spatial.subcatch_polygon_x.resize(un);
    if (ctx.spatial.subcatch_polygon_y.size() < un)  ctx.spatial.subcatch_polygon_y.resize(un);

    return SWMM_OK;
}

// ============================================================================
// Property setters (BUILDING or OPENED)
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_outlet(SWMM_Engine engine, int idx, int node_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.outlet_node[static_cast<std::size_t>(idx)] = node_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_area(SWMM_Engine engine, int idx, double area) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.area[static_cast<std::size_t>(idx)] = area;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_width(SWMM_Engine engine, int idx, double width) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: width stored INTERNAL ft -> convert display->internal
    ctx.subcatches.width[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, width);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_slope(SWMM_Engine engine, int idx, double slope) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // `slope` is a percentage per the API contract (e.g. 2.0 = 2%); the runoff
    // module and the [SUBCATCHMENTS] parser both store it as a fraction
    // (ctx.subcatches.slope = %Slope / 100), so convert here to match — and to
    // stay consistent with swmm_subcatch_set_imperv_pct, which divides likewise.
    ctx.subcatches.slope[static_cast<std::size_t>(idx)] = slope / 100.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_imperv_pct(SWMM_Engine engine, int idx, double pct) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.frac_imperv[static_cast<std::size_t>(idx)] = pct / 100.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_zero_imperv_pct(SWMM_Engine engine, int idx, double pct) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.frac_imperv_no_store[static_cast<std::size_t>(idx)] = pct / 100.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_n_imperv(SWMM_Engine engine, int idx, double n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.n_imperv[static_cast<std::size_t>(idx)] = n;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_n_perv(SWMM_Engine engine, int idx, double n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.n_perv[static_cast<std::size_t>(idx)] = n;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_ds_imperv(SWMM_Engine engine, int idx, double ds) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.ds_imperv[static_cast<std::size_t>(idx)] = ds;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_ds_perv(SWMM_Engine engine, int idx, double ds) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.ds_perv[static_cast<std::size_t>(idx)] = ds;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_gage(SWMM_Engine engine, int idx, int gage_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.gage[static_cast<std::size_t>(idx)] = gage_idx;
    return SWMM_OK;
}

// ============================================================================
// Infiltration parameters
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_infil_horton(SWMM_Engine engine, int idx,
                                                     double f0, double fmin,
                                                     double decay, double dry_time) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.subcatches.infil_model[uidx] = 0; // HORTON
    ctx.subcatches.infil_p1[uidx] = f0;
    ctx.subcatches.infil_p2[uidx] = fmin;
    ctx.subcatches.infil_p3[uidx] = decay;
    ctx.subcatches.infil_p4[uidx] = dry_time;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_infil_green_ampt(SWMM_Engine engine, int idx,
                                                         double suction, double conductivity,
                                                         double initial_deficit) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.subcatches.infil_model[uidx] = 2; // GREEN_AMPT
    ctx.subcatches.infil_p1[uidx] = suction;
    ctx.subcatches.infil_p2[uidx] = conductivity;
    ctx.subcatches.infil_p3[uidx] = initial_deficit;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_infil_curve_number(SWMM_Engine engine, int idx,
                                                           double cn, double drying_time) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.subcatches.infil_model[uidx] = 4; // CURVE_NUMBER
    ctx.subcatches.infil_p1[uidx] = cn;
    // p3 is the drying-time slot for CURVE_NUMBER: it is the third
    // [INFILTRATION] column and legacy curvenum_setParams reads p[2].
    ctx.subcatches.infil_p3[uidx] = drying_time;
    return SWMM_OK;
}

// Set ONLY the infiltration model code (0=HORTON, 1=MOD_HORTON, 2=GREEN_AMPT,
// 3=MOD_GREEN_AMPT, 4=CURVE_NUMBER). The per-model parameters in infil_p1..p5
// are positionally overloaded (their meaning depends on the model), so a model
// switch leaves the stored parameters interpreted under the new model. Callers
// that change the model type should follow this with the matching
// swmm_subcatch_set_infil_horton / _green_ampt / _curve_number call to install
// the correct parameters. Returns SWMM_ERR_BADPARAM for an out-of-range code.
SWMM_ENGINE_API int swmm_subcatch_set_infil_model(SWMM_Engine engine, int idx, int model) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (model < 0 || model > 4) return SWMM_ERR_BADPARAM;
    ctx.subcatches.infil_model[static_cast<std::size_t>(idx)] = model;
    return SWMM_OK;
}

// ============================================================================
// Property getters
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_area(SWMM_Engine engine, int idx, double* area) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: area is stored in DISPLAY units (ac/ha), converted at-use in Runoff.cpp — return raw
    if (area) *area = ctx.subcatches.area[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_imperv_pct(SWMM_Engine engine, int idx, double* pct) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (pct) *pct = ctx.subcatches.frac_imperv[static_cast<std::size_t>(idx)] * 100.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_zero_imperv_pct(SWMM_Engine engine, int idx, double* pct) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (pct) *pct = ctx.subcatches.frac_imperv_no_store[static_cast<std::size_t>(idx)] * 100.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_outlet(SWMM_Engine engine, int idx, int* node_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (node_idx) *node_idx = ctx.subcatches.outlet_node[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_width(SWMM_Engine engine, int idx, double* w) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: width stored INTERNAL ft -> convert internal->display
    if (w) *w = to_display(ctx, openswmm::ucf::LENGTH, ctx.subcatches.width[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_slope(SWMM_Engine engine, int idx, double* s) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // Internally slope is stored as a fraction (%Slope / 100). swmm_subcatch_set_slope
    // divides the incoming percentage by 100, so the getter must multiply back by 100
    // to round-trip — matching the symmetric swmm_subcatch_get/set_imperv_pct pair.
    if (s) *s = ctx.subcatches.slope[static_cast<std::size_t>(idx)] * 100.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_n_imperv(SWMM_Engine engine, int idx, double* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (n) *n = ctx.subcatches.n_imperv[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_n_perv(SWMM_Engine engine, int idx, double* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (n) *n = ctx.subcatches.n_perv[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_ds_imperv(SWMM_Engine engine, int idx, double* ds) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (ds) *ds = ctx.subcatches.ds_imperv[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_ds_perv(SWMM_Engine engine, int idx, double* ds) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (ds) *ds = ctx.subcatches.ds_perv[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_gage(SWMM_Engine engine, int idx, int* gage_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (gage_idx) *gage_idx = ctx.subcatches.gage[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_outlet_subcatch(SWMM_Engine engine, int idx, int sc_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.outlet_subcatch[static_cast<std::size_t>(idx)] = sc_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_outlet_subcatch(SWMM_Engine engine, int idx, int* sc_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (sc_idx) *sc_idx = ctx.subcatches.outlet_subcatch[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Infiltration getters
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_infil_model(SWMM_Engine engine, int idx, int* model) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (model) *model = ctx.subcatches.infil_model[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil_horton(SWMM_Engine engine, int idx,
                                                     double* f0, double* fmin,
                                                     double* decay, double* dry_time) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    if (f0)       *f0       = ctx.subcatches.infil_p1[uidx];
    if (fmin)     *fmin     = ctx.subcatches.infil_p2[uidx];
    if (decay)    *decay    = ctx.subcatches.infil_p3[uidx];
    if (dry_time) *dry_time = ctx.subcatches.infil_p4[uidx];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil_green_ampt(SWMM_Engine engine, int idx,
                                                         double* suction, double* conductivity,
                                                         double* deficit) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    if (suction)      *suction      = ctx.subcatches.infil_p1[uidx];
    if (conductivity) *conductivity = ctx.subcatches.infil_p2[uidx];
    if (deficit)      *deficit      = ctx.subcatches.infil_p3[uidx];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil_curve_number(SWMM_Engine engine, int idx,
                                                           double* cn, double* drying_time) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    if (cn)          *cn          = ctx.subcatches.infil_p1[uidx];
    if (drying_time) *drying_time = ctx.subcatches.infil_p3[uidx];
    return SWMM_OK;
}

// ============================================================================
// Subcatchment statistics
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_stat_precip(SWMM_Engine engine, int idx, double* vol) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: stat precip volume stored INTERNAL ft³ -> convert internal->display
    if (vol) *vol = to_display(ctx, openswmm::ucf::VOLUME, ctx.subcatches.stat_precip_vol[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_stat_runoff_vol(SWMM_Engine engine, int idx, double* vol) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: stat runoff volume stored INTERNAL ft³ -> convert internal->display
    if (vol) *vol = to_display(ctx, openswmm::ucf::VOLUME, ctx.subcatches.stat_runoff_vol[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_stat_max_runoff(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: stat max runoff stored INTERNAL cfs -> convert internal->display
    if (rate) *rate = to_display(ctx, openswmm::ucf::FLOW, ctx.subcatches.stat_max_runoff[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// ============================================================================
// Subcatchment landuse coverage
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_coverage(SWMM_Engine engine, int sc_idx, int lu_idx, double fraction) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(sc_idx >= 0 && sc_idx < ctx.n_subcatches());
    CHECK_INDEX(lu_idx >= 0 && lu_idx < ctx.n_landuses());

    // Ensure coverage matrix is sized
    if (ctx.subcatches.coverage_n_landuses != ctx.n_landuses() ||
        static_cast<int>(ctx.subcatches.coverage.size()) !=
            ctx.n_subcatches() * ctx.n_landuses()) {
        ctx.subcatches.resize_coverage(ctx.n_subcatches(), ctx.n_landuses());
    }

    auto k = static_cast<std::size_t>(sc_idx) *
             static_cast<std::size_t>(ctx.n_landuses()) +
             static_cast<std::size_t>(lu_idx);
    ctx.subcatches.coverage[k] = fraction;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_coverage(SWMM_Engine engine, int sc_idx, int lu_idx, double* fraction) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(sc_idx >= 0 && sc_idx < ctx.n_subcatches());
    CHECK_INDEX(lu_idx >= 0 && lu_idx < ctx.n_landuses());

    if (ctx.subcatches.coverage.empty() ||
        ctx.subcatches.coverage_n_landuses != ctx.n_landuses()) {
        if (fraction) *fraction = 0.0;
        return SWMM_OK;
    }

    auto k = static_cast<std::size_t>(sc_idx) *
             static_cast<std::size_t>(ctx.n_landuses()) +
             static_cast<std::size_t>(lu_idx);
    if (fraction) *fraction = ctx.subcatches.coverage[k];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_coverages(SWMM_Engine engine, int sc_idx, double* out, int n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(sc_idx >= 0 && sc_idx < ctx.n_subcatches());
    CHECK_INDEX(n >= 0 && n <= ctx.n_landuses());
    if (!out) return SWMM_ERR_BADPARAM;

    const bool sized = !ctx.subcatches.coverage.empty() &&
                       ctx.subcatches.coverage_n_landuses == ctx.n_landuses();
    const auto base = static_cast<std::size_t>(sc_idx) *
                      static_cast<std::size_t>(ctx.n_landuses());
    for (int lu = 0; lu < n; ++lu)
        out[lu] = sized ? ctx.subcatches.coverage[base + static_cast<std::size_t>(lu)]
                        : 0.0;
    return SWMM_OK;
}

// ============================================================================
// Initial pollutant loadings ([LOADINGS])
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_initial_loading(SWMM_Engine engine, int sc_idx, int pollut_idx, double buildup) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(sc_idx >= 0 && sc_idx < ctx.n_subcatches());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    // Ensure the quality arrays are sized ([LOADINGS] parks the initial
    // buildup in subcatches.conc — same storage handle_loadings uses).
    if (ctx.subcatches.conc_n_pollutants != ctx.n_pollutants() ||
        static_cast<int>(ctx.subcatches.conc.size()) !=
            ctx.n_subcatches() * ctx.n_pollutants()) {
        ctx.subcatches.resize_quality(ctx.n_pollutants());
    }

    auto k = static_cast<std::size_t>(sc_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);
    ctx.subcatches.conc[k] = buildup;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_initial_loading(SWMM_Engine engine, int sc_idx, int pollut_idx, double* buildup) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(sc_idx >= 0 && sc_idx < ctx.n_subcatches());
    CHECK_INDEX(pollut_idx >= 0 && pollut_idx < ctx.n_pollutants());

    if (ctx.subcatches.conc.empty() ||
        ctx.subcatches.conc_n_pollutants != ctx.n_pollutants()) {
        if (buildup) *buildup = 0.0;
        return SWMM_OK;
    }

    auto k = static_cast<std::size_t>(sc_idx) *
             static_cast<std::size_t>(ctx.n_pollutants()) +
             static_cast<std::size_t>(pollut_idx);
    if (buildup) *buildup = ctx.subcatches.conc[k];
    return SWMM_OK;
}

// ============================================================================
// Hydraulic state getters
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_runoff(SWMM_Engine engine, int idx, double* runoff) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: runoff stored INTERNAL cfs -> convert internal->display
    if (runoff) *runoff = to_display(ctx, openswmm::ucf::FLOW, ctx.subcatches.runoff[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_groundwater(SWMM_Engine engine, int idx, double* gw_flow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: groundwater flow stored INTERNAL cfs -> convert internal->display
    if (gw_flow) *gw_flow = to_display(ctx, openswmm::ucf::FLOW, ctx.subcatches.gw_flow[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_rainfall(SWMM_Engine engine, int idx, double* rainfall) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: state rainfall stored INTERNAL ft/s -> convert internal->display
    if (rainfall) *rainfall = to_display(ctx, openswmm::ucf::RAINFALL, ctx.subcatches.rainfall[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_snow_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto* eng = to_engine(engine);
    const auto& ctx = eng->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // Area-weighted snow pack SWE from the snow solver state,
    // internal ft → user depth units (in US, mm SI)
    if (depth) *depth = to_display(ctx, openswmm::ucf::RAINDEPTH,
                                   eng->subcatchSnowDepth(idx));
    return SWMM_OK;
}

// ============================================================================
// Groundwater configuration ([GROUNDWATER])
// ============================================================================
// These configure the subcatchment's [GROUNDWATER] flow routing. Values are
// stored exactly as parsed from the input file (raw user units, no internal
// conversion) so they round-trip identically — mirroring HydrologyHandler's
// handle_groundwater(). This is distinct from the runtime gw STATE
// (theta / lower_depth) injected via swmm_subcatch_set_gw_state below.

SWMM_ENGINE_API int swmm_subcatch_set_aquifer(SWMM_Engine engine, int idx, int aquifer_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.gw_aquifer[static_cast<std::size_t>(idx)] = aquifer_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_aquifer(SWMM_Engine engine, int idx, int* aquifer_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (aquifer_idx) *aquifer_idx = ctx.subcatches.gw_aquifer[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_gw_node(SWMM_Engine engine, int idx, int node_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.gw_node[static_cast<std::size_t>(idx)] = node_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_gw_node(SWMM_Engine engine, int idx, int* node_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (node_idx) *node_idx = ctx.subcatches.gw_node[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// Groundwater flow parameters, in [GROUNDWATER] token order:
// SurfEl, A1, B1, A2, B2, A3, Twgr (gw_tw), Hstar (gw_hstar). Stored raw.
SWMM_ENGINE_API int swmm_subcatch_set_gw_params(SWMM_Engine engine, int idx,
                                                double surf_elev, double a1, double b1,
                                                double a2, double b2, double a3,
                                                double tw, double hstar) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.subcatches.gw_surf_elev[uidx] = surf_elev;
    ctx.subcatches.gw_a1[uidx]        = a1;
    ctx.subcatches.gw_b1[uidx]        = b1;
    ctx.subcatches.gw_a2[uidx]        = a2;
    ctx.subcatches.gw_b2[uidx]        = b2;
    ctx.subcatches.gw_a3[uidx]        = a3;
    ctx.subcatches.gw_tw[uidx]        = tw;
    ctx.subcatches.gw_hstar[uidx]     = hstar;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_gw_params(SWMM_Engine engine, int idx,
                                                double* surf_elev, double* a1, double* b1,
                                                double* a2, double* b2, double* a3,
                                                double* tw, double* hstar) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    if (surf_elev) *surf_elev = ctx.subcatches.gw_surf_elev[uidx];
    if (a1)        *a1        = ctx.subcatches.gw_a1[uidx];
    if (b1)        *b1        = ctx.subcatches.gw_b1[uidx];
    if (a2)        *a2        = ctx.subcatches.gw_a2[uidx];
    if (b2)        *b2        = ctx.subcatches.gw_b2[uidx];
    if (a3)        *a3        = ctx.subcatches.gw_a3[uidx];
    if (tw)        *tw        = ctx.subcatches.gw_tw[uidx];
    if (hstar)     *hstar     = ctx.subcatches.gw_hstar[uidx];
    return SWMM_OK;
}

// ============================================================================
// State injection (data assimilation)
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_gw_state(SWMM_Engine engine, int idx,
                                               double theta, double lower_depth) {
    CHECK_HANDLE(engine);
    auto* eng = to_engine(engine);
    auto& ctx = eng->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto& soa = eng->gwSolver().state();
    auto ui = static_cast<std::size_t>(idx);
    if (ui >= soa.theta.size() || soa.total_depth[ui] <= 0.0)
        return SWMM_ERR_BADPARAM;   // no groundwater on this subcatchment

    if (theta >= 0.0) {
        // clamp to physical range [wilting point fraction, porosity]
        double porosity = soa.porosity[ui];
        soa.theta[ui] = (theta > porosity) ? porosity : theta;
    }
    if (lower_depth >= 0.0) {
        double ld = to_internal(ctx, openswmm::ucf::LENGTH, lower_depth);
        double max_d = soa.total_depth[ui];
        soa.lower_depth[ui] = (ld > max_d) ? max_d : ld;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_gw_state(SWMM_Engine engine, int idx,
                                               double* theta, double* lower_depth) {
    CHECK_HANDLE(engine);
    const auto* eng = to_engine(engine);
    const auto& ctx = eng->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    const auto& soa = eng->gwSolver().state();
    auto ui = static_cast<std::size_t>(idx);
    if (ui >= soa.theta.size() || soa.total_depth[ui] <= 0.0)
        return SWMM_ERR_BADPARAM;

    if (theta)       *theta = soa.theta[ui];
    if (lower_depth) *lower_depth = to_display(ctx, openswmm::ucf::LENGTH,
                                               soa.lower_depth[ui]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_snow_state(SWMM_Engine engine, int idx,
                                                 int surface, double swe, double fw,
                                                 double ati, double coldc) {
    CHECK_HANDLE(engine);
    auto* eng = to_engine(engine);
    auto& ctx = eng->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto ui = static_cast<std::size_t>(idx);
    if (ctx.subcatches.snowpack[ui] < 0) return SWMM_ERR_BADPARAM;
    CHECK_INDEX(surface >= 0 && surface < openswmm::snow::N_SUBAREAS);

    auto& soa = eng->snowSolver().state();
    auto pk = static_cast<std::size_t>(idx * openswmm::snow::N_SUBAREAS + surface);
    if (pk >= soa.wsnow.size()) return SWMM_ERR_BADPARAM;

    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    if (swe >= 0.0)
        soa.wsnow[pk] = to_internal(ctx, openswmm::ucf::RAINDEPTH, swe);
    if (fw >= 0.0)
        soa.fw[pk] = to_internal(ctx, openswmm::ucf::RAINDEPTH, fw);
    if (ati > -999.0) {
        double t = ati;
        if (unit_sys == 1) t = t * 9.0 / 5.0 + 32.0;
        soa.ati[pk] = t;
    }
    if (coldc >= 0.0)
        soa.coldc[pk] = to_internal(ctx, openswmm::ucf::RAINDEPTH, coldc);

    // free water cannot exceed the pack
    if (soa.fw[pk] > soa.wsnow[pk]) soa.fw[pk] = soa.wsnow[pk];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_snow_state(SWMM_Engine engine, int idx,
                                                 int surface, double* swe, double* fw,
                                                 double* ati, double* coldc) {
    CHECK_HANDLE(engine);
    const auto* eng = to_engine(engine);
    const auto& ctx = eng->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto ui = static_cast<std::size_t>(idx);
    if (ctx.subcatches.snowpack[ui] < 0) return SWMM_ERR_BADPARAM;
    CHECK_INDEX(surface >= 0 && surface < openswmm::snow::N_SUBAREAS);

    const auto& soa = eng->snowSolver().state();
    auto pk = static_cast<std::size_t>(idx * openswmm::snow::N_SUBAREAS + surface);
    if (pk >= soa.wsnow.size()) return SWMM_ERR_BADPARAM;

    int unit_sys = openswmm::ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
    if (swe)   *swe   = to_display(ctx, openswmm::ucf::RAINDEPTH, soa.wsnow[pk]);
    if (fw)    *fw    = to_display(ctx, openswmm::ucf::RAINDEPTH, soa.fw[pk]);
    if (ati)   *ati   = (unit_sys == 1) ? (soa.ati[pk] - 32.0) * 5.0 / 9.0
                                        : soa.ati[pk];
    if (coldc) *coldc = to_display(ctx, openswmm::ucf::RAINDEPTH, soa.coldc[pk]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_evap(SWMM_Engine engine, int idx, double* evap) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: evap loss stored INTERNAL ft/s -> convert internal->display
    if (evap) *evap = to_display(ctx, openswmm::ucf::EVAPRATE, ctx.subcatches.evap_loss[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil(SWMM_Engine engine, int idx, double* infil) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: infil loss stored INTERNAL ft/s -> convert internal->display
    if (infil) *infil = to_display(ctx, openswmm::ucf::RAINFALL, ctx.subcatches.infil_loss[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// ============================================================================
// Runtime forcing
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_rainfall(SWMM_Engine engine, int idx, double rainfall) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // units: state rainfall stored INTERNAL ft/s -> convert display->internal
    ctx.subcatches.rainfall[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::RAINFALL, rainfall);
    return SWMM_OK;
}

// ============================================================================
// Water quality (Phase 8)
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_quality(SWMM_Engine engine, int subcatch_idx,
                                               int pollutant_idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(subcatch_idx >= 0 && subcatch_idx < ctx.n_subcatches());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    if (conc) *conc = ctx.subcatches.conc[
        static_cast<std::size_t>(subcatch_idx) * static_cast<std::size_t>(np) +
        static_cast<std::size_t>(pollutant_idx)];
    return SWMM_OK;
}

// ============================================================================
// Bulk access
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_runoff_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_subcatches());
    // units: runoff stored INTERNAL cfs -> convert internal->display per element
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.subcatches.runoff[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                    double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    const int n = std::min(count, ctx.n_subcatches());
    for (int i = 0; i < n; ++i) {
        buf[i] = ctx.subcatches.conc[
            static_cast<std::size_t>(i) * static_cast<std::size_t>(np) +
            static_cast<std::size_t>(pollutant_idx)];
    }
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Phase 3 bulk getters — Subcatchments.
//
// rainfall, evap_loss, infil_loss are simple SoA memcpys. snow_depth mirrors
// the scalar accessor (which currently returns 0.0 since snow state lives in
// the SnowSolver, not SubcatchData) — when snow integration lands, both the
// scalar and bulk variants get updated together. IDs follow the stride-packed
// UTF-8 format established by swmm_node_get_ids_bulk.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_subcatch_get_rainfall_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    // units: state rainfall stored INTERNAL ft/s -> convert internal->display per element
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::RAINFALL, ctx.subcatches.rainfall[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_evap_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    // units: evap loss stored INTERNAL ft/s -> convert internal->display per element
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::EVAPRATE, ctx.subcatches.evap_loss[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    // units: infil loss stored INTERNAL ft/s -> convert internal->display per element
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::RAINFALL, ctx.subcatches.infil_loss[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_snow_depth_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    // Mirror the scalar accessor's placeholder behavior: zero-fill until
    // snow state is integrated with SubcatchData.
    std::fill_n(buf, n, 0.0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_ids_bulk(SWMM_Engine engine,
                                                char* buf,
                                                int stride,
                                                int count) {
    CHECK_HANDLE(engine);
    if (!buf || stride < 2 || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    const std::size_t s = static_cast<std::size_t>(stride);

    std::fill_n(buf, s * static_cast<std::size_t>(n), '\0');
    for (int i = 0; i < n; ++i) {
        const std::string& name = ctx.subcatch_names.name_of(i);
        const std::size_t copy_n = std::min(name.size(), s - 1);
        std::memcpy(buf + static_cast<std::size_t>(i) * s,
                    name.data(), copy_n);
    }
    return SWMM_OK;
}

// ============================================================================
// Ponded quality
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_ponded_quality(SWMM_Engine engine,
    int subcatch_idx, int pollutant_idx, double* mass) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(subcatch_idx >= 0 && subcatch_idx < ctx.n_subcatches());
    int np = ctx.subcatches.conc_n_pollutants;
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    auto idx = static_cast<std::size_t>(subcatch_idx) * static_cast<std::size_t>(np) +
               static_cast<std::size_t>(pollutant_idx);
    if (mass) *mass = (idx < ctx.subcatches.ponded_qual.size())
                    ? ctx.subcatches.ponded_qual[idx] : 0.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_ponded_quality(SWMM_Engine engine,
    int subcatch_idx, int pollutant_idx, double mass) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(subcatch_idx >= 0 && subcatch_idx < ctx.n_subcatches());
    int np = ctx.subcatches.conc_n_pollutants;
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    auto idx = static_cast<std::size_t>(subcatch_idx) * static_cast<std::size_t>(np) +
               static_cast<std::size_t>(pollutant_idx);
    if (idx < ctx.subcatches.ponded_qual.size())
        ctx.subcatches.ponded_qual[idx] = mass;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_rename(SWMM_Engine engine, int idx, const char* newId) {
    CHECK_HANDLE(engine);
    if (!newId || newId[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    return ctx.subcatch_names.rename(idx, newId) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_subcatch_get_tag(SWMM_Engine engine, int idx,
                                            char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    const auto u = static_cast<std::size_t>(idx);
    const std::string& s = (u < ctx.subcatches.tags.size()) ? ctx.subcatches.tags[u]
                                                            : std::string{};
    const int copy_len = std::min(static_cast<int>(s.size()), buflen - 1);
    if (copy_len > 0) std::memcpy(buf, s.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_tag(SWMM_Engine engine, int idx,
                                            const char* tag) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    const auto u = static_cast<std::size_t>(idx);
    if (u >= ctx.subcatches.tags.size()) ctx.subcatches.tags.resize(u + 1);
    ctx.subcatches.tags[u] = (tag != nullptr) ? std::string(tag) : std::string{};
    return SWMM_OK;
}

// ============================================================================
// Aquifers ([AQUIFERS] section) — Slice BM.0 list + add; setters land with BP
// ============================================================================

SWMM_ENGINE_API int swmm_aquifer_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().aquifers.count();
}

SWMM_ENGINE_API int swmm_aquifer_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    // Route through the registry (case-insensitive, legacy hash.c parity)
    // instead of scanning aquifers.names, which could disagree with it.
    return to_engine(engine)->context().aquifer_names.find(id);
}

SWMM_ENGINE_API const char* swmm_aquifer_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().aquifers.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

SWMM_ENGINE_API int swmm_aquifer_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    // Reject duplicates (case-insensitive) BEFORE touching the backing store;
    // an unguarded NameIndex::add would throw across the C ABI.
    if (ctx.aquifer_names.find(id) >= 0) return SWMM_ERR_BADPARAM;

    auto& aq = ctx.aquifers;
    aq.names.push_back(id);
    aq.porosity.push_back(0.0);
    aq.wilting_point.push_back(0.0);
    aq.field_capacity.push_back(0.0);
    aq.conductivity.push_back(0.0);
    aq.conduct_slope.push_back(0.0);
    aq.tension_slope.push_back(0.0);
    aq.upper_evap.push_back(0.0);
    aq.lower_evap.push_back(0.0);
    aq.lower_loss.push_back(0.0);
    aq.bottom_elev.push_back(0.0);
    aq.water_table_elev.push_back(0.0);
    aq.upper_moist.push_back(0.0);
    aq.upper_evap_pat.push_back("");

    ctx.aquifer_names.add(id);
    return SWMM_OK;
}

namespace {
// Map a SWMM_AquiferParam code to the backing store vector (input-file units).
std::vector<double>* aquifer_param_vec(openswmm::SimulationContext& ctx, int param) {
    auto& aq = ctx.aquifers;
    switch (param) {
        case SWMM_AQUIFER_POROSITY:         return &aq.porosity;
        case SWMM_AQUIFER_WILTING_POINT:    return &aq.wilting_point;
        case SWMM_AQUIFER_FIELD_CAPACITY:   return &aq.field_capacity;
        case SWMM_AQUIFER_CONDUCTIVITY:     return &aq.conductivity;
        case SWMM_AQUIFER_CONDUCT_SLOPE:    return &aq.conduct_slope;
        case SWMM_AQUIFER_TENSION_SLOPE:    return &aq.tension_slope;
        case SWMM_AQUIFER_UPPER_EVAP_FRAC:  return &aq.upper_evap;
        case SWMM_AQUIFER_LOWER_EVAP_DEPTH: return &aq.lower_evap;
        case SWMM_AQUIFER_LOWER_LOSS_COEFF: return &aq.lower_loss;
        case SWMM_AQUIFER_BOTTOM_ELEV:      return &aq.bottom_elev;
        case SWMM_AQUIFER_WATER_TABLE_ELEV: return &aq.water_table_elev;
        case SWMM_AQUIFER_UPPER_MOISTURE:   return &aq.upper_moist;
        default: return nullptr;
    }
}

// Structural / initial-condition parameters bound or seed groundwater state,
// so they may only change before start(); the flux coefficients are read
// (via the solver's refreshed copies) each step and are sound mid-run.
bool aquifer_param_prestart_only(int param) {
    switch (param) {
        case SWMM_AQUIFER_POROSITY:
        case SWMM_AQUIFER_WILTING_POINT:
        case SWMM_AQUIFER_FIELD_CAPACITY:
        case SWMM_AQUIFER_BOTTOM_ELEV:
        case SWMM_AQUIFER_WATER_TABLE_ELEV:
        case SWMM_AQUIFER_UPPER_MOISTURE:
            return true;
        default:
            return false;
    }
}
} // namespace

SWMM_ENGINE_API int swmm_aquifer_get_param(SWMM_Engine engine, int idx, int param, double* value) {
    CHECK_HANDLE(engine);
    if (!value) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.aquifers.count());
    const auto* vec = aquifer_param_vec(ctx, param);
    if (!vec) return SWMM_ERR_BADPARAM;
    *value = (*vec)[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_aquifer_set_param(SWMM_Engine engine, int idx, int param, double value) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.aquifers.count());
    auto* vec = aquifer_param_vec(ctx, param);
    if (!vec) return SWMM_ERR_BADPARAM;

    const bool editable = (ctx.state == openswmm::EngineState::BUILDING ||
                           ctx.state == openswmm::EngineState::OPENED);
    if (aquifer_param_prestart_only(param) && !editable)
        return SWMM_ERR_LIFECYCLE;

    // Light physical bounds (full cross-field validation runs at start —
    // Gap #81): fractions in [0, 1], everything but elevations non-negative.
    switch (param) {
        case SWMM_AQUIFER_POROSITY:
        case SWMM_AQUIFER_WILTING_POINT:
        case SWMM_AQUIFER_FIELD_CAPACITY:
        case SWMM_AQUIFER_UPPER_EVAP_FRAC:
        case SWMM_AQUIFER_UPPER_MOISTURE:
            if (value < 0.0 || value > 1.0) return SWMM_ERR_BADPARAM;
            break;
        case SWMM_AQUIFER_BOTTOM_ELEV:
        case SWMM_AQUIFER_WATER_TABLE_ELEV:
            break;  // elevations may be negative
        default:
            if (value < 0.0) return SWMM_ERR_BADPARAM;
            break;
    }

    (*vec)[static_cast<std::size_t>(idx)] = value;

    // The GW solver reads per-subcatchment copies made at start; re-derive
    // the flux-coefficient columns so a mid-run edit applies next step.
    if (!editable)
        to_engine(engine)->refreshAquiferParams();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_aquifer_get_evap_pattern(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.aquifers.count());
    const std::string& s = ctx.aquifers.upper_evap_pat[static_cast<std::size_t>(idx)];
    const std::size_t n = std::min(s.size(), static_cast<std::size_t>(buflen - 1));
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_aquifer_set_evap_pattern(SWMM_Engine engine, int idx, const char* name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    // The pattern name is resolved to a [PATTERNS] index at start(), so it is
    // only meaningful while the model is still being built (pre-start).
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.aquifers.count());
    ctx.aquifers.upper_evap_pat[static_cast<std::size_t>(idx)] = (name ? std::string(name) : std::string{});
    return SWMM_OK;
}

// ============================================================================
// Snowpacks ([SNOWPACKS] section) — Slice BM.0 list + add; setters land with BP
// ============================================================================

SWMM_ENGINE_API int swmm_snowpack_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().snowpacks.count();
}

SWMM_ENGINE_API int swmm_snowpack_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    // Route through the registry (case-insensitive, legacy hash.c parity)
    // instead of scanning snowpacks.names, which could disagree with it.
    return to_engine(engine)->context().snowpack_names.find(id);
}

SWMM_ENGINE_API const char* swmm_snowpack_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().snowpacks.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

SWMM_ENGINE_API int swmm_snowpack_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    // Reject duplicates (case-insensitive) BEFORE touching the backing store;
    // an unguarded NameIndex::add would throw across the C ABI.
    if (ctx.snowpack_names.find(id) >= 0) return SWMM_ERR_BADPARAM;

    auto& sp = ctx.snowpacks;
    sp.names.push_back(id);
    sp.plowable.push_back({});
    sp.impervious.push_back({});
    sp.pervious.push_back({});
    sp.removal.push_back({});
    sp.removal_subcatch.push_back("");

    ctx.snowpack_names.add(id);
    return SWMM_OK;
}

namespace {
// The three snow-melt surfaces (PLOWABLE / IMPERVIOUS / PERVIOUS) share an
// identical 7-value layout; these helpers back the per-surface API functions.
// Surface parameters seed per-subcatchment snow state at start(), so writes are
// pre-start-only (BUILDING / OPENED).
int snowpack_set_surface(openswmm::SimulationContext& ctx,
                         std::vector<std::array<double, 7>>& surf, int idx,
                         double cmin, double cmax, double tbase,
                         double fwfrac, double sd0, double fw0, double last) {
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;
    if (idx < 0 || idx >= ctx.snowpacks.count()) return SWMM_ERR_BADINDEX;
    auto& p = surf[static_cast<std::size_t>(idx)];
    p[0] = cmin; p[1] = cmax; p[2] = tbase; p[3] = fwfrac;
    p[4] = sd0;  p[5] = fw0;  p[6] = last;
    return SWMM_OK;
}

int snowpack_get_surface(const openswmm::SimulationContext& ctx,
                         const std::vector<std::array<double, 7>>& surf, int idx,
                         double* cmin, double* cmax, double* tbase,
                         double* fwfrac, double* sd0, double* fw0, double* last) {
    if (idx < 0 || idx >= ctx.snowpacks.count()) return SWMM_ERR_BADINDEX;
    const auto& p = surf[static_cast<std::size_t>(idx)];
    if (cmin)   *cmin   = p[0];
    if (cmax)   *cmax   = p[1];
    if (tbase)  *tbase  = p[2];
    if (fwfrac) *fwfrac = p[3];
    if (sd0)    *sd0    = p[4];
    if (fw0)    *fw0    = p[5];
    if (last)   *last   = p[6];
    return SWMM_OK;
}
} // namespace

SWMM_ENGINE_API int swmm_snowpack_set_plowable(SWMM_Engine engine, int idx,
                                               double cmin, double cmax, double tbase,
                                               double fwfrac, double sd0, double fw0, double last) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    return snowpack_set_surface(ctx, ctx.snowpacks.plowable, idx, cmin, cmax, tbase, fwfrac, sd0, fw0, last);
}

SWMM_ENGINE_API int swmm_snowpack_get_plowable(SWMM_Engine engine, int idx,
                                               double* cmin, double* cmax, double* tbase,
                                               double* fwfrac, double* sd0, double* fw0, double* last) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return snowpack_get_surface(ctx, ctx.snowpacks.plowable, idx, cmin, cmax, tbase, fwfrac, sd0, fw0, last);
}

SWMM_ENGINE_API int swmm_snowpack_set_impervious(SWMM_Engine engine, int idx,
                                                 double cmin, double cmax, double tbase,
                                                 double fwfrac, double sd0, double fw0, double last) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    return snowpack_set_surface(ctx, ctx.snowpacks.impervious, idx, cmin, cmax, tbase, fwfrac, sd0, fw0, last);
}

SWMM_ENGINE_API int swmm_snowpack_get_impervious(SWMM_Engine engine, int idx,
                                                 double* cmin, double* cmax, double* tbase,
                                                 double* fwfrac, double* sd0, double* fw0, double* last) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return snowpack_get_surface(ctx, ctx.snowpacks.impervious, idx, cmin, cmax, tbase, fwfrac, sd0, fw0, last);
}

SWMM_ENGINE_API int swmm_snowpack_set_pervious(SWMM_Engine engine, int idx,
                                               double cmin, double cmax, double tbase,
                                               double fwfrac, double sd0, double fw0, double last) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    return snowpack_set_surface(ctx, ctx.snowpacks.pervious, idx, cmin, cmax, tbase, fwfrac, sd0, fw0, last);
}

SWMM_ENGINE_API int swmm_snowpack_get_pervious(SWMM_Engine engine, int idx,
                                               double* cmin, double* cmax, double* tbase,
                                               double* fwfrac, double* sd0, double* fw0, double* last) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    return snowpack_get_surface(ctx, ctx.snowpacks.pervious, idx, cmin, cmax, tbase, fwfrac, sd0, fw0, last);
}

SWMM_ENGINE_API int swmm_snowpack_set_removal(SWMM_Engine engine, int idx,
                                              double dsnow, double fout, double fimp,
                                              double fperv, double fimelt, double fsubcatch) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.snowpacks.count());
    auto& r = ctx.snowpacks.removal[static_cast<std::size_t>(idx)];
    r[0] = dsnow; r[1] = fout; r[2] = fimp; r[3] = fperv; r[4] = fimelt; r[5] = fsubcatch;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_snowpack_get_removal(SWMM_Engine engine, int idx,
                                              double* dsnow, double* fout, double* fimp,
                                              double* fperv, double* fimelt, double* fsubcatch) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.snowpacks.count());
    const auto& r = ctx.snowpacks.removal[static_cast<std::size_t>(idx)];
    if (dsnow)     *dsnow     = r[0];
    if (fout)      *fout      = r[1];
    if (fimp)      *fimp      = r[2];
    if (fperv)     *fperv     = r[3];
    if (fimelt)    *fimelt    = r[4];
    if (fsubcatch) *fsubcatch = r[5];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_snowpack_set_removal_subcatch(SWMM_Engine engine, int idx, const char* name) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.snowpacks.count());
    ctx.snowpacks.removal_subcatch[static_cast<std::size_t>(idx)] = (name ? std::string(name) : std::string{});
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_snowpack_get_removal_subcatch(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.snowpacks.count());
    const std::string& s = ctx.snowpacks.removal_subcatch[static_cast<std::size_t>(idx)];
    const std::size_t n = std::min(s.size(), static_cast<std::size_t>(buflen - 1));
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return SWMM_OK;
}

// ============================================================================
// Precipitation scale factors
//
// No CHECK_GEOMETRY on the setters — these are deliberately settable mid-run so
// a calibration or RTC loop can drive them, matching swmm_gage_set_scale_factor.
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_set_rain_scale_factor(SWMM_Engine engine, int idx, double factor) {
    CHECK_HANDLE(engine);
    if (factor <= 0.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.rain_scale_factor[static_cast<std::size_t>(idx)] = factor;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_rain_scale_factor(SWMM_Engine engine, int idx, double* factor) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (factor) *factor = ctx.subcatches.rain_scale_factor[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_snow_scale_factor(SWMM_Engine engine, int idx, double factor) {
    CHECK_HANDLE(engine);
    if (factor <= 0.0) return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.snow_scale_factor[static_cast<std::size_t>(idx)] = factor;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_snow_scale_factor(SWMM_Engine engine, int idx, double* factor) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (factor) *factor = ctx.subcatches.snow_scale_factor[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

} /* extern "C" */
