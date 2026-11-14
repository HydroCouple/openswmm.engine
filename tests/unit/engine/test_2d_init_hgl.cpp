/**
 * @file      test_2d_init_hgl.cpp
 * @brief     [2D_INITIAL_HGL] — initial water-surface elevation for 2D cells.
 *
 * @details A 2D cell's initial state can be authored as a depth (the
 *          [2D_TRIANGLES] INIT_DEPTH column) or, alternatively, as an absolute
 *          water-surface elevation in [2D_INITIAL_HGL]. The engine converts
 *          the elevation to a depth per cell and clamps it at the cell invert,
 *          so a cell whose bed is above the prescribed level starts exactly
 *          dry. These tests pin that contract on a two-triangle sloping patch:
 *
 *            vertices z = 0, 0, 2, 2  (20 m square)
 *            cell 0 = (0,1,2)  centroid bed 2/3
 *            cell 1 = (0,2,3)  centroid bed 4/3
 *
 *          Artefacts land in tests/output/init_hgl_2d (CLAUDE.md §4.1).
 *
 *          Needs the full 2D module (OPENSWMM_BUILD_2D).
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

constexpr double M_TO_FT = 3.280839895013123; // 1 / 0.3048
constexpr double kBed0 = 2.0 / 3.0;           // cell 0 centroid bed (m)
constexpr double kBed1 = 4.0 / 3.0;           // cell 1 centroid bed (m)

const fs::path kOutDir = fs::path(OPENSWMM_INIT_HGL_TEST_OUT_DIR) / "init_hgl_2d";

struct Deck {
    bool si = true;
    const char* closure = "FLAT";
    double depth0_m = 0.0, depth1_m = 0.0; ///< INIT_DEPTH per cell (metres)
    /// [2D_INITIAL_HGL] rows as (cell token, elevation in metres); empty = no section.
    std::vector<std::pair<std::string, double>> hgl_m;
};

/** Build the two-triangle sloping-patch deck, authored in metres or in feet. */
std::string build_model(const Deck& d) {
    const double L = d.si ? 1.0 : M_TO_FT;
    std::ostringstream s;
    s.precision(12);
    s << "[OPTIONS]\n"
      << "FLOW_UNITS           " << (d.si ? "CMS" : "CFS") << "\n"
      << "FLOW_ROUTING         DYNWAVE\n"
      << "START_DATE           01/01/2026\n"
      << "START_TIME           00:00:00\n"
      << "END_DATE             01/01/2026\n"
      << "END_TIME             00:02:00\n"
      << "REPORT_STEP          00:01:00\n"
      << "ROUTING_STEP         2\n\n"
      << "[JUNCTIONS]\n"
      << "J1      " << 1.0 * L << "  " << 5.0 * L << "  0  0  0\n\n"
      << "[OUTFALLS]\n"
      << "O1      0  FREE  NO\n\n"
      << "[CONDUITS]\n"
      << "C1      J1    O1  " << 50.0 * L << "  0.013  0  0  0\n\n"
      << "[XSECTIONS]\n"
      << "C1      CIRCULAR  " << 0.5 * L << "  0  0  0  1\n\n"
      << "[2D_OPTIONS]\n"
      << "MAX_TIMESTEP     1\n"
      << "DRY_DEPTH        0.002\n"
      << "CELL_CLOSURE     " << d.closure << "\n"
      << "REPORT_2D        NO\n\n"
      << "[2D_VERTICES]\n"
      << "0 0 0\n"
      << 20.0 * L << " 0 0\n"
      << 20.0 * L << " " << 20.0 * L << " " << 2.0 * L << "\n"
      << "0 " << 20.0 * L << " " << 2.0 * L << "\n\n"
      << "[2D_TRIANGLES]\n"
      << "0 1 2 0.03 " << d.depth0_m * L << "\n"
      << "0 2 3 0.03 " << d.depth1_m * L << "\n";
    if (!d.hgl_m.empty()) {
        s << "\n[2D_INITIAL_HGL]\n;;CELL  HGL\n";
        for (const auto& row : d.hgl_m)
            s << row.first << "  " << row.second * L << "\n";
    }
    return s.str();
}

struct RunResult {
    bool ok = false;
    std::vector<double> depth0; ///< per-cell depth right after start (m)
    std::vector<double> head0;  ///< per-cell head right after start (m)
    std::vector<double> depth1; ///< per-cell depth after the run (m)
    std::vector<double> api_hgl; ///< authored HGL read back before initialize (mesh units)
};

fs::path write_deck(const std::string& text, const std::string& tag) {
    fs::create_directories(kOutDir);
    const fs::path inp = kOutDir / (tag + ".inp");
    std::ofstream f(inp);
    f << text;
    return inp;
}

