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
 * @file bench_fv_closure.cpp
 * @brief Per-op cost of the explicit-FV cross-section closure (FvKernels.hpp)
 *        on the geometries NetworkMeshBuilder::buildGeometry builds.
 *
 * @details Phase 0 of plans/FV1D_CLOSURE_KERNEL_PERF_PLAN_2026-09-11.md. The
 *          solver's closure-call counters ([PERF-FV] n.area/n.width/n.i1/
 *          n.hydrad/n.invert) × the ns/call measured here is how the closure's
 *          share of a routing step is sized; and every candidate replacement
 *          kernel reports against these rows in the same binary.
 *
 *          Ops, named after how the solver calls them:
 *
 *            area              areaOfDepth(g, h)
 *            width             widthOfDepth(g, h)
 *            hydrad            hydRadOfDepth(g, h)
 *            area_i1           i1OfDepth(g, h, areaOfDepth(g, h)) — the pair
 *                              faceSide evaluates for the cell's own side
 *            faceside          one side of a first-order face flux: area+I₁ at
 *                              the cell depth, then area+width+I₁ at the
 *                              reconstructed depth (2 area, 1 width, 2 I₁)
 *            invert            depthOfArea(g, a) — Newton on the closure cubic
 *            roundtrip         depthOfArea(g, areaOfDepth(g, h))
 *            eval_fused        closureEval(g.closure_tbl, h) — A, T, I₁ from
 *                              one panel locate (what faceSide and the
 *                              per-cell cache call)
 *
 *          Inputs are precomputed outside the timed region: a temporally
 *          coherent random walk in h over [0, 1.5·y_full] (the substep-to-
 *          substep pattern the solver actually presents, slot band included)
 *          and the areas of those depths for the inverse ops. A deterministic
 *          LCG, so two runs sweep byte-identical inputs.
 *
 *          Output is CSV on stdout: shape,op,calls,reps,ns_med,ns_min,ns_max,
 *          followed by one `# roundtrip_max_rel` comment line per shape (the
 *          |depthOfArea(areaOfDepth(h)) − h| / y_full maximum over the sweep,
 *          recorded so the accuracy of the inverse rides along with its cost).
 *
 * Usage:
 *   bench_fv_closure [--slot-celerity C] [--samples N] [--deck file.inp ...]
 *
 *   With no deck the built-in analytic shapes are timed. Each --deck adds its
 *   IRREGULAR / STREET links (through link::buildXSectParams, exactly as the
 *   mesh builder sees them) as one row per link, capped at 64 links per deck.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "../../src/engine/core/SWMMEngine.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/hydraulics/Link.hpp"
#include "../../src/engine/hydraulics/XSectBatch.hpp"
#include "../../src/engine/hydraulics/fv/FvKernels.hpp"
#include "../../src/engine/hydraulics/fv/NetworkMeshBuilder.hpp"

