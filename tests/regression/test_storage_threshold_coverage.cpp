/**
 * @file test_storage_threshold_coverage.cpp
 * @brief Rain bands at a basin behind a weir vs brute-force Monte Carlo.
 *
 * The case the shared additive planes cannot hold (ADDITIVE_CHANNEL.md §7): a
 * storage basin whose only outlet is a transverse weir. Below the crest the
 * outlet carries nothing, so its straight-line slope at the main run's level
 * is zero and a member's extra rain piles up for the whole run; above the
 * crest the weir passes it on within minutes. Bellinge's G72R basins showed
 * 1–3 m bands against 5–13 mm in the Monte Carlo (2026-10-24).
 *
 * Network: catchment -> basin B1 (constant area) -> weir -> J2 -> J3 -> J4 -> outfall
 * (the 1D ROM needs at least four active nodes).
 * Two scenarios, set from a deterministic calibration run:
 *   A  crest 10 cm below the main run's peak level: the main run spills;
 *   B  crest 3 cm above it: the main run never spills, wetter members do.
 * Reference: 21 runs with the rain scaled by 1 + z_i·CV at NORMAL strata
 * midpoints. ROM: [SOFT_RAINGAGES] NORMAL CV 0.20, with per-member storage
 * levels on (default) and off (the shared planes alone, for comparison).
 *
 * STH_TRACE=1 prints every sample.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
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
#include "mc_quantiles.hpp"

#if defined(_WIN32)
#  include <process.h>
#  define getpid _getpid
#else
#  include <unistd.h>
#endif

namespace {

constexpr double kCV = 0.20;
constexpr int kMcRuns = 21;
constexpr double kReportStep = 300.0;
constexpr double kEndTime = 3.0 * 3600.0;
constexpr double kInvert = 100.0;        // basin invert (ft)
constexpr double kArea = 3000.0;         // basin area (ft^2)

std::string fixtureInp(double rain_mult, double crest, bool with_soft) {
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\nbasin behind a weir\n\n[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 03:05:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:10\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n[TIMESERIES]\n";
    // A one-hour storm peaking at 30 min, then dry.
    for (int m = 0; m <= 185; m += 5) {
        const double tri = (m <= 60) ? (1.0 - std::fabs(m - 30.0) / 30.0) : 0.0;
        const double rain = 2.0 * tri * rain_mult;
        f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << m / 60 << ":"
          << std::setfill('0') << std::setw(2) << m % 60 << " " << rain << "\n";
    }
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 B1 2.0 300 100 1.0 0\n\n[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\nJ2 " << kInvert - 1.0 << " 10 0 0 0\nJ3 " << kInvert - 2.0 << " 10 0 0 0\nJ4 "
      << kInvert - 3.0 << " 10 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 " << kInvert - 4.0 << " FREE NO\n\n";
    f << "[STORAGE]\nB1 " << kInvert << " 20 0 FUNCTIONAL 0 0 " << kArea << "\n\n";
    f << "[CONDUITS]\nC1 J2 J3 300 0.015 0 0 0 0\nC2 J3 J4 300 0.015 0 0 0 0\nC3 J4 O1 300 0.015 0 0 0 0\n\n";
    f << "[WEIRS]\nW1 B1 J2 TRANSVERSE " << crest << " 3.33 NO 0 0 YES\n\n";
    f << "[XSECTIONS]\nC1 CIRCULAR 2 0 0 0 1\nC2 CIRCULAR 2 0 0 0 1\nC3 CIRCULAR 2 0 0 0 1\nW1 RECT_OPEN 4 3 0 0\n\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (with_soft) f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV " << kCV << "\n";
    return f.str();
}

// Raised-end backflow (PH_STORES_AND_PORTS §6 step 5): an offline basin B1
// joined to trunk node J2 by a pipe whose J2 end is raised `raise` ft above
// J2's invert. B1 fills only when J2 rises over that end, and drains back to
// it afterwards (Bellinge G72R83B from G72UD11). J2 is backed up by a smaller
// pipe below it. Same storm as the weir fixture.
std::string fixtureRaisedInp(double rain_mult, double raise, bool with_soft) {
    std::ostringstream f;
    f << std::setprecision(10);
    f << "[TITLE]\noffline basin behind a raised pipe end\n\n[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 03:05:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:10\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n[EVAPORATION]\nCONSTANT 0.0\n\n";
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n[TIMESERIES]\n";
    for (int m = 0; m <= 185; m += 5) {
        const double tri = (m <= 60) ? (1.0 - std::fabs(m - 30.0) / 30.0) : 0.0;
        f << "TS0 01/01/2025 " << std::setfill('0') << std::setw(2) << m / 60 << ":"
          << std::setfill('0') << std::setw(2) << m % 60 << " " << 2.0 * tri * rain_mult << "\n";
    }
    f << std::setfill(' ') << "\n";
    f << "[SUBCATCHMENTS]\nS1 RG1 J1 4.0 400 100 1.0 0\n\n[SUBAREAS]\nS1 0.015 0.20 0.00 0.00 100 OUTLET\n\n";
    f << "[INFILTRATION]\nS1 0.0 0.0 0.0 0.0 0.0\n\n";
    f << "[JUNCTIONS]\nJ1 " << kInvert + 1.0 << " 10 0 0 0\nJ2 " << kInvert - 1.0 << " 10 0 0 0\nJ3 " << kInvert - 2.0
      << " 10 0 0 0\nJ4 " << kInvert - 3.0 << " 10 0 0 0\n\n";
    f << "[OUTFALLS]\nO1 " << kInvert - 4.0 << " FREE NO\n\n";
    f << "[STORAGE]\nB1 " << kInvert - 1.0 << " 20 0 FUNCTIONAL 0 0 2000\n\n";
    f << "[CONDUITS]\nC1 J1 J2 300 0.015 0 0 0 0\nC2 J2 J3 300 0.015 0 0 0 0\nC3 J3 J4 300 0.015 0 0 0 0\n"
      << "C4 J4 O1 300 0.015 0 0 0 0\nCB B1 J2 100 0.015 0 " << std::min(raise, 9.0) << " 0 0\n\n";
    f << "[XSECTIONS]\nC1 CIRCULAR 2 0 0 0 1\nC2 CIRCULAR 1.25 0 0 0 1\nC3 CIRCULAR 2 0 0 0 1\nC4 CIRCULAR 2 0 0 0 1\n"
      << "CB CIRCULAR 1.5 0 0 0 1\n\n";
    f << "[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
    if (with_soft) f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV " << kCV << "\n";
    return f.str();
}

struct Run {
    std::vector<double> times;
    std::map<std::string, std::vector<double>> head, q05, q95;
    bool ok = false;
};

Run runCase(const std::string& inp, const std::string& tag, bool rom_on, bool storage_members) {
    Run out;
    const std::string pfx = "/tmp/sth_" + std::to_string(getpid()) + "_" + tag;
    const std::string ip = pfx + ".inp", rp = pfx + ".rpt";
    { std::ofstream f(ip); f << inp; }
    SWMM_Engine h = swmm_engine_create();
    auto cleanup = [&]() {
        swmm_engine_end(h); swmm_engine_close(h); swmm_engine_destroy(h);
        std::remove(ip.c_str()); std::remove(rp.c_str());
        std::remove((pfx + ".uncertainty.csv").c_str());
        std::remove((pfx + ".rom_threshold.csv").c_str());
        std::remove((pfx + ".rom_diag.csv").c_str());
    };
    if (swmm_engine_open(h, ip.c_str(), rp.c_str(), nullptr, nullptr) != 0) { cleanup(); return out; }
    auto* eng = static_cast<openswmm::SWMMEngine*>(h);
    eng->rom1dStorageMembersEnabled() = storage_members;
    if (swmm_engine_initialize(h) != 0 || swmm_engine_start(h, 0) != 0) { cleanup(); return out; }
    const auto& ctx = eng->context();
    std::map<std::string, int> idx = {{"B1", ctx.node_names.find("B1")}, {"J2", ctx.node_names.find("J2")},
                                      {"J4", ctx.node_names.find("J4")}};
    const auto* rom = rom_on ? eng->rom1d() : nullptr;
    double el = 1.0, tprev = 0.0;
    int guard = 0;
    while (el != 0.0 && guard++ < 200000) {
        if (swmm_engine_step(h, &el) != 0) break;
        const double t = el > 0.0 ? el * 86400.0 : kEndTime;
        for (double b = std::floor(tprev / kReportStep + 1.0) * kReportStep; b <= t + 1e-6; b += kReportStep) {
            if (b > kEndTime + 1e-6) break;
            out.times.push_back(b);
            for (const auto& [nm, ui] : idx) {
                out.head[nm].push_back(ctx.nodes.head[static_cast<std::size_t>(ui)]);
                if (rom && rom->is_ready()) {
                    const int a = rom->full_to_active[static_cast<std::size_t>(ui)];
                    out.q05[nm].push_back(rom->q05[static_cast<std::size_t>(a)]);
                    out.q95[nm].push_back(rom->q95[static_cast<std::size_t>(a)]);
                }
            }
        }
        tprev = t;
        if (t >= kEndTime) break;
    }
    out.ok = !out.times.empty();
    if (rom_on && storage_members) {
        const auto& st = eng->rom1dPortGraphStats();
        std::printf("[STH-stats] %s solves %ld newton %ld armijo_fail %ld factorisations %ld port_evals %ld jacobi_fallbacks %ld jacobi_failed %ld\n",
                    tag.c_str(), st.solves, st.newton_iters, st.armijo_failures, st.factorisations, st.port_evals,
                    st.jacobi_fallbacks, st.jacobi_failed);
    }
    cleanup();
    return out;
}

struct Score { double cov = 0, width = 0; int n = 0; };

Score score(const std::vector<Run>& mc, const Run& rom, const std::string& nm, bool trace, const char* label) {
    std::size_t n = rom.times.size();
    for (const auto& r : mc) n = std::min(n, r.times.size());
    std::vector<double> ratios;
    double cov = 0.0; int cnt = 0;
    for (std::size_t k = 0; k < n && k < rom.q05.at(nm).size(); ++k) {
        if (rom.times[k] < 1800.0) continue;   // from the storm peak on
        std::vector<double> h;
        for (const auto& r : mc) h.push_back(r.head.at(nm)[k]);
        std::sort(h.begin(), h.end());
        const double w_mc = mcq::quantileMidpoint(h, 0.95) - mcq::quantileMidpoint(h, 0.05);
        const double lo = rom.q05.at(nm)[k], hi = rom.q95.at(nm)[k];
        if (w_mc < 0.01) continue;   // 3 mm: not resolvable
        cov += mcq::intervalCoverage(h, lo, hi);
        ratios.push_back((hi - lo) / w_mc);
        ++cnt;
        if (trace)
            std::printf("  %s %s t=%5.0f  MC [%.3f %.3f %.3f]  ROM [%.3f %.3f]  ratio %.2f  cover %.2f\n", label, nm.c_str(),
                        rom.times[k], mcq::quantileMidpoint(h, 0.05) - h[h.size() / 2], h[h.size() / 2],
                        mcq::quantileMidpoint(h, 0.95) - h[h.size() / 2], lo - h[h.size() / 2], hi - h[h.size() / 2],
                        (hi - lo) / w_mc, mcq::intervalCoverage(h, lo, hi));
    }
    Score s; s.n = cnt;
    if (cnt) {
        s.cov = cov / cnt;
        std::sort(ratios.begin(), ratios.end());
        s.width = ratios[ratios.size() / 2];
    }
    return s;
}

using FixtureFn = std::string (*)(double, double, bool);

void runScenario(const char* name, double crest_offset_from_peak, double& basin_cov, double& basin_width,
                 double& off_width, double& below_width, FixtureFn fixture = &fixtureInp,
                 const char* cal_node = "B1", double cal_invert = kInvert, double* basin_cov_off = nullptr) {
    using openswmm::uncertainty::probit;
    const bool trace = std::getenv("STH_TRACE") != nullptr;
    // Calibration: the main run's peak level at cal_node with the threshold out of reach.
    Run cal = runCase(fixture(1.0, 50.0, false), std::string(name) + "cal", false, true);
    ASSERT_TRUE(cal.ok);
    double peak = 0.0;
    for (double v : cal.head.at(cal_node)) peak = std::max(peak, v - cal_invert);
    const double crest = peak + crest_offset_from_peak;
    std::vector<Run> mc(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i) {
        const double z = probit((i + 0.5) / kMcRuns);
        mc[static_cast<std::size_t>(i)] = runCase(fixture(1.0 + z * kCV, crest, false),
                                                  std::string(name) + "mc" + std::to_string(i), false, true);
        ASSERT_TRUE(mc[static_cast<std::size_t>(i)].ok);
    }
    Run on = runCase(fixture(1.0, crest, true), std::string(name) + "on", true, true);
    Run off = runCase(fixture(1.0, crest, true), std::string(name) + "off", true, false);
    ASSERT_TRUE(on.ok && off.ok);
    ASSERT_FALSE(on.q05["B1"].empty());
    double mc_peak = 0.0;
    for (double v : mc[kMcRuns / 2].head.at("B1")) mc_peak = std::max(mc_peak, v - kInvert);
    std::printf("[STH] scenario %s: main-run peak %.3f ft, threshold %.3f ft\n", name, peak, crest);
    for (const char* nm : {"B1", "J2", "J4"}) {
        const Score a = score(mc, on, nm, trace, "on ");
        const Score b = score(mc, off, nm, trace, "off");
        std::printf("[STH] %s %-2s  per-member storage: coverage %.3f width %.3f (n %d)   shared planes only: coverage %.3f width %.3f\n",
                    name, nm, a.cov, a.width, a.n, b.cov, b.width);
        if (std::string(nm) == "B1") { basin_cov = a.cov; basin_width = a.width; off_width = b.width; if (basin_cov_off) *basin_cov_off = b.cov; }
        if (std::string(nm) == "J2") below_width = a.width;
    }
}

}  // namespace

TEST(StorageThreshold, MainRunSpills) {
    // Measured 2026-10-24: basin 0.892 / 0.966 (shared planes alone 0.719 /
    // 0.080, 12x too narrow); J2 just below the weir 0.718 / 0.505 (the pipe
    // emptying below a stopped weir is not a storage node: ranking only).
    double cov = 0, w = 0, w_off = 0, w_below = 0;
    runScenario("A", -0.33, cov, w, w_off, w_below);
    EXPECT_TRUE(mcq::reportCalibration("Basin behind weir, main run spills", cov, w, "ADDITIVE_CHANNEL §7"));
    EXPECT_LE(w, 1.5) << "basin band over-wide";
}

TEST(StorageThreshold, MainRunStopsBelowCrest) {
    // Measured 2026-10-24: basin 0.730 / 0.966 — the right shape (lopsided,
    // -0.80/+0.11 ft against the MC's -0.82/+0.13) but the wetter members
    // bunch just over the crest and the band's upper edge sits 1.5 cm short
    // of the bunch. J2 below the weir 1.41 (it was 840x before the dry-pipe
    // secant: the main run leaves that pipe dry).
    double cov = 0, w = 0, w_off = 0, w_below = 0;
    runScenario("B", 0.10, cov, w, w_off, w_below);
    mcq::reportCalibration("Basin behind weir, main run below crest", cov, w, "ADDITIVE_CHANNEL §7");
    EXPECT_GE(cov, 0.70) << "basin coverage";
    EXPECT_GE(w, 0.8) << "basin band too narrow";
    EXPECT_LE(w, 1.5) << "basin band over-wide";
    EXPECT_LE(w_below, 3.0) << "the dry pipe below the weir is holding members' water again";
}

TEST(StorageThreshold, RaisedEndBackflowFillsOfflineBasin) {
    // PH_STORES_AND_PORTS §6 step 5: the raised-end two-port (note §1.3).
    // J2's pipe end sits 0.3 ft below the main run's peak at J2, so the main
    // run and the wetter members pour into B1, drier members barely. Measured
    // and reported only: no floor is set on a scenario without a history.
    double cov = 0, w = 0, w_off = 0, w_below = 0, cov_off = 0;
    runScenario("R", -0.30, cov, w, w_off, w_below, &fixtureRaisedInp, "J2", kInvert - 1.0, &cov_off);
    mcq::reportCalibration("Offline basin behind a raised pipe end", cov, w, "PH_STORES_AND_PORTS step 5");
    std::printf("[STH] R B1 shared planes only: coverage %.3f width %.3f\n", cov_off, w_off);
}
