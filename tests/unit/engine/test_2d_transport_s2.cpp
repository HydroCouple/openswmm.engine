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
 * @file test_2d_transport_s2.cpp
 * @brief Overland transport S2 — source concentrations and limited explicit
 *        dispersion on the face cadence.
 *
 * @details S1 established that species mass rides the marcher's own face
 *          accumulators. S2 adds the three things S1 deliberately left at
 *          zero: the concentration rain arrives at, the concentration a
 *          boundary inflow arrives at, and an isotropic dispersive exchange
 *          booked on the SAME face, at the SAME cadence, into the SAME
 *          accumulators as advection.
 *
 *          The still pond is the instrument here. On a flat bed with a level
 *          surface nothing flows, so whatever the species does is entirely
 *          the S2 term under test — rain, boundary, or dispersion — with no
 *          advective signal to hide behind. The S1 uniformity gate is then
 *          re-applied to the source gates: rain at the pond's concentration
 *          must leave the pond uniform to round-off, and so must inflow at
 *          the pond's concentration. Those are the falsifiers for a source
 *          booked to the wrong volume, at the wrong time, or to the wrong
 *          cell.
 *
 *          Dispersion is gated on the three properties an explicit
 *          exchange can lose: mass (conservation to reduction round-off),
 *          the discrete max principle (the limiter's whole purpose), and
 *          the Fickian spreading rate (variance of a pulse grows as 2·D·t).
 *          The limiter is exercised deliberately with a D far above the
 *          explicit stability limit, and the bind counter must say so.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

#include "2d/data/BoundaryData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include "core/SWMMEngine.hpp"

using namespace openswmm::twoD;

namespace {

/// Uniform strip: nx × ny cells of dx × dx, two triangles per cell, flat.
MeshData makeStrip(int nx, int ny, double dx, double n = 0.03) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = i * dx;
            mesh.vy[v] = j * dx;
            mesh.vz[v] = 0.0;
        }
    mesh.resize_triangles(2 * nx * ny);
    int t = 0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const int v00 = j * nvx + i,       v10 = j * nvx + i + 1;
            const int v01 = (j + 1) * nvx + i, v11 = (j + 1) * nvx + i + 1;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v10; mesh.tri_v2[t] = v11; ++t;
            mesh.tri_v0[t] = v00; mesh.tri_v1[t] = v11; mesh.tri_v2[t] = v01; ++t;
        }
    for (int i = 0; i < mesh.n_triangles(); ++i) mesh.mannings_n[i] = n;
    buildMeshTopology(mesh);
    return mesh;
}

/// Still pond of uniform depth `h` everywhere, species at `c0`.
SurfaceStateData makePond(const MeshData& mesh, const SolverOptions2D& opts,
                          int n_species, double h, double c0) {
    SurfaceStateData s;
    s.resize(mesh.n_triangles(), mesh.n_vertices());
    s.transport.resize(n_species, mesh.n_triangles(), 0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        s.volume[i] = h * mesh.tri_area[i];
        inertial::cellEtaDepth(mesh, opts, i, s.volume[i], s.head[i], s.depth[i]);
        for (int sp = 0; sp < n_species; ++sp)
            s.transport.cell_mass[s.transport.idx(sp, i)] = c0 * s.volume[i];
    }
    return s;
}

void run(ExplicitInertialSolver& solver, double T, double dt_out = 5.0) {
    for (double t = 0.0; t < T; t += dt_out)
        solver.advance(t, std::min(t + dt_out, T));
}

struct WetStats { int wet = 0; double cmin = 1e300, cmax = -1e300, worst_rel = 0.0; };
WetStats wetStats(const MeshData& mesh, const SolverOptions2D& opts,
                  const SurfaceStateData& s, int sp, double c_ref) {
    WetStats w;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double v_dry = opts.dry_depth * mesh.tri_area[i];
        if (!(s.volume[i] > v_dry)) continue;
        ++w.wet;
        const double c = s.transport.concentration(sp, i, s.volume[i], v_dry);
        w.cmin = std::min(w.cmin, c);
        w.cmax = std::max(w.cmax, c);
        if (c_ref > 0.0) w.worst_rel = std::max(w.worst_rel, std::fabs(c - c_ref) / c_ref);
    }
    return w;
}

