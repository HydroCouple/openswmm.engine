/**
 * @file test_rom_coverage.cpp
 * @brief PR 10 — ROM uncertainty bands vs brute-force Monte Carlo (fixes:
 *        credibility). Validates the composite effect of reform PRs 4–9.
 *
 * Reference: 21 deterministic engine runs (no ROM) with Manning's n globally
 * scaled by the LHS strata midpoints of ±20% — the same parameter prior the
 * ROM samples — giving empirical head quantiles per (node, report time).
 * ROM run: identical network, `1D MANNINGS_N 0.20`, M = 50.
 *
 * Assertions (thresholds tightened from the first measured run; the measured
 * values are documented next to each assertion):
 *   (a) coverage — ROM [q05, q95] contains the MC median at report times
 *       t > 60 s for at least the stated fraction of (node, time) samples;
 *   (b) width ratio — ROM (q95−q05) vs MC (q95−q05) stays within the stated
 *       band wherever the MC width is resolvable (> 1e-6 m).
 *
 * Runtime: 22 engine runs on a 6-node, 10-minute network — ~2 s total.
 *
 * @warning STATUS 2026-08-04: this file existed since 586e492b (2026-07-08)
 *          but was never wired into CMake — registered for the first time
 *          while starting PR P4 (HSYM_RESIDUALS_PR_CHECKLIST.md). On first
 *          run under the current base it FAILS: coverage=1.000 but width
 *          ratio min/med/max = 0.024/0.082/0.889 (in-band 0.303), far below
 *          the documented 0.676/1.251/1.610. Root cause is NOT the mechanical
 *          port (buildROM1D/computeK1d/SpectralROM1D verified byte-faithful
 *          to the pre-port sidecar) and NOT a units bug (the internal-always-
 *          feet architecture in PostParseResolver.cpp's convert_inputs_to_
 *          internal/convert_internal_to_display pair is correct, deliberate,
 *          longstanding design — ruled out after initially suspecting it).
 *          The gap is left deliberately UNFIXED and this test deliberately
 *          left RED, per this checklist's own hard rule 2 ("nobody tunes a
 *          tolerance to green a spread-magnitude test") — see
 *          history_decisions.md's "P4 baseline: PR-10 harness fails on first
 *          run" entry and the checklist's P4 section for the escalation.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "openswmm/engine/openswmm_engine.h"
#include "core/SWMMEngine.hpp"
#include "uncertainty/SpectralROM1D.hpp"

#if defined(_WIN32)
#  include <process.h>   // _getpid
#  define getpid _getpid
#else
#  include <unistd.h>    // getpid
#endif

namespace {

// ============================================================================
// Fixture network — the Phase-9 chain (J1..J5 → O1), 10-minute run.
// ============================================================================

constexpr double kBaseRoughness = 0.013;
constexpr double kPert          = 0.20;   // ±20% Manning prior (both MC and ROM)
constexpr int    kMcRuns        = 21;
constexpr double kRoutingStep   = 30.0;   // s
constexpr double kReportStep    = 60.0;   // s
constexpr double kEndTime       = 3600.0; // s (sampling window)

std::string fixtureInp(double rough_mult, bool with_rom) {
    char rough[32];
    std::snprintf(rough, sizeof(rough), "%.9f", kBaseRoughness * rough_mult);
    std::string inp = R"([TITLE]
ROM vs MC coverage validation (PR 10)

[OPTIONS]
FLOW_UNITS           CMS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2025
START_TIME           00:00:00
REPORT_START_DATE    01/01/2025
REPORT_START_TIME    00:00:00
END_DATE             01/01/2025
END_TIME             01:05:00
REPORT_STEP          00:01:00
ROUTING_STEP         0:00:30
MIN_SURFAREA         1.0
MAX_TRIALS           8
HEAD_TOLERANCE       0.005
MINIMUM_STEP         0.5
THREADS              1

[JUNCTIONS]
;;Name  Elevation  MaxDepth  InitDepth  SurDepth  Aponded
J1      100        5         0.10       0         0
J2       95        5         0.10       0         0
J3       90        5         0.10       0         0
J4       85        5         0.10       0         0
J5       80        5         0.10       0         0

[OUTFALLS]
;;Name  Elevation  Type  Stage  Gated
O1      75         FREE         NO

[CONDUITS]
;;Name  From  To   Length  Roughness  InOffset  OutOffset  InitFlow  MaxFlow
C1      J1    J2   100     ROUGH      0         0          0         0
C2      J2    J3   100     ROUGH      0         0          0         0
C3      J3    J4   100     ROUGH      0         0          0         0
C4      J4    J5   100     ROUGH      0         0          0         0
C5      J5    O1   100     ROUGH      0         0          0         0

[XSECTIONS]
;;Link  Shape    Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR 1.0    0      0      0      1
C2      CIRCULAR 1.0    0      0      0      1
C3      CIRCULAR 1.0    0      0      0      1
C4      CIRCULAR 1.0    0      0      0      1
C5      CIRCULAR 1.0    0      0      0      1

[DWF]
;;Node  Constituent  Baseline  Patterns
J1      FLOW         0.1

[REPORT]
INPUT      NO
CONTINUITY YES
FLOWSTATS  YES
CONTROLS   NO
SUBCATCHMENTS NONE
NODES      ALL
LINKS      ALL
)";
    if (with_rom) {
        inp += "\n[UNCERTAINTY]\n1D  MANNINGS_N  0.20\n";
    }
    // Substitute all ROUGH placeholders.
    std::string::size_type pos;
    while ((pos = inp.find("ROUGH")) != std::string::npos)
        inp.replace(pos, 5, rough);
    return inp;
}

// ============================================================================
// Engine run helpers
// ============================================================================

struct RunResult {
    // heads[node_name][report_index] — deterministic head at report times
    std::map<std::string, std::vector<double>> heads;
    // ROM quantiles (ROM run only), same indexing
    std::map<std::string, std::vector<double>> q05, q50, q95;
    std::vector<double> times;   // report times (s), aligned with the vectors
    bool ok = false;
};

RunResult runCase(const std::string& inp_text, const char* tag, bool with_rom) {
    RunResult out;
    // Per-process prefix to avoid collisions under parallel ctest -j.
    const std::string pfx = "/tmp/rom_cov_" + std::to_string(getpid()) + "_" + tag;
    const std::string inp_path = pfx + ".inp";
    const std::string rpt_path = pfx + ".rpt";
    {
        std::ofstream f(inp_path);
        f << inp_text;
    }

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

    // Junction indices by name (J1..J5); outfall excluded.
    std::map<std::string, int> node_idx;
    for (int i = 0; i < n_nodes; ++i) {
        std::string nm = ctx.node_names.name_of(i);
        if (!nm.empty() && nm[0] == 'J') node_idx[nm] = i;
    }

    const auto* rom = with_rom ? eng->rom1d() : nullptr;

    // Track engine time via the elapsed value the API returns (days; 0 at
    // the final step). The engine may sub-step internally, so a step counter
    // would misreport time. Sample whenever a report boundary is crossed —
    // that is also when the engine has just refreshed the ROM quantiles.
    // The fixture END_TIME (12 min) deliberately exceeds the sampling window
    // (10 min): the loop exits at kEndTime during normal stepping and never
    // reaches the engine's final-boundary stepping, which can stall on
    // sub-second time snapping (see the OADate rounding note in .memory).
    double elapsed = 1.0;
    double t_prev = 0.0;
    int guard = 0, stalled = 0;
    while (elapsed != 0.0 && guard < 20000) {
        if (swmm_engine_step(handle, &elapsed) != 0) break;
        ++guard;
        const double t_now = (elapsed > 0.0) ? elapsed * 86400.0 : kEndTime;
        stalled = (t_now - t_prev < 1e-9) ? stalled + 1 : 0;
        if (stalled > 200) break;   // no time progress — bail with what we have

        // Report boundaries crossed in (t_prev, t_now]
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
    // Require a non-trivial number of report samples so later indexing is safe.
    // The exact count depends on the engine's adaptive stepping, but every
    // run must produce the same count for the MC-vs-ROM comparison to index
    // correctly. We verify cross-run consistency in the test body by
    // comparing all MC times vectors against the ROM's.
    out.ok = !out.times.empty();
    if (out.ok) {
        // For ROM runs, verify quantile vectors are complete per junction.
        const std::size_t n = out.times.size();
        for (const auto& [nm, _] : out.heads) {
            if (out.q05.count(nm) && out.q05[nm].size() != n)
                out.ok = false;
        }
    }
    cleanup();
    return out;
}

}  // namespace

// ============================================================================
// The validation test
// ============================================================================

TEST(RomCoverage, BandsBracketBruteForceMonteCarlo) {
    // --- Reference: 21 deterministic runs at the LHS strata midpoints -------
    // mult_i = 0.8 + (i + 0.5)/21 * 0.4 — the same ±20% uniform prior the
    // ROM's internal design stratifies.
    std::vector<RunResult> mc(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i) {
        const double mult =
            (1.0 - kPert) + (static_cast<double>(i) + 0.5) / kMcRuns * 2.0 * kPert;
        const std::string tag = "mc" + std::to_string(i);
        mc[static_cast<std::size_t>(i)] =
            runCase(fixtureInp(mult, /*with_rom=*/false), tag.c_str(), false);
        ASSERT_TRUE(mc[static_cast<std::size_t>(i)].ok) << "MC run " << i << " failed";
    }

    // --- ROM run -------------------------------------------------------------
    RunResult rom = runCase(fixtureInp(1.0, /*with_rom=*/true), "rom", true);
    ASSERT_TRUE(rom.ok) << "ROM run failed";
    ASSERT_FALSE(rom.q05.empty()) << "ROM produced no quantiles";

    // --- Verify all runs share the same number of report samples -------------
    // If any run bailed early (stall guard / error), its time vector is shorter
    // and the indexed MC-vs-ROM comparison would go out of range. Use the
    // minimum across all runs as the safe comparison window.
    std::size_t n_samples = rom.times.size();
    for (int i = 0; i < kMcRuns; ++i) {
        n_samples = std::min(n_samples, mc[static_cast<std::size_t>(i)].times.size());
    }
    ASSERT_GT(n_samples, 0u) << "No report samples collected";
    for (const auto& [nm, _] : rom.q05) {
        ASSERT_GE(rom.q05.at(nm).size(), n_samples)
            << "ROM quantile vector too short for node " << nm;
    }

    // --- Compare at every (junction, report time) with t > 60 s --------------
    // MC empirical quantiles from the 21 sorted heads (nearest-rank):
    //   q05 → index 1, q50 → index 10, q95 → index 19.
    int n_total = 0, n_covered = 0;
    int n_width = 0, n_width_ok = 0;
    double ratio_min = 1e300, ratio_max = 0.0;
    std::vector<double> ratios;

    for (const auto& [nm, rom_q05] : rom.q05) {
        const auto& rom_q95 = rom.q95.at(nm);
        for (std::size_t k = 0; k < n_samples; ++k) {
            if (rom.times[k] <= 60.0) continue;
            const bool late = rom.times[k] >= 0.5 * kEndTime;  // saturated regime

            std::vector<double> h(kMcRuns);
            for (int i = 0; i < kMcRuns; ++i)
                h[static_cast<std::size_t>(i)] = mc[static_cast<std::size_t>(i)].heads.at(nm)[k];
            std::sort(h.begin(), h.end());
            const double mc_q50   = h[10];
            const double mc_width = h[19] - h[1];

            ++n_total;
            if (rom_q05[k] <= mc_q50 && mc_q50 <= rom_q95[k]) ++n_covered;

            // Width comparison only in the saturated regime (second half of
            // the window): the deviation-form spread starts at zero by
            // construction and grows toward its parametric steady state, so
            // early-window under-prediction is expected behavior, not error
            // (DEVIATION_FORM.md §4.3; VALIDATION.md).
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

    // Diagnostic report (measured values documented in VALIDATION.md).
    std::printf("[ROM-vs-MC] samples=%d  coverage=%.3f  width-ratio "
                "min/med/max = %.3f / %.3f / %.3f  (in-band frac %.3f of %d)\n",
                n_total, coverage, ratio_min, ratio_med, ratio_max,
                width_frac, n_width);

    // Measured on the first full run (2026-07-08, this fixture):
    //   coverage = 0.997 (294/295), width ratio min/med/max =
    //   0.676 / 1.251 / 1.610, in-band fraction 1.000 of 155.
    // Thresholds below are the checklist floors tightened toward those
    // actuals with margin for platform-to-platform solver noise.

    // (a) Coverage (checklist floor 0.90; measured 0.997).
    EXPECT_GE(coverage, 0.95)
        << "ROM [q05,q95] must contain the MC median at >=95% of samples";

    // (b) Width ratio in the saturated regime (checklist band [0.3, 3.0] at
    //     >=80%; measured 100% inside [0.68, 1.61]).
    EXPECT_GE(width_frac, 0.95)
        << "ROM band width must stay within [0.3x, 3x] of the MC width "
        << "at >=95% of saturated-regime samples";
    EXPECT_GE(ratio_med, 0.5) << "median width ratio implausibly low";
    EXPECT_LE(ratio_med, 2.0) << "median width ratio implausibly high";
}
