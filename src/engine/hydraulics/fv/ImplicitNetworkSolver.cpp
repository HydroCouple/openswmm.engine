// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      ImplicitNetworkSolver.cpp
 * @brief     Standalone implicit finite-volume network solver for supported wet regimes.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ImplicitNetworkSolver.hpp"
#include "ImplicitWetKernels.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <utility>

namespace openswmm::fv {
namespace {
using S = ImplicitNetworkSolver;
namespace k = kernels;
bool wet(const S::Problem& p) {
    return p.regime == S::Regime::BoundedWetSlot;
}
double minimumDepth(const S::Problem& p, const FvGeometry& g) {
    return wet(p) ? p.minimum_wet_depth : g.y_full + p.minimum_slot_excess;
}
bool wetClosure(const FvGeometry& g, double h) {
    const auto e = k::closureEval(g.closure_tbl, h);
    const double r = k::hydRadOfDepth(g, h);
    return std::isfinite(e.a) && e.a > k::kDryArea && std::isfinite(e.t) && e.t > 0.0 &&
           std::isfinite(e.i1) && std::isfinite(r) && r > 0.0 &&
           std::isfinite(k::celerity(e.a, e.t));
}
double rate(const std::vector<double>& v, int i) {
    return v.empty() ? 0.0 : v[i];
}
bool finite(const std::vector<double>& v) {
    return std::all_of(v.begin(), v.end(), [](double x) { return std::isfinite(x); });
}
bool unknown(const S::Problem& p, int n) {
    return (p.mesh->node_kind[n] == kNodeJunction || p.mesh->node_kind[n] == kNodeStorage) &&
           std::isnan(p.fixed_head[n]);
}
// Same piecewise-linear volume closure as ExplicitFvSolver, bounded to the
// sampled curve and rim. No extrapolated/ponded volume in this prototype.
double storageVolume(const NetworkMeshData& m, int j, double head, double* area = nullptr) {
    const double d = head - m.node_invert[j], dd = m.node_vol_dmax[j] / (kNodeVolSamples - 1);
    int i = std::clamp(static_cast<int>(d / dd), 0, kNodeVolSamples - 2);
    // At a rim coincident with a sample, use the admissible left derivative.
    // Mesh tables may carry a constant (clamped) tail ABOVE the physical rim.
    if (d == std::min(m.node_full_depth[j], m.node_vol_dmax[j]) && i > 0 && i * dd == d)
        --i;
    const auto off = static_cast<std::size_t>(m.node_vol_off[j]) + i;
    const double v0 = m.node_vol_tbl[off], dv = m.node_vol_tbl[off + 1] - v0;
    if (area)
        *area = dv / dd;
    if (d >= m.node_vol_dmax[j])
        return m.node_vol_tbl[static_cast<std::size_t>(m.node_vol_off[j]) + kNodeVolSamples - 1];
    const double t = (d - static_cast<double>(i) * dd) / dd;
    return v0 + dv * t;
}
bool storageHead(const NetworkMeshData& m, int j, double head) {
    const double d = head - m.node_invert[j];
    const double top = std::min(m.node_full_depth[j], m.node_vol_dmax[j]);
    // Check absolute head too: subtraction at a nonzero invert can round a
    // just-above-rim head back onto the supported depth limit.
    return std::isfinite(d) && d >= 0.0 && d <= top && head >= m.node_invert[j] &&
           head <= m.node_invert[j] + top;
}
bool valid(const S::Problem& p, double dt) {
    if (!p.mesh || !std::isfinite(dt) || dt <= 0.0 || !std::isfinite(p.minimum_slot_excess) ||
        p.minimum_slot_excess <= 0.0 || !std::isfinite(p.maximum_depth) ||
        (p.regime != S::Regime::PressurizedOnly && !wet(p)) ||
        (wet(p) && (!std::isfinite(p.minimum_wet_depth) || p.minimum_wet_depth <= k::kDryDepth ||
                    !(p.maximum_depth > p.minimum_wet_depth))))
        return false;
    const auto& m = *p.mesh;
    const auto n = m.cell_dx.size(), nf = m.face_cl.size(), nn = m.node_invert.size(),
               nc = m.conduit_cell_begin.size();
    if (!n || nn > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        n > (static_cast<std::size_t>(std::numeric_limits<int>::max()) - nn) / 2 ||
        p.old_area.size() != n || p.old_discharge.size() != n ||
        (!p.initial_depth.empty() && (p.initial_depth.size() != n || !finite(p.initial_depth))) ||
        !finite(p.old_area) || !finite(p.old_discharge) || p.fixed_head.size() != nn ||
        p.initial_head.size() != nn || !finite(p.initial_head) ||
        (!p.old_node_volume.empty() &&
         (p.old_node_volume.size() != nn || !finite(p.old_node_volume))) ||
        (!p.node_lateral.empty() && (p.node_lateral.size() != nn || !finite(p.node_lateral))) ||
        (!p.cell_lateral.empty() && (p.cell_lateral.size() != n || !finite(p.cell_lateral))) ||
        m.cell_geom.size() != n || m.cell_conduit.size() != n || m.cell_zb.size() != n ||
        m.cell_dzdx.size() != n || m.cell_face0.size() != n || m.cell_face1.size() != n ||
        m.cell_side0.size() != n || m.cell_side1.size() != n || m.face_cr.size() != nf ||
        m.face_node.size() != nf || m.face_geom.size() != nf || m.face_zb.size() != nf ||
        m.face_dir_l.size() != nf || m.face_dir_r.size() != nf ||
        (!m.face_gate.empty() && m.face_gate.size() != nf) ||
        (!m.face_culvert.empty() && m.face_culvert.size() != nf) || m.node_kind.size() != nn ||
        (!m.node_can_pond.empty() && m.node_can_pond.size() != nn) ||
        (wet(p) && m.node_sur_depth.size() != nn) ||
        (!m.node_vol_off.empty() && m.node_vol_off.size() != nn) ||
        m.conduit_cell_count.size() != nc || m.conduit_rough_factor.size() != nc ||
        m.conduit_loss_inlet.size() != nc || m.conduit_loss_outlet.size() != nc ||
        (!m.conduit_culvert_code.empty() && m.conduit_culvert_code.size() != nc) ||
        !m.struct_link.empty() || !m.struct_n1.empty() || !m.struct_n2.empty())
        return false;
    for (const auto& g : m.geom)
        if (g.is_open || g.xs.type == static_cast<int>(XSectShape::FORCE_MAIN) ||
            !std::isfinite(g.y_full) || !(g.t_slot > 0.0) || !(g.a_crown > 0.0) ||
            !(p.maximum_depth > minimumDepth(p, g)) ||
            (wet(p) && !wetClosure(g, p.minimum_wet_depth)))
            return false;
    std::size_t covered = 0;
    for (std::size_t c = 0; c < nc; ++c) {
        const int count = m.conduit_cell_count[c];
        if (count <= 0 || m.conduit_cell_begin[c] != static_cast<int>(covered) ||
            static_cast<std::size_t>(count) > n - covered ||
            !std::isfinite(m.conduit_rough_factor[c]) || m.conduit_rough_factor[c] < 0.0 ||
            m.conduit_loss_inlet[c] != 0.0 || m.conduit_loss_outlet[c] != 0.0 ||
            (!m.conduit_culvert_code.empty() && m.conduit_culvert_code[c] != 0))
            return false;
        for (int i = 0; i < count; ++i)
            if (m.cell_conduit[covered + i] != static_cast<int>(c))
                return false;
        covered += count;
    }
    if (covered != n)
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        if (m.cell_geom[i] < 0 || m.cell_geom[i] >= static_cast<int>(m.geom.size()) ||
            !std::isfinite(m.cell_zb[i]) || !std::isfinite(m.cell_dzdx[i]) ||
            !std::isfinite(m.cell_dx[i]) || m.cell_dx[i] <= 0.0 || p.old_area[i] <= 0.0 ||
            m.cell_face0[i] == m.cell_face1[i])
            return false;
        const auto& g = m.geom[m.cell_geom[i]];
        const double old_depth =
            p.initial_depth.empty() ? k::depthOfArea(g, p.old_area[i]) : p.initial_depth[i];
        if (!p.initial_depth.empty() && k::areaOfDepth(g, old_depth) != p.old_area[i])
            return false;
        if (!std::isfinite(old_depth) || old_depth < minimumDepth(p, g) ||
            old_depth > p.maximum_depth || (wet(p) && !wetClosure(g, old_depth)))
            return false;
        for (int side = 0; side < 2; ++side) {
            const int f = side ? m.cell_face1[i] : m.cell_face0[i];
            const int s = side ? m.cell_side1[i] : m.cell_side0[i];
            if (f < 0 || f >= static_cast<int>(nf) || (s != 0 && s != 1) ||
                (s == 0 ? m.face_cl[f] : m.face_cr[f]) != static_cast<int>(i))
                return false;
        }
        // A valid local axis has one incoming and one outgoing end. This also
        // accepts reversed virtual splices without assuming face-array order.
        const auto endSign = [&](int f, int s) {
            return (s == 0 ? 1 : -1) * (s == 0 ? m.face_dir_l[f] : m.face_dir_r[f]);
        };
        if (endSign(m.cell_face0[i], m.cell_side0[i]) + endSign(m.cell_face1[i], m.cell_side1[i]) !=
            0)
            return false;
    }
    std::vector<int> cell_degree(n, 0), node_degree(nn, 0);
    for (std::size_t f = 0; f < nf; ++f) {
        const int l = m.face_cl[f], r = m.face_cr[f], node = m.face_node[f];
        if (l < -1 || r < -1 || l >= static_cast<int>(n) || r >= static_cast<int>(n) || l == r ||
            node < -1 || node >= static_cast<int>(nn) ||
            (m.face_dir_l[f] != 1 && m.face_dir_l[f] != -1) ||
            (m.face_dir_r[f] != 1 && m.face_dir_r[f] != -1) || m.face_geom[f] < 0 ||
            m.face_geom[f] >= static_cast<int>(m.geom.size()) || !std::isfinite(m.face_zb[f]) ||
            (l >= 0 && r >= 0 && node != -1) || (!m.face_gate.empty() && m.face_gate[f] != 0) ||
            (!m.face_culvert.empty() && m.face_culvert[f] >= 0))
            return false;
        if (l >= 0)
            ++cell_degree[l];
        if (r >= 0)
            ++cell_degree[r];
        if (node >= 0)
            ++node_degree[node];
    }
    if (std::any_of(cell_degree.begin(), cell_degree.end(), [](int d) { return d != 2; }))
        return false;
    for (std::size_t j = 0; j < nn; ++j) {
        if (wet(p) && (!std::isfinite(m.node_sur_depth[j]) || m.node_sur_depth[j] < 0.0))
            return false;
        if (m.node_kind[j] != kNodeJunction && m.node_kind[j] != kNodeVirtual &&
            m.node_kind[j] != kNodeStorage && m.node_kind[j] != kNodeOutfall)
            return false;
        if (!m.node_can_pond.empty() && m.node_can_pond[j])
            return false;
        if (m.node_kind[j] == kNodeStorage) {
            if (p.old_node_volume.size() != nn || m.node_vol_off.size() != nn ||
                m.node_vol_dmax.size() != nn || m.node_full_depth.size() != nn ||
                !std::isfinite(m.node_invert[j]) || !std::isfinite(m.node_vol_dmax[j]) ||
                !(m.node_vol_dmax[j] > 0.0) || !std::isfinite(m.node_full_depth[j]) ||
                !(m.node_full_depth[j] > 0.0) || !std::isnan(p.fixed_head[j]) ||
                m.node_vol_off[j] < 0 ||
                static_cast<std::size_t>(m.node_vol_off[j]) > m.node_vol_tbl.size() ||
                m.node_vol_tbl.size() - static_cast<std::size_t>(m.node_vol_off[j]) <
                    kNodeVolSamples)
                return false;
            const auto off = static_cast<std::size_t>(m.node_vol_off[j]);
            if (m.node_vol_tbl[off] != 0.0)
                return false;
            const double dd = m.node_vol_dmax[j] / (kNodeVolSamples - 1);
            if (!(dd > 0.0))
                return false;
            for (int i = 1; i < kNodeVolSamples; ++i) {
                const double area = (m.node_vol_tbl[off + i] - m.node_vol_tbl[off + i - 1]) / dd;
                const bool active =
                    (i - 1) * dd < std::min(m.node_full_depth[j], m.node_vol_dmax[j]);
                if (!std::isfinite(area) || area < 0.0 || (active && area == 0.0))
                    return false;
            }
            if (!storageHead(m, j, p.initial_head[j]) ||
                storageVolume(m, j, p.initial_head[j]) != p.old_node_volume[j])
                return false;
        } else if ((!m.node_vol_off.empty() && m.node_vol_off[j] >= 0) ||
                   (!p.old_node_volume.empty() && p.old_node_volume[j] != 0.0))
            return false;
        if (m.node_kind[j] == 1) {
            if (node_degree[j] || !std::isnan(p.fixed_head[j]) || rate(p.node_lateral, j) != 0.0)
                return false;
        } else if (!node_degree[j] || (!std::isfinite(p.fixed_head[j]) && !unknown(p, j)) ||
                   (std::isfinite(p.fixed_head[j]) && rate(p.node_lateral, j) != 0.0))
            return false;
    }
    // Do not trust inconsistent CSR signs to silently create/destroy water.
    if (nn || !m.node_face_ptr.empty()) {
        if (m.node_face_ptr.size() != nn + 1 || m.node_face_ptr[0] != 0 ||
            m.node_face_idx.size() != m.node_face_sign.size() ||
            m.node_face_ptr.back() != static_cast<int>(m.node_face_idx.size()))
            return false;
        std::vector<int> seen(nf, 0);
        for (std::size_t j = 0; j < nn; ++j) {
            const int b = m.node_face_ptr[j], e = m.node_face_ptr[j + 1];
            if (b < 0 || e < b || e > static_cast<int>(m.node_face_idx.size()) ||
                e - b != node_degree[j])
                return false;
            for (int t = b; t < e; ++t) {
                const int f = m.node_face_idx[t];
                if (f < 0 || f >= static_cast<int>(nf) || m.face_node[f] != static_cast<int>(j) ||
                    ++seen[f] != 1 || m.node_face_sign[t] != (m.face_cl[f] < 0 ? -1.0 : 1.0))
                    return false;
            }
        }
    }
    return true;
}
bool admissible(const S::Problem& p, const S::Trial& x) {
    const auto& m = *p.mesh;
    if (x.depth.size() != p.old_area.size() || x.discharge.size() != x.depth.size() ||
        x.head.size() != p.fixed_head.size() || !finite(x.depth) || !finite(x.discharge) ||
        !finite(x.head))
        return false;
    for (std::size_t i = 0; i < x.depth.size(); ++i) {
        const auto& g = m.geom[m.cell_geom[i]];
        if (x.depth[i] < minimumDepth(p, g) || x.depth[i] > p.maximum_depth ||
            (wet(p) && !wetClosure(g, x.depth[i])))
            return false;
    }
    for (std::size_t j = 0; j < x.head.size(); ++j) {
        if (std::isfinite(p.fixed_head[j]) && x.head[j] != p.fixed_head[j])
            return false;
        if (m.node_kind[j] == kNodeStorage && !storageHead(m, j, x.head[j]))
            return false;
    }
    return true;
}
bool face(const S::Problem& p, const S::Trial& x, int f, S::FaceReceipt& out) {
    const auto& m = *p.mesh;
    const auto& gf = m.geom[m.face_geom[f]];
    const int cells[] = {m.face_cl[f], m.face_cr[f]};
    const int dirs[] = {m.face_dir_l[f], m.face_dir_r[f]};
    double eta[2]{}, bed[2]{}, u[2]{}, raw[2]{};
    for (int s = 0; s < 2; ++s)
        if (cells[s] >= 0) {
            const int c = cells[s];
            const auto& g = m.geom[m.cell_geom[c]];
            const auto e = k::closureEval(g.closure_tbl, x.depth[c]);
            eta[s] = m.cell_zb[c] + x.depth[c];
            bed[s] = m.cell_zb[c];
            u[s] = dirs[s] * x.discharge[c] / e.a;
            raw[s] = e.i1;
        }
    for (int s = 0; s < 2; ++s)
        if (cells[s] < 0) {
            const int other = 1 - s, node = m.face_node[f];
            bed[s] = m.face_zb[f];
            eta[s] = node >= 0 ? x.head[node] : eta[other];
            u[s] = node >= 0 ? u[other] : -u[other];
        }
    const double zstar = std::max(bed[0], bed[1]);
    k::FaceState state[2];
    for (int s = 0; s < 2; ++s) {
        const double h = eta[s] - zstar;
        // The wet floor applies to reconstructed sides too; no clipping.
        if (!std::isfinite(h) || h < minimumDepth(p, gf) || h > p.maximum_depth ||
            (wet(p) && !wetClosure(gf, h)))
            return false;
        const auto e = k::closureEval(gf.closure_tbl, h);
        state[s].a = e.a;
        state[s].u = u[s];
        state[s].q = e.a * u[s];
        state[s].c = k::celerity(e.a, e.t);
        state[s].i1 = e.i1;
        state[s].press = cells[s] < 0 && m.face_node[f] >= 0 ? 2 : 1;
        if (wet(p)) {
            state[s].press = h >= gf.y_full ? 1 : 0;
            if (cells[s] < 0 && m.face_node[f] >= 0 && m.node_sur_depth[m.face_node[f]] <= 0.0)
                state[s].press = 2;
        }
    }
    out.flux = k::riemannFlux(state[0], state[1]);
    out.correction_left = cells[0] >= 0 ? k::kGravity * (raw[0] - state[0].i1) : 0.0;
    out.correction_right = cells[1] >= 0 ? k::kGravity * (raw[1] - state[1].i1) : 0.0;
    return std::isfinite(out.flux.mass) && std::isfinite(out.flux.mom) &&
           std::isfinite(out.correction_left) && std::isfinite(out.correction_right);
}
double friction(const S::Problem& p, const S::Trial& x, int c) {
    const auto& m = *p.mesh;
    const auto& g = m.geom[m.cell_geom[c]];
    const double a = k::areaOfDepth(g, x.depth[c]), r = k::hydRadOfDepth(g, x.depth[c]),
                 q = x.discharge[c];
    return m.conduit_rough_factor[m.cell_conduit[c]] * std::fabs(q / a) * q / (r * std::cbrt(r));
}
// Same reconstruction as face(), seeded in its local column order. Only wet
// mode uses these derivatives; all trial receipts use the shared FV kernels.
bool wetFaceDerivatives(const S::Problem& p, const S::Trial& x, int f, bool head_unknown,
                        implicit_wet::Flux& flux, implicit_wet::Jet& left,
                        implicit_wet::Jet& right) {
    namespace w = implicit_wet;
    const auto& m = *p.mesh;
    const auto& gf = m.geom[m.face_geom[f]];
    const int cells[] = {m.face_cl[f], m.face_cr[f]}, dirs[] = {m.face_dir_l[f], m.face_dir_r[f]},
              j = m.face_node[f];
    w::Jet eta[2], u[2], raw[2];
    double bed[2]{};
    int column = 0;
    for (int s = 0; s < 2; ++s)
        if (cells[s] >= 0) {
            const int c = cells[s];
            const auto h = w::Jet::seed(x.depth[c], column++),
                       q = w::Jet::seed(x.discharge[c], column++);
            const auto e = w::closure(m.geom[m.cell_geom[c]].closure_tbl, h);
            bed[s] = m.cell_zb[c];
            eta[s] = bed[s] + h;
            u[s] = dirs[s] * q / e.area;
            raw[s] = e.moment;
        }
    for (int s = 0; s < 2; ++s)
        if (cells[s] < 0) {
            bed[s] = m.face_zb[f];
            eta[s] = j >= 0 ? (head_unknown ? w::Jet::seed(x.head[j], column) : w::Jet(x.head[j]))
                            : eta[1 - s];
            u[s] = j >= 0 ? u[1 - s] : -1.0 * u[1 - s];
        }
    const double zstar = std::max(bed[0], bed[1]);
    w::State states[2];
    for (int s = 0; s < 2; ++s) {
        const auto h = eta[s] - zstar;
        uint8_t press = h.v >= gf.y_full ? 1 : 0;
        if (cells[s] < 0 && j >= 0 && m.node_sur_depth[j] <= 0.0)
            press = 2;
        states[s] = w::state(gf.closure_tbl, h, u[s], press);
    }
    flux = w::flux(states[0], states[1]);
    left = cells[0] >= 0 ? k::kGravity * (raw[0] - states[0].i1) : w::Jet();
    right = cells[1] >= 0 ? k::kGravity * (raw[1] - states[1].i1) : w::Jet();
    return w::finite(flux.mass) && w::finite(flux.momentum) && w::finite(left) && w::finite(right);
}
std::vector<int> nodeRows(const S::Problem& p) {
    std::vector<int> rows(p.fixed_head.size(), -1);
    int row = static_cast<int>(2 * p.old_area.size());
    for (std::size_t j = 0; j < rows.size(); ++j)
        if (unknown(p, j))
            rows[j] = row++;
    return rows;
}
std::vector<double> flatten(const S::Problem& p, const S::Evaluation& e,
                            const S::Linearization& l) {
    std::vector<double> r(l.row_scale.size());
    for (std::size_t i = 0; i < e.volume.size(); ++i) {
        r[2 * i] = e.volume[i];
        r[2 * i + 1] = e.momentum[i];
    }
    for (std::size_t j = 0; j < l.nodes.size(); ++j) {
        const int node = l.nodes[j];
        r[2 * e.volume.size() + j] =
            p.mesh->node_kind[node] == kNodeStorage ? e.storage[node] : e.junction[node];
    }
    return r;
}
} // namespace

