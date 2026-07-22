/**
 * @file test_2d_analytic_jv.cpp
 * @brief Parity of the analytic J·v (SurfaceTangent) against a central
 *        finite difference of the diffusive-wave RHS.
 *
 * The analytic tangent must reproduce ∂f/∂y·v to ~1e-6 (relative) on smooth
 * states for the whole default RHS (interior flux + evaporation). This is the
 * gate that guards the analytic Jacobian shipped as the CVODE J·v — a wrong
 * tangent still "works" (GMRES falls back to more iterations) but destroys the
 * speedup, so it is verified directly here rather than only through wall time.
 *
 * @ingroup engine_2d
 */

#include <gtest/gtest.h>
#include <cmath>
#include <vector>
#include <random>

#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/BoundaryData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VfrClosure.hpp"
#include "2d/solver/SurfaceFluxCalculator.hpp"
#include "2d/solver/SurfaceTangent.hpp"
#include "2d/coupling/NodeCoupling.hpp"
#include "data/NodeData.hpp"

using namespace openswmm::twoD;

namespace {

// A small structured grid (nx×ny cells, union-jack triangulated) with a gentle
// tilt, so there are many interior edges with real (nonzero) flux.
MeshData makeGridMesh(int nx, int ny, double dx, double slope) {
    MeshData mesh;
    const int nvx = nx + 1, nvy = ny + 1;
    const int nv = nvx * nvy;
    const int nt = 2 * nx * ny;
    mesh.resize_vertices(nv);
    auto vid = [&](int ix, int iy) { return iy * nvx + ix; };
    for (int iy = 0; iy < nvy; ++iy)
        for (int ix = 0; ix < nvx; ++ix) {
            const int v = vid(ix, iy);
            mesh.vx[v] = ix * dx;
            mesh.vy[v] = iy * dx;
            mesh.vz[v] = 100.0 + slope * (nx * dx - ix * dx);  // tilt toward +x
        }
    mesh.resize_triangles(nt);
    mesh.mannings_n.assign(nt, 0.03);
    int t = 0;
    for (int iy = 0; iy < ny; ++iy)
        for (int ix = 0; ix < nx; ++ix) {
            const int sw = vid(ix, iy),     se = vid(ix + 1, iy);
            const int nw = vid(ix, iy + 1), ne = vid(ix + 1, iy + 1);
            if ((ix + iy) % 2 == 0) {
                mesh.tri_v0[t] = sw; mesh.tri_v1[t] = se; mesh.tri_v2[t] = ne; ++t;
                mesh.tri_v0[t] = sw; mesh.tri_v1[t] = ne; mesh.tri_v2[t] = nw; ++t;
            } else {
                mesh.tri_v0[t] = sw; mesh.tri_v1[t] = se; mesh.tri_v2[t] = nw; ++t;
                mesh.tri_v0[t] = se; mesh.tri_v1[t] = ne; mesh.tri_v2[t] = nw; ++t;
            }
        }
    buildMeshTopology(mesh);
    return mesh;
}

// Reconstruct (η, depth) from volume under the active closure — matches the
// solver's reconstructFromVolume for FLAT (the default) exactly.
void reconstruct(const MeshData& m, const SolverOptions2D& o,
                 SurfaceStateData& s, const std::vector<double>& V) {
    for (int i = 0; i < m.n_triangles(); ++i) {
        const double A = m.tri_area[i];
        const double v = (V[i] > 0.0) ? V[i] : 0.0;
        s.depth[i] = (A > 1.0e-30) ? v / A : 0.0;
        if (o.cell_closure == CellClosure2D::VFR) {
            double z1 = m.vz[m.tri_v0[i]], z2 = m.vz[m.tri_v1[i]], z3 = m.vz[m.tri_v2[i]];
            vfrSort3(z1, z2, z3);
            s.head[i] = vfrEtaFromMeanDepth(z1, z2, z3, s.depth[i], o.vfr_min_wet_frac);
        } else {
            s.head[i] = m.tri_cz[i] + s.depth[i];   // FLAT
        }
    }
}

void rhs(const MeshData& m, const SolverOptions2D& o, SurfaceStateData& s,
         const std::vector<double>& V, std::vector<double>& ydot) {
    reconstruct(m, o, s, V);
    computeEdgeFluxes(m, s, o);
    assembleRHS(m, s, o, ydot.data());
}

}  // namespace

