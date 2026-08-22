// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file bench_xsect_eval.cpp
 * @brief Microbenchmark of the IRREGULAR cross-section evaluation chain on
 *        REAL transect tables (plans/XSECT_LOOKUP_ACCEL_PLAN.md, phase 0).
 *
 * @details Opens one or more decks through the real engine, builds the same
 *          XSectParams the routing paths use (link::buildXSectParams, table
 *          pointers into ctx.transect_tables), and times each evaluator the
 *          way the solvers call it:
 *
 *            fwd_AofY / fwd_WofY / fwd_RofY   uniform-grid lookup (lookup_exact
 *                                             via XsectEval — the scalar/FV form)
 *            inv_YofA                         binary-search invLookup
 *            SofA                             a * RofA^(2/3) (one inv + one fwd + pow)
 *            newton_AofS                      the full Newton chain (getSofA +
 *                                             central-difference dSdA per iter)
 *            brent_depthOfArea                faithful replica of the FV
 *                                             inversion (FvKernels.hpp:316):
 *                                             bisection/secant on AofY with the
 *                                             same 1e-10*a_full exit, so the
 *                                             3-6 forward evals per call are
 *                                             reproduced (documented replica,
 *                                             not the shipped symbol)
 *            gather_area                      the batch perlink_tabulated
 *                                             shape: per-element table-pointer
 *                                             gather then one area lookup
 *            batch_AR_2pass / batch_AR_fused  the DW STEP-D shape: area and
 *                                             hyd-radius over a whole group,
 *                                             as two perlink_tabulated passes
 *                                             (pre-A2) vs one fused pass
 *            inv_YofA_bisect                  inv_YofA with the A1 bucket maps
 *                                             unbound — the pre-A1 locate()
 *
 *          The `*_bisect` / `*_2pass` variants are the PRE-change code paths,
 *          kept compiled here so each accelerated op reports against its own
 *          baseline in the same binary and the same run.
 *
 *          Output is CSV on stdout: deck,op,calls,reps,ns_med,ns_min,ns_max.
 *          Median over kReps repetitions of the whole input sweep; a volatile
 *          sink defeats dead-code elimination.
 *
 *          --dump-tables <dir> instead writes one CSV per transect (51-row
 *          area/hrad/width tables + full-depth properties + the RAW store
 *          stations/elevations), for offline analysis of the engine-built
 *          tables (a Python reimplementation of buildTables would not do: the
 *          hrad conveyance back-solve and legacy dy accumulation are parity
 *          traps).
 *
 * Usage:
 *   bench_xsect_eval deck1.inp [deck2.inp ...]
 *   bench_xsect_eval --dump-tables out_dir deck1.inp [deck2.inp ...]
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/engine/core/SWMMEngine.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/hydraulics/Link.hpp"
#include "../../src/engine/hydraulics/XSectBatch.hpp"
#include "../../src/engine/hydraulics/XSectKernels.hpp"

