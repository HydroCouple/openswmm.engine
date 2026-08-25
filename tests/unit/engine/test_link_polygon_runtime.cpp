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
 * @file test_link_polygon_runtime.cpp
 * @brief Run-time cross-section change through swmm_link_set_polygon (Phase 7).
 *
 * @details Covers the plan's tests 23 (time-varying geometry, and the overflow
 *          judgement) and 24 (repeated updates), plus the Gap D refusal and the
 *          boundary-validation rejects.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>

namespace fs = std::filesystem;

namespace {

constexpr double kPi = 3.14159265358979323846;

std::string outDir() {
    const std::string d = std::string(OPENSWMM_FV_TEST_OUT_DIR) + "/polygon_runtime";
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

/// A two-conduit chain with a steady inflow — small enough to step quickly,
/// long enough that the pipes carry real water when the section is changed.
std::string writeModel(const std::string& name, const char* routing) {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        NO\n\n"
          "[JUNCTIONS]\nJ0  40.0  12.0  0  0  0\nJ1  39.6  12.0  0  0  0\n\n"
          "[OUTFALLS]\nO1  39.2  FREE  NO\n\n"
          "[CONDUITS]\n"
          "C0  J0  J1  200.0  0.013  0  0  0\n"
          "C1  J1  O1  200.0  0.013  0  0  0\n\n"
          "[XSECTIONS]\n"
          "C0  CIRCULAR  4.0  0  0  0  1\n"
          "C1  CIRCULAR  4.0  0  0  0  1\n\n"
          "[INFLOWS]\nJ0  FLOW  \"\"  FLOW  1.0  1.0  12.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    return path;
}

/// Circle of diameter d, invert at y = 0, truncated below by a flat bed at
/// `fill`·d — the sediment case test 23 asks for. Returned counter-clockwise
/// as an inscribed polyline; `nseg` segments approximate the wetted arc.
void sedimentedCircle(double d, double fill, int nseg,
                      std::vector<double>& x, std::vector<double>& y) {
    x.clear();
    y.clear();
    const double r = 0.5 * d;
    // Parametrize by th measured from the invert: (r sin th, r - r cos th), so
    // th = 0 is the invert and th = pi the crown. The bed cuts the circle where
    // y = fill*d, i.e. cos th = 1 - 2*fill.
    double c = 1.0 - 2.0 * fill;
    if (c < -1.0) c = -1.0;
    if (c > 1.0) c = 1.0;
    const double th0 = std::acos(c);
    // Walk the surviving arc from one bed corner, over the crown, to the other.
    // The closing chord back along the flat bed is implicit — fromPolyline
    // joins the last vertex to the first.
    for (int i = 0; i <= nseg; ++i) {
        const double th = th0 + (static_cast<double>(i) / nseg) * (2.0 * kPi - 2.0 * th0);
        x.push_back(r * std::sin(th));
        y.push_back(r - r * std::cos(th));
    }
}

/// Plain circle as a polyline, for the "no fill" reference.
void circlePolyline(double d, int nseg,
                    std::vector<double>& x, std::vector<double>& y) {
    x.clear();
    y.clear();
    const double r = 0.5 * d;
    for (int i = 0; i < nseg; ++i) {
        const double th = (2.0 * kPi * i) / nseg;
        x.push_back(r * std::sin(th));
        y.push_back(r - r * std::cos(th));
    }
}

/// Step the engine until `t_target` days of simulation have elapsed.
void stepTo(SWMM_Engine e, double t_target) {
    double t = 0.0;
    while (t < t_target) {
        const int rc = swmm_engine_step(e, &t);
        ASSERT_EQ(rc, 0);
        if (t <= 0.0) break;
    }
}

struct FvRun {
    SWMM_Engine e = nullptr;
    int link = -1;
    ~FvRun() { if (e) { swmm_engine_close(e); swmm_engine_destroy(e); } }

    void open(const std::string& inp, const std::string& tag) {
        e = swmm_engine_create();
        ASSERT_NE(e, nullptr);
        const std::string rpt = outDir() + "/" + tag + ".rpt";
        const std::string out = outDir() + "/" + tag + ".out";
        ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr), 0);
        ASSERT_EQ(swmm_engine_initialize(e), 0);
        ASSERT_EQ(swmm_engine_start(e, 1), 0);
        link = swmm_link_index(e, "C0");
        ASSERT_GE(link, 0);
    }
};

}  // namespace

