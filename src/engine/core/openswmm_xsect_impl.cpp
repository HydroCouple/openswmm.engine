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
 * @file openswmm_xsect_impl.cpp
 * @brief C API implementation — standalone cross-section geometry.
 *
 * @details A SWMM_XSect handle owns an XSectParams plus, for the tabulated
 *          shapes, its own TransectData. Owning the table (rather than
 *          borrowing ctx.transect_tables) is what lets a handle taken from a
 *          link outlive the engine it came from.
 *
 *          Every query converts display units → internal ft, calls the same
 *          xsect:: kernel the routing solvers call, and converts the result
 *          back. Construction likewise goes through xsect::setParams /
 *          link::applyTabulatedXSectParams — there is no geometry maths in this
 *          file, by design: a second implementation would be a second thing to
 *          keep at legacy parity.
 *
 * @see include/openswmm/engine/openswmm_xsect.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_xsect.h"
#include "../hydraulics/Link.hpp"
#include "../hydraulics/Street.hpp"
#include "../hydraulics/Transect.hpp"
#include "../hydraulics/XSectBatch.hpp"
#include "UnitConversion.hpp"
#include "TypeHelpers.hpp"

#include <cmath>
#include <memory>
#include <vector>

namespace {

using openswmm::XSectParams;
using openswmm::XSectShape;
using openswmm::XsectShape;
using openswmm::transect::TransectData;

// ============================================================================
// Handle
// ============================================================================

// Non-copyable on purpose: params.area_tbl/hrad_tbl/width_tbl point into
// `table`, so a copy would leave the clone's pointers aimed at the original's
// storage. Handles are heap-allocated and passed by pointer; nothing needs to
// copy one.
struct XSectHandle {
    XSectParams  params{};
    TransectData table{};        ///< Owned; only populated for tabulated shapes.
    openswmm::chebsec::ChebSection cheb{}; ///< Owned; only populated for a compiled boundary.
    XsectShape   shape = XsectShape::CIRCULAR;   ///< Public (storage) shape code.
    int          unit_system = 0;                ///< 0 = US, 1 = SI.
    int          flow_units  = 0;                ///< FlowUnits code (0..5).

    XSectHandle() = default;
    XSectHandle(const XSectHandle&) = delete;
    XSectHandle& operator=(const XSectHandle&) = delete;

