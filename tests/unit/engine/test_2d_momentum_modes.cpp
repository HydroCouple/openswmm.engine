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
 * @file      test_2d_momentum_modes.cpp
 * @brief     Gates for the MOMENTUM_EQUATION closures FULL_SWE and DIFFUSIVE_WAVE
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
#include "2d/solver/SurfaceFluxCalculator.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "core/SimulationContext.hpp"
#include "core/SWMMEngine.hpp"
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_engine.h>

using namespace openswmm::twoD;

namespace {

constexpr double G = 9.80665;

/**
 * @brief Build a uniform rectangular momentum-test mesh with two triangles per square.
 *
 * No dimension, finite-value or callback-result validation occurs; callback/allocation/topology
 * failures propagate.
 *
 * @param[in] nx Positive square count along x; derived counts must fit integer/storage limits.
 * @param[in] ny Positive square count along y; derived counts must fit integer/storage limits.
 * @param[in] dx Positive square edge length in metres.
 * @param[in] z Synchronous callable z(x,y) returning vertex bed elevation in metres; not retained.
 * @param[in] n Uniform Manning roughness, default 0.03.
 * @return Owned mesh with sampled bed, uniform roughness and built geometry/topology.
 */
template <typename ZFn> MeshData makeTriGrid(int nx, int ny, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = z(i * dx, j * dx);
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
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

/**
 * @brief Build a uniform rectangular momentum-test mesh with one quad per square.
 *
 * No dimension, finite-value or callback-result validation occurs; callback/allocation/topology
 * failures propagate.
 *
 * @param[in] nx Positive square count along x; derived counts must fit integer/storage limits.
 * @param[in] ny Positive square count along y; derived counts must fit integer/storage limits.
 * @param[in] dx Positive square edge length in metres.
 * @param[in] z Synchronous callable z(x,y) returning vertex bed elevation in metres; not retained.
 * @param[in] n Uniform Manning roughness, default 0.03.
 * @return Owned mesh with sampled bed, uniform roughness and built geometry/topology.
 */
template <typename ZFn> MeshData makeQuadGrid(int nx, int ny, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(nx * ny);
    int c = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i, v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_quad(c++, v00, v10, v11, v01);
        }
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

/**
 * @brief Build a one-row quad strip with two triangular end squares.
 *
 * No dimension/geometry/finite validation occurs here; callback/allocation/topology exceptions
 * propagate.
 *
 * @param[in] nx Positive interior quad count; derived nx+2 columns/counts must fit storage/index
 *     limits.
 * @param[in] dx Positive square edge length in metres.
 * @param[in] z Synchronous bed-elevation callable z(x,y) in metres, not retained.
 * @param[in] n Uniform Manning roughness, default 0.03.
 * @return Owned mesh of nx quads and four end triangles, with built topology.
 */
template <typename ZFn> MeshData makeMixedStrip(int nx, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int ncol = nx + 2, nvx = ncol + 1;
    mesh.resize_vertices(nvx * 2);
    for (int j = 0; j < 2; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = z(i * dx, j * dx);
        }
    mesh.resize_triangles(4 + nx);
    auto V = [&](int i, int j) { return j * nvx + i; };
    mesh.set_triangle(0, V(0, 0), V(1, 0), V(1, 1));
    mesh.set_triangle(1, V(0, 0), V(1, 1), V(0, 1));
    mesh.set_triangle(2, V(ncol - 1, 0), V(ncol, 0), V(ncol, 1));
    mesh.set_triangle(3, V(ncol - 1, 0), V(ncol, 1), V(ncol - 1, 1));
    for (int i = 0; i < nx; ++i)
        mesh.set_quad(4 + i, V(i + 1, 0), V(i + 2, 0), V(i + 2, 1), V(i + 1, 1));
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

/**
 * @brief Create dry fixture water storage with heads equal to cell bed elevations.
 *
 * @param[in] mesh Built mesh supplying compatible counts and cell bed elevations in metres.
 * @return Owned resized state with zero volume/depth and head=tri_cz for every cell; unrelated
 *     fields use defaults.
 */
SurfaceStateData makeState(const MeshData& mesh) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i] = mesh.tri_cz[i];
        state.depth[i] = 0.0;
        state.volume[i] = 0.0;
    }
    return state;
}

/**
 * @brief Seed a uniform water-surface elevation through the selected cell closure.
 *
 * Converts eta to cell volume then back to head/depth with production closure helpers, allowing dry
 * cells where appropriate. Does not initialize momentum or run dynamics; preconditions follow the
 * selected closure.
 *
 * @param[in] mesh Built mesh compatible with state.
 * @param[in] opts Options selecting the volume/elevation closure.
 * @param[in,out] state Already resized surface state receiving per-cell volume/head/depth; other
 *     arrays remain unchanged.
 * @param[in] eta Common absolute water-surface elevation in metres.
 */
void seedSurface(const MeshData& mesh, const SolverOptions2D& opts, SurfaceStateData& state,
                 double eta) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.volume[i] = inertial::cellVolumeFromEta(mesh, opts, i, eta);
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
    }
}

