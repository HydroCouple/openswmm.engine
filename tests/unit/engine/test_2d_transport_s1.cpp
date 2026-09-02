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
 * @file test_2d_transport_s1.cpp
 * @brief Overland transport S1 — species mass rides the LTS marcher.
 *
 * @details These gates drive `ExplicitInertialSolver` directly on the graded
 *          strip `test_2d_lts_equivalence.cpp` established, so a species
 *          front crosses REAL tier interfaces and REAL wet/dry fronts.
 *
 *          **Gate 1 is the whole of S1's claim** (D-2DT3): a uniform
 *          concentration must survive arbitrary flow to ROUND-OFF. It is the
 *          one property that catches a tier-cadence mismatch, a donor read at
 *          the wrong substep, and a mass/volume inconsistency all at once —
 *          and nothing else does. Its falsifier (gate 5) forces every cell to
 *          tier 0; if the property only holds there, the tier handling is
 *          untested and gate 1 is a false comfort.
 *
 *          The fixtures are CLOSED domains with no species sources, because
 *          S1's sources (rain, boundary inflow, coupling spill) arrive at
 *          zero concentration by design — a uniformity test with an inflow
 *          would fail for the right reason and prove nothing.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"

using namespace openswmm::twoD;

namespace {

/// nx cells of geometrically growing width (dx0, dx0·r, …) — the same
/// multiscale strip the LTS gates use, so cells land in DIFFERENT tiers.
MeshData makeGradedStrip(int nx, int ny, double dx0, double ratio,
                         double slope = 0.0, double n = 0.03) {
    MeshData mesh;
    std::vector<double> xs(static_cast<std::size_t>(nx + 1), 0.0);
    double dx = dx0;
    for (int i = 1; i <= nx; ++i) {
        xs[static_cast<std::size_t>(i)] = xs[static_cast<std::size_t>(i - 1)] + dx;
        dx *= ratio;
    }
    const int nvx = nx + 1, nvy = ny + 1;
    mesh.resize_vertices(nvx * nvy);
    for (int j = 0; j < nvy; ++j)
        for (int i = 0; i < nvx; ++i) {
            const int v = j * nvx + i;
            mesh.vx[v] = xs[static_cast<std::size_t>(i)];
            mesh.vy[v] = j * dx0;
            mesh.vz[v] = -slope * xs[static_cast<std::size_t>(i)];
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

SurfaceStateData makeState(const MeshData& mesh, int n_species) {
    SurfaceStateData s;
    s.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        s.head[i] = mesh.tri_cz[i];
        s.depth[i] = 0.0;
        s.volume[i] = 0.0;
    }
    s.transport.resize(n_species, mesh.n_triangles(), 0);
    return s;
}

/// Dam-break: a 1.5 m column over x < x_dam, dry beyond; species seeded at
/// concentration `c0` in the wet cells (mass = c0 · V).
void seedDamBreak(const MeshData& mesh, const SolverOptions2D& opts,
                  SurfaceStateData& s, double x_dam, double c0) {
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (mesh.tri_cx[i] < x_dam) {
            s.volume[i] = 1.5 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, s.volume[i], s.head[i],
                                   s.depth[i]);
            for (int sp = 0; sp < s.transport.n_species; ++sp)
                s.transport.cell_mass[s.transport.idx(sp, i)] =
                    c0 * s.volume[i];
        }
    }
}

double totalMass(const SurfaceTransportState& tr, int sp) {
    return tr.totalIncludingLedgers(sp);
}

}  // namespace

