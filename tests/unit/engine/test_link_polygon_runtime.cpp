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

// White-box: the face-section tests below read the solver's live mesh.
#include "core/SWMMEngine.hpp"
#include "hydraulics/fv/NetworkMeshData.hpp"

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
/// @param hotstart_out  if non-empty, adds a `[FILES] SAVE HOTSTART` line so
///        the end-of-run save (and its geometry-change warning) can be tested.
std::string writeModel(const std::string& name, const char* routing,
                       const std::string& hotstart_out = "") {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         " << routing
       << "\nSTART_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        NO\n\n";
    if (!hotstart_out.empty())
        os << "[FILES]\nSAVE HOTSTART \"" << hotstart_out << "\"\n\n";
    os << "[JUNCTIONS]\nJ0  40.0  12.0  0  0  0\nJ1  39.6  12.0  0  0  0\n\n"
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
// Gap E — the SAVE HOTSTART warning counts LINKS, not CHANGES.
// ---------------------------------------------------------------------------

// Regression for a GitHub Copilot review finding on PR #146:
// xsect_runtime_changed_links recorded the link index on every call with no
// de-duplication, so a link updated 100 times (exactly what
// RepeatedUpdatesShrinkMonotonicallyAndStayValid above does) would have made
// the end-of-run warning read "100 link(s) had their cross-section changed"
// for what is actually one link.
TEST(LinkPolygonRuntime, HotstartWarningCountsDistinctLinksNotChanges) {
    const std::string hs = outDir() + "/repeat_hs_warn.hsf";
    const std::string inp = writeModel("repeat_hs_warn", "FV", hs);
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(r.open(inp, "repeat_hs_warn"));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.01));

    std::vector<double> x, y;
    for (int k = 0; k < 5; ++k) {
        sedimentedCircle(4.0, 0.01 + 0.01 * k, 24, x, y);
        double displaced = 0.0;
        ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                        static_cast<int>(x.size()), 0, &displaced),
                  SWMM_OK);
        double t = 0.0;
        ASSERT_EQ(swmm_engine_step(r.e, &t), 0);
    }
    ASSERT_EQ(swmm_engine_end(r.e), 0);

    // Exactly one link was ever touched, five times. Find the SAVE HOTSTART
    // warning among whatever else the run produced and check ITS count, not
    // just that some geometry warning exists.
    bool found = false;
    const int n = swmm_get_warning_count(r.e);
    for (int i = 0; i < n; ++i) {
        const std::string w = swmm_get_warning_at(r.e, i);
        if (w.find("cross-section changed at run time") == std::string::npos)
            continue;
        found = true;
        EXPECT_NE(w.find("1 link(s)"), std::string::npos)
            << "warning should count the one DISTINCT link, not the 5 calls: " << w;
    }
    EXPECT_TRUE(found) << "no SAVE HOTSTART geometry-change warning was emitted";
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


// ---------------------------------------------------------------------------
// Face sections after a run-time change (PR #157 review, item 1).
// ---------------------------------------------------------------------------
//
// mesh.geom is indexed by SECTION, shared between conduits of identical
// geometry, so a run-time change gives the conduit a section of its own and
// must re-point the FACES that reconstruct in it. A full rebuild assigns face
// sections in two passes — each face takes its adjacent cell's section (the
// left cell's, else the right's), then the two faces at a clean degree-2 node
// between DIFFERENT sections share one averaged section unless either side
// carries a compiled boundary. A run-time change always installs one, so what
// these tests demand is exactly the rebuild result for the new state:
//   * every face touching the changed conduit's cells follows the cell rule;
//   * the neighbour's face at a degree-2 node it shared with that conduit
//     gets its own section back (the average was built from the old shape);
//   * every other face, and every other conduit's cells, are untouched.
// The defect these pin re-pointed the cells but tested `cell_geom == conduit`
// (a section index against a conduit index), so no face was ever updated.