/**
 * @brief Sum the selected prefix of SI surface cell volumes.
 *
 * Does not include groundwater, exchange queues or boundary ledgers; does not mutate state.
 *
 * @param[in] s Borrowed surface state with volume storage covering nt cells.
 * @param[in] nt Nonnegative cell count no greater than the volume vector length.
 * @return Ordered double sum in m3, zero for an empty prefix.
 */
double totalVolume(const SurfaceStateData& s, int nt) {
    double v = 0.0;
    for (int i = 0; i < nt; ++i)
        v += s.volume[i];
    return v;
}

// Stoker's exact wet dam break (middle state by bisection).
/**
 * @brief Solve the intermediate state of the exact wet-bed dam-break reference.
 *
 * Uses gravity 9.80665 m/s2 and 200 bisection iterations on the rarefaction/shock matching
 * equation. Requires the intended hl>hr>0 regime; square roots and hm-hr division are unguarded. No
 * production solver state is read or changed.
 *
 * @param[in] hl Positive upstream depth in metres, strictly greater than hr.
 * @param[in] hr Positive downstream depth in metres.
 * @param[out] hm Intermediate water depth in metres.
 * @param[out] um Intermediate velocity in m/s.
 * @param[out] s Right-moving shock speed in m/s.
 */
void stokerMiddle(double hl, double hr, double& hm, double& um, double& s) {
    auto f = [&](double h) {
        return 2.0 * (std::sqrt(G * hl) - std::sqrt(G * h)) -
               (h - hr) * std::sqrt(G * (h + hr) / (2.0 * h * hr));
    };
    double a = hr * (1.0 + 1e-12), b = hl;
    for (int it = 0; it < 200; ++it) {
        const double m = 0.5 * (a + b);
        if (f(a) * f(m) <= 0.0)
            b = m;
        else
            a = m;
    }
    hm = 0.5 * (a + b);
    um = 2.0 * (std::sqrt(G * hl) - std::sqrt(G * hm));
    s = um * hm / (hm - hr);
}
/**
 * @brief Evaluate the self-similar wet-bed dam-break depth reference.
 *
 * Calls stokerMiddle for every sample; no zero-time/invalid-depth guard or adaptive solve occurs.
 *
 * @param[in] x Sample position in metres.
 * @param[in] t Strictly positive time in seconds.
 * @param[in] hl Upstream depth in metres, strictly greater than hr.
 * @param[in] hr Strictly positive downstream depth in metres.
 * @param[in] x0 Initial dam location in metres.
 * @return Exact reference depth in metres from left reservoir, rarefaction, intermediate state or
 *     right reservoir.
 */
double stokerDepth(double x, double t, double hl, double hr, double x0) {
    double hm, um, s;
    stokerMiddle(hl, hr, hm, um, s);
    const double c0 = std::sqrt(G * hl), cm = std::sqrt(G * hm), xi = (x - x0) / t;
    if (xi <= -c0)
        return hl;
    if (xi < um - cm) {
        const double c = (2.0 * c0 - xi) / 3.0;
        return c * c / G;
    }
    if (xi < s)
        return hm;
    return hr;
}
/**
 * @brief Evaluate the exact dry-bed dam-break depth reference.
 *
 * Uses gravity 9.80665 m/s2; no zero-time/negative-depth validation or production solver call
 * occurs.
 *
 * @param[in] x Sample position in metres.
 * @param[in] t Strictly positive time in seconds.
 * @param[in] hl Positive upstream reservoir depth in metres.
 * @param[in] x0 Initial dam location in metres.
 * @return Reference depth in metres: hl left of the fan, fan depth inside, zero beyond the dry
 *     front.
 */
double ritterDepth(double x, double t, double hl, double x0) {
    const double c0 = std::sqrt(G * hl), xi = (x - x0) / t;
    if (xi <= -c0)
        return hl;
    if (xi < 2.0 * c0) {
        const double c = (2.0 * c0 - xi) / 3.0;
        return c * c / G;
    }
    return 0.0;
}

// Relative L1 depth error of a strip run (column-mean depth vs reference).
template <typename Ref>
/**
 * @brief Compute a column-mean relative depth error against a scalar reference.
 *
 * Clamps centroid bins to [0,nx-1], uses arithmetic cell means rather than area weights, and
 * assigns zero to empty bins. Requires a positive included reference sum; denominator is unguarded.
 * Callback/allocation failures propagate; no state mutation occurs.
 *
 * @param[in] mesh Mesh with centroid x coordinates and cells compatible with st.
 * @param[in] st Stable surface state with per-cell depth in metres.
 * @param[in] nx Strictly positive number of x bins, fitting storage limits.
 * @param[in] dx Strictly positive x-bin width in metres.
 * @param[in] ref Synchronous callable returning reference depth in metres at x; not retained.
 * @return Sum of absolute column-mean errors divided by summed reference depth over bins with
 *     reference>1e-9.
 */
double stripL1(const MeshData& mesh, const SurfaceStateData& st, int nx, double dx, Ref ref) {
    std::vector<double> sum(nx, 0.0), cnt(nx, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        int k = static_cast<int>(mesh.tri_cx[i] / dx);
        k = std::max(0, std::min(nx - 1, k));
        sum[k] += st.depth[i];
        cnt[k] += 1.0;
    }
    double num = 0.0, den = 0.0;
    for (int k = 0; k < nx; ++k) {
        const double h = sum[k] / std::max(cnt[k], 1.0), r = ref((k + 0.5) * dx);
        if (r > 1e-9) {
            num += std::fabs(h - r);
            den += r;
        }
    }
    return num / den;
}

} // namespace

