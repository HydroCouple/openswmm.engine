/**
 * @file demo_2d_spectral_rom.cpp
 * @brief Demo: SpectralROM uncertainty propagation.
 *
 * @details A Gaussian depth bump (h=0.1 m, sigma=15 m) centred off-domain-midpoint
 *          (30 m, 35 m) decays on a 50×50 sloped mesh under diffusion-wave physics.
 *          With ±25% uncertainty in Manning's n, different ensemble members drain the
 *          peak at different rates, producing a widening 5th/50th/95th-percentile
 *          uncertainty band over 300 s.
 *
 *          Key physics:
 *            - K_eff  = h_mean^(5/3) / (n * sqrt(S))  — diffusive conductance
 *            - rate_j = lambda_j * K_eff / mannings_mult[i]  — mode j decay rate
 *            - a_j(t+dt) = a_j(t) * exp(-rate_j * dt)  — exact exponential decay
 *
 *          The Gaussian is deliberately placed off the domain midpoint (30 m, 35 m)
 *          to break the symmetry with the Laplacian eigenmodes; a centred bump
 *          would project to near-zero on all anti-symmetric low-frequency modes.
 *          k=30 modes capture ~35% of the bump peak; the remaining energy lies in
 *          higher-frequency modes outside the retained basis.
 *
 *          Output table (per 30-second interval):
 *            t(s)  q50_pk  q05_pk  q95_pk  sprd_mn  sprd_mx  sprd%  pk_ret%
 *
 * @note Run with:  ./demo_2d_spectral_rom
 */

#ifdef OPENSWMM_HAS_2D

#include "2d/data/MeshData.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/solver/SpectralPrecond2D.hpp"
#include "2d/uncertainty/SpectralROM.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

using namespace openswmm::twoD;

// ============================================================================
// Mesh helper
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
            int v00 = i * (N + 1) + j,    v01 = i * (N + 1) + j + 1;
            int v10 = (i + 1) * (N + 1) + j, v11 = (i + 1) * (N + 1) + j + 1;
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
    return mesh;
}

// ============================================================================
// main
// ============================================================================

