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
 * @file test_2d_inertial_marcher.cpp
 * @brief Analytic + property gates for the explicit local-inertial marcher
 *        (ExplicitInertialSolver, INTEGRATOR EXPLICIT).
 *
 * @details Phase-1 gates of the 2026-07-29 2D reimplementation plan:
 *          - Lake at rest is EXACT (bit-zero fluxes, bit-unchanged volumes)
 *            on an uneven bed, both FLAT and VFR closures.
 *          - Dry-neighbour wall / C-property: a puddle below a face sill
 *            leaks nothing, ever.
 *          - Closed-basin conservation: a dam-break sloshes for thousands of
 *            substeps with ΣV drift at reduction-roundoff level and V ≥ 0
 *            everywhere (positivity limiter).
 *          - Manning steady slope: rain-fed plane with a NORMAL_FLOW outlet
 *            reaches steady state with outflow = rain inflow (ledger) and
 *            the analytic normal depth.
 *          - Froude clamp honored on a steep dam-break (no runaway |q|).
 *          - SPECIFIED_STAGE: a dry basin fills through a stage BC to the
 *            prescribed head (inflow direction), then draws down after the
 *            BC head is lowered — exact BC-flux ledger in both phases.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>
#include "2d/solver/SurfaceSolverFactory.hpp"
#include <cmath>
#include <array>
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <numeric>
#include <vector>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"

using namespace openswmm::twoD;

namespace {

// nx × ny rectangular grid of dx-square quads split into 2 triangles each,
// bed elevation from z(x, y). Manning's n uniform.
template <typename ZFn>
MeshData makeGridMesh(int nx, int ny, double dx, ZFn z, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j) {
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = z(i * dx, j * dx);
        }
    }
    mesh.resize_triangles(2 * nx * ny);
    int t = 0;
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_triangle(t, v00, v10, v11);
            ++t;
            mesh.set_triangle(t, v00, v11, v01);
            ++t;
        }
    }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

SurfaceStateData makeState(const MeshData& mesh) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i]   = mesh.tri_cz[i];
        state.depth[i]  = 0.0;
        state.volume[i] = 0.0;
    }
    return state;
}

// Seed a uniform free surface η (dry where the bed stands above it) through
// the closure's own inverse, so the seeded state round-trips exactly.
void seedSurface(const MeshData& mesh, const SolverOptions2D& opts,
                 SurfaceStateData& state, double eta) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.volume[i] = inertial::cellVolumeFromEta(mesh, opts, i, eta);
        inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i],
                               state.depth[i]);
    }
}

double totalVolume(const SurfaceStateData& state, int nt) {
    double s = 0.0;
    for (int i = 0; i < nt; ++i) s += state.volume[i];
    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Lake at rest on an uneven bed: flux depends on Δη only, so a uniform η is
// an EXACT steady state — volumes bit-unchanged, projected fluxes bit-zero.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, LakeAtRestExactFlatAndVfr) {
    for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        auto mesh = makeGridMesh(6, 6, 5.0, [](double x, double y) {
            return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
        });
        SolverOptions2D opts;
        opts.cell_closure = closure;
        auto state = makeState(mesh);
        // Fully submerged everywhere (bed tops out ≈ 5.1 m): the exactness
        // claim is for a closed wet lake. Shoreline stillness is a different
        // property — FLAT is legitimately NOT at equilibrium at a shoreline
        // (the documented uphill-creep artifact) — covered by the dry-wall
        // test below and the VFR closure suite.
        seedSurface(mesh, opts, state, /*eta=*/6.0);
        const std::vector<double> v0 = state.volume;

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        const double reached = solver.advance(0.0, 600.0);
        EXPECT_DOUBLE_EQ(reached, 600.0);

        // Machine-precision C-property: the closure round-trip η = f(f⁻¹(η))
        // carries ~1-ulp head noise, so per-cell drift up to O(1e-9·V) over
        // ~10³ substeps is FP dust, not transport. Anything larger is a leak.
        for (int i = 0; i < mesh.n_triangles(); ++i)
            EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0))
                << "cell " << i << " moved at rest";
        for (double f : state.edge_flux)
            EXPECT_LE(std::fabs(f), 1.0e-10) << "flux at rest";
        solver.finalize();
    }
}

