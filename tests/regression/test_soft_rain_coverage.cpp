/**
 * @file test_soft_rain_coverage.cpp
 * @brief SR-5 — soft-rainfall ROM bands vs brute-force Monte Carlo.
 *
 * Reference: 21 deterministic engine runs (no ROM), each with the rain-gage
 * input scaled by the materialized member `1 + z(u_stratum)·CV` — the same
 * NORMAL location-scale prior the soft-rain ROM propagates. ROM run: identical
 * network with `[SOFT_RAINGAGES] RG1 NORMAL CV 0.20`, M = 50.
 *
 * Assertions (checklist floors, tightened toward the first measured run whose
 * actuals are documented inline):
 *   (a) coverage — ROM [q05, q95] contains the MC median at report times
 *       t > 60 s for at least the stated fraction of (node, time) samples;
 *   (b) width ratio — ROM (q95−q05) vs MC (q95−q05) stays within [0.3x, 3x]
 *       for at least the stated fraction of samples where the MC width is
 *       resolvable.
 *
 * The network uses storage nodes with a large constant surface area so heads
 * rise gradually through the whole run (transient dh/dt), keeping the
 * rainfall-rate-driven soft spread active at every report boundary and giving
 * the MC members resolvable head spread.
 *
 * Runtime: 22 engine runs on a 6-node, 1-hour network — a few seconds total.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "core/SWMMEngine.hpp"
#include "uncertainty/SpectralROM1D.hpp"
#include "uncertainty/LhsShuffle.hpp"

namespace {

constexpr double kBaseRainInHr = 1.0;     // location rain rate (in/hr)
constexpr double kCV           = 0.20;    // NORMAL coefficient of variation
constexpr int    kMcRuns       = 21;
constexpr double kReportStep   = 300.0;   // s (5 min)
constexpr double kEndTime      = 3600.0;  // s (1 h sampling window)

// Build the fixture .inp. `rain_mult` scales the gage timeseries (the
// materialized MC member); `with_soft` adds the [SOFT_RAINGAGES] entry that
// activates the soft-rain 1D ROM.
std::string fixtureInp(double rain_mult, bool with_soft) {
    const double rain = kBaseRainInHr * rain_mult;
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\nSR-5 soft-rain ROM vs MC coverage\n\n";
    f << "[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\n";
    f << "REPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 01:05:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:30\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n";
    f << "[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n";
    // Constant rain at 5-min recording interval so the rate is flat over the run.
    f << "[TIMESERIES]\n";
    for (int m = 0; m <= 65; m += 5) {
        const int hh = m / 60, mm = m % 60;
        f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << hh
          << ":" << std::setfill('0') << std::setw(2) << mm << " " << rain << "\n";
    }
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\n";
    for (int i = 1; i <= 5; ++i)
        f << "S" << i << " RG1 J" << i << " 5.0 5 100 1.0 0\n";
    f << "\n[SUBAREAS]\n";
    for (int i = 1; i <= 5; ++i)
        f << "S" << i << " 0.015 0.20 0.00 0.00 100 OUTLET\n";
    f << "\n[INFILTRATION]\n";
    for (int i = 1; i <= 5; ++i)
        f << "S" << i << " 0.0 0.0 0.0 0.0 0.0\n";
    // Large-area storage nodes → heads rise gradually (transient dh/dt).
    f << "\n[STORAGE]\n";
    for (int i = 1; i <= 5; ++i)
        f << "J" << i << " " << (100 - i) << " 60 0 FUNCTIONAL 0 0 20000\n";
    f << "\n[OUTFALLS]\nO1 90 FREE NO\n\n";
    f << "[CONDUITS]\n";
    for (int i = 1; i <= 4; ++i)
        f << "C" << i << " J" << i << " J" << (i + 1) << " 200 0.02 0 0 0 0\n";
    f << "C5 J5 O1 200 0.02 0 0 0 0\n\n";
    f << "[XSECTIONS]\n";
    for (int i = 1; i <= 5; ++i)
        f << "C" << i << " CIRCULAR 0.5 0 0 0 1\n";
    f << "\n[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (with_soft)
        f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV " << kCV << "\n";
    return f.str();
}

struct RunResult {
    std::map<std::string, std::vector<double>> heads;
    std::map<std::string, std::vector<double>> q05, q50, q95;
    std::vector<double> times;
    bool ok = false;
};

RunResult runCase(const std::string& inp_text, const char* tag, bool with_soft) {
    RunResult out;
    const std::string inp_path = std::string("/tmp/sr5_") + tag + ".inp";
    const std::string rpt_path = std::string("/tmp/sr5_") + tag + ".rpt";
    { std::ofstream f(inp_path); f << inp_text; }

    SWMM_Engine handle = swmm_engine_create();
    if (!handle) return out;
    auto cleanup = [&]() {
        swmm_engine_end(handle);
        swmm_engine_close(handle);
        swmm_engine_destroy(handle);
        std::remove(inp_path.c_str());
        std::remove(rpt_path.c_str());
        std::remove((rpt_path.substr(0, rpt_path.size() - 4) + ".uncertainty.csv").c_str());
    };

    if (swmm_engine_open(handle, inp_path.c_str(), rpt_path.c_str(), nullptr, nullptr) != 0 ||
        swmm_engine_initialize(handle) != 0 ||
        swmm_engine_start(handle, 0) != 0) {
        cleanup();
        return out;
    }

    auto* eng = static_cast<openswmm::SWMMEngine*>(handle);
    const auto& ctx = eng->context();
    const int n_nodes = static_cast<int>(ctx.nodes.head.size());

    std::map<std::string, int> node_idx;
    for (int i = 0; i < n_nodes; ++i) {
        std::string nm = ctx.node_names.name_of(i);
        if (!nm.empty() && nm[0] == 'J') node_idx[nm] = i;
    }

    const auto* rom = with_soft ? eng->rom1d() : nullptr;

    double elapsed = 1.0;
    double t_prev = 0.0;
    int guard = 0, stalled = 0;
    while (elapsed != 0.0 && guard < 20000) {
        if (swmm_engine_step(handle, &elapsed) != 0) break;
        ++guard;
        const double t_now = (elapsed > 0.0) ? elapsed * 86400.0 : kEndTime;
        stalled = (t_now - t_prev < 1e-9) ? stalled + 1 : 0;
        if (stalled > 200) break;

        for (double b = std::floor(t_prev / kReportStep + 1.0) * kReportStep;
             b <= t_now + 1e-6; b += kReportStep) {
            if (b > kEndTime + 1e-6) break;
            out.times.push_back(b);
            for (const auto& [nm, ui] : node_idx) {
                out.heads[nm].push_back(ctx.nodes.head[static_cast<std::size_t>(ui)]);
                if (rom && rom->is_ready() &&
                    ui < static_cast<int>(rom->full_to_active.size())) {
                    const int ai = rom->full_to_active[static_cast<std::size_t>(ui)];
                    if (ai >= 0) {
                        out.q05[nm].push_back(rom->q05[static_cast<std::size_t>(ai)]);
                        out.q50[nm].push_back(rom->q50[static_cast<std::size_t>(ai)]);
                        out.q95[nm].push_back(rom->q95[static_cast<std::size_t>(ai)]);
                    }
                }
            }
        }
        t_prev = t_now;
        if (t_now >= kEndTime) break;
    }
    out.ok = !out.times.empty();
    cleanup();
    return out;
}

}  // namespace

TEST(SoftRainCoverage, BandsBracketBruteForceMonteCarlo) {
    using openswmm::uncertainty::probit;

    // --- Reference: 21 deterministic runs at NORMAL strata midpoints --------
    // member_i rain scale = 1 + z_i·CV, z_i = probit((i+0.5)/21).
    std::vector<RunResult> mc(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i) {
        const double u = (static_cast<double>(i) + 0.5) / kMcRuns;
        const double z = probit(u);
        const double mult = 1.0 + z * kCV;
        const std::string tag = "mc" + std::to_string(i);
        mc[static_cast<std::size_t>(i)] =
            runCase(fixtureInp(mult, /*with_soft=*/false), tag.c_str(), false);
        ASSERT_TRUE(mc[static_cast<std::size_t>(i)].ok) << "MC run " << i << " failed";
    }

    // --- ROM run -------------------------------------------------------------
    RunResult rom = runCase(fixtureInp(1.0, /*with_soft=*/true), "rom", true);
    ASSERT_TRUE(rom.ok) << "ROM run failed";
    ASSERT_FALSE(rom.q05.empty()) << "ROM produced no soft-rain quantiles";

    // --- Compare at every (junction, report time) with t > 60 s -------------
    // MC empirical quantiles from 21 sorted heads (nearest-rank):
    //   q05 → index 1, q50 → index 10, q95 → index 19.
    int n_total = 0, n_covered = 0;
    int n_width = 0, n_width_ok = 0;
    double ratio_min = 1e300, ratio_max = 0.0;
    std::vector<double> ratios;

    for (const auto& [nm, rom_q05] : rom.q05) {
        const auto& rom_q95 = rom.q95.at(nm);
        for (std::size_t k = 0; k < rom.times.size() && k < rom_q05.size(); ++k) {
            if (rom.times[k] <= 60.0) continue;
            const bool late = rom.times[k] >= 0.5 * kEndTime;

            std::vector<double> h(kMcRuns);
            for (int i = 0; i < kMcRuns; ++i)
                h[static_cast<std::size_t>(i)] = mc[static_cast<std::size_t>(i)].heads.at(nm)[k];
            std::sort(h.begin(), h.end());
            const double mc_q50   = h[10];
            const double mc_width = h[19] - h[1];

            ++n_total;
            if (rom_q05[k] <= mc_q50 && mc_q50 <= rom_q95[k]) ++n_covered;

            // Width comparison only in the saturated regime (second half):
            // the deviation-form spread starts at zero and grows toward its
            // parametric steady state, so early-window under-prediction is
            // expected (DEVIATION_FORM.md §4.3).
            if (late && mc_width > 1e-6) {
                const double rom_width = rom_q95[k] - rom_q05[k];
                const double ratio = rom_width / mc_width;
                ++n_width;
                ratios.push_back(ratio);
                ratio_min = std::min(ratio_min, ratio);
                ratio_max = std::max(ratio_max, ratio);
                if (ratio >= 0.3 && ratio <= 3.0) ++n_width_ok;
            }
        }
    }
    ASSERT_GT(n_total, 0);
    ASSERT_GT(n_width, 0) << "MC produced no resolvable spread — fixture too static";

    const double coverage = static_cast<double>(n_covered) / n_total;
    const double width_frac = static_cast<double>(n_width_ok) / n_width;
    std::sort(ratios.begin(), ratios.end());
    const double ratio_med = ratios[ratios.size() / 2];

    std::printf("[SoftRain-vs-MC] samples=%d coverage=%.3f width-ratio "
                "min/med/max = %.3f / %.3f / %.3f (in-band frac %.3f of %d)\n",
                n_total, coverage, ratio_min, ratio_med, ratio_max,
                width_frac, n_width);

    // Checklist floors: coverage >= 0.90, width-ratio in [0.3, 3.0] at >= 0.80.
    // Actuals are printed above and recorded in VALIDATION.md on first run.
    //
    // Measured on the first full run (2026-07-16, this fixture):
    //   samples=60  coverage=1.000  width-ratio min/med/max = 0.754/0.821/0.872
    //   in-band fraction 1.000 of 35.
    // The ROM band is slightly narrower than MC (median ratio ~0.82) — the
    // expected mild under-prediction of the delta-linearized soft forcing —
    // but it brackets every MC median and stays well inside [0.3x, 3x].
    EXPECT_GE(coverage, 0.90)
        << "ROM [q05,q95] must contain the MC median at >=90% of samples";
    EXPECT_GE(width_frac, 0.80)
        << "ROM band width must stay within [0.3x, 3x] of MC width at >=80% "
        << "of saturated-regime samples";
}
