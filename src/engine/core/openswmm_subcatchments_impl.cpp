/**
 * @file openswmm_subcatchments_impl.cpp
 * @brief C API implementation — subcatchment identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_subcatchments.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_subcatchments.h"

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
    ctx.subcatches.resize(n);

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

} /* extern "C" */
