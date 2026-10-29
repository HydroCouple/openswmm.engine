// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "MomentumPhysics2D.hpp"
#include "../data/BoundaryData.hpp"
#include "InertialKernels.hpp"
#include "InertialEdges.hpp"
#include "../data/MeshData.hpp"
#include <stdexcept>

namespace openswmm::twoD {
void MomentumPhysics2D::rotate(std::vector<double>& qx, std::vector<double>& qy, double angle) {
    if (angle == 0)
        return;
    const double c = std::cos(angle), s = std::sin(angle);
    for (std::size_t i = 0; i < qx.size(); ++i) {
        const double x = qx[i], y = qy[i];
        qx[i] = c * x + s * y;
        qy[i] = c * y - s * x;
    }
}

double MomentumPhysics2D::evaluate(const MeshData& m, const InertialEdges& edges,
                                   const std::vector<double>& h, const std::vector<double>& eta,
                                   const std::vector<double>& qx, const std::vector<double>& qy,
                                   double dry, double cs, std::vector<double>& rx,
                                   std::vector<double>& ry, double fixed_viscosity, double background,
                                   double depth_coefficient) {
    const int n = m.n_cells();
    rx.assign(n, 0);
    ry.assign(n, 0);
    viscosity.assign(n, 0);
    strain.assign(n, 0);
    vorticity.assign(n, 0);
    if (depth_coefficient > 0 && static_cast<int>(wall_distance.size()) != n)
        computeWallDistance(m);
    bound_x_.assign(n, 0);
    bound_y_.assign(n, 0);
    stencils_.assign(n, Stencil{});
    connected_.assign(edges.ne, 0);
    for (int e = 0; e < edges.ne; ++e) {
        const int i = edges.cL[e], j = edges.cR[e];
        const double sill = std::max(eta[i] - h[i], eta[j] - h[j]);
        connected_[e] = edges.conveyance[e] > 0 && h[i] > dry && h[j] > dry &&
                        std::min(eta[i], eta[j]) - sill > dry;
    }
    for (int i = 0; i < n; ++i) {
        if (h[i] <= dry)
            continue;
        auto& st = stencils_[i];
        st.ids[0] = i;
        st.count = 1;
        double xx = 0, xy = 0, yy = 0;
        for (int p = edges.cell_ptr[i]; p < edges.cell_ptr[i + 1]; ++p) {
            const int e = edges.cell_edge[p];
            if (!connected_[e])
                continue;
            const int j = edges.cL[e] == i ? edges.cR[e] : edges.cL[e];
            const double dx = m.tri_cx[j] - m.tri_cx[i], dy = m.tri_cy[j] - m.tri_cy[i];
            const double w = 1 / (dx * dx + dy * dy);
            st.ids[st.count] = j;
            st.gx[st.count] = w * dx;
            st.gy[st.count] = w * dy;
            ++st.count;
            xx += w * dx * dx;
            xy += w * dx * dy;
            yy += w * dy * dy;
        }
        const double det = xx * yy - xy * xy;
        if (st.count < 3 || det <= 1e-10 * (xx + yy) * (xx + yy)) {
            st.count = 1;
            continue;
        }
        double ux = 0, uy = 0, vx = 0, vy = 0;
        for (int k = 1; k < st.count; ++k) {
            const double x = st.gx[k], y = st.gy[k];
            st.gx[k] = (yy * x - xy * y) / det;
            st.gy[k] = (xx * y - xy * x) / det;
            st.gx[0] -= st.gx[k];
            st.gy[0] -= st.gy[k];
        }
        for (int k = 0; k < st.count; ++k) {
            const int j = st.ids[k];
            const double u = qx[j] / h[j], v = qy[j] / h[j];
            ux += st.gx[k] * u;
            uy += st.gy[k] * u;
            vx += st.gx[k] * v;
            vy += st.gy[k] * v;
        }
        strain[i] = std::sqrt(2 * ux * ux + 2 * vy * vy + (uy + vx) * (uy + vx));
        vorticity[i] = std::abs(vx - uy);
        viscosity[i] = fixed_viscosity >= 0 ? fixed_viscosity
                     : depth_coefficient > 0
                         ? background + std::pow(std::min(depth_coefficient * h[i], kappa * wall_distance[i]), 2) *
                                            vorticity[i]
                         : background + cs * cs * m.tri_area[i] * strain[i];
        if (!std::isfinite(viscosity[i]))
            throw std::runtime_error("Nonfinite Smagorinsky viscosity.");
    }
    // A corner with one wet neighbor cannot determine a transverse
    // gradient. Extend a connected neighbor's complete stencil; this
    // preserves rigid rotation and linear patch tests at such corners.
    for (int i = 0; i < n; ++i) {
        if (h[i] <= dry || stencils_[i].count != 1)
            continue;
        for (int p = edges.cell_ptr[i]; p < edges.cell_ptr[i + 1]; ++p) {
            const int e = edges.cell_edge[p];
            if (!connected_[e])
                continue;
            const int j = edges.cL[e] == i ? edges.cR[e] : edges.cL[e];
            if (stencils_[j].count > 1) {
                stencils_[i] = stencils_[j];
                break;
            }
        }
        const auto& st = stencils_[i];
        double ux = 0, uy = 0, vx = 0, vy = 0;
        for (int k = 0; k < st.count; ++k) {
            const int j = st.ids[k];
            const double u = qx[j] / h[j], v = qy[j] / h[j];
            ux += st.gx[k] * u;
            uy += st.gy[k] * u;
            vx += st.gx[k] * v;
            vy += st.gy[k] * v;
        }
        strain[i] = std::sqrt(2 * ux * ux + 2 * vy * vy + (uy + vx) * (uy + vx));
        vorticity[i] = std::abs(vx - uy);
        viscosity[i] = fixed_viscosity >= 0 ? fixed_viscosity
                     : depth_coefficient > 0
                         ? background + std::pow(std::min(depth_coefficient * h[i], kappa * wall_distance[i]), 2) *
                                            vorticity[i]
                         : background + cs * cs * m.tri_area[i] * strain[i];
    }
    for (int e = 0; e < edges.ne; ++e) {
        if (!connected_[e])
            continue;
        const int i = edges.cL[e], j = edges.cR[e];
        const double nx = edges.nx[e], ny = edges.ny[e];
        const double dx = m.tri_cx[j] - m.tri_cx[i], dy = m.tri_cy[j] - m.tri_cy[i];
        const double dn = dx * nx + dy * ny;
        if (!(dn > 0))
            throw std::runtime_error("Invalid mixing face geometry.");
        std::array<int, 12> ids{};
        std::array<double, 12> gx{}, gy{};
        int count = 0;
        auto add = [&](int id, double x, double y) {
            int k = 0;
            while (k < count && ids[k] != id)
                ++k;
            if (k == count) {
                ids[k] = id;
                ++count;
            }
            gx[k] += x;
            gy[k] += y;
        };
        const double fx = edges.mx[e], fy = edges.my[e];
        const double di = std::hypot(fx - m.tri_cx[i], fy - m.tri_cy[i]);
        const double dj = std::hypot(fx - m.tri_cx[j], fy - m.tri_cy[j]);
        const double wi = dj / (di + dj), wj = 1 - wi;
        for (int cell : {i, j}) {
            const auto& st = stencils_[cell];
            const double w = cell == i ? wi : wj;
            for (int k = 0; k < st.count; ++k)
                add(st.ids[k], w * st.gx[k], w * st.gy[k]);
        }
        add(i, 0, 0);
        add(j, 0, 0);
        double ux = 0, uy = 0, vx = 0, vy = 0, bx = 0, by = 0;
        const double nu =
            fixed_viscosity >= 0 ? fixed_viscosity : wi * viscosity[i] + wj * viscosity[j];
        const double wet =
            std::max(0., std::min(eta[i], eta[j]) - std::max(eta[i] - h[i], eta[j] - h[j]));
        const double weight = edges.xi[e] * wet * nu * edges.conveyance[e];
        for (int k = 0; k < count; ++k) {
            const int id = ids[k];
            const double delta = (id == j ? 1. : 0.) - (id == i ? 1. : 0.);
            const double correction = (delta - gx[k] * dx - gy[k] * dy) / dn;
            gx[k] += nx * correction;
            gy[k] += ny * correction;
            ux += gx[k] * qx[id] / h[id];
            uy += gy[k] * qx[id] / h[id];
            vx += gx[k] * qy[id] / h[id];
            vy += gy[k] * qy[id] / h[id];
            bx += std::abs(2 * nx * gx[k] + ny * gy[k]) + std::abs(ny * gx[k]);
            by += std::abs(nx * gy[k]) + std::abs(nx * gx[k] + 2 * ny * gy[k]);
        }
        const double tx = weight * (2 * ux * nx + (uy + vx) * ny);
        const double ty = weight * (2 * vy * ny + (uy + vx) * nx);
        rx[i] += tx / m.tri_area[i];
        ry[i] += ty / m.tri_area[i];
        rx[j] -= tx / m.tri_area[j];
        ry[j] -= ty / m.tri_area[j];
        for (int cell : {i, j}) {
            const double mass = m.tri_area[cell] * h[cell];
            bound_x_[cell] += weight * bx / mass;
            bound_y_[cell] += weight * by / mass;
        }
    }
    double rate = 0;
    for (int i = 0; i < n; ++i)
        rate = std::max({rate, bound_x_[i], bound_y_[i]});
    if (!std::isfinite(rate))
        throw std::runtime_error("Nonfinite mixing timestep bound.");
    last_limit = rate > 0 ? .4 / rate : std::numeric_limits<double>::infinity();
    return last_limit;
}

void MomentumPhysics2D::advance(const MeshData& m, const InertialEdges& edges,
                                const std::vector<double>& h, const std::vector<double>& eta,
                                std::vector<double>& qx, std::vector<double>& qy, double dry,
                                double cs, double seconds, double background,
                                double depth_coefficient) {
    if (seconds <= 0 || (cs == 0 && background == 0 && depth_coefficient == 0))
        return;
    struct Rollback {
        std::vector<double>& x;
        std::vector<double>& y;
        std::vector<double> old_x, old_y;
        bool accepted = false;
        ~Rollback() {
            if (!accepted) {
                x.swap(old_x);
                y.swap(old_y);
            }
        }
    } rollback{qx, qy, qx, qy};
    double elapsed = 0;
    while (elapsed < seconds) {
        const double bound =
            evaluate(m, edges, h, eta, qx, qy, dry, cs, rx_, ry_, -1, background, depth_coefficient);
        double dt = std::min(seconds - elapsed, bound);
        const double e0 = energy(m, h, qx, qy, dry);
        bool accepted = false;
        for (int retry = 0; retry < 40; ++retry) {
            tx_ = qx;
            ty_ = qy;
            for (std::size_t j = 0; j < tx_.size(); ++j) {
                tx_[j] += dt * rx_[j];
                ty_[j] += dt * ry_[j];
            }
            const double stage_bound =
                evaluate(m, edges, h, eta, tx_, ty_, dry, cs, sx_, sy_, -1, background, depth_coefficient);
            if (dt <= stage_bound) {
                for (std::size_t j = 0; j < tx_.size(); ++j) {
                    tx_[j] = .5 * (qx[j] + tx_[j] + dt * sx_[j]);
                    ty_[j] = .5 * (qy[j] + ty_[j] + dt * sy_[j]);
                }
                const double e1 = energy(m, h, tx_, ty_, dry);
                if (std::isfinite(e1) && e1 <= e0 + 1e-13 * std::max(e0, 1e-30)) {
                    accepted = true;
                    break;
                }
            }
            dt *= .5;
            ++retries;
        }
        if (!accepted || elapsed + dt == elapsed)
            throw std::runtime_error("Smagorinsky mixing cannot find an admissible timestep.");
        qx.swap(tx_);
        qy.swap(ty_);
        elapsed = dt == seconds - elapsed ? seconds : elapsed + dt;
        ++substeps;
    }
    evaluate(m, edges, h, eta, qx, qy, dry, cs, rx_, ry_, -1, background, depth_coefficient);
    rollback.accepted = true;
}

void MomentumPhysics2D::computeWallDistance(const MeshData& m) {
    std::vector<std::array<double, 4>> segments;
    for (int c = 0; c < m.n_cells(); ++c)
        for (int k = 0; k < m.cell_vertex_count(c); ++k)
            if (m.cell_neighbour(c, k) < 0) {
                int a, b;
                m.cell_edge_vertices(c, k, a, b);
                segments.push_back({m.vx[a], m.vy[a], m.vx[b], m.vy[b]});
            }
    wall_distance.assign(m.n_cells(), std::numeric_limits<double>::infinity());
    for (int c = 0; c < m.n_cells(); ++c) {
        const double px = m.tri_cx[c], py = m.tri_cy[c];
        double best = std::numeric_limits<double>::infinity();
        for (const auto& s : segments) {
            const double dx = s[2] - s[0], dy = s[3] - s[1], len2 = dx * dx + dy * dy;
            const double t = len2 > 0 ? std::clamp(((px - s[0]) * dx + (py - s[1]) * dy) / len2, 0.0, 1.0) : 0.0;
            best = std::min(best, std::hypot(px - s[0] - t * dx, py - s[1] - t * dy));
        }
        wall_distance[c] = best;
    }
}

double MomentumPhysics2D::energy(const MeshData& m, const std::vector<double>& h,
                                 const std::vector<double>& qx, const std::vector<double>& qy,
                                 double dry) {
    double sum = 0;
    for (int i = 0; i < m.n_cells(); ++i)
        if (h[i] > dry)
            sum += .5 * m.tri_area[i] * (qx[i] * qx[i] + qy[i] * qy[i]) / h[i];
    return sum;
}
} // namespace openswmm::twoD