// ---------------------------------------------------------------------------
// Kernel-level properties of the HLLC face flux.
// ---------------------------------------------------------------------------
TEST(SweKernels, HllcConsistencyAndSymmetry) {
    swe::FaceSide L{0.7, 0.9, -0.3};
    double fh, fn, ft, ss, ph, pn, pt;
    swe::hllcFlux(L, L, fh, fn, ft, ss);
    swe::physicalFlux(L, ph, pn, pt);
    EXPECT_NEAR(fh, ph, 1e-12);
    EXPECT_NEAR(fn, pn, 1e-12);
    EXPECT_NEAR(ft, pt, 1e-12);

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
    EXPECT_EQ(fh, 0.0);
    EXPECT_EQ(fn, 0.0);
}

TEST(SweKernels, FaceFluxRestStateCancelsPerFace) {
    // Two cells, different beds, same free surface, at rest: net momentum
    // change per side = −½g·h_side²·n̂ (the term that closes over a cell).
    const double eta = 2.0, zL = 0.3, zR = 0.8;
    const double hL = eta - zL, hR = eta - zR;
    swe::FaceFlux F;
    double cLx, cLy, cRx, cRy;
    swe::faceFlux(eta, hL, 0, 0, eta, hR, 0, 0, 1.0, 0.0, 1e-3, F, cLx, cLy, cRx, cRy);
    EXPECT_NEAR(F.mass, 0.0, 1e-14);
    EXPECT_NEAR(-F.mx + cLx, -0.5 * G * hL * hL, 1e-12);
    EXPECT_NEAR(F.mx + cRx, +0.5 * G * hR * hR, 1e-12);
    EXPECT_NEAR(F.my, 0.0, 1e-14);
}

// ---------------------------------------------------------------------------
// Marcher gates, both new closures.
// ---------------------------------------------------------------------------
// Independent clipped wet interval: length times mean positive endpoint height.
static double velocityTestSection(double za, double zb, double eta, double length) {
    const double ha = eta - za, hb = eta - zb;
    if (ha <= 0 && hb <= 0) return 0.;
    if (ha >= 0 && hb >= 0) return length * (ha + hb) / 2.;
    const double wet = std::max(ha, hb), dry = std::min(ha, hb);
    return length * wet / (wet - dry) * wet / 2.;
}

TEST(FaceVelocity, RecoversPartiallyWetTriangleAndQuadFieldsWithoutSpeedCap) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::DIFFUSIVE_WAVE})
      for (bool quad : {false, true})
       for (double scale : {.1, 1., 20.})
        for (double angle : {0., .37, 1.5707963267948966})
         for (double slope : {0., .12, 3.})
          for (double fraction : {.01, .1, .5, 1.5})
           for (double fast : {1., 60.}) {
            MeshData mesh;
            mesh.resize_vertices(quad ? 4 : 3);
            const double x[] = {0, 1, quad ? 1. : 0., 0}, y[] = {0, 0, 1, 1};
            for (int v = 0; v < mesh.n_vertices(); ++v) {
                mesh.vx[v] = scale * (x[v] * std::cos(angle) - y[v] * std::sin(angle));
                mesh.vy[v] = scale * (x[v] * std::sin(angle) + y[v] * std::cos(angle));
                mesh.vz[v] = scale * slope * (x[v] + .25 * y[v]);
            }
            mesh.resize_triangles(1);
            if (quad) mesh.set_quad(0, 0, 1, 2, 3);
            else mesh.set_triangle(0, 0, 1, 2);
            buildMeshTopology(mesh);
            SolverOptions2D opts;
            opts.momentum = mode;
            opts.cell_closure = CellClosure2D::VFR;
            opts.face_reconstruction = FaceDepth2D::VFR_FACE;
            opts.vfr_min_wet_frac = 0.;
            opts.dry_depth = 1e-8; // Resolve even the smallest manufactured wet interval.
            auto state = makeState(mesh);
            const auto [lo, hi] = std::minmax_element(mesh.vz.begin(), mesh.vz.end());
            const double eta = *lo + fraction * std::max(.005, *hi - *lo);
            state.volume[0] = inertial::cellVolumeFromEta(mesh, opts, 0, eta);
            inertial::cellEtaDepth(mesh, opts, 0, state.volume[0], state.head[0], state.depth[0]);
            const double ux = .2 * fast, uy = -.1 * fast;
            for (int e = 0; e < mesh.cell_vertex_count(0); ++e) {
                int a, b; mesh.cell_edge_vertices(0, e, a, b);
                const int slot = MeshData::slot(0, e);
                state.edge_flux[slot] = velocityTestSection(mesh.vz[a], mesh.vz[b], state.head[0],
                    mesh.edge_length[slot]) * (ux * mesh.edge_nx[slot] + uy * mesh.edge_ny[slot]);
            }
            const auto volume = state.volume, flow = state.edge_flux;
            computeFaceVelocity(mesh, state, opts);
            EXPECT_NEAR(state.face_vx[0], ux, 2e-10) << quad << ',' << scale << ',' << slope;
            EXPECT_NEAR(state.face_vy[0], uy, 2e-10);
            EXPECT_EQ(state.volume, volume);
            EXPECT_EQ(state.edge_flux, flow);
           }
}