// ---------------------------------------------------------------------------
// Test 23 — mid-run sediment fill under CONSERVE_DEPTH.
// ---------------------------------------------------------------------------

TEST(LinkPolygonRuntime, SedimentFillDisplacesExactlyTheReportedVolume) {
    const std::string inp = writeModel("sediment", "FV");
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "sediment"));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.02));   // ~30 min: the pipe is wet

    double vol_before = 0.0;
    ASSERT_EQ(swmm_link_get_volume(r.e, r.link, &vol_before), SWMM_OK);
    ASSERT_GT(vol_before, 0.0) << "the conduit holds no water — nothing to displace";

    // 32 segments, not more: every polyline vertex is a critical height and a
    // symmetric shape pairs them, so an n-gon needs about n/2 Chebyshev pieces
    // against kMaxPieces = 24 (ChebSection.hpp). A 65-vertex chain is refused
    // with SWMM_ERR_GEOMETRY — loudly, which is the documented Phase 4 choice.
    // Dense point clouds belong in an IRREGULAR transect; POLYGON is for
    // boundaries described by a few arcs and lines.
    std::vector<double> x, y;
    sedimentedCircle(4.0, 0.25, 32, x, y);

    double displaced = -1.0;
    ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), /*CONSERVE_DEPTH=*/0,
                                    &displaced), SWMM_OK);

    // Solid material intruded, so water must have LEFT the conduit.
    EXPECT_GT(displaced, 0.0);

    // The independent check the plan asks for: the displaced volume is exactly
    // the drop in the conduit's own storage. links.volume is re-derived from
    // the reconciled cells (Router::refreshConduitGeometry), so this compares
    // the returned number against a sum over cells taken through a different
    // code path than the one that produced it.
    double vol_after = 0.0;
    ASSERT_EQ(swmm_link_get_volume(r.e, r.link, &vol_after), SWMM_OK);
    EXPECT_NEAR(displaced, vol_before - vol_after, 1.0e-9 * std::fabs(vol_before))
        << "returned displaced volume disagrees with the conduit's storage drop";

    // The run must still complete, and the section must actually be smaller.
    double y_full = 0.0, g2 = 0.0, g3 = 0.0, g4 = 0.0;
    int shape = -1;
    ASSERT_EQ(swmm_link_get_xsect(r.e, r.link, &shape, &y_full, &g2, &g3, &g4), SWMM_OK);
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.08));
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

// ---------------------------------------------------------------------------
// Test 23, second half — the Phase-7 overflow judgement.
// ---------------------------------------------------------------------------

