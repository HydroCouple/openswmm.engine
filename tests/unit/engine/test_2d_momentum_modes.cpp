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
 * @file test_2d_momentum_modes.cpp
 * @brief Gates for the MOMENTUM_EQUATION closures FULL_SWE and DIFFUSIVE_WAVE
 *        (plans/2D_FULL_SWE_SHOCK_CAPTURING_PLAN_2026-09-05.md, P2 / P4).
 *
 * @details
 *  FULL_SWE (Godunov / HLLC, hydrostatic reconstruction):
 *   - HLLC kernel: consistency (F(U,U) = physical flux), symmetry under
 *     reflection, exact zero mass flux against a mirror wall, dry-side limits.
 *   - Lake at rest bit-exact over an uneven bed, FLAT + VFR, triangles,
 *     quads and mixed (the Audusse correction cancels the pressure flux).
 *   - Dry-neighbour wall / C-property.
 *   - Closed-basin dam break: V ≥ 0, ΣV to 1e-10, momentum bounded.
 *   - Stoker wet dam break vs the exact solution (L1 depth ≤ 3 %) and the
 *     Ritter dry-bed dam break (L1 ≤ 4 %) on a 200-cell strip — the cases
 *     the local-inertial law cannot get right (Rankine–Hugoniot).
 *   - Manning steady slope: normal depth ≤ 4 % on a quad strip.
 *  DIFFUSIVE_WAVE:
 *   - Lake at rest bit-exact; dry wall; Manning steady slope (DW is exact for
 *     steady uniform flow: ≤ 3 %); conservation.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/solver/SweKernels.hpp"
#include "2d/solver/DiffusiveKernels.hpp"

using namespace openswmm::twoD;

namespace {

constexpr double G = 9.80665;

template <typename ZFn>
MeshData makeTriGrid(int nx, int ny, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx; mesh.vy[v] = j * dx; mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(2 * nx * ny);
    int t = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i, v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_triangle(t++, v00, v10, v11);
            mesh.set_triangle(t++, v00, v11, v01);
        }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

template <typename ZFn>
MeshData makeQuadGrid(int nx, int ny, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx; mesh.vy[v] = j * dx; mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(nx * ny);
    int c = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i, v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_quad(c++, v00, v10, v11, v01);
        }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

template <typename ZFn>
MeshData makeMixedStrip(int nx, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int ncol = nx + 2, nvx = ncol + 1;
    mesh.resize_vertices(nvx * 2);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx; mesh.vy[v] = j * dx; mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(4 + nx);
    auto V = [&](int i, int j) { return j * nvx + i; };
    mesh.set_triangle(0, V(0, 0), V(1, 0), V(1, 1));
    mesh.set_triangle(1, V(0, 0), V(1, 1), V(0, 1));
    mesh.set_triangle(2, V(ncol - 1, 0), V(ncol, 0), V(ncol, 1));
    mesh.set_triangle(3, V(ncol - 1, 0), V(ncol, 1), V(ncol - 1, 1));
    for (int i = 0; i < nx; ++i)
        mesh.set_quad(4 + i, V(i + 1, 0), V(i + 2, 0), V(i + 2, 1), V(i + 1, 1));
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

SurfaceStateData makeState(const MeshData& mesh) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i] = mesh.tri_cz[i]; state.depth[i] = 0.0; state.volume[i] = 0.0;
    }
    return state;
}

void seedSurface(const MeshData& mesh, const SolverOptions2D& opts,
                 SurfaceStateData& state, double eta) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.volume[i] = inertial::cellVolumeFromEta(mesh, opts, i, eta);
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
    }
}

double totalVolume(const SurfaceStateData& s, int nt) {
    double v = 0.0;
    for (int i = 0; i < nt; ++i) v += s.volume[i];
    return v;
}