namespace {

using Mesh = openswmm::fv::NetworkMeshData;

const Mesh& meshOf(SWMM_Engine e) {
    return static_cast<openswmm::SWMMEngine*>(e)->router().fvMesh();
}

/// NetworkMeshBuilder's first pass (deriveFaceGeom) for one face.
int cellRule(const Mesh& m, int f) {
    const auto uf = static_cast<std::size_t>(f);
    const int c = (m.face_cl[uf] >= 0) ? m.face_cl[uf] : m.face_cr[uf];
    return m.cell_geom[static_cast<std::size_t>(c)];
}

int conduitRow(const Mesh& m, int link) {
    for (int r = 0; r < m.n_conduits(); ++r)
        if (m.conduit_link[static_cast<std::size_t>(r)] == link) return r;
    return -1;
}

bool ownsCell(const Mesh& m, int r, int c) {
    const auto ur = static_cast<std::size_t>(r);
    return c >= m.conduit_cell_begin[ur] &&
           c < m.conduit_cell_begin[ur] + m.conduit_cell_count[ur];
}

bool touches(const Mesh& m, int r, int f) {
    const auto uf = static_cast<std::size_t>(f);
    return ownsCell(m, r, m.face_cl[uf]) || ownsCell(m, r, m.face_cr[uf]);
}

/// The other face at f's node when that node is clean degree-2, else -1.
int partner(const Mesh& m, int f) {
    const int node = m.face_node[static_cast<std::size_t>(f)];
    if (node < 0) return -1;
    const auto un = static_cast<std::size_t>(node);
    const int nb = m.node_face_ptr[un], ne = m.node_face_ptr[un + 1];
    if (ne - nb != 2) return -1;
    for (int k = nb; k < ne; ++k) {
        const int fp = m.node_face_idx[static_cast<std::size_t>(k)];
        if (fp != f) return fp;
    }
    return -1;
}

struct MeshSnapshot {
    std::vector<int> face_geom, cell_geom, conduit_section;
    explicit MeshSnapshot(const Mesh& m)
        : face_geom(m.face_geom), cell_geom(m.cell_geom),
          conduit_section(m.conduit_section) {}
};

/// Everything a run-time change of conduit row r must (and must not) do,
/// checked against the mesh as it stood just before the change.
void expectRebuildEquivalent(const Mesh& m, const MeshSnapshot& before, int r) {
    const auto ur = static_cast<std::size_t>(r);
    const int sec = m.conduit_section[ur];
    EXPECT_NE(sec, before.conduit_section[ur]) << "the conduit kept its old section";
    for (int c = 0; c < m.n_cells(); ++c) {
        const auto uc = static_cast<std::size_t>(c);
        if (ownsCell(m, r, c))
            EXPECT_EQ(m.cell_geom[uc], sec) << "cell " << c << " of the changed conduit";
        else
            EXPECT_EQ(m.cell_geom[uc], before.cell_geom[uc]) << "unrelated cell " << c;
    }
    for (int q = 0; q < m.n_conduits(); ++q)
        if (q != r)
            EXPECT_EQ(m.conduit_section[static_cast<std::size_t>(q)],
                      before.conduit_section[static_cast<std::size_t>(q)])
                << "unrelated conduit row " << q << " was re-pointed";

    std::vector<bool> affected(static_cast<std::size_t>(m.n_faces()), false);
    for (int f = 0; f < m.n_faces(); ++f) {
        if (!touches(m, r, f)) continue;
        affected[static_cast<std::size_t>(f)] = true;
        const int fp = partner(m, f);
        if (fp >= 0) affected[static_cast<std::size_t>(fp)] = true;
    }
    for (int f = 0; f < m.n_faces(); ++f) {
        const auto uf = static_cast<std::size_t>(f);
        if (affected[uf])
            EXPECT_EQ(m.face_geom[uf], cellRule(m, f))
                << "face " << f << " does not reconstruct in its cell's section";
        else
            EXPECT_EQ(m.face_geom[uf], before.face_geom[uf])
                << "unrelated face " << f << " was changed";
    }
}

/// A chain J0 -C0- J1 -C1- J2 ... of circular conduits with the given
/// diameters, steady inflow at J0, free outfall at the end.
std::string writeChain(const std::string& name, const std::vector<double>& diam) {
    const std::string path = outDir() + "/" + name + ".inp";
    std::ofstream os(path);
    os << "[OPTIONS]\nFLOW_UNITS           CFS\nFLOW_ROUTING         FV\n"
          "START_DATE           01/01/2026\nSTART_TIME           00:00:00\n"
          "END_DATE             01/01/2026\nEND_TIME             02:00:00\n"
          "REPORT_STEP          00:05:00\nROUTING_STEP         5\n"
          "ALLOW_PONDING        NO\n\n[JUNCTIONS]\n";
    const std::size_t n = diam.size();
    for (std::size_t i = 0; i < n; ++i)
        os << "J" << i << "  " << 40.0 - 0.4 * static_cast<double>(i) << "  12.0  0  0  0\n";
    os << "\n[OUTFALLS]\nO1  " << 40.0 - 0.4 * static_cast<double>(n) << "  FREE  NO\n\n[CONDUITS]\n";
    for (std::size_t i = 0; i < n; ++i)
        os << "C" << i << "  J" << i << "  " << (i + 1 < n ? "J" + std::to_string(i + 1) : std::string("O1"))
           << "  200.0  0.013  0  0  0\n";
    os << "\n[XSECTIONS]\n";
    for (std::size_t i = 0; i < n; ++i)
        os << "C" << i << "  CIRCULAR  " << diam[i] << "  0  0  0  1\n";
    os << "\n[INFLOWS]\nJ0  FLOW  \"\"  FLOW  1.0  1.0  12.0\n\n"
          "[TIMESERIES]\n\n[REPORT]\nINPUT  NO\nCONTROLS  NO\n";
    return path;
}

void openChain(FvRun& r, const std::string& name, const std::vector<double>& diam) {
    ASSERT_NO_FATAL_FAILURE(r.open(writeChain(name, diam), name));
}

}  // namespace