/** Open, start, sample the initial state, run to the end, sample again. */
RunResult run_model(const std::string& text, const std::string& tag) {
    RunResult r;
    const fs::path inp = write_deck(text, tag);
    const fs::path rpt = kOutDir / (tag + ".rpt");
    const fs::path out = kOutDir / (tag + ".out");

    SWMM_Engine eng = swmm_engine_create();
    if (swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(), out.string().c_str(),
                         nullptr) != SWMM_OK) {
        swmm_engine_destroy(eng);
        return r;
    }
    int nt = 0;
    swmm_2d_triangle_count(eng, &nt);
    r.api_hgl.assign(static_cast<std::size_t>(nt), 0.0);
    for (int t = 0; t < nt; ++t)
        swmm_2d_triangle_get_init_hgl(eng, t, &r.api_hgl[static_cast<std::size_t>(t)]);

    if (swmm_engine_initialize(eng) != SWMM_OK || swmm_engine_start(eng, 1) != SWMM_OK) {
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
        return r;
    }
    r.depth0.assign(static_cast<std::size_t>(nt), -1.0);
    r.head0.assign(static_cast<std::size_t>(nt), -1.0);
    r.depth1.assign(static_cast<std::size_t>(nt), -1.0);
    const bool sampled = swmm_2d_get_depths_bulk(eng, r.depth0.data()) == SWMM_OK &&
                         swmm_2d_get_heads_bulk(eng, r.head0.data()) == SWMM_OK;

    double elapsed = 0.0;
    while (swmm_engine_step(eng, &elapsed) == SWMM_OK && elapsed > 0.0) {
    }
    const bool sampled_end = swmm_2d_get_depths_bulk(eng, r.depth1.data()) == SWMM_OK;

    swmm_engine_end(eng);
    swmm_engine_report(eng);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
    r.ok = sampled && sampled_end && nt == 2;
    return r;
}

std::string slurp(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// FLAT closure: depth = max(0, HGL - centroid bed). The level (1.0) is above
// cell 0's bed and below cell 1's, so cell 1 must start exactly dry — and
// indistinguishable from a cell authored with INIT_DEPTH 0.
TEST(InitHgl2D, FlatClosureClampsAtTheBed) {
    Deck hgl;
    hgl.hgl_m = {{"*", 1.0}};
    const RunResult r = run_model(build_model(hgl), "flat_clamp");
    ASSERT_TRUE(r.ok);

    EXPECT_NEAR(r.depth0[0], 1.0 - kBed0, 1e-9);
    EXPECT_EQ(r.depth0[1], 0.0) << "a cell whose bed is above the HGL must start dry";

    Deck dry; // no initial water anywhere
    const RunResult c = run_model(build_model(dry), "flat_dry_control");
    ASSERT_TRUE(c.ok);
    EXPECT_EQ(r.head0[1], c.head0[1]) << "clamped cell must seed the dry-cell head";
}

// An HGL below every bed leaves the whole mesh dry (and never negative).
TEST(InitHgl2D, LevelBelowEveryBedStartsDry) {
    for (const char* closure : {"FLAT", "VFR"}) {
        Deck d;
        d.closure = closure;
        d.hgl_m = {{"*", -5.0}};
        const RunResult r = run_model(build_model(d), std::string("below_bed_") + closure);
        ASSERT_TRUE(r.ok) << closure;
        EXPECT_EQ(r.depth0[0], 0.0) << closure;
        EXPECT_EQ(r.depth0[1], 0.0) << closure;
    }
}

// The HGL deck and the hand-computed equivalent INIT_DEPTH deck are the same
// initial condition, and a level pool over a sloping bed stays at rest.
TEST(InitHgl2D, EquivalentToHandComputedDepthAndStaysAtRest) {
    Deck hgl;
    hgl.hgl_m = {{"*", 3.0}};
    const RunResult a = run_model(build_model(hgl), "pool_hgl");

    Deck depth;
    depth.depth0_m = 3.0 - kBed0;
    depth.depth1_m = 3.0 - kBed1;
    const RunResult b = run_model(build_model(depth), "pool_depth");

    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_NEAR(a.depth0[i], b.depth0[i], 1e-9) << "cell " << i;
        EXPECT_NEAR(a.head0[i], 3.0, 1e-9) << "cell " << i;
        // Lake at rest: walled patch, uniform level, no forcing.
        EXPECT_NEAR(a.depth1[i], a.depth0[i], 1e-9) << "cell " << i << " moved from rest";
    }
}