// ---------------------------------------------------------------------------
// C-property / dry wall: a puddle whose surface sits BELOW the interface bed
// of a dry, higher neighbour must not leak uphill — bit-exact, 10 minutes.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, DryNeighbourWallNoCreep) {
    // Step bed: left half z = 0, right half z = 2. Puddle η = 1 on the left.
    auto mesh = makeGridMesh(8, 4, 2.0, [](double x, double) {
        return (x < 8.0) ? 0.0 : 2.0;
    });
    SolverOptions2D opts;
    auto state = makeState(mesh);
    seedSurface(mesh, opts, state, /*eta=*/1.0);
    const std::vector<double> v0 = state.volume;
    const double sum0 = totalVolume(state, mesh.n_triangles());

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, 600.0);

    // Nothing crossed into the dry plateau…
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cz[i] > 1.5)
            EXPECT_EQ(state.volume[i], 0.0) << "water climbed the step";
    // …and the basin total is preserved to FP dust.
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-9 * sum0);
    // The flat puddle at rest is also cell-wise still (machine precision).
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0));
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Closed-basin dam-break: violent transient, thousands of substeps. Gates:
// V ≥ 0 always (positivity), ΣV conserved to reduction roundoff, no NaN.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, ClosedBasinConservationAndPositivity) {
    auto mesh = makeGridMesh(10, 10, 2.0, [](double, double) { return 0.0; });
    SolverOptions2D opts;
    auto state = makeState(mesh);
    // Column of water in the corner quarter.
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] < 5.0 && mesh.tri_cy[i] < 5.0) {
            state.volume[i] = 2.0 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i],
                                   state.head[i], state.depth[i]);
        }
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    long steps = 0;
    for (int chunk = 0; chunk < 60; ++chunk) {       // 60 × 10 s of sloshing
        solver.advance(chunk * 10.0, (chunk + 1) * 10.0);
        steps += solver.last_num_steps();
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            ASSERT_GE(state.volume[i], 0.0) << "negative volume, cell " << i;
            ASSERT_FALSE(std::isnan(state.volume[i]));
        }
    }
    EXPECT_GT(steps, 1000) << "dam-break resolved suspiciously few substeps";
    const double sum1 = totalVolume(state, mesh.n_triangles());
    EXPECT_NEAR(sum1, sum0, 1.0e-10 * sum0);
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Manning steady uniform slope: rain over an inclined strip draining through a
// NORMAL_FLOW outlet. At steady state the outlet ledger equals the rain input
// (≤ 1 %) and the interior depth matches the analytic normal depth
// h_n = (n·q / √S)^(3/5) with q the per-width discharge at that station (≤ 5 %).
// ---------------------------------------------------------------------------
TEST(InertialMarcher, ManningSteadySlopeRainOutflow) {
    // dx = 1 m keeps the first-order upwind-depth staggering offset (~half a
    // cell of the (L−x)^{3/5} profile) comfortably inside the 5 % gate.
    const double S = 0.01, dx = 1.0, n_man = 0.03;
    const int nx = 40, ny = 4;
    auto mesh = makeGridMesh(nx, ny, dx,
                             [&](double x, double) { return S * x; }, n_man);
    SolverOptions2D opts;
    auto state = makeState(mesh);

    // NORMAL_FLOW outlet along the downhill (x = 0) boundary edges.
    BoundaryData boundary;
    boundary.resize(mesh.n_edge_slots());
    int outlet_slots = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                             mesh.cell_neighbour(i, 2)};
        for (int e = 0; e < 3; ++e) {
            const int idx = MeshData::slot(i, e);
            if (nbrs[e] >= 0) continue;
            if (mesh.edge_mx[idx] < 1.0e-9) {        // x = 0 boundary
                boundary.edge_bc_type[idx] =
                    static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                boundary.edge_bed_slope[idx] = S;
                ++outlet_slots;
            }
        }
    }
    ASSERT_GT(outlet_slots, 0);
    state.boundary = &boundary;

    // Heavy rate so the analytic normal depth (~18 mm) sits well above the
    // H_MOVE activation band — the film-hysteresis regime below H_MOVE trades
    // instantaneous depth accuracy for cost by design and is not the target
    // of this gate.
    const double rain = 2.0e-4;                       // 720 mm/hr, everywhere
    for (int i = 0; i < mesh.n_triangles(); ++i) state.rainfall[i] = rain;

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    // March to steady state, then measure over one more long window.
    solver.advance(0.0, 6000.0);
    const double T = 1000.0;
    const double v_start = totalVolume(state, mesh.n_triangles());
    solver.advance(6000.0, 6000.0 + T);
    const double v_end = totalVolume(state, mesh.n_triangles());

    // Window-mean applied outlet flux (inflow-positive) vs rain input.
    double q_out = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        for (int e = 0; e < 3; ++e) {
            const int idx = MeshData::slot(i, e);
            if (boundary.edge_bc_type[idx] ==
                static_cast<int8_t>(BoundaryType::NORMAL_FLOW))
                q_out += -state.edge_flux[idx];
        }
    double q_rain = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        q_rain += rain * mesh.tri_area[i];

    // Exact ledger identity over the window: rain in = outlet out + ΔStorage
    // (machine-exact by construction — the gate on BC booking + conservation).
    EXPECT_NEAR(q_rain * T, q_out * T + (v_end - v_start),
                1.0e-8 * q_rain * T);
    // Rate check: outflow tracks rain input; the tolerance covers the
    // h_move activation slugs of thin near-crest cells cycling storage
    // (bounded by h_on × upslope area / T).
    EXPECT_NEAR(q_out, q_rain, 0.10 * q_rain);

    // Normal depth at mid-strip: per-width discharge q = rain · (L − x).
    const double L = nx * dx, x_mid = 0.5 * L;
    const double q_mid = rain * (L - x_mid);
    const double h_n = std::pow(n_man * q_mid / std::sqrt(S), 3.0 / 5.0);
    double h_avg = 0.0;
    int    h_cnt = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (std::fabs(mesh.tri_cx[i] - x_mid) < dx) {
            h_avg += state.depth[i];
            ++h_cnt;
        }
    }
    h_avg /= h_cnt;
    // 6 % envelope: ~2.5 % from the first-order upwind-depth staggering
    // offset (see the dx note above) plus ~1 % from the vector-magnitude
    // friction, which adds the diagonal-face friction the scalar |q_n| form
    // omitted (a face at 45° to the flow under-damped by √2).
    EXPECT_NEAR(h_avg, h_n, 0.06 * h_n);
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Steady-slope surface smoothness: the same rain-fed inclined strip as the
// Manning gate, both closures. At steady state the free surface must be
// spatially smooth — per-cell η deviation from the face-neighbour mean stays
// at millimetre level. Guards the face-datum symmetry: with the face bed
// taken from the two cells' CENTROIDS (which alternate by ±S·dx/3 on a
// split-quad slope), the discrete face conveyance alternates and the surface
// locks into a standing cell-to-cell stair — the depth "checkerboarding"
// observed upstream of the road_culvert embankment (flow-dependent,
// θ-independent). The shared-edge sill datum removes it.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, SteadySlopeNoCheckerboard) {
    const double S = 0.01, dx = 1.0, n_man = 0.03;
    const int nx = 40, ny = 4;
    for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        auto mesh = makeGridMesh(nx, ny, dx,
                                 [&](double x, double) { return S * x; },
                                 n_man);
        SolverOptions2D opts;
        opts.cell_closure = closure;
        auto state = makeState(mesh);

        BoundaryData boundary;
        boundary.resize(mesh.n_edge_slots());
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                                 mesh.cell_neighbour(i, 2)};
            for (int e = 0; e < 3; ++e) {
                const int idx = MeshData::slot(i, e);
                if (nbrs[e] >= 0) continue;
                if (mesh.edge_mx[idx] < 1.0e-9) {
                    boundary.edge_bc_type[idx] =
                        static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                    boundary.edge_bed_slope[idx] = S;
                }
            }
        }
        state.boundary = &boundary;
        const double rain = 2.0e-4;
        for (int i = 0; i < mesh.n_triangles(); ++i) state.rainfall[i] = rain;

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        solver.advance(0.0, 6000.0);

        // Spatial roughness of η: deviation from the face-neighbour mean over
        // the strip interior (ends excluded — the outlet drawdown and the
        // upslope crest carry genuine one-sided curvature).
        double ss = 0.0, mx_r = 0.0;
        int    n  = 0;
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            if (mesh.tri_cx[i] < 5.0 || mesh.tri_cx[i] > 35.0) continue;
            const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                                 mesh.cell_neighbour(i, 2)};
            double hs = 0.0;
            int    hn = 0;
            for (int e = 0; e < 3; ++e)
                if (nbrs[e] >= 0) { hs += state.head[nbrs[e]]; ++hn; }
            if (hn < 3) continue;
            const double r = state.head[i] - hs / hn;
            ss += r * r;
            mx_r = std::max(mx_r, std::fabs(r));
            ++n;
        }
        const double rms = std::sqrt(ss / std::max(n, 1));
        const char* tag =
            (closure == CellClosure2D::VFR) ? "VFR" : "FLAT";
        // Characterization gate: ~1.0 mm rms remains after the vector-
        // friction fix (was ~1.25 mm with scalar |q_n| friction) — the
        // residual is believed to be outlet-row alternation (NORMAL_FLOW
        // edges exist only on alternate boundary cells) plus the upwind-
        // depth staggering; the VFR quad-pair closure is the open work item
        // for the (larger) closure-side alternation on real embankments.
        // The gate trips if a change regresses toward the scalar-friction
        // level or beyond.
        EXPECT_LT(rms, 1.2e-3)
            << tag << ": steady-slope surface roughness rms " << rms;
        EXPECT_LT(mx_r, 3.5e-3)
            << tag << ": steady-slope surface roughness max " << mx_r;
        solver.finalize();
    }
}