namespace {

using openswmm::XSectParams;
using openswmm::XSectShape;
using openswmm::fv::FvGeometry;
namespace k = openswmm::fv::kernels;

volatile double g_sink = 0.0;   // defeats DCE across every timed loop

constexpr int kReps = 9;        // repetitions of the whole sweep; median reported

struct Timed {
    double ns_med, ns_min, ns_max;
};

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

void report(const std::string& shape, const char* op, std::size_t calls, Timed t) {
    std::printf("%s,%s,%zu,%d,%.2f,%.2f,%.2f\n",
                shape.c_str(), op, calls, kReps, t.ns_med, t.ns_min, t.ns_max);
}

/// One named geometry under test.
struct Case {
    std::string name;
    FvGeometry  g;
};


/// Analytic shape from legacy-order geom1..4 (the [XSECTIONS] columns).
Case analytic(const char* name, XSectShape type, double p0, double p1 = 0.0,
              double p2 = 0.0, double p3 = 0.0, double slot_celerity = 100.0) {
    Case c;
    c.name = name;
    XSectParams xs;
    xs.type = static_cast<int>(type);
    double p[4] = {p0, p1, p2, p3};
    openswmm::xsect::setParams(xs, xs.type, p, 1.0);
    openswmm::fv::buildGeometry(xs, openswmm::xsect::isOpen(xs.type),
                                slot_celerity, c.g);
    return c;
}

/// Deterministic LCG in [0, 1).
struct Lcg {
    unsigned long long s = 0x9E3779B97F4A7C15ULL;
    double next() {
        s = s * 6364136223846793005ULL + 1442695040888963407ULL;
        return static_cast<double>(s >> 11) * (1.0 / 9007199254740992.0);
    }
};

/// A coherent random walk in depth over [0, h_max]: each step moves by at most
/// ±step. This is the substep-to-substep pattern a cell presents, and it is
/// what keeps the bracket/locate cost honest (a uniform sweep would be the
/// pessimistic control; both are reported).
std::vector<double> walk(int n, double h_max, double step, Lcg& rng) {
    std::vector<double> h(static_cast<std::size_t>(n));
    double x = 0.4 * h_max;
    for (int i = 0; i < n; ++i) {
        x += step * (2.0 * rng.next() - 1.0);
        if (x < 0.0) x = -x;
        if (x > h_max) x = 2.0 * h_max - x;
        h[static_cast<std::size_t>(i)] = x;
    }
    return h;
}

void bench_case(const Case& c, int n_samples, Lcg& rng) {
    const FvGeometry& g = c.g;
    if (!(g.y_full > 0.0) || !(g.a_crown > 0.0)) {
        std::fprintf(stderr, "%s: degenerate geometry — skipped\n", c.name.c_str());
        return;
    }

    // Inputs, outside the timed region. The walk covers the slot band and a
    // stretch above the crown (1.5·y_full), because both are on the hot path
    // of a surcharging network.
    const std::vector<double> hs = walk(n_samples, 1.5 * g.y_full, 0.02 * g.y_full, rng);
    std::vector<double> hstar(hs.size());
    std::vector<double> as(hs.size());
    for (std::size_t i = 0; i < hs.size(); ++i) {
        hstar[i] = 0.9 * hs[i];                       // a reconstructed depth
        as[i]    = k::areaOfDepth(g, hs[i]);          // the inverse's input
    }
    const std::size_t calls = hs.size();

    report(c.name, "area", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double h : hs) acc += k::areaOfDepth(g, h);
        g_sink = g_sink + acc;
    }));
    report(c.name, "width", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double h : hs) acc += k::widthOfDepth(g, h);
        g_sink = g_sink + acc;
    }));
    report(c.name, "hydrad", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double h : hs) acc += k::hydRadOfDepth(g, h);
        g_sink = g_sink + acc;
    }));
    report(c.name, "area_i1", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double h : hs) {
            const double a = k::areaOfDepth(g, h);
            acc += a + k::i1OfDepth(g, h, a);
        }
        g_sink = g_sink + acc;
    }));
    report(c.name, "faceside", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (std::size_t i = 0; i < hs.size(); ++i) {
            const double h = hs[i];
            const double i1_raw = (h > k::kDryDepth)
                ? k::i1OfDepth(g, h, k::areaOfDepth(g, h)) : 0.0;
            const double hst = hstar[i];
            if (hst <= k::kDryDepth) { acc += i1_raw; continue; }
            const double a = k::areaOfDepth(g, hst);
            const double cc = k::celerity(a, k::widthOfDepth(g, hst));
            acc += i1_raw + a + cc + k::i1OfDepth(g, hst, a);
        }
        g_sink = g_sink + acc;
    }));
    report(c.name, "invert", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double a : as) acc += k::depthOfArea(g, a);
        g_sink = g_sink + acc;
    }));
    report(c.name, "roundtrip", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double h : hs) acc += k::depthOfArea(g, k::areaOfDepth(g, h));
        g_sink = g_sink + acc;
    }));

    // The fused evaluation: A, T and I₁ from one panel locate — what
    // faceSide and the per-cell cache call.
    report(c.name, "eval_fused", calls, time_sweep(calls, [&] {
        double acc = 0.0;
        for (double h : hs) {
            const k::ClosureEval e = k::closureEval(g.closure_tbl, h);
            acc += e.a + e.t + e.i1;
        }
        g_sink = g_sink + acc;
    }));

    // Accuracy of the inverse over the same sweep, so cost and correctness
    // are read side by side.
    double max_rel = 0.0;
    for (double h : hs) {
        const double back = k::depthOfArea(g, k::areaOfDepth(g, h));
        max_rel = std::max(max_rel, std::fabs(back - h) / g.y_full);
    }
    std::printf("# %s roundtrip_max_rel=%.3e y_full=%.6g a_crown=%.6g t_slot=%.6g\n",
                c.name.c_str(), max_rel, g.y_full, g.a_crown, g.t_slot);
}

}  // namespace