// HGL wins over INIT_DEPTH on the cell it is prescribed for; other cells keep
// their depth. A `*` row followed by a cell row gives a per-cell override.
TEST(InitHgl2D, HglOverridesDepthPerCell) {
    Deck d;
    d.depth0_m = 0.5;
    d.depth1_m = 0.5;
    d.hgl_m = {{"0", 1.0}};
    const RunResult r = run_model(build_model(d), "mixed");
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.depth0[0], 1.0 - kBed0, 1e-9) << "HGL must override INIT_DEPTH";
    EXPECT_NEAR(r.depth0[1], 0.5, 1e-9) << "cell without an HGL keeps INIT_DEPTH";
    EXPECT_NEAR(r.api_hgl[0], 1.0, 1e-12);
    EXPECT_TRUE(std::isnan(r.api_hgl[1])) << "unset HGL reads back as NaN";

    Deck o;
    o.hgl_m = {{"*", 3.0}, {"1", 2.0}};
    const RunResult q = run_model(build_model(o), "star_then_cell");
    ASSERT_TRUE(q.ok);
    EXPECT_NEAR(q.depth0[0], 3.0 - kBed0, 1e-9);
    EXPECT_NEAR(q.depth0[1], 2.0 - kBed1, 1e-9);
}

// The HGL is an elevation on the mesh datum, so it follows mesh units: the
// same level authored in feet on a CFS deck seeds the same SI depths.
TEST(InitHgl2D, FollowsMeshUnits) {
    Deck si;
    si.hgl_m = {{"*", 1.5}};
    Deck us = si;
    us.si = false;
    const RunResult a = run_model(build_model(si), "units_cms");
    const RunResult b = run_model(build_model(us), "units_cfs");
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    EXPECT_NEAR(b.api_hgl[0], 1.5 * M_TO_FT, 1e-9) << "getter reports mesh units";
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_NEAR(a.depth0[i], 1.5 - (i == 0 ? kBed0 : kBed1), 1e-9);
        EXPECT_NEAR(b.depth0[i], a.depth0[i], 1e-6) << "feet read as metres?";
    }
}

// VFR closure: the invert is the lowest vertex, so both cells are partially
// wet at HGL 1.0. Mean depth of a planar-bed triangle with z = (0,0,2) under a
// level at 1 is 5/12; with z = (0,2,2) it is 1/12.
TEST(InitHgl2D, VfrClosureSeedsThePartialVolume) {
    Deck d;
    d.closure = "VFR";
    d.hgl_m = {{"*", 1.0}};
    const RunResult r = run_model(build_model(d), "vfr_partial");
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.depth0[0], 5.0 / 12.0, 1e-3);
    EXPECT_NEAR(r.depth0[1], 1.0 / 12.0, 1e-3);
    EXPECT_NEAR(r.head0[0], 1.0, 1e-3);
    EXPECT_NEAR(r.head0[1], 1.0, 1e-3);
}

// API setters persist through the .inp writer: a uniform level collapses to
// one `*` row, a partial one writes a row per prescribed cell, and clearing
// with NaN removes the section.
TEST(InitHgl2D, ApiAndWriterRoundTrip) {
    const fs::path inp = write_deck(build_model(Deck{}), "roundtrip_src");
    const fs::path rpt = kOutDir / "roundtrip_src.rpt";
    const fs::path out = kOutDir / "roundtrip_src.out";
    const double nan = std::numeric_limits<double>::quiet_NaN();

    SWMM_Engine eng = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr),
              SWMM_OK);
    double h = 0.0;
    ASSERT_EQ(swmm_2d_triangle_get_init_hgl(eng, 0, &h), SWMM_OK);
    EXPECT_TRUE(std::isnan(h)) << "no section: HGL unset";
    EXPECT_NE(swmm_2d_set_triangle_init_hgl(eng, 0, std::numeric_limits<double>::infinity()),
              SWMM_OK);
    EXPECT_NE(swmm_2d_set_triangle_init_hgl(eng, 7, 1.0), SWMM_OK);

    // Uniform -> single `*` row.
    ASSERT_EQ(swmm_2d_set_triangle_init_hgl(eng, 0, 1.25), SWMM_OK);
    ASSERT_EQ(swmm_2d_set_triangle_init_hgl(eng, 1, 1.25), SWMM_OK);
    const fs::path uniform = kOutDir / "roundtrip_uniform.inp";
    ASSERT_EQ(swmm_model_write(eng, uniform.string().c_str()), SWMM_OK);
    {
        const std::string text = slurp(uniform);
        const auto at = text.find("[2D_INITIAL_HGL]");
        ASSERT_NE(at, std::string::npos);
        EXPECT_NE(text.find("*", at), std::string::npos);
    }

    // Partial -> per-cell row; negative elevations are legal.
    ASSERT_EQ(swmm_2d_set_triangle_init_hgl(eng, 0, nan), SWMM_OK);
    ASSERT_EQ(swmm_2d_set_triangle_init_hgl(eng, 1, -0.75), SWMM_OK);
    const fs::path partial = kOutDir / "roundtrip_partial.inp";
    ASSERT_EQ(swmm_model_write(eng, partial.string().c_str()), SWMM_OK);

    // Cleared -> no section.
    ASSERT_EQ(swmm_2d_set_triangle_init_hgl(eng, 1, nan), SWMM_OK);
    const fs::path cleared = kOutDir / "roundtrip_cleared.inp";
    ASSERT_EQ(swmm_model_write(eng, cleared.string().c_str()), SWMM_OK);
    EXPECT_EQ(slurp(cleared).find("[2D_INITIAL_HGL]"), std::string::npos);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);

    auto reopen = [&](const fs::path& p, double* h0, double* h1) {
        SWMM_Engine e = swmm_engine_create();
        const fs::path r = fs::path(p).replace_extension(".rpt");
        const fs::path o = fs::path(p).replace_extension(".out");
        const int rc = swmm_engine_open(e, p.string().c_str(), r.string().c_str(),
                                        o.string().c_str(), nullptr);
        if (rc == SWMM_OK) {
            swmm_2d_triangle_get_init_hgl(e, 0, h0);
            swmm_2d_triangle_get_init_hgl(e, 1, h1);
        }
        swmm_engine_close(e);
        swmm_engine_destroy(e);
        return rc;
    };
    double h0 = 0.0, h1 = 0.0;
    ASSERT_EQ(reopen(uniform, &h0, &h1), SWMM_OK);
    EXPECT_DOUBLE_EQ(h0, 1.25);
    EXPECT_DOUBLE_EQ(h1, 1.25);
    ASSERT_EQ(reopen(partial, &h0, &h1), SWMM_OK);
    EXPECT_TRUE(std::isnan(h0));
    EXPECT_DOUBLE_EQ(h1, -0.75);
}