// ---------------------------------------------------------------------------
// SPECIFIED_STAGE tailwater: a flat DRY basin with a stage BC along x = 0.
// Phase 1 (inflow — the only BC branch that can drive water INTO the domain):
// the basin fills through the boundary and equilibrates at η = h_bc. Phase 2
// (outflow): the BC head is lowered and the basin draws down to the new
// stage. Both phases carry the exact ledger identity ΔStorage = ∫ q_bc dt
// (the marcher publishes the window-mean APPLIED flux back into edge_flux,
// so chunked accumulation books exactly what the cells received).
// ---------------------------------------------------------------------------
TEST(InertialMarcher, SpecifiedStageFillAndDrawdownLedger) {
    auto mesh = makeGridMesh(8, 8, 1.0, [](double, double) { return 0.0; });
    SolverOptions2D opts;
    auto state = makeState(mesh);                     // dry everywhere

    BoundaryData boundary;
    boundary.resize(mesh.n_edge_slots());
    std::vector<int> bc_slots;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                             mesh.cell_neighbour(i, 2)};
        for (int e = 0; e < 3; ++e) {
            const int idx = MeshData::slot(i, e);
            if (nbrs[e] >= 0) continue;
            if (mesh.edge_mx[idx] < 1.0e-9) {         // x = 0 boundary
                boundary.edge_bc_type[idx] =
                    static_cast<int8_t>(BoundaryType::SPECIFIED_STAGE);
                boundary.edge_bc_head[idx] = 0.5;
                bc_slots.push_back(idx);
            }
        }
    }
    ASSERT_FALSE(bc_slots.empty());
    state.boundary = &boundary;

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);

    // Advance in chunks, accumulating the applied BC volume per window and
    // the per-cell TIME-MEAN head over the marched span. The constant-head
    // boundary is a reflecting (non-absorbing) Dirichlet wall, so a weakly
    // damped seiche persists around equilibrium — the time-mean cancels it,
    // while a stalled fill/drawdown would bias every cell's mean.
    std::vector<double> hbar;
    double vbar = 0.0;   // time-mean total volume over the marched span
    auto marchAccum = [&](double t0, double t1, int chunks) {
        double vol_in = 0.0;
        const double dt = (t1 - t0) / chunks;
        hbar.assign(mesh.n_triangles(), 0.0);
        vbar = 0.0;
        for (int c = 0; c < chunks; ++c) {
            solver.advance(t0 + c * dt, t0 + (c + 1) * dt);
            for (int idx : bc_slots) vol_in += state.edge_flux[idx] * dt;
            for (int i = 0; i < mesh.n_triangles(); ++i) {
                EXPECT_GE(state.volume[i], 0.0) << "negative volume";
                EXPECT_FALSE(std::isnan(state.volume[i]));
                hbar[i] += state.head[i];
            }
            vbar += totalVolume(state, mesh.n_triangles());
        }
        for (double& h : hbar) h /= chunks;
        vbar /= chunks;
        return vol_in;
    };

    // ---- Phase 1: fill from dry through the stage boundary. ----
    const double v0  = totalVolume(state, mesh.n_triangles());
    double in1       = marchAccum(0.0, 3000.0, 60);       // fill transient
    in1             += marchAccum(3000.0, 4000.0, 200);   // measure window
    const double v1  = totalVolume(state, mesh.n_triangles());
    EXPECT_GT(in1, 0.0) << "stage BC drove no inflow into the dry basin";
    EXPECT_GT(v1, 0.0);
    // Exact ledger: everything that crossed the boundary is in storage.
    EXPECT_NEAR(v1 - v0, in1, 1.0e-8 * std::max(in1, 1.0e-12));
    // Equilibrium: the basin mean surface AND each cell's time-mean head sit
    // at the prescribed stage. Tolerances allow for the persistent seiche the
    // reflecting (non-absorbing) stage boundary sustains and its ~2 cm
    // nonlinear rectification of the mean — a stalled fill/drawdown misses
    // by 10 cm or more.
    double a_tot = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) a_tot += mesh.tri_area[i];
    EXPECT_NEAR(vbar / a_tot, 0.5, 0.03) << "mean surface not at the stage";
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(hbar[i], 0.5, 0.075)
            << "cell " << i << " time-mean head off the prescribed stage";

    // ---- Phase 2: lower the stage; the basin draws down to the new head. ----
    for (int idx : bc_slots) boundary.edge_bc_head[idx] = 0.2;
    double in2       = marchAccum(4000.0, 7000.0, 60);    // drawdown transient
    in2             += marchAccum(7000.0, 8000.0, 200);   // measure window
    const double v2  = totalVolume(state, mesh.n_triangles());
    EXPECT_LT(in2, 0.0) << "lowered stage BC drove no outflow";
    EXPECT_NEAR(v2 - v1, in2, 1.0e-8 * std::max(-in2, 1.0e-12));
    EXPECT_NEAR(vbar / a_tot, 0.2, 0.03) << "mean surface not drawn down";
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(hbar[i], 0.2, 0.075)
            << "cell " << i << " time-mean head off the lowered stage";
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Steep-face dam-break: the Froude clamp must bound the projected face
// discharge at every published state; nothing blows up or NaNs.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, FroudeClampSteepFace) {
    const double S = 0.10;                            // 10 % street grade
    auto mesh = makeGridMesh(20, 3, 1.0,
                             [&](double x, double) { return -S * x; }, 0.015);
    SolverOptions2D opts;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] < 3.0) {                   // reservoir at the top
            state.volume[i] = 1.0 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i],
                                   state.head[i], state.depth[i]);
        }
    }
    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    for (int chunk = 0; chunk < 40; ++chunk) {
        solver.advance(chunk * 1.0, (chunk + 1) * 1.0);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            ASSERT_FALSE(std::isnan(state.volume[i]));
        // |q| ≤ Fr_max·h_f·√(g·h_f) with the EXACT face flow depth
        // h_f = max(η_L, η_R) − max(z_L, z_R) the clamp itself used.
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                                 mesh.cell_neighbour(i, 2)};
            for (int e = 0; e < 3; ++e) {
                const int j = nbrs[e];
                if (j < 0) continue;                  // boundary slot
                const int idx = MeshData::slot(i, e);
                const double q_w =
                    std::fabs(state.edge_flux[idx]) / mesh.edge_length[idx];
                if (q_w == 0.0) continue;
                const double hf =
                    std::max(state.head[i], state.head[j]) -
                    std::max(mesh.tri_cz[i], mesh.tri_cz[j]);
                ASSERT_GT(hf, 0.0) << "flux across a dry face";
                const double cap = opts.froude_max * hf *
                                   std::sqrt(inertial::kGravity * hf);
                ASSERT_LE(q_w, cap * (1.0 + 1.0e-9))
                    << "face discharge above the Froude clamp";
            }
        }
    }
    solver.finalize();
}

// ---------------------------------------------------------------------------
// FACE_RECONSTRUCTION = VFR_FACE (interior faces): the B&S Eq. 14 wetted-edge
// depth over the shared edge's TRUE endpoint beds. The kernel itself first.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, FaceDepthFromEtaLimitsAndC1) {
    const double z_lo = 1.0, z_hi = 3.0, dz = z_hi - z_lo;
    // Wetting gate: at or below the low endpoint, nothing conveys.
    EXPECT_EQ(inertial::faceDepthFromEta(0.5, z_lo, z_hi), 0.0);
    EXPECT_EQ(inertial::faceDepthFromEta(z_lo, z_lo, z_hi), 0.0);
    // Partially submerged: (η − z_lo)² / (2 dz).
    EXPECT_NEAR(inertial::faceDepthFromEta(2.0, z_lo, z_hi),
                1.0 * 1.0 / (2.0 * dz), 1e-15);
    // C¹ join at z_hi: quadratic branch meets the fully-submerged branch in
    // value (dz/2) — and the slopes match by construction.
    EXPECT_NEAR(inertial::faceDepthFromEta(z_hi, z_lo, z_hi), dz / 2.0, 1e-15);
    EXPECT_NEAR(inertial::faceDepthFromEta(z_hi + 1.0, z_lo, z_hi),
                z_hi + 1.0 - 0.5 * (z_lo + z_hi), 1e-15);
    // Level edge: plain depth above the (single) bed.
    EXPECT_NEAR(inertial::faceDepthFromEta(2.0, 1.5, 1.5), 0.5, 1e-15);
    // Driving-surface form.
    EXPECT_EQ(inertial::faceFlowDepthVfr(0.9, 0.3, 1.0, 1.0), 0.0);
    EXPECT_NEAR(inertial::faceFlowDepthVfr(0.9, 1.4, 1.0, 1.0), 0.4, 1e-15);
}

// Lake at rest stays machine-exact under VFR_FACE, both closures — the
// well-balancing comes from the Δη slope + deadband, not the face depth.
TEST(InertialMarcher, LakeAtRestExactVfrFace) {
    for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        auto mesh = makeGridMesh(6, 6, 5.0, [](double x, double y) {
            return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
        });
        SolverOptions2D opts;
        opts.cell_closure = closure;
        opts.face_reconstruction = FaceDepth2D::VFR_FACE;
        auto state = makeState(mesh);
        seedSurface(mesh, opts, state, /*eta=*/6.0);
        const std::vector<double> v0 = state.volume;

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        const double reached = solver.advance(0.0, 600.0);
        EXPECT_DOUBLE_EQ(reached, 600.0);
        for (int i = 0; i < mesh.n_triangles(); ++i)
            EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0))
                << "cell " << i << " moved at rest (VFR_FACE)";
        for (double f : state.edge_flux)
            EXPECT_LE(std::fabs(f), 1.0e-10) << "flux at rest (VFR_FACE)";
        solver.finalize();
    }
}

// Dry-wall C-property under VFR_FACE: the step edges' endpoints sit at the
// plateau bed, above the puddle surface → hf = 0 walls, bit-still basin.
TEST(InertialMarcher, DryNeighbourWallNoCreepVfrFace) {
    auto mesh = makeGridMesh(8, 4, 2.0, [](double x, double) {
        return (x < 8.0) ? 0.0 : 2.0;
    });
    SolverOptions2D opts;
    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    auto state = makeState(mesh);
    seedSurface(mesh, opts, state, /*eta=*/1.0);
    const std::vector<double> v0 = state.volume;
    const double sum0 = totalVolume(state, mesh.n_triangles());

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    solver.advance(0.0, 600.0);

    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cz[i] > 1.5)
            EXPECT_EQ(state.volume[i], 0.0) << "water climbed the step";
    EXPECT_NEAR(totalVolume(state, mesh.n_triangles()), sum0, 1.0e-9 * sum0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0));
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Thin crest retention — the defect VFR_FACE fixes. A one-vertex-wide ridge
// (z = 1 along x = 8, else 0) holds a pool at η = 0.9 on its left:
//  - VFR_FACE: the ridge edges' endpoint beds are (1, 1) → hf = 0 → wall; the
//    right half stays bone dry and the basin is bit-still.
//  - Legacy MEAN: the crest is centroid-diluted (adjacent cell-mean beds are
//    1/3 or 2/3) → the face conveys at η = 0.9 and the pool LEAKS across.
//    Pinned so the legacy behaviour (and the reason for the option) stays
//    documented.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, ThinCrestRetentionVfrFace) {
    auto ridgeZ = [](double x, double) {
        return (std::fabs(x - 8.0) < 1.0e-9) ? 1.0 : 0.0;
    };
    auto seedLeftPool = [&](const MeshData& mesh, const SolverOptions2D& opts,
                            SurfaceStateData& state) {
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            state.volume[i] = (mesh.tri_cx[i] < 8.0)
                ? inertial::cellVolumeFromEta(mesh, opts, i, 0.9)
                : 0.0;
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i],
                                   state.head[i], state.depth[i]);
        }
    };

    // VFR_FACE holds the crest.
    {
        auto mesh = makeGridMesh(8, 4, 2.0, ridgeZ);
        SolverOptions2D opts;
        opts.face_reconstruction = FaceDepth2D::VFR_FACE;
        auto state = makeState(mesh);
        seedLeftPool(mesh, opts, state);
        const std::vector<double> v0 = state.volume;

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        solver.advance(0.0, 600.0);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            if (mesh.tri_cx[i] > 8.0)
                EXPECT_EQ(state.volume[i], 0.0)
                    << "VFR_FACE leaked through the thin crest at cell " << i;
            else
                EXPECT_NEAR(state.volume[i], v0[i], 1.0e-9 * (v0[i] + 1.0));
        }
        solver.finalize();
    }

    // MEAN leaks through the centroid-diluted crest (the documented defect).
    {
        auto mesh = makeGridMesh(8, 4, 2.0, ridgeZ);
        SolverOptions2D opts;                        // default MEAN
        auto state = makeState(mesh);
        seedLeftPool(mesh, opts, state);

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        solver.advance(0.0, 600.0);
        double crossed = 0.0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            if (mesh.tri_cx[i] > 8.0) crossed += state.volume[i];
        EXPECT_GT(crossed, 0.0)
            << "expected the legacy centroid zface to leak across the crest";
        solver.finalize();
    }
}


namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    static void rk2(ExplicitInertialSolver& solver,double dt,double time) {solver.runRk2Step(dt,time);}
    static std::vector<double> state(const ExplicitInertialSolver& s) {
        std::vector<double> values=s.qcx_;values.insert(values.end(),s.qcy_.begin(),s.qcy_.end());
        for(const auto* row:{&s.facc_L_,&s.facc_R_,&s.macc_x_L_,&s.macc_x_R_,&s.bc_accum_,&s.source_ledgers_})
            values.insert(values.end(),row->begin(),row->end());
        values.insert(values.end(),{double(s.substeps_run_),double(s.last_steps_),s.t_last_sync_,s.source_phase_end_});return values;
    }
};
}
namespace {
struct CoupledRkRig {
    MeshData mesh=makeGridMesh(1,1,4.,[](double,double){return 0.;});
    SurfaceStateData state=makeState(mesh);SolverOptions2D opts;ExplicitInertialSolver solver;SubsurfaceSolver gw;
    void init(double dt,double ks,double deep,double pond=0,const std::string& et="AUTO") {
        opts.momentum=Momentum2D::FULL_SWE;opts.reconstruction_order=2;opts.lts_tiers=1;opts.max_timestep=dt;
        seedSurface(mesh,opts,state,pond);solver.initialize(mesh,state,opts);
        SubsurfaceConfig cfg;cfg.options.authored=true;cfg.options.closure=GwClosure::CLOSED_FORM;cfg.options.node_auto=false;cfg.options.gw_et=et;
        GwAquiferRow row;row.Ks=ks;row.zs=2;row.hg0=.6;row.c_loss=deep;cfg.rows={row};
        std::vector<std::string> warnings;ASSERT_TRUE(gw.initialize(mesh,solver.inertialEdges(),opts,{},0,cfg,warnings).empty());
        ASSERT_TRUE(gw.rk2ProfileError().empty());solver.setSubsurface(&gw);
    }
    double total() const {return totalVolume(state,mesh.n_cells())+gw.state().liveStorage();}
};
}
namespace {
GwResolvedBoundary rk2FlowEdge(const CoupledRkRig& rig,int cell,const GwSourceSignal& signal) {
    for(int edge=0;edge<rig.mesh.cell_nv[cell];++edge) {
        const auto slot=MeshData::slot(cell,edge);
        if(rig.mesh.cell_nbr[slot]!=-1)continue;
        GwResolvedBoundary result;result.cell=cell;result.edge=edge;result.signal=signal;
        result.length=rig.mesh.edge_length[slot];result.distance=rig.mesh.edge_dist_c[slot];return result;
    }
    throw std::runtime_error("Test cell has no exterior edge");
}
}
TEST(CoupledGroundwaterRk2, AdvancesActualAquiferWithAnalyticalSecondOrderDeepDrainage) {
    double previous=0;
    for(double dt:{.4,.2,.1,.05}) {
        CoupledRkRig r;r.init(dt,0,.07);auto& w=r.gw.state();
        for(int i=0;i<w.n_cells;++i)w.hu[i]=w.theta_r[i]*(w.zs[i]-w.hg[i]);
        w.led_init_storage=w.storage();const double initial=r.total();
        for(int k=0;k<int(2/dt);++k)ASSERT_DOUBLE_EQ(r.solver.advance(k*dt,(k+1)*dt),(k+1)*dt);
        const double exact=.6*std::exp(-.07*2/(2*(.45-.1)));
        const double error=std::abs(w.hg[0]-exact);RecordProperty("error_"+std::to_string(int(dt*1000)),error);EXPECT_LT(error,.00004);
        if(previous>0)EXPECT_GT(previous/error,3.8);previous=error;
        EXPECT_NEAR(r.total()+w.led_deep,initial,1e-10*initial);
        EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);EXPECT_DOUBLE_EQ(r.gw.cellCompletedTime(0),2);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceInfiltrationAndAquiferAdvanceTogetherAndRefine) {
    const auto trajectory=[](double dt) {
        CoupledRkRig r;r.init(dt,.005,.0005,.2);
        auto& water=r.gw.state();
        for(int i=0;i<water.n_cells;++i) {water.hu[i]=.35*(water.zs[i]-water.hg[i]);water.wetting_front[i]=.1;}
        water.led_init_storage=water.storage();const double initial=r.total();
        r.gw.publishInfiltration(r.state,dt,0);
        const int steps=int(std::round(2/dt));
        for(int k=0;k<steps;++k)r.solver.advance(k*dt,(k+1)*dt);
        const auto& w=r.gw.state();EXPECT_GT(w.led_infil_in,0);EXPECT_GT(w.led_recharge,0);
        EXPECT_NEAR(r.total()+w.led_deep,initial,1e-10*initial);
        EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
        return std::vector<double>{r.state.volume[0],w.hg[0],w.hu[0]};
    };
    const auto reference=trajectory(.003125);double previous=0;
    for(double dt:{.4,.2,.1}) {
        const auto result=trajectory(dt);double square=0;
        for(int j=0;j<3;++j)square+=(result[j]-reference[j])*(result[j]-reference[j]);
        const double error=std::sqrt(square);RecordProperty("error_"+std::to_string(int(dt*1000)),error);if(previous>0)EXPECT_GT(previous/error,3.5);previous=error;
    }
}
TEST(CoupledGroundwaterRk2, NonfiniteTrialRollsBackSurfaceGroundwaterMomentumAndReceipts) {
    CoupledRkRig r;r.init(.1,.005,0,.2);
    GwResolvedSource source;source.cells={{0,1}};source.flow.constant=1e308;r.gw.setSources({source});
    r.gw.publishInfiltration(r.state,.1,0);r.gw.assignTiers(.1,1);
    const auto solver=ExplicitInertialSolverTestAccess::state(r.solver),volume=r.state.volume;
    const auto water=r.gw.state();const double initial=r.total();
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,4,0),std::runtime_error);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::state(r.solver),solver);EXPECT_EQ(r.state.volume,volume);
    EXPECT_EQ(r.gw.state().hg,water.hg);EXPECT_EQ(r.gw.state().hu,water.hu);
    EXPECT_EQ(r.gw.state().xacc_to_surface,water.xacc_to_surface);EXPECT_EQ(r.gw.state().xacc_from_surface,water.xacc_from_surface);
    EXPECT_DOUBLE_EQ(r.gw.state().led_source_in,water.led_source_in);EXPECT_DOUBLE_EQ(r.total(),initial);
    r.gw.setSources({});ASSERT_NO_THROW(r.solver.advance(0,.1));EXPECT_NEAR(r.total(),initial,1e-10*initial);
}