/// Mass-weighted mean and variance of x over the species distribution.
void xMoments(const MeshData& mesh, const SurfaceTransportState& tr, int sp,
              double& mean, double& var) {
    double m = 0.0, mx = 0.0, mxx = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double w = tr.cell_mass[tr.idx(sp, i)];
        m += w; mx += w * mesh.tri_cx[i]; mxx += w * mesh.tri_cx[i] * mesh.tri_cx[i];
    }
    mean = mx / m;
    var  = mxx / m - mean * mean;
}

/// Flat mesh edge slot (tri*3+e) of the first boundary edge lying on x == x0.
int boundaryEdgeOnX(const MeshData& mesh, double x0) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const int vv[3]  = {mesh.tri_v0[i], mesh.tri_v1[i], mesh.tri_v2[i]};
        const int nbr[3] = {mesh.tri_nbr0[i], mesh.tri_nbr1[i], mesh.tri_nbr2[i]};
        for (int e = 0; e < 3; ++e) {
            if (nbr[e] >= 0) continue;
            const int va = vv[(e + 1) % 3], vb = vv[(e + 2) % 3];
            if (std::fabs(mesh.vx[va] - x0) < 1e-12 &&
                std::fabs(mesh.vx[vb] - x0) < 1e-12)
                return i * 3 + e;
        }
    }
    return -1;
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. Rain at the pond's own concentration leaves a still pond UNIFORM to
//    round-off — the S1 property re-applied to the rain source. The gained
//    ledger must equal what fell (Σ R·A·T·c_rain), and the S1 continuity
//    quantity (surface + lost − gained) must be invariant.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, RainAtPondConcentrationKeepsPondUniform) {
    auto mesh = makeStrip(20, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    const double c0 = 3.0, R = 1.0e-5, T = 600.0;
    auto s = makePond(mesh, opts, 1, 0.5, c0);
    s.transport.rain_conc.assign(1, c0);
    std::fill(s.rainfall.begin(), s.rainfall.end(), R);
    const double m0 = s.transport.totalIncludingLedgers(0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, T);

    const auto w = wetStats(mesh, opts, s, 0, c0);
    EXPECT_EQ(w.wet, mesh.n_triangles());
    EXPECT_LT(w.worst_rel, 1.0e-12)
        << "rain at c_rain == c0 changed a cell's concentration: the rain "
           "mass is not booked against the same volume the rain adds";
    double area = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) area += mesh.tri_area[i];
    EXPECT_NEAR(s.transport.gained_rainfall[0], R * area * T * c0,
                1.0e-9 * R * area * T * c0);
    EXPECT_NEAR(s.transport.totalIncludingLedgers(0), m0, 1.0e-10 * m0);
}