bool ImplicitNetworkSolver::residual(const Problem& p, const Trial& x, double dt,
                                     Evaluation& output) {
    if (!valid(p, dt) || !admissible(p, x))
        return false;
    const auto& m = *p.mesh;
    Evaluation e;
    e.volume.resize(x.depth.size());
    e.momentum.resize(x.depth.size());
    e.junction.assign(x.head.size(), 0.0);
    e.faces.resize(m.face_cl.size());
    e.storage.assign(x.head.size(), 0.0);
    e.node_volume.assign(x.head.size(), 0.0);
    for (int i = 0; i < m.n_cells(); ++i) {
        const auto& g = m.geom[m.cell_geom[i]];
        e.volume[i] = m.cell_dx[i] * (k::areaOfDepth(g, x.depth[i]) - p.old_area[i]) -
                      dt * rate(p.cell_lateral, i);
        e.momentum[i] = m.cell_dx[i] * (x.discharge[i] - p.old_discharge[i]) +
                        dt * m.cell_dx[i] * friction(p, x, i);
    }
    for (int j = 0; j < m.n_nodes(); ++j)
        if (unknown(p, j))
            e.junction[j] = rate(p.node_lateral, j);
    std::vector<double> mass_divergence(x.depth.size(), 0.0),
        momentum_divergence(x.depth.size(), 0.0);
    for (int f = 0; f < m.n_faces(); ++f) {
        auto& receipt = e.faces[f];
        if (!face(p, x, f, receipt))
            return false;
        for (int side = 0; side < 2; ++side) {
            const int c = side == 0 ? m.face_cl[f] : m.face_cr[f];
            if (c < 0)
                continue;
            const double sign = side == 0 ? 1.0 : -1.0;
            const int dir = side == 0 ? m.face_dir_l[f] : m.face_dir_r[f];
            const double correction =
                side == 0 ? receipt.correction_left : receipt.correction_right;
            mass_divergence[c] += sign * receipt.flux.mass;
            momentum_divergence[c] += sign * dir * (receipt.flux.mom + correction);
        }
        const int j = m.face_node[f];
        if (j >= 0 && unknown(p, j))
            e.junction[j] += (m.face_cl[f] < 0 ? -1.0 : 1.0) * receipt.flux.mass;
    }
    for (int c = 0; c < m.n_cells(); ++c) {
        e.volume[c] += dt * mass_divergence[c];
        e.momentum[c] += dt * momentum_divergence[c];
    }
    for (int j = 0; j < m.n_nodes(); ++j)
        if (m.node_kind[j] == kNodeStorage) {
            e.node_volume[j] = storageVolume(m, j, x.head[j]);
            e.storage[j] = e.node_volume[j] - p.old_node_volume[j] - dt * e.junction[j];
        }
    if (!finite(e.volume) || !finite(e.momentum) || !finite(e.junction) || !finite(e.storage) ||
        !finite(e.node_volume))
        return false;
    output = std::move(e);
    return true;
}

