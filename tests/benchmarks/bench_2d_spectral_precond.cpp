/**
 * @file bench_2d_spectral_precond.cpp
 * @brief Benchmark: CVODE with no preconditioner vs spectral Laplacian preconditioner.
 *
 * @details For each grid size (50×50 and 100×100), runs two configurations:
 *   - baseline : SPGMR with SUN_PREC_NONE
 *   - spectral : SPGMR with SpectralPrecond2D (k=10 modes)
 *
 * Reports per run:
 *   - Wall-clock time (ms)
 *   - CVODE internal step count
 *   - Cumulative linear solver iterations (CVodeGetNumLinIters)
 *   - Spectral preconditioner eigenmodes retained
 *
 * @section bench_setup Setup
 * Structured N×N triangular mesh (2N² triangles) on a 100m × 100m flat domain.
 * Initial condition: Gaussian depth bump h = 0.3 * exp(-((x-50)²+(y-50)²)/200).
 * Simulated for 30 seconds with CVODE BDF (rel_tol=1e-4, abs_tol=1e-6).
 *
 * @note Requires -DOPENSWMM_BUILD_2D=ON at CMake configure time.
 * @note Run with:
 *   ./bench_2d_spectral_precond --benchmark_repetitions=3
 */

#ifdef OPENSWMM_HAS_2D

#include <benchmark/benchmark.h>

#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/solver/CvodeSurfaceSolver.hpp"

#include <cmath>
#include <vector>

using namespace openswmm::twoD;

// ============================================================================
// Helpers
// ============================================================================

/// Build an N×N structured triangular mesh on [0, domain_m] × [0, domain_m].
/// Produces (N+1)² vertices and 2N² triangles.
/// slope_x: bed elevation decreases as z = -slope_x * x (mild downslope in +x).
/// With slope > 0, the diffusive conductance K ∝ h^{5/3}/√|∇h| stays bounded
/// away from ∞ because |∇h| ≈ slope at steady state, preventing CVODE stiffness.
static MeshData makeStructuredMesh(int N, double domain_m = 100.0,
                                    double slope_x = 0.002) {
    MeshData mesh;
    int nv = (N + 1) * (N + 1);
    int nt = 2 * N * N;
    mesh.resize_vertices(nv);
    mesh.resize_triangles(nt);

    double dx = domain_m / N;
    double dy = domain_m / N;

    // Vertex layout: v(i,j) = i*(N+1) + j  for i=0..N (y), j=0..N (x)
    for (int i = 0; i <= N; ++i) {
        for (int j = 0; j <= N; ++j) {
            int vi = i * (N + 1) + j;
            double x = j * dx;
            mesh.vx[static_cast<std::size_t>(vi)] = x;
            mesh.vy[static_cast<std::size_t>(vi)] = i * dy;
            mesh.vz[static_cast<std::size_t>(vi)] = -slope_x * x;  // downslope in +x
        }
    }

    // Triangle layout: each cell (i,j) has two triangles.
    // T0 (lower-right): v(i,j), v(i,j+1), v(i+1,j+1)
    // T1 (upper-left):  v(i,j), v(i+1,j+1), v(i+1,j)
    int t = 0;
    for (int i = 0; i < N; ++i) {
        for (int j = 0; j < N; ++j) {
            int v00 = i * (N + 1) + j;
            int v01 = i * (N + 1) + j + 1;
            int v10 = (i + 1) * (N + 1) + j;
            int v11 = (i + 1) * (N + 1) + j + 1;

            mesh.tri_v0[static_cast<std::size_t>(t)]     = v00;
            mesh.tri_v1[static_cast<std::size_t>(t)]     = v01;
            mesh.tri_v2[static_cast<std::size_t>(t)]     = v11;
            mesh.mannings_n[static_cast<std::size_t>(t)] = 0.035;
            ++t;

            mesh.tri_v0[static_cast<std::size_t>(t)]     = v00;
            mesh.tri_v1[static_cast<std::size_t>(t)]     = v11;
            mesh.tri_v2[static_cast<std::size_t>(t)]     = v10;
            mesh.mannings_n[static_cast<std::size_t>(t)] = 0.035;
            ++t;
        }
    }

    buildMeshTopology(mesh);
    buildVertexStencils(mesh);
    return mesh;
}