// ---------------------------------------------------------------------------
// 2. Clean rain (no rain_conc) dilutes; total-minus-gained is invariant and
//    every cell dilutes by the SAME factor V0/(V0 + R·T·A).
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, CleanRainDilutesUniformly) {
    auto mesh = makeStrip(20, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    const double c0 = 3.0, R = 1.0e-5, T = 600.0, h0 = 0.5;
    auto s = makePond(mesh, opts, 1, h0, c0);
    std::fill(s.rainfall.begin(), s.rainfall.end(), R);
    const double m0 = s.transport.totalIncludingLedgers(0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, T);

    const double c_exp = c0 * h0 / (h0 + R * T);
    const auto w = wetStats(mesh, opts, s, 0, c_exp);
    EXPECT_LT(w.worst_rel, 1.0e-10);
    EXPECT_EQ(s.transport.gained_rainfall[0], 0.0);
    EXPECT_NEAR(s.transport.totalIncludingLedgers(0), m0, 1.0e-10 * m0);
}

// ---------------------------------------------------------------------------
// 3. Dispersion: a concentration step in a still pond smooths, conserves
//    mass to round-off, obeys the discrete max principle, and — at a D well
//    inside the explicit limit — never binds the limiter.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, DispersionSmoothsConservesAndObeysMaxPrinciple) {
    auto mesh = makeStrip(40, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    opts.dispersion = 0.05;   // D·dt/dx² ≈ 0.015 at the gravity-wave dt
    auto s = makePond(mesh, opts, 1, 1.0, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] < 20.0)
            s.transport.cell_mass[s.transport.idx(0, i)] = 10.0 * s.volume[i];
    const double m0 = s.transport.totalIncludingLedgers(0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, 300.0);

    const auto w = wetStats(mesh, opts, s, 0, 0.0);
    EXPECT_NEAR(s.transport.totalIncludingLedgers(0), m0, 1.0e-10 * m0);
    EXPECT_LE(w.cmax, 10.0 * (1.0 + 1.0e-10)) << "new maximum created";
    EXPECT_GE(w.cmin, -1.0e-12)                << "negative concentration";
    EXPECT_EQ(s.transport.dispersion_limiter_binds, 0L)
        << "limiter bound at a D far inside the explicit stability limit";
    // The step is gone: cells straddling x = 20 carry intermediate values.
    bool smoothed = false;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double c = s.transport.concentration(0, i, s.volume[i], 0.0);
        if (c > 0.5 && c < 9.5) smoothed = true;
    }
    EXPECT_TRUE(smoothed) << "dispersion did nothing on a still pond";
    // Column-mean profile is monotone in x (no ringing).
    std::vector<double> col(40, 0.0), colv(40, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const auto k = static_cast<std::size_t>(mesh.tri_cx[i]);
        col[k]  += s.transport.cell_mass[s.transport.idx(0, i)];
        colv[k] += s.volume[i];
    }
    for (std::size_t k = 1; k < 40; ++k)
        EXPECT_LE(col[k] / colv[k], col[k - 1] / colv[k - 1] * (1.0 + 1.0e-10))
            << "profile not monotone at column " << k;
}

// ---------------------------------------------------------------------------
// 4. Fickian rate: the x-variance of a narrow pulse grows by ≈ 2·D·T.
//    The two-point face flux on this regular triangulation is expected to
//    recover the continuum rate to within a few tens of percent; the band is
//    deliberately wide and is to be TIGHTENED from the observed value by the
//    validating agent, not loosened.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, PulseVarianceGrowsAtTwoDT) {
    auto mesh = makeStrip(80, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    opts.dispersion = 0.05;
    const double T = 400.0;
    auto s = makePond(mesh, opts, 1, 1.0, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (std::fabs(mesh.tri_cx[i] - 40.0) < 2.0)
            s.transport.cell_mass[s.transport.idx(0, i)] = 10.0 * s.volume[i];
    double mean0, var0;
    xMoments(mesh, s.transport, 0, mean0, var0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, T);

    double mean1, var1;
    xMoments(mesh, s.transport, 0, mean1, var1);
    EXPECT_NEAR(mean1, mean0, 1.0e-6) << "pulse drifted on a still pond";
    const double growth = var1 - var0, expect = 2.0 * opts.dispersion * T;
    // Band TIGHTENED from the handoff's uncalibrated [0.6, 1.4] per its own
    // instruction: the check measured growth/expect = 1.006944 on this mesh
    // — the two-point face flux recovers the continuum Fickian rate to
    // 0.7 %, so the §5 concern (S2b gradient-based dispersion needed for
    // consistency) is closed in the negative.
    EXPECT_GT(growth, 0.9 * expect);
    EXPECT_LT(growth, 1.1 * expect);
    EXPECT_EQ(s.transport.dispersion_limiter_binds, 0L);
}

// ---------------------------------------------------------------------------
// 5. Limiter: a D far above the explicit limit MUST bind, and with it bound
//    mass is still conserved and the max principle still holds — the
//    limiter degrades the rate, never the physics.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, OverResolvedDispersionBindsLimiterAndStaysBounded) {
    auto mesh = makeStrip(40, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    opts.dispersion = 20.0;   // D·dt/dx² ≫ ½
    auto s = makePond(mesh, opts, 1, 1.0, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] < 20.0)
            s.transport.cell_mass[s.transport.idx(0, i)] = 10.0 * s.volume[i];
    const double m0 = s.transport.totalIncludingLedgers(0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, 60.0);

    const auto w = wetStats(mesh, opts, s, 0, 0.0);
    EXPECT_GT(s.transport.dispersion_limiter_binds, 0L)
        << "an unstable explicit D did not trip the limiter";
    EXPECT_NEAR(s.transport.totalIncludingLedgers(0), m0, 1.0e-10 * m0);
    EXPECT_LE(w.cmax, 10.0 * (1.0 + 1.0e-10));
    EXPECT_GE(w.cmin, -1.0e-12);
}

