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
 * @file      test_2d_inertial_marcher.cpp
 * @brief     Analytic + property gates for the explicit local-inertial marcher
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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
#include "2d/input/SectionHandlers2D.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/gw/GwTransportData.hpp"
#include "2d/subsurface/LayerTransport.hpp"
#include "core/CompletedSourceArchive.hpp"
#include "2d/heat/MeshHeat.hpp"
#include "core/SWMMEngine.hpp"
#include "core/DateTime.hpp"
#include <openswmm/engine/openswmm_reactions.h>

using namespace openswmm::twoD;

namespace {

// nx × ny rectangular grid of dx-square quads split into 2 triangles each,
// bed elevation from z(x, y). Manning's n uniform.
/**
 * @brief Build a connected rectangular SI grid split into two triangles per square.
 *
 * Derived vertex/triangle products must fit int and allocation sizes. Does not validate
 * elevations/dimensions, initialize water or attach a solver; allocation/callable/topology errors
 * propagate.
 *
 * @param[in] nx Positive square-cell count in x.
 * @param[in] ny Positive square-cell count in y.
 * @param[in] dx Positive square side length in metres.
 * @param[in] z Synchronous callable mapping x/y metres to bed elevation metres; copied for the call
 *     and not retained.
 * @param[in] n Uniform Manning roughness assigned to each triangle.
 * @return Owned mesh with vertices, triangle connectivity, roughness and built topology.
 */
template <typename ZFn> MeshData makeGridMesh(int nx, int ny, double dx, ZFn z, double n = 0.03) {
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
            const int v00 = j * nvx + i, v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.set_triangle(t, v00, v10, v11);
            ++t;
            mesh.set_triangle(t, v00, v11, v01);
            ++t;
        }
    }
    for (int i = 0; i < mesh.n_triangles(); ++i)
        mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

/**
 * @brief Create dry surface state matching the fixture triangle/vertex counts.
 *
 * Uses mesh.n_triangles and mesh.n_vertices for resize as authored; does not initialize an inertial
 * solver or derive nonzero boundary/coupling values.
 *
 * @param[in] mesh Borrowed topologized mesh with populated triangle centroid elevations.
 * @return Owned zero-depth/zero-volume state with each triangle head at bed elevation.
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

// Seed a uniform free surface η (dry where the bed stands above it) through
// the closure's own inverse, so the seeded state round-trips exactly.
/**
 * @brief Seed one common free surface through the configured cell-volume closure.
 *
 * Uses cellVolumeFromEta then cellEtaDepth for each triangle, retaining closure-consistent
 * dry/partially wetted cells. Does not reset fluxes, transport or solver clocks; no ownership is
 * transferred.
 *
 * @param[in] mesh Borrowed mesh with valid triangle geometry.
 * @param[in] opts Borrowed closure/subgrid options consistent with the intended solver.
 * @param[in,out] state Preallocated state covering all triangles; volume/head/depth rows are
 *     replaced.
 * @param[in] eta Target absolute free-surface elevation in metres.
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
 * @param[in] state Borrowed surface state with volume storage covering nt cells.
 * @param[in] nt Nonnegative cell count no greater than the volume vector length.
 * @return Ordered double sum in m3, zero for an empty prefix.
 */
double totalVolume(const SurfaceStateData& state, int nt) {
    double s = 0.0;
    for (int i = 0; i < nt; ++i)
        s += state.volume[i];
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// Lake at rest on an uneven bed: flux depends on Δη only, so a uniform η is
// an EXACT steady state — volumes bit-unchanged, projected fluxes bit-zero.
// ---------------------------------------------------------------------------
TEST(InertialMomentum, PhysicalTimeRelaxationIsIndependentOfStepPartition) {
    constexpr double theta = 0.8, reference = 1.0, reconstructed = -0.3;
    const double expected = reconstructed + (2.0 - reconstructed) * std::pow(theta, 2.0);
    for (const int partitions : {1, 2, 8, 128, 1024}) {
        double q = 2.0;
        const double blend = inertial::momentumBlendFraction(theta, 2.0 / partitions, reference);
        for (int k = 0; k < partitions; ++k)
            q += blend * (reconstructed - q);
        EXPECT_NEAR(q, expected, 5e-14) << partitions;
        EXPECT_GE(blend, 0.0);
        EXPECT_LE(blend, 1.0);
    }
    EXPECT_DOUBLE_EQ(inertial::momentumBlendFraction(theta, .01, 0.0), 1.0 - theta);
    EXPECT_DOUBLE_EQ(inertial::momentumBlendFraction(1.0, .01, reference), 0.0);
    EXPECT_DOUBLE_EQ(inertial::momentumBlendFraction(theta, 0.0, reference), 0.0);
    const double tiny = inertial::momentumBlendFraction(theta, 1e-14, reference);
    EXPECT_GT(tiny, 0.0);
    EXPECT_NEAR(tiny / 1e-14, -std::log(theta), 1e-14);
}

static void checkLakeAtRestExactFlatAndVfr(double reference_step) {
    for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        auto mesh = makeGridMesh(6, 6, 5.0, [](double x, double y) {
            return 0.05 * x + 0.11 * y + 0.3 * std::sin(0.7 * x);
        });
        SolverOptions2D opts;
        opts.theta_reference_step = reference_step;
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

TEST(InertialMarcher, LakeAtRestExactFlatAndVfr) { checkLakeAtRestExactFlatAndVfr(0.0); }
TEST(InertialMomentum, LakeAtRestExactFlatAndVfr) { checkLakeAtRestExactFlatAndVfr(1.0); }

// ---------------------------------------------------------------------------
// C-property / dry wall: a puddle whose surface sits BELOW the interface bed
// of a dry, higher neighbour must not leak uphill — bit-exact, 10 minutes.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, DryNeighbourWallNoCreep) {
    // Step bed: left half z = 0, right half z = 2. Puddle η = 1 on the left.
    auto mesh = makeGridMesh(8, 4, 2.0, [](double x, double) { return (x < 8.0) ? 0.0 : 2.0; });
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
static void checkClosedBasinConservationAndPositivity(double reference_step) {
    auto mesh = makeGridMesh(10, 10, 2.0, [](double, double) { return 0.0; });
    SolverOptions2D opts;
    opts.theta_reference_step = reference_step;
    auto state = makeState(mesh);
    // Column of water in the corner quarter.
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] < 5.0 && mesh.tri_cy[i] < 5.0) {
            state.volume[i] = 2.0 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
        }
    }
    const double sum0 = totalVolume(state, mesh.n_triangles());

    ExplicitInertialSolver solver;
    solver.initialize(mesh, state, opts);
    long steps = 0;
    for (int chunk = 0; chunk < 60; ++chunk) { // 60 × 10 s of sloshing
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

TEST(InertialMarcher, ClosedBasinConservationAndPositivity) { checkClosedBasinConservationAndPositivity(0.0); }
TEST(InertialMomentum, ClosedBasinConservationAndPositivity) { checkClosedBasinConservationAndPositivity(1.0); }

// ---------------------------------------------------------------------------
// Manning steady uniform slope: rain over an inclined strip draining through a
// NORMAL_FLOW outlet. At steady state the outlet ledger equals the rain input
// (≤ 1 %) and the interior depth matches the analytic normal depth
// h_n = (n·q / √S)^(3/5) with q the per-width discharge at that station (≤ 5 %).
// ---------------------------------------------------------------------------
static void checkManningSteadySlopeRainOutflow(double reference_step) {
    // dx = 1 m keeps the first-order upwind-depth staggering offset (~half a
    // cell of the (L−x)^{3/5} profile) comfortably inside the 5 % gate.
    const double S = 0.01, dx = 1.0, n_man = 0.03;
    const int nx = 40, ny = 4;
    auto mesh = makeGridMesh(nx, ny, dx, [&](double x, double) { return S * x; }, n_man);
    SolverOptions2D opts;
    opts.theta_reference_step = reference_step;
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
            if (nbrs[e] >= 0)
                continue;
            if (mesh.edge_mx[idx] < 1.0e-9) { // x = 0 boundary
                boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
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
    const double rain = 2.0e-4; // 720 mm/hr, everywhere
    for (int i = 0; i < mesh.n_triangles(); ++i)
        state.rainfall[i] = rain;

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
            if (boundary.edge_bc_type[idx] == static_cast<int8_t>(BoundaryType::NORMAL_FLOW))
                q_out += -state.edge_flux[idx];
        }
    double q_rain = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        q_rain += rain * mesh.tri_area[i];

    // Exact ledger identity over the window: rain in = outlet out + ΔStorage
    // (machine-exact by construction — the gate on BC booking + conservation).
    EXPECT_NEAR(q_rain * T, q_out * T + (v_end - v_start), 1.0e-8 * q_rain * T);
    // Rate check: outflow tracks rain input; the tolerance covers the
    // h_move activation slugs of thin near-crest cells cycling storage
    // (bounded by h_on × upslope area / T).
    EXPECT_NEAR(q_out, q_rain, 0.10 * q_rain);

    // Normal depth at mid-strip: per-width discharge q = rain · (L − x).
    const double L = nx * dx, x_mid = 0.5 * L;
    const double q_mid = rain * (L - x_mid);
    const double h_n = std::pow(n_man * q_mid / std::sqrt(S), 3.0 / 5.0);
    double h_avg = 0.0;
    int h_cnt = 0;
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

TEST(InertialMarcher, ManningSteadySlopeRainOutflow) { checkManningSteadySlopeRainOutflow(0.0); }
TEST(InertialMomentum, ManningSteadySlopeRainOutflow) { checkManningSteadySlopeRainOutflow(1.0); }

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
static void checkSteadySlopeNoCheckerboard(double reference_step) {
    const double S = 0.01, dx = 1.0, n_man = 0.03;
    const int nx = 40, ny = 4;
    for (const auto closure : {CellClosure2D::FLAT, CellClosure2D::VFR}) {
        auto mesh = makeGridMesh(nx, ny, dx, [&](double x, double) { return S * x; }, n_man);
        SolverOptions2D opts;
        opts.theta_reference_step = reference_step;
        opts.cell_closure = closure;
        auto state = makeState(mesh);

        BoundaryData boundary;
        boundary.resize(mesh.n_edge_slots());
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                                 mesh.cell_neighbour(i, 2)};
            for (int e = 0; e < 3; ++e) {
                const int idx = MeshData::slot(i, e);
                if (nbrs[e] >= 0)
                    continue;
                if (mesh.edge_mx[idx] < 1.0e-9) {
                    boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::NORMAL_FLOW);
                    boundary.edge_bed_slope[idx] = S;
                }
            }
        }
        state.boundary = &boundary;
        const double rain = 2.0e-4;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            state.rainfall[i] = rain;

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        solver.advance(0.0, 6000.0);

        // Spatial roughness of η: deviation from the face-neighbour mean over
        // the strip interior (ends excluded — the outlet drawdown and the
        // upslope crest carry genuine one-sided curvature).
        double ss = 0.0, mx_r = 0.0;
        int n = 0;
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            if (mesh.tri_cx[i] < 5.0 || mesh.tri_cx[i] > 35.0)
                continue;
            const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                                 mesh.cell_neighbour(i, 2)};
            double hs = 0.0;
            int hn = 0;
            for (int e = 0; e < 3; ++e)
                if (nbrs[e] >= 0) {
                    hs += state.head[nbrs[e]];
                    ++hn;
                }
            if (hn < 3)
                continue;
            const double r = state.head[i] - hs / hn;
            ss += r * r;
            mx_r = std::max(mx_r, std::fabs(r));
            ++n;
        }
        const double rms = std::sqrt(ss / std::max(n, 1));
        const char* tag = (closure == CellClosure2D::VFR) ? "VFR" : "FLAT";
        // Characterization gate: ~1.0 mm rms remains after the vector-
        // friction fix (was ~1.25 mm with scalar |q_n| friction) — the
        // residual is believed to be outlet-row alternation (NORMAL_FLOW
        // edges exist only on alternate boundary cells) plus the upwind-
        // depth staggering; the VFR quad-pair closure is the open work item
        // for the (larger) closure-side alternation on real embankments.
        // The gate trips if a change regresses toward the scalar-friction
        // level or beyond.
        EXPECT_LT(rms, 1.2e-3) << tag << ": steady-slope surface roughness rms " << rms;
        EXPECT_LT(mx_r, 3.5e-3) << tag << ": steady-slope surface roughness max " << mx_r;
        solver.finalize();
    }
}

TEST(InertialMarcher, SteadySlopeNoCheckerboard) { checkSteadySlopeNoCheckerboard(0.0); }
TEST(InertialMomentum, SteadySlopeNoCheckerboard) { checkSteadySlopeNoCheckerboard(1.0); }

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
    auto state = makeState(mesh); // dry everywhere

    BoundaryData boundary;
    boundary.resize(mesh.n_edge_slots());
    std::vector<int> bc_slots;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const int nbrs[3] = {mesh.cell_neighbour(i, 0), mesh.cell_neighbour(i, 1),
                             mesh.cell_neighbour(i, 2)};
        for (int e = 0; e < 3; ++e) {
            const int idx = MeshData::slot(i, e);
            if (nbrs[e] >= 0)
                continue;
            if (mesh.edge_mx[idx] < 1.0e-9) { // x = 0 boundary
                boundary.edge_bc_type[idx] = static_cast<int8_t>(BoundaryType::SPECIFIED_STAGE);
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
    double vbar = 0.0; // time-mean total volume over the marched span
    auto marchAccum = [&](double t0, double t1, int chunks) {
        double vol_in = 0.0;
        const double dt = (t1 - t0) / chunks;
        hbar.assign(mesh.n_triangles(), 0.0);
        vbar = 0.0;
        for (int c = 0; c < chunks; ++c) {
            solver.advance(t0 + c * dt, t0 + (c + 1) * dt);
            for (int idx : bc_slots)
                vol_in += state.edge_flux[idx] * dt;
            for (int i = 0; i < mesh.n_triangles(); ++i) {
                EXPECT_GE(state.volume[i], 0.0) << "negative volume";
                EXPECT_FALSE(std::isnan(state.volume[i]));
                hbar[i] += state.head[i];
            }
            vbar += totalVolume(state, mesh.n_triangles());
        }
        for (double& h : hbar)
            h /= chunks;
        vbar /= chunks;
        return vol_in;
    };

    // ---- Phase 1: fill from dry through the stage boundary. ----
    const double v0 = totalVolume(state, mesh.n_triangles());
    double in1 = marchAccum(0.0, 3000.0, 60); // fill transient
    in1 += marchAccum(3000.0, 4000.0, 200);   // measure window
    const double v1 = totalVolume(state, mesh.n_triangles());
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
    for (int i = 0; i < mesh.n_triangles(); ++i)
        a_tot += mesh.tri_area[i];
    EXPECT_NEAR(vbar / a_tot, 0.5, 0.03) << "mean surface not at the stage";
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(hbar[i], 0.5, 0.075)
            << "cell " << i << " time-mean head off the prescribed stage";

    // ---- Phase 2: lower the stage; the basin draws down to the new head. ----
    for (int idx : bc_slots)
        boundary.edge_bc_head[idx] = 0.2;
    double in2 = marchAccum(4000.0, 7000.0, 60); // drawdown transient
    in2 += marchAccum(7000.0, 8000.0, 200);      // measure window
    const double v2 = totalVolume(state, mesh.n_triangles());
    EXPECT_LT(in2, 0.0) << "lowered stage BC drove no outflow";
    EXPECT_NEAR(v2 - v1, in2, 1.0e-8 * std::max(-in2, 1.0e-12));
    EXPECT_NEAR(vbar / a_tot, 0.2, 0.03) << "mean surface not drawn down";
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_NEAR(hbar[i], 0.2, 0.075) << "cell " << i << " time-mean head off the lowered stage";
    solver.finalize();
}

// ---------------------------------------------------------------------------
// Steep-face dam-break: the Froude clamp must bound the projected face
// discharge at every published state; nothing blows up or NaNs.
// ---------------------------------------------------------------------------
TEST(InertialMarcher, FroudeClampSteepFace) {
    const double S = 0.10; // 10 % street grade
    auto mesh = makeGridMesh(20, 3, 1.0, [&](double x, double) { return -S * x; }, 0.015);
    SolverOptions2D opts;
    auto state = makeState(mesh);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] < 3.0) { // reservoir at the top
            state.volume[i] = 1.0 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
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
                if (j < 0)
                    continue; // boundary slot
                const int idx = MeshData::slot(i, e);
                const double q_w = std::fabs(state.edge_flux[idx]) / mesh.edge_length[idx];
                if (q_w == 0.0)
                    continue;
                const double hf = std::max(state.head[i], state.head[j]) -
                                  std::max(mesh.tri_cz[i], mesh.tri_cz[j]);
                ASSERT_GT(hf, 0.0) << "flux across a dry face";
                const double cap = opts.froude_max * hf * std::sqrt(inertial::kGravity * hf);
                ASSERT_LE(q_w, cap * (1.0 + 1.0e-9)) << "face discharge above the Froude clamp";
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
    EXPECT_NEAR(inertial::faceDepthFromEta(2.0, z_lo, z_hi), 1.0 * 1.0 / (2.0 * dz), 1e-15);
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
    auto mesh = makeGridMesh(8, 4, 2.0, [](double x, double) { return (x < 8.0) ? 0.0 : 2.0; });
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
    auto ridgeZ = [](double x, double) { return (std::fabs(x - 8.0) < 1.0e-9) ? 1.0 : 0.0; };
    auto seedLeftPool = [&](const MeshData& mesh, const SolverOptions2D& opts,
                            SurfaceStateData& state) {
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            state.volume[i] =
                (mesh.tri_cx[i] < 8.0) ? inertial::cellVolumeFromEta(mesh, opts, i, 0.9) : 0.0;
            inertial::cellEtaDepth(mesh, opts, i, state.volume[i], state.head[i], state.depth[i]);
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
        SolverOptions2D opts; // default MEAN
        auto state = makeState(mesh);
        seedLeftPool(mesh, opts, state);

        ExplicitInertialSolver solver;
        solver.initialize(mesh, state, opts);
        solver.advance(0.0, 600.0);
        double crossed = 0.0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            if (mesh.tri_cx[i] > 8.0)
                crossed += state.volume[i];
        EXPECT_GT(crossed, 0.0) << "expected the legacy centroid zface to leak across the crest";
        solver.finalize();
    }
}

namespace openswmm::twoD {
struct ExplicitInertialSolverTestAccess {
    /**
     * @brief Execute the private coupled RK2 step directly for a numerical fixture.
     *
     * Bypasses the public advance scheduler and calls runRk2Step. State/ledgers may mutate and
     * production exceptions propagate; caller prepares/compares clocks and boundaries explicitly.
     *
     * @param[in,out] solver Initialized/prepared test solver with live borrowed mesh/state/subsurface
     *     owners.
     * @param[in] dt Positive step duration in seconds accepted by the selected profile.
     * @param[in] time Elapsed step beginning in seconds.
     */
    static void rk2(ExplicitInertialSolver& solver, double dt, double time) {
        solver.runRk2Step(dt, time);
    }
    /**
     * @brief Execute the private RK2 bed-contact operator for a test interval.
     *
     * Calls advanceRk2BedContact directly, bypassing public scheduling; contacts/transport/thermal
     * state can mutate and failures propagate.
     *
     * @param[in,out] solver Prepared compatible solver whose borrowed owners remain alive.
     * @param[in] dt Positive contact duration in seconds.
     */
    static void contact(ExplicitInertialSolver& solver, double dt) {
        solver.advanceRk2BedContact(dt);
    }
    /**
     * @brief Synchronize/rebuild fixture solver state at elapsed time zero.
     *
     * Calls private syncAndRebuild(0), so geometry/front/internal caches may change. Does not run a
     * public advance interval or transfer ownership.
     *
     * @param[in,out] solver Initialized solver with valid borrowed mesh/state owners.
     */
    static void prepare(ExplicitInertialSolver& solver) { solver.syncAndRebuild(0); }
    /**
     * @brief Copy internal RK2 reaction diagnostics into a borrowed model context.
     *
     * Calls syncRk2ReactionReport directly; does not itself integrate reactions or advance time.
     *
     * @param[in,out] solver Initialized solver containing reaction reporting state.
     * @param[in,out] ctx Caller-owned compatible reaction context receiving synchronized report values.
     */
    static void reactionReport(ExplicitInertialSolver& solver, openswmm::SimulationContext& ctx) {
        solver.syncRk2ReactionReport(ctx);
    }
    /**
     * @brief Capture selected private solver arrays and clocks for exact state comparisons.
     *
     * Entries retain mixed native internal units. Does not capture every solver member or create a
     * restart archive; callers compare identical layouts only.
     *
     * @param[in] s Borrowed initialized solver, unchanged.
     * @return Owned flattened vector of qcx/qcy, selected face/momentum/BC/source accumulators, step
     *     counters as doubles and sync/source clocks.
     */
    static std::vector<double> state(const ExplicitInertialSolver& s) {
        std::vector<double> values = s.qcx_;
        values.insert(values.end(), s.qcy_.begin(), s.qcy_.end());
        // Keep the historical logical payload independent of private packet storage.
        [&]<class Solver>(const Solver& solver) {
            if constexpr (requires { solver.face_accum_; }) {
                const auto stride = solver.face_accum_stride_;
                const auto count = solver.face_accum_.size() / stride;
                const std::size_t sides = stride == 6 ? 4 : 2;
                for (std::size_t side = 0; side < sides; ++side)
                    for (std::size_t e = 0; e < count; ++e)
                        values.push_back(solver.face_accum_[e * stride + side]);
            } else {
                for (const auto* row : {&solver.facc_L_, &solver.facc_R_,
                                       &solver.macc_x_L_, &solver.macc_x_R_})
                    values.insert(values.end(), row->begin(), row->end());
            }
        }(s);
        for (const auto* row : {&s.bc_accum_, &s.source_ledgers_})
            values.insert(values.end(), row->begin(), row->end());
        values.insert(values.end(), {double(s.substeps_run_), double(s.last_steps_), s.t_last_sync_,
                                     s.source_phase_end_});
        return values;
    }
};
}
namespace {
struct CoupledRkRig {
    MeshData mesh = makeGridMesh(1, 1, 4., [](double, double) { return 0.; });
    SurfaceStateData state = makeState(mesh);
    SolverOptions2D opts;
    ExplicitInertialSolver solver;
    SubsurfaceSolver gw;
    /**
     * @brief Initialize the owned two-cell coupled RK2 water fixture.
     *
     * Uses FULL_SWE/order2 with one LTS tier, a two-metre aquifer seeded at 0.6 m saturated height,
     * then initializes GW against solver-owned edges and attaches it. Fatal assertions require GW
     * acceptance/RK2 profile; partial owned state remains on failure. Rig members must remain
     * alive/unmoved through solver use; no public engine/file lifecycle is involved.
     *
     * @param[in] dt Positive maximum mesh timestep in seconds.
     * @param[in] ks Groundwater saturated conductivity in m/s under default identity unit factors.
     * @param[in] deep Native groundwater deep-loss conductivity in m/s.
     * @param[in] pond Initial absolute uniform free surface in metres over the flat zero bed.
     * @param[in] et Raw groundwater ET policy string.
     * @param[in] closure Requested groundwater closure.
     * @param[in] law Soil characteristic law.
     * @param[in] layers Requested positive groundwater layer count.
     * @param[in] capillary Enable capillary diffusion when true.
     */
    void init(double dt, double ks, double deep, double pond = 0, const std::string& et = "AUTO",
              GwClosure closure = GwClosure::CLOSED_FORM, SoilChar law = SoilChar::RUSSO,
              int layers = 8, bool capillary = false) {
        opts.momentum = Momentum2D::FULL_SWE;
        opts.reconstruction_order = 2;
        opts.lts_tiers = 1;
        opts.max_timestep = dt;
        seedSurface(mesh, opts, state, pond);
        solver.initialize(mesh, state, opts);
        SubsurfaceConfig cfg;
        cfg.options.authored = true;
        cfg.options.closure = closure;
        cfg.options.soil_char = law;
        cfg.options.m_layers = layers;
        cfg.options.capillary_diff = capillary;
        cfg.options.node_auto = false;
        cfg.options.gw_et = et;
        GwAquiferRow row;
        row.Ks = ks;
        row.zs = 2;
        row.hg0 = .6;
        row.c_loss = deep;
        cfg.rows = {row};
        std::vector<std::string> warnings;
        ASSERT_EQ(gw.initialize(mesh, solver.inertialEdges(), opts, {}, 0, cfg, warnings), "");
        ASSERT_TRUE(gw.rk2ProfileError().empty());
        solver.setSubsurface(&gw);
    }
    /**
     * @brief Sum live surface and groundwater water in the coupled rig.
     *
     * Excludes unrelated source-return/queued export ledgers and does not mutate state.
     * @return Total m3 from current surface cell volumes plus gw.liveStorage.
     */
    double total() const { return totalVolume(state, mesh.n_cells()) + gw.state().liveStorage(); }
};
}
namespace {
/**
 * @brief Form one groundwater flow boundary on the first exterior edge of a cell.
 *
 * Throws runtime_error when the cell has no exterior edge. Does not install the boundary, validate
 * signal units or retain rig storage; caller chooses a flow/head mode consistent with the signal.
 *
 * @param[in] rig Borrowed rig with valid mesh edge geometry.
 * @param[in] cell Valid zero-based cell row.
 * @param[in] signal Borrowed groundwater boundary signal copied into the result.
 * @return Owned boundary value with cell/edge/length/distance and copied signal; remaining fields
 *     retain defaults.
 */
GwResolvedBoundary rk2FlowEdge(const CoupledRkRig& rig, int cell, const GwSourceSignal& signal) {
    for (int edge = 0; edge < rig.mesh.cell_nv[cell]; ++edge) {
        const auto slot = MeshData::slot(cell, edge);
        if (rig.mesh.cell_nbr[slot] != -1)
            continue;
        GwResolvedBoundary result;
        result.cell = cell;
        result.edge = edge;
        result.signal = signal;
        result.length = rig.mesh.edge_length[slot];
        result.distance = rig.mesh.edge_dist_c[slot];
        return result;
    }
    throw std::runtime_error("Test cell has no exterior edge");
}
}
TEST(CoupledGroundwaterRk2, AdvancesActualAquiferWithAnalyticalSecondOrderDeepDrainage) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig r;
        r.init(dt, 0, .07);
        auto& w = r.gw.state();
        for (int i = 0; i < w.n_cells; ++i)
            w.hu[i] = w.theta_r[i] * (w.zs[i] - w.hg[i]);
        w.led_init_storage = w.storage();
        const double initial = r.total();
        for (int k = 0; k < int(2 / dt); ++k)
            ASSERT_DOUBLE_EQ(r.solver.advance(k * dt, (k + 1) * dt), (k + 1) * dt);
        const double exact = .6 * std::exp(-.07 * 2 / (2 * (.45 - .1)));
        const double error = std::abs(w.hg[0] - exact);
        RecordProperty("error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, .00004);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.8);
        previous = error;
        EXPECT_NEAR(r.total() + w.led_deep, initial, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
        EXPECT_DOUBLE_EQ(r.gw.cellCompletedTime(0), 2);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceInfiltrationAndAquiferAdvanceTogetherAndRefine) {
    const auto trajectory = [](double dt) {
        CoupledRkRig r;
        r.init(dt, .005, .0005, .2);
        auto& water = r.gw.state();
        for (int i = 0; i < water.n_cells; ++i) {
            water.hu[i] = .35 * (water.zs[i] - water.hg[i]);
            water.wetting_front[i] = .1;
        }
        water.led_init_storage = water.storage();
        const double initial = r.total();
        r.gw.publishInfiltration(r.state, dt, 0);
        const int steps = int(std::round(2 / dt));
        for (int k = 0; k < steps; ++k)
            r.solver.advance(k * dt, (k + 1) * dt);
        const auto& w = r.gw.state();
        EXPECT_GT(w.led_infil_in, 0);
        EXPECT_GT(w.led_recharge, 0);
        EXPECT_NEAR(r.total() + w.led_deep, initial, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
        return std::vector<double>{r.state.volume[0], w.hg[0], w.hu[0]};
    };
    const auto reference = trajectory(.003125);
    double previous = 0;
    for (double dt : {.4, .2, .1}) {
        const auto result = trajectory(dt);
        double square = 0;
        for (int j = 0; j < 3; ++j)
            square += (result[j] - reference[j]) * (result[j] - reference[j]);
        const double error = std::sqrt(square);
        RecordProperty("error_" + std::to_string(int(dt * 1000)), error);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, NonfiniteTrialRollsBackSurfaceGroundwaterMomentumAndReceipts) {
    CoupledRkRig r;
    r.init(.1, .005, 0, .2);
    GwResolvedSource source;
    source.cells = {{0, 1}};
    source.flow.constant = 1e308;
    r.gw.setSources({source});
    r.gw.publishInfiltration(r.state, .1, 0);
    r.gw.assignTiers(.1, 1);
    const auto solver = ExplicitInertialSolverTestAccess::state(r.solver), volume = r.state.volume;
    const auto water = r.gw.state();
    const double initial = r.total();
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4, 0), std::runtime_error);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::state(r.solver), solver);
    EXPECT_EQ(r.state.volume, volume);
    EXPECT_EQ(r.gw.state().hg, water.hg);
    EXPECT_EQ(r.gw.state().hu, water.hu);
    EXPECT_EQ(r.gw.state().xacc_to_surface, water.xacc_to_surface);
    EXPECT_EQ(r.gw.state().xacc_from_surface, water.xacc_from_surface);
    EXPECT_DOUBLE_EQ(r.gw.state().led_source_in, water.led_source_in);
    EXPECT_DOUBLE_EQ(r.total(), initial);
    r.gw.setSources({});
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    EXPECT_NEAR(r.total(), initial, 1e-10 * initial);
}

namespace {
/**
 * @brief Create the numerical RK2 evidence directory and form an artifact path.
 *
 * Creates the root and propagates filesystem errors; does not create individual artifacts, sanitize
 * names or remove old files.
 *
 * @param[in] file Raw filename/path appended below the output root.
 * @return Owned path below SWMM_RK2_TEST_OUT or tests/output/coupled_gw_rk2.
 */
std::filesystem::path rk2Output(const std::string& file) {
    const char* location = std::getenv("SWMM_RK2_TEST_OUT");
    const auto root = std::filesystem::path(location ? location : "tests/output/coupled_gw_rk2");
    std::filesystem::create_directories(root);
    return root / file;
}
/**
 * @brief Read binary numerical evidence into owned storage.
 *
 * Does not parse/validate the artifact; local stream closes at return and allocation exceptions
 * propagate.
 *
 * @param[in] path Borrowed source filename.
 * @return Owned bytes, empty/partial on unchecked open/read failure.
 */
std::string rk2File(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}
}
TEST(CoupledGroundwaterRk2, CompletedCheckpointResumesAllOwnedStateExactly) {
    CoupledRkRig a, b;
    a.init(.05, .005, .0005, .2);
    b.init(.05, .005, .0005, .2);
    a.gw.publishInfiltration(a.state, .05, 0);
    b.gw.publishInfiltration(b.state, .05, 0);
    const auto saved = rk2Output("accepted.chk"), restored = rk2Output("restored.chk");
    for (int k = 0; k < 7; ++k) {
        a.solver.advance(k * .05, (k + 1) * .05);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty()) << k;
    }
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    EXPECT_EQ(rk2File(saved), rk2File(restored));
    for (int k = 7; k < 20; ++k) {
        a.solver.advance(k * .05, (k + 1) * .05);
        b.solver.advance(k * .05, (k + 1) * .05);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
        EXPECT_EQ(rk2File(saved), rk2File(restored)) << k;
    }
}
TEST(InertialMomentum, CheckpointRejectsDifferentReferenceIntervalWithoutMutation) {
    CoupledRkRig a, b;
    a.opts.theta_reference_step = 1.0;
    b.opts.theta_reference_step = 2.0;
    a.init(.05, .005, .0005, .2);
    b.init(.05, .005, .0005, .2);
    a.solver.advance(0, .05);
    const auto saved = rk2Output("momentum-reference.chk");
    const auto before = rk2Output("momentum-before.chk");
    const auto after = rk2Output("momentum-after.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(before.string()).empty());
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.opts.theta_reference_step = 1.0;
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    a.solver.advance(.05, .1);
    b.solver.advance(.05, .1);
    EXPECT_EQ(a.state.volume, b.state.volume);
}

TEST(CoupledGroundwaterRk2, CorruptAndIncompatibleCheckpointRefusalIsAtomicAndRetryWorks) {
    CoupledRkRig a, b;
    a.init(.05, .005, .0005, .2);
    b.init(.05, .005, .0005, .2);
    a.solver.advance(0, .05);
    const auto saved = rk2Output("good.chk"), bad = rk2Output("corrupt.chk"),
               before = rk2Output("fresh.chk"), after = rk2Output("refused.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(before.string()).empty());
    auto bytes = rk2File(saved);
    ASSERT_GT(bytes.size(), 32);
    bytes[bytes.size() / 2] ^= 1;
    {
        std::ofstream file(bad, std::ios::binary);
        file.write(bytes.data(), bytes.size());
    }
    EXPECT_FALSE(b.solver.loadCheckpoint(bad.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(before), rk2File(after));
    const double conductivity = b.gw.state().Ks[0];
    b.gw.state().Ks[0] *= 2;
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    b.gw.state().Ks[0] = conductivity;
    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(saved), rk2File(after));
}
TEST(CoupledGroundwaterRk2, SingularIntakeStartupAndBoundedWellsConserveThroughWettingAndDrying) {
    for (double flow : {100., -100.}) {
        CoupledRkRig r;
        r.init(.025, .005, .0005, .2);
        const double initial = r.total();
        GwResolvedSource source;
        source.cells = {{0, 1}};
        source.flow.constant = flow;
        r.gw.setSources({source});
        for (int k = 0; k < 20; ++k) {
            ASSERT_NO_THROW(r.solver.advance(k * .025, (k + 1) * .025));
            const auto& w = r.gw.state();
            EXPECT_NEAR(r.total() + w.led_deep + w.led_et - w.led_source_in + w.led_source_out,
                        initial, 1e-10 * std::max(1., r.total()));
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * std::max(1., w.led_init_storage));
            for (int i = 0; i < w.n_cells; ++i) {
                EXPECT_GE(w.hg[i], 0);
                EXPECT_LE(w.hg[i], w.zs[i]);
                EXPECT_GE(w.hu[i], 0);
                EXPECT_GE(r.state.volume[i], 0);
            }
        }
        if (flow > 0)
            EXPECT_GT(r.gw.state().led_dunne, 0);
        else {
            EXPECT_GT(r.gw.state().led_source_out, 0);
            EXPECT_LT(r.gw.state().led_source_out, 50);
        }
    }
}

TEST(CoupledGroundwaterRk2, UnsupportedOwnersRefuseBeforeAnyNumericalMutation) {
    CoupledRkRig r;
    r.init(.05, .005, 0, .2);
    const auto before = r.state.volume;
    r.gw.state().closure[0] = int8_t(GwClosure::SIGMA);
    EXPECT_FALSE(r.gw.rk2ProfileError().empty());
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, .05, 0), std::runtime_error);
    r.gw.state().closure[0] = int8_t(GwClosure::CLOSED_FORM);
    GwResolvedSource source;
    source.cells = {{0, 1}};
    source.terms.resize(1);
    r.gw.setSources({source});
    EXPECT_FALSE(r.gw.rk2ProfileError().empty());
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, .05, 0), std::runtime_error);
    r.gw.setSources({});
    r.state.heat.enabled = true;
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, .05, 0), std::runtime_error);
    r.state.heat.enabled = false;
    EXPECT_EQ(r.state.volume, before);
    EXPECT_DOUBLE_EQ(r.gw.cellCompletedTime(0), 0);
}

TEST(CoupledGroundwaterRk2, FailedAdvanceRestoresTheWholeCallBeforeRetry) {
    CoupledRkRig r;
    r.init(.05, .005, 0, .2);
    GwResolvedSource source;
    source.cells = {{0, 1}};
    source.flow.constant = 1e308;
    r.gw.setSources({source});
    const auto before = rk2Output("advance-before.chk"), after = rk2Output("advance-after.chk");
    ASSERT_TRUE(r.solver.saveCheckpoint(before.string()).empty());
    EXPECT_THROW(r.solver.advance(0, 4), std::runtime_error);
    ASSERT_TRUE(r.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.gw.setSources({});
    const double initial = r.total();
    ASSERT_NO_THROW(r.solver.advance(0, .05));
    EXPECT_NEAR(r.total(), initial, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, LateralDarcyRelaxationMatchesAnAnalyticalNonlinearDonorSolution) {
    double previous = 0;
    for (double dt : {.8, .4, .2, .1}) {
        CoupledRkRig r;
        r.init(dt, .005, 0, 0, "NONE");
        auto& w = r.gw.state();
        w.hg[0] = .8;
        w.hg[1] = .4;
        for (int i = 0; i < 2; ++i)
            w.hu[i] = w.theta_r[i] * (w.zs[i] - w.hg[i]);
        w.led_init_storage = w.storage();
        const double initial = r.total();
        const auto& edges = r.solver.inertialEdges();
        ASSERT_EQ(edges.ne, 1);
        const double c = .005 * edges.xi[0] * edges.inv_dx[0] / ((.45 - .1) * w.area[0]);
        const double attenuation = std::exp(-c * 1.2 * 8),
                     difference = 1.2 * .4 * attenuation / (1.2 + .4 * (1 - attenuation));
        for (int k = 0; k < int(std::round(8 / dt)); ++k)
            r.solver.advance(k * dt, (k + 1) * dt);
        const double error = std::abs(w.hg[0] - (1.2 + difference) / 2);
        RecordProperty("error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.8);
        previous = error;
        EXPECT_NEAR(w.hg[0] + w.hg[1], 1.2, 1e-12);
        EXPECT_NEAR(r.total(), initial, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
    }
}
TEST(CoupledGroundwaterRk2, FullStepAndTwoHalfStepsDifferAtThirdLocalOrder) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05, .025}) {
        CoupledRkRig full, half;
        full.init(.4, .005, .0005, .2);
        half.init(.4, .005, .0005, .2);
        for (auto* r : {&full, &half}) {
            auto& w = r->gw.state();
            for (int i = 0; i < w.n_cells; ++i) {
                w.hu[i] = .35 * (w.zs[i] - w.hg[i]);
                w.wetting_front[i] = .1;
            }
            w.led_init_storage = w.storage();
        }
        full.solver.advance(0, dt);
        half.solver.advance(0, dt / 2);
        half.solver.advance(dt / 2, dt);
        double error = 0;
        for (int i = 0; i < 2; ++i)
            error += std::abs(full.state.volume[i] - half.state.volume[i]) +
                     std::abs(full.gw.state().hg[i] - half.gw.state().hg[i]);
        RecordProperty("difference_" + std::to_string(int(dt * 1000)), error);
        // Keep the coarse measurements; require the unchanged local-order
        // gate on four successive steps in the asymptotic range.
        if (previous > 0 && dt <= .1)
            EXPECT_GT(previous / error, 7.);
        previous = error;
    }
}

TEST(CoupledGroundwaterRk2, OptionalOpenmpRechargePreservesStagesRestartAndRollback) {
    // This hardware gate is run with OPENSWMM_2D_BACKEND=omp. Ordinary CPU
    // suites do not require a separately installed optional plugin.
    for (bool head : {false, true}) {
        SCOPED_TRACE(head);
        CoupledRkRig a, b, cpu;
        a.init(.05, .005, .0005, .2);
        b.init(.05, .005, .0005, .2);
        cpu.init(.05, .005, .0005, .2);
        auto backend = makeGwRechargeBackend(a.opts, a.mesh.n_cells());
        if (!backend.evaluate) {
            ASSERT_FALSE(std::getenv("OPENSWMM_GW_TEST_REQUIRE_PLUGIN"));
            GTEST_SKIP() << backend.diagnostic;
        }
        a.gw.setRechargeBackend(backend);
        b.gw.setRechargeBackend(backend);
        GwResolvedSource well;
        well.name = "omp-reversing";
        well.cells = {{0, 1}};
        well.flow.times = {0, .17, .43, 1};
        well.flow.values = {.002, .006, -.002, .001};
        GwSourceSignal edge;
        edge.times = {0, .13, .67, 1};
        edge.values = {.004, -.001, .002, .001};
        for (auto* rig : {&a, &b, &cpu}) {
            auto boundary = rk2FlowEdge(*rig, 1, edge);
            boundary.head = head;
            if (head) {
                const double bottom = rig->gw.state().z_bed[1];
                boundary.signal.values = {bottom + 1, bottom + .2, bottom + 2.5, bottom + .8};
            }
            rig->gw.setSources({well});
            rig->gw.setBoundaries({boundary});
        }
        const std::string prefix = head ? "omp-head" : "omp-flow";
        const auto saved = rk2Output(prefix + "-accepted.chk"),
                   restored = rk2Output(prefix + "-restored.chk"),
                   before = rk2Output(prefix + "-before.chk");
        const double initial = a.total();
        for (int k = 0; k < 20; ++k) {
            a.solver.advance(k * .05, (k + 1) * .05);
            cpu.solver.advance(k * .05, (k + 1) * .05);
            EXPECT_EQ(a.state.volume, cpu.state.volume);
            EXPECT_EQ(a.gw.state().hg, cpu.gw.state().hg);
            EXPECT_EQ(a.gw.state().hu, cpu.gw.state().hu);
            ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
            if (k == 7) {
                ASSERT_TRUE(cpu.solver.saveCheckpoint(before.string()).empty());
                EXPECT_FALSE(cpu.solver.loadCheckpoint(saved.string()).empty());
                ASSERT_TRUE(cpu.solver.saveCheckpoint(restored.string()).empty());
                EXPECT_EQ(rk2File(before), rk2File(restored));
                ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
                EXPECT_EQ(b.gw.rechargeCells(), 0u);
            } else if (k > 7)
                b.solver.advance(k * .05, (k + 1) * .05);
            if (k >= 7) {
                ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
                EXPECT_EQ(rk2File(saved), rk2File(restored));
            }
        }
        EXPECT_GT(a.gw.rechargeCells(), 0u);
        EXPECT_GT(b.gw.rechargeCells(), 0u);
        EXPECT_NEAR(a.total() + a.gw.state().led_deep,
                    initial + a.gw.state().led_source_in - a.gw.state().led_source_out +
                        a.gw.state().led_boundary_in - a.gw.state().led_boundary_out,
                    1e-10 * initial);
        GwResolvedSource bad;
        bad.cells = {{0, 1}};
        bad.flow.constant = 1e308;
        a.gw.setSources({bad});
        const auto batches = a.gw.rechargeBatches(), cells = a.gw.rechargeCells();
        const auto state = ExplicitInertialSolverTestAccess::state(a.solver),
                   volume = a.state.volume;
        EXPECT_THROW(a.solver.advance(1, 5), std::runtime_error);
        EXPECT_EQ(a.gw.rechargeBatches(), batches);
        EXPECT_EQ(a.gw.rechargeCells(), cells);
        EXPECT_EQ(a.state.volume, volume);
        EXPECT_EQ(ExplicitInertialSolverTestAccess::state(a.solver), state);
        a.gw.setSources({});
        ASSERT_NO_THROW(a.solver.advance(1, 1.05));
    }
}

TEST(CoupledGroundwaterRk2, AffineWellAndDeepDrainageConvergeAtSecondOrder) {
    for (bool edge : {false, true}) {
        SCOPED_TRACE(edge);
        double previous = 0;
        for (double dt : {.4, .2, .1, .05}) {
            CoupledRkRig r;
            r.init(dt, 0, .07);
            auto& w = r.gw.state();
            for (int i = 0; i < w.n_cells; ++i)
                w.hu[i] = w.theta_r[i] * (w.zs[i] - w.hg[i]);
            w.led_init_storage = w.storage();
            const double initial = r.total();
            GwResolvedSource well;
            well.name = "ramp";
            well.cells = {{0, 1}};
            well.flow.times = {0, 2};
            well.flow.values = {.003, .009};
            if (edge)
                r.gw.setBoundaries({rk2FlowEdge(r, 0, well.flow)});
            else
                r.gw.setSources({well});
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            for (int k = 0; k < int(2 / dt); ++k)
                ASSERT_DOUBLE_EQ(r.solver.advance(k * dt, (k + 1) * dt), (k + 1) * dt);
            const double yield = w.theta_s[0] - w.theta_r[0], lambda = .07 / (2 * yield),
                         storage = yield * w.area[0];
            const double decay = std::exp(-lambda * 2);
            const double exact = .6 * decay + .003 / storage * (1 - decay) / lambda +
                                 .003 / storage * (2 / lambda - (1 - decay) / (lambda * lambda));
            const double error = std::abs(w.hg[0] - exact);
            RecordProperty(std::string(edge ? "affine_edge_error_" : "affine_well_error_") +
                               std::to_string(int(dt * 1000)),
                           error);
            EXPECT_LT(error, .00004);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.7);
            previous = error;
            EXPECT_NEAR(edge ? w.led_boundary_in : w.led_source_in, .012, 1e-14);
            EXPECT_NEAR(r.total() + w.led_deep, initial + .012, 1e-10 * initial);
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
        }
    }
}

TEST(CoupledGroundwaterRk2, SignedWellKnotsPreserveGrossBudgetsAndExactCheckpointContinuation) {
    CoupledRkRig a, b;
    a.init(.4, .005, .01, .2);
    b.init(.4, .005, .01, .2);
    GwResolvedSource well;
    well.name = "reversing";
    well.cells = {{0, 1}};
    well.flow.times = {0, .15, .35, .7, 1};
    well.flow.values = {.02, .06, -.02, -.04, .02};
    GwResolvedSource other;
    other.name = "asynchronous";
    other.cells = {{1, 1}};
    other.flow.times = {0, .21, .49, 1};
    other.flow.values = {.01, .03, 0, .01};
    a.gw.setSources({well, other});
    b.gw.setSources({well, other});
    ASSERT_TRUE(a.gw.rk2ProfileError().empty());
    const double initial = a.total();
    ASSERT_DOUBLE_EQ(a.solver.advance(0, .45), .45);
    const auto saved = rk2Output("timed-well.chk"), restored = rk2Output("timed-well-restored.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    auto changed = well;
    changed.flow.values[1] += .001;
    b.gw.setSources({changed, other});
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    const auto before = rk2File(restored);
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    EXPECT_EQ(rk2File(restored), before);
    b.gw.setSources({well, other});
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    for (double end : {.8, 1.}) {
        ASSERT_DOUBLE_EQ(a.solver.advance(a.gw.cellCompletedTime(0), end), end);
        ASSERT_DOUBLE_EQ(b.solver.advance(b.gw.cellCompletedTime(0), end), end);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
        EXPECT_EQ(rk2File(saved), rk2File(restored));
    }
    // Independent trapezoids split where the prescribed straight lines cross zero.
    const double incoming =
        .006 + .5 * .15 * .06 + .5 * .1 * .02 + .21 * .02 + .28 * .015 + .51 * .005;
    const double outgoing = .5 * .05 * .02 + .35 * .03 + .5 * .2 * .04;
    const auto& w = a.gw.state();
    EXPECT_NEAR(w.led_source_in, incoming, 1e-13);
    EXPECT_NEAR(w.led_source_out, outgoing, 1e-13);
    EXPECT_NEAR(a.total() + w.led_deep, initial + incoming - outgoing, 1e-10 * initial);
    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, OversubscribedTimedWellRemainsBoundedAndConservative) {
    CoupledRkRig r;
    r.init(.2, 0, 0);
    const double initial = r.total();
    GwResolvedSource well;
    well.name = "pump-cycle";
    well.cells = {{0, 1}};
    well.flow.times = {0, .2, .6, 1};
    well.flow.values = {-100, -100, 1, 1};
    r.gw.setSources({well});
    ASSERT_TRUE(r.gw.rk2ProfileError().empty());
    ASSERT_DOUBLE_EQ(r.solver.advance(0, 1), 1);
    const auto& w = r.gw.state();
    const double positive = .4 + .5 * (.4 / 101);
    EXPECT_NEAR(w.led_source_in, positive, 1e-12);
    EXPECT_GT(w.led_source_out, 0);
    EXPECT_LT(w.led_source_out, 40);
    for (int cell = 0; cell < w.n_cells; ++cell) {
        EXPECT_TRUE(std::isfinite(w.hg[cell]));
        EXPECT_GE(w.hg[cell], 0);
        EXPECT_LE(w.hg[cell], w.zs[cell]);
        EXPECT_GE(w.hu[cell], 0);
        EXPECT_LE(w.hu[cell], w.theta_s[cell] * (w.zs[cell] - w.hg[cell]) + 1e-12);
    }
    EXPECT_NEAR(r.total(), initial + w.led_source_in - w.led_source_out, 1e-10 * initial);
    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, LaterWellIntervalFailureRollsBackEarlierAcceptedStages) {
    CoupledRkRig r, control;
    r.init(.1, .005, 0, .2);
    control.init(.1, .005, 0, .2);
    GwResolvedSource well;
    well.name = "overflow";
    well.cells = {{0, 1}};
    well.flow.times = {0, .5, 4.5};
    well.flow.values = {.001, .001, 1e308};
    r.gw.setSources({well});
    control.gw.setSources({well});
    for (auto* rig : {&r, &control}) {
        rig->gw.publishInfiltration(rig->state, .1, 0);
        rig->gw.assignTiers(.1, 1);
    }
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver, .5, 0));
    EXPECT_NEAR(control.gw.state().led_source_in, .0005, 1e-14);
    const auto numerical = ExplicitInertialSolverTestAccess::state(r.solver),
               volume = r.state.volume;
    const auto water = r.gw.state();
    const double initial = r.total();
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4.5, 0), std::runtime_error);
    EXPECT_EQ(ExplicitInertialSolverTestAccess::state(r.solver), numerical);
    EXPECT_EQ(r.state.volume, volume);
    EXPECT_EQ(r.gw.state().hg, water.hg);
    EXPECT_EQ(r.gw.state().hu, water.hu);
    EXPECT_EQ(r.gw.state().xacc_to_surface, water.xacc_to_surface);
    EXPECT_EQ(r.gw.state().xacc_from_surface, water.xacc_from_surface);
    EXPECT_DOUBLE_EQ(r.gw.state().led_source_in, water.led_source_in);
    EXPECT_DOUBLE_EQ(r.total(), initial);
    r.gw.setSources({});
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    EXPECT_NEAR(r.total(), initial, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, FlowEdgesShareWellStagesAndPreserveRestartDiagnostics) {
    CoupledRkRig a, b;
    a.init(.4, .005, .01, .2);
    b.init(.4, .005, .01, .2);
    GwResolvedSource well;
    well.name = "well-and-edge";
    well.cells = {{0, 1}};
    well.flow.times = {0, .15, .35, .7, 1};
    well.flow.values = {.02, .06, -.02, -.04, .02};
    GwSourceSignal signal;
    signal.times = {0, .31, .83, 1};
    signal.values = {.04, -.02, .01, .03};
    const auto edge = rk2FlowEdge(a, 1, signal);
    a.gw.setSources({well});
    b.gw.setSources({well});
    a.gw.setBoundaries({edge});
    b.gw.setBoundaries({edge});
    ASSERT_TRUE(a.gw.rk2ProfileError().empty());
    const double initial = a.total();
    ASSERT_DOUBLE_EQ(a.solver.advance(0, .45), .45);
    const auto saved = rk2Output("flow-edge.chk"), restored = rk2Output("flow-edge-restored.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    auto changed = edge;
    changed.signal.values[1] += .001;
    b.gw.setBoundaries({changed});
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    const auto before = rk2File(restored);
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    EXPECT_EQ(rk2File(restored), before);
    b.gw.setBoundaries({edge});
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    for (double end : {.8, 1.}) {
        ASSERT_DOUBLE_EQ(a.solver.advance(a.gw.cellCompletedTime(0), end), end);
        ASSERT_DOUBLE_EQ(b.solver.advance(b.gw.cellCompletedTime(0), end), end);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
        EXPECT_EQ(rk2File(saved), rk2File(restored));
    }
    // Independent signed trapezoids: well +0.0115/-0.015, edge +0.0084/-0.0045 m3.
    const auto& w = a.gw.state();
    EXPECT_NEAR(w.led_source_in, .0115, 1e-13);
    EXPECT_NEAR(w.led_source_out, .015, 1e-13);
    EXPECT_NEAR(w.led_boundary_in, .0084, 1e-13);
    EXPECT_NEAR(w.led_boundary_out, .0045, 1e-13);
    const auto& receipt = a.gw.boundaries().front();
    EXPECT_NEAR(receipt.requested_in, .0084, 1e-13);
    EXPECT_NEAR(receipt.applied_in, .0084, 1e-13);
    EXPECT_NEAR(receipt.requested_out, .0045, 1e-13);
    EXPECT_NEAR(receipt.applied_out, .0045, 1e-13);
    EXPECT_NEAR(a.total() + w.led_deep, initial + .0115 + .0084 - .015 - .0045, 1e-10 * initial);
    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, FlowEdgePumpingCapsGrossAcceptedWithdrawals) {
    CoupledRkRig r;
    r.init(.2, 0, 0);
    const double initial = r.total();
    GwSourceSignal signal;
    signal.times = {0, .2, .6, 1};
    signal.values = {-100, -100, 1, 1};
    r.gw.setBoundaries({rk2FlowEdge(r, 0, signal)});
    ASSERT_TRUE(r.gw.rk2ProfileError().empty());
    ASSERT_DOUBLE_EQ(r.solver.advance(0, 1), 1);
    const auto& w = r.gw.state();
    const auto& receipt = r.gw.boundaries().front();
    EXPECT_NEAR(receipt.requested_in, .4 + .5 * (.4 / 101), 1e-12);
    EXPECT_NEAR(receipt.requested_out, 20 + .5 * (.4 * 100 / 101) * 100, 1e-12);
    EXPECT_LT(receipt.applied_out, receipt.requested_out);
    EXPECT_GT(receipt.applied_out, 0);
    EXPECT_DOUBLE_EQ(receipt.applied_out, w.led_boundary_out);
    EXPECT_DOUBLE_EQ(receipt.applied_in, w.led_boundary_in);
    for (int cell = 0; cell < w.n_cells; ++cell) {
        EXPECT_TRUE(std::isfinite(w.hg[cell]));
        EXPECT_GE(w.hg[cell], 0);
        EXPECT_LE(w.hg[cell], w.zs[cell]);
        EXPECT_GE(w.hu[cell], 0);
        EXPECT_LE(w.hu[cell], w.theta_s[cell] * (w.zs[cell] - w.hg[cell]) + 1e-12);
    }
    EXPECT_NEAR(r.total(), initial + w.led_boundary_in - w.led_boundary_out, 1e-10 * initial);
    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, LaterFlowEdgeFailureRollsBackReceiptsAndUnqualifiedOwnersRefuse) {
    CoupledRkRig r, control;
    r.init(.1, .005, 0, .2);
    control.init(.1, .005, 0, .2);
    GwSourceSignal signal;
    signal.times = {0, .5, 4.5};
    signal.values = {.001, .001, 1e308};
    const auto edge = rk2FlowEdge(r, 0, signal);
    r.gw.setBoundaries({edge});
    control.gw.setBoundaries({edge});
    for (auto* rig : {&r, &control}) {
        rig->gw.publishInfiltration(rig->state, .1, 0);
        rig->gw.assignTiers(.1, 1);
    }
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver, .5, 0));
    EXPECT_NEAR(control.gw.boundaries().front().applied_in, .0005, 1e-14);
    const auto before = rk2Output("flow-edge-before.chk"), after = rk2Output("flow-edge-after.chk");
    ASSERT_TRUE(r.solver.saveCheckpoint(before.string()).empty());
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4.5, 0), std::runtime_error);
    ASSERT_TRUE(r.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(after), rk2File(before));
    auto head = edge;
    head.head = true;
    head.terms.resize(1);
    r.gw.setBoundaries({head});
    EXPECT_FALSE(r.gw.rk2ProfileError().empty());
    const auto water = r.gw.state().hg;
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    EXPECT_EQ(r.gw.state().hg, water);
    head.head = false;
    head.terms.resize(1);
    r.gw.setBoundaries({head});
    EXPECT_FALSE(r.gw.rk2ProfileError().empty());
    r.gw.setBoundaries({});
    const double initial = r.total();
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    EXPECT_NEAR(r.total(), initial, 1e-10 * initial);
}

TEST(CoupledGroundwaterRk2, ConstantHeadEdgesConvergeToTheExactUnconfinedSolution) {
    for (double head : {.2, 1.}) {
        double previous = 0;
        for (double dt : {.8, .4, .2, .1}) {
            SCOPED_TRACE(head);
            SCOPED_TRACE(dt);
            CoupledRkRig r;
            r.init(dt, .02, 0, 0, "NONE");
            auto& w = r.gw.state();
            for (int cell = 0; cell < w.n_cells; ++cell)
                w.hu[cell] = w.theta_r[cell] * (w.zs[cell] - w.hg[cell]);
            w.led_init_storage = w.storage();
            const double initial = r.total();
            GwSourceSignal signal;
            signal.constant = w.z_bed[0] + head;
            auto first = rk2FlowEdge(r, 0, signal), second = rk2FlowEdge(r, 1, signal);
            first.head = second.head = true;
            ASSERT_DOUBLE_EQ(first.length, second.length);
            ASSERT_DOUBLE_EQ(first.distance, second.distance);
            r.gw.setBoundaries({first, second});
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            const double k = .02 * .5 * first.length / (first.distance * (.45 - .1) * w.area[0]);
            const double ratio = (.6 - head) / (.6 + head) * std::exp(-2 * head * k * 8);
            const double exact = head * (1 + ratio) / (1 - ratio);
            for (int tick = 0; tick < int(std::round(8 / dt)); ++tick)
                r.solver.advance(tick * dt, (tick + 1) * dt);
            const double error = std::abs(w.hg[0] - exact);
            EXPECT_LT(error, 1e-4);
            RecordProperty("head_" + std::to_string(int(head * 10)) + "_error_" +
                               std::to_string(int(dt * 1000)),
                           error);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.7);
            previous = error;
            EXPECT_DOUBLE_EQ(w.hg[0], w.hg[1]);
            EXPECT_NEAR(r.total(), initial + w.led_boundary_in - w.led_boundary_out,
                        1e-10 * initial);
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
        }
    }
}

namespace {
/**
 * @brief Build the fixed piecewise-linear head cycle used by independent drainage tests.
 *
 * Copies fixed relative values 1/-0.25/2.5/0.4 m above bottom; does not
 * validate/install/interpolate the signal here.
 *
 * @param[in] bottom Aquifer-bottom datum elevation in metres.
 * @return Owned signal at 0/0.25/0.75/1 seconds with datum-adjusted heads.
 */
GwSourceSignal rk2HeadCycle(double bottom) {
    GwSourceSignal signal;
    signal.times = {0, .25, .75, 1};
    signal.values = {bottom + 1, bottom - .25, bottom + 2.5, bottom + .4};
    return signal;
}
/**
 * @brief Build one prescribed-head boundary for each of the two fixture cells.
 *
 * Uses rk2FlowEdge geometry and propagates missing-edge errors; does not install boundaries or
 * retain borrowed references.
 *
 * @param[in] rig Borrowed two-cell rig, each cell having an exterior edge.
 * @param[in] signal Borrowed datum-head signal in metres, copied into both rows.
 * @return Owned two-entry boundary vector with head flags set true.
 */
std::vector<GwResolvedBoundary> rk2HeadEdges(const CoupledRkRig& rig,
                                             const GwSourceSignal& signal) {
    auto a = rk2FlowEdge(rig, 0, signal), b = rk2FlowEdge(rig, 1, signal);
    a.head = b.head = true;
    return {a, b};
}
/**
 * @brief Integrate the independent clipped-head boundary ODE with classical RK4.
 *
 * Uses fixed 0.6 m initial height, porosity difference 0.35, conductivity scale 0.02*0.5 and a
 * two-metre thickness bound. Does not call production forcing/integrators or validate numerical
 * stability; caller chooses enough steps to stay in the intended smooth regime.
 *
 * @param[in] length Positive boundary length in metres.
 * @param[in] distance Positive boundary-to-cell distance in metres.
 * @param[in] area Positive receiving cell area in m2.
 * @param[in] steps Positive uniform step count over one second, default 800000.
 * @return Array of final saturated height m, integrated inflow m3 and integrated outflow m3.
 */
std::array<long double, 3> headCycleReference(double length, double distance, double area,
                                              int steps = 800000) {
    // Independent normalized-height ODE: pressure head is clipped at the
    // bottom, wetted thickness at [0,2]. No production forcing/integrator calls.
    const long double scale = .02L * .5L * length / distance, storage = .35L * area;
    const auto rhs = [&](long double t, long double h) {
        const long double prescribed = t <= .25L   ? 1 - 5 * t
                                       : t <= .75L ? -.25L + 5.5L * (t - .25L)
                                                   : 2.5L - 8.4L * (t - .75L);
        const long double outside = std::max(0.L, prescribed), wet = std::min(2.L, outside);
        const long double q = scale * (h + wet) * (outside - h);
        return std::array<long double, 3>{q / storage, std::max(0.L, q), std::max(0.L, -q)};
    };
    std::array<long double, 3> result{.6L, 0, 0};
    const long double dt = 1.L / steps;
    for (int n = 0; n < steps; ++n) {
        const long double t = n * dt;
        const auto a = rhs(t, result[0]), b = rhs(t + dt / 2, result[0] + dt * a[0] / 2),
                   c = rhs(t + dt / 2, result[0] + dt * b[0] / 2),
                   d = rhs(t + dt, result[0] + dt * c[0]);
        for (int row = 0; row < 3; ++row)
            result[row] += dt * (a[row] + 2 * b[row] + 2 * c[row] + d[row]) / 6;
    }
    return result;
}
/**
 * @brief Set unsaturated cells to residual content and refresh the initial water baseline.
 *
 * Sets hu=theta_r*(zs-hg) for every cell and resets led_init_storage to storage(). Does not rebuild
 * SIGMA profiles, clear pending transfers or reset other ledgers; intended before the trial
 * advance.
 *
 * @param[in,out] rig Initialized groundwater rig; hu and led_init_storage are replaced.
 */
void residualGround(CoupledRkRig& rig) {
    auto& w = rig.gw.state();
    for (int cell = 0; cell < w.n_cells; ++cell)
        w.hu[cell] = w.theta_r[cell] * (w.zs[cell] - w.hg[cell]);
    w.led_init_storage = w.storage();
}
}

TEST(CoupledGroundwaterRk2, TimedClippedHeadEdgesMatchAnIndependentScalarReference) {
    double previous = 0;
    for (double dt : {.05, .025, .0125, .00625, .003125, .0015625}) {
        SCOPED_TRACE(dt);
        CoupledRkRig r;
        r.init(dt, .02, 0, 0, "NONE");
        residualGround(r);
        auto& w = r.gw.state();
        const auto edges = rk2HeadEdges(r, rk2HeadCycle(w.z_bed[0]));
        r.gw.setBoundaries(edges);
        const double initial = r.total();
        const auto reference = headCycleReference(edges[0].length, edges[0].distance, w.area[0]);
        if (dt == .05) {
            const auto refined =
                headCycleReference(edges[0].length, edges[0].distance, w.area[0], 1600000);
            for (int row = 0; row < 3; ++row)
                EXPECT_NEAR(double(reference[row]), double(refined[row]), 1e-13);
        }
        for (int tick = 0; tick < int(std::round(1 / dt)); ++tick)
            r.solver.advance(tick * dt, (tick + 1) * dt);
        const auto& receipt = r.gw.boundaries()[0];
        const double storage = .35 * w.area[0];
        const double error =
            std::max({std::abs(w.hg[0] - double(reference[0])),
                      std::abs(receipt.applied_in - double(reference[1])) / storage,
                      std::abs(receipt.applied_out - double(reference[2])) / storage});
        RecordProperty("head_cycle_error_" + std::to_string(int(dt * 1e6)), error);
        EXPECT_LT(error, 1e-4);
        // Keep coarse event/cancellation errors as evidence. The four
        // finest steps qualify the asymptotic rate; monotone coarse error
        // reduction across head reversals/clipping is not asserted.
        if (dt <= .00625)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        EXPECT_GT(receipt.applied_in, 0);
        EXPECT_GT(receipt.applied_out, 0);
        EXPECT_NEAR(r.total(), initial + w.led_boundary_in - w.led_boundary_out, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
    }
}

TEST(CoupledGroundwaterRk2, TimedHeadCheckpointRefusalIsAtomicAndContinuationIsExact) {
    CoupledRkRig a, b;
    a.init(.05, .02, 0, 0, "NONE");
    b.init(.05, .02, 0, 0, "NONE");
    residualGround(a);
    residualGround(b);
    const auto edges = rk2HeadEdges(a, rk2HeadCycle(a.gw.state().z_bed[0]));
    a.gw.setBoundaries(edges);
    b.gw.setBoundaries(edges);
    ASSERT_DOUBLE_EQ(a.solver.advance(0, .37), .37);
    const auto saved = rk2Output("head-cycle.chk"), restored = rk2Output("head-cycle-restored.chk");
    ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
    auto changed = edges;
    changed[0].signal.values[1] += .001;
    b.gw.setBoundaries(changed);
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    const auto before = rk2File(restored);
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
    EXPECT_EQ(rk2File(restored), before);
    b.gw.setBoundaries(edges);
    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
    for (double end : {.71, 1.}) {
        ASSERT_DOUBLE_EQ(a.solver.advance(a.gw.cellCompletedTime(0), end), end);
        ASSERT_DOUBLE_EQ(b.solver.advance(b.gw.cellCompletedTime(0), end), end);
        ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
        ASSERT_TRUE(b.solver.saveCheckpoint(restored.string()).empty());
        EXPECT_EQ(rk2File(saved), rk2File(restored));
    }
}

TEST(CoupledGroundwaterRk2, HeadDrivenSaturationAndDrainageConserveAcrossBoundedEvents) {
    for (double height : {-5., 10.}) {
        SCOPED_TRACE(height);
        CoupledRkRig r;
        r.init(.05, .1, 0, 0, "NONE");
        residualGround(r);
        auto& w = r.gw.state();
        GwSourceSignal signal;
        signal.constant = w.z_bed[0] + height;
        r.gw.setBoundaries(rk2HeadEdges(r, signal));
        const double initial = r.total();
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        if (height > 0)
            EXPECT_GT(w.led_dunne, 0);
        else
            EXPECT_GT(w.led_boundary_out, 0);
        for (int cell = 0; cell < w.n_cells; ++cell) {
            EXPECT_TRUE(std::isfinite(w.hg[cell]));
            EXPECT_GE(w.hg[cell], 0);
            EXPECT_LE(w.hg[cell], w.zs[cell]);
            EXPECT_GE(w.hu[cell], 0);
            EXPECT_LE(w.hu[cell], w.theta_s[cell] * (w.zs[cell] - w.hg[cell]) + 1e-12);
        }
        EXPECT_NEAR(r.total(), initial + w.led_boundary_in - w.led_boundary_out, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
    }
}

TEST(CoupledGroundwaterRk2, LaterHeadFailureRestoresEveryPriorStageAndReceipt) {
    CoupledRkRig r, control;
    r.init(.1, 1, 0, 0, "NONE");
    control.init(.1, 1, 0, 0, "NONE");
    residualGround(r);
    residualGround(control);
    GwSourceSignal signal;
    const double base = r.gw.state().z_bed[0];
    signal.times = {0, .5, 4.5};
    signal.values = {base + .6, base + .6, 1e308};
    GwResolvedSource well;
    well.name = "head-failure-control";
    well.cells = {{0, 1}};
    well.flow.constant = .001;
    for (auto* rig : {&r, &control}) {
        rig->gw.setBoundaries(rk2HeadEdges(*rig, signal));
        rig->gw.setSources({well});
        rig->gw.publishInfiltration(rig->state, .1, 0);
        rig->gw.assignTiers(.1, 1);
    }
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver, .5, 0));
    EXPECT_NEAR(control.gw.state().led_source_in, .0005, 1e-14);
    const auto before = rk2Output("head-overflow-before.chk"),
               after = rk2Output("head-overflow-after.chk");
    ASSERT_TRUE(r.solver.saveCheckpoint(before.string()).empty());
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4.5, 0), std::runtime_error);
    ASSERT_TRUE(r.solver.saveCheckpoint(after.string()).empty());
    EXPECT_EQ(rk2File(before), rk2File(after));
    signal.constant = base + .6;
    signal.times.clear();
    signal.values.clear();
    r.gw.setBoundaries(rk2HeadEdges(r, signal));
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    EXPECT_NEAR(r.gw.state().continuityResidual(), 0, 1e-9);
}

namespace {
/**
 * @brief Integrate the fixed smooth enslaved-Gardner drainage height reference.
 *
 * Uses a local scalar rate formula and fixed two-metre column/0.35 porosity gap; no production
 * solver calls or bound protection occur. Caller chooses a resolved timestep within the smooth
 * physical regime.
 *
 * @param[in] steps Positive RK4 subdivision count over two seconds.
 * @return Final saturated height in metres from initial 0.6 m.
 */
double enslavedGardnerDrainageReference(int steps) {
    const double dt = 2. / steps;
    const auto rate = [](double h) { return -.07 * h / (2 * .35 * (-std::expm1(-2 * (2 - h)))); };
    double h = .6;
    for (int k = 0; k < steps; ++k) {
        const double a = rate(h), b = rate(h + .5 * dt * a), c = rate(h + .5 * dt * b),
                     d = rate(h + dt * c);
        h += dt * (a + 2 * b + 2 * c + d) / 6;
    }
    return h;
}
}
TEST(CoupledGroundwaterRk2, EnslavedNonlinearStorageDrainageConvergesAtSecondOrder) {
    const double reference = enslavedGardnerDrainageReference(20000);
    ASSERT_NEAR(reference, enslavedGardnerDrainageReference(40000), 1e-13);
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig r;
        r.init(dt, 0, .07, 0, "NONE", GwClosure::ENSLAVED, SoilChar::GARDNER);
        ASSERT_TRUE(r.gw.rk2ProfileError().empty());
        const double initial = r.total();
        for (int k = 0; k < int(std::round(2 / dt)); ++k)
            r.solver.advance(k * dt, (k + 1) * dt);
        const auto& w = r.gw.state();
        const double error = std::abs(w.hg[0] - reference);
        RecordProperty("enslaved_drainage_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.7);
        previous = error;
        EXPECT_NEAR(r.total() + w.led_deep, initial, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
        EXPECT_DOUBLE_EQ(r.gw.cellCompletedTime(0), 2);
    }
}
TEST(CoupledGroundwaterRk2, EnslavedBlendConservesWaterAndRecoversEquilibriumForEverySoilLaw) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN}) {
        SCOPED_TRACE(int(law));
        for (const auto heads : {std::pair{.1, 1.1}, std::pair{0., 2.}, std::pair{1.85, 1.95}}) {
            CoupledRkRig r;
            r.init(.1, 0, 0, 0, "NONE", GwClosure::ENSLAVED, law);
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            soil::Params p;
            p.law = law;
            auto& w = r.gw.state();
            for (int i = 0; i < w.n_cells; ++i) {
                w.hg[i] = heads.first;
                w.hu[i] = soil::equilibriumStorage(p, w.zs[i] - w.hg[i]);
            }
            const auto initial = r.gw;
            for (int i = 0; i < w.n_cells; ++i) {
                w.hg[i] = heads.second;
                w.hu[i] = soil::equilibriumStorage(p, w.zs[i] - w.hg[i]);
            }
            const double target = .5 * (w.storage() + initial.state().storage());
            ASSERT_NO_THROW(r.gw.blendRk2Water(initial, .1));
            EXPECT_NEAR(w.storage(), target, 2e-14);
            for (int i = 0; i < w.n_cells; ++i) {
                EXPECT_DOUBLE_EQ(w.hu[i], soil::equilibriumStorage(p, w.zs[i] - w.hg[i]));
                EXPECT_GE(w.hg[i], 0);
                EXPECT_LE(w.hg[i], w.zs[i]);
                if (heads.first == .1)
                    EXPECT_GT(std::abs(w.hg[i] - .5 * (heads.first + heads.second)), 1e-4);
            }
        }
    }
}
TEST(CoupledGroundwaterRk2, EnslavedSurfaceIntakeAndTableRemainConservativeAndRefine) {
    const auto trajectory = [](double dt) {
        CoupledRkRig r;
        r.init(dt, .005, .0005, .2, "NONE", GwClosure::ENSLAVED, SoilChar::GARDNER);
        if (!r.gw.rk2ProfileError().empty())
            throw std::runtime_error(r.gw.rk2ProfileError());
        auto& w = r.gw.state();
        std::fill(w.wetting_front.begin(), w.wetting_front.end(), .1);
        const double initial = r.total();
        soil::Params p;
        p.law = SoilChar::GARDNER;
        for (int k = 0; k < int(std::round(2 / dt)); ++k)
            r.solver.advance(k * dt, (k + 1) * dt);
        EXPECT_GT(w.led_infil_in, 0);
        EXPECT_NEAR(r.total() + w.led_deep, initial, 1e-10 * initial);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
        for (int i = 0; i < w.n_cells; ++i)
            EXPECT_DOUBLE_EQ(w.hu[i], soil::equilibriumStorage(p, w.zs[i] - w.hg[i]));
        return std::vector<double>{r.state.volume[0], w.hg[0], w.hu[0]};
    };
    std::vector<double> reference;
    ASSERT_NO_THROW(reference = trajectory(.0015625));
    double previous = 0;
    for (double dt : {.2, .1, .05, .025, .0125}) {
        const auto actual = trajectory(dt);
        double error = 0;
        for (std::size_t i = 0; i < actual.size(); ++i)
            error += std::pow(actual[i] - reference[i], 2);
        error = std::sqrt(error);
        RecordProperty("enslaved_intake_error_" + std::to_string(int(dt * 1000)), error);
        // Retain the coarse result; require the frozen rate on four finer steps.
        EXPECT_LT(error, 1e-4);
        if (previous > 0 && dt <= .05)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
    }
}

TEST(CoupledGroundwaterRk2, EnslavedForcingRestartAndLateFailureRollbackPreserveEveryOwnedState) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN})
        for (bool head : {false, true}) {
            SCOPED_TRACE(int(law));
            SCOPED_TRACE(head);
            CoupledRkRig a, b;
            a.init(.05, .005, .001, .1, "NONE", GwClosure::ENSLAVED, law);
            b.init(.05, .005, .001, .1, "NONE", GwClosure::ENSLAVED, law);
            ASSERT_TRUE(a.gw.rk2ProfileError().empty());
            ASSERT_TRUE(b.gw.rk2ProfileError().empty());
            // A second closure shares the same stage transaction and lateral face.
            if (!head)
                for (auto* rig : {&a, &b}) {
                    auto& w = rig->gw.state();
                    w.closure[1] = int8_t(GwClosure::CLOSED_FORM);
                    w.hu[1] = .25 * (w.zs[1] - w.hg[1]);
                    w.led_init_storage = w.storage();
                }
            GwResolvedSource well;
            well.name = "enslaved-reversing";
            well.cells = {{0, 1}};
            well.flow.times = {0, .17, .43, 1};
            well.flow.values = {.02, .06, -.02, .01};
            GwSourceSignal edge;
            edge.times = {0, .13, .67, 1};
            edge.values = {.01, -.01, .02, .01};
            for (auto* rig : {&a, &b}) {
                auto boundary = rk2FlowEdge(*rig, 1, edge);
                boundary.head = head;
                if (head) {
                    const double bottom = rig->gw.state().z_bed[1];
                    boundary.signal.values = {bottom + 1.1, bottom + .2, bottom + 2.5, bottom + .8};
                }
                rig->gw.setSources({well});
                rig->gw.setBoundaries({boundary});
            }
            const auto prefix = "enslaved-" + std::to_string(int(law)) + "-" + std::to_string(head);
            const auto saved = rk2Output(prefix + "-accepted.chk"),
                       after = rk2Output(prefix + "-after.chk"),
                       fresh = rk2Output(prefix + "-fresh.chk");
            const double initial = a.total();
            for (int k = 0; k < 20; ++k) {
                ASSERT_NO_THROW(a.solver.advance(k * .05, (k + 1) * .05));
                ASSERT_TRUE(a.solver.saveCheckpoint(saved.string()).empty());
                if (k == 7) {
                    ASSERT_TRUE(b.solver.saveCheckpoint(fresh.string()).empty());
                    const auto closure = b.gw.state().closure[0];
                    b.gw.state().closure[0] = int8_t(GwClosure::CLOSED_FORM);
                    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
                    b.gw.state().closure[0] = closure;
                    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
                    EXPECT_EQ(rk2File(fresh), rk2File(after));
                    ASSERT_TRUE(b.solver.loadCheckpoint(saved.string()).empty());
                } else if (k > 7)
                    ASSERT_NO_THROW(b.solver.advance(k * .05, (k + 1) * .05));
                if (k >= 7) {
                    ASSERT_TRUE(b.solver.saveCheckpoint(after.string()).empty());
                    EXPECT_EQ(rk2File(saved), rk2File(after));
                }
            }
            const auto& w = a.gw.state();
            EXPECT_GT(w.led_boundary_in, 0);
            EXPECT_GT(w.led_boundary_out, 0);
            EXPECT_NEAR(a.total() + w.led_deep + w.led_source_out + w.led_boundary_out,
                        initial + w.led_source_in + w.led_boundary_in, 1e-10 * initial);
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
            GwResolvedSource bad;
            bad.cells = {{0, 1}};
            bad.flow.times = {1, 1.13, 1.67, 9};
            bad.flow.values = {.01, .02, 1e308, 1e308};
            a.gw.setSources({bad});
            ASSERT_TRUE(a.solver.saveCheckpoint(fresh.string()).empty());
            EXPECT_THROW(a.solver.advance(1, 9), std::runtime_error);
            ASSERT_TRUE(a.solver.saveCheckpoint(after.string()).empty());
            EXPECT_EQ(rk2File(fresh), rk2File(after));
            a.gw.setSources({well});
            ASSERT_NO_THROW(a.solver.advance(1, 1.05));
        }
}
TEST(CoupledGroundwaterRk2, EnslavedBoundsAndInvalidEquilibriumHaveExplicitConservationAndRefusal) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN})
        for (double flow : {100., -100.}) {
            SCOPED_TRACE(int(law));
            SCOPED_TRACE(flow);
            CoupledRkRig r;
            r.init(.025, .005, .0005, 0, "NONE", GwClosure::ENSLAVED, law);
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            auto& w = r.gw.state();
            const double initial = r.total();
            GwResolvedSource source;
            source.cells = {{0, 1}};
            source.flow.constant = flow;
            r.gw.setSources({source});
            soil::Params p;
            p.law = law;
            for (int k = 0; k < 20; ++k) {
                ASSERT_NO_THROW(r.solver.advance(k * .025, (k + 1) * .025));
                EXPECT_NEAR(r.total() + w.led_deep + w.led_source_out, initial + w.led_source_in,
                            1e-10 * std::max(initial, r.total()));
                EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * std::max(initial, r.total()));
                for (int i = 0; i < w.n_cells; ++i) {
                    EXPECT_GE(w.hg[i], 0);
                    EXPECT_LE(w.hg[i], w.zs[i]);
                    EXPECT_GE(r.state.volume[i], 0);
                    EXPECT_DOUBLE_EQ(w.hu[i], soil::equilibriumStorage(p, w.zs[i] - w.hg[i]));
                }
            }
            if (flow > 0)
                EXPECT_GT(w.led_dunne, 0);
            else
                EXPECT_LT(w.led_source_out, 50);
            const auto volume = r.state.volume;
            const auto clocks = ExplicitInertialSolverTestAccess::state(r.solver);
            w.hu[0] += .001;
            EXPECT_FALSE(r.gw.rk2ProfileError().empty());
            EXPECT_THROW(r.solver.advance(.5, .525), std::runtime_error);
            EXPECT_EQ(r.state.volume, volume);
            EXPECT_EQ(ExplicitInertialSolverTestAccess::state(r.solver), clocks);
        }
}

TEST(CoupledGroundwaterRk2, EnslavedFullStepAndTwoHalfStepsHaveThirdLocalOrder) {
    double previous = 0;
    for (double dt : {.1, .05, .025, .0125, .00625}) {
        CoupledRkRig full, half;
        for (auto* rig : {&full, &half}) {
            rig->init(.4, .005, .0005, .2, "NONE", GwClosure::ENSLAVED, SoilChar::GARDNER);
            ASSERT_TRUE(rig->gw.rk2ProfileError().empty());
            std::fill(rig->gw.state().wetting_front.begin(), rig->gw.state().wetting_front.end(),
                      .1);
        }
        full.solver.advance(0, dt);
        half.solver.advance(0, dt / 2);
        half.solver.advance(dt / 2, dt);
        double error = 0;
        for (int i = 0; i < full.gw.state().n_cells; ++i)
            error += std::abs(full.state.volume[i] - half.state.volume[i]) +
                     std::abs(full.gw.state().hg[i] - half.gw.state().hg[i]);
        RecordProperty("enslaved_local_difference_" + std::to_string(int(dt * 10000)), error);
        // Keep the coarse result, with four finer steps testing local order.
        EXPECT_GT(error, 0);
        if (previous > 0 && dt <= .025)
            EXPECT_GT(previous / error, 7.);
        previous = error;
    }
}

namespace {
/**
 * @brief Seed a depth-varying SIGMA moisture profile and refresh its water baseline.
 *
 * Writes theta=0.24+0.015*layer, recomputes hu via columnStorage using current unsaturated
 * thickness, then resets led_init_storage. Does not alter saturated height or validate that every
 * requested layer stays within material moisture bounds.
 *
 * @param[in,out] rig Initialized SIGMA rig with valid layer-major theta_sigma arrays.
 */
void seedRkSigma(CoupledRkRig& rig) {
    auto& w = rig.gw.state();
    for (int cell = 0; cell < w.n_cells; ++cell) {
        for (int layer = 0; layer < w.m_layers; ++layer)
            w.theta_sigma[std::size_t(layer) * w.n_cells + cell] = .24 + .015 * layer;
        w.hu[cell] = sigma::columnStorage(&w.theta_sigma[cell], w.m_layers, w.n_cells,
                                          w.zs[cell] - w.hg[cell]);
    }
    w.led_init_storage = w.storage();
}
/**
 * @brief Integrate the independent moving-layer Gardner water reference over two seconds.
 *
 * Uses fixed four layers, Ks=0.003 m/s, porosity 0.45/residual 0.1, initial saturated height 0.6 m
 * and ponding 0.2 m. Local conservative layer/mesh-motion ODE has no production integration calls.
 * Throws if trial moisture reaches bounds or ponding exhausts; caller selects a smooth
 * well-resolved regime.
 *
 * @param[in] deep Deep-loss conductivity in m/s used in saturated-table flux.
 * @param[in] steps Positive uniform classical-RK4 subdivision count.
 * @return Owned six-entry state: saturated height m, four layer water depths m per unit area, and
 *     surface ponded depth m.
 */
std::vector<double> sigmaGardnerReference(double deep, int steps) {
    constexpr int m = 4;
    constexpr double ks = .003, porosity = .45, residual = .1;
    std::vector<double> y(m + 2);
    y[0] = .6;
    y.back() = .2;
    for (int j = 0; j < m; ++j)
        y[j + 1] = (.24 + .015 * j) * (2 - y[0]) / m;
    const auto rate = [&](const std::vector<double>& state) {
        std::vector<double> out(m + 2), theta(m), conductivity(m), flux(m + 1);
        const double length = 2 - state[0];
        for (int j = 0; j < m; ++j) {
            theta[j] = m * state[j + 1] / length;
            if (!(theta[j] > residual && theta[j] < porosity))
                throw std::runtime_error("Independent smooth SIGMA oracle reached a layer bound");
            conductivity[j] = ks * (theta[j] - residual) / (porosity - residual);
        }
        const double table =
                         (conductivity.back() - deep * state[0] / 2) / (porosity - theta.back()),
                     grid = -table;
        const double suction = -std::log((theta[0] - residual) / (porosity - residual)) / 2;
        flux[0] = ks * (1 + (state.back() + suction) / (.5 * length / m));
        if (!(state.back() > 0))
            throw std::runtime_error("Independent smooth SIGMA oracle exhausted surface water");
        for (int j = 1; j < m; ++j)
            flux[j] = conductivity[j - 1] - theta[grid > 0 ? j : j - 1] * (double(j) / m) * grid;
        flux[m] = conductivity.back() - theta.back() * grid;
        out[0] = table;
        out.back() = -flux[0];
        for (int j = 0; j < m; ++j)
            out[j + 1] = flux[j] - flux[j + 1];
        return out;
    };
    const double dt = 2. / steps;
    for (int step = 0; step < steps; ++step) {
        const auto a = rate(y);
        auto trial = y;
        for (std::size_t j = 0; j < y.size(); ++j)
            trial[j] = y[j] + dt * .5 * a[j];
        const auto b = rate(trial);
        for (std::size_t j = 0; j < y.size(); ++j)
            trial[j] = y[j] + dt * .5 * b[j];
        const auto c = rate(trial);
        for (std::size_t j = 0; j < y.size(); ++j)
            trial[j] = y[j] + dt * c[j];
        const auto d = rate(trial);
        for (std::size_t j = 0; j < y.size(); ++j)
            y[j] += dt * (a[j] + 2 * b[j] + 2 * c[j] + d[j]) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2, SigmaLayerTableAndSurfaceConvergeToIndependentGardnerSystem) {
    for (double deep : {0., .01}) {
        SCOPED_TRACE(deep);
        const auto reference = sigmaGardnerReference(deep, 20000),
                   refined = sigmaGardnerReference(deep, 40000);
        for (std::size_t j = 0; j < reference.size(); ++j)
            ASSERT_NEAR(reference[j], refined[j], 1e-12);
        double previous = 0;
        for (double dt : {.2, .1, .05, .025}) {
            CoupledRkRig r;
            r.init(dt, .003, deep, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            seedRkSigma(r);
            const double initial = r.total();
            for (int k = 0; k < int(std::round(2 / dt)); ++k)
                ASSERT_NO_THROW(r.solver.advance(k * dt, (k + 1) * dt));
            const auto& w = r.gw.state();
            std::vector<double> actual{w.hg[0]};
            for (int j = 0; j < w.m_layers; ++j)
                actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (w.zs[0] - w.hg[0]) /
                                 w.m_layers);
            actual.push_back(r.state.volume[0] / w.area[0]);
            double error = 0;
            for (std::size_t j = 0; j < actual.size(); ++j)
                error += std::pow(actual[j] - reference[j], 2);
            error = std::sqrt(error);
            RecordProperty(std::string(deep > 0 ? "sigma_falling_error_" : "sigma_rising_error_") +
                               std::to_string(int(dt * 1000)),
                           error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            EXPECT_NEAR(r.total() + w.led_deep, initial, 1e-10 * initial);
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * initial);
            EXPECT_NEAR(w.hu[0],
                        sigma::columnStorage(w.theta_sigma.data(), w.m_layers, w.n_cells,
                                             w.zs[0] - w.hg[0]),
                        1e-13);
            if (deep > 0)
                EXPECT_LT(w.hg[0], .6);
            else
                EXPECT_GT(w.hg[0], .6);
        }
    }
}
TEST(CoupledGroundwaterRk2, SigmaBlendUsesCanonicalLayerWaterAndDerivedBulkStorage) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN})
        for (int layers : {2, 4, 8}) {
            SCOPED_TRACE(int(law));
            SCOPED_TRACE(layers);
            CoupledRkRig r;
            r.init(.1, 0, 0, 0, "NONE", GwClosure::SIGMA, law, layers);
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            auto& w = r.gw.state();
            for (int cell = 0; cell < w.n_cells; ++cell) {
                w.hg[cell] = .1;
                for (int j = 0; j < layers; ++j)
                    w.theta_sigma[std::size_t(j) * w.n_cells + cell] = .2 + .01 * j;
                w.hu[cell] = sigma::columnStorage(&w.theta_sigma[cell], layers, w.n_cells, 1.9);
            }
            const auto before = r.gw;
            for (int cell = 0; cell < w.n_cells; ++cell) {
                w.hg[cell] = 1.4;
                for (int j = 0; j < layers; ++j)
                    w.theta_sigma[std::size_t(j) * w.n_cells + cell] = .35 - .01 * j;
                w.hu[cell] = sigma::columnStorage(&w.theta_sigma[cell], layers, w.n_cells, .6);
            }
            const double target = .5 * (before.state().storage() + w.storage());
            ASSERT_NO_THROW(r.gw.blendRk2Water(before, .1));
            EXPECT_NEAR(w.storage(), target, 2e-14);
            for (int cell = 0; cell < w.n_cells; ++cell)
                for (int j = 0; j < layers; ++j) {
                    const double expected =
                        .5 * ((.2 + .01 * j) * 1.9 / layers + (.35 - .01 * j) * .6 / layers);
                    const double actual = w.theta_sigma[std::size_t(j) * w.n_cells + cell] *
                                          (w.zs[cell] - w.hg[cell]) / layers;
                    EXPECT_NEAR(actual, expected, 2e-16);
                }
            for (double thickness : {1e-4, 1e-6, 1e-8, 1e-10}) {
                for (int cell = 0; cell < w.n_cells; ++cell) {
                    w.hg[cell] = w.zs[cell];
                    w.hu[cell] = 0;
                    for (int j = 0; j < layers; ++j)
                        w.theta_sigma[std::size_t(j) * w.n_cells + cell] = w.theta_s[cell];
                }
                const auto saturated = r.gw;
                for (int cell = 0; cell < w.n_cells; ++cell) {
                    w.hg[cell] = w.zs[cell] - thickness;
                    w.hu[cell] = w.theta_s[cell] * (w.zs[cell] - w.hg[cell]);
                }
                const double water = .5 * (w.storage() + saturated.state().storage());
                ASSERT_NO_THROW(r.gw.blendRk2Water(saturated, .2));
                EXPECT_NEAR(w.storage(), water, 2e-14);
                for (int cell = 0; cell < w.n_cells; ++cell)
                    for (int j = 0; j < layers; ++j)
                        EXPECT_NEAR(w.theta_sigma[std::size_t(j) * w.n_cells + cell],
                                    w.theta_s[cell], 1e-15);
            }
        }
}

TEST(CoupledGroundwaterRk2, SigmaFullStepAndTwoHalfStepsHaveThirdLocalOrder) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig full, half;
        for (auto* r : {&full, &half}) {
            r->init(.4, .003, .01, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            ASSERT_TRUE(r->gw.rk2ProfileError().empty());
            seedRkSigma(*r);
        }
        full.solver.advance(0, dt);
        half.solver.advance(0, dt / 2);
        half.solver.advance(dt / 2, dt);
        double difference = std::abs(full.gw.state().hg[0] - half.gw.state().hg[0]) +
                            std::abs(full.state.volume[0] - half.state.volume[0]);
        for (int j = 0; j < 4; ++j) {
            const auto amount = [&](const CoupledRkRig& r) {
                const auto& w = r.gw.state();
                return w.theta_sigma[std::size_t(j) * w.n_cells] * (w.zs[0] - w.hg[0]) / 4;
            };
            difference += std::abs(amount(full) - amount(half));
        }
        RecordProperty("sigma_local_difference_" + std::to_string(int(dt * 1000)), difference);
        EXPECT_GT(difference, 0);
        if (previous > 0)
            EXPECT_GT(previous / difference, 7.);
        previous = difference;
    }
}
TEST(CoupledGroundwaterRk2, SigmaBoundsCollapseAndRewetRetainEveryAcceptedWaterAmount) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN})
        for (bool capillary : {false, true})
            for (int initial_state : {0, 1, 2}) {
                SCOPED_TRACE(int(law));
                SCOPED_TRACE(capillary);
                SCOPED_TRACE(initial_state);
                CoupledRkRig r;
                r.init(.025, .0001, .0002, .005, "NONE", GwClosure::SIGMA, law, 4, capillary);
                ASSERT_TRUE(r.gw.rk2ProfileError().empty());
                auto& w = r.gw.state();
                if (initial_state > 0)
                    for (int i = 0; i < w.n_cells; ++i) {
                        w.hg[i] = initial_state == 1 ? w.zs[i] : 0;
                        w.hu[i] = initial_state == 1 ? 0 : w.theta_r[i] * w.zs[i];
                        for (int j = 0; j < w.m_layers; ++j)
                            w.theta_sigma[std::size_t(j) * w.n_cells + i] =
                                initial_state == 1 ? w.theta_s[i] : w.theta_r[i];
                    }
                w.led_init_storage = w.storage();
                const double initial = r.total();
                ExplicitInertialSolverTestAccess::prepare(r.solver);
                for (int step = 0; step < 20; ++step) {
                    GwResolvedSource well;
                    well.cells = {{0, 1}};
                    well.flow.constant = step < 5 ? (initial_state == 2 ? -100. : 100.)
                                                  : (step < 10 ? (initial_state == 2 ? 100. : -100.)
                                                               : (step < 15 ? .02 : -.02));
                    r.gw.setSources({well});
                    ASSERT_NO_THROW(
                        ExplicitInertialSolverTestAccess::rk2(r.solver, .025, step * .025));
                    EXPECT_NEAR(r.total() + w.led_deep + w.led_source_out,
                                initial + w.led_source_in, 1e-10 * std::max(initial, r.total()));
                    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10 * std::max(initial, r.total()));
                    for (int i = 0; i < w.n_cells; ++i) {
                        EXPECT_GE(w.hg[i], 0);
                        EXPECT_LE(w.hg[i], w.zs[i]);
                        EXPECT_GE(r.state.volume[i], 0);
                        EXPECT_NEAR(w.hu[i],
                                    sigma::columnStorage(&w.theta_sigma[i], w.m_layers, w.n_cells,
                                                         w.zs[i] - w.hg[i]),
                                    1e-13);
                        for (int j = 0; j < w.m_layers; ++j) {
                            const double theta = w.theta_sigma[std::size_t(j) * w.n_cells + i];
                            EXPECT_GE(theta, w.theta_r[i] - 1e-13);
                            EXPECT_LE(theta, w.theta_s[i] + 1e-13);
                        }
                    }
                }
                EXPECT_GT(w.led_dunne, 0);
                EXPECT_GT(w.led_source_out, 0);
                if (initial_state == 1)
                    EXPECT_LT(w.hg[0], w.zs[0]);
                if (initial_state == 2)
                    EXPECT_GT(w.led_infil_in, 0);
            }
}
TEST(CoupledGroundwaterRk2, SigmaForcingMixedClosuresAndRestartKeepTheWholeTransaction) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN})
        for (bool head : {false, true})
            for (int companion : {-1, 0, 1}) {
                SCOPED_TRACE(int(law));
                SCOPED_TRACE(head);
                SCOPED_TRACE(companion);
                CoupledRkRig a, b;
                for (auto* r : {&a, &b}) {
                    r->init(.025, .0001, .0002, .1, "NONE", GwClosure::SIGMA, law, 4, head);
                    ASSERT_TRUE(r->gw.rk2ProfileError().empty());
                    seedRkSigma(*r);
                    auto& w = r->gw.state();
                    if (companion >= 0) {
                        w.closure[1] = int8_t(companion);
                        if (companion == int(GwClosure::ENSLAVED)) {
                            soil::Params p;
                            p.law = law;
                            w.hu[1] = soil::equilibriumStorage(p, w.zs[1] - w.hg[1]);
                        }
                    }
                    w.led_init_storage = w.storage();
                    GwResolvedSource well;
                    well.name = "sigma-knots";
                    well.cells = {{0, 1}};
                    well.flow.times = {0, .13, .27, .5};
                    well.flow.values = {.02, -.03, .01, .02};
                    r->gw.setSources({well});
                    GwSourceSignal signal;
                    signal.times = {0, .09, .31, .5};
                    signal.values = {.01, -.01, .02, .01};
                    auto edge = rk2FlowEdge(*r, 1, signal);
                    edge.head = head;
                    if (head) {
                        const double bottom = w.z_bed[1];
                        edge.signal.values = {bottom + 1.1, bottom + .2, bottom + 2.5, bottom + .8};
                    }
                    r->gw.setBoundaries({edge});
                }
                const auto prefix = "sigma-" + std::to_string(int(law)) + "-" +
                                    std::to_string(head) + "-" + std::to_string(companion);
                const auto saved = rk2Output(prefix + "-accepted.chk"),
                           after = rk2Output(prefix + "-after.chk"),
                           fresh = rk2Output(prefix + "-fresh.chk");
                const double initial = a.total();
                for (int step = 0; step < 20; ++step) {
                    ASSERT_NO_THROW(a.solver.advance(step * .025, (step + 1) * .025));
                    if (step == 7) {
                        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                        ASSERT_EQ(b.solver.saveCheckpoint(fresh.string()), "");
                        b.gw.state().alpha[0] *= 2;
                        EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
                        b.gw.state().alpha[0] /= 2;
                        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
                        EXPECT_EQ(rk2File(fresh), rk2File(after));
                        ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
                    } else if (step > 7)
                        ASSERT_NO_THROW(b.solver.advance(step * .025, (step + 1) * .025));
                }
                ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
                EXPECT_EQ(rk2File(saved), rk2File(after));
                const auto& w = a.gw.state();
                EXPECT_GT(w.led_boundary_in, 0);
                EXPECT_GT(w.led_boundary_out, 0);
                EXPECT_NEAR(a.total() + w.led_deep + w.led_source_out + w.led_boundary_out,
                            initial + w.led_source_in + w.led_boundary_in, 1e-10 * initial);
                GwResolvedSource bad;
                bad.cells = {{0, 1}};
                bad.flow.times = {.5, .61, .83, 9};
                bad.flow.values = {.01, .02, 1e308, 1e308};
                a.gw.setSources({bad});
                ASSERT_EQ(a.solver.saveCheckpoint(fresh.string()), "");
                EXPECT_THROW(a.solver.advance(.5, 9), std::runtime_error);
                ASSERT_EQ(a.solver.saveCheckpoint(after.string()), "");
                EXPECT_EQ(rk2File(fresh), rk2File(after));
                a.gw.setSources({});
                ASSERT_NO_THROW(a.solver.advance(.5, .525));
            }
}
TEST(CoupledGroundwaterRk2, SigmaInvalidLayerAndBulkStatesRefuseWithoutMutationAndCanRetry) {
    for (int mutation = 0; mutation < 5; ++mutation) {
        CoupledRkRig r;
        r.init(.05, .0001, .0002, .1, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        ASSERT_TRUE(r.gw.rk2ProfileError().empty());
        auto& w = r.gw.state();
        const auto layer = w.theta_sigma;
        const auto bulk = w.hu;
        if (mutation == 0)
            w.theta_sigma[0] = 1;
        else if (mutation == 1)
            w.theta_sigma[0] = std::numeric_limits<double>::quiet_NaN();
        else if (mutation == 2)
            w.theta_sigma.pop_back();
        else if (mutation == 3)
            w.hu[0] += .001;
        else
            w.theta_sigma[0] = std::nextafter(w.theta_s[0], 1.);
        const auto volume = r.state.volume,
                   clock = ExplicitInertialSolverTestAccess::state(r.solver);
        EXPECT_FALSE(r.gw.rk2ProfileError().empty());
        EXPECT_THROW(r.solver.advance(0, .05), std::runtime_error);
        EXPECT_EQ(r.state.volume, volume);
        EXPECT_EQ(ExplicitInertialSolverTestAccess::state(r.solver), clock);
        w.theta_sigma = layer;
        w.hu = bulk;
        ASSERT_NO_THROW(r.solver.advance(0, .05));
    }
}

TEST(CoupledGroundwaterRk2, SigmaCapillaryTransportRetainsSecondTemporalOrder) {
    const auto trajectory = [](double dt, bool capillary) {
        CoupledRkRig r;
        r.init(dt, .003, .01, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4, capillary);
        if (!r.gw.rk2ProfileError().empty())
            throw std::runtime_error(r.gw.rk2ProfileError());
        seedRkSigma(r);
        const double initial = r.total();
        for (int step = 0; step < int(std::round(2 / dt)); ++step)
            r.solver.advance(step * dt, (step + 1) * dt);
        const auto& w = r.gw.state();
        EXPECT_NEAR(r.total() + w.led_deep, initial, 1e-10 * initial);
        std::vector<double> result{w.hg[0], r.state.volume[0] / w.area[0]};
        for (int j = 0; j < 4; ++j)
            result.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (w.zs[0] - w.hg[0]) / 4);
        return result;
    };
    const auto reference = trajectory(.0015625, true), plain = trajectory(.0015625, false);
    double change = 0;
    for (std::size_t i = 0; i < plain.size(); ++i)
        change += std::abs(plain[i] - reference[i]);
    EXPECT_GT(change, 1e-8);
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        const auto actual = trajectory(dt, true);
        double error = 0;
        for (std::size_t i = 0; i < actual.size(); ++i)
            error += std::pow(actual[i] - reference[i], 2);
        error = std::sqrt(error);
        RecordProperty("sigma_capillary_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, AtmosphericWaterLedgerAndRefreshClockAreAcceptedOnce) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (SoilChar law : {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY,
                             SoilChar::VAN_GENUCHTEN}) {
            SCOPED_TRACE(int(closure));
            SCOPED_TRACE(int(law));
            CoupledRkRig r;
            r.init(.02, .0001, .0002, .001, "BOUNDARY_ET", closure, law, 4, true);
            ASSERT_TRUE(r.gw.rk2ProfileError().empty());
            if (closure == GwClosure::SIGMA)
                seedRkSigma(r);
            r.opts.evaporation = true;
            std::fill(r.state.evap_rate.begin(), r.state.evap_rate.end(), .002);
            std::fill(r.gw.state().wetting_front.begin(), r.gw.state().wetting_front.end(), .1);
            auto& w = r.gw.state();
            const double initial = r.total();
            for (int step = 0; step < 100; ++step)
                ASSERT_NO_THROW(r.solver.advance(step * .02, (step + 1) * .02));
            EXPECT_GT(w.led_et, 0);
            EXPECT_GT(r.state.evap_loss_total, 0);
            EXPECT_NEAR(r.total() + w.led_deep + w.led_et + r.state.evap_loss_total, initial,
                        1e-10 * initial);
            for (int cell = 0; cell < w.n_cells; ++cell) {
                EXPECT_NEAR(w.et_potential_cumulative[cell], .004 * w.area[cell], 1e-12);
                EXPECT_NEAR(w.et_potential_cumulative[cell],
                            w.et_surface_cumulative[cell] + w.et_soil_cumulative[cell] +
                                w.et_unused_cumulative[cell] + w.et_pending[cell],
                            1e-12);
                EXPECT_DOUBLE_EQ(w.et_refresh[cell], 2.);
            }
        }
}

namespace {
/**
 * @brief Initialize groundwater pollutant and optional age rows for a coupled rig.
 *
 * Uses owned temporary row/config values and discards initialization warnings. Does not seed
 * nonzero mass or validate arbitrary layer/closure combinations; caller seeds compatible
 * initialized state before advance.
 *
 * @param[in,out] r Water-initialized rig whose GW transport state is rebuilt.
 * @param[in] layered Select LAYERED rather than BULK unsaturated transport.
 * @param[in] age Add the native seconds-valued water-age row after Dye.
 * @param[in] surface Also size surface transport rows, enable groundwater coupling and
 *     reinitialize/rebind solver.
 */
void initRkQuality(CoupledRkRig& r, bool layered, bool age = false, bool surface = false) {
    RowLayoutLite rows;
    rows.n_species = age ? 2 : 1;
    rows.n_pollut = 1;
    rows.age_row = age ? 1 : -1;
    rows.names =
        age ? std::vector<std::string>{"Dye", "__WATER_AGE__"} : std::vector<std::string>{"Dye"};
    GwTransportData data;
    data.options.unsat_transport = layered ? "LAYERED" : "BULK";
    std::vector<std::string> warnings;
    r.gw.initTransport(rows, &data, {}, warnings);
    if (surface) {
        r.state.transport.resize(rows.n_species, r.mesh.n_cells(), 0);
        auto& t = r.state.transport;
        t.n_pollut = rows.n_pollut;
        t.age_row = rows.age_row;
        t.row_names = rows.names;
        r.opts.groundwater = 1;
        r.solver.initialize(r.mesh, r.state, r.opts);
        r.solver.setSubsurface(&r.gw);
    }
}
/**
 * @brief Seed uniform dissolved/sorbed concentrations and reset initial species inventories.
 *
 * Uses age=5 s with zero sorption, bulk/layer moisture as configured and kd*rho_b partition volume.
 * Refreshes layered unsaturated totals, resets init_mass to current GW storage and seeds surface
 * rows when active. Does not clear every transport ledger or validate dimensions/material bounds;
 * intended before trial advance.
 *
 * @param[in,out] r Rig with matching initialized GW/surface transport layouts and physical water
 *     arrays.
 * @param[in] concentration Native concentration for non-age rows, multiplied by SI water/partition
 *     volume.
 * @param[in] kd Nonnegative native sorption partition coefficient in m3/kg, ignored for the age
 *     row.
 */
void seedRkQuality(CoupledRkRig& r, double concentration, double kd = 0) {
    auto& t = r.gw.transport();
    auto& w = r.gw.state();
    for (int s = 0; s < t.n_species; ++s)
        for (int i = 0; i < w.n_cells; ++i) {
            const auto k = t.idx(s, i);
            const bool age = s == t.age_row;
            t.kd[k] = age ? 0 : kd;
            const double partition = t.kd[k] * t.rho_b[i], c = age ? 5 : concentration;
            t.sat_mass[k] = c * w.area[i] * w.hg[i] * (w.theta_s[i] + partition);
            if (t.n_layers) {
                for (int j = 0; j < t.n_layers; ++j)
                    t.layer_mass[t.layerIdx(s, j, i)] =
                        c * w.area[i] * (w.zs[i] - w.hg[i]) / t.n_layers *
                        (w.theta_sigma[std::size_t(j) * w.n_cells + i] + partition);
                t.refreshUnsat(s, i);
            } else
                t.unsat_mass[k] = c * w.area[i] * (w.hu[i] + partition * (w.zs[i] - w.hg[i]));
        }
    for (int s = 0; s < t.n_species; ++s)
        t.init_mass[s] = t.storage(s);
    if (r.state.transport.active())
        for (int s = 0; s < t.n_species; ++s)
            for (int i = 0; i < w.n_cells; ++i)
                r.state.transport.cell_mass[r.state.transport.idx(s, i)] =
                    (s == t.age_row ? 5 : concentration) * r.state.volume[i];
}
}
TEST(CoupledGroundwaterRk2, PassiveStirredAquiferMatchesAnalyticalRetardedMixing) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double kd : {0., .001}) {
                SCOPED_TRACE(int(closure));
                SCOPED_TRACE(layered);
                SCOPED_TRACE(kd);
                double previous = 0;
                for (double dt : {.4, .2, .1, .05}) {
                    CoupledRkRig r;
                    r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                    initRkQuality(r, layered);
                    ASSERT_EQ(r.gw.rk2ProfileError(), "");
                    seedRkQuality(r, 2, kd);
                    auto& t = r.gw.transport();
                    const auto& w = r.gw.state();
                    GwResolvedSource source;
                    source.name = "stirred";
                    source.cells = {{0, 1}};
                    source.flow.constant = .1;
                    GwResolvedSourceTerm term;
                    term.row = 0;
                    term.signal.constant = 7;
                    source.terms = {term};
                    r.gw.setSources({source});
                    GwSourceSignal flow;
                    flow.constant = -.1;
                    auto edge = rk2FlowEdge(r, 0, flow);
                    edge.mass_in.assign(1, 0);
                    edge.mass_out.assign(1, 0);
                    edge.flux_in.assign(1, 0);
                    edge.flux_out.assign(1, 0);
                    r.gw.setBoundaries({edge});
                    const double capacity = w.area[0] * w.hg[0] * (w.theta_s[0] + kd * t.rho_b[0]);
                    for (int step = 0; step < int(std::round(2 / dt)); ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                    const double exact = 7 + (2 - 7) * std::exp(-.1 * 2 / capacity),
                                 error = std::abs(t.sat_mass[0] / capacity - exact);
                    RecordProperty("mixing_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" +
                                       std::to_string(int(kd * 1000)) + "_" +
                                       std::to_string(int(dt * 1000)),
                                   error);
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    EXPECT_NEAR(t.residual(0), 0, 1e-10 * std::max(1., t.init_mass[0]));
                    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
                }
            }
        }
}
TEST(CoupledGroundwaterRk2, PassiveAndAgeRestartRetainCanonicalLayerAndGenerationLedgers) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig a, b;
            for (auto* r : {&a, &b}) {
                r->init(.05, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                initRkQuality(*r, layered, true);
                ASSERT_EQ(r->gw.rk2ProfileError(), "");
                seedRkQuality(*r, 2, .001);
            }
            const auto initial = a.gw.transport().init_mass;
            const double water = a.gw.state().storage();
            for (int step = 0; step < 20; ++step) {
                ASSERT_NO_THROW(a.solver.advance(step * .05, (step + 1) * .05));
                if (step == 7) {
                    const auto saved = rk2Output("quality-age.chk");
                    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
                } else if (step > 7)
                    ASSERT_NO_THROW(b.solver.advance(step * .05, (step + 1) * .05));
            }
            EXPECT_EQ(a.gw.transport().sat_mass, b.gw.transport().sat_mass);
            EXPECT_EQ(a.gw.transport().layer_mass, b.gw.transport().layer_mass);
            EXPECT_EQ(a.gw.transport().generated_age, b.gw.transport().generated_age);
            EXPECT_NEAR(a.gw.transport().storage(1), initial[1] + water, 1e-10);
            EXPECT_NEAR(a.gw.transport().residual(1), 0, 1e-10);
            const auto x = rk2Output("quality-final-a.chk"), y = rk2Output("quality-final-b.chk");
            ASSERT_EQ(a.solver.saveCheckpoint(x.string()), "");
            ASSERT_EQ(b.solver.saveCheckpoint(y.string()), "");
            EXPECT_EQ(rk2File(x), rk2File(y));
        }
}

TEST(CoupledGroundwaterRk2, PassiveLayerEulerUsesJointBeginningDonorsAndRetainsDryMass) {
    const std::vector<double> capacity{1, 1, 0}, initial{2, 1, 5};
    const std::vector<LayerTransportMove> moves{{0, 1, 10}, {0, -1, 10}, {1, -1, 10}};
    const auto result = advanceLayerTransportEuler(capacity, initial, initial, moves);
    ASSERT_EQ(result.error, "");
    ASSERT_EQ(result.transferred.size(), 3);
    EXPECT_DOUBLE_EQ(result.transferred[0], 1);
    EXPECT_DOUBLE_EQ(result.transferred[1], 1);
    EXPECT_DOUBLE_EQ(result.transferred[2], 1);
    EXPECT_EQ(result.mass, (std::vector<double>{0, 1, 5}));
    EXPECT_DOUBLE_EQ(std::accumulate(result.mass.begin(), result.mass.end(), 0.) +
                         result.transferred[1] + result.transferred[2],
                     8);
    // Reordering exports retains their common donor scale.
    const auto reversed = advanceLayerTransportEuler(capacity, initial, initial,
                                                     {{0, -1, 10}, {0, 1, 10}, {1, -1, 10}});
    ASSERT_EQ(reversed.error, "");
    EXPECT_EQ(reversed.mass, result.mass);
    auto bad = capacity;
    bad[0] = std::numeric_limits<double>::quiet_NaN();
    EXPECT_FALSE(advanceLayerTransportEuler(bad, initial, initial, moves).error.empty());
}

TEST(CoupledGroundwaterRk2, PassiveSharedSurfaceLayerTransportRefinesAndClosesEveryLedger) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            const auto trajectory = [&](double dt) {
                CoupledRkRig r;
                r.init(dt, .003, .001, .2, "NONE", closure, SoilChar::GARDNER, 4, true);
                initRkQuality(r, layered, true, true);
                seedRkQuality(r, 2, .001);
                auto& t = r.gw.transport();
                auto& surface = r.state.transport;
                for (int i = 0; i < t.n_cells; ++i)
                    r.gw.state().wetting_front[i] = .1;
                // A distinguishable surface pulse makes coupling order observable.
                for (int i = 0; i < t.n_cells; ++i)
                    surface.cell_mass[surface.idx(0, i)] *= 3;
                const double initial = surface.totalIncludingLedgers(0),
                             age_initial = surface.totalIncludingLedgers(1);
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    r.solver.advance(step * dt, (step + 1) * dt);
                EXPECT_NEAR(surface.totalIncludingLedgers(0), initial, 1e-10 * initial);
                EXPECT_NEAR(surface.totalIncludingLedgers(1), age_initial, 1e-10 * age_initial);
                for (int s = 0; s < t.n_species; ++s) {
                    EXPECT_NEAR(t.residual(s), 0, 1e-10 * t.init_mass[s]);
                    EXPECT_NEAR(surface.lost_infiltration[s], t.gained_infil[s], 1e-12);
                }
                std::vector<double> result = surface.cell_mass;
                result.insert(result.end(), t.sat_mass.begin(), t.sat_mass.end());
                if (layered)
                    result.insert(result.end(), t.layer_mass.begin(), t.layer_mass.end());
                else
                    result.insert(result.end(), t.unsat_mass.begin(), t.unsat_mass.end());
                return result;
            };
            const auto reference = trajectory(.0015625);
            double previous = 0;
            for (double dt : {.2, .1, .05, .025, .0125}) {
                const auto actual = trajectory(dt);
                double error = 0;
                for (std::size_t i = 0; i < actual.size(); ++i)
                    error += std::pow(actual[i] - reference[i], 2);
                error = std::sqrt(error);
                RecordProperty("shared_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                if (dt <= .1) {
                    EXPECT_LT(error, .001);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                }
                previous = error;
            }
        }
}
TEST(CoupledGroundwaterRk2, PassiveSharedRestartRollbackAndIdentityRefusalAreAtomic) {
    for (bool layered : {false, true}) {
        CoupledRkRig a, b;
        for (auto* r : {&a, &b}) {
            r->init(.025, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4, true);
            initRkQuality(*r, layered, true, true);
            seedRkQuality(*r, 2, .001);
        }
        const auto saved = rk2Output("shared-quality.chk"), before = rk2Output("shared-before.chk"),
                   after = rk2Output("shared-after.chk");
        for (int step = 0; step < 20; ++step) {
            ASSERT_NO_THROW(a.solver.advance(step * .025, (step + 1) * .025));
            if (step == 7) {
                ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
                b.state.transport.rain_conc = {9, 0};
                EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
                b.state.transport.rain_conc.clear();
                ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
                EXPECT_EQ(rk2File(before), rk2File(after));
                ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            } else if (step > 7)
                ASSERT_NO_THROW(b.solver.advance(step * .025, (step + 1) * .025));
        }
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(saved), rk2File(after));
        GwResolvedSource source;
        source.cells = {{0, 1}};
        source.flow.times = {.5, .6, .8, 9};
        source.flow.values = {.01, .02, 1e308, 1e308};
        a.gw.setSources({source});
        ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
        EXPECT_THROW(a.solver.advance(.5, 9), std::runtime_error);
        ASSERT_EQ(a.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        a.gw.setSources({});
        ASSERT_NO_THROW(a.solver.advance(.5, .525));
    }
}

TEST(CoupledGroundwaterRk2, PassiveAllSoilsRetainMassAcrossSaturationPumpingAndRewetting) {
    for (SoilChar law :
         {SoilChar::RUSSO, SoilChar::GARDNER, SoilChar::BROOKS_COREY, SoilChar::VAN_GENUCHTEN})
        for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
            for (bool layered : {false, true}) {
                if (layered && closure != GwClosure::SIGMA)
                    continue;
                SCOPED_TRACE(int(law));
                SCOPED_TRACE(int(closure));
                SCOPED_TRACE(layered);
                CoupledRkRig r;
                r.init(.025, .0001, .0002, .005, "NONE", closure, law, 4, true);
                initRkQuality(r, layered, true, true);
                seedRkQuality(r, 2, .001);
                auto& t = r.gw.transport();
                auto& w = r.gw.state();
                const double initial = r.total();
                const double surface0 = r.state.transport.totalIncludingLedgers(0),
                             surface_age0 = r.state.transport.totalIncludingLedgers(1);
                ExplicitInertialSolverTestAccess::prepare(r.solver);
                for (int step = 0; step < 24; ++step) {
                    GwResolvedSource source;
                    source.cells = {{0, 1}};
                    source.flow.constant =
                        step < 6 ? 100. : (step < 12 ? -100. : (step < 18 ? .01 : -.01));
                    GwResolvedSourceTerm solute;
                    solute.row = 0;
                    solute.signal.constant = 7;
                    GwResolvedSourceTerm age;
                    age.row = 1;
                    age.signal.constant = 2;
                    source.terms = {solute, age};
                    r.gw.setSources({source});
                    ASSERT_NO_THROW(
                        ExplicitInertialSolverTestAccess::rk2(r.solver, .025, step * .025));
                    EXPECT_NEAR(r.total() + w.led_deep + w.led_source_out,
                                initial + w.led_source_in, 1e-10 * std::max(initial, r.total()));
                    for (int s = 0; s < t.n_species; ++s)
                        EXPECT_NEAR(t.residual(s), 0,
                                    1e-10 * std::max(t.init_mass[s], t.gained_source[s]));
                    EXPECT_NEAR(r.state.transport.totalIncludingLedgers(0), surface0,
                                1e-10 * std::max(1., surface0));
                    EXPECT_NEAR(r.state.transport.totalIncludingLedgers(1), surface_age0,
                                1e-10 * std::max(1., surface_age0));
                    for (const auto* mass :
                         {&t.sat_mass, &t.unsat_mass, &t.layer_mass, &r.state.transport.cell_mass})
                        for (double v : *mass) {
                            EXPECT_TRUE(std::isfinite(v));
                            EXPECT_GE(v, 0);
                        }
                }
                EXPECT_GT(w.led_dunne, 0);
                EXPECT_GT(w.led_source_out, 0);
            }
}

TEST(CoupledGroundwaterRk2, PassiveEulerColumnStagesMatchIndependentAnalyticalChain) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05, .025}) {
        std::vector<double> mass{1, 0, 0};
        const std::vector<double> capacity{1, 1, 1};
        double exported = 0;
        const std::vector<LayerTransportMove> moves{
            {0, 1, .1 * dt}, {1, 2, .1 * dt}, {2, -1, .1 * dt}};
        for (int step = 0; step < int(std::round(2 / dt)); ++step) {
            const auto a = advanceLayerTransportEuler(capacity, mass, mass, moves);
            ASSERT_EQ(a.error, "");
            const auto b = advanceLayerTransportEuler(capacity, a.mass, a.mass, moves);
            ASSERT_EQ(b.error, "");
            for (int i = 0; i < 3; ++i)
                mass[i] = .5 * (mass[i] + b.mass[i]);
            exported += .5 * (a.transferred[2] + b.transferred[2]);
        }
        const double e = std::exp(-.2);
        const std::vector<double> exact{e, .2 * e, .02 * e};
        double error = 0;
        for (int i = 0; i < 3; ++i)
            error += std::pow(mass[i] - exact[i], 2);
        error = std::sqrt(error);
        RecordProperty("column_chain_" + std::to_string(int(dt * 1000)), error);
        if (dt <= .2) {
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
        }
        previous = error;
        EXPECT_NEAR(std::accumulate(mass.begin(), mass.end(), 0.) + exported, 1, 1e-14);
    }
}

TEST(CoupledGroundwaterRk2, PassiveMovingTableApproachesIndependentUniformDissolvedState) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.1, .05, .025, .0125}) {
                CoupledRkRig r;
                r.init(dt, .003, .001, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkQuality(r, layered, false, true);
                seedRkQuality(r, 2, .001);
                auto& w = r.gw.state();
                auto& t = r.gw.transport();
                for (int i = 0; i < w.n_cells; ++i)
                    w.wetting_front[i] = .1;
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    r.solver.advance(step * dt, (step + 1) * dt);
                double error = 0;
                for (int i = 0; i < w.n_cells; ++i) {
                    const auto k = t.idx(0, i);
                    const double partition = t.kd[k] * t.rho_b[i];
                    error += std::pow(
                        t.sat_mass[k] - 2 * w.area[i] * w.hg[i] * (w.theta_s[i] + partition), 2);
                    error +=
                        std::pow(t.unsat_mass[k] -
                                     2 * w.area[i] * (w.hu[i] + partition * (w.zs[i] - w.hg[i])),
                                 2);
                    error += std::pow(r.state.transport.cell_mass[i] - 2 * r.state.volume[i], 2);
                }
                error = std::sqrt(error);
                RecordProperty("uniform_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 10000)),
                               error);
                EXPECT_LT(error, 1e-3);
                if (previous > 1e-9 && error > 1e-9)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
            }
        }
}

namespace {
struct RkReactionSystem {
    SWMM_Engine handle = swmm_engine_new();
    /**
     * @brief Create the owned editable chemistry context used by direct RK2 reaction tests.
     *
     * Creates an empty engine and applies full-coupling SEC chemistry with A/B bulk MG species and
     * tight tolerances. Throws runtime_error on allocation/apply rejection. Raw engine member is not
     * RAII during failed construction, so an apply failure after allocation bypasses destructor
     * cleanup; intended successful fixture configs are required. Tokens are copied unescaped; context
     * must remain alive while borrowed reaction bindings are used.
     *
     * @param[in] laws Raw REACTION_SUBSURFACE rows.
     * @param[in] solver Raw reaction solver keyword, default RK5.
     * @param[in] pipe Raw REACTION_PIPES rows; empty omits the section.
     * @param[in] terms Raw REACTION_TERMS rows; empty omits the section.
     */
    explicit RkReactionSystem(const std::string& laws, const std::string& solver = "RK5",
                              const std::string& pipe = "", const std::string& terms = "") {
        char error[512]{};
        const auto text = "[REACTION_OPTIONS]\nSOLVER " + solver +
                          "\nRATE_UNITS SEC\nCOUPLING FULL\nATOL 1e-13\nRTOL 1e-11\n"
                          "[REACTION_SPECIES]\nBULK A MG\nBULK B MG\n" +
                          (terms.empty() ? "" : "[REACTION_TERMS]\n" + terms) +
                          "[REACTION_SUBSURFACE]\n" + laws +
                          (pipe.empty() ? "" : "[REACTION_PIPES]\n" + pipe);
        if (!handle ||
            swmm_reactions_apply_text(handle, text.c_str(), error, sizeof(error)) != SWMM_OK)
            throw std::runtime_error(error);
    }
    /**
     * @brief Destroy a successfully constructed reaction fixture engine.
     *
     * Checks the stored handle before destruction; context/bindings borrowed from it become invalid.
     * Does not create or remove chemistry files.
     */
    ~RkReactionSystem() {
        if (handle)
            swmm_engine_destroy(handle);
    }
    /**
     * @brief Borrow the fixture engine simulation context.
     *
     * Requires a non-null live new-engine handle handle. Performs an unchecked pointer
     * cast/dereference, without transferring ownership or extending the engine lifetime.
     * @return Mutable context reference valid until the owning engine handle is destroyed or replaces
     *     model storage.
     */
    openswmm::SimulationContext& context() {
        return reinterpret_cast<openswmm::SWMMEngine*>(handle)->context();
    }
};
/**
 * @brief Initialize two-species reacting GW/surface state with optional temperature.
 *
 * Seeds GW A concentration=2, B=0 and optional temperature=0 using dissolved/sorbed water
 * capacities, resets GW init_mass and zeroes new surface mass. Fatally requires reaction
 * initialization/RK2 compatibility, reinitializes solver and binds GW. Caller keeps system context
 * and rig members alive through use, seeds any desired surface concentrations separately, and
 * honors partial-setup assertion failures.
 *
 * @param[in,out] r Initialized water rig whose transport/solver state is rebuilt.
 * @param[in,out] system Live reaction-system owner supplying the context used by GW reaction
 *     bindings.
 * @param[in] layered Use LAYERED unsaturated species storage when true.
 * @param[in] kd Native m3/kg partition coefficient for A/B; temperature always unsorbed.
 * @param[in] temperature Add a temperature row after A/B when true.
 */
void initRkReactions(CoupledRkRig& r, RkReactionSystem& system, bool layered, double kd = 0,
                     bool temperature = false) {
    RowLayoutLite rows;
    rows.n_species = temperature ? 3 : 2;
    rows.n_msx = 2;
    rows.names = {"A", "B"};
    if (temperature) {
        rows.temp_row = 2;
        rows.names.push_back("__TEMPERATURE__");
    }
    GwTransportData data;
    data.options.unsat_transport = layered ? "LAYERED" : "BULK";
    std::vector<std::string> warnings;
    r.gw.initTransport(rows, &data, {}, warnings);
    auto& t = r.gw.transport();
    auto& w = r.gw.state();
    for (int species = 0; species < rows.n_species; ++species)
        for (int i = 0; i < w.n_cells; ++i) {
            const auto k = t.idx(species, i);
            t.kd[k] = species == rows.temp_row ? 0 : kd;
            const double partition = t.kd[k] * t.rho_b[i], concentration = species == 0 ? 2 : 0;
            t.sat_mass[k] = concentration * w.area[i] * w.hg[i] * (w.theta_s[i] + partition);
            if (layered) {
                for (int j = 0; j < t.n_layers; ++j)
                    t.layer_mass[t.layerIdx(species, j, i)] =
                        concentration * w.area[i] * (w.zs[i] - w.hg[i]) / t.n_layers *
                        (w.theta_sigma[std::size_t(j) * w.n_cells + i] + partition);
                t.refreshUnsat(species, i);
            } else
                t.unsat_mass[k] =
                    concentration * w.area[i] * (w.hu[i] + partition * (w.zs[i] - w.hg[i]));
        }
    for (int species = 0; species < rows.n_species; ++species)
        t.init_mass[species] = t.storage(species);
    ASSERT_EQ(r.gw.initReactions(system.context()), "");
    r.state.transport.resize(rows.n_species, r.mesh.n_cells(), 0);
    r.state.transport.n_pollut = 0;
    r.state.transport.n_msx = 2;
    r.state.transport.temp_row = rows.temp_row;
    r.state.transport.row_names = rows.names;
    r.opts.groundwater = 1;
    r.solver.initialize(r.mesh, r.state, r.opts);
    r.solver.setSubsurface(&r.gw);
    ASSERT_EQ(r.gw.rk2ProfileError(), "");
}
const std::string kRkConversion =
    "* SAT A RATE -.2*A\n* SAT B RATE .2*A\n* UNSAT A RATE -.2*A\n* UNSAT B RATE .2*A\n";
}
TEST(CoupledGroundwaterRk2, ReactionsSymmetricSplitMatchesIndependentAffineStirredReactor) {
    RkReactionSystem system(kRkConversion);
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double kd : {0., .001}) {
                SCOPED_TRACE(int(closure));
                SCOPED_TRACE(layered);
                SCOPED_TRACE(kd);
                double previous = 0;
                for (double dt : {.1, .05, .025, .0125}) {
                    CoupledRkRig r;
                    r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                    initRkReactions(r, system, layered, kd);
                    auto& t = r.gw.transport();
                    const auto& w = r.gw.state();
                    GwResolvedSource source;
                    source.name = "reactive-mixing";
                    source.cells = {{0, 1}};
                    source.flow.constant = .1;
                    GwResolvedSourceTerm term;
                    term.row = 0;
                    term.signal.constant = 7;
                    source.terms = {term};
                    r.gw.setSources({source});
                    GwSourceSignal flow;
                    flow.constant = -.1;
                    auto edge = rk2FlowEdge(r, 0, flow);
                    for (auto* receipt :
                         {&edge.mass_in, &edge.mass_out, &edge.flux_in, &edge.flux_out})
                        receipt->assign(2, 0);
                    r.gw.setBoundaries({edge});
                    const double volume = w.area[0] * w.hg[0] * w.theta_s[0];
                    const double capacity = w.area[0] * w.hg[0] * (w.theta_s[0] + kd * t.rho_b[0]);
                    const double rate = (.1 + .2 * volume) / capacity, steady = .7 / rate;
                    const double exact_a = steady + (2 * capacity - steady) * std::exp(-rate * 2);
                    const double exact_total =
                        7 * capacity + (2 * capacity - 7 * capacity) * std::exp(-.1 * 2 / capacity);
                    for (int step = 0; step < int(std::round(2 / dt)); ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                    const double error =
                        std::hypot(t.sat_mass[t.idx(0, 0)] - exact_a,
                                   t.sat_mass[t.idx(1, 0)] - (exact_total - exact_a));
                    RecordProperty("reaction_mixing_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" +
                                       std::to_string(int(kd * 1000)) + "_" +
                                       std::to_string(int(dt * 10000)),
                                   error);
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    for (int species = 0; species < 2; ++species)
                        EXPECT_NEAR(t.residual(species), 0, 1e-10 * std::max(1., t.init_mass[0]));
                    EXPECT_NEAR(t.lost_reaction[0] + t.lost_reaction[1], 0,
                                1e-10 * std::max(1., t.init_mass[0]));
                }
            }
        }
}

TEST(CoupledGroundwaterRk2, ReactionsConvertAIntoBAtActualRetardedCapacity) {
    for (const std::string solver : {"RK5", "ROS2", "BDF2"}) {
        RkReactionSystem system(kRkConversion, solver);
        for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
            for (bool layered : {false, true}) {
                if (layered && closure != GwClosure::SIGMA)
                    continue;
                SCOPED_TRACE(solver);
                SCOPED_TRACE(int(closure));
                SCOPED_TRACE(layered);
                CoupledRkRig r;
                r.init(.05, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                initRkReactions(r, system, layered, .001);
                const auto initial = r.gw.transport();
                const auto& w = r.gw.state();
                for (int step = 0; step < 20; ++step)
                    ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
                const auto& t = r.gw.transport();
                for (int i = 0; i < w.n_cells; ++i) {
                    const double partition = t.kd[t.idx(0, i)] * t.rho_b[i];
                    const double sat_keep =
                        std::exp(-.2 * w.theta_s[i] / (w.theta_s[i] + partition));
                    EXPECT_NEAR(t.sat_mass[t.idx(0, i)],
                                initial.sat_mass[initial.idx(0, i)] * sat_keep,
                                initial.init_mass[0] * 1e-8);
                    if (layered)
                        for (int j = 0; j < t.n_layers; ++j) {
                            const double theta = w.theta_sigma[std::size_t(j) * w.n_cells + i];
                            const auto k = t.layerIdx(0, j, i);
                            const double expected =
                                initial.layer_mass[k] * std::exp(-.2 * theta / (theta + partition));
                            EXPECT_NEAR(t.layer_mass[k], expected, initial.init_mass[0] * 1e-8);
                        }
                    else {
                        const double theta = w.hu[i] / (w.zs[i] - w.hg[i]);
                        EXPECT_NEAR(t.unsat_mass[t.idx(0, i)],
                                    initial.unsat_mass[t.idx(0, i)] *
                                        std::exp(-.2 * theta / (theta + partition)),
                                    initial.init_mass[0] * 1e-8);
                    }
                }
                EXPECT_NEAR(t.storage(0) + t.storage(1), initial.init_mass[0],
                            initial.init_mass[0] * 1e-10);
                for (int species = 0; species < 2; ++species)
                    EXPECT_NEAR(t.residual(species), 0, initial.init_mass[0] * 1e-10);
            }
    }
}
TEST(CoupledGroundwaterRk2, ReactionsRestartAndChangedLawRefusalKeepTheWholeNumericalOwner) {
    RkReactionSystem system(kRkConversion),
        changed(
            "* SAT A RATE -.3*A\n* SAT B RATE .3*A\n* UNSAT A RATE -.3*A\n* UNSAT B RATE .3*A\n");
    for (bool layered : {false, true}) {
        CoupledRkRig a, b, bad;
        for (auto* r : {&a, &b, &bad}) {
            r->init(.025, .0001, .0002, .1, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkReactions(*r, r == &bad ? changed : system, layered, .001);
        }
        const auto saved = rk2Output("reaction-quality.chk"),
                   before = rk2Output("reaction-before.chk"),
                   after = rk2Output("reaction-after.chk");
        for (int step = 0; step < 20; ++step) {
            ASSERT_NO_THROW(a.solver.advance(step * .025, (step + 1) * .025));
            if (step == 7) {
                ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                ASSERT_EQ(bad.solver.saveCheckpoint(before.string()), "");
                EXPECT_FALSE(bad.solver.loadCheckpoint(saved.string()).empty());
                ASSERT_EQ(bad.solver.saveCheckpoint(after.string()), "");
                EXPECT_EQ(rk2File(before), rk2File(after));
                ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            } else if (step > 7)
                ASSERT_NO_THROW(b.solver.advance(step * .025, (step + 1) * .025));
        }
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(saved), rk2File(after));
    }
}
TEST(CoupledGroundwaterRk2, ReactionsLaterCellFailureRestoresEarlierCellsAndCanRetry) {
    RkReactionSystem bad(
        "* SAT A RATE -.2*A\n* SAT B RATE .2*A\n* UNSAT A RATE -SQRT(GW_THETA-.2)*A\n* UNSAT B RATE SQRT(GW_THETA-.2)*A\n"),
        good(kRkConversion);
    CoupledRkRig r;
    r.init(.05, 0, 0, 0, "NONE", GwClosure::CLOSED_FORM, SoilChar::GARDNER, 4);
    auto& w = r.gw.state();
    w.hu[0] = .3 * (w.zs[0] - w.hg[0]);
    w.hu[1] = .1 * (w.zs[1] - w.hg[1]);
    w.led_init_storage = w.storage();
    initRkReactions(r, bad, false);
    const auto before = rk2Output("reaction-failed-before.chk"),
               after = rk2Output("reaction-failed-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, .05), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(r.gw.initReactions(good.context()), "");
    ASSERT_NO_THROW(r.solver.advance(0, .05));
    EXPECT_GT(r.gw.transport().lost_reaction[0], 0);
    EXPECT_LT(r.gw.transport().lost_reaction[1], 0);
}
TEST(CoupledGroundwaterRk2, ReactionsTotalMassDecayIsAppliedOnceAndDoesNotDecayWaterAge) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig r;
            r.init(.05, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
            initRkQuality(r, layered, true, true);
            seedRkQuality(r, 2, .001);
            auto& t = r.gw.transport();
            for (int i = 0; i < t.n_cells; ++i)
                t.decay[t.idx(0, i)] = .3;
            const auto initial = t.init_mass;
            const double water = r.gw.state().storage();
            for (int step = 0; step < 20; ++step)
                ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
            EXPECT_NEAR(t.storage(0), initial[0] * std::exp(-.3), 1e-12 * initial[0]);
            EXPECT_NEAR(t.lost_reaction[0], initial[0] * (1 - std::exp(-.3)), 1e-12 * initial[0]);
            EXPECT_NEAR(t.storage(1), initial[1] + water, 1e-12 * initial[1]);
            EXPECT_DOUBLE_EQ(t.lost_reaction[1], 0);
        }
}

TEST(CoupledGroundwaterRk2, ReactionsEulerRetainsItsDeclaredFirstOrderAndOnePhysicalInterval) {
    RkReactionSystem system(kRkConversion, "EUL");
    CoupledRkRig r;
    r.init(.05, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkReactions(r, system, true);
    const double initial = r.gw.transport().init_mass[0];
    for (int step = 0; step < 20; ++step)
        ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
    const auto& t = r.gw.transport();
    const double expected = initial * std::pow(1 - .2 * .025, 40);
    EXPECT_NEAR(t.storage(0), expected, initial * 1e-12);
    EXPECT_NEAR(t.storage(0) + t.storage(1), initial, initial * 1e-12);
}
TEST(CoupledGroundwaterRk2, ReactionsFormulaAndEquilibriumPreserveTheirOwnProductionLedgers) {
    for (const std::string form : {"FORMULA 2*A", "EQUIL B-2*A"}) {
        RkReactionSystem system("* SAT B " + form + "\n* UNSAT B " + form + "\n");
        CoupledRkRig r;
        r.init(.05, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(r, system, true, .001);
        const double initial = r.gw.transport().init_mass[0];
        for (int step = 0; step < 20; ++step)
            ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
        const auto& t = r.gw.transport();
        EXPECT_NEAR(t.storage(0), initial, initial * 1e-10);
        EXPECT_NEAR(t.storage(1), 2 * initial, initial * 1e-10);
        EXPECT_NEAR(t.lost_reaction[1], -2 * initial, initial * 1e-10);
        EXPECT_NEAR(t.residual(0), 0, initial * 1e-10);
        EXPECT_NEAR(t.residual(1), 0, initial * 1e-10);
    }
}

TEST(CoupledGroundwaterRk2, ReactionsStiffRosenbrockConversionRetainsBoundsAndExactRestart) {
    RkReactionSystem system(
        "* SAT A RATE -1000*A\n* SAT B RATE 1000*A-.2*B\n* UNSAT A RATE -1000*A\n* UNSAT B RATE 1000*A-.2*B\n",
        "ROS2");
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(*r, system, true);
    }
    const double initial = a.gw.transport().init_mass[0];
    ASSERT_NO_THROW(a.solver.advance(0, .05));
    const auto saved = rk2Output("stiff-reactor.chk"),
               restored = rk2Output("stiff-reactor-restored.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.05, .1));
    ASSERT_NO_THROW(b.solver.advance(.05, .1));
    const auto& t = a.gw.transport();
    EXPECT_NEAR(t.storage(0), initial * std::exp(-100), initial * 1e-8);
    EXPECT_NEAR(t.storage(1), initial * 1000 / 999.8 * (std::exp(-.02) - std::exp(-100)),
                initial * 1e-8);
    for (const auto* mass : {&t.sat_mass, &t.layer_mass})
        for (double value : *mass) {
            EXPECT_TRUE(std::isfinite(value));
            EXPECT_GE(value, 0);
        }
    for (int species = 0; species < 2; ++species)
        EXPECT_NEAR(t.residual(species), 0, initial * 1e-10);
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(restored.string()), "");
    EXPECT_EQ(rk2File(saved), rk2File(restored));
}

TEST(CoupledGroundwaterRk2, ReactionsDecayingPrecursorAndAnotherSpeciesLawRemainSymmetric) {
    RkReactionSystem system("* SAT B RATE A\n* UNSAT B RATE A\n");
    double previous = 0;
    for (double dt : {.1, .05, .025, .0125}) {
        CoupledRkRig r;
        r.init(dt, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(r, system, true);
        auto& t = r.gw.transport();
        for (int cell = 0; cell < t.n_cells; ++cell)
            t.decay[t.idx(0, cell)] = .3;
        const double initial = t.init_mass[0];
        for (int step = 0; step < int(std::round(1 / dt)); ++step)
            r.solver.advance(step * dt, (step + 1) * dt);
        EXPECT_NEAR(t.storage(0), initial * std::exp(-.3), initial * 1e-12);
        const double error = std::abs(t.storage(1) - initial * (1 - std::exp(-.3)) / .3);
        RecordProperty("precursor_" + std::to_string(int(dt * 10000)), error);
        EXPECT_LT(error, initial * 1e-5);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.8);
        previous = error;
        for (int row = 0; row < 2; ++row)
            EXPECT_NEAR(t.residual(row), 0, initial * 1e-10);
    }
}

TEST(CoupledGroundwaterRk2, ReactionsExhaustedWorkBudgetPreservesStateAndValidRetry) {
    RkReactionSystem bad("* SAT A RATE -1e5*A\n* UNSAT A RATE -1e5*A\n", "ROS2"),
        good(kRkConversion);
    CoupledRkRig r;
    r.init(.05, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkReactions(r, bad, true);
    const auto before = rk2Output("reaction-cap-before.chk"),
               after = rk2Output("reaction-cap-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    try {
        r.solver.advance(0, .05);
        FAIL() << "Expected the bounded reaction work budget to fail";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("substep cap"), std::string::npos) << error.what();
    }
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(r.gw.initReactions(good.context()), "");
    ASSERT_NO_THROW(r.solver.advance(0, .05));
    const auto& t = r.gw.transport();
    EXPECT_NEAR(t.storage(0) + t.storage(1), t.init_mass[0], t.init_mass[0] * 1e-10);
}

namespace {
const std::string kRkSurfaceConversion = "RATE A -.3*A\nRATE B .3*A\n";
/**
 * @brief Seed surface species A and bind the test reaction context to RK2.
 *
 * Replaces row-zero surface mass and fatally asserts configureRk2Reactions acceptance. Other
 * species/ledgers remain unchanged; caller keeps the chemistry context alive and uses a compatible
 * scheme.
 *
 * @param[in,out] r Rig with active matching surface reaction layout; row zero is A.
 * @param[in,out] system Live chemistry owner borrowed for surface reaction configuration.
 * @param[in] concentration Native A concentration multiplied by each SI cell water volume.
 */
void seedRkSurfaceReaction(CoupledRkRig& r, RkReactionSystem& system, double concentration = 6) {
    auto& tr = r.state.transport;
    for (int cell = 0; cell < tr.n_cells; ++cell)
        tr.cell_mass[tr.idx(0, cell)] = concentration * r.state.volume[cell];
    ASSERT_EQ(r.solver.configureRk2Reactions(system.context()), "");
}
/**
 * @brief Integrate an independent moving-water/two-constituent column reference over two seconds.
 *
 * Water lengths are metres; constituent contents retain native concentration*water-depth units.
 * Uses local Gardner water, upwind advection, fixed 1 m dispersion/1e-9 m2/s diffusion, deep loss
 * and 0.2/s ground versus 0.3/s surface conversion. No production integrator/forcing calls occur;
 * positive water capacities, valid logarithms and bounded moisture throughout all trial stages are
 * caller preconditions, without defensive clamps or adaptive refinement.
 *
 * @param[in] steps Positive uniform classical-RK4 subdivision count; caller resolves the smooth
 *     physical regime.
 * @param[in] temperature True disables conversion and uses -2 C ground/6 C surface passive
 *     temperature; false uses A=2 ground/6 surface and A-to-B kinetics.
 * @return Owned 18-entry per-unit-area state: saturated height, four layer water depths, surface
 *     depth, then six A/temperature contents and six B contents in SAT/layer0..3/surface order.
 */
std::vector<double> reactiveColumnReference(int steps, bool temperature = false) {
    // Independent smooth Gardner water + two-species finite-volume ODE.
    // Stores: SAT, four moving aqueous layers, surface. No production calls.
    constexpr int stores = 6;
    constexpr double ks = .003, deep = .001, porosity = .45, residual = .1;
    std::vector<double> y(18);
    y[0] = .6;
    y[5] = .2;
    for (int j = 0; j < 4; ++j)
        y[j + 1] = (.24 + .015 * j) * (2 - y[0]) / 4;
    const double initial = temperature ? -2 : 2;
    y[6] = initial * porosity * y[0];
    for (int j = 1; j < 5; ++j)
        y[6 + j] = initial * y[j];
    y[11] = 6 * y[5];
    const auto rhs = [&](const std::vector<double>& v) {
        std::vector<double> out(18), theta(4), k(4), flux(5), water(6);
        const double length = 2 - v[0];
        for (int j = 0; j < 4; ++j) {
            theta[j] = 4 * v[j + 1] / length;
            k[j] = ks * (theta[j] - residual) / (porosity - residual);
        }
        const double table = (k.back() - deep * v[0] / 2) / (porosity - theta.back()),
                     grid = -table;
        flux[0] = ks * (1 + (v[5] - std::log((theta[0] - residual) / (porosity - residual)) / 2) /
                                (.5 * length / 4));
        for (int j = 1; j < 4; ++j)
            flux[j] = k[j - 1] - theta[grid > 0 ? j : j - 1] * (double(j) / 4) * grid;
        flux[4] = k.back() - theta.back() * grid;
        out[0] = table;
        out[5] = -flux[0];
        for (int j = 0; j < 4; ++j)
            out[j + 1] = flux[j] - flux[j + 1];
        water[0] = porosity * v[0];
        for (int j = 1; j < 6; ++j)
            water[j] = v[j];
        for (int store = 0; store < stores; ++store) {
            const double conversion = temperature ? 0 : (store == 5 ? .3 : .2) * v[6 + store];
            out[6 + store] -= conversion;
            out[12 + store] += conversion;
        }
        for (int species = 0; species < 2; ++species) {
            const int base = 6 + 6 * species;
            const auto move = [&](int from, int to, double q) {
                if (q < 0) {
                    std::swap(from, to);
                    q = -q;
                }
                const double amount = q * v[base + from] / water[from];
                out[base + from] -= amount;
                out[base + to] += amount;
            };
            move(5, 1, flux[0]);
            for (int j = 1; j < 4; ++j)
                move(j, j + 1, flux[j]);
            move(4, 0, flux[4]);
            // Default longitudinal dispersivity 1m and molecular 1e-9m2/s.
            for (int j = 1; j < 4; ++j) {
                const double face_theta = 2 * theta[j - 1] * theta[j] / (theta[j - 1] + theta[j]);
                const double coefficient = (face_theta * 1e-9 + std::abs(flux[j])) / (length / 4);
                const double dispersed =
                    coefficient * (v[base + j] / water[j] - v[base + j + 1] / water[j + 1]);
                out[base + j] -= dispersed;
                out[base + j + 1] += dispersed;
            }
            out[base] -= deep * v[0] / 2 * v[base] / water[0];
        }
        return out;
    };
    const double dt = 2. / steps;
    for (int step = 0; step < steps; ++step) {
        const auto a = rhs(y);
        auto v = y;
        for (int j = 0; j < 18; ++j)
            v[j] = y[j] + .5 * dt * a[j];
        const auto b = rhs(v);
        for (int j = 0; j < 18; ++j)
            v[j] = y[j] + .5 * dt * b[j];
        const auto c = rhs(v);
        for (int j = 0; j < 18; ++j)
            v[j] = y[j] + dt * c[j];
        const auto d = rhs(v);
        for (int j = 0; j < 18; ++j)
            y[j] += dt * (a[j] + 2 * b[j] + 2 * c[j] + d[j]) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2, SurfaceReactionsAnalyticalConversionAndIndependentDomainLedgers) {
    for (const std::string integrator : {"RK5", "ROS2", "BDF2"}) {
        RkReactionSystem system(kRkConversion, integrator, kRkSurfaceConversion);
        for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
            for (bool layered : {false, true}) {
                if (layered && closure != GwClosure::SIGMA)
                    continue;
                CoupledRkRig r;
                r.init(.05, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkReactions(r, system, layered);
                seedRkSurfaceReaction(r, system);
                auto& t = r.state.transport;
                const double initial = t.totalIncludingLedgers(0);
                for (int step = 0; step < 20; ++step)
                    ASSERT_NO_THROW(r.solver.advance(.05 * step, .05 * (step + 1)));
                EXPECT_NEAR(t.totalIncludingLedgers(0) + r.solver.surfaceReactionLoss(0), initial,
                            initial * 1e-10);
                EXPECT_NEAR(t.totalIncludingLedgers(1) + r.solver.surfaceReactionLoss(1), 0,
                            initial * 1e-10);
                EXPECT_NEAR(t.totalIncludingLedgers(0), initial * std::exp(-.3), initial * 1e-8);
                EXPECT_NEAR(t.totalIncludingLedgers(0) + t.totalIncludingLedgers(1), initial,
                            initial * 1e-10);
                EXPECT_NEAR(r.solver.surfaceReactionLoss(0) + r.solver.surfaceReactionLoss(1), 0,
                            initial * 1e-10);
                for (int row = 0; row < 2; ++row)
                    EXPECT_NEAR(r.gw.transport().residual(row), 0, initial * 1e-10);
            }
    }
}
TEST(CoupledGroundwaterRk2, SurfaceReactionsAndMovingLayersConvergeToIndependentCoupledOde) {
    const auto exact = reactiveColumnReference(20000), refined = reactiveColumnReference(40000);
    for (int j = 0; j < 18; ++j)
        ASSERT_NEAR(exact[j], refined[j], 1e-11);
    RkReactionSystem system(kRkConversion, "RK5", kRkSurfaceConversion);
    double previous = 0;
    for (double dt : {.1, .05, .025, .0125}) {
        CoupledRkRig r;
        r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        seedRkSigma(r);
        initRkReactions(r, system, true);
        seedRkSurfaceReaction(r, system);
        const auto surface_initial = r.state.transport.cell_mass;
        for (int step = 0; step < int(std::round(2 / dt)); ++step)
            ASSERT_NO_THROW(r.solver.advance(dt * step, dt * (step + 1)));
        const auto& w = r.gw.state();
        const auto& g = r.gw.transport();
        const auto& s = r.state.transport;
        std::vector<double> actual{w.hg[0]};
        for (int j = 0; j < 4; ++j)
            actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (w.zs[0] - w.hg[0]) / 4);
        actual.push_back(r.state.volume[0] / w.area[0]);
        for (int row = 0; row < 2; ++row) {
            actual.push_back(g.sat_mass[g.idx(row, 0)] / w.area[0]);
            for (int j = 0; j < 4; ++j)
                actual.push_back(g.layer_mass[g.layerIdx(row, j, 0)] / w.area[0]);
            actual.push_back(s.cell_mass[s.idx(row, 0)] / w.area[0]);
            double initial = 0;
            for (int cell = 0; cell < s.n_cells; ++cell)
                initial += surface_initial[s.idx(row, cell)];
            EXPECT_NEAR(s.totalIncludingLedgers(row) + r.solver.surfaceReactionLoss(row), initial,
                        1e-10);
            EXPECT_NEAR(g.residual(row), 0, 1e-10);
            EXPECT_NEAR(s.lost_infiltration[row], g.gained_infil[row], 1e-12);
        }
        double error = 0;
        for (int j = 0; j < 18; ++j)
            error += std::pow(actual[j] - exact[j], 2);
        error = std::sqrt(error);
        RecordProperty("surface_reactive_column_" + std::to_string(int(dt * 10000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, SurfaceReactionCheckpointKeepsSignedLedgersAndRejectsChangedPipeLaw) {
    // No groundwater laws: refusal must bind the surface owner itself.
    RkReactionSystem system("", "RK5", kRkSurfaceConversion),
        changed("", "RK5", "RATE A -.4*A\nRATE B .4*A\n");
    CoupledRkRig a, b, bad;
    for (auto* r : {&a, &b, &bad}) {
        auto& ctx = r == &bad ? changed : system;
        r->init(.025, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(*r, ctx, true);
        seedRkSurfaceReaction(*r, ctx);
    }
    const auto saved = rk2Output("surface-reactions.chk"),
               before = rk2Output("surface-reactions-before.chk"),
               after = rk2Output("surface-reactions-after.chk");
    for (int step = 0; step < 40; ++step) {
        ASSERT_NO_THROW(a.solver.advance(.025 * step, .025 * (step + 1)));
        if (step == 12) {
            ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(bad.solver.saveCheckpoint(before.string()), "");
            EXPECT_FALSE(bad.solver.loadCheckpoint(saved.string()).empty());
            ASSERT_EQ(bad.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
        } else if (step > 12)
            ASSERT_NO_THROW(b.solver.advance(.025 * step, .025 * (step + 1)));
    }
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(saved), rk2File(after));
    EXPECT_EQ(a.solver.surfaceReactionLoss(0), b.solver.surfaceReactionLoss(0));
    EXPECT_EQ(a.solver.surfaceReactionLoss(1), b.solver.surfaceReactionLoss(1));
}
TEST(CoupledGroundwaterRk2, SurfaceReactionLaterCellAndLaterKnotFailuresRestoreEveryOwner) {
    RkReactionSystem good(kRkConversion, "RK5", kRkSurfaceConversion),
        bad_cell(kRkConversion, "RK5", "RATE A -sqrt(A-1)*A\n"),
        bad_knot(kRkConversion, "RK5", "RATE A -sqrt(.1-DT)*A\n");
    for (bool knot : {false, true}) {
        auto& ctx = knot ? bad_knot : bad_cell;
        CoupledRkRig r;
        r.init(.4, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(r, ctx, true);
        if (knot) {
            GwResolvedSource source;
            source.name = "reaction-clock-cuts";
            source.cells = {{0, 1}};
            source.flow.times = {0, .1, .4};
            source.flow.values = {0, 0, 0};
            r.gw.setSources({source});
        }
        seedRkSurfaceReaction(r, ctx);
        if (!knot)
            r.state.transport.cell_mass[r.state.transport.idx(0, 1)] = .5 * r.state.volume[1];
        ExplicitInertialSolverTestAccess::prepare(r.solver);
        const auto before = rk2Output("surface-failed-before.chk"),
                   after = rk2Output("surface-failed-after.chk");
        ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
        EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, .4, 0), std::runtime_error);
        ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        ASSERT_EQ(r.gw.initReactions(good.context()), "");
        ASSERT_EQ(r.solver.configureRk2Reactions(good.context()), "");
        ASSERT_NO_THROW(r.solver.advance(0, .4));
        EXPECT_GT(r.solver.surfaceReactionLoss(0), 0);
        EXPECT_LT(r.solver.surfaceReactionLoss(1), 0);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceReactionRejectsUnavailableVariablesBeforeMutation) {
    RkReactionSystem bad(kRkConversion, "RK5", "RATE A -HRT*A\n"),
        good(kRkConversion, "RK5", kRkSurfaceConversion);
    CoupledRkRig r;
    r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkReactions(r, bad, true);
    const auto mass = r.state.transport.cell_mass;
    EXPECT_FALSE(r.solver.configureRk2Reactions(bad.context()).empty());
    EXPECT_EQ(r.state.transport.cell_mass, mass);
    EXPECT_DOUBLE_EQ(r.solver.surfaceReactionLoss(0), 0);
    ASSERT_EQ(r.gw.initReactions(good.context()), "");
    seedRkSurfaceReaction(r, good);
    ASSERT_NO_THROW(r.solver.advance(0, .05));
    EXPECT_GT(r.solver.surfaceReactionLoss(0), 0);
}
TEST(CoupledGroundwaterRk2,
     SurfaceReactionEulerAndAlgebraicProjectionsRetainTheirOwnOrderAndLedgers) {
    for (const std::string law :
         {"FORMULA B 2*A\n", "EQUIL B B-2*A\n", "RATE A -.3*A\nRATE B .3*A\n"}) {
        const bool euler = law.find("RATE") == 0;
        RkReactionSystem system(kRkConversion, euler ? "EUL" : "RK5", law);
        CoupledRkRig r;
        r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(r, system, true);
        seedRkSurfaceReaction(r, system);
        auto& t = r.state.transport;
        const double initial = t.totalIncludingLedgers(0);
        for (int step = 0; step < 20; ++step)
            ASSERT_NO_THROW(r.solver.advance(.05 * step, .05 * (step + 1)));
        EXPECT_NEAR(t.totalIncludingLedgers(0),
                    euler ? initial * std::pow(1 - .3 * .025, 40) : initial, initial * 1e-10);
        EXPECT_NEAR(t.totalIncludingLedgers(1),
                    euler ? initial - t.totalIncludingLedgers(0) : 2 * initial, initial * 1e-10);
        EXPECT_NEAR(t.totalIncludingLedgers(1) + r.solver.surfaceReactionLoss(1), 0,
                    initial * 1e-10);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceDecayLeavesAgeUnreactedAndBindsCheckpointIdentity) {
    RkReactionSystem system("");
    system.context().pollutants.resize_pollutants(1);
    system.context().pollutants.k_decay[0] = .3;
    CoupledRkRig a, b, bad;
    for (auto* r : {&a, &b, &bad}) {
        r->init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2);
        ASSERT_EQ(r->solver.configureRk2Reactions(system.context()), "");
    }
    system.context().pollutants.k_decay[0] = .4;
    ASSERT_EQ(bad.solver.configureRk2Reactions(system.context()), "");
    system.context().pollutants.k_decay[0] = .3;
    const auto initial = a.state.transport.totalIncludingLedgers(0);
    const auto saved = rk2Output("surface-decay.chk");
    ASSERT_NO_THROW(a.solver.advance(0, .5));
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    EXPECT_FALSE(bad.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.5, 1));
    ASSERT_NO_THROW(b.solver.advance(.5, 1));
    EXPECT_NEAR(a.state.transport.totalIncludingLedgers(0), initial * std::exp(-.3),
                initial * 1e-12);
    EXPECT_NEAR(a.state.transport.totalIncludingLedgers(0) + a.solver.surfaceReactionLoss(0),
                initial, initial * 1e-10);
    EXPECT_DOUBLE_EQ(a.solver.surfaceReactionLoss(1), 0);
    EXPECT_EQ(a.state.transport.cell_mass, b.state.transport.cell_mass);
    EXPECT_EQ(a.solver.surfaceReactionLoss(0), b.solver.surfaceReactionLoss(0));
}
TEST(CoupledGroundwaterRk2, SurfaceStiffReactionsRemainBoundedAndRestartExactly) {
    RkReactionSystem system(kRkConversion, "ROS2", "RATE A -1000*A\nRATE B 1000*A-.2*B\n");
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkReactions(*r, system, true);
        seedRkSurfaceReaction(*r, system);
    }
    const double initial = a.state.transport.totalIncludingLedgers(0);
    const auto saved = rk2Output("surface-stiff.chk"),
               restored = rk2Output("surface-stiff-restored.chk");
    ASSERT_NO_THROW(a.solver.advance(0, .05));
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.05, .1));
    ASSERT_NO_THROW(b.solver.advance(.05, .1));
    const auto& t = a.state.transport;
    EXPECT_NEAR(t.totalIncludingLedgers(0), initial * std::exp(-100), initial * 1e-8);
    EXPECT_NEAR(t.totalIncludingLedgers(1),
                initial * 1000 / 999.8 * (std::exp(-.02) - std::exp(-100)), initial * 1e-8);
    for (double mass : t.cell_mass) {
        EXPECT_TRUE(std::isfinite(mass));
        EXPECT_GE(mass, 0);
    }
    EXPECT_NEAR(t.totalIncludingLedgers(0) + a.solver.surfaceReactionLoss(0), initial,
                initial * 1e-10);
    EXPECT_NEAR(t.totalIncludingLedgers(1) + a.solver.surfaceReactionLoss(1), 0, initial * 1e-10);
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(restored.string()), "");
    EXPECT_EQ(rk2File(saved), rk2File(restored));
}

TEST(CoupledGroundwaterRk2, SurfaceDecayReportReceiptsConvertOnceAndSurviveComponentRestart) {
    RkReactionSystem system("");
    auto& ctx = system.context();
    ctx.pollutants.resize_pollutants(1);
    ctx.pollutants.k_decay[0] = .3;
    ctx.mass_balance.qual_routing_reacted.assign(1, 0);
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        r->opts.flow_2d_to_1d = 35.31466672148859;
        initRkQuality(*r, true, false, true);
        seedRkQuality(*r, 2);
        ASSERT_EQ(r->solver.configureRk2Reactions(ctx), "");
    }
    ASSERT_NO_THROW(a.solver.advance(0, .5));
    ExplicitInertialSolverTestAccess::reactionReport(a.solver, ctx);
    const double prefix = ctx.mass_balance.qual_routing_reacted[0];
    EXPECT_NEAR(prefix, a.solver.surfaceReactionLoss(0) * a.opts.flow_2d_to_1d, 1e-12);
    ExplicitInertialSolverTestAccess::reactionReport(a.solver, ctx);
    EXPECT_DOUBLE_EQ(ctx.mass_balance.qual_routing_reacted[0], prefix);
    const auto saved = rk2Output("surface-report-receipt.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ExplicitInertialSolverTestAccess::reactionReport(b.solver, ctx);
    EXPECT_DOUBLE_EQ(ctx.mass_balance.qual_routing_reacted[0], prefix);
    ASSERT_NO_THROW(b.solver.advance(.5, 1));
    ExplicitInertialSolverTestAccess::reactionReport(b.solver, ctx);
    EXPECT_NEAR(ctx.mass_balance.qual_routing_reacted[0],
                b.solver.surfaceReactionLoss(0) * b.opts.flow_2d_to_1d, 1e-12);
    const auto final = ctx.mass_balance.qual_routing_reacted;
    ExplicitInertialSolverTestAccess::reactionReport(b.solver, ctx);
    EXPECT_EQ(ctx.mass_balance.qual_routing_reacted, final);
}

TEST(CoupledGroundwaterRk2, SessionCpuPolicyBindsCheckpointIdentityWithoutMutatingFreshTarget) {
    CoupledRkRig a, b;
    a.init(.05, 0, 0, .2);
    b.init(.05, 0, 0, .2);
    b.opts.auto_backend_cpu = true;
    const auto saved = rk2Output("rk2-policy-source.chk"),
               before = rk2Output("rk2-policy-before.chk"),
               after = rk2Output("rk2-policy-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.opts.auto_backend_cpu = false;
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(b.solver.advance(0, .05));
}

namespace {
/**
 * @brief Seed passive water temperature content in groundwater and surface pools.
 *
 * Fatally requires a GW temperature row, stores temperature*water-volume content in
 * saturated/bulk-or-layered unsaturated/surface cells, refreshes layer totals and resets GW initial
 * temperature inventory. Does not initialize porous-solid energy, change water volumes or clear
 * every ledger; intended for passive temperature transport fixtures.
 *
 * @param[in,out] r Rig with allocated matching GW/surface temperature rows and consistent
 *     water/layer arrays.
 * @param[in] ground Uniform groundwater temperature in degrees C.
 * @param[in] surface Uniform surface temperature in degrees C.
 */
void seedRkTemperature(CoupledRkRig& r, double ground, double surface) {
    auto& t = r.gw.transport();
    const auto& w = r.gw.state();
    const int row = t.temp_row;
    ASSERT_GE(row, 0);
    for (int cell = 0; cell < w.n_cells; ++cell) {
        t.sat_mass[t.idx(row, cell)] = ground * w.area[cell] * w.hg[cell] * w.theta_s[cell];
        if (t.n_layers) {
            for (int j = 0; j < t.n_layers; ++j)
                t.layer_mass[t.layerIdx(row, j, cell)] =
                    ground * w.area[cell] * (w.zs[cell] - w.hg[cell]) / t.n_layers *
                    w.theta_sigma[std::size_t(j) * w.n_cells + cell];
            t.refreshUnsat(row, cell);
        } else
            t.unsat_mass[t.idx(row, cell)] = ground * w.area[cell] * w.hu[cell];
        r.state.transport.cell_mass[r.state.transport.idx(r.state.transport.temp_row, cell)] =
            surface * r.state.volume[cell];
    }
    t.init_mass[row] = t.storage(row);
}
/**
 * @brief Initialize a passive temperature-only GW/surface RK2 layout.
 *
 * Creates one reserved temperature row, disables coupled-heat option, calls seedRkTemperature,
 * reinitializes/rebinds solver and fatally requires RK2 profile acceptance. Discards transport
 * warnings; does not create solid heat storage or reaction bindings.
 *
 * @param[in,out] r Water-initialized rig whose transport/solver state is rebuilt.
 * @param[in] layered Select layered versus bulk unsaturated temperature content.
 * @param[in] ground Initial groundwater temperature in degrees C.
 * @param[in] surface Initial surface temperature in degrees C.
 */
void initRkTemperature(CoupledRkRig& r, bool layered, double ground = -2, double surface = 6) {
    RowLayoutLite rows;
    rows.n_species = 1;
    rows.temp_row = 0;
    rows.names = {"__TEMPERATURE__"};
    GwTransportData data;
    data.options.unsat_transport = layered ? "LAYERED" : "BULK";
    std::vector<std::string> warnings;
    r.gw.initTransport(rows, &data, {}, warnings);
    r.state.transport.resize(1, r.mesh.n_cells(), 0);
    auto& t = r.state.transport;
    t.n_pollut = 0;
    t.temp_row = 0;
    t.row_names = rows.names;
    r.opts.groundwater = 1;
    r.opts.coupled_heat = false;
    seedRkTemperature(r, ground, surface);
    r.solver.initialize(r.mesh, r.state, r.opts);
    r.solver.setSubsurface(&r.gw);
    ASSERT_EQ(r.gw.rk2ProfileError(), "");
}
}
TEST(CoupledGroundwaterRk2, WaterTemperatureStirredAquiferCrossesZeroAtSecondOrder) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkTemperature(r, layered, -.2, -.2);
                auto& t = r.gw.transport();
                const auto& w = r.gw.state();
                GwResolvedSource source;
                source.name = "temperature-mixing";
                source.cells = {{0, 1}};
                source.flow.constant = .1;
                GwResolvedSourceTerm term;
                term.row = 0;
                term.signal.constant = 7;
                source.terms = {term};
                r.gw.setSources({source});
                GwSourceSignal flow;
                flow.constant = -.1;
                auto edge = rk2FlowEdge(r, 0, flow);
                edge.mass_in.assign(1, 0);
                edge.mass_out.assign(1, 0);
                edge.flux_in.assign(1, 0);
                edge.flux_out.assign(1, 0);
                r.gw.setBoundaries({edge});
                const double capacity = w.area[0] * w.hg[0] * w.theta_s[0];
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(dt * step, dt * (step + 1)));
                const double expected = 7 - 7.2 * std::exp(-.1 * 2 / capacity),
                             actual = t.sat_mass[0] / capacity;
                const double error = std::abs(actual - expected);
                EXPECT_GT(actual, 0);
                EXPECT_LT(error, 1e-4);
                RecordProperty("temperature_mixing_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                EXPECT_NEAR(t.residual(0), 0, 1e-10 * std::max(1., std::abs(t.init_mass[0])));
                EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
            }
        }
}
TEST(CoupledGroundwaterRk2, WaterTemperatureMovingLayersMatchIndependentSignedTransportOde) {
    const auto exact = reactiveColumnReference(20000, true),
               refined = reactiveColumnReference(40000, true);
    for (int j = 0; j < 18; ++j)
        ASSERT_NEAR(exact[j], refined[j], 1e-11);
    double previous = 0;
    for (double dt : {.1, .05, .025, .0125}) {
        CoupledRkRig r;
        r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        seedRkSigma(r);
        initRkTemperature(r, true);
        const double initial = r.state.transport.totalIncludingLedgers(0);
        for (int step = 0; step < int(std::round(2 / dt)); ++step)
            ASSERT_NO_THROW(r.solver.advance(dt * step, dt * (step + 1)));
        const auto& w = r.gw.state();
        const auto& t = r.gw.transport();
        std::vector<double> actual{w.hg[0]};
        for (int j = 0; j < 4; ++j)
            actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (w.zs[0] - w.hg[0]) / 4);
        actual.push_back(r.state.volume[0] / w.area[0]);
        actual.push_back(t.sat_mass[0] / w.area[0]);
        for (int j = 0; j < 4; ++j)
            actual.push_back(t.layer_mass[t.layerIdx(0, j, 0)] / w.area[0]);
        actual.push_back(r.state.transport.cell_mass[0] / w.area[0]);
        double error = 0;
        for (int j = 0; j < 12; ++j)
            error += std::pow(actual[j] - exact[j], 2);
        error = std::sqrt(error);
        RecordProperty("water_temperature_column_" + std::to_string(int(dt * 10000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        EXPECT_NEAR(t.residual(0), 0, 1e-10);
        EXPECT_NEAR(r.state.transport.totalIncludingLedgers(0), initial, 1e-10);
        EXPECT_NEAR(r.state.transport.lost_infiltration[0], t.gained_infil[0], 1e-12);
    }
}
TEST(CoupledGroundwaterRk2, WaterTemperatureSetsEachDomainsActualReactionTemperature) {
    const std::string ground =
        "* SAT A RATE -.2*exp(.01*TEMP)*A\n* SAT B RATE .2*exp(.01*TEMP)*A\n* UNSAT A RATE -.2*exp(.01*TEMP)*A\n* UNSAT B RATE .2*exp(.01*TEMP)*A\n";
    RkReactionSystem system(ground, "RK5",
                            "RATE A -.2*exp(.01*TEMP)*A\nRATE B .2*exp(.01*TEMP)*A\n");
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double cold : {-5., 10.}) {
                CoupledRkRig r;
                r.init(.05, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkReactions(r, system, layered, 0, true);
                seedRkTemperature(r, cold, 5 - cold);
                seedRkSurfaceReaction(r, system);
                const double ground_initial = r.gw.transport().init_mass[0],
                             surface_initial = r.state.transport.totalIncludingLedgers(0);
                for (int step = 0; step < 20; ++step)
                    ASSERT_NO_THROW(r.solver.advance(.05 * step, .05 * (step + 1)));
                EXPECT_NEAR(r.gw.transport().storage(0),
                            ground_initial * std::exp(-.2 * std::exp(.01 * cold)),
                            ground_initial * 1e-8);
                EXPECT_NEAR(r.state.transport.totalIncludingLedgers(0),
                            surface_initial * std::exp(-.2 * std::exp(.01 * (5 - cold))),
                            surface_initial * 1e-8);
                EXPECT_DOUBLE_EQ(r.solver.surfaceReactionLoss(2), 0);
                EXPECT_DOUBLE_EQ(r.gw.transport().lost_reaction[2], 0);
                EXPECT_NEAR(r.gw.transport().residual(2), 0, 1e-10);
            }
        }
}
TEST(CoupledGroundwaterRk2, WaterTemperatureColdForcingRestartsExactlyAndBindsHeatMode) {
    CoupledRkRig a, b, bad;
    for (auto* r : {&a, &b, &bad}) {
        r->init(.025, .0001, .0002, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(*r, true);
        GwResolvedSource source;
        source.name = "cold-well";
        source.cells = {{0, 1}};
        source.flow.times = {0, .2, .6, 1};
        source.flow.values = {.01, -.01, .02, 0};
        GwResolvedSourceTerm term;
        term.row = 0;
        term.signal.constant = -5;
        source.terms = {term};
        r->gw.setSources({source});
        GwSourceSignal flow;
        flow.times = {0, .3, .7, 1};
        flow.values = {.001, -.001, .001, 0};
        auto edge = rk2FlowEdge(*r, 0, flow);
        GwBoundaryTerm quality;
        quality.row = 0;
        quality.signal.constant = -8;
        edge.terms = {quality};
        edge.mass_in.assign(1, 0);
        edge.mass_out.assign(1, 0);
        edge.flux_in.assign(1, 0);
        edge.flux_out.assign(1, 0);
        r->gw.setBoundaries({edge});
    }
    bad.opts.water_heat_capacity *= 2;
    const auto saved = rk2Output("cold-temperature.chk"),
               before = rk2Output("cold-temperature-before.chk"),
               after = rk2Output("cold-temperature-after.chk");
    for (int step = 0; step < 40; ++step) {
        ASSERT_NO_THROW(a.solver.advance(.025 * step, .025 * (step + 1)));
        if (step == 9) {
            ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
            ASSERT_EQ(bad.solver.saveCheckpoint(before.string()), "");
            EXPECT_FALSE(bad.solver.loadCheckpoint(saved.string()).empty());
            ASSERT_EQ(bad.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
        } else if (step > 9)
            ASSERT_NO_THROW(b.solver.advance(.025 * step, .025 * (step + 1)));
    }
    EXPECT_LT(a.gw.boundaries()[0].mass_in[0], 0);
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(saved), rk2File(after));
    EXPECT_NEAR(a.gw.transport().residual(0), 0, 1e-10);
    bad.opts.water_heat_capacity = a.opts.water_heat_capacity;
    bad.opts.coupled_heat = true;
    EXPECT_THROW(bad.solver.advance(0, .025), std::runtime_error);
}
TEST(CoupledGroundwaterRk2, WaterTemperatureLaterFailureRollsBackAllSignedStoresAndReceipts) {
    CoupledRkRig r;
    r.init(.1, .005, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkTemperature(r, true);
    GwResolvedSource source;
    source.name = "cold-overflow";
    source.cells = {{0, 1}};
    source.flow.times = {0, .5, 4.5};
    source.flow.values = {.001, .001, 1e308};
    GwResolvedSourceTerm term;
    term.row = 0;
    term.signal.constant = -3;
    source.terms = {term};
    r.gw.setSources({source});
    ExplicitInertialSolverTestAccess::prepare(r.solver);
    const auto before = rk2Output("cold-failure-before.chk"),
               after = rk2Output("cold-failure-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4.5, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    source.flow.times.clear();
    source.flow.values.clear();
    source.flow.constant = .001;
    r.gw.setSources({source});
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    EXPECT_NEAR(r.gw.transport().residual(0), 0, 1e-10);
}

TEST(InertialMarcher, SignedTemperatureAuthoringKeepsChemistryAndAgeNonnegative) {
    std::vector<SurfaceRouter2D::PendingInitialQualityRow> initial;
    std::vector<SurfaceRouter2D::PendingBoundaryQualityRow> boundary;
    ASSERT_EQ(parse2DInitialQualityLine({"*", "__TEMPERATURE__", "-4"}, initial), "");
    ASSERT_EQ(parse2DBoundaryQualityLine({"1", "0", "__TEMPERATURE__", "-7"}, boundary), "");
    EXPECT_DOUBLE_EQ(initial[0].conc, -4);
    EXPECT_DOUBLE_EQ(boundary[0].conc, -7);
    for (const std::string species : {"TSS", "A", "__WATER_AGE__"}) {
        EXPECT_FALSE(parse2DInitialQualityLine({"*", species, "-1"}, initial).empty());
        EXPECT_FALSE(parse2DBoundaryQualityLine({"1", "0", species, "-1"}, boundary).empty());
    }
    for (const std::string value : {"nan", "inf", "1e309"}) {
        EXPECT_FALSE(parse2DInitialQualityLine({"*", "__TEMPERATURE__", value}, initial).empty());
        EXPECT_FALSE(
            parse2DBoundaryQualityLine({"1", "0", "__TEMPERATURE__", value}, boundary).empty());
    }
    EXPECT_EQ(initial.size(), 1);
    EXPECT_EQ(boundary.size(), 1);
}

TEST(CoupledGroundwaterRk2, WaterTemperatureImposedHeatFluxHasIndependentJoulesAndNoWater) {
    constexpr double cp = 4186000;
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double power : {-10000., 10000.}) {
                CoupledRkRig r;
                r.init(.1, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkTemperature(r, layered, -2, -2);
                const double initial = r.gw.transport().storage(0);
                GwSourceSignal flow;
                auto edge = rk2FlowEdge(r, 0, flow);
                GwBoundaryTerm term;
                term.row = 0;
                term.flux = true;
                term.scale = edge.length / cp;
                term.signal.constant = power;
                edge.terms = {term};
                edge.mass_in.assign(1, 0);
                edge.mass_out.assign(1, 0);
                edge.flux_in.assign(1, 0);
                edge.flux_out.assign(1, 0);
                r.gw.setBoundaries({edge});
                for (int step = 0; step < 20; ++step)
                    ASSERT_NO_THROW(r.solver.advance(.1 * step, .1 * (step + 1)));
                const auto& t = r.gw.transport();
                const auto& boundary = r.gw.boundaries()[0];
                EXPECT_NEAR(cp * (t.storage(0) - initial), power * edge.length * 2, 1e-6);
                EXPECT_DOUBLE_EQ(boundary.applied_in, 0);
                EXPECT_DOUBLE_EQ(boundary.applied_out, 0);
                EXPECT_DOUBLE_EQ(boundary.mass_in[0], 0);
                EXPECT_DOUBLE_EQ(boundary.mass_out[0], 0);
                EXPECT_NEAR(cp * (boundary.flux_in[0] - boundary.flux_out[0]),
                            power * edge.length * 2, 1e-6);
                EXPECT_NEAR(t.residual(0), 0, 1e-10);
            }
        }
}
TEST(CoupledGroundwaterRk2, WaterTemperatureEtKeepsSignedSensibleExportsAndUniformWetStores) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double temperature : {-3., 5.})
                for (double pond : {0., .01}) {
                    CoupledRkRig r;
                    r.init(.05, 0, 0, pond, "BOTH", closure, SoilChar::GARDNER, 4);
                    initRkTemperature(r, layered, temperature, temperature);
                    r.state.evap_rate.assign(r.mesh.n_cells(), 1e-4);
                    const double surface_initial = r.state.transport.totalIncludingLedgers(0);
                    for (int step = 0; step < 40; ++step)
                        ASSERT_NO_THROW(r.solver.advance(.05 * step, .05 * (step + 1)));
                    const auto& w = r.gw.state();
                    const auto& t = r.gw.transport();
                    const auto& s = r.state.transport;
                    EXPECT_GT(w.led_et + r.state.evap_loss_total, 0);
                    EXPECT_NEAR(t.lost_et[0], temperature * w.led_et, 1e-8);
                    EXPECT_NEAR(t.residual(0), 0, 1e-10);
                    EXPECT_NEAR(s.totalIncludingLedgers(0), surface_initial, 1e-10);
                    for (int cell = 0; cell < w.n_cells; ++cell) {
                        const double sat = w.area[cell] * w.hg[cell] * w.theta_s[cell],
                                     unsat = w.area[cell] * w.hu[cell];
                        if (sat > 1e-10)
                            EXPECT_NEAR(t.sat_mass[t.idx(0, cell)] / sat, temperature, 1e-5);
                        if (unsat > 1e-10)
                            EXPECT_NEAR(t.unsat_mass[t.idx(0, cell)] / unsat, temperature, 1e-5);
                    }
                }
        }
}

TEST(CoupledGroundwaterRk2, WaterTemperatureHeaterAndKineticsUseOnePhysicalSecondOrderClock) {
    const std::string ground =
        "* SAT A RATE -.2*exp(.01*TEMP)*A\n* SAT B RATE .2*exp(.01*TEMP)*A\n* UNSAT A RATE -.2*exp(.01*TEMP)*A\n* UNSAT B RATE .2*exp(.01*TEMP)*A\n";
    RkReactionSystem system(ground);
    constexpr double cp = 4186000;
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                initRkReactions(r, system, layered, 0, true);
                seedRkTemperature(r, -2, 6);
                r.gw.transport().dispersion_on = false;
                auto& t = r.gw.transport();
                const double initial = t.sat_mass[0], volume = r.gw.state().area[0] *
                                                               r.gw.state().hg[0] *
                                                               r.gw.state().theta_s[0];
                GwSourceSignal flow;
                auto edge = rk2FlowEdge(r, 0, flow);
                GwBoundaryTerm term;
                term.row = 2;
                term.flux = true;
                term.scale = edge.length / cp;
                term.signal.constant = cp * volume * 5 / edge.length;
                edge.terms = {term};
                edge.mass_in.assign(3, 0);
                edge.mass_out.assign(3, 0);
                edge.flux_in.assign(3, 0);
                edge.flux_out.assign(3, 0);
                r.gw.setBoundaries({edge});
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(dt * step, dt * (step + 1)));
                EXPECT_NEAR(r.solver.last_step_size(), dt, 1e-14);
                const double integrated = .2 * std::exp(-.02) * std::expm1(.1) / .05;
                const double error =
                    std::abs(t.sat_mass[0] - initial * std::exp(-integrated)) / initial;
                RecordProperty("heated_kinetics_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                EXPECT_NEAR(t.sat_mass[t.idx(2, 0)] / volume, 8, 1e-10);
                EXPECT_NEAR(t.storage(0) + t.storage(1), t.init_mass[0], 1e-10);
                EXPECT_DOUBLE_EQ(t.lost_reaction[2], 0);
                for (int row = 0; row < 3; ++row)
                    EXPECT_NEAR(t.residual(row), 0, 1e-10);
            }
        }
}

namespace {
struct RkPorousOwner {
    openswmm::SimulationContext context;
    GwTransportData config;
    /**
     * @brief Initialize owned porous thermal configuration and bind it to the numerical rig.
     *
     * Owner context/config must remain alive through bound heat use; intended for fresh owner because
     * config rows append. Sets water heat capacity 4186000 J/(m3 K), porous-equilibrium/SENSIBLE_ONLY
     * heat, adiabatic boundaries and -2 C SAT/UNSAT/surface seeds. Fatally requires heat/porewater/RK2
     * initialization; discards other warnings and leaves partial state after a helper assertion.
     *
     * @param[in,out] r Water rig whose transport/heat/porewater/solver state is rebuilt.
     * @param[in] layered Select layered versus bulk aqueous temperature inventory.
     * @param[in] layers Positive thermal-column layer count.
     * @param[in] conduction Enable vertical ground conduction.
     * @param[in] conductivity Solid material thermal conductivity in W/(m K).
     * @param[in] second_conductivity Positive value overrides conductivity in cell one; nonpositive
     *     omits that row.
     * @param[in] lateral Enable lateral thermal conduction.
     */
    void initialize(CoupledRkRig& r, bool layered, int layers = 1, bool conduction = false,
                    double conductivity = 2, double second_conductivity = 0, bool lateral = false) {
        context.options.heat_transport = true;
        context.options.water_density = 1000;
        context.options.water_specific_heat = 4186;
        config.options.thermal_model = "POROUS_EQUILIBRIUM";
        config.options.thermal_layers = layers;
        config.options.unsat_transport = layered ? "LAYERED" : "BULK";
        config.options.conduction = conduction;
        config.options.dispersion = false;
        config.options.lateral_conduction = lateral;
        config.options.soil_et_heat = "SENSIBLE_ONLY";
        config.options.surface_thermal_bc = config.options.deep_thermal_bc = "ADIABATIC";
        GwParamsRow material;
        material.lambda_s = conductivity;
        config.params = {material};
        if (second_conductivity > 0) {
            material.scope = GwScope::CELL;
            material.cell = 1;
            material.lambda_s = second_conductivity;
            config.params.push_back(material);
        }
        GwInitialQualityRow initial;
        initial.species = "__TEMPERATURE__";
        initial.value = -2;
        initial.zone = GwZone::SAT;
        config.initial_quality.push_back(initial);
        initial.zone = GwZone::UNSAT;
        config.initial_quality.push_back(initial);
        RowLayoutLite rows;
        rows.n_species = 1;
        rows.temp_row = 0;
        rows.names = {"__TEMPERATURE__"};
        std::vector<std::string> warnings;
        r.gw.initTransport(rows, &config, {}, warnings);
        r.state.transport.resize(1, r.mesh.n_cells(), 0);
        auto& tr = r.state.transport;
        tr.n_pollut = 0;
        tr.temp_row = 0;
        tr.row_names = rows.names;
        for (int cell = 0; cell < tr.n_cells; ++cell)
            tr.cell_mass[tr.idx(0, cell)] = -2 * r.state.volume[cell];
        r.opts.groundwater = 1;
        r.opts.coupled_heat = false;
        r.opts.water_heat_capacity = 4186000;
        ASSERT_EQ(r.gw.initHeat(context, config), "");
        ASSERT_EQ(r.gw.initPorewater(), "");
        r.solver.initialize(r.mesh, r.state, r.opts);
        r.solver.setSubsurface(&r.gw);
        ASSERT_EQ(r.gw.rk2ProfileError(), "");
    }
};
const std::string kRkPorousConversion =
    "* SAT A RATE -.2*exp(.01*TEMP)*A\n* SAT B RATE .2*exp(.01*TEMP)*A\n"
    "* UNSAT A RATE -.2*exp(.01*TEMP)*A\n* UNSAT B RATE .2*exp(.01*TEMP)*A\n";
const std::string kRkPorousSurfaceConversion =
    "RATE A -.3*exp(.01*TEMP)*A\nRATE B .3*exp(.01*TEMP)*A\n";
/**
 * @brief Build coupled porous heat, two reacting species and optional age inventories.
 *
 * Seeds A=2, B=0, optional age=5 s and requested temperature in all water stores, including sorbed
 * capacities for solutes; resets GW init_mass. Initializes heat/porewater/reactions, recreates
 * surface layout, binds solver/GW and surface reactions, and fatally checks compatibility. All
 * owners must remain alive/unmoved through advancement. No authoring/range validation occurs beyond
 * downstream initialization; warnings are discarded and partial setup can remain after assertions.
 *
 * @param[in,out] r Water rig whose GW/surface transport and solver state are replaced.
 * @param[in,out] owner Fresh live porous owner providing retained configuration/context.
 * @param[in,out] system Live chemistry owner providing retained reaction/heat context.
 * @param[in] layered Use layered rather than bulk aqueous transport.
 * @param[in] age Add native seconds-valued water age after A/B.
 * @param[in] kd Native A/B partition coefficient in m3/kg; converted to authored L/kg via *1000.
 * @param[in] temperature Uniform initial ground/surface temperature in degrees C.
 * @param[in] thermal_layers Positive thermal-column layer count.
 * @param[in] conduction Enable vertical ground conduction.
 */
void initRkPorousQuality(CoupledRkRig& r, RkPorousOwner& owner, RkReactionSystem& system,
                         bool layered, bool age = false, double kd = 0, double temperature = -2,
                         int thermal_layers = 3, bool conduction = false) {
    owner.initialize(r, layered, thermal_layers, conduction);
    auto& ctx = system.context();
    ctx.options.heat_transport = true;
    ctx.options.water_density = 1000;
    ctx.options.water_specific_heat = 4186;
    for (auto& row : owner.config.initial_quality)
        row.value = temperature;
    owner.config.options.transport_msx = true;
    owner.config.options.transport_age = age;
    for (const auto& name : {"A", "B"}) {
        GwSorptionRow row;
        row.species = name;
        row.kd = kd * 1000;
        owner.config.sorption.push_back(row);
    }
    RowLayoutLite rows;
    rows.n_msx = 2;
    rows.names = {"A", "B"};
    if (age) {
        rows.age_row = 2;
        rows.names.push_back("__WATER_AGE__");
    }
    rows.temp_row = int(rows.names.size());
    rows.names.push_back("__TEMPERATURE__");
    rows.n_species = int(rows.names.size());
    std::vector<std::string> warnings;
    r.gw.initTransport(rows, &owner.config, {}, warnings);
    auto& tr = r.gw.transport();
    const auto& w = r.gw.state();
    for (int species = 0; species < rows.n_species; ++species)
        for (int cell = 0; cell < w.n_cells; ++cell) {
            const auto index = tr.idx(species, cell);
            const double partition = tr.kd[index] * tr.rho_b[cell];
            const double concentration = species == 0               ? 2
                                         : species == rows.age_row  ? 5
                                         : species == rows.temp_row ? temperature
                                                                    : 0;
            tr.sat_mass[index] =
                concentration * w.area[cell] * w.hg[cell] * (w.theta_s[cell] + partition);
            if (layered) {
                for (int j = 0; j < tr.n_layers; ++j)
                    tr.layer_mass[tr.layerIdx(species, j, cell)] =
                        concentration * w.area[cell] * (w.zs[cell] - w.hg[cell]) / tr.n_layers *
                        (w.theta_sigma[std::size_t(j) * w.n_cells + cell] + partition);
                tr.refreshUnsat(species, cell);
            } else
                tr.unsat_mass[index] = concentration * w.area[cell] *
                                       (w.hu[cell] + partition * (w.zs[cell] - w.hg[cell]));
        }
    for (int species = 0; species < rows.n_species; ++species)
        tr.init_mass[species] = tr.storage(species);
    ASSERT_EQ(r.gw.initHeat(ctx, owner.config), "");
    ASSERT_EQ(r.gw.initPorewater(), "");
    ASSERT_EQ(r.gw.initReactions(ctx), "");
    auto& surface = r.state.transport;
    surface.resize(rows.n_species, r.mesh.n_cells(), 0);
    surface.n_pollut = 0;
    surface.n_msx = 2;
    surface.age_row = rows.age_row;
    surface.temp_row = rows.temp_row;
    surface.row_names = rows.names;
    for (int species = 0; species < rows.n_species; ++species)
        for (int cell = 0; cell < w.n_cells; ++cell) {
            const double concentration = species == 0               ? 2
                                         : species == rows.age_row  ? 5
                                         : species == rows.temp_row ? temperature
                                                                    : 0;
            surface.cell_mass[surface.idx(species, cell)] = concentration * r.state.volume[cell];
        }
    r.solver.initialize(r.mesh, r.state, r.opts);
    r.solver.setSubsurface(&r.gw);
    ASSERT_EQ(r.solver.configureRk2Reactions(ctx), "");
    ASSERT_EQ(r.gw.rk2ProfileError(), "");
}
/**
 * @brief Sum porous groundwater thermal-column energy across cells/layers.
 *
 * Does not include surface water energy or thermal export ledgers and does not mutate state.
 *
 * @param[in] r Borrowed rig containing initialized or empty heat state.
 * @return Total stored energy in joules, zero when no layers exist.
 */
double rkPorousEnergy(const CoupledRkRig& r) {
    double value = 0;
    for (const auto& cell : r.gw.heat().cells)
        value += std::accumulate(cell.column.energy.begin(), cell.column.energy.end(), 0.);
    return value;
}
/**
 * @brief Assert each porous thermal column closes its stored-energy ledger.
 *
 * Nonfatally compares energy-initial_j against
 * advective/source/surface/deep/latent/lateral/water/channel/LID contact joules, using
 * 1e-10*max(1,abs(initial),abs(net)). Does not include unrelated surface energy, rerun heat or
 * repair errors.
 *
 * @param[in] r Borrowed rig with populated column energy and heat-term counters.
 */
void checkRkPorousLedgers(const CoupledRkRig& r) {
    for (const auto& c : r.gw.heat().cells) {
        const double energy = std::accumulate(c.column.energy.begin(), c.column.energy.end(), 0.);
        const double net = c.advective_j + c.source_j + c.surface_j + c.deep_j + c.latent_j +
                           c.lateral_j + c.water_contact_j + c.channel_contact_j + c.lid_contact_j;
        EXPECT_NEAR(energy - c.initial_j, net,
                    1e-10 * std::max({1., std::abs(c.initial_j), std::abs(net)}));
    }
}
/**
 * @brief Integrate the independent continuous water/porous-energy/chemistry column reference.
 *
 * Uses fixed Gardner water, three physical heat layers, dual-number overlap derivatives, material
 * heat storage/conduction, upwind transfers and temperature-dependent reactions. Optional chemistry
 * contents retain native concentration-depth or seconds-depth units. Does not call staged
 * production projections/integrators. Requires positive capacities/ponding, valid logarithms and
 * nonzero overlap/partition denominators throughout all trial stages; there is no general bound
 * handling, finite validation or adaptive timestep selection.
 *
 * @param[in] steps Positive classical-RK4 subdivision count over two seconds, resolving the
 *     intended smooth/stiff regime.
 * @param[in] chemistry Append reacting A/B and water-age states when true.
 * @param[in] layered For chemistry, use four moving aqueous layers plus SAT; false uses three fixed
 *     physical ground stores.
 * @param[in] partition Dimensionless sorption capacity kd*rho_b for A/B; age remains unsorbed.
 * @param[in] surface_contact Enable conductive contact between surface water and the first physical
 *     thermal layer.
 * @param[in] solute_contact When chemistry is active, enable weighted finite-bed solute/age
 *     contact.
 * @param[in] rainfall Nonnegative uniform inflow depth rate in m/s; carries -8 C heat, A
 *     concentration 7 and age 3 s.
 * @param[in] ground_flow Nonnegative paired well/pump rate per unit area in m/s at the deepest
 *     store, with fixed incoming composition and extra prescribed A/heat rates.
 * @return Owned vector: entries 0..5 are saturated height/four aqueous water depths/surface depth
 *     in m, entry 6 surface C*m content, entries 7..9 ground energy per area divided by 4186000
 *     J/(m3 K); optional A/B/age blocks follow, each ground_stores+1 wide with surface last.
 */
std::vector<double> porousColumnReference(int steps, bool chemistry = false, bool layered = false,
                                          double partition = 0, bool surface_contact = false,
                                          bool solute_contact = false, double rainfall = 0,
                                          double ground_flow = 0) {
    // Independent continuous Gardner water/energy ODE. Differentiate the
    // overlap integrals with dual numbers, rather than calling the staged
    // projection or reconstructing an accepted numerical water packet.
    constexpr double phi = .45, residual = .1, ks = .003, deep = .001, cp = 4186000, dx = 2. / 3;
    constexpr double grain = (1 - phi) * 2650 * 880 / cp;
    using Dual = std::array<double, 2>;
    const auto add = [](Dual a, Dual b) { return Dual{a[0] + b[0], a[1] + b[1]}; };
    const auto sub = [](Dual a, Dual b) { return Dual{a[0] - b[0], a[1] - b[1]}; };
    const auto mul = [](Dual a, Dual b) { return Dual{a[0] * b[0], a[1] * b[0] + a[0] * b[1]}; };
    const auto low = [](Dual a, Dual b) { return a[0] < b[0] ? a : b; };
    const auto high = [](Dual a, Dual b) { return a[0] > b[0] ? a : b; };
    const auto water = [&](const std::vector<double>& v, const std::vector<double>& rate) {
        const Dual length = {2 - v[0], -rate[0]};
        std::array<Dual, 3> result{};
        for (int j = 0; j < 3; ++j) {
            const Dual top = {dx * j, 0}, bottom = {dx * (j + 1), 0};
            for (int k = 0; k < 4; ++k) {
                const Dual theta = {4 * v[k + 1] / length[0],
                                    4 * rate[k + 1] / length[0] -
                                        4 * v[k + 1] * length[1] / (length[0] * length[0])};
                const auto overlap =
                    high(Dual{0, 0}, sub(low(bottom, mul(length, Dual{double(k + 1) / 4, 0})),
                                         high(top, mul(length, Dual{double(k) / 4, 0}))));
                result[j] = add(result[j], mul(theta, overlap));
            }
            result[j] =
                add(result[j], mul(Dual{phi, 0}, high(Dual{0, 0}, sub(bottom, high(top, length)))));
        }
        return result;
    };
    const int ground_stores = layered ? 5 : 3, width = ground_stores + 1;
    std::vector<double> y(chemistry ? 10 + 3 * width : 10);
    y[0] = .6;
    y[5] = .2;
    y[6] = 6 * y[5];
    for (int k = 0; k < 4; ++k)
        y[k + 1] = (.24 + .015 * k) * (2 - y[0]) / 4;
    const auto initial_water = water(y, std::vector<double>(10));
    for (int j = 0; j < 3; ++j)
        y[7 + j] = -2 * (grain * dx + initial_water[j][0]);
    if (chemistry)
        for (int row = 0; row < 3; ++row) {
            const int base = 10 + row * width;
            for (int j = 0; j < ground_stores; ++j) {
                const double bulk = layered ? (j == 4 ? y[0] : (2 - y[0]) / 4) : dx;
                const double aqueous =
                    layered ? (j == 4 ? phi * y[0] : y[j + 1]) : initial_water[j][0];
                y[base + j] = row == 0   ? 2 * (aqueous + partition * bulk)
                              : row == 2 ? 5 * aqueous
                                         : 0;
            }
            y[base + ground_stores] = (row == 0 ? 6 : row == 2 ? 5 : 0) * y[5];
        }
    const auto rhs = [&](const std::vector<double>& v) {
        std::vector<double> out(v.size());
        std::array<double, 4> theta{}, conductivity{};
        std::array<double, 5> flux{};
        const double length = 2 - v[0];
        for (int k = 0; k < 4; ++k) {
            theta[k] = 4 * v[k + 1] / length;
            conductivity[k] = ks * (theta[k] - residual) / (phi - residual);
        }
        out[0] = (conductivity[3] - deep * v[0] / 2) / (phi - theta[3]);
        const double grid = -out[0];
        flux[0] = ks * (1 + (v[5] - std::log((theta[0] - residual) / (phi - residual)) / 2) /
                                (.5 * length / 4));
        for (int k = 1; k < 4; ++k)
            flux[k] = conductivity[k - 1] - theta[grid > 0 ? k : k - 1] * (double(k) / 4) * grid;
        flux[4] = conductivity[3] - theta[3] * grid;
        for (int k = 0; k < 4; ++k)
            out[k + 1] = flux[k] - flux[k + 1];
        out[5] = -flux[0];
        const auto fixed = water(v, out);
        std::array<double, 3> temperature{}, lambda{}, external{};
        for (int j = 0; j < 3; ++j) {
            temperature[j] = v[7 + j] / (grain * dx + fixed[j][0]);
            const double moisture = fixed[j][0] / dx;
            lambda[j] = (1 - phi) * 2e5 + moisture * .606 + (phi - moisture) * .025;
        }
        external[0] = flux[0];
        external[2] = -deep * v[0] / 2;
        out[6] = -flux[0] * v[6] / v[5];
        out[7] += flux[0] * v[6] / v[5];
        out[9] += external[2] * temperature[2];
        if (ground_flow != 0)
            out[9] += ground_flow * (-8 - temperature[2]) + .0001;
        double vertical = 0;
        std::array<double, 2> vertical_flow{};
        for (int j = 0; j < 2; ++j) {
            vertical += external[j] - fixed[j][1];
            vertical_flow[j] = vertical;
            const double advective =
                vertical * (vertical >= 0 ? temperature[j] : temperature[j + 1]);
            const double contact = (temperature[j] - temperature[j + 1]) /
                                   (cp * (.5 * dx / lambda[j] + .5 * dx / lambda[j + 1]));
            out[7 + j] -= advective + contact;
            out[8 + j] += advective + contact;
        }
        if (surface_contact) {
            const double contact = lambda[0] * (v[6] / v[5] - temperature[0]) / (cp * .5 * dx);
            out[7] += contact;
            out[6] -= contact;
        }
        if (chemistry) {
            std::vector<double> aqueous(ground_stores), bulk(ground_stores),
                local_temperature(ground_stores);
            for (int j = 0; j < ground_stores; ++j) {
                if (layered) {
                    const double top = j == 4 ? length : length * j / 4,
                                 bottom = j == 4 ? 2 : length * (j + 1) / 4;
                    bulk[j] = bottom - top;
                    aqueous[j] = j == 4 ? phi * v[0] : v[j + 1];
                    for (int k = 0; k < 3; ++k)
                        local_temperature[j] +=
                            temperature[k] *
                            std::max(0., std::min(bottom, dx * (k + 1)) - std::max(top, dx * k)) /
                            bulk[j];
                } else {
                    bulk[j] = dx;
                    aqueous[j] = fixed[j][0];
                    local_temperature[j] = temperature[j];
                }
            }
            for (int row = 0; row < 3; ++row) {
                const int base = 10 + row * width;
                std::vector<double> concentration(width);
                const double retained = row == 2 ? 0 : partition;
                for (int j = 0; j < ground_stores; ++j)
                    concentration[j] = v[base + j] / (aqueous[j] + retained * bulk[j]);
                concentration[ground_stores] = v[base + ground_stores] / v[5];
                const auto move = [&](int from, int to, double q) {
                    if (q < 0) {
                        std::swap(from, to);
                        q = -q;
                    }
                    const double amount = q * concentration[from];
                    out[base + from] -= amount;
                    out[base + to] += amount;
                };
                move(ground_stores, 0, flux[0]);
                if (layered) {
                    for (int j = 1; j <= 4; ++j) {
                        move(j - 1, j, flux[j]);
                        move(j - 1, j, -retained * (double(j) / 4) * grid);
                    }
                } else
                    for (int j = 0; j < 2; ++j)
                        move(j, j + 1, vertical_flow[j]);
                out[base + ground_stores - 1] -= deep * v[0] / 2 * concentration[ground_stores - 1];
                if (ground_flow != 0)
                    out[base + ground_stores - 1] +=
                        ground_flow * ((row == 0   ? 7
                                        : row == 2 ? 3
                                                   : 0) -
                                       concentration[ground_stores - 1]) +
                        (row == 0 ? .0002 : 0);
                if (solute_contact) {
                    double total_water = 0;
                    for (const auto& layer : fixed)
                        total_water += layer[0];
                    for (int physical = 0; physical < 3; ++physical) {
                        std::vector<double> capacity(ground_stores, 0);
                        double mapped = 0, sum = 0;
                        for (int j = 0; j < ground_stores; ++j) {
                            if (layered) {
                                const double top = j == 4 ? length : length * j / 4,
                                             bottom = j == 4 ? 2 : length * (j + 1) / 4;
                                const double overlap =
                                    std::max(0., std::min(bottom, dx * (physical + 1)) -
                                                     std::max(top, dx * physical));
                                capacity[j] = overlap * (aqueous[j] / bulk[j] + retained);
                            } else if (j == physical)
                                capacity[j] = aqueous[j] + retained * bulk[j];
                            mapped += concentration[j] * capacity[j];
                            sum += capacity[j];
                        }
                        const double transfer = .04 / 2 * (fixed[physical][0] / total_water) *
                                                (concentration[ground_stores] - mapped / sum);
                        out[base + ground_stores] -= transfer;
                        for (int j = 0; j < ground_stores; ++j)
                            out[base + j] += transfer * (transfer >= 0 ? capacity[j] / sum
                                                                       : concentration[j] *
                                                                             capacity[j] / mapped);
                    }
                }
            }
            for (int j = 0; j < ground_stores; ++j) {
                const double converted = .2 * std::exp(.01 * local_temperature[j]) * aqueous[j] /
                                         (aqueous[j] + partition * bulk[j]) * v[10 + j];
                out[10 + j] -= converted;
                out[10 + width + j] += converted;
                out[10 + 2 * width + j] += aqueous[j];
            }
            const double surface_conversion =
                .3 * std::exp(.01 * v[6] / v[5]) * v[10 + ground_stores];
            out[10 + ground_stores] -= surface_conversion;
            out[10 + width + ground_stores] += surface_conversion;
            out[10 + 2 * width + ground_stores] += v[5];
            if (rainfall != 0) {
                out[10 + ground_stores] += 7 * rainfall;
                out[10 + 2 * width + ground_stores] += 3 * rainfall;
            }
        }
        if (rainfall != 0) {
            out[5] += rainfall;
            out[6] -= 8 * rainfall;
        }
        return out;
    };
    const double dt = 2. / steps;
    for (int step = 0; step < steps; ++step) {
        const auto a = rhs(y);
        auto v = y;
        for (std::size_t k = 0; k < y.size(); ++k)
            v[k] = y[k] + .5 * dt * a[k];
        const auto b = rhs(v);
        for (std::size_t k = 0; k < y.size(); ++k)
            v[k] = y[k] + .5 * dt * b[k];
        const auto c = rhs(v);
        for (std::size_t k = 0; k < y.size(); ++k)
            v[k] = y[k] + dt * c[k];
        const auto d = rhs(v);
        for (std::size_t k = 0; k < y.size(); ++k)
            y[k] += dt * (a[k] + 2 * b[k] + 2 * c[k] + d[k]) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2, PorousStirredWaterRetainsGrainCapacityAndSecondOrderTemperature) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner owner;
                owner.initialize(r, layered);
                const auto& w = r.gw.state();
                const double q = .5, cp = 4186000, area = w.area[0], depth = w.zs[0];
                const double capacity = area * (depth * (1 - w.theta_s[0]) * 2650 * 880 +
                                                cp * (w.hu[0] + w.theta_s[0] * w.hg[0]));
                ASSERT_NEAR(r.gw.heat().cells[0].column.capacity[0], capacity, 1e-7);
                GwResolvedSource source;
                source.name = "porous-stirred";
                source.cells = {{0, 1}};
                source.flow.constant = q;
                GwResolvedSourceTerm term;
                term.row = 0;
                term.signal.constant = 7;
                source.terms = {term};
                r.gw.setSources({source});
                GwSourceSignal flow;
                flow.constant = -q;
                auto edge = rk2FlowEdge(r, 0, flow);
                edge.mass_in.assign(1, 0);
                edge.mass_out.assign(1, 0);
                edge.flux_in.assign(1, 0);
                edge.flux_out.assign(1, 0);
                r.gw.setBoundaries({edge});
                const double initial_water = r.total();
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                const double exact = 7 - 9 * std::exp(-cp * q * 2 / capacity);
                const double actual = r.gw.heat().cells[0].column.energy[0] / capacity,
                             error = std::abs(actual - exact) / 9;
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                RecordProperty("porous_stirred_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                EXPECT_NEAR(r.total(), initial_water, 1e-10);
                EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
                checkRkPorousLedgers(r);
            }
        }
}
TEST(CoupledGroundwaterRk2, PorousThreeLayerConductionMatchesIndependentFiniteVolumeEigenmode) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner owner;
                owner.initialize(r, layered, 3, true, 2e5);
                const auto& c = r.gw.heat().cells[0].column;
                const auto& m = c.material;
                const double dz = 2. / 3, area = r.gw.state().area[0];
                std::array<double, 3> cap{}, lambda{};
                for (int j = 0; j < 3; ++j) {
                    cap[j] = area * dz * ((1 - m.porosity) * 2650 * 880 + c.theta[j] * 4186000);
                    lambda[j] = (1 - m.porosity) * 2e5 + c.theta[j] * .606 +
                                (m.porosity - c.theta[j]) * .025;
                    ASSERT_NEAR(cap[j], c.capacity[j], 1e-7);
                }
                const double g1 = area / (.5 * dz / lambda[0] + .5 * dz / lambda[1]),
                             g2 = area / (.5 * dz / lambda[1] + .5 * dz / lambda[2]);
                const double trace =
                    g1 * (1 / cap[0] + 1 / cap[1]) + g2 * (1 / cap[1] + 1 / cap[2]);
                const double product =
                    g1 * g2 * (cap[0] + cap[1] + cap[2]) / (cap[0] * cap[1] * cap[2]);
                const double rate = .5 * (trace - std::sqrt(trace * trace - 4 * product));
                std::array<double, 3> mode = {1, 1 - rate * cap[0] / g1, 0};
                mode[2] = -(cap[0] + cap[1] * mode[1]) / cap[2];
                const double scale =
                    std::max({std::abs(mode[0]), std::abs(mode[1]), std::abs(mode[2])});
                for (double& v : mode)
                    v /= scale;
                for (int cell = 0; cell < r.gw.state().n_cells; ++cell) {
                    auto& owner_cell = r.gw.heat().cells[cell];
                    for (int j = 0; j < 3; ++j)
                        owner_cell.column.energy[j] =
                            owner_cell.column.capacity[j] * (-2 + mode[j]);
                    owner_cell.column.initial_energy = owner_cell.column.energy;
                    owner_cell.initial_j = std::accumulate(owner_cell.column.energy.begin(),
                                                           owner_cell.column.energy.end(), 0.);
                    r.gw.heat().refreshView(cell, r.gw.transport());
                }
                const double initial = rkPorousEnergy(r);
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                double error = 0;
                for (int j = 0; j < 3; ++j)
                    error = std::max(error, std::abs(c.energy[j] / cap[j] -
                                                     (-2 + mode[j] * std::exp(-rate * 2))));
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                RecordProperty("porous_conduction_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                EXPECT_NEAR(rkPorousEnergy(r), initial, 1e-10 * std::max(1., std::abs(initial)));
                checkRkPorousLedgers(r);
            }
        }
}
TEST(CoupledGroundwaterRk2,
     PorousMovingWaterAndIndependentThermalGridHaveSecondOrderCoupledEnergy) {
    const auto exact = porousColumnReference(20000), refined = porousColumnReference(40000);
    for (int k = 0; k < 10; ++k)
        ASSERT_NEAR(exact[k], refined[k], 1e-10);
    for (bool layered : {false, true}) {
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            RkPorousOwner owner;
            owner.initialize(r, layered, 3, true, 2e5);
            for (int cell = 0; cell < r.state.transport.n_cells; ++cell)
                r.state.transport.cell_mass[r.state.transport.idx(0, cell)] =
                    6 * r.state.volume[cell];
            const double cp = 4186000, area = r.gw.state().area[0];
            const auto total = [&]() {
                return rkPorousEnergy(r) + cp * std::accumulate(r.state.transport.cell_mass.begin(),
                                                                r.state.transport.cell_mass.end(),
                                                                0.);
            };
            const double initial_energy = total(), initial_water = r.total();
            for (int step = 0; step < int(std::round(2 / dt)); ++step)
                ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
            const auto& w = r.gw.state();
            std::vector<double> actual = {w.hg[0]};
            for (int k = 0; k < 4; ++k)
                actual.push_back(w.theta_sigma[std::size_t(k) * w.n_cells] * (2 - w.hg[0]) / 4);
            actual.push_back(r.state.volume[0] / area);
            actual.push_back(r.state.transport.cell_mass[0] / area);
            for (double energy : r.gw.heat().cells[0].column.energy)
                actual.push_back(energy / (area * cp));
            double error = 0;
            for (int k = 0; k < 10; ++k)
                error += std::pow(actual[k] - exact[k], 2);
            error = std::sqrt(error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("porous_column_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 10000)),
                           error);
            EXPECT_NEAR(total() + cp * r.gw.transport().lost_deep[0], initial_energy,
                        1e-10 * std::max(1., std::abs(initial_energy)));
            EXPECT_NEAR(r.total() + w.led_deep, initial_water, 1e-10);
            checkRkPorousLedgers(r);
        }
    }
}
TEST(CoupledGroundwaterRk2, PorousCheckpointRetainsColdPacketsAndRefusesChangedMaterialAtomically) {
    for (bool layered : {false, true}) {
        CoupledRkRig a, b, bad;
        RkPorousOwner oa, ob, oc;
        const auto initialize = [&](CoupledRkRig& r, RkPorousOwner& owner, double conductivity) {
            r.init(.025, .0001, .0002, .1, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            owner.initialize(r, layered, 3, true, conductivity);
            GwResolvedSource source;
            source.name = "porous-cold-well";
            source.cells = {{0, 1}};
            source.flow.times = {0, .2, .6, 1};
            source.flow.values = {.01, -.01, .02, 0};
            GwResolvedSourceTerm term;
            term.row = 0;
            term.signal.constant = -5;
            source.terms = {term};
            r.gw.setSources({source});
            GwSourceSignal flow;
            flow.times = {0, .3, .7, 1};
            flow.values = {.001, -.001, .001, 0};
            auto edge = rk2FlowEdge(r, 0, flow);
            GwBoundaryTerm quality;
            quality.row = 0;
            quality.signal.constant = -8;
            edge.terms = {quality};
            edge.mass_in.assign(1, 0);
            edge.mass_out.assign(1, 0);
            edge.flux_in.assign(1, 0);
            edge.flux_out.assign(1, 0);
            r.gw.setBoundaries({edge});
        };
        initialize(a, oa, 2);
        initialize(b, ob, 2);
        initialize(bad, oc, 4);
        const auto saved = rk2Output("porous-temperature.chk"),
                   before = rk2Output("porous-temperature-before.chk"),
                   after = rk2Output("porous-temperature-after.chk");
        for (int step = 0; step < 40; ++step) {
            ASSERT_NO_THROW(a.solver.advance(step * .025, (step + 1) * .025));
            if (step == 9) {
                ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                ASSERT_EQ(bad.solver.saveCheckpoint(before.string()), "");
                EXPECT_FALSE(bad.solver.loadCheckpoint(saved.string()).empty());
                ASSERT_EQ(bad.solver.saveCheckpoint(after.string()), "");
                EXPECT_EQ(rk2File(before), rk2File(after));
                ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            } else if (step > 9)
                ASSERT_NO_THROW(b.solver.advance(step * .025, (step + 1) * .025));
        }
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(saved), rk2File(after));
        checkRkPorousLedgers(a);
        checkRkPorousLedgers(b);
        EXPECT_LT(a.gw.boundaries()[0].mass_in[0], 0);
    }
}
TEST(CoupledGroundwaterRk2, PorousLaterCellThermalWorkFailureRestoresEnergyAndCanRetrySmallerStep) {
    CoupledRkRig r;
    r.init(.1, 0, 0, 0, "NONE", GwClosure::CLOSED_FORM, SoilChar::GARDNER, 4);
    RkPorousOwner owner;
    owner.initialize(r, false, 3, true, 2, 1e15);
    auto& cell = r.gw.heat().cells[0];
    for (int j = 0; j < 3; ++j)
        cell.column.energy[j] = cell.column.capacity[j] * (-2 + 3 * j);
    cell.column.initial_energy = cell.column.energy;
    cell.initial_j = std::accumulate(cell.column.energy.begin(), cell.column.energy.end(), 0.);
    r.gw.heat().refreshView(0, r.gw.transport());
    const auto initial = cell.column.energy;
    const auto before = rk2Output("porous-work-before.chk"),
               after = rk2Output("porous-work-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    try {
        r.solver.advance(0, .1);
        FAIL() << "Expected bounded midpoint work refusal in the second cell";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(std::string(error.what()).find("subdivision limit"), std::string::npos)
            << error.what();
    }
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    EXPECT_EQ(r.gw.heat().cells[0].column.energy, initial);
    ASSERT_NO_THROW(r.solver.advance(0, 1e-6));
    checkRkPorousLedgers(r);
}
TEST(CoupledGroundwaterRk2, PorousSignedHeatInputsKeepIndependentJoulesWithoutBoundaryWater) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (bool top : {false, true})
                for (double power : {-10000., 10000.}) {
                    CoupledRkRig r;
                    r.init(.1, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    owner.initialize(r, layered, 3, true);
                    GwSourceSignal flow;
                    auto edge = rk2FlowEdge(r, 0, flow);
                    if (top) {
                        owner.config.options.surface_thermal_bc = "HEAT_FLUX";
                        owner.config.options.surface_thermal_arg = std::to_string(power);
                    } else {
                        GwBoundaryTerm term;
                        term.row = 0;
                        term.flux = true;
                        term.scale = edge.length / 4186000;
                        term.signal.constant = power;
                        edge.terms = {term};
                        edge.mass_in.assign(1, 0);
                        edge.mass_out.assign(1, 0);
                        edge.flux_in.assign(1, 0);
                        edge.flux_out.assign(1, 0);
                        r.gw.setBoundaries({edge});
                    }
                    const double initial = rkPorousEnergy(r), initial_water = r.total();
                    double area = 0;
                    for (double value : r.gw.state().area)
                        area += value;
                    const double expected = power * 2 * (top ? area : edge.length);
                    for (int step = 0; step < 20; ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * .1, (step + 1) * .1));
                    EXPECT_NEAR(rkPorousEnergy(r) - initial, expected, 1e-6);
                    double receipt = 0;
                    for (const auto& c : r.gw.heat().cells)
                        receipt += top ? c.surface_j : c.source_j;
                    EXPECT_NEAR(receipt, expected, 1e-6);
                    EXPECT_NEAR(r.total(), initial_water, 1e-10);
                    checkRkPorousLedgers(r);
                    if (!top) {
                        EXPECT_DOUBLE_EQ(r.gw.boundaries()[0].applied_in, 0);
                        EXPECT_DOUBLE_EQ(r.gw.boundaries()[0].applied_out, 0);
                    }
                }
        }
}
TEST(CoupledGroundwaterRk2, PorousSensibleEtKeepsGrainsAndSignedEnergyAcrossWetAndDrySurface) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double temperature : {-3., 5.})
                for (double pond : {0., 1e-5, .01}) {
                    CoupledRkRig r;
                    r.init(.05, 0, 0, pond, "BOTH", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    owner.initialize(r, layered, 3);
                    for (int cell = 0; cell < r.gw.state().n_cells; ++cell) {
                        auto& c = r.gw.heat().cells[cell];
                        for (std::size_t j = 0; j < c.column.energy.size(); ++j)
                            c.column.energy[j] = temperature * c.column.capacity[j];
                        c.column.initial_energy = c.column.energy;
                        c.initial_j =
                            std::accumulate(c.column.energy.begin(), c.column.energy.end(), 0.);
                        r.gw.heat().refreshView(cell, r.gw.transport());
                        r.state.transport.cell_mass[cell] = temperature * r.state.volume[cell];
                    }
                    r.opts.evaporation = true;
                    r.state.evap_rate.assign(r.mesh.n_cells(), 1e-4);
                    const auto energy = [&]() {
                        return rkPorousEnergy(r) +
                               4186000 * std::accumulate(r.state.transport.cell_mass.begin(),
                                                         r.state.transport.cell_mass.end(), 0.);
                    };
                    const double initial_energy = energy(), initial_water = r.total();
                    for (int step = 0; step < 40; ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
                    const auto& w = r.gw.state();
                    EXPECT_GT(w.led_et + r.state.evap_loss_total, 0);
                    EXPECT_NEAR(energy() + 4186000 * (r.gw.transport().lost_et[0] +
                                                      temperature * r.state.evap_loss_total),
                                initial_energy, 1e-10 * std::max(1., std::abs(initial_energy)));
                    EXPECT_NEAR(r.total() + w.led_et + r.state.evap_loss_total, initial_water,
                                1e-10);
                    checkRkPorousLedgers(r);
                    for (const auto& c : r.gw.heat().cells)
                        for (std::size_t j = 0; j < c.column.energy.size(); ++j) {
                            EXPECT_GT(c.column.capacity[j], 0);
                            EXPECT_NEAR(c.column.energy[j] / c.column.capacity[j], temperature,
                                        1e-5);
                        }
                }
        }
}
TEST(CoupledGroundwaterRk2, PorousHeterogeneousLateralHeatMatchesIndependentPairDecay) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner owner;
                owner.initialize(r, layered, 1, true, 1e6, 4e6, true);
                const auto& w = r.gw.state();
                const double mean_theta = (w.hu[0] + w.theta_s[0] * w.hg[0]) / 2;
                const double capacity = 8 * 2 * ((1 - .45) * 2650 * 880 + mean_theta * 4186000);
                const auto k = [&](double grain) {
                    return (1 - .45) * grain + mean_theta * .606 + (.45 - mean_theta) * .025;
                };
                const double distance = 4 / (3 * std::sqrt(2.)),
                             g = 4 * std::sqrt(2.) * .6 / (distance / k(1e6) + distance / k(4e6));
                for (int cell = 0; cell < 2; ++cell) {
                    auto& c = r.gw.heat().cells[cell];
                    ASSERT_NEAR(c.column.capacity[0], capacity, 1e-7);
                    c.column.energy[0] = capacity * (cell == 0 ? -2 : 6);
                    c.column.initial_energy = c.column.energy;
                    c.initial_j = c.column.energy[0];
                    r.gw.heat().refreshView(cell, r.gw.transport());
                }
                const double initial = rkPorousEnergy(r);
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                const double exact = 2 - 4 * std::exp(-2 * g * 2 / capacity),
                             error = std::abs(r.gw.heat().cells[0].column.energy[0] / capacity -
                                              exact) /
                                     8;
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                RecordProperty("porous_lateral_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                EXPECT_NEAR(rkPorousEnergy(r), initial, 1e-10 * std::max(1., std::abs(initial)));
                EXPECT_NEAR(r.gw.heat().cells[0].lateral_j + r.gw.heat().cells[1].lateral_j, 0,
                            1e-7);
                checkRkPorousLedgers(r);
            }
        }
}
TEST(CoupledGroundwaterRk2, PorousReactiveCanonicalInventoriesRespectSorptionTemperatureAndAge) {
    for (const std::string integrator : {"RK5", "ROS2", "BDF2"}) {
        RkReactionSystem system(kRkPorousConversion, integrator, kRkPorousSurfaceConversion);
        for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
            for (bool layered : {false, true}) {
                if (layered && closure != GwClosure::SIGMA)
                    continue;
                for (double kd : {0., .001})
                    for (double temperature : {-3., 5.}) {
                        SCOPED_TRACE(integrator);
                        SCOPED_TRACE(int(closure));
                        SCOPED_TRACE(layered);
                        SCOPED_TRACE(kd);
                        SCOPED_TRACE(temperature);
                        CoupledRkRig r;
                        r.init(.05, 0, 0, .1, "NONE", closure, SoilChar::GARDNER, 4);
                        RkPorousOwner owner;
                        initRkPorousQuality(r, owner, system, layered, true, kd, temperature);
                        const auto& w = r.gw.state();
                        const auto& tr = r.gw.transport();
                        const auto& cell = r.gw.heat().cells[0];
                        std::vector<double> initial, eta;
                        if (layered) {
                            const double partition = tr.kd[tr.idx(0, 0)] * tr.rho_b[0];
                            initial.push_back(tr.sat_mass[tr.idx(0, 0)]);
                            eta.push_back(w.theta_s[0] / (w.theta_s[0] + partition));
                            for (int j = 0; j < tr.n_layers; ++j) {
                                const double theta = w.theta_sigma[std::size_t(j) * w.n_cells];
                                initial.push_back(tr.layer_mass[tr.layerIdx(0, j, 0)]);
                                eta.push_back(theta / (theta + partition));
                            }
                        } else
                            for (std::size_t j = 0; j < cell.column.theta.size(); ++j) {
                                const double water =
                                    cell.column.theta[j] * cell.column.area *
                                    (cell.column.depth_edges[j + 1] - cell.column.depth_edges[j]);
                                initial.push_back(cell.porewater.mass[j]);
                                eta.push_back(water / (water + cell.porewater.retention[j]));
                            }
                        const double initial_water = w.storage(),
                                     surface_water = totalVolume(r.state, r.mesh.n_cells());
                        const auto surface_initial = r.state.transport.cell_mass;
                        for (int step = 0; step < 40; ++step)
                            ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
                        for (int i = 0; i < w.n_cells; ++i)
                            for (std::size_t j = 0; j < initial.size(); ++j) {
                                const double expected =
                                    initial[j] *
                                    std::exp(-.2 * std::exp(.01 * temperature) * eta[j] * 2);
                                const double a =
                                    layered
                                        ? (j == 0 ? tr.sat_mass[tr.idx(0, i)]
                                                  : tr.layer_mass[tr.layerIdx(0, int(j) - 1, i)])
                                        : r.gw.heat().cells[i].porewater.mass[j];
                                const double b =
                                    layered
                                        ? (j == 0 ? tr.sat_mass[tr.idx(1, i)]
                                                  : tr.layer_mass[tr.layerIdx(1, int(j) - 1, i)])
                                        : r.gw.heat().cells[i].porewater.mass[initial.size() + j];
                                EXPECT_NEAR(a, expected, 1e-8 * std::max(1., initial[j]));
                                EXPECT_NEAR(a + b, initial[j], 1e-10 * std::max(1., initial[j]));
                            }
                        EXPECT_NEAR(tr.storage(tr.age_row), 7 * initial_water,
                                    1e-10 * initial_water);
                        EXPECT_NEAR(tr.generated_age[tr.age_row], 2 * initial_water,
                                    1e-10 * initial_water);
                        for (int species : {0, 1, tr.age_row})
                            EXPECT_NEAR(tr.residual(species), 0,
                                        1e-10 * std::max(1., tr.init_mass[species]));
                        EXPECT_DOUBLE_EQ(tr.lost_reaction[tr.temp_row], 0);
                        EXPECT_DOUBLE_EQ(tr.lost_reaction[tr.age_row], 0);
                        const auto& surface = r.state.transport;
                        EXPECT_NEAR(surface.generated_age[surface.age_row], 2 * surface_water,
                                    1e-10);
                        for (int i = 0; i < w.n_cells; ++i) {
                            EXPECT_NEAR(surface.cell_mass[surface.idx(0, i)],
                                        surface_initial[surface.idx(0, i)] *
                                            std::exp(-.3 * std::exp(.01 * temperature) * 2),
                                        1e-8);
                            EXPECT_NEAR(surface.cell_mass[surface.idx(surface.age_row, i)],
                                        7 * r.state.volume[i], 1e-10);
                            for (std::size_t j = 0; j < r.gw.heat().cells[i].column.energy.size();
                                 ++j)
                                EXPECT_NEAR(r.gw.heat().cells[i].column.energy[j] /
                                                r.gw.heat().cells[i].column.capacity[j],
                                            temperature, 1e-10);
                        }
                        checkRkPorousLedgers(r);
                    }
            }
    }
}
TEST(CoupledGroundwaterRk2, PorousReactiveMovingColumnMatchesIndependentWaterEnergyMassAndAgeOde) {
    constexpr double kd = .001, partition = kd * 2650 * (1 - .45), cp = 4186000;
    for (bool layered : {false, true}) {
        const auto exact = porousColumnReference(20000, true, layered, partition),
                   refined = porousColumnReference(40000, true, layered, partition);
        for (std::size_t k = 0; k < exact.size(); ++k)
            ASSERT_NEAR(exact[k], refined[k], 1e-10);
        RkReactionSystem system(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            RkPorousOwner owner;
            initRkPorousQuality(r, owner, system, layered, true, kd, -2, 3, true);
            auto& surface = r.state.transport;
            const auto& w = r.gw.state();
            const auto& tr = r.gw.transport();
            owner.config.params[0].lambda_s = 2e5;
            for (int cell = 0; cell < w.n_cells; ++cell) {
                auto& column = r.gw.heat().cells[cell].column;
                column.material.grain_conductivity = 2e5;
                ASSERT_EQ(column.setMoisture(column.theta), "");
                surface.cell_mass[surface.idx(0, cell)] = 6 * r.state.volume[cell];
                surface.cell_mass[surface.idx(surface.temp_row, cell)] = 6 * r.state.volume[cell];
            }
            ASSERT_NEAR(tr.kd[0] * tr.rho_b[0], partition, 1e-14);
            const auto surface_mass = [&](int row) {
                double result = 0;
                for (int i = 0; i < surface.n_cells; ++i)
                    result += surface.cell_mass[surface.idx(row, i)];
                return result;
            };
            const double initial_mass =
                tr.storage(0) + tr.storage(1) + surface_mass(0) + surface_mass(1);
            const double initial_age = tr.storage(tr.age_row) + surface_mass(surface.age_row);
            const double initial_energy = rkPorousEnergy(r) + cp * surface_mass(surface.temp_row),
                         area = w.area[0];
            for (int step = 0; step < int(std::round(2 / dt)); ++step)
                ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
            std::vector<double> actual = {w.hg[0]};
            for (int j = 0; j < 4; ++j)
                actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (2 - w.hg[0]) / 4);
            actual.push_back(r.state.volume[0] / area);
            actual.push_back(surface.cell_mass[surface.idx(surface.temp_row, 0)] / area);
            for (double energy : r.gw.heat().cells[0].column.energy)
                actual.push_back(energy / (area * cp));
            for (int row : {0, 1, tr.age_row}) {
                if (layered) {
                    for (int j = 0; j < tr.n_layers; ++j)
                        actual.push_back(tr.layer_mass[tr.layerIdx(row, j, 0)] / area);
                    actual.push_back(tr.sat_mass[tr.idx(row, 0)] / area);
                } else {
                    const auto& pore = r.gw.heat().cells[0].porewater;
                    const auto n = r.gw.heat().cells[0].column.theta.size();
                    for (std::size_t j = 0; j < n; ++j)
                        actual.push_back(pore.mass[std::size_t(row) * n + j] / area);
                }
                actual.push_back(surface.cell_mass[surface.idx(row, 0)] / area);
            }
            ASSERT_EQ(actual.size(), exact.size());
            double error = 0;
            for (std::size_t k = 0; k < exact.size(); ++k)
                error += std::pow(actual[k] - exact[k], 2);
            error = std::sqrt(error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("porous_reactive_column_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 10000)),
                           error);
            EXPECT_NEAR(tr.storage(0) + tr.storage(1) + surface_mass(0) + surface_mass(1) +
                            tr.lost_deep[0] + tr.lost_deep[1],
                        initial_mass, 1e-10 * initial_mass);
            EXPECT_NEAR(tr.storage(tr.age_row) + surface_mass(surface.age_row) +
                            tr.lost_deep[tr.age_row] - tr.generated_age[tr.age_row] -
                            surface.generated_age[surface.age_row],
                        initial_age, 1e-10 * initial_age);
            EXPECT_NEAR(rkPorousEnergy(r) +
                            cp * (surface_mass(surface.temp_row) + tr.lost_deep[tr.temp_row]),
                        initial_energy, 1e-10 * std::max(1., std::abs(initial_energy)));
            for (int row : {0, 1, tr.age_row})
                EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
            checkRkPorousLedgers(r);
        }
    }
}
TEST(CoupledGroundwaterRk2, PorousSharedRateUsesEachPhaseVariablesAndAllReachableTerms) {
    RkReactionSystem system(
        "* SAT A RATE -.2*SAT_RATE\n* SAT B RATE .2*SAT_RATE\n* UNSAT A RATE -.3*A\n* UNSAT B RATE .3*A\n",
        "RK5", "", "SAT_RATE SQRT(GW_THETA-.3)*A\n");
    ASSERT_EQ(system.context().reactions.subsurface_laws.size(), 4);
    CoupledRkRig r;
    r.init(.05, 0, 0, 0, "NONE", GwClosure::CLOSED_FORM, SoilChar::GARDNER, 4);
    RkPorousOwner owner;
    initRkPorousQuality(r, owner, system, false, true, .001, -2, 1);
    const auto& cell = r.gw.heat().cells[0];
    const double water = cell.column.theta[0] * cell.column.area * 2;
    const double sat = cell.saturated_water[0], capacity = water + cell.porewater.retention[0],
                 initial = cell.porewater.mass[0];
    // SAT_RATE is valid in SAT and unreachable in UNSAT, whose selected
    // expression does not use that term. Both contributions see one C_A.
    const double rate = (.2 * std::sqrt(.45 - .3) * sat + .3 * (water - sat)) / capacity;
    for (int step = 0; step < 40; ++step)
        ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
    for (const auto& current : r.gw.heat().cells) {
        EXPECT_NEAR(current.porewater.mass[0], initial * std::exp(-rate * 2), 1e-8 * initial);
        EXPECT_NEAR(current.porewater.mass[0] + current.porewater.mass[1], initial,
                    1e-10 * initial);
    }
}
TEST(CoupledGroundwaterRk2, PorousDecayingPrecursorUsesCanonicalMassAndSymmetricReactionClock) {
    RkReactionSystem system("* SAT B RATE .2*A\n* UNSAT B RATE .2*A\n");
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner owner;
                initRkPorousQuality(r, owner, system, layered, false, .001);
                auto& tr = r.gw.transport();
                const auto& w = r.gw.state();
                std::vector<double> initial, eta;
                for (int cell = 0; cell < w.n_cells; ++cell)
                    tr.decay[tr.idx(0, cell)] = .3;
                if (layered) {
                    const double partition = tr.kd[0] * tr.rho_b[0];
                    initial.push_back(tr.sat_mass[0]);
                    eta.push_back(w.theta_s[0] / (w.theta_s[0] + partition));
                    for (int j = 0; j < tr.n_layers; ++j) {
                        const double theta = w.theta_sigma[std::size_t(j) * w.n_cells];
                        initial.push_back(tr.layer_mass[tr.layerIdx(0, j, 0)]);
                        eta.push_back(theta / (theta + partition));
                    }
                } else {
                    const auto& c = r.gw.heat().cells[0];
                    for (std::size_t j = 0; j < c.column.theta.size(); ++j) {
                        const double water =
                            c.column.theta[j] * c.column.area *
                            (c.column.depth_edges[j + 1] - c.column.depth_edges[j]);
                        initial.push_back(c.porewater.mass[j]);
                        eta.push_back(water / (water + c.porewater.retention[j]));
                    }
                }
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                double error = 0;
                for (std::size_t j = 0; j < initial.size(); ++j) {
                    const double a = layered
                                         ? (j == 0 ? tr.sat_mass[0]
                                                   : tr.layer_mass[tr.layerIdx(0, int(j) - 1, 0)])
                                         : r.gw.heat().cells[0].porewater.mass[j];
                    const double b = layered
                                         ? (j == 0 ? tr.sat_mass[tr.idx(1, 0)]
                                                   : tr.layer_mass[tr.layerIdx(1, int(j) - 1, 0)])
                                         : r.gw.heat().cells[0].porewater.mass[initial.size() + j];
                    EXPECT_NEAR(a, initial[j] * std::exp(-.6), 1e-10 * initial[j]);
                    const double exact = initial[j] * .2 * eta[j] / .3 * (-std::expm1(-.6));
                    error += std::pow(b - exact, 2);
                }
                error = std::sqrt(error) / std::accumulate(initial.begin(), initial.end(), 0.);
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                RecordProperty("porous_precursor_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                for (int row : {0, 1})
                    EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
                EXPECT_DOUBLE_EQ(tr.lost_reaction[tr.temp_row], 0);
                checkRkPorousLedgers(r);
            }
        }
}
TEST(CoupledGroundwaterRk2, PorousEulerAndAlgebraicLawsKeepTheirDeclaredSingleOwner) {
    for (const std::string kind : {"EUL", "FORMULA", "EQUIL"})
        for (bool layered : {false, true}) {
            const auto laws = kind == "EUL"
                                  ? kRkConversion
                                  : "* SAT B " + kind + " " +
                                        (kind == "FORMULA" ? "2*A" : "B-2*A") + "\n* UNSAT B " +
                                        kind + " " + (kind == "FORMULA" ? "2*A" : "B-2*A") + "\n";
            RkReactionSystem system(laws, kind == "EUL" ? "EUL" : "RK5");
            CoupledRkRig r;
            r.init(.05, 0, 0, .1, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            RkPorousOwner owner;
            initRkPorousQuality(r, owner, system, layered, true, .001);
            const double initial = r.gw.transport().init_mass[0];
            const auto before = r.gw.heat().cells[0].porewater.mass;
            const auto tr_before = r.gw.transport();
            for (int step = 0; step < 40; ++step)
                ASSERT_NO_THROW(r.solver.advance(step * .05, (step + 1) * .05));
            const auto& tr = r.gw.transport();
            if (kind != "EUL") {
                EXPECT_NEAR(tr.storage(0), initial, 1e-10 * initial);
                EXPECT_NEAR(tr.storage(1), 2 * initial, 1e-10 * initial);
                EXPECT_NEAR(tr.lost_reaction[1], -2 * initial, 1e-10 * initial);
            } else
                for (int cell = 0; cell < tr.n_cells; ++cell) {
                    if (layered) {
                        for (int zone = 0; zone <= tr.n_layers; ++zone) {
                            const double theta =
                                zone == 0
                                    ? r.gw.state().theta_s[cell]
                                    : r.gw.state()
                                          .theta_sigma[std::size_t(zone - 1) * tr.n_cells + cell];
                            const double eta =
                                theta / (theta + tr.kd[tr.idx(0, cell)] * tr.rho_b[cell]);
                            const double old =
                                zone == 0 ? tr_before.sat_mass[tr.idx(0, cell)]
                                          : tr_before.layer_mass[tr.layerIdx(0, zone - 1, cell)];
                            const double actual =
                                zone == 0 ? tr.sat_mass[tr.idx(0, cell)]
                                          : tr.layer_mass[tr.layerIdx(0, zone - 1, cell)];
                            EXPECT_NEAR(actual, old * std::pow(1 - .2 * eta * .025, 80),
                                        1e-10 * old);
                        }
                    } else {
                        const auto& c = r.gw.heat().cells[cell];
                        const auto n = c.column.theta.size();
                        for (std::size_t j = 0; j < n; ++j) {
                            const double water =
                                c.column.theta[j] * c.column.area *
                                (c.column.depth_edges[j + 1] - c.column.depth_edges[j]);
                            const double eta = water / (water + c.porewater.retention[j]);
                            EXPECT_NEAR(c.porewater.mass[j],
                                        before[j] * std::pow(1 - .2 * eta * .025, 80),
                                        1e-10 * before[j]);
                        }
                    }
                    EXPECT_NEAR(tr.storage(0) + tr.storage(1), initial, 1e-10 * initial);
                }
            for (int row : {0, 1, tr.age_row})
                EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
            EXPECT_DOUBLE_EQ(tr.lost_reaction[tr.age_row], 0);
            EXPECT_DOUBLE_EQ(tr.lost_reaction[tr.temp_row], 0);
        }
}
TEST(CoupledGroundwaterRk2,
     PorousStiffReactiveCheckpointRetainsCanonicalMassAndRejectsLawOrSorptionChanges) {
    const std::string laws =
        "* SAT A RATE -1000*A\n* SAT B RATE 1000*A-.2*B\n* UNSAT A RATE -1000*A\n* UNSAT B RATE 1000*A-.2*B\n";
    RkReactionSystem system(laws, "ROS2"),
        changed(
            "* SAT A RATE -1001*A\n* SAT B RATE 1001*A-.2*B\n* UNSAT A RATE -1001*A\n* UNSAT B RATE 1001*A-.2*B\n",
            "ROS2");
    for (bool layered : {false, true}) {
        CoupledRkRig a, b, bad_law, bad_kd;
        RkPorousOwner oa, ob, oc, od;
        for (auto* rig : {&a, &b, &bad_law, &bad_kd})
            rig->init(.025, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkPorousQuality(a, oa, system, layered, true, .001);
        initRkPorousQuality(b, ob, system, layered, true, .001);
        initRkPorousQuality(bad_law, oc, changed, layered, true, .001);
        initRkPorousQuality(bad_kd, od, system, layered, true, .002);
        const auto saved = rk2Output("porous-reactive.chk"),
                   before = rk2Output("porous-reactive-before.chk"),
                   after = rk2Output("porous-reactive-after.chk");
        const double initial = a.gw.transport().init_mass[0];
        for (int step = 0; step < 20; ++step) {
            ASSERT_NO_THROW(a.solver.advance(step * .025, (step + 1) * .025));
            if (step == 7) {
                ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
                for (auto* bad : {&bad_law, &bad_kd}) {
                    ASSERT_EQ(bad->solver.saveCheckpoint(before.string()), "");
                    EXPECT_FALSE(bad->solver.loadCheckpoint(saved.string()).empty());
                    ASSERT_EQ(bad->solver.saveCheckpoint(after.string()), "");
                    EXPECT_EQ(rk2File(before), rk2File(after));
                }
                ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            } else if (step > 7)
                ASSERT_NO_THROW(b.solver.advance(step * .025, (step + 1) * .025));
        }
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(saved), rk2File(after));
        for (int row : {0, 1, a.gw.transport().age_row})
            EXPECT_NEAR(a.gw.transport().residual(row), 0, 1e-10 * std::max(1., initial));
        EXPECT_LT(a.gw.transport().storage(0), initial * 1e-6);
        EXPECT_GT(a.gw.transport().storage(1), initial * .9);
        checkRkPorousLedgers(a);
        checkRkPorousLedgers(b);
    }
}
TEST(CoupledGroundwaterRk2, PorousLaterKnotReactionFailureRestoresEveryCanonicalOwnerAndCanRetry) {
    RkReactionSystem system("* SAT A RATE -SQRT(.3-DT)*A\n* SAT B RATE SQRT(.3-DT)*A\n"
                            "* UNSAT A RATE -SQRT(.3-DT)*A\n* UNSAT B RATE SQRT(.3-DT)*A\n");
    CoupledRkRig r;
    r.init(.1, .0001, 0, .1, "NONE", GwClosure::CLOSED_FORM, SoilChar::GARDNER, 4);
    RkPorousOwner owner;
    initRkPorousQuality(r, owner, system, false, true, .001);
    GwResolvedSource source;
    source.name = "porous-cold-cut";
    source.cells = {{0, 1}};
    source.flow.times = {0, .5, 4.5};
    source.flow.values = {.001, .001, .001};
    for (int row = 0; row < r.gw.transport().n_species; ++row) {
        GwResolvedSourceTerm term;
        term.row = row;
        term.signal.constant = row == 0                           ? 7
                               : row == r.gw.transport().temp_row ? -8
                               : row == r.gw.transport().age_row  ? 3
                                                                  : 0;
        source.terms.push_back(term);
    }
    r.gw.setSources({source});
    ExplicitInertialSolverTestAccess::prepare(r.solver);
    const auto before = rk2Output("porous-reactive-failure-before.chk"),
               after = rk2Output("porous-reactive-failure-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4.5, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    for (int row : {0, 1, r.gw.transport().age_row})
        EXPECT_NEAR(r.gw.transport().residual(row), 0,
                    1e-10 * std::max(1., r.gw.transport().init_mass[row]));
    checkRkPorousLedgers(r);
}

namespace {
/**
 * @brief Add an owned thermal boundary time series to the porous owner context.
 *
 * Converts times to native day values via /86400 and copies y values. Does not validate
 * sizes/order, clear old x values on a reused name, or select which boundary consumes the table.
 * Caller uses a fresh name and keeps context alive for bound table access.
 *
 * @param[in,out] owner Live porous owner; context start/current dates become zero and a table is
 *     added.
 * @param[in] name Fresh unique table name, copied into context.
 * @param[in] seconds Borrowed elapsed-second abscissae, expected sorted and matched in count to
 *     values.
 * @param[in] values Borrowed thermal boundary values in the selected policy units; copied without
 *     conversion.
 */
void rkThermalSeries(RkPorousOwner& owner, const std::string& name,
                     const std::vector<double>& seconds, const std::vector<double>& values) {
    owner.context.options.start_date = 0;
    owner.context.current_date = 0;
    const int index = owner.context.tables.add(name, openswmm::TableType::TIMESERIES);
    auto& table = owner.context.tables[index];
    table.y = values;
    for (double t : seconds)
        table.x.push_back(owner.context.options.start_date + t / 86400.);
}
/**
 * @brief Reinitialize heat/porewater with selected top and deep thermal boundary policies.
 *
 * Fatally asserts heat and porewater acceptance. Caller supplies required TOP/DEEP context tables
 * or policy arguments and keeps the owner alive; does not preserve old thermal inventories or
 * advance time.
 *
 * @param[in,out] r Rig whose existing heat/porewater state is replaced by initialization.
 * @param[in,out] owner Live porous configuration/context owner, mutated to retain selected
 *     policies.
 * @param[in] top Raw surface thermal policy, using TOP as its argument/table name.
 * @param[in] deep Raw deep thermal policy, using DEEP as its argument/table name.
 */
void applyRkThermalBoundary(CoupledRkRig& r, RkPorousOwner& owner, const std::string& top,
                            const std::string& deep) {
    owner.config.options.surface_thermal_bc = top;
    owner.config.options.surface_thermal_arg = "TOP";
    owner.config.options.deep_thermal_bc = deep;
    owner.config.options.deep_thermal_arg = "DEEP";
    ASSERT_EQ(r.gw.initHeat(owner.context, owner.config), "");
    ASSERT_EQ(r.gw.initPorewater(), "");
}
}
TEST(CoupledGroundwaterRk2, PorousLatentEtMatchesIndependentTemperatureCapacityRelation) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double temperature : {-3., 5.}) {
                double previous = 0;
                for (double dt : {.4, .2, .1, .05}) {
                    SCOPED_TRACE(int(closure));
                    SCOPED_TRACE(layered);
                    SCOPED_TRACE(temperature);
                    SCOPED_TRACE(dt);
                    CoupledRkRig r;
                    r.init(dt, 0, 0, 0, "BOUNDARY_ET", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    owner.initialize(r, layered);
                    owner.config.options.soil_et_heat = "LATENT_SURFACE";
                    for (int i = 0; i < r.gw.state().n_cells; ++i) {
                        auto& c = r.gw.heat().cells[i];
                        c.column.energy[0] = temperature * c.column.capacity[0];
                        c.column.initial_energy = c.column.energy;
                        c.initial_j = c.column.energy[0];
                        r.gw.heat().refreshView(i, r.gw.transport());
                    }
                    r.opts.evaporation = true;
                    r.state.evap_rate.assign(r.mesh.n_cells(), .005);
                    const double water0 = r.total(), energy0 = rkPorousEnergy(r),
                                 capacity0 = r.gw.heat().cells[0].column.capacity[0];
                    for (int step = 0; step < int(std::round(2 / dt)); ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                    const auto& c = r.gw.heat().cells[0];
                    const double actual = c.column.energy[0] / c.column.capacity[0];
                    const double exact =
                        2499000. / 2360 +
                        (temperature - 2499000. / 2360) *
                            std::pow(capacity0 / c.column.capacity[0], 2360. / 4186);
                    const double error = std::abs(actual - exact) / 20;
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    RecordProperty("latent_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" +
                                       std::to_string(int(temperature)) + "_" +
                                       std::to_string(int(dt * 1000)),
                                   error);
                    double receipts = 0;
                    for (const auto& cell : r.gw.heat().cells) {
                        EXPECT_LT(cell.latent_j, 0);
                        receipts += cell.advective_j + cell.latent_j;
                    }
                    EXPECT_GT(r.gw.state().led_et, 0);
                    EXPECT_NEAR(r.total() + r.gw.state().led_et, water0, 1e-10 * water0);
                    for (int i = 0; i < r.gw.state().n_cells; ++i) {
                        EXPECT_NEAR(r.gw.state().et_potential_cumulative[i],
                                    .005 * 2 * r.gw.state().area[i], 1e-13);
                        EXPECT_DOUBLE_EQ(r.gw.state().et_pending[i], 0);
                        EXPECT_NEAR(r.gw.state().et_refresh[i], 2, 1e-13);
                    }
                    EXPECT_NEAR(rkPorousEnergy(r) - energy0, receipts,
                                1e-10 * std::max(1., std::abs(energy0)));
                    checkRkPorousLedgers(r);
                }
            }
        }
}
TEST(CoupledGroundwaterRk2, PorousThermalSeriesFollowIndependentPiecewiseRobinSolution) {
    const std::vector<double> tt{0, .37, .91, 2}, tv{-6, 8, -4, 6}, bt{0, .23, 1.1, 2},
        bv{-1, 3, -2, 1};
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.2, .1, .05, .025}) {
                SCOPED_TRACE(int(closure));
                SCOPED_TRACE(layered);
                SCOPED_TRACE(dt);
                CoupledRkRig r;
                r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner owner;
                owner.initialize(r, layered, 1, true, 4e6);
                rkThermalSeries(owner, "TOP", tt, tv);
                rkThermalSeries(owner, "DEEP", bt, bv);
                applyRkThermalBoundary(r, owner, "TIMESERIES", "TIMESERIES");
                const auto& col = r.gw.heat().cells[0].column;
                const double gtop = col.area * col.conductivity[0],
                             gdeep = col.area * col.conductivity[0] /
                                     (1 + owner.config.options.deep_depth);
                const double rate = (gtop + gdeep) / col.capacity[0];
                GwSourceSignal top, bottom;
                top.times = tt;
                top.values = tv;
                bottom.times = bt;
                bottom.values = bv;
                std::vector<double> cuts = tt;
                cuts.insert(cuts.end(), bt.begin(), bt.end());
                std::sort(cuts.begin(), cuts.end());
                cuts.erase(std::unique(cuts.begin(), cuts.end()), cuts.end());
                double exact = -2;
                for (std::size_t j = 1; j < cuts.size(); ++j) {
                    const double t = cuts[j - 1], h = cuts[j] - t,
                                 a = (gtop * top.at(t) + gdeep * bottom.at(t)) / (gtop + gdeep);
                    const double b =
                        ((gtop * top.at(t + h) + gdeep * bottom.at(t + h)) / (gtop + gdeep) - a) /
                        h;
                    exact = a + b * h - b / rate + (exact - a + b / rate) * std::exp(-rate * h);
                }
                const auto date = owner.context.current_date;
                for (int k = 0; k < 2; ++k) {
                    owner.context.tables[k].cursor.index = 2;
                    owner.context.tables[k].cursor.direction = -1;
                }
                for (int step = 0; step < int(std::round(2 / dt)); ++step)
                    ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                const double error =
                    std::abs(r.gw.heat().cells[0].column.energy[0] / col.capacity[0] - exact) / 20;
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                RecordProperty("thermal_series_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
                EXPECT_DOUBLE_EQ(owner.context.current_date, date);
                for (int k = 0; k < 2; ++k) {
                    EXPECT_EQ(owner.context.tables[k].cursor.index, 2);
                    EXPECT_EQ(owner.context.tables[k].cursor.direction, -1);
                }
                checkRkPorousLedgers(r);
            }
        }
}
TEST(CoupledGroundwaterRk2, PorousFluxSeriesBookExactSignedJoulesIncludingInactiveIntervals) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig r;
            r.init(.3, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
            RkPorousOwner owner;
            owner.initialize(r, layered, 3, true);
            rkThermalSeries(owner, "TOP", {.2, .7, 1.6}, {-100, 200, -50});
            rkThermalSeries(owner, "DEEP", {0, .4, 1.2, 2}, {20, -40, 30, -20});
            applyRkThermalBoundary(r, owner, "FLUX_TIMESERIES", "FLUX_TIMESERIES");
            const double before = rkPorousEnergy(r),
                         area = std::accumulate(r.gw.state().area.begin(), r.gw.state().area.end(),
                                                0.);
            ASSERT_NO_THROW(r.solver.advance(0, 2.));
            double top = 0, deep = 0;
            for (const auto& c : r.gw.heat().cells) {
                top += c.surface_j;
                deep += c.deep_j;
            }
            const double jt = area * (.5 * (-100 + 200) * .5 + .5 * (200 - 50) * .9);
            const double jd =
                area * (.5 * (20 - 40) * .4 + .5 * (-40 + 30) * .8 + .5 * (30 - 20) * .8);
            EXPECT_NEAR(top, jt, 1e-6);
            EXPECT_NEAR(deep, jd, 1e-6);
            EXPECT_NEAR(rkPorousEnergy(r) - before, jt + jd, 1e-6);
            checkRkPorousLedgers(r);
        }
}
TEST(CoupledGroundwaterRk2, PorousLatentWetDryEvaporationKeepsSeparateSensibleAndLatentReceipts) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double pond : {0., 1e-5, .01})
                for (double demand : {.0001, 100.}) {
                    CoupledRkRig r;
                    r.init(.05, 0, 0, pond, "BOUNDARY_ET", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    owner.initialize(r, layered, 3);
                    owner.config.options.soil_et_heat = "LATENT_SURFACE";
                    r.opts.evaporation = true;
                    r.state.evap_rate.assign(r.mesh.n_cells(), demand);
                    const auto energy = [&]() {
                        return rkPorousEnergy(r) +
                               4186000 * std::accumulate(r.state.transport.cell_mass.begin(),
                                                         r.state.transport.cell_mass.end(), 0.);
                    };
                    const double initial_energy = energy(), initial_water = r.total();
                    ASSERT_NO_THROW(r.solver.advance(0, 2));
                    double latent = 0;
                    for (const auto& c : r.gw.heat().cells)
                        latent += c.latent_j;
                    const double sensible =
                        4186000 * (r.gw.transport().lost_et[0] - 2 * r.state.evap_loss_total);
                    EXPECT_NEAR(energy() + sensible - latent, initial_energy,
                                1e-10 * std::max(1., std::abs(initial_energy)));
                    EXPECT_NEAR(r.total() + r.gw.state().led_et + r.state.evap_loss_total,
                                initial_water, 1e-10 * initial_water);
                    EXPECT_LE(latent, 0);
                    checkRkPorousLedgers(r);
                }
        }
}
TEST(CoupledGroundwaterRk2, PorousThermalSeriesCheckpointBindsValuesAnchorAndLatentModeAtomically) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig a, b;
            RkPorousOwner oa, ob;
            const auto init = [&](CoupledRkRig& r, RkPorousOwner& owner) {
                r.init(.1, .0001, 0, .01, "BOUNDARY_ET", closure, SoilChar::GARDNER, 4);
                owner.initialize(r, layered, 3, true, 1e5);
                rkThermalSeries(owner, "TOP", {0, .37, .91, 2}, {-6, 8, -4, 6});
                rkThermalSeries(owner, "DEEP", {0, .23, 1.1, 2}, {20, -40, 30, -20});
                applyRkThermalBoundary(r, owner, "TIMESERIES", "FLUX_TIMESERIES");
                owner.config.options.soil_et_heat = "LATENT_SURFACE";
                r.opts.evaporation = true;
                r.state.evap_rate.assign(r.mesh.n_cells(), .0001);
            };
            init(a, oa);
            init(b, ob);
            ASSERT_NO_THROW(a.solver.advance(0, .6));
            const auto saved = rk2Output("thermal-series-source.chk"),
                       before = rk2Output("thermal-series-before.chk"),
                       after = rk2Output("thermal-series-after.chk");
            ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
            // Version 3 did not stage dry-cell demand or its accepted clock.
            // It cannot certify exact continuation under the corrected cadence.
            openswmm::detail::CompletedSourceArchive current(rk2File(saved));
            std::string magic, payload;
            uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
            current(magic, version, endian, long_bytes, payload, crc);
            current.finish();
            ASSERT_EQ(version, 4);
            version = 3;
            openswmm::detail::CompletedSourceArchive historical;
            historical(magic, version, endian, long_bytes, payload, crc);
            const auto old = rk2Output("thermal-series-v3.chk");
            std::ofstream(old, std::ios::binary) << historical.bytes();
            ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
            EXPECT_FALSE(b.solver.loadCheckpoint(old.string()).empty());
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            const auto refuse = [&]() {
                ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
                EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
                ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
                EXPECT_EQ(rk2File(before), rk2File(after));
            };
            ob.context.tables[0].y[1] += .1;
            refuse();
            ob.context.tables[0].y[1] -= .1;
            ob.context.options.start_date = 1;
            refuse();
            ob.context.options.start_date = 0;
            ob.config.options.soil_et_heat = "SENSIBLE_ONLY";
            refuse();
            ob.config.options.soil_et_heat = "LATENT_SURFACE";
            ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            ASSERT_NO_THROW(a.solver.advance(.6, 2));
            ASSERT_NO_THROW(b.solver.advance(.6, 2));
            ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            checkRkPorousLedgers(a);
            checkRkPorousLedgers(b);
            CoupledRkRig fresh;
            RkPorousOwner fresh_owner;
            init(fresh, fresh_owner);
            fresh_owner.context.tables[0].x[2] = fresh_owner.context.tables[0].x[1];
            ASSERT_EQ(fresh.solver.saveCheckpoint(before.string()), "");
            EXPECT_THROW(fresh.solver.advance(0, .2), std::runtime_error);
            ASSERT_EQ(fresh.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            fresh_owner.context.tables[0].x[2] = .91 / 86400.;
            ASSERT_EQ(fresh.solver.saveCheckpoint(before.string()), "");
            EXPECT_THROW(fresh.solver.advance(0, 2.5), std::runtime_error);
            ASSERT_EQ(fresh.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ASSERT_NO_THROW(fresh.solver.advance(0, .1));
            checkRkPorousLedgers(fresh);
        }
}
TEST(CoupledGroundwaterRk2,
     PorousThermalLaterReactionFailureRestoresEnergyAgeAndSharedSeriesCursor) {
    const std::string laws =
        "* SAT A RATE -SQRT(.3-DT)*A\n* SAT B RATE SQRT(.3-DT)*A\n* UNSAT A RATE -SQRT(.3-DT)*A\n* UNSAT B RATE SQRT(.3-DT)*A\n";
    RkReactionSystem rx(laws), control_rx(laws);
    CoupledRkRig r, control;
    RkPorousOwner owner, control_owner;
    const auto init = [&](CoupledRkRig& rig, RkPorousOwner& o, RkReactionSystem& system) {
        rig.init(.1, .0001, 0, .1, "BOUNDARY_ET", GwClosure::CLOSED_FORM, SoilChar::GARDNER, 4);
        initRkPorousQuality(rig, o, system, false, true, .001, -2, 3, true);
        auto& ctx = system.context();
        ctx.options.start_date = 0;
        ctx.current_date = 0;
        for (const std::string name : {"TOP", "DEEP"}) {
            const int index = ctx.tables.add(name, openswmm::TableType::TIMESERIES);
            auto& t = ctx.tables[index];
            t.x = {0, 1. / 86400., 4.5 / 86400.};
            t.y = name == "TOP" ? std::vector<double>{-6, 7, -4} : std::vector<double>{20, -40, 30};
        }
        o.config.options.surface_thermal_bc = "TIMESERIES";
        o.config.options.surface_thermal_arg = "TOP";
        o.config.options.deep_thermal_bc = "FLUX_TIMESERIES";
        o.config.options.deep_thermal_arg = "DEEP";
        o.config.options.soil_et_heat = "LATENT_SURFACE";
        ASSERT_EQ(rig.gw.initHeat(ctx, o.config), "");
        ASSERT_EQ(rig.gw.initPorewater(), "");
        GwResolvedSource source;
        source.name = "thermal-cold-cut";
        source.cells = {{0, 1}};
        source.flow.times = {0, .5, 4.5};
        source.flow.values = {.001, .001, .001};
        for (int row = 0; row < rig.gw.transport().n_species; ++row) {
            GwResolvedSourceTerm term;
            term.row = row;
            term.signal.constant = row == 0                             ? 7
                                   : row == rig.gw.transport().temp_row ? -8
                                   : row == rig.gw.transport().age_row  ? 3
                                                                        : 0;
            source.terms.push_back(term);
        }
        rig.gw.setSources({source});
        ExplicitInertialSolverTestAccess::prepare(rig.solver);
        for (int k = 0; k < 2; ++k) {
            ctx.tables[k].cursor.index = 1;
            ctx.tables[k].cursor.direction = -1;
        }
        ctx.current_date = .123; // an unrelated borrowed calendar is never a stage clock
    };
    init(r, owner, rx);
    init(control, control_owner, control_rx);
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver, .5, 0));
    EXPECT_NE(control.gw.heat().cells[0].surface_j, 0);
    const auto before = rk2Output("thermal-later-failure-before.chk"),
               after = rk2Output("thermal-later-failure-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 4.5, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    EXPECT_DOUBLE_EQ(rx.context().current_date, .123);
    for (int k = 0; k < 2; ++k) {
        EXPECT_EQ(rx.context().tables[k].cursor.index, 1);
        EXPECT_EQ(rx.context().tables[k].cursor.direction, -1);
    }
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    for (int row : {0, 1, r.gw.transport().age_row})
        EXPECT_NEAR(r.gw.transport().residual(row), 0,
                    1e-10 * std::max(1., r.gw.transport().init_mass[row]));
    checkRkPorousLedgers(r);
}
TEST(CoupledGroundwaterRk2, PorousHeldAirMatchesSignedAnalyticalRobinRelaxation) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double air : {-5., 7.}) {
                double previous = 0;
                for (double dt : {.4, .2, .1, .05}) {
                    CoupledRkRig r;
                    r.init(dt, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    owner.initialize(r, layered, 1, true, 4e6);
                    owner.config.options.surface_thermal_bc = "AIR";
                    owner.config.options.surface_resistance = 2e-6;
                    owner.context.climate_state.temperature = 32 + 1.8 * air;
                    const auto& col = r.gw.heat().cells[0].column;
                    const double g = col.area / (1 / col.conductivity[0] + 2e-6),
                                 rate = g / col.capacity[0];
                    const double initial = rkPorousEnergy(r), date = owner.context.current_date,
                                 held = owner.context.climate_state.temperature;
                    for (int step = 0; step < int(std::round(2 / dt)); ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                    const double exact = air + (-2 - air) * std::exp(-rate * 2),
                                 actual = col.energy[0] / col.capacity[0];
                    const double error = std::abs(actual - exact) / 10;
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    RecordProperty("held_air_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" + std::to_string(int(air)) +
                                       "_" + std::to_string(int(dt * 1000)),
                                   error);
                    double receipt = 0;
                    for (const auto& c : r.gw.heat().cells)
                        receipt += c.surface_j;
                    EXPECT_NEAR(rkPorousEnergy(r) - initial, receipt,
                                1e-10 * std::max(1., std::abs(initial)));
                    EXPECT_DOUBLE_EQ(owner.context.current_date, date);
                    EXPECT_DOUBLE_EQ(owner.context.climate_state.temperature, held);
                    checkRkPorousLedgers(r);
                }
            }
        }
}
TEST(CoupledGroundwaterRk2,
     PorousHeldAirCheckpointBindsWeatherAndInvalidInputLeavesNumericalOwnersUntouched) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig a, b;
            RkPorousOwner oa, ob;
            const auto init = [&](CoupledRkRig& r, RkPorousOwner& o) {
                r.init(.1, 0, 0, 0, "NONE", closure, SoilChar::GARDNER, 4);
                o.initialize(r, layered, 3, true, 1e6);
                o.config.options.surface_thermal_bc = "AIR";
                o.config.options.surface_resistance = 2e-6;
                o.context.climate_state.temperature = 44.6;
            };
            init(a, oa);
            init(b, ob);
            ASSERT_NO_THROW(a.solver.advance(0, .6));
            const auto saved = rk2Output("held-air-source.chk"),
                       before = rk2Output("held-air-before.chk"),
                       after = rk2Output("held-air-after.chk");
            ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
            ob.context.climate_state.temperature = 43;
            ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
            EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ob.context.climate_state.temperature = 44.6;
            ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            ASSERT_NO_THROW(a.solver.advance(.6, 2));
            ASSERT_NO_THROW(b.solver.advance(.6, 2));
            ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            CoupledRkRig fresh;
            RkPorousOwner owner;
            init(fresh, owner);
            ASSERT_EQ(fresh.solver.saveCheckpoint(before.string()), "");
            owner.context.climate_state.temperature = std::numeric_limits<double>::quiet_NaN();
            EXPECT_THROW(fresh.solver.advance(0, .1), std::runtime_error);
            EXPECT_TRUE(std::isnan(owner.context.climate_state.temperature));
            owner.context.climate_state.temperature = 44.6;
            ASSERT_EQ(fresh.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ASSERT_NO_THROW(fresh.solver.advance(0, .1));
            checkRkPorousLedgers(fresh);
        }
}

namespace {
/**
 * @brief Initialize finite surface-water/porous-ground thermal contact.
 *
 * Initializes porous heat with vertical conduction, replaces layer energies/baselines, refreshes
 * aqueous temperature views, then binds SURFACE_WATER boundary and ground-column mesh heat through
 * owner context pointers. Disables atmospheric/radiative heat for this fixture, reinitializes
 * solver and fatally checks acceptance. Requires owner/rig lifetimes and physical capacities; no
 * file/model lifecycle or timestep is advanced.
 *
 * @param[in,out] r Water rig whose thermal/solver state is rebuilt.
 * @param[in,out] owner Fresh live porous owner retained through bound context/config use.
 * @param[in] layered Select layered aqueous temperature storage.
 * @param[in] layers Positive thermal column layer count.
 * @param[in] conductivity Solid conductivity in W/(m K).
 * @param[in] second Positive cell-one conductivity override in W/(m K), otherwise absent.
 * @param[in] water_temperature Initial surface water temperature in degrees C.
 * @param[in] soil_temperature Initial ground-column temperature in degrees C.
 */
void initRkFiniteSurface(CoupledRkRig& r, RkPorousOwner& owner, bool layered, int layers = 1,
                         double conductivity = 5e5, double second = 0, double water_temperature = 6,
                         double soil_temperature = -2) {
    owner.initialize(r, layered, layers, true, conductivity, second);
    for (int cell = 0; cell < r.mesh.n_cells(); ++cell) {
        auto& c = r.gw.heat().cells[cell];
        for (std::size_t j = 0; j < c.column.energy.size(); ++j)
            c.column.energy[j] = soil_temperature * c.column.capacity[j];
        c.column.initial_energy = c.column.energy;
        c.initial_j = std::accumulate(c.column.energy.begin(), c.column.energy.end(), 0.);
        r.gw.heat().refreshView(cell, r.gw.transport());
        r.state.transport.cell_mass[cell] = water_temperature * r.state.volume[cell];
    }
    owner.config.options.surface_thermal_bc = "SURFACE_WATER";
    auto& ctx = owner.context;
    ctx.twod_io.gw = &owner.config;
    ctx.twod_io.aquifer_state = &r.gw.state();
    r.opts.coupled_heat = true;
    r.opts.bed_heat_model = 2;
    r.opts.heat_surface = r.opts.heat_radiative = 0;
    std::vector<uint8_t> covered(r.mesh.n_cells(), 1);
    ASSERT_EQ(initializeMeshHeat(ctx, r.mesh, r.state, r.opts, &r.gw.heat(), &covered), "");
    r.solver.initialize(r.mesh, r.state, r.opts);
    r.solver.setSubsurface(&r.gw);
    ASSERT_EQ(r.gw.rk2ProfileError(), "");
}
/**
 * @brief Sum ground and surface energy for the temperature-only finite-contact fixture.
 *
 * This sums every surface cell_mass entry and is valid only for the temperature-only layout; mixed
 * solute/age layouts require their own budget logic. Does not include exported heat or mutate
 * state.
 *
 * @param[in] r Borrowed rig whose surface transport contains only temperature-volume entries.
 * @return Porous column joules plus 4186000 J/(m3 K) times all surface temperature-volume content.
 */
double rkFiniteEnergy(const CoupledRkRig& r) {
    return rkPorousEnergy(r) + 4186000 * std::accumulate(r.state.transport.cell_mass.begin(),
                                                         r.state.transport.cell_mass.end(), 0.);
}
}
TEST(CoupledGroundwaterRk2, FiniteSurfaceSoilContactMatchesSignedTwoStoreSolution) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (const auto initial :
                 {std::array<double, 2>{6, -2}, std::array<double, 2>{-6, 3}}) {
                double previous = 0;
                for (double dt : {.4, .2, .1, .05}) {
                    CoupledRkRig r;
                    r.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    initRkFiniteSurface(r, owner, layered, 1, 5e5, 0, initial[0], initial[1]);
                    const auto& c = r.gw.heat().cells[0].column;
                    const double soil = c.capacity[0], water = 4186000 * r.state.volume[0],
                                 g = c.area * c.conductivity[0];
                    const double mean = (water * initial[0] + soil * initial[1]) / (soil + water),
                                 decay = std::exp(-g * (1 / water + 1 / soil) * 2);
                    const double exact_soil =
                        mean - water / (water + soil) * (initial[0] - initial[1]) * decay;
                    const double exact_water =
                        mean + soil / (water + soil) * (initial[0] - initial[1]) * decay;
                    const double before = rkFiniteEnergy(r);
                    for (int step = 0; step < int(std::round(2 / dt)); ++step)
                        ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
                    const double ts = c.energy[0] / c.capacity[0],
                                 tw = r.state.transport.cell_mass[0] / r.state.volume[0];
                    const double error = std::hypot(ts - exact_soil, tw - exact_water) / 20;
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    RecordProperty("finite_pair_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" +
                                       std::to_string(int(initial[0])) + "_" +
                                       std::to_string(int(dt * 1000)),
                                   error);
                    EXPECT_NEAR(rkFiniteEnergy(r), before, 1e-10 * std::max(1., std::abs(before)));
                    checkRkPorousLedgers(r);
                    for (int i = 0; i < r.mesh.n_cells(); ++i) {
                        EXPECT_NEAR(r.state.heat.ground_column_seconds[i], 2, 1e-13);
                        EXPECT_NEAR(r.state.heat.water_j[i] + r.gw.heat().cells[i].water_contact_j,
                                    0, 1e-6);
                    }
                }
            }
        }
}
TEST(CoupledGroundwaterRk2, FiniteSurfaceMovingColumnMatchesIndependentCoupledWaterEnergyOde) {
    const auto exact = porousColumnReference(20000, false, false, 0, true),
               refined = porousColumnReference(40000, false, false, 0, true);
    for (int k = 0; k < 10; ++k)
        ASSERT_NEAR(exact[k], refined[k], 1e-10);
    for (bool layered : {false, true}) {
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            RkPorousOwner owner;
            initRkFiniteSurface(r, owner, layered, 3, 2e5);
            const double initial_energy = rkFiniteEnergy(r), initial_water = r.total(),
                         area = r.gw.state().area[0];
            for (int step = 0; step < int(std::round(2 / dt)); ++step)
                ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
            const auto& w = r.gw.state();
            std::vector<double> result(10);
            result[0] = w.hg[0];
            for (int j = 0; j < 4; ++j)
                result[j + 1] = w.theta_sigma[std::size_t(j) * w.n_cells] * (2 - w.hg[0]) / 4;
            result[5] = r.state.volume[0] / area;
            result[6] = r.state.transport.cell_mass[0] / area;
            for (int j = 0; j < 3; ++j)
                result[7 + j] = r.gw.heat().cells[0].column.energy[j] / (4186000 * area);
            double error = 0;
            for (int j = 0; j < 10; ++j)
                error += std::pow(result[j] - exact[j], 2);
            error = std::sqrt(error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("finite_moving_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 10000)),
                           error);
            EXPECT_NEAR(r.total() + w.led_deep, initial_water, 1e-10 * initial_water);
            EXPECT_NEAR(rkFiniteEnergy(r) + 4186000 * r.gw.transport().lost_deep[0], initial_energy,
                        1e-10 * std::max(1., std::abs(initial_energy)));
            checkRkPorousLedgers(r);
        }
    }
}
TEST(CoupledGroundwaterRk2, FiniteSurfaceWetDryEtKeepsSensibleAndSoilLatentJoulesSeparate) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double pond : {0., 1e-5, .01})
                for (double demand : {.0001, 100.})
                    for (const std::string mode : {"SENSIBLE_ONLY", "LATENT_SURFACE"}) {
                        SCOPED_TRACE(int(closure));
                        SCOPED_TRACE(layered);
                        SCOPED_TRACE(pond);
                        SCOPED_TRACE(demand);
                        SCOPED_TRACE(mode);
                        CoupledRkRig r;
                        r.init(.05, 0, 0, pond, "BOUNDARY_ET", closure, SoilChar::GARDNER, 4);
                        RkPorousOwner owner;
                        initRkFiniteSurface(r, owner, layered, 3, 2e5);
                        owner.config.options.soil_et_heat = mode;
                        r.opts.evaporation = true;
                        r.state.evap_rate.assign(r.mesh.n_cells(), demand);
                        const double initial_energy = rkFiniteEnergy(r), initial_water = r.total();
                        ASSERT_NO_THROW(r.solver.advance(0, 2));
                        double latent = 0;
                        for (const auto& c : r.gw.heat().cells)
                            latent += c.latent_j;
                        const double exported = 4186000 * (r.state.transport.lost_external[0] +
                                                           r.gw.transport().lost_et[0]);
                        EXPECT_NEAR(rkFiniteEnergy(r) + exported - latent, initial_energy,
                                    1e-10 * std::max(1., std::abs(initial_energy)));
                        EXPECT_NEAR(r.total() + r.state.evap_loss_total + r.gw.state().led_et,
                                    initial_water, 1e-10 * std::max(1., initial_water));
                        for (const auto& c : r.gw.heat().cells)
                            for (double value : c.column.energy)
                                EXPECT_TRUE(std::isfinite(value));
                        checkRkPorousLedgers(r);
                    }
        }
}
TEST(CoupledGroundwaterRk2,
     FiniteSurfaceCheckpointRestoresReciprocalOwnerAndRefusesChangedContactDefinition) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig a, b;
            RkPorousOwner oa, ob;
            const auto init = [&](CoupledRkRig& r, RkPorousOwner& o) {
                r.init(.1, .0001, 0, .1, "NONE", closure, SoilChar::GARDNER, 4);
                initRkFiniteSurface(r, o, layered, 3, 2e5);
                GwResolvedSource source;
                source.name = "finite-cold-well";
                source.cells = {{0, 1}};
                source.flow.times = {0, .37, 1.1, 2};
                source.flow.values = {.001, -.0005, .0008, -.0002};
                GwResolvedSourceTerm term;
                term.row = 0;
                term.signal.constant = -8;
                source.terms = {term};
                r.gw.setSources({source});
            };
            init(a, oa);
            init(b, ob);
            ASSERT_NO_THROW(a.solver.advance(0, .7));
            const auto saved = rk2Output("finite-source.chk"),
                       before = rk2Output("finite-before.chk"),
                       after = rk2Output("finite-after.chk");
            ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
            const double thickness = ob.context.heat_config.sediment.bed_thickness;
            ob.context.heat_config.sediment.bed_thickness = thickness + .01;
            ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
            EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ob.context.heat_config.sediment.bed_thickness = thickness;
            ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
            b.state.heat.surface = true;
            EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
            b.state.heat.surface = false;
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
            EXPECT_EQ(b.state.heat.aquifer_ground, &b.gw.heat());
            ASSERT_NO_THROW(a.solver.advance(.7, 2));
            ASSERT_NO_THROW(b.solver.advance(.7, 2));
            ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            EXPECT_EQ(b.state.heat.aquifer_ground, &b.gw.heat());
            checkRkPorousLedgers(a);
            checkRkPorousLedgers(b);
        }
}
TEST(CoupledGroundwaterRk2, FiniteSurfaceLaterCellWorkLimitRestoresBothThermalOwnersAndCanRetry) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig r, control;
            r.init(.1, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
            control.init(.1, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
            RkPorousOwner owner, control_owner;
            initRkFiniteSurface(r, owner, layered, 3, 5e5, 1e12);
            initRkFiniteSurface(control, control_owner, layered, 3);
            ExplicitInertialSolverTestAccess::prepare(r.solver);
            ExplicitInertialSolverTestAccess::prepare(control.solver);
            ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(control.solver, 2, 0));
            EXPECT_NE(control.gw.heat().cells[0].water_contact_j, 0);
            const auto before = rk2Output("finite-work-before.chk"),
                       after = rk2Output("finite-work-after.chk");
            const double initial = rkFiniteEnergy(r);
            ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
            EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 2, 0), std::runtime_error);
            ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            ASSERT_NO_THROW(r.solver.advance(0, .001));
            EXPECT_NEAR(rkFiniteEnergy(r), initial, 1e-10 * std::max(1., std::abs(initial)));
            checkRkPorousLedgers(r);
        }
}
TEST(CoupledGroundwaterRk2,
     FiniteSurfaceTinyWaterPairRemainsMonotoneWithItsExactReciprocalReceipt) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig r;
            r.init(.1, 0, 0, 1e-12, "NONE", closure, SoilChar::GARDNER, 4);
            RkPorousOwner owner;
            initRkFiniteSurface(r, owner, layered);
            const double water_capacity = 4186000 * r.state.volume[0],
                         soil_capacity = r.gw.heat().cells[0].column.capacity[0];
            const double mean = (6 * water_capacity - 2 * soil_capacity) /
                                (water_capacity + soil_capacity),
                         expected = water_capacity * (6 - mean);
            ASSERT_NO_THROW(r.solver.advance(0, .1));
            for (int i = 0; i < r.mesh.n_cells(); ++i) {
                EXPECT_NEAR(r.state.transport.cell_mass[i] / r.state.volume[i], mean, 1e-10);
                EXPECT_NEAR(r.gw.heat().cells[i].column.energy[0] / soil_capacity, mean, 1e-10);
                EXPECT_NEAR(r.gw.heat().cells[i].water_contact_j, expected, 1e-12);
                EXPECT_NEAR(r.state.heat.water_j[i], -expected, 1e-12);
            }
            checkRkPorousLedgers(r);
        }
}
TEST(CoupledGroundwaterRk2, FiniteSurfaceFullAndTwoHalfStepsHaveThirdOrderLocalDifference) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig full, halves;
                full.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                halves.init(.5 * dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner a, b;
                initRkFiniteSurface(full, a, layered);
                initRkFiniteSurface(halves, b, layered);
                ASSERT_NO_THROW(full.solver.advance(0, dt));
                ASSERT_NO_THROW(halves.solver.advance(0, .5 * dt));
                ASSERT_NO_THROW(halves.solver.advance(.5 * dt, dt));
                const double surface = full.state.transport.cell_mass[0] / full.state.volume[0] -
                                       halves.state.transport.cell_mass[0] / halves.state.volume[0];
                const double soil = full.gw.heat().cells[0].column.energy[0] /
                                        full.gw.heat().cells[0].column.capacity[0] -
                                    halves.gw.heat().cells[0].column.energy[0] /
                                        halves.gw.heat().cells[0].column.capacity[0];
                const double error = std::hypot(surface, soil) / 20;
                EXPECT_LT(error, 1e-3);
                if (previous > 0)
                    EXPECT_GT(previous / error, 7);
                previous = error;
                RecordProperty("finite_local_" + std::to_string(int(closure)) + "_" +
                                   std::to_string(layered) + "_" + std::to_string(int(dt * 1000)),
                               error);
            }
        }
}

namespace {
/**
 * @brief Initialize reactive/age contact between finite surface water and porous ground.
 *
 * Begins with porous A/B/age/temperature at -2 C, configures two-metre bed and 0.04 native solute
 * diffusivity, enables ground-column SURFACE_WATER contact and seeds surface A=8/age=11 s. Fatally
 * requires mesh heat and reaction configuration, then rebuilds/rebinds solver. Owners must remain
 * alive/unmoved; assertion failures leave partial setup.
 *
 * @param[in,out] r Water rig whose transport/heat/solver state is rebuilt.
 * @param[in,out] owner Fresh live porous config/context owner.
 * @param[in,out] system Live chemistry context owner retained during bound reactions/heat.
 * @param[in] layered Select layered aqueous species storage.
 * @param[in] thermal_layers Positive thermal column layer count.
 * @param[in] kd Native A/B partition coefficient in m3/kg.
 */
void initRkFiniteQuality(CoupledRkRig& r, RkPorousOwner& owner, RkReactionSystem& system,
                         bool layered, int thermal_layers = 1, double kd = .001) {
    initRkPorousQuality(r, owner, system, layered, true, kd, -2, thermal_layers);
    auto& ctx = system.context();
    ctx.twod_io.gw = &owner.config;
    ctx.twod_io.aquifer_state = &r.gw.state();
    ctx.heat_config.sediment.bed_thickness = 2;
    ctx.heat_config.sediment.solute_diffusivity = .04;
    owner.config.options.surface_thermal_bc = "SURFACE_WATER";
    r.opts.coupled_heat = true;
    r.opts.bed_heat_model = 2;
    r.opts.heat_surface = r.opts.heat_radiative = 0;
    std::vector<uint8_t> covered(r.mesh.n_cells(), 1);
    ASSERT_EQ(initializeMeshHeat(ctx, r.mesh, r.state, r.opts, &r.gw.heat(), &covered), "");
    auto& tr = r.state.transport;
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        tr.cell_mass[tr.idx(0, i)] = 8 * r.state.volume[i];
        tr.cell_mass[tr.idx(tr.age_row, i)] = 11 * r.state.volume[i];
    }
    r.solver.initialize(r.mesh, r.state, r.opts);
    r.solver.setSubsurface(&r.gw);
    ASSERT_EQ(r.solver.configureRk2Reactions(ctx), "");
}
/**
 * @brief Assert per-species contact/reaction/age budgets and porous energy closure.
 *
 * For each non-temperature row, compares stored plus selected losses minus generated age against
 * initial at relative 1e-10 with floor one; checks GW residual and equal/opposite bed-contact
 * gains. Also calls checkRkPorousLedgers. Native concentration-volume/age-volume units are retained
 * per row; does not alter ledgers or assert arbitrary absent transport channels.
 *
 * @param[in] r Borrowed valid matching surface/GW reactive transport layout.
 * @param[in] initial Borrowed initial total-inventory vector covering every species row, from
 *     finiteQualityInitial.
 */
void checkFiniteQualityBudgets(const CoupledRkRig& r, const std::vector<double>& initial) {
    const auto& t = r.gw.transport();
    const auto& s = r.state.transport;
    for (int row = 0; row < t.n_species; ++row)
        if (row != t.temp_row) {
            double surface = 0;
            for (int i = 0; i < t.n_cells; ++i)
                surface += s.cell_mass[s.idx(row, i)];
            EXPECT_NEAR(t.storage(row) + surface + t.lost_reaction[row] +
                            r.solver.surfaceReactionLoss(row) + t.lost_et[row] +
                            s.lost_external[row] - t.generated_age[row] - s.generated_age[row],
                        initial[row], 1e-10 * std::max(1., initial[row]));
            EXPECT_NEAR(t.residual(row), 0, 1e-10 * std::max(1., t.init_mass[row]));
            EXPECT_NEAR(t.gained_contact[row] + s.gained_bed[row], 0,
                        1e-10 * std::max(1., initial[row]));
        }
    checkRkPorousLedgers(r);
}
/**
 * @brief Capture total groundwater-plus-surface inventory per transport row.
 *
 * Sums GW storage(row) and surface cell_mass; excludes exports, generated age and reaction losses.
 * Does not convert temperature content to joules or validate layout bounds.
 *
 * @param[in] r Borrowed rig with matching GW/surface row/cell layouts.
 * @return Owned vector in per-row native content units, including temperature-volume when present.
 */
std::vector<double> finiteQualityInitial(const CoupledRkRig& r) {
    std::vector<double> result(r.gw.transport().n_species);
    for (int row = 0; row < int(result.size()); ++row) {
        result[row] = r.gw.transport().storage(row);
        for (int i = 0; i < r.mesh.n_cells(); ++i)
            result[row] += r.state.transport.cell_mass[r.state.transport.idx(row, i)];
    }
    return result;
}
// Independent canonical reactor ODE, in mass per unit mesh area. Geometry
// intersections are integrated here without calling the production map.
/**
 * @brief Integrate an independent static finite-bed contact reference using fixture capacities.
 *
 * Reads only fixture material/moisture/kd/rho_b metadata, independently computes overlaps/transfer
 * rates and integrates fixed 1.1 m bed/0.044 diffusivity contact. Uses initial ground A=2/age=5 s
 * and surface A=8/age=11 s, 0.2 m ponding. Not a general reference from arbitrary live mass state;
 * requires positive capacities/overlaps and compatible layouts, with no validation or finite
 * guards.
 *
 * @param[in] r Borrowed rig matching the fixed 2 m column, 0.6 m table, three thermal layers and
 *     intended initialized moisture/sorption profile.
 * @param[in] layered Use four aqueous layers plus SAT; false uses three physical ground stores.
 * @param[in] row Valid intended A or age row; selects sorption capacity and fixed initial
 *     concentration/age presets.
 * @param[in] steps Positive uniform RK4 step count over two seconds.
 * @return Owned ground-store contents followed by surface content, in native concentration-depth
 *     units or seconds-depth for age.
 */
std::vector<double> finiteContactReference(const CoupledRkRig& r, bool layered, int row,
                                           int steps) {
    const auto& w = r.gw.state();
    const auto& t = r.gw.transport();
    const double partition = row == t.age_row ? 0 : t.kd[t.idx(row, 0)] * t.rho_b[0];
    const int nr = layered ? 5 : 3;
    constexpr double dx = 2. / 3, bed = 1.1, diffusivity = .044;
    std::vector<std::vector<double>> capacities(3, std::vector<double>(nr)),
        water(3, std::vector<double>(nr));
    std::vector<double> total(nr), aqueous(nr), physical(3), theta(3), fraction(3);
    double overlap = 0;
    for (int j = 0; j < 3; ++j)
        for (int k = 0; k < nr; ++k) {
            const double top = layered ? (k == 4 ? 1.4 : .35 * k) : dx * k,
                         bottom = layered ? (k == 4 ? 2 : .35 * (k + 1)) : dx * (k + 1);
            const double length =
                std::max(0., std::min(dx * (j + 1), bottom) - std::max(dx * j, top));
            double moisture;
            if (layered)
                moisture = k == 4 ? .45 : w.theta_sigma[std::size_t(k) * w.n_cells];
            else
                moisture = r.gw.heat().cells[0].column.theta[k];
            water[j][k] = length * moisture;
            capacities[j][k] = length * (moisture + partition);
            total[k] += capacities[j][k];
            aqueous[k] += water[j][k];
            physical[j] += capacities[j][k];
            theta[j] += water[j][k] / dx;
        }
    for (int j = 0; j < 3; ++j) {
        fraction[j] = theta[j] * std::max(0., std::min(bed, dx * (j + 1)) - dx * j);
        overlap += fraction[j];
    }
    std::vector<double> y(nr + 1);
    y.back() = (row == t.age_row ? 11 : 8) * .2;
    for (int k = 0; k < nr; ++k)
        y[k] = (row == t.age_row ? 5 : 2) * total[k];
    const auto rate = [&](const std::vector<double>& mass) {
        std::vector<double> out(nr + 1, 0);
        if (row == t.age_row) {
            for (int k = 0; k < nr; ++k)
                out[k] = aqueous[k];
            out.back() = .2;
        }
        for (int j = 0; j < 3; ++j) {
            double mapped = 0;
            for (int k = 0; k < nr; ++k)
                mapped += mass[k] * capacities[j][k] / total[k];
            const double flux = diffusivity / bed * fraction[j] / overlap *
                                (mass.back() / .2 - mapped / physical[j]);
            out.back() -= flux;
            for (int k = 0; k < nr; ++k)
                out[k] += flux * (flux >= 0 ? capacities[j][k] / physical[j]
                                            : (mass[k] * capacities[j][k] / total[k]) / mapped);
        }
        return out;
    };
    const double dt = 2. / steps;
    for (int n = 0; n < steps; ++n) {
        const auto a = rate(y);
        auto z = y;
        for (int k = 0; k <= nr; ++k)
            z[k] = y[k] + .5 * dt * a[k];
        const auto b = rate(z);
        for (int k = 0; k <= nr; ++k)
            z[k] = y[k] + .5 * dt * b[k];
        const auto c = rate(z);
        for (int k = 0; k <= nr; ++k)
            z[k] = y[k] + dt * c[k];
        const auto d = rate(z);
        for (int k = 0; k <= nr; ++k)
            y[k] += dt * (a[k] + 2 * b[k] + 2 * c[k] + d[k]) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2,
     FiniteChemicalContactMatchesSignedRetardedPairWithSeparateAgeProduction) {
    RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double surface_concentration : {.5, 8.}) {
                double previous = 0;
                for (double dt : {.4, .2, .1, .05}) {
                    CoupledRkRig r;
                    r.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                    RkPorousOwner owner;
                    initRkFiniteQuality(r, owner, system, layered);
                    auto& surface = r.state.transport;
                    for (int i = 0; i < surface.n_cells; ++i)
                        surface.cell_mass[surface.idx(0, i)] =
                            surface_concentration * r.state.volume[i];
                    const auto initial = finiteQualityInitial(r);
                    const auto& w = r.gw.state();
                    const auto& t = r.gw.transport();
                    const double cw = r.state.volume[0],
                                 cs = w.area[0] * (w.hu[0] + w.theta_s[0] * w.hg[0] +
                                                   2 * t.kd[t.idx(0, 0)] * t.rho_b[0]);
                    const double mean = (cw * surface_concentration + cs * 2) / (cw + cs),
                                 g = .04 / 2 * w.area[0];
                    const double exact = mean + cs / (cw + cs) * (surface_concentration - 2) *
                                                    std::exp(-g * (1 / cw + 1 / cs) * 2);
                    ASSERT_NO_THROW(r.solver.advance(0, 2));
                    const double error =
                        std::abs(surface.cell_mass[surface.idx(0, 0)] / cw - exact) / 8;
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    RecordProperty("finite_solute_pair_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" +
                                       std::to_string(int(surface_concentration * 10)) + "_" +
                                       std::to_string(int(dt * 1000)),
                                   error);
                    EXPECT_NEAR(t.generated_age[t.age_row], 2 * w.storage(), 1e-10);
                    EXPECT_NEAR(surface.generated_age[surface.age_row], 2 * cw * w.n_cells, 1e-10);
                    checkFiniteQualityBudgets(r, initial);
                }
            }
        }
}
TEST(CoupledGroundwaterRk2, FiniteUnequalGridChemicalAndAgeContactsMatchIndependentCanonicalStar) {
    RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
    for (bool layered : {false, true})
        for (int row : {0, 2}) {
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
                seedRkSigma(r);
                RkPorousOwner owner;
                initRkFiniteQuality(r, owner, system, layered, 3);
                system.context().heat_config.sediment.bed_thickness = 1.1;
                system.context().heat_config.sediment.solute_diffusivity = .044;
                const auto initial = finiteQualityInitial(r);
                const auto reference = finiteContactReference(r, layered, row, 20000),
                           refined = finiteContactReference(r, layered, row, 40000);
                for (std::size_t k = 0; k < reference.size(); ++k)
                    ASSERT_NEAR(reference[k], refined[k], 1e-10);
                ASSERT_NO_THROW(r.solver.advance(0, 2));
                const auto& t = r.gw.transport();
                const double area = r.gw.state().area[0];
                std::vector<double> actual;
                if (layered) {
                    for (int k = 0; k < 4; ++k)
                        actual.push_back(t.layer_mass[t.layerIdx(row, k, 0)] / area);
                    actual.push_back(t.sat_mass[t.idx(row, 0)] / area);
                } else {
                    const auto& p = r.gw.heat().cells[0].porewater;
                    for (int k = 0; k < 3; ++k)
                        actual.push_back(p.mass[std::size_t(row) * 3 + k] / area);
                }
                actual.push_back(r.state.transport.cell_mass[r.state.transport.idx(row, 0)] / area);
                double error = 0;
                for (std::size_t k = 0; k < actual.size(); ++k)
                    error += std::pow(actual[k] - reference[k], 2);
                error = std::sqrt(error) / 10;
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                RecordProperty("finite_star_" + std::to_string(layered) + "_" +
                                   std::to_string(row) + "_" + std::to_string(int(dt * 1000)),
                               error);
                checkFiniteQualityBudgets(r, initial);
            }
        }
}
TEST(CoupledGroundwaterRk2, FiniteChemicalContactLocalDifferencesHaveThirdOrder) {
    RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
    for (bool layered : {false, true}) {
        double previous = 0;
        for (double dt : {.4, .2, .1, .05}) {
            CoupledRkRig a, b;
            a.init(dt, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            b.init(.5 * dt, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            RkPorousOwner oa, ob;
            initRkFiniteQuality(a, oa, system, layered, 3);
            initRkFiniteQuality(b, ob, system, layered, 3);
            ASSERT_NO_THROW(a.solver.advance(0, dt));
            ASSERT_NO_THROW(b.solver.advance(0, .5 * dt));
            ASSERT_NO_THROW(b.solver.advance(.5 * dt, dt));
            double error = 0;
            const auto& x = a.state.transport.cell_mass;
            const auto& y = b.state.transport.cell_mass;
            for (std::size_t k = 0; k < x.size(); ++k)
                error += std::pow(x[k] - y[k], 2);
            error = std::sqrt(error) / 100;
            EXPECT_LT(error, 1e-3);
            if (previous > 0)
                EXPECT_GT(previous / error, 7);
            previous = error;
            RecordProperty("finite_solute_local_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 1000)),
                           error);
        }
    }
}
TEST(CoupledGroundwaterRk2,
     FiniteContactProtectsAcceptedMassAndWaterAndDoesNotRecycleStageInflows) {
    RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        RkPorousOwner owner;
        initRkFiniteQuality(r, owner, system, layered, 3);
        auto& cell = r.gw.heat().cells[0];
        auto& pore = cell.porewater;
        const auto original = pore;
        if (layered) {
            for (std::size_t k = 0; k < pore.sigma.overlaps.size(); ++k)
                pore.sigma.reserved[k] = .75 * pore.sigma.mass[k];
            pore.refreshSigmaView();
        } else
            for (int j = 0; j < 3; ++j)
                pore.reserved_out[j] = .75 * pore.mass[j];
        for (int j = 0; j < 3; ++j)
            cell.reserved_out_water[j] =
                .5 * cell.column.theta[j] * cell.column.area *
                (cell.column.depth_edges[j + 1] - cell.column.depth_edges[j]);
        const auto reserved = pore.reserved_out;
        double expected = 0;
        for (int j = 0; j < 3; ++j) {
            const double water = cell.column.theta[j] * cell.column.area *
                                 (cell.column.depth_edges[j + 1] - cell.column.depth_edges[j]);
            double all_water = 0;
            for (int k = 0; k < 3; ++k)
                all_water += cell.column.theta[k] *
                             (cell.column.depth_edges[k + 1] - cell.column.depth_edges[k]);
            const double q = .04 / 2 * cell.column.area * (water / cell.column.area / all_water);
            expected += .1 * q * (pore.mass[j] - reserved[j]) / (.5 * water + pore.retention[j]);
        }
        const double mass =
            applyLocalBedSpeciesEuler(r.mesh, r.state.heat, 0, 0, 0, r.state.volume[0], .1);
        EXPECT_NEAR(mass, expected, 1e-12);
        EXPECT_EQ(pore.reserved_out, reserved);
        for (int j = 0; j < 3; ++j)
            EXPECT_GE(pore.mass[j], reserved[j]);
        pore = original;
        cell.reserved_out_water.assign(3, 0);
        system.context().heat_config.sediment.solute_diffusivity = 1e8;
        const double before = std::accumulate(pore.mass.begin(), pore.mass.begin() + 3, 0.);
        const double available = .001,
                     after = applyLocalBedSpeciesEuler(r.mesh, r.state.heat, 0, 0, available,
                                                       r.state.volume[0], 1);
        EXPECT_GE(after, 0);
        EXPECT_NEAR(after + std::accumulate(pore.mass.begin(), pore.mass.begin() + 3, 0.),
                    before + available, 1e-10 * before);
        const auto dry = pore;
        EXPECT_EQ(applyLocalBedSpeciesEuler(r.mesh, r.state.heat, 0, 0, .01, 0, 1), .01);
        EXPECT_EQ(pore.mass, dry.mass);
        if (!layered) {
            pore = original;
            pore.mass[0] = 0;
            const double soil_before =
                std::accumulate(pore.mass.begin(), pore.mass.begin() + 3, 0.);
            const double result = applyLocalBedSpeciesEuler(r.mesh, r.state.heat, 0, 0, available,
                                                            r.state.volume[0], 1);
            EXPECT_NEAR(pore.mass[0], available, 1e-15);
            EXPECT_GT(result, available);
            EXPECT_NEAR(result + std::accumulate(pore.mass.begin(), pore.mass.begin() + 3, 0.),
                        soil_before + available, 1e-10 * soil_before);
        }
    }
}
TEST(CoupledGroundwaterRk2,
     FiniteReactiveContactCheckpointContinuesExactlyAndBindsContactAndSorption) {
    RkReactionSystem system(
        "* SAT A RATE -1000*A\n* SAT B RATE 1000*A-.2*B\n* UNSAT A RATE -1000*A\n* UNSAT B RATE 1000*A-.2*B\n",
        "ROS2", "RATE A -.3*A\nRATE B .3*A\n");
    for (bool layered : {false, true}) {
        CoupledRkRig a, b, bad;
        RkPorousOwner oa, ob, oc;
        const auto init = [&](CoupledRkRig& r, RkPorousOwner& o, double kd) {
            r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkFiniteQuality(r, o, system, layered, 3, kd);
        };
        init(a, oa, .001);
        init(b, ob, .001);
        init(bad, oc, .002);
        const auto initial = finiteQualityInitial(a);
        ASSERT_NO_THROW(a.solver.advance(0, .7));
        const auto saved = rk2Output("finite-reactive-contact.chk"),
                   before = rk2Output("finite-reactive-before.chk"),
                   after = rk2Output("finite-reactive-after.chk");
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(bad.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(bad.solver.loadCheckpoint(saved.string()).empty());
        ASSERT_EQ(bad.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        system.context().heat_config.sediment.solute_diffusivity = .08;
        ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        system.context().heat_config.sediment.solute_diffusivity = .04;
        ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
        ASSERT_NO_THROW(a.solver.advance(.7, 2));
        ASSERT_NO_THROW(b.solver.advance(.7, 2));
        ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        checkFiniteQualityBudgets(a, initial);
        checkFiniteQualityBudgets(b, initial);
        EXPECT_NE(a.gw.transport().gained_contact[1], 0);
    }
}
TEST(CoupledGroundwaterRk2, FiniteLaterCellChemicalFailureRestoresAllOwnersAndSupportsRetry) {
    for (bool layered : {false, true}) {
        RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
        CoupledRkRig r;
        r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        RkPorousOwner owner;
        initRkFiniteQuality(r, owner, system, layered, 3);
        auto& overrides = system.context().heat_config.overrides;
        overrides.sed_cell.assign(r.mesh.n_cells(), system.context().heat_config.sediment);
        overrides.sed_cell[1].solute_diffusivity = -1;
        const auto before = rk2Output("finite-chemical-failure-before.chk"),
                   after = rk2Output("finite-chemical-failure-after.chk");
        ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
        const auto initial = finiteQualityInitial(r);
        EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
        ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        EXPECT_DOUBLE_EQ(r.gw.transport().gained_contact[0], 0);
        overrides.sed_cell[1].solute_diffusivity = .04;
        ASSERT_NO_THROW(r.solver.advance(0, .1));
        EXPECT_GT(r.gw.transport().gained_contact[0], 0);
        checkFiniteQualityBudgets(r, initial);
    }
}
TEST(CoupledGroundwaterRk2, FiniteChemicalContactUsesOnlyTheWettedVfrFootprint) {
    RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.mesh = makeGridMesh(1, 1, 4, [](double x, double) { return x / 4; });
        r.opts.cell_closure = CellClosure2D::VFR;
        r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        RkPorousOwner owner;
        initRkFiniteQuality(r, owner, system, layered);
        std::vector<double> exact;
        for (int i = 0; i < r.mesh.n_cells(); ++i) {
            const double wet =
                r.mesh.tri_area[i] * (i == 0 ? .04 : .36); // clipped plane z=x/4 at stage .2
            EXPECT_LT(wet, .99 * r.mesh.tri_area[i]);
            EXPECT_GT(wet, 0);
            const double cw = r.state.volume[i];
            const auto& cell = r.gw.heat().cells[i];
            const double cs =
                cell.column.theta[0] * cell.column.area * 2 + cell.porewater.retention[0];
            const double mean = (8 * cw + 2 * cs) / (cw + cs), g = .04 / 2 * wet;
            exact.push_back(mean + cs / (cw + cs) * 6 * std::exp(-g * (1 / cw + 1 / cs) * 1e-4));
        }
        const auto file = rk2Output("vfr-geometric-contact.chk");
        ASSERT_EQ(r.solver.saveCheckpoint(file.string()), "");
        EXPECT_NE(rk2File(file).find("RK2 finite surface/soil geometric contact v2"),
                  std::string::npos);
        const auto before = finiteQualityInitial(r);
        ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::contact(r.solver, 1e-4));
        for (int i = 0; i < r.mesh.n_cells(); ++i)
            EXPECT_NEAR(r.state.transport.cell_mass[r.state.transport.idx(0, i)] /
                            r.state.volume[i],
                        exact[i], 1e-10);
        checkFiniteQualityBudgets(r, before);
    }
}
TEST(CoupledGroundwaterRk2,
     FiniteReactiveMovingColumnMatchesIndependentJointWaterHeatChemicalAndAgeOde) {
    constexpr double partition = .001 * 2650 * (1 - .45), cp = 4186000;
    for (bool layered : {false, true}) {
        const auto exact = porousColumnReference(20000, true, layered, partition, true, true),
                   refined = porousColumnReference(40000, true, layered, partition, true, true);
        for (std::size_t k = 0; k < exact.size(); ++k)
            ASSERT_NEAR(exact[k], refined[k], 1e-10);
        RkReactionSystem system(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            RkPorousOwner owner;
            initRkFiniteQuality(r, owner, system, layered, 3);
            owner.config.options.conduction = true;
            auto& surface = r.state.transport;
            const auto& w = r.gw.state();
            const auto& tr = r.gw.transport();
            owner.config.params[0].lambda_s = 2e5;
            for (int i = 0; i < w.n_cells; ++i) {
                auto& column = r.gw.heat().cells[i].column;
                column.material.grain_conductivity = 2e5;
                ASSERT_EQ(column.setMoisture(column.theta), "");
                surface.cell_mass[surface.idx(0, i)] = 6 * r.state.volume[i];
                surface.cell_mass[surface.idx(surface.age_row, i)] = 5 * r.state.volume[i];
                surface.cell_mass[surface.idx(surface.temp_row, i)] = 6 * r.state.volume[i];
            }
            const auto sum = [&](int row) {
                double result = 0;
                for (int i = 0; i < surface.n_cells; ++i)
                    result += surface.cell_mass[surface.idx(row, i)];
                return result;
            };
            const double initial_mass = tr.storage(0) + tr.storage(1) + sum(0) + sum(1),
                         initial_age = tr.storage(tr.age_row) + sum(surface.age_row);
            const double initial_energy = rkPorousEnergy(r) + cp * sum(surface.temp_row),
                         area = w.area[0];
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            std::vector<double> actual = {w.hg[0]};
            for (int j = 0; j < 4; ++j)
                actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (2 - w.hg[0]) / 4);
            actual.push_back(r.state.volume[0] / area);
            actual.push_back(surface.cell_mass[surface.idx(surface.temp_row, 0)] / area);
            for (double energy : r.gw.heat().cells[0].column.energy)
                actual.push_back(energy / (area * cp));
            for (int row : {0, 1, tr.age_row}) {
                if (layered) {
                    for (int j = 0; j < 4; ++j)
                        actual.push_back(tr.layer_mass[tr.layerIdx(row, j, 0)] / area);
                    actual.push_back(tr.sat_mass[tr.idx(row, 0)] / area);
                } else {
                    const auto& p = r.gw.heat().cells[0].porewater;
                    for (int j = 0; j < 3; ++j)
                        actual.push_back(p.mass[std::size_t(row) * 3 + j] / area);
                }
                actual.push_back(surface.cell_mass[surface.idx(row, 0)] / area);
            }
            ASSERT_EQ(actual.size(), exact.size());
            double error = 0;
            for (std::size_t j = 0; j < actual.size(); ++j)
                error += std::pow(actual[j] - exact[j], 2);
            error = std::sqrt(error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("finite_reactive_moving_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 10000)),
                           error);
            EXPECT_NEAR(tr.storage(0) + tr.storage(1) + sum(0) + sum(1) + tr.lost_deep[0] +
                            tr.lost_deep[1],
                        initial_mass, 1e-10 * initial_mass);
            EXPECT_NEAR(tr.storage(tr.age_row) + sum(surface.age_row) + tr.lost_deep[tr.age_row] -
                            tr.generated_age[tr.age_row] - surface.generated_age[surface.age_row],
                        initial_age, 1e-10 * initial_age);
            EXPECT_NEAR(rkPorousEnergy(r) +
                            cp * (sum(surface.temp_row) + tr.lost_deep[tr.temp_row]),
                        initial_energy, 1e-10 * std::max(1., std::abs(initial_energy)));
            for (int row : {0, 1, tr.age_row})
                EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
            checkRkPorousLedgers(r);
        }
    }
}
TEST(CoupledGroundwaterRk2, ContactAreaUsesClippedTriangleAndQuadPlanesAndHasNoArtificialWetFloor) {
    auto mesh = makeGridMesh(1, 1, 4, [](double x, double) { return x / 4; });
    SolverOptions2D opts;
    opts.cell_closure = CellClosure2D::VFR;
    EXPECT_NEAR(inertial::cellWetArea(mesh, opts, 0, 8 * .008 / 3), 8 * .04, 1e-12);
    EXPECT_NEAR(inertial::cellWetArea(mesh, opts, 1, 8 * (.2 - 1. / 3 + .512 / 3)), 8 * .36, 1e-12);
    opts.vfr_min_wet_frac = .1;
    EXPECT_DOUBLE_EQ(inertial::cellWetArea(mesh, opts, 0, 0), 0);
    EXPECT_GT(inertial::cellWetArea(mesh, opts, 0, 1e-20), 0);
    EXPECT_LT(inertial::cellWetArea(mesh, opts, 0, 1e-20), .1 * 8);
    mesh.resize_triangles(1);
    mesh.set_quad(0, 0, 1, 3, 2);
    buildMeshTopology(mesh);
    opts.vfr_min_wet_frac = .001;
    EXPECT_NEAR(inertial::cellWetArea(mesh, opts, 0, .32), 3.2, 1e-12);
    EXPECT_DOUBLE_EQ(inertial::cellWetArea(mesh, opts, 0, 0), 0);
    opts.cell_closure = CellClosure2D::FLAT;
    EXPECT_DOUBLE_EQ(inertial::cellWetArea(mesh, opts, 0, .32), 16);
}
TEST(CoupledGroundwaterRk2, FirstOrderFiniteHeatAlsoUsesThePhysicalWettedPlane) {
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.mesh = makeGridMesh(1, 1, 4, [](double x, double) { return x / 4; });
        r.opts.cell_closure = CellClosure2D::VFR;
        r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        RkPorousOwner owner;
        initRkFiniteSurface(r, owner, layered);
        for (int i = 0; i < r.mesh.n_cells(); ++i) {
            const double wet = r.mesh.tri_area[i] * (i == 0 ? .04 : .36);
            const auto& c = r.gw.heat().cells[i].column;
            const double water = 4186000 * r.state.volume[i], soil = c.capacity[0],
                         g = wet * c.conductivity[0], weight = .01 * g * soil / (soil + .01 * g);
            const double expected = (6 * water - 2 * weight) / (water + weight);
            const double result = applyMeshHeatCell(
                r.mesh, r.opts, r.state.heat, i, 6 * r.state.volume[i], r.state.volume[i], 0, .01);
            EXPECT_NEAR(result / r.state.volume[i], expected, 1e-10);
            EXPECT_EQ(r.state.heat.failures[i], 0);
            EXPECT_NEAR(r.state.heat.water_j[i] + r.gw.heat().cells[i].water_contact_j, 0, 1e-7);
        }
        checkRkPorousLedgers(r);
    }
}
TEST(CoupledGroundwaterRk2, FiniteChemicalAgeWetDryEtClosesSeparateWaterMassAndLatentEnergy) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double pond : {0., 1e-5, .01})
                for (double demand : {.0001, 100.})
                    for (const std::string mode : {"SENSIBLE_ONLY", "LATENT_SURFACE"}) {
                        SCOPED_TRACE(int(closure));
                        SCOPED_TRACE(layered);
                        SCOPED_TRACE(pond);
                        SCOPED_TRACE(demand);
                        SCOPED_TRACE(mode);
                        RkReactionSystem system("* SAT A RATE 0\n* UNSAT A RATE 0\n");
                        CoupledRkRig r;
                        r.init(.05, 0, 0, pond, "BOUNDARY_ET", closure, SoilChar::GARDNER, 4);
                        RkPorousOwner owner;
                        initRkFiniteQuality(r, owner, system, layered, 3);
                        owner.config.options.soil_et_heat = mode;
                        r.opts.evaporation = true;
                        r.state.evap_rate.assign(r.mesh.n_cells(), demand);
                        const auto initial = finiteQualityInitial(r);
                        const auto& s = r.state.transport;
                        const auto& t = r.gw.transport();
                        double surface_energy = 0;
                        for (int i = 0; i < s.n_cells; ++i)
                            surface_energy += 4186000 * s.cell_mass[s.idx(s.temp_row, i)];
                        const double initial_energy = rkPorousEnergy(r) + surface_energy,
                                     initial_water = r.total();
                        ASSERT_NO_THROW(r.solver.advance(0, 2));
                        surface_energy = 0;
                        double latent = 0;
                        for (int i = 0; i < s.n_cells; ++i) {
                            surface_energy += 4186000 * s.cell_mass[s.idx(s.temp_row, i)];
                            latent += r.gw.heat().cells[i].latent_j;
                        }
                        EXPECT_NEAR(rkPorousEnergy(r) + surface_energy +
                                        4186000 *
                                            (s.lost_external[s.temp_row] + t.lost_et[t.temp_row]) -
                                        latent,
                                    initial_energy, 1e-10 * std::max(1., std::abs(initial_energy)));
                        EXPECT_NEAR(r.total() + r.gw.state().led_et + r.state.evap_loss_total,
                                    initial_water, 1e-10 * std::max(1., initial_water));
                        EXPECT_DOUBLE_EQ(t.lost_et[0], 0);
                        EXPECT_DOUBLE_EQ(s.lost_external[0], 0);
                        checkFiniteQualityBudgets(r, initial);
                        for (int row : {0, 1, s.age_row})
                            for (int i = 0; i < s.n_cells; ++i)
                                EXPECT_GE(s.cell_mass[s.idx(row, i)], 0);
                    }
        }
}

namespace {
/**
 * @brief Configure the finite-contact rig for controlled sensible/radiative atmospheric heat.
 *
 * Uses one ground layer, conductivity 20000 W/(m K), initial surface/ground 20/-2 C, no evaporation
 * and fixed weather/radiative coefficients. Does not advance time or independently prove physical
 * isolation; flags are applied exactly to the fixture owners. Maintains borrowed owner/rig
 * lifetimes and propagates nested setup assertion failures.
 *
 * @param[in,out] r Water rig whose thermal/contact state is rebuilt.
 * @param[in,out] o Fresh retained porous owner providing mutable weather/context/config.
 * @param[in] sensible Enable surface sensible heat in both options and heat state.
 * @param[in] radiation Enable radiative heat in both options and heat state.
 * @param[in] isolated Zero incoming shared longwave/shortwave and set owner config conduction=false
 *     after base setup.
 */
void initRkAtmosphere(CoupledRkRig& r, RkPorousOwner& o, bool sensible = true,
                      bool radiation = true, bool isolated = false) {
    initRkFiniteSurface(r, o, false, 1, 2e4, 0, 20, -2);
    r.opts.evaporation = 0;
    r.opts.heat_surface = sensible;
    r.opts.heat_radiative = radiation;
    r.state.heat.surface = sensible;
    r.state.heat.radiative = radiation;
    auto& ctx = o.context;
    ctx.climate_state.temperature = 41;
    ctx.climate_state.wind_speed = 3;
    ctx.climate_state.humidity = 60;
    ctx.options.wind_func_coeff_a = 1e-6;
    ctx.options.wind_func_coeff_b = 0;
    ctx.options.pressure_ratio = 1;
    ctx.climate_state.radiation.shared_heat_forcing = true;
    ctx.climate_state.radiation.longwave = isolated ? 0 : 250;
    ctx.heat_state.shortwave_now = isolated ? 0 : 500;
    ctx.heat_state.cloud_now = 0;
    auto& rc = ctx.heat_config.radiative;
    rc.emiss_water = 1;
    rc.emiss_landcover = 0;
    rc.lw_reflection = 0;
    rc.sky_view = 1;
    rc.albedo = 0;
    rc.shade_factor = 0;
    if (isolated)
        o.config.options.conduction = false;
}
/**
 * @brief Assert atmospheric heat partitions and whole finite-contact energy change.
 *
 * Nonfatally compares atmospheric joules to radiative+sensible+latent terms, requires zero
 * latent/evaporated water, then compares total energy change to summed atmospheric input at 1e-10
 * scaled tolerance. Calls porous ledger checks; does not update weather, solve heat or clear
 * counters.
 *
 * @param[in] r Borrowed controlled atmospheric fixture with zero evaporation and temperature-only
 *     surface layout.
 * @param[in] initial Initial combined ground/surface energy in joules.
 */
void checkAtmosphericJoules(const CoupledRkRig& r, double initial) {
    double external = 0;
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        const auto& h = r.state.heat;
        external += h.atmospheric_j[i];
        EXPECT_NEAR(h.atmospheric_j[i], h.radiative_j[i] + h.sensible_j[i] + h.latent_j[i],
                    1e-10 * std::max(1., std::abs(h.atmospheric_j[i])));
        EXPECT_DOUBLE_EQ(h.latent_j[i], 0);
        EXPECT_DOUBLE_EQ(h.evaporated_m3[i], 0);
    }
    EXPECT_NEAR(rkFiniteEnergy(r) - initial, external,
                1e-10 * std::max({1., std::abs(initial), std::abs(external)}));
    checkRkPorousLedgers(r);
}
/**
 * @brief Integrate a two-temperature atmospheric/contact reference with fixed weather.
 *
 * Uses local Stefan-Boltzmann/sensible formulas and net incoming radiation 750 W/m2 with air at 5
 * C. No production heat calls or evaporation occur. Caller chooses resolved physically meaningful
 * capacities/conductance; no finite-value/stability validation or adaptive integration is provided.
 *
 * @param[in] water Positive surface-water thermal capacity in J/K.
 * @param[in] soil Positive ground thermal capacity in J/K.
 * @param[in] g Water-ground thermal conductance in W/K.
 * @param[in] area Atmospheric surface exchange area in m2.
 * @param[in] steps Positive classical-RK4 subdivision count over two seconds.
 * @return Array of final water and soil temperatures in degrees C, from 20/-2 C initial state.
 */
std::array<double, 2> atmosphereReference(double water, double soil, double g, double area,
                                          int steps) {
    std::array<double, 2> y{20, -2};
    const double dt = 2. / steps;
    const auto rate = [&](const std::array<double, 2>& t) {
        const double k = t[0] + 273.15;
        const double radiation = 5.67e-8 * k * k * k * k - 750;
        const double sensible = 1000 * 1000 * (2499 - 2.36 * t[0]) * 1e-6 * .061 * (t[0] - 5);
        return std::array<double, 2>{(-g * (t[0] - t[1]) - area * (radiation + sensible)) / water,
                                     g * (t[0] - t[1]) / soil};
    };
    for (int j = 0; j < steps; ++j) {
        auto a = rate(y), trial = y;
        for (int k = 0; k < 2; ++k)
            trial[k] = y[k] + dt * .5 * a[k];
        auto b = rate(trial);
        for (int k = 0; k < 2; ++k)
            trial[k] = y[k] + dt * .5 * b[k];
        auto c = rate(trial);
        for (int k = 0; k < 2; ++k)
            trial[k] = y[k] + dt * c[k];
        auto d = rate(trial);
        for (int k = 0; k < 2; ++k)
            y[k] += dt * (a[k] + 2 * b[k] + 2 * c[k] + d[k]) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2, AtmosphereRadiationMatchesIndependentFourthPowerCooling) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig r;
        r.init(dt, 0, 0, 1e-5, "NONE");
        RkPorousOwner o;
        initRkAtmosphere(r, o, false, true, true);
        const double initial = rkFiniteEnergy(r), water = 4186000 * r.state.volume[0],
                     area = r.mesh.tri_area[0];
        const double kelvin = 293.15 /
                              std::cbrt(1 + 3 * 5.67e-8 * area / water * std::pow(293.15, 3) * 2),
                     exact = kelvin - 273.15;
        for (int j = 0; j < int(std::round(2 / dt)); ++j)
            ASSERT_NO_THROW(r.solver.advance(j * dt, (j + 1) * dt));
        const double error =
            std::abs(r.state.transport.cell_mass[0] / r.state.volume[0] - exact) / 300;
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        RecordProperty("atmosphere_radiation_" + std::to_string(int(dt * 1000)), error);
        checkAtmosphericJoules(r, initial);
    }
}
TEST(CoupledGroundwaterRk2, AtmosphereJointNonlinearFiniteGraphHasSecondOrderEnergy) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig r;
        r.init(dt, 0, 0, .02, "NONE");
        RkPorousOwner o;
        initRkAtmosphere(r, o);
        const auto& col = r.gw.heat().cells[0].column;
        const double area = col.area, soil = col.capacity[0], water = 4186000 * r.state.volume[0],
                     g = area * col.conductivity[0];
        const auto exact = atmosphereReference(water, soil, g, area, 20000),
                   refined = atmosphereReference(water, soil, g, area, 40000);
        for (int k = 0; k < 2; ++k)
            ASSERT_NEAR(exact[k], refined[k], 1e-10);
        const double initial = rkFiniteEnergy(r);
        for (int j = 0; j < int(std::round(2 / dt)); ++j)
            ASSERT_NO_THROW(r.solver.advance(j * dt, (j + 1) * dt));
        const double error =
            std::hypot(r.state.transport.cell_mass[0] / r.state.volume[0] - exact[0],
                       col.energy[0] / soil - exact[1]) /
            20;
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        RecordProperty("atmosphere_joint_" + std::to_string(int(dt * 1000)), error);
        checkAtmosphericJoules(r, initial);
        EXPECT_LT(r.state.heat.sensible_j[0], 0);
        EXPECT_GT(r.state.heat.radiative_j[0], 0);
    }
}
TEST(CoupledGroundwaterRk2, AtmosphereLocalStepDoublingHasThirdOrderDifference) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig a, b;
        a.init(dt, 0, 0, .02, "NONE");
        b.init(dt / 2, 0, 0, .02, "NONE");
        RkPorousOwner oa, ob;
        initRkAtmosphere(a, oa);
        initRkAtmosphere(b, ob);
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, dt / 2));
        ASSERT_NO_THROW(b.solver.advance(dt / 2, dt));
        const double error =
            std::abs(a.state.transport.cell_mass[0] - b.state.transport.cell_mass[0]);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 7);
        previous = error;
        RecordProperty("atmosphere_local_" + std::to_string(int(dt * 1000)), error);
    }
}
TEST(CoupledGroundwaterRk2, AtmosphereCheckpointBindsHeldWeatherAndContinuesExactly) {
    CoupledRkRig a, b;
    a.init(.1, 0, 0, .02, "NONE");
    b.init(.1, 0, 0, .02, "NONE");
    RkPorousOwner oa, ob;
    initRkAtmosphere(a, oa);
    initRkAtmosphere(b, ob);
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("atmosphere-source.chk"),
               before = rk2Output("atmosphere-before.chk"),
               after = rk2Output("atmosphere-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    for (double* value :
         {&ob.context.climate_state.temperature, &ob.context.climate_state.humidity,
          &ob.context.climate_state.wind_speed, &ob.context.heat_state.shortwave_now,
          &ob.context.heat_state.cloud_now, &ob.context.climate_state.radiation.longwave,
          &ob.context.options.wind_func_coeff_a, &ob.context.options.wind_func_coeff_b,
          &ob.context.options.pressure_ratio}) {
        const double old = *value;
        *value += .01;
        ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        *value = old;
    }
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.6, 2));
    ASSERT_NO_THROW(b.solver.advance(.6, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2, AtmosphereInvalidLaterCellAndWorkLimitRestoreAllOwners) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .02, "NONE");
    RkPorousOwner o;
    initRkAtmosphere(r, o);
    const auto before = rk2Output("atmosphere-failure-before.chk"),
               after = rk2Output("atmosphere-failure-after.chk");
    const double old = r.state.transport.cell_mass[1];
    r.state.transport.cell_mass[1] = -300 * r.state.volume[1];
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.state.transport.cell_mass[1] = old;
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    o.context.heat_state.shortwave_now = std::numeric_limits<double>::quiet_NaN();
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    o.context.heat_state.shortwave_now = 500;
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    o.context.options.wind_func_coeff_a = 1e5;
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_NO_THROW(r.solver.advance(0, 1e-8));
    checkRkPorousLedgers(r);
}

TEST(CoupledGroundwaterRk2, AtmosphereUsesPhysicalVfrFootprintAndDoesNotHeatDryResidue) {
    CoupledRkRig r;
    r.mesh = makeGridMesh(1, 1, 4, [](double x, double) { return x / 4; });
    r.opts.cell_closure = CellClosure2D::VFR;
    r.init(.1, 0, 0, .2, "NONE");
    RkPorousOwner o;
    initRkAtmosphere(r, o, false, true, true);
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        const double wet = r.mesh.tri_area[i] * (i == 0 ? .04 : .36), volume = r.state.volume[i],
                     water = 4186000 * volume;
        const double exact =
            293.15 / std::cbrt(1 + 3 * 5.67e-8 * wet / water * std::pow(293.15, 3)) - 273.15;
        const double result =
            applyMeshHeatCell(r.mesh, r.opts, r.state.heat, i, 20 * volume, volume, 0, 1, true);
        EXPECT_EQ(r.state.heat.failures[i], 0);
        EXPECT_NEAR(result / volume, exact, 1e-8);
        EXPECT_NEAR(4186000 * (result - 20 * volume), r.state.heat.atmospheric_j[i], 1e-8);
    }
    const double before = rkPorousEnergy(r);
    const auto receipts = r.state.heat.atmospheric_j;
    for (int i = 0; i < r.mesh.n_cells(); ++i)
        EXPECT_DOUBLE_EQ(applyMeshHeatCell(r.mesh, r.opts, r.state.heat, i, 17, 0, 0, 1, true), 17);
    EXPECT_DOUBLE_EQ(rkPorousEnergy(r), before);
    EXPECT_EQ(r.state.heat.atmospheric_j, receipts);
}
TEST(CoupledGroundwaterRk2, SurfaceLatentNativeDemandSharesSensibleAtmosphereAndCanBeDisabled) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .02, "NONE");
    RkPorousOwner o;
    initRkAtmosphere(r, o);
    r.opts.evaporation = 1;
    r.state.evap_rate.assign(r.mesh.n_cells(), .001);
    const double before = rkFiniteEnergy(r);
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    const auto& h = r.state.heat;
    double atmosphere = 0, latent = 0;
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        atmosphere += h.atmospheric_j[i];
        latent += h.latent_j[i];
        EXPECT_NEAR(h.atmospheric_j[i], h.radiative_j[i] + h.sensible_j[i] + h.latent_j[i],
                    1e-10 * std::max(1., std::abs(h.atmospheric_j[i])));
    }
    EXPECT_LT(latent, 0);
    EXPECT_NEAR(rkFiniteEnergy(r) - before,
                atmosphere - r.state.transport.lost_external[0] * 4186000,
                1e-10 * std::max(1., std::abs(before)));
    r.opts.evaporation = 0;
    const double water = r.total();
    const auto previous_latent = h.latent_j;
    ASSERT_NO_THROW(r.solver.advance(.1, .2));
    EXPECT_NEAR(r.total(), water, 1e-12);
    EXPECT_EQ(h.latent_j, previous_latent);
    checkRkPorousLedgers(r);
}
TEST(CoupledGroundwaterRk2, AtmosphereSharesReactiveChemicalAgeAndFiniteContactBudgets) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            RkReactionSystem rx(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
            CoupledRkRig r;
            r.init(.05, 0, 0, .02, "NONE", closure, SoilChar::GARDNER, 4);
            RkPorousOwner o;
            initRkFiniteQuality(r, o, rx, layered, 3);
            auto& ctx = rx.context();
            r.opts.evaporation = 0;
            r.opts.heat_surface = r.opts.heat_radiative = 1;
            r.state.heat.surface = r.state.heat.radiative = true;
            ctx.climate_state.temperature = 41;
            ctx.climate_state.humidity = 60;
            ctx.climate_state.wind_speed = 3;
            ctx.options.wind_func_coeff_a = 1e-6;
            ctx.options.wind_func_coeff_b = 0;
            ctx.options.pressure_ratio = 1;
            ctx.heat_state.shortwave_now = 500;
            ctx.heat_state.cloud_now = 0;
            ctx.climate_state.radiation.shared_heat_forcing = true;
            ctx.climate_state.radiation.longwave = 250;
            const auto energy = [&]() {
                double result = rkPorousEnergy(r);
                const auto& tr = r.state.transport;
                for (int i = 0; i < tr.n_cells; ++i)
                    result += 4186000 * tr.cell_mass[tr.idx(tr.temp_row, i)];
                return result;
            };
            const auto initial = finiteQualityInitial(r);
            const double before = energy();
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            const auto& h = r.state.heat;
            const double external =
                std::accumulate(h.atmospheric_j.begin(), h.atmospheric_j.end(), 0.);
            EXPECT_NE(external, 0);
            EXPECT_NEAR(energy() - before, external, 1e-10 * std::max(1., std::abs(before)));
            checkFiniteQualityBudgets(r, initial);
            for (int i = 0; i < r.mesh.n_cells(); ++i)
                EXPECT_NEAR(h.atmospheric_j[i], h.radiative_j[i] + h.sensible_j[i],
                            1e-10 * std::max(1., std::abs(h.atmospheric_j[i])));
            EXPECT_GT(r.gw.transport().lost_reaction[0], 0);
            EXPECT_GT(r.solver.surfaceReactionLoss(0), 0);
        }
}

namespace {
/**
 * @brief Replace equal supply/withdrawal runtime sources in every surface cell.
 *
 * Uses owner qualified-test and stable supply/withdraw IDs, species-zero concentration 7, age=3 s
 * where present and zero explicit rates. Copies row vectors into the source table and rebuilds cell
 * lookup; does not route water, validate numeric ranges or clear unrelated sources.
 *
 * @param[in,out] r Rig whose surface runtime-source table and lookup are mutated.
 * @param[in] flow Nonnegative source magnitude in m3/s; supply uses +flow, withdrawal -flow.
 * @param[in] until Elapsed expiry seconds, default infinity.
 */
void setRkRuntimePair(CoupledRkRig& r, double flow = .2,
                      double until = std::numeric_limits<double>::infinity()) {
    auto& rows = r.state.runtime_sources;
    for (int cell = 0; cell < r.mesh.n_cells(); ++cell)
        for (bool sink : {false, true}) {
            RuntimeSource row;
            row.id = sink ? "withdraw" : "supply";
            row.owner = "qualified-test";
            row.cell = cell;
            row.flow = sink ? -flow : flow;
            row.until = until;
            row.concentrations.assign(r.state.transport.n_species, 0);
            if (!row.concentrations.empty())
                row.concentrations[0] = 7;
            if (r.state.transport.age_row >= 0)
                row.concentrations[r.state.transport.age_row] = 3;
            row.rates.assign(r.state.transport.n_species, 0);
            rows.replace(std::move(row));
        }
    rows.rebuild(r.mesh.n_cells());
}
}
TEST(CoupledGroundwaterRk2, RuntimeSourcePairHasSecondOrderMixingAndSeparateAgeProduction) {
    for (bool layered : {false, true}) {
        double previous = 0;
        for (double dt : {.4, .2, .1, .05}) {
            CoupledRkRig r;
            r.init(dt, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkQuality(r, layered, true, true);
            seedRkQuality(r, 2, .001);
            auto& tr = r.state.transport;
            for (int i = 0; i < tr.n_cells; ++i)
                tr.cell_mass[tr.idx(tr.age_row, i)] = 5 * r.state.volume[i];
            setRkRuntimePair(r);
            const double v = r.state.volume[0], initial = r.total();
            const double exact = 7 + (2 - 7) * std::exp(-.2 / v * 2),
                         age = 3 + v / .2 + (5 - 3 - v / .2) * std::exp(-.2 / v * 2);
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            const double error = std::max(std::abs(tr.cell_mass[0] / v - exact),
                                          std::abs(tr.cell_mass[tr.idx(tr.age_row, 0)] / v - age)) /
                                 10;
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("runtime_pair_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 1000)),
                           error);
            EXPECT_NEAR(r.total(), initial, 1e-10 * initial);
            for (int i = 0; i < tr.n_cells; ++i) {
                const auto& in = r.state.runtime_sources.find("supply", i)->receipt;
                const auto& out = r.state.runtime_sources.find("withdraw", i)->receipt;
                EXPECT_NEAR(in.water_in, .4, 1e-13);
                EXPECT_NEAR(out.water_out, .4, 1e-13);
                EXPECT_NEAR(in.applied_mass[0], 2.8, 1e-12);
                EXPECT_NEAR(tr.cell_mass[tr.idx(0, i)] - 2 * v,
                            in.applied_mass[0] + out.applied_mass[0], 1e-12);
                EXPECT_NEAR(tr.cell_mass[tr.idx(tr.age_row, i)] - 5 * v,
                            in.applied_mass[tr.age_row] + out.applied_mass[tr.age_row] + 2 * v,
                            1e-11);
            }
        }
    }
}
TEST(CoupledGroundwaterRk2, RuntimeSourceExpiryCheckpointContinuesAndBindsPrescription) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkRuntimePair(*r, .2, .735);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("runtime-rk2-source.chk"),
               before = rk2Output("runtime-rk2-before.chk"),
               after = rk2Output("runtime-rk2-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    b.state.runtime_sources.rows[0].flow += .01;
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.state.runtime_sources.rows[0].flow -= .01;
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.6, 2));
    ASSERT_NO_THROW(b.solver.advance(.6, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    for (const auto& row : a.state.runtime_sources.rows) {
        EXPECT_FALSE(row.enabled);
        EXPECT_NEAR(row.receipt.water_in + row.receipt.water_out, .2 * .735, 1e-12);
    }
}
TEST(CoupledGroundwaterRk2, RuntimeSourceLaterOverflowRestoresAllOwnersAndCanRetry) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkQuality(r, true, true, true);
    seedRkQuality(r, 2, .001);
    setRkRuntimePair(r);
    r.state.runtime_sources.find("supply", 1)->flow = 1e308;
    const auto before = rk2Output("runtime-overflow-before.chk"),
               after = rk2Output("runtime-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 10, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.state.runtime_sources.find("supply", 1)->flow = .2;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}
TEST(CoupledGroundwaterRk2, RuntimeSourceDryStartUsesBeginningDonorsAndClosesGrossReceipts) {
    CoupledRkRig r;
    r.init(.2, 0, 0, 0, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkQuality(r, true, true, true);
    seedRkQuality(r, 2, .001);
    for (int i = 0; i < r.mesh.n_cells(); ++i)
        r.state.transport.cell_mass[r.state.transport.idx(0, i)] = 1;
    setRkRuntimePair(r, 1);
    for (auto& row : r.state.runtime_sources.rows)
        if (row.flow < 0)
            row.flow = -100;
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, .2, 0));
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        const auto& in = r.state.runtime_sources.find("supply", i)->receipt;
        const auto& out = r.state.runtime_sources.find("withdraw", i)->receipt;
        EXPECT_NEAR(r.state.volume[i], .075, 1e-13);
        EXPECT_NEAR(in.water_in, .2, 1e-13);
        EXPECT_NEAR(out.water_out, .125, 1e-13);
        EXPECT_NEAR(r.state.transport.cell_mass[r.state.transport.idx(0, i)], .775, 1e-12);
        EXPECT_NEAR(1 + in.applied_mass[0] + out.applied_mass[0], .775, 1e-12);
        EXPECT_LT(out.requested, out.applied);
        EXPECT_NEAR(out.last_dt, .2, 1e-15);
        EXPECT_NEAR(out.last_flow, -.125 / .2, 1e-12);
    }
}
TEST(CoupledGroundwaterRk2, RuntimeSourcesKeepColdWaterAndSignedHeatPowerWithSeparateJoules) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig r;
        r.init(dt, 0, 0, .2, "NONE");
        RkPorousOwner o;
        o.initialize(r, false);
        setRkRuntimePair(r);
        for (auto& row : r.state.runtime_sources.rows) {
            row.concentrations[0] = -5;
            if (row.flow > 0)
                row.heat = -1000;
        }
        const double v = r.state.volume[0], equilibrium = -5 - 1000 / (4186000 * .2),
                     exact = equilibrium + (-2 - equilibrium) * std::exp(-.2 / v * 2);
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        const double error = std::abs(r.state.transport.cell_mass[0] / v - exact);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        RecordProperty("runtime_cold_" + std::to_string(int(dt * 1000)), error);
        for (int i = 0; i < r.mesh.n_cells(); ++i) {
            const auto& in = r.state.runtime_sources.find("supply", i)->receipt;
            const auto& out = r.state.runtime_sources.find("withdraw", i)->receipt;
            EXPECT_NEAR(in.heat_requested, -2000, 1e-10);
            EXPECT_NEAR(in.heat_applied, -2000, 1e-10);
            EXPECT_NEAR(r.state.transport.cell_mass[i] + 2 * v,
                        in.applied_mass[0] + out.applied_mass[0], 1e-12);
        }
        checkRkPorousLedgers(r);
    }
}
TEST(CoupledGroundwaterRk2, RuntimeSourceOrderAndCompetingChemicalSinksRetainCanonicalBudgets) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkRuntimePair(*r);
        for (auto& row : r->state.runtime_sources.rows)
            row.rates[0] = -100;
    }
    std::reverse(b.state.runtime_sources.rows.begin(), b.state.runtime_sources.rows.end());
    b.state.runtime_sources.index.clear();
    for (std::size_t k = 0; k < b.state.runtime_sources.rows.size(); ++k) {
        const auto& row = b.state.runtime_sources.rows[k];
        b.state.runtime_sources.index[row.id][row.cell] = k;
    }
    b.state.runtime_sources.rebuild(b.mesh.n_cells());
    ASSERT_NO_THROW(a.solver.advance(0, 2));
    ASSERT_NO_THROW(b.solver.advance(0, 2));
    for (int i = 0; i < a.mesh.n_cells(); ++i) {
        double delta = 0;
        for (const std::string name : {"supply", "withdraw"}) {
            const auto& ar = a.state.runtime_sources.find(name, i)->receipt;
            const auto& br = b.state.runtime_sources.find(name, i)->receipt;
            EXPECT_NEAR(ar.water_in, br.water_in, 1e-13);
            EXPECT_NEAR(ar.water_out, br.water_out, 1e-13);
            delta += ar.applied_mass[0];
        }
        EXPECT_GE(a.state.transport.cell_mass[i], 0);
        EXPECT_NEAR(a.state.transport.cell_mass[i], b.state.transport.cell_mass[i], 1e-12);
        EXPECT_NEAR(a.state.transport.cell_mass[i] - 3.2, delta, 1e-11);
    }
}
TEST(CoupledGroundwaterRk2, RuntimeSourcesShareFiniteReactiveAgeAndPhysicalEnergyBudgets) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            RkReactionSystem rx(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
            CoupledRkRig r;
            r.init(.05, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
            RkPorousOwner o;
            initRkFiniteQuality(r, o, rx, layered, 3);
            setRkRuntimePair(r, .01);
            for (auto& row : r.state.runtime_sources.rows) {
                row.concentrations[r.state.transport.temp_row] = -8;
                if (row.flow > 0)
                    row.heat = -100;
            }
            const auto energy = [&]() {
                double result = rkPorousEnergy(r);
                const auto& tr = r.state.transport;
                for (int i = 0; i < tr.n_cells; ++i)
                    result += 4186000 * tr.cell_mass[tr.idx(tr.temp_row, i)];
                return result;
            };
            auto initial = finiteQualityInitial(r);
            const double before = energy();
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            const auto& tr = r.state.transport;
            for (int row = 0; row < tr.n_species; ++row)
                initial[row] += tr.gained_external[row];
            checkFiniteQualityBudgets(r, initial);
            double provider_energy = 0;
            for (const auto& source : r.state.runtime_sources.rows)
                provider_energy += 4186000 * source.receipt.applied_mass[tr.temp_row];
            EXPECT_NEAR(energy() - before, provider_energy, 1e-10 * std::max(1., std::abs(before)));
            for (int row = 0; row < tr.n_species; ++row) {
                double accepted = 0;
                for (const auto& source : r.state.runtime_sources.rows)
                    accepted += source.receipt.applied_mass[row];
                if (row == tr.temp_row)
                    accepted += std::accumulate(r.state.heat.water_j.begin(),
                                                r.state.heat.water_j.end(), 0.) /
                                4186000;
                EXPECT_NEAR(accepted, tr.gained_external[row] - tr.lost_external[row],
                            1e-10 * std::max(1., std::abs(accepted)));
            }
        }
}
TEST(CoupledGroundwaterRk2, RuntimeSourceCorruptGrossReceiptRefusesBeforeAnyOwnerIsInstalled) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkRuntimePair(*r, .2, .735);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("runtime-corrupt-source.chk"),
               bad = rk2Output("runtime-corrupt-gross.chk"),
               before = rk2Output("runtime-corrupt-before.chk"),
               after = rk2Output("runtime-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    openswmm::detail::CompletedSourceArchive original(rk2File(saved));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const std::string marker = "RK2 copied surface runtime sources v1";
    const auto pos = payload.rfind(marker);
    ASSERT_NE(pos, std::string::npos);
    // The mutable record is profile, fixed row count, enabled, then requested
    // and applied water. Change a physical receipt, retaining a valid CRC.
    const auto offset = pos + marker.size() + sizeof(uint64_t) + sizeof(bool) + sizeof(double);
    ASSERT_LT(offset + sizeof(double), payload.size());
    double value = 0;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    value += 1;
    std::memcpy(payload.data() + offset, &value, sizeof(value));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    const auto error = b.solver.loadCheckpoint(bad.string());
    EXPECT_NE(error.find("net and gross"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
}

namespace {
/**
 * @brief Install a uniform runtime rain/quality override for the RK2 fixture.
 *
 * Sets rainfall channel one/all cells/replace mode at 0.01 m/s, owner rain-test, row-zero
 * concentration 7 and age=3 s where present. Initializes receipt mass slots and rebuilds lookup.
 * Does not advance rainfall, validate layout/ranges or clear unrelated forcing owners.
 *
 * @param[in,out] r Rig whose runtime forcing table/lookup are mutated.
 * @param[in] until Elapsed override expiry in seconds, default 0.735.
 */
void setRkRain(CoupledRkRig& r, double until = .735) {
    RuntimeForcing row;
    row.channel = 1;
    row.cell = -1;
    row.mode = 1;
    row.rate = .01;
    row.until = until;
    row.owner = "rain-test";
    row.concentrations.assign(r.state.transport.n_species, 0);
    if (!row.concentrations.empty())
        row.concentrations[0] = 7;
    if (r.state.transport.age_row >= 0)
        row.concentrations[r.state.transport.age_row] = 3;
    row.receipt.requested_mass.assign(row.concentrations.size(), 0);
    row.receipt.applied_mass.assign(row.concentrations.size(), 0);
    r.state.runtime_forcings.replace(std::move(row));
    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
}
}
TEST(CoupledGroundwaterRk2, RuntimeForcingRainAndAgeMatchIndependentExpiryIntegrals) {
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(r, layered, true, true);
        seedRkQuality(r, 2, .001);
        setRkRain(r);
        const auto before = r.state.volume;
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        ASSERT_EQ(r.state.rk2_rain_pending.size(), before.size());
        for (int i = 0; i < r.mesh.n_cells(); ++i) {
            const double rain = .01 * r.mesh.tri_area[i] * .735,
                         age = 2 * before[i] +
                               .01 * r.mesh.tri_area[i] * (2 * .735 - .5 * .735 * .735);
            EXPECT_NEAR(r.state.volume[i], before[i] + rain, 1e-12);
            EXPECT_NEAR(r.state.rk2_rain_pending[i], rain, 1e-12);
            EXPECT_NEAR(r.state.transport.cell_mass[r.state.transport.idx(0, i)],
                        2 * before[i] + 7 * rain, 1e-12);
            EXPECT_NEAR(r.state.transport.cell_mass[r.state.transport.idx(1, i)],
                        5 * before[i] + 3 * rain + age, 1e-11);
        }
        EXPECT_FALSE(r.state.runtime_forcings.active());
        EXPECT_NEAR(r.state.transport.generated_age[1],
                    2 * std::accumulate(before.begin(), before.end(), 0.) +
                        .01 * 16 * (2 * .735 - .5 * .735 * .735),
                    1e-11);
    }
}
TEST(CoupledGroundwaterRk2,
     RuntimeForcingCheckpointRestoresPendingRainAndExpiredPrescriptionExactly) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkRain(*r);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("forcing-source.chk"), before = rk2Output("forcing-before.chk"),
               after = rk2Output("forcing-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    b.state.runtime_forcings.rows[0].rate += .01;
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.state.runtime_forcings.rows[0].rate -= .01;
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.6, 2));
    ASSERT_NO_THROW(b.solver.advance(.6, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    EXPECT_EQ(a.state.rk2_rain_pending, b.state.rk2_rain_pending);
    EXPECT_FALSE(b.state.runtime_forcings.active());
}
TEST(CoupledGroundwaterRk2, RuntimeForcingLaterNumericFailureRestoresPendingRainAndAllOwners) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkQuality(r, true, true, true);
    seedRkQuality(r, 2, .001);
    setRkRain(r);
    RuntimeForcing row = r.state.runtime_forcings.rows[0];
    row.cell = 1;
    row.rate = 1e308;
    r.state.runtime_forcings.replace(row);
    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
    const auto before = rk2Output("forcing-failure-before.chk"),
               after = rk2Output("forcing-failure-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 10, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.state.runtime_forcings.find(1, 1)->rate = .01;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}
TEST(CoupledGroundwaterRk2, SurfaceLatentCopiedDemandExpiryPreservesAcceptedPhysicalEnergy) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .02, "NONE");
    RkPorousOwner o;
    initRkAtmosphere(r, o);
    RuntimeForcing row;
    row.channel = 2;
    row.cell = -1;
    row.rate = .01;
    row.until = .0735;
    row.receipt.requested_mass.assign(1, 0);
    row.receipt.applied_mass.assign(1, 0);
    r.state.runtime_forcings.replace(row);
    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
    const double before = rkFiniteEnergy(r);
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    const auto& h = r.state.heat;
    double atmosphere = 0, evaporated = 0;
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        atmosphere += h.atmospheric_j[i];
        evaporated += h.evaporated_m3[i];
        EXPECT_LT(h.latent_j[i], 0);
    }
    EXPECT_NEAR(evaporated, .01 * .0735 * 16, 1e-12);
    EXPECT_FALSE(r.state.runtime_forcings.rows[0].enabled);
    EXPECT_NEAR(rkFiniteEnergy(r) - before,
                atmosphere - r.state.transport.lost_external[0] * 4186000,
                1e-10 * std::max(1., std::abs(before)));
    const auto previous = h.latent_j;
    ASSERT_NO_THROW(r.solver.advance(.1, .2));
    EXPECT_EQ(h.latent_j, previous);
    checkRkPorousLedgers(r);
}

TEST(CoupledGroundwaterRk2,
     RuntimeForcingRainMovingColumnMatchesIndependentReactiveWaterHeatAgeOde) {
    constexpr double partition = .001 * 2650 * (1 - .45), cp = 4186000;
    for (bool layered : {false, true}) {
        const auto exact = porousColumnReference(20000, true, layered, partition, true, true, .002),
                   refined =
                       porousColumnReference(40000, true, layered, partition, true, true, .002);
        for (std::size_t k = 0; k < exact.size(); ++k)
            ASSERT_NEAR(exact[k], refined[k], 1e-10);
        RkReactionSystem system(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            RkPorousOwner owner;
            initRkFiniteQuality(r, owner, system, layered, 3);
            owner.config.options.conduction = true;
            auto& surface = r.state.transport;
            const auto& w = r.gw.state();
            const auto& tr = r.gw.transport();
            owner.config.params[0].lambda_s = 2e5;
            for (int i = 0; i < w.n_cells; ++i) {
                auto& column = r.gw.heat().cells[i].column;
                column.material.grain_conductivity = 2e5;
                ASSERT_EQ(column.setMoisture(column.theta), "");
                surface.cell_mass[surface.idx(0, i)] = 6 * r.state.volume[i];
                surface.cell_mass[surface.idx(surface.age_row, i)] = 5 * r.state.volume[i];
                surface.cell_mass[surface.idx(surface.temp_row, i)] = 6 * r.state.volume[i];
            }
            setRkRain(r, std::numeric_limits<double>::infinity());
            auto& rain = r.state.runtime_forcings.rows[0];
            rain.rate = .002;
            rain.concentrations[surface.temp_row] = -8;
            const auto sum = [&](int row) {
                double result = 0;
                for (int i = 0; i < surface.n_cells; ++i)
                    result += surface.cell_mass[surface.idx(row, i)];
                return result;
            };
            const double initial_mass = tr.storage(0) + tr.storage(1) + sum(0) + sum(1),
                         initial_age = tr.storage(tr.age_row) + sum(surface.age_row);
            const double initial_energy = rkPorousEnergy(r) + cp * sum(surface.temp_row),
                         area = w.area[0];
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            std::vector<double> actual = {w.hg[0]};
            for (int j = 0; j < 4; ++j)
                actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (2 - w.hg[0]) / 4);
            actual.push_back(r.state.volume[0] / area);
            actual.push_back(surface.cell_mass[surface.idx(surface.temp_row, 0)] / area);
            for (double energy : r.gw.heat().cells[0].column.energy)
                actual.push_back(energy / (area * cp));
            for (int row : {0, 1, tr.age_row}) {
                if (layered) {
                    for (int j = 0; j < 4; ++j)
                        actual.push_back(tr.layer_mass[tr.layerIdx(row, j, 0)] / area);
                    actual.push_back(tr.sat_mass[tr.idx(row, 0)] / area);
                } else {
                    const auto& p = r.gw.heat().cells[0].porewater;
                    for (int j = 0; j < 3; ++j)
                        actual.push_back(p.mass[std::size_t(row) * 3 + j] / area);
                }
                actual.push_back(surface.cell_mass[surface.idx(row, 0)] / area);
            }
            ASSERT_EQ(actual.size(), exact.size());
            double error = 0;
            for (std::size_t j = 0; j < actual.size(); ++j)
                error += std::pow(actual[j] - exact[j], 2);
            error = std::sqrt(error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("forcing_reactive_moving_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 10000)),
                           error);
            EXPECT_NEAR(tr.storage(0) + tr.storage(1) + sum(0) + sum(1) + tr.lost_deep[0] +
                            tr.lost_deep[1],
                        initial_mass + surface.gained_rainfall[0] + surface.gained_rainfall[1],
                        1e-10 * initial_mass);
            EXPECT_NEAR(tr.storage(tr.age_row) + sum(surface.age_row) + tr.lost_deep[tr.age_row] -
                            tr.generated_age[tr.age_row] - surface.generated_age[surface.age_row],
                        initial_age + surface.gained_rainfall[surface.age_row],
                        1e-10 * initial_age);
            EXPECT_NEAR(rkPorousEnergy(r) +
                            cp * (sum(surface.temp_row) + tr.lost_deep[tr.temp_row]),
                        initial_energy + cp * surface.gained_rainfall[surface.temp_row],
                        1e-10 * std::max(1., std::abs(initial_energy)));
            for (int row : {0, 1, tr.age_row})
                EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
            checkRkPorousLedgers(r);
        }
    }
}
TEST(CoupledGroundwaterRk2, RuntimeForcingAddQualityPrecedenceAndFallbackHaveIndependentIntegrals) {
    CoupledRkRig r;
    r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkQuality(r, true, true, true);
    seedRkQuality(r, 2, .001);
    r.state.rainfall.assign(r.mesh.n_cells(), .001);
    setRkRain(r);
    r.state.runtime_forcings.rows[0].mode = 2;
    auto row = r.state.runtime_forcings.rows[0];
    row.cell = 1;
    row.mode = 1;
    row.rate = .02;
    row.until = .5;
    row.concentrations[0] = 9;
    r.state.runtime_forcings.replace(row);
    row.cell = 0;
    row.channel = 4;
    row.rate = 0;
    row.until = .3;
    row.concentrations[0] = 11;
    r.state.runtime_forcings.replace(row);
    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
    const double before = r.total(), initial_mass = 6.4;
    ASSERT_NO_THROW(r.solver.advance(0, 2));
    const double rain0 = 8 * (.001 * 2 + .01 * .735),
                 rain1 = 8 * (.02 * .5 + .011 * (.735 - .5) + .001 * (2 - .735));
    EXPECT_NEAR(r.state.rk2_rain_pending[0], rain0, 1e-12);
    EXPECT_NEAR(r.state.rk2_rain_pending[1], rain1, 1e-12);
    EXPECT_NEAR(r.total() - before, rain0 + rain1, 1e-11);
    const double mass =
        8 * (.011 * .3 * 11 + .011 * (.735 - .3) * 7 + .02 * .5 * 9 + .011 * (.735 - .5) * 7);
    EXPECT_NEAR(r.state.transport.gained_rainfall[0], mass, 1e-12);
    EXPECT_NEAR(r.state.transport.cell_mass[0] + r.state.transport.cell_mass[1],
                initial_mass + mass, 1e-11);
    EXPECT_FALSE(r.state.runtime_forcings.active());
}
TEST(CoupledGroundwaterRk2, RuntimeForcingPairedInfiltrationSharesPhysicalHeatAndChemicalReceipts) {
    for (bool layered : {false, true}) {
        RkReactionSystem rx("* SAT A RATE 0\n* UNSAT A RATE 0\n");
        CoupledRkRig r;
        r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        RkPorousOwner o;
        initRkFiniteQuality(r, o, rx, layered, 3);
        const double before = r.total();
        const auto initial = finiteQualityInitial(r);
        RuntimeForcing row;
        row.channel = 3;
        row.cell = 0;
        row.rate = .001;
        row.until = .735;
        row.receipt.requested_mass.assign(r.state.transport.n_species, 0);
        row.receipt.applied_mass.assign(r.state.transport.n_species, 0);
        r.state.runtime_forcings.replace(row);
        r.state.runtime_forcings.rebuild(r.mesh.n_cells());
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        const auto& b = r.state.runtime_forcings.rows[0].receipt;
        EXPECT_NEAR(b.requested, .001 * 8 * .735, 1e-12);
        EXPECT_NEAR(b.applied, b.requested, 1e-12);
        EXPECT_NEAR(b.water_out, b.applied, 1e-12);
        EXPECT_DOUBLE_EQ(b.water_in, 0);
        EXPECT_NEAR(r.total(), before, 1e-10 * before);
        const int temperature = r.state.transport.temp_row;
        EXPECT_NEAR(b.heat_applied, 4186000 * b.applied_mass[temperature], 1e-8);
        EXPECT_NEAR(b.heat_requested, 4186000 * b.requested_mass[temperature], 1e-8);
        for (int q = 0; q < r.state.transport.n_species; ++q)
            EXPECT_NEAR(b.applied_mass[q], r.state.transport.lost_infiltration[q], 1e-11);
        checkFiniteQualityBudgets(r, initial);
        EXPECT_FALSE(r.state.runtime_forcings.active());
    }
}
TEST(CoupledGroundwaterRk2,
     RuntimeForcingWetDryEtRetainsNonvolatileMassAndSeparateSoilLatentEnergy) {
    for (bool layered : {false, true})
        for (double pond : {0., 1e-5, .01})
            for (double demand : {.0001, 100.})
                for (const std::string mode : {"SENSIBLE_ONLY", "LATENT_SURFACE"}) {
                    RkReactionSystem rx("* SAT A RATE 0\n* UNSAT A RATE 0\n");
                    CoupledRkRig r;
                    r.init(.05, 0, 0, pond, "BOUNDARY_ET", GwClosure::SIGMA, SoilChar::GARDNER, 4);
                    RkPorousOwner o;
                    initRkFiniteQuality(r, o, rx, layered, 3);
                    o.config.options.soil_et_heat = mode;
                    RuntimeForcing row;
                    row.channel = 2;
                    row.cell = -1;
                    row.rate = demand;
                    row.until = .735;
                    row.receipt.requested_mass.assign(r.state.transport.n_species, 0);
                    row.receipt.applied_mass.assign(r.state.transport.n_species, 0);
                    r.state.runtime_forcings.replace(row);
                    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
                    const auto initial = finiteQualityInitial(r);
                    const auto energy = [&]() {
                        double result = rkPorousEnergy(r);
                        for (int i = 0; i < r.mesh.n_cells(); ++i)
                            result += 4186000 * r.state.transport.cell_mass[r.state.transport.idx(
                                                    r.state.transport.temp_row, i)];
                        return result;
                    };
                    const double before = energy(), water = r.total();
                    ASSERT_NO_THROW(r.solver.advance(0, 2));
                    const auto& tr = r.state.transport;
                    const auto& gt = r.gw.transport();
                    double latent = 0;
                    for (const auto& cell : r.gw.heat().cells)
                        latent += cell.latent_j;
                    EXPECT_NEAR(energy() +
                                    4186000 *
                                        (tr.lost_external[tr.temp_row] + gt.lost_et[gt.temp_row]) -
                                    latent,
                                before, 1e-10 * std::max(1., std::abs(before)));
                    EXPECT_NEAR(std::accumulate(r.state.heat.evaporated_m3.begin(),
                                                r.state.heat.evaporated_m3.end(), 0.),
                                r.state.evap_loss_total, 1e-12);
                    EXPECT_NEAR(r.total() + r.state.evap_loss_total + r.gw.state().led_et, water,
                                1e-10 * std::max(1., water));
                    EXPECT_DOUBLE_EQ(tr.lost_external[0], 0);
                    EXPECT_DOUBLE_EQ(gt.lost_et[0], 0);
                    checkFiniteQualityBudgets(r, initial);
                }
}
TEST(CoupledGroundwaterRk2, RuntimeForcingDryUncoveredCellReceivesRainAndAgeAtBothStages) {
    CoupledRkRig r;
    r.init(.1, 0, 0, 0, "NONE");
    SubsurfaceConfig cfg;
    cfg.options.authored = true;
    cfg.options.node_auto = false;
    cfg.options.gw_et = "NONE";
    GwAquiferRow aquifer;
    aquifer.scope = 2;
    aquifer.cell = 0;
    aquifer.Ks = 0;
    aquifer.zs = 2;
    aquifer.hg0 = .6;
    aquifer.c_loss = 0;
    cfg.rows = {aquifer};
    std::vector<std::string> warnings;
    ASSERT_EQ(r.gw.initialize(r.mesh, r.solver.inertialEdges(), r.opts, {}, 0, cfg, warnings), "");
    ASSERT_TRUE(r.gw.covers(0));
    ASSERT_FALSE(r.gw.covers(1));
    initRkQuality(r, false, true, true);
    seedRkQuality(r, 2);
    setRkRain(r, std::numeric_limits<double>::infinity());
    auto& row = r.state.runtime_forcings.rows[0];
    row.cell = 1;
    row.rate = 1e-6;
    r.state.runtime_forcings.index.clear();
    r.state.runtime_forcings.index[1][1] = 0;
    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
    ASSERT_NO_THROW(r.solver.advance(0, 2));
    const double water = 8e-6 * 2;
    EXPECT_GT(r.state.volume[1], 0);
    EXPECT_NEAR(r.state.volume[0] + r.state.volume[1], water, 1e-12);
    EXPECT_NEAR(r.state.rk2_rain_pending[1], water, 1e-12);
    EXPECT_NEAR(r.state.transport.cell_mass[r.state.transport.idx(0, 0)] +
                    r.state.transport.cell_mass[r.state.transport.idx(0, 1)],
                7 * water, 1e-12);
    EXPECT_NEAR(r.state.transport.cell_mass[r.state.transport.idx(1, 0)] +
                    r.state.transport.cell_mass[r.state.transport.idx(1, 1)],
                4 * water, 1e-12);
    EXPECT_NEAR(r.state.transport.generated_age[1], water, 1e-12);
}
TEST(CoupledGroundwaterRk2, RuntimeForcingClearRestoresNativeRainWithoutDroppingPendingPrefix) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkQuality(r, true, true, true);
    seedRkQuality(r, 2, .001);
    r.state.rainfall.assign(2, .001);
    setRkRain(r, std::numeric_limits<double>::infinity());
    ASSERT_NO_THROW(r.solver.advance(0, .4));
    r.state.runtime_forcings.rows[0].enabled = false;
    r.state.runtime_forcings.rebuild(2);
    ASSERT_NO_THROW(r.solver.advance(.4, 2));
    for (int i = 0; i < 2; ++i)
        EXPECT_NEAR(r.state.rk2_rain_pending[i], 8 * (.01 * .4 + .001 * 1.6), 1e-12);
    EXPECT_NEAR(r.state.transport.gained_rainfall[0], 16 * .01 * .4 * 7, 1e-12);
}
TEST(CoupledGroundwaterRk2,
     RuntimeForcingRainAndCopiedWithdrawalsHaveSecondGlobalAndThirdLocalOrder) {
    const auto initialize = [](CoupledRkRig& r, double dt) {
        r.init(dt, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(r, true, true, true);
        seedRkQuality(r, 2, .001);
        setRkRuntimePair(r);
        for (auto& row : r.state.runtime_sources.rows)
            if (row.flow > 0)
                row.flow = 0;
        setRkRain(r, std::numeric_limits<double>::infinity());
        r.state.runtime_forcings.rows[0].rate = .2 / 8;
    };
    double previous = 0, previous_local = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig r, a, b;
        initialize(r, dt);
        initialize(a, dt);
        initialize(b, dt / 2);
        const double volume = r.state.volume[0];
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        const double exact = 7 + (2 - 7) * std::exp(-.2 / volume * 2),
                     error = std::abs(r.state.transport.cell_mass[0] / volume - exact) / 10;
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, dt / 2));
        ASSERT_NO_THROW(b.solver.advance(dt / 2, dt));
        const double local =
            std::abs(a.state.transport.cell_mass[0] - b.state.transport.cell_mass[0]);
        EXPECT_GT(local, 0);
        if (previous_local > 0)
            EXPECT_GT(previous_local / local, 7);
        previous_local = local;
        RecordProperty("forcing_mixing_" + std::to_string(int(dt * 1000)), error);
        RecordProperty("forcing_local_" + std::to_string(int(dt * 1000)), local);
        const auto& tr = r.state.transport;
        EXPECT_NEAR(tr.cell_mass[0] + tr.cell_mass[1] + tr.lost_external[0],
                    2 * volume * 2 + tr.gained_rainfall[0], 1e-11);
    }
}
TEST(CoupledGroundwaterRk2, RuntimeForcingCorruptPendingRainRefusesBeforeAnyOwnerIsInstalled) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkRain(*r);
    }
    ASSERT_NO_THROW(a.solver.advance(0, 2));
    const auto saved = rk2Output("forcing-corrupt-source.chk"),
               bad = rk2Output("forcing-corrupt-pending.chk"),
               before = rk2Output("forcing-corrupt-before.chk"),
               after = rk2Output("forcing-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    openswmm::detail::CompletedSourceArchive original(rk2File(saved));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    ASSERT_NE(payload.rfind("RK2 held forcing and accepted rain v2"), std::string::npos);
    const double value = -1;
    std::memcpy(payload.data() + payload.size() - sizeof(value), &value, sizeof(value));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    const auto error = b.solver.loadCheckpoint(bad.string());
    EXPECT_NE(error.find("rainfall receipt"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
}
TEST(CoupledGroundwaterRk2,
     RuntimeForcingCheckpointRefusesHistoricalFiniteHeatEvaporationReceiptPolicy) {
    CoupledRkRig a, b;
    RkPorousOwner oa, ob;
    const auto initialize = [](CoupledRkRig& r, RkPorousOwner& o) {
        r.init(.1, 0, 0, .2, "NONE");
        initRkFiniteSurface(r, o, false);
        r.state.evap_rate.assign(r.mesh.n_cells(), .001);
    };
    initialize(a, oa);
    initialize(b, ob);
    ASSERT_NO_THROW(a.solver.advance(0, .2));
    EXPECT_NEAR(
        std::accumulate(a.state.heat.evaporated_m3.begin(), a.state.heat.evaporated_m3.end(), 0.),
        a.state.evap_loss_total, 1e-12);
    const auto saved = rk2Output("mesh-evap-policy-source.chk"),
               bad = rk2Output("mesh-evap-policy-old.chk"),
               before = rk2Output("mesh-evap-policy-before.chk"),
               after = rk2Output("mesh-evap-policy-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    openswmm::detail::CompletedSourceArchive original(rk2File(saved));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    openswmm::detail::CompletedSourceArchive body(payload);
    std::string model;
    body(model);
    const auto rest = payload.substr(sizeof(uint64_t) + model.size());
    const std::string policy = "RK2 accepted mesh evaporation receipt v1";
    const auto pos = model.find(policy);
    ASSERT_NE(pos, std::string::npos);
    ASSERT_GE(pos, sizeof(uint64_t));
    model.erase(pos - sizeof(uint64_t), sizeof(uint64_t) + policy.size());
    openswmm::detail::CompletedSourceArchive prefix;
    prefix(model);
    payload = prefix.bytes() + rest;
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive historical;
    historical(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << historical.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(bad.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
}
TEST(CoupledGroundwaterRk2,
     RuntimeForcingColdRainAndExhaustingEtRemainWithinPassiveTemperatureBounds) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .01, "NONE");
    RkPorousOwner o;
    o.initialize(r, false);
    for (int i = 0; i < r.mesh.n_cells(); ++i)
        r.state.transport.cell_mass[i] = 6 * r.state.volume[i];
    setRkRain(r, std::numeric_limits<double>::infinity());
    auto& rain = r.state.runtime_forcings.rows[0];
    rain.rate = 1;
    rain.concentrations[0] = -8;
    RuntimeForcing evaporation;
    evaporation.channel = 2;
    evaporation.cell = -1;
    evaporation.rate = 100;
    evaporation.receipt.requested_mass.assign(1, 0);
    evaporation.receipt.applied_mass.assign(1, 0);
    r.state.runtime_forcings.replace(evaporation);
    r.state.runtime_forcings.rebuild(r.mesh.n_cells());
    const double initial = r.total();
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    for (int i = 0; i < r.mesh.n_cells(); ++i) {
        ASSERT_GT(r.state.volume[i], 0);
        const double temperature = r.state.transport.cell_mass[i] / r.state.volume[i];
        EXPECT_GE(temperature, -8 - 1e-12);
        EXPECT_LE(temperature, 6 + 1e-12);
    }
    EXPECT_NEAR(r.total() + r.state.evap_loss_total, initial + 16 * .1, 1e-10 * initial);
}
TEST(CoupledGroundwaterRk2, RuntimeForcingFusedRowsDebitAcceptedFaceWaterBeforeColdRainEt) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .01, "NONE");
    r.state.volume[0] *= 2;
    for (int i = 0; i < 2; ++i)
        inertial::cellEtaDepth(r.mesh, r.opts, i, r.state.volume[i], r.state.head[i],
                               r.state.depth[i]);
    RowLayoutLite rows;
    rows.n_species = 9;
    rows.n_pollut = 8;
    rows.temp_row = 8;
    for (int k = 0; k < 8; ++k)
        rows.names.push_back("Dye" + std::to_string(k));
    rows.names.push_back("__TEMPERATURE__");
    GwTransportData data;
    std::vector<std::string> warnings;
    r.gw.initTransport(rows, &data, {}, warnings);
    auto& tr = r.state.transport;
    tr.resize(9, 2, 0);
    tr.n_pollut = 8;
    tr.temp_row = 8;
    tr.row_names = rows.names;
    r.opts.groundwater = 1;
    seedRkTemperature(r, -2, 6);
    for (int q = 0; q < 8; ++q)
        for (int i = 0; i < 2; ++i)
            tr.cell_mass[tr.idx(q, i)] = 2 * r.state.volume[i];
    r.solver.initialize(r.mesh, r.state, r.opts);
    r.solver.setSubsurface(&r.gw);
    ASSERT_EQ(r.gw.rk2ProfileError(), "");
    setRkRain(r, std::numeric_limits<double>::infinity());
    auto& rain = r.state.runtime_forcings.rows[0];
    rain.rate = 1;
    for (int q = 0; q < 8; ++q)
        rain.concentrations[q] = 7;
    rain.concentrations[8] = -8;
    RuntimeForcing evaporation;
    evaporation.channel = 2;
    evaporation.cell = -1;
    evaporation.rate = 100;
    evaporation.receipt.requested_mass.assign(9, 0);
    evaporation.receipt.applied_mass.assign(9, 0);
    r.state.runtime_forcings.replace(evaporation);
    r.state.runtime_forcings.rebuild(2);
    const double initial = r.total(), initial_surface = r.state.volume[0] + r.state.volume[1];
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    for (int i = 0; i < 2; ++i) {
        ASSERT_GT(r.state.volume[i], 0);
        const double temperature = tr.cell_mass[tr.idx(8, i)] / r.state.volume[i];
        EXPECT_GE(temperature, -8 - 1e-12);
        EXPECT_LE(temperature, 6 + 1e-12);
    }
    EXPECT_NEAR(r.total() + r.state.evap_loss_total, initial + 16 * .1, 1e-10 * initial);
    for (int q = 0; q < 8; ++q) {
        EXPECT_DOUBLE_EQ(tr.lost_external[q], 0);
        EXPECT_NEAR(tr.cell_mass[tr.idx(q, 0)] + tr.cell_mass[tr.idx(q, 1)],
                    2 * initial_surface + 7 * 16 * .1, 1e-10);
    }
}
TEST(CoupledGroundwaterRk2,
     RuntimeForcingWaterOnlyStagesPreserveRainAndCheckpointWithoutSpeciesRows) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .01, "NONE");
        RuntimeForcing row;
        row.cell = -1;
        row.channel = 1;
        row.rate = 1;
        r->state.runtime_forcings.replace(row);
        row.channel = 2;
        row.rate = 100;
        r->state.runtime_forcings.replace(row);
        r->state.runtime_forcings.rebuild(2);
    }
    const double initial = a.total();
    ASSERT_NO_THROW(a.solver.advance(0, .1));
    EXPECT_NEAR(a.total() + a.state.evap_loss_total, initial + 1.6, 1e-10 * initial);
    EXPECT_NEAR(
        std::accumulate(a.state.rk2_rain_pending.begin(), a.state.rk2_rain_pending.end(), 0.), 1.6,
        1e-12);
    const auto saved = rk2Output("forcing-water-only-source.chk"),
               before = rk2Output("forcing-water-only-before.chk"),
               after = rk2Output("forcing-water-only-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.1, .2));
    ASSERT_NO_THROW(b.solver.advance(.1, .2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

namespace {
/**
 * @brief Replace equal well/pump runtime sources in every groundwater cell.
 *
 * Uses owner ground-runtime-test and well/pump IDs, concentration 7 in ordinary rows, age=3 s and
 * temperature=-8 C where present, with zero explicit rates. Rebuilds lookup but does not integrate
 * water/heat/species or validate inputs.
 *
 * @param[in,out] r Initialized rig whose GW runtime-source table/lookup are mutated.
 * @param[in] flow Nonnegative flow magnitude in m3/s; well positive, pump negative.
 * @param[in] until Elapsed expiry seconds, default infinity.
 */
void setRkGroundRuntimePair(CoupledRkRig& r, double flow = .1,
                            double until = std::numeric_limits<double>::infinity()) {
    auto& rows = r.gw.runtimeSources();
    const auto& tr = r.gw.transport();
    for (int cell = 0; cell < r.mesh.n_cells(); ++cell)
        for (bool sink : {false, true}) {
            RuntimeSource row;
            row.id = sink ? "pump" : "well";
            row.owner = "ground-runtime-test";
            row.cell = cell;
            row.flow = sink ? -flow : flow;
            row.until = until;
            row.concentrations.assign(tr.n_species, 7);
            if (tr.age_row >= 0)
                row.concentrations[tr.age_row] = 3;
            if (tr.temp_row >= 0)
                row.concentrations[tr.temp_row] = -8;
            row.rates.assign(tr.n_species, 0);
            rows.replace(std::move(row));
        }
    rows.rebuild(r.mesh.n_cells());
}
}
TEST(CoupledGroundwaterRk2, GroundRuntimeAnalyticalRetardedMixingAndAgeUseAcceptedReceipts) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            for (double kd : {0., .001}) {
                double previous = 0;
                for (double dt : {.4, .2, .1, .05}) {
                    CoupledRkRig r;
                    r.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                    initRkQuality(r, layered, true, true);
                    seedRkQuality(r, 2, kd);
                    setRkGroundRuntimePair(r);
                    auto& tr = r.gw.transport();
                    const auto& w = r.gw.state();
                    const double water = r.total(),
                                 capacity = w.area[0] * w.hg[0] * (w.theta_s[0] + kd * tr.rho_b[0]);
                    ASSERT_NO_THROW(r.solver.advance(0, 2));
                    const double exact = 7 - 5 * std::exp(-.2 / capacity),
                                 error = std::abs(tr.sat_mass[0] / capacity - exact);
                    EXPECT_LT(error, 1e-4);
                    if (previous > 0)
                        EXPECT_GT(previous / error, 3.5);
                    previous = error;
                    RecordProperty("ground_runtime_mixing_" + std::to_string(int(closure)) + "_" +
                                       std::to_string(layered) + "_" + std::to_string(kd) + "_" +
                                       std::to_string(dt),
                                   error);
                    EXPECT_NEAR(r.total(), water, 1e-10);
                    EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
                    for (int s = 0; s < tr.n_species; ++s)
                        EXPECT_NEAR(tr.residual(s), 0, 1e-10);
                    for (int i = 0; i < tr.n_cells; ++i) {
                        const auto& in = r.gw.runtimeSources().find("well", i)->receipt;
                        const auto& out = r.gw.runtimeSources().find("pump", i)->receipt;
                        EXPECT_NEAR(in.water_in, .2, 1e-13);
                        EXPECT_NEAR(out.water_out, .2, 1e-13);
                        EXPECT_NEAR(in.applied_mass[0], 1.4, 1e-13);
                        EXPECT_NEAR(in.last_flow, .1, 1e-13);
                        EXPECT_NEAR(out.last_flow, -.1, 1e-13);
                    }
                }
            }
        }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeHeaterAndColdMixingHaveIndependentSignedEnergyIntegral) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkTemperature(r, layered, -2, -2);
                setRkGroundRuntimePair(r);
                for (auto& row : r.gw.runtimeSources().rows)
                    if (row.flow > 0)
                        row.heat = 4186;
                const auto& w = r.gw.state();
                const double capacity = w.area[0] * w.hg[0] * w.theta_s[0];
                ASSERT_NO_THROW(r.solver.advance(0, 2));
                const double equilibrium = -8 + .001 / .1,
                             exact = equilibrium + (-2 - equilibrium) * std::exp(-.2 / capacity);
                const auto& tr = r.gw.transport();
                const double error = std::abs(tr.sat_mass[0] / capacity - exact);
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                for (int i = 0; i < w.n_cells; ++i) {
                    const auto& receipt = r.gw.runtimeSources().find("well", i)->receipt;
                    EXPECT_NEAR(receipt.heat_applied, 8372, 1e-8);
                    EXPECT_NEAR(receipt.heat_requested, 8372, 1e-8);
                }
                EXPECT_NEAR(tr.residual(0), 0, 1e-10);
                EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
            }
        }
}
TEST(CoupledGroundwaterRk2,
     GroundRuntimeCheckpointBindsPrescriptionsAndContinuesAcrossExpiryExactly) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkGroundRuntimePair(*r, .1, .735);
    }
    const auto saved = rk2Output("ground-runtime.chk"),
               before = rk2Output("ground-runtime-before.chk"),
               after = rk2Output("ground-runtime-after.chk");
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    b.gw.runtimeSources().rows[0].flow += .01;
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.gw.runtimeSources().rows[0].flow -= .01;
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.6, 2));
    ASSERT_NO_THROW(b.solver.advance(.6, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    for (const auto& row : a.gw.runtimeSources().rows) {
        EXPECT_FALSE(row.enabled);
        EXPECT_NEAR(row.receipt.water_in + row.receipt.water_out, .0735, 1e-13);
    }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeLaterProposalFailureRestoresAllOwnersAndReceipts) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
    initRkQuality(r, true, true, true);
    seedRkQuality(r, 2, .001);
    setRkGroundRuntimePair(r);
    r.gw.runtimeSources().find("well", 1)->flow = 1e308;
    const auto before = rk2Output("ground-overflow-before.chk"),
               after = rk2Output("ground-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 10, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.gw.runtimeSources().find("well", 1)->flow = .1;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}
TEST(CoupledGroundwaterRk2, GroundRuntimeIndependentChemicalSinksCannotSpendArrivingMass) {
    for (bool layered : {false, true})
        for (double kd : {0., .001}) {
            CoupledRkRig r;
            r.init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkQuality(r, layered);
            seedRkQuality(r, 0, kd);
            setRkGroundRuntimePair(r, 0);
            for (auto& row : r.gw.runtimeSources().rows) {
                row.flow = 0;
                row.rates[0] = row.id == "well" ? 10 : -100;
            }
            ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, .1, 0));
            for (int cell = 0; cell < r.mesh.n_cells(); ++cell) {
                const auto& in = r.gw.runtimeSources().find("well", cell)->receipt;
                const auto& out = r.gw.runtimeSources().find("pump", cell)->receipt;
                EXPECT_NEAR(in.applied_mass[0], 1, 1e-13);
                EXPECT_GE(out.applied_mass[0], -.5);
                EXPECT_GT(r.gw.transport().sat_mass[cell], 0);
            }
            EXPECT_NEAR(r.gw.transport().residual(0), 0, 1e-12);
        }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeExhaustingColdPumpRetainsPassiveTemperatureBounds) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig r;
            r.init(.1, .001, .5, .2, "NONE", closure, SoilChar::GARDNER, 4);
            initRkTemperature(r, layered, 6, 6);
            setRkGroundRuntimePair(r, 100);
            ASSERT_NO_THROW(r.solver.advance(0, .1));
            const auto& tr = r.gw.transport();
            const auto& w = r.gw.state();
            for (int i = 0; i < w.n_cells; ++i)
                if (w.hg[i] > 0) {
                    const double t = tr.sat_mass[i] / (w.theta_s[i] * w.hg[i] * w.area[i]);
                    EXPECT_GE(t, -8 - 1e-10);
                    EXPECT_LE(t, 6 + 1e-10);
                }
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
            EXPECT_NEAR(tr.residual(0), 0, 1e-9);
        }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeSameNameOverrideAndExpiryRestoreAuthoredWell) {
    for (bool clear : {false, true}) {
        CoupledRkRig r;
        r.init(.1, 0, 0, .2, "NONE");
        initRkQuality(r, false);
        seedRkQuality(r, 2);
        GwResolvedSource source;
        source.name = "well";
        source.cells = {{0, 1}};
        source.flow.constant = .03;
        r.gw.setSources({source});
        setRkGroundRuntimePair(r, .1, .735);
        ASSERT_NO_THROW(r.solver.advance(0, .6));
        if (clear)
            ASSERT_TRUE(r.gw.runtimeSources().clear("well"));
        ASSERT_NO_THROW(r.solver.advance(.6, 2));
        EXPECT_NEAR(r.gw.state().led_source_in,
                    .2 * (clear ? .6 : .735) + .03 * (2 - (clear ? .6 : .735)), 1e-12);
        EXPECT_NEAR(r.gw.state().continuityResidual(), 0, 1e-10);
    }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeWaterOnlyNeedsNoSpeciesAndRestartIsExact) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE");
        setRkGroundRuntimePair(*r, .1, .735);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("ground-water-only.chk"),
               before = rk2Output("ground-water-before.chk"),
               after = rk2Output("ground-water-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.6, 2));
    ASSERT_NO_THROW(b.solver.advance(.6, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    EXPECT_NEAR(a.gw.state().continuityResidual(), 0, 1e-10);
}
TEST(CoupledGroundwaterRk2, GroundRuntimePorousHeatUsesActualFixedGridCapacityAndCountsHeaterOnce) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
                RkPorousOwner owner;
                owner.initialize(r, layered, 3);
                setRkGroundRuntimePair(r);
                for (auto& row : r.gw.runtimeSources().rows)
                    if (row.flow > 0)
                        row.heat = 4186;
                const auto& column = r.gw.heat().cells[0].column;
                const double capacity = column.capacity.back(), initial = rkPorousEnergy(r),
                             equilibrium = -8 + .001 / .1;
                ASSERT_NO_THROW(r.solver.advance(0, 2));
                const double exact = equilibrium +
                                     (-2 - equilibrium) * std::exp(-.1 * 4186000 * 2 / capacity),
                             error = std::abs(column.energy.back() / capacity - exact);
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                double net = 0;
                for (const auto& row : r.gw.runtimeSources().rows)
                    net += 4186000 * row.receipt.applied_mass[0];
                EXPECT_NEAR(rkPorousEnergy(r) - initial, net,
                            1e-10 * std::max(1., std::abs(initial)));
                checkRkPorousLedgers(r);
                for (const auto& cell : r.gw.heat().cells)
                    EXPECT_NEAR(cell.source_j, 8372, 1e-8);
            }
        }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeSharesFiniteReactiveChemicalAgeAndPhysicalHeatOwners) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            RkReactionSystem system(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
            CoupledRkRig r;
            r.init(.05, .003, .001, .2, "NONE", closure, SoilChar::GARDNER, 4);
            RkPorousOwner owner;
            initRkFiniteQuality(r, owner, system, layered, 3);
            setRkGroundRuntimePair(r, .02, .735);
            setRkRuntimePair(r, .01, 1.237);
            for (auto& row : r.gw.runtimeSources().rows)
                if (row.flow > 0) {
                    row.rates[0] = .001;
                    row.heat = 4186;
                }
            auto initial = finiteQualityInitial(r);
            const auto energy_now = [&]() {
                double e = rkPorousEnergy(r);
                for (int i = 0; i < r.mesh.n_cells(); ++i)
                    e += 4186000 *
                         r.state.transport
                             .cell_mass[r.state.transport.idx(r.state.transport.temp_row, i)];
                return e;
            };
            const double water = r.total(), energy = energy_now();
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            const auto& tr = r.gw.transport();
            double provider_energy = 0;
            for (const auto& row : r.gw.runtimeSources().rows) {
                for (int s = 0; s < tr.n_species; ++s)
                    initial[s] += row.receipt.applied_mass[s];
                provider_energy += 4186000 * row.receipt.applied_mass[tr.temp_row];
            }
            for (const auto& row : r.state.runtime_sources.rows) {
                for (int s = 0; s < tr.n_species; ++s)
                    initial[s] += row.receipt.applied_mass[s];
                provider_energy += 4186000 * row.receipt.applied_mass[tr.temp_row];
            }
            for (int row = 0; row < tr.n_species; ++row)
                if (row != tr.temp_row) {
                    double surface = 0;
                    for (int i = 0; i < tr.n_cells; ++i)
                        surface += r.state.transport.cell_mass[r.state.transport.idx(row, i)];
                    EXPECT_NEAR(tr.storage(row) + surface + tr.lost_reaction[row] +
                                    r.solver.surfaceReactionLoss(row) + tr.lost_deep[row] +
                                    tr.lost_et[row] - tr.generated_age[row] -
                                    r.state.transport.generated_age[row],
                                initial[row], 1e-10 * std::max(1., initial[row]));
                    EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
                }
            checkRkPorousLedgers(r);
            EXPECT_NEAR(r.total() + r.gw.state().led_deep, water, 1e-10 * water);
            EXPECT_NEAR(energy_now() + 4186000 * tr.lost_deep[tr.temp_row] - energy,
                        provider_energy, 1e-10 * std::max(1., std::abs(energy)));
        }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeLocalStepDoublingHasThirdOrderDifference) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig a, b;
        for (auto* r : {&a, &b}) {
            r->init(dt, 0, 0, .2, "NONE");
            initRkQuality(*r, false);
            seedRkQuality(*r, 2, .001);
            setRkGroundRuntimePair(*r);
        }
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, dt / 2));
        ASSERT_NO_THROW(b.solver.advance(dt / 2, dt));
        const double error = std::abs(a.gw.transport().sat_mass[0] - b.gw.transport().sat_mass[0]);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 7);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeMatchesExistingAuthoredWellPhysicalTrajectory) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig a, b;
            for (auto* r : {&a, &b}) {
                r->init(.05, .003, .001, .2, "NONE", closure, SoilChar::GARDNER, 4);
                initRkQuality(*r, layered, true, true);
                seedRkQuality(*r, 2, .001);
            }
            setRkGroundRuntimePair(a);
            std::vector<GwResolvedSource> sources;
            for (int i = 0; i < b.mesh.n_cells(); ++i)
                for (bool sink : {false, true}) {
                    GwResolvedSource source;
                    source.name = sink ? "pump" : "well";
                    source.cells = {{i, 1}};
                    source.flow.constant = sink ? -.1 : .1;
                    for (int row = 0; row < 2; ++row) {
                        GwResolvedSourceTerm term;
                        term.row = row;
                        term.signal.constant = row == 0 ? 7 : 3;
                        source.terms.push_back(term);
                    }
                    sources.push_back(source);
                }
            b.gw.setSources(sources);
            ASSERT_NO_THROW(a.solver.advance(0, 2));
            ASSERT_NO_THROW(b.solver.advance(0, 2));
            EXPECT_EQ(a.gw.state().hg, b.gw.state().hg);
            EXPECT_EQ(a.gw.state().hu, b.gw.state().hu);
            for (auto member :
                 {&SubsurfaceTransportState::sat_mass, &SubsurfaceTransportState::layer_mass,
                  &SubsurfaceTransportState::generated_age}) {
                const auto& actual = a.gw.transport().*member;
                const auto& reference = b.gw.transport().*member;
                ASSERT_EQ(actual.size(), reference.size());
                for (std::size_t k = 0; k < actual.size(); ++k)
                    EXPECT_NEAR(actual[k], reference[k], 1e-13);
            }
        }
}
TEST(CoupledGroundwaterRk2, GroundRuntimeCorruptGrossReceiptRefusesBeforeInstallingAnyOwner) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkGroundRuntimePair(*r, .1, .735);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("ground-corrupt-source.chk"),
               bad = rk2Output("ground-corrupt-gross.chk"),
               before = rk2Output("ground-corrupt-before.chk"),
               after = rk2Output("ground-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    openswmm::detail::CompletedSourceArchive original(rk2File(saved));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const std::string marker = "RK2 copied groundwater runtime sources v1";
    const auto pos = payload.rfind(marker);
    ASSERT_NE(pos, std::string::npos);
    const auto offset = pos + marker.size() + sizeof(uint64_t) + sizeof(bool) + sizeof(double);
    ASSERT_LT(offset + sizeof(double), payload.size());
    double value = 0;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    value += 1;
    std::memcpy(payload.data() + offset, &value, sizeof(value));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("net and gross"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2, GroundRuntimeDryHeaterRetainsRequestedAndAppliedDiagnostics) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE");
    initRkTemperature(r, false);
    auto& w = r.gw.state();
    auto& tr = r.gw.transport();
    for (int i = 0; i < w.n_cells; ++i) {
        w.hg[i] = 0;
        tr.sat_mass[i] = 0;
    }
    w.led_init_storage = w.storage();
    tr.init_mass[0] = tr.storage(0);
    setRkGroundRuntimePair(r, 0);
    for (auto& row : r.gw.runtimeSources().rows)
        if (row.id == "well")
            row.heat = 4186;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    for (int i = 0; i < w.n_cells; ++i) {
        const auto& b = r.gw.runtimeSources().find("well", i)->receipt;
        EXPECT_NEAR(b.heat_requested, 418.6, 1e-12);
        EXPECT_EQ(b.heat_applied, 0);
        EXPECT_NEAR(b.requested_mass[0], .0001, 1e-15);
        EXPECT_EQ(b.applied_mass[0], 0);
    }
    EXPECT_NEAR(tr.residual(0), 0, 1e-12);
}
TEST(CoupledGroundwaterRk2, GroundRuntimeInvalidRowsKeepBoundaryAndSpeciesGuardsAtomic) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE");
    initRkQuality(r, false, true);
    seedRkQuality(r, 2);
    setRkGroundRuntimePair(r);
    auto& row = r.gw.runtimeSources().rows[0];
    const double water = r.total();
    row.boundary_edge = 0;
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    row.boundary_edge = -1;
    row.rates[1] = 1;
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    row.rates[1] = 0;
    row.concentrations.pop_back();
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    EXPECT_DOUBLE_EQ(r.total(), water);
}
TEST(CoupledGroundwaterRk2, GroundRuntimeCompetingProvidersAreIndependentOfInsertionOrder) {
    for (bool layered : {false, true}) {
        CoupledRkRig a, b;
        for (auto* r : {&a, &b}) {
            r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkQuality(*r, layered);
            seedRkQuality(*r, 2, .001);
            setRkGroundRuntimePair(*r, .1);
            for (auto& row : r->gw.runtimeSources().rows)
                row.rates[0] = -.2;
        }
        auto reversed = b.gw.runtimeSources().rows;
        std::reverse(reversed.begin(), reversed.end());
        b.gw.runtimeSources() = {};
        for (auto& row : reversed)
            b.gw.runtimeSources().replace(row);
        b.gw.runtimeSources().rebuild(2);
        ASSERT_NO_THROW(a.solver.advance(0, 2));
        ASSERT_NO_THROW(b.solver.advance(0, 2));
        for (std::size_t k = 0; k < a.gw.transport().sat_mass.size(); ++k)
            EXPECT_NEAR(a.gw.transport().sat_mass[k], b.gw.transport().sat_mass[k], 1e-13);
        for (const auto& row : a.gw.runtimeSources().rows)
            EXPECT_NEAR(row.receipt.applied_mass[0],
                        b.gw.runtimeSources().find(row.id, row.cell)->receipt.applied_mass[0],
                        1e-13);
    }
}

TEST(CoupledGroundwaterRk2, GroundRuntimeMovingColumnMatchesIndependentReactiveWaterHeatAgeOde) {
    constexpr double partition = .001 * 2650 * (1 - .45), cp = 4186000;
    for (bool layered : {false, true}) {
        const auto exact =
                       porousColumnReference(20000, true, layered, partition, true, true, 0, .0025),
                   refined =
                       porousColumnReference(40000, true, layered, partition, true, true, 0, .0025);
        for (std::size_t k = 0; k < exact.size(); ++k)
            ASSERT_NEAR(exact[k], refined[k], 1e-10);
        RkReactionSystem system(kRkPorousConversion, "RK5", kRkPorousSurfaceConversion);
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            r.init(dt, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            seedRkSigma(r);
            RkPorousOwner owner;
            initRkFiniteQuality(r, owner, system, layered, 3);
            owner.config.options.conduction = true;
            auto& surface = r.state.transport;
            const auto& w = r.gw.state();
            const auto& tr = r.gw.transport();
            owner.config.params[0].lambda_s = 2e5;
            for (int i = 0; i < w.n_cells; ++i) {
                auto& column = r.gw.heat().cells[i].column;
                column.material.grain_conductivity = 2e5;
                ASSERT_EQ(column.setMoisture(column.theta), "");
                surface.cell_mass[surface.idx(0, i)] = 6 * r.state.volume[i];
                surface.cell_mass[surface.idx(surface.age_row, i)] = 5 * r.state.volume[i];
                surface.cell_mass[surface.idx(surface.temp_row, i)] = 6 * r.state.volume[i];
            }
            setRkGroundRuntimePair(r, .0025 * w.area[0]);
            for (auto& source : r.gw.runtimeSources().rows) {
                source.concentrations[1] = 0;
                if (source.flow > 0) {
                    source.rates[0] = .0002 * w.area[source.cell];
                    source.heat = .0001 * cp * w.area[source.cell];
                }
            }
            const auto sum = [&](int row) {
                double result = 0;
                for (int i = 0; i < surface.n_cells; ++i)
                    result += surface.cell_mass[surface.idx(row, i)];
                return result;
            };
            const double initial_mass = tr.storage(0) + tr.storage(1) + sum(0) + sum(1),
                         initial_age = tr.storage(tr.age_row) + sum(surface.age_row);
            const double initial_energy = rkPorousEnergy(r) + cp * sum(surface.temp_row),
                         area = w.area[0];
            ASSERT_NO_THROW(r.solver.advance(0, 2));
            std::vector<double> actual = {w.hg[0]};
            for (int j = 0; j < 4; ++j)
                actual.push_back(w.theta_sigma[std::size_t(j) * w.n_cells] * (2 - w.hg[0]) / 4);
            actual.push_back(r.state.volume[0] / area);
            actual.push_back(surface.cell_mass[surface.idx(surface.temp_row, 0)] / area);
            for (double energy : r.gw.heat().cells[0].column.energy)
                actual.push_back(energy / (area * cp));
            for (int row : {0, 1, tr.age_row}) {
                if (layered) {
                    for (int j = 0; j < 4; ++j)
                        actual.push_back(tr.layer_mass[tr.layerIdx(row, j, 0)] / area);
                    actual.push_back(tr.sat_mass[tr.idx(row, 0)] / area);
                } else {
                    const auto& p = r.gw.heat().cells[0].porewater;
                    for (int j = 0; j < 3; ++j)
                        actual.push_back(p.mass[std::size_t(row) * 3 + j] / area);
                }
                actual.push_back(surface.cell_mass[surface.idx(row, 0)] / area);
            }
            ASSERT_EQ(actual.size(), exact.size());
            double error = 0;
            for (std::size_t j = 0; j < actual.size(); ++j)
                error += std::pow(actual[j] - exact[j], 2);
            error = std::sqrt(error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            RecordProperty("ground_runtime_reactive_moving_" + std::to_string(layered) + "_" +
                               std::to_string(int(dt * 10000)),
                           error);
            const auto net_source = [&](int row) {
                double m = 0;
                for (const auto& source : r.gw.runtimeSources().rows)
                    m += source.receipt.applied_mass[row];
                return m;
            };
            EXPECT_NEAR(tr.storage(0) + tr.storage(1) + sum(0) + sum(1) + tr.lost_deep[0] +
                            tr.lost_deep[1],
                        initial_mass + net_source(0) + net_source(1), 1e-10 * initial_mass);
            EXPECT_NEAR(tr.storage(tr.age_row) + sum(surface.age_row) + tr.lost_deep[tr.age_row] -
                            tr.generated_age[tr.age_row] - surface.generated_age[surface.age_row],
                        initial_age + net_source(tr.age_row), 1e-10 * initial_age);
            EXPECT_NEAR(rkPorousEnergy(r) +
                            cp * (sum(surface.temp_row) + tr.lost_deep[tr.temp_row]),
                        initial_energy + cp * net_source(tr.temp_row),
                        1e-10 * std::max(1., std::abs(initial_energy)));
            for (int row : {0, 1, tr.age_row})
                EXPECT_NEAR(tr.residual(row), 0, 1e-10 * std::max(1., tr.init_mass[row]));
            checkRkPorousLedgers(r);
        }
    }
}

namespace {
/**
 * @brief Build a groundwater runtime boundary-source row at a selected exterior edge.
 *
 * Throws runtime_error when selected exterior edge is absent. Seeds ordinary concentration 7, age=3
 * s and temperature=-8 C where present. Does not install the row or retain references; caller
 * ensures valid cell/geometry and tuple layout.
 *
 * @param[in] r Borrowed rig with stable initialized mesh and GW transport row layout.
 * @param[in] cell Valid zero-based cell.
 * @param[in] head True prescribes a datum head; false a signed native runtime source flow.
 * @param[in] value Datum head in metres when head=true; otherwise m3/s positive into aquifer and
 *     negative out.
 * @param[in] outer Nonnegative ordinal among cell exterior edges.
 * @return Owned source row with boundary ID/owner, selected edge, copied species tuple and zero
 *     explicit rates.
 */
RuntimeSource rkGroundBoundary(CoupledRkRig& r, int cell, bool head, double value, int outer = 0) {
    RuntimeSource row;
    row.cell = cell;
    for (int edge = 0; edge < r.mesh.cell_vertex_count(cell); ++edge)
        if (r.mesh.cell_neighbour(cell, edge) < 0) {
            if (outer-- == 0) {
                row.boundary_edge = edge;
                break;
            }
        }
    if (row.boundary_edge < 0)
        throw std::runtime_error("Missing test exterior aquifer edge");
    row.id = "boundary/" + std::to_string(cell) + "/" + std::to_string(row.boundary_edge);
    row.owner = "boundary-test";
    if (head)
        row.boundary_head = value;
    else
        row.flow = value;
    const auto& tr = r.gw.transport();
    row.concentrations.assign(tr.n_species, 7);
    row.rates.assign(tr.n_species, 0);
    if (tr.age_row >= 0)
        row.concentrations[tr.age_row] = 3;
    if (tr.temp_row >= 0)
        row.concentrations[tr.temp_row] = -8;
    return row;
}
/**
 * @brief Install one runtime groundwater boundary per cell and rebuild lookup.
 *
 * Uses first exterior edge and rkGroundBoundary defaults for every cell. Does not clear unrelated
 * IDs, advance physics or roll back earlier replaced rows if a later cell fails; missing-edge
 * exceptions propagate.
 *
 * @param[in,out] r Initialized rig whose GW runtime sources are replaced by boundary ID.
 * @param[in] head True selects datum-head prescription, false signed source flow.
 * @param[in] value Datum metres or m3/s positive into aquifer, according to head.
 */
void setRkGroundBoundary(CoupledRkRig& r, bool head, double value) {
    for (int cell = 0; cell < r.mesh.n_cells(); ++cell)
        r.gw.runtimeSources().replace(rkGroundBoundary(r, cell, head, value));
    r.gw.runtimeSources().rebuild(r.mesh.n_cells());
}
}
TEST(CoupledGroundwaterRk2, GroundBoundaryHeadHasIndependentAnalyticalSecondOrderRelaxation) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig r;
        r.init(dt, .02, 0, 0, "NONE", GwClosure::CLOSED_FORM, SoilChar::GARDNER);
        auto& w = r.gw.state();
        for (int i = 0; i < w.n_cells; ++i)
            w.hu[i] = w.theta_r[i] * (w.zs[i] - w.hg[i]);
        w.led_init_storage = w.storage();
        setRkGroundBoundary(r, true, w.z_bed[0] + .8);
        const double initial = r.total();
        const int slot = MeshData::slot(0, r.gw.runtimeSources().rows[0].boundary_edge);
        const double coefficient =
            .02 * r.mesh.edge_length[slot] /
            (2 * r.mesh.edge_dist_c[slot] * (w.theta_s[0] - w.theta_r[0]) * w.area[0]);
        const double exact = .8 * std::tanh(std::atanh(.6 / .8) + coefficient * .8 * 2);
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        const double error = std::abs(w.hg[0] - exact);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        RecordProperty("ground_boundary_head_" + std::to_string(dt), error);
        double incoming = 0;
        for (const auto& row : r.gw.runtimeSources().rows)
            incoming += row.receipt.applied;
        EXPECT_NEAR(r.total() - initial, incoming, 1e-10);
        EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
    }
}
TEST(CoupledGroundwaterRk2, GroundBoundaryFlowHasIndependentRetardedQualityMixing) {
    for (bool layered : {false, true})
        for (double kd : {0., .001}) {
            double previous = 0;
            for (double dt : {.4, .2, .1, .05}) {
                CoupledRkRig r;
                r.init(dt, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
                initRkQuality(r, layered, true, true);
                seedRkQuality(r, 2, kd);
                setRkGroundBoundary(r, false, .1);
                for (int i = 0; i < r.mesh.n_cells(); ++i)
                    r.gw.runtimeSources().replace(rkGroundBoundary(r, i, false, -.1, 1));
                r.gw.runtimeSources().rebuild(2);
                const auto& w = r.gw.state();
                const auto& tr = r.gw.transport();
                const double capacity = w.area[0] * w.hg[0] * (w.theta_s[0] + kd * tr.rho_b[0]);
                ASSERT_NO_THROW(r.solver.advance(0, 2));
                const double exact = 7 - 5 * std::exp(-.2 / capacity),
                             error = std::abs(tr.sat_mass[0] / capacity - exact);
                EXPECT_LT(error, 1e-4);
                if (previous > 0)
                    EXPECT_GT(previous / error, 3.5);
                previous = error;
                for (int row = 0; row < tr.n_species; ++row)
                    EXPECT_NEAR(tr.residual(row), 0, 1e-10);
                EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
            }
        }
}
TEST(CoupledGroundwaterRk2, GroundBoundaryMatchesAuthoredHydraulicAndQualityEdgeTrajectories) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true})
            for (bool head : {false, true})
                for (int thermal : {0, 1, 2}) {
                    if (layered && closure != GwClosure::SIGMA)
                        continue;
                    CoupledRkRig a, b;
                    RkPorousOwner oa, ob;
                    for (auto* r : {&a, &b}) {
                        r->init(.05, .003, .001, .2, "NONE", closure, SoilChar::GARDNER, 4);
                        if (thermal == 2)
                            (r == &a ? oa : ob).initialize(*r, layered, 3);
                        else if (thermal == 1)
                            initRkTemperature(*r, layered);
                        else {
                            initRkQuality(*r, layered, true, true);
                            seedRkQuality(*r, 2, .001);
                        }
                    }
                    setRkGroundBoundary(a, head, head ? .8 : .01);
                    std::vector<GwResolvedBoundary> authored;
                    for (const auto& row : a.gw.runtimeSources().rows) {
                        GwSourceSignal signal;
                        signal.constant = head ? row.boundary_head : row.flow;
                        auto edge = rk2FlowEdge(b, row.cell, signal);
                        edge.head = head;
                        for (int s = 0; s < b.gw.transport().n_species; ++s) {
                            GwBoundaryTerm term;
                            term.row = s;
                            term.signal.constant = row.concentrations[s];
                            edge.terms.push_back(term);
                        }
                        edge.mass_in.assign(b.gw.transport().n_species, 0);
                        edge.mass_out = edge.flux_in = edge.flux_out = edge.mass_in;
                        authored.push_back(edge);
                    }
                    b.gw.setBoundaries(authored);
                    ASSERT_NO_THROW(a.solver.advance(0, 2));
                    ASSERT_NO_THROW(b.solver.advance(0, 2));
                    for (auto member : {&SubsurfaceState::hg, &SubsurfaceState::hu})
                        for (int i = 0; i < a.mesh.n_cells(); ++i)
                            EXPECT_NEAR((a.gw.state().*member)[i], (b.gw.state().*member)[i],
                                        1e-13);
                    for (auto member : {&SubsurfaceTransportState::sat_mass,
                                        &SubsurfaceTransportState::layer_mass,
                                        &SubsurfaceTransportState::unsat_mass}) {
                        const auto& actual = a.gw.transport().*member;
                        const auto& reference = b.gw.transport().*member;
                        ASSERT_EQ(actual.size(), reference.size());
                        for (std::size_t k = 0; k < actual.size(); ++k)
                            EXPECT_NEAR(actual[k], reference[k], 1e-13);
                    }
                    if (thermal == 2) {
                        checkRkPorousLedgers(a);
                        checkRkPorousLedgers(b);
                        for (int i = 0; i < a.mesh.n_cells(); ++i)
                            for (std::size_t j = 0; j < a.gw.heat().cells[i].column.energy.size();
                                 ++j)
                                EXPECT_NEAR(a.gw.heat().cells[i].column.energy[j],
                                            b.gw.heat().cells[i].column.energy[j], 1e-7);
                    }
                }
}
TEST(CoupledGroundwaterRk2,
     GroundBoundaryHeadCheckpointBindsPrescriptionAndRestoresEvaluatedFlowExactly) {
    for (bool layered : {false, true}) {
        CoupledRkRig a, b;
        for (auto* r : {&a, &b}) {
            r->init(.1, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkQuality(*r, layered, true, true);
            seedRkQuality(*r, 2, .001);
            setRkGroundBoundary(*r, true, .8);
        }
        ASSERT_NO_THROW(a.solver.advance(0, .6));
        ASSERT_NE(a.gw.runtimeSources().rows[0].flow, 0);
        const auto saved = rk2Output("ground-boundary.chk"),
                   before = rk2Output("ground-boundary-before.chk"),
                   after = rk2Output("ground-boundary-after.chk");
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        for (double* value : {&b.gw.runtimeSources().rows[0].boundary_head,
                              &b.gw.runtimeSources().rows[0].concentrations[0]}) {
            const double old = *value;
            *value += .1;
            ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
            EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
            ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
            EXPECT_EQ(rk2File(before), rk2File(after));
            *value = old;
        }
        ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
        EXPECT_EQ(a.gw.runtimeSources().rows[0].flow, b.gw.runtimeSources().rows[0].flow);
        ASSERT_NO_THROW(a.solver.advance(.6, 2));
        ASSERT_NO_THROW(b.solver.advance(.6, 2));
        ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
    }
}
TEST(CoupledGroundwaterRk2, GroundBoundaryLaterHeadOverflowRestoresEveryPhysicalOwnerAndCanRetry) {
    CoupledRkRig r;
    r.init(.1, .003, .001, .2, "NONE");
    initRkTemperature(r, false);
    setRkGroundBoundary(r, true, .8);
    r.gw.runtimeSources().rows[1].boundary_head = 1e308;
    const auto before = rk2Output("ground-boundary-head-overflow-before.chk"),
               after = rk2Output("ground-boundary-head-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.gw.runtimeSources().rows[1].boundary_head = .8;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}
TEST(CoupledGroundwaterRk2,
     GroundBoundaryClearReversalAndExhaustionKeepColdWaterAndCanonicalBudgets) {
    for (GwClosure closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true}) {
            if (layered && closure != GwClosure::SIGMA)
                continue;
            CoupledRkRig r;
            r.init(.05, 0, 0, .2, "NONE", closure, SoilChar::GARDNER, 4);
            initRkTemperature(r, layered, 6, 6);
            setRkGroundBoundary(r, false, .1);
            ASSERT_NO_THROW(r.solver.advance(0, .3));
            for (auto& row : r.gw.runtimeSources().rows)
                row.flow = -100;
            ASSERT_NO_THROW(r.solver.advance(.3, .35));
            const auto& w = r.gw.state();
            const auto& tr = r.gw.transport();
            for (int i = 0; i < w.n_cells; ++i)
                if (w.hg[i] > 0) {
                    const double t = tr.sat_mass[i] / (w.area[i] * w.theta_s[i] * w.hg[i]);
                    EXPECT_GE(t, -8 - 1e-10);
                    EXPECT_LE(t, 6 + 1e-10);
                }
            for (auto& row : r.gw.runtimeSources().rows)
                row.enabled = false;
            r.gw.runtimeSources().rebuild(2);
            const double water = r.total();
            ASSERT_NO_THROW(r.solver.advance(.35, .5));
            EXPECT_NEAR(r.total(), water, 1e-10);
            EXPECT_NEAR(w.continuityResidual(), 0, 1e-10);
            EXPECT_NEAR(tr.residual(0), 0, 1e-10);
        }
}
TEST(CoupledGroundwaterRk2,
     GroundBoundaryInvalidHydraulicBindingAndAuthoredOverlapRefuseAtomically) {
    CoupledRkRig r;
    r.init(.1, .003, 0, .2, "NONE");
    initRkQuality(r, false);
    seedRkQuality(r, 2);
    setRkGroundBoundary(r, true, .8);
    auto& row = r.gw.runtimeSources().rows[0];
    const double water = r.total();
    const int edge = row.boundary_edge;
    for (int k = 0; k < r.mesh.cell_vertex_count(0); ++k)
        if (r.mesh.cell_neighbour(0, k) >= 0)
            row.boundary_edge = k;
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    row.boundary_edge = edge;
    GwSourceSignal signal;
    signal.constant = .01;
    auto boundary = rk2FlowEdge(r, 0, signal);
    boundary.mass_in = boundary.mass_out = boundary.flux_in = boundary.flux_out = {0};
    r.gw.setBoundaries({boundary});
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    EXPECT_DOUBLE_EQ(r.total(), water);
}
TEST(CoupledGroundwaterRk2, GroundBoundaryLocalHeadStepDoublingHasThirdOrderDifference) {
    double previous = 0;
    for (double dt : {.4, .2, .1, .05}) {
        CoupledRkRig a, b;
        for (auto* r : {&a, &b}) {
            r->init(dt, .02, 0, 0, "NONE");
            setRkGroundBoundary(*r, true, .8);
        }
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, dt / 2));
        ASSERT_NO_THROW(b.solver.advance(dt / 2, dt));
        const double error = std::abs(a.gw.state().hg[0] - b.gw.state().hg[0]);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 7);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, GroundBoundaryFlowCorruptReceiptRefusesBeforeAnyOwnerIsInstalled) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE");
        initRkQuality(*r, false);
        seedRkQuality(*r, 2);
        setRkGroundBoundary(*r, false, .01);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    const auto saved = rk2Output("boundary-corrupt.chk"), bad = rk2Output("boundary-bad.chk"),
               before = rk2Output("boundary-corrupt-before.chk"),
               after = rk2Output("boundary-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    openswmm::detail::CompletedSourceArchive original(rk2File(saved));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const std::string marker = "RK2 copied groundwater boundaries v1";
    const auto pos = payload.rfind(marker);
    ASSERT_NE(pos, std::string::npos);
    const auto offset = pos + marker.size() + sizeof(uint64_t) + sizeof(bool) + sizeof(double);
    ASSERT_LT(offset + sizeof(double), payload.size());
    double value = 0;
    std::memcpy(&value, payload.data() + offset, sizeof(value));
    value += 1;
    std::memcpy(payload.data() + offset, &value, sizeof(value));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("net and gross"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2, GroundBoundaryClearedRowsAndMixedOrdinarySourcesContinueExactly) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, .003, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
        setRkGroundRuntimePair(*r, .01, .735);
        setRkGroundBoundary(*r, true, .8);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .6));
    for (auto& row : a.gw.runtimeSources().rows)
        if (row.boundary_edge >= 0)
            row.enabled = false;
    a.gw.runtimeSources().rebuild(2);
    const auto saved = rk2Output("boundary-cleared.chk"),
               before = rk2Output("boundary-cleared-before.chk"),
               after = rk2Output("boundary-cleared-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.6, 2));
    ASSERT_NO_THROW(b.solver.advance(.6, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2, GroundBoundaryHeadRecomputesFlowInsteadOfSpendingAnOldDiagnostic) {
    CoupledRkRig a, b;
    for (auto* r : {&a, &b}) {
        r->init(.1, .003, 0, .2, "NONE");
        initRkTemperature(*r, false);
        setRkGroundBoundary(*r, true, .8);
    }
    a.gw.runtimeSources().rows[0].flow = 1e308;
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(a.solver, 10, 0));
    ASSERT_NO_THROW(ExplicitInertialSolverTestAccess::rk2(b.solver, 10, 0));
    EXPECT_EQ(a.gw.state().hg, b.gw.state().hg);
    EXPECT_EQ(a.gw.transport().sat_mass, b.gw.transport().sat_mass);
    EXPECT_EQ(a.gw.runtimeSources().rows[0].receipt.applied,
              b.gw.runtimeSources().rows[0].receipt.applied);
}

namespace {
/**
 * @brief Install one copied-quality runtime surface edge and refresh solver boundary caches.
 *
 * Throws on absent edge; existing nonempty boundary arrays must already cover all mesh slots and
 * selected edge length must be positive. Saves original runtime state, assigns owner surface,
 * ensures receipt storage, attaches borrowed boundary pointer and refreshes caches. Does not route
 * flux or validate arbitrary tuple/range combinations.
 *
 * @param[in,out] r Initialized rig; surface boundary pointer/quality map and solver caches are
 *     mutated.
 * @param[in,out] boundary Caller-owned edge data, allocated here only when empty; must outlive
 *     attached rig use.
 * @param[in] cell Valid cell index.
 * @param[in] stage True selects specified stage; false specified outward flow.
 * @param[in] value Datum stage metres or signed outward m3/s; flow is divided by edge length
 *     internally.
 * @param[in] quality Borrowed full concentration tuple copied for inflow, matching surface species
 *     layout.
 * @param[in] ordinal Nonnegative exterior-edge ordinal within the cell.
 * @return Selected global MeshData slot index.
 */
int setRkSurfaceBoundary(CoupledRkRig& r, BoundaryData& boundary, int cell, bool stage,
                         double value, const std::vector<double>& quality, int ordinal = 0) {
    if (boundary.size() == 0)
        boundary.resize(r.mesh.n_cells() * kMaxCellVerts);
    int chosen = -1;
    for (int edge = 0; edge < r.mesh.cell_vertex_count(cell); ++edge)
        if (r.mesh.cell_neighbour(cell, edge) < 0 && ordinal-- == 0) {
            chosen = MeshData::slot(cell, edge);
            break;
        }
    if (chosen < 0)
        throw std::runtime_error("Test has no exterior edge");
    boundary.saveRuntime(chosen);
    boundary.runtime_owner[chosen] = "surface";
    boundary.edge_bc_type[chosen] =
        int8_t(stage ? BoundaryType::SPECIFIED_STAGE : BoundaryType::SPECIFIED_FLOW);
    if (stage)
        boundary.edge_bc_head[chosen] = value;
    else
        boundary.edge_bc_flow[chosen] = value / r.mesh.edge_length[chosen];
    boundary.ensureReceipt(chosen, r.state.transport.n_species);
    r.state.transport.runtime_bc_conc[chosen] = quality;
    r.state.boundary = &boundary;
    r.solver.refreshBoundaries();
    return chosen;
}
}
TEST(CoupledGroundwaterRk2, SurfaceBoundaryConstantFlowCarriesIndependentWaterAndQualityIntegrals) {
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(r, layered, true, true);
        seedRkQuality(r, 2);
        BoundaryData boundary;
        const auto slot = setRkSurfaceBoundary(r, boundary, 0, false, -.01, {7, 3});
        ASSERT_EQ(r.solver.saveCheckpoint(rk2Output("surface-boundary-initial.chk").string()), "");
        const auto initial = r.total();
        const double mass =
            std::accumulate(r.state.transport.cell_mass.begin(),
                            r.state.transport.cell_mass.begin() + r.mesh.n_cells(), 0.);
        ASSERT_NO_THROW(r.solver.advance(0, 1));
        const auto& receipt = boundary.receipts.at(slot);
        EXPECT_NEAR(r.total() - initial, .01, 1e-10);
        EXPECT_NEAR(receipt.water_in, .01, 1e-12);
        EXPECT_EQ(receipt.water_out, 0);
        EXPECT_NEAR(receipt.applied_mass[0], .07, 1e-12);
        EXPECT_NEAR(std::accumulate(r.state.transport.cell_mass.begin(),
                                    r.state.transport.cell_mass.begin() + r.mesh.n_cells(), 0.) -
                        mass,
                    .07, 1e-10);
        EXPECT_NEAR(r.state.transport.gained_boundary[0], .07, 1e-12);
        EXPECT_NEAR(receipt.last_flow * receipt.last_dt, .01 * .05, 1e-12);
    }
}
TEST(CoupledGroundwaterRk2,
     SurfaceBoundaryComponentContinuationKeepsReceiptsMomentumAndClearHistory) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    for (auto* r : {&a, &b}) {
        r->init(.05, .003, .001, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkQuality(*r, true, true, true);
        seedRkQuality(*r, 2, .001);
    }
    const auto slot = setRkSurfaceBoundary(a, ba, 0, false, -.01, {7, 3});
    setRkSurfaceBoundary(b, bb, 0, false, -.01, {7, 3});
    ASSERT_EQ(a.solver.saveCheckpoint(rk2Output("surface-boundary-restart-initial.chk").string()),
              "");
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto saved = rk2Output("surface-boundary-accepted.chk"),
               after = rk2Output("surface-boundary-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    for (double end : {1., 1.735}) {
        const double begin = a.gw.cellCompletedTime(0);
        ASSERT_NO_THROW(a.solver.advance(begin, end));
        ASSERT_NO_THROW(b.solver.advance(begin, end));
        ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(saved), rk2File(after));
    }
    for (auto* r : {&a, &b}) {
        auto& boundary = r == &a ? ba : bb;
        boundary.clearRuntime(slot);
        r->state.transport.runtime_bc_conc.erase(slot);
        r->solver.refreshBoundaries();
    }
    ASSERT_NO_THROW(a.solver.advance(1.735, 2));
    ASSERT_NO_THROW(b.solver.advance(1.735, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(saved), rk2File(after));
    EXPECT_GT(ba.receipts.at(slot).water_in, 0);
}

TEST(CoupledGroundwaterRk2,
     SurfaceBoundaryMixingHasIndependentSecondOrderChemicalAndAgeTrajectories) {
    double previous = 0;
    int uniform = 0;
    for (double dt : {.4, .2, .1, .05, .025}) {
        CoupledRkRig r;
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell)
            for (int edge = 0; edge < r.mesh.cell_vertex_count(cell); ++edge)
                if (r.mesh.cell_neighbour(cell, edge) >= 0)
                    r.mesh.edge_conveyance[MeshData::slot(cell, edge)] = 0;
        r.init(dt, 0, 0, .2, "NONE");
        initRkQuality(r, false, true, true);
        seedRkQuality(r, 2);
        BoundaryData boundary;
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell) {
            setRkSurfaceBoundary(r, boundary, cell, false, -.1, {7, 3});
            setRkSurfaceBoundary(r, boundary, cell, false, .1, {7, 3}, 1);
        }
        long evaluations = 0;
        for (int step = 0; step < int(std::round(2 / dt)); ++step) {
            ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
            evaluations += r.solver.last_num_steps();
        }
        RecordProperty("surface_boundary_evaluations_" + std::to_string(int(dt * 1000)),
                       evaluations);
        const double volume = .2 * r.mesh.tri_area[0], attenuation = std::exp(-.1 * 2 / volume),
                     chemical = 7 - 5 * attenuation,
                     age = 3 + volume / .1 + (5 - 3 - volume / .1) * attenuation;
        double error = 0;
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell) {
            EXPECT_NEAR(r.state.volume[cell], volume, 1e-12);
            error = std::max(
                error,
                std::abs(r.state.transport.cell_mass[r.state.transport.idx(0, cell)] / volume -
                         chemical));
            error = std::max(
                error,
                std::abs(r.state.transport.cell_mass[r.state.transport.idx(1, cell)] / volume -
                         age));
        }
        RecordProperty("surface_boundary_mix_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, .0002);
        const long expected = 2 * int(std::round(2 / dt));
        if (evaluations != expected) {
            EXPECT_EQ(dt, .4);
            EXPECT_GT(evaluations, expected);
        } else {
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            ++uniform;
        }
        for (int row = 0; row < 2; ++row)
            EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row),
                        row == 1 ? 5 * 2 * volume : 2 * 2 * volume, 1e-10);
    }
    EXPECT_EQ(uniform, 4);
}
TEST(CoupledGroundwaterRk2, SurfaceBoundaryStageFullAndHalfStepsHaveThirdOrderLocalDifference) {
    double previous = 0;
    for (double dt : {.08, .04, .02, .01}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        for (auto* r : {&a, &b}) {
            for (auto& n : r->mesh.mannings_n)
                n = 0;
            r->init(dt, 0, 0, .2, "NONE");
            initRkQuality(*r, false, true, true);
            seedRkQuality(*r, 2);
        }
        setRkSurfaceBoundary(a, ba, 0, true, .22, {7, 3});
        setRkSurfaceBoundary(b, bb, 0, true, .22, {7, 3});
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, dt / 2));
        ASSERT_NO_THROW(b.solver.advance(dt / 2, dt));
        double error = 0;
        for (int cell = 0; cell < a.mesh.n_cells(); ++cell)
            error = std::max(error, std::abs(a.state.volume[cell] - b.state.volume[cell]));
        RecordProperty("surface_boundary_stage_local_" + std::to_string(int(dt * 1000)), error);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 6.5);
        previous = error;
    }
}

TEST(CoupledGroundwaterRk2, SurfaceBoundaryColdReversalAndCompetingExportsProtectBeginningDonors) {
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(r, layered, 6, 6);
        BoundaryData boundary;
        const auto a = setRkSurfaceBoundary(r, boundary, 0, false, -.1, {-8});
        const auto b = setRkSurfaceBoundary(r, boundary, 0, false, .1, {-8}, 1);
        RuntimeForcing rain;
        rain.channel = 1;
        rain.cell = 0;
        rain.mode = 1;
        rain.rate = .001;
        rain.concentrations = {-8};
        rain.receipt.requested_mass = {0};
        rain.receipt.applied_mass = {0};
        r.state.runtime_forcings.replace(rain);
        r.state.runtime_forcings.rebuild(2);
        const double initial = r.total(), energy = r.state.transport.totalIncludingLedgers(0) +
                                                   r.gw.transport().storage(0);
        ASSERT_NO_THROW(r.solver.advance(0, .735));
        boundary.edge_bc_flow[a] = 1000 / r.mesh.edge_length[a];
        boundary.edge_bc_flow[b] = 1000 / r.mesh.edge_length[b];
        ASSERT_NO_THROW(r.solver.advance(.735, 2));
        for (int cell = 0; cell < 2; ++cell) {
            const auto k = r.state.transport.idx(0, cell);
            EXPECT_GE(r.state.volume[cell], 0);
            if (r.state.volume[cell] > 1e-12) {
                const double temp = r.state.transport.cell_mass[k] / r.state.volume[cell];
                EXPECT_GE(temp, -8 - 1e-10);
                EXPECT_LE(temp, 6 + 1e-10);
            }
        }
        const auto& x = boundary.receipts.at(a);
        const auto& y = boundary.receipts.at(b);
        EXPECT_GT(x.water_in, 0);
        EXPECT_GT(x.water_out, 0);
        EXPECT_GT(y.water_out, 0);
        EXPECT_NEAR(r.total() - initial,
                    x.water_in + y.water_in - x.water_out - y.water_out +
                        r.state.rk2_rain_pending[0] + r.state.rk2_rain_pending[1],
                    1e-10);
        EXPECT_NEAR(r.state.transport.totalIncludingLedgers(0) + r.gw.transport().storage(0),
                    energy, 1e-10);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceBoundaryLaterHeadOverflowRollsBackEveryOwnerAndAllowsRetry) {
    CoupledRkRig r;
    r.init(.05, .003, .001, .2, "NONE");
    initRkQuality(r, false, true, true);
    seedRkQuality(r, 2);
    BoundaryData boundary;
    setRkSurfaceBoundary(r, boundary, 0, true, .22, {7, 3});
    const auto late = setRkSurfaceBoundary(r, boundary, 1, true, 1e308, {7, 3});
    const auto before = rk2Output("surface-boundary-overflow-before.chk"),
               after = rk2Output("surface-boundary-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    boundary.edge_bc_head[late] = .22;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}

TEST(CoupledGroundwaterRk2, SurfaceBoundaryChangedDescriptorAndCorruptReceiptRefuseAtomically) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, .2, "NONE");
        initRkQuality(*r, false, true, true);
        seedRkQuality(*r, 2);
    }
    const auto slot = setRkSurfaceBoundary(a, ba, 0, false, -.01, {7, 3});
    setRkSurfaceBoundary(b, bb, 0, false, -.01, {7, 3});
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto good = rk2Output("surface-boundary-corrupt-good.chk"),
               bad = rk2Output("surface-boundary-corrupt-bad.chk"),
               before = rk2Output("surface-boundary-corrupt-before.chk"),
               after = rk2Output("surface-boundary-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    for (double* value : {&bb.edge_bc_flow[slot], &b.state.transport.runtime_bc_conc[slot][0]}) {
        const auto old = *value;
        *value += .001;
        b.solver.refreshBoundaries();
        ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(b.solver.loadCheckpoint(good.string()).empty());
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        *value = old;
        b.solver.refreshBoundaries();
    }
    openswmm::detail::CompletedSourceArchive original(rk2File(good));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const std::string marker = "RK2 copied surface boundaries v1";
    const auto pos = payload.rfind(marker);
    ASSERT_NE(pos, std::string::npos);
    const auto offset = pos + marker.size() + sizeof(uint64_t) +
                        ba.edge_bc_cum_flux.size() * sizeof(double) + sizeof(double);
    ASSERT_LT(offset + sizeof(double), payload.size());
    double applied = 0;
    std::memcpy(&applied, payload.data() + offset, sizeof(applied));
    applied += 1;
    std::memcpy(payload.data() + offset, &applied, sizeof(applied));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("receipt"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 1));
    ASSERT_NO_THROW(b.solver.advance(.735, 1));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

TEST(CoupledGroundwaterRk2, SurfaceLatentVariableVolumeCoolingHasIndependentSecondOrderSolution) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig r;
        r.init(dt, 0, 0, .02, "NONE");
        RkPorousOwner owner;
        initRkAtmosphere(r, owner, true, false, true);
        r.opts.evaporation = 1;
        owner.context.options.wind_func_coeff_a = 0;
        for (auto& rate : r.state.evap_rate)
            rate = .0005;
        const auto file = rk2Output("surface-latent-initial.chk");
        ASSERT_EQ(r.solver.saveCheckpoint(file.string()), "");
        const double initial = rkFiniteEnergy(r), water = r.state.volume[0];
        for (int step = 0; step < int(std::round(2 / dt)); ++step)
            ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
        const double final_water = water - .0005 * r.mesh.tri_area[0] * 2;
        const double exact =
            2499000. / 2360 - (2499000. / 2360 - 20) * std::pow(water / final_water, 2360. / 4186);
        double error = 0, latent = 0, sensible = 0;
        for (int cell = 0; cell < 2; ++cell) {
            EXPECT_NEAR(r.state.volume[cell], final_water, 1e-10);
            const double temperature =
                r.state.transport.cell_mass[r.state.transport.idx(0, cell)] / r.state.volume[cell];
            error = std::max(error, std::abs(temperature - exact));
            latent += r.state.heat.latent_j[cell];
            EXPECT_NEAR(r.state.heat.evaporated_m3[cell], .0005 * r.mesh.tri_area[cell] * 2, 1e-12);
            EXPECT_NEAR(r.state.heat.atmospheric_j[cell], r.state.heat.latent_j[cell], 1e-10);
        }
        sensible = r.state.transport.lost_external[0] * 4186000;
        EXPECT_LT(latent, 0);
        EXPECT_NEAR(rkFiniteEnergy(r) - initial, latent - sensible,
                    1e-10 * std::max(1., std::abs(initial)));
        RecordProperty("surface_latent_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, .002);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
    }
}

TEST(CoupledGroundwaterRk2, SurfaceLatentDryingRetriesWholeIntervalsAndKeepsFiniteOwnedEnergy) {
    CoupledRkRig r;
    r.opts.dry_depth = .001;
    r.init(.2, 0, 0, .0011, "NONE");
    RkPorousOwner o;
    initRkAtmosphere(r, o, true, false, false);
    r.opts.evaporation = 1;
    o.context.options.wind_func_coeff_a = 0;
    r.state.evap_rate.assign(2, .001);
    const double initial = rkFiniteEnergy(r), water = r.total();
    ASSERT_NO_THROW(r.solver.advance(0, .2));
    EXPECT_GT(r.solver.last_num_steps(), 2);
    ASSERT_NO_THROW(r.solver.advance(.2, 1));
    double atmosphere = 0, evaporated = 0;
    for (int i = 0; i < 2; ++i) {
        EXPECT_GT(r.state.volume[i], 0);
        EXPECT_LE(r.state.depth[i], r.opts.dry_depth);
        const double temp = r.state.transport.cell_mass[i] / r.state.volume[i];
        EXPECT_TRUE(std::isfinite(temp));
        EXPECT_GT(temp, -273.15);
        atmosphere += r.state.heat.atmospheric_j[i];
        evaporated += r.state.heat.evaporated_m3[i];
    }
    EXPECT_NEAR(r.total() + evaporated, water, 1e-10);
    EXPECT_NEAR(rkFiniteEnergy(r) - initial,
                atmosphere - r.state.transport.lost_external[0] * 4186000,
                1e-10 * std::max(1., std::abs(initial)));
    const auto saved = rk2Output("surface-latent-dry.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(saved.string()), "");
}
TEST(CoupledGroundwaterRk2, SurfaceLatentWorkLimitRestoresEveryOwnerAndSupportsAValidRetry) {
    CoupledRkRig r;
    r.opts.dry_depth = 0;
    r.init(1, 0, 0, .001, "NONE");
    RkPorousOwner o;
    initRkAtmosphere(r, o, true, false, true);
    r.opts.evaporation = 1;
    o.context.options.wind_func_coeff_a = 0;
    r.state.evap_rate = {.000001, 1};
    const auto before = rk2Output("surface-latent-limit-before.chk"),
               after = rk2Output("surface-latent-limit-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, 1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    r.state.evap_rate.assign(2, .000001);
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}
TEST(CoupledGroundwaterRk2, SurfaceLatentNativeAndCopiedDemandContinueExactlyAndBindTheirPolicy) {
    CoupledRkRig a, b;
    RkPorousOwner oa, ob;
    for (auto* r : {&a, &b}) {
        r->init(.1, .003, .001, .02, "NONE");
        initRkAtmosphere(*r, r == &a ? oa : ob);
        r->opts.evaporation = 1;
        r->state.evap_rate.assign(2, .0001);
        setRkRuntimePair(*r, .001, 1.237);
        RuntimeForcing demand;
        demand.channel = 2;
        demand.cell = 0;
        demand.rate = .0002;
        demand.until = 1.237;
        demand.receipt.requested_mass = {0};
        demand.receipt.applied_mass = {0};
        r->state.runtime_forcings.replace(demand);
        r->state.runtime_forcings.rebuild(2);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto good = rk2Output("surface-latent-good.chk"),
               before = rk2Output("surface-latent-fresh.chk"),
               after = rk2Output("surface-latent-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    EXPECT_NE(rk2File(good).find("RK2 accepted surface latent cooling v1"), std::string::npos);
    b.state.evap_rate[1] = .0003;
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.state.evap_rate[1] = .0001;
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 2));
    ASSERT_NO_THROW(b.solver.advance(.735, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

namespace {
/**
 * @brief Integrate the shrinking-water atmospheric/contact/reaction/age reference.
 *
 * Uses evaporation 0.0002 m/s over area, water capacity 4186000 J/(m3 K), fixed incoming
 * radiation/weather and temperature-dependent A-to-B rate. Integrates local equations without
 * production heat/reaction calls. Positive remaining water and resolved trial temperatures are
 * preconditions; no dry-out/event handling, bound checks or adaptive refinement occurs.
 *
 * @param[in] volume Initial water volume in m3, greater than 0.0004*area so water remains positive
 *     for two seconds.
 * @param[in] soil Positive fixed ground thermal capacity in J/K.
 * @param[in] conductance Water-ground conductance in W/K.
 * @param[in] area Nonnegative atmospheric/evaporation area in m2.
 * @param[in] steps Positive classical-RK4 subdivision count over two seconds.
 * @param[in] mass_a Initial A amount in the fixture native concentration*m3 units.
 * @param[in] mass_b Initial B amount in the same units.
 * @param[in] age Initial water-age moment in seconds*m3, not a mean age.
 * @return Array: water/soil temperatures C; accumulated radiative/sensible/latent energy gains J;
 *     advected evaporation energy out J; final A/B amounts; final age moment seconds*m3.
 */
std::array<double, 9> surfaceLatentAtmosphereReference(double volume, double soil,
                                                       double conductance, double area, int steps,
                                                       double mass_a = 0, double mass_b = 0,
                                                       double age = 0) {
    std::array<double, 9> y{20, -2, 0, 0, 0, 0, mass_a, mass_b, age};
    const double dt = 2. / steps, evaporation = .0002 * area;
    const auto rate = [&](double time, const std::array<double, 9>& state) {
        const double water = 4186000 * (volume - evaporation * time), kelvin = state[0] + 273.15;
        const double latent = 1000 * (2499 - 2.36 * state[0]),
                     radiation = area * (5.67e-8 * kelvin * kelvin * kelvin * kelvin - 750);
        const double sensible = area * 1000 * latent * 1e-6 * .061 * (state[0] - 5),
                     vapour = 1000 * latent * evaporation;
        return std::array<double, 9>{
            (-conductance * (state[0] - state[1]) - radiation - sensible - vapour) / water,
            conductance * (state[0] - state[1]) / soil,
            -radiation,
            -sensible,
            -vapour,
            4186000 * state[0] * evaporation,
            -.2 * std::exp(.01 * state[0]) * state[6],
            .2 * std::exp(.01 * state[0]) * state[6],
            volume - evaporation * time - evaporation * state[8] / (volume - evaporation * time)};
    };
    for (int step = 0; step < steps; ++step) {
        const double t = step * dt;
        const auto a = rate(t, y);
        auto trial = y;
        for (int k = 0; k < 9; ++k)
            trial[k] = y[k] + .5 * dt * a[k];
        const auto b = rate(t + .5 * dt, trial);
        for (int k = 0; k < 9; ++k)
            trial[k] = y[k] + .5 * dt * b[k];
        const auto c = rate(t + .5 * dt, trial);
        for (int k = 0; k < 9; ++k)
            trial[k] = y[k] + dt * c[k];
        const auto d = rate(t + dt, trial);
        for (int k = 0; k < 9; ++k)
            y[k] += dt * (a[k] + 2 * b[k] + 2 * c[k] + d[k]) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2,
     SurfaceLatentFiniteSoilSensibleRadiationAndJoulesMatchAnIndependentReference) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig r;
        r.init(dt, 0, 0, .02, "NONE");
        RkPorousOwner o;
        initRkAtmosphere(r, o);
        r.opts.evaporation = 1;
        r.state.evap_rate.assign(2, .0002);
        const auto& col = r.gw.heat().cells[0].column;
        const double water = r.state.volume[0], capacity = 4186000 * water, soil = col.capacity[0],
                     area = col.area, g = area * col.conductivity[0];
        const auto reference = surfaceLatentAtmosphereReference(water, soil, g, area, 20000),
                   refined = surfaceLatentAtmosphereReference(water, soil, g, area, 40000);
        for (int k = 0; k < 6; ++k)
            ASSERT_NEAR(reference[k], refined[k],
                        k < 2 ? 1e-10 : 1e-10 * std::max(1., std::abs(reference[k])));
        const double initial = rkFiniteEnergy(r);
        for (int step = 0; step < int(std::round(2 / dt)); ++step)
            ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
        double error = 0, atmosphere = 0;
        for (int i = 0; i < 2; ++i) {
            const auto& h = r.state.heat;
            const auto& c = r.gw.heat().cells[i].column;
            const double temperature = r.state.transport.cell_mass[i] / r.state.volume[i];
            error = std::max({error, std::abs(temperature - reference[0]) / 20,
                              std::abs(c.energy[0] / soil - reference[1]) / 20,
                              std::abs(h.radiative_j[i] - reference[2]) / (20 * capacity),
                              std::abs(h.sensible_j[i] - reference[3]) / (20 * capacity),
                              std::abs(h.latent_j[i] - reference[4]) / (20 * capacity)});
            atmosphere += h.atmospheric_j[i];
            EXPECT_NEAR(h.atmospheric_j[i], h.radiative_j[i] + h.sensible_j[i] + h.latent_j[i],
                        1e-10 * std::max(1., std::abs(h.atmospheric_j[i])));
        }
        const double sensible = r.state.transport.lost_external[0] * 4186000;
        error = std::max(error, std::abs(sensible - 2 * reference[5]) / (40 * capacity));
        RecordProperty("surface_latent_finite_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        EXPECT_NEAR(rkFiniteEnergy(r) - initial, atmosphere - sensible,
                    1e-10 * std::max(1., std::abs(initial)));
        checkRkPorousLedgers(r);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceLatentFiniteGraphFullAndHalfStepsHaveThirdOrderDifference) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig a, b;
        RkPorousOwner oa, ob;
        for (auto* r : {&a, &b}) {
            r->init(dt, 0, 0, .02, "NONE");
            initRkAtmosphere(*r, r == &a ? oa : ob);
            r->opts.evaporation = 1;
            r->state.evap_rate.assign(2, .0002);
        }
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, .5 * dt));
        ASSERT_NO_THROW(b.solver.advance(.5 * dt, dt));
        const auto& ac = a.gw.heat().cells[0].column;
        const auto& bc = b.gw.heat().cells[0].column;
        const double error =
            std::hypot(a.state.transport.cell_mass[0] / a.state.volume[0] -
                           b.state.transport.cell_mass[0] / b.state.volume[0],
                       ac.energy[0] / ac.capacity[0] - bc.energy[0] / bc.capacity[0]);
        RecordProperty("surface_latent_local_" + std::to_string(int(dt * 1000)), error);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 6.5);
        previous = error;
    }
}

TEST(CoupledGroundwaterRk2,
     SurfaceLatentTemperatureDependentReactionAndAgeMatchAJointIndependentReference) {
    double previous = 0, previous_chemical = 0;
    for (double dt : {.2, .1, .05, .025}) {
        RkReactionSystem system(
            "* SAT A RATE 0\n* SAT B RATE 0\n* UNSAT A RATE 0\n* UNSAT B RATE 0\n", "RK5",
            "RATE A -.2*exp(.01*TEMP)*A\nRATE B .2*exp(.01*TEMP)*A\n");
        CoupledRkRig r;
        r.init(dt, 0, 0, .02, "NONE");
        RkPorousOwner owner;
        initRkFiniteQuality(r, owner, system, false, 1, 0);
        auto& ctx = system.context();
        owner.config.options.conduction = true;
        ctx.heat_config.sediment.solute_diffusivity = 0;
        ctx.heat_config.sediment.hyporheic_velocity = 0;
        r.opts.evaporation = 1;
        r.opts.heat_surface = r.opts.heat_radiative = 1;
        r.state.heat.surface = r.state.heat.radiative = true;
        r.state.evap_rate.assign(2, .0002);
        ctx.climate_state.temperature = 41;
        ctx.climate_state.wind_speed = 3;
        ctx.climate_state.humidity = 60;
        ctx.options.wind_func_coeff_a = 1e-6;
        ctx.options.wind_func_coeff_b = 0;
        ctx.options.pressure_ratio = 1;
        ctx.climate_state.radiation.shared_heat_forcing = true;
        ctx.climate_state.radiation.longwave = 250;
        ctx.heat_state.shortwave_now = 500;
        ctx.heat_state.cloud_now = 0;
        auto& rc = ctx.heat_config.radiative;
        rc.emiss_water = 1;
        rc.emiss_landcover = 0;
        rc.lw_reflection = 0;
        rc.sky_view = 1;
        rc.albedo = 0;
        rc.shade_factor = 0;
        auto& tr = r.state.transport;
        for (int i = 0; i < 2; ++i)
            tr.cell_mass[tr.idx(tr.temp_row, i)] = 20 * r.state.volume[i];
        const auto& col = r.gw.heat().cells[0].column;
        const double volume = r.state.volume[0], soil = col.capacity[0], area = col.area,
                     g = area * col.conductivity[0];
        const auto initial = finiteQualityInitial(r);
        const auto energy = [&]() {
            double e = rkPorousEnergy(r);
            for (int i = 0; i < 2; ++i)
                e += 4186000 * tr.cell_mass[tr.idx(tr.temp_row, i)];
            return e;
        };
        const double e0 = energy();
        const auto exact = surfaceLatentAtmosphereReference(volume, soil, g, area, 20000,
                                                            8 * volume, 0, 11 * volume),
                   refined = surfaceLatentAtmosphereReference(volume, soil, g, area, 40000,
                                                              8 * volume, 0, 11 * volume);
        for (int k = 0; k < 9; ++k)
            ASSERT_NEAR(exact[k], refined[k], 1e-10 * std::max(1., std::abs(exact[k])));
        for (int step = 0; step < int(std::round(2 / dt)); ++step)
            ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
        double error = 0, chemical = 0, atmosphere = 0;
        for (int i = 0; i < 2; ++i) {
            error = std::max(
                {error,
                 std::abs(tr.cell_mass[tr.idx(tr.temp_row, i)] / r.state.volume[i] - exact[0]) / 20,
                 std::abs(r.gw.heat().cells[i].column.energy[0] / soil - exact[1]) / 20,
                 std::abs(tr.cell_mass[tr.idx(tr.age_row, i)] - exact[8]) /
                     std::max(1., std::abs(exact[8]))});
            chemical =
                std::max({chemical, std::abs(tr.cell_mass[tr.idx(0, i)] - exact[6]) / (8 * volume),
                          std::abs(tr.cell_mass[tr.idx(1, i)] - exact[7]) / (8 * volume)});
            atmosphere += r.state.heat.atmospheric_j[i];
            EXPECT_NEAR(tr.cell_mass[tr.idx(0, i)] + tr.cell_mass[tr.idx(1, i)], 8 * volume, 1e-10);
        }
        error = std::max(error, chemical);
        RecordProperty("surface_latent_reactive_error_" + std::to_string(int(dt * 1000)), error);
        RecordProperty("surface_latent_chemical_error_" + std::to_string(int(dt * 1000)), chemical);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        if (previous_chemical > 0)
            EXPECT_GT(previous_chemical / chemical, 3.5);
        previous = error;
        previous_chemical = chemical;
        EXPECT_NEAR(energy() - e0, atmosphere - 4186000 * tr.lost_external[tr.temp_row],
                    1e-10 * std::max(1., std::abs(e0)));
        checkFiniteQualityBudgets(r, initial);
    }
}

TEST(CoupledGroundwaterRk2,
     SurfaceLatentClearedCopiedDemandRetainsRestartHistoryWithoutFurtherLatentLoss) {
    CoupledRkRig a, b;
    RkPorousOwner oa, ob;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .02, "NONE");
        initRkAtmosphere(*r, r == &a ? oa : ob);
        RuntimeForcing row;
        row.channel = 2;
        row.cell = 0;
        row.rate = .0002;
        row.receipt.requested_mass = {0};
        row.receipt.applied_mass = {0};
        r->state.runtime_forcings.replace(row);
        r->state.runtime_forcings.rebuild(2);
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    a.state.runtime_forcings.rows[0].enabled = false;
    const auto latent = a.state.heat.latent_j;
    const auto saved = rk2Output("surface-latent-cleared.chk"),
               left = rk2Output("surface-latent-cleared-left.chk"),
               right = rk2Output("surface-latent-cleared-right.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    EXPECT_NE(rk2File(saved).find("RK2 accepted surface latent cooling v1"), std::string::npos);
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 2));
    ASSERT_NO_THROW(b.solver.advance(.735, 2));
    EXPECT_EQ(a.state.heat.latent_j, latent);
    EXPECT_EQ(b.state.heat.latent_j, latent);
    ASSERT_EQ(a.solver.saveCheckpoint(left.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(right.string()), "");
    EXPECT_EQ(rk2File(left), rk2File(right));
}
TEST(CoupledGroundwaterRk2, SurfaceLatentChecksumValidNonfiniteEnergyRefusesBeforeInstallation) {
    CoupledRkRig a, b;
    RkPorousOwner oa, ob;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .02, "NONE");
        initRkAtmosphere(*r, r == &a ? oa : ob);
        r->opts.evaporation = 1;
        r->state.evap_rate = {.0002, .0001};
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto good = rk2Output("surface-latent-corrupt-good.chk"),
               bad = rk2Output("surface-latent-corrupt-bad.chk"),
               before = rk2Output("surface-latent-corrupt-before.chk"),
               after = rk2Output("surface-latent-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    openswmm::detail::CompletedSourceArchive original(rk2File(good));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const double joules = a.state.heat.latent_j[0];
    const std::string needle(reinterpret_cast<const char*>(&joules), sizeof(joules));
    const auto offset = payload.find(needle);
    ASSERT_NE(offset, std::string::npos);
    ASSERT_EQ(payload.find(needle, offset + 1), std::string::npos);
    const double invalid = std::numeric_limits<double>::quiet_NaN();
    std::memcpy(payload.data() + offset, &invalid, sizeof(invalid));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("thermal"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 1));
    ASSERT_NO_THROW(b.solver.advance(.735, 1));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

namespace {
/**
 * @brief Install a fixture boundary as authored state with static quality rows.
 *
 * Uses setRkSurfaceBoundary, then removes runtime original/owner/copied concentration records and
 * appends one authored BC quality row per species before refresh. Repeated calls can append
 * duplicate quality rows; intended fresh target state/valid indices are required. Does not advance
 * time or transfer boundary ownership.
 *
 * @param[in,out] r Initialized rig receiving boundary/quality/cache changes.
 * @param[in,out] boundary Caller-owned borrowed boundary object, retained by r.state.
 * @param[in] cell Valid mesh cell.
 * @param[in] stage True specifies stage, false signed outward flow.
 * @param[in] value Datum metres or outward m3/s according to stage.
 * @param[in] quality Full copied tuple with one entry per surface species.
 * @param[in] ordinal Exterior-edge ordinal.
 * @return Selected global edge slot.
 */
int setRkAuthoredSurfaceBoundary(CoupledRkRig& r, BoundaryData& boundary, int cell, bool stage,
                                 double value, const std::vector<double>& quality,
                                 int ordinal = 0) {
    const auto slot = setRkSurfaceBoundary(r, boundary, cell, stage, value, quality, ordinal);
    boundary.runtime_original.erase(slot);
    boundary.runtime_owner.erase(slot);
    r.state.transport.runtime_bc_conc.erase(slot);
    for (int row = 0; row < r.state.transport.n_species; ++row)
        r.state.transport.bc_quality_rows.push_back({slot, row, quality.at(row)});
    r.solver.refreshBoundaries();
    return slot;
}
}
TEST(CoupledGroundwaterRk2, AuthoredSurfaceConstantStageAndFlowMatchCopiedPhysicalTransactions) {
    for (const auto closure : {GwClosure::CLOSED_FORM, GwClosure::ENSLAVED, GwClosure::SIGMA})
        for (bool layered : {false, true})
            for (bool stage : {false, true}) {
                if (layered && closure != GwClosure::SIGMA)
                    continue;
                CoupledRkRig a, b;
                BoundaryData ba, bb;
                for (auto* r : {&a, &b}) {
                    r->init(.05, .003, .001, .2, "NONE", closure, SoilChar::GARDNER, 4);
                    initRkQuality(*r, layered, true, true);
                    seedRkQuality(*r, 2, .001);
                }
                const auto slot =
                    setRkAuthoredSurfaceBoundary(a, ba, 0, stage, stage ? .22 : -.01, {7, 3});
                setRkSurfaceBoundary(b, bb, 0, stage, stage ? .22 : -.01, {7, 3});
                ASSERT_EQ(
                    a.solver.saveCheckpoint(rk2Output("authored-surface-initial.chk").string()),
                    "");
                const double initial = a.total();
                ASSERT_NO_THROW(a.solver.advance(0, .735));
                ASSERT_NO_THROW(b.solver.advance(0, .735));
                EXPECT_EQ(a.state.volume, b.state.volume);
                EXPECT_EQ(a.state.transport.cell_mass, b.state.transport.cell_mass);
                EXPECT_EQ(a.gw.state().hg, b.gw.state().hg);
                EXPECT_EQ(a.gw.transport().sat_mass, b.gw.transport().sat_mass);
                EXPECT_EQ(ba.receipts.at(slot).applied, bb.receipts.at(slot).applied);
                EXPECT_EQ(ba.receipts.at(slot).applied_mass, bb.receipts.at(slot).applied_mass);
                EXPECT_NEAR(a.total() + a.gw.state().led_deep - initial,
                            ba.receipts.at(slot).applied, 1e-10);
            }
}

TEST(CoupledGroundwaterRk2,
     AuthoredSurfaceMixingHasIndependentSecondOrderChemicalAndAgeTrajectories) {
    double previous = 0;
    int uniform = 0;
    for (double dt : {.4, .2, .1, .05, .025}) {
        CoupledRkRig r;
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell)
            for (int edge = 0; edge < r.mesh.cell_vertex_count(cell); ++edge)
                if (r.mesh.cell_neighbour(cell, edge) >= 0)
                    r.mesh.edge_conveyance[MeshData::slot(cell, edge)] = 0;
        r.init(dt, 0, 0, .2, "NONE");
        initRkQuality(r, false, true, true);
        seedRkQuality(r, 2);
        BoundaryData boundary;
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell) {
            setRkAuthoredSurfaceBoundary(r, boundary, cell, false, -.1, {7, 3});
            setRkAuthoredSurfaceBoundary(r, boundary, cell, false, .1, {7, 3}, 1);
        }
        long evaluations = 0;
        for (int step = 0; step < int(std::round(2 / dt)); ++step) {
            ASSERT_NO_THROW(r.solver.advance(step * dt, (step + 1) * dt));
            evaluations += r.solver.last_num_steps();
        }
        RecordProperty("surface_boundary_evaluations_" + std::to_string(int(dt * 1000)),
                       evaluations);
        const double volume = .2 * r.mesh.tri_area[0], attenuation = std::exp(-.1 * 2 / volume),
                     chemical = 7 - 5 * attenuation,
                     age = 3 + volume / .1 + (5 - 3 - volume / .1) * attenuation;
        double error = 0;
        for (int cell = 0; cell < r.mesh.n_cells(); ++cell) {
            EXPECT_NEAR(r.state.volume[cell], volume, 1e-12);
            error = std::max(
                error,
                std::abs(r.state.transport.cell_mass[r.state.transport.idx(0, cell)] / volume -
                         chemical));
            error = std::max(
                error,
                std::abs(r.state.transport.cell_mass[r.state.transport.idx(1, cell)] / volume -
                         age));
        }
        RecordProperty("surface_boundary_mix_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, .0002);
        const long expected = 2 * int(std::round(2 / dt));
        if (evaluations != expected) {
            EXPECT_EQ(dt, .4);
            EXPECT_GT(evaluations, expected);
        } else {
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            ++uniform;
        }
        for (int row = 0; row < 2; ++row)
            EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row),
                        row == 1 ? 5 * 2 * volume : 2 * 2 * volume, 1e-10);
    }
    EXPECT_EQ(uniform, 4);
}

TEST(CoupledGroundwaterRk2, AuthoredSurfaceStageFullAndHalfStepsHaveThirdOrderLocalDifference) {
    double previous = 0;
    for (double dt : {.08, .04, .02, .01}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        for (auto* r : {&a, &b}) {
            for (auto& n : r->mesh.mannings_n)
                n = 0;
            r->init(dt, 0, 0, .2, "NONE");
            initRkQuality(*r, false, true, true);
            seedRkQuality(*r, 2);
        }
        setRkAuthoredSurfaceBoundary(a, ba, 0, true, .22, {7, 3});
        setRkAuthoredSurfaceBoundary(b, bb, 0, true, .22, {7, 3});
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, dt / 2));
        ASSERT_NO_THROW(b.solver.advance(dt / 2, dt));
        double error = 0;
        for (int cell = 0; cell < a.mesh.n_cells(); ++cell)
            error = std::max(error, std::abs(a.state.volume[cell] - b.state.volume[cell]));
        RecordProperty("surface_boundary_stage_local_" + std::to_string(int(dt * 1000)), error);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 6.5);
        previous = error;
    }
}

TEST(CoupledGroundwaterRk2, AuthoredSurfaceColdReversalAndCompetingExportsProtectBeginningDonors) {
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(r, layered, 6, 6);
        BoundaryData boundary;
        const auto a = setRkAuthoredSurfaceBoundary(r, boundary, 0, false, -.1, {-8});
        const auto b = setRkAuthoredSurfaceBoundary(r, boundary, 0, false, .1, {-8}, 1);
        RuntimeForcing rain;
        rain.channel = 1;
        rain.cell = 0;
        rain.mode = 1;
        rain.rate = .001;
        rain.concentrations = {-8};
        rain.receipt.requested_mass = {0};
        rain.receipt.applied_mass = {0};
        r.state.runtime_forcings.replace(rain);
        r.state.runtime_forcings.rebuild(2);
        const double initial = r.total(), energy = r.state.transport.totalIncludingLedgers(0) +
                                                   r.gw.transport().storage(0);
        ASSERT_NO_THROW(r.solver.advance(0, .735));
        boundary.edge_bc_flow[a] = 1000 / r.mesh.edge_length[a];
        boundary.edge_bc_flow[b] = 1000 / r.mesh.edge_length[b];
        ASSERT_NO_THROW(r.solver.advance(.735, 2));
        for (int cell = 0; cell < 2; ++cell) {
            const auto k = r.state.transport.idx(0, cell);
            EXPECT_GE(r.state.volume[cell], 0);
            if (r.state.volume[cell] > 1e-12) {
                const double temp = r.state.transport.cell_mass[k] / r.state.volume[cell];
                EXPECT_GE(temp, -8 - 1e-10);
                EXPECT_LE(temp, 6 + 1e-10);
            }
        }
        const auto& x = boundary.receipts.at(a);
        const auto& y = boundary.receipts.at(b);
        EXPECT_GT(x.water_in, 0);
        EXPECT_GT(x.water_out, 0);
        EXPECT_GT(y.water_out, 0);
        EXPECT_NEAR(r.total() - initial,
                    x.water_in + y.water_in - x.water_out - y.water_out +
                        r.state.rk2_rain_pending[0] + r.state.rk2_rain_pending[1],
                    1e-10);
        EXPECT_NEAR(r.state.transport.totalIncludingLedgers(0) + r.gw.transport().storage(0),
                    energy, 1e-10);
    }
}

TEST(CoupledGroundwaterRk2, AuthoredSurfaceLaterHeadOverflowRollsBackEveryOwnerAndAllowsRetry) {
    CoupledRkRig r;
    r.init(.05, .003, .001, .2, "NONE");
    initRkQuality(r, false, true, true);
    seedRkQuality(r, 2);
    BoundaryData boundary;
    setRkAuthoredSurfaceBoundary(r, boundary, 0, true, .22, {7, 3});
    const auto late = setRkAuthoredSurfaceBoundary(r, boundary, 1, true, 1e308, {7, 3});
    const auto before = rk2Output("authored-surface-overflow-before.chk"),
               after = rk2Output("authored-surface-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, .1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    boundary.edge_bc_head[late] = .22;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
}

TEST(CoupledGroundwaterRk2, AuthoredSurfaceChangedDescriptorAndCorruptReceiptRefuseAtomically) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, .2, "NONE");
        initRkQuality(*r, false, true, true);
        seedRkQuality(*r, 2);
    }
    const auto slot = setRkAuthoredSurfaceBoundary(a, ba, 0, false, -.01, {7, 3});
    setRkAuthoredSurfaceBoundary(b, bb, 0, false, -.01, {7, 3});
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto good = rk2Output("authored-surface-corrupt-good.chk"),
               bad = rk2Output("authored-surface-corrupt-bad.chk"),
               before = rk2Output("authored-surface-corrupt-before.chk"),
               after = rk2Output("authored-surface-corrupt-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    for (double* value : {&bb.edge_bc_flow[slot], &b.state.transport.bc_quality_rows[0].conc}) {
        const auto old = *value;
        *value += .001;
        b.solver.refreshBoundaries();
        ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(b.solver.loadCheckpoint(good.string()).empty());
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        *value = old;
        b.solver.refreshBoundaries();
    }
    openswmm::detail::CompletedSourceArchive original(rk2File(good));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const std::string marker = "RK2 authored surface boundaries v1";
    const auto pos = payload.rfind(marker);
    ASSERT_NE(pos, std::string::npos);
    const auto offset = pos + marker.size() + sizeof(uint64_t) +
                        ba.edge_bc_cum_flux.size() * sizeof(double) + sizeof(double);
    ASSERT_LT(offset + sizeof(double), payload.size());
    double applied = 0;
    std::memcpy(&applied, payload.data() + offset, sizeof(applied));
    applied += 1;
    std::memcpy(payload.data() + offset, &applied, sizeof(applied));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("receipt"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 1));
    ASSERT_NO_THROW(b.solver.advance(.735, 1));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

TEST(CoupledGroundwaterRk2, AuthoredSurfaceOverrideClearAndHiddenQualityAreRestartBound) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(*r, true, 6, 6);
        auto& boundary = r == &a ? ba : bb;
        setRkAuthoredSurfaceBoundary(*r, boundary, 0, false, -.001, {-8});
        setRkSurfaceBoundary(*r, boundary, 0, false, -.002, {-4});
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto saved = rk2Output("authored-surface-hidden.chk"),
               before = rk2Output("authored-surface-hidden-before.chk"),
               after = rk2Output("authored-surface-hidden-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    b.state.transport.bc_quality_rows[0].conc = -7;
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    b.state.transport.bc_quality_rows[0].conc = -8;
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    const auto slot = ba.runtime_original.begin()->first;
    for (auto* r : {&a, &b}) {
        auto& boundary = r == &a ? ba : bb;
        boundary.clearRuntime(slot);
        r->state.transport.runtime_bc_conc.erase(slot);
        r->solver.refreshBoundaries();
    }
    const double old = ba.receipts.at(slot).applied_mass[0];
    ASSERT_NO_THROW(a.solver.advance(.735, 2));
    ASSERT_NO_THROW(b.solver.advance(.735, 2));
    EXPECT_NEAR(ba.receipts.at(slot).applied_mass[0] - old, -8 * .001 * (2 - .735), 1e-12);
    EXPECT_NEAR(ba.receipts.at(slot).heat_applied, ba.receipts.at(slot).applied_mass[0] * 4186000,
                1e-8);
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

TEST(CoupledGroundwaterRk2,
     AuthoredSurfaceUnspecifiedIncomingQualityUsesCleanWaterWithoutInventingSpecies) {
    CoupledRkRig r;
    r.init(.05, 0, 0, .2, "NONE");
    initRkQuality(r, false, true, true);
    seedRkQuality(r, 2);
    BoundaryData boundary;
    const auto slot = setRkAuthoredSurfaceBoundary(r, boundary, 0, false, -.01, {7, 3});
    r.state.transport.bc_quality_rows.clear();
    r.solver.refreshBoundaries();
    const double initial = r.total();
    ASSERT_NO_THROW(r.solver.advance(0, .735));
    EXPECT_NEAR(r.total() - initial, .00735, 1e-10);
    EXPECT_EQ(boundary.receipts.at(slot).applied_mass, (std::vector<double>{0, 0}));
    for (int row = 0; row < 2; ++row)
        EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row), row == 0 ? 2 * 3.2 : 5 * 3.2,
                    1e-10);
}

namespace {
/**
 * @brief Attach a dated stage/flow series to an authored surface boundary.
 *
 * Seeds boundary from values.front, adds a slot-derived series name and native calendar-day
 * abscissae, then assigns stage/flow series index/name. Does not validate lengths/order, clear a
 * reused table or call refresh after assigning the series fields; caller uses a fresh compatible
 * table/context and follows normal test scheduling.
 *
 * @param[in,out] r Initialized rig receiving authored boundary/cache state.
 * @param[in,out] boundary Caller-owned boundary object retained by the rig.
 * @param[in,out] context Live context owning the added time-series table; must remain available
 *     during evaluation.
 * @param[in] cell Valid mesh cell.
 * @param[in] stage True series values are datum stage, false total outward flow.
 * @param[in] times Nonempty sorted elapsed seconds, count matching values.
 * @param[in] values Matching nonempty stage metres or outward m3/s; flow ordinates are divided by
 *     edge length.
 * @param[in] quality Full constant inflow quality tuple copied to authored rows.
 * @param[in] ordinal Exterior-edge ordinal.
 * @return Selected global edge slot.
 */
int setRkSurfaceSignal(CoupledRkRig& r, BoundaryData& boundary,
                       openswmm::SimulationContext& context, int cell, bool stage,
                       const std::vector<double>& times, const std::vector<double>& values,
                       const std::vector<double>& quality, int ordinal = 0) {
    const auto slot =
        setRkAuthoredSurfaceBoundary(r, boundary, cell, stage, values.front(), quality, ordinal);
    const auto name = "surface-signal-" + std::to_string(slot);
    const int index = context.tables.add(name, openswmm::TableType::TIMESERIES);
    auto& table = context.tables.tables[index];
    for (std::size_t k = 0; k < times.size(); ++k) {
        table.x.push_back(context.options.start_date + times[k] / 86400.);
        table.y.push_back(stage ? values[k] : values[k] / r.mesh.edge_length[slot]);
    }
    (stage ? boundary.edge_bc_tseries : boundary.edge_bc_flow_tseries)[slot] = index;
    (stage ? boundary.edge_bc_tseries_name : boundary.edge_bc_flow_tseries_name)[slot] = name;
    return slot;
}
/**
 * @brief Integrate the uniform polygon-stage scalar reference independently of production flux
 * code.
 *
 * Uses characteristic stage ghost depth 0.21+0.005*t, gravity 9.80665 m/s2 and the subcritical HLL
 * scalar mass-law reduction after momentum cancellation. Caller remains in the intended
 * positive-depth/subcritical regime; square roots/divisions are unguarded and no production solver
 * calls or state mutation occurs.
 *
 * @param[in] perimeter Positive exterior perimeter in metres.
 * @param[in] area Positive cell area in m2.
 * @param[in] steps Positive uniform classical-RK4 subdivision count over two seconds.
 * @return Final uniform depth in metres from initial 0.2 m.
 */
double independentSurfaceStageDepth(double perimeter, double area, int steps) {
    const auto derivative = [&](double t, double depth) {
        // Uniform characteristic stage ghosts on a closed polygon cancel
        // momentum. Their subcritical HLL mass law reduces to this scalar ODE.
        const double ghost = .21 + .005 * t, ci = std::sqrt(9.80665 * depth),
                     cg = std::sqrt(9.80665 * ghost);
        const double flux = (2 * ci - 3 * cg) * (ghost * cg - (2 * ci - cg) * depth) / (2 * cg);
        return -perimeter * flux / area;
    };
    double y = .2;
    const double dt = 2. / steps;
    for (int k = 0; k < steps; ++k) {
        const double t = k * dt, a = derivative(t, y), b = derivative(t + .5 * dt, y + .5 * dt * a),
                     c = derivative(t + .5 * dt, y + .5 * dt * b),
                     d = derivative(t + dt, y + dt * c);
        y += dt * (a + 2 * b + 2 * c + d) / 6;
    }
    return y;
}
}
TEST(CoupledGroundwaterRk2, SurfaceSeriesStageMatchesAnIndependentCharacteristicMassOde) {
    double previous = 0;
    for (double dt : {.2, .1, .05, .025}) {
        CoupledRkRig r;
        for (auto& neighbour : r.mesh.cell_nbr)
            neighbour = -1;
        for (auto& n : r.mesh.mannings_n)
            n = 0;
        r.init(dt, 0, 0, .2, "NONE");
        initRkQuality(r, false, true, true);
        seedRkQuality(r, 2);
        BoundaryData boundary;
        openswmm::SimulationContext context;
        context.options.start_date = 0;
        for (int cell = 0; cell < 2; ++cell)
            for (int edge = 0; edge < 3; ++edge)
                setRkSurfaceSignal(r, boundary, context, cell, true, {0, 2}, {.21, .22}, {7, 3},
                                   edge);
        for (auto& table : context.tables.tables) {
            table.cursor.index = 1;
            table.cursor.direction = -1;
        }
        ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
        double perimeter = 0;
        for (int edge = 0; edge < 3; ++edge)
            perimeter += r.mesh.edge_length[MeshData::slot(0, edge)];
        const auto exact = independentSurfaceStageDepth(perimeter, r.mesh.tri_area[0], 20000),
                   refined = independentSurfaceStageDepth(perimeter, r.mesh.tri_area[0], 40000);
        ASSERT_NEAR(exact, refined, 1e-12);
        long evaluations = 0;
        for (int k = 0; k < int(std::round(2 / dt)); ++k) {
            ASSERT_NO_THROW(r.solver.advance(k * dt, (k + 1) * dt));
            evaluations += r.solver.last_num_steps();
        }
        EXPECT_EQ(evaluations, 2 * int(std::round(2 / dt)));
        double error = 0;
        for (int cell = 0; cell < 2; ++cell)
            error = std::max(error, std::abs(r.state.depth[cell] - exact));
        RecordProperty("surface_series_stage_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        for (const auto& table : context.tables.tables) {
            EXPECT_EQ(table.cursor.index, 1);
            EXPECT_EQ(table.cursor.direction, -1);
        }
        for (int row = 0; row < 2; ++row)
            EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row), row == 0 ? 2 * 3.2 : 5 * 3.2,
                        1e-10);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceSeriesFlowMixingHasIndependentChemicalAndAgeRefinements) {
    const double volume = 1.6;
    const auto age_reference = [&](int steps) {
        double age = 5;
        const double dt = 2. / steps;
        const auto rate = [&](double t, double value) {
            return 1 + (.08 + .01 * t) / volume * (3 - value);
        };
        for (int k = 0; k < steps; ++k) {
            const double t = k * dt, a = rate(t, age), b = rate(t + .5 * dt, age + .5 * dt * a),
                         c = rate(t + .5 * dt, age + .5 * dt * b), d = rate(t + dt, age + dt * c);
            age += dt * (a + 2 * b + 2 * c + d) / 6;
        }
        return age;
    };
    const double age = age_reference(20000);
    ASSERT_NEAR(age, age_reference(40000), 1e-12);
    const double chemical = 7 - 5 * std::exp(-(.08 * 2 + .005 * 4) / volume);
    double previous = 0;
    for (double dt : {.1, .05, .025, .0125}) {
        CoupledRkRig r;
        for (auto& factor : r.mesh.edge_conveyance)
            factor = 0;
        r.init(dt, 0, 0, .2, "NONE");
        initRkQuality(r, false, true, true);
        seedRkQuality(r, 2);
        BoundaryData boundary;
        openswmm::SimulationContext context;
        context.options.start_date = 0;
        for (int cell = 0; cell < 2; ++cell) {
            const int in =
                setRkSurfaceSignal(r, boundary, context, cell, false, {0, 2}, {-.08, -.10}, {7, 3});
            const int out = setRkSurfaceSignal(r, boundary, context, cell, false, {0, 2},
                                               {.08, .10}, {7, 3}, 1);
            r.mesh.edge_conveyance[in] = r.mesh.edge_conveyance[out] = 1;
        }
        ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
        long evaluations = 0;
        for (int k = 0; k < int(std::round(2 / dt)); ++k) {
            ASSERT_NO_THROW(r.solver.advance(k * dt, (k + 1) * dt));
            evaluations += r.solver.last_num_steps();
        }
        EXPECT_EQ(evaluations, 2 * int(std::round(2 / dt)));
        double error = 0;
        for (int cell = 0; cell < 2; ++cell) {
            EXPECT_NEAR(r.state.volume[cell], volume, 1e-12);
            error = std::max(
                {error,
                 std::abs(r.state.transport.cell_mass[r.state.transport.idx(0, cell)] / volume -
                          chemical),
                 std::abs(r.state.transport.cell_mass[r.state.transport.idx(1, cell)] / volume -
                          age)});
        }
        RecordProperty("surface_series_flow_error_" + std::to_string(int(dt * 1000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GT(previous / error, 3.5);
        previous = error;
        for (int row = 0; row < 2; ++row)
            EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row),
                        row == 0 ? 2 * 2 * volume : 5 * 2 * volume, 1e-10);
    }
}

TEST(CoupledGroundwaterRk2, SurfaceSeriesColdReversalHasIndependentGrossWaterAndPhysicalEnergy) {
    const std::vector<double> times{0, .735, 1.237, 2}, flow{-.012, .008, -.004, .006};
    double incoming = 0, outgoing = 0;
    for (std::size_t k = 1; k < times.size(); ++k) {
        const double h = times[k] - times[k - 1], a = flow[k - 1], b = flow[k];
        if (a * b >= 0) {
            const double volume = .5 * (a + b) * h;
            if (volume < 0)
                incoming -= volume;
            else
                outgoing += volume;
        } else {
            const double fraction = std::abs(a) / (std::abs(a) + std::abs(b));
            const double first = .5 * a * h * fraction, last = .5 * b * h * (1 - fraction);
            incoming -= std::min(0., first) + std::min(0., last);
            outgoing += std::max(0., first) + std::max(0., last);
        }
    }
    for (bool layered : {false, true}) {
        CoupledRkRig r;
        r.init(.2, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(r, layered, 6, 6);
        BoundaryData boundary;
        openswmm::SimulationContext context;
        context.options.start_date = 0;
        const auto slot = setRkSurfaceSignal(r, boundary, context, 0, false, times, flow, {-8});
        ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
        const double water = r.total(), energy = 4186000 * (r.state.transport.cell_mass[0] +
                                                            r.state.transport.cell_mass[1] +
                                                            r.gw.transport().storage(0));
        ASSERT_NO_THROW(r.solver.advance(0, 2));
        const auto& receipt = boundary.receipts.at(slot);
        EXPECT_NEAR(receipt.water_in, incoming, 1e-12);
        EXPECT_NEAR(receipt.water_out, outgoing, 1e-12);
        EXPECT_NEAR(r.total() - water, incoming - outgoing, 1e-10);
        const double final =
            4186000 * (r.state.transport.cell_mass[0] + r.state.transport.cell_mass[1] +
                       r.gw.transport().storage(0));
        EXPECT_NEAR(final - energy, receipt.heat_applied, 1e-10 * std::max(1., std::abs(energy)));
        EXPECT_NEAR(receipt.heat_applied, 4186000 * receipt.applied_mass[0], 1e-8);
        for (int cell = 0; cell < 2; ++cell) {
            const double temp = r.state.transport.cell_mass[cell] / r.state.volume[cell];
            EXPECT_GE(temp, -8 - 1e-10);
            EXPECT_LE(temp, 6 + 1e-10);
        }
        ASSERT_EQ(r.solver.saveCheckpoint(rk2Output("surface-series-cold.chk").string()), "");
    }
}
TEST(CoupledGroundwaterRk2, SurfaceSeriesSmoothStageFullAndHalfStepsHaveThirdOrderDifference) {
    double previous = 0;
    for (double dt : {.08, .04, .02, .01}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        openswmm::SimulationContext ca, cb;
        ca.options.start_date = cb.options.start_date = 0;
        for (auto* r : {&a, &b}) {
            for (auto& neighbour : r->mesh.cell_nbr)
                neighbour = -1;
            for (auto& n : r->mesh.mannings_n)
                n = 0;
            r->init(dt, 0, 0, .2, "NONE");
            initRkQuality(*r, false, true, true);
            seedRkQuality(*r, 2);
            auto& boundary = r == &a ? ba : bb;
            auto& context = r == &a ? ca : cb;
            for (int cell = 0; cell < 2; ++cell)
                for (int edge = 0; edge < 3; ++edge)
                    setRkSurfaceSignal(*r, boundary, context, cell, true, {0, 2}, {.21, .22},
                                       {7, 3}, edge);
            ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, 1, 1), "");
        }
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, .5 * dt));
        ASSERT_NO_THROW(b.solver.advance(.5 * dt, dt));
        const double error = std::abs(a.state.depth[0] - b.state.depth[0]);
        RecordProperty("surface_series_stage_local_" + std::to_string(int(dt * 1000)), error);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 6.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, SurfaceSeriesSignalUnitConversionsPreserveTheSamePhysicalTrajectory) {
    for (bool stage : {false, true}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        openswmm::SimulationContext ca, cb;
        ca.options.start_date = cb.options.start_date = 0;
        for (auto* r : {&a, &b}) {
            r->init(.05, 0, 0, .2, "NONE");
            initRkQuality(*r, false, true, true);
            seedRkQuality(*r, 2);
            auto& boundary = r == &a ? ba : bb;
            auto& context = r == &a ? ca : cb;
            setRkSurfaceSignal(*r, boundary, context, 0, stage, {0, .735, 2},
                               stage ? std::vector<double>{.21, .22, .215}
                                     : std::vector<double>{-.001, .002, -.001},
                               {7, 3});
            const double scale = r == &a ? 1 : stage ? .3048 : .028316846592;
            if (r == &b)
                for (auto& table : context.tables.tables)
                    for (auto& value : table.y)
                        value /= scale;
            ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, stage ? scale : 1,
                                                            stage ? 1 : scale),
                      "");
            ASSERT_NO_THROW(r->solver.advance(0, 2));
        }
        for (int cell = 0; cell < 2; ++cell)
            EXPECT_NEAR(a.state.volume[cell], b.state.volume[cell], 1e-13);
        for (std::size_t k = 0; k < a.state.transport.cell_mass.size(); ++k)
            EXPECT_NEAR(a.state.transport.cell_mass[k], b.state.transport.cell_mass[k], 1e-13);
    }
}
TEST(CoupledGroundwaterRk2,
     SurfaceSeriesChangedSignalAnchorUnitsAndCorruptEvaluatedStateRefuseAtomically) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    openswmm::SimulationContext ca, cb;
    ca.options.start_date = cb.options.start_date = 0;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(*r, true, 6, 6);
        auto& boundary = r == &a ? ba : bb;
        auto& context = r == &a ? ca : cb;
        setRkSurfaceSignal(*r, boundary, context, 0, false, {0, 2}, {-.001, -.002}, {-8});
        ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, 1, 1), "");
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto good = rk2Output("surface-series-good.chk"),
               bad = rk2Output("surface-series-corrupt.chk"),
               before = rk2Output("surface-series-before.chk"),
               after = rk2Output("surface-series-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 3; ++mutation) {
        const auto original = cb.tables.tables[0].y;
        if (mutation == 0)
            cb.tables.tables[0].y.back() *= 1.1;
        if (mutation == 1)
            cb.options.start_date = .25;
        ASSERT_EQ(b.solver.configureRk2BoundarySignals(cb, 1, mutation == 2 ? .5 : 1), "");
        ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(b.solver.loadCheckpoint(good.string()).empty());
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        cb.tables.tables[0].y = original;
        cb.options.start_date = 0;
        ASSERT_EQ(b.solver.configureRk2BoundarySignals(cb, 1, 1), "");
    }
    openswmm::detail::CompletedSourceArchive original(rk2File(good));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    const auto slot = ba.receipts.begin()->first;
    double flow = ba.edge_bc_flow[slot];
    const std::string needle(reinterpret_cast<const char*>(&flow), sizeof(flow));
    const auto offset = payload.find(needle);
    ASSERT_NE(offset, std::string::npos);
    ASSERT_EQ(payload.find(needle, offset + 1), std::string::npos);
    flow += .000123;
    std::memcpy(payload.data() + offset, &flow, sizeof(flow));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("completed clock"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 2));
    ASSERT_NO_THROW(b.solver.advance(.735, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2,
     SurfaceSeriesLateHeadFailureRestoresEvaluatedFieldsAndAllowsAnOverrideRetry) {
    CoupledRkRig r;
    r.init(.05, 0, 0, .2, "NONE");
    initRkQuality(r, false, true, true);
    seedRkQuality(r, 2);
    BoundaryData boundary;
    openswmm::SimulationContext context;
    context.options.start_date = 0;
    const auto slot = setRkSurfaceSignal(r, boundary, context, 0, true, {0, .735, 1.237},
                                         {.21, .22, 1e308}, {7, 3});
    ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
    const auto before = rk2Output("surface-series-overflow-before.chk"),
               after = rk2Output("surface-series-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, 1.5), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    setRkSurfaceBoundary(r, boundary, 0, true, .22, {7, 3});
    boundary.edge_bc_tseries[slot] = -1;
    boundary.edge_bc_tseries_name[slot].clear();
    r.solver.refreshBoundaries();
    ASSERT_NO_THROW(r.solver.advance(0, 1.5));
}

TEST(CoupledGroundwaterRk2, SurfaceSeriesCopiedOverrideAndClearKeepExactComponentHistory) {
    for (bool stage : {false, true}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        openswmm::SimulationContext ca, cb;
        ca.options.start_date = cb.options.start_date = 0;
        int slot = -1;
        const auto override = [&](CoupledRkRig& r, BoundaryData& boundary) {
            setRkSurfaceBoundary(r, boundary, 0, stage, stage ? .23 : -.003, {-4});
            boundary.edge_bc_tseries[slot] = boundary.edge_bc_flow_tseries[slot] = -1;
            boundary.edge_bc_tseries_name[slot].clear();
            boundary.edge_bc_flow_tseries_name[slot].clear();
            r.solver.refreshBoundaries();
        };
        for (auto* r : {&a, &b}) {
            r->init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkTemperature(*r, true, 6, 6);
            auto& boundary = r == &a ? ba : bb;
            auto& context = r == &a ? ca : cb;
            slot = setRkSurfaceSignal(
                *r, boundary, context, 0, stage, {0, 2},
                stage ? std::vector<double>{.21, .22} : std::vector<double>{-.001, -.002}, {-8});
            ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, 1, 1), "");
        }
        ASSERT_NO_THROW(a.solver.advance(0, .735));
        override(a, ba);
        override(b, bb);
        const auto good = rk2Output("surface-series-override.chk"),
                   left = rk2Output("surface-series-override-left.chk"),
                   right = rk2Output("surface-series-override-right.chk");
        ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
        ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
        ASSERT_NO_THROW(a.solver.advance(.735, 1.237));
        ASSERT_NO_THROW(b.solver.advance(.735, 1.237));
        ASSERT_EQ(a.solver.saveCheckpoint(left.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(right.string()), "");
        EXPECT_EQ(rk2File(left), rk2File(right));
        for (auto* r : {&a, &b}) {
            auto& boundary = r == &a ? ba : bb;
            boundary.clearRuntime(slot);
            r->state.transport.runtime_bc_conc.erase(slot);
            r->solver.refreshBoundaries();
        }
        ASSERT_NO_THROW(a.solver.advance(1.237, 2));
        ASSERT_NO_THROW(b.solver.advance(1.237, 2));
        ASSERT_EQ(a.solver.saveCheckpoint(left.string()), "");
        ASSERT_EQ(b.solver.saveCheckpoint(right.string()), "");
        EXPECT_EQ(rk2File(left), rk2File(right));
    }
}

namespace {
/**
 * @brief Replace an authored zero-flow edge with normal-flow slope behavior.
 *
 * Uses setRkAuthoredSurfaceBoundary then NORMAL_FLOW type/slope and refreshBoundaries. Does not
 * independently validate roughness/slope/tuple sizes or advance time.
 *
 * @param[in,out] r Initialized rig receiving boundary/cache state.
 * @param[in,out] boundary Caller-owned boundary object retained by the rig.
 * @param[in] cell Valid cell.
 * @param[in] slope Native dimensionless bed slope copied without validation.
 * @param[in] quality Full copied authored quality tuple.
 * @param[in] ordinal Exterior-edge ordinal.
 * @return Selected global slot.
 */
int setRkNormalBoundary(CoupledRkRig& r, BoundaryData& boundary, int cell, double slope,
                        const std::vector<double>& quality, int ordinal = 0) {
    const auto slot = setRkAuthoredSurfaceBoundary(r, boundary, cell, false, 0, quality, ordinal);
    boundary.edge_bc_type[slot] = int8_t(BoundaryType::NORMAL_FLOW);
    boundary.edge_bed_slope[slot] = slope;
    r.solver.refreshBoundaries();
    return slot;
}
}
TEST(CoupledGroundwaterRk2, SurfaceStateNormalFlowMatchesIndependentSignedManningStorage) {
    for (double slope : {.0001, -.0001}) {
        double previous = 0;
        for (double dt : {.1, .05, .025, .0125}) {
            CoupledRkRig r;
            for (auto& neighbour : r.mesh.cell_nbr)
                neighbour = -1;
            r.init(dt, 0, 0, .2, "NONE");
            initRkQuality(r, false, true, true);
            seedRkQuality(r, 2);
            BoundaryData boundary;
            for (int cell = 0; cell < 2; ++cell)
                for (int edge = 0; edge < 3; ++edge)
                    setRkNormalBoundary(r, boundary, cell, slope, {7, 3}, edge);
            ASSERT_EQ(
                r.solver.saveCheckpoint(rk2Output("surface-state-normal-initial.chk").string()),
                "");
            double perimeter = 0;
            for (int edge = 0; edge < 3; ++edge)
                perimeter += r.mesh.edge_length[MeshData::slot(0, edge)];
            const double coefficient = std::copysign(std::sqrt(std::abs(slope)) / .03, slope) *
                                       perimeter / r.mesh.tri_area[0];
            const double exact = std::pow(std::pow(.2, -2. / 3) + (2. / 3) * coefficient * 2, -1.5),
                         water = r.total();
            long evaluations = 0;
            for (int k = 0; k < int(std::round(2 / dt)); ++k) {
                ASSERT_NO_THROW(r.solver.advance(k * dt, (k + 1) * dt));
                evaluations += r.solver.last_num_steps();
            }
            EXPECT_EQ(evaluations, 2 * int(std::round(2 / dt)));
            double error = 0, external = 0;
            for (int cell = 0; cell < 2; ++cell)
                error = std::max(error, std::abs(r.state.depth[cell] - exact));
            for (const auto& row : boundary.receipts)
                external += row.second.applied;
            EXPECT_NEAR(r.total() - water, external, 1e-10);
            RecordProperty(std::string("surface_normal_") + (slope < 0 ? "in_" : "out_") +
                               std::to_string(int(dt * 1000)),
                           error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            for (int row = 0; row < 2; ++row)
                EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row),
                            row == 0 ? 2 * 3.2 : 5 * 3.2, 1e-10);
        }
    }
}

TEST(CoupledGroundwaterRk2, SurfaceStateNormalFlowHasThirdOrderLocalDifferences) {
    double previous = 0;
    for (double dt : {.08, .04, .02, .01}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        for (auto* r : {&a, &b}) {
            for (auto& neighbour : r->mesh.cell_nbr)
                neighbour = -1;
            r->init(dt, 0, 0, .2, "NONE");
            auto& boundary = r == &a ? ba : bb;
            for (int cell = 0; cell < 2; ++cell)
                for (int edge = 0; edge < 3; ++edge)
                    setRkNormalBoundary(*r, boundary, cell, .0001, {}, edge);
        }
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, .5 * dt));
        ASSERT_NO_THROW(b.solver.advance(.5 * dt, dt));
        const double error = std::abs(a.state.depth[0] - b.state.depth[0]);
        RecordProperty("surface_normal_local_" + std::to_string(int(dt * 1000)), error);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 6.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, SurfaceStateNormalColdInflowAndExhaustingOutflowKeepPhysicalReceipts) {
    for (double slope : {-.0001, 10000.}) {
        CoupledRkRig r;
        r.opts.dry_depth = 0;
        for (auto& neighbour : r.mesh.cell_nbr)
            neighbour = -1;
        r.init(.05, 0, 0, slope > 0 ? .001 : .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(r, true, 6, 6);
        BoundaryData boundary;
        for (int cell = 0; cell < 2; ++cell)
            for (int edge = 0; edge < 3; ++edge)
                setRkNormalBoundary(r, boundary, cell, slope, {-8}, edge);
        const double water = r.total(), energy = 4186000 * (r.state.transport.cell_mass[0] +
                                                            r.state.transport.cell_mass[1] +
                                                            r.gw.transport().storage(0));
        ASSERT_NO_THROW(r.solver.advance(0, .5));
        double applied = 0, joules = 0;
        for (const auto& row : boundary.receipts) {
            applied += row.second.applied;
            joules += row.second.heat_applied;
            EXPECT_NEAR(row.second.heat_applied, 4186000 * row.second.applied_mass[0], 1e-8);
        }
        EXPECT_NEAR(r.total() - water, applied, 1e-10);
        const double final =
            4186000 * (r.state.transport.cell_mass[0] + r.state.transport.cell_mass[1] +
                       r.gw.transport().storage(0));
        EXPECT_NEAR(final - energy, joules, 1e-10 * std::max(1., std::abs(energy)));
        for (int cell = 0; cell < 2; ++cell) {
            EXPECT_GE(r.state.volume[cell], 0);
            if (r.state.volume[cell] > 1e-15) {
                const double temp = r.state.transport.cell_mass[cell] / r.state.volume[cell];
                EXPECT_GE(temp, -8 - 1e-10);
                EXPECT_LE(temp, 6 + 1e-10);
            }
        }
        if (slope > 0) {
            EXPECT_LT(r.state.volume[0], .1 * .001 * r.mesh.tri_area[0]);
            double requested = 0;
            for (const auto& row : boundary.receipts)
                requested += row.second.requested;
            EXPECT_LT(requested, applied);
        } else
            EXPECT_GT(applied, 0);
    }
}
TEST(CoupledGroundwaterRk2, SurfaceStateNormalZeroSlopeOrRoughnessRetainsAClosedLake) {
    for (bool zero_roughness : {false, true}) {
        CoupledRkRig r;
        if (zero_roughness)
            for (auto& n : r.mesh.mannings_n)
                n = 0;
        r.init(.1, 0, 0, .2, "NONE");
        BoundaryData boundary;
        for (int cell = 0; cell < 2; ++cell)
            for (int edge = 0; edge < 2; ++edge)
                setRkNormalBoundary(r, boundary, cell, zero_roughness ? .001 : 0, {}, edge);
        const auto water = r.state.volume;
        ASSERT_NO_THROW(r.solver.advance(0, 1));
        EXPECT_EQ(r.state.volume, water);
        for (const auto& row : boundary.receipts)
            EXPECT_EQ(row.second.applied, 0);
    }
}

namespace {
/**
 * @brief Add an authored rating curve and bind it to a selected surface edge.
 *
 * Creates an authored zero-flow edge, copies curve x/y, assigns RATING_CURVE type/index/name and
 * refreshes caches. Does not validate equal sizes/monotonicity or handle reused names specially;
 * context/boundary lifetimes must cover solver use.
 *
 * @param[in,out] r Initialized rig receiving boundary/cache state.
 * @param[in,out] boundary Caller-owned retained boundary data.
 * @param[in,out] context Live context owning the new slot-named rating table.
 * @param[in] cell Valid mesh cell.
 * @param[in] stages Borrowed ordered datum-stage ordinates in metres.
 * @param[in] flows Matching outward-flow ordinates in m3/s.
 * @param[in] quality Full copied authored inflow quality tuple.
 * @param[in] ordinal Exterior-edge ordinal.
 * @return Selected global edge slot.
 */
int setRkRatingBoundary(CoupledRkRig& r, BoundaryData& boundary,
                        openswmm::SimulationContext& context, int cell,
                        const std::vector<double>& stages, const std::vector<double>& flows,
                        const std::vector<double>& quality, int ordinal = 0) {
    const auto slot = setRkAuthoredSurfaceBoundary(r, boundary, cell, false, 0, quality, ordinal);
    const auto name = "surface-rating-" + std::to_string(slot);
    const int index = context.tables.add(name, openswmm::TableType::CURVE_RATING);
    auto& table = context.tables.tables[index];
    table.x = stages;
    table.y = flows;
    boundary.edge_bc_type[slot] = int8_t(BoundaryType::RATING_CURVE);
    boundary.edge_bc_rating_curve[slot] = index;
    boundary.edge_bc_rating_curve_name[slot] = name;
    r.solver.refreshBoundaries();
    return slot;
}
}
TEST(CoupledGroundwaterRk2, SurfaceStateRatingMatchesAnIndependentLinearReservoirSolution) {
    for (double equilibrium : {.1, .3}) {
        double previous = 0;
        for (double dt : {.2, .1, .05, .025}) {
            CoupledRkRig r;
            for (auto& neighbour : r.mesh.cell_nbr)
                neighbour = -1;
            for (auto& n : r.mesh.mannings_n)
                n = 0;
            r.init(dt, 0, 0, .2, "NONE");
            initRkQuality(r, false, true, true);
            seedRkQuality(r, 2);
            BoundaryData boundary;
            openswmm::SimulationContext context;
            context.options.start_date = 0;
            for (int cell = 0; cell < 2; ++cell)
                for (int edge = 0; edge < 3; ++edge)
                    setRkRatingBoundary(
                        r, boundary, context, cell, {0, .37, 1},
                        {-.2 * equilibrium, .2 * (.37 - equilibrium), .2 * (1 - equilibrium)},
                        {7, 3}, edge);
            for (auto& table : context.tables.tables) {
                table.cursor.index = 1;
                table.cursor.direction = -1;
            }
            ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
            ASSERT_EQ(
                r.solver.saveCheckpoint(rk2Output("surface-state-rating-initial.chk").string()),
                "");
            double perimeter = 0;
            for (int edge = 0; edge < 3; ++edge)
                perimeter += r.mesh.edge_length[MeshData::slot(0, edge)];
            const double coefficient = .2 * perimeter / r.mesh.tri_area[0],
                         exact = equilibrium + (.2 - equilibrium) * std::exp(-coefficient * 2),
                         water = r.total();
            long evaluations = 0;
            for (int k = 0; k < int(std::round(2 / dt)); ++k) {
                ASSERT_NO_THROW(r.solver.advance(k * dt, (k + 1) * dt));
                evaluations += r.solver.last_num_steps();
            }
            EXPECT_EQ(evaluations, 2 * int(std::round(2 / dt)));
            double error = 0, external = 0;
            for (int cell = 0; cell < 2; ++cell)
                error = std::max(error, std::abs(r.state.depth[cell] - exact));
            for (const auto& row : boundary.receipts)
                external += row.second.applied;
            EXPECT_NEAR(r.total() - water, external, 1e-10);
            RecordProperty(std::string("surface_rating_") + (equilibrium > .2 ? "in_" : "out_") +
                               std::to_string(int(dt * 1000)),
                           error);
            EXPECT_LT(error, 1e-4);
            if (previous > 0)
                EXPECT_GT(previous / error, 3.5);
            previous = error;
            for (int row = 0; row < 2; ++row)
                EXPECT_NEAR(r.state.transport.totalIncludingLedgers(row),
                            row == 0 ? 2 * 3.2 : 5 * 3.2, 1e-10);
            for (const auto& table : context.tables.tables) {
                EXPECT_EQ(table.cursor.index, 1);
                EXPECT_EQ(table.cursor.direction, -1);
            }
        }
    }
}

TEST(CoupledGroundwaterRk2, SurfaceStateRatingHasThirdOrderLocalDifferences) {
    double previous = 0;
    for (double dt : {.08, .04, .02, .01}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        openswmm::SimulationContext ca, cb;
        ca.options.start_date = cb.options.start_date = 0;
        for (auto* r : {&a, &b}) {
            for (auto& neighbour : r->mesh.cell_nbr)
                neighbour = -1;
            for (auto& n : r->mesh.mannings_n)
                n = 0;
            r->init(dt, 0, 0, .2, "NONE");
            auto& boundary = r == &a ? ba : bb;
            auto& context = r == &a ? ca : cb;
            for (int cell = 0; cell < 2; ++cell)
                for (int edge = 0; edge < 3; ++edge)
                    setRkRatingBoundary(*r, boundary, context, cell, {0, 1}, {-.06, .14}, {}, edge);
            ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, 1, 1), "");
        }
        ASSERT_NO_THROW(a.solver.advance(0, dt));
        ASSERT_NO_THROW(b.solver.advance(0, .5 * dt));
        ASSERT_NO_THROW(b.solver.advance(.5 * dt, dt));
        const double error = std::abs(a.state.depth[0] - b.state.depth[0]);
        RecordProperty("surface_rating_local_" + std::to_string(int(dt * 1000)), error);
        EXPECT_GT(error, 0);
        if (previous > 0)
            EXPECT_GT(previous / error, 6.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, SurfaceStateRatingPreservesLegacyExtrapolationAndSignalUnits) {
    struct Case {
        double bed, eta, first, last, qfirst, qlast, expected;
    };
    for (const auto c : std::vector<Case>{{0, .1, .3, .7, .06, .02, .02},
                                          {0, .5, .3, .7, .06, .02, .04},
                                          {0, .9, .3, .7, .06, .02, .02},
                                          {-1, -.8, -.3, .7, .06, .02, .06},
                                          {0, .9, .3, .7, .02, .06, .08}}) {
        CoupledRkRig a, b;
        BoundaryData ba, bb;
        openswmm::SimulationContext ca, cb;
        ca.options.start_date = cb.options.start_date = 0;
        for (auto* r : {&a, &b}) {
            r->mesh = makeGridMesh(1, 1, 4., [&](double, double) { return c.bed; });
            r->state = makeState(r->mesh);
            r->init(.05, 0, 0, c.eta, "NONE");
            initRkTemperature(*r, false, 6, 6);
            auto& boundary = r == &a ? ba : bb;
            auto& context = r == &a ? ca : cb;
            const auto slot = setRkRatingBoundary(*r, boundary, context, 0, {c.first, c.last},
                                                  {c.qfirst, c.qlast}, {-8});
            const double length = r == &a ? 1 : .3048, flow = r == &a ? 1 : .028316846592;
            for (auto& table : context.tables.tables) {
                for (auto& x : table.x)
                    x /= length;
                for (auto& y : table.y)
                    y /= flow;
                table.cursor.index = 1;
                table.cursor.direction = -1;
            }
            ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, length, flow), "");
            EXPECT_NEAR(boundary.edge_bc_flow[slot], c.expected, 1e-13);
            const double water = r->total(), energy = 4186000 * (r->state.transport.cell_mass[0] +
                                                                 r->state.transport.cell_mass[1] +
                                                                 r->gw.transport().storage(0));
            ASSERT_NO_THROW(r->solver.advance(0, .1));
            const auto& receipt = boundary.receipts.at(slot);
            EXPECT_NEAR(r->total() - water, receipt.applied, 1e-10);
            const double final =
                4186000 * (r->state.transport.cell_mass[0] + r->state.transport.cell_mass[1] +
                           r->gw.transport().storage(0));
            EXPECT_NEAR(final - energy, receipt.heat_applied,
                        1e-10 * std::max(1., std::abs(energy)));
            for (const auto& table : context.tables.tables) {
                EXPECT_EQ(table.cursor.index, 1);
                EXPECT_EQ(table.cursor.direction, -1);
            }
            ASSERT_EQ(r->solver.saveCheckpoint(
                          rk2Output("surface-state-rating-extrapolate.chk").string()),
                      "");
        }
        for (std::size_t k = 0; k < a.state.volume.size(); ++k)
            EXPECT_NEAR(a.state.volume[k], b.state.volume[k], 1e-13);
    }
}
TEST(CoupledGroundwaterRk2,
     SurfaceStateRatingMalformedReconfigurationPreservesItsFreshImmutableOwner) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE");
    BoundaryData boundary;
    openswmm::SimulationContext context;
    context.options.start_date = 0;
    setRkRatingBoundary(r, boundary, context, 0, {0, 1}, {-.001, .002}, {});
    ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
    const auto before = rk2Output("surface-state-rating-admission-before.chk"),
               after = rk2Output("surface-state-rating-admission-after.chk");
    for (int mutation = 0; mutation < 10; ++mutation) {
        const auto original = context.tables.tables[0];
        auto& table = context.tables.tables[0];
        if (mutation == 0)
            table.y.back() = std::numeric_limits<double>::quiet_NaN();
        if (mutation == 1)
            table.x.back() = table.x.front();
        if (mutation == 2)
            table.y.pop_back();
        if (mutation == 3)
            table.type = openswmm::TableType::TIMESERIES;
        if (mutation == 4)
            table.is_file_based = true;
        if (mutation == 5)
            table.num_cols = 2;
        if (mutation == 6)
            table.x.clear();
        ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(r.solver
                         .configureRk2BoundarySignals(
                             context,
                             mutation == 7   ? 1e-320
                             : mutation == 9 ? 0
                                             : 1,
                             mutation == 8 ? std::numeric_limits<double>::infinity() : 1)
                         .empty());
        ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        table = original;
    }
}
TEST(CoupledGroundwaterRk2, SurfaceStateRatingChangedCurveOrCorruptEvaluatedFlowRefusesAtomically) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    openswmm::SimulationContext ca, cb;
    ca.options.start_date = cb.options.start_date = 0;
    int slot = -1;
    for (auto* r : {&a, &b}) {
        r->init(.1, 0, 0, .2, "NONE");
        initRkQuality(*r, false, true, true);
        seedRkQuality(*r, 2);
        auto& boundary = r == &a ? ba : bb;
        auto& context = r == &a ? ca : cb;
        slot = setRkRatingBoundary(*r, boundary, context, 0, {0, 1}, {-.06, .14}, {7, 3});
        ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, 1, 1), "");
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    const auto good = rk2Output("surface-state-rating-good.chk"),
               bad = rk2Output("surface-state-rating-bad.chk"),
               before = rk2Output("surface-state-rating-before.chk"),
               after = rk2Output("surface-state-rating-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    for (int mutation = 0; mutation < 3; ++mutation) {
        const auto original = cb.tables.tables[0];
        if (mutation == 0)
            cb.tables.tables[0].y.back() += .01;
        ASSERT_EQ(b.solver.configureRk2BoundarySignals(cb, mutation == 1 ? .5 : 1,
                                                       mutation == 2 ? .5 : 1),
                  "");
        ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
        EXPECT_FALSE(b.solver.loadCheckpoint(good.string()).empty());
        ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
        EXPECT_EQ(rk2File(before), rk2File(after));
        cb.tables.tables[0] = original;
        ASSERT_EQ(b.solver.configureRk2BoundarySignals(cb, 1, 1), "");
    }
    openswmm::detail::CompletedSourceArchive original(rk2File(good));
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, crc = 0;
    original(magic, version, endian, long_bytes, payload, crc);
    original.finish();
    double flow = ba.edge_bc_flow[slot];
    const std::string needle(reinterpret_cast<const char*>(&flow), sizeof(flow));
    const auto offset = payload.find(needle);
    ASSERT_NE(offset, std::string::npos);
    ASSERT_EQ(payload.find(needle, offset + 1), std::string::npos);
    flow += .00123;
    std::memcpy(payload.data() + offset, &flow, sizeof(flow));
    crc = ~uint32_t(0);
    for (unsigned char c : payload) {
        crc ^= c;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
    }
    crc = ~crc;
    openswmm::detail::CompletedSourceArchive corrupted;
    corrupted(magic, version, endian, long_bytes, payload, crc);
    std::ofstream(bad, std::ios::binary) << corrupted.bytes();
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_NE(b.solver.loadCheckpoint(bad.string()).find("completed clock"), std::string::npos);
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 2));
    ASSERT_NO_THROW(b.solver.advance(.735, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2,
     SurfaceStateRatingOverrideHiddenQualityClearAndSeriesKeepExactComponentHistory) {
    CoupledRkRig a, b;
    BoundaryData ba, bb;
    openswmm::SimulationContext ca, cb;
    ca.options.start_date = cb.options.start_date = 0;
    int slot = -1;
    for (auto* r : {&a, &b}) {
        r->init(.05, 0, 0, .2, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
        initRkTemperature(*r, true, 6, 6);
        auto& boundary = r == &a ? ba : bb;
        auto& context = r == &a ? ca : cb;
        slot = setRkRatingBoundary(*r, boundary, context, 0, {0, 1}, {-.0003, .0007}, {-8});
        setRkSurfaceSignal(*r, boundary, context, 1, true, {0, .735, 1.237, 2},
                           {.21, .22, .215, .23}, {-8});
        ASSERT_EQ(r->solver.configureRk2BoundarySignals(context, 1, 1), "");
    }
    ASSERT_NO_THROW(a.solver.advance(0, .735));
    for (auto* r : {&a, &b}) {
        auto& boundary = r == &a ? ba : bb;
        setRkSurfaceBoundary(*r, boundary, 0, false, -.003, {-4});
        boundary.edge_bc_rating_curve[slot] = -1;
        boundary.edge_bc_rating_curve_name[slot].clear();
        r->solver.refreshBoundaries();
    }
    const auto good = rk2Output("surface-state-rating-override.chk"),
               left = rk2Output("surface-state-rating-override-left.chk"),
               right = rk2Output("surface-state-rating-override-right.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    b.state.transport.bc_quality_rows[0].conc = -7;
    ASSERT_EQ(b.solver.saveCheckpoint(left.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(good.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(right.string()), "");
    EXPECT_EQ(rk2File(left), rk2File(right));
    b.state.transport.bc_quality_rows[0].conc = -8;
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    ASSERT_NO_THROW(a.solver.advance(.735, 1.237));
    ASSERT_NO_THROW(b.solver.advance(.735, 1.237));
    for (auto* r : {&a, &b}) {
        auto& boundary = r == &a ? ba : bb;
        boundary.clearRuntime(slot);
        r->state.transport.runtime_bc_conc.erase(slot);
        r->solver.refreshBoundaries();
    }
    ASSERT_NO_THROW(a.solver.advance(1.237, 2));
    ASSERT_NO_THROW(b.solver.advance(1.237, 2));
    ASSERT_EQ(a.solver.saveCheckpoint(left.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(right.string()), "");
    EXPECT_EQ(rk2File(left), rk2File(right));
}
TEST(CoupledGroundwaterRk2,
     SurfaceStateRatingLateOverflowRestoresEveryOwnerAndSupportsOverrideRetry) {
    CoupledRkRig r;
    r.init(.05, 0, 0, .2, "NONE");
    initRkTemperature(r, false, 6, 6);
    BoundaryData boundary;
    openswmm::SimulationContext context;
    context.options.start_date = 0;
    const auto slot = setRkRatingBoundary(r, boundary, context, 0, {0, .2001, .2002},
                                          {-.001, -.001, 1e308}, {-8});
    ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
    const auto before = rk2Output("surface-state-rating-overflow-before.chk"),
               after = rk2Output("surface-state-rating-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    EXPECT_THROW(r.solver.advance(0, 1), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    setRkSurfaceBoundary(r, boundary, 0, false, -.0001, {-8});
    boundary.edge_bc_rating_curve[slot] = -1;
    boundary.edge_bc_rating_curve_name[slot].clear();
    r.solver.refreshBoundaries();
    ASSERT_NO_THROW(r.solver.advance(0, 1));
}

TEST(CoupledGroundwaterRk2, SurfaceStateSignalInitialOverflowPreservesItsFreshImmutableOwner) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE");
    BoundaryData boundary;
    openswmm::SimulationContext context;
    context.options.start_date = 0;
    setRkSurfaceSignal(r, boundary, context, 0, false, {-1, 1}, {-.001, -.002}, {});
    ASSERT_EQ(r.solver.configureRk2BoundarySignals(context, 1, 1), "");
    const auto before = rk2Output("surface-state-signal-initial-before.chk"),
               after = rk2Output("surface-state-signal-initial-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    context.tables.tables[0].y = {-1e308, 1e308};
    EXPECT_FALSE(r.solver.configureRk2BoundarySignals(context, 1, 1).empty());
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}

namespace {
/**
 * @brief Initialize owned mesh snow state and bind its retained context to the rig.
 *
 * Authors fixed US snow/weather/site controls and ten-second duration; native melt coefficient is
 * chosen for 0.005 m/s liquid release at the specified warm air, not a calibrated physical
 * snowpack. Builds SI mesh-area geometry, initializes snow and sets borrowed snow_context plus unit
 * catch factors. Throws on weather/snow rejection; appends into fresh context names and does not
 * initialize a full public engine.
 *
 * @param[in,out] r Initialized water rig whose snow state/context/catch factors are replaced.
 * @param[in,out] ctx Fresh caller-owned snow/weather context, which must outlive snow evaluation.
 * @param[in] ueb Use UEB with cached hourly meteorology/free-water seed; false uses manufactured
 *     native melt.
 * @param[in] partial Assign snow only to cell zero; false applies SP to all mesh cells.
 * @param[in] depth Initial snow-water-equivalent depth in metres, converted to authored inches.
 */
void initOwnedRkSnow(CoupledRkRig& r, openswmm::SimulationContext& ctx, bool ueb = false,
                     bool partial = false, double depth = .1) {
    ctx.options.ignore_snow_melt = false;
    ctx.options.heat_transport = false;
    ctx.options.flow_units = openswmm::FlowUnits::CFS;
    ctx.options.start_date = openswmm::datetime::encodeDate(2026, 1, 1);
    ctx.options.end_date = ctx.options.start_date + 10. / 86400.;
    ctx.options.total_duration_ms = 10000;
    ctx.climate_state.temperature = ctx.climate_state.temperature_src = 41;
    ctx.climate_state.snow_divt = 32;
    ctx.climate_state.humidity = 50;
    ctx.snowpack_names.add("SP");
    ctx.snowpacks.names = {"SP"};
    // Manufactured constant melt: native US coefficient *9F *0.3048/43200
    // gives exactly .005m/s. This is an order gate, not a calibrated snowpack.
    const double coefficient = .005 * 43200 / (.3048 * 9);
    ctx.snowpacks.pervious = {
        std::array<double, 7>{coefficient, coefficient, 32, 0, depth / .0254, 0, 0}};
    if (partial)
        ctx.snowpacks.mesh_config.cells[0].pack = "SP";
    else
        ctx.snowpacks.mesh_config.defaults["*"].pack = "SP";
    if (ueb) {
        ctx.snowpacks.config.model = openswmm::surface::SnowKind::Ueb;
        // Seed drainable free water; a newly warming dry UEB pack retains
        // its first melt internally rather than immediately producing runoff.
        ctx.snowpacks.pervious[0][3] = .2;
        ctx.snowpacks.pervious[0][5] = .01 / .0254;
        ctx.snowpacks.config.site = openswmm::surface::ClimateSite{45, -120, -8, 1000};
        std::istringstream met(
            "01/01/2026 00:00 5 50 1 400 375\n01/01/2026 01:00 5 50 1 400 375\n");
        if (auto error = ctx.snowpacks.meteorology.read(met); !error.empty())
            throw std::runtime_error(error);
    }
    std::vector<openswmm::surface::MeshSnowGeometry> geometry(r.mesh.n_cells());
    for (int i = 0; i < r.mesh.n_cells(); ++i)
        geometry[i].area = r.mesh.tri_area[i];
    if (auto error = r.state.snow.initialize(ctx, geometry); !error.empty())
        throw std::runtime_error(error);
    r.state.snow_context = &ctx;
    r.state.snow_catch.assign(r.mesh.n_cells(), 1);
}
/**
 * @brief Sum mesh snow ledger stored water across the rig.
 *
 * Does not include surface/GW water or mutate snow state.
 *
 * @param[in] r Borrowed rig with current or empty snow ledgers.
 * @return Stored snow water in m3, zero when no ledgers exist.
 */
double rkSnowStorage(const CoupledRkRig& r) {
    double volume = 0;
    for (const auto& b : r.state.snow.ledgers())
        volume += b.stored;
    return volume;
}
/**
 * @brief Run the manufactured melt/infiltration fixture and return selected final water state.
 *
 * Builds a local water/GW rig, seeds moisture, publishes infiltration and optionally retains local
 * snow context. Executes round(duration/dt) intervals, then nonfatally checks combined
 * water/deep-export and snow ledger/liquid release expectations using requested duration. Rounding
 * can change actual endpoint when inputs do not divide; no input-size/finite guard is applied and
 * solver errors propagate.
 *
 * @param[in] dt Positive fixed interval in seconds; should divide duration for the intended
 *     endpoint.
 * @param[in] snow Initialize the native manufactured snowpack when true; otherwise use no snow.
 * @param[in] duration Nonnegative target seconds, normally within the ten-second snow context.
 * @return Owned three-entry vector: cell-zero surface volume m3, saturated height m and unsaturated
 *     water depth m.
 */
std::vector<double> rkSnowTrajectory(double dt, bool snow, double duration = 2) {
    CoupledRkRig r;
    r.init(dt, .005, .0005, .2);
    openswmm::SimulationContext ctx;
    auto& water = r.gw.state();
    for (int i = 0; i < water.n_cells; ++i) {
        water.hu[i] = .35 * (water.zs[i] - water.hg[i]);
        water.wetting_front[i] = .1;
    }
    water.led_init_storage = water.storage();
    r.gw.publishInfiltration(r.state, dt, 0);
    if (snow)
        initOwnedRkSnow(r, ctx);
    const double initial = r.total() + rkSnowStorage(r);
    for (int k = 0; k < int(std::round(duration / dt)); ++k)
        r.solver.advance(k * dt, (k + 1) * dt);
    EXPECT_NEAR(r.total() + rkSnowStorage(r) + water.led_deep, initial, 1e-10 * initial);
    if (snow) {
        double liquid = 0;
        for (const auto& b : r.state.snow.ledgers()) {
            liquid += b.liquid;
            EXPECT_NEAR(b.residual(), 0, 1e-10);
        }
        EXPECT_NEAR(liquid,
                    .005 * duration *
                        std::accumulate(r.mesh.tri_area.begin(), r.mesh.tri_area.end(), 0.),
                    1e-10);
        EXPECT_DOUBLE_EQ(r.state.snow.completedEnd(), r.solver.completedSourceTime());
    }
    return {r.state.volume[0], water.hg[0], water.hu[0]};
}
}
TEST(CoupledGroundwaterRk2, OwnedSnowSourceHasFourGlobalRefinementsAndIndependentMeltBudget) {
    const auto response = [](double dt) {
        auto a = rkSnowTrajectory(dt, true), b = rkSnowTrajectory(dt, false);
        for (std::size_t i = 0; i < a.size(); ++i)
            a[i] -= b[i];
        return a;
    };
    const auto reference = response(.00078125);
    double previous = 0;
    for (double dt : {.1, .05, .025, .0125}) {
        const auto result = response(dt);
        double square = 0;
        for (std::size_t i = 0; i < result.size(); ++i)
            square += std::pow(result[i] - reference[i], 2);
        const double error = std::sqrt(square);
        RecordProperty("snow_error_" + std::to_string(int(dt * 100000)), error);
        EXPECT_LT(error, 1e-4);
        if (previous > 0)
            EXPECT_GE(previous / error, 3.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, OwnedSnowComponentRestoresItsActualPointStateAndRejectsChangedWeather) {
    CoupledRkRig a, b;
    a.init(.1, .005, .0005, .2);
    b.init(.1, .005, .0005, .2);
    openswmm::SimulationContext ca, cb;
    ASSERT_NO_THROW(initOwnedRkSnow(a, ca));
    ASSERT_NO_THROW(initOwnedRkSnow(b, cb));
    ASSERT_NO_THROW(a.solver.advance(0, .2));
    const auto saved = rk2Output("snow-owned.chk"), before = rk2Output("snow-before.chk"),
               after = rk2Output("snow-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(saved.string()), "");
    cb.climate_state.temperature += 1;
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_FALSE(b.solver.loadCheckpoint(saved.string()).empty());
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    cb.climate_state.temperature -= 1;
    ASSERT_EQ(b.solver.loadCheckpoint(saved.string()), "");
    EXPECT_EQ(a.state.snow.saveCheckpoint(), b.state.snow.saveCheckpoint());
    ASSERT_NO_THROW(a.solver.advance(.2, 1));
    ASSERT_NO_THROW(b.solver.advance(.2, 1));
    ASSERT_EQ(a.solver.saveCheckpoint(before.string()), "");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
}
TEST(CoupledGroundwaterRk2,
     OwnedSnowRollbackIncludesPointHistoryAfterLaterSourceOverflowAndRetries) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE");
    openswmm::SimulationContext ctx;
    ASSERT_NO_THROW(initOwnedRkSnow(r, ctx));
    setRkRuntimePair(r);
    r.state.runtime_sources.find("supply", 1)->flow = 1e308;
    const auto before = rk2Output("snow-overflow-before.chk"),
               after = rk2Output("snow-overflow-after.chk");
    ASSERT_EQ(r.solver.saveCheckpoint(before.string()), "");
    const auto radiation = ctx.climate_state.radiation;
    EXPECT_THROW(ExplicitInertialSolverTestAccess::rk2(r.solver, 10, 0), std::runtime_error);
    ASSERT_EQ(r.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    EXPECT_DOUBLE_EQ(ctx.climate_state.radiation.date, radiation.date);
    EXPECT_DOUBLE_EQ(ctx.climate_state.radiation.shortwave, radiation.shortwave);
    r.state.runtime_sources.find("supply", 1)->flow = .2;
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    EXPECT_GT(r.state.snow.ledgers()[0].liquid, 0);
}
TEST(CoupledGroundwaterRk2, OwnedSnowCoverageProtectsOnlyCoveredCellFromNativeEvaporation) {
    CoupledRkRig r;
    r.init(.1, 0, 0, .2, "NONE");
    r.opts.evaporation = 1;
    std::fill(r.state.evap_rate.begin(), r.state.evap_rate.end(), .001);
    openswmm::SimulationContext ctx;
    ASSERT_NO_THROW(initOwnedRkSnow(r, ctx, false, true));
    const double initial = r.total() + rkSnowStorage(r);
    ASSERT_NO_THROW(r.solver.advance(0, .1));
    double area = 0;
    for (int i = 0; i < r.mesh.n_cells(); ++i)
        if (!r.state.snow.owns(i))
            area += r.mesh.tri_area[i];
    EXPECT_NEAR(r.state.evap_loss_total, .001 * .1 * area, 1e-12);
    EXPECT_NEAR(r.total() + rkSnowStorage(r) + r.state.evap_loss_total, initial, 1e-10);
}
TEST(CoupledGroundwaterRk2, OwnedUebWaterSourceKeepsEnergyAndScratchWeatherInsideItsTransaction) {
    CoupledRkRig r;
    r.init(.1, .001, 0, .2, "NONE");
    openswmm::SimulationContext ctx;
    ASSERT_NO_THROW(initOwnedRkSnow(r, ctx, true));
    const double initial = r.total() + rkSnowStorage(r);
    const auto radiation = ctx.climate_state.radiation;
    ASSERT_NO_THROW(r.solver.advance(0, 1));
    double liquid = 0;
    for (const auto& b : r.state.snow.ledgers()) {
        EXPECT_NEAR(b.residual(), 0, 1e-10);
        EXPECT_NEAR(b.energyResidual(), 0, 1e-8);
        liquid += b.liquid;
    }
    EXPECT_GT(liquid, 0);
    double sublimation = 0;
    for (const auto& b : r.state.snow.ledgers())
        sublimation += b.sublimation;
    EXPECT_NEAR(r.total() + rkSnowStorage(r) + r.gw.state().led_deep + sublimation, initial, 1e-10);
    EXPECT_DOUBLE_EQ(ctx.climate_state.radiation.date, radiation.date);
    EXPECT_DOUBLE_EQ(ctx.climate_state.radiation.shortwave, radiation.shortwave);
}

TEST(CoupledGroundwaterRk2, OwnedSnowSourceHasThirdLocalDifferences) {
    double previous = 0;
    for (double duration : {.1, .05, .025, .0125}) {
        auto a = rkSnowTrajectory(duration, true, duration),
             b = rkSnowTrajectory(duration, false, duration);
        const auto c = rkSnowTrajectory(.5 * duration, true, duration),
                   d = rkSnowTrajectory(.5 * duration, false, duration);
        double square = 0;
        for (std::size_t i = 0; i < a.size(); ++i)
            square += std::pow((a[i] - b[i]) - (c[i] - d[i]), 2);
        const double error = std::sqrt(square);
        RecordProperty("snow_local_" + std::to_string(int(duration * 100000)), error);
        if (previous > 0)
            EXPECT_GE(previous / error, 6.5);
        previous = error;
    }
}
TEST(CoupledGroundwaterRk2, OwnedSnowMeltOutAndDrySurfaceConserveAllWater) {
    for (double pond : {0., .02}) {
        CoupledRkRig r;
        r.init(.025, .001, 0, pond, "NONE");
        r.opts.evaporation = 1;
        std::fill(r.state.evap_rate.begin(), r.state.evap_rate.end(), .001);
        openswmm::SimulationContext ctx;
        ASSERT_NO_THROW(initOwnedRkSnow(r, ctx, false, false, .001));
        const double initial = r.total() + rkSnowStorage(r), snow_initial = rkSnowStorage(r);
        ASSERT_NO_THROW(r.solver.advance(0, 1));
        double liquid = 0;
        for (const auto& b : r.state.snow.ledgers()) {
            EXPECT_NEAR(b.stored, 0, 1e-12);
            EXPECT_NEAR(b.residual(), 0, 1e-10);
            liquid += b.liquid;
        }
        EXPECT_NEAR(liquid, snow_initial, 1e-10);
        EXPECT_NEAR(r.total() + rkSnowStorage(r) + r.state.evap_loss_total + r.gw.state().led_deep,
                    initial, 1e-10);
        for (double volume : r.state.volume)
            EXPECT_GE(volume, 0);
    }
}
TEST(CoupledGroundwaterRk2, OwnedSnowValidChecksumClockMutationRefusesBeforeInstallation) {
    using Archive = openswmm::detail::CompletedSourceArchive;
    CoupledRkRig a, b;
    a.init(.1, 0, 0, .2, "NONE");
    b.init(.1, 0, 0, .2, "NONE");
    openswmm::SimulationContext ca, cb;
    ASSERT_NO_THROW(initOwnedRkSnow(a, ca));
    ASSERT_NO_THROW(initOwnedRkSnow(b, cb));
    ASSERT_NO_THROW(a.solver.advance(0, .2));
    const auto good = rk2Output("snow-clock-good.chk"), bad = rk2Output("snow-clock-bad.chk"),
               before = rk2Output("snow-clock-before.chk"),
               after = rk2Output("snow-clock-after.chk");
    ASSERT_EQ(a.solver.saveCheckpoint(good.string()), "");
    std::string magic, payload;
    uint32_t version = 0, endian = 0, long_bytes = 0, sum = 0;
    Archive in(rk2File(good));
    in(magic, version, endian, long_bytes, payload, sum);
    in.finish();
    const auto snow = a.state.snow.saveCheckpoint();
    const auto start = payload.find(snow);
    ASSERT_NE(start, std::string::npos);
    Archive record(snow);
    std::string snow_magic, identity;
    double last = 0;
    std::array<double, 4> forcing{};
    std::vector<unsigned char> contacts;
    record(snow_magic, identity, last, forcing, contacts);
    Archive prefix;
    prefix(snow_magic, identity, last, forcing, contacts);
    const double wrong =
        std::nextafter(a.state.snow.completedEnd(), std::numeric_limits<double>::infinity());
    ASSERT_LT(start + prefix.bytes().size() + sizeof(wrong), payload.size());
    std::memcpy(payload.data() + start + prefix.bytes().size(), &wrong, sizeof(wrong));
    sum = ~uint32_t(0);
    for (unsigned char byte : payload) {
        sum ^= byte;
        for (int k = 0; k < 8; ++k)
            sum = (sum >> 1) ^ ((sum & 1) ? 0xedb88320u : 0u);
    }
    sum = ~sum;
    Archive out;
    out(magic, version, endian, long_bytes, payload, sum);
    {
        std::ofstream file(bad, std::ios::binary);
        file.write(out.bytes().data(), out.bytes().size());
    }
    ASSERT_EQ(b.solver.saveCheckpoint(before.string()), "");
    EXPECT_EQ(b.solver.loadCheckpoint(bad.string()),
              "RK2 snow and numerical source clocks differ.");
    ASSERT_EQ(b.solver.saveCheckpoint(after.string()), "");
    EXPECT_EQ(rk2File(before), rk2File(after));
    ASSERT_EQ(b.solver.loadCheckpoint(good.string()), "");
    EXPECT_EQ(a.state.snow.saveCheckpoint(), b.state.snow.saveCheckpoint());
}

TEST(CoupledGroundwaterRk2, AtmosphericColumnsKeepShapeAndRefusalRecoveryIndependent) {
    struct Owner {
        CoupledRkRig water;
        RkPorousOwner ground;
        double elapsed = 0;
        Owner(int layers, bool sigma, bool atmosphere) {
            water.init(.1, 0, 0, .02, "NONE", GwClosure::SIGMA, SoilChar::GARDNER, 4);
            initRkFiniteSurface(water, ground, sigma, layers, 2, 0, 20, -2);
            water.opts.evaporation = 0;
            water.opts.heat_surface = water.opts.heat_radiative = atmosphere;
            water.state.heat.surface = water.state.heat.radiative = atmosphere;
            auto& ctx = ground.context;
            ctx.climate_state.temperature = 41;
            ctx.climate_state.wind_speed = 3;
            ctx.climate_state.humidity = 60;
            ctx.options.wind_func_coeff_a = 1e-6;
            ctx.options.wind_func_coeff_b = 0;
            ctx.options.pressure_ratio = 1;
            ctx.climate_state.radiation.shared_heat_forcing = true;
            ctx.climate_state.radiation.longwave = 250;
            ctx.heat_state.shortwave_now = 500;
            ctx.heat_state.cloud_now = 0;
            auto& rc = ctx.heat_config.radiative;
            rc.emiss_water = 1; rc.emiss_landcover = 0; rc.lw_reflection = 0;
            rc.sky_view = 1; rc.albedo = 0; rc.shade_factor = 0;
        }
        void advance() {
            const double end = elapsed + .1;
            if (water.solver.advance(elapsed, end) != end)
                throw std::runtime_error("Atmospheric owner missed its endpoint.");
            elapsed = end;
        }
        std::string save(const std::filesystem::path& path) {
            const auto error = water.solver.saveCheckpoint(path.string());
            if (!error.empty()) throw std::runtime_error(error);
            return rk2File(path);
        }
    };
    int index = 0;
    for (int layers : {1, 33, 8}) {
        const bool sigma = layers == 33;
        Owner reference(layers, sigma, true);
        for (int step = 0; step < 3; ++step) reference.advance();
        const auto expected = reference.save(rk2Output("atmos-reuse-reference.chk"));
        Owner other(33, true, false);
        other.advance(); other.advance();
        Owner small(1, false, true);
        small.advance(); small.advance();
        Owner actual(layers, sigma, true);
        for (int step = 0; step < 3; ++step) actual.advance();
        const auto saved = rk2Output("atmos-reuse-owner-" + std::to_string(index++) + ".chk");
        EXPECT_EQ(actual.save(saved), expected);
        Owner restored(layers, sigma, true);
        ASSERT_EQ(restored.water.solver.loadCheckpoint(saved.string()), "");
        EXPECT_EQ(restored.save(rk2Output("atmos-reuse-roundtrip.chk")), expected);
        if (layers != 8) continue;
        const double accepted = actual.water.state.transport.cell_mass[1];
        actual.water.state.transport.cell_mass[1] = -300 * actual.water.state.volume[1];
        const auto invalid = actual.save(rk2Output("atmos-reuse-invalid-before.chk"));
        EXPECT_THROW(actual.water.solver.advance(actual.elapsed, actual.elapsed + .1),
                     std::runtime_error);
        EXPECT_EQ(actual.save(rk2Output("atmos-reuse-invalid-after.chk")), invalid);
        actual.water.state.transport.cell_mass[1] = accepted;
        actual.advance(); reference.advance();
        EXPECT_EQ(actual.save(rk2Output("atmos-reuse-recovered.chk")),
                  reference.save(rk2Output("atmos-reuse-recovery-reference.chk")));
    }
}