TEST(FaceVelocity, OneWetEdgeAndInconsistentDrySectionsAreUnavailable) {
    auto mesh = makeTriGrid(1, 1, 1., [](double, double y) { return 3. * y; });
    SolverOptions2D opts;
    opts.cell_closure = CellClosure2D::VFR;
    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    opts.dry_depth = 1e-4;
    opts.vfr_min_wet_frac = 0.;
    // Isolate one wet triangle so a dry neighbor's regularized head cannot
    // supply a second hydraulic section in this rank-deficiency fixture.
    mesh.resize_triangles(1);
    mesh.set_triangle(0, 0, 1, 3);
    buildMeshTopology(mesh);
    auto state = makeState(mesh);
    seedSurface(mesh, opts, state, .02);
    computeFaceVelocity(mesh, state, opts);
    ASSERT_GT(state.depth[0], opts.dry_depth);
    EXPECT_TRUE(std::isnan(state.face_vx[0]));
    EXPECT_TRUE(std::isnan(state.face_vy[0]));
    // Two independent wet edges determine a finite zero vector, until a
    // nonzero averaged flow is introduced across the third, dry section.
    mesh.vz.assign(4, 1.);
    mesh.vz[0] = 0.;
    buildMeshTopology(mesh);
    state = makeState(mesh);
    seedSurface(mesh, opts, state, .5);
    computeFaceVelocity(mesh, state, opts);
    ASSERT_DOUBLE_EQ(state.face_vx[0], 0.);
    ASSERT_DOUBLE_EQ(state.face_vy[0], 0.);
    state.edge_flux[MeshData::slot(0, 0)] = .01;
    computeFaceVelocity(mesh, state, opts);
    EXPECT_TRUE(std::isnan(state.face_vx[0]));
    state.depth[0] = 0.;
    computeFaceVelocity(mesh, state, opts);
    EXPECT_EQ(state.face_vx[0], 0.);
    EXPECT_EQ(state.face_vy[0], 0.);
}

TEST(FaceVelocity, StageAndPorosityUseTheirHydraulicSections) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::DIFFUSIVE_WAVE})
      for (auto face : {FaceDepth2D::MEAN, FaceDepth2D::VFR_FACE}) {
        auto mesh = makeTriGrid(1, 1, 1., [](double, double) { return 0.; });
        auto state = makeState(mesh);
        SolverOptions2D opts;
        opts.momentum = mode;
        opts.face_reconstruction = face;
        seedSurface(mesh, opts, state, .002);
        BoundaryData boundary;
        boundary.resize(mesh.n_cells() * kMaxCellVerts);
        state.boundary = &boundary;
        for (int c = 0; c < mesh.n_cells(); ++c)
          for (int e = 0; e < 3; ++e) {
            const int slot = MeshData::slot(c, e);
            const bool inside = mesh.cell_neighbour(c, e) >= 0;
            mesh.edge_conveyance[slot] = inside ? .1 : 1.;
            boundary.edge_bc_type[slot] = static_cast<int8_t>(BoundaryType::SPECIFIED_STAGE);
            boundary.edge_bc_head[slot] = .1;
            const double area = mesh.edge_length[slot] * (inside ? .002 * .1 : .1);
            state.edge_flux[slot] = area * (12. * mesh.edge_nx[slot] - 6. * mesh.edge_ny[slot]);
          }
        computeFaceVelocity(mesh, state, opts);
        for (int c = 0; c < mesh.n_cells(); ++c) {
            EXPECT_NEAR(state.face_vx[c], 12., 1e-10);
            EXPECT_NEAR(state.face_vy[c], -6., 1e-10);
        }
        std::fill(mesh.edge_nx.begin(), mesh.edge_nx.end(), 1.);
        std::fill(mesh.edge_ny.begin(), mesh.edge_ny.end(), 0.);
        computeFaceVelocity(mesh, state, opts);
        EXPECT_TRUE(std::isnan(state.face_vx[0]));
      }
}

TEST(MomentumModes, VelocityDiagnosticsPreserveAllClosureHydraulics) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::DIFFUSIVE_WAVE, Momentum2D::FULL_SWE})
      for (bool quads : {false, true}) {
        auto mesh = quads ? makeQuadGrid(6, 3, 1., [](double x, double y) { return .04*x + .02*y; })
                         : makeTriGrid(6, 3, 1., [](double x, double y) { return .04*x + .02*y; });
        auto state = makeState(mesh);
        SolverOptions2D opts;
        opts.momentum = mode;
        opts.cell_closure = CellClosure2D::VFR;
        opts.face_reconstruction = FaceDepth2D::VFR_FACE;
        opts.dry_depth = opts.h_move = 1e-4;
        opts.lts_tiers = 4;
        seedSurface(mesh, opts, state, .15);
        auto reference = state;
        ExplicitInertialSolver solver, control;
        solver.initialize(mesh, state, opts);
        control.initialize(mesh, reference, opts);
        for (int step = 0; step < 50; ++step) {
            solver.advance(step * .02, (step + 1) * .02);
            control.advance(step * .02, (step + 1) * .02);
            if (mode == Momentum2D::FULL_SWE) solver.publishMomentumVelocity();
            else computeFaceVelocity(mesh, state, opts);
            EXPECT_EQ(state.depth, reference.depth);
            EXPECT_EQ(state.head, reference.head);
            EXPECT_EQ(state.volume, reference.volume);
            EXPECT_EQ(state.edge_flux, reference.edge_flux);
        }
      }
}