// Analytic J·v == central-difference J·v on a random wet state, FLAT closure.
TEST(AnalyticJv, MatchesFiniteDifferenceFlat) {
    auto mesh = makeGridMesh(6, 6, 10.0, 0.01);
    const int nt = mesh.n_triangles();

    SolverOptions2D opts;
    opts.num_threads = 1;
    SurfaceStateData state;
    state.resize(nt, mesh.n_vertices());

    // Random wet volumes (depths ~0.05–0.55 m) so every cell conveys.
    std::mt19937 rng(12345);
    std::uniform_real_distribution<double> ud(0.05, 0.55);
    std::vector<double> V(nt), v(nt);
    for (int i = 0; i < nt; ++i) V[i] = ud(rng) * mesh.tri_area[i];
    for (int i = 0; i < nt; ++i) v[i] = ud(rng) - 0.3;   // direction, signed

    // Analytic J·v.
    reconstruct(mesh, opts, state, V);
    SurfaceTangents tang;
    buildSurfaceTangents(mesh, state, opts, tang);
    std::vector<double> Jv(nt, 0.0);
    applyTangentJv(mesh, opts, tang, /*nc*/ 0, v.data(), Jv.data());

    // Central-difference J·v. eps ~1e-4 (relative ~1e-5 on V~O(10)): small
    // enough that O(eps²) truncation is ~1e-8, large enough that catastrophic
    // cancellation stays well below the tolerance.
    const double eps = 1.0e-4;
    std::vector<double> Vp(nt), Vm(nt), yp(nt), ym(nt);
    for (int i = 0; i < nt; ++i) { Vp[i] = V[i] + eps * v[i]; Vm[i] = V[i] - eps * v[i]; }
    rhs(mesh, opts, state, Vp, yp);
    rhs(mesh, opts, state, Vm, ym);

    double max_abs = 0.0, max_rel = 0.0;
    for (int i = 0; i < nt; ++i) {
        const double fd = (yp[i] - ym[i]) / (2.0 * eps);
        const double ad = Jv[i];
        const double aerr = std::abs(ad - fd);
        max_abs = std::max(max_abs, aerr);
        const double scale = std::max(std::abs(fd), 1.0e-4);
        max_rel = std::max(max_rel, aerr / scale);
    }
    EXPECT_LT(max_rel, 1.0e-6)
        << "analytic vs FD J·v mismatch: max_rel=" << max_rel
        << " max_abs=" << max_abs;
}

// Same parity under the VFR closure + VFR_FACE wetting gate (Phase 3 default).
TEST(AnalyticJv, MatchesFiniteDifferenceVfr) {
    auto mesh = makeGridMesh(6, 6, 10.0, 0.02);
    const int nt = mesh.n_triangles();

    SolverOptions2D opts;
    opts.num_threads = 1;
    opts.cell_closure = CellClosure2D::VFR;
    opts.face_reconstruction = FaceDepth2D::VFR_FACE;
    SurfaceStateData state;
    state.resize(nt, mesh.n_vertices());

    std::mt19937 rng(999);
    std::uniform_real_distribution<double> ud(0.10, 0.60);
    std::vector<double> V(nt), v(nt);
    for (int i = 0; i < nt; ++i) V[i] = ud(rng) * mesh.tri_area[i];
    for (int i = 0; i < nt; ++i) v[i] = ud(rng) - 0.35;

    reconstruct(mesh, opts, state, V);
    SurfaceTangents tang;
    buildSurfaceTangents(mesh, state, opts, tang);
    std::vector<double> Jv(nt, 0.0);
    applyTangentJv(mesh, opts, tang, 0, v.data(), Jv.data());

    const double eps = 1.0e-4;
    std::vector<double> Vp(nt), Vm(nt), yp(nt), ym(nt);
    for (int i = 0; i < nt; ++i) { Vp[i] = V[i] + eps * v[i]; Vm[i] = V[i] - eps * v[i]; }
    rhs(mesh, opts, state, Vp, yp);
    rhs(mesh, opts, state, Vm, ym);

    double max_rel = 0.0;
    for (int i = 0; i < nt; ++i) {
        const double fd = (yp[i] - ym[i]) / (2.0 * eps);
        const double scale = std::max(std::abs(fd), 1.0e-4);
        max_rel = std::max(max_rel, std::abs(Jv[i] - fd) / scale);
    }
    // VFR's dη/dV varies across the wetting front, so allow a touch more FD slack.
    EXPECT_LT(max_rel, 5.0e-5) << "VFR analytic vs FD J·v mismatch: " << max_rel;
}