/// Initial condition: uniform depth d0 everywhere plus a small Gaussian perturbation.
/// On a sloped mesh, head h = z + d, so setting depth=d0+bump gives head = z+d0+bump.
/// The uniform component is near the Manning steady state on a slope;
/// the Gaussian bump drives non-trivial Krylov solve during transient relaxation.
static SurfaceStateData makeInitialState(const MeshData& mesh,
                                          double d0    = 0.10,
                                          double bump  = 0.03,
                                          double cx0   = 50.0,
                                          double cy0   = 50.0,
                                          double sigma = 12.0) {
    int nt = mesh.n_triangles();
    int nv = mesh.n_vertices();
    SurfaceStateData state;
    state.resize(nt, nv);

    double inv2sig2 = 1.0 / (2.0 * sigma * sigma);
    for (int i = 0; i < nt; ++i) {
        double cx = mesh.tri_cx[static_cast<std::size_t>(i)];
        double cy = mesh.tri_cy[static_cast<std::size_t>(i)];
        double r2 = (cx - cx0)*(cx - cx0) + (cy - cy0)*(cy - cy0);
        double depth = d0 + bump * std::exp(-r2 * inv2sig2);
        state.depth[static_cast<std::size_t>(i)] = depth;
        state.head [static_cast<std::size_t>(i)] = mesh.tri_cz[static_cast<std::size_t>(i)] + depth;
    }
    return state;
}

// ============================================================================
// Benchmark fixture
// ============================================================================

struct BenchParams {
    int  grid_n;          // N for N×N cells
    bool spectral;        // true = spectral precond, false = none
    int  spectral_modes;  // number of modes (only used when spectral=true)
};

static void RunCvodeBench(benchmark::State& state, BenchParams p) {
    MeshData mesh = makeStructuredMesh(p.grid_n);

    for (auto _ : state) {
        state.PauseTiming();
        SurfaceStateData surf = makeInitialState(mesh);
        SolverOptions2D opts;
        opts.rel_tolerance = 1.0e-3;
        opts.abs_tolerance = 1.0e-5;
        opts.max_timestep  = 5.0;
        opts.min_timestep  = 0.0;    // no minimum — let CVODE choose
        opts.max_cvode_steps = 100000;
        opts.max_krylov_dim  = 30;
        opts.linear_solver   = LinearSolverType::GMRES;
        if (p.spectral) {
            opts.preconditioner  = PreconditionerType::SPECTRAL;
            opts.spectral_num_modes = p.spectral_modes;
        } else {
            opts.preconditioner  = PreconditionerType::NONE;
        }

        CvodeSurfaceSolver solver;
        solver.initialize(mesh, surf, opts);
        solver.suppress_warnings();
        state.ResumeTiming();

        solver.advance(0.0, 10.0);

        state.PauseTiming();
        long li = solver.get_num_lin_iters();
        long ns = solver.last_num_steps();
        state.counters["lin_iters"] = benchmark::Counter(
            static_cast<double>(li), benchmark::Counter::kAvgIterations);
        state.counters["cvode_steps"] = benchmark::Counter(
            static_cast<double>(ns), benchmark::Counter::kAvgIterations);
        state.ResumeTiming();
    }
}

// ============================================================================
// Benchmark registrations
// ============================================================================

BENCHMARK_CAPTURE(RunCvodeBench, NoPrecond_50x50,
                  BenchParams{50, false, 0})
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

BENCHMARK_CAPTURE(RunCvodeBench, Spectral_50x50,
                  BenchParams{50, true, 10})
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

BENCHMARK_CAPTURE(RunCvodeBench, NoPrecond_100x100,
                  BenchParams{100, false, 0})
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

BENCHMARK_CAPTURE(RunCvodeBench, Spectral_100x100,
                  BenchParams{100, true, 10})
    ->Unit(benchmark::kMillisecond)
    ->Repetitions(3)
    ->ReportAggregatesOnly(true);

#endif // OPENSWMM_HAS_2D

// Google Benchmark requires main() — provided by benchmark::benchmark_main
// (linked via benchmark::benchmark_main target in CMakeLists.txt).
// When OPENSWMM_HAS_2D is not defined, this file is empty and
// benchmark_main provides a no-op main that exits 0.