// ---------------------------------------------------------------------------
// 1. D-2DT3 — THE S1 gate. Uniform concentration under a multiscale dam-break
//    across 6 LTS tiers and a moving wet/dry front stays uniform to round-off.
//
//    Asserted per wet cell as |c − c0| / c0 against 1e-12, and ALSO as the
//    global mass total, so a compensating error (mass leaking from one cell
//    into its neighbour) is caught by the first even though the second is
//    silent on it.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS1, UniformConcentrationSurvivesTieredDamBreak) {
    auto mesh = makeGradedStrip(24, 4, 1.0, 1.08, 0.0, 0.10);
    SolverOptions2D opts;
    opts.lts_tiers = 6;
    auto s = makeState(mesh, 1);
    const double c0 = 7.25;
    seedDamBreak(mesh, opts, s, 4.0, c0);
    const double m0 = totalMass(s.transport, 0);
    ASSERT_GT(m0, 0.0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    for (double t = 0.0; t < 300.0; t += 5.0)
        solver.advance(t, std::min(t + 5.0, 300.0));

    // Every wet cell reads c0. The dry threshold is the hydraulics' own.
    int wet = 0;
    double worst = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double v_dry = opts.dry_depth * mesh.tri_area[i];
        if (!(s.volume[i] > v_dry)) continue;
        ++wet;
        const double c = s.transport.concentration(0, i, s.volume[i], v_dry);
        worst = std::max(worst, std::fabs(c - c0) / c0);
    }
    ASSERT_GT(wet, 20) << "the front did not spread — deck not discriminating";
    EXPECT_LT(worst, 1.0e-12)
        << "a wet cell departed from the seed concentration: the species "
           "flux is not riding the volume flux on the same cadence";
    EXPECT_NEAR(totalMass(s.transport, 0), m0, 1.0e-10 * m0)
        << "total species mass drifted (closed domain, no sources)";
}

// ---------------------------------------------------------------------------
// 2. A species FRONT (step in concentration mid-column) is transported, mass
//    conserved to reduction round-off, and the discrete max principle holds:
//    no cell ever exceeds the initial max or falls below the initial min.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS1, FrontConservesMassAndObeysMaxPrinciple) {
    auto mesh = makeGradedStrip(24, 4, 1.0, 1.08, 0.0, 0.10);
    SolverOptions2D opts;
    opts.lts_tiers = 4;
    auto s = makeState(mesh, 1);
    seedDamBreak(mesh, opts, s, 4.0, 0.0);
    // Concentration step INSIDE the wet column: x < 2 tagged at 10, else 0.
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] < 2.0 && s.volume[i] > 0.0)
            s.transport.cell_mass[s.transport.idx(0, i)] = 10.0 * s.volume[i];
    const double m0 = totalMass(s.transport, 0);
    ASSERT_GT(m0, 0.0);

    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    double cmax_seen = 0.0, cmin_seen = 1e300;
    for (double t = 0.0; t < 240.0; t += 5.0) {
        solver.advance(t, std::min(t + 5.0, 240.0));
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const double v_dry = opts.dry_depth * mesh.tri_area[i];
            if (!(s.volume[i] > v_dry)) continue;
            const double c = s.transport.concentration(0, i, s.volume[i], v_dry);
            cmax_seen = std::max(cmax_seen, c);
            cmin_seen = std::min(cmin_seen, c);
        }
    }
    EXPECT_NEAR(totalMass(s.transport, 0), m0, 1.0e-10 * m0);
    EXPECT_LE(cmax_seen, 10.0 * (1.0 + 1.0e-10)) << "new maximum created";
    EXPECT_GE(cmin_seen, -1.0e-12)                << "negative concentration";
    // And the front actually moved: cells past the dam carry species now.
    double downstream = 0.0;
    for (int i = 0; i < mesh.n_triangles(); ++i)
        if (mesh.tri_cx[i] > 4.0)
            downstream += s.transport.cell_mass[s.transport.idx(0, i)];
    EXPECT_GT(downstream, 0.05 * m0) << "species never crossed the dam line";
}

