/**
 * @file test_2d_bc_units.cpp
 * @brief Unit conversions of 2D boundary-condition data exchanged with the
 *        1D (project-unit) side, on a US-customary (feet / CFS) project.
 *
 * @details Pins the contract fixed on 2026-09-03 (see
 *          plans/UNITS_1D2D_EXCHANGE_HANDOFF_2026-09-03.md):
 *   - Constant SPECIFIED_STAGE follows the MESH units (ft → m with the
 *     mesh; already m under a `;; UNITS: SI (m)` header).
 *   - TS_STAGE / rating-curve stage follows FLOW_UNITS ONLY — [TIMESERIES]
 *     is 1D project data in feet even when the mesh file is tagged SI.
 *   - SPECIFIED_FLOW / TS_FLOW follow FLOW_UNITS (CFS/m → m³/s/m).
 *   - The writers emit constant flows back in display units, so a
 *     write → reopen round trip does not convert twice; a repeated
 *     initialize() does not scale the constants twice either.
 *
 * Artefacts land in tests/output/bc_units_2d (CLAUDE.md §4.1).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_2d.h>

namespace fs = std::filesystem;

namespace {

constexpr double kFtToM   = 0.3048;
constexpr double kCfsToCms = 0.028316846592;

const fs::path kOutDir = fs::path(OPENSWMM_BC_UNITS_TEST_OUT_DIR) / "bc_units_2d";

/// 4-triangle square mesh, one junction + outfall, US units. Edge (tri 0,
/// local edge 2) = v0–v1 = the bottom boundary; edge (tri 1, local 2) =
/// v1–v2 = the right boundary (edge e is opposite vertex e).
std::string buildModel(bool mesh_si_header, const std::string& bc_rows,
                       const std::string& extra_sections = {}) {
    std::ostringstream s;
    s << "[OPTIONS]\n"
         "FLOW_UNITS           CFS\n"
         "FLOW_ROUTING         DYNWAVE\n"
         "START_DATE           01/01/2026\n"
         "START_TIME           00:00:00\n"
         "END_DATE             01/01/2026\n"
         "END_TIME             00:10:00\n"
         "REPORT_STEP          00:05:00\n"
         "ROUTING_STEP         10\n"
         "\n[JUNCTIONS]\nJ1      0.0   3.0       0          0         0\n"
         "\n[OUTFALLS]\nO1     -1.5    FREE  NO\n"
         "\n[CONDUITS]\nC1      J1    O1  100.0    0.013      0         0          0\n"
         "\n[XSECTIONS]\nC1      CIRCULAR  1.0    0      0      0      1\n"
         "\n[2D_OPTIONS]\nMAX_TIMESTEP 2\nDRY_DEPTH 0.001\nREPORT_2D NO\n"
         "\n[2D_VERTICES]\n";
    if (mesh_si_header) s << ";; UNITS: SI (m)\n";
    // Z = 10 in the mesh's own units (10 ft, or 10 m under the SI header).
    s << ";;X      Y      Z\n"
         " 0.0    0.0   10.0\n"
         "20.0    0.0   10.0\n"
         "20.0   20.0   10.0\n"
         " 0.0   20.0   10.0\n"
         "10.0   10.0   10.0\n"
         "\n[2D_TRIANGLES]\n"
         "0     1   4   0.03        0.0\n"
         "1     2   4   0.03        0.0\n"
         "2     3   4   0.03        0.0\n"
         "3     0   4   0.03        0.0\n"
         "\n[2D_BOUNDARY_CONDITIONS]\n"
         ";;TRI EDGE TYPE PARAM_1\n"
      << bc_rows << "\n" << extra_sections;
    return s.str();
}

std::string writeInp(const std::string& name, const std::string& text) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path p = kOutDir / (name + ".inp");
    std::ofstream f(p);
    f << text;
    return p.string();
}

std::string readAll(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Eng {
    SWMM_Engine e = nullptr;
    explicit Eng(const std::string& inp, const std::string& stem) {
        e = swmm_engine_create();
        const std::string rpt = (kOutDir / (stem + ".rpt")).string();
        const std::string out = (kOutDir / (stem + ".out")).string();
        open_rc = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    }
    ~Eng() { if (e) { swmm_engine_close(e); swmm_engine_destroy(e); } }
    int open_rc = -1;
    double head(int tri, int edge) const {
        double v = std::nan("");
        swmm_2d_get_edge_bc_head(e, tri, edge, &v);
        return v;
    }
    double flow(int tri, int edge) const {
        double v = std::nan("");
        swmm_2d_get_edge_bc_flow(e, tri, edge, &v);
        return v;
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Constant stage / flow: scaled once at initialize, written back in display
// units, no double conversion on reopen or on a repeated initialize().
// ---------------------------------------------------------------------------

TEST(BcUnits2D, ConstantsScaleOnceAndRoundTripThroughWriter) {
    // Stage 32.808 ft = 10.0 m; flow 1.0 CFS/m = 0.028317 m³/s/m.
    const std::string inp = writeInp("const_us",
        buildModel(false,
                   "0 2 SPECIFIED_STAGE 32.808\n"
                   "1 2 SPECIFIED_FLOW  1.0\n"));

    Eng a(inp, "const_us");
    ASSERT_EQ(a.open_rc, SWMM_OK);
    // Pre-initialize: values are still the authored display units.
    EXPECT_NEAR(a.head(0, 2), 32.808, 1e-9);
    EXPECT_NEAR(a.flow(1, 2), 1.0, 1e-12);

    ASSERT_EQ(swmm_engine_initialize(a.e), SWMM_OK);
    EXPECT_NEAR(a.head(0, 2), 32.808 * kFtToM, 1e-9);
    EXPECT_NEAR(a.flow(1, 2), 1.0 * kCfsToCms, 1e-12);

    // Repeated initialize must not scale the constants again.
    ASSERT_EQ(swmm_engine_initialize(a.e), SWMM_OK);
    EXPECT_NEAR(a.head(0, 2), 32.808 * kFtToM, 1e-9);
    EXPECT_NEAR(a.flow(1, 2), 1.0 * kCfsToCms, 1e-12);

    // Writer: heads go out in SI under the SI header; flows go out in
    // display units (there is no SI header for flows).
    const std::string saved = (kOutDir / "const_us_saved.inp").string();
    ASSERT_EQ(swmm_model_write(a.e, saved.c_str()), SWMM_OK);
    const std::string text = readAll(saved);
    EXPECT_NE(text.find("UNITS: SI (m)"), std::string::npos)
        << "post-initialize save must tag the (now metric) mesh";
    EXPECT_NE(text.find("SPECIFIED_FLOW"), std::string::npos);
    // "1" (display CFS/m), not 0.0283...
    EXPECT_EQ(text.find("0.0283"), std::string::npos)
        << "constant SPECIFIED_FLOW was written in m³/s/m instead of CFS/m:\n" << text;

    // Reopen the saved file: values must come back identical after initialize.
    Eng b(saved, "const_us_saved");
    ASSERT_EQ(b.open_rc, SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(b.e), SWMM_OK);
    EXPECT_NEAR(b.head(0, 2), 32.808 * kFtToM, 1e-6)
        << "constant stage double-converted on reopen";
    EXPECT_NEAR(b.flow(1, 2), 1.0 * kCfsToCms, 1e-9)
        << "constant flow double-converted on reopen";
}

// ---------------------------------------------------------------------------
// Time-series stage follows FLOW_UNITS even when the mesh is tagged SI.
// ---------------------------------------------------------------------------

namespace {
double tsStageAfterOneStep(const std::string& stem, bool si_header) {
    // Series value 32.808 (feet — project units) on the bottom edge.
    const std::string inp = writeInp(stem,
        buildModel(si_header,
                   "0 2 TS_STAGE StageTS\n",
                   "\n[TIMESERIES]\n"
                   "StageTS  0:00  32.808\n"
                   "StageTS  1:00  32.808\n"));
    Eng e(inp, stem);
    if (e.open_rc != SWMM_OK) return std::nan("");
    if (swmm_engine_initialize(e.e) != SWMM_OK) return std::nan("");
    if (swmm_engine_start(e.e, 0) != SWMM_OK) return std::nan("");
    double elapsed = 0.0;
    if (swmm_engine_step(e.e, &elapsed) != SWMM_OK) return std::nan("");
    const double h = e.head(0, 2);
    swmm_engine_end(e.e);
    return h;
}
} // namespace

TEST(BcUnits2D, TsStageIsProjectFeetOnUntaggedMesh) {
    const double h = tsStageAfterOneStep("ts_stage_ft_mesh", false);
    ASSERT_FALSE(std::isnan(h));
    EXPECT_NEAR(h, 32.808 * kFtToM, 1e-6);   // 10.0 m
}

TEST(BcUnits2D, TsStageIsStillProjectFeetOnSiTaggedMesh) {
    // The mesh header says the MESH is metres; the [TIMESERIES] is still 1D
    // project data in feet. Before the fix this read 32.808 as metres.
    const double h = tsStageAfterOneStep("ts_stage_si_mesh", true);
    ASSERT_FALSE(std::isnan(h));
    EXPECT_NEAR(h, 32.808 * kFtToM, 1e-6)
        << "TS_STAGE value taken as metres because the mesh was tagged SI";
}

// ---------------------------------------------------------------------------
// Constant stage under an SI header is NOT rescaled (already metres).
// ---------------------------------------------------------------------------

TEST(BcUnits2D, ConstantStageUnderSiHeaderIsAlreadyMetres) {
    const std::string inp = writeInp("const_si_mesh",
        buildModel(true, "0 2 SPECIFIED_STAGE 10.0\n"));
    Eng e(inp, "const_si_mesh");
    ASSERT_EQ(e.open_rc, SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e.e), SWMM_OK);
    EXPECT_NEAR(e.head(0, 2), 10.0, 1e-9);
}