TEST(MomentumModes, FullSwePublishesNativeVelocityWithoutOptionalPhysics) {
    for (bool quads : {false, true})
      for (double initial_u : {.3, 12.}) {
        auto mesh = quads ? makeQuadGrid(2, 2, 1., [](double, double) { return 0.; })
                         : makeTriGrid(2, 2, 1., [](double, double) { return 0.; });
        auto state = makeState(mesh);
        SolverOptions2D opts;
        opts.momentum = Momentum2D::FULL_SWE;
        opts.num_threads = 2;
        for (int i = 0; i < mesh.n_cells(); ++i) {
            state.depth[i] = state.head[i] = .01;
            state.volume[i] = .01 * mesh.tri_area[i];
            mesh.tri_init_u[i] = initial_u;
            mesh.tri_init_v[i] = -.4;
        }
        state.volume[0] = 0.;
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        ASSERT_FALSE(opts.momentumPhysics());
        // No published edge flux at initialization: RT0 cannot recover the
        // authored momentum. The output must still show its native speed.
        computeFaceVelocity(mesh, state, opts);
        EXPECT_DOUBLE_EQ(state.face_vx[1], 0.);
        solver.publishMomentumVelocity();
        EXPECT_DOUBLE_EQ(state.face_vx[0], 0.);
        EXPECT_DOUBLE_EQ(state.face_vy[0], 0.);
        for (int i = 1; i < mesh.n_cells(); ++i) {
            EXPECT_NEAR(state.face_vx[i], -initial_u, 1e-14);
            EXPECT_NEAR(state.face_vy[i], .4, 1e-14);
        }
        const auto volume = state.volume;
        const auto flux = state.edge_flux;
        solver.publishMomentumVelocity();
        EXPECT_EQ(state.volume, volume);
        EXPECT_EQ(state.edge_flux, flux);
    }
}

TEST(MomentumModes, FullSweWettingFrontDoesNotReportRiemannFluxAsCellMomentum) {
    auto mesh = makeTriGrid(24, 3, .5, [](double, double) { return 0.; });
    auto state = makeState(mesh);
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    opts.cfl_number = .5;
    opts.lts_tiers = 1;
    opts.dry_depth = opts.h_move = 1e-4;
    for (int i = 0; i < mesh.n_cells(); ++i)
        state.volume[i] = mesh.tri_cx[i] < 4. ? .06 * mesh.tri_area[i] : 0.;
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0., .02);
    computeFaceVelocity(mesh, state, opts);
    constexpr int front = 17;
    ASSERT_GT(state.depth[front], opts.dry_depth);
    ASSERT_LT(state.depth[front], .003);
    // A reconstructed edge velocity is a diagnostic, not conserved momentum.
    // Native SWE publication must still replace it without changing hydraulics.
    const auto volume = state.volume;
    const auto flux = state.edge_flux;
    solver.publishMomentumVelocity();
    EXPECT_GT(std::hypot(state.face_vx[front], state.face_vy[front]), .1);
    EXPECT_LT(std::hypot(state.face_vx[front], state.face_vy[front]), 1.);
    EXPECT_EQ(state.volume, volume);
    EXPECT_EQ(state.edge_flux, flux);
}

TEST(MomentumModes, RouterPublishesNativeInitialAndRefreshedVelocity) {
    SurfaceRouter2D router;
    router.mesh() = makeTriGrid(2, 2, 1., [](double, double) { return 0.; });
    auto& mesh = router.mesh();
    std::fill(mesh.tri_init_depth.begin(), mesh.tri_init_depth.end(), .01);
    std::fill(mesh.tri_init_u.begin(), mesh.tri_init_u.end(), .3);
    std::fill(mesh.tri_init_v.begin(), mesh.tri_init_v.end(), -.4);
    router.options().momentum = Momentum2D::FULL_SWE;
    router.options().backend = Backend2D::CPU;
    openswmm::SimulationContext ctx;
    ctx.options.flow_units = openswmm::FlowUnits::CMS;
    router.initialize(ctx);
    ASSERT_TRUE(router.isActive());
    for (int i = 0; i < mesh.n_cells(); ++i) {
        EXPECT_NEAR(router.state().face_vx[i], -.3, 1e-14);
        EXPECT_NEAR(router.state().face_vy[i], .4, 1e-14);
    }
    router.step(ctx, .001, 0.);
    router.refreshRenderFieldsIfStale();
    auto reconstructed = router.state();
    computeFaceVelocity(mesh, reconstructed, router.options());
    double difference = 0.;
    for (int i = 0; i < mesh.n_cells(); ++i)
        difference += std::hypot(reconstructed.face_vx[i] - router.state().face_vx[i],
                                  reconstructed.face_vy[i] - router.state().face_vy[i]);
    EXPECT_GT(difference, .01);
    router.finalize(ctx);
}

