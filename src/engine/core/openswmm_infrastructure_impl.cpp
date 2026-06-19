/**
 * @file openswmm_infrastructure_impl.cpp
 * @brief C API implementation — transects, streets, inlets, LID controls, LID usage.
 *
 * @see include/openswmm/engine/openswmm_infrastructure.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_infrastructure.h"

#include <cctype>
#include <string>

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
    ts.comments.push_back(std::string{});
    ts.n_left.push_back(0.0);
    ts.n_right.push_back(0.0);
    ts.n_channel.push_back(0.0);
    ts.x_left_bank.push_back(0.0);
    ts.x_right_bank.push_back(0.0);
    ts.x_left_encroachment.push_back(0.0);
    ts.x_right_encroachment.push_back(0.0);
    ts.x_factor.push_back(1.0);
    ts.y_factor.push_back(1.0);
    ts.length_factor.push_back(1.0);
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

SWMM_ENGINE_API int swmm_transect_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    const auto& names = to_engine(engine)->context().transects.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
}

SWMM_ENGINE_API const char* swmm_transect_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().transects.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

// ----------------------------------------------------------------------------
// Per-field getters / setters (DA-ENG-09 + BQ-TR-02)
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_transect_get_roughness(SWMM_Engine engine, int idx,
                                                 double* n_left, double* n_right, double* n_channel) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    if (n_left)    *n_left    = ts.n_left[ui];
    if (n_right)   *n_right   = ts.n_right[ui];
    if (n_channel) *n_channel = ts.n_channel[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_set_bank_stations(SWMM_Engine engine, int idx,
                                                     double x_left, double x_right) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    ts.x_left_bank[ui]  = x_left;
    ts.x_right_bank[ui] = x_right;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_get_bank_stations(SWMM_Engine engine, int idx,
                                                     double* x_left, double* x_right) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    if (x_left)  *x_left  = ts.x_left_bank[ui];
    if (x_right) *x_right = ts.x_right_bank[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_set_encroachment_stations(SWMM_Engine engine, int idx,
                                                             double x_left, double x_right) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    ts.x_left_encroachment[ui]  = x_left;
    ts.x_right_encroachment[ui] = x_right;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_get_encroachment_stations(SWMM_Engine engine, int idx,
                                                             double* x_left, double* x_right) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    if (x_left)  *x_left  = ts.x_left_encroachment[ui];
    if (x_right) *x_right = ts.x_right_encroachment[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_set_modifiers(SWMM_Engine engine, int idx,
                                                 double x_factor, double y_factor, double length_factor) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    ts.x_factor[ui]      = x_factor;
    ts.y_factor[ui]      = y_factor;
    ts.length_factor[ui] = length_factor;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_get_modifiers(SWMM_Engine engine, int idx,
                                                 double* x_factor, double* y_factor, double* length_factor) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    if (x_factor)      *x_factor      = ts.x_factor[ui];
    if (y_factor)      *y_factor      = ts.y_factor[ui];
    if (length_factor) *length_factor = ts.length_factor[ui];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_set_comments(SWMM_Engine engine, int idx, const char* text) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    ts.comments[ui] = (text ? std::string(text) : std::string{});
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_get_comments(SWMM_Engine engine, int idx, char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    const std::string& s = ts.comments[ui];
    const std::size_t n = std::min(s.size(), static_cast<std::size_t>(buflen - 1));
    std::memcpy(buf, s.data(), n);
    buf[n] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_get_station_count(SWMM_Engine engine, int idx) {
    if (!engine) return -1;
    auto& ts = to_engine(engine)->context().transects;
    if (idx < 0 || idx >= ts.count()) return -1;
    return static_cast<int>(ts.stations[static_cast<std::size_t>(idx)].size());
}

SWMM_ENGINE_API int swmm_transect_get_station(SWMM_Engine engine, int idx, int station_idx,
                                               double* station, double* elevation) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    const auto& xs = ts.stations[ui];
    const auto& ys = ts.elevations[ui];
    CHECK_INDEX(station_idx >= 0 && station_idx < static_cast<int>(xs.size()));
    const auto si = static_cast<std::size_t>(station_idx);
    if (station)   *station   = xs[si];
    if (elevation) *elevation = ys[si];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_clear_stations(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);
    ts.stations[ui].clear();
    ts.elevations[ui].clear();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_rename(SWMM_Engine engine, int idx, const char* new_id) {
    CHECK_HANDLE(engine);
    if (!new_id || new_id[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ts = to_engine(engine)->context().transects;
    CHECK_INDEX(idx >= 0 && idx < ts.count());
    const auto ui = static_cast<std::size_t>(idx);

    // Same-name (case-sensitive) is a no-op.
    if (ts.names[ui] == new_id) return SWMM_OK;

    // Case-insensitive collision check against every other slot.
    auto ieq = [](const std::string& a, const std::string& b) {
        if (a.size() != b.size()) return false;
        for (std::size_t i = 0; i < a.size(); ++i) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    };
    const std::string newName(new_id);
    for (std::size_t i = 0; i < ts.names.size(); ++i) {
        if (i == ui) continue;
        if (ieq(ts.names[i], newName)) return SWMM_ERR_BADPARAM;
    }

    ts.names[ui] = newName;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_transect_remove(SWMM_Engine engine, int idx) {
    CHECK_HANDLE(engine);
    auto& ts = to_engine(engine)->context().transects;
    // Out-of-range is a no-op SWMM_OK (mirrors pattern mutation API).
    if (idx < 0 || idx >= ts.count()) return SWMM_OK;
    const auto ui = static_cast<std::size_t>(idx);

    ts.names.erase(ts.names.begin() + ui);
    ts.comments.erase(ts.comments.begin() + ui);
    ts.n_left.erase(ts.n_left.begin() + ui);
    ts.n_right.erase(ts.n_right.begin() + ui);
    ts.n_channel.erase(ts.n_channel.begin() + ui);
    ts.x_left_bank.erase(ts.x_left_bank.begin() + ui);
    ts.x_right_bank.erase(ts.x_right_bank.begin() + ui);
    ts.x_left_encroachment.erase(ts.x_left_encroachment.begin() + ui);
    ts.x_right_encroachment.erase(ts.x_right_encroachment.begin() + ui);
    ts.x_factor.erase(ts.x_factor.begin() + ui);
    ts.y_factor.erase(ts.y_factor.begin() + ui);
    ts.length_factor.erase(ts.length_factor.begin() + ui);
    ts.stations.erase(ts.stations.begin() + ui);
    ts.elevations.erase(ts.elevations.begin() + ui);

    return SWMM_OK;
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

SWMM_ENGINE_API int swmm_street_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    const auto& names = to_engine(engine)->context().streets.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
}

SWMM_ENGINE_API const char* swmm_street_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().streets.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

SWMM_ENGINE_API int swmm_street_get_params(SWMM_Engine engine, int idx,
                                             double* t_crown, double* h_curb, double* sx, double* n_road,
                                             double* gutter_depres, double* gutter_width, int* sides,
                                             double* back_width, double* back_slope, double* back_n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.streets.count());
    const auto ui = static_cast<std::size_t>(idx);

    if (t_crown)       *t_crown       = ctx.streets.t_crown[ui];
    if (h_curb)        *h_curb        = ctx.streets.h_curb[ui];
    if (sx)            *sx            = ctx.streets.sx[ui];
    if (n_road)        *n_road        = ctx.streets.n_road[ui];
    if (gutter_depres) *gutter_depres = ctx.streets.gutter_depres[ui];
    if (gutter_width)  *gutter_width  = ctx.streets.gutter_width[ui];
    if (sides)         *sides         = ctx.streets.sides[ui];
    if (back_width)    *back_width    = ctx.streets.back_width[ui];
    if (back_slope)    *back_slope    = ctx.streets.back_slope[ui];
    if (back_n)        *back_n        = ctx.streets.back_n[ui];

    return SWMM_OK;
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

SWMM_ENGINE_API int swmm_inlet_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    const auto& names = to_engine(engine)->context().inlets.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
}

SWMM_ENGINE_API const char* swmm_inlet_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().inlets.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

// ============================================================================
// LID controls
// ============================================================================

namespace {
// LID type codes match legacy SWMM 5 2-letter codes used by the .inp parser
// (HydrologyHandler [LID_CONTROLS]). Index = openswmm_infrastructure.h type
// enum (0 = BIO_CELL, 1 = RAIN_GARDEN, ...).
inline const char* lid_type_code(int type) {
    static const char* codes[] = {"BC", "RG", "GR", "IT", "PP", "RB", "RD", "VS"};
    if (type < 0 || type >= static_cast<int>(sizeof(codes) / sizeof(codes[0])))
        return "";
    return codes[type];
}
} // namespace

SWMM_ENGINE_API int swmm_lid_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;

    auto& lid = ctx.lid_controls;
    lid.names.push_back(id);
    lid.lid_type.push_back(lid_type_code(type));
    lid.surface.push_back({});
    lid.soil.push_back({});
    lid.pavement.push_back({});
    lid.storage.push_back({});
    lid.drain.push_back({});
    lid.drainmat.push_back({});
    lid.removals.push_back({});

    ctx.lid_names.add(id);
    return SWMM_OK;
}

// The surface/soil/storage layer parameters seed per-unit LID state at
// start() (soil moisture from wilting point/porosity, storage depth from the
// initial saturation), so mid-run mutation has no single correct meaning —
// they are pre-start-only (SWMM_ERR_LIFECYCLE while running). The drain
// parameters are pure flux coefficients evaluated each step against current
// head, so swmm_lid_set_drain is callable mid-run and refreshes the LID
// solver's per-unit parameter cache.

SWMM_ENGINE_API int swmm_lid_set_surface(SWMM_Engine engine, int idx, double storage, double roughness, double slope) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(idx >= 0 && idx < ctx.lid_controls.count());
    if (storage < 0.0 || roughness < 0.0 || slope < 0.0)
        return SWMM_ERR_BADPARAM;
    // SURFACE layer: [0]=StorHt, [1]=VegVolFrac, [2]=Roughness, [3]=SurfSlope
    auto& p = ctx.lid_controls.surface[static_cast<std::size_t>(idx)];
    p[0] = storage;
    p[2] = roughness;
    p[3] = slope;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_soil(SWMM_Engine engine, int idx, double thick, double porosity, double fc, double wp, double ksat, double kslope) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(idx >= 0 && idx < ctx.lid_controls.count());
    if (thick < 0.0 || porosity <= 0.0 || porosity > 1.0
        || fc < 0.0 || fc >= porosity || wp < 0.0 || wp >= fc
        || ksat < 0.0)
        return SWMM_ERR_BADPARAM;
    // SOIL layer: [0]=Thick, [1]=Poros, [2]=FC, [3]=WP, [4]=Ksat, [5]=Kslope
    auto& p = ctx.lid_controls.soil[static_cast<std::size_t>(idx)];
    p[0] = thick;
    p[1] = porosity;
    p[2] = fc;
    p[3] = wp;
    p[4] = ksat;
    p[5] = kslope;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_storage(SWMM_Engine engine, int idx, double thick, double void_frac, double ksat) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    if (ctx.state != openswmm::EngineState::BUILDING &&
        ctx.state != openswmm::EngineState::OPENED)
        return SWMM_ERR_LIFECYCLE;
    CHECK_INDEX(idx >= 0 && idx < ctx.lid_controls.count());
    if (thick < 0.0 || void_frac <= 0.0 || void_frac > 1.0 || ksat < 0.0)
        return SWMM_ERR_BADPARAM;
    // STORAGE layer: [0]=Thick, [1]=VoidRatio, [2]=Ksat
    auto& p = ctx.lid_controls.storage[static_cast<std::size_t>(idx)];
    p[0] = thick;
    p[1] = void_frac;
    p[2] = ksat;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_set_drain(SWMM_Engine engine, int idx, double coeff, double expon, double offset) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.lid_controls.count());
    if (coeff < 0.0 || expon < 0.0 || offset < 0.0)
        return SWMM_ERR_BADPARAM;
    // DRAIN layer: [0]=Coeff, [1]=Expon, [2]=Offset
    auto& p = ctx.lid_controls.drain[static_cast<std::size_t>(idx)];
    p[0] = coeff;
    p[1] = expon;
    p[2] = offset;
    // The step loop reads the LID solver's per-unit copies; refresh them so a
    // mid-run edit takes effect on the next step (no-op before start()).
    to_engine(engine)->refreshLIDDrainParams();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_lid_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().lid_controls.count();
}

SWMM_ENGINE_API int swmm_lid_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    const auto& names = to_engine(engine)->context().lid_controls.names;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == id) return static_cast<int>(i);
    }
    return -1;
}

SWMM_ENGINE_API const char* swmm_lid_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& names = to_engine(engine)->context().lid_controls.names;
    if (idx < 0 || idx >= static_cast<int>(names.size())) return nullptr;
    return names[static_cast<std::size_t>(idx)].c_str();
}

// ============================================================================
// LID usage (assign LID to subcatchment)
// ============================================================================

SWMM_ENGINE_API int swmm_lid_usage_add(SWMM_Engine engine, int subcatch_idx, int lid_idx, int number, double area, double width, double init_sat, double from_imperv) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(subcatch_idx >= 0 && subcatch_idx < ctx.n_subcatches());
    CHECK_INDEX(lid_idx >= 0 && lid_idx < ctx.lid_controls.count());
    auto& u = ctx.lid_usage;
    u.subcatch_index.push_back(subcatch_idx);
    u.lid_index.push_back(lid_idx);
    u.number.push_back(number);
    u.area.push_back(area);
    u.width.push_back(width);
    u.init_sat.push_back(init_sat);
    u.from_imperv.push_back(from_imperv);
    u.to_perv.push_back(0);
    u.rpt_file.emplace_back();
    u.drain_to.emplace_back();
    u.from_perv.push_back(0.0);
    u.resize_wb(u.count());   // keep water-balance vectors sized to the config rows
    return SWMM_OK;
}

// Total number of LID usage rows across all subcatchments. To enumerate the
// usages on one subcatchment, iterate [0, count) and filter on the
// subcatch_idx returned by swmm_lid_usage_get (mirrors how ext-inflows are
// filtered per node).
SWMM_ENGINE_API int swmm_lid_usage_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().lid_usage.count();
}

// Read one LID usage row by global index. Any out-param may be null.
SWMM_ENGINE_API int swmm_lid_usage_get(SWMM_Engine engine, int usage_idx,
                                       int* subcatch_idx, int* lid_idx, int* number,
                                       double* area, double* width, double* init_sat,
                                       double* from_imperv, int* to_perv, double* from_perv) {
    CHECK_HANDLE(engine);
    const auto& u = to_engine(engine)->context().lid_usage;
    CHECK_INDEX(usage_idx >= 0 && usage_idx < u.count());
    auto ui = static_cast<std::size_t>(usage_idx);
    if (subcatch_idx) *subcatch_idx = u.subcatch_index[ui];
    if (lid_idx)      *lid_idx      = u.lid_index[ui];
    if (number)       *number       = u.number[ui];
    if (area)         *area         = u.area[ui];
    if (width)        *width        = u.width[ui];
    if (init_sat)     *init_sat     = u.init_sat[ui];
    if (from_imperv)  *from_imperv  = u.from_imperv[ui];
    if (to_perv)      *to_perv      = u.to_perv[ui];
    if (from_perv)    *from_perv    = u.from_perv[ui];
    return SWMM_OK;
}

// Remove one LID usage row by global index (erases the row from every parallel
// vector in lockstep). Indices of later rows shift down by one.
SWMM_ENGINE_API int swmm_lid_usage_remove(SWMM_Engine engine, int usage_idx) {
    CHECK_HANDLE(engine);
    auto& u = to_engine(engine)->context().lid_usage;
    CHECK_INDEX(usage_idx >= 0 && usage_idx < u.count());
    auto ui = static_cast<std::ptrdiff_t>(usage_idx);
    u.subcatch_index.erase(u.subcatch_index.begin() + ui);
    u.lid_index.erase(u.lid_index.begin() + ui);
    u.number.erase(u.number.begin() + ui);
    u.area.erase(u.area.begin() + ui);
    u.width.erase(u.width.begin() + ui);
    u.init_sat.erase(u.init_sat.begin() + ui);
    u.from_imperv.erase(u.from_imperv.begin() + ui);
    u.to_perv.erase(u.to_perv.begin() + ui);
    u.rpt_file.erase(u.rpt_file.begin() + ui);
    u.drain_to.erase(u.drain_to.begin() + ui);
    u.from_perv.erase(u.from_perv.begin() + ui);
    u.resize_wb(u.count());
    return SWMM_OK;
}

} /* extern "C" */
