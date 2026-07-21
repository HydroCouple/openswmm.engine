/**
 * @file demo_1d_spectral_rom.cpp
 * @brief Demo: 1D SpectralROM — Gaussian flood peak with Manning's n uncertainty.
 *
 * @details A Gaussian depth bump decays on a 1D sloped channel under
 *          diffusion-wave physics.  With ±25% uncertainty in Manning's n, M=50
 *          ensemble members drain the peak at different rates, producing a
 *          widening 5th/50th/95th-percentile uncertainty band over 300 s.
 *
 *          Eigenbasis: DCT-II — analytically exact, no mesh topology required.
 *            phi_k[j] = sqrt(2/n) * cos(k*pi*(2j+1)/(2n)),  k=1..K
 *            lambda_k = 2*(1 - cos(k*pi/n))               (graph Laplacian)
 *
 *          Ensemble advance: exact exponential integrator per member × mode.
 *            a_k(t+dt) = a_k(t) * exp(-lambda_k * K_eff/mannings_mult * dt)
 *
 *          The bump is placed at x=35m (not at a quarter/third/half of L)
 *          to avoid zero projection from mode-parity cancellation.
 *
 * @note Run with:  ./demo_1d_spectral_rom
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numeric>
#include <vector>

#ifndef M_PI
static constexpr double M_PI = 3.14159265358979323846;
#endif

int main() {
    // ---- Configuration ----
    const int    n       = 100;    // cells along channel
    const double L       = 100.0;  // channel length (m)
    const double slope   = 0.002;  // longitudinal slope
    const double mann    = 0.035;  // base Manning's n
    const double amp     = 0.10;   // Gaussian bump amplitude (m)
    const double sig     = 15.0;   // Gaussian sigma (m)
    const double x0      = 35.0;   // bump centre — not at L/2, L/3, or L/4
    const double dt      = 30.0;   // timestep (s)
    const int    nsteps  = 10;     // total = 300 s
    const int    M       = 50;     // ensemble members
    const double m_pert  = 0.25;   // ±25% Manning's n perturbation
    const int    K       = 20;     // retained DCT modes

    const double dx = L / n;

    // ---- Cell centres ----
    std::vector<double> x(static_cast<std::size_t>(n));
    for (int j = 0; j < n; ++j)
        x[static_cast<std::size_t>(j)] = (j + 0.5) * dx;

    // ---- Gaussian bump initial condition ----
    std::vector<double> h0(static_cast<std::size_t>(n));
    double h_mean = 0.0;
    for (int j = 0; j < n; ++j) {
        double d = x[static_cast<std::size_t>(j)] - x0;
        h0[static_cast<std::size_t>(j)] = amp * std::exp(-d * d / (2.0 * sig * sig));
        h_mean += h0[static_cast<std::size_t>(j)];
    }
    h_mean /= n;

    // K_eff from domain-mean depth (diffusive conductance)
    const double K_eff = std::pow(h_mean, 5.0 / 3.0) / (mann * std::sqrt(slope));

    // ---- DCT-II eigenbasis (Neumann Laplacian, cell-centred) ----
    // phi_k[j] = sqrt(2/n) * cos(k*pi*(2j+1)/(2n))  for k=1..K, j=0..n-1
    // lambda_k = 2*(1 - cos(k*pi/n))
    //
    // Orthonormality: sum_j phi_k[j]^2 = 1  (discrete L2)
    // These are the exact eigenvectors of the tridiagonal path-graph Laplacian
    // with Neumann (free) boundary conditions.
    //
    std::vector<double> lambda(static_cast<std::size_t>(K));
    std::vector<double> P(static_cast<std::size_t>(K * n));  // K×n, row k = mode k
    const double scl = std::sqrt(2.0 / n);
    for (int k = 1; k <= K; ++k) {
        lambda[static_cast<std::size_t>(k - 1)] =
            2.0 * (1.0 - std::cos(k * M_PI / n));
        for (int j = 0; j < n; ++j)
            P[static_cast<std::size_t>((k - 1) * n + j)] =
                scl * std::cos(k * M_PI * (2 * j + 1) / (2.0 * n));
    }

    // ---- Project h0 onto eigenmodes: a0_k = phi_k^T * h0 ----
    std::vector<double> a0(static_cast<std::size_t>(K));
    for (int k = 0; k < K; ++k) {
        double dot = 0.0;
        for (int j = 0; j < n; ++j)
            dot += P[static_cast<std::size_t>(k * n + j)]
                 * h0[static_cast<std::size_t>(j)];
        a0[static_cast<std::size_t>(k)] = dot;
    }

    // ---- Check reconstruction quality at t=0 ----
    std::vector<double> h_rec(static_cast<std::size_t>(n), 0.0);
    for (int k = 0; k < K; ++k)
        for (int j = 0; j < n; ++j)
            h_rec[static_cast<std::size_t>(j)] +=
                P[static_cast<std::size_t>(k * n + j)]
              * a0[static_cast<std::size_t>(k)];
    double peak_rec = *std::max_element(h_rec.begin(), h_rec.end());

    // ---- LHS ensemble for Manning's multipliers ----
    // M strata uniformly covering [1-m_pert, 1+m_pert]; members in forward order.
    std::vector<double> mm(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        mm[static_cast<std::size_t>(i)] =
            (1.0 - m_pert) + (i + 0.5) / M * (2.0 * m_pert);

    // ---- Ensemble coefficients: M×K ----
    std::vector<double> a(static_cast<std::size_t>(M * K));
    for (int i = 0; i < M; ++i)
        for (int k = 0; k < K; ++k)
            a[static_cast<std::size_t>(i * K + k)] = a0[static_cast<std::size_t>(k)];

    // ---- Quantile workspace ----
    std::vector<double> q05(static_cast<std::size_t>(n));
    std::vector<double> q50(static_cast<std::size_t>(n));
    std::vector<double> q95(static_cast<std::size_t>(n));
    std::vector<double> sbuf(static_cast<std::size_t>(M));

    const int i05 = std::max(0, static_cast<int>(0.05 * (M - 1) + 0.5));
    const int i50 = static_cast<int>(0.50 * (M - 1) + 0.5);
    const int i95 = std::min(M - 1, static_cast<int>(0.95 * (M - 1) + 0.5));

    auto computeQuantiles = [&]() {
        for (int j = 0; j < n; ++j) {
            for (int i = 0; i < M; ++i) {
                double h = 0.0;
                for (int k = 0; k < K; ++k)
                    h += P[static_cast<std::size_t>(k * n + j)]
                       * a[static_cast<std::size_t>(i * K + k)];
                sbuf[static_cast<std::size_t>(i)] = std::max(h, 0.0);
            }
            std::sort(sbuf.begin(), sbuf.end());
            q05[static_cast<std::size_t>(j)] = sbuf[static_cast<std::size_t>(i05)];
            q50[static_cast<std::size_t>(j)] = sbuf[static_cast<std::size_t>(i50)];
            q95[static_cast<std::size_t>(j)] = sbuf[static_cast<std::size_t>(i95)];
        }
    };

    auto advanceROM = [&](double ddt) {
        for (int i = 0; i < M; ++i) {
            double Ksc = K_eff / mm[static_cast<std::size_t>(i)];
            for (int k = 0; k < K; ++k) {
                double rate = lambda[static_cast<std::size_t>(k)] * Ksc;
                if (rate > 1e-12)
                    a[static_cast<std::size_t>(i * K + k)] *= std::exp(-rate * ddt);
            }
        }
    };

    // ---- Header ----
    double lam1 = lambda[0];
    std::printf("1D SpectralROM: ±%.0f%% Manning's n uncertainty\n\n",
                m_pert * 100.0);
    std::printf("  Channel: n=%d cells, L=%.0f m, dx=%.1f m, slope=%.3f\n",
                n, L, dx, slope);
    std::printf("  Bump:    amp=%.3f m, sigma=%.0f m, centre x=%.0f m\n",
                amp, sig, x0);
    std::printf("  h_mean=%.4f m  K_eff=%.3f m^(4/3)/s\n",
                h_mean, K_eff);
    std::printf("  DCT-II basis: K=%d modes  ROM captures %.1f%% of bump peak\n",
                K, 100.0 * peak_rec / amp);
    std::printf("  Mode-1: lambda=%.5f  tau_nom=%.0f s"
                "  tau_fast(n*0.75)=%.0f s  tau_slow(n*1.25)=%.0f s\n\n",
                lam1, 1.0 / (lam1 * K_eff),
                1.0 / (lam1 * K_eff / (1.0 - m_pert)),
                1.0 / (lam1 * K_eff / (1.0 + m_pert)));

    std::printf("  Ensemble: M=%d members, Manning's mult in [%.2f, %.2f]\n\n",
                M, mm[0], mm[static_cast<std::size_t>(M - 1)]);

    // ---- Print table header ----
    std::printf("%-6s  %-8s  %-8s  %-8s  %-8s  %-8s  %-7s  %-8s\n",
                "t(s)", "q50_pk", "q05_pk", "q95_pk",
                "sprd_mn", "sprd_mx", "sprd%", "pk_ret%");
    std::printf("%-6s  %-8s  %-8s  %-8s  %-8s  %-8s  %-7s  %-8s\n",
                "------", "--------", "--------", "--------",
                "--------", "--------", "-------", "--------");

    computeQuantiles();

    // Lock the reference cell to the initial bump peak — consistent tracking
    // prevents spurious spread% swings when the spatial maximum shifts over time.
    const int pk_cell = static_cast<int>(
        std::max_element(q50.begin(), q50.end()) - q50.begin());
    const auto upk = static_cast<std::size_t>(pk_cell);
    double q50_peak0 = q50[upk];

    auto printRow = [&](double t) {
        double sm = 0.0, smx = 0.0;
        for (int j = 0; j < n; ++j) {
            double sp = q95[static_cast<std::size_t>(j)]
                      - q05[static_cast<std::size_t>(j)];
            sm += sp;
            smx = std::max(smx, sp);
        }
        double spct = (q50[upk] > 1e-8)
            ? 100.0 * (q95[upk] - q05[upk]) / q50[upk] : 0.0;
        double pret = (q50_peak0 > 1e-8)
            ? 100.0 * q50[upk] / q50_peak0 : 0.0;
        std::printf("%-6.0f  %-8.5f  %-8.5f  %-8.5f  %-8.6f  %-8.6f  %-7.2f  %-8.1f\n",
                    t, q50[upk], q05[upk], q95[upk],
                    sm / n, smx, spct, pret);
    };

    printRow(0.0);

    // ---- Time loop ----
    double t = 0.0;
    for (int step = 0; step < nsteps; ++step) {
        advanceROM(dt);
        computeQuantiles();
        t += dt;
        printRow(t);
    }

    // ---- Spatial profile at final time ----
    std::printf("\n--- Spatial uncertainty profile at t=%.0f s ---\n\n", t);
    std::printf("  %-7s  %-7s  %-7s  %-7s  %-7s  %-7s\n",
                "x(m)", "h0", "q05", "q50", "q95", "band");

    // Find peak for ASCII bar scaling
    double q95max = *std::max_element(q95.begin(), q95.end());
    const int bar_width = 36;

    for (int j = 0; j < n; j += 2) {
        auto uj = static_cast<std::size_t>(j);
        double q5 = q05[uj], q_med = q50[uj], q9 = q95[uj];
        double h_init = h0[uj];

        // ASCII bar: [q05....q50....q95] scaled to bar_width chars
        char bar[bar_width + 3];
        std::fill(bar, bar + sizeof(bar), ' ');
        bar[0] = '|';
        bar[bar_width + 1] = '|';
        bar[bar_width + 2] = '\0';

        if (q95max > 1e-8) {
            int p5  = static_cast<int>(q5    / q95max * bar_width);
            int p50 = static_cast<int>(q_med / q95max * bar_width);
            int p95 = static_cast<int>(q9    / q95max * bar_width);
            p5  = std::max(0, std::min(bar_width - 1, p5));
            p50 = std::max(0, std::min(bar_width - 1, p50));
            p95 = std::max(0, std::min(bar_width - 1, p95));
            for (int b = p5; b <= p95; ++b)
                bar[b + 1] = '-';
            bar[p50 + 1] = '|';
        }

        std::printf("  %-7.1f  %-7.5f  %-7.5f  %-7.5f  %-7.5f  %s\n",
                    x[uj], h_init, q5, q_med, q9, bar);
    }
    std::printf("  (ASCII bar scaled to q95_max=%.5f m; '-' = [q05,q95], '|' = q50)\n",
                q95max);

    // ---- Spread by depth tertile ----
    std::printf("\n--- Relative spread by depth tertile at t=%.0f s ---\n", t);
    std::vector<int> by_depth(static_cast<std::size_t>(n));
    std::iota(by_depth.begin(), by_depth.end(), 0);
    std::sort(by_depth.begin(), by_depth.end(), [&](int a, int b) {
        return q50[static_cast<std::size_t>(a)] < q50[static_cast<std::size_t>(b)];
    });

    std::printf("  %-16s  %-10s  %-10s  %-8s\n",
                "tertile", "mean_q50(m)", "mean_band(m)", "band/q50");
    const char* tnames[] = {"T1 (shallow)", "T2 (mid)", "T3 (deep)"};
    for (int t3 = 0; t3 < 3; ++t3) {
        int start = t3 * n / 3;
        int end   = (t3 + 1) * n / 3;
        double sum_q50 = 0.0, sum_sp = 0.0;
        int n_wet = 0;
        for (int ki = start; ki < end; ++ki) {
            int i = by_depth[static_cast<std::size_t>(ki)];
            auto ui = static_cast<std::size_t>(i);
            if (q50[ui] > 1e-4) {
                sum_q50 += q50[ui];
                sum_sp  += q95[ui] - q05[ui];
                ++n_wet;
            }
        }
        if (n_wet > 0) {
            double mq = sum_q50 / n_wet;
            double ms = sum_sp  / n_wet;
            std::printf("  %-16s  %-10.6f  %-10.6f  %-8.4f\n",
                        tnames[t3], mq, ms, ms / mq);
        } else {
            std::printf("  %-16s  (dry)\n", tnames[t3]);
        }
    }

    std::printf("\nDone.\n");
    return 0;
}