int main() {
    // ---- Configuration ----
    const int    N        = 50;
    const double domain_m = 100.0;
    const double slope_x  = 0.002;
    const double mannings = 0.035;
    const double bump_amp = 0.10;    // Gaussian bump amplitude (m)
    const double bump_sig = 15.0;    // Gaussian sigma (m)
    const double cx0 = 30.0, cy0 = 35.0;  // off-centre — avoids zero projection from domain symmetry
    const double dt       = 30.0;    // timestep (s)
    const int    n_steps  = 10;      // total = 300 s
    const int    M        = 50;
    const double m_pert   = 0.25;    // ±25% Manning's n uncertainty
    const double r_pert   = 0.00;    // no rainfall uncertainty — isolates Manning's effect
    const int    k_modes  = 30;

    // ---- Build mesh ----
    std::printf("Building %d×%d mesh (%d triangles, slope=%.3f)...\n",
                N, N, 2 * N * N, slope_x);
    MeshData mesh = makeStructuredMesh(N, domain_m, slope_x);
    int nt = mesh.n_triangles();

    // ---- Gaussian bump initial condition ----
    std::vector<double> h0(static_cast<std::size_t>(nt), 0.0);
    double h_mean = 0.0;
    for (int i = 0; i < nt; ++i) {
        double cx = mesh.tri_cx[static_cast<std::size_t>(i)];
        double cy = mesh.tri_cy[static_cast<std::size_t>(i)];
        double r2 = (cx - cx0) * (cx - cx0) + (cy - cy0) * (cy - cy0);
        h0[static_cast<std::size_t>(i)] = bump_amp * std::exp(-r2 / (2.0 * bump_sig * bump_sig));
        h_mean += h0[static_cast<std::size_t>(i)];
    }
    h_mean /= nt;

    // K_eff from domain-mean depth of the bump
    const double K_eff = std::pow(h_mean, 5.0 / 3.0) / (mannings * std::sqrt(slope_x));
    std::printf("  bump peak=%.3f m, domain-mean depth=%.5f m, K_eff=%.3f m^(4/3)/s\n\n",
                bump_amp, h_mean, K_eff);

    // ---- Build eigenbasis ----
    std::printf("Building spectral basis (k=%d modes)...\n", k_modes);
    SpectralPrecond2D basis;
    if (!basis.build(mesh, k_modes)) {
        std::fprintf(stderr, "ERROR: basis build failed\n");
        return 1;
    }
    std::printf("  Retained %d modes (λ_min=%.4f, λ_max=%.4f)\n",
                basis.num_kept,
                basis.eigenvalues.front(),
                basis.eigenvalues.back());

    // Fastest and slowest expected decay rates (mode 1, Manning's extremes)
    double lam1  = basis.eigenvalues.front();
    double rate1_nominal = lam1 * K_eff;
    double rate1_fast    = lam1 * K_eff / (1.0 - m_pert);
    double rate1_slow    = lam1 * K_eff / (1.0 + m_pert);
    std::printf("  Mode-1 decay: τ_nominal=%.0f s, τ_fast=%.0f s (n×0.8), τ_slow=%.0f s (n×1.2)\n\n",
                1.0 / rate1_nominal, 1.0 / rate1_fast, 1.0 / rate1_slow);

    // ---- Initialize ROM ----
    SpectralROM rom;
    rom.basis         = &basis;
    rom.n_ensemble    = M;
    rom.mannings_pert = m_pert;
    rom.rainfall_pert = r_pert;
    rom.initialize();
    rom.seed(h0.data());

    // Initial quantiles immediately after seed (all members identical → tight)
    rom.computeQuantiles(h0.data());
    double q50_peak0 = *std::max_element(rom.q50.begin(), rom.q50.end());
    std::printf("Seeded.  Peak q50 at t=0: %.5f m (vs bump amplitude %.3f m)\n",
                q50_peak0, bump_amp);
    std::printf("  (ROM captures %.1f%% of peak via k=%d modes)\n\n",
                100.0 * q50_peak0 / bump_amp, basis.num_kept);

    // ---- Header ----
    std::printf("%-6s  %-8s  %-8s  %-8s  %-8s  %-8s  %-7s  %-8s\n",
                "t(s)", "q50_pk", "q05_pk", "q95_pk", "sprd_mn", "sprd_mx", "sprd%", "pk_ret%");
    std::printf("%-6s  %-8s  %-8s  %-8s  %-8s  %-8s  %-7s  %-8s\n",
                "------", "--------", "--------", "--------",
                "--------", "--------", "-------", "--------");

    // ---- Time-stepping loop ----
    // No rainfall (nullptr) — isolates Manning's n uncertainty
    double t = 0.0;
    for (int step = 0; step < n_steps; ++step) {
        rom.advance(dt, K_eff, nullptr, nullptr, h0.data());
        rom.computeQuantiles(h0.data());
        t += dt;

        // Peak cell (maximum q50)
        int   peak_cell = static_cast<int>(
            std::max_element(rom.q50.begin(), rom.q50.end()) - rom.q50.begin());
        auto  pu = static_cast<std::size_t>(peak_cell);
        double q50_pk = rom.q50[pu];
        double q05_pk = rom.q05[pu];
        double q95_pk = rom.q95[pu];

        // Domain-mean spread
        double spread_sum = 0.0, spread_max = 0.0;
        for (int i = 0; i < nt; ++i) {
            auto ui = static_cast<std::size_t>(i);
            double sp = rom.q95[ui] - rom.q05[ui];
            spread_sum += sp;
            spread_max  = std::max(spread_max, sp);
        }
        double spread_mean = spread_sum / nt;
        double spread_pct  = (q50_pk > 1e-8) ? 100.0 * (q95_pk - q05_pk) / q50_pk : 0.0;
        double peak_ret    = (q50_peak0 > 1e-8) ? 100.0 * q50_pk / q50_peak0 : 0.0;

        std::printf("%-6.0f  %-8.5f  %-8.5f  %-8.5f  %-8.6f  %-8.6f  %-7.2f  %-8.1f\n",
                    t, q50_pk, q05_pk, q95_pk, spread_mean, spread_max,
                    spread_pct, peak_ret);
    }

    // ---- Spatial spread distribution at final time ----
    std::printf("\n--- Spatial spread at t=%.0f s ---\n", t);

    std::vector<double> spreads(static_cast<std::size_t>(nt));
    for (int i = 0; i < nt; ++i)
        spreads[static_cast<std::size_t>(i)] = rom.q95[static_cast<std::size_t>(i)]
                                             - rom.q05[static_cast<std::size_t>(i)];
    std::sort(spreads.begin(), spreads.end());

    std::printf("  p50=%.5f  p75=%.5f  p90=%.5f  max=%.5f  (m)\n",
                spreads[static_cast<std::size_t>(nt / 2)],
                spreads[static_cast<std::size_t>(3 * nt / 4)],
                spreads[static_cast<std::size_t>(9 * nt / 10)],
                spreads.back());

    // Depth-quartile relative spread (Manning's nonlinearity: deeper = larger relative uncertainty)
    std::vector<int> by_depth(static_cast<std::size_t>(nt));
    std::iota(by_depth.begin(), by_depth.end(), 0);
    std::sort(by_depth.begin(), by_depth.end(), [&](int a, int b) {
        return rom.q50[static_cast<std::size_t>(a)] < rom.q50[static_cast<std::size_t>(b)];
    });

    std::printf("\nRelative spread by depth quartile:\n");
    std::printf("  %-18s  %-10s  %-10s  %-8s\n",
                "quartile", "mean_q50(m)", "mean_sprd(m)", "sprd/q50");
    const char* qnames[] = {"Q1 (shallow)", "Q2", "Q3", "Q4 (deep)"};
    for (int q = 0; q < 4; ++q) {
        int start = q * nt / 4;
        int end   = (q + 1) * nt / 4;
        double sum_q50 = 0.0, sum_sp = 0.0;
        int    n_pos = 0;
        for (int k = start; k < end; ++k) {
            int i = by_depth[static_cast<std::size_t>(k)];
            auto ui = static_cast<std::size_t>(i);
            double q50 = rom.q50[ui];
            if (q50 > 1e-4) {  // 0.1 mm — below this is the ROM reconstruction noise floor
                sum_q50 += q50;
                sum_sp  += rom.q95[ui] - rom.q05[ui];
                ++n_pos;
            }
        }
        if (n_pos > 0) {
            double mean_q50 = sum_q50 / n_pos;
            double mean_sp  = sum_sp  / n_pos;
            std::printf("  %-18s  %-10.6f  %-10.6f  %-8.4f\n",
                        qnames[q], mean_q50, mean_sp, mean_sp / mean_q50);
        } else {
            std::printf("  %-18s  (all dry)\n", qnames[q]);
        }
    }

    std::printf("\nDone.\n");
    return 0;
}

#else

#include <cstdio>
int main() {
    std::printf("Demo requires -DOPENSWMM_BUILD_2D=ON\n");
    return 0;
}

#endif // OPENSWMM_HAS_2D