// ---------------------------------------------------------------------------
// 6. D = 0 books nothing: a step in a still pond is bit-identical before and
//    after (no face touches the species accumulators), so S1 decks are
//    unchanged by S2's presence.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, ZeroDispersionIsInert) {
    auto mesh = makeStrip(20, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    opts.dispersion = 0.0;
    auto s = makePond(mesh, opts, 1, 1.0, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] < 10.0)
            s.transport.cell_mass[s.transport.idx(0, i)] = 10.0 * s.volume[i];
    const auto mass0 = s.transport.cell_mass;

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, 120.0);

    EXPECT_EQ(s.transport.cell_mass, mass0);
    EXPECT_EQ(s.transport.dispersion_limiter_binds, 0L);
}

// ---------------------------------------------------------------------------
// 7. Boundary inflow at the pond's concentration keeps the pond uniform to
//    round-off, and the gained_boundary ledger equals inflow volume × c.
//    Falsifies a source booked to the wrong slot, wrong species stride, or
//    against a volume other than the one the BC actually added.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, BoundaryInflowAtPondConcentrationKeepsPondUniform) {
    auto mesh = makeStrip(20, 4, 1.0);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    const double c0 = 4.0;
    auto s = makePond(mesh, opts, 2, 0.5, c0);   // two species: stride check
    BoundaryData bd;
    bd.resize(3 * mesh.n_triangles());
    const int slot = boundaryEdgeOnX(mesh, 0.0);
    ASSERT_GE(slot, 0);
    bd.edge_bc_type[slot] = static_cast<int8_t>(BoundaryType::SPECIFIED_FLOW);
    bd.edge_bc_flow[slot] = -1.0e-3;   // outward per metre; negative = INFLOW
    s.boundary = &bd;
    // Species 0 arrives at c0 (pond stays uniform); species 1 arrives clean.
    s.transport.bc_quality_rows.push_back({slot, 0, c0});
    const double m0_s0 = s.transport.totalIncludingLedgers(0);
    const double m0_s1 = s.transport.totalIncludingLedgers(1);
    double vol0 = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) vol0 += s.volume[i];

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    run(solver, 600.0);

    double vol1 = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) vol1 += s.volume[i];
    const double v_in = vol1 - vol0;
    ASSERT_GT(v_in, 0.0) << "the inflow boundary admitted no water";

    const auto w0 = wetStats(mesh, opts, s, 0, c0);
    EXPECT_LT(w0.worst_rel, 1.0e-12)
        << "inflow at c == c0 changed a cell: boundary species not booked "
           "against the volume the BC added";
    EXPECT_NEAR(s.transport.gained_boundary[0], v_in * c0, 1.0e-9 * v_in * c0);
    EXPECT_NEAR(s.transport.totalIncludingLedgers(0), m0_s0, 1.0e-10 * m0_s0);
    // Species 1 got nothing from the boundary and diluted.
    EXPECT_EQ(s.transport.gained_boundary[1], 0.0);
    EXPECT_NEAR(s.transport.totalIncludingLedgers(1), m0_s1, 1.0e-10 * m0_s1);
    // The dilution shows where the clean water ARRIVES — the BC cell's
    // column — not across the whole strip: 0.6 m³ into a 40 m³ pond leaves
    // the far end at exactly c0 after 600 s (the handoff's draft asserted
    // cmax < c0, which demands the front traverse the strip). No cell may
    // EXCEED c0 (nothing concentrates species 1), and at least one cell
    // must have measurably diluted.
    const auto w1 = wetStats(mesh, opts, s, 1, 0.0);
    EXPECT_LE(w1.cmax, c0 * (1.0 + 1.0e-12));
    EXPECT_LT(w1.cmin, c0 * (1.0 - 1.0e-6))
        << "no cell diluted: the clean boundary water never entered";
}