    /// Re-aim the params at this handle's own table. Call after `table` is set
    /// or moved — TransectData holds the tables inline, so its address changes.
    void adoptTable(int transect_idx) {
        openswmm::link::applyTabulatedXSectParams(params, table,
                                                  params.y_full, transect_idx);
    }
};

XSectHandle* to_xsect(SWMM_XSect h) noexcept {
    return static_cast<XSectHandle*>(h);
}

#define CHECK_XSECT(h)  do { if (!(h)) return SWMM_ERR_BADHANDLE; } while (0)
#define CHECK_OUT(p)    do { if (!(p)) return SWMM_ERR_BADPARAM;  } while (0)

// ============================================================================
// Units
// ============================================================================

// The handle carries flow_units (not just a unit system) because UCF() derives
// everything from it: length/area follow getUnitSystem(flow_units), while flow
// uses Qcf[flow_units] directly. A standalone handle is created with the base
// flow unit of its system.
constexpr int kFlowUnitsForSystem[2] = { 0 /*CFS*/, 3 /*CMS*/ };

/// Length UCF: display → internal is `v / ucf_len`, internal → display is `* ucf_len`.
double ucf_len(const XSectHandle& h) {
    return openswmm::ucf::Ucf[openswmm::ucf::LENGTH][static_cast<std::size_t>(h.unit_system)];
}

/// Flow UCF: display → internal (cfs) is `v / ucf_flow`.
double ucf_flow(const XSectHandle& h) {
    return openswmm::ucf::Qcf[static_cast<std::size_t>(h.flow_units)];
}

// Area is a length², and the section factor A·R^(2/3) is a length^(8/3).
double ucf_area(const XSectHandle& h) { const double L = ucf_len(h); return L * L; }
double ucf_sf(const XSectHandle& h)   {
    const double L = ucf_len(h);
    return L * L * std::pow(L, 2.0 / 3.0);
}

// ============================================================================
// Validation
// ============================================================================

bool valid_unit_system(int us) { return us == 0 || us == 1; }

/// A finite, non-negative query input. Rejects NaN (which would otherwise
/// propagate silently through the kernels) and negative depths/areas/flows.
bool valid_input(double v) { return std::isfinite(v) && v >= 0.0; }

bool is_tabulated(XsectShape s) {
    return s == XsectShape::IRREGULAR || s == XsectShape::CUSTOM ||
           s == XsectShape::STREET_XSECT || s == XsectShape::POLYGON;
}

bool valid_shape(int s) {
    return s >= static_cast<int>(XsectShape::CIRCULAR) &&
           s <= static_cast<int>(XsectShape::POLYGON);
}

// ============================================================================
// Query plumbing
// ============================================================================

// Every scalar query is "validate → to internal → kernel → to display", and
// every array query is the same over n elements. Expressing that once keeps the
// 9 × 2 entry points from drifting apart.
using Kernel = double (*)(const XSectParams&, double);

int query_scalar(SWMM_XSect h, double in, double* out, Kernel k,
                 double in_ucf, double out_ucf) {
    CHECK_XSECT(h);
    CHECK_OUT(out);
    if (!valid_input(in)) return SWMM_ERR_BADPARAM;
    const XSectHandle& xh = *to_xsect(h);
    *out = k(xh.params, in / in_ucf) * out_ucf;
    return SWMM_OK;
}

int query_array(SWMM_XSect h, const double* in, int n, double* out, Kernel k,
                double in_ucf, double out_ucf) {
    CHECK_XSECT(h);
    if (n < 0) return SWMM_ERR_BADPARAM;
    if (n == 0) return SWMM_OK;
    CHECK_OUT(in);
    CHECK_OUT(out);
    const XSectHandle& xh = *to_xsect(h);
    for (int i = 0; i < n; ++i) {
        if (!valid_input(in[i])) return SWMM_ERR_BADPARAM;
    }
    for (int i = 0; i < n; ++i) {
        out[i] = k(xh.params, in[i] / in_ucf) * out_ucf;
    }
    return SWMM_OK;
}

// Kernel adapters — xsect::isOpen takes an int and getAmax takes no value, so
// they do not match the Kernel signature directly.
double k_area_of_depth(const XSectParams& p, double y)  { return openswmm::xsect::getAofY(p, y); }
double k_width_of_depth(const XSectParams& p, double y) { return openswmm::xsect::getWofY(p, y); }
double k_hydrad_of_depth(const XSectParams& p, double y){ return openswmm::xsect::getRofY(p, y); }
double k_depth_of_area(const XSectParams& p, double a)  { return openswmm::xsect::getYofA(p, a); }
double k_hydrad_of_area(const XSectParams& p, double a) { return openswmm::xsect::getRofA(p, a); }
double k_sf_of_area(const XSectParams& p, double a)     { return openswmm::xsect::getSofA(p, a); }
double k_area_of_sf(const XSectParams& p, double s)     { return openswmm::xsect::getAofS(p, s); }
double k_dsda(const XSectParams& p, double a)           { return openswmm::xsect::getdSdA(p, a); }
double k_ycrit(const XSectParams& p, double q)          { return openswmm::xsect::getYcrit(p, q); }

const char* const kShapeNames[27] = {
    "CIRCULAR", "FILLED_CIRCULAR", "RECT_CLOSED", "RECT_OPEN", "TRAPEZOIDAL",
    "TRIANGULAR", "PARABOLIC", "POWER", "MODBASKETHANDLE", "EGGSHAPED",
    "HORSESHOE", "GOTHIC", "CATENARY", "SEMIELLIPTICAL", "BASKETHANDLE",
    "SEMICIRCULAR", "RECT_TRIANG", "RECT_ROUND", "HORIZ_ELLIPSE",
    "VERT_ELLIPSE", "ARCH", "IRREGULAR", "CUSTOM", "FORCE_MAIN",
    "STREET_XSECT", "DUMMY", "POLYGON"
};

} // namespace

