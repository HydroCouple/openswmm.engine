/**
 * @file test_2d_active_set.cpp
 * @brief Dry-cell active-set (wet-front masking) unit + equivalence tests.
 *
 * @details Covers: [2D_OPTIONS] parsing, the ActiveSetBuilder (seeding, BFS
 *          halo, force-activated coupling stencils, departing-cell cleanup,
 *          breach detection), and the core promise — a masked CVODE advance
 *          reproduces the unmasked solution on a dam-break front while never
 *          touching cells outside the halo (those must stay bit-identical).
 *
 *          NOTE on exactness: frozen cells' trajectories are bit-frozen by
 *          construction (ydot ≡ 0). Active cells agree with the unmasked run
 *          to solver tolerance rather than bit-for-bit: the unmasked
 *          finite-difference Jacobian-vector product sees the (physically
 *          irrelevant) wetting response of far dry cells to Krylov-vector
 *          perturbations, which masking suppresses — a sub-tolerance
 *          difference by construction, bounded by CVODE's error control.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <vector>

#include "2d/data/ActiveSetData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VfrClosure.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/solver/ActiveSetBuilder.hpp"
#include "2d/solver/SurfaceFluxCalculator.hpp"
#include "2d/input/SectionHandlers2D.hpp"

#ifdef OPENSWMM_HAS_2D
#include "2d/solver/CvodeSurfaceSolver.hpp"
#endif

using namespace openswmm::twoD;

namespace {

// A 1 m wide, N-quad strip (2N triangles) on a flat bed: v(i,0)/v(i,1) columns
// at x = i, quads split into (lower, upper) triangles. Water placed in quad 0
// propagates rightward as a dam-break front.
MeshData makeStripMesh(int n_quads) {
    MeshData mesh;
    const int cols = n_quads + 1;
    mesh.resize_vertices(2 * cols);
    for (int c = 0; c < cols; ++c) {
        mesh.vx[2 * c]     = static_cast<double>(c);
        mesh.vy[2 * c]     = 0.0;
        mesh.vz[2 * c]     = 0.0;
        mesh.vx[2 * c + 1] = static_cast<double>(c);
        mesh.vy[2 * c + 1] = 1.0;
        mesh.vz[2 * c + 1] = 0.0;
    }
    mesh.resize_triangles(2 * n_quads);
    for (int q = 0; q < n_quads; ++q) {
        const int bl = 2 * q, tl = 2 * q + 1, br = 2 * q + 2, tr = 2 * q + 3;
        mesh.tri_v0[2 * q] = bl; mesh.tri_v1[2 * q] = br; mesh.tri_v2[2 * q] = tr;
        mesh.tri_v0[2 * q + 1] = bl; mesh.tri_v1[2 * q + 1] = tr;
        mesh.tri_v2[2 * q + 1] = tl;
        mesh.mannings_n[2 * q]     = 0.03;
        mesh.mannings_n[2 * q + 1] = 0.03;
    }
    buildMeshTopology(mesh);
    buildVertexStencils(mesh);
    return mesh;
}

SurfaceStateData makeState(const MeshData& mesh) {
    SurfaceStateData state;
    state.resize(mesh.n_triangles(), mesh.n_vertices());
    for (int i = 0; i < mesh.n_triangles(); ++i) {
        state.head[i]  = mesh.tri_cz[i];
        state.depth[i] = 0.0;
        state.volume[i] = 0.0;
    }
    return state;
}

// The closure's free surface for a given cell-mean depth — mirror of the
// file-local SurfaceRouter2D::headFromMeanDepth. Solver-driven tests must seed
// heads through this so a dry cell's head matches what reconstructFromVolume
// produces (FLAT: tri_cz; VFR: the dry anchor vfrDryEta, NOT tri_cz), otherwise
// a frozen cell — skipped by the active-set unpack — keeps a stale head its
// active neighbours read across the shared edge, breaking masked==unmasked.
double closureHead(const MeshData& mesh, const SolverOptions2D& opts,
                   int i, double d) {
    if (opts.cell_closure == CellClosure2D::VFR) {
        double z1 = mesh.vz[mesh.tri_v0[i]];
        double z2 = mesh.vz[mesh.tri_v1[i]];
        double z3 = mesh.vz[mesh.tri_v2[i]];
        vfrSort3(z1, z2, z3);
        return vfrEtaFromMeanDepth(z1, z2, z3, d, opts.vfr_min_wet_frac);
    }
    return mesh.tri_cz[i] + d;
}

} // namespace

// ============================================================================
// Option parsing
// ============================================================================

TEST(ActiveSetParsing, ParseFormatRoundTrip) {
    SolverOptions2D opts;
    EXPECT_FALSE(opts.active_set);          // default OFF
    EXPECT_EQ(opts.active_set_halo, 2);     // default halo

    auto err = parse2DOptionsLine({"ACTIVE_SET", "YES"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(opts.active_set);
    EXPECT_EQ(format2DOptionValue(opts, "ACTIVE_SET"), "YES");

    err = parse2DOptionsLine({"ACTIVE_SET_HALO", "4"}, opts);
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(opts.active_set_halo, 4);
    EXPECT_EQ(format2DOptionValue(opts, "ACTIVE_SET_HALO"), "4");

    EXPECT_TRUE(is2DOptionKey("ACTIVE_SET"));
    EXPECT_TRUE(is2DOptionKey("ACTIVE_SET_HALO"));
    EXPECT_FALSE(parse2DOptionsLine({"ACTIVE_SET", "MAYBE"}, opts).empty());
    EXPECT_FALSE(parse2DOptionsLine({"ACTIVE_SET_HALO", "0"}, opts).empty());
}

// ============================================================================
// Builder
// ============================================================================

TEST(ActiveSetBuilder, SeedsHaloAndOuterRing) {
    auto mesh  = makeStripMesh(12);           // 24 triangles in a chain
    auto state = makeState(mesh);
    SolverOptions2D opts;

    ActiveSetData as;
    as.halo_rings    = 2;
    as.wet_depth_eps = 1.0e-3 * opts.dry_depth;
    as.resize(mesh.n_triangles(), mesh.n_vertices());

    // Wet the leftmost quad only.
    state.volume[0] = 0.1 * mesh.tri_area[0];
    state.depth[0]  = 0.1;
    state.head[0]   = mesh.tri_cz[0] + 0.1;

    rebuildActiveSet(mesh, state, nullptr, nullptr, opts, as);

    EXPECT_EQ(as.n_seed, 1);
    EXPECT_EQ(as.cell_ring[0], 0);
    // Ring growth along the strip adjacency; far cells stay frozen.
    EXPECT_TRUE(as.cell_active[1]);           // shares an edge with cell 0
    EXPECT_FALSE(as.cell_active[23])
        << "far end of the strip must stay frozen";
    EXPECT_FALSE(as.active_cells.empty());
    EXPECT_FALSE(as.outer_ring_cells.empty());
    for (int i : as.outer_ring_cells)
        EXPECT_EQ(as.cell_ring[i], as.halo_rings);

    // Every vertex of every active cell is marked.
    for (int i : as.active_cells) {
        EXPECT_TRUE(as.vert_active[mesh.tri_v0[i]]);
        EXPECT_TRUE(as.vert_active[mesh.tri_v1[i]]);
        EXPECT_TRUE(as.vert_active[mesh.tri_v2[i]]);
    }
}

TEST(ActiveSetBuilder, SourcesAndCouplingStencilsSeed) {
    auto mesh  = makeStripMesh(12);
    auto state = makeState(mesh);
    SolverOptions2D opts;

    ActiveSetData as;
    as.halo_rings    = 1;
    as.wet_depth_eps = 1.0e-3 * opts.dry_depth;
    as.resize(mesh.n_triangles(), mesh.n_vertices());

    state.rainfall[10]      = 1.0e-6;   // rained-on cell
    state.coupling_flux[20] = 1.0e-4;   // held exchange target

    std::vector<CouplingPoint> cps;
    CouplingPoint cp{};
    cp.cell_idx = 5; cp.vertex_idx = -1; cp.node_idx = 0;
    cp.cd = 0.65; cp.area = 1.0; cp.is_outfall = false; cp.has_flap_gate = false;
    cps.push_back(cp);

    // Held path: a zero-exchange coupling stencil contributes nothing this
    // window (the flux is held), so cell 5 must stay frozen.
    rebuildActiveSet(mesh, state, nullptr, &cps, opts, as, /*live=*/false);
    EXPECT_EQ(as.cell_ring[10], 0);  // rainfall seed
    EXPECT_EQ(as.cell_ring[20], 0);  // coupling-flux seed
    EXPECT_FALSE(as.cell_active[5])
        << "held path must not blanket-activate zero-exchange stencils";
    EXPECT_FALSE(as.cell_active[15]) << "cell between seeds stays frozen";

    // Live path: the exchange is evaluated inside the RHS, so every coupling
    // stencil must be force-activated regardless of current flux.
    rebuildActiveSet(mesh, state, nullptr, &cps, opts, as, /*live=*/true);
    EXPECT_EQ(as.cell_ring[5], 0)
        << "live path must force-activate coupling-point cells";
}