// ---------------------------------------------------------------------------
// 8. A [2D_BOUNDARY_QUALITY] row on a WALL edge is refused at initialize —
//    a concentration on an edge that admits no water describes nothing.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS2, BoundaryQualityOnWallEdgeIsRefused) {
    auto mesh = makeStrip(4, 2, 1.0);
    SolverOptions2D opts;
    auto s = makePond(mesh, opts, 1, 0.5, 1.0);
    BoundaryData bd;
    bd.resize(3 * mesh.n_triangles());
    s.boundary = &bd;
    const int slot = boundaryEdgeOnX(mesh, 0.0);   // left as WALL
    ASSERT_GE(slot, 0);
    s.transport.bc_quality_rows.push_back({slot, 0, 1.0});
    ExplicitInertialSolver solver;
    EXPECT_THROW(solver.initialize(mesh, s, opts), std::runtime_error);
}

// ---------------------------------------------------------------------------
// 9. [2D_INITIAL_QUALITY] end-to-end through the DECK path — added by the
//    check: the handoff's eight gates all drive the solver directly, so the
//    parser, the pending rows, the `* < TAG < CELL` precedence and the
//    mass = conc × initial-volume seeding had NO observer (lesson 223's
//    shape at the section level). Four cells, three scopes, two species,
//    plus the refusal legs D-2DT-style fatality demands.
//
//    Fixtures use the `_s2iq_` prefix (collision-checked).
// ---------------------------------------------------------------------------
namespace {

/// The rain-on-grid pan test_2d_infil_integration.cpp established: a flat
/// 20×20 m closed basin of four tagged triangles with initial standing
/// water, an uncoupled minimal 1D network so the routing loop exists, and
/// no boundary — nothing moves, so the seeded concentrations are read back
/// exactly as seeded.
std::string iqDeck(const std::string& iq_section) {
    return "[OPTIONS]\n"
           "FLOW_UNITS           CMS\nFLOW_ROUTING         DYNWAVE\n"
           "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
           "END_DATE             01/01/2026\nEND_TIME             00:10:00\n"
           "REPORT_STEP          00:05:00\nROUTING_STEP         10\n\n"
           "[POLLUTANTS]\n"
           ";;Name Units Crain Cgw Crdii Kdecay\n"
           "Cu     MG/L  0     0   0     0\n"
           "Zn     MG/L  0     0   0     0\n\n"
           "[JUNCTIONS]\nJ1 0.0 1.0 0 0 0\n\n"
           "[OUTFALLS]\nO1 -0.5 FREE NO\n\n"
           "[CONDUITS]\nC1 J1 O1 30.0 0.013 0 0 0\n\n"
           "[XSECTIONS]\nC1 CIRCULAR 0.3 0 0 0 1\n\n"
           "[2D_OPTIONS]\nMAX_TIMESTEP 2\nDRY_DEPTH 0.001\nREPORT_2D NO\n\n"
           "[2D_VERTICES]\n"
           " 0.0    0.0   10.0\n20.0    0.0   10.0\n"
           "20.0   20.0   10.0\n 0.0   20.0   10.0\n10.0   10.0   10.0\n\n"
           "[2D_TRIANGLES]\n"
           ";;V1 V2 V3 MANNINGS_N INIT_DEPTH TAG\n"
           "0  1  4  0.03  0.5  LAWN\n"
           "1  2  4  0.03  0.5  WOODS\n"
           "2  3  4  0.03  0.5  LAWN\n"
           "3  0  4  0.03  0.5  PAVED\n\n" +
           iq_section +
           "[REPORT]\nINPUT NO\n";
}

void write_iq(const std::string& p, const std::string& body) {
    std::ofstream f(p);
    ASSERT_TRUE(f.is_open()) << p;
    f << body;
}

/// Open + initialize + start; returns the engine on full success, nullptr
/// otherwise (destroyed). The refusal legs assert nullptr — the router's
/// initialize throws are routed through the engine's error path.
SWMM_Engine iqOpen(const std::string& tag, const std::string& body) {
    write_iq(tag + ".inp", body);
    SWMM_Engine e = swmm_engine_create();
    if (!e) return nullptr;
    if (swmm_engine_open(e, (tag + ".inp").c_str(), (tag + ".rpt").c_str(),
                         (tag + ".out").c_str(), nullptr) != SWMM_OK ||
        swmm_engine_initialize(e) != SWMM_OK ||
        swmm_engine_start(e, 1) != SWMM_OK) {
        swmm_engine_destroy(e);
        return nullptr;
    }
    return e;
}

}  // namespace

