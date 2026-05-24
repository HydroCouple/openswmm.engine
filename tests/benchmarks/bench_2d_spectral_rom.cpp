/**
 * @file bench_2d_spectral_rom.cpp
 * @brief Benchmark: SpectralROM ensemble uncertainty quantification vs. full
 *        Monte Carlo (M independent CVODE simulations).
 *
 * @details Compares two approaches to producing 5th/50th/95th-percentile depth
 *          fields under ±20% uncertainty in Manning's n and rainfall:
 *
 *   ROM  — 1 deterministic CVODE solve + SpectralROM sidecar:
 *            initialize → seed → N × advance(dt, K_eff, rain) → computeQuantiles
 *            Cost per timestep: O(M × k)  where k ≪ n
 *
 *   Full MC — M independent CVODE solves, each with a perturbed parameter set.
 *            Cost per timestep: O(M × n)  (M full nonlinear RHS evaluations)
 *
 * Reported counters (per-iteration means):
 *   - Wall-clock time (ms)
 *   - n_tri  : number of mesh cells
 *   - n_modes: number of retained ROM modes
 *   - n_mem  : number of ensemble members M
 *
 * @section bench_setup Setup
 * Structured N×N triangular mesh (2N² triangles) on a 100 m × 100 m domain
 * with a mild x-slope (0.002) to keep K_eff bounded.
 * Initial condition: uniform depth 0.1 m + small Gaussian bump (σ=12 m, bump=3 cm).
 * Rainfall: spatially-uniform 1e-5 m/s (36 mm/hr) throughout.
 *
 * ROM: 30 steps of dt=1s (= 30 s physical time, matching the CVODE runs).
 * Full MC: each member advances CVODE from t=0 to t=30 s adaptively.
 *
 * @note Requires -DOPENSWMM_BUILD_2D=ON at CMake configure time.
 * @note Run with:
 *   ./bench_2d_spectral_rom --benchmark_repetitions=3
 */

#ifdef OPENSWMM_HAS_2D

#include <benchmark/benchmark.h>

#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/solver/CvodeSurfaceSolver.hpp"
#include "2d/solver/SpectralPrecond2D.hpp"
#include "2d/uncertainty/SpectralROM.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

using namespace openswmm::twoD;

// ============================================================================
// Shared helpers (same as bench_2d_spectral_precond.cpp)
// ============================================================================