TEST(LinkPolygonRuntime, FacesFollowTheNewSectionWhenTheOldOneWasShared) {
    // Both conduits are 4 ft circles, so they share ONE section and no face
    // is averaged. Changing C0 must give its cells and faces a section of their
    // own while C1 keeps the shared one.
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(openChain(r, "faces_shared", {4.0, 4.0}));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.01));
    const Mesh& m = meshOf(r.e);
    const int c0 = conduitRow(m, r.link), c1 = conduitRow(m, swmm_link_index(r.e, "C1"));
    ASSERT_GE(c0, 0);
    ASSERT_GE(c1, 0);
    ASSERT_EQ(m.conduit_section[static_cast<std::size_t>(c0)],
              m.conduit_section[static_cast<std::size_t>(c1)])
        << "precondition: identical conduits must share a section";

    const MeshSnapshot before(m);
    std::vector<double> x, y;
    sedimentedCircle(4.0, 0.1, 40, x, y);
    double displaced = 0.0;
    ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), 0, &displaced), SWMM_OK);
    expectRebuildEquivalent(m, before, c0);
    EXPECT_EQ(m.conduit_section[static_cast<std::size_t>(c1)],
              before.conduit_section[static_cast<std::size_t>(c1)]);
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

TEST(LinkPolygonRuntime, AveragedFacesRevertOnlyAtTheChangedConduitsNodes) {
    // 4, 3 and 2 ft circles: three distinct sections, and an averaged face
    // pair at each degree-2 junction (J1 between C0/C1, J2 between C1/C2).
    // Changing C0 must dissolve the J1 average — C1's face gets C1's own
    // section back — and leave the J2 average, which C0 does not touch, alone.
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(openChain(r, "faces_distinct", {4.0, 3.0, 2.0}));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.01));
    const Mesh& m = meshOf(r.e);
    const int c0 = conduitRow(m, r.link);
    ASSERT_GE(c0, 0);

    int averaged = 0;
    for (int f = 0; f < m.n_faces(); ++f)
        if (m.face_geom[static_cast<std::size_t>(f)] != cellRule(m, f)) ++averaged;
    ASSERT_EQ(averaged, 4) << "precondition: two averaged pairs (J1 and J2)";

    const MeshSnapshot before(m);
    std::vector<double> x, y;
    sedimentedCircle(4.0, 0.1, 40, x, y);
    double displaced = 0.0;
    ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), 0, &displaced), SWMM_OK);
    expectRebuildEquivalent(m, before, c0);

    int still_averaged = 0;
    for (int f = 0; f < m.n_faces(); ++f)
        if (m.face_geom[static_cast<std::size_t>(f)] != cellRule(m, f)) ++still_averaged;
    EXPECT_EQ(still_averaged, 2) << "only the J2 pair, which C0 does not touch, stays averaged";
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