int main(int argc, char** argv) {
    double slot_celerity = 100.0;
    int n_samples = 4096;
    std::vector<std::string> decks;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--slot-celerity") == 0 && i + 1 < argc)
            slot_celerity = std::atof(argv[++i]);
        else if (std::strcmp(argv[i], "--samples") == 0 && i + 1 < argc)
            n_samples = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--deck") == 0 && i + 1 < argc)
            decks.emplace_back(argv[++i]);
        else {
            std::fprintf(stderr,
                "usage: %s [--slot-celerity C] [--samples N] [--deck f.inp ...]\n",
                argv[0]);
            return 2;
        }
    }

    std::printf("shape,op,calls,reps,ns_med,ns_min,ns_max\n");
    Lcg rng;

    // The dominant sewer shapes first, then the ones whose closed forms the
    // exact-geometry program will replace, in the 3–5 ft class.
    const Case cases[] = {
        analytic("circular_3ft",      XSectShape::CIRCULAR,      3.0, 0.0, 0.0, 0.0, slot_celerity),
        analytic("force_main_3ft",    XSectShape::FORCE_MAIN,    3.0, 120.0, 0.0, 0.0, slot_celerity),
        analytic("rect_closed_4x3",   XSectShape::RECT_CLOSED,   3.0, 4.0, 0.0, 0.0, slot_celerity),
        analytic("rect_open_10x5",    XSectShape::RECT_OPEN,     5.0, 10.0, 0.0, 0.0, slot_celerity),
        analytic("trapezoid_5_4_2_2", XSectShape::TRAPEZOIDAL,   5.0, 4.0, 2.0, 2.0, slot_celerity),
        analytic("triangular_5x10",   XSectShape::TRIANGULAR,    5.0, 10.0, 0.0, 0.0, slot_celerity),
        analytic("horiz_ellipse_3x4", XSectShape::HORIZ_ELLIPSE, 3.0, 4.0, 0.0, 0.0, slot_celerity),
        analytic("eggshaped_3ft",     XSectShape::EGGSHAPED,     3.0, 0.0, 0.0, 0.0, slot_celerity),
        analytic("parabolic_5x10",    XSectShape::PARABOLIC,     5.0, 10.0, 0.0, 0.0, slot_celerity),
    };
    for (const Case& c : cases) bench_case(c, n_samples, rng);

    for (const auto& deck : decks) {
        openswmm::SWMMEngine eng;
        const std::string rpt = deck + ".benchrpt";
        if (eng.open(deck.c_str(), rpt.c_str(), nullptr) != 0) {
            std::fprintf(stderr, "open(%s) failed — skipped\n", deck.c_str());
            continue;
        }
        auto& ctx = eng.context();
        const std::string stem = std::filesystem::path(deck).stem().string();
        int taken = 0;
        for (int j = 0; j < ctx.n_links() && taken < 64; ++j) {
            const auto uj = static_cast<std::size_t>(j);
            const auto shape = ctx.links.xsect_shape[uj];
            if (shape != openswmm::XsectShape::IRREGULAR &&
                shape != openswmm::XsectShape::STREET_XSECT)
                continue;
            ctx.links.xsect_batch_shape[uj] = openswmm::link::translateShape(shape);
            XSectParams xs =
                openswmm::link::buildXSectParams(ctx.links, uj, &ctx.transect_tables);
            if (!(xs.a_full > 0.0) || !(xs.y_full > 0.0)) continue;
            Case c;
            c.name = stem + ":L" + std::to_string(j);
            openswmm::fv::buildGeometry(xs, openswmm::xsect::isOpen(xs.type),
                                        slot_celerity, c.g);
            bench_case(c, n_samples, rng);
            ++taken;
        }
        if (taken == 0)
            std::fprintf(stderr, "%s: no IRREGULAR/STREET links — nothing timed\n",
                         stem.c_str());
    }
    return 0;
}