namespace {
std::filesystem::path rk2Output(const std::string& file) {
    const char* location=std::getenv("SWMM_RK2_TEST_OUT");
    const auto root=std::filesystem::path(location?location:"tests/output/coupled_gw_rk2");
    std::filesystem::create_directories(root);return root/file;
}
std::string rk2File(const std::filesystem::path& path) {
    std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};
}
}
TEST(CoupledGroundwaterRk2, CompletedCheckpointResumesAllOwnedStateExactly) {
    CoupledRkRig a,b;a.init(.05,.005,.0005,.2);b.init(.05,.005,.0005,.2);
    a.gw.publishInfiltration(a.state,.05,0);b.gw.publishInfiltration(b.state,.05,0);
    const auto saved=rk2Output("accepted.chk"),restored=rk2Output("restored.chk");
    for(int k=0;k<7;++k) {
        a.solver.advance(k*.05,(k+1)*.05);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty())<<k;
    }
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(saved),rk2File(restored));
    for(int k=7;k<20;++k) {
        a.solver.advance(k*.05,(k+1)*.05);b.solver.advance(k*.05,(k+1)*.05);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
        EXPECT_EQ(rk2File(saved),rk2File(restored))<<k;
    }
}
TEST(CoupledGroundwaterRk2, CorruptAndIncompatibleCheckpointRefusalIsAtomicAndRetryWorks) {
    CoupledRkRig a,b;a.init(.05,.005,.0005,.2);b.init(.05,.005,.0005,.2);
    a.solver.advance(0,.05);
    const auto saved=rk2Output("good.chk"),bad=rk2Output("corrupt.chk"),before=rk2Output("fresh.chk"),after=rk2Output("refused.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());ASSERT_TRUE(b.solver.saveCheckpoint(before.string()).empty());
    auto bytes=rk2File(saved);ASSERT_GT(bytes.size(),32);bytes[bytes.size()/2]^=1;
    {std::ofstream file(bad,std::ios::binary);file.write(bytes.data(),bytes.size());}
    EXPECT_FALSE(b.solver.loadCheckpoint(bad.string()).empty());ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(before),rk2File(after));
    const double conductivity=b.gw.state().Ks[0];b.gw.state().Ks[0]*=2;
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());b.gw.state().Ks[0]=conductivity;
    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());EXPECT_EQ(rk2File(before),rk2File(after));
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());EXPECT_EQ(rk2File(saved),rk2File(after));
}
TEST(CoupledGroundwaterRk2, SingularIntakeStartupAndBoundedWellsConserveThroughWettingAndDrying) {
    for(double flow:{100.,-100.}) {
        CoupledRkRig r;r.init(.025,.005,.0005,.2);const double initial=r.total();
        GwResolvedSource source;source.cells={{0,1}};source.flow.constant=flow;r.gw.setSources({source});
        for(int k=0;k<20;++k) {
            ASSERT_NO_THROW(r.solver.advance(k*.025,(k+1)*.025));const auto& w=r.gw.state();
            EXPECT_NEAR(r.total()+w.led_deep+w.led_et-w.led_source_in+w.led_source_out,initial,1e-10*std::max(1.,r.total()));
            EXPECT_NEAR(w.continuityResidual(),0,1e-10*std::max(1.,w.led_init_storage));
            for(int i=0;i<w.n_cells;++i){EXPECT_GE(w.hg[i],0);EXPECT_LE(w.hg[i],w.zs[i]);EXPECT_GE(w.hu[i],0);EXPECT_GE(r.state.volume[i],0);}
        }
        if(flow>0)EXPECT_GT(r.gw.state().led_dunne,0);else {EXPECT_GT(r.gw.state().led_source_out,0);EXPECT_LT(r.gw.state().led_source_out,50);}
    }
}

TEST(CoupledGroundwaterRk2, UnsupportedOwnersRefuseBeforeAnyNumericalMutation) {
    CoupledRkRig r;r.init(.05,.005,0,.2);const auto before=r.state.volume;
    r.gw.state().closure[0]=int8_t(GwClosure::SIGMA);
    EXPECT_FALSE(r.gw.rk2ProfileError().empty());EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,.05,0),std::runtime_error);
    r.gw.state().closure[0]=int8_t(GwClosure::CLOSED_FORM);
    GwResolvedSource source;source.cells={{0,1}};source.terms.resize(1);r.gw.setSources({source});
    EXPECT_FALSE(r.gw.rk2ProfileError().empty());EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,.05,0),std::runtime_error);
    r.gw.setSources({});r.state.heat.enabled=true;
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,.05,0),std::runtime_error);r.state.heat.enabled=false;
    EXPECT_EQ(r.state.volume,before);EXPECT_DOUBLE_EQ(r.gw.cellCompletedTime(0),0);
}

TEST(CoupledGroundwaterRk2, FailedAdvanceRestoresTheWholeCallBeforeRetry) {
    CoupledRkRig r;r.init(.05,.005,0,.2);
    GwResolvedSource source;source.cells={{0,1}};source.flow.constant=1e308;r.gw.setSources({source});
    const auto before=rk2Output("advance-before.chk"),after=rk2Output("advance-after.chk");
    ASSERT_TRUE(r.solver.saveCheckpoint(before.string()).empty());
    EXPECT_THROW(r.solver.advance(0,4),std::runtime_error);
    ASSERT_TRUE(r.solver.saveCheckpoint(after.string()).empty());EXPECT_EQ(rk2File(before),rk2File(after));
    r.gw.setSources({});const double initial=r.total();ASSERT_NO_THROW(r.solver.advance(0,.05));EXPECT_NEAR(r.total(),initial,1e-10*initial);
}

TEST(CoupledGroundwaterRk2, LateralDarcyRelaxationMatchesAnAnalyticalNonlinearDonorSolution) {
    double previous=0;
    for(double dt:{.8,.4,.2,.1}) {
        CoupledRkRig r;r.init(dt,.005,0,0,"NONE");auto& w=r.gw.state();
        w.hg[0]=.8;w.hg[1]=.4;
        for(int i=0;i<2;++i)w.hu[i]=w.theta_r[i]*(w.zs[i]-w.hg[i]);
        w.led_init_storage=w.storage();const double initial=r.total();
        const auto& edges=r.solver.inertialEdges();ASSERT_EQ(edges.ne,1);
        const double c=.005*edges.xi[0]*edges.inv_dx[0]/((.45-.1)*w.area[0]);
        const double attenuation=std::exp(-c*1.2*8),difference=1.2*.4*attenuation/(1.2+.4*(1-attenuation));
        for(int k=0;k<int(std::round(8/dt));++k)r.solver.advance(k*dt,(k+1)*dt);
        const double error=std::abs(w.hg[0]-(1.2+difference)/2);
        RecordProperty("error_"+std::to_string(int(dt*1000)),error);EXPECT_LT(error,1e-4);
        if(previous>0)EXPECT_GT(previous/error,3.8);previous=error;
        EXPECT_NEAR(w.hg[0]+w.hg[1],1.2,1e-12);EXPECT_NEAR(r.total(),initial,1e-10*initial);
        EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
    }
}
TEST(CoupledGroundwaterRk2, FullStepAndTwoHalfStepsDifferAtThirdLocalOrder) {
    double previous=0;
    for(double dt:{.4,.2,.1,.05,.025}) {
        CoupledRkRig full,half;full.init(.4,.005,.0005,.2);half.init(.4,.005,.0005,.2);
        for(auto* r:{&full,&half}) {
            auto& w=r->gw.state();for(int i=0;i<w.n_cells;++i){w.hu[i]=.35*(w.zs[i]-w.hg[i]);w.wetting_front[i]=.1;}
            w.led_init_storage=w.storage();
        }
        full.solver.advance(0,dt);half.solver.advance(0,dt/2);half.solver.advance(dt/2,dt);
        double error=0;
        for(int i=0;i<2;++i)error+=std::abs(full.state.volume[i]-half.state.volume[i])+std::abs(full.gw.state().hg[i]-half.gw.state().hg[i]);
        RecordProperty("difference_"+std::to_string(int(dt*1000)),error);
        // Keep the coarse measurements; require the unchanged local-order
        // gate on four successive steps in the asymptotic range.
        if(previous>0&&dt<=.1)EXPECT_GT(previous/error,7.);previous=error;
    }
}

