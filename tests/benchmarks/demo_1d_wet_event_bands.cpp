/**
 * @file demo_1d_wet_event_bands.cpp
 * @brief Wet weather confidence bands for an Edinburgh-type steep sewer trunk.
 *
 * @details Propagates Manning's n uncertainty through a 30-minute urban storm on
 *          a 1-km steep channel representative of synthetic_city_04 (Edinburgh).
 *
 *          Parameters derived from city_04 network statistics:
 *            - median pipe slope  = 0.035
 *            - Manning's n        = 0.013  (clean concrete)
 *            - equivalent width   = 2.0 m  (rectangular section)
 *            - trunk length       = 1000 m
 *
 *          Physics: kinematic wave, upwind finite-difference.
 *            Q_j   = (1/n) * W * h_j^(5/3) * sqrt(S)
 *            h_j^{k+1} = h_j^k - (dt/dx) * (Q_j - Q_{j-1}) / W
 *
 *          Upstream BC: normal depth for the triangular storm inflow.
 *          Downstream: free — last-cell depth reads out as outflow.
 *
 *          Ensemble: M=50 LHS strata over Manning's n ∈ [0.75, 1.25] × base.
 *          CFL is checked at runtime; all members satisfy CFL < 1 for dt=2s, dx=10m.
 *
 *          Why kinematic wave, not the DCT-II ROM from demo_1d_spectral_rom.cpp?
 *          At slope=0.035, diffusion is negligible: D·T / L^2 ≈ 0.0004. The wave
 *          translates rather than diffuses, so the DCT-II diffusion basis misses the
 *          dominant physics. For gentler slopes (≤ 0.005) the diffusion-wave ROM is
 *          the right model.
 *
 * @note Run with:  ./demo_1d_wet_event_bands
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

    // ---- Channel parameters (city_04 representative) ----
    const int    N_cells = 100;     // routing cells along trunk
    const double L       = 1000.0;  // trunk length (m)
    const double slope   = 0.035;   // Edinburgh median pipe slope
    const double mann    = 0.013;   // clean concrete Manning's n
    const double W       = 2.0;     // equivalent rectangular width (m)

    // ---- Simulation ----
    const double dt      = 2.0;     // routing timestep (s) — CFL-safe for all members
    const int    n_steps = 900;     // 1800 s total
    const int    out_int = 30;      // print every out_int steps (60 s)

    // ---- Storm: asymmetric triangular hyetograph ----
    const double Q_peak  = 0.20;    // peak inflow (m^3/s)
    const double t_peak  = 600.0;   // time of peak (s) — 1/3 of storm
    const double t_storm = 1800.0;  // total storm duration (s)

    // ---- Ensemble ----
    const int    M       = 50;
    const double m_pert  = 0.25;    // ±25% Manning's n

    const double dx = L / N_cells;

    // ---- LHS ensemble: M strata over [1-pert, 1+pert] ----
    std::vector<double> mm(static_cast<std::size_t>(M));
    for (int i = 0; i < M; ++i)
        mm[static_cast<std::size_t>(i)] =
            (1.0 - m_pert) + (i + 0.5) / M * (2.0 * m_pert);

    // ---- Helper lambdas ----

    // Storm inflow (m^3/s)
    auto Q_storm = [&](double t) -> double {
        if (t <= 0.0)      return 0.0;
        if (t <= t_peak)   return Q_peak * (t / t_peak);
        if (t <= t_storm)  return Q_peak * (1.0 - (t - t_peak) / (t_storm - t_peak));
        return 0.0;
    };

    // Normal depth for rectangular wide channel: h = (Q·n / (W·sqrt(S)))^(3/5)
    auto normal_depth = [&](double Q, double n_eff) -> double {
        if (Q < 1e-10) return 0.0;
        return std::pow(Q * n_eff / (W * std::sqrt(slope)), 0.6);
    };

    // Manning's discharge from depth
    auto manning_Q = [&](double h, double n_eff) -> double {
        if (h <= 1e-5) return 0.0;
        return (1.0 / n_eff) * W * std::pow(h, 5.0 / 3.0) * std::sqrt(slope);
    };

    // ---- CFL check (peak flow, fastest member) ----
    {
        double n_min = mann * mm[0];  // smallest n → fastest celerity
        double h_chk = normal_depth(Q_peak, n_min);
        double V_chk = (h_chk > 1e-5) ? Q_peak / (W * h_chk) : 0.0;
        double c_chk = (5.0 / 3.0) * V_chk;
        double cfl   = dt * c_chk / dx;
        if (cfl > 1.0) {
            std::fprintf(stderr,
                "WARNING: CFL = %.3f > 1 for n_min=%.4f.  "
                "Reduce dt or increase dx.\n", cfl, n_min);
        }
    }

    // ---- Ensemble depth arrays: M × N ----
    std::vector<double> h_ens(static_cast<std::size_t>(M * N_cells), 0.0);
    std::vector<double> Q_col(static_cast<std::size_t>(N_cells));

    // ---- Pre-compute nominal wave travel time and time to peak ----
    {
        double h0 = normal_depth(Q_peak, mann);
        double c0 = (5.0 / 3.0) * Q_peak / (W * h0);
        double h_fast = normal_depth(Q_peak, mann * mm[0]);
        double c_fast = (5.0 / 3.0) * Q_peak / (W * h_fast);
        double h_slow = normal_depth(Q_peak, mann * mm[static_cast<std::size_t>(M - 1)]);
        double c_slow = (5.0 / 3.0) * Q_peak / (W * h_slow);

        std::printf("Kinematic wave routing — wet weather confidence bands\n");
        std::printf("City-04 (Edinburgh) representative trunk\n\n");
        std::printf("  Channel: L=%.0f m, N=%d cells, dx=%.1f m, slope=%.3f\n",
                    L, N_cells, dx, slope);
        std::printf("  Manning's n: base=%.3f  range=[%.4f, %.4f] (±%.0f%%)\n",
                    mann, mann * mm[0], mann * mm[static_cast<std::size_t>(M - 1)],
                    m_pert * 100.0);
        std::printf("  Storm: Q_peak=%.2f m^3/s, t_peak=%.0f s, t_end=%.0f s\n\n",
                    Q_peak, t_peak, t_storm);
        std::printf("  Normal depth at peak: h_n=%.4f m (median n)\n", h0);
        std::printf("  Kinematic celerity range:  c_fast=%.3f m/s (n*0.75)"
                    "  →  c_slow=%.3f m/s (n*1.25)\n", c_fast, c_slow);
        std::printf("  Travel time range:  T_fast=%.0f s  →  T_slow=%.0f s  "
                    "(nominal %.0f s)\n\n", L / c_fast, L / c_slow, L / c0);
        std::printf("  Ensemble: M=%d LHS members\n\n", M);
    }

    // ---- Table header ----
    std::printf("%-7s  %-8s  %-8s  %-8s  %-8s  %-7s  %-8s\n",
                "t(s)", "Q_in", "q05_out", "q50_out", "q95_out", "sprd%", "ASCII");
    std::printf("%-7s  %-8s  %-8s  %-8s  %-8s  %-7s  %-8s\n",
                "-------", "--------", "--------", "--------", "--------",
                "-------", "-----------------------------------");

    // Quantile index helpers
    const int i05 = std::max(0, static_cast<int>(0.05 * (M - 1) + 0.5));
    const int i50 = static_cast<int>(0.50 * (M - 1) + 0.5);
    const int i95 = std::min(M - 1, static_cast<int>(0.95 * (M - 1) + 0.5));
    std::vector<double> qbuf(static_cast<std::size_t>(M));

    const int bar_w = 34;
    double Q_scale = Q_peak * 1.05;  // scale for ASCII bar

    // ---- Time loop ----
    for (int step = 0; step <= n_steps; ++step) {
        double t    = step * dt;
        double Q_in = Q_storm(t);

        if (step > 0) {
            // Advance all ensemble members one kinematic wave step
            for (int i = 0; i < M; ++i) {
                auto   ui    = static_cast<std::size_t>(i);
                double n_eff = mann * mm[ui];
                auto*  h     = h_ens.data() + ui * N_cells;

                // Upstream BC: normal depth for this member's n
                h[0] = normal_depth(Q_in, n_eff);

                // Manning's discharge at each cell
                for (int j = 0; j < N_cells; ++j)
                    Q_col[static_cast<std::size_t>(j)] = manning_Q(h[j], n_eff);

                // Upwind update: interior + downstream cells
                for (int j = 1; j < N_cells; ++j) {
                    double flux = (Q_col[static_cast<std::size_t>(j)]
                                 - Q_col[static_cast<std::size_t>(j - 1)]);
                    h[j] = std::max(0.0, h[j] - (dt / dx) * flux / W);
                }
            }
        }

        // Print output row at each out_int steps
        if (step % out_int == 0) {
            // Collect outflow from downstream cell of each member
            for (int i = 0; i < M; ++i) {
                auto   ui    = static_cast<std::size_t>(i);
                double n_eff = mann * mm[ui];
                qbuf[ui] = manning_Q(
                    h_ens[ui * N_cells + static_cast<std::size_t>(N_cells - 1)],
                    n_eff);
            }
            std::sort(qbuf.begin(), qbuf.end());

            double q05 = qbuf[static_cast<std::size_t>(i05)];
            double q50 = qbuf[static_cast<std::size_t>(i50)];
            double q95 = qbuf[static_cast<std::size_t>(i95)];

            double spct = (q50 > 1e-5) ? 100.0 * (q95 - q05) / q50 : 0.0;

            // ASCII bar: scaled to Q_scale
            char bar[bar_w + 4];
            std::fill(bar, bar + sizeof(bar), ' ');
            bar[0] = '|'; bar[bar_w + 1] = '|'; bar[bar_w + 2] = '\0';

            if (Q_scale > 1e-8) {
                int p05 = std::max(0, std::min(bar_w - 1,
                              static_cast<int>(q05 / Q_scale * bar_w)));
                int p50 = std::max(0, std::min(bar_w - 1,
                              static_cast<int>(q50 / Q_scale * bar_w)));
                int p95 = std::max(0, std::min(bar_w - 1,
                              static_cast<int>(q95 / Q_scale * bar_w)));
                for (int b = p05; b <= p95; ++b)
                    bar[b + 1] = '-';
                bar[p50 + 1] = '|';
            }

            std::printf("%-7.0f  %-8.4f  %-8.4f  %-8.4f  %-8.4f  %-7.1f  %s\n",
                        t, Q_in, q05, q50, q95, spct, bar);
        }
    }

    // ---- Peak summary ----
    // Collect each member's peak outflow and time of peak
    std::vector<double> peak_Q(static_cast<std::size_t>(M), 0.0);
    std::vector<double> peak_t(static_cast<std::size_t>(M), 0.0);

    // Re-run to find peaks (or we can track during the loop — do a second pass
    // from the final ensemble state; instead, recompute from scratch)
    {
        // Reset ensemble
        std::fill(h_ens.begin(), h_ens.end(), 0.0);
        for (int step = 1; step <= n_steps; ++step) {
            double t    = step * dt;
            double Q_in = Q_storm(t);
            for (int i = 0; i < M; ++i) {
                auto   ui    = static_cast<std::size_t>(i);
                double n_eff = mann * mm[ui];
                auto*  h     = h_ens.data() + ui * N_cells;
                h[0] = normal_depth(Q_in, n_eff);
                for (int j = 0; j < N_cells; ++j)
                    Q_col[static_cast<std::size_t>(j)] = manning_Q(h[j], n_eff);
                for (int j = 1; j < N_cells; ++j) {
                    double flux = (Q_col[static_cast<std::size_t>(j)]
                                 - Q_col[static_cast<std::size_t>(j - 1)]);
                    h[j] = std::max(0.0, h[j] - (dt / dx) * flux / W);
                }
                double q_out = manning_Q(
                    h[static_cast<std::size_t>(N_cells - 1)], n_eff);
                if (q_out > peak_Q[ui]) {
                    peak_Q[ui] = q_out;
                    peak_t[ui] = t;
                }
            }
        }

        std::sort(peak_Q.begin(), peak_Q.end());
        std::sort(peak_t.begin(), peak_t.end());

        std::printf("\n--- Peak outflow summary ---\n\n");
        std::printf("  Peak Q:   q05=%.4f  q50=%.4f  q95=%.4f  m^3/s"
                    "  (input peak = %.4f)\n",
                    peak_Q[static_cast<std::size_t>(i05)],
                    peak_Q[static_cast<std::size_t>(i50)],
                    peak_Q[static_cast<std::size_t>(i95)],
                    Q_peak);
        std::printf("  Time of peak:  q05=%.0f s  q50=%.0f s  q95=%.0f s\n",
                    peak_t[static_cast<std::size_t>(i05)],
                    peak_t[static_cast<std::size_t>(i50)],
                    peak_t[static_cast<std::size_t>(i95)]);
        std::printf("  Peak timing spread: %.0f s (5th–95th)\n",
                    peak_t[static_cast<std::size_t>(i95)]
                  - peak_t[static_cast<std::size_t>(i05)]);
        std::printf("  Peak magnitude spread: %.1f%%\n",
                    100.0 * (peak_Q[static_cast<std::size_t>(i95)]
                           - peak_Q[static_cast<std::size_t>(i05)])
                          / peak_Q[static_cast<std::size_t>(i50)]);
    }

    std::printf("\nDone.\n");
    return 0;
}