namespace {

using openswmm::SimulationContext;
using openswmm::SWMMEngine;
using openswmm::XSectParams;
using openswmm::xsect::XsectEval;

volatile double g_sink = 0.0;   // defeats DCE across every timed loop

constexpr int kReps = 9;        // repetitions of the whole sweep; median reported

struct Timed {
    double ns_med, ns_min, ns_max;
};

/// Time fn() (one full sweep of `calls` evaluator invocations) kReps times.
template <class F>
Timed time_sweep(std::size_t calls, F&& fn) {
    std::vector<double> per_call(kReps);
    for (int r = 0; r < kReps; ++r) {
        const auto t0 = std::chrono::steady_clock::now();
        fn();
        const auto t1 = std::chrono::steady_clock::now();
        per_call[static_cast<std::size_t>(r)] =
            std::chrono::duration<double, std::nano>(t1 - t0).count() /
            static_cast<double>(calls);
    }
    std::sort(per_call.begin(), per_call.end());
    return {per_call[kReps / 2], per_call.front(), per_call.back()};
}

void report(const std::string& deck, const char* op, std::size_t calls, Timed t) {
    std::printf("%s,%s,%zu,%d,%.2f,%.2f,%.2f\n",
                deck.c_str(), op, calls, kReps, t.ns_med, t.ns_min, t.ns_max);
}

/// Faithful replica of FvKernels.hpp depthOfArea: bracketed bisection/secant
/// on f(y) = AofY(y) - a with the FV exit tolerance (|f| <= 1e-10 * a_full or
/// 60 iterations). Kept here so the bench does not need FvGeometry plumbing;
/// the iteration count and per-iteration cost (one forward area lookup) match
/// the shipped inversion's 3-6 evals per call on smooth sections.
double brent_depth_of_area(const XsectEval& ev, const XSectParams& xs, double a) {
    if (a <= 0.0) return 0.0;
    if (a >= xs.a_full) return xs.y_full;
    double lo = 0.0, hi = xs.y_full;
    double flo = -a, fhi = xs.a_full - a;
    double y = hi * (a / xs.a_full);   // proportional first guess
    const double tol = 1e-10 * xs.a_full;
    for (int it = 0; it < 60; ++it) {
        const double f = ev.getAofY(xs, y) - a;
        if (std::fabs(f) <= tol) return y;
        if (f > 0.0) { hi = y; fhi = f; }
        else         { lo = y; flo = f; }
        // secant step, bisection fallback (Brent-lite, same eval count regime)
        const double denom = fhi - flo;
        double ynew = (denom != 0.0) ? (lo * fhi - hi * flo) / denom
                                     : 0.5 * (lo + hi);
        if (!(ynew > lo && ynew < hi)) ynew = 0.5 * (lo + hi);
        y = ynew;
    }
    return y;
}

int dump_tables(const SimulationContext& ctx, const std::string& deck_stem,
                const std::filesystem::path& out_dir) {
    std::filesystem::create_directories(out_dir);
    const int nt = ctx.transects.count();
    for (int t = 0; t < nt; ++t) {
        const auto ut = static_cast<std::size_t>(t);
        if (ut >= ctx.transect_tables.size()) break;
        const auto& td = ctx.transect_tables[ut];
        const auto path = out_dir / (deck_stem + "_" + td.name + ".csv");
        FILE* f = std::fopen(path.string().c_str(), "w");
        if (!f) { std::fprintf(stderr, "cannot write %s\n", path.string().c_str()); return 1; }
        std::fprintf(f, "# transect,%s\n# deck,%s\n", td.name.c_str(), deck_stem.c_str());
        std::fprintf(f, "# y_full,%.17g\n# a_full,%.17g\n# r_full,%.17g\n# w_max,%.17g\n",
                     td.y_full, td.a_full, td.r_full, td.w_max);
        std::fprintf(f, "# s_max,%.17g\n# a_max,%.17g\n", td.s_max, td.a_max);
        std::fprintf(f, "# x_left_bank,%.17g\n# x_right_bank,%.17g\n",
                     td.x_left_bank, td.x_right_bank);
        std::fprintf(f, "# n_left,%.17g\n# n_right,%.17g\n# n_channel,%.17g\n",
                     td.n_left, td.n_right, td.n_channel);
        // RAW store data (display units) — what [TRANSECTS] round-trips.
        std::fprintf(f, "# raw_stations");
        for (double s : ctx.transects.stations[ut]) std::fprintf(f, ",%.17g", s);
        std::fprintf(f, "\n# raw_elevations");
        for (double e : ctx.transects.elevations[ut]) std::fprintf(f, ",%.17g", e);
        std::fprintf(f, "\nidx,area_norm,hrad_norm,width_norm\n");
        for (int i = 0; i < openswmm::transect::N_TRANSECT_TBL; ++i)
            std::fprintf(f, "%d,%.17g,%.17g,%.17g\n",
                         i, td.area_tbl[i], td.hrad_tbl[i], td.width_tbl[i]);
        std::fclose(f);
    }
    std::fprintf(stderr, "%s: dumped %d transect tables to %s\n",
                 deck_stem.c_str(), nt, out_dir.string().c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string dump_dir;
    std::vector<std::string> decks;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--dump-tables") == 0 && i + 1 < argc)
            dump_dir = argv[++i];
        else
            decks.push_back(argv[i]);
    }
    if (decks.empty()) {
        std::fprintf(stderr,
                     "usage: %s [--dump-tables out_dir] deck1.inp [...]\n", argv[0]);
        return 2;
    }

    if (dump_dir.empty())
        std::printf("deck,op,calls,reps,ns_med,ns_min,ns_max\n");

    for (const auto& deck : decks) {
        SWMMEngine eng;
        const std::string rpt = deck + ".benchrpt";
        if (eng.open(deck.c_str(), rpt.c_str(), nullptr) != 0) {
            std::fprintf(stderr, "open(%s) failed — skipped\n", deck.c_str());
            continue;
        }
        auto& ctx = eng.context();
        const std::string stem = std::filesystem::path(deck).stem().string();

        if (!dump_dir.empty()) {
            const int rc = dump_tables(ctx, stem, dump_dir);
            if (rc != 0) return rc;
            continue;
        }

        // XSectParams for every IRREGULAR link — identical to what DW/FV use.
        // xsect_batch_shape is only populated at initialize() (SWMMEngine.cpp
        // q0 note), so translate it here the same way before building params.
        std::vector<XSectParams> xss;
        for (int j = 0; j < ctx.n_links(); ++j) {
            const auto uj = static_cast<std::size_t>(j);
            if (ctx.links.xsect_shape[uj] != openswmm::XsectShape::IRREGULAR)
                continue;
            ctx.links.xsect_batch_shape[uj] =
                openswmm::link::translateShape(ctx.links.xsect_shape[uj]);
            XSectParams xs =
                openswmm::link::buildXSectParams(ctx.links, uj, &ctx.transect_tables);
            if (xs.area_tbl && xs.a_full > 0.0 && xs.y_full > 0.0)
                xss.push_back(xs);
        }
        if (xss.empty()) {
            std::fprintf(stderr, "%s: no IRREGULAR links — skipped\n", stem.c_str());
            continue;
        }

        const XsectEval& ev = openswmm::xsect::hostEval();

        // Deterministic input sweeps: kSamples fractions per link, avoiding the
        // exact endpoints (0 and full are early-out branches, not the lookup).
        constexpr int kSamples = 64;
        std::vector<double> frac(kSamples);
        for (int i = 0; i < kSamples; ++i)
            frac[static_cast<std::size_t>(i)] = (i + 0.5) / kSamples;

        const std::size_t calls = xss.size() * static_cast<std::size_t>(kSamples);

        const auto sweep = [&](auto&& per_call) {
            double acc = 0.0;
            for (const auto& xs : xss)
                for (double fr : frac) acc += per_call(xs, fr);
            g_sink = g_sink + acc;
        };

        report(stem, "fwd_AofY", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return ev.getAofY(xs, fr * xs.y_full); });
        }));
        report(stem, "fwd_WofY", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return ev.getWofY(xs, fr * xs.y_full); });
        }));
        report(stem, "fwd_RofY", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return ev.getRofY(xs, fr * xs.y_full); });
        }));
        report(stem, "inv_YofA", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return ev.getYofA(xs, fr * xs.a_full); });
        }));
        report(stem, "SofA", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return ev.getSofA(xs, fr * xs.a_full); });
        }));
        report(stem, "newton_AofS", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return ev.getAofS(xs, fr * xs.s_full); });
        }));
        report(stem, "brent_depthOfArea", calls, time_sweep(calls, [&] {
            sweep([&](const XSectParams& xs, double fr) {
                return brent_depth_of_area(ev, xs, fr * xs.a_full); });
        }));

        // Batch gather shape: iterate links in the inner loop so consecutive
        // evaluations chase different table pointers, like perlink_tabulated.
        report(stem, "gather_area", calls, time_sweep(calls, [&] {
            double acc = 0.0;
            for (double fr : frac)
                for (const auto& xs : xss)
                    acc += ev.getAofY(xs, fr * xs.y_full);
            g_sink = g_sink + acc;
        }));

        // ---- A1: inv_YofA with the bucket maps unbound (the pre-change path).
        // Same evaluator, same params, only the search strategy differs — so
        // the ratio against inv_YofA above is the LUT's own contribution.
        openswmm::xsect::XsectTables no_lut_tbl = openswmm::xsect::hostTables();
        no_lut_tbl.luts = nullptr;
        const XsectEval bisecting{no_lut_tbl};
        std::vector<XSectParams> xss_nolut = xss;
        for (auto& xs : xss_nolut) xs.area_lut = nullptr;

        report(stem, "inv_YofA_bisect", calls, time_sweep(calls, [&] {
            double acc = 0.0;
            for (const auto& xs : xss_nolut)
                for (double fr : frac) acc += bisecting.getYofA(xs, fr * xs.a_full);
            g_sink = g_sink + acc;
        }));

        // ---- A2: the DW STEP-D batch shape — area + hyd-radius over the whole
        // group at one depth, as two perlink_tabulated passes vs one fused pass.
        const std::size_t nlink = xss.size();
        std::vector<double> b_nrm(nlink), b_af(nlink), b_rf(nlink);
        std::vector<const double*> b_ta(nlink), b_tb(nlink);
        for (std::size_t li = 0; li < nlink; ++li) {
            b_nrm[li] = xss[li].y_full;
#ifdef SWMM_XSECT_FAST_LOOKUP
            b_nrm[li] = (xss[li].y_full > 0.0) ? 1.0 / xss[li].y_full : 0.0;
#endif
            b_af[li] = xss[li].a_full;
            b_rf[li] = xss[li].r_full;
            b_ta[li] = xss[li].area_tbl;
            b_tb[li] = xss[li].hrad_tbl;
        }
        std::vector<double> b_a(nlink), b_r(nlink);
        const int tbl_n = xss.front().transect_tbl_size;

        // The depth sweeps are materialised OUTSIDE the timed region. Filling
        // them costs a strided walk over the XSectParams array (~150 B/link),
        // which on a 20k-link deck is more traffic than the kernel itself — in
        // the timed loop it would swamp the very difference being measured.
        std::vector<std::vector<double>> depth_sweeps;
        depth_sweeps.reserve(frac.size());
        for (double fr : frac) {
            std::vector<double> d(nlink);
            for (std::size_t li = 0; li < nlink; ++li) d[li] = fr * xss[li].y_full;
            depth_sweeps.push_back(std::move(d));
        }
        const std::size_t batch_calls = nlink * depth_sweeps.size();

        report(stem, "batch_AR_2pass", batch_calls, time_sweep(batch_calls, [&] {
            for (const auto& d : depth_sweeps) {
                openswmm::xsect_batch::perlink_tabulated(
                    d.data(), b_nrm.data(), b_af.data(), b_ta.data(),
                    tbl_n, b_a.data(), static_cast<int>(nlink));
                openswmm::xsect_batch::perlink_tabulated(
                    d.data(), b_nrm.data(), b_rf.data(), b_tb.data(),
                    tbl_n, b_r.data(), static_cast<int>(nlink));
                g_sink = g_sink + b_a[0] + b_r[0];
            }
        }));
        report(stem, "batch_AR_fused", batch_calls, time_sweep(batch_calls, [&] {
            for (const auto& d : depth_sweeps) {
                openswmm::xsect_batch::perlink_tabulated_pair(
                    d.data(), b_nrm.data(), b_af.data(), b_rf.data(),
                    b_ta.data(), b_tb.data(), tbl_n,
                    b_a.data(), b_r.data(), static_cast<int>(nlink));
                g_sink = g_sink + b_a[0] + b_r[0];
            }
        }));

    }
    return 0;
}