TEST(MomentumModes, BulkVelocityApiMatchesPhysicalOutputAndRefreshesWithoutAdvancing) {
    openswmm::SWMMEngine engine;
    auto handle = reinterpret_cast<SWMM_Engine>(&engine);
    auto& router = engine.surfaceRouter2D();
    router.mesh() = makeTriGrid(2, 2, 1., [](double, double) { return 0.; });
    auto& mesh = router.mesh();
    std::fill(mesh.tri_init_depth.begin(), mesh.tri_init_depth.end(), .01);
    std::fill(mesh.tri_init_u.begin(), mesh.tri_init_u.end(), .3);
    std::fill(mesh.tri_init_v.begin(), mesh.tri_init_v.end(), -.4);
    mesh.tri_init_depth[0] = 0.;
    router.options().momentum = Momentum2D::FULL_SWE;
    router.options().backend = Backend2D::CPU;
    engine.context().options.flow_units = openswmm::FlowUnits::CMS;
    std::vector<double> vx(mesh.n_cells()), vy(mesh.n_cells());
    EXPECT_EQ(swmm_2d_get_velocities_bulk(nullptr, vx.data(), vy.data()), SWMM_ERR_BADHANDLE);
    EXPECT_EQ(swmm_2d_get_velocities_bulk(handle, vx.data(), vy.data()), SWMM_ERR_LIFECYCLE);
    router.initialize(engine.context());
    EXPECT_EQ(swmm_2d_get_velocities_bulk(handle, nullptr, vy.data()), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_get_velocities_bulk(handle, vx.data(), nullptr), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_get_velocities_bulk(handle, vx.data(), vx.data()), SWMM_ERR_BADPARAM);
    ASSERT_EQ(swmm_2d_get_velocities_bulk(handle, vx.data(), vy.data()), SWMM_OK);
    EXPECT_DOUBLE_EQ(vx[0], 0.);
    EXPECT_DOUBLE_EQ(vy[0], 0.);
    for (int i = 1; i < mesh.n_cells(); ++i) {
        EXPECT_NEAR(vx[i], .3, 1e-14);
        EXPECT_NEAR(vy[i], -.4, 1e-14);
    }
    router.step(engine.context(), .001, 0.);
    const auto volume = router.state().volume, head = router.state().head;
    const auto flux = router.state().edge_flux;
    ASSERT_EQ(swmm_2d_get_velocities_bulk(handle, vx.data(), vy.data()), SWMM_OK);
    for (int i = 0; i < mesh.n_cells(); ++i) {
        EXPECT_DOUBLE_EQ(vx[i], -router.state().face_vx[i]);
        EXPECT_DOUBLE_EQ(vy[i], -router.state().face_vy[i]);
    }
    EXPECT_EQ(volume, router.state().volume);
    EXPECT_EQ(head, router.state().head);
    EXPECT_EQ(flux, router.state().edge_flux);
    router.finalize(engine.context());
}

TEST(MomentumModes, NativeVelocityPublicationLeavesOtherClosuresUntouched) {
    for (auto mode : {Momentum2D::LOCAL_INERTIAL, Momentum2D::DIFFUSIVE_WAVE}) {
        auto mesh = makeTriGrid(2, 2, 1., [](double, double) { return 0.; });
        auto state = makeState(mesh);
        SolverOptions2D opts;
        opts.momentum = mode;
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        std::fill(state.face_vx.begin(), state.face_vx.end(), 7.);
        std::fill(state.face_vy.begin(), state.face_vy.end(), -8.);
        solver.publishMomentumVelocity();
        EXPECT_EQ(state.face_vx, std::vector<double>(mesh.n_cells(), 7.));
        EXPECT_EQ(state.face_vy, std::vector<double>(mesh.n_cells(), -8.));
    }
}

TEST(MomentumModes, LakeAtRestExactAllShapesAndClosures) {
    for (const auto mode : {Momentum2D::FULL_SWE, Momentum2D::DIFFUSIVE_WAVE}) {
        for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
            for (int kind = 0; kind < 3; ++kind) {
                auto bed = [](double x, double y) {
                    return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
                };
                MeshData mesh = (kind == 0)   ? makeTriGrid(6, 6, 5.0, bed)
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
                for (double f : state.edge_flux)
                    EXPECT_LE(std::fabs(f), 1.0e-9);
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
            if (mesh.tri_cz[i] > 1.5)
                EXPECT_EQ(state.volume[i], 0.0) << "mode " << int(mode);
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
                inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i],
                                       state.depth[i]);
            }
        const double sum0 = totalVolume(state, mesh.n_triangles());
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        const int chunks = (mode == Momentum2D::FULL_SWE) ? 40 : 6; // DW is Δx²-bound
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
    const int nx = 200, ny = 2;
    const double dx = 0.05;
    auto mesh = makeTriGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = 1e-9; // frictionless
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.dry_depth = 1e-4;
    opts.h_move = 1e-4;
    opts.cfl_number = 0.5;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double h = (mesh.tri_cx[i] < 5.0) ? 0.005 : 0.001;
        state.volume[i] = h * mesh.tri_area[i];
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int c = 0; c < 6; ++c)
        solver.advance(c * 1.0, (c + 1) * 1.0);
    const double l1 = stripL1(mesh, state, nx, dx,
                              [](double x) { return stokerDepth(x, 6.0, 0.005, 0.001, 5.0); });
    EXPECT_LT(l1, 0.03) << "Stoker L1 " << l1;
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1e-10 * sum0);
    // The bore is where the exact solution puts it (within 2 cells).
    double hm, um, s;
    stokerMiddle(0.005, 0.001, hm, um, s);
    double x_bore = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (state.depth[i] > 0.5 * (hm + 0.001))
            x_bore = std::max(x_bore, mesh.tri_cx[i]);
    EXPECT_NEAR(x_bore, 5.0 + s * 6.0, 2.5 * dx);
    solver.finalize();
}