TEST(SurfaceTransportS2, InitialQualitySeedsThroughTheDeckAtAllThreeScopes) {
    SWMM_Engine e = iqOpen("_s2iq_ok", iqDeck(
        "[2D_INITIAL_QUALITY]\n"
        "* Cu 2.0\n"
        "TAG LAWN Cu 5.0\n"
        "CELL 2 Cu 9.0\n"
        "* Zn 1.5\n\n"));
    ASSERT_NE(e, nullptr) << "a valid [2D_INITIAL_QUALITY] deck must open";

    const auto& router =
        static_cast<openswmm::SWMMEngine*>(e)->surfaceRouter2D();
    const auto& st = router.state();
    const auto& tr = st.transport;
    ASSERT_EQ(tr.n_species, 2);
    ASSERT_EQ(tr.n_cells, 4);

    // Precedence `* < TAG < CELL`, by kind not file order: cells 0 and 2
    // are LAWN (5.0 beats the 2.0 broadcast), cell 1 is CELL 2 in the
    // user's 1-based spelling (9.0 beats both), cell 3 falls through to
    // the broadcast. Zn broadcast everywhere. Mass = conc × the cell's
    // INITIAL volume, so reading back mass/volume before anything moves
    // recovers the concentrations exactly.
    const double cu[4] = {5.0, 9.0, 5.0, 2.0};
    for (int c = 0; c < 4; ++c) {
        ASSERT_GT(st.volume[c], 0.0);
        EXPECT_NEAR(tr.cell_mass[tr.idx(0, c)] / st.volume[c], cu[c],
                    1.0e-12 * cu[c]) << "Cu at cell " << c;
        EXPECT_NEAR(tr.cell_mass[tr.idx(1, c)] / st.volume[c], 1.5,
                    1.5e-12) << "Zn at cell " << c;
    }
    swmm_engine_destroy(e);

    // Refusals — each row would silently seed nothing, so each is fatal.
    EXPECT_EQ(iqOpen("_s2iq_sp", iqDeck(
        "[2D_INITIAL_QUALITY]\n* Pb 1.0\n\n")), nullptr)
        << "unknown species accepted";
    EXPECT_EQ(iqOpen("_s2iq_tag", iqDeck(
        "[2D_INITIAL_QUALITY]\nTAG MEADOW Cu 1.0\n\n")), nullptr)
        << "tag matching no triangle accepted";
    EXPECT_EQ(iqOpen("_s2iq_cell", iqDeck(
        "[2D_INITIAL_QUALITY]\nCELL 99 Cu 1.0\n\n")), nullptr)
        << "off-mesh cell accepted";
}
