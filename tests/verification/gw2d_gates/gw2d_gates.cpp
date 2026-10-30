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
 * @file gw2d_gates.cpp
 * @brief The two-zone groundwater plan's numerical gates, as a standalone
 *        program that needs no gtest, no project file and no engine.
 *
 * @section why Why this is a `main()` and not a gtest
 *
 * These are conservation identities on a kernel, and they are the FIRST thing
 * anyone picking this work up should run. A binary that links four translation
 * units and prints a table of residuals can be run before the engine builds,
 * bisected against, and pasted into a review. A gtest that needs the whole
 * engine target cannot. The authoring surface — sections, round-trip, the C
 * API — is tested the other way, in `tests/unit/engine/test_2d_aquifer.cpp`,
 * because that genuinely needs a model.
 *
 * @section order Run them in this order and stop at the first failure
 *
 * The order is the plan's own, and it is not cosmetic: each gate assumes the
 * ones above it hold. A lateral-Darcy residual means nothing if the single
 * cell does not conserve, and a cross-tier residual means nothing if the
 * lateral flux is wrong. Diagnosing gate 5 with gate 2 broken is how a day
 * disappears.
 *
 * | # | Gate | What a failure means |
 * |---|---|---|
 * | 0 | the σ column alone           | closure B's ALE bookkeeping (see below) |
 * | 1 | closure A, one cell, closed  | the bulk ODE's handover term |
 * | 2 | closure A, one cell, forced  | `q⁺` bookkeeping or the ledger |
 * | 3 | SIGMA, one cell, forced      | the σ handover, or specific yield |
 * | 4 | ENSLAVED, one cell, forced   | `d(hᵤ*)/dL = θ(L)` is not holding |
 * | 5 | Dunne closes                 | saturation excess is being lost |
 * | 6 | lateral Darcy, two cells     | the ±ΔV side accumulators |
 * | 7 | lateral Darcy equilibrates   | upwind transmissivity or the sign |
 * | 8 | cross-tier == single-tier    | G-B: a cadence is stranding volume |
 *
 * Gate 0 runs FIRST and tests `advanceColumn` on its own, with no solver, no
 * mesh and no table: closure B is the only part of the kernel with a moving
 * grid, and a σ bug shows up in gates 1b/3 as "the SIGMA closure is broken"
 * with no clue which half. Its four checks are the ones the plan names —
 * plan gate 5 (the ALE identity), the closed-column oscillation that caught
 * the clamping bug `SigmaColumn.cpp` §6 describes, the m-convergence sweep,
 * and the reported-flux audit:
 *
 *   ΔS  ==  Δt·( f_top − et_taken − f_bot − overflow_to_sat + deficit_from_sat )
 *
 * That identity is the column's whole contract with `SubsurfaceSolver`, which
 * books every one of those five numbers. A column that conserves internally
 * but MIS-REPORTS one of them leaks in the solver while passing any gate that
 * only sums `columnStorage`, so the residual below is deliberately computed
 * from the reported fluxes rather than from the state.
 *
 * Every gate reports an absolute residual in m³ (gate 0: m of water) against a
 * storage of order 1, so `1e-12` is machine noise and `1e-6` is a bug.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/subsurface/SigmaColumn.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace openswmm::twoD;