bool ImplicitNetworkSolver::linearize(const Problem& p, const Trial& x, double dt, double step,
                                      Linearization& output) {
    if (!std::isfinite(step) || step <= 0.0)
        return false;
    Evaluation e;
    if (!residual(p, x, dt, e))
        return false;
    const auto& m = *p.mesh;
    const int n = m.n_cells();
    const auto node_row = nodeRows(p);
    Linearization l;
    for (int j = 0; j < m.n_nodes(); ++j)
        if (node_row[j] >= 0)
            l.nodes.push_back(j);
    const int size = 2 * n + static_cast<int>(l.nodes.size());
    l.row_scale.resize(size);
    l.column_scale.assign(size, 1.0);
    using Entry = std::pair<int, double>;
    std::vector<std::vector<Entry>> rows(size);
    for (int i = 0; i < size; ++i) {
        rows[i].reserve(12);
        rows[i].emplace_back(i, 0.0);
    }
    auto trial = x;
    for (int c = 0; c < n; ++c) {
        const auto& g = m.geom[m.cell_geom[c]];
        const double h = wet(p) && !p.initial_depth.empty() ? p.initial_depth[c]
                                                            : k::depthOfArea(g, p.old_area[c]);
        const double t = k::widthOfDepth(g, h);
        const double qs = k::celerity(p.old_area[c], t) * t;
        l.column_scale[2 * c + 1] = qs;
        l.row_scale[2 * c] = m.cell_dx[c] * t;
        l.row_scale[2 * c + 1] = m.cell_dx[c] * qs;
        rows[2 * c].emplace_back(2 * c, m.cell_dx[c] * k::widthOfDepth(g, x.depth[c]));
        rows[2 * c + 1].emplace_back(2 * c + 1, m.cell_dx[c]);
        if (wet(p)) {
            const auto e = k::closureEval(g.closure_tbl, x.depth[c]);
            const double r = k::hydRadOfDepth(g, x.depth[c]);
            const double factor =
                m.conduit_rough_factor[m.cell_conduit[c]] / (e.a * r * std::cbrt(r));
            const double dh =
                friction(p, x, c) *
                (-e.t / e.a -
                 (4.0 / 3.0) * implicit_wet::radiusSlope(g.closure_tbl, x.depth[c]) / r);
            const double dq = 2.0 * factor * std::fabs(x.discharge[c]);
            rows[2 * c + 1].emplace_back(2 * c, dt * m.cell_dx[c] * dh);
            rows[2 * c + 1].emplace_back(2 * c + 1, dt * m.cell_dx[c] * dq);
            continue;
        }
        for (int v = 0; v < 2; ++v) {
            double& value = v == 0 ? trial.depth[c] : trial.discharge[c];
            const double base = value, delta = step * std::max(1.0, std::fabs(base));
            const double lo =
                v == 0 ? std::max(base - delta, g.y_full + p.minimum_slot_excess) : base - delta;
            const double hi = v == 0 ? std::min(base + delta, p.maximum_depth) : base + delta;
            if (!(hi > lo))
                return false;
            value = hi;
            const double plus = friction(p, trial, c);
            value = lo;
            const double minus = friction(p, trial, c);
            value = base;
            rows[2 * c + 1].emplace_back(2 * c + v, dt * m.cell_dx[c] * (plus - minus) / (hi - lo));
        }
    }
    for (int j : l.nodes) {
        double scale = 0.0;
        for (int t = m.node_face_ptr[j]; t < m.node_face_ptr[j + 1]; ++t) {
            const auto& g = m.geom[m.face_geom[m.node_face_idx[t]]];
            if (wet(p)) {
                const int f = m.node_face_idx[t],
                          c = m.face_cl[f] >= 0 ? m.face_cl[f] : m.face_cr[f];
                const double h = x.head[j] - std::max(m.face_zb[f], m.cell_zb[c]);
                if (h < minimumDepth(p, g) || h > p.maximum_depth || !wetClosure(g, h))
                    return false;
                const auto e = k::closureEval(g.closure_tbl, h);
                scale += k::celerity(e.a, e.t) * e.t;
            } else
                scale += k::celerity(g.a_crown, g.t_slot) * g.t_slot;
        }
        if (m.node_kind[j] == kNodeStorage) {
            double old_area = 0.0, area = 0.0;
            storageVolume(m, j, p.initial_head[j], &old_area);
            storageVolume(m, j, x.head[j], &area);
            l.row_scale[node_row[j]] = old_area + dt * scale;
            rows[node_row[j]].emplace_back(node_row[j], area);
        } else
            l.row_scale[node_row[j]] = scale;
    }
    // One mutable trial for ALL face-local perturbations, not an O(N) copy per
    // column. Work depends on the original face graph and junction degrees.
    for (int f = 0; f < m.n_faces(); ++f) {
        const int cells[] = {m.face_cl[f], m.face_cr[f]}, j = m.face_node[f];
        int columns[5]{}, count = 0;
        for (int c : cells)
            if (c >= 0) {
                columns[count++] = 2 * c;
                columns[count++] = 2 * c + 1;
            }
        if (j >= 0 && node_row[j] >= 0)
            columns[count++] = node_row[j];
        if (wet(p)) {
            implicit_wet::Flux flux;
            implicit_wet::Jet left, right;
            if (!wetFaceDerivatives(p, x, f, j >= 0 && node_row[j] >= 0, flux, left, right))
                return false;
            for (int v = 0; v < count; ++v) {
                const int col = columns[v];
                const double mass = flux.mass.d[v];
                for (int side = 0; side < 2; ++side)
                    if (cells[side] >= 0) {
                        const int c = cells[side],
                                  dir = side == 0 ? m.face_dir_l[f] : m.face_dir_r[f];
                        const double sign = side == 0 ? 1.0 : -1.0;
                        rows[2 * c].emplace_back(col, dt * sign * mass);
                        rows[2 * c + 1].emplace_back(
                            col, dt * sign * dir *
                                     (flux.momentum.d[v] + (side == 0 ? left.d[v] : right.d[v])));
                    }
                if (j >= 0 && node_row[j] >= 0)
                    rows[node_row[j]].emplace_back(col,
                                                   (m.node_kind[j] == kNodeStorage ? -dt : 1.0) *
                                                       (cells[0] < 0 ? -1.0 : 1.0) * mass);
            }
            continue;
        }
        for (int v = 0; v < count; ++v) {
            const int col = columns[v];
            double& value = col < 2 * n
                                ? (col % 2 ? trial.discharge[col / 2] : trial.depth[col / 2])
                                : trial.head[j];
            const double base = value, delta = step * std::max(1.0, std::fabs(base));
            double lower = -std::numeric_limits<double>::infinity(),
                   upper = std::numeric_limits<double>::infinity();
            if (col < 2 * n && col % 2 == 0) {
                lower = m.geom[m.cell_geom[col / 2]].y_full + p.minimum_slot_excess;
                upper = p.maximum_depth;
            }
            double hi = std::min(base + delta, upper), lo = std::max(base - delta, lower);
            FaceReceipt plus, minus;
            value = hi;
            bool hp = face(p, trial, f, plus);
            value = lo;
            bool hm = face(p, trial, f, minus);
            value = base;
            // One-sided differences at the supported reconstructed-depth bound.
            if (!hp) {
                hi = base;
                plus = e.faces[f];
            }
            if (!hm) {
                lo = base;
                minus = e.faces[f];
            }
            if (!(hi > lo))
                return false;
            const double mass = (plus.flux.mass - minus.flux.mass) / (hi - lo);
            for (int side = 0; side < 2; ++side)
                if (cells[side] >= 0) {
                    const int c = cells[side];
                    const double sign = side == 0 ? 1.0 : -1.0;
                    const int dir = side == 0 ? m.face_dir_l[f] : m.face_dir_r[f];
                    const double cp = side == 0 ? plus.correction_left : plus.correction_right;
                    const double cm = side == 0 ? minus.correction_left : minus.correction_right;
                    rows[2 * c].emplace_back(col, dt * sign * mass);
                    rows[2 * c + 1].emplace_back(
                        col, dt * sign * dir * ((plus.flux.mom + cp) - (minus.flux.mom + cm)) /
                                 (hi - lo));
                }
            if (j >= 0 && node_row[j] >= 0)
                rows[node_row[j]].emplace_back(col, (m.node_kind[j] == kNodeStorage ? -dt : 1.0) *
                                                        (cells[0] < 0 ? -1.0 : 1.0) * mass);
        }
    }
    l.residual = flatten(p, e, l);
    l.matrix.row.push_back(0);
    for (int r = 0; r < size; ++r) {
        if (!(l.row_scale[r] > 0.0) || !std::isfinite(l.row_scale[r]) ||
            !(l.column_scale[r] > 0.0) || !std::isfinite(l.column_scale[r]))
            return false;
        auto& row = rows[r];
        std::sort(row.begin(), row.end(),
                  [](const Entry& a, const Entry& b) { return a.first < b.first; });
        for (std::size_t t = 0; t < row.size();) {
            const int c = row[t].first;
            double value = 0.0;
            do {
                value += row[t++].second;
            } while (t < row.size() && row[t].first == c);
            value *= l.column_scale[c] / l.row_scale[r];
            if (!std::isfinite(value))
                return false;
            l.matrix.column.push_back(c);
            l.matrix.value.push_back(value);
        }
        l.matrix.row.push_back(l.matrix.value.size());
        l.residual[r] /= l.row_scale[r];
    }
    if (!finite(l.residual))
        return false;
    output = std::move(l);
    return true;
}