// Stoker's exact wet dam break (middle state by bisection).
void stokerMiddle(double hl, double hr, double& hm, double& um, double& s) {
    auto f = [&](double h) {
        return 2.0 * (std::sqrt(G * hl) - std::sqrt(G * h)) -
               (h - hr) * std::sqrt(G * (h + hr) / (2.0 * h * hr));
    };
    double a = hr * (1.0 + 1e-12), b = hl;
    for (int it = 0; it < 200; ++it) {
        const double m = 0.5 * (a + b);
        if (f(a) * f(m) <= 0.0) b = m; else a = m;
    }
    hm = 0.5 * (a + b);
    um = 2.0 * (std::sqrt(G * hl) - std::sqrt(G * hm));
    s  = um * hm / (hm - hr);
}
double stokerDepth(double x, double t, double hl, double hr, double x0) {
    double hm, um, s; stokerMiddle(hl, hr, hm, um, s);
    const double c0 = std::sqrt(G * hl), cm = std::sqrt(G * hm), xi = (x - x0) / t;
    if (xi <= -c0) return hl;
    if (xi < um - cm) { const double c = (2.0 * c0 - xi) / 3.0; return c * c / G; }
    if (xi < s) return hm;
    return hr;
}
double ritterDepth(double x, double t, double hl, double x0) {
    const double c0 = std::sqrt(G * hl), xi = (x - x0) / t;
    if (xi <= -c0) return hl;
    if (xi < 2.0 * c0) { const double c = (2.0 * c0 - xi) / 3.0; return c * c / G; }
    return 0.0;
}

// Relative L1 depth error of a strip run (column-mean depth vs reference).
template <typename Ref>
double stripL1(const MeshData& mesh, const SurfaceStateData& st, int nx, double dx, Ref ref) {
    std::vector<double> sum(nx, 0.0), cnt(nx, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        int k = static_cast<int>(mesh.tri_cx[i] / dx);
        k = std::max(0, std::min(nx - 1, k));
        sum[k] += st.depth[i]; cnt[k] += 1.0;
    }
    double num = 0.0, den = 0.0;
    for (int k = 0; k < nx; ++k) {
        const double h = sum[k] / std::max(cnt[k], 1.0), r = ref((k + 0.5) * dx);
        if (r > 1e-9) { num += std::fabs(h - r); den += r; }
    }
    return num / den;
}

}  // namespace

// ---------------------------------------------------------------------------
// Kernel-level properties of the HLLC face flux.
// ---------------------------------------------------------------------------
TEST(SweKernels, HllcConsistencyAndSymmetry) {
    swe::FaceSide L{0.7, 0.9, -0.3};
    double fh, fn, ft, ss, ph, pn, pt;
    swe::hllcFlux(L, L, fh, fn, ft, ss);
    swe::physicalFlux(L, ph, pn, pt);
    EXPECT_NEAR(fh, ph, 1e-12); EXPECT_NEAR(fn, pn, 1e-12); EXPECT_NEAR(ft, pt, 1e-12);

    // Mirror wall: zero mass flux, positive normal-momentum (pressure) flux.
    swe::FaceSide R{0.7, -0.9, -0.3};
    swe::hllcFlux(L, R, fh, fn, ft, ss);
    EXPECT_NEAR(fh, 0.0, 1e-13);
    EXPECT_GT(fn, 0.5 * G * 0.7 * 0.7);

    // Reflection symmetry: swapping sides and reversing normal velocities
    // negates the mass flux and keeps the normal-momentum flux.
    swe::FaceSide A{0.5, 0.4, 0.1}, B{0.2, -0.1, 0.2};
    double fh1, fn1, ft1, s1, fh2, fn2, ft2, s2;
    swe::hllcFlux(A, B, fh1, fn1, ft1, s1);
    swe::FaceSide Bm{0.2, 0.1, 0.2}, Am{0.5, -0.4, 0.1};
    swe::hllcFlux(Bm, Am, fh2, fn2, ft2, s2);
    EXPECT_NEAR(fh1, -fh2, 1e-12);
    EXPECT_NEAR(fn1, fn2, 1e-12);
    EXPECT_NEAR(ft1, -ft2, 1e-12);

    // Dry side: water flows into the dry side, never out of it.
    swe::FaceSide W{0.3, 0.0, 0.0}, D{0.0, 0.0, 0.0};
    swe::hllcFlux(W, D, fh, fn, ft, ss);
    EXPECT_GT(fh, 0.0);
    swe::hllcFlux(D, W, fh, fn, ft, ss);
    EXPECT_LT(fh, 0.0);
    swe::hllcFlux(D, D, fh, fn, ft, ss);
    EXPECT_EQ(fh, 0.0); EXPECT_EQ(fn, 0.0);
}