TEST(MomentumModes, RitterDryDamBreakFullSwe) {
    const int nx = 200, ny = 2;
    const double dx = 0.05;
    auto mesh = makeTriGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = 1e-9;
    SolverOptions2D opts;
    opts.momentum = Momentum2D::FULL_SWE;
    opts.dry_depth = 1e-4;
    opts.h_move = 1e-4;
    opts.cfl_number = 0.5;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] >= 5.0)
            continue;
        state.volume[i] = 0.005 * mesh.tri_area[i];
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int c = 0; c < 6; ++c) {
        solver.advance(c * 1.0, (c + 1) * 1.0);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            ASSERT_GE(state.volume[i], 0.0);
    }
    const double l1 =
        stripL1(mesh, state, nx, dx, [](double x) { return ritterDepth(x, 6.0, 0.005, 5.0); });
    EXPECT_LT(l1, 0.04) << "Ritter L1 " << l1;
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1e-10 * sum0);
    // The front advanced past 6.5 m (ideal toe 7.66 m; a first-order scheme
    // lags the infinitely thin toe, but not the water-carrying part).
    double x_front = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (state.depth[i] > 2e-4)
            x_front = std::max(x_front, mesh.tri_cx[i]);
    EXPECT_GT(x_front, 6.5);
    solver.finalize();
}

// RECONSTRUCTION_ORDER 2 (MUSCL + SSP-RK2, global dt): the Stoker rarefaction
// and bore sharpen — L1 below 1 % where first order sits near 0.7–1 %; the
// rest state stays exact and mass is conserved through the two-stage update.
TEST(MomentumModes, StokerSecondOrderSharper) {
    const int nx = 200, ny = 2;
    const double dx = 0.05;
    double l1[2] = {0.0, 0.0};
    for (int order = 1; order <= 2; ++order) {
        auto mesh = makeTriGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.0);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            mesh.mannings_n[i] = 1e-9;
        SolverOptions2D opts;
        opts.momentum = Momentum2D::FULL_SWE;
        opts.reconstruction_order = order;
        opts.dry_depth = 1e-4;
        opts.h_move = 1e-4;
        opts.cfl_number = 0.5;
        auto state = makeState(mesh);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const double h = (mesh.tri_cx[i] < 5.0) ? 0.005 : 0.001;
            state.volume[i] = h * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
        }
        const double sum0 = totalVolume(state, mesh.n_triangles());
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        if (order == 2)
            EXPECT_EQ(opts.lts_tiers, 1);
        for (int c = 0; c < 6; ++c)
            solver.advance(c * 1.0, (c + 1) * 1.0);
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
                if (mesh.cell_neighbour(i, e) >= 0)
                    continue;
                if (mesh.edge_mx[idx] < 1.0e-9) {
                    boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                    boundary.edge_bed_slope[idx] = S;
                }
            }
        state.boundary = &boundary;
        const double rain = 2.0e-4;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            state.rainfall[i] = rain;
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
        for (int i = 0; i < mesh.n_triangles(); ++i)
            q_rain += rain * mesh.tri_area[i];
        EXPECT_NEAR(q_rain * T, q_out * T + (v_end - v_start), 1.0e-8 * q_rain * T) << int(mode);
        EXPECT_NEAR(q_out, q_rain, 0.10 * q_rain) << int(mode);
        const double L = nx * dx, x_mid = 0.5 * L;
        const double h_n = std::pow(n_man * rain * (L - x_mid) / std::sqrt(S), 3.0 / 5.0);
        double h_avg = 0.0;
        int cnt = 0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            if (std::fabs(mesh.tri_cx[i] - x_mid) < dx) {
                h_avg += state.depth[i];
                ++cnt;
            }
        h_avg /= cnt;
        EXPECT_NEAR(h_avg, h_n, 0.05 * h_n) << "mode " << int(mode);
        solver.finalize();
    }
}