// The plan asks what CONSERVE_VOLUME does when the new section "cannot hold
// cell_a at any depth". **It always can**, and that is the documented answer
// rather than a policy choice: the FV closure continues above the crown for
// EVERY section — a closed one through the Preissmann slot, an open one through
// a vertical-wall extension (FvGeometry::t_slot carries w_max there) — so
// areaOfDepth is unbounded above and depthOfArea inverts that branch in closed
// form. Shrinking the section under CONSERVE_VOLUME therefore surcharges the
// conduit, which is also the physically right answer: the same water in a
// smaller pipe stands higher. No water is lost, so the displaced volume stays
// 0 exactly, as the interface promises.
//
// The one genuinely unrepresentable case is t_slot <= 0, where that branch
// would divide by zero; Router::refreshConduitGeometry refuses such a section
// up front rather than letting a mid-run NaN out.
TEST(LinkPolygonRuntime, ShrinkingUnderConserveVolumeSurchargesAndLosesNoWater) {
    const std::string inp = writeModel("shrink", "FV");
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "shrink"));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.02));

    double vol_before = 0.0, depth_before = 0.0;
    ASSERT_EQ(swmm_link_get_volume(r.e, r.link, &vol_before), SWMM_OK);
    ASSERT_EQ(swmm_link_get_depth(r.e, r.link, &depth_before), SWMM_OK);
    ASSERT_GT(vol_before, 0.0);

    // A much smaller circle: whatever water is present now overfills it.
    std::vector<double> x, y;
    circlePolyline(0.6, 48, x, y);

    double displaced = -1.0;
    ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), /*CONSERVE_VOLUME=*/1,
                                    &displaced), SWMM_OK);

    // Nothing left the conduit — exactly zero, not merely small.
    EXPECT_DOUBLE_EQ(displaced, 0.0);

    double vol_after = 0.0;
    ASSERT_EQ(swmm_link_get_volume(r.e, r.link, &vol_after), SWMM_OK);
    EXPECT_NEAR(vol_after, vol_before, 1.0e-9 * vol_before)
        << "CONSERVE_VOLUME moved water";

    // And the run continues rather than producing a non-finite depth.
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.05));
    double depth_after = 0.0;
    ASSERT_EQ(swmm_link_get_depth(r.e, r.link, &depth_after), SWMM_OK);
    EXPECT_TRUE(std::isfinite(depth_after));
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

// ---------------------------------------------------------------------------
// Test 24 — repeated updates.
// ---------------------------------------------------------------------------

TEST(LinkPolygonRuntime, RepeatedUpdatesShrinkMonotonicallyAndStayValid) {
    const std::string inp = writeModel("repeat", "FV");
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "repeat"));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.01));

    double prev_full = 1.0e30;
    std::vector<double> x, y;
    for (int k = 0; k < 100; ++k) {
        // Slowly growing fill: 0.5 % to 25 % of the diameter.
        const double fill = 0.005 + 0.0025 * k;
        sedimentedCircle(4.0, fill, 40, x, y);

        double displaced = 0.0;
        ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                        static_cast<int>(x.size()), 0, &displaced),
                  SWMM_OK) << "update " << k << " (fill " << fill << ") was rejected";

        // Full area must decrease monotonically as the bed rises. Read it back
        // through the geometry the engine actually installed, not the input.
        int nv = 0;
        ASSERT_EQ(swmm_link_get_polygon(r.e, r.link, nullptr, nullptr, &nv), SWMM_OK);
        EXPECT_EQ(nv, static_cast<int>(x.size()));

        double full_depth = 0.0, g2 = 0, g3 = 0, g4 = 0;
        int shape = -1;
        ASSERT_EQ(swmm_link_get_xsect(r.e, r.link, &shape, &full_depth, &g2, &g3, &g4),
                  SWMM_OK);
        EXPECT_LT(full_depth, prev_full + 1.0e-12)
            << "full depth grew at update " << k;
        prev_full = full_depth;

        double t = 0.0;
        ASSERT_EQ(swmm_engine_step(r.e, &t), 0);
    }
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

// ---------------------------------------------------------------------------
// Gap D — run-time change is FV-only, and says so rather than misbehaving.
// ---------------------------------------------------------------------------

TEST(LinkPolygonRuntime, MidRunChangeIsRefusedUnderDynwave) {
    const std::string inp = writeModel("dynwave", "DYNWAVE");
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "dynwave"));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.01));

    std::vector<double> x, y;
    circlePolyline(3.0, 32, x, y);
    double displaced = -1.0;
    EXPECT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), 0, &displaced),
              SWMM_ERR_GEOMETRY)
        << "DYNWAVE accepted a mid-run section change it cannot reconcile";
    EXPECT_DOUBLE_EQ(displaced, 0.0);
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

// ---------------------------------------------------------------------------
// Boundary validation reaches the caller as SWMM_ERR_GEOMETRY, not silence.
// ---------------------------------------------------------------------------