static MeshData makeStructuredMesh(int N, double domain_m = 100.0,
                                    double slope_x = 0.002) {
    MeshData mesh;
    int nv = (N + 1) * (N + 1);
    int nt = 2 * N * N;
    mesh.resize_vertices(nv);
    mesh.resize_triangles(nt);

    double dx = domain_m / N;
    double dy = domain_m / N;

    for (int i = 0; i <= N; ++i) {
        for (int j = 0; j <= N; ++j) {
            int vi = i * (N + 1) + j;
            double x = j * dx;
            mesh.vx[static_cast<std::size_t>(vi)] = x;
            mesh.vy[static_cast<std::size_t>(vi)] = i * dy;
            mesh.vz[static_cast<std::size_t>(vi)] = -slope_x * x;
        }
    }

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

static SurfaceStateData makeInitialState(const MeshData& mesh,
                                          double d0    = 0.10,
                                          double bump  = 0.03,
                                          double sigma = 12.0) {
    int nt = mesh.n_triangles();
    int nv = mesh.n_vertices();
    SurfaceStateData state;
    state.resize(nt, nv);

    double cx0 = 50.0, cy0 = 50.0;
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

// Estimate scalar K_eff from mean depth and slope.
// K = h^(5/3) / (n * sqrt(S))  — Manning diffusive conductance.
static double estimateKeff(double mean_depth, double mannings_n, double slope) {
    return std::pow(mean_depth, 5.0 / 3.0) / (mannings_n * std::sqrt(slope));
}

// ============================================================================
// ROM benchmark
// ============================================================================
// Times: basis build (outside loop) + seed + 30 × advance(dt=1s) + computeQuantiles.
// The basis build is amortised over multiple benchmark iterations because it is
// a one-off cost in production (done once at sim start).

struct ROMParams {
    int grid_n;
    int n_modes;   // k
    int n_members; // M
};

static void BenchROM(benchmark::State& state, ROMParams p) {
    // --- one-time setup outside the timing loop ---
    MeshData mesh = makeStructuredMesh(p.grid_n);
    SpectralPrecond2D basis;
    basis.build(mesh, p.n_modes);

    int n = mesh.n_triangles();

    // Constant rainfall field
    const double rain_rate = 1.0e-5;  // m/s
    std::vector<double> rainfall(static_cast<std::size_t>(n), rain_rate);

    // K_eff estimate for base state (mean depth 0.1 m, n=0.035, S=0.002)
    const double K_eff  = estimateKeff(0.10, 0.035, 0.002);
    const double dt_rom = 1.0;          // ROM timestep (s)
    const int    n_steps = 30;          // = 30 s total

    for (auto _ : state) {
        state.PauseTiming();
        SurfaceStateData surf = makeInitialState(mesh);
        state.ResumeTiming();

        // --- timed work ---
        SpectralROM rom;
        rom.basis         = &basis;
        rom.n_ensemble    = p.n_members;
        rom.mannings_pert = 0.20;
        rom.rainfall_pert = 0.20;
        rom.initialize();

        rom.seed(surf.depth.data());

        for (int s = 0; s < n_steps; ++s)
            rom.advance(dt_rom, K_eff, rainfall.data());

        rom.computeQuantiles();

        // Prevent dead-code elimination: read one quantile value
        benchmark::DoNotOptimize(rom.q95[0]);

        state.PauseTiming();
        state.counters["n_tri"]   = static_cast<double>(n);
        state.counters["n_modes"] = static_cast<double>(basis.num_kept);
        state.counters["n_mem"]   = static_cast<double>(p.n_members);
        state.ResumeTiming();
    }
}

// ============================================================================
// Full Monte Carlo benchmark
// ============================================================================
// Times: M independent CvodeSurfaceSolver runs from t=0 to t=30 s.
// Each run uses a uniformly-perturbed Manning's n (forward LHS, same as ROM
// design) so results are comparable.  No rainfall perturbation for simplicity.

struct MCParams {
    int grid_n;
    int n_members;  // M
};

static void BenchFullMC(benchmark::State& state, MCParams p) {
    MeshData mesh = makeStructuredMesh(p.grid_n);
    int n = mesh.n_triangles();

    // Pre-compute Manning's n perturbation multipliers (same LHS as ROM)
    int M = p.n_members;
    std::vector<double> mannings_mults(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i) {
        double t = (static_cast<double>(i) + 0.5) / static_cast<double>(M);
        mannings_mults[static_cast<std::size_t>(i)] = 0.80 + t * 0.40;  // [0.8, 1.2]
    }

    // Rainfall as source field
    const double rain_rate = 1.0e-5;

    for (auto _ : state) {
        state.PauseTiming();
        SurfaceStateData base_surf = makeInitialState(mesh);
        state.ResumeTiming();

        // Depth matrix: n_members × n_tri (for quantile extraction)
        std::vector<std::vector<double>> member_depths(
            static_cast<std::size_t>(M),
            std::vector<double>(static_cast<std::size_t>(n)));

        for (int i = 0; i < M; ++i) {
            // Build per-member mesh with scaled Manning's n
            MeshData m_mesh = mesh;
            double mm = mannings_mults[static_cast<std::size_t>(i)];
            for (int t = 0; t < n; ++t)
                m_mesh.mannings_n[static_cast<std::size_t>(t)] *= mm;

            // Per-member rainfall source (applied via SurfaceStateData.source)
            SurfaceStateData surf = base_surf;
            for (int t = 0; t < n; ++t)
                surf.rainfall[static_cast<std::size_t>(t)] = rain_rate;

            SolverOptions2D opts;
            opts.rel_tolerance   = 1.0e-3;
            opts.abs_tolerance   = 1.0e-5;
            opts.max_timestep    = 5.0;
            opts.max_cvode_steps = 100000;
            opts.linear_solver   = LinearSolverType::GMRES;
            opts.preconditioner  = PreconditionerType::NONE;

            CvodeSurfaceSolver solver;
            solver.initialize(m_mesh, surf, opts);
            solver.suppress_warnings();
            solver.advance(0.0, 30.0);

            // surf.depth is updated in-place by the solver
            for (int t = 0; t < n; ++t)
                member_depths[static_cast<std::size_t>(i)][static_cast<std::size_t>(t)]
                    = surf.depth[static_cast<std::size_t>(t)];
        }

        // Compute q95 at cell 0 to prevent dead-code elimination
        std::vector<double> cell0_depths(static_cast<std::size_t>(M));
        for (int i = 0; i < M; ++i)
            cell0_depths[static_cast<std::size_t>(i)]
                = member_depths[static_cast<std::size_t>(i)][0];
        std::sort(cell0_depths.begin(), cell0_depths.end());
        int idx95 = std::min(M - 1, static_cast<int>(0.95 * (M - 1.0) + 0.5));
        benchmark::DoNotOptimize(cell0_depths[static_cast<std::size_t>(idx95)]);

        state.PauseTiming();
        state.counters["n_tri"] = static_cast<double>(n);
        state.counters["n_mem"] = static_cast<double>(M);
        state.ResumeTiming();
    }
}

// ============================================================================
// Registrations
// ============================================================================

// ROM on 50×50 mesh
BENCHMARK_CAPTURE(BenchROM, ROM_50x50_k6_M20,  ROMParams{50, 6, 20})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

BENCHMARK_CAPTURE(BenchROM, ROM_50x50_k10_M20, ROMParams{50, 10, 20})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

BENCHMARK_CAPTURE(BenchROM, ROM_50x50_k10_M50, ROMParams{50, 10, 50})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

// Full MC on 50×50 mesh (M=1 gives single-run baseline, M=20/50 for comparison)
BENCHMARK_CAPTURE(BenchFullMC, MC_50x50_M1,  MCParams{50, 1})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

BENCHMARK_CAPTURE(BenchFullMC, MC_50x50_M20, MCParams{50, 20})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

// ROM on 100×100 mesh
BENCHMARK_CAPTURE(BenchROM, ROM_100x100_k10_M20, ROMParams{100, 10, 20})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

// Full MC on 100×100 mesh (single run baseline only; full M=20 is too slow for routine CI)
BENCHMARK_CAPTURE(BenchFullMC, MC_100x100_M1, MCParams{100, 1})
    ->Unit(benchmark::kMillisecond)->Repetitions(3)->ReportAggregatesOnly(true);

#endif // OPENSWMM_HAS_2D
