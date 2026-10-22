/**
 * @file      test_2d_bc_units.cpp
 * @brief     Unit conversions of 2D boundary-condition data exchanged with the
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
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
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

constexpr double kFtToM = 0.3048;
constexpr double kCfsToCms = 0.028316846592;

const fs::path kOutDir = fs::path(OPENSWMM_BC_UNITS_TEST_OUT_DIR) / "bc_units_2d";

/**
 * @brief Construct a four-triangle US-project deck for boundary unit conversions.
 *
 * The FLOW_UNITS remain CFS in both mesh-header variants. Only constructs text; deliberately does
 * not convert stage or per-metre flow values before parser/initialization tests.
 *
 * @param[in] mesh_si_header Add the SI mesh-coordinate header when true; otherwise
 *     coordinates/elevations follow US mesh units.
 * @param[in] bc_rows Raw 2D_BOUNDARY_CONDITIONS data rows with caller-supplied indexing/type/value
 *     tokens.
 * @param[in] extra_sections Raw additional sections, typically time series or rating curves,
 *     appended verbatim.
 * @return Owned ten-minute CFS/DYNWAVE deck with numeric mesh coordinates0..20 and bed10 in the
 *     selected mesh units.
 */
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
    if (mesh_si_header)
        s << ";; UNITS: SI (m)\n";
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
      << bc_rows << "\n"
      << extra_sections;
    return s.str();
}

/**
 * @brief Write a boundary-units input artifact beneath the configured output directory.
 *
 * Uses error-code create_directories and unchecked stream open/write/close; existing input is
 * truncated. Does not parse or run the deck.
 *
 * @param[in] name Filesystem-safe basename without extension.
 * @param[in] text Complete deck text inserted verbatim.
 * @return Owned .inp path, regardless of ignored directory creation or stream failure.
 */
std::string writeInp(const std::string& name, const std::string& text) {
    std::error_code ec;
    fs::create_directories(kOutDir, ec);
    const fs::path p = kOutDir / (name + ".inp");
    std::ofstream f(p);
    f << text;
    return p.string();
}

/**
 * @brief Collect an entire fixture file into owned text.
 *
 * Uses ordinary text mode and its platform newline rules. The local stream closes on return; no
 * model state is touched and no borrowed buffer is returned. Default stream exception settings
 * apply; allocation exceptions may propagate.
 *
 * @param[in] path Borrowed input filename used for this synchronous open/read.
 * @return Owned string collected from the stream; empty can mean an empty file or failure to open.
 *     Partial read failures have no separate status.
 */