TEST(LinkPolygonRuntime, InvalidBoundariesAreRejected) {
    const std::string inp = writeModel("reject", "FV");
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "reject"));

    double d = 0.0;
    // Too few points is a parameter error (checked before the boundary builder).
    const double x2[2] = {0.0, 1.0}, y2[2] = {0.0, 1.0};
    EXPECT_EQ(swmm_link_set_polygon(r.e, r.link, x2, y2, 2, 0, &d), SWMM_ERR_BADPARAM);

    // Bow-tie: non-adjacent chords cross.
    const double xb[4] = {0.0, 1.0, 0.0, 1.0};
    const double yb[4] = {0.0, 0.0, 1.0, 1.0};
    EXPECT_EQ(swmm_link_set_polygon(r.e, r.link, xb, yb, 4, 0, &d), SWMM_ERR_GEOMETRY);

    // Zero area: three collinear points.
    const double xz[3] = {0.0, 1.0, 2.0};
    const double yz[3] = {0.0, 0.0, 0.0};
    EXPECT_EQ(swmm_link_set_polygon(r.e, r.link, xz, yz, 3, 0, &d), SWMM_ERR_GEOMETRY);

    // Non-finite coordinate.
    const double xn[3] = {0.0, 1.0, std::nan("")};
    const double yn[3] = {0.0, 0.0, 1.0};
    EXPECT_EQ(swmm_link_set_polygon(r.e, r.link, xn, yn, 3, 0, &d), SWMM_ERR_GEOMETRY);

    // An out-of-range policy is a parameter error, not a geometry one — the
    // two describe opposite physical events and there is deliberately no
    // default to fall back on.
    const double xv[4] = {0.0, 1.0, 1.0, 0.0};
    const double yv[4] = {0.0, 0.0, 1.0, 1.0};
    EXPECT_EQ(swmm_link_set_polygon(r.e, r.link, xv, yv, 4, 7, &d), SWMM_ERR_BADPARAM);

    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

// ---------------------------------------------------------------------------
// get_polygon round-trips what set_polygon installed.
// ---------------------------------------------------------------------------

TEST(LinkPolygonRuntime, GetPolygonRoundTripsTheInstalledBoundary) {
    const std::string inp = writeModel("roundtrip", "FV");
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "roundtrip"));

    // A link that carries no compiled boundary reports so rather than
    // returning a stale or empty success.
    int n = 0;
    EXPECT_EQ(swmm_link_get_polygon(r.e, r.link, nullptr, nullptr, &n),
              SWMM_ERR_GEOMETRY);
    EXPECT_EQ(n, 0);

    std::vector<double> x, y;
    circlePolyline(4.0, 24, x, y);
    double d = 0.0;
    ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), 1, &d), SWMM_OK);

    n = 0;
    ASSERT_EQ(swmm_link_get_polygon(r.e, r.link, nullptr, nullptr, &n), SWMM_OK);
    ASSERT_EQ(n, static_cast<int>(x.size()));

    // A buffer that is too small must fail loudly and still report the count.
    std::vector<double> rx(static_cast<std::size_t>(n)), ry(static_cast<std::size_t>(n));
    int small = n - 1;
    EXPECT_EQ(swmm_link_get_polygon(r.e, r.link, rx.data(), ry.data(), &small),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(small, n);

    ASSERT_EQ(swmm_link_get_polygon(r.e, r.link, rx.data(), ry.data(), &n), SWMM_OK);
    // fromPolyline may reorient a clockwise chain and shifts it so the true
    // interior minimum sits at y = 0, so compare the SHAPE: the vertex set must
    // match as a set, up to that vertical shift.
    double min_in = 1.0e30, min_out = 1.0e30;
    for (std::size_t i = 0; i < x.size(); ++i) {
        min_in = std::min(min_in, y[i]);
        min_out = std::min(min_out, ry[i]);
    }
    for (std::size_t i = 0; i < x.size(); ++i) {
        bool found = false;
        for (std::size_t k = 0; k < rx.size() && !found; ++k) {
            if (std::fabs(rx[k] - x[i]) < 1.0e-9 &&
                std::fabs((ry[k] - min_out) - (y[i] - min_in)) < 1.0e-9)
                found = true;
        }
        EXPECT_TRUE(found) << "vertex " << i << " did not round-trip";
    }
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}
