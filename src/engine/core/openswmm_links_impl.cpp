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
 * @file openswmm_links_impl.cpp
 * @brief C API implementation — link identity, creation, properties, state, bulk.
 *
 * @see include/openswmm/engine/openswmm_links.h
 * @ingroup engine_api
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "openswmm_api_common.hpp"
#include "../../../include/openswmm/engine/openswmm_links.h"
#include "../input/PostParseResolver.hpp"
#include "../hydraulics/Street.hpp"
#include "../hydraulics/Transect.hpp"
#include "TypeHelpers.hpp"
#include "StringCase.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ============================================================================
// ABI ⇄ storage shape-code parity (compile-time)
// ============================================================================
//
// swmm_link_set_xsect/get_xsect pass the shape code straight through with a
// static_cast between `int` and openswmm::XsectShape — there is no translation
// step and there must not be one, because SWMM_XSectShape IS the storage
// numbering. These assertions are what make that cast sound: renumber either
// side and the build breaks here instead of silently storing the wrong shape.
//
// This is not hypothetical. Before 6.0 the two disagreed for every code from 8
// up (SWMM_XSECT_IRREGULAR=19 landed on VERT_ELLIPSE), and with nothing pinning
// them the defect reached the GUI's shape picker and the Python enum. Keep the
// list exhaustive — a shape added to XsectShape but not asserted here is
// exactly the hole that let it happen.
//
// NOTE: the legacy/batch openswmm::XSectShape (XSectBatch.hpp) is a THIRD,
// unrelated numbering used by the geometry kernels; cross it only through
// link::translateShape().
namespace {
#define OPENSWMM_ASSERT_SHAPE(abi, stored)                                     \
    static_assert(static_cast<int>(abi) ==                                     \
                      static_cast<int>(openswmm::XsectShape::stored),          \
                  #abi " must equal openswmm::XsectShape::" #stored            \
                       " — the C ABI and the engine's storage codes are the "  \
                       "same numbering (see openswmm_links.h)")

OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_CIRCULAR,        CIRCULAR);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_FILLED_CIRCULAR, FILLED_CIRCULAR);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_RECT_CLOSED,     RECT_CLOSED);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_RECT_OPEN,       RECT_OPEN);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_TRAPEZOIDAL,     TRAPEZOIDAL);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_TRIANGULAR,      TRIANGULAR);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_PARABOLIC,       PARABOLIC);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_POWER,           POWER);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_MOD_BASKET,      MODBASKETHANDLE);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_EGGSHAPED,       EGGSHAPED);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_HORSESHOE,       HORSESHOE);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_GOTHIC,          GOTHIC);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_CATENARY,        CATENARY);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_SEMIELLIPTICAL,  SEMIELLIPTICAL);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_BASKETHANDLE,    BASKETHANDLE);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_SEMICIRCULAR,    SEMICIRCULAR);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_RECT_TRIANG,     RECT_TRIANG);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_RECT_ROUND,      RECT_ROUND);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_HORIZ_ELLIPSE,   HORIZ_ELLIPSE);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_VERT_ELLIPSE,    VERT_ELLIPSE);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_ARCH,            ARCH);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_IRREGULAR,       IRREGULAR);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_CUSTOM,          CUSTOM);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_FORCE_MAIN,      FORCE_MAIN);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_STREET,          STREET_XSECT);
OPENSWMM_ASSERT_SHAPE(SWMM_XSECT_DUMMY,           DUMMY);

#undef OPENSWMM_ASSERT_SHAPE
} // namespace

extern "C" {

// ============================================================================
// Identity
// ============================================================================

SWMM_ENGINE_API int swmm_link_count(SWMM_Engine engine) {
    if (!engine) return -1;
    return to_engine(engine)->context().n_links();
}

SWMM_ENGINE_API int swmm_link_index(SWMM_Engine engine, const char* id) {
    if (!engine || !id) return -1;
    return to_engine(engine)->context().link_names.find(id);
}

SWMM_ENGINE_API const char* swmm_link_id(SWMM_Engine engine, int idx) {
    if (!engine) return nullptr;
    const auto& ctx = to_engine(engine)->context();
    if (idx < 0 || idx >= ctx.n_links()) return nullptr;
    return ctx.link_names.name_of(idx).c_str();
}

// ============================================================================
// Creation (BUILDING or OPENED — "editable" states)
// ============================================================================

SWMM_ENGINE_API int swmm_link_add(SWMM_Engine engine, const char* id, int type) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    openswmm::LinkType internal_type = openswmm::LinkType::CONDUIT;
    if (!openswmm::c_to_internal_link_type(type, internal_type))
        return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    if (ctx.link_names.find(id) >= 0)
        return SWMM_ERR_BADPARAM;

    int idx = ctx.link_names.add(id);
    int n = ctx.link_names.size();
    ctx.links.grow_to(n);
    const auto un = static_cast<std::size_t>(n);
    if (ctx.spatial.link_x.size() < un)            ctx.spatial.link_x.resize(un, 0.0);
    if (ctx.spatial.link_y.size() < un)            ctx.spatial.link_y.resize(un, 0.0);
    if (ctx.spatial.link_vertices_x.size() < un)  ctx.spatial.link_vertices_x.resize(un);
    if (ctx.spatial.link_vertices_y.size() < un)  ctx.spatial.link_vertices_y.resize(un);
    // Set type and create the subtype side-table row (single source of truth).
    // Mirrors swmm_node_add; keeps the wide type slot in sync via set_link_type.
    ctx.link_subtypes.set_link_type(ctx.links, idx, internal_type);

    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_pop_last(SWMM_Engine engine, const char* id) {
    CHECK_HANDLE(engine);
    if (!id) return SWMM_ERR_BADPARAM;

    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);

    const int n = ctx.link_names.size();
    if (n <= 0) return SWMM_ERR_BADINDEX;

    const int tail = n - 1;
    if (ctx.link_names.name_of(tail) != id)
        return SWMM_ERR_BADINDEX;

    ctx.link_names.pop_back();
    ctx.links.erase_at(tail);
    ctx.link_subtypes.erase_link(tail, ctx.links.count());
    // Shrink spatial arrays to match reduced link count
    if (!ctx.spatial.link_x.empty()) ctx.spatial.link_x.pop_back();
    if (!ctx.spatial.link_y.empty()) ctx.spatial.link_y.pop_back();
    if (!ctx.spatial.link_vertices_x.empty()) ctx.spatial.link_vertices_x.pop_back();
    if (!ctx.spatial.link_vertices_y.empty()) ctx.spatial.link_vertices_y.pop_back();
    return SWMM_OK;
}