extern "C" {

// ============================================================================
// Construction
// ============================================================================

SWMM_ENGINE_API int swmm_xsect_create(int shape, double geom1, double geom2,
                                      double geom3, double geom4,
                                      int unit_system, SWMM_XSect* out) {
    CHECK_OUT(out);
    *out = nullptr;
    if (!valid_unit_system(unit_system)) return SWMM_ERR_BADPARAM;
    if (!valid_shape(shape)) return SWMM_ERR_BADPARAM;

    const auto s = static_cast<XsectShape>(shape);
    // The tabulated shapes carry no geometry of their own — geom1 there is an
    // index into a table this constructor has no access to. Route the caller to
    // the constructor that can actually build the section.
    if (is_tabulated(s)) return SWMM_ERR_BADPARAM;

    if (!std::isfinite(geom1) || !std::isfinite(geom2) ||
        !std::isfinite(geom3) || !std::isfinite(geom4)) return SWMM_ERR_BADPARAM;

    auto h = std::make_unique<XSectHandle>();
    h->shape       = s;
    h->unit_system = unit_system;
    h->flow_units  = kFlowUnitsForSystem[static_cast<std::size_t>(unit_system)];

    if (s == XsectShape::DUMMY) {
        // No geometry: setParams has no DUMMY case, so stamp the type directly.
        // Every kernel switches on it and returns 0.
        h->params.type = static_cast<int>(XSectShape::DUMMY);
        *out = h.release();
        return SWMM_OK;
    }

    // Feed the RAW display geoms plus the length ucf, exactly as legacy
    // xsect_setParams expects: it divides only the length-valued params and
    // leaves slopes / size codes / C-factors alone.
    const double p[4] = { geom1, geom2, geom3, geom4 };
    const double ucf = openswmm::ucf::Ucf[openswmm::ucf::LENGTH]
                                         [static_cast<std::size_t>(unit_system)];
    // setParams returns 0 on success, -1 on rejected geometry.
    const int rc = openswmm::xsect::setParams(h->params,
                                              openswmm::link::translateShape(s),
                                              p, ucf);
    if (rc != 0 || h->params.a_full <= 0.0) return SWMM_ERR_BADPARAM;

    *out = h.release();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_xsect_create_irregular(
    const double* stations, const double* elevations, int n_pts,
    double x_left_bank, double x_right_bank,
    double n_left, double n_channel, double n_right, double length_factor,
    int unit_system, SWMM_XSect* out) {
    CHECK_OUT(out);
    *out = nullptr;
    CHECK_OUT(stations);
    CHECK_OUT(elevations);
    if (n_pts < 2) return SWMM_ERR_BADPARAM;
    if (!valid_unit_system(unit_system)) return SWMM_ERR_BADPARAM;
    if (!(n_channel > 0.0) || !std::isfinite(n_channel)) return SWMM_ERR_BADPARAM;
    if (n_left < 0.0 || n_right < 0.0 || length_factor < 0.0) return SWMM_ERR_BADPARAM;
    for (int i = 0; i < n_pts; ++i) {
        if (!std::isfinite(stations[i]) || !std::isfinite(elevations[i]))
            return SWMM_ERR_BADPARAM;
    }

    auto h = std::make_unique<XSectHandle>();
    h->shape       = XsectShape::IRREGULAR;
    h->unit_system = unit_system;
    h->flow_units  = kFlowUnitsForSystem[static_cast<std::size_t>(unit_system)];

    const double ucf = openswmm::ucf::Ucf[openswmm::ucf::LENGTH]
                                         [static_cast<std::size_t>(unit_system)];
    TransectData& td = h->table;
    td.name          = "";
    td.n_left        = n_left;
    td.n_right       = n_right;
    td.n_channel     = n_channel;
    td.length_factor = (length_factor > 0.0) ? length_factor : 1.0;
    td.stations.reserve(static_cast<std::size_t>(n_pts));
    td.elevations.reserve(static_cast<std::size_t>(n_pts));
    for (int i = 0; i < n_pts; ++i) {
        td.stations.push_back(stations[i] / ucf);
        td.elevations.push_back(elevations[i] / ucf);
    }
    td.x_left_bank  = x_left_bank / ucf;
    td.x_right_bank = x_right_bank / ucf;

    openswmm::transect::buildTables(td);
    if (!(td.a_full > 0.0) || !(td.y_full > 0.0)) return SWMM_ERR_BADPARAM;

    h->params.type = static_cast<int>(XSectShape::IRREGULAR);
    h->adoptTable(-1);
    *out = h.release();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_xsect_create_custom(
    double y_full, const double* curve_depths, const double* curve_widths,
    int n_pts, int unit_system, SWMM_XSect* out) {
    CHECK_OUT(out);
    *out = nullptr;
    CHECK_OUT(curve_depths);
    CHECK_OUT(curve_widths);
    if (n_pts < 2) return SWMM_ERR_BADPARAM;
    if (!valid_unit_system(unit_system)) return SWMM_ERR_BADPARAM;
    if (!(y_full > 0.0) || !std::isfinite(y_full)) return SWMM_ERR_BADPARAM;
    for (int i = 0; i < n_pts; ++i) {
        if (!std::isfinite(curve_depths[i]) || !std::isfinite(curve_widths[i]))
            return SWMM_ERR_BADPARAM;
    }

    auto h = std::make_unique<XSectHandle>();
    h->shape       = XsectShape::CUSTOM;
    h->unit_system = unit_system;
    h->flow_units  = kFlowUnitsForSystem[static_cast<std::size_t>(unit_system)];

    const double ucf = openswmm::ucf::Ucf[openswmm::ucf::LENGTH]
                                         [static_cast<std::size_t>(unit_system)];
    const double y_full_ft = y_full / ucf;

    // The curve is normalized (y/yFull vs w/wMax), so it needs no unit
    // conversion; buildCustomTables builds at unit height and
    // applyTabulatedXSectParams scales by y_full.
    openswmm::transect::buildCustomTables(h->table, y_full_ft, curve_depths,
                                          curve_widths, n_pts);
    if (!(h->table.a_full > 0.0)) return SWMM_ERR_BADPARAM;

    h->params.type   = static_cast<int>(XSectShape::CUSTOM);
    h->params.y_full = y_full_ft;   // adoptTable reads this for the CUSTOM scaling
    h->adoptTable(-1);
    *out = h.release();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_xsect_create_street(
    double width, double curb_height, double slope, double roughness,
    double gutter_depression, double gutter_width, int sides,
    double back_width, double back_slope, double back_roughness,
    int unit_system, SWMM_XSect* out) {
    CHECK_OUT(out);
    *out = nullptr;
    if (!valid_unit_system(unit_system)) return SWMM_ERR_BADPARAM;
    if (sides != 1 && sides != 2) return SWMM_ERR_BADPARAM;
    if (!(width > 0.0) || !(curb_height > 0.0) || !(roughness > 0.0))
        return SWMM_ERR_BADPARAM;
    if (!std::isfinite(slope) || !std::isfinite(gutter_depression) ||
        !std::isfinite(gutter_width) || !std::isfinite(back_width) ||
        !std::isfinite(back_slope) || !std::isfinite(back_roughness))
        return SWMM_ERR_BADPARAM;

    auto h = std::make_unique<XSectHandle>();
    h->shape       = XsectShape::STREET_XSECT;
    h->unit_system = unit_system;
    h->flow_units  = kFlowUnitsForSystem[static_cast<std::size_t>(unit_system)];

    // Mirrors the [STREETS] → StreetParams conversion in PostParseResolver /
    // swmm_link_set_xsect: lengths display → ft, slopes % → fraction.
    const double inv_len = openswmm::ucf::Ucf_inv[openswmm::ucf::LENGTH]
                                                 [static_cast<std::size_t>(unit_system)];
    openswmm::street::StreetParams sp;
    sp.width             = width             * inv_len;
    sp.curb_height       = curb_height       * inv_len;
    sp.slope             = slope             / 100.0;
    sp.roughness         = roughness;
    sp.gutter_depression = gutter_depression * inv_len;
    sp.gutter_width      = gutter_width      * inv_len;
    sp.sides             = sides;
    sp.back_width        = back_width        * inv_len;
    sp.back_slope        = back_slope        / 100.0;
    sp.back_roughness    = back_roughness;

    openswmm::street::buildTransect(sp, h->table);
    if (!(h->table.a_full > 0.0) || !(h->table.y_full > 0.0))
        return SWMM_ERR_BADPARAM;

    h->params.type = static_cast<int>(XSectShape::STREET_XSECT);
    h->adoptTable(-1);
    *out = h.release();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_create_xsect(SWMM_Engine engine, int link_idx,
                                           SWMM_XSect* out) {
    CHECK_HANDLE(engine);
    CHECK_OUT(out);
    *out = nullptr;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(link_idx);

    const auto s = ctx.links.xsect_shape[uidx];
    if (!valid_shape(static_cast<int>(s))) return SWMM_ERR_BADPARAM;

    // The full-flow properties this handle needs (r_full/s_full/s_max/*_bot and
    // the batch shape code) are derived by PostParseResolver, which runs at
    // open/finalize. In BUILDING state swmm_link_set_xsect has stored only
    // y_full/a_full/w_max and left xsect_batch_shape at 0 — which is DUMMY, so
    // a handle built here would answer 0 to every query instead of failing.
    // Refuse rather than hand back a plausible-looking wrong answer.
    if (ctx.state == openswmm::EngineState::BUILDING) return SWMM_ERR_LIFECYCLE;

    auto h = std::make_unique<XSectHandle>();
    h->shape       = s;
    h->flow_units  = static_cast<int>(ctx.options.flow_units);
    h->unit_system = openswmm::ucf::getUnitSystem(h->flow_units);

    // buildXSectParams reads the geometry the engine actually resolved, so a
    // link handle reflects the model rather than re-deriving from raw geoms.
    h->params = openswmm::link::buildXSectParams(ctx.links, uidx,
                                                 &ctx.transect_tables,
                                                 &ctx.cheb_sections);

    // ...but it takes the shape from links.xsect_batch_shape, which is a
    // hot-loop cache that SWMMEngine::initialize() fills — and then only for
    // conduits. At OPENED state it is still 0, which decodes as DUMMY and makes
    // every kernel return 0 for an otherwise perfectly good section. The
    // resolver has already populated the real geometry by then, so translate
    // the authoritative xsect_shape instead of trusting the cache.
    h->params.type = openswmm::link::translateShape(s);

    if (h->params.a_full <= 0.0 && s != XsectShape::DUMMY)
        return SWMM_ERR_BADPARAM;   // e.g. a pump: no cross-section to copy.

    // buildXSectParams aimed the table pointers into ctx.transect_tables, which
    // dies with the engine. Take our own copy and re-point at it so the handle
    // outlives the model.
    if (is_tabulated(s) && h->params.transect >= 0 &&
        static_cast<std::size_t>(h->params.transect) < ctx.transect_tables.size()) {
        h->table = ctx.transect_tables[static_cast<std::size_t>(h->params.transect)];
        h->params.area_tbl          = h->table.area_tbl;
        h->params.hrad_tbl          = h->table.hrad_tbl;
        h->params.width_tbl         = h->table.width_tbl;
        h->params.area_lut          = &h->table.area_lut;
        h->params.transect_tbl_size = openswmm::transect::N_TRANSECT_TBL;
    }

    // Same lifetime concern for a compiled Chebyshev boundary: buildXSectParams
    // aimed params.cheb into ctx.cheb_sections, which dies with the engine.
    // ChebSection is POD/trivially copyable (see ChebSection.hpp), so take our
    // own copy and re-point at it, exactly like the transect table above.
    if (h->params.cheb) {
        h->cheb = *h->params.cheb;
        h->params.cheb = &h->cheb;
    }

    *out = h.release();
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_xsect_free(SWMM_XSect xsect) {
    delete to_xsect(xsect);
    return SWMM_OK;
}

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_xsect_get_shape(SWMM_XSect xsect, int* shape) {
    CHECK_XSECT(xsect);
    CHECK_OUT(shape);
    *shape = static_cast<int>(to_xsect(xsect)->shape);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_xsect_get_units(SWMM_XSect xsect, int* unit_system,
                                         int* flow_units) {
    CHECK_XSECT(xsect);
    const XSectHandle& h = *to_xsect(xsect);
    if (unit_system) *unit_system = h.unit_system;
    if (flow_units)  *flow_units  = h.flow_units;
    return SWMM_OK;
}

SWMM_ENGINE_API const char* swmm_xsect_shape_name(int shape) {
    if (!valid_shape(shape)) return nullptr;
    return kShapeNames[static_cast<std::size_t>(shape)];
}

// ============================================================================
// Queries — scalar
// ============================================================================

SWMM_ENGINE_API int swmm_xsect_area_of_depth(SWMM_XSect h, double depth, double* area) {
    CHECK_XSECT(h);
    return query_scalar(h, depth, area, k_area_of_depth,
                        ucf_len(*to_xsect(h)), ucf_area(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_width_of_depth(SWMM_XSect h, double depth, double* width) {
    CHECK_XSECT(h);
    return query_scalar(h, depth, width, k_width_of_depth,
                        ucf_len(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_hydrad_of_depth(SWMM_XSect h, double depth, double* hydrad) {
    CHECK_XSECT(h);
    return query_scalar(h, depth, hydrad, k_hydrad_of_depth,
                        ucf_len(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_depth_of_area(SWMM_XSect h, double area, double* depth) {
    CHECK_XSECT(h);
    return query_scalar(h, area, depth, k_depth_of_area,
                        ucf_area(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_hydrad_of_area(SWMM_XSect h, double area, double* hydrad) {
    CHECK_XSECT(h);
    return query_scalar(h, area, hydrad, k_hydrad_of_area,
                        ucf_area(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_sectfactor_of_area(SWMM_XSect h, double area, double* sf) {
    CHECK_XSECT(h);
    return query_scalar(h, area, sf, k_sf_of_area,
                        ucf_area(*to_xsect(h)), ucf_sf(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_area_of_sectfactor(SWMM_XSect h, double sf, double* area) {
    CHECK_XSECT(h);
    return query_scalar(h, sf, area, k_area_of_sf,
                        ucf_sf(*to_xsect(h)), ucf_area(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_dsda(SWMM_XSect h, double area, double* dsda) {
    CHECK_XSECT(h);
    // dS/dA has units of length^(8/3) / length² = length^(2/3).
    const double out_ucf = std::pow(ucf_len(*to_xsect(h)), 2.0 / 3.0);
    return query_scalar(h, area, dsda, k_dsda, ucf_area(*to_xsect(h)), out_ucf);
}

SWMM_ENGINE_API int swmm_xsect_critical_depth(SWMM_XSect h, double flow, double* ycrit) {
    CHECK_XSECT(h);
    return query_scalar(h, flow, ycrit, k_ycrit,
                        ucf_flow(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_full_properties(SWMM_XSect h, double* y_full,
                                               double* a_full, double* r_full,
                                               double* w_max, double* s_full,
                                               double* a_max) {
    CHECK_XSECT(h);
    const XSectHandle& xh = *to_xsect(h);
    const double L = ucf_len(xh), A = ucf_area(xh), S = ucf_sf(xh);
    if (y_full) *y_full = xh.params.y_full * L;
    if (a_full) *a_full = xh.params.a_full * A;
    if (r_full) *r_full = xh.params.r_full * L;
    if (w_max)  *w_max  = xh.params.w_max  * L;
    if (s_full) *s_full = xh.params.s_full * S;
    if (a_max)  *a_max  = openswmm::xsect::getAmax(xh.params) * A;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_xsect_is_open(SWMM_XSect h, int* is_open) {
    CHECK_XSECT(h);
    CHECK_OUT(is_open);
    *is_open = openswmm::xsect::isOpen(to_xsect(h)->params.type) ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Queries — array
// ============================================================================

SWMM_ENGINE_API int swmm_xsect_area_of_depth_array(SWMM_XSect h, const double* depth,
                                                   int n, double* area) {
    CHECK_XSECT(h);
    return query_array(h, depth, n, area, k_area_of_depth,
                       ucf_len(*to_xsect(h)), ucf_area(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_width_of_depth_array(SWMM_XSect h, const double* depth,
                                                    int n, double* width) {
    CHECK_XSECT(h);
    return query_array(h, depth, n, width, k_width_of_depth,
                       ucf_len(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_hydrad_of_depth_array(SWMM_XSect h, const double* depth,
                                                     int n, double* hydrad) {
    CHECK_XSECT(h);
    return query_array(h, depth, n, hydrad, k_hydrad_of_depth,
                       ucf_len(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_depth_of_area_array(SWMM_XSect h, const double* area,
                                                   int n, double* depth) {
    CHECK_XSECT(h);
    return query_array(h, area, n, depth, k_depth_of_area,
                       ucf_area(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_hydrad_of_area_array(SWMM_XSect h, const double* area,
                                                    int n, double* hydrad) {
    CHECK_XSECT(h);
    return query_array(h, area, n, hydrad, k_hydrad_of_area,
                       ucf_area(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_sectfactor_of_area_array(SWMM_XSect h, const double* area,
                                                        int n, double* sf) {
    CHECK_XSECT(h);
    return query_array(h, area, n, sf, k_sf_of_area,
                       ucf_area(*to_xsect(h)), ucf_sf(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_area_of_sectfactor_array(SWMM_XSect h, const double* sf,
                                                        int n, double* area) {
    CHECK_XSECT(h);
    return query_array(h, sf, n, area, k_area_of_sf,
                       ucf_sf(*to_xsect(h)), ucf_area(*to_xsect(h)));
}

SWMM_ENGINE_API int swmm_xsect_dsda_array(SWMM_XSect h, const double* area,
                                          int n, double* dsda) {
    CHECK_XSECT(h);
    const double out_ucf = std::pow(ucf_len(*to_xsect(h)), 2.0 / 3.0);
    return query_array(h, area, n, dsda, k_dsda, ucf_area(*to_xsect(h)), out_ucf);
}

SWMM_ENGINE_API int swmm_xsect_critical_depth_array(SWMM_XSect h, const double* flow,
                                                    int n, double* ycrit) {
    CHECK_XSECT(h);
    return query_array(h, flow, n, ycrit, k_ycrit,
                       ucf_flow(*to_xsect(h)), ucf_len(*to_xsect(h)));
}

} // extern "C"