namespace {

int g_failures = 0;

void report(const char* name, double residual, double tol) {
    const bool ok = std::isfinite(residual) && std::fabs(residual) <= tol;
    if (!ok) ++g_failures;
    std::printf("  %-46s %11.3e  (tol %7.1e)  %s\n", name, residual, tol,
                ok ? "PASS" : "**FAIL**");
}

// ---------------------------------------------------------------------------
// Gate 0: the σ column on its own (closure B)
// ---------------------------------------------------------------------------

soil::Params sigmaSoil() {
    soil::Params p;
    p.law     = SoilChar::RUSSO;
    p.Ks      = 1.0e-5;
    p.theta_s = 0.45;
    p.theta_r = 0.10;
    p.alpha   = 2.0;
    return p;
}

/**
 * @brief March one σ column and return the worst violation of its reported
 *        conservation identity, in metres of water.
 *
 * @param m       layer count
 * @param nsteps  steps to march
 * @param dt      step (s)
 * @param table   `step -> L`, the column thickness. Driving L directly is what
 *                makes this a test of the ALE machinery rather than of the
 *                saturated solver that would otherwise choose L.
 * @param q_in    infiltration offered at the top (m/s)
 * @param q_et    ET demand at the top (m/s)
 */
double sigmaResidual(int m, int nsteps, double dt,
                     double (*table)(int, double), double q_in, double q_et) {
    const soil::Params p = sigmaSoil();
    std::vector<double> theta(static_cast<std::size_t>(m), 0.0);
    double L = table(0, 0.0);
    sigma::seedHydrostatic(p, theta.data(), m, 1, L);

    double worst = 0.0;
    for (int s = 0; s < nsteps; ++s) {
        const double L_next = table(s + 1, dt);
        const double S0 = sigma::columnStorage(theta.data(), m, 1, L);

        sigma::ColumnStep st;
        st.L_old = L;
        st.L_new = L_next;
        st.dt    = dt;
        st.q_in  = q_in;
        st.q_et  = q_et;
        sigma::advanceColumn(p, theta.data(), m, 1, st);

        const double S1 = sigma::columnStorage(theta.data(), m, 1, L_next);
        // The identity the solver relies on. Every term is one the solver
        // books somewhere, so a mis-report here is a leak there.
        const double booked = dt * (st.f_top - st.et_taken - st.f_bot
                                    - st.overflow_to_sat + st.deficit_from_sat);
        worst = std::max(worst, std::fabs((S1 - S0) - booked));
        L = L_next;
    }
    return worst;
}

/// A table that does not move: the pure-flux limit of the identity.
double tableStill(int, double) { return 2.0; }

/// A table falling steadily — the column grows, faces sweep DOWN, and the
/// grid term takes its donor from below. The sign of that upwind choice is
/// what plan gate 5 is really testing.
double tableFalling(int s, double dt) {
    return 2.0 + 1.0e-4 * dt * static_cast<double>(s);
}

/// The closed-column oscillation: the table rises and falls through the same
/// range for the whole run. This is the gate that caught the clamping bug —
/// a rising table compresses the layers past θ_s, and the surplus has to go
/// somewhere it can be counted.
double tableOscillating(int s, double dt) {
    const double t = dt * static_cast<double>(s);
    return 2.0 + 1.2 * std::sin(2.0 * 3.14159265358979323846 * t / 3600.0);
}

void gateSigmaColumn() {
    const int    m  = 8;
    const int    ns = 2000;
    const double dt = 1.0;

    report("0a. sigma: still column books its fluxes",
           sigmaResidual(m, ns, dt, tableStill, 0.0, 0.0), 1.0e-12);

    // Plan gate 5 — the ALE identity under a moving lower boundary.
    report("0b. sigma: ALE handover across a moving table",
           sigmaResidual(m, ns, dt, tableFalling, 0.0, 0.0), 1.0e-12);

    // The oscillation, driven hard enough to saturate and to stretch.
    report("0c. sigma: closed-column oscillation",
           sigmaResidual(m, ns, dt, tableOscillating, 0.0, 0.0), 1.0e-12);

    // Forced at BOTH ends at once: infiltration competing with ET is the one
    // arrangement in which the top face can be scaled by the positivity share
    // AFTER f_top and et_taken were reported, so it is the case that audits
    // the reported numbers rather than the state.
    // Forced at both ends, with an ET demand two orders past anything the top
    // layer can supply. The top face is the one flux `advanceColumn` reports
    // BEFORE the positivity share runs (§5 rescales f[0] but does not revisit
    // st.f_top / st.et_taken), so if the share ever scaled it the reported
    // numbers would drift from the applied ones and this residual — computed
    // from the reported numbers — would grow.
    //
    // It does not, and the reason is worth writing down: Feddes stress drives
    // the demand to zero as the layer dries, so ET is throttled well below its
    // availability cap, and the share could only bind the top face if gravity
    // alone drained layer 0 within one step — which needs dt > (θ₀−θ_r)·dz/K,
    // some 8·10³ s here, far past the column's own dt_limit. The gate holds
    // because the CFL bound protects it, not by luck.
    report("0d. sigma: infiltration and ET compete at the top",
           sigmaResidual(m, ns, dt, tableOscillating, 2.0e-6, 1.0e-3), 1.0e-12);

    // m-convergence: the identity is a property of the discretisation, not of
    // a lucky layer count. Worst residual across the sweep.
    double worst_m = 0.0;
    for (const int mm : {4, 8, 16, 32})
        worst_m = std::max(worst_m,
                           sigmaResidual(mm, 500, dt, tableOscillating, 1.0e-6, 0.0));
    report("0e. sigma: m in {4,8,16,32} all conserve", worst_m, 1.0e-12);
}

// ---------------------------------------------------------------------------
// Meshes
// ---------------------------------------------------------------------------

/// Fill the derived geometry of a triangle mesh whose connectivity and vertex
/// coordinates are already set. Deliberately explicit rather than calling
/// MeshBuilder: a gate that depends on the mesh builder cannot isolate a
/// kernel bug from a meshing one.
void finishGeometry(MeshData& m) {
    const int nt = m.n_triangles();
    for (int t = 0; t < nt; ++t) {
        const int nv = m.cell_vertex_count(t);
        double cx = 0.0, cy = 0.0, cz = 0.0, a2 = 0.0;
        for (int k = 0; k < nv; ++k) {
            const int va = m.cell_vertex(t, k);
            const int vb = m.cell_vertex(t, (k + 1) % nv);
            a2 += m.vx[va] * m.vy[vb] - m.vx[vb] * m.vy[va];
            cx += m.vx[va];
            cy += m.vy[va];
            cz += m.vz[va];
        }
        m.tri_area[t] = 0.5 * std::fabs(a2);
        m.tri_cx[t] = cx / nv;
        m.tri_cy[t] = cy / nv;
        m.tri_cz[t] = cz / nv;
        m.mannings_n[t] = 0.03;
        for (int k = 0; k < nv; ++k) {
            int va = 0, vb = 0;
            m.cell_edge_vertices(t, k, va, vb);
            const int sl = MeshData::slot(t, k);
            const double dx = m.vx[vb] - m.vx[va];
            const double dy = m.vy[vb] - m.vy[va];
            const double len = std::hypot(dx, dy);
            m.edge_length[sl] = len;
            m.edge_mx[sl] = 0.5 * (m.vx[va] + m.vx[vb]);
            m.edge_my[sl] = 0.5 * (m.vy[va] + m.vy[vb]);
            m.edge_mz[sl] = 0.5 * (m.vz[va] + m.vz[vb]);
            double nxv = (len > 0.0) ? dy / len : 0.0;
            double nyv = (len > 0.0) ? -dx / len : 0.0;
            // Orient outward from the centroid.
            if ((m.edge_mx[sl] - m.tri_cx[t]) * nxv +
                (m.edge_my[sl] - m.tri_cy[t]) * nyv < 0.0) {
                nxv = -nxv;
                nyv = -nyv;
            }
            m.edge_nx[sl] = nxv;
            m.edge_ny[sl] = nyv;
            m.edge_dist_c[sl] =
                std::fabs((m.edge_mx[sl] - m.tri_cx[t]) * nxv +
                          (m.edge_my[sl] - m.tri_cy[t]) * nyv);
            m.edge_conveyance[sl] = 1.0;
        }
    }
}

/// One isolated cell: a unit right triangle, every edge a wall.
MeshData oneCell(double z = 10.0) {
    MeshData m;
    m.resize_vertices(3);
    m.vx = {0.0, 1.0, 0.0};
    m.vy = {0.0, 0.0, 1.0};
    m.vz = {z, z, z};
    m.resize_triangles(1);
    m.set_triangle(0, 0, 1, 2);
    for (int k = 0; k < 3; ++k) m.cell_nbr[MeshData::slot(0, k)] = -1;
    finishGeometry(m);
    return m;
}

/// Two cells sharing one edge, with a bed tilt so a table difference drives a
/// lateral flux. Square split on its diagonal.
MeshData twoCells(double z0 = 10.0, double z1 = 10.0) {
    MeshData m;
    m.resize_vertices(4);
    m.vx = {0.0, 1.0, 0.0, 1.0};
    m.vy = {0.0, 0.0, 1.0, 1.0};
    m.vz = {z0, 0.5 * (z0 + z1), 0.5 * (z0 + z1), z1};
    m.resize_triangles(2);
    m.set_triangle(0, 0, 1, 2);
    m.set_triangle(1, 1, 3, 2);
    for (int t = 0; t < 2; ++t)
        for (int k = 0; k < 3; ++k) m.cell_nbr[MeshData::slot(t, k)] = -1;
    // Cell 0's edge opposite vertex 0 joins vertices 1,2 — the shared edge.
    m.cell_nbr[MeshData::slot(0, 0)] = 1;
    // Cell 1's edge opposite vertex 1 joins vertices 3,2 … the shared edge of
    // cell 1 is the one whose endpoints are {1,2}: local edge k has endpoints
    // v[(k+1)%3], v[(k+2)%3]; for cell 1 (v = 1,3,2) that is k=1 → {2,1}.
    m.cell_nbr[MeshData::slot(1, 1)] = 0;
    finishGeometry(m);
    return m;
}

// ---------------------------------------------------------------------------
// Harness
// ---------------------------------------------------------------------------

struct Rig {
    MeshData            mesh;
    InertialEdges       edges;
    SolverOptions2D     opts;
    SurfaceStateData    surf;
    SubsurfaceConfig    cfg;
    SubsurfaceSolver    gw;
    std::vector<std::string> warnings;