bool ImplicitNetworkSolver::Matrix::apply(const double* x, double* y) const {
    for (std::size_t i = 0; i + 1 < row.size(); ++i) {
        double sum = 0.0;
        for (auto t = row[i]; t < row[i + 1]; ++t)
            sum += value[t] * x[column[t]];
        if (!std::isfinite(sum))
            return false;
        y[i] = sum;
    }
    return true;
}

ImplicitNetworkSolver::LinearResult
ImplicitNetworkSolver::solveLinear(const Matrix& a, const std::vector<double>& rhs,
                                   const NativeGmres::Controls& controls,
                                   std::vector<double>& output, LinearWorkspace* workspace) {
    LinearResult result;
    LinearWorkspace local;
    auto& work = workspace ? *workspace : local;
    result.workspace.enabled = workspace != nullptr;
    auto drop = [&]() {
        if (work.bytes()) {
            work.release();
            result.workspace.cache_released = true;
        }
    };
    auto finish = [&]() {
        if (workspace) {
            result.preconditioner_bytes = std::max(result.preconditioner_bytes, work.factorBytes());
            if (!result.converged())
                drop();
            result.workspace.retained_bytes = work.bytes();
        }
        return result;
    };
    if (workspace && work.bytes() > controls.workspace_limit_bytes)
        drop();
    const auto n = rhs.size();
    if (!n || n > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
        a.row.size() != n + 1 || a.row.front() != 0 || a.row.back() != a.value.size() ||
        a.value.size() != a.column.size() || !finite(rhs) || !finite(a.value))
        return finish();
    for (std::size_t i = 0; i < n; ++i) {
        if (a.row[i] > a.row[i + 1] || a.row[i + 1] > a.value.size())
            return finish();
        int previous = -1;
        for (auto t = a.row[i]; t < a.row[i + 1]; ++t) {
            if (a.column[t] <= previous || a.column[t] >= static_cast<int>(n))
                return finish();
            previous = a.column[t];
        }
    }
    const bool reuse = workspace && work.matches(a, Preconditioner::ScalarIlu0, 0);
    if (workspace && !reuse)
        drop();
    auto fits = [&](std::initializer_list<std::pair<std::size_t, std::size_t>> terms) {
        std::size_t bytes = 0;
        for (auto [count, size] : terms) {
            if (count && size > (controls.workspace_limit_bytes - bytes) / count) {
                result.gmres.status = NativeGmres::Status::MemoryLimit;
                return false;
            }
            bytes += count * size;
        }
        return true;
    };
    if (!fits({{a.value.size(), sizeof(double)},
               {n, sizeof(std::size_t)},
               {workspace ? n + 1 : 0, sizeof(std::size_t)},
               {workspace ? a.column.size() : 0, sizeof(int)}}))
        return finish();
    try {
        auto& lu = work.scalar_lu_;
        auto& diagonal = work.diagonal_;
        lu.assign(a.value.begin(), a.value.end());
        diagonal.resize(n);
        if (!reuse) {
            for (std::size_t i = 0; i < n; ++i) {
                const auto b = a.column.begin() + a.row[i], e = a.column.begin() + a.row[i + 1];
                const auto d = std::lower_bound(b, e, static_cast<int>(i));
                if (d == e || *d != static_cast<int>(i))
                    return finish();
                diagonal[i] = static_cast<std::size_t>(d - a.column.begin());
            }
            if (workspace) {
                work.source_row_ = a.row;
                work.source_column_ = a.column;
                work.backend_ = Preconditioner::ScalarIlu0;
                work.pairs_ = 0;
                work.valid_ = true;
                result.workspace.symbolic_rebuilt = true;
            }
        } else
            result.workspace.symbolic_reused = true;
        result.preconditioner_bytes = work.factorBytes();
        if (!fits({{result.preconditioner_bytes, 1}}))
            return finish();
        for (std::size_t i = 0; i < n; ++i) {
            for (auto t = a.row[i]; t < diagonal[i]; ++t) {
                const int j = a.column[t];
                lu[t] /= lu[diagonal[j]];
                for (auto u = diagonal[j] + 1; u < a.row[j + 1]; ++u) {
                    const auto b = a.column.begin() + t + 1, e = a.column.begin() + a.row[i + 1];
                    const auto pos = std::lower_bound(b, e, a.column[u]);
                    if (pos != e && *pos == a.column[u])
                        lu[pos - a.column.begin()] -= lu[t] * lu[u];
                }
            }
            double scale = 0.0;
            for (auto t = a.row[i]; t < a.row[i + 1]; ++t) {
                if (!std::isfinite(lu[t])) {
                    result.gmres.status = NativeGmres::Status::PreconditionerFailure;
                    return finish();
                }
                scale = std::max(scale, std::fabs(lu[t]));
            }
            if (std::fabs(lu[diagonal[i]]) <= 1.e-12 * std::max(1.0, scale)) {
                lu[diagonal[i]] += std::copysign(.01 * std::max(1.0, scale), lu[diagonal[i]]);
                ++result.shifted_pivots;
            }
        }
        auto apply = [&](const double* x, double* y) { return a.apply(x, y); };
        auto inverse = [&](const double* x, double* y) {
            for (std::size_t i = 0; i < n; ++i) {
                y[i] = x[i];
                for (auto t = a.row[i]; t < diagonal[i]; ++t)
                    y[i] -= lu[t] * y[a.column[t]];
            }
            for (std::size_t i = n; i-- > 0;) {
                for (auto t = diagonal[i] + 1; t < a.row[i + 1]; ++t)
                    y[i] -= lu[t] * y[a.column[t]];
                y[i] /= lu[diagonal[i]];
                if (!std::isfinite(y[i]))
                    return false;
            }
            return true;
        };
        auto limits = controls;
        limits.workspace_limit_bytes -= result.preconditioner_bytes;
        result.gmres = work.gmres_.solve(rhs, apply, inverse, limits, output);
    } catch (const std::bad_alloc&) {
        result.gmres.status = NativeGmres::Status::MemoryLimit;
    }
    return finish();
}