namespace openswmm::twoD {
double MomentumPhysics2D::wallDrag(const MeshData& m, const BoundaryData& b,
    const std::vector<int>& slots, const std::vector<double>& h,
    const std::vector<double>& eta, std::vector<double>& qx, std::vector<double>& qy,
    double dry, double seconds, bool vfr) {
    double removed = 0;
    const auto apply = [&](int slot) {
        const int i = MeshData::slot_cell(slot);
        if (h[i] <= dry) return;
        double hw = h[i];
        if (vfr) {
            int a, z;
            m.cell_edge_vertices(i, MeshData::slot_local(slot), a, z);
            hw = inertial::faceDepthFromEta(eta[i], std::min(m.vz[a], m.vz[z]),
                                            std::max(m.vz[a], m.vz[z]));
        }
        const double k = b.edge_wall_drag[slot] * m.edge_length[slot] * hw /
                         (m.tri_area[i] * h[i]);
        if (k == 0) return;
        const double tx = -m.edge_ny[slot], ty = m.edge_nx[slot];
        const double ut = (qx[i] * tx + qy[i] * ty) / h[i];
        if (ut == 0) return;
        const double next = std::copysign(1.0 / (1.0 / std::abs(ut) + .5 * seconds * k), ut);
        const double dq = h[i] * (next - ut);
        qx[i] += dq * tx;
        qy[i] += dq * ty;
        removed += .5 * m.tri_area[i] * h[i] * (ut - next) * (ut + next);
    };
    for (int slot : slots) apply(slot);
    for (auto it = slots.rbegin(); it != slots.rend(); ++it) apply(*it);
    return removed;
}
} // namespace openswmm::twoD