// A `*` row ahead of the cells has nothing to assign to: rejected, not ignored.
TEST(InitHgl2D, RejectsStarRowBeforeTheCells) {
    const std::string text = "[2D_INITIAL_HGL]\n*  1.0\n\n" + build_model(Deck{});
    const fs::path inp = write_deck(text, "star_before_cells");
    const fs::path rpt = kOutDir / "star_before_cells.rpt";
    const fs::path out = kOutDir / "star_before_cells.out";
    SWMM_Engine eng = swmm_engine_create();
    EXPECT_NE(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr),
              SWMM_OK);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

// With the mesh in a [2D_MESH_FILE] the section belongs in that file: there it
// applies, while a copy in the .inp (read before the mesh loads) is rejected.
TEST(InitHgl2D, MeshFileCarriesTheSection) {
    const std::string full = build_model(Deck{});
    const auto at = full.find("[2D_VERTICES]");
    ASSERT_NE(at, std::string::npos);
    fs::create_directories(kOutDir);
    {
        std::ofstream m(kOutDir / "sidecar_mesh.2dm");
        m << full.substr(at) << "\n[2D_INITIAL_HGL]\n*  1.0\n";
    }
    const std::string inp_text =
        full.substr(0, at) + "[2D_MESH_FILE]\nFILE  sidecar_mesh.2dm\n";
    const RunResult r = run_model(inp_text, "sidecar_hgl");
    ASSERT_TRUE(r.ok);
    EXPECT_NEAR(r.depth0[0], 1.0 - kBed0, 1e-9);
    EXPECT_EQ(r.depth0[1], 0.0);

    const fs::path inp = write_deck(inp_text + "\n[2D_INITIAL_HGL]\n*  1.0\n", "sidecar_inline_hgl");
    SWMM_Engine eng = swmm_engine_create();
    const fs::path rpt = kOutDir / "sidecar_inline_hgl.rpt", out = kOutDir / "sidecar_inline_hgl.out";
    EXPECT_NE(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                               out.string().c_str(), nullptr),
              SWMM_OK);
    swmm_engine_close(eng);
    swmm_engine_destroy(eng);
}

// Malformed rows are rejected at open.
TEST(InitHgl2D, RejectsBadRows) {
    for (const char* row : {"5 1.0", "0 abc", "0 nan", "0"}) {
        std::string text = build_model(Deck{});
        text += std::string("\n[2D_INITIAL_HGL]\n") + row + "\n";
        const fs::path inp = write_deck(text, "bad_row");
        const fs::path rpt = kOutDir / "bad_row.rpt";
        const fs::path out = kOutDir / "bad_row.out";
        SWMM_Engine eng = swmm_engine_create();
        EXPECT_NE(swmm_engine_open(eng, inp.string().c_str(), rpt.string().c_str(),
                                   out.string().c_str(), nullptr),
                  SWMM_OK)
            << "row accepted: " << row;
        swmm_engine_close(eng);
        swmm_engine_destroy(eng);
    }
}