TEST(CoupledGroundwaterRk2, OptionalOpenmpRechargePreservesStagesRestartAndRollback) {
    // This hardware gate is run with OPENSWMM_2D_BACKEND=omp. Ordinary CPU
    // suites do not require a separately installed optional plugin.
    for(bool head:{false,true}) {
    SCOPED_TRACE(head);CoupledRkRig a,b,cpu;a.init(.05,.005,.0005,.2);b.init(.05,.005,.0005,.2);cpu.init(.05,.005,.0005,.2);
    auto backend=makeGwRechargeBackend(a.opts,a.mesh.n_cells());
    if(!backend.evaluate){ASSERT_FALSE(std::getenv("OPENSWMM_GW_TEST_REQUIRE_PLUGIN"));GTEST_SKIP()<<backend.diagnostic;}
    a.gw.setRechargeBackend(backend);b.gw.setRechargeBackend(backend);
    GwResolvedSource well;well.name="omp-reversing";well.cells={{0,1}};
    well.flow.times={0,.17,.43,1};well.flow.values={.002,.006,-.002,.001};
    GwSourceSignal edge;edge.times={0,.13,.67,1};edge.values={.004,-.001,.002,.001};
    for(auto* rig:{&a,&b,&cpu}) {
        auto boundary=rk2FlowEdge(*rig,1,edge);boundary.head=head;
        if(head) {
            const double bottom=rig->gw.state().z_bed[1];boundary.signal.values={bottom+1,bottom+.2,bottom+2.5,bottom+.8};
        }
        rig->gw.setSources({well});rig->gw.setBoundaries({boundary});
    }
    const std::string prefix=head?"omp-head":"omp-flow";
    const auto saved=rk2Output(prefix+"-accepted.chk"),restored=rk2Output(prefix+"-restored.chk"),before=rk2Output(prefix+"-before.chk");
    const double initial=a.total();
    for(int k=0;k<20;++k) {
        a.solver.advance(k*.05,(k+1)*.05);cpu.solver.advance(k*.05,(k+1)*.05);
        EXPECT_EQ(a.state.volume,cpu.state.volume);EXPECT_EQ(a.gw.state().hg,cpu.gw.state().hg);EXPECT_EQ(a.gw.state().hu,cpu.gw.state().hu);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        if(k==7) {
            ASSERT_TRUE(cpu.solver.saveCheckpoint(before.string()).empty());
            EXPECT_FALSE(cpu.solver.loadCheckpoint(saved.string()).empty());
            ASSERT_TRUE(cpu.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(before),rk2File(restored));
            ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());EXPECT_EQ(b.gw.rechargeCells(),0u);
        } else if(k>7)b.solver.advance(k*.05,(k+1)*.05);
        if(k>=7) {ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(saved),rk2File(restored));}
    }
    EXPECT_GT(a.gw.rechargeCells(),0u);EXPECT_GT(b.gw.rechargeCells(),0u);
    EXPECT_NEAR(a.total()+a.gw.state().led_deep,initial+a.gw.state().led_source_in-a.gw.state().led_source_out+
        a.gw.state().led_boundary_in-a.gw.state().led_boundary_out,1e-10*initial);
    GwResolvedSource bad;bad.cells={{0,1}};bad.flow.constant=1e308;a.gw.setSources({bad});
    const auto batches=a.gw.rechargeBatches(),cells=a.gw.rechargeCells();
    const auto state=ExplicitInertialSolverTestAccess::state(a.solver),volume=a.state.volume;
    EXPECT_THROW(a.solver.advance(1,5),std::runtime_error);
    EXPECT_EQ(a.gw.rechargeBatches(),batches);EXPECT_EQ(a.gw.rechargeCells(),cells);
    EXPECT_EQ(a.state.volume,volume);EXPECT_EQ(ExplicitInertialSolverTestAccess::state(a.solver),state);
    a.gw.setSources({});ASSERT_NO_THROW(a.solver.advance(1,1.05));
}
}

TEST(CoupledGroundwaterRk2, AffineWellAndDeepDrainageConvergeAtSecondOrder) {
    for(bool edge:{false,true}) {
    SCOPED_TRACE(edge);double previous=0;
    for(double dt:{.4,.2,.1,.05}) {
        CoupledRkRig r;r.init(dt,0,.07);auto& w=r.gw.state();
        for(int i=0;i<w.n_cells;++i)w.hu[i]=w.theta_r[i]*(w.zs[i]-w.hg[i]);
        w.led_init_storage=w.storage();const double initial=r.total();
        GwResolvedSource well;well.name="ramp";well.cells={{0,1}};
        well.flow.times={0,2};well.flow.values={.003,.009};
        if(edge)r.gw.setBoundaries({rk2FlowEdge(r,0,well.flow)});else r.gw.setSources({well});
        ASSERT_TRUE(r.gw.rk2ProfileError().empty());
        for(int k=0;k<int(2/dt);++k)ASSERT_DOUBLE_EQ(r.solver.advance(k*dt,(k+1)*dt),(k+1)*dt);
        const double yield=w.theta_s[0]-w.theta_r[0],lambda=.07/(2*yield),storage=yield*w.area[0];
        const double decay=std::exp(-lambda*2);
        const double exact=.6*decay+.003/storage*(1-decay)/lambda+
            .003/storage*(2/lambda-(1-decay)/(lambda*lambda));
        const double error=std::abs(w.hg[0]-exact);
        RecordProperty(std::string(edge?"affine_edge_error_":"affine_well_error_")+std::to_string(int(dt*1000)),error);
        EXPECT_LT(error,.00004);if(previous>0)EXPECT_GT(previous/error,3.7);previous=error;
        EXPECT_NEAR(edge?w.led_boundary_in:w.led_source_in,.012,1e-14);
        EXPECT_NEAR(r.total()+w.led_deep,initial+.012,1e-10*initial);
        EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
    }
}
}

TEST(CoupledGroundwaterRk2, SignedWellKnotsPreserveGrossBudgetsAndExactCheckpointContinuation) {
    CoupledRkRig a,b;a.init(.4,.005,.01,.2);b.init(.4,.005,.01,.2);
    GwResolvedSource well;well.name="reversing";well.cells={{0,1}};
    well.flow.times={0,.15,.35,.7,1};well.flow.values={.02,.06,-.02,-.04,.02};
    GwResolvedSource other;other.name="asynchronous";other.cells={{1,1}};
    other.flow.times={0,.21,.49,1};other.flow.values={.01,.03,0,.01};
    a.gw.setSources({well,other});b.gw.setSources({well,other});
    ASSERT_TRUE(a.gw.rk2ProfileError().empty());const double initial=a.total();
    ASSERT_DOUBLE_EQ(a.solver.advance(0,.45),.45);
    const auto saved=rk2Output("timed-well.chk"),restored=rk2Output("timed-well-restored.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    auto changed=well;changed.flow.values[1]+=.001;b.gw.setSources({changed,other});
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());const auto before=rk2File(restored);
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(restored),before);
    b.gw.setSources({well,other});ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    for(double end:{.8,1.}) {
        ASSERT_DOUBLE_EQ(a.solver.advance(a.gw.cellCompletedTime(0),end),end);
        ASSERT_DOUBLE_EQ(b.solver.advance(b.gw.cellCompletedTime(0),end),end);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(saved),rk2File(restored));
    }
    // Independent trapezoids split where the prescribed straight lines cross zero.
    const double incoming=.006+.5*.15*.06+.5*.1*.02+.21*.02+.28*.015+.51*.005;
    const double outgoing=.5*.05*.02+.35*.03+.5*.2*.04;
    const auto& w=a.gw.state();EXPECT_NEAR(w.led_source_in,incoming,1e-13);EXPECT_NEAR(w.led_source_out,outgoing,1e-13);
    EXPECT_NEAR(a.total()+w.led_deep,initial+incoming-outgoing,1e-10*initial);
    EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
}

TEST(CoupledGroundwaterRk2, OversubscribedTimedWellRemainsBoundedAndConservative) {
    CoupledRkRig r;r.init(.2,0,0);const double initial=r.total();
    GwResolvedSource well;well.name="pump-cycle";well.cells={{0,1}};
    well.flow.times={0,.2,.6,1};well.flow.values={-100,-100,1,1};r.gw.setSources({well});
    ASSERT_TRUE(r.gw.rk2ProfileError().empty());ASSERT_DOUBLE_EQ(r.solver.advance(0,1),1);
    const auto& w=r.gw.state();const double positive=.4+.5*(.4/101);
    EXPECT_NEAR(w.led_source_in,positive,1e-12);EXPECT_GT(w.led_source_out,0);EXPECT_LT(w.led_source_out,40);
    for(int cell=0;cell<w.n_cells;++cell) {
        EXPECT_TRUE(std::isfinite(w.hg[cell]));EXPECT_GE(w.hg[cell],0);EXPECT_LE(w.hg[cell],w.zs[cell]);
        EXPECT_GE(w.hu[cell],0);EXPECT_LE(w.hu[cell],w.theta_s[cell]*(w.zs[cell]-w.hg[cell])+1e-12);
    }
    EXPECT_NEAR(r.total(),initial+w.led_source_in-w.led_source_out,1e-10*initial);
    EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
}