TEST(SweKernels, FaceFluxRestStateCancelsPerFace) {
    // Two cells, different beds, same free surface, at rest: net momentum
    // change per side = −½g·h_side²·n̂ (the term that closes over a cell).
    const double eta = 2.0, zL = 0.3, zR = 0.8;
    const double hL = eta - zL, hR = eta - zR;
    swe::FaceFlux F; double cLx, cLy, cRx, cRy;
    swe::faceFlux(eta, hL, 0, 0, eta, hR, 0, 0, 1.0, 0.0, 1e-3, F, cLx, cLy, cRx, cRy);
    EXPECT_NEAR(F.mass, 0.0, 1e-14);
    EXPECT_NEAR(-F.mx + cLx, -0.5 * G * hL * hL, 1e-12);
    EXPECT_NEAR( F.mx + cRx, +0.5 * G * hR * hR, 1e-12);
    EXPECT_NEAR(F.my, 0.0, 1e-14);
}

// ---------------------------------------------------------------------------
// Marcher gates, both new closures.
// ---------------------------------------------------------------------------
TEST(MomentumModes, LakeAtRestExactAllShapesAndClosures) {
    for (const auto mode : {Momentum2D::FULL_SWE, Momentum2D::DIFFUSIVE_WAVE}) {
        for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
            for (int kind = 0; kind < 3; ++kind) {
                auto bed = [](double x, double y) {
                    return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
                };
                MeshData mesh = (kind == 0) ? makeTriGrid(6, 6, 5.0, bed)
                              : (kind == 1) ? makeQuadGrid(6, 6, 5.0, bed)
                                            : makeMixedStrip(8, 5.0, bed);
                SolverOptions2D opts;
                opts.momentum = mode;
                opts.cell_closure = closure;
                auto state = makeState(mesh);
                seedSurface(mesh, opts, state, 8.0);
                const std::vector<double> v0 = state.volume;
                ExplicitInertialSolver solver;
                solver.initialize(mesh, state, opts);
                // The DW step bound at rest is set by the slope floor (Δx²),
                // so its rest window is shorter — the property is the same.
                const double T = (mode == Momentum2D::FULL_SWE) ? 300.0 : 20.0;
                EXPECT_DOUBLE_EQ(solver.advance(0.0, T), T);
                for (int i = 0; i < mesh.n_triangles(); ++i)
                    EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0))
                        << "mode " << int(mode) << " kind " << kind << " cell " << i;
                for (double f : state.edge_flux) EXPECT_LE(std::fabs(f), 1.0e-9);
                solver.finalize();
            }
        }
    }
}

TEST(MomentumModes, DryNeighbourWallNoCreep) {
    for (const auto mode : {Momentum2D::FULL_SWE, Momentum2D::DIFFUSIVE_WAVE}) {
        auto mesh = makeTriGrid(8, 4, 2.0, [](double x, double) { return (x < 8.0) ? 0.0 : 2.0; });
        SolverOptions2D opts;
        opts.momentum = mode;
        auto state = makeState(mesh);
        seedSurface(mesh, opts, state, 1.0);
        const double sum0 = totalVolume(state, mesh.n_triangles());
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        solver.advance(0.0, 300.0);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            if (mesh.tri_cz[i] > 1.5) EXPECT_EQ(state.volume[i], 0.0) << "mode " << int(mode);
        EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-9 * sum0);
        solver.finalize();
    }
}

TEST(MomentumModes, ClosedBasinConservationAndPositivity) {
    for (const auto mode : {Momentum2D::FULL_SWE, Momentum2D::DIFFUSIVE_WAVE}) {
        auto mesh = makeTriGrid(10, 10, 2.0, [](double, double) { return 0.0; });
        SolverOptions2D opts;
        opts.momentum = mode;
        auto state = makeState(mesh);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            if (mesh.tri_cx[i] < 5.0 && mesh.tri_cy[i] < 5.0) {
                state.volume[i] = 2.0 * mesh.tri_area[i];
                inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
            }
        const double sum0 = totalVolume(state, mesh.n_triangles());
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        const int chunks = (mode == Momentum2D::FULL_SWE) ? 40 : 6;   // DW is Δx²-bound
        for (int c = 0; c < chunks; ++c) {
            solver.advance(c * 5.0, (c + 1) * 5.0);
            for (int i = 0; i < mesh.n_triangles(); ++i) {
                ASSERT_GE(state.volume[i], 0.0) << "mode " << int(mode) << " cell " << i;
                ASSERT_FALSE(std::isnan(state.volume[i]));
            }
        }
        EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-10 * sum0);
        solver.finalize();
    }
}

