/**
 * @file openswmm_subcatchments_impl.cpp
 * @brief C API implementation — subcatchment identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_subcatchments.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_subcatchments.h"

#include <algorithm>
#include <cstring>
#include <string>

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
    ctx.subcatches.width[static_cast<std::size_t>(idx)] = width;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_set_slope(SWMM_Engine engine, int idx, double slope) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    ctx.subcatches.slope[static_cast<std::size_t>(idx)] = slope;
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
                                                           double cn) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    auto uidx = static_cast<std::size_t>(idx);
    ctx.subcatches.infil_model[uidx] = 4; // CURVE_NUMBER
    ctx.subcatches.infil_p1[uidx] = cn;
    return SWMM_OK;
}

// ============================================================================
// Property getters
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_area(SWMM_Engine engine, int idx, double* area) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
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
    if (w) *w = ctx.subcatches.width[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_slope(SWMM_Engine engine, int idx, double* s) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (s) *s = ctx.subcatches.slope[static_cast<std::size_t>(idx)];
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

SWMM_ENGINE_API int swmm_subcatch_get_infil_curve_number(SWMM_Engine engine, int idx, double* cn) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (cn) *cn = ctx.subcatches.infil_p1[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Subcatchment statistics
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_stat_precip(SWMM_Engine engine, int idx, double* vol) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (vol) *vol = ctx.subcatches.stat_precip_vol[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_stat_runoff_vol(SWMM_Engine engine, int idx, double* vol) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (vol) *vol = ctx.subcatches.stat_runoff_vol[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_stat_max_runoff(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (rate) *rate = ctx.subcatches.stat_max_runoff[static_cast<std::size_t>(idx)];
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

// ============================================================================
// Hydraulic state getters
// ============================================================================

SWMM_ENGINE_API int swmm_subcatch_get_runoff(SWMM_Engine engine, int idx, double* runoff) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (runoff) *runoff = ctx.subcatches.runoff[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_groundwater(SWMM_Engine engine, int idx, double* gw_flow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (gw_flow) *gw_flow = ctx.subcatches.gw_flow[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_rainfall(SWMM_Engine engine, int idx, double* rainfall) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (rainfall) *rainfall = ctx.subcatches.rainfall[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_snow_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    // Snow state is managed by SnowSolver, not SubcatchData — return 0.0 for now
    if (depth) *depth = 0.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_evap(SWMM_Engine engine, int idx, double* evap) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (evap) *evap = ctx.subcatches.evap_loss[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil(SWMM_Engine engine, int idx, double* infil) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_subcatches());
    if (infil) *infil = ctx.subcatches.infil_loss[static_cast<std::size_t>(idx)];
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
    ctx.subcatches.rainfall[static_cast<std::size_t>(idx)] = rainfall;
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
    std::copy(ctx.subcatches.runoff.begin(), ctx.subcatches.runoff.begin() + n, buf);
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
    std::copy(ctx.subcatches.rainfall.begin(),
              ctx.subcatches.rainfall.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_evap_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    std::copy(ctx.subcatches.evap_loss.begin(),
              ctx.subcatches.evap_loss.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_subcatch_get_infil_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_subcatches());
    std::copy(ctx.subcatches.infil_loss.begin(),
              ctx.subcatches.infil_loss.begin() + n, buf);
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
    const auto& names = to_engine(engine)->context().aquifers.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
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

// ============================================================================
// Snowpacks ([SNOWPACKS] section) — Slice BM.0 list + add; setters land with BP
// ============================================================================

SWMM_ENGINE_API int swmm_snowpack_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().snowpacks.count();
}

SWMM_ENGINE_API int swmm_snowpack_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    const auto& names = to_engine(engine)->context().snowpacks.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
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

} /* extern "C" */
