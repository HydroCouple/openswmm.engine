/**
 * @file openswmm_infrastructure_impl.cpp
 * @brief C API implementation — transects, streets, inlets, LID controls, LID usage.
 *
 * @see include/openswmm/engine/openswmm_infrastructure.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_infrastructure.h"

extern "C" {

// ============================================================================
// Transects
// ============================================================================

SWMM_ENGINE_API int swmm_transect_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    auto& ts = ctx.transects;

    ts.names.push_back(id);
    ts.n_left.push_back(0.0);
    ts.n_right.push_back(0.0);
    ts.n_channel.push_back(0.0);
    ts.x_left_bank.push_back(0.0);
    ts.x_right_bank.push_back(0.0);
    ts.x_factor.push_back(1.0);
    ts.y_factor.push_back(1.0);
    ts.stations.push_back({});
    ts.elevations.push_back({});

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_set_roughness(SWMM_Engine engine, int idx, double n_left, double n_right, double n_channel) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.transects.count());
    const auto ui = static_cast<std::size_t>(idx);
    ctx.transects.n_left[ui]    = n_left;
    ctx.transects.n_right[ui]   = n_right;
    ctx.transects.n_channel[ui] = n_channel;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_add_station(SWMM_Engine engine, int idx, double station, double elevation) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.transects.count());
    const auto ui = static_cast<std::size_t>(idx);
    ctx.transects.stations[ui].push_back(station);
    ctx.transects.elevations[ui].push_back(elevation);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().transects.count();
}

// ============================================================================
// Streets
// ============================================================================

SWMM_ENGINE_API int swmm_street_add(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    auto& st = ctx.streets;

    st.names.push_back(id);
    st.t_crown.push_back(0.0);
    st.h_curb.push_back(0.0);
    st.sx.push_back(0.0);
    st.n_road.push_back(0.0);
    st.gutter_depres.push_back(0.0);
    st.gutter_width.push_back(0.0);
    st.sides.push_back(1);
    st.back_width.push_back(0.0);
    st.back_slope.push_back(0.0);
    st.back_n.push_back(0.0);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_street_set_params(SWMM_Engine engine, int idx,
                                             double t_crown, double h_curb, double sx, double n_road,
                                             double gutter_depres, double gutter_width, int sides,
                                             double back_width, double back_slope, double back_n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.streets.count());
    const auto ui = static_cast<std::size_t>(idx);

    ctx.streets.t_crown[ui]       = t_crown;
    ctx.streets.h_curb[ui]        = h_curb;
    ctx.streets.sx[ui]            = sx;
    ctx.streets.n_road[ui]        = n_road;
    ctx.streets.gutter_depres[ui] = gutter_depres;
    ctx.streets.gutter_width[ui]  = gutter_width;
    ctx.streets.sides[ui]         = sides;
    ctx.streets.back_width[ui]    = back_width;
    ctx.streets.back_slope[ui]    = back_slope;
    ctx.streets.back_n[ui]        = back_n;

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_street_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().streets.count();
}

// ============================================================================
// Inlets
// ============================================================================

SWMM_ENGINE_API int swmm_inlet_add(SWMM_Engine engine, const char* id, const char* type) {
    CHECK_HANDLE(engine);
    if (!id || !type) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    auto& inl = ctx.inlets;

    inl.names.push_back(id);
    inl.inlet_type.push_back(type);
    inl.length.push_back(0.0);
    inl.width.push_back(0.0);
    inl.grate_type.push_back("");
    inl.open_area.push_back(0.0);
    inl.splash_veloc.push_back(0.0);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_inlet_set_params(SWMM_Engine engine, int idx, double length, double width,
                                            const char* grate_type, double open_area, double splash_veloc) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.inlets.count());
    const auto ui = static_cast<std::size_t>(idx);

    ctx.inlets.length[ui]       = length;
    ctx.inlets.width[ui]        = width;
    ctx.inlets.grate_type[ui]   = grate_type ? grate_type : "";
    ctx.inlets.open_area[ui]    = open_area;
    ctx.inlets.splash_veloc[ui] = splash_veloc;

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_inlet_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().inlets.count();
}

// ============================================================================
// LID controls
// ============================================================================

SWMM_ENGINE_API int swmm_lid_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;
    (void)type;  // TODO: store LID type when LID SoA store is expanded
    // Placeholder — LID control store not yet in InfraData; returns OK
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_surface(SWMM_Engine engine, int idx, double storage, double roughness, double slope) {
    CHECK_HANDLE(engine);
    (void)idx; (void)storage; (void)roughness; (void)slope;
    // TODO: implement when LID SoA store is expanded
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_soil(SWMM_Engine engine, int idx, double thick, double porosity, double fc, double wp, double ksat, double kslope) {
    CHECK_HANDLE(engine);
    (void)idx; (void)thick; (void)porosity; (void)fc; (void)wp; (void)ksat; (void)kslope;
    // TODO: implement when LID SoA store is expanded
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_storage(SWMM_Engine engine, int idx, double thick, double void_frac, double ksat) {
    CHECK_HANDLE(engine);
    (void)idx; (void)thick; (void)void_frac; (void)ksat;
    // TODO: implement when LID SoA store is expanded
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_drain(SWMM_Engine engine, int idx, double coeff, double expon, double offset) {
    CHECK_HANDLE(engine);
    (void)idx; (void)coeff; (void)expon; (void)offset;
    // TODO: implement when LID SoA store is expanded
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_count(SWMM_Engine engine) {
    CHECK_HANDLE(engine);
    // TODO: return actual LID count when LID SoA store is expanded
    return 0;
}

// ============================================================================
// LID usage (assign LID to subcatchment)
// ============================================================================

SWMM_ENGINE_API int swmm_lid_usage_add(SWMM_Engine engine, int subcatch_idx, int lid_idx, int number, double area, double width, double init_sat, double from_imperv) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(subcatch_idx >= 0 && subcatch_idx < ctx.n_subcatches());
    (void)lid_idx; (void)number; (void)area; (void)width; (void)init_sat; (void)from_imperv;
    // TODO: implement when LID usage SoA store is expanded
    return SWMM_OK;
}

} /* extern "C" */