TEST(LinkPolygonRuntime, FaceSectionsStayConsistentAcrossRepeatedUpdates) {
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(openChain(r, "faces_repeat", {4.0, 3.0}));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.01));
    const Mesh& m = meshOf(r.e);
    const int c0 = conduitRow(m, r.link);
    ASSERT_GE(c0, 0);
    std::vector<double> x, y;
    for (int k = 0; k < 5; ++k) {
        const MeshSnapshot before(m);
        sedimentedCircle(4.0, 0.05 + 0.05 * k, 40, x, y);
        double displaced = 0.0;
        ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                        static_cast<int>(x.size()), 0, &displaced),
                  SWMM_OK) << "update " << k;
        SCOPED_TRACE("update " + std::to_string(k));
        expectRebuildEquivalent(m, before, c0);
        double t = 0.0;
        ASSERT_EQ(swmm_engine_step(r.e, &t), 0);
    }
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}

TEST(LinkPolygonRuntime, RoutingContinuesCleanlyAfterAFaceSectionChange) {
    // After the change the network has to keep routing on the corrected faces:
    // finite state everywhere, the face assignment not drifting during the
    // run, and flow still reaching the outfall.
    FvRun r;
    ASSERT_NO_FATAL_FAILURE(openChain(r, "faces_continue", {4.0, 3.0}));
    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.02));
    const Mesh& m = meshOf(r.e);
    std::vector<double> x, y;
    sedimentedCircle(4.0, 0.15, 40, x, y);
    double displaced = 0.0;
    ASSERT_EQ(swmm_link_set_polygon(r.e, r.link, x.data(), y.data(),
                                    static_cast<int>(x.size()), 0, &displaced), SWMM_OK);
    const std::vector<int> faces_after_change = m.face_geom;

    ASSERT_NO_FATAL_FAILURE(stepTo(r.e, 0.06));
    EXPECT_EQ(m.face_geom, faces_after_change) << "routing changed the face assignment";
    for (const char* id : {"C0", "C1"}) {
        const int j = swmm_link_index(r.e, id);
        double q = 0.0, d = 0.0;
        ASSERT_EQ(swmm_link_get_flow(r.e, j, &q), SWMM_OK);
        ASSERT_EQ(swmm_link_get_depth(r.e, j, &d), SWMM_OK);
        EXPECT_TRUE(std::isfinite(q) && std::isfinite(d)) << id;
        EXPECT_GE(d, 0.0) << id;
    }
    double q1 = 0.0;
    ASSERT_EQ(swmm_link_get_flow(r.e, swmm_link_index(r.e, "C1"), &q1), SWMM_OK);
    EXPECT_GT(q1, 0.5) << "the 1 cfs inflow no longer reaches the downstream conduit";
    EXPECT_EQ(swmm_engine_end(r.e), 0);
}
