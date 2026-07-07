/**
 * @file test_kokkos_rhs_parity.cpp
 * @brief Phase 1 parity check: Kokkos RHS vs. serial CPU RHS.
 *
 * @details Builds a small hand-made mesh + state and evaluates ydot = f(y)
 *          two ways — through the serial finite-volume pipeline in
 *          SurfaceFluxCalculator.cpp and through the Kokkos kernels in
 *          KokkosSurfaceKernels.hpp — then asserts they agree to a tight
 *          tolerance. This isolates the kernel math from the SUNDIALS-Kokkos
 *          N_Vector integration, so it can be run with ONLY Kokkos present
 *          (no SUNDIALS build required).
 *
 *          The mesh is synthetic and need not be physically meaningful: both
 *          paths consume identical inputs, so any disagreement is a kernel
 *          translation bug. Inputs are chosen to exercise the interesting
 *          branches: interior vs. boundary edges, hydrostatic upwinding, a
 *          near-dry cell (depth below dry_depth, flux → 0 smoothly), a sub-ε
 *          head difference (the √|Δη| regSqrt regularization), per-edge
 *          conveyance, and the rainfall/coupling/evaporation source terms.
 *          Both paths use the VOLUME formulation: y is the cell volume V and
 *          η = tri_cz + V/A, h̄ = V/A are reconstructed before the flux pipeline.
 *
 *          Exit code 0 on pass, 1 on failure.
 *
 * @ingroup engine_2d_gpu
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "../../src/engine/2d/data/MeshData.hpp"
#include "../../src/engine/2d/data/SurfaceStateData.hpp"
#include "../../src/engine/2d/data/SolverOptions2D.hpp"
#include "../../src/engine/2d/solver/SurfaceFluxCalculator.hpp"
#include "../../src/engine/2d/gpu/KokkosSurfaceKernels.hpp"

#include <Kokkos_Core.hpp>

#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

using namespace openswmm::twoD;

namespace {

// Copy a host std::vector<double> into a fresh device DView.
gpu::DView toD(const std::vector<double>& h, const char* name) {
    gpu::DView d(std::string(name), h.size());
    if (!h.empty()) {
        Kokkos::View<const double*, Kokkos::HostSpace,
                     Kokkos::MemoryTraits<Kokkos::Unmanaged>> hv(h.data(), h.size());
        Kokkos::deep_copy(d, hv);
    }
    return d;
}
gpu::CIView toI(const std::vector<int>& h, const char* name) {
    gpu::IView d(std::string(name), h.size());
    if (!h.empty()) {
        Kokkos::View<const int*, Kokkos::HostSpace,
                     Kokkos::MemoryTraits<Kokkos::Unmanaged>> hv(h.data(), h.size());
        Kokkos::deep_copy(d, hv);
    }
    return d;
}

// Build a 4-triangle chain (0-1-2-3) with mixed interior/boundary edges.
void buildMesh(MeshData& m, SurfaceStateData& s) {
    const int nt = 4, nv = 6;
    m.resize_vertices(nv);
    m.resize_triangles(nt);
    s.resize(nt, nv);

    // Centroids along x; arbitrary but distinct.
    const double cx[4] = {0.0, 1.0, 2.0, 3.0};
    for (int i = 0; i < nt; ++i) {
        m.tri_cx[i]   = cx[i];
        m.tri_cy[i]   = 0.0;
        m.tri_cz[i]   = 10.0;          // ground elevation
        m.tri_area[i] = 2.0 + 0.1 * i; // positive, distinct
        m.mannings_n[i] = 0.03 + 0.005 * i;
    }
    // Neighbour chain: each interior edge links to the next cell; rest walls.
    const int nbr[4][3] = {{ 1, -1, -1},
                           { 0,  2, -1},
                           { 1,  3, -1},
                           { 2, -1, -1}};
    for (int i = 0; i < nt; ++i) {
        m.tri_nbr0[i] = nbr[i][0];
        m.tri_nbr1[i] = nbr[i][1];
        m.tri_nbr2[i] = nbr[i][2];
        for (int e = 0; e < 3; ++e) {
            const int idx = i * 3 + e;
            m.edge_length[idx]     = 1.0 + 0.25 * e;
            m.edge_nx[idx]         = (e == 0) ? 1.0 : (e == 1 ? 0.0 : -0.7071);
            m.edge_ny[idx]         = (e == 0) ? 0.0 : (e == 1 ? 1.0 :  0.7071);
            m.edge_conveyance[idx] = (i == 1 && e == 1) ? 0.5 : 1.0; // one restricted edge
        }
    }
    // Non-empty vertex stencils so the vertex-reconstruction stage (and thus
    // the node_head / vert_head diagnostic) is actually exercised. Arbitrary
    // but valid: triangle indices in [0, nt), weights per vertex sum to 1.
    const std::vector<std::vector<std::pair<int, double>>> sten = {
        {{0, 1.0}},
        {{0, 0.5}, {1, 0.5}},
        {{1, 0.6}, {2, 0.4}},
        {{2, 0.5}, {3, 0.5}},
        {{3, 1.0}},
        {{0, 0.25}, {3, 0.75}},
    };
    m.vert_stencil_ptr.assign(nv + 1, 0);
    m.vert_stencil_idx.clear();
    m.vert_stencil_wt.clear();
    for (int b = 0; b < nv; ++b) {
        for (const auto& pr : sten[b]) {
            m.vert_stencil_idx.push_back(pr.first);
            m.vert_stencil_wt.push_back(pr.second);
        }
        m.vert_stencil_ptr[b + 1] =
            m.vert_stencil_ptr[b] + static_cast<int>(sten[b].size());
    }

    // Heads chosen so depths span dry (below dry_depth) to wet, and so the
    // (0,1) pair has a sub-ε head difference (Δη = 3e-4 < flux_dh_eps = 1e-3)
    // that exercises the regSqrt regularized branch. depth_i = head_i - tri_cz_i.
    const double head[4] = {10.50, 10.5003, 10.0005, 10.30};
    for (int i = 0; i < nt; ++i) {
        s.head[i]  = head[i];
        s.depth[i] = std::max(head[i] - m.tri_cz[i], 0.0);
        s.rainfall[i]      = 1.0e-6 * (i + 1);
        s.coupling_flux[i] = 2.0e-6 * (i + 1);
        s.evap_rate[i]     = 5.0e-7 * (i + 1);
    }
}

} // namespace

int main() {
    MeshData mesh;
    SurfaceStateData state;
    SolverOptions2D opts;          // defaults: dry_depth=1e-3, limiter_epsilon=1e-6
    buildMesh(mesh, state);
    const int nt = mesh.n_triangles();

    const int nv = mesh.n_vertices();

    // y = cell volume V (volume formulation): V = A·max(η − tri_cz, 0).
    std::vector<double> y(nt);
    for (int i = 0; i < nt; ++i) {
        const double d = std::max(state.head[i] - mesh.tri_cz[i], 0.0);
        y[i] = mesh.tri_area[i] * d;
    }

    // -------- Reference: serial CPU pipeline --------
    std::vector<double> ydot_ref(nt, 0.0);
    std::vector<double> gx_ref, gy_ref, gxl_ref, gyl_ref;   // gradients
    std::vector<double> vhead_ref(nv, 0.0);                 // node_head
    {
        SurfaceStateData st = state;   // copy so the two paths don't share buffers
        // Reconstruct (η, h̄) from the volume state y = V (mirrors
        // reconstructFromVolume; the same reconstruction the Kokkos unpack does).
        for (int i = 0; i < nt; ++i) {
            const double A = mesh.tri_area[i];
            const double v = std::max(y[i], 0.0);
            const double d = (A > 1.0e-30) ? v / A : 0.0;
            st.depth[i] = d;
            st.head[i]  = mesh.tri_cz[i] + d;
        }
        // Vertex reconstruction reference (mirrors reconstructVertexHeads;
        // computed inline so the test stays linked against only the CPU flux
        // unit — no extra translation unit).
        for (int b = 0; b < nv; ++b) {
            double h = 0.0;
            for (int k = mesh.vert_stencil_ptr[b]; k < mesh.vert_stencil_ptr[b + 1]; ++k)
                h += mesh.vert_stencil_wt[k] * st.head[mesh.vert_stencil_idx[k]];
            vhead_ref[b] = h;
        }
        computeUnlimitedGradients(mesh, st);
        computeLimitedGradients(mesh, st, opts.limiter_epsilon);
        computeEdgeFluxes(mesh, st, opts);
        assembleRHS(mesh, st, opts, ydot_ref.data());
        gx_ref = st.grad_hx;   gy_ref = st.grad_hy;
        gxl_ref = st.grad_hx_lim; gyl_ref = st.grad_hy_lim;
    }

    // -------- Kokkos pipeline --------
    std::vector<double> ydot_gpu(nt, 0.0);
    std::vector<double> gx_gpu(nt), gy_gpu(nt), gxl_gpu(nt), gyl_gpu(nt);
    std::vector<double> vhead_gpu(nv, 0.0);
    int failures = 0;
    {
        Kokkos::ScopeGuard guard;

        gpu::MeshViews mv;
        mv.n_tri  = nt;
        mv.n_vert = mesh.n_vertices();
        mv.tri_cz          = toD(mesh.tri_cz, "tri_cz");
        mv.tri_area        = toD(mesh.tri_area, "tri_area");
        mv.tri_cx          = toD(mesh.tri_cx, "tri_cx");
        mv.tri_cy          = toD(mesh.tri_cy, "tri_cy");
        mv.mannings_n      = toD(mesh.mannings_n, "mannings_n");
        mv.tri_nbr0        = toI(mesh.tri_nbr0, "nbr0");
        mv.tri_nbr1        = toI(mesh.tri_nbr1, "nbr1");
        mv.tri_nbr2        = toI(mesh.tri_nbr2, "nbr2");
        mv.edge_length     = toD(mesh.edge_length, "edge_length");
        mv.edge_nx         = toD(mesh.edge_nx, "edge_nx");
        mv.edge_ny         = toD(mesh.edge_ny, "edge_ny");
        mv.edge_conveyance = toD(mesh.edge_conveyance, "edge_conv");
        mv.vert_ptr        = toI(mesh.vert_stencil_ptr, "vptr");
        mv.vert_idx        = toI(mesh.vert_stencil_idx, "vidx");
        mv.vert_wt         = toD(mesh.vert_stencil_wt, "vwt");

        gpu::StateViews sv;
        sv.head          = gpu::DView("head", nt);
        sv.depth         = gpu::DView("depth", nt);
        sv.grad_hx       = gpu::DView("gx", nt);
        sv.grad_hy       = gpu::DView("gy", nt);
        sv.grad_hx_lim   = gpu::DView("gxl", nt);
        sv.grad_hy_lim   = gpu::DView("gyl", nt);
        sv.vert_head     = gpu::DView("vhead", mesh.n_vertices());
        sv.edge_flux     = gpu::DView("eflux", static_cast<std::size_t>(nt) * 3);
        sv.rainfall      = toD(state.rainfall, "rain");
        sv.coupling_flux = toD(state.coupling_flux, "coup");
        sv.evap_rate     = toD(state.evap_rate, "evap");
        sv.precond_diag  = gpu::DView("pdiag", nt);

        gpu::DView yv    = toD(y, "y");
        gpu::DView ydotv("ydot", nt);

        gpu::evaluateRhs(mv, sv, yv, ydotv, opts.dry_depth, opts.limiter_epsilon,
                         opts.flux_dh_eps);
        Kokkos::fence();

        auto pull = [](gpu::DView d, std::vector<double>& out) {
            auto h = Kokkos::create_mirror_view(d);
            Kokkos::deep_copy(h, d);
            for (std::size_t i = 0; i < out.size(); ++i) out[i] = h(i);
        };
        pull(ydotv, ydot_gpu);
        pull(sv.grad_hx,     gx_gpu);
        pull(sv.grad_hy,     gy_gpu);
        pull(sv.grad_hx_lim, gxl_gpu);
        pull(sv.grad_hy_lim, gyl_gpu);
        pull(sv.vert_head,   vhead_gpu);
    }

    // -------- Compare every field the serial path and the kernels share --------
    const double atol = 1.0e-12, rtol = 1.0e-10;
    auto compare = [&](const char* name, const std::vector<double>& ref,
                       const std::vector<double>& gpu) {
        const int before = failures;
        for (std::size_t i = 0; i < ref.size(); ++i) {
            const double diff = std::abs(gpu[i] - ref[i]);
            const double tol  = atol + rtol * std::abs(ref[i]);
            if (diff > tol) {
                std::printf("  %-10s[%zu]: ref=% .17e gpu=% .17e |diff|=% .3e FAIL\n",
                            name, i, ref[i], gpu[i], diff);
                ++failures;
            }
        }
        std::printf("  %-10s: %zu values, %s\n", name, ref.size(),
                    failures == before ? "ok" : "FAIL");
    };

    compare("ydot",     ydot_ref,  ydot_gpu);
    compare("grad_hx",  gx_ref,    gx_gpu);
    compare("grad_hy",  gy_ref,    gy_gpu);
    compare("grad_hx_l", gxl_ref,  gxl_gpu);
    compare("grad_hy_l", gyl_ref,  gyl_gpu);
    compare("vert_head", vhead_ref, vhead_gpu);

    std::printf(failures == 0 ? "\nPARITY PASS\n" : "\nPARITY FAIL (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}