bool ImplicitNetworkSolver::initializeHeads(const Problem& p, Trial& x, double tolerance) {
    if (!valid(p, 1.0) || !admissible(p, x) || !std::isfinite(tolerance) || tolerance <= 0.0)
        return false;
    auto trial = x;
    const auto& m = *p.mesh;
    for (int j = 0; j < m.n_nodes(); ++j)
        if (m.node_kind[j] == kNodeJunction && unknown(p, j)) {
            double lo = -std::numeric_limits<double>::infinity(),
                   hi = std::numeric_limits<double>::infinity();
            for (int t = m.node_face_ptr[j]; t < m.node_face_ptr[j + 1]; ++t) {
                const int f = m.node_face_idx[t],
                          c = m.face_cl[f] >= 0 ? m.face_cl[f] : m.face_cr[f];
                const auto& g = m.geom[m.face_geom[f]];
                const double z = std::max(m.face_zb[f], m.cell_zb[c]);
                lo = std::max(lo, z + minimumDepth(p, g));
                hi = std::min(hi, z + p.maximum_depth);
            }
            lo = std::nextafter(lo, std::numeric_limits<double>::infinity());
            hi = std::nextafter(hi, -std::numeric_limits<double>::infinity());
            auto balance = [&](double h, double& value) {
                trial.head[j] = h;
                value = rate(p.node_lateral, j);
                for (int t = m.node_face_ptr[j]; t < m.node_face_ptr[j + 1]; ++t) {
                    const int f = m.node_face_idx[t];
                    FaceReceipt receipt;
                    if (!face(p, trial, f, receipt))
                        return false;
                    value += m.node_face_sign[t] * receipt.flux.mass;
                }
                return std::isfinite(value);
            };
            double a = 0.0, b = 0.0, v = 0.0;
            if (!(hi > lo) || !balance(lo, a) || !balance(hi, b))
                return false;
            if (std::fabs(a) <= tolerance) {
                trial.head[j] = lo;
                continue;
            }
            if (std::fabs(b) <= tolerance) {
                trial.head[j] = hi;
                continue;
            }
            if (std::signbit(a) == std::signbit(b))
                return false;
            bool converged = false;
            for (int it = 0; it < 128; ++it) {
                const double h = it == 0 ? std::clamp(x.head[j], lo, hi) : lo + .5 * (hi - lo);
                if (!balance(h, v))
                    return false;
                if (std::fabs(v) <= tolerance) {
                    converged = true;
                    break;
                }
                if (std::signbit(v) == std::signbit(a)) {
                    lo = h;
                    a = v;
                } else
                    hi = h;
                if (lo == hi)
                    break;
            }
            if (!converged)
                return false;
        }
    x = std::move(trial);
    return true;
}