// ============================================================================
// Connectivity
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_nodes(SWMM_Engine engine, int idx,
                                         int from_node_idx, int to_node_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.node1[static_cast<std::size_t>(idx)] = from_node_idx;
    ctx.links.node2[static_cast<std::size_t>(idx)] = to_node_idx;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_from_node(SWMM_Engine engine, int idx, int* node_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (node_idx) *node_idx = ctx.links.node1[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_to_node(SWMM_Engine engine, int idx, int* node_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (node_idx) *node_idx = ctx.links.node2[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Geometry setters
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_length(SWMM_Engine engine, int idx, double length) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display LENGTH -> internal ft
    {
        const double L = to_internal(ctx, openswmm::ucf::LENGTH, length);
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (cr >= 0) ctx.link_subtypes.conduits.length[static_cast<std::size_t>(cr)] = L;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_roughness(SWMM_Engine engine, int idx, double n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (cr >= 0) ctx.link_subtypes.conduits.roughness[static_cast<std::size_t>(cr)] = n;
    }
    // Refresh the conduit's derived dynamic-wave coefficients so the edited
    // roughness actually changes the simulation (slope and section geometry
    // are unaffected by a roughness edit, so this recompute is exact).
    openswmm::input::recompute_conduit_flow_properties(ctx, idx);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_offset_up(SWMM_Engine engine, int idx, double offset) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display LENGTH -> internal ft
    ctx.links.offset1[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, offset);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_offset_dn(SWMM_Engine engine, int idx, double offset) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display LENGTH -> internal ft
    ctx.links.offset2[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::LENGTH, offset);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_initial_flow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INITIAL_COND(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display FLOW -> internal cfs
    ctx.links.flow[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::FLOW, flow);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_max_flow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display FLOW -> internal cfs
    ctx.links.q_limit[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::FLOW, flow);
    return SWMM_OK;
}

// Engine gap BN-LINK-01a (added 2026-05-25) — symmetric getter for
// swmm_link_set_initial_flow. Reads the same SoA slot the setter writes.
// Read-only: usable in any post-construction state (no CHECK_GEOMETRY).
SWMM_ENGINE_API int swmm_link_get_initial_flow(SWMM_Engine engine, int idx, double* flow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal cfs -> display FLOW
    if (flow) *flow = to_display(ctx, openswmm::ucf::FLOW, ctx.links.flow[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// Engine gap BN-LINK-01b (added 2026-05-25) — symmetric getter for
// swmm_link_set_max_flow. Reads the `q_limit` SoA slot. 0.0 means no
// limit (mirrors the setter's contract documented in openswmm_links.h:223).
SWMM_ENGINE_API int swmm_link_get_max_flow(SWMM_Engine engine, int idx, double* flow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal cfs -> display FLOW
    if (flow) *flow = to_display(ctx, openswmm::ucf::FLOW, ctx.links.q_limit[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// Engine gap BN-LINK-02 (added 2026-05-25) — orifice TYPE (SIDE / BOTTOM)
// accessors. Stored in `links.param1` per the legacy convention documented
// in LinkData.hpp:379 and LinksHandler.cpp:138 — 0.0 = BOTTOM, 1.0 = SIDE.
// The integer ABI here uses 0 = SIDE, 1 = BOTTOM to match the legacy
// SWMM-GUI combo order (SWMM-GUI/Epaswmm5/objprops.txt:862 lists
// 'SIDE' first, 'BOTTOM' second). The engine-internal float storage
// stays as-is; the int↔float mapping lives in this accessor pair so
// callers always see the legacy enum ordering.
//
// Returns SWMM_ERR_BADPARAM if the link type isn't ORIFICE; tests pin the
// contract so callers don't silently mutate a conduit's `param1` slot
// (which means something else for conduits).
SWMM_ENGINE_API int swmm_link_set_orifice_type(SWMM_Engine engine, int idx, int type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::ORIFICE)
        return SWMM_ERR_BADPARAM;
    if (type != 0 && type != 1) return SWMM_ERR_BADPARAM;
    // GUI 0=SIDE → engine 1.0=SIDE ; GUI 1=BOTTOM → engine 0.0=BOTTOM.
    {
        const int orr = ctx.link_subtypes.orifice_row(idx);
        if (orr >= 0) ctx.link_subtypes.orifices.orifice_type[static_cast<std::size_t>(orr)] = (type == 0) ? 1.0 : 0.0;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_orifice_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::ORIFICE)
        return SWMM_ERR_BADPARAM;
    if (type) {
        // Engine 1.0=SIDE → GUI 0=SIDE; Engine 0.0=BOTTOM → GUI 1=BOTTOM.
        const int orr = ctx.link_subtypes.orifice_row(idx);
        const double p1 = (orr >= 0) ? ctx.link_subtypes.orifices.orifice_type[static_cast<std::size_t>(orr)] : 0.0;
        *type = (p1 >= 0.5) ? 0 : 1;
    }
    return SWMM_OK;
}

// Engine gap BN-LINK-03 (added 2026-05-25) — weir TYPE accessors. Five
// values matching the legacy WeirType enum order in
// `legacy/engine/enums.h:925`: TRANSVERSE=0, SIDEFLOW=1, VNOTCH=2,
// TRAPEZOIDAL=3, ROADWAY=4. Stored in `links.param1` per the legacy
// convention (LinksHandler.cpp:171-178). ROADWAY is accepted by the
// accessor even though the .inp parser doesn't yet recognise the
// "ROADWAY" keyword — the engine simulation code (legacy/engine/link.c)
// has the case branch, so an interactively-set ROADWAY weir does work.
// Filing the .inp parser-side ROADWAY tokenisation as a separate gap.
SWMM_ENGINE_API int swmm_link_set_weir_type(SWMM_Engine engine, int idx, int type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::WEIR)
        return SWMM_ERR_BADPARAM;
    if (type < 0 || type > 4) return SWMM_ERR_BADPARAM;
    {
        const int wr = ctx.link_subtypes.weir_row(idx);
        if (wr >= 0) ctx.link_subtypes.weirs.weir_type[static_cast<std::size_t>(wr)] = static_cast<double>(type);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_weir_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::WEIR)
        return SWMM_ERR_BADPARAM;
    if (type) {
        // Round to nearest int — weir_type is a double slot but only
        // discrete integer-valued weir-type codes are stored.
        const int wr = ctx.link_subtypes.weir_row(idx);
        const double p1 = (wr >= 0) ? ctx.link_subtypes.weirs.weir_type[static_cast<std::size_t>(wr)] : 0.0;
        const int raw = static_cast<int>(p1 + 0.5);
        *type = (raw < 0 || raw > 4) ? 0 : raw;
    }
    return SWMM_OK;
}

// Engine gap BN-LINK-04 (added 2026-05-25) — outlet RATING CURVE TYPE
// accessors. Four values: 0=FUNCTIONAL_HEAD, 1=FUNCTIONAL_DEPTH,
// 2=TABULAR_HEAD, 3=TABULAR_DEPTH. Encoding matches the existing
// LinksHandler.cpp:214-221 convention; stored in `links.param1` per the
// established legacy pattern.
//
// Functional coefficient (`links.cd`) and tabular curve index
// (`links.pump_curve`) reuse the existing scalar accessors
// (`swmm_link_set_discharge_coeff` / `swmm_link_set_pump_curve`); only
// the type code and the functional exponent (`links.param2`) need
// new outlet-typed accessor pairs to be unambiguous about which
// link kind they apply to.
SWMM_ENGINE_API int swmm_link_set_outlet_rating_type(SWMM_Engine engine, int idx, int type) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::OUTLET)
        return SWMM_ERR_BADPARAM;
    if (type < 0 || type > 3) return SWMM_ERR_BADPARAM;
    {
        const int olr = ctx.link_subtypes.outlet_row(idx);
        if (olr >= 0) ctx.link_subtypes.outlets.outlet_type[static_cast<std::size_t>(olr)] = static_cast<double>(type);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_outlet_rating_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::OUTLET)
        return SWMM_ERR_BADPARAM;
    if (type) {
        const int olr = ctx.link_subtypes.outlet_row(idx);
        const double p1 = (olr >= 0) ? ctx.link_subtypes.outlets.outlet_type[static_cast<std::size_t>(olr)] : 0.0;
        const int raw = static_cast<int>(p1 + 0.5);
        *type = (raw < 0 || raw > 3) ? 0 : raw;
    }
    return SWMM_OK;
}

// Outlet functional exponent (`links.param2`). Only valid for outlets;
// for the TABULAR/* rating types the engine ignores the stored value.
SWMM_ENGINE_API int swmm_link_set_outlet_expon(SWMM_Engine engine, int idx, double expon) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::OUTLET)
        return SWMM_ERR_BADPARAM;
    {
        const int olr = ctx.link_subtypes.outlet_row(idx);
        if (olr >= 0) ctx.link_subtypes.outlets.expon[static_cast<std::size_t>(olr)] = expon;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_outlet_expon(SWMM_Engine engine, int idx, double* expon) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::OUTLET)
        return SWMM_ERR_BADPARAM;
    if (expon) {
        const int olr = ctx.link_subtypes.outlet_row(idx);
        *expon = (olr >= 0) ? ctx.link_subtypes.outlets.expon[static_cast<std::size_t>(olr)] : 0.0;
    }
    return SWMM_OK;
}

// Engine gap BN-LINK-05 (added 2026-05-25) — pump startup / shutoff
// depth accessors. Engine state already exists at LinkData.hpp:310-313
// (`pump_startup`, `pump_shutoff`); these accessors expose it through
// the public ABI. Non-pump links rejected.
SWMM_ENGINE_API int swmm_link_set_pump_startup_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::PUMP)
        return SWMM_ERR_BADPARAM;
    // units: display LENGTH -> internal ft
    {
        const double d = to_internal(ctx, openswmm::ucf::LENGTH, depth);
        const int pr = ctx.link_subtypes.pump_row(idx);
        if (pr >= 0) ctx.link_subtypes.pumps.startup[static_cast<std::size_t>(pr)] = d;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_pump_startup_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::PUMP)
        return SWMM_ERR_BADPARAM;
    // units: internal ft -> display LENGTH
    const int pr = ctx.link_subtypes.pump_row(idx);
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH,
        (pr >= 0) ? ctx.link_subtypes.pumps.startup[static_cast<std::size_t>(pr)] : 0.0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_pump_shutoff_depth(SWMM_Engine engine, int idx, double depth) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::PUMP)
        return SWMM_ERR_BADPARAM;
    // units: display LENGTH -> internal ft
    {
        const double d = to_internal(ctx, openswmm::ucf::LENGTH, depth);
        const int pr = ctx.link_subtypes.pump_row(idx);
        if (pr >= 0) ctx.link_subtypes.pumps.shutoff[static_cast<std::size_t>(pr)] = d;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_pump_shutoff_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::PUMP)
        return SWMM_ERR_BADPARAM;
    // units: internal ft -> display LENGTH
    const int pr = ctx.link_subtypes.pump_row(idx);
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH,
        (pr >= 0) ? ctx.link_subtypes.pumps.shutoff[static_cast<std::size_t>(pr)] : 0.0);
    return SWMM_OK;
}

// Engine gap BN-LINK-06 (added 2026-05-25) — orifice open/close rate
// accessor pair. Engine state exists at LinkData.hpp:390 (`orate`).
// Stores fraction per second (0 = instantaneous) per the legacy
// convention; legacy SWMM-GUI uses the "Time to Open/Close" label in
// hours, but the engine stores the rate. The GUI's
// `setOrificeOrateHours` helper converts the legacy hours-based value
// to the engine's rate-per-second on write.
SWMM_ENGINE_API int swmm_link_set_orifice_open_close_rate(SWMM_Engine engine, int idx, double rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::ORIFICE)
        return SWMM_ERR_BADPARAM;
    {
        const int orr = ctx.link_subtypes.orifice_row(idx);
        if (orr >= 0) ctx.link_subtypes.orifices.orate[static_cast<std::size_t>(orr)] = rate;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_orifice_open_close_rate(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto uidx = static_cast<std::size_t>(idx);
    if (ctx.links.type[uidx] != openswmm::LinkType::ORIFICE)
        return SWMM_ERR_BADPARAM;
    if (rate) {
        const int orr = ctx.link_subtypes.orifice_row(idx);
        *rate = (orr >= 0) ? ctx.link_subtypes.orifices.orate[static_cast<std::size_t>(orr)] : 0.0;
    }
    return SWMM_OK;
}

// ============================================================================
// Cross-section
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_xsect(SWMM_Engine engine, int idx,
                                          int shape, double geom1, double geom2,
                                          double geom3, double geom4) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);

    auto xs = static_cast<openswmm::XsectShape>(shape);
    ctx.links.xsect_shape[uidx] = xs;

    // Retain the raw [XSECTIONS] Geom1–Geom4 (display units) for lossless
    // serialization — the derived fields set below (y_full/w_max/a_full/...)
    // cannot reproduce the input bottom width and side slopes for trapezoids
    // and similar multi-parameter shapes.  See LinkData::xsect_geom1.
    ctx.links.xsect_geom1[uidx] = geom1;
    ctx.links.xsect_geom2[uidx] = geom2;
    ctx.links.xsect_geom3[uidx] = geom3;
    ctx.links.xsect_geom4[uidx] = geom4;

    // STREET cross-sections reference a named [STREETS] entry by index in
    // geom1 (mirroring IRREGULAR/transects). geom1 is an index, not a length,
    // so resolve it here — before the length-unit conversions below. Record
    // the street name for round-trip [XSECTIONS] export and build + attach the
    // street transect table exactly like PostParseResolver::resolve so the
    // link's geometry is valid for same-session hydraulics.
    if (xs == openswmm::XsectShape::STREET_XSECT) {
        const int si = static_cast<int>(std::llround(geom1));
        if (si < 0 || si >= ctx.streets.count()) return SWMM_ERR_BADPARAM;
        const auto su = static_cast<std::size_t>(si);
        ctx.links.pump_curve_name[uidx] = ctx.streets.names[su];

        const int us = openswmm::ucf::getUnitSystem(
            static_cast<int>(ctx.options.flow_units));
        const double inv_len =
            openswmm::ucf::Ucf_inv[openswmm::ucf::LENGTH][static_cast<std::size_t>(us)];
        openswmm::street::StreetParams sp;
        sp.width             = ctx.streets.t_crown[su]       * inv_len;
        sp.curb_height       = ctx.streets.h_curb[su]        * inv_len;
        sp.slope             = ctx.streets.sx[su]            / 100.0;   // % → fraction
        sp.roughness         = ctx.streets.n_road[su];
        sp.gutter_depression = ctx.streets.gutter_depres[su] * inv_len;
        sp.gutter_width      = ctx.streets.gutter_width[su]  * inv_len;
        sp.sides             = ctx.streets.sides[su];
        sp.back_width        = ctx.streets.back_width[su]    * inv_len;
        sp.back_slope        = ctx.streets.back_slope[su]    / 100.0;   // % → fraction
        sp.back_roughness    = ctx.streets.back_n[su];

        openswmm::transect::TransectData td;
        td.name = ctx.streets.names[su];
        openswmm::street::buildTransect(sp, td);

        const int idx_tbl = static_cast<int>(ctx.transect_tables.size());
        ctx.transect_tables.push_back(std::move(td));
        const auto& built = ctx.transect_tables[static_cast<std::size_t>(idx_tbl)];
        ctx.links.xsect_curve[uidx]  = idx_tbl;
        ctx.links.xsect_y_full[uidx] = built.y_full;
        ctx.links.xsect_a_full[uidx] = built.a_full;
        ctx.links.xsect_r_full[uidx] = built.r_full;
        ctx.links.xsect_w_max[uidx]  = built.w_max;
        return SWMM_OK;
    }

    // IRREGULAR cross-sections reference a [TRANSECTS] entry by index in
    // geom1, mirroring STREET. geom1 is an index, not a length; without this
    // branch it fell into the default arm below, which stored the index as
    // y_full (feet) and left the transect reference dangling — the GUI's
    // transect picker corrupted the link it assigned to.
    if (xs == openswmm::XsectShape::IRREGULAR) {
        const int si = static_cast<int>(std::llround(geom1));
        if (si < 0 || si >= ctx.transects.count()) return SWMM_ERR_BADPARAM;
        const auto su = static_cast<std::size_t>(si);
        ctx.links.pump_curve_name[uidx] = ctx.transects.names[su];
        // geom1 for IRREGULAR is the transect reference, whose identity is the
        // NAME in pump_curve_name — never keep the numeric index in retained
        // storage where a fallback path could read it as a dimension. geom2-4
        // stay as plain retained storage (already set above): they are not
        // part of the section's geometry, but the GUI's inline editors expect
        // sibling values to survive a set/get cycle.
        ctx.links.xsect_geom1[uidx] = 0.0;

        // After a deck open PostParseResolver builds ctx.transect_tables
        // store-aligned, so row si usually already has its table — reuse it,
        // matching the resolver's xsect_curve = store index. A transect added
        // through the API after open has no table yet: build one from the
        // store with the shared helper (bit-identical to the resolver) and
        // push it. Push-back only — cached table pointers must stay stable
        // (test_transect_table_stability).
        int ti;
        if (su < ctx.transect_tables.size() &&
            ctx.transect_tables[su].name == ctx.transects.names[su]) {
            ti = si;
        } else {
            const int t_us = openswmm::ucf::getUnitSystem(
                static_cast<int>(ctx.options.flow_units));
            const double t_ucf =
                openswmm::ucf::Ucf[openswmm::ucf::LENGTH][static_cast<std::size_t>(t_us)];
            openswmm::transect::TransectData td;
            if (!openswmm::transect::buildFromStore(ctx.transects, si, t_ucf, td))
                return SWMM_ERR_BADPARAM;   // no valid channel Manning's n
            ti = static_cast<int>(ctx.transect_tables.size());
            ctx.transect_tables.push_back(std::move(td));
        }
        const auto& built = ctx.transect_tables[static_cast<std::size_t>(ti)];
        ctx.links.xsect_curve[uidx]  = ti;
        ctx.links.xsect_y_full[uidx] = built.y_full;
        ctx.links.xsect_a_full[uidx] = built.a_full;
        ctx.links.xsect_r_full[uidx] = built.r_full;
        ctx.links.xsect_w_max[uidx]  = built.w_max;
        return SWMM_OK;
    }

    // units: convert incoming DISPLAY geom values to INTERNAL (ft) following the
    // same shape-dependent field roles as PostParseResolver::convert_inputs_to_internal.
    //   geom1 (y_full / full depth or diameter): LENGTH for ALL shapes.
    //   geom2 (w_max / width):                   LENGTH except FORCE_MAIN
    //                                            (geom2 is a roughness coeff there — raw).
    //   geom3 (y_bot):                           LENGTH only for RECT_TRIANG,
    //                                            RECT_ROUND, MODBASKETHANDLE; for other
    //                                            shapes it is a side-slope/exponent — raw.
    //   geom4: dimensionless side-slope in every supported shape here — raw.
    geom1 = to_internal(ctx, openswmm::ucf::LENGTH, geom1);
    if (xs != openswmm::XsectShape::FORCE_MAIN)
        geom2 = to_internal(ctx, openswmm::ucf::LENGTH, geom2);
    if (xs == openswmm::XsectShape::RECT_TRIANG ||
        xs == openswmm::XsectShape::RECT_ROUND ||
        xs == openswmm::XsectShape::MODBASKETHANDLE)
        geom3 = to_internal(ctx, openswmm::ucf::LENGTH, geom3);
    // geom4 left raw (dimensionless side-slope for the shapes handled below).

    switch (xs) {
        case openswmm::XsectShape::CIRCULAR: {
            double d = geom1;
            ctx.links.xsect_y_full[uidx] = d;
            ctx.links.xsect_a_full[uidx] = M_PI * d * d / 4.0;
            ctx.links.xsect_w_max[uidx]  = d;
            break;
        }
        case openswmm::XsectShape::RECT_CLOSED:
        case openswmm::XsectShape::RECT_OPEN: {
            double h = geom1, w = geom2;
            ctx.links.xsect_y_full[uidx] = h;
            ctx.links.xsect_a_full[uidx] = h * w;
            ctx.links.xsect_w_max[uidx]  = w;
            break;
        }
        case openswmm::XsectShape::TRAPEZOIDAL: {
            double h = geom1, bw = geom2, ss1 = geom3, ss2 = geom4;
            ctx.links.xsect_y_full[uidx] = h;
            double tw = bw + (ss1 + ss2) * h;
            ctx.links.xsect_a_full[uidx] = (bw + tw) * h / 2.0;
            ctx.links.xsect_w_max[uidx]  = tw;
            break;
        }
        case openswmm::XsectShape::TRIANGULAR: {
            double h = geom1, tw = geom2;
            ctx.links.xsect_y_full[uidx] = h;
            ctx.links.xsect_a_full[uidx] = tw * h / 2.0;
            ctx.links.xsect_w_max[uidx]  = tw;
            break;
        }
        default: {
            // Generic fallback: store geom1 as y_full, compute area if possible
            ctx.links.xsect_y_full[uidx] = geom1;
            ctx.links.xsect_a_full[uidx] = geom1 * geom2;
            ctx.links.xsect_w_max[uidx]  = geom2;
            break;
        }
    }

    (void)geom3; (void)geom4;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_xsect(SWMM_Engine engine, int idx,
                                          int* shape, double* geom1, double* geom2,
                                          double* geom3, double* geom4) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);
    const auto xs = ctx.links.xsect_shape[uidx];
    if (shape) *shape = static_cast<int>(xs);

    // STREET links report geom1 = street index (mirroring IRREGULAR/transect).
    // The street identity is the name retained in pump_curve_name; xsect_curve
    // points at the built geometry table, not the [STREETS] index, so resolve
    // the name back to a street index here.
    if (xs == openswmm::XsectShape::STREET_XSECT) {
        int si = -1;
        const auto& nm = ctx.links.pump_curve_name[uidx];
        if (!nm.empty()) {
            for (int s = 0; s < ctx.streets.count(); ++s) {
                if (openswmm::ieq(ctx.streets.names[static_cast<std::size_t>(s)],
                                  nm)) {
                    si = s;
                    break;
                }
            }
        }
        if (geom1) *geom1 = static_cast<double>(si);
        if (geom2) *geom2 = 0.0;
        if (geom3) *geom3 = 0.0;
        if (geom4) *geom4 = 0.0;
        return SWMM_OK;
    }

    // IRREGULAR links report geom1 = transect index, mirroring STREET. The
    // transect identity is the name retained in pump_curve_name (xsect_curve
    // can point at a session-pushed table, not the store row). The old default
    // path returned y_full here — a depth where a reference belongs — so any
    // get→set cycle rewrote the reference as a number.
    if (xs == openswmm::XsectShape::IRREGULAR) {
        int si = -1;
        const auto& nm = ctx.links.pump_curve_name[uidx];
        if (!nm.empty()) {
            for (int t = 0; t < ctx.transects.count(); ++t) {
                if (openswmm::ieq(ctx.transects.names[static_cast<std::size_t>(t)],
                                  nm)) {
                    si = t;
                    break;
                }
            }
        } else if (ctx.links.xsect_curve[uidx] >= 0 &&
                   ctx.links.xsect_curve[uidx] < ctx.transects.count()) {
            si = ctx.links.xsect_curve[uidx];
        }
        if (geom1) *geom1 = static_cast<double>(si);
        // geom2-4: plain retained storage (0 for parser-loaded links — the
        // [XSECTIONS] handler retains nothing for IRREGULAR).
        if (geom2) *geom2 = ctx.links.xsect_geom2[uidx];
        if (geom3) *geom3 = ctx.links.xsect_geom3[uidx];
        if (geom4) *geom4 = ctx.links.xsect_geom4[uidx];
        return SWMM_OK;
    }

    // Prefer the retained raw Geom1–Geom4 (set by swmm_link_set_xsect / the
    // [XSECTIONS] parser) so the call mirrors what was supplied — these survive
    // the derived-geometry overwrite that loses trapezoid bottom width / side
    // slopes.  xsect_geom1 == 0 means "not populated" (object built by a path
    // that doesn't set these); fall back to the legacy derived reconstruction.
    if (ctx.links.xsect_geom1[uidx] != 0.0) {
        if (geom1) *geom1 = ctx.links.xsect_geom1[uidx];
        if (geom2) *geom2 = ctx.links.xsect_geom2[uidx];
        if (geom3) *geom3 = ctx.links.xsect_geom3[uidx];
        if (geom4) *geom4 = ctx.links.xsect_geom4[uidx];
        return SWMM_OK;
    }

    // units: INTERNAL -> DISPLAY following the same shape-dependent field roles
    // as PostParseResolver::convert_inputs_to_internal (mirrored, inverse direction).
    // geom1 = y_full (full depth/diameter): LENGTH for ALL shapes.
    if (geom1) *geom1 = to_display(ctx, openswmm::ucf::LENGTH, ctx.links.xsect_y_full[uidx]);
    // geom2 = w_max (width): LENGTH except FORCE_MAIN (roughness coeff there — raw).
    if (geom2) {
        *geom2 = (xs == openswmm::XsectShape::FORCE_MAIN)
            ? ctx.links.xsect_w_max[uidx]
            : to_display(ctx, openswmm::ucf::LENGTH, ctx.links.xsect_w_max[uidx]);
    }
    // geom3 = a_full (full flow area): LENGTH-SQUARED → apply LENGTH factor twice.
    if (geom3) {
        const double f = openswmm::ucf::UCF(openswmm::ucf::LENGTH, ctx.options);
        *geom3 = ctx.links.xsect_a_full[uidx] * f * f;
    }
    if (geom4) *geom4 = 0.0;
    return SWMM_OK;
}

