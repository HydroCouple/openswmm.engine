// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
// Analytic, fully pressurized graph fixtures for the P0 baseline audit.
#pragma once

#include "../../unit/engine/fv_test_support.hpp"

namespace fvbaseline {
using namespace fvtest;

inline bool networkCase(const std::string& name) {
    return name == "tee_pressure" || name == "loop_pressure" ||
           name == "mixed_reversed" || name == "tee_rest" || name == "loop_rest";
}

inline bool restCase(const std::string& name) {
    return name == "pressure_rest" || name == "tee_rest" || name == "loop_rest";
}

inline Channel makeNetwork(const std::string& name, int cells, double celerity) {
    struct Edge { int from, to; double length; };
    const bool loop = name == "loop_pressure" || name == "loop_rest";
    std::vector<Edge> edges;
    if (loop) {
        // Internal fork and merge remain unknowns, so the loop is not cut by
        // prescribed heads. Each has degree three and exercises the CG path.
        edges = {{0, 1, 100}, {1, 2, 150}, {2, 4, 150},
                 {1, 3, 150}, {3, 4, 150}, {4, 5, 100}};
    } else if (name == "mixed_reversed") {
        edges = {{0, 1, 100}, {1, 2, 200}, {3, 1, 400}};
    } else {
        edges = {{0, 1, 200}, {1, 2, 200}, {1, 3, 200}};
    }
    Channel ch;
    auto& m = ch.mesh;
    ch.n = cells * static_cast<int>(edges.size());
    const int nodes = loop ? 6 : 4;
    m.geom.resize(1);
    buildGeometry(circular(3.0), false, celerity, m.geom[0]);
    std::vector<std::vector<int>> node_faces(nodes);
    std::vector<std::vector<double>> node_signs(nodes);
    auto face = [&](int l, int r, int node, double dx) {
        const int f = m.n_faces();
        m.face_cl.push_back(l); m.face_cr.push_back(r);
        m.face_node.push_back(node); m.face_zb.push_back(0);
        m.face_dx.push_back(dx); m.face_dir_l.push_back(1);
        m.face_dir_r.push_back(1); m.face_virtual.push_back(0);
        m.face_vj_node.push_back(-1);
        if (node >= 0) {
            node_faces[node].push_back(f);
            node_signs[node].push_back(l < 0 ? -1.0 : 1.0);
        }
    };
    for (int e = 0; e < static_cast<int>(edges.size()); ++e) {
        const int begin = e * cells;
        const double dx = edges[e].length / cells;
        m.conduit_cell_begin.push_back(begin); m.conduit_cell_count.push_back(cells);
        m.conduit_link.push_back(e); m.conduit_section.push_back(0);
        m.conduit_roughness.push_back(0.013);
        m.conduit_rough_factor.push_back(32.2 * (0.013 / 1.486) * (0.013 / 1.486));
        m.conduit_loss_inlet.push_back(0); m.conduit_loss_outlet.push_back(0);
        m.conduit_slope.push_back(0); m.conduit_culvert_code.push_back(0);
        m.conduit_culvert_curve.emplace_back(); m.conduit_culvert_mitered.push_back(0);
        m.chain_ptr.push_back(begin);
        for (int i = 0; i < cells; ++i) {
            m.cell_geom.push_back(0); m.cell_conduit.push_back(e);
            m.cell_dx.push_back(dx); m.cell_zb.push_back(0); m.cell_dzdx.push_back(0);
            m.chain_cells.push_back(begin + i); m.chain_dir.push_back(1);
            m.cell_chain.push_back(e); m.cell_chain_pos.push_back(i);
        }
        face(-1, begin, edges[e].from, 0.5 * dx);
        for (int i = 1; i < cells; ++i) face(begin + i - 1, begin + i, -1, dx);
        face(begin + cells - 1, -1, edges[e].to, 0.5 * dx);
    }
    m.chain_ptr.push_back(ch.n);
    m.cell_face0.assign(ch.n, -1); m.cell_face1.assign(ch.n, -1);
    m.cell_side0.assign(ch.n, 0); m.cell_side1.assign(ch.n, 0);
    for (int f = 0; f < m.n_faces(); ++f) {
        for (int side = 0; side < 2; ++side) {
            const int c = side == 0 ? m.face_cl[f] : m.face_cr[f];
            if (c < 0) continue;
            if (m.cell_face0[c] < 0) { m.cell_face0[c] = f; m.cell_side0[c] = side; }
            else { m.cell_face1[c] = f; m.cell_side1[c] = side; }
        }
    }
    m.node_invert.assign(nodes, 0); m.node_full_depth.assign(nodes, 1.e6);
    m.node_ponded_area.assign(nodes, 0); m.node_sur_depth.assign(nodes, 0);
    m.node_can_pond.assign(nodes, 0); m.node_kind.assign(nodes, kNodeJunction);
    m.node_kind[0] = kNodeOutfall;
    if (loop) m.node_kind[5] = kNodeOutfall;
    else m.node_kind[2] = m.node_kind[3] = kNodeOutfall;
    m.node_area.assign(nodes, constants::MIN_SURFAREA);
    m.node_vol_off.assign(nodes, -1); m.node_vol_dmax.assign(nodes, 0);
    m.node_vol_atop.assign(nodes, 0);
    m.node_face_ptr.push_back(0);
    for (int n = 0; n < nodes; ++n) {
        m.node_face_idx.insert(m.node_face_idx.end(), node_faces[n].begin(), node_faces[n].end());
        m.node_face_sign.insert(m.node_face_sign.end(), node_signs[n].begin(), node_signs[n].end());
        m.node_face_zb.insert(m.node_face_zb.end(), node_faces[n].size(), 0);
        m.node_face_ptr.push_back(static_cast<int>(m.node_face_idx.size()));
    }
    ch.state.resize(ch.n, nodes, 0);
    seedLevel(ch, 5.5);
    ch.state.node_head.assign(nodes, 5.5);
    if (!restCase(name)) ch.state.node_head[0] = 6.0;
    return ch;
}

inline double symmetryDifference(const std::string& name, const Channel& ch) {
    std::vector<std::pair<int, int>> pairs;
    if (name == "tee_pressure" || name == "tee_rest") pairs = {{1, 2}};
    if (name == "loop_pressure" || name == "loop_rest") pairs = {{1, 3}, {2, 4}};
    double error = 0;
    for (auto [a, b] : pairs) {
        const int begin_a = ch.mesh.conduit_cell_begin[a];
        const int begin_b = ch.mesh.conduit_cell_begin[b];
        for (int i = 0; i < ch.mesh.conduit_cell_count[a]; ++i)
            error = std::max(error, std::abs(ch.state.cell_q[begin_a + i] - ch.state.cell_q[begin_b + i]));
    }
    return error;
}
} // namespace fvbaseline
