/**
 * @file test_soft_rain_corr_coverage.cpp
 * @brief CL-1e — correlated soft-rainfall ROM bands vs correlated Monte Carlo.
 *
 * Sibling of the SR-5 comonotone coverage test (`test_soft_rain_coverage.cpp`).
 * Where SR-5 validated the scalar (`COHERENCE FULL`) soft-rain ROM against a
 * brute-force MC whose members scale rain by one scalar `1 + z_i·CV`, this test
 * validates the *spatially-correlated* path (`COHERENCE CORR_LEN <meters>`).
 *
 * Reference: 21 deterministic engine runs (no ROM), each with a *per-node* rain
 * scaling drawn from the SAME `CorrelatedFieldGenerator::generateCoefficientField`
 * the engine uses internally — member `i`'s rain at node `t` is
 * `base · (1 + W[i][t]·CV)`, where `W` is the marginal-preserving rank/copula
 * field over the five junction coordinates at correlation length `ℓ`. Each MC
 * member therefore has the correct per-node NORMAL marginal *and* the finite
 * spatial correlation the feature introduces (member `i` can be wet upstream and
 * dry downstream).
 *
 * ROM run: the identical five-node network with a single
 * `[SOFT_RAINGAGES] RG1 NORMAL CV 0.40 COHERENCE CORR_LEN ℓ`, M = 50 — the
 * engine builds its own correlated field over the same coordinates.
 *
 * Assertions (checklist floors, analog of SR-5):
 *   (a) coverage — correlated ROM [q05, q95] contains the correlated-MC median
 *       at report times t > 60 s for ≥ 0.90 of (node, time) samples;
 *   (b) width ratio — ROM (q95−q05) vs MC (q95−q05) stays within [0.3x, 3x] for
 *       ≥ 0.80 of saturated-regime samples;
 *   (c) physical point of the feature — the correlated ROM band at the most
 *       downstream junction (J5) is strictly NARROWER than the comonotone
 *       (`COHERENCE FULL`) ROM band for the same spread (spatial decorrelation
 *       cancels accumulated upstream uncertainty).
 *
 * The network mirrors SR-5: large-area storage nodes so heads rise gradually
 * (transient dh/dt) keeping the rainfall-rate-driven soft spread active at every
 * report boundary.
 *
 * Runtime: 21 MC + 2 ROM engine runs on a 6-node, 1-hour network — a few
 * seconds total.
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
#include "2d/uncertainty/CorrelatedFieldGenerator.hpp"
#include "2d/uncertainty/SpatialUncertaintyField.hpp"

#if defined(_WIN32)
#  include <process.h>   // _getpid
#  define getpid _getpid
#else
#  include <unistd.h>    // getpid
#endif

namespace {

constexpr double kBaseRainInHr = 2.0;     // location rain rate (in/hr)
constexpr double kCV           = 0.40;    // NORMAL coefficient of variation
constexpr int    kMcRuns       = 21;
constexpr int    kNodes        = 5;       // junctions J1..J5
constexpr double kSpacingM     = 200.0;   // node spacing along the chain (m)
constexpr double kCorrLen      = 120.0;   // ℓ for the coverage comparison (m)
constexpr double kCorrLenNarrow = 30.0;   // ℓ ≪ spacing for the narrowing check
constexpr double kReportStep   = 300.0;   // s (5 min)
constexpr double kEndTime      = 3600.0;  // s (1 h sampling window)

// Junction (x, y) coordinates — a straight chain 200 m apart. These are the
// exact coordinates the engine's active-node field generator consumes, so the
// MC field shares the same spatial geometry.
double nodeX(int t) { return static_cast<double>(t) * kSpacingM; }  // t = 0..4
double nodeY(int)   { return 0.0; }

// Build the shared [OPTIONS]/[TIMESERIES-free] network preamble.
void writePreamble(std::ostringstream& f) {
    f << std::setprecision(10);
    f << "[TITLE]\nCL-1e correlated soft-rain ROM vs correlated MC\n\n";
    f << "[OPTIONS]\n";
    f << "FLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n";
    f << "START_DATE 01/01/2025\nSTART_TIME 00:00:00\n";
    f << "REPORT_START_DATE 01/01/2025\nREPORT_START_TIME 00:00:00\n";
    f << "END_DATE 01/01/2025\nEND_TIME 01:05:00\n";
    f << "REPORT_STEP 00:05:00\nWET_STEP 00:01:00\nDRY_STEP 00:05:00\nROUTING_STEP 00:00:30\n";
    f << "MINIMUM_STEP 0.5\nTHREADS 1\n\n";
    f << "[EVAPORATION]\nCONSTANT 0.0\n\n";
}

// Emit a flat constant-rate timeseries `name` at intensity `rain` (in/hr).
void writeTimeseries(std::ostringstream& f, const char* name, double rain) {
    const double r = std::max(0.0, rain);
    for (int m = 0; m <= 65; m += 5) {
        const int hh = m / 60, mm = m % 60;
        f << name << " 01/01/2025 " << std::setfill('0') << std::setw(2) << hh
          << ":" << std::setfill('0') << std::setw(2) << mm << " " << r << "\n";
    }
    f << std::setfill(' ');
}

void writeNetworkBody(std::ostringstream& f) {
    f << "[SUBAREAS]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "S" << i << " 0.015 0.20 0.00 0.00 100 OUTLET\n";
    f << "\n[INFILTRATION]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "S" << i << " 0.0 0.0 0.0 0.0 0.0\n";
    // Large-area storage nodes → heads rise gradually (transient dh/dt).
    f << "\n[STORAGE]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "J" << i << " " << (100 - i) << " 60 0 FUNCTIONAL 0 0 20000\n";
    f << "\n[OUTFALLS]\nO1 90 FREE NO\n\n";
    f << "[CONDUITS]\n";
    for (int i = 1; i <= kNodes - 1; ++i)
        f << "C" << i << " J" << i << " J" << (i + 1) << " 200 0.02 0 0 0 0\n";
    f << "C5 J5 O1 200 0.02 0 0 0 0\n\n";
    f << "[XSECTIONS]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "C" << i << " CIRCULAR 0.5 0 0 0 1\n";
    // Node coordinates required by COHERENCE CORR_LEN.
    f << "\n[COORDINATES]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "J" << i << " " << nodeX(i - 1) << " " << nodeY(i - 1) << "\n";
    f << "O1 " << nodeX(kNodes) << " 0.0\n";
    f << "\n[REPORT]\nINPUT NO\nCONTINUITY YES\nNODES ALL\nLINKS ALL\n\n";
}

// MC fixture: five independent gages, one per subcatchment, each at a
// per-node rain intensity (the materialized correlated member). No soft config.
std::string fixtureMc(const double rain[kNodes]) {
    std::ostringstream f;
    writePreamble(f);
    f << "[RAINGAGES]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "RG" << i << " INTENSITY 0:05 1.0 TIMESERIES TS" << i << "\n";
    f << "\n[TIMESERIES]\n";
    for (int i = 1; i <= kNodes; ++i) {
        const std::string nm = "TS" + std::to_string(i);
        writeTimeseries(f, nm.c_str(), rain[i - 1]);
    }
    f << "\n[SUBCATCHMENTS]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "S" << i << " RG" << i << " J" << i << " 5.0 5 100 1.0 0\n";
    f << "\n";
    writeNetworkBody(f);
    return f.str();
}

// ROM fixture: a single shared gage at base rain feeding all subcatchments,
// plus a [SOFT_RAINGAGES] entry with the requested coherence clause.
std::string fixtureRom(const char* coherence) {
    std::ostringstream f;
    writePreamble(f);
    f << "[RAINGAGES]\nRG1 INTENSITY 0:05 1.0 TIMESERIES TS0\n\n";
    f << "[TIMESERIES]\n";
    writeTimeseries(f, "TS0", kBaseRainInHr);
    f << "\n[SUBCATCHMENTS]\n";
    for (int i = 1; i <= kNodes; ++i)
        f << "S" << i << " RG1 J" << i << " 5.0 5 100 1.0 0\n";
    f << "\n";
    writeNetworkBody(f);
    f << "[SOFT_RAINGAGES]\nRG1 NORMAL CV " << kCV << " " << coherence << "\n";
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
    const std::string pfx = "/tmp/cl1e_" + std::to_string(getpid()) + "_" + tag;
    const std::string inp_path = pfx + ".inp";
    const std::string rpt_path = pfx + ".rpt";
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
    if (out.ok) {
        const std::size_t n = out.times.size();
        for (const auto& [nm, _] : out.heads) {
            if ((out.q05.count(nm) && out.q05[nm].size() != n)
                || (out.q50.count(nm) && out.q50[nm].size() != n)
                || (out.q95.count(nm) && out.q95[nm].size() != n))
                out.ok = false;
        }
    }
    cleanup();
    return out;
}

// Maximum ROM band width max|q95 - q05| for a single junction across the run.
double romBandAtNode(const RunResult& r, const std::string& node) {
    if (!r.q05.count(node) || !r.q95.count(node)) return 0.0;
    const auto& lo = r.q05.at(node);
    const auto& hi = r.q95.at(node);
    double worst = 0.0;
    for (std::size_t k = 0; k < lo.size() && k < hi.size(); ++k)
        worst = std::max(worst, std::abs(hi[k] - lo[k]));
    return worst;
}

}  // namespace

// ---------------------------------------------------------------------------
// (a) + (b): correlated ROM bands bracket a correlated Monte-Carlo reference.
// ---------------------------------------------------------------------------
TEST(SoftRainCorrCoverage, CorrelatedBandsBracketCorrelatedMonteCarlo) {
    using openswmm::uncertainty::probit;
    using openswmm::twoD::CorrelatedFieldGenerator;
    using openswmm::twoD::SpatialUncertaintyField;

    // --- Build the correlated per-member field the MC materializes ----------
    // coeff_i = z_i = probit((i+0.5)/21): the NORMAL strata midpoints, so each
    // node's per-member marginal is exactly {1 + z_i·CV}. The rank/copula field
    // decorrelates those coefficients across the five junctions at ℓ = kCorrLen.
    std::vector<double> coeff(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i)
        coeff[static_cast<std::size_t>(i)] = probit((static_cast<double>(i) + 0.5) / kMcRuns);

    std::vector<double> cx(kNodes), cy(kNodes);
    for (int t = 0; t < kNodes; ++t) { cx[static_cast<std::size_t>(t)] = nodeX(t);
                                       cy[static_cast<std::size_t>(t)] = nodeY(t); }

    SpatialUncertaintyField field;
    CorrelatedFieldGenerator::generateCoefficientField(
        cx.data(), cy.data(), kNodes, coeff, kCorrLen,
        UINT64_C(0xC0FFEE15CA1E0101), field);
    ASSERT_TRUE(field.is_spatial());
    ASSERT_EQ(field.n_members, kMcRuns);
    ASSERT_EQ(field.n_cells, kNodes);

    // --- Reference: 21 deterministic runs with per-node correlated rain -----
    std::vector<RunResult> mc(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i) {
        double rain[kNodes];
        for (int t = 0; t < kNodes; ++t)
            rain[t] = kBaseRainInHr * (1.0 + field.at(i, t) * kCV);
        const std::string tag = "mc" + std::to_string(i);
        mc[static_cast<std::size_t>(i)] = runCase(fixtureMc(rain), tag.c_str(), false);
        ASSERT_TRUE(mc[static_cast<std::size_t>(i)].ok) << "MC run " << i << " failed";
    }

    // --- ROM run (correlated, matching ℓ) -----------------------------------
    std::ostringstream coh;
    coh << "COHERENCE CORR_LEN " << kCorrLen;
    RunResult rom = runCase(fixtureRom(coh.str().c_str()), "romcorr", true);
    ASSERT_TRUE(rom.ok) << "correlated ROM run failed";
    ASSERT_FALSE(rom.q05.empty()) << "ROM produced no soft-rain quantiles";

    std::size_t n_samples = rom.times.size();
    for (int i = 0; i < kMcRuns; ++i)
        n_samples = std::min(n_samples, mc[static_cast<std::size_t>(i)].times.size());
    ASSERT_GT(n_samples, 0u) << "No report samples collected";

    // --- Compare at every (junction, report time) with t > 60 s -------------
    int n_total = 0, n_covered = 0;
    int n_width = 0, n_width_ok = 0;
    double ratio_min = 1e300, ratio_max = 0.0;
    std::vector<double> ratios;

    for (const auto& [nm, rom_q05] : rom.q05) {
        const auto& rom_q95 = rom.q95.at(nm);
        for (std::size_t k = 0; k < n_samples && k < rom_q05.size(); ++k) {
            if (rom.times[k] <= 60.0) continue;
            const bool late = rom.times[k] >= 0.5 * kEndTime;

            std::vector<double> h(kMcRuns);
            for (int i = 0; i < kMcRuns; ++i)
                h[static_cast<std::size_t>(i)] = mc[static_cast<std::size_t>(i)].heads.at(nm)[k];
            std::sort(h.begin(), h.end());
            const double mc_q50   = h[10];      // nearest-rank median of 21
            const double mc_width = h[19] - h[1];  // q95 − q05 (nearest-rank)

            ++n_total;
            if (rom_q05[k] <= mc_q50 && mc_q50 <= rom_q95[k]) ++n_covered;

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

    std::printf("[SoftRainCorr-vs-MC] samples=%d coverage=%.3f width-ratio "
                "min/med/max = %.3f / %.3f / %.3f (in-band frac %.3f of %d)\n",
                n_total, coverage, ratio_min, ratio_med, ratio_max,
                width_frac, n_width);

    // Checklist floors: coverage >= 0.90, width-ratio in [0.3, 3.0] at >= 0.80.
    // Actuals are printed above and recorded in VALIDATION.md on first run.
    EXPECT_GE(coverage, 0.90)
        << "correlated ROM [q05,q95] must contain the correlated-MC median at "
        << ">=90% of samples";
    EXPECT_GE(width_frac, 0.80)
        << "correlated ROM band width must stay within [0.3x, 3x] of MC width "
        << "at >=80% of saturated-regime samples";
}

// ---------------------------------------------------------------------------
// (c): the physical point — correlated bands are narrower downstream than the
// comonotone bands for the same spread (spatial decorrelation cancels).
// ---------------------------------------------------------------------------
TEST(SoftRainCorrCoverage, CorrelatedNarrowerThanComonotoneDownstream) {
    // Same network, same CV; only the coherence clause changes. A short
    // correlation length (ℓ ≪ node spacing) makes each junction rank members
    // independently, so accumulated upstream uncertainty partially cancels at
    // the most-downstream junction J5.
    RunResult rom_full = runCase(fixtureRom("COHERENCE FULL"), "full", true);
    std::ostringstream coh;
    coh << "COHERENCE CORR_LEN " << kCorrLenNarrow;
    RunResult rom_corr = runCase(fixtureRom(coh.str().c_str()), "corr30", true);
    ASSERT_TRUE(rom_full.ok) << "comonotone ROM run failed";
    ASSERT_TRUE(rom_corr.ok) << "correlated ROM run failed";

    const double band_full = romBandAtNode(rom_full, "J5");
    const double band_corr = romBandAtNode(rom_corr, "J5");

    std::printf("[SoftRainCorr-narrowing] J5 band comonotone=%.6f "
                "correlated(ℓ=30)=%.6f ratio=%.3f\n",
                band_full, band_corr,
                band_full > 0.0 ? band_corr / band_full : 0.0);

    ASSERT_GT(band_full, 1.0e-6) << "comonotone J5 band must be resolvable";
    EXPECT_LT(band_corr, band_full)
        << "short-correlation-length band at the downstream junction must be "
        << "narrower than the comonotone band (spatial decorrelation cancels)";
}