TEST(ActiveSetBuilder, DepartingCellFluxSlotsZeroed) {
    auto mesh  = makeStripMesh(6);
    auto state = makeState(mesh);
    SolverOptions2D opts;

    ActiveSetData as;
    as.halo_rings    = 1;
    as.wet_depth_eps = 1.0e-3 * opts.dry_depth;
    as.resize(mesh.n_triangles(), mesh.n_vertices());

    state.volume[4] = 0.05 * mesh.tri_area[4];
    rebuildActiveSet(mesh, state, nullptr, nullptr, opts, as);
    ASSERT_TRUE(as.cell_active[4]);

    // Leave stale fluxes on cell 4, dry it out, rebuild → slots zeroed.
    state.edge_flux[4 * 3 + 0] = 1.0;
    state.edge_flux[4 * 3 + 1] = -2.0;
    state.volume[4] = 0.0;
    rebuildActiveSet(mesh, state, nullptr, nullptr, opts, as);
    EXPECT_FALSE(as.cell_active[4]);
    EXPECT_EQ(state.edge_flux[4 * 3 + 0], 0.0);
    EXPECT_EQ(state.edge_flux[4 * 3 + 1], 0.0);
}

TEST(ActiveSetBuilder, BreachDetection) {
    auto mesh  = makeStripMesh(8);
    auto state = makeState(mesh);
    SolverOptions2D opts;

    ActiveSetData as;
    as.halo_rings    = 1;
    as.wet_depth_eps = 1.0e-3 * opts.dry_depth;
    as.resize(mesh.n_triangles(), mesh.n_vertices());

    state.volume[0] = 0.1 * mesh.tri_area[0];
    rebuildActiveSet(mesh, state, nullptr, nullptr, opts, as);
    EXPECT_FALSE(activeSetBreached(mesh, state, as));

    // Wet an outer-ring cell → breach.
    ASSERT_FALSE(as.outer_ring_cells.empty());
    const int outer = as.outer_ring_cells.front();
    state.volume[outer] = 0.01 * mesh.tri_area[outer];
    EXPECT_TRUE(activeSetBreached(mesh, state, as));
}

