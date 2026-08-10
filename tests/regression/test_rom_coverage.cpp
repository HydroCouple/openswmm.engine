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
 * @warning STATUS 2026-08-04 — THIS TEST IS DELIBERATELY RED. It existed since
 *          586e492b (2026-07-08) but was never wired into CMake; registered for
 *          the first time while starting PR P4. It FAILS: coverage=1.000 but
 *          width ratio min/med/max = 0.024/0.082/0.889 (in-band 0.303) vs the
 *          documented 0.676/1.251/1.610.
 *
 *          ROOT CAUSE (measured, not inferred): **the "saturated regime"
 *          premise below is false for this fixture.** The ROM's modal
 *          deviations relax at rate lambda_j * K1d, and on this network the
 *          dominant mode carries b_0 = 0.652 of the sensitivity signal (~76%)
 *          but has tau_0 = 1/(lambda_0*K1d) = 1/(0.0807 * 2.276e-4) ~= 54,500 s
 *          ~= 15 HOURS. At the 1-hour sampling window it is 6.4% saturated, so
 *          the band is ~15x too narrow. The ROM itself is exact: driven to
 *          t = 100,000 s every mode converges to its analytic fixed point
 *          (mode 0 -> 84.05% vs 84.05% predicted; modes 1-3 -> 100.0%). The
 *          spatial signature confirms it — J1, farthest from the grounded
 *          outfall and most mode-0-dominated, is worst (0.027); J5, adjacent to
 *          it and dominated by fast high modes, is nearly right (0.894).
 *
 *          SECONDARY DEFECT (real, but only 1.49x of the ~5x needed):
 *          computeK1d() computes the SI diffusion-wave diffusivity
 *          D = h^(5/3)/(2*n*sqrt(S)) but now receives h and L in internal FEET
 *          while Manning's n stays SI. `convert_inputs_to_internal`
 *          (PostParseResolver.cpp) does NOT exist on the sidecar base where the
 *          reference numbers were measured — it arrived upstream with
 *          a1319721 (2026-07-05, Bellinge bit-parity ladder). Net effect:
 *          K1d = 0.673x the correct SI value, tau 1.49x too long. Worth fixing
 *          on its own merits; it does NOT green this test.
 *
 *          Correcting that still leaves only ~9.4% saturation at 1 h, so the
 *          documented 0.676/1.251/1.610 is NOT reproducible from this fixture
 *          at this window on either base. Note also that at FULL saturation the
 *          ratio is ~2.36, which would still trip the ratio_med <= 2.0 assert —
 *          so the acceptance bounds need revisiting alongside the fixture.
 *
 *          Left UNFIXED per hard rule 2 ("nobody tunes a tolerance to green a
 *          spread-magnitude test"). See history_decisions.md and the P4 section
 *          of HSYM_RESIDUALS_PR_CHECKLIST.md.
 */

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
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

RunResult runCase(const std::string& inp_text, const char* tag, bool with_rom,
                   double end_time_s = kEndTime, double report_step_s = kReportStep,
                   bool phase_enabled = true) {
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

    if (swmm_engine_open(handle, inp_path.c_str(), rpt_path.c_str(), nullptr, nullptr) != 0) {
        cleanup();
        return out;
    }
    // PR H11: the off switch must be flipped in the open()->initialize()
    // window -- buildROM1D() (which copies the engine's phase config into
    // the ROM's own copy) runs inside initialize(). Inert when with_rom is
    // false (no [UNCERTAINTY] section, no ROM ever built).
    if (!phase_enabled) {
        static_cast<openswmm::SWMMEngine*>(handle)->rom1dPhaseConfig().enabled = false;
    }
    if (swmm_engine_initialize(handle) != 0 ||
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
        const double t_now = (elapsed > 0.0) ? elapsed * 86400.0 : end_time_s;
        stalled = (t_now - t_prev < 1e-9) ? stalled + 1 : 0;
        if (stalled > 200) break;   // no time progress — bail with what we have

        // Report boundaries crossed in (t_prev, t_now]
        for (double b = std::floor(t_prev / report_step_s + 1.0) * report_step_s;
             b <= t_now + 1e-6; b += report_step_s) {
            if (b > end_time_s + 1e-6) break;
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
        if (t_now >= end_time_s) break;
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

// ============================================================================
// PR H5 — surcharged-regime sensitivity attenuation: the gate that makes the
// design real. VALIDATION.md (reform PR 10) documented the ROM over-predicting
// band widths 50-190x in surcharged regime on an earlier variant of this same
// topology; H5's alpha ramp (RomSurchargeAttenuation.hpp) is meant to close
// that gap. This is the closed-loop check: does it actually land in-band
// against brute-force MC, not just internally consistent with itself.
//
// Fixture: SAME 5-junction chain as the free-surface test above, but with a
// LOCALIZED bottleneck: C1-C4 stay at the generous 1.0 m diameter (never the
// limiting factor), C5 (the last conduit, into the outfall) is undersized to
// 0.3 m, and DWF at J1 is raised to 0.40 CMS. MaxDepth is raised to 30 m
// (mechanically inert here -- see below -- kept generous defensively).
//
// This design REPLACES an earlier attempt (uniformly undersized 1.0m-or-0.3m
// pipes on EVERY conduit, DWF pushed well past chain-wide capacity) that had
// to be abandoned after extensive scratch-harness measurement (2026-08-06)
// found it was NOT a validatable fixture:
//   - Compounding a bottleneck through all 5 conduits made surcharge depth
//     swing from "doesn't reach crown" to ">100 m over crown" across the
//     checklist's own +-20% Manning prior -- a genuine near-bifurcation in
//     capacity-vs-demand (rough=0.83 stayed BELOW crown at the same DWF that
//     put rough=1.17 tens of meters over it), not a fixture bug.
//   - MC members landing on opposite sides of that bifurcation don't have a
//     comparable "band" to measure a ROM against at all.
// Localizing the bottleneck to ONE conduit turns that chain-wide compounding
// into a single-stage, continuously-graded response: measured (scratch
// diagnostic, 2026-08-06) max(head-crown) at t=3600s across the prior's
// rough_mult in {0.83, 1.0, 1.17} is {19.2, 31.6, 47.0} m under EXPLICIT and
// {14.5, 21.2, 27.9} m under SEMI_IMPLICIT -- smooth and monotonic in
// roughness, no jump, cross-mode ratio ~1.5x (not the >100x the P4 compat
// matrix hit when a fixture straddles a flooding TRANSITION -- history_
// decisions.md, "P4 compat matrix: first fixture attempt flooded the
// network"). Confirmed via a MaxDepth sweep (15/30/60/100 m, all identical
// results) that these are genuine backwater equilibria, not a MaxDepth/
// ponding ceiling artifact -- MaxDepth=30 here is pure headroom, not a lever.
// Surcharge fraction of the window: 90-95% across the same rough_mult range,
// comfortably past the checklist's >=50% floor, for every case measured.
// ============================================================================

constexpr double kSurchargeDwf      = 0.40;  // CMS -- see topology note above
constexpr double kSurchargeMaxDepth = 30.0;  // m (headroom only, not a lever)

std::string fixtureInpSurcharged(double rough_mult, bool with_rom,
                                  bool node_continuity_semi) {
    char rough[32];
    std::snprintf(rough, sizeof(rough), "%.9f", kBaseRoughness * rough_mult);
    char dwf[32];
    std::snprintf(dwf, sizeof(dwf), "%.4f", kSurchargeDwf);
    char maxd[16];
    std::snprintf(maxd, sizeof(maxd), "%.2f", kSurchargeMaxDepth);

    std::string inp = R"([TITLE]
ROM vs MC coverage validation, surcharged regime (PR H5)

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
)";
    // Robust in-place injection: appended while the [OPTIONS] block is still
    // being built, never a search-and-insert into an already-assembled file
    // (the fragile pattern that silently no-op'd on a real-world .inp during
    // P4's own Bellinge rerun -- see the "OPTIONS keys must be inserted
    // inside the actual [OPTIONS] block" lesson in .memory/current.md).
    inp += std::string("NODE_CONTINUITY      ") +
           (node_continuity_semi ? "SEMI_IMPLICIT\n" : "EXPLICIT\n");

    inp += R"(
[JUNCTIONS]
;;Name  Elevation  MaxDepth  InitDepth  SurDepth  Aponded
J1      100        MAXD       0.10       0         0
J2       95        MAXD       0.10       0         0
J3       90        MAXD       0.10       0         0
J4       85        MAXD       0.10       0         0
J5       80        MAXD       0.10       0         0

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
C5      CIRCULAR 0.3    0      0      0      1

[DWF]
;;Node  Constituent  Baseline  Patterns
J1      FLOW         DWFBASE

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
    std::string::size_type pos;
    while ((pos = inp.find("ROUGH")) != std::string::npos)
        inp.replace(pos, 5, rough);
    while ((pos = inp.find("MAXD")) != std::string::npos)
        inp.replace(pos, 4, maxd);
    while ((pos = inp.find("DWFBASE")) != std::string::npos)
        inp.replace(pos, 7, dwf);
    return inp;
}

// Crown elevation per junction: invert + conduit diameter (1.0 m), the same
// closed form buildRom1dThresholds() uses -- every junction here connects to
// a 1.0 m CIRCULAR conduit, so this is exact, not approximate.
double crownElevOf(const std::string& node_name) {
    static const std::map<std::string, double> kInvert = {
        {"J1", 100.0}, {"J2", 95.0}, {"J3", 90.0}, {"J4", 85.0}, {"J5", 80.0},
    };
    return kInvert.at(node_name) + 1.0;
}

struct SurchargeCellResult {
    bool   ok           = false;
    double coverage      = 0.0;
    double ratio_min     = 1e300, ratio_med = 0.0, ratio_max = 0.0;
    double surcharged_frac = 0.0;
    int    n_total = 0, n_width = 0;
};

// Runs the same kMcRuns brute-force MC design as the free-surface test above
// (identical LHS strata, identical ROM perturbation) against the surcharged
// fixture, under one NODE_CONTINUITY mode. MC members and the ROM run the
// SAME mode (the P4 compat-matrix pattern) -- comparing a mode-A ROM against
// mode-B brute force would not be a fair test of either.
SurchargeCellResult runSurchargedCell(bool node_continuity_semi) {
    SurchargeCellResult res;

    std::vector<RunResult> mc(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i) {
        const double mult =
            (1.0 - kPert) + (static_cast<double>(i) + 0.5) / kMcRuns * 2.0 * kPert;
        const std::string tag =
            (node_continuity_semi ? "sc_semi_mc" : "sc_expl_mc") + std::to_string(i);
        mc[static_cast<std::size_t>(i)] = runCase(
            fixtureInpSurcharged(mult, /*with_rom=*/false, node_continuity_semi),
            tag.c_str(), false);
        if (!mc[static_cast<std::size_t>(i)].ok) return res;
    }

    RunResult rom = runCase(
        fixtureInpSurcharged(1.0, /*with_rom=*/true, node_continuity_semi),
        node_continuity_semi ? "sc_semi_rom" : "sc_expl_rom", true);
    if (!rom.ok || rom.q05.empty()) return res;

    std::size_t n_samples = rom.times.size();
    for (int i = 0; i < kMcRuns; ++i)
        n_samples = std::min(n_samples, mc[static_cast<std::size_t>(i)].times.size());
    if (n_samples == 0) return res;
    for (const auto& [nm, _] : rom.q05)
        if (rom.q05.at(nm).size() < n_samples) return res;

    int n_total = 0, n_covered = 0, n_width = 0, n_width_ok = 0;
    int n_surcharge_samples = 0, n_surcharge_hit = 0;
    double ratio_min = 1e300, ratio_max = 0.0;
    std::vector<double> ratios;

    for (const auto& [nm, rom_q05] : rom.q05) {
        const auto& rom_q95 = rom.q95.at(nm);
        const double crown = crownElevOf(nm);
        for (std::size_t k = 0; k < n_samples; ++k) {
            if (rom.times[k] <= 60.0) continue;
            const bool late = rom.times[k] >= 0.5 * kEndTime;  // same spin-up rationale as above

            std::vector<double> h(kMcRuns);
            for (int i = 0; i < kMcRuns; ++i)
                h[static_cast<std::size_t>(i)] = mc[static_cast<std::size_t>(i)].heads.at(nm)[k];
            std::sort(h.begin(), h.end());
            const double mc_q50   = h[10];
            const double mc_width = h[19] - h[1];

            ++n_total;
            if (rom_q05[k] <= mc_q50 && mc_q50 <= rom_q95[k]) ++n_covered;

            ++n_surcharge_samples;
            if (rom.heads.at(nm)[k] > crown) ++n_surcharge_hit;

            if (late && mc_width > 1e-6) {
                const double ratio = (rom_q95[k] - rom_q05[k]) / mc_width;
                ++n_width;
                ratios.push_back(ratio);
                ratio_min = std::min(ratio_min, ratio);
                ratio_max = std::max(ratio_max, ratio);
                if (ratio >= 0.3 && ratio <= 3.0) ++n_width_ok;
            }
        }
    }
    if (n_total == 0 || n_width == 0) return res;

    std::sort(ratios.begin(), ratios.end());
    res.ok              = true;
    res.coverage         = static_cast<double>(n_covered) / n_total;
    res.ratio_min        = ratio_min;
    res.ratio_med        = ratios[ratios.size() / 2];
    res.ratio_max        = ratio_max;
    res.surcharged_frac  = static_cast<double>(n_surcharge_hit) / n_surcharge_samples;
    res.n_total          = n_total;
    res.n_width          = n_width;
    (void)n_width_ok;
    return res;
}

void assertSurchargedCell(const char* label, const SurchargeCellResult& r) {
    ASSERT_TRUE(r.ok) << label << ": a run failed or produced no comparable samples";
    std::printf("[ROM-vs-MC SURCHARGED %s] samples=%d  coverage=%.3f  "
                "width-ratio min/med/max = %.3f / %.3f / %.3f  "
                "surcharged_frac=%.3f  (n_width=%d)\n",
                label, r.n_total, r.coverage, r.ratio_min, r.ratio_med,
                r.ratio_max, r.surcharged_frac, r.n_width);

    // The regime precondition itself: this fixture must actually be
    // surcharged for most of the window, or the band-width comparison below
    // isn't testing what it claims to.
    EXPECT_GE(r.surcharged_frac, 0.50)
        << label << ": fixture must be surcharged for >=50% of the window "
        << "(measured " << r.surcharged_frac << ") -- H5's whole premise is "
        << "attenuation IN surcharge, not in a fixture that never reaches it";

    // H5's acceptance bounds (HSYM_RESIDUALS_PR_CHECKLIST.md, PR H5):
    // coverage >= 0.90, width-ratio median in [0.3, 3.0]. Per standing rule
    // 1.2 ("nobody tunes a tolerance to green a spread-magnitude test"),
    // these are the checklist's own numbers, not adjusted from what was
    // measured while writing this test.
    EXPECT_GE(r.coverage, 0.90)
        << label << ": ROM [q05,q95] must contain the MC median at >=90% "
        << "of samples in the surcharged regime";
    EXPECT_GE(r.ratio_med, 0.3)
        << label << ": median width ratio must not be more than ~3x too "
        << "narrow -- if it is, that's the pre-H5 defect, not a tuning target";
    EXPECT_LE(r.ratio_med, 3.0)
        << label << ": median width ratio must not be more than ~3x too "
        << "wide -- over-attenuation is also a formulation gap, not noise";
}

// @warning STATUS 2026-08-06 -- DELIBERATELY RED, root-caused not tuned away.
// Measured: coverage=0.997, width-ratio min/med/max = 0.016/0.034/1.866. The
// SEMI_IMPLICIT cell below (same fixture, same alpha_floor) passes cleanly at
// ratio_med=1.031 -- this is EXPLICIT-specific, not a generic H5 shortfall.
//
// ROOT CAUSE (per-node breakdown, scratch diagnostic 2026-08-06): the pooled
// median is dragged down by J4/J5 specifically (the two nodes downstream of
// the C5 bottleneck, where alpha attenuates to the floor) -- ratio ~0.02 at
// both, vs J1 (never attenuated) at ratio ~1.87, comfortably in-band. At
// J4/J5 the brute-force MC's OWN q05-q95 width is ~28 m: under EXPLICIT's
// discrete surcharge branch, the +-20% Manning prior alone moves the settled
// backwater depth at the bottleneck from ~19 m to ~47 m over crown (measured
// directly, not inferred) -- a real, steep, physical sensitivity of
// pressurized single-conduit backwater to conveyance near a chokepoint, not
// a fixture artifact (confirmed monotonic and MaxDepth-independent when this
// fixture was designed; see the topology note above). H5's alpha_floor
// cannot bridge a gap this size without floor values (~0.7-0.8, measured by
// direct trial) that erase most of the attenuation this PR exists to add --
// at floor=0.15 SEMI_IMPLICIT's own ratio_max (currently 2.799) would clear
// the checklist's 3.0 ceiling, so a single global floor cannot satisfy both
// cells on this fixture.
//
// Left RED rather than loosening the checklist's own [0.3,3.0]/>=0.90 bounds
// (hard rule 2) or picking a floor that only passes one cell by accident.
//
// RESOLVED 2026-08-09: checked whether the gap is fixture-severity-dependent
// before accepting it as a limitation -- swept DWF 0.25-0.40 CMS and found a
// MILDER surcharge does NOT narrow EXPLICIT's dynamic range; near threshold
// the ratio of extremes gets WORSE (7.5x at DWF=0.30 vs 2.4x at the shipped
// DWF=0.40) because the low-roughness member's settled depth shrinks toward
// zero, not because the physical spread narrows. DWF=0.40 is the
// best-behaved point measured, not an unlucky pick. **Decision: accepted as
// a documented limitation of source-side attenuation under
// NODE_CONTINUITY EXPLICIT specifically** (not a bug, not fixable by
// re-tuning this fixture or alpha_floor) -- see
// docs/uncertainty/HOW_IT_WORKS.md §8 and USER_GUIDE.md limitation 10, both
// of which now recommend NODE_CONTINUITY SEMI_IMPLICIT when validated
// surcharged bands are needed. This test stays registered and red as a live
// regression guard, and as the ready-made gate if a future session pursues
// the flow/Froude-keyed attenuation signal option (reusing H3's
// link_froude) noted as the remaining real alternative in VALIDATION.md.
// See VALIDATION.md §3(d) and history_decisions.md for the full record.
TEST(RomCoverageSurcharged, ExplicitContinuity) {
    assertSurchargedCell("EXPLICIT", runSurchargedCell(/*node_continuity_semi=*/false));
}

TEST(RomCoverageSurcharged, SemiImplicitContinuity) {
    assertSurchargedCell("SEMI_IMPLICIT", runSurchargedCell(/*node_continuity_semi=*/true));
}

// ============================================================================
// PR H11 — per-member phase coordinate: the front-passage gate. VALIDATION.md
// (reform PR 10) documented an UNREPRESENTED 2-4 m transient width at front
// passage -- the ROM's amplitude channel is anchored to the deterministic
// trajectory and carries no timing/phase information at all. H11's
// tau_i(x) = (mm_i-1)*T̄(x) travel-time shift (RomPhaseCoordinate.hpp) is
// meant to close that gap. This is the closed-loop check: does it actually
// land in-band against brute-force MC during a genuine front-passage
// transient, not just internally consistent with itself (test_spectral_
// rom1d.cpp's PhaseCoordinate suite) or plumbed correctly (test_engine_1d_
// rom_lifecycle.cpp's travel-time tests).
//
// Fixture: SAME 5-junction chain topology as the free-surface test above,
// but LONG conduits (800 m each, vs the free-surface fixture's 100 m) so a
// filling front's arrival time at the downstream nodes is spread over
// multiple minutes -- resolvable at REPORT_STEP=60s -- instead of arriving
// within a single routing step. InitDepth=0 (genuinely dry start, not the
// free-surface fixture's InitDepth=0.10) so there IS a front to pass. A
// single generous 1.0 m CIRCULAR conduit everywhere keeps the whole run
// free-surface (H5's alpha stays ~1 throughout), isolating the phase
// channel from the attenuation channel H5 already validates separately.
//
// VERIFIED by scratch probe (2026-08-10, not asserted a priori) before
// trusting this topology, per this project's own standing practice after
// several abandoned fixture designs elsewhere in this file and in H5's
// history (see the topology note on fixtureInpSurcharged above, and the
// P5 "still water isn't still" trap in .memory/history_failures.md):
// at DWF=0.15 CMS, L=800 m/conduit, MaxDepth=8 m, measured half-rise
// arrival time (t_half) at rough_mult in {0.8, 1.0, 1.2}:
//   J4: {1350.5, 1590.5, 1800.5} s -- range 450 s (7.5 report steps)
//   J5: {1830.5, 2160.5, 2430.5} s -- range 600 s (10.0 report steps)
// comfortably clearing the "front-arrival time must differ by >=2 report
// steps" bar. head stayed 2.5-2.7 ft below crown at every node throughout
// the 2-hour window at every rough_mult tried -- never surcharges.
constexpr double kFrontEndTime  = 7200.0;  // s (2 h) -- see the probe note above
constexpr double kFrontDwf      = 0.15;    // CMS
constexpr double kFrontLength   = 800.0;   // m per conduit
constexpr double kFrontMaxDepth = 8.0;     // m

std::string fixtureInpFront(double rough_mult, bool with_rom) {
    char rough[32];
    std::snprintf(rough, sizeof(rough), "%.9f", kBaseRoughness * rough_mult);
    char len[32];
    std::snprintf(len, sizeof(len), "%.2f", kFrontLength);
    char dwf[32];
    std::snprintf(dwf, sizeof(dwf), "%.4f", kFrontDwf);
    char maxd[16];
    std::snprintf(maxd, sizeof(maxd), "%.2f", kFrontMaxDepth);

    std::string inp = R"([TITLE]
ROM vs MC coverage validation, front-passage regime (PR H11)

[OPTIONS]
FLOW_UNITS           CMS
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2025
START_TIME           00:00:00
REPORT_START_DATE    01/01/2025
REPORT_START_TIME    00:00:00
END_DATE             01/01/2025
END_TIME             02:00:00
REPORT_STEP          00:01:00
ROUTING_STEP         0:00:30
MIN_SURFAREA         1.0
MAX_TRIALS           8
HEAD_TOLERANCE       0.005
MINIMUM_STEP         0.5
THREADS              1

[JUNCTIONS]
;;Name  Elevation  MaxDepth  InitDepth  SurDepth  Aponded
J1      100        MAXD       0.0        0         0
J2       95        MAXD       0.0        0         0
J3       90        MAXD       0.0        0         0
J4       85        MAXD       0.0        0         0
J5       80        MAXD       0.0        0         0

[OUTFALLS]
;;Name  Elevation  Type  Stage  Gated
O1      75         FREE         NO

[CONDUITS]
;;Name  From  To   Length  Roughness  InOffset  OutOffset  InitFlow  MaxFlow
C1      J1    J2   LEN     ROUGH      0         0          0         0
C2      J2    J3   LEN     ROUGH      0         0          0         0
C3      J3    J4   LEN     ROUGH      0         0          0         0
C4      J4    J5   LEN     ROUGH      0         0          0         0
C5      J5    O1   LEN     ROUGH      0         0          0         0

[XSECTIONS]
;;Link  Shape    Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR 1.0    0      0      0      1
C2      CIRCULAR 1.0    0      0      0      1
C3      CIRCULAR 1.0    0      0      0      1
C4      CIRCULAR 1.0    0      0      0      1
C5      CIRCULAR 1.0    0      0      0      1

[DWF]
;;Node  Constituent  Baseline  Patterns
J1      FLOW         DWFBASE

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
    std::string::size_type pos;
    while ((pos = inp.find("ROUGH")) != std::string::npos)
        inp.replace(pos, std::strlen("ROUGH"), rough);
    while ((pos = inp.find("LEN")) != std::string::npos)
        inp.replace(pos, std::strlen("LEN"), len);
    while ((pos = inp.find("MAXD")) != std::string::npos)
        inp.replace(pos, std::strlen("MAXD"), maxd);
    while ((pos = inp.find("DWFBASE")) != std::string::npos)
        inp.replace(pos, std::strlen("DWFBASE"), dwf);
    return inp;
}

/// Front-passage sample selection (design §4D): from a node's own
/// deterministic head series (h, length >= n_samples), find t_50 -- the
/// first report time at which h crosses the midpoint of its [min, max]
/// range over the window -- then return the indices of every report time
/// within +-3*kReportStep of t_50. Empty when the series never crosses its
/// own midpoint (should not happen on a genuine front-passage fixture).
std::vector<std::size_t> frontWindowIndices(const std::vector<double>& times,
                                             const std::vector<double>& h,
                                             std::size_t n_samples) {
    std::vector<std::size_t> idx;
    if (n_samples == 0) return idx;
    double hmin = h[0], hmax = h[0];
    for (std::size_t i = 0; i < n_samples; ++i) {
        hmin = std::min(hmin, h[i]);
        hmax = std::max(hmax, h[i]);
    }
    const double half = hmin + 0.5 * (hmax - hmin);
    double t50 = -1.0;
    for (std::size_t i = 0; i < n_samples; ++i) {
        if (h[i] >= half) { t50 = times[i]; break; }
    }
    if (t50 < 0.0) return idx;
    for (std::size_t i = 0; i < n_samples; ++i) {
        if (std::fabs(times[i] - t50) <= 3.0 * kReportStep + 1e-6) idx.push_back(i);
    }
    return idx;
}

struct FrontCellResult {
    bool   ok        = false;
    double coverage   = 0.0;
    double ratio_min  = 1e300, ratio_med = 0.0, ratio_max = 0.0;
    int    n_total = 0, n_width = 0;
};

/// Runs the same kMcRuns brute-force MC design as the free-surface test
/// above (identical LHS strata, identical ROM perturbation) against the
/// front-passage fixture, then compares ONLY at each node's own
/// front-passage window (frontWindowIndices). @p phase_enabled toggles
/// SpectralROM1D::phase_cfg.enabled on the ROM run via runCase()'s
/// open()->initialize()-window hook -- MC runs never build a ROM at all, so
/// they are unaffected and are re-run per cell for the same self-contained-
/// per-cell structure runSurchargedCell() above already established.
FrontCellResult runFrontCell(bool phase_enabled) {
    FrontCellResult res;

    std::vector<RunResult> mc(kMcRuns);
    for (int i = 0; i < kMcRuns; ++i) {
        const double mult =
            (1.0 - kPert) + (static_cast<double>(i) + 0.5) / kMcRuns * 2.0 * kPert;
        const std::string tag =
            (phase_enabled ? "front_phase_mc" : "front_base_mc") + std::to_string(i);
        mc[static_cast<std::size_t>(i)] = runCase(
            fixtureInpFront(mult, /*with_rom=*/false), tag.c_str(), false,
            kFrontEndTime, kReportStep, /*phase_enabled=*/true);
        if (!mc[static_cast<std::size_t>(i)].ok) return res;
    }

    RunResult rom = runCase(
        fixtureInpFront(1.0, /*with_rom=*/true),
        phase_enabled ? "front_phase_rom" : "front_base_rom", true,
        kFrontEndTime, kReportStep, phase_enabled);
    if (!rom.ok || rom.q05.empty()) return res;

    std::size_t n_samples = rom.times.size();
    for (int i = 0; i < kMcRuns; ++i)
        n_samples = std::min(n_samples, mc[static_cast<std::size_t>(i)].times.size());
    if (n_samples == 0) return res;
    for (const auto& [nm, _] : rom.q05)
        if (rom.q05.at(nm).size() < n_samples) return res;

    int n_total = 0, n_covered = 0, n_width = 0, n_width_ok = 0;
    double ratio_min = 1e300, ratio_max = 0.0;
    std::vector<double> ratios;

    for (const auto& [nm, rom_q05] : rom.q05) {
        const auto& rom_q95    = rom.q95.at(nm);
        const auto& rom_series = rom.heads.at(nm);  // ROM run's own deterministic trajectory
        const auto idx = frontWindowIndices(rom.times, rom_series, n_samples);
        if (idx.size() < 3) continue;  // no resolvable front at this node

        for (std::size_t k : idx) {
            std::vector<double> h(kMcRuns);
            for (int i = 0; i < kMcRuns; ++i)
                h[static_cast<std::size_t>(i)] = mc[static_cast<std::size_t>(i)].heads.at(nm)[k];
            std::sort(h.begin(), h.end());
            const double mc_q50   = h[10];
            const double mc_width = h[19] - h[1];

            ++n_total;
            if (rom_q05[k] <= mc_q50 && mc_q50 <= rom_q95[k]) ++n_covered;

            if (mc_width > 1e-6) {
                const double ratio = (rom_q95[k] - rom_q05[k]) / mc_width;
                ++n_width;
                ratios.push_back(ratio);
                ratio_min = std::min(ratio_min, ratio);
                ratio_max = std::max(ratio_max, ratio);
                if (ratio >= 0.5 && ratio <= 2.0) ++n_width_ok;
            }
        }
    }
    if (n_total == 0 || n_width == 0) return res;

    std::sort(ratios.begin(), ratios.end());
    res.ok       = true;
    res.coverage = static_cast<double>(n_covered) / n_total;
    res.ratio_min = ratio_min;
    res.ratio_med = ratios[ratios.size() / 2];
    res.ratio_max = ratio_max;
    res.n_total  = n_total;
    res.n_width  = n_width;
    (void)n_width_ok;
    return res;
}

TEST(RomCoverageFront, PhaseCoordinate) {
    const FrontCellResult r = runFrontCell(/*phase_enabled=*/true);
    ASSERT_TRUE(r.ok) << "a run failed or produced no comparable front-passage samples";
    std::printf("[ROM-vs-MC FRONT PhaseCoordinate] samples=%d  coverage=%.3f  "
                "width-ratio min/med/max = %.3f / %.3f / %.3f  (n_width=%d)\n",
                r.n_total, r.coverage, r.ratio_min, r.ratio_med, r.ratio_max, r.n_width);

    // H11's acceptance bounds (HSYM_RESIDUALS_PR_CHECKLIST.md, PR H11):
    // coverage >= 0.90, width-ratio median in [0.5, 2.0]. Per standing rule
    // 1.2, these are the checklist's own numbers, not adjusted from what was
    // measured while writing this test. Per hard rule 2, if this does not
    // pass, it stays RED with a root-caused @warning block -- never loosen
    // these bounds or reshape the fixture to force green (see §5 of the H11
    // design plan).
    EXPECT_GE(r.coverage, 0.90)
        << "ROM [q05,q95] must contain the MC median at >=90% of "
        << "front-passage samples";
    EXPECT_GE(r.ratio_med, 0.5)
        << "median width ratio must not be more than 2x too narrow during "
        << "front passage -- the timing spread H11 exists to recover";
    EXPECT_LE(r.ratio_med, 2.0)
        << "median width ratio must not be more than 2x too wide -- "
        << "over-recovery is also a formulation gap, not noise";
}

TEST(RomCoverageFront, AmplitudeOnlyBaseline) {
    // The SAME fixture and MC design as PhaseCoordinate above, but with
    // SpectralROM1D::phase_cfg.enabled forced false -- the pre-H11 amplitude-
    // only ROM. No acceptance bounds: this is the "before" measurement
    // VALIDATION.md needs to state HOW MUCH of the front-passage width gap
    // H11 actually recovers, not a pass/fail gate in its own right.
    const FrontCellResult r = runFrontCell(/*phase_enabled=*/false);
    ASSERT_TRUE(r.ok) << "a run failed or produced no comparable front-passage samples";
    std::printf("[ROM-vs-MC FRONT AmplitudeOnlyBaseline] samples=%d  coverage=%.3f  "
                "width-ratio min/med/max = %.3f / %.3f / %.3f  (n_width=%d)\n",
                r.n_total, r.coverage, r.ratio_min, r.ratio_med, r.ratio_max, r.n_width);
}
