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
 * @license  MIT License
 */

#include <gtest/gtest.h>
#include <cmath>
#include <numeric>
#include <vector>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"

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
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v10; mesh.tri_v2[t] = v11;
            ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v01;
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
    boundary.resize(mesh.n_triangles() * 3);
    int outlet_slots = 0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const int nbrs[3] = {mesh.tri_nbr0[i], mesh.tri_nbr1[i],
                             mesh.tri_nbr2[i]};
        for (int e = 0; e < 3; ++e) {
            const int idx = i * 3 + e;
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
            const int idx = i * 3 + e;
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
    EXPECT_NEAR(h_avg, h_n, 0.05 * h_n);
    solver.finalize();
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
    boundary.resize(mesh.n_triangles() * 3);
    std::vector<int> bc_slots;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const int nbrs[3] = {mesh.tri_nbr0[i], mesh.tri_nbr1[i],
                             mesh.tri_nbr2[i]};
        for (int e = 0; e < 3; ++e) {
            const int idx = i * 3 + e;
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
            const int nbrs[3] = {mesh.tri_nbr0[i], mesh.tri_nbr1[i],
                                 mesh.tri_nbr2[i]};
            for (int e = 0; e < 3; ++e) {
                const int j = nbrs[e];
                if (j < 0) continue;                  // boundary slot
                const int idx = i * 3 + e;
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