#ifdef OPENSWMM_HAS_2D

// ============================================================================
// Masked vs unmasked equivalence — dam-break front on a strip
// ============================================================================

TEST(ActiveSetEquivalence, DamBreakFrontMatchesUnmasked) {
    constexpr int    kQuads    = 16;    // 32 triangles
    constexpr double kMound    = 0.25;  // m of water in quad 0
    constexpr double kWindow   = 0.1;   // s per advance
    constexpr int    kWindows  = 20;    // 2 s total

    auto mesh = makeStripMesh(kQuads);
    SolverOptions2D opts;
    opts.linear_solver  = LinearSolverType::GMRES;
    opts.preconditioner = PreconditionerType::JACOBI;
    opts.max_timestep   = kWindow;
    // The sharp dam-break front needs sub-millisecond first steps and a
    // coarser-than-default error target to integrate at all (both runs use
    // identical settings, which is what the equivalence assertion needs).
    opts.min_timestep   = 1.0e-6;
    opts.abs_tolerance  = 1.0e-5;
    // This test isolates active-set masking equivalence, so both runs must use
    // the SAME J·v. The masked run auto-falls-back to finite-difference J·v
    // (the analytic tangent does not mask frozen cells); pin the reference to
    // FD too, or the two solvers differ by more than the per-cell tolerance.
    opts.jacobian       = Jacobian2D::FD;

    auto initState = [&](SurfaceStateData& st) {
        st = makeState(mesh);
        // Dry cells: the closure's dry head (vfrDryEta under VFR, tri_cz under
        // FLAT) so frozen cells match the unmasked reconstruction exactly.
        for (int i = 0; i < mesh.n_triangles(); ++i)
            st.head[i] = closureHead(mesh, opts, i, 0.0);
        for (int i : {0, 1}) {  // both triangles of quad 0
            st.depth[i]  = kMound;
            st.head[i]   = closureHead(mesh, opts, i, kMound);
            st.volume[i] = kMound * mesh.tri_area[i];
        }
    };

    // --- Unmasked reference ------------------------------------------------
    SurfaceStateData ref;
    initState(ref);
    CvodeSurfaceSolver ref_solver;
    ref_solver.initialize(mesh, ref, opts);

    // --- Masked run ----------------------------------------------------------
    SurfaceStateData msk;
    initState(msk);
    ActiveSetData as;
    // 4 rings = two quads of headroom; the diffusive tail creeps well past
    // the visible front on this fine strip, so give it room (the router's
    // breach-redo handles this adaptively in production).
    as.halo_rings    = 4;
    // Above the solver's error floor (mirrors SurfaceRouter2D::initialize):
    // implicit solves splash tolerance-level films across the active set.
    as.wet_depth_eps = std::max(1.0e-3 * opts.dry_depth,
                                10.0 * opts.abs_tolerance);
    as.resize(mesh.n_triangles(), mesh.n_vertices());
    msk.active_set = &as;              // wired but not yet enabled
    seedInactiveState(mesh, msk, opts);
    as.enabled = true;

    CvodeSurfaceSolver msk_solver;
    msk_solver.initialize(mesh, msk, opts);

    const int nt = mesh.n_triangles();
    double t_ref = 0.0, t_msk = 0.0;
    long total_active = 0;
    for (int w = 0; w < kWindows; ++w) {
        rebuildActiveSet(mesh, msk, nullptr, nullptr, opts, as);
        total_active += as.n_active();

        t_ref = ref_solver.advance(t_ref, (w + 1) * kWindow);
        t_msk = msk_solver.advance(t_msk, (w + 1) * kWindow);
        ASSERT_GT(t_ref, w * kWindow);
        ASSERT_GT(t_msk, w * kWindow);
        ASSERT_FALSE(activeSetBreached(mesh, msk, as))
            << "front outran the halo at window " << w
            << " — widen the halo or shorten the window in this test";

        // Frozen-by-construction: every never-activated cell is bit-identical
        // to its initial (zero) state.
        for (int i = 0; i < nt; ++i) {
            if (!as.cell_active[i]) {
                EXPECT_EQ(msk.volume[i], 0.0)
                    << "frozen cell " << i << " moved at window " << w;
            }
        }
    }

    // The mask must actually be masking (front stays well short of the strip
    // end for this duration).
    EXPECT_LT(total_active, static_cast<long>(nt) * kWindows)
        << "active set never excluded any cell — test is vacuous";

    // Mass conservation, both runs (walls all around). BDF truncation error
    // is per-COMPONENT (abs_tolerance × A each), so the total drifts by up to
    // nt × atol — the masked and unmasked runs must both sit inside that
    // envelope, and near each other.
    const double v0 = 2.0 * kMound * mesh.tri_area[0];
    const double mass_tol = nt * opts.abs_tolerance * mesh.tri_area[0] * 2.0;
    double vr = 0.0, vm = 0.0;
    for (int i = 0; i < nt; ++i) { vr += ref.volume[i]; vm += msk.volume[i]; }
    EXPECT_NEAR(vr, v0, mass_tol);
    EXPECT_NEAR(vm, v0, mass_tol);
    EXPECT_NEAR(vm, vr, mass_tol);

    // Per-cell agreement to solver tolerance (see file header note); the
    // front position must match exactly.
    int front_ref = -1, front_msk = -1;
    // Per-cell masked-vs-unmasked agreement. Tied to the solver's own per-cell
    // error floor (abs_tolerance × area): two independent BDF integrations that
    // each control component error to ~atol can legitimately differ by a small
    // multiple of it, and the halo truncates the diffusive tail differently in
    // the two runs. Under the VFR_FACE conveyance the edge depth is evaluated
    // as faceDepthFromEta(η) rather than the stored cell-mean depth — arithmetic
    // that agrees to rounding on a flat cell but shifts the truncation error by
    // FP-level amounts, ~1.5e-6 here (5e-5 relative), still an order of magnitude
    // under atol. A real active-set leak would be O(cell volume), ~1e-2.
    const double cell_tol = 2.0 * opts.abs_tolerance * mesh.tri_area[0];
    for (int i = 0; i < nt; ++i) {
        EXPECT_NEAR(msk.volume[i], ref.volume[i], cell_tol)
            << "cell " << i << " diverged";
        if (ref.depth[i] > opts.dry_depth) front_ref = i;
        if (msk.depth[i] > opts.dry_depth) front_msk = i;
    }
    EXPECT_EQ(front_ref, front_msk) << "wetting-front position diverged";

    ref_solver.finalize();
    msk_solver.finalize();
}

#endif // OPENSWMM_HAS_2D