// ---------------------------------------------------------------------------
// 3. Two species, different seeds, one solver: the layout is species-major
//    and the accumulators are per species, so the two must not bleed. A
//    stride bug reads plausibly on ONE species and only shows with two.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS1, TwoSpeciesDoNotBleed) {
    auto mesh = makeGradedStrip(16, 3, 1.0, 1.08, 0.0, 0.10);
    SolverOptions2D opts;
    opts.lts_tiers = 3;
    auto s = makeState(mesh, 2);
    seedDamBreak(mesh, opts, s, 4.0, 0.0);
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        if (!(s.volume[i] > 0.0)) continue;
        s.transport.cell_mass[s.transport.idx(0, i)] = 3.0 * s.volume[i];
        s.transport.cell_mass[s.transport.idx(1, i)] = 11.0 * s.volume[i];
    }
    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    for (double t = 0.0; t < 200.0; t += 5.0)
        solver.advance(t, std::min(t + 5.0, 200.0));

    // Both uniform in the wet region (gate 1's property, per species), at
    // their OWN seeds. Species 1 reading 3 or species 0 reading 11 is the
    // stride bug this exists to catch.
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double v_dry = opts.dry_depth * mesh.tri_area[i];
        if (!(s.volume[i] > v_dry)) continue;
        EXPECT_NEAR(s.transport.concentration(0, i, s.volume[i], v_dry), 3.0,
                    3.0e-12) << "cell " << i;
        EXPECT_NEAR(s.transport.concentration(1, i, s.volume[i], v_dry), 11.0,
                    11.0e-12) << "cell " << i;
    }
}

// ---------------------------------------------------------------------------
// 4. Infiltration removes mass at the cell's concentration and BOOKS it: the
//    ledger + surface total is invariant, the surface alone falls, and the
//    concentration does not change (water and mass leave together).
//    Evaporation, by contrast, removes water only — concentration RISES.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS1, InfiltrationBooksMassAndEvaporationConcentrates) {
    // Flat, closed, uniform pond — no advection, sinks only.
    auto mesh = makeGradedStrip(6, 2, 2.0, 1.0, 0.0, 0.05);
    SolverOptions2D opts;
    opts.lts_tiers = 1;

    // (a) infiltration
    {
        auto s = makeState(mesh, 1);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            s.volume[i] = 0.5 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, s.volume[i], s.head[i],
                                   s.depth[i]);
            s.transport.cell_mass[s.transport.idx(0, i)] = 4.0 * s.volume[i];
            // 1e-4 m/s x 600 s = 60 mm from a 500 mm pond = 12 % of the
            // volume, comfortably past the 5 % bar below. The handoff's
            // draft used 1e-5, which removes 1.2 % — the mechanism worked
            // (surface read exactly 0.988·m0) and the gate failed on its
            // own arithmetic, not on the code.
            s.infil_rate[i] = 1.0e-4;   // m/s
        }
        const double m0 = totalMass(s.transport, 0);
        ExplicitInertialSolver solver;
        solver.initialize(mesh, s, opts);
        for (double t = 0.0; t < 600.0; t += 10.0) solver.advance(t, t + 10.0);

        double surf = 0.0;
        for (int i = 0; i < mesh.n_triangles(); ++i)
            surf += s.transport.cell_mass[s.transport.idx(0, i)];
        EXPECT_LT(surf, 0.95 * m0) << "infiltration removed no species";
        EXPECT_GT(s.transport.lost_infiltration[0], 0.0);
        EXPECT_NEAR(totalMass(s.transport, 0), m0, 1.0e-10 * m0)
            << "mass left the surface without reaching the ledger";
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const double v_dry = opts.dry_depth * mesh.tri_area[i];
            if (s.volume[i] > v_dry)
                EXPECT_NEAR(s.transport.concentration(0, i, s.volume[i], v_dry),
                            4.0, 4.0e-9) << "infiltration changed concentration";
        }
    }
    // (b) evaporation
    {
        auto s = makeState(mesh, 1);
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            s.volume[i] = 0.5 * mesh.tri_area[i];
            inertial::cellEtaDepth(mesh, opts, i, s.volume[i], s.head[i],
                                   s.depth[i]);
            s.transport.cell_mass[s.transport.idx(0, i)] = 4.0 * s.volume[i];
            s.evap_rate[i] = 1.0e-5;
        }
        const double m0 = totalMass(s.transport, 0);
        ExplicitInertialSolver solver;
        solver.initialize(mesh, s, opts);
        for (double t = 0.0; t < 600.0; t += 10.0) solver.advance(t, t + 10.0);

        EXPECT_NEAR(totalMass(s.transport, 0), m0, 1.0e-12 * m0)
            << "evaporation must remove NO species mass";
        EXPECT_DOUBLE_EQ(s.transport.lost_infiltration[0], 0.0);
        int checked = 0;
        for (int i = 0; i < mesh.n_triangles(); ++i) {
            const double v_dry = opts.dry_depth * mesh.tri_area[i];
            if (!(s.volume[i] > v_dry)) continue;
            EXPECT_GT(s.transport.concentration(0, i, s.volume[i], v_dry), 4.01)
                << "evaporation did not up-concentrate — KD1's 1D defect, "
                   "reproduced on the surface";
            ++checked;
        }
        ASSERT_GT(checked, 0);
    }
}