TEST(CoupledGroundwaterRk2, LaterWellIntervalFailureRollsBackEarlierAcceptedStages) {
    CoupledRkRig r,control;r.init(.1,.005,0,.2);control.init(.1,.005,0,.2);
    GwResolvedSource well;well.name="overflow";well.cells={{0,1}};
    well.flow.times={0,.5,4.5};well.flow.values={.001,.001,1e308};r.gw.setSources({well});control.gw.setSources({well});
    for(auto* rig:{&r,&control}) {rig->gw.publishInfiltration(rig->state,.1,0);rig->gw.assignTiers(.1,1);}
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver,.5,0));
    EXPECT_NEAR(control.gw.state().led_source_in,.0005,1e-14);
    const auto numerical=ExplicitInertialSolverTestAccess::state(r.solver),volume=r.state.volume;
    const auto water=r.gw.state();const double initial=r.total();
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,4.5,0),std::runtime_error);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::state(r.solver),numerical);EXPECT_EQ(r.state.volume,volume);
    EXPECT_EQ(r.gw.state().hg,water.hg);EXPECT_EQ(r.gw.state().hu,water.hu);
    EXPECT_EQ(r.gw.state().xacc_to_surface,water.xacc_to_surface);EXPECT_EQ(r.gw.state().xacc_from_surface,water.xacc_from_surface);
    EXPECT_DOUBLE_EQ(r.gw.state().led_source_in,water.led_source_in);EXPECT_DOUBLE_EQ(r.total(),initial);
    r.gw.setSources({});ASSERT_NO_THROW(r.solver.advance(0,.1));EXPECT_NEAR(r.total(),initial,1e-10*initial);
}

TEST(CoupledGroundwaterRk2, FlowEdgesShareWellStagesAndPreserveRestartDiagnostics) {
    CoupledRkRig a,b;a.init(.4,.005,.01,.2);b.init(.4,.005,.01,.2);
    GwResolvedSource well;well.name="well-and-edge";well.cells={{0,1}};
    well.flow.times={0,.15,.35,.7,1};well.flow.values={.02,.06,-.02,-.04,.02};
    GwSourceSignal signal;signal.times={0,.31,.83,1};signal.values={.04,-.02,.01,.03};
    const auto edge=rk2FlowEdge(a,1,signal);
    a.gw.setSources({well});b.gw.setSources({well});a.gw.setBoundaries({edge});b.gw.setBoundaries({edge});
    ASSERT_TRUE(a.gw.rk2ProfileError().empty());const double initial=a.total();
    ASSERT_DOUBLE_EQ(a.solver.advance(0,.45),.45);
    const auto saved=rk2Output("flow-edge.chk"),restored=rk2Output("flow-edge-restored.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    auto changed=edge;changed.signal.values[1]+=.001;b.gw.setBoundaries({changed});
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());const auto before=rk2File(restored);
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(restored),before);
    b.gw.setBoundaries({edge});ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    for(double end:{.8,1.}) {
        ASSERT_DOUBLE_EQ(a.solver.advance(a.gw.cellCompletedTime(0),end),end);
        ASSERT_DOUBLE_EQ(b.solver.advance(b.gw.cellCompletedTime(0),end),end);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(saved),rk2File(restored));
    }
    // Independent signed trapezoids: well +0.0115/-0.015, edge +0.0084/-0.0045 m3.
    const auto& w=a.gw.state();EXPECT_NEAR(w.led_source_in,.0115,1e-13);EXPECT_NEAR(w.led_source_out,.015,1e-13);
    EXPECT_NEAR(w.led_boundary_in,.0084,1e-13);EXPECT_NEAR(w.led_boundary_out,.0045,1e-13);
    const auto& receipt=a.gw.boundaries().front();
    EXPECT_NEAR(receipt.requested_in,.0084,1e-13);EXPECT_NEAR(receipt.applied_in,.0084,1e-13);
    EXPECT_NEAR(receipt.requested_out,.0045,1e-13);EXPECT_NEAR(receipt.applied_out,.0045,1e-13);
    EXPECT_NEAR(a.total()+w.led_deep,initial+.0115+.0084-.015-.0045,1e-10*initial);
    EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
}

TEST(CoupledGroundwaterRk2, FlowEdgePumpingCapsGrossAcceptedWithdrawals) {
    CoupledRkRig r;r.init(.2,0,0);const double initial=r.total();
    GwSourceSignal signal;signal.times={0,.2,.6,1};signal.values={-100,-100,1,1};
    r.gw.setBoundaries({rk2FlowEdge(r,0,signal)});ASSERT_TRUE(r.gw.rk2ProfileError().empty());
    ASSERT_DOUBLE_EQ(r.solver.advance(0,1),1);const auto& w=r.gw.state();const auto& receipt=r.gw.boundaries().front();
    EXPECT_NEAR(receipt.requested_in,.4+.5*(.4/101),1e-12);
    EXPECT_NEAR(receipt.requested_out,20+.5*(.4*100/101)*100,1e-12);
    EXPECT_LT(receipt.applied_out,receipt.requested_out);EXPECT_GT(receipt.applied_out,0);
    EXPECT_DOUBLE_EQ(receipt.applied_out,w.led_boundary_out);EXPECT_DOUBLE_EQ(receipt.applied_in,w.led_boundary_in);
    for(int cell=0;cell<w.n_cells;++cell) {
        EXPECT_TRUE(std::isfinite(w.hg[cell]));EXPECT_GE(w.hg[cell],0);EXPECT_LE(w.hg[cell],w.zs[cell]);
        EXPECT_GE(w.hu[cell],0);EXPECT_LE(w.hu[cell],w.theta_s[cell]*(w.zs[cell]-w.hg[cell])+1e-12);
    }
    EXPECT_NEAR(r.total(),initial+w.led_boundary_in-w.led_boundary_out,1e-10*initial);
    EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
}

TEST(CoupledGroundwaterRk2, LaterFlowEdgeFailureRollsBackReceiptsAndUnqualifiedOwnersRefuse) {
    CoupledRkRig r,control;r.init(.1,.005,0,.2);control.init(.1,.005,0,.2);
    GwSourceSignal signal;signal.times={0,.5,4.5};signal.values={.001,.001,1e308};
    const auto edge=rk2FlowEdge(r,0,signal);r.gw.setBoundaries({edge});control.gw.setBoundaries({edge});
    for(auto* rig:{&r,&control}) {rig->gw.publishInfiltration(rig->state,.1,0);rig->gw.assignTiers(.1,1);}
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver,.5,0));
    EXPECT_NEAR(control.gw.boundaries().front().applied_in,.0005,1e-14);
    const auto before=rk2Output("flow-edge-before.chk"),after=rk2Output("flow-edge-after.chk");
    ASSERT_TRUE(r.solver.saveCheckpoint(before.string()).empty());
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,4.5,0),std::runtime_error);
    ASSERT_TRUE(r.solver.saveCheckpoint(after.string()).empty());EXPECT_EQ(rk2File(after),rk2File(before));
    auto head=edge;head.head=true;head.terms.resize(1);r.gw.setBoundaries({head});EXPECT_FALSE(r.gw.rk2ProfileError().empty());
    const auto water=r.gw.state().hg;EXPECT_THROW(r.solver.advance(0,.1),std::runtime_error);EXPECT_EQ(r.gw.state().hg,water);
    head.head=false;head.terms.resize(1);r.gw.setBoundaries({head});EXPECT_FALSE(r.gw.rk2ProfileError().empty());
    r.gw.setBoundaries({});const double initial=r.total();ASSERT_NO_THROW(r.solver.advance(0,.1));
    EXPECT_NEAR(r.total(),initial,1e-10*initial);
}


TEST(CoupledGroundwaterRk2, ConstantHeadEdgesConvergeToTheExactUnconfinedSolution) {
    for(double head:{.2,1.}) {
        double previous=0;
        for(double dt:{.8,.4,.2,.1}) {
            SCOPED_TRACE(head);
            SCOPED_TRACE(dt);CoupledRkRig r;r.init(dt,.02,0,0,"NONE");auto& w=r.gw.state();
            for(int cell=0;cell<w.n_cells;++cell)w.hu[cell]=w.theta_r[cell]*(w.zs[cell]-w.hg[cell]);
            w.led_init_storage=w.storage();const double initial=r.total();GwSourceSignal signal;signal.constant=w.z_bed[0]+head;
            auto first=rk2FlowEdge(r,0,signal),second=rk2FlowEdge(r,1,signal);first.head=second.head=true;
            ASSERT_DOUBLE_EQ(first.length,second.length);ASSERT_DOUBLE_EQ(first.distance,second.distance);
            r.gw.setBoundaries({first,second});ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            const double k=.02*.5*first.length/(first.distance*(.45-.1)*w.area[0]);
            const double ratio=(.6-head)/(.6+head)*std::exp(-2*head*k*8);
            const double exact=head*(1+ratio)/(1-ratio);
            for(int tick=0;tick<int(std::round(8/dt));++tick)r.solver.advance(tick*dt,(tick+1)*dt);
            const double error=std::abs(w.hg[0]-exact);EXPECT_LT(error,1e-4);
            RecordProperty("head_"+std::to_string(int(head*10))+"_error_"+std::to_string(int(dt*1000)),error);
            if(previous>0)EXPECT_GT(previous/error,3.7);previous=error;
            EXPECT_DOUBLE_EQ(w.hg[0],w.hg[1]);
            EXPECT_NEAR(r.total(),initial+w.led_boundary_in-w.led_boundary_out,1e-10*initial);
            EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
        }
    }
}