// Parity with a SPECIFIED_STAGE boundary on every domain-boundary edge — the
// outfall-tailwater case that gates weir/road/Bellinge onto the analytic path.
TEST(AnalyticJv, MatchesFiniteDifferenceSpecifiedStage) {
    auto mesh = makeGridMesh(5, 5, 10.0, 0.01);
    const int nt = mesh.n_triangles();

    SolverOptions2D opts;
    opts.num_threads = 1;
    SurfaceStateData state;
    state.resize(nt, mesh.n_vertices());

    // Attach a SPECIFIED_STAGE BC (prescribed head) to every boundary edge.
    BoundaryData bc;
    const int ne = nt * 3;
    bc.edge_bc_type.assign(ne, static_cast<int8_t>(BoundaryType::WALL));
    bc.edge_bed_slope.assign(ne, 0.0);
    bc.edge_bc_head.assign(ne, 0.0);
    bc.edge_bc_flow.assign(ne, 0.0);
    bc.edge_bc_cum_flux.assign(ne, 0.0);
    for (int i = 0; i < nt; ++i)
        for (int e = 0; e < 3; ++e) {
            const int nbr = (e == 0) ? mesh.tri_nbr0[i]
                          : (e == 1) ? mesh.tri_nbr1[i] : mesh.tri_nbr2[i];
            if (nbr < 0) {
                const int slot = i * 3 + e;
                bc.edge_bc_type[slot] =
                    static_cast<int8_t>(BoundaryType::SPECIFIED_STAGE);
                // A stage a little above the local bed so the edge conveys.
                bc.edge_bc_head[slot] = mesh.tri_cz[i] + 0.20;
            }
        }
    state.boundary = &bc;

    std::mt19937 rng(2024);
    std::uniform_real_distribution<double> ud(0.05, 0.55);
    std::vector<double> V(nt), v(nt);
    for (int i = 0; i < nt; ++i) V[i] = ud(rng) * mesh.tri_area[i];
    for (int i = 0; i < nt; ++i) v[i] = ud(rng) - 0.3;

    reconstruct(mesh, opts, state, V);
    SurfaceTangents tang;
    buildSurfaceTangents(mesh, state, opts, tang);
    std::vector<double> Jv(nt, 0.0);
    applyTangentJv(mesh, opts, tang, 0, v.data(), Jv.data());

    const double eps = 1.0e-4;
    std::vector<double> Vp(nt), Vm(nt), yp(nt), ym(nt);
    for (int i = 0; i < nt; ++i) { Vp[i] = V[i] + eps * v[i]; Vm[i] = V[i] - eps * v[i]; }
    rhs(mesh, opts, state, Vp, yp);
    rhs(mesh, opts, state, Vm, ym);

    double max_rel = 0.0;
    for (int i = 0; i < nt; ++i) {
        const double fd = (yp[i] - ym[i]) / (2.0 * eps);
        const double scale = std::max(std::abs(fd), 1.0e-4);
        max_rel = std::max(max_rel, std::abs(Jv[i] - fd) / scale);
    }
    EXPECT_LT(max_rel, 1.0e-5)
        << "SPECIFIED_STAGE boundary analytic vs FD J·v mismatch: " << max_rel;
}