std::string readAll(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

struct Eng {
    SWMM_Engine e = nullptr;
    /**
     * @brief Create/open a boundary-units fixture while retaining its raw open status.
     *
     * Stores the created handle even when open fails and sets open_rc to that status. Does not
     * independently check create, initialize or start; destructor owns cleanup. Avoid copying this
     * handle owner.
     *
     * @param[in] inp Readable input-deck path.
     * @param[in] stem Filesystem-safe basename for .rpt/.out files under kOutDir, whose directory
     *     already exists.
     */
    explicit Eng(const std::string& inp, const std::string& stem) {
        e = swmm_engine_create();
        const std::string rpt = (kOutDir / (stem + ".rpt")).string();
        const std::string out = (kOutDir / (stem + ".out")).string();
        open_rc = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    }
    /**
     * @brief Close/destroy a non-null boundary-units fixture engine.
     *
     * Ignores close status; any borrowed state must not outlive this scope. Does not call engine_end
     * separately or delete review artifacts.
     */
    ~Eng() {
        if (e) {
            swmm_engine_close(e);
            swmm_engine_destroy(e);
        }
    }
    int open_rc = -1;
    /**
     * @brief Read stored stage-head boundary data without asserting API status.
     *
     * Requires a live parsed engine; ignores getter status and does not resolve time-series values or
     * advance the solver itself.
     *
     * @param[in] tri Valid zero-based cell index in the fixture mesh.
     * @param[in] edge Valid zero-based local boundary edge index.
     * @return Stage in current authored mesh units before initialization and metres afterward; NaN
     *     remains if the API does not assign the initialized result.
     */
    double head(int tri, int edge) const {
        double v = std::nan("");
        swmm_2d_get_edge_bc_head(e, tri, edge, &v);
        return v;
    }
    /**
     * @brief Read stored per-metre boundary-flow data without asserting API status.
     *
     * Ignores getter status; does not modify boundaries, resolve signals or run a timestep.
     *
     * @param[in] tri Valid zero-based cell index in the fixture mesh.
     * @param[in] edge Valid zero-based local boundary edge index.
     * @return Flow in CFS/m before fixture initialization and m3/s/m afterward; NaN if the API leaves
     *     the initialized result unchanged.
     */
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
    const std::string inp = writeInp("const_us", buildModel(false, "0 2 SPECIFIED_STAGE 32.808\n"
                                                                   "1 2 SPECIFIED_FLOW  1.0\n"));

    Eng a(inp, "const_us");
    ASSERT_EQ(a.open_rc, SWMM_OK);
    // Pre-initialize: values are still the authored display units. In the
    // freshly OPENED state BoundaryData is unsized until the pending rows
    // are drained (SVBC round A8 — the getters refuse cleanly before that),
    // so drain them the way the GUI does before its opened-state reads.
    ASSERT_EQ(swmm_2d_prepare_for_edit(a.e), SWMM_OK);
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
        << "constant SPECIFIED_FLOW was written in m³/s/m instead of CFS/m:\n"
        << text;

    // Reopen the saved file: values must come back identical after initialize.
    Eng b(saved, "const_us_saved");
    ASSERT_EQ(b.open_rc, SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(b.e), SWMM_OK);
    EXPECT_NEAR(b.head(0, 2), 32.808 * kFtToM, 1e-6) << "constant stage double-converted on reopen";
    EXPECT_NEAR(b.flow(1, 2), 1.0 * kCfsToCms, 1e-9) << "constant flow double-converted on reopen";
}

// ---------------------------------------------------------------------------
// Time-series stage follows FLOW_UNITS even when the mesh is tagged SI.
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Run a stage-series fixture until its lagging surface window resolves a nonzero head.
 *
 * Writes constant StageTS32.808 ft, starts with output disabled and steps at most50 times, stopping
 * when head!=0 or elapsed<=0. Calls end only after the loop; scoped Eng always closes/destroys.
 * Despite its historical name this may perform multiple steps, and does not assert success
 * internally.
 *
 * @param[in] stem Filesystem-safe input/report/output artifact basename under kOutDir.
 * @param[in] si_header Select SI-tagged mesh coordinates while retaining project CFS/feet signal
 *     units.
 * @return Resolved stage head in metres after initialization; NaN on open/init/start/step error, or
 *     possibly zero after completion/50-call bound without resolution.
 */
double tsStageAfterOneStep(const std::string& stem, bool si_header) {
    // Series value 32.808 (feet — project units) on the bottom edge.
    const std::string inp = writeInp(stem, buildModel(si_header, "0 2 TS_STAGE StageTS\n",
                                                      "\n[TIMESERIES]\n"
                                                      "StageTS  0:00  32.808\n"
                                                      "StageTS  1:00  32.808\n"));
    Eng e(inp, stem);
    if (e.open_rc != SWMM_OK)
        return std::nan("");
    if (swmm_engine_initialize(e.e) != SWMM_OK)
        return std::nan("");
    if (swmm_engine_start(e.e, 0) != SWMM_OK)
        return std::nan("");
    // The 2D solver advances in sync windows that lag the 1D clock by one
    // step, and a TS-driven head is written by resolveBoundaryValues() only
    // when a window actually runs — after a single 1D step the slot still
    // holds its drained 0.0. Step until the window has fired (bounded so a
    // genuinely broken lookup still fails the test instead of hanging).
    double h = 0.0;
    for (int k = 0; k < 50; ++k) {
        double elapsed = 0.0;
        if (swmm_engine_step(e.e, &elapsed) != SWMM_OK)
            return std::nan("");
        h = e.head(0, 2);
        if (h != 0.0 || elapsed <= 0.0)
            break;
    }
    swmm_engine_end(e.e);
    return h;
}
} // namespace

TEST(BcUnits2D, TsStageIsProjectFeetOnUntaggedMesh) {
    const double h = tsStageAfterOneStep("ts_stage_ft_mesh", false);
    ASSERT_FALSE(std::isnan(h));
    EXPECT_NEAR(h, 32.808 * kFtToM, 1e-6); // 10.0 m
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
    const std::string inp =
        writeInp("const_si_mesh", buildModel(true, "0 2 SPECIFIED_STAGE 10.0\n"));
    Eng e(inp, "const_si_mesh");
    ASSERT_EQ(e.open_rc, SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e.e), SWMM_OK);
    EXPECT_NEAR(e.head(0, 2), 10.0, 1e-9);
}