    void build(int n_tiers = 1) {
        edges.build(mesh);
        opts.lts_tiers = n_tiers;
        surf.resize(mesh.n_triangles(), mesh.n_vertices());
        GwUnitFactors uf;   // identity: the rows below are already SI
        const std::string err =
            gw.initialize(mesh, edges, opts, uf, 0, cfg, warnings);
        if (!err.empty()) {
            std::printf("  initialize failed: %s\n", err.c_str());
            ++g_failures;
        }
    }

    /// Total water in both zones plus anything still parked in an accumulator.
    /// A gate that summed only the state would pass while volume sat stranded
    /// on a face — which is exactly the failure G-B exists to prevent, so the
    /// invariant has to include the accumulators.
    double totalWater() const {
        double s = gw.state().storage();
        for (double v : gw.state().eacc_L) s += v;
        for (double v : gw.state().eacc_R) s += v;
        for (double v : gw.state().xacc_from_surface) s += v;
        for (double v : gw.state().xacc_to_surface) s += v;
        return s;
    }

    /// Re-tier from the current state, exactly as syncAndRebuild does.
    void rebuild(double dt0) {
        gw.refreshDtCell(mesh, edges);
        gw.assignTiers(dt0, opts.lts_tiers);
    }

    /// Run `nsub` base substeps of the halving-order macro cycle, exactly as
    /// ExplicitInertialSolver::runMacroCycle drives it.
    void march(double dt0, int nsub) {
        const int K = gw.tierCount();
        for (int s = 0; s < nsub; ++s) {
            for (int k = 0; k < K; ++k) {
                if (s % (1 << k)) continue;
                gw.fireGwFaces(k, (1 << k) * dt0);
            }
            for (int k = 0; k < K; ++k) {
                if (s % (1 << k)) continue;
                gw.fireGwCells(k, (1 << k) * dt0, surf);
            }
        }
        gw.settle(surf);
    }
};

GwAquiferRow baseRow(GwClosure closure, SoilChar law = SoilChar::RUSSO) {
    GwAquiferRow r;
    r.scope   = 0;
    r.Ks      = 1.0e-5;      // m/s
    r.zs      = 4.0;         // m
    r.theta_s = 0.45;
    r.theta_r = 0.10;
    r.alpha   = 2.0;         // 1/m
    r.hg0     = 1.0;         // m
    r.closure = closure;
    r.closure_set = true;
    r.soil_char = law;
    r.soil_char_set = true;
    return r;
}

/// Drain everything the aquifer handed the surface, and report the volume, so
/// a gate can put it back on the books.
double drainToSurface(Rig& rig) {
    double v = 0.0;
    for (int i = 0; i < rig.mesh.n_triangles(); ++i) v += rig.gw.takeToSurface(i);
    rig.gw.compactPending();
    return v;
}

// ---------------------------------------------------------------------------
// Gates 1-4: one cell, one closure at a time
// ---------------------------------------------------------------------------

/// Closed cell, no sources, no sinks: total water must not move at all. This
/// is the gate that catches a handover term with the wrong sign, because a
/// wrong handover shows up as a slow drift with nothing driving it.
void gateClosed(const char* label, GwClosure closure) {
    Rig rig;
    rig.mesh = oneCell();
    rig.cfg.options.authored = true;
    rig.cfg.options.dunne    = true;
    rig.cfg.rows.push_back(baseRow(closure));
    rig.build();

    rig.rebuild(60.0);
    const double S0 = rig.totalWater();
    double worst = 0.0;
    for (int step = 0; step < 400; ++step) {
        rig.march(60.0, 1);
        worst = std::max(worst, std::fabs(rig.totalWater() - S0));
    }
    report(label, worst, 1.0e-9);
}

/// Constant infiltration into a closed cell: storage must rise by exactly the
/// water delivered, less whatever came back up as saturation excess.
void gateForced(const char* label, GwClosure closure, double q_in_mps,
                int nsteps, double dt) {
    Rig rig;
    rig.mesh = oneCell();
    rig.cfg.options.authored = true;
    rig.cfg.rows.push_back(baseRow(closure));
    rig.build();

    rig.rebuild(dt);
    const double A  = rig.mesh.tri_area[0];
    const double S0 = rig.totalWater();
    double delivered = 0.0, returned = 0.0;
    for (int step = 0; step < nsteps; ++step) {
        const double vol = q_in_mps * A * dt;
        rig.gw.bookInfiltrationFromSurface(0, vol);
        delivered += vol;
        rig.march(dt, 1);
        returned += drainToSurface(rig);
    }
    const double S1 = rig.totalWater();
    report(label, (S1 - S0) - (delivered - returned), 1.0e-9 + 1.0e-12 * delivered);
}

/// Push the table to the surface and keep pushing. Everything that arrives
/// after saturation must come back as Dunne excess — none of it may vanish,
/// and none may be invented.
void gateDunne() {
    Rig rig;
    rig.mesh = oneCell();
    rig.cfg.options.authored = true;
    rig.cfg.options.dunne    = true;
    auto r = baseRow(GwClosure::CLOSED_FORM);
    r.hg0 = 3.9;               // a table 0.1 m below the surface
    rig.cfg.rows.push_back(r);
    rig.build();

    rig.rebuild(60.0);
    const double A  = rig.mesh.tri_area[0];
    const double S0 = rig.totalWater();
    double delivered = 0.0, returned = 0.0;
    for (int step = 0; step < 2000; ++step) {
        const double vol = 1.0e-5 * A * 60.0;   // 36 mm/hr, well past capacity
        rig.gw.bookInfiltrationFromSurface(0, vol);
        delivered += vol;
        rig.march(60.0, 1);
        returned += drainToSurface(rig);
    }
    report("5. Dunne: delivered == stored + returned",
           (rig.totalWater() - S0) - (delivered - returned),
           1.0e-9 + 1.0e-12 * delivered);
    // …and it must actually have saturated, or the gate proves nothing.
    if (returned <= 0.0) {
        std::printf("  %-46s %11s               **FAIL** (never saturated)\n",
                    "5b. Dunne actually fired", "-");
        ++g_failures;
    } else {
        std::printf("  %-46s %11.3e m3 returned    PASS\n",
                    "5b. Dunne actually fired", returned);
    }
}

// ---------------------------------------------------------------------------
// Gates 6-8: two cells, lateral Darcy, and the LTS ladder
// ---------------------------------------------------------------------------

Rig makeTwoCell(double hg_left, double hg_right, int n_tiers) {
    Rig rig;
    rig.mesh = twoCells();
    rig.cfg.options.authored = true;
    rig.cfg.options.dunne    = false;   // isolate the lateral term
    auto r = baseRow(GwClosure::CLOSED_FORM);
    r.hg0 = hg_left;
    rig.cfg.rows.push_back(r);
    auto r1 = baseRow(GwClosure::CLOSED_FORM);
    r1.scope = 2;
    r1.cell  = 1;
    r1.hg0   = hg_right;
    rig.cfg.rows.push_back(r1);
    rig.build(n_tiers);
    return rig;
}

void gateLateral() {
    Rig rig = makeTwoCell(3.0, 0.5, 1);
    if (rig.edges.ne != 1) {
        std::printf("  two-cell mesh produced %d interior edges, expected 1\n",
                    rig.edges.ne);
        ++g_failures;
        return;
    }
    rig.rebuild(60.0);
    const double S0 = rig.totalWater();
    const double d0 = std::fabs(rig.gw.state().hg[0] - rig.gw.state().hg[1]);
    double worst = 0.0;
    for (int step = 0; step < 5000; ++step) {
        rig.march(60.0, 1);
        worst = std::max(worst, std::fabs(rig.totalWater() - S0));
    }
    report("6. lateral Darcy conserves total water", worst, 1.0e-9);

    const double d1 = std::fabs(rig.gw.state().hg[0] - rig.gw.state().hg[1]);
    // Flat bed, equal soils: the two tables must approach each other, never
    // separate. A sign error here shows as d1 > d0 and nothing else.
    if (d1 < d0) {
        std::printf("  %-46s %11.3e -> %.3e   PASS\n",
                    "7. lateral Darcy equilibrates the tables", d0, d1);
    } else {
        std::printf("  %-46s %11.3e -> %.3e   **FAIL**\n",
                    "7. lateral Darcy equilibrates the tables", d0, d1);
        ++g_failures;
    }
}

/// The G-B gate. Run the identical problem with one tier and with four, over
/// the same span. Total water must be conserved in BOTH — the trajectories
/// differ (they are different discretisations), but neither may leak.
void gateCrossTier() {
    for (int K : {1, 4}) {
        Rig rig = makeTwoCell(3.0, 0.5, K);
        // Force the two cells onto different rungs so the cross-tier path is
        // actually exercised: without this they may agree on a tier and the
        // gate would pass trivially.
        if (K > 1) {
            rig.gw.state().dt_cell[0] = 60.0;
            rig.gw.state().dt_cell[1] = 480.0;
            rig.gw.assignTiers(60.0, K);
            if (rig.gw.state().tier[0] == rig.gw.state().tier[1]) {
                std::printf("  cross-tier setup did not separate the tiers\n");
                ++g_failures;
            }
        }
        const double S0 = rig.totalWater();
        double worst = 0.0;
        for (int cycle = 0; cycle < 400; ++cycle) {
            rig.march(60.0, 1 << (K - 1));
            worst = std::max(worst, std::fabs(rig.totalWater() - S0));
        }
        char buf[80];
        std::snprintf(buf, sizeof buf, "8. conserves with LTS_TIERS = %d", K);
        report(buf, worst, 1.0e-9);
    }
}

}  // namespace

