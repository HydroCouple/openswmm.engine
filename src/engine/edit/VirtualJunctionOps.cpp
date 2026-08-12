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
 * @file VirtualJunctionOps.cpp
 * @brief Virtual-junction rule validation, flag editing, split and fusion.
 * @see VirtualJunctionOps.hpp
 * @ingroup engine_edit
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "VirtualJunctionOps.hpp"
#include "ObjectDeleter.hpp"
#include "../core/ErrorCodes.hpp"
#include "../input/PostParseResolver.hpp"
#include "../2d/data/MeshData.hpp"

#include <algorithm>
#include <cmath>

namespace openswmm::edit {

namespace {

constexpr double VJ_OFFSET_TOL = 1.0e-9;

/// Find up to two links attached to `node_idx`; returns total touch count.
int find_attached(const SimulationContext& ctx, int node_idx,
                  int& link_a, int& link_b) {
    link_a = link_b = -1;
    int touches = 0;
    for (int j = 0; j < ctx.n_links(); ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const int ends[2] = { ctx.links.node1[uj], ctx.links.node2[uj] };
        for (const int n : ends) {
            if (n != node_idx) continue;
            if      (touches == 0) link_a = j;
            else if (touches == 1) link_b = j;
            ++touches;
        }
    }
    return touches;
}

double offset_at_node(const SimulationContext& ctx, int j, int node_idx) {
    const auto uj = static_cast<std::size_t>(j);
    return (ctx.links.node1[uj] == node_idx) ? ctx.links.offset1[uj]
                                             : ctx.links.offset2[uj];
}

/// Conduit slope from end elevations and length (same convention as
/// PostParseResolver: drop over horizontal distance, negative when adverse).
double slope_from_ends(double e1, double e2, double length) {
    if (length <= 0.0) return 0.0;
    double delta = std::fabs(e1 - e2);
    double slope;
    if (delta >= length) slope = delta / length;
    else                 slope = delta / std::sqrt(length * length - delta * delta);
    return (e1 < e2) ? -slope : slope;
}

} // namespace

// ============================================================================
// vj_rule_violation
// ============================================================================

int vj_rule_violation(const SimulationContext& ctx, int node_idx) {
    if (node_idx < 0 || node_idx >= ctx.n_nodes()) return ERR_VJ_LINK_COUNT;

    int j1 = -1, j2 = -1;
    const int touches = find_attached(ctx, node_idx, j1, j2);

    // Rule 1: exactly two attached links, both conduits.
    if (touches != 2 ||
        ctx.links.type[static_cast<std::size_t>(j1)] != LinkType::CONDUIT ||
        ctx.links.type[static_cast<std::size_t>(j2)] != LinkType::CONDUIT)
        return ERR_VJ_LINK_COUNT;

    const auto u1 = static_cast<std::size_t>(j1);
    const auto u2 = static_cast<std::size_t>(j2);

    // Rule 2: identical cross-section (shape, geoms, curve/transect,
    // barrels). Roughness MAY differ (a material change at a grade break is
    // legitimate — resolved decision D2).
    const int cr1 = ctx.link_subtypes.conduit_row(j1);
    const int cr2 = ctx.link_subtypes.conduit_row(j2);
    const auto& CD = ctx.link_subtypes.conduits;
    const int b1 = (cr1 >= 0) ? CD.barrels[static_cast<std::size_t>(cr1)] : 1;
    const int b2 = (cr2 >= 0) ? CD.barrels[static_cast<std::size_t>(cr2)] : 1;
    if (ctx.links.xsect_shape[u1] != ctx.links.xsect_shape[u2] ||
        ctx.links.xsect_geom1[u1] != ctx.links.xsect_geom1[u2] ||
        ctx.links.xsect_geom2[u1] != ctx.links.xsect_geom2[u2] ||
        ctx.links.xsect_geom3[u1] != ctx.links.xsect_geom3[u2] ||
        ctx.links.xsect_geom4[u1] != ctx.links.xsect_geom4[u2] ||
        ctx.links.xsect_curve[u1] != ctx.links.xsect_curve[u2] ||
        b1 != b2)
        return ERR_VJ_XSECT_MISMATCH;

    // Rule 3: zero offsets at the node — the contract is invert continuity.
    if (std::fabs(offset_at_node(ctx, j1, node_idx)) > VJ_OFFSET_TOL ||
        std::fabs(offset_at_node(ctx, j2, node_idx)) > VJ_OFFSET_TOL)
        return ERR_VJ_OFFSET;

    // Rule 5: no lateral inflow source may target the node.
    for (int r = 0; r < ctx.ext_inflows.count(); ++r)
        if (ctx.ext_inflows.node_idx[static_cast<std::size_t>(r)] == node_idx)
            return ERR_VJ_LATERAL_INFLOW;
    for (int r = 0; r < ctx.dwf_inflows.count(); ++r)
        if (ctx.dwf_inflows.node_idx[static_cast<std::size_t>(r)] == node_idx)
            return ERR_VJ_LATERAL_INFLOW;
    for (int r = 0; r < ctx.rdii_assigns.count(); ++r)
        if (ctx.rdii_assigns.node_idx[static_cast<std::size_t>(r)] == node_idx)
            return ERR_VJ_LATERAL_INFLOW;
    for (int s = 0; s < ctx.subcatches.count(); ++s)
        if (ctx.subcatches.outlet_node[static_cast<std::size_t>(s)] == node_idx)
            return ERR_VJ_LATERAL_INFLOW;
    {
        const std::string& name = ctx.node_names.name_of(node_idx);
        for (const auto& d : ctx.lid_usage.drain_to)
            if (!d.empty() && d == name)
                return ERR_VJ_LATERAL_INFLOW;
    }
    if (ctx.twod_io.mesh != nullptr) {
        const auto& m = *ctx.twod_io.mesh;
        for (const int n : m.vert_coupled_node)
            if (n == node_idx) return ERR_VJ_LATERAL_INFLOW;
        for (const int n : m.tri_coupled_node)
            if (n == node_idx) return ERR_VJ_LATERAL_INFLOW;
    }

    return 0;
}

// ============================================================================
// vj_apply_derived_geometry
// ============================================================================

void vj_apply_derived_geometry(SimulationContext& ctx, int node_idx) {
    int j1 = -1, j2 = -1;
    if (find_attached(ctx, node_idx, j1, j2) < 1 || j1 < 0) return;
    const auto ui = static_cast<std::size_t>(node_idx);
    ctx.nodes.full_depth[ui]  = ctx.links.xsect_y_full[static_cast<std::size_t>(j1)];
    ctx.nodes.sur_depth[ui]   = 0.0;
    ctx.nodes.ponded_area[ui] = 0.0;
}

// ============================================================================
// vj_set_virtual
// ============================================================================

int vj_set_virtual(SimulationContext& ctx, int node_idx, bool make_virtual) {
    if (node_idx < 0 || node_idx >= ctx.n_nodes()) return -1;
    const auto ui = static_cast<std::size_t>(node_idx);

    if (!make_virtual) {
        ctx.nodes.is_virtual[ui] = 0;
        return 0;
    }

    if (ctx.nodes.type[ui] != NodeType::JUNCTION) return -1;
    const int code = vj_rule_violation(ctx, node_idx);
    if (code != 0) return code;

    ctx.nodes.is_virtual[ui] = 1;
    vj_apply_derived_geometry(ctx, node_idx);
    return 0;
}

// ============================================================================
// vj_split_conduit
// ============================================================================

SplitResult vj_split_conduit(SimulationContext& ctx, int link_idx, double t,
                             const std::string& new_node_name,
                             const std::string& new_link_name,
                             bool make_virtual) {
    SplitResult res;
    if (link_idx < 0 || link_idx >= ctx.n_links() ||
        !(t > 0.0 && t < 1.0) ||
        new_node_name.empty() || new_link_name.empty() ||
        ctx.node_names.find(new_node_name) >= 0 ||
        ctx.link_names.find(new_link_name) >= 0) {
        res.err = -1;
        return res;
    }
    const auto uj = static_cast<std::size_t>(link_idx);
    if (ctx.links.type[uj] != LinkType::CONDUIT) { res.err = -1; return res; }

    auto& links = ctx.links;
    auto& nodes = ctx.nodes;
    const int n1 = links.node1[uj];
    const int n2 = links.node2[uj];
    if (n1 < 0 || n2 < 0) { res.err = -1; return res; }

    // --- Break-point invert: interpolate along the conduit gradient ---
    const double e1 = nodes.invert_elev[static_cast<std::size_t>(n1)] + links.offset1[uj];
    const double e2 = nodes.invert_elev[static_cast<std::size_t>(n2)] + links.offset2[uj];
    const double break_invert = e1 + (e2 - e1) * t;

    // --- Vertex-aware polyline split point + vertex partition ---
    std::vector<double> px, py;
    px.push_back(ctx.spatial.node_x[static_cast<std::size_t>(n1)]);
    py.push_back(ctx.spatial.node_y[static_cast<std::size_t>(n1)]);
    if (uj < ctx.spatial.link_vertices_x.size()) {
        const auto& vx = ctx.spatial.link_vertices_x[uj];
        const auto& vy = ctx.spatial.link_vertices_y[uj];
        px.insert(px.end(), vx.begin(), vx.end());
        py.insert(py.end(), vy.begin(), vy.end());
    }
    px.push_back(ctx.spatial.node_x[static_cast<std::size_t>(n2)]);
    py.push_back(ctx.spatial.node_y[static_cast<std::size_t>(n2)]);

    double total = 0.0;
    std::vector<double> seg(px.size() - 1, 0.0);
    for (std::size_t s = 0; s + 1 < px.size(); ++s) {
        seg[s] = std::hypot(px[s + 1] - px[s], py[s + 1] - py[s]);
        total += seg[s];
    }
    double sx = px.front(), sy = py.front();
    std::size_t split_seg = 0;   // vertices [1 .. split_seg] stay upstream
    if (total > 0.0) {
        double target = t * total, run = 0.0;
        for (std::size_t s = 0; s < seg.size(); ++s) {
            if (run + seg[s] >= target || s + 1 == seg.size()) {
                const double f = (seg[s] > 0.0) ? (target - run) / seg[s] : 0.0;
                sx = px[s] + (px[s + 1] - px[s]) * f;
                sy = py[s] + (py[s + 1] - py[s]) * f;
                split_seg = s;
                break;
            }
            run += seg[s];
        }
    }

    // --- New node ---
    const int ni = ctx.node_names.add(new_node_name);
    ctx.nodes.grow_to(ctx.node_names.size());
    {
        const auto un = static_cast<std::size_t>(ctx.node_names.size());
        if (ctx.spatial.node_x.size() < un) ctx.spatial.node_x.resize(un, 0.0);
        if (ctx.spatial.node_y.size() < un) ctx.spatial.node_y.resize(un, 0.0);
    }
    ctx.node_subtypes.set_node_type(ctx.nodes, ni, NodeType::JUNCTION);
    const auto uni = static_cast<std::size_t>(ni);
    nodes.invert_elev[uni] = break_invert;
    nodes.full_depth[uni]  = links.xsect_y_full[uj];
    ctx.spatial.node_x[uni] = sx;
    ctx.spatial.node_y[uni] = sy;

    // --- New downstream conduit ---
    const int nj = ctx.link_names.add(new_link_name);
    links.grow_to(ctx.link_names.size());
    {
        const auto ul = static_cast<std::size_t>(ctx.link_names.size());
        if (ctx.spatial.link_vertices_x.size() < ul) ctx.spatial.link_vertices_x.resize(ul);
        if (ctx.spatial.link_vertices_y.size() < ul) ctx.spatial.link_vertices_y.resize(ul);
    }
    const int new_row = ctx.link_subtypes.set_link_type(links, nj, LinkType::CONDUIT);
    const auto unj = static_cast<std::size_t>(nj);

    // Cross-section copy (identical by construction).
    links.xsect_shape[unj]  = links.xsect_shape[uj];
    links.xsect_y_full[unj] = links.xsect_y_full[uj];
    links.xsect_a_full[unj] = links.xsect_a_full[uj];
    links.xsect_w_max[unj]  = links.xsect_w_max[uj];
    links.xsect_geom1[unj]  = links.xsect_geom1[uj];
    links.xsect_geom2[unj]  = links.xsect_geom2[uj];
    links.xsect_geom3[unj]  = links.xsect_geom3[uj];
    links.xsect_geom4[unj]  = links.xsect_geom4[uj];
    links.xsect_curve[unj]  = links.xsect_curve[uj];
    links.xsect_r_full[unj] = links.xsect_r_full[uj];
    links.xsect_s_full[unj] = links.xsect_s_full[uj];
    links.xsect_s_max[unj]  = links.xsect_s_max[uj];
    links.xsect_y_bot[unj]  = links.xsect_y_bot[uj];
    links.xsect_a_bot[unj]  = links.xsect_a_bot[uj];
    links.xsect_s_bot[unj]  = links.xsect_s_bot[uj];
    links.xsect_r_bot[unj]  = links.xsect_r_bot[uj];
    links.xsect_batch_shape[unj] = links.xsect_batch_shape[uj];
    links.is_closed[unj]    = links.is_closed[uj];
    links.q_limit[unj]      = links.q_limit[uj];
    links.has_flap_gate[unj]= links.has_flap_gate[uj];
    links.direction[unj]    = links.direction[uj];
    links.q0[unj]           = links.q0[uj];

    // Conduit side-table copy + length split.
    auto& CD = ctx.link_subtypes.conduits;
    const int orig_row = ctx.link_subtypes.conduit_row(link_idx);
    if (orig_row >= 0 && new_row >= 0) {
        const auto uor = static_cast<std::size_t>(orig_row);
        const auto unr = static_cast<std::size_t>(new_row);
        const double L = CD.length[uor];
        CD.roughness[unr]    = CD.roughness[uor];
        CD.barrels[unr]      = CD.barrels[uor];
        CD.culvert_code[unr] = CD.culvert_code[uor];
        CD.seep_rate[unr]    = CD.seep_rate[uor];
        // Loss partition: entrance loss stays with the upstream conduit,
        // exit loss moves to the new downstream conduit, average loss on both.
        CD.loss_inlet[unr]   = 0.0;
        CD.loss_outlet[unr]  = CD.loss_outlet[uor];
        CD.loss_avg[unr]     = CD.loss_avg[uor];
        CD.loss_outlet[uor]  = 0.0;
        // The break point lies on the original gradient, so both halves keep
        // the original slope.
        CD.slope[unr]        = CD.slope[uor];
        CD.length[uor]       = L * t;
        CD.length[unr]       = L * (1.0 - t);
        CD.mod_length[uor]   = CD.length[uor];
        CD.mod_length[unr]   = CD.length[unr];
    }

    // Rewire: original keeps the upstream end; new conduit takes the
    // downstream end and its offset.
    links.offset1[unj] = 0.0;
    links.offset2[unj] = links.offset2[uj];
    links.node1[unj]   = ni;
    links.node2[unj]   = n2;
    links.node2[uj]    = ni;
    links.offset2[uj]  = 0.0;

    // Vertex partition: interior points at cumulative length <= t stay with
    // the original conduit; the rest move to the new conduit.
    if (uj < ctx.spatial.link_vertices_x.size()) {
        auto& vx = ctx.spatial.link_vertices_x[uj];
        auto& vy = ctx.spatial.link_vertices_y[uj];
        std::vector<double> upx, upy, dnx, dny;
        for (std::size_t v = 0; v < vx.size(); ++v) {
            // Vertex v is the endpoint of polyline segment v (points are
            // node1, verts..., node2), so it stays upstream when its segment
            // index is at or before the split segment endpoint.
            if (v + 1 <= split_seg) { upx.push_back(vx[v]); upy.push_back(vy[v]); }
            else                    { dnx.push_back(vx[v]); dny.push_back(vy[v]); }
        }
        vx = std::move(upx);
        vy = std::move(upy);
        ctx.spatial.link_vertices_x[unj] = std::move(dnx);
        ctx.spatial.link_vertices_y[unj] = std::move(dny);
    }

    // Derived flow properties for both halves.
    input::recompute_conduit_flow_properties(ctx, link_idx);
    input::recompute_conduit_flow_properties(ctx, nj);

    if (make_virtual) {
        const int code = vj_set_virtual(ctx, ni, true);
        if (code != 0) { res.err = code; }  // node/link remain as a regular split
    }

    res.new_node_idx = ni;
    res.new_link_idx = nj;
    return res;
}

// ============================================================================
// vj_fuse
// ============================================================================

int vj_fuse(SimulationContext& ctx, int node_idx, int* surviving_link_out) {
    if (node_idx < 0 || node_idx >= ctx.n_nodes()) return -1;
    const auto ui = static_cast<std::size_t>(node_idx);
    if (!ctx.nodes.is_virtual[ui]) return -1;

    int j1 = -1, j2 = -1;
    const int touches = find_attached(ctx, node_idx, j1, j2);
    if (touches != 2 ||
        ctx.links.type[static_cast<std::size_t>(j1)] != LinkType::CONDUIT ||
        ctx.links.type[static_cast<std::size_t>(j2)] != LinkType::CONDUIT)
        return ERR_VJ_LINK_COUNT;

    auto& links = ctx.links;
    // Through orientation: up flows into the node, dn flows out of it.
    int up = -1, dn = -1;
    if      (links.node2[static_cast<std::size_t>(j1)] == node_idx &&
             links.node1[static_cast<std::size_t>(j2)] == node_idx) { up = j1; dn = j2; }
    else if (links.node2[static_cast<std::size_t>(j2)] == node_idx &&
             links.node1[static_cast<std::size_t>(j1)] == node_idx) { up = j2; dn = j1; }
    else return ERR_VJ_LINK_COUNT;

    const auto uu = static_cast<std::size_t>(up);
    const auto ud = static_cast<std::size_t>(dn);

    // Merge geometry: node coordinate becomes an interior vertex so the map
    // alignment is preserved — unless it lies (within tolerance) on the
    // straight segment between its neighbours, in which case it is dropped
    // so a split→fuse round-trip restores the original geometry exactly.
    std::vector<double> mx, my;
    if (uu < ctx.spatial.link_vertices_x.size()) {
        mx = ctx.spatial.link_vertices_x[uu];
        my = ctx.spatial.link_vertices_y[uu];
    }
    {
        const double nx = ctx.spatial.node_x[ui];
        const double ny = ctx.spatial.node_y[ui];
        const auto uup = static_cast<std::size_t>(links.node1[uu]);
        double px = mx.empty() ? ctx.spatial.node_x[uup] : mx.back();
        double py = my.empty() ? ctx.spatial.node_y[uup] : my.back();
        double qx, qy;
        const bool dn_has_verts = (ud < ctx.spatial.link_vertices_x.size() &&
                                   !ctx.spatial.link_vertices_x[ud].empty());
        if (dn_has_verts) {
            qx = ctx.spatial.link_vertices_x[ud].front();
            qy = ctx.spatial.link_vertices_y[ud].front();
        } else {
            const auto und = static_cast<std::size_t>(links.node2[ud]);
            qx = ctx.spatial.node_x[und];
            qy = ctx.spatial.node_y[und];
        }
        const double cross = (qx - px) * (ny - py) - (qy - py) * (nx - px);
        const double span2 = (qx - px) * (qx - px) + (qy - py) * (qy - py);
        const bool collinear = (cross * cross) <= 1.0e-18 * std::max(span2 * span2, 1.0);
        const bool between = ((nx - px) * (qx - px) + (ny - py) * (qy - py)) >= 0.0 &&
                             ((nx - qx) * (px - qx) + (ny - qy) * (py - qy)) >= 0.0;
        if (!(collinear && between)) {
            mx.push_back(nx);
            my.push_back(ny);
        }
    }
    if (ud < ctx.spatial.link_vertices_x.size()) {
        const auto& dvx = ctx.spatial.link_vertices_x[ud];
        const auto& dvy = ctx.spatial.link_vertices_y[ud];
        mx.insert(mx.end(), dvx.begin(), dvx.end());
        my.insert(my.end(), dvy.begin(), dvy.end());
    }

    // Rewire the surviving conduit to the downstream conduit's outlet.
    const int n2 = links.node2[ud];
    links.node2[uu]   = n2;
    links.offset2[uu] = links.offset2[ud];
    if (uu < ctx.spatial.link_vertices_x.size()) {
        ctx.spatial.link_vertices_x[uu] = std::move(mx);
        ctx.spatial.link_vertices_y[uu] = std::move(my);
    }

    auto& CD = ctx.link_subtypes.conduits;
    const int ur = ctx.link_subtypes.conduit_row(up);
    const int dr = ctx.link_subtypes.conduit_row(dn);
    if (ur >= 0 && dr >= 0) {
        const auto uur = static_cast<std::size_t>(ur);
        const auto udr = static_cast<std::size_t>(dr);
        const double L = CD.length[uur] + CD.length[udr];
        CD.length[uur]      = L;
        CD.mod_length[uur]  = L;
        // Exit loss of the pair belongs to the merged conduit's outlet end.
        CD.loss_outlet[uur] = CD.loss_outlet[udr];
        const double eu = ctx.nodes.invert_elev[static_cast<std::size_t>(links.node1[uu])]
                          + links.offset1[uu];
        const double ed = ctx.nodes.invert_elev[static_cast<std::size_t>(n2)]
                          + links.offset2[uu];
        CD.slope[uur] = slope_from_ends(eu, ed, L);
    }

    // Retire the downstream conduit, then the (now link-free) node.
    edit::delete_link(ctx, dn);
    const int surviving = (up > dn) ? up - 1 : up;
    edit::delete_node(ctx, node_idx);

    input::recompute_conduit_flow_properties(ctx, surviving);

    if (surviving_link_out) *surviving_link_out = surviving;
    return 0;
}

} // namespace openswmm::edit