// A prescribed-discharge boundary must deliver EXACTLY what it prescribes.
// The ghost-cell Riemann boundary used to deliver a wave-speed-weighted blend
// of the interior and prescribed fluxes instead: measured 7-21 % short on the
// SWASHES bump inlets and reversed outright (+143 m³/s against a prescribed
// 10) on a supercritical inlet. Subcritical inflow, supercritical inflow and
// a dry-bed inflow are all checked, against the volume the domain gained.
TEST(MomentumModes, SpecifiedFlowInflowIsExactFullSwe) {
    struct Case {
        const char* name;
        double q_in;
        double h0;
    };
    // q_in is per metre of edge (m²/s), inward. h0 = 0 starts the strip dry.
    const Case cases[] = {
        {"subcritical", 0.40, 0.60},   // Fr = q/(h sqrt(g h)) ~ 0.28
        {"supercritical", 2.00, 0.30}, // Fr ~ 3.9 at the inlet cell
        {"dry bed", 0.50, 0.00},
    };
    for (const auto& c : cases) {
        const double dx = 1.0;
        const int nx = 30, ny = 2;
        auto mesh = makeQuadGrid(nx, ny, dx, [](double, double) { return 0.0; }, 0.02);
        SolverOptions2D opts;
        opts.momentum = Momentum2D::FULL_SWE;
        auto state = makeState(mesh);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            state.depth[i] = c.h0;
            state.head[i] = c.h0;
            state.volume[i] = c.h0 * mesh.tri_area[i];
        }
        BoundaryData boundary;
        boundary.resize(mesh.n_edge_slots());
        double inlet_len = 0.0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
                const int idx = MeshData::slot(i, e);
                if (mesh.cell_neighbour(i, e) >= 0)
                    continue;
                // edge_mx / edge_my are the edge MIDPOINT: the inlet is the
                // west face at x = 0 (the side walls sit at x >= dx/2).
                if (mesh.edge_mx[idx] > 1.0e-9)
                    continue;
                boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::SPECIFIED_FLOW);
                boundary.edge_bc_flow[idx] = -c.q_in; // outward +, so inflow < 0
                inlet_len += mesh.edge_length[idx];
            }
        ASSERT_GT(inlet_len, 0.0) << c.name;
        state.boundary = &boundary;
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        // Short window, and short enough that the front cannot reach the far
        // (walled) end — every drop that entered is still in the domain.
        const double T = 4.0;
        const double v0 = totalVolume(state, mesh.n_triangles());
        solver.advance(0.0, T);
        const double v1 = totalVolume(state, mesh.n_triangles());
        const double expected = c.q_in * inlet_len * T;
        EXPECT_NEAR(v1 - v0, expected, 1.0e-3 * expected)
            << c.name << ": inflow boundary delivered " << (v1 - v0) << " m³ against a prescribed "
            << expected;
        solver.finalize();
    }
}

// NORMAL_FLOW under FULL_SWE is the Manning law the local-inertial and
// diffusive paths apply, not a zero-gradient outlet: doubling the edge's bed
// slope must raise the outflow by sqrt(2), and a zero slope must hold water.
TEST(MomentumModes, NormalFlowOutletUsesManningFullSwe) {
    auto run = [](double S) {
        const double dx = 1.0, n_man = 0.03, h0 = 0.4;
        const int nx = 20, ny = 2;
        auto mesh = makeQuadGrid(nx, ny, dx, [](double, double) { return 0.0; }, n_man);
        SolverOptions2D opts;
        opts.momentum = Momentum2D::FULL_SWE;
        auto state = makeState(mesh);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            state.depth[i] = h0;
            state.head[i] = h0;
            state.volume[i] = h0 * mesh.tri_area[i];
        }
        BoundaryData boundary;
        boundary.resize(mesh.n_edge_slots());
        for (int i = 0; i < mesh.n_triangles(); ++i)
            for (int e = 0; e < mesh.cell_vertex_count(i); ++e) {
                const int idx = MeshData::slot(i, e);
                if (mesh.cell_neighbour(i, e) >= 0)
                    continue;
                // Outlet = the east face at x = nx·dx (midpoint coordinate).
                if (mesh.edge_mx[idx] < nx * dx - 1.0e-9)
                    continue;
                boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                boundary.edge_bed_slope[idx] = S;
            }
        state.boundary = &boundary;
        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        const double v0 = totalVolume(state, mesh.n_triangles());
        solver.advance(0.0, 0.5);
        const double v1 = totalVolume(state, mesh.n_triangles());
        solver.finalize();
        return v0 - v1; // volume that left
    };
    // An inert outlet (no slope) conveys nothing — the local-inertial law's
    // contract. A zero-gradient ghost would make it absorbing and drain the
    // pond; that was the FULL_SWE behaviour before this law was applied.
    EXPECT_NEAR(run(0.0), 0.0, 1.0e-9) << "a zero-slope normal-flow outlet must not drain";
    const double out1 = run(0.0025), out2 = run(0.01);
    EXPECT_GT(out1, 0.0);
    EXPECT_GT(out2, out1) << "more slope must convey more";
    // Against the Manning prediction at the initial depth over the 2 m outlet
    // (the outlet cells draw down over the window, so the integral lands
    // under it — the gate is that the LAW is applied, not the exact integral).
    const double h0 = 0.4, n_man = 0.03, T = 0.5, outlet = 2.0;
    const double q_pred = std::pow(h0, 5.0 / 3.0) * std::sqrt(0.0025) / n_man;
    EXPECT_GT(out1, 0.35 * q_pred * outlet * T) << q_pred;
    EXPECT_LT(out1, 1.05 * q_pred * outlet * T) << q_pred;
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
    EXPECT_EQ(opts.front_rebuild, -1); // AUTO untouched
}