int main() {
    std::printf("\nTwo-zone groundwater — plan numerical gates\n");
    std::printf("Run in order; stop at the first failure.\n\n");

    std::printf("The sigma column alone (closure B kernel):\n");
    gateSigmaColumn();

    std::printf("\nOne cell, closed (no sources, no sinks):\n");
    gateClosed("1. CLOSED_FORM holds its water", GwClosure::CLOSED_FORM);
    gateClosed("1b. SIGMA holds its water",       GwClosure::SIGMA);
    gateClosed("1c. ENSLAVED holds its water",    GwClosure::ENSLAVED);

    std::printf("\nOne cell, constant infiltration:\n");
    gateForced("2. CLOSED_FORM books what it is given",
               GwClosure::CLOSED_FORM, 1.0e-7, 2000, 60.0);
    gateForced("3. SIGMA books what it is given",
               GwClosure::SIGMA, 1.0e-7, 2000, 60.0);
    gateForced("4. ENSLAVED books what it is given",
               GwClosure::ENSLAVED, 1.0e-7, 2000, 60.0);

    std::printf("\nSaturation excess:\n");
    gateDunne();

    std::printf("\nTwo cells, lateral Darcy:\n");
    gateLateral();

    std::printf("\nCross-tier (guarantee G-B):\n");
    gateCrossTier();

    std::printf("\n%s — %d failure(s)\n\n",
                g_failures == 0 ? "ALL GATES PASS" : "GATES FAILED",
                g_failures);
    return g_failures == 0 ? 0 : 1;
}