// Single-cell live orifice coupling (Phase 3d): parity of the analytic coupling
// tangent — the driving-cell diagonal fold (−dQ/dV_c) AND the augmented ∫Q dt
// accumulator row (dQ/dV_c·v[c]) — against a central difference of the full live
// RHS (interior flux + −Q scatter + accumulator). This is the gate that lets the
// live path run analytic J·v instead of the finite-difference fallback.
TEST(AnalyticJv, MatchesFiniteDifferenceSingleCellCoupling) {
    auto mesh = makeGridMesh(6, 6, 10.0, 0.01);
    const int nt = mesh.n_triangles();

    SolverOptions2D opts;
    opts.num_threads = 1;
    SurfaceStateData state;
    state.resize(nt, mesh.n_vertices());

    // One junction node coupled to an interior cell as a single-cell (centroid)
    // point. Geometry chosen so the exchange sits in smooth regions of every gate
    // (orifice √-law, cap gate fully open, wet ramp saturated) — the tangent must
    // still match FD there, and the smoothness keeps the outer FD clean.
    const int ccell   = nt / 2;
    const double bed   = mesh.tri_cz[ccell];
    const double L1D   = opts.len_1d_to_2d;         // ft → m (0.3048)

    openswmm::NodeData nodes;
    nodes.type.assign(1, openswmm::NodeType::JUNCTION);
    nodes.invert_elev.assign(1, (bed - 2.0) / L1D);         // deep invert
    nodes.full_depth.assign(1, 1.0 / L1D);                  // crown = bed − 1.0 m
    nodes.head.assign(1, (bed + 0.15) / L1D);               // h_1d ≈ bed + 0.15 m
    nodes.depth.assign(1, nodes.head[0] - nodes.invert_elev[0]);

    std::vector<CouplingPoint> cps(1);
    cps[0].cell_idx   = ccell;
    cps[0].vertex_idx = -1;          // single-cell / centroid coupling
    cps[0].node_idx   = 0;
    cps[0].cd         = 0.6;
    cps[0].area       = 1.0;
    cps[0].is_outfall = false;
    cps[0].has_flap_gate = false;
    state.node_coupling = &cps;
    state.nodes_1d      = &nodes;

    const int nc   = 1;
    const int ntot = nt + nc;

    std::mt19937 rng(77);
    std::uniform_real_distribution<double> ud(0.05, 0.55);
    std::vector<double> V(ntot, 0.0), v(ntot);
    for (int i = 0; i < nt; ++i) V[i] = ud(rng) * mesh.tri_area[i];
    V[ccell] = 0.40 * mesh.tri_area[ccell];    // firmly draining (Δh ≈ +0.25 m)
    for (int i = 0; i < ntot; ++i) v[i] = ud(rng) - 0.3;

    // Full live RHS (interior + single-cell coupling), used for the outer FD.
    auto liveRhs = [&](const std::vector<double>& Vf, std::vector<double>& yd) {
        reconstruct(mesh, opts, state, Vf);
        computeEdgeFluxes(mesh, state, opts);
        assembleRHS(mesh, state, opts, yd.data());
        for (int k = 0; k < nc; ++k) {
            const double Q = computeNodeCouplingQ(cps[k], mesh, state, nodes, opts);
            yd[cps[k].cell_idx] += -Q;    // cell loses the drain
            yd[nt + k]           = Q;     // dC_k/dt = Q
        }
    };

    // Analytic J·v (interior tangent + coupling FD linearization, one build).
    reconstruct(mesh, opts, state, V);
    SurfaceTangents tang;
    buildSurfaceTangents(mesh, state, opts, tang);
    std::vector<double> Jv(ntot, 0.0);
    applyTangentJv(mesh, opts, tang, nc, v.data(), Jv.data());

    // Central-difference J·v of the full live RHS.
    const double eps = 1.0e-4;
    std::vector<double> Vp(ntot), Vm(ntot), yp(ntot), ym(ntot);
    for (int i = 0; i < ntot; ++i) { Vp[i] = V[i] + eps * v[i]; Vm[i] = V[i] - eps * v[i]; }
    liveRhs(Vp, yp);
    liveRhs(Vm, ym);

    double max_rel = 0.0, max_abs = 0.0;
    bool saw_coupling_row = false;
    for (int i = 0; i < ntot; ++i) {
        const double fd = (yp[i] - ym[i]) / (2.0 * eps);
        const double aerr = std::abs(Jv[i] - fd);
        max_abs = std::max(max_abs, aerr);
        const double scale = std::max(std::abs(fd), 1.0e-4);
        max_rel = std::max(max_rel, aerr / scale);
        if (i >= nt && std::abs(fd) > 1.0e-6) saw_coupling_row = true;
    }
    EXPECT_TRUE(saw_coupling_row)
        << "accumulator row dQ/dV·v[c] was ~0 — test geometry did not exercise it";
    EXPECT_LT(max_rel, 1.0e-5)
        << "single-cell coupling analytic vs FD J·v mismatch: max_rel=" << max_rel
        << " max_abs=" << max_abs;
}
