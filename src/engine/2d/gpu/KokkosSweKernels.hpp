// SPDX-License-Identifier: Apache-2.0
#pragma once
#include "KokkosTypes.hpp"
#pragma push_macro("OPENSWMM_KERNEL_FN")
#undef OPENSWMM_KERNEL_FN
#define OPENSWMM_KERNEL_FN KOKKOS_INLINE_FUNCTION
#include "../solver/SweKernels.hpp"
#pragma pop_macro("OPENSWMM_KERNEL_FN")

namespace openswmm::twoD::gpu::swe_kernels {
// Experimental building blocks, NOT a registered FULL_SWE backend. Geometry
// uses compact unique faces and padded four-slot cell geometry (tri/quad).
// Caller owns validated extents, active lists and execution ordering. Views
// must live in ExecSpace memory. No host pointers are captured by kernels.
struct Mesh {
    IView nv, neighbour, cell_ptr, cell_edge, cell_sign, cL, cR;
    DView cx, cy, area, nx, ny, xi, mx, my, conveyance;
    DView cell_arm_x, cell_arm_y, slot_arm_x, slot_arm_y;
    DView boundary_nx, boundary_ny, boundary_length;
};
struct State {
    DView head, depth, volume, qx, qy;
    IView active, tier;
};
// Four coalesced cell blocks: elevation, u, v, depth (not interleaved).
struct Gradients {
    DView x, y;
};
// Each face writes only its own left/right slots. Cell gathering must consume
// and clear its own side in CSR order before a slot is reused or published.
struct Transfers {
    DView waterL, waterR, mxL, mxR, myL, myR, discharge;
};

inline void gradients(const Mesh &mesh, const State &state, const Gradients &grad,
                      const IView &cells, int count, double dry) {
    const int n = static_cast<int>(state.depth.extent(0));
    Kokkos::parallel_for(
        "swe_connected_gradients", Kokkos::RangePolicy<ExecSpace>(0, count), KOKKOS_LAMBDA(int k) {
            const int i = cells(k);
            grad.x(3 * n + i) = grad.y(3 * n + i) = grad.x(0 * n + i) = grad.y(0 * n + i) =
                grad.x(1 * n + i) = grad.y(1 * n + i) = grad.x(2 * n + i) = grad.y(2 * n + i) = 0.0;
            const double hi = state.depth(i);
            if (hi <= 10.0 * dry)
                return;
            const double ei = state.head(i);
            const double ui = state.qx(i) / hi, vi = state.qy(i) / hi;
            const double wi[4] = {ei, ui, vi, hi};
            double gx[4] = {}, gy[4] = {};
            double wmin[4] = {ei, ui, vi, hi}, wmax[4] = {ei, ui, vi, hi};
            const int begin = mesh.cell_ptr(i), end = mesh.cell_ptr(i + 1);
            unsigned connected = 0;
            int nfaces = 0;
            bool shore = false;
            for (int p = begin; p < end; ++p) {
                const int e = mesh.cell_edge(p);
                const int j = (mesh.cL(e) == i) ? mesh.cR(e) : mesh.cL(e);
                const double hj = state.depth(j);
                if (hj <= dry || !state.active(j)) {
                    shore = true;
                    continue;
                }
                const double ej = state.head(j);
                const double sill = Kokkos::max(ei - hi, ej - hj);
                // A wet neighbour behind a dry sill is not a sample of this
                // cell's connected water surface or velocity field.
                if (ei - sill <= dry || ej - sill <= dry) {
                    shore = true;
                    continue;
                }
                connected |= 1u << (p - begin);
                const double uj = state.qx(j) / hj, vj = state.qy(j) / hj;
                const double sgn = static_cast<double>(mesh.cell_sign(p));
                const double nx = sgn * mesh.nx(e) * mesh.xi(e), ny = sgn * mesh.ny(e) * mesh.xi(e);
                const double w[4] = {0.5 * (ei + ej), 0.5 * (ui + uj), 0.5 * (vi + vj),
                                     0.5 * (hi + hj)};
                const double wj[4] = {ej, uj, vj, hj};
                for (int m = 0; m < 4; ++m) {
                    gx[m] += w[m] * nx;
                    gy[m] += w[m] * ny;
                    wmin[m] = Kokkos::min(wmin[m], wj[m]);
                    wmax[m] = Kokkos::max(wmax[m], wj[m]);
                }
                ++nfaces;
            }
            if (nfaces == 0)
                return;
            const int nvc = mesh.nv(i);
            double phi[4] = {1.0, 1.0, 1.0, 1.0};
            if (shore) {
                if (nfaces < 2)
                    return;
                // Fit eta and depth to connected wet neighbours. Inverse squared
                // distance weighting makes the geometry test scale independent.
                // Keep this extra work out of the fully wet interior path.
                double xx = 0.0, xy = 0.0, yy = 0.0;
                double bx[4] = {}, by[4] = {};
                for (int p = begin; p < end; ++p) {
                    if (!(connected & (1u << (p - begin))))
                        continue;
                    const int e = mesh.cell_edge(p);
                    const int j = (mesh.cL(e) == i) ? mesh.cR(e) : mesh.cL(e);
                    const double dx = mesh.cx(j) - mesh.cx(i);
                    const double dy = mesh.cy(j) - mesh.cy(i);
                    const double wt = 1.0 / (dx * dx + dy * dy);
                    xx += wt * dx * dx;
                    xy += wt * dx * dy;
                    yy += wt * dy * dy;
                    const double wj[4] = {state.head(j), 0.0, 0.0, state.depth(j)};
                    for (int m = 0; m <= 3; m += 3) {
                        bx[m] += wt * dx * (wj[m] - wi[m]);
                        by[m] += wt * dy * (wj[m] - wi[m]);
                    }
                }
                const double det = xx * yy - xy * xy;
                // Nearly opposite wet neighbours do not reliably constrain a
                // transverse slope. Require a Gram-matrix condition number <98.
                if (det <= 1e-2 * (xx + yy) * (xx + yy))
                    return;
                for (int m = 0; m <= 3; m += 3) {
                    gx[m] = (yy * bx[m] - xy * by[m]) / det;
                    gy[m] = (xx * by[m] - xy * bx[m]) / det;
                }
                // A drying cell must export its own velocity. Extrapolating it
                // from the remaining wet neighbours can remove water faster
                // than momentum and accelerate the residual film.
                gx[1] = gy[1] = gx[2] = gy[2] = 0.0;
                phi[1] = phi[2] = 0.0;
            } else {
                // Physical boundary faces have no CSR entry: zero-gradient
                // contribution w_i*n*length completes the Green-Gauss sum.
                if (nfaces < nvc) {
                    for (int kk = 0; kk < nvc; ++kk) {
                        if (mesh.neighbour(4 * i + kk) >= 0)
                            continue;
                        const int slot = 4 * i + kk;
                        const double nx = mesh.boundary_nx(slot) * mesh.boundary_length(slot);
                        const double ny = mesh.boundary_ny(slot) * mesh.boundary_length(slot);
                        for (int m = 0; m < 4; ++m) {
                            gx[m] += wi[m] * nx;
                            gy[m] += wi[m] * ny;
                        }
                    }
                }
                const double inv_a = 1.0 / mesh.area(i);
                for (int m = 0; m < 4; ++m) {
                    gx[m] *= inv_a;
                    gy[m] *= inv_a;
                }
            }
            for (int p = begin; p < end; ++p) {
                if (!(connected & (1u << (p - begin))))
                    continue;
                const double ax = mesh.cell_arm_x(p), ay = mesh.cell_arm_y(p);
                for (int m = 0; m < 4; ++m) {
                    const double wf = wi[m] + gx[m] * ax + gy[m] * ay;
                    phi[m] = Kokkos::min(phi[m], swe::bjLimiter(wi[m], wf, wmin[m], wmax[m]));
                }
            }
            // Connected interior faces are already bounded by positive wet
            // depths. At shore/boundary cells also constrain the omitted faces,
            // so clipping a negative face value cannot create a different depth
            // polynomial. Scale eta with h to preserve a flat reconstructed bed.
            if (nfaces < nvc) {
                double positive_scale = 1.0;
                for (int kk = 0; kk < nvc; ++kk) {
                    const double ax = mesh.slot_arm_x(4 * i + kk);
                    const double ay = mesh.slot_arm_y(4 * i + kk);
                    const double dh = phi[3] * (gx[3] * ax + gy[3] * ay);
                    if (dh < -hi)
                        positive_scale = Kokkos::min(positive_scale, -hi / dh);
                }
                phi[3] *= positive_scale;
                phi[0] *= positive_scale;
            }
            grad.x(3 * n + i) = phi[3] * gx[3];
            grad.y(3 * n + i) = phi[3] * gy[3];
            grad.x(0 * n + i) = phi[0] * gx[0];
            grad.y(0 * n + i) = phi[0] * gy[0];
            grad.x(1 * n + i) = phi[1] * gx[1];
            grad.y(1 * n + i) = phi[1] * gy[1];
            grad.x(2 * n + i) = phi[2] * gx[2];
            grad.y(2 * n + i) = phi[2] * gy[2];
        });
}

// Interior flux booking for a single Euler stage. Pressure corrections are
// retained at blocked sills and are not multiplied by the conveyance factor,
// matching the CPU wall-pressure convention. The positivity factor scales
// mass and advective/pressure Riemann momentum together.
inline void faces(const Mesh &mesh, const State &state, const Gradients &grad, const Transfers &acc,
                  const IView &face_list, int count, const IView &face_tier, double dt, double dry,
                  double beta, bool second_order, bool global_step) {
    const int n = static_cast<int>(state.depth.extent(0));
    Kokkos::parallel_for(
        "swe_face_transfers", Kokkos::RangePolicy<ExecSpace>(0, count), KOKKOS_LAMBDA(int k) {
            const int e = face_list(k), a = mesh.cL(e), b = mesh.cR(e);
            swe::FaceFlux f;
            double clx, cly, crx, cry;
            bool wet;
            if (second_order) {
                const double ha = state.depth(a), hb = state.depth(b);
                const double za = state.head(a) - ha, zb = state.head(b) - hb;
                const double ax = mesh.mx(e) - mesh.cx(a), ay = mesh.my(e) - mesh.cy(a);
                const double bx = mesh.mx(e) - mesh.cx(b), by = mesh.my(e) - mesh.cy(b);
                const double ea = state.head(a) + grad.x(a) * ax + grad.y(a) * ay;
                const double eb = state.head(b) + grad.x(b) * bx + grad.y(b) * by;
                const double ua =
                    (ha > dry ? state.qx(a) / ha : 0.0) + grad.x(n + a) * ax + grad.y(n + a) * ay;
                const double va = (ha > dry ? state.qy(a) / ha : 0.0) + grad.x(2 * n + a) * ax +
                                  grad.y(2 * n + a) * ay;
                const double ub =
                    (hb > dry ? state.qx(b) / hb : 0.0) + grad.x(n + b) * bx + grad.y(n + b) * by;
                const double vb = (hb > dry ? state.qy(b) / hb : 0.0) + grad.x(2 * n + b) * bx +
                                  grad.y(2 * n + b) * by;
                const double hfA =
                    Kokkos::max(0.0, ha + grad.x(3 * n + a) * ax + grad.y(3 * n + a) * ay);
                const double hfB =
                    Kokkos::max(0.0, hb + grad.x(3 * n + b) * bx + grad.y(3 * n + b) * by);
                wet = swe::faceFluxReconBed(ea, ua, va, za, ha, eb, ub, vb, zb, hb, mesh.nx(e),
                                            mesh.ny(e), dry, f, clx, cly, crx, cry, ea - hfA,
                                            eb - hfB);
            } else {
                wet = swe::faceFlux(state.head(a), state.depth(a), state.qx(a), state.qy(a),
                                    state.head(b), state.depth(b), state.qx(b), state.qy(b),
                                    mesh.nx(e), mesh.ny(e), dry, f, clx, cly, crx, cry);
            }
            if (!wet) {
                acc.discharge(e) = 0.0;
                return;
            }
            double fh = f.mass * mesh.conveyance(e), fx = f.mx * mesh.conveyance(e),
                   fy = f.my * mesh.conveyance(e);
            if (fh != 0.0) {
                const int giver = fh > 0.0 ? a : b;
                const int refire = global_step ? 1 : (1 << (state.tier(giver) - face_tier(e)));
                const double budget =
                    (beta / mesh.nv(giver)) / refire * Kokkos::max(state.volume(giver), 0.0);
                const double take = Kokkos::abs(fh) * mesh.xi(e) * dt;
                if (take > budget) {
                    const double scale = take > 0.0 ? budget / take : 0.0;
                    fh *= scale;
                    fx *= scale;
                    fy *= scale;
                }
            }
            const double xdt = mesh.xi(e) * dt, dv = fh * xdt;
            acc.discharge(e) = fh;
            acc.waterL(e) -= dv;
            acc.waterR(e) += dv;
            acc.mxL(e) += (-fx + clx) * xdt;
            acc.myL(e) += (-fy + cly) * xdt;
            acc.mxR(e) += (fx + crx) * xdt;
            acc.myR(e) += (fy + cry) * xdt;
        });
}

// Gather only: no closure, wall pressure, friction, sources or RK ledger
// averaging. Returns integrated momentum increments (m^4/s), not q updates.
// Those operations belong to the future stage driver; do not call this a
// complete Euler/RK2 surface advance.
inline void gather(const Mesh &mesh, const State &state, const Transfers &acc, const IView &cells,
                   int count, const DView &dmx, const DView &dmy) {
    Kokkos::parallel_for(
        "swe_cell_gather", Kokkos::RangePolicy<ExecSpace>(0, count), KOKKOS_LAMBDA(int k) {
            const int i = cells(k);
            double dv = 0.0, dx = 0.0, dy = 0.0;
            for (int p = mesh.cell_ptr(i); p < mesh.cell_ptr(i + 1); ++p) {
                const int e = mesh.cell_edge(p);
                if (mesh.cell_sign(p) > 0) {
                    dv += acc.waterL(e);
                    dx += acc.mxL(e);
                    dy += acc.myL(e);
                    acc.waterL(e) = acc.mxL(e) = acc.myL(e) = 0.0;
                } else {
                    dv += acc.waterR(e);
                    dx += acc.mxR(e);
                    dy += acc.myR(e);
                    acc.waterR(e) = acc.mxR(e) = acc.myR(e) = 0.0;
                }
            }
            state.volume(i) = Kokkos::max(0.0, state.volume(i) + dv);
            dmx(i) = dx;
            dmy(i) = dy;
        });
}
// Caller must refresh depth/head after volume/source updates before this
// kernel. wall_ptr/slot enumerate only physical WALL boundaries. Non-wall
// boundaries and live node exchange are separate ordered stage operations.
inline void momentum(const Mesh &mesh, const State &state, const IView &cells, int count,
                     const DView &dmx, const DView &dmy, const IView &wall_ptr,
                     const IView &wall_slot, const DView &roughness, double dt, double dry) {
    Kokkos::parallel_for(
        "swe_cell_momentum", Kokkos::RangePolicy<ExecSpace>(0, count), KOKKOS_LAMBDA(int k) {
            const int i = cells(k);
            const double h = state.depth(i);
            if (h <= dry) {
                state.qx(i) = state.qy(i) = 0.0;
                return;
            }
            double dx = dmx(i), dy = dmy(i);
            for (int w = wall_ptr(i); w < wall_ptr(i + 1); ++w) {
                const int slot = wall_slot(w);
                const double nx = mesh.boundary_nx(slot), ny = mesh.boundary_ny(slot);
                const double ux = state.qx(i) / h, uy = state.qy(i) / h;
                const double un = ux * nx + uy * ny, ut = -ux * ny + uy * nx;
                const double qxg = h * (-un * nx - ut * ny), qyg = h * (-un * ny + ut * nx);
                swe::FaceFlux f;
                double lx, ly, rx, ry;
                if (swe::faceFlux(state.head(i), h, state.qx(i), state.qy(i), state.head(i), h, qxg,
                                  qyg, nx, ny, dry, f, lx, ly, rx, ry)) {
                    const double xdt = mesh.boundary_length(slot) * dt;
                    dx += (-f.mx + lx) * xdt;
                    dy += (-f.my + ly) * xdt;
                }
            }
            const double area = mesh.area(i);
            double qx = (area * state.qx(i) + dx) / area, qy = (area * state.qy(i) + dy) / area;
            swe::frictionUpdate(qx, qy, h, roughness(i), dt);
            state.qx(i) = qx;
            state.qy(i) = qy;
        });
}

// Generic RK2 ledger finish: snapshot before either Euler stage, accumulate
// both stages, then retain half their increment. NOT a state/closure update.
// Snapshot and live views must not alias and must have equal extents.
inline void averageLedger(const DView &initial, const DView &live) {
    Kokkos::parallel_for(
        "swe_rk2_ledger", Kokkos::RangePolicy<ExecSpace>(0, live.extent(0)),
        KOKKOS_LAMBDA(int i) { live(i) = initial(i) + 0.5 * (live(i) - initial(i)); });
}
} // namespace openswmm::twoD::gpu::swe_kernels