// ---------------------------------------------------------------------------
// 5. THE FALSIFIER FOR GATE 1. Same dam-break with every cell forced to tier
//    0 (lts_tiers = 1). Gate 1's property must STILL hold here — and the
//    point is the comparison: if a future change makes gate 1 fail at K=6
//    while this passes, the defect is in the tier handling and nowhere else.
//    If both fail, it is the donor timing or the mass/volume bookkeeping.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS1, UniformConcentrationAtSingleTierIsTheControl) {
    auto mesh = makeGradedStrip(24, 4, 1.0, 1.08, 0.0, 0.10);
    SolverOptions2D opts;
    opts.lts_tiers = 1;
    auto s = makeState(mesh, 1);
    const double c0 = 7.25;
    seedDamBreak(mesh, opts, s, 4.0, c0);
    ExplicitInertialSolver solver;
    solver.initialize(mesh, s, opts);
    for (double t = 0.0; t < 300.0; t += 5.0)
        solver.advance(t, std::min(t + 5.0, 300.0));
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        const double v_dry = opts.dry_depth * mesh.tri_area[i];
        if (!(s.volume[i] > v_dry)) continue;
        EXPECT_NEAR(s.transport.concentration(0, i, s.volume[i], v_dry), c0,
                    c0 * 1.0e-12) << "cell " << i;
    }
}

// ---------------------------------------------------------------------------
// 6. Transport OFF (n_species = 0) allocates nothing in the marcher and the
//    hydrodynamics are bit-identical to a run with it on: species must be a
//    pure passenger.
// ---------------------------------------------------------------------------
TEST(SurfaceTransportS1, TransportIsAPassengerOnTheHydrodynamics) {
    auto mesh = makeGradedStrip(24, 4, 1.0, 1.08, 0.0, 0.10);
    SolverOptions2D opts;
    opts.lts_tiers = 4;

    auto off = makeState(mesh, 0);
    seedDamBreak(mesh, opts, off, 4.0, 0.0);
    auto on = makeState(mesh, 2);
    seedDamBreak(mesh, opts, on, 4.0, 5.0);
    ASSERT_FALSE(off.transport.active());
    ASSERT_TRUE(on.transport.active());

    ExplicitInertialSolver a, b;
    a.initialize(mesh, off, opts);
    b.initialize(mesh, on, opts);
    for (double t = 0.0; t < 240.0; t += 5.0) {
        a.advance(t, std::min(t + 5.0, 240.0));
        b.advance(t, std::min(t + 5.0, 240.0));
    }
    for (int i = 0; i < mesh.n_triangles(); ++i)
        EXPECT_EQ(off.volume[i], on.volume[i])
            << "species transport perturbed the water at cell " << i;
}