// ============================================================================
// Geometry getters
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_type(SWMM_Engine engine, int idx, int* type) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (type) *type = static_cast<int>(ctx.links.type[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_length(SWMM_Engine engine, int idx, double* length) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft -> display LENGTH
    const int cr = ctx.link_subtypes.conduit_row(idx);
    const double L = (cr >= 0) ? ctx.link_subtypes.conduits.length[static_cast<std::size_t>(cr)] : 0.0;
    if (length) *length = to_display(ctx, openswmm::ucf::LENGTH, L);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_roughness(SWMM_Engine engine, int idx, double* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const int cr = ctx.link_subtypes.conduit_row(idx);
    if (n) *n = (cr >= 0) ? ctx.link_subtypes.conduits.roughness[static_cast<std::size_t>(cr)] : 0.01;
    return SWMM_OK;
}

// ============================================================================
// Hydraulic state
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_flow(SWMM_Engine engine, int idx, double* flow) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal cfs -> display FLOW
    if (flow) *flow = to_display(ctx, openswmm::ucf::FLOW, ctx.links.flow[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_flow(SWMM_Engine engine, int idx, double flow) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display FLOW -> internal cfs
    ctx.links.flow[static_cast<std::size_t>(idx)] = to_internal(ctx, openswmm::ucf::FLOW, flow);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_depth(SWMM_Engine engine, int idx, double* depth) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft -> display LENGTH
    if (depth) *depth = to_display(ctx, openswmm::ucf::LENGTH, ctx.links.depth[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_velocity(SWMM_Engine engine, int idx, double* velocity) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (velocity) {
        auto uidx = static_cast<std::size_t>(idx);
        double q = ctx.links.flow[uidx];
        double d = ctx.links.depth[uidx];
        double y_full = ctx.links.xsect_y_full[uidx];
        double a_full = ctx.links.xsect_a_full[uidx];
        // Approximate flow area from depth/y_full ratio times full area
        double area = (y_full > 0.0 && a_full > 0.0 && d > 0.0)
                      ? a_full * (d / y_full)
                      : 0.0;
        // units: internal ft/s -> display LENGTH (velocity carries LENGTH UCF)
        const double v_internal = (area > 1.0e-12) ? q / area : 0.0;
        *velocity = to_display(ctx, openswmm::ucf::LENGTH, v_internal);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_capacity(SWMM_Engine engine, int idx, double* capacity) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (capacity) {
        auto uidx = static_cast<std::size_t>(idx);
        double q = ctx.links.flow[uidx];
        const int cr = ctx.link_subtypes.conduit_row(idx);
        double qf = (cr >= 0) ? ctx.link_subtypes.conduits.q_full[static_cast<std::size_t>(cr)] : 0.0;
        *capacity = (qf > 1.0e-12) ? q / qf : 0.0;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft^3 -> display VOLUME
    if (volume) *volume = to_display(ctx, openswmm::ucf::VOLUME, ctx.links.volume[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_slot_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft^3 -> display VOLUME. FV routing only; 0.0 under DW.
    if (volume) *volume = to_display(ctx, openswmm::ucf::VOLUME, ctx.links.slot_volume[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// ============================================================================
// Runtime forcing
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_control_setting(SWMM_Engine engine, int idx, double setting) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.setting[static_cast<std::size_t>(idx)] = setting;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_control_setting(SWMM_Engine engine, int idx, double* setting) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (setting) *setting = ctx.links.setting[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_target_setting(SWMM_Engine engine, int idx, double setting) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.target_setting[static_cast<std::size_t>(idx)] = setting;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_target_setting(SWMM_Engine engine, int idx, double* setting) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (setting) *setting = ctx.links.target_setting[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_closed(SWMM_Engine engine, int idx, int closed) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.is_closed[static_cast<std::size_t>(idx)] = (closed != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_closed(SWMM_Engine engine, int idx, int* closed) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (closed) *closed = ctx.links.is_closed[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

// ============================================================================
// Water quality (Phase 8)
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_quality(SWMM_Engine engine, int link_idx,
                                           int pollutant_idx, double* conc) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(link_idx >= 0 && link_idx < ctx.n_links());
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    if (conc) *conc = ctx.links.conc[
        static_cast<std::size_t>(link_idx) * static_cast<std::size_t>(np) +
        static_cast<std::size_t>(pollutant_idx)];
    return SWMM_OK;
}

// ============================================================================
// Bulk access
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_flows_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    // units: internal cfs -> display FLOW (per element)
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::FLOW, ctx.links.flow[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_depths_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    // units: internal ft -> display LENGTH (per element)
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::LENGTH, ctx.links.depth[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_flows_bulk(SWMM_Engine engine, const double* buf, int count) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_RUNNING(ctx);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const int n = std::min(count, ctx.n_links());
    // units: display FLOW -> internal cfs (per element)
    for (int i = 0; i < n; ++i)
        ctx.links.flow[static_cast<std::size_t>(i)] =
            to_internal(ctx, openswmm::ucf::FLOW, buf[i]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_quality_bulk(SWMM_Engine engine, int pollutant_idx,
                                                double* buf, int count) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    int np = ctx.n_pollutants();
    CHECK_INDEX(pollutant_idx >= 0 && pollutant_idx < np);
    const int n = std::min(count, ctx.n_links());
    for (int i = 0; i < n; ++i) {
        buf[i] = ctx.links.conc[
            static_cast<std::size_t>(i) * static_cast<std::size_t>(np) +
            static_cast<std::size_t>(pollutant_idx)];
    }
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Phase 3 bulk getters — Links.
//
// Three of these (volumes, control_settings, target_settings) are simple SoA
// memcpys. Three (velocities, capacities, hyd_powers) recompute a derived
// quantity per link — there is no SoA column for them — but bulk-mode still
// saves the C ABI crossing cost and any Python-level iteration. The
// arithmetic is taken verbatim from the matching scalar accessors above so
// the bulk vs scalar parity tests stay bit-equivalent.
//
// Stride-packed IDs follow the same format as swmm_node_get_ids_bulk.
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_link_get_velocities_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double q = ctx.links.flow[ui];
        const double d = ctx.links.depth[ui];
        const double y_full = ctx.links.xsect_y_full[ui];
        const double a_full = ctx.links.xsect_a_full[ui];
        const double area = (y_full > 0.0 && a_full > 0.0 && d > 0.0)
                            ? a_full * (d / y_full) : 0.0;
        // units: internal ft/s -> display LENGTH (velocity carries LENGTH UCF)
        const double v_internal = (area > 1.0e-12) ? q / area : 0.0;
        buf[i] = to_display(ctx, openswmm::ucf::LENGTH, v_internal);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_capacities_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double q = ctx.links.flow[ui];
        const int cr = ctx.link_subtypes.conduit_row(i);
        const double qf = (cr >= 0) ? ctx.link_subtypes.conduits.q_full[static_cast<std::size_t>(cr)] : 0.0;
        buf[i] = (qf > 1.0e-12) ? q / qf : 0.0;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_volumes_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    // units: internal ft^3 -> display VOLUME (per element)
    for (int i = 0; i < n; ++i)
        buf[i] = to_display(ctx, openswmm::ucf::VOLUME, ctx.links.volume[static_cast<std::size_t>(i)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_control_settings_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.setting.begin(), ctx.links.setting.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_target_settings_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    std::copy(ctx.links.target_setting.begin(),
              ctx.links.target_setting.begin() + n, buf);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_hyd_powers_bulk(SWMM_Engine engine, double* buf, int count) {
    CHECK_HANDLE(engine);
    if (!buf || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    constexpr double GAMMA = 62.4;
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const int n1 = ctx.links.node1[ui];
        const int n2 = ctx.links.node2[ui];
        const double h1 = (n1 >= 0)
            ? ctx.nodes.head[static_cast<std::size_t>(n1)] : 0.0;
        const double h2 = (n2 >= 0)
            ? ctx.nodes.head[static_cast<std::size_t>(n2)] : 0.0;
        buf[i] = GAMMA * std::fabs(ctx.links.flow[ui]) * std::fabs(h1 - h2);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_ids_bulk(SWMM_Engine engine,
                                            char* buf,
                                            int stride,
                                            int count) {
    CHECK_HANDLE(engine);
    if (!buf || stride < 2 || count <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    const int n = std::min(count, ctx.n_links());
    const std::size_t s = static_cast<std::size_t>(stride);

    std::fill_n(buf, s * static_cast<std::size_t>(n), '\0');
    for (int i = 0; i < n; ++i) {
        const std::string& name = ctx.link_names.name_of(i);
        const std::size_t copy_n = std::min(name.size(), s - 1);
        std::memcpy(buf + static_cast<std::size_t>(i) * s,
                    name.data(), copy_n);
    }
    return SWMM_OK;
}

// ============================================================================
// Pump Link API
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_pump_curve(SWMM_Engine engine, int idx, int curve_idx) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int pr = ctx.link_subtypes.pump_row(idx);
        if (pr >= 0) {
            ctx.link_subtypes.pumps.curve[static_cast<std::size_t>(pr)] = curve_idx;
        } else {
            const int olr = ctx.link_subtypes.outlet_row(idx);
            if (olr >= 0) ctx.link_subtypes.outlets.curve[static_cast<std::size_t>(olr)] = curve_idx;
        }
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_pump_curve(SWMM_Engine engine, int idx, int* curve_idx) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (curve_idx) {
        const int pr = ctx.link_subtypes.pump_row(idx);
        if (pr >= 0) {
            *curve_idx = ctx.link_subtypes.pumps.curve[static_cast<std::size_t>(pr)];
        } else {
            const int olr = ctx.link_subtypes.outlet_row(idx);
            *curve_idx = (olr >= 0) ? ctx.link_subtypes.outlets.curve[static_cast<std::size_t>(olr)] : -1;
        }
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_pump_init_state(SWMM_Engine engine, int idx, int on) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INITIAL_COND(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int pr = ctx.link_subtypes.pump_row(idx);
        if (pr >= 0) ctx.link_subtypes.pumps.init_state[static_cast<std::size_t>(pr)] = (on != 0) ? uint8_t{1} : uint8_t{0};
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_pump_init_state(SWMM_Engine engine, int idx, int* on) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (on) {
        const int pr = ctx.link_subtypes.pump_row(idx);
        *on = (pr >= 0 && ctx.link_subtypes.pumps.init_state[static_cast<std::size_t>(pr)]) ? 1 : 0;
    }
    return SWMM_OK;
}

// ============================================================================
// Weir Link API
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_crest_height(SWMM_Engine engine, int idx, double h) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display LENGTH -> internal ft
    {
        const double ch = to_internal(ctx, openswmm::ucf::LENGTH, h);
        const int wr = ctx.link_subtypes.weir_row(idx);
        if (wr >= 0) {
            ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)] = ch;
        } else {
            const int olr = ctx.link_subtypes.outlet_row(idx);
            if (olr >= 0) ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)] = ch;
        }
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_crest_height(SWMM_Engine engine, int idx, double* h) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft -> display LENGTH
    if (h) {
        const int wr = ctx.link_subtypes.weir_row(idx);
        double ch;
        if (wr >= 0) {
            ch = ctx.link_subtypes.weirs.crest_height[static_cast<std::size_t>(wr)];
        } else {
            const int olr = ctx.link_subtypes.outlet_row(idx);
            ch = (olr >= 0) ? ctx.link_subtypes.outlets.crest_height[static_cast<std::size_t>(olr)] : 0.0;
        }
        *h = to_display(ctx, openswmm::ucf::LENGTH, ch);
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_discharge_coeff(SWMM_Engine engine, int idx, double cd) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int orr = ctx.link_subtypes.orifice_row(idx);
        if (orr >= 0) {
            ctx.link_subtypes.orifices.cd[static_cast<std::size_t>(orr)] = cd;
        } else {
            const int wr = ctx.link_subtypes.weir_row(idx);
            if (wr >= 0) {
                ctx.link_subtypes.weirs.cd[static_cast<std::size_t>(wr)] = cd;
            } else {
                const int olr = ctx.link_subtypes.outlet_row(idx);
                if (olr >= 0) ctx.link_subtypes.outlets.coeff[static_cast<std::size_t>(olr)] = cd;
            }
        }
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_discharge_coeff(SWMM_Engine engine, int idx, double* cd) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (cd) {
        const int orr = ctx.link_subtypes.orifice_row(idx);
        if (orr >= 0) {
            *cd = ctx.link_subtypes.orifices.cd[static_cast<std::size_t>(orr)];
        } else {
            const int wr = ctx.link_subtypes.weir_row(idx);
            if (wr >= 0) {
                *cd = ctx.link_subtypes.weirs.cd[static_cast<std::size_t>(wr)];
            } else {
                const int olr = ctx.link_subtypes.outlet_row(idx);
                *cd = (olr >= 0) ? ctx.link_subtypes.outlets.coeff[static_cast<std::size_t>(olr)] : 0.0;
            }
        }
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_end_contractions(SWMM_Engine engine, int idx, double n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int wr = ctx.link_subtypes.weir_row(idx);
        if (wr >= 0) ctx.link_subtypes.weirs.end_contractions[static_cast<std::size_t>(wr)] = n;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_end_contractions(SWMM_Engine engine, int idx, double* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (n) {
        const int wr = ctx.link_subtypes.weir_row(idx);
        *n = (wr >= 0) ? ctx.link_subtypes.weirs.end_contractions[static_cast<std::size_t>(wr)] : 0.0;
    }
    return SWMM_OK;
}

// ============================================================================
// Conduit Loss Coefficients
// ============================================================================

SWMM_ENGINE_API int swmm_link_set_loss_coeff(SWMM_Engine engine, int idx, double inlet, double outlet, double avg) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto uidx = static_cast<std::size_t>(idx);
    {
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (cr >= 0) {
            const auto ucr = static_cast<std::size_t>(cr);
            ctx.link_subtypes.conduits.loss_inlet[ucr]  = inlet;
            ctx.link_subtypes.conduits.loss_outlet[ucr] = outlet;
            ctx.link_subtypes.conduits.loss_avg[ucr]    = avg;
        }
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_loss_coeff(SWMM_Engine engine, int idx, double* inlet, double* outlet, double* avg) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const int cr = ctx.link_subtypes.conduit_row(idx);
    const auto& CD = ctx.link_subtypes.conduits;
    if (inlet)  *inlet  = (cr >= 0) ? CD.loss_inlet[static_cast<std::size_t>(cr)]  : 0.0;
    if (outlet) *outlet = (cr >= 0) ? CD.loss_outlet[static_cast<std::size_t>(cr)] : 0.0;
    if (avg)    *avg    = (cr >= 0) ? CD.loss_avg[static_cast<std::size_t>(cr)]    : 0.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_flap_gate(SWMM_Engine engine, int idx, int has_gate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    ctx.links.has_flap_gate[static_cast<std::size_t>(idx)] = (has_gate != 0);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_flap_gate(SWMM_Engine engine, int idx, int* has_gate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (has_gate) *has_gate = ctx.links.has_flap_gate[static_cast<std::size_t>(idx)] ? 1 : 0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_seep_rate(SWMM_Engine engine, int idx, double rate) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: display RAINFALL -> internal ft/sec
    {
        const double sr = to_internal(ctx, openswmm::ucf::RAINFALL, rate);
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (cr >= 0) ctx.link_subtypes.conduits.seep_rate[static_cast<std::size_t>(cr)] = sr;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_seep_rate(SWMM_Engine engine, int idx, double* rate) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft/sec -> display RAINFALL
    const int cr = ctx.link_subtypes.conduit_row(idx);
    const double sr = (cr >= 0) ? ctx.link_subtypes.conduits.seep_rate[static_cast<std::size_t>(cr)] : 0.0;
    if (rate) *rate = to_display(ctx, openswmm::ucf::RAINFALL, sr);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_culvert_code(SWMM_Engine engine, int idx, int code) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (cr >= 0) ctx.link_subtypes.conduits.culvert_code[static_cast<std::size_t>(cr)] = code;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_culvert_code(SWMM_Engine engine, int idx, int* code) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (code) {
        const int cr = ctx.link_subtypes.conduit_row(idx);
        *code = (cr >= 0) ? ctx.link_subtypes.conduits.culvert_code[static_cast<std::size_t>(cr)] : 0;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_barrels(SWMM_Engine engine, int idx, int n) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_GEOMETRY(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    {
        const int cr = ctx.link_subtypes.conduit_row(idx);
        if (cr >= 0) ctx.link_subtypes.conduits.barrels[static_cast<std::size_t>(cr)] = n;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_barrels(SWMM_Engine engine, int idx, int* n) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (n) {
        const int cr = ctx.link_subtypes.conduit_row(idx);
        *n = (cr >= 0) ? ctx.link_subtypes.conduits.barrels[static_cast<std::size_t>(cr)] : 1;
    }
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_slope(SWMM_Engine engine, int idx, double* slope) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const int cr = ctx.link_subtypes.conduit_row(idx);
    if (slope) *slope = (cr >= 0) ? ctx.link_subtypes.conduits.slope[static_cast<std::size_t>(cr)] : 0.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_offset_up(SWMM_Engine engine, int idx, double* offset) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft -> display LENGTH
    if (offset) *offset = to_display(ctx, openswmm::ucf::LENGTH, ctx.links.offset1[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_offset_dn(SWMM_Engine engine, int idx, double* offset) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft -> display LENGTH
    if (offset) *offset = to_display(ctx, openswmm::ucf::LENGTH, ctx.links.offset2[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// ============================================================================
// Link Statistics
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_stat_max_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal cfs -> display FLOW
    if (val) *val = to_display(ctx, openswmm::ucf::FLOW, ctx.links.stat_max_flow[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_max_velocity(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft/s -> display LENGTH (velocity carries LENGTH UCF)
    if (val) *val = to_display(ctx, openswmm::ucf::LENGTH, ctx.links.stat_max_veloc[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_max_filling(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_max_filling[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_peak_slot_share(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // dimensionless 0..1; FV routing only, 0 under DW
    if (val) *val = ctx.links.stat_peak_slot_share[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_slot_share(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // run-level time-integrated share: (∫slot dt)/(∫stored dt), 0..1
    const auto u = static_cast<std::size_t>(idx);
    const double denom = ctx.links.stat_vol_dt[u];
    if (val) *val = (denom > 0.0) ? ctx.links.stat_slot_vol_dt[u] / denom : 0.0;
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_vol_flow(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft^3 -> display VOLUME
    if (val) *val = to_display(ctx, openswmm::ucf::VOLUME, ctx.links.stat_vol_flow[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_surcharge_time(SWMM_Engine engine, int idx, double* val) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (val) *val = ctx.links.stat_time_surcharged[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

// ============================================================================
// Pump utilization statistics
// ============================================================================

SWMM_ENGINE_API int swmm_link_get_stat_pump_cycles(SWMM_Engine engine, int idx, int* cycles) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (cycles) *cycles = ctx.links.stat_pump_cycles[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_pump_on_time(SWMM_Engine engine, int idx, double* seconds) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    if (seconds) *seconds = ctx.links.stat_pump_on_time[static_cast<std::size_t>(idx)];
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_get_stat_pump_volume(SWMM_Engine engine, int idx, double* volume) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    // units: internal ft^3 -> display VOLUME
    if (volume) *volume = to_display(ctx, openswmm::ucf::VOLUME, ctx.links.stat_pump_volume[static_cast<std::size_t>(idx)]);
    return SWMM_OK;
}

// ----------------------------------------------------------------------------
// Bulk pump statistics — single pass over all links.
//
// Non-pump links receive sentinel values (cycles = -1, on_time = 0.0,
// volume = 0.0) so the caller can distinguish them. Any of cycles / on_time
// / volume may be NULL if the caller does not need that output.
//
// Design note: we deliberately do NOT early-exit the iteration when all
// outputs are NULL — that case is reported as SWMM_ERR_BADPARAM at entry,
// rather than silently being a no-op (the caller almost certainly made a
// mistake if they call with all three null).
// ----------------------------------------------------------------------------

SWMM_ENGINE_API int swmm_link_get_pump_stats_bulk(SWMM_Engine engine,
                                                   int* cycles,
                                                   double* on_time,
                                                   double* volume,
                                                   int count) {
    CHECK_HANDLE(engine);
    if (count <= 0) return SWMM_ERR_BADPARAM;
    if (!cycles && !on_time && !volume) return SWMM_ERR_BADPARAM;

    const auto& ctx = to_engine(engine)->context();
    const int n_links = ctx.n_links();
    const int n = std::min(count, n_links);

    // Defensive: the per-link statistics vectors are sized at engine init.
    // If for some reason they have not been sized (caller invoked too early),
    // fall back to sentinel for the entire range rather than dereferencing.
    const bool stats_sized =
        static_cast<int>(ctx.links.stat_pump_cycles.size())  >= n_links &&
        static_cast<int>(ctx.links.stat_pump_on_time.size()) >= n_links &&
        static_cast<int>(ctx.links.stat_pump_volume.size())  >= n_links;

    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const bool is_pump =
            stats_sized && ctx.links.type[ui] == openswmm::LinkType::PUMP;

        if (cycles)  cycles[i]  = is_pump ? ctx.links.stat_pump_cycles[ui]  : -1;
        if (on_time) on_time[i] = is_pump ? ctx.links.stat_pump_on_time[ui] : 0.0;
        // units: volume component internal ft^3 -> display VOLUME; cycles
        // (count) and on_time (seconds) stay raw.
        if (volume)  volume[i]  = is_pump
            ? to_display(ctx, openswmm::ucf::VOLUME, ctx.links.stat_pump_volume[ui])
            : 0.0;
    }
    return SWMM_OK;
}

// ============================================================================
// Hydraulic power
// ============================================================================

// TODO(units): hydraulic power composite-unit conversion
SWMM_ENGINE_API int swmm_link_get_hyd_power(SWMM_Engine engine, int idx, double* power) {
    CHECK_HANDLE(engine);
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    auto ui = static_cast<std::size_t>(idx);
    int n1 = ctx.links.node1[ui];
    int n2 = ctx.links.node2[ui];
    double h1 = (n1 >= 0) ? ctx.nodes.head[static_cast<std::size_t>(n1)] : 0.0;
    double h2 = (n2 >= 0) ? ctx.nodes.head[static_cast<std::size_t>(n2)] : 0.0;
    constexpr double GAMMA = 62.4;
    if (power) *power = GAMMA * std::fabs(ctx.links.flow[ui]) * std::fabs(h1 - h2);
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_rename(SWMM_Engine engine, int idx, const char* newId) {
    CHECK_HANDLE(engine);
    if (!newId || newId[0] == '\0') return SWMM_ERR_BADPARAM;
    auto& ctx = to_engine(engine)->context();
    CHECK_EDITABLE(ctx);
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    return ctx.link_names.rename(idx, newId) ? SWMM_OK : SWMM_ERR_BADPARAM;
}

SWMM_ENGINE_API int swmm_link_get_tag(SWMM_Engine engine, int idx,
                                       char* buf, int buflen) {
    CHECK_HANDLE(engine);
    if (!buf || buflen <= 0) return SWMM_ERR_BADPARAM;
    const auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto u = static_cast<std::size_t>(idx);
    const std::string& s = (u < ctx.links.tags.size()) ? ctx.links.tags[u]
                                                       : std::string{};
    const int copy_len = std::min(static_cast<int>(s.size()), buflen - 1);
    if (copy_len > 0) std::memcpy(buf, s.c_str(), static_cast<std::size_t>(copy_len));
    buf[copy_len] = '\0';
    return SWMM_OK;
}

SWMM_ENGINE_API int swmm_link_set_tag(SWMM_Engine engine, int idx,
                                       const char* tag) {
    CHECK_HANDLE(engine);
    auto& ctx = to_engine(engine)->context();
    CHECK_INDEX(idx >= 0 && idx < ctx.n_links());
    const auto u = static_cast<std::size_t>(idx);
    if (u >= ctx.links.tags.size()) ctx.links.tags.resize(u + 1);
    ctx.links.tags[u] = (tag != nullptr) ? std::string(tag) : std::string{};
    return SWMM_OK;
}

} /* extern "C" */