ImplicitNetworkSolver::Result ImplicitNetworkSolver::solve(const Problem& p, double dt,
                                                           const Controls& controls,
                                                           Endpoint& output) {
    Result result;
    const auto cache_limit = controls.method == LinearMethod::FullGmres
                                 ? controls.linear.workspace_limit_bytes
                                 : std::min(controls.linear.workspace_limit_bytes,
                                            controls.condensation.workspace_limit_bytes);
    bool entry_evicted = linear_workspace_.bytes() > 0 && (!controls.reuse_linear_workspace ||
                                                           linear_workspace_.bytes() > cache_limit);
    if (!controls.reuse_linear_workspace || linear_workspace_.bytes() > cache_limit)
        linear_workspace_.release();
    struct FailedSolve {
        LinearWorkspace& workspace;
        Result& result;
        ~FailedSolve() {
            if (!result.converged())
                workspace.release();
        }
    } cleanup{linear_workspace_, result};
    auto* workspace = controls.reuse_linear_workspace ? &linear_workspace_ : nullptr;
    if (!valid(p, dt) || controls.max_newton < 0 || controls.max_backtracks < 0 ||
        !std::isfinite(controls.difference_step) || controls.difference_step <= 0.0 ||
        (controls.preconditioner != Preconditioner::ScalarIlu0 &&
         controls.preconditioner != Preconditioner::BlockIlu0) ||
        (controls.method != LinearMethod::FullGmres &&
         controls.method != LinearMethod::CondensedThenGmres &&
         controls.method != LinearMethod::CondensedOnly))
        return result;
    for (double v :
         {controls.absolute_volume, controls.relative_volume, controls.absolute_momentum,
          controls.relative_momentum, controls.absolute_junction, controls.relative_junction})
        if (!std::isfinite(v) || v < 0.0)
            return result;
    if (controls.absolute_volume <= 0.0 || controls.absolute_momentum <= 0.0 ||
        controls.absolute_junction <= 0.0)
        return result;
    const auto& m = *p.mesh;
    const int n = m.n_cells();
    Trial x;
    x.discharge = p.old_discharge;
    x.head = p.initial_head;
    for (int i = 0; i < n; ++i)
        x.depth.push_back(p.initial_depth.empty()
                              ? k::depthOfArea(m.geom[m.cell_geom[i]], p.old_area[i])
                              : p.initial_depth[i]);
    for (int j = 0; j < m.n_nodes(); ++j)
        if (std::isfinite(p.fixed_head[j]))
            x.head[j] = p.fixed_head[j];
    if (!admissible(p, x))
        return result;
    if (!initializeHeads(p, x, .1 * controls.absolute_junction)) {
        result.status = Status::InitializationFailure;
        return result;
    }
    Evaluation e;
    if (!residual(p, x, dt, e))
        return result;
    const auto node_row = nodeRows(p);
    Linearization layout;
    for (int j = 0; j < m.n_nodes(); ++j)
        if (node_row[j] >= 0)
            layout.nodes.push_back(j);
    layout.row_scale.resize(2 * n + layout.nodes.size());
    std::vector<double> budget(layout.row_scale.size());
    for (int i = 0; i < n; ++i) {
        budget[2 * i] = controls.absolute_volume +
                        controls.relative_volume * std::max(1.0, m.cell_dx[i] * p.old_area[i]);
        budget[2 * i + 1] = controls.absolute_momentum +
                            controls.relative_momentum *
                                std::max(1.0, m.cell_dx[i] * std::fabs(p.old_discharge[i]));
    }
    for (int j : layout.nodes) {
        if (m.node_kind[j] == kNodeStorage) {
            budget[node_row[j]] = controls.absolute_volume +
                                  controls.relative_volume * std::max(1.0, p.old_node_volume[j]);
            continue;
        }
        double throughput = std::fabs(rate(p.node_lateral, j));
        for (int t = m.node_face_ptr[j]; t < m.node_face_ptr[j + 1]; ++t)
            throughput += std::fabs(e.faces[m.node_face_idx[t]].flux.mass);
        budget[node_row[j]] =
            controls.absolute_junction + controls.relative_junction * std::max(1.0, throughput);
    }
    auto norm = [&](const Evaluation& value) {
        const auto r = flatten(p, value, layout);
        double v = 0.0;
        for (std::size_t i = 0; i < r.size(); ++i)
            v = std::max(v, std::fabs(r[i]) / budget[i]);
        return v;
    };
    for (int it = 0; it <= controls.max_newton; ++it) {
        if (!residual(p, x, dt, e)) {
            result.status = Status::NonFinite;
            return result;
        }
        result.scaled_residual = norm(e);
        result.max_volume_residual = 0.0;
        result.max_momentum_residual = 0.0;
        result.max_junction_residual = 0.0;
        result.max_storage_residual = 0.0;
        for (double v : e.volume)
            result.max_volume_residual = std::max(result.max_volume_residual, std::fabs(v));
        for (double v : e.momentum)
            result.max_momentum_residual = std::max(result.max_momentum_residual, std::fabs(v));
        for (int j : layout.nodes) {
            if (m.node_kind[j] == kNodeStorage)
                result.max_storage_residual =
                    std::max(result.max_storage_residual, std::fabs(e.storage[j]));
            else
                result.max_junction_residual =
                    std::max(result.max_junction_residual, std::fabs(e.junction[j]));
        }
        if (result.scaled_residual <= 1.0) {
            Endpoint endpoint;
            endpoint.depth = std::move(x.depth);
            endpoint.discharge = std::move(x.discharge);
            endpoint.head = std::move(x.head);
            for (int i = 0; i < n; ++i)
                endpoint.area.push_back(k::areaOfDepth(m.geom[m.cell_geom[i]], endpoint.depth[i]));
            endpoint.faces = std::move(e.faces);
            endpoint.node_volume = std::move(e.node_volume);
            output = std::move(endpoint);
            result.status = Status::Converged;
            return result;
        }
        if (it == controls.max_newton) {
            result.status = Status::IterationLimit;
            return result;
        }
        Linearization l;
        if (!linearize(p, x, dt, controls.difference_step, l)) {
            result.status = Status::NonFinite;
            return result;
        }
        auto rhs = l.residual;
        for (auto& v : rhs)
            v = -v;
        auto limits = controls.linear;
        for (std::size_t r = 0; r < budget.size(); ++r)
            limits.absolute_tolerance =
                std::min(limits.absolute_tolerance, .1 * budget[r] / l.row_scale[r]);
        std::vector<double> delta;
        if (controls.method == LinearMethod::FullGmres) {
            result.last_linear = controls.preconditioner == Preconditioner::BlockIlu0
                                     ? solveBlockLinear(l.matrix, rhs, n, limits, delta, workspace)
                                     : solveLinear(l.matrix, rhs, limits, delta, workspace);
        } else {
            auto condensed = controls.condensation;
            condensed.preconditioner = controls.preconditioner;
            result.last_linear = solveCondensed(m, l.matrix, rhs, limits, condensed,
                                                controls.method == LinearMethod::CondensedThenGmres,
                                                delta, workspace);
        }
        if (workspace && entry_evicted) {
            result.last_linear.workspace.cache_released = true;
            entry_evicted = false;
        }
        result.linear_history.push_back(result.last_linear);
        result.gmres_iterations +=
            result.last_linear.gmres.iterations +
            (result.last_linear.fallback ? result.last_linear.condensation.reduced.iterations : 0);
        if (!result.last_linear.converged()) {
            result.status = Status::LinearFailure;
            return result;
        }
        bool accepted = false;
        double fraction = 1.0;
        for (int back = 0; back <= controls.max_backtracks; ++back) {
            auto trial = x;
            for (int i = 0; i < n; ++i) {
                trial.depth[i] += fraction * l.column_scale[2 * i] * delta[2 * i];
                trial.discharge[i] += fraction * l.column_scale[2 * i + 1] * delta[2 * i + 1];
            }
            for (int j : l.nodes)
                trial.head[j] += fraction * delta[node_row[j]];
            Evaluation next;
            if (residual(p, trial, dt, next) &&
                (norm(next) <= 1.0 ||
                 norm(next) < (1.0 - 1.e-4 * fraction) * result.scaled_residual)) {
                x = std::move(trial);
                accepted = true;
                break;
            }
            fraction *= .5;
            ++result.backtracks;
        }
        if (!accepted) {
            result.status = Status::LineSearchFailed;
            return result;
        }
        ++result.newton_iterations;
    }
    return result;
}
} // namespace openswmm::fv