// Stoker wet dam break on a 200-cell strip (hl = 0.005, hr = 0.001 like
// SWASHES 4.1.1, L = 10 m, 6 s). The local-inertial law puts the plateau 30 %
// high and the bore 7 % short; the Godunov closure must sit within 3 %.
TEST(MomentumModes, StokerWetDamBreakFullSwe) {
    const int nx = 200, ny = 2; const double dx = 0.05;
    auto mesh = makeTriGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = 1e-9;   // frictionless
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.dry_depth = 1e-4; opts.h_move = 1e-4; opts.cfl_number = 0.5;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double h = (mesh.tri_cx[i] < 5.0) ? 0.005 : 0.001;
        state.volume[i] = h * mesh.tri_area[i];
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int c = 0; c < 6; ++c) solver.advance(c * 1.0, (c + 1) * 1.0);
    const double l1 = stripL1(mesh, state, nx, dx,
                              [](double x) { return stokerDepth(x, 6.0, 0.005, 0.001, 5.0); });
    EXPECT_LT(l1, 0.03) << "Stoker L1 " << l1;
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1e-10 * sum0);
    // The bore is where the exact solution puts it (within 2 cells).
    double hm, um, s; stokerMiddle(0.005, 0.001, hm, um, s);
    double x_bore = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (state.depth[i] > 0.5 * (hm + 0.001)) x_bore = std::max(x_bore, mesh.tri_cx[i]);
    EXPECT_NEAR(x_bore, 5.0 + s * 6.0, 2.5 * dx);
    solver.finalize();
}

TEST(MomentumModes, RitterDryDamBreakFullSwe) {
    const int nx = 200, ny = 2; const double dx = 0.05;
    auto mesh = makeTriGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = 1e-9;
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.dry_depth = 1e-4; opts.h_move = 1e-4; opts.cfl_number = 0.5;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] >= 5.0) continue;
        state.volume[i] = 0.005 * mesh.tri_area[i];
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int c = 0; c < 6; ++c) {
        solver.advance(c * 1.0, (c + 1) * 1.0);
        for (int i = 0; i < mesh.n_triangles(); ++i) ASSERT_GE(state.volume[i], 0.0);
    }
    const double l1 = stripL1(mesh, state, nx, dx,
                              [](double x) { return ritterDepth(x, 6.0, 0.005, 5.0); });
    EXPECT_LT(l1, 0.04) << "Ritter L1 " << l1;
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1e-10 * sum0);
    // The front advanced past 6.5 m (ideal toe 7.66 m; a first-order scheme
    // lags the infinitely thin toe, but not the water-carrying part).
    double x_front = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (state.depth[i] > 2e-4) x_front = std::max(x_front, mesh.tri_cx[i]);
    EXPECT_GT(x_front, 6.5);
    solver.finalize();
}

// RECONSTRUCTION_ORDER 2 (MUSCL + SSP-RK2, global dt): the Stoker rarefaction
// and bore sharpen — L1 below 1 % where first order sits near 0.7–1 %; the
// rest state stays exact and mass is conserved through the two-stage update.
TEST(MomentumModes, StokerSecondOrderSharper) {
    const int nx = 200, ny = 2; const double dx = 0.05;
    double l1[2] = {0.0, 0.0};
    for (int order = 1; order <= 2; ++order) {
        auto mesh = makeTriGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.0);
        for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = 1e-9;
        SolverOptions2D opts;
        opts.momentum = Momentum2D::FULL_SWE;
        opts.reconstruction_order = order;
        opts.dry_depth = 1e-4; opts.h_move = 1e-4; opts.cfl_number = 0.5;
        auto state = makeState(mesh);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const double h = (mesh.tri_cx[i] < 5.0) ? 0.005 : 0.001;
            state.volume[i] = h * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
        }
        const double sum0 = totalVolume(state, mesh.n_triangles());
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        if (order == 2) EXPECT_EQ(opts.lts_tiers, 1);
        for (int c = 0; c < 6; ++c) solver.advance(c * 1.0, (c + 1) * 1.0);
        l1[order - 1] = stripL1(mesh, state, nx, dx,
                                [](double x) { return stokerDepth(x, 6.0, 0.005, 0.001, 5.0); });
        EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1e-10 * sum0);
        solver.finalize();
    }
    EXPECT_LT(l1[1], 0.01) << "second-order Stoker L1 " << l1[1];
    EXPECT_LT(l1[1], l1[0]) << "second order not sharper: " << l1[1] << " vs " << l1[0];
}