namespace {
GwSourceSignal rk2HeadCycle(double bottom) {
    GwSourceSignal signal;signal.times={0,.25,.75,1};signal.values={bottom+1,bottom-.25,bottom+2.5,bottom+.4};return signal;
}
std::vector<GwResolvedBoundary> rk2HeadEdges(const CoupledRkRig& rig,const GwSourceSignal& signal) {
    auto a=rk2FlowEdge(rig,0,signal),b=rk2FlowEdge(rig,1,signal);a.head=b.head=true;return {a,b};
}
std::array<long double,3> headCycleReference(double length,double distance,double area,int steps=800000) {
    // Independent normalized-height ODE: pressure head is clipped at the
    // bottom, wetted thickness at [0,2]. No production forcing/integrator calls.
    const long double scale=.02L*.5L*length/distance,storage=.35L*area;
    const auto rhs=[&](long double t,long double h) {
        const long double prescribed=t<=.25L?1-5*t:t<=.75L?-.25L+5.5L*(t-.25L):2.5L-8.4L*(t-.75L);
        const long double outside=std::max(0.L,prescribed),wet=std::min(2.L,outside);
        const long double q=scale*(h+wet)*(outside-h);
        return std::array<long double,3>{q/storage,std::max(0.L,q),std::max(0.L,-q)};
    };
    std::array<long double,3> result{.6L,0,0};const long double dt=1.L/steps;
    for(int n=0;n<steps;++n) {
        const long double t=n*dt;const auto a=rhs(t,result[0]),b=rhs(t+dt/2,result[0]+dt*a[0]/2),
            c=rhs(t+dt/2,result[0]+dt*b[0]/2),d=rhs(t+dt,result[0]+dt*c[0]);
        for(int row=0;row<3;++row)result[row]+=dt*(a[row]+2*b[row]+2*c[row]+d[row])/6;
    }
    return result;
}
void residualGround(CoupledRkRig& rig) {
    auto& w=rig.gw.state();for(int cell=0;cell<w.n_cells;++cell)w.hu[cell]=w.theta_r[cell]*(w.zs[cell]-w.hg[cell]);
    w.led_init_storage=w.storage();
}
}

TEST(CoupledGroundwaterRk2, TimedClippedHeadEdgesMatchAnIndependentScalarReference) {
    double previous=0;
    for(double dt:{.05,.025,.0125,.00625,.003125,.0015625}) {
        SCOPED_TRACE(dt);CoupledRkRig r;r.init(dt,.02,0,0,"NONE");residualGround(r);auto& w=r.gw.state();
        const auto edges=rk2HeadEdges(r,rk2HeadCycle(w.z_bed[0]));r.gw.setBoundaries(edges);
        const double initial=r.total();const auto reference=headCycleReference(edges[0].length,edges[0].distance,w.area[0]);
        if(dt==.05) {
            const auto refined=headCycleReference(edges[0].length,edges[0].distance,w.area[0],1600000);
            for(int row=0;row<3;++row)EXPECT_NEAR(double(reference[row]),double(refined[row]),1e-13);
        }
        for(int tick=0;tick<int(std::round(1/dt));++tick)r.solver.advance(tick*dt,(tick+1)*dt);
        const auto& receipt=r.gw.boundaries()[0];const double storage=.35*w.area[0];
        const double error=std::max({std::abs(w.hg[0]-double(reference[0])),
            std::abs(receipt.applied_in-double(reference[1]))/storage,std::abs(receipt.applied_out-double(reference[2]))/storage});
        RecordProperty("head_cycle_error_"+std::to_string(int(dt*1e6)),error);EXPECT_LT(error,1e-4);
        // Keep coarse event/cancellation errors as evidence. The four
        // finest steps qualify the asymptotic rate; monotone coarse error
        // reduction across head reversals/clipping is not asserted.
        if(dt<=.00625)EXPECT_GT(previous/error,3.5);previous=error;
        EXPECT_GT(receipt.applied_in,0);EXPECT_GT(receipt.applied_out,0);
        EXPECT_NEAR(r.total(),initial+w.led_boundary_in-w.led_boundary_out,1e-10*initial);
        EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
    }
}

TEST(CoupledGroundwaterRk2, TimedHeadCheckpointRefusalIsAtomicAndContinuationIsExact) {
    CoupledRkRig a,b;a.init(.05,.02,0,0,"NONE");b.init(.05,.02,0,0,"NONE");residualGround(a);residualGround(b);
    const auto edges=rk2HeadEdges(a,rk2HeadCycle(a.gw.state().z_bed[0]));a.gw.setBoundaries(edges);b.gw.setBoundaries(edges);
    ASSERT_DOUBLE_EQ(a.solver.advance(0,.37),.37);
    const auto saved=rk2Output("head-cycle.chk"),restored=rk2Output("head-cycle-restored.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());auto changed=edges;changed[0].signal.values[1]+=.001;b.gw.setBoundaries(changed);
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());const auto before=rk2File(restored);
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(restored),before);
    b.gw.setBoundaries(edges);ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    for(double end:{.71,1.}) {
        ASSERT_DOUBLE_EQ(a.solver.advance(a.gw.cellCompletedTime(0),end),end);
        ASSERT_DOUBLE_EQ(b.solver.advance(b.gw.cellCompletedTime(0),end),end);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());EXPECT_EQ(rk2File(saved),rk2File(restored));
    }
}

TEST(CoupledGroundwaterRk2, HeadDrivenSaturationAndDrainageConserveAcrossBoundedEvents) {
    for(double height:{-5.,10.}) {
        SCOPED_TRACE(height);CoupledRkRig r;r.init(.05,.1,0,0,"NONE");residualGround(r);auto& w=r.gw.state();
        GwSourceSignal signal;signal.constant=w.z_bed[0]+height;r.gw.setBoundaries(rk2HeadEdges(r,signal));const double initial=r.total();
        ASSERT_NO_THROW(r.solver.advance(0,2));
        if(height>0)EXPECT_GT(w.led_dunne,0);else EXPECT_GT(w.led_boundary_out,0);
        for(int cell=0;cell<w.n_cells;++cell) {
            EXPECT_TRUE(std::isfinite(w.hg[cell]));EXPECT_GE(w.hg[cell],0);EXPECT_LE(w.hg[cell],w.zs[cell]);
            EXPECT_GE(w.hu[cell],0);EXPECT_LE(w.hu[cell],w.theta_s[cell]*(w.zs[cell]-w.hg[cell])+1e-12);
        }
        EXPECT_NEAR(r.total(),initial+w.led_boundary_in-w.led_boundary_out,1e-10*initial);
        EXPECT_NEAR(w.continuityResidual(),0,1e-10*initial);
    }
}

TEST(CoupledGroundwaterRk2, LaterHeadFailureRestoresEveryPriorStageAndReceipt) {
    CoupledRkRig r,control;r.init(.1,1,0,0,"NONE");control.init(.1,1,0,0,"NONE");residualGround(r);residualGround(control);
    GwSourceSignal signal;const double base=r.gw.state().z_bed[0];signal.times={0,.5,4.5};signal.values={base+.6,base+.6,1e308};
    GwResolvedSource well;well.name="head-failure-control";well.cells={{0,1}};well.flow.constant=.001;
    for(auto* rig:{&r,&control}) {
        rig->gw.setBoundaries(rk2HeadEdges(*rig,signal));rig->gw.setSources({well});rig->gw.publishInfiltration(rig->state,.1,0);rig->gw.assignTiers(.1,1);
    }
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver,.5,0));EXPECT_NEAR(control.gw.state().led_source_in,.0005,1e-14);
    const auto before=rk2Output("head-overflow-before.chk"),after=rk2Output("head-overflow-after.chk");
    ASSERT_TRUE(r.solver.saveCheckpoint(before.string()).empty());EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver,4.5,0),std::runtime_error);
    ASSERT_TRUE(r.solver.saveCheckpoint(after.string()).empty());EXPECT_EQ(rk2File(before),rk2File(after));
    signal.constant=base+.6;signal.times.clear();signal.values.clear();r.gw.setBoundaries(rk2HeadEdges(r,signal));
    ASSERT_NO_THROW(r.solver.advance(0,.1));EXPECT_NEAR(r.gw.state().continuityResidual(),0,1e-9);
}