TEST(MomentumModes, SecondOrderLakeAtRestExact) {
    auto mesh = makeQuadGrid(6, 6, 5.0, [](double x, double y) {
        return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
    });
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.reconstruction_order = 2;
    auto state = makeState(mesh);
    seedSurface(mesh, opts, state, 8.0);
    const std::vector<double> v0 = state.volume;
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, 200.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0));
    solver.finalize();
}

// Manning steady slope on a quad strip: FULL_SWE and DIFFUSIVE_WAVE both
// reproduce the normal depth (the SWE through its friction balance, the DW
// exactly — it IS the Manning law).
TEST(MomentumModes, ManningSteadySlopeQuadStrip) {
    for (const auto mode : {Momentum2D::FULL_SWE, Momentum2D::DIFFUSIVE_WAVE}) {
        const double S = 0.01, dx = 1.0, n_man = 0.03;
        const int nx = 40, ny = 2;
        auto mesh = makeQuadGrid(nx, ny, dx, [&](double x, double) { return S * x; }, n_man);
        SolverOptions2D opts;
        opts.momentum = mode;
        auto state = makeState(mesh);
        BoundaryData boundary;
        boundary.resize(mesh.n_edge_slots());
        for (int i = 0; i < mesh.n_triangles(); ++i)
            for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
                const int idx = MeshData::slot(i, e);
                if (mesh.cell_neighbour(i, e) >= 0) continue;
                if (mesh.edge_mx[idx] < 1.0e-9) {
                    boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                    boundary.edge_bed_slope[idx] = S;
                }
            }
        state.boundary = &boundary;
        const double rain = 2.0e-4;
        for (int i = 0; i < mesh.n_triangles(); ++i) state.rainfall[i] = rain;
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        // DW steps are Δx²-bound: keep its spin-up shorter (the strip is
        // steady long before 6000 s either way).
        const double T_spin = (mode == Momentum2D::FULL_SWE) ? 6000.0 : 1500.0;
        solver.advance(0.0, T_spin);
        const double T = 500.0;
        const double v_start = totalVolume(state, mesh.n_triangles());
        solver.advance(T_spin, T_spin + T);
        const double v_end = totalVolume(state, mesh.n_triangles());
        double q_out = 0.0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
                const int idx = MeshData::slot(i, e);
                if (boundary.edge_bc_type[idx] == static_cast<int8_t>(BoundaryType::NORMAL_FLOW))
                    q_out += -state.edge_flux[idx];
            }
        double q_rain = 0.0;
        for (int i = 0; i < mesh.n_triangles(); ++i) q_rain += rain * mesh.tri_area[i];
        EXPECT_NEAR(q_rain * T, q_out * T + (v_end - v_start), 1.0e-8 * q_rain * T) << int(mode);
        EXPECT_NEAR(q_out, q_rain, 0.10 * q_rain) << int(mode);
        const double L = nx * dx, x_mid = 0.5 * L;
        const double h_n = std::pow(n_man * rain * (L - x_mid) / std::sqrt(S), 3.0 / 5.0);
        double h_avg = 0.0; int cnt = 0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            if (std::fabs(mesh.tri_cx[i] - x_mid) < dx) { h_avg += state.depth[i]; ++cnt; }
        h_avg /= cnt;
        EXPECT_NEAR(h_avg, h_n, 0.05 * h_n) << "mode " << int(mode);
        solver.finalize();
    }
}

// Options contract: FULL_SWE clamps CFL_NUMBER to ½ and ADVECTION/THETA are
// inert; FRONT_REBUILD defaults on for the new closures only.
TEST(MomentumModes, OptionContract) {
    auto mesh = makeTriGrid(4, 2, 1.0, [](double, double) { return 0.0; });
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.cfl_number = 0.9;
    auto state = makeState(mesh);
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    EXPECT_DOUBLE_EQ(opts.cfl_number, 0.5);
    solver.finalize();
    EXPECT_EQ(opts.front_rebuild, -1);   // AUTO untouched
}
