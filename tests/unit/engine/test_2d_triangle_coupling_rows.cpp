/**
 * @file test_2d_triangle_coupling_rows.cpp
 * @brief Repeated-row `[2D_TRIANGLE_NODE_MAP]` — node→cell couplings.
 *
 * @details A triangle may carry SEVERAL coupling rows, one per node (the
 *          weir/orifice-endpoint case: two 1D nodes land in one 2D cell).
 *          Before MESH-REMAP-01 the parser overwrote per-triangle arrays,
 *          so the last line won and every earlier coupling was silently
 *          dropped. Covers:
 *            1. two rows for the same triangle parse → 2 coupling rows,
 *               and `InpWriter` emits BOTH on save (round-trip);
 *            2. the new C API (`swmm_2d_add_triangle_coupling`,
 *               `_clear_triangle_couplings`, `_triangle_coupling_rows`,
 *               `_get_triangle_coupling_row`) — append semantics, error
 *               paths, survival across a short run and a model write;
 *            3. the legacy single-row path still behaves identically.
 *
 *          Registered only when OPENSWMM_BUILD_2D=ON.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_2d.h>
#include <openswmm/engine/openswmm_nodes.h>

namespace fs = std::filesystem;

namespace {

// Two junctions draining to one outfall — J1/J2 are the "weir endpoints"
// that both sit inside 2D cell 0. CMS keeps the mesh in metres so AREA
// needs no unit conversion.
const char* k1DBase = R"INP(
[OPTIONS]
FLOW_UNITS           CMS
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
END_DATE             01/01/2026
END_TIME             00:10:00
REPORT_STEP          00:01:00
ROUTING_STEP         5

[JUNCTIONS]
;;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded
J1      10    3         0          0         0
J2      9.8   3         0          0         0

[OUTFALLS]
;;Name  Elev  Type  Gated
O1      9     FREE  NO

[CONDUITS]
;;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow
C1      J1    J2  50      0.013      0         0          0
C2      J2    O1  100     0.013      0         0          0

[XSECTIONS]
;;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR  1      0      0      0      1
C2      CIRCULAR  1      0      0      0      1
)INP";

const char* k2DMesh = R"INP(
[2D_OPTIONS]
MAX_TIMESTEP        5
DRY_DEPTH           0.002
LINEAR_SOLVER       GMRES
PRECONDITIONER      JACOBI
REPORT_2D           NO

[2D_VERTICES]
;;X     Y     Z
0       0     10
10      0     10.5
10      10    11
0       10    11.5

[2D_TRIANGLES]
;;V1  V2  V3  MANNINGS_N
0     1   2   0.03
0     2   3   0.045
)INP";

// Repeated-row form: TWO nodes coupled to triangle 0.
const char* kTwoRowsSameTriangle = R"INP(
[2D_TRIANGLE_NODE_MAP]
;;TRIANGLE NODE  CD    AREA
0          J1    0.65  2
0          J2    0.65  2
)INP";

void write_file(const fs::path& p, const std::string& text) {
    std::ofstream f(p);
    f << text;
}

std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// Count data rows in the [2D_TRIANGLE_NODE_MAP] section of an .inp text.
int count_triangle_map_rows(const std::string& text) {
    const std::string sec = "[2D_TRIANGLE_NODE_MAP]";
    auto pos = text.find(sec);
    if (pos == std::string::npos) return -1;
    std::istringstream in(text.substr(pos + sec.size()));
    std::string line;
    int rows = 0;
    while (std::getline(in, line)) {
        auto b = line.find_first_not_of(" \t\r");
        if (b == std::string::npos) continue;
        if (line[b] == '[') break;              // next section
        if (line.compare(b, 1, ";") == 0) continue;
        ++rows;
    }
    return rows;
}

} // namespace

class TriangleCouplingRowsTest : public ::testing::Test {
protected:
    fs::path dir_;
    SWMM_Engine eng_ = nullptr;
    SWMM_Engine eng2_ = nullptr;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "openswmm_tri_coupling_rows_test";
        fs::create_directories(dir_);
    }

    void TearDown() override {
        if (eng_)  { swmm_engine_close(eng_);  swmm_engine_destroy(eng_); }
        if (eng2_) { swmm_engine_close(eng2_); swmm_engine_destroy(eng2_); }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    SWMM_Engine open_engine(const fs::path& inp) {
        SWMM_Engine e = swmm_engine_create();
        EXPECT_NE(e, nullptr);
        EXPECT_EQ(swmm_engine_open(e, inp.string().c_str(), "", "", nullptr), 0)
            << "open failed for " << inp;
        return e;
    }
};

// ---------------------------------------------------------------------------
// 1. Parse + write round-trip: two rows for triangle 0 survive the save.
//    (Previously last-line-wins: J1 was dropped and only J2 was written.)
// ---------------------------------------------------------------------------

TEST_F(TriangleCouplingRowsTest, TwoRowsOnOneTriangleRoundTripThroughInp) {
    const fs::path inp_a = dir_ / "two_rows.inp";
    write_file(inp_a, std::string(k1DBase) + k2DMesh + kTwoRowsSameTriangle);

    eng_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_), 0);

    int rows = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 2) << "both [2D_TRIANGLE_NODE_MAP] rows must be kept";

    const int j1 = swmm_node_index(eng_, "J1");
    const int j2 = swmm_node_index(eng_, "J2");
    ASSERT_GE(j1, 0);
    ASSERT_GE(j2, 0);

    int  tri = -1, node = -1;
    double cd = 0.0, area = 0.0;
    ASSERT_EQ(swmm_2d_get_triangle_coupling_row(eng_, 0, &tri, &node, &cd, &area), 0);
    EXPECT_EQ(tri, 0);
    EXPECT_EQ(node, j1);
    EXPECT_NEAR(cd,   0.65, 1e-12);
    EXPECT_NEAR(area, 2.0,  1e-12);
    ASSERT_EQ(swmm_2d_get_triangle_coupling_row(eng_, 1, &tri, &node, &cd, &area), 0);
    EXPECT_EQ(tri, 0);
    EXPECT_EQ(node, j2);

    // Write → both rows must appear in the emitted section.
    const fs::path inp_b = dir_ / "two_rows_out.inp";
    ASSERT_EQ(swmm_model_write(eng_, inp_b.string().c_str()), 0);
    const std::string text = read_file(inp_b);
    EXPECT_EQ(count_triangle_map_rows(text), 2) << text;

    // Reopen: the row count survives the full round trip.
    eng2_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng2_), 0);
    int rows_b = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng2_, &rows_b), 0);
    EXPECT_EQ(rows_b, 2);
}

// ---------------------------------------------------------------------------
// 2. Legacy single-row files behave exactly as before (mirror arrays kept).
// ---------------------------------------------------------------------------

TEST_F(TriangleCouplingRowsTest, LegacySingleRowStillMirrorsToPerTriangleGetter) {
    const fs::path inp = dir_ / "one_row.inp";
    write_file(inp, std::string(k1DBase) + k2DMesh +
                        "\n[2D_TRIANGLE_NODE_MAP]\n1  J2  0.7  3\n");

    eng_ = open_engine(inp);
    ASSERT_EQ(swmm_engine_initialize(eng_), 0);

    int rows = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 1);

    const int j2 = swmm_node_index(eng_, "J2");
    ASSERT_GE(j2, 0);

    // The legacy per-triangle accessor still resolves (last-row-wins mirror).
    int coupled = -2;
    ASSERT_EQ(swmm_2d_triangle_get_coupled_node(eng_, 1, &coupled), 0);
    EXPECT_EQ(coupled, j2);
    // Triangle 0 carries no coupling.
    ASSERT_EQ(swmm_2d_triangle_get_coupled_node(eng_, 0, &coupled), 0);
    EXPECT_LT(coupled, 0);
}

// ---------------------------------------------------------------------------
// 3. C API — add appends (two nodes on one triangle), survives a short run
//    and a model write, and the reopened model still carries both rows.
// ---------------------------------------------------------------------------

TEST_F(TriangleCouplingRowsTest, AddApiAppendsAndSurvivesRunAndWrite) {
    const fs::path inp_a = dir_ / "api_add.inp";
    write_file(inp_a, std::string(k1DBase) + k2DMesh);   // no coupling authored

    eng_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_), 0);

    int rows = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    ASSERT_EQ(rows, 0);

    // Two nodes → the SAME cell (what the GUI's Remap authors for a
    // weir pair whose endpoints fall in one triangle).
    ASSERT_EQ(swmm_2d_add_triangle_coupling(eng_, 0, "J1", 0.65, 2.0), 0);
    ASSERT_EQ(swmm_2d_add_triangle_coupling(eng_, 0, "J2", 0.65, 2.0), 0);
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 2);

    const int j1 = swmm_node_index(eng_, "J1");
    const int j2 = swmm_node_index(eng_, "J2");
    ASSERT_GE(j1, 0);
    ASSERT_GE(j2, 0);
    int tri = -1, node = -1; double cd = 0.0, area = 0.0;
    ASSERT_EQ(swmm_2d_get_triangle_coupling_row(eng_, 0, &tri, &node, &cd, &area), 0);
    EXPECT_EQ(tri, 0);
    EXPECT_EQ(node, j1) << "node name must resolve at add time";
    ASSERT_EQ(swmm_2d_get_triangle_coupling_row(eng_, 1, &tri, &node, &cd, &area), 0);
    EXPECT_EQ(node, j2);

    // A short run must accept two coupling points sharing one cell.
    ASSERT_EQ(swmm_engine_start(eng_, 1), 0);
    double elapsed = 0.0;
    for (int i = 0; i < 20; ++i) {
        ASSERT_EQ(swmm_engine_step(eng_, &elapsed), 0) << "step " << i;
        if (elapsed <= 0.0) break;
    }
    ASSERT_EQ(swmm_engine_end(eng_), 0);

    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 2) << "rows must survive the run";

    const fs::path inp_b = dir_ / "api_add_out.inp";
    ASSERT_EQ(swmm_model_write(eng_, inp_b.string().c_str()), 0);
    EXPECT_EQ(count_triangle_map_rows(read_file(inp_b)), 2);

    eng2_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng2_), 0);
    int rows_b = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng2_, &rows_b), 0);
    EXPECT_EQ(rows_b, 2);
}

// ---------------------------------------------------------------------------
// 4. C API — clear drops every row (and the legacy mirror), so a re-author
//    from the GUI does not accumulate duplicates.
// ---------------------------------------------------------------------------

TEST_F(TriangleCouplingRowsTest, ClearRemovesRowsAndLegacyMirror) {
    const fs::path inp = dir_ / "api_clear.inp";
    write_file(inp, std::string(k1DBase) + k2DMesh + kTwoRowsSameTriangle);

    eng_ = open_engine(inp);
    ASSERT_EQ(swmm_engine_initialize(eng_), 0);

    int rows = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    ASSERT_EQ(rows, 2);

    ASSERT_EQ(swmm_2d_clear_triangle_couplings(eng_), 0);
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 0);

    int coupled = -2;
    ASSERT_EQ(swmm_2d_triangle_get_coupled_node(eng_, 0, &coupled), 0);
    EXPECT_LT(coupled, 0) << "legacy mirror must be cleared too";

    // Re-author one row — the count reflects only the new authoring.
    ASSERT_EQ(swmm_2d_add_triangle_coupling(eng_, 1, "J2", 0.8, 4.5), 0);
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 1);

    const fs::path out = dir_ / "api_clear_out.inp";
    ASSERT_EQ(swmm_model_write(eng_, out.string().c_str()), 0);
    EXPECT_EQ(count_triangle_map_rows(read_file(out)), 1);
}

// ---------------------------------------------------------------------------
// 5. C API — error paths.
// ---------------------------------------------------------------------------

TEST_F(TriangleCouplingRowsTest, ApiRejectsBadArguments) {
    const fs::path inp = dir_ / "api_errors.inp";
    write_file(inp, std::string(k1DBase) + k2DMesh);

    eng_ = open_engine(inp);
    ASSERT_EQ(swmm_engine_initialize(eng_), 0);

    EXPECT_NE(swmm_2d_add_triangle_coupling(eng_, -1, "J1", 0.65, 2.0), 0);
    EXPECT_NE(swmm_2d_add_triangle_coupling(eng_, 99, "J1", 0.65, 2.0), 0);
    EXPECT_NE(swmm_2d_add_triangle_coupling(eng_, 0, "",   0.65, 2.0), 0);
    EXPECT_NE(swmm_2d_add_triangle_coupling(eng_, 0, nullptr, 0.65, 2.0), 0);
    EXPECT_NE(swmm_2d_add_triangle_coupling(eng_, 0, "J1", 0.0,  2.0), 0);
    EXPECT_NE(swmm_2d_add_triangle_coupling(eng_, 0, "J1", 0.65, 0.0), 0);

    int rows = -1;
    ASSERT_EQ(swmm_2d_triangle_coupling_rows(eng_, &rows), 0);
    EXPECT_EQ(rows, 0) << "no rejected call may have appended a row";
    EXPECT_NE(swmm_2d_triangle_coupling_rows(eng_, nullptr), 0);

    int tri = -1, node = -1; double cd = 0.0, area = 0.0;
    EXPECT_NE(swmm_2d_get_triangle_coupling_row(eng_, 0, &tri, &node, &cd, &area), 0)
        << "no rows authored yet";
    ASSERT_EQ(swmm_2d_add_triangle_coupling(eng_, 0, "J1", 0.65, 2.0), 0);
    EXPECT_NE(swmm_2d_get_triangle_coupling_row(eng_, -1, &tri, &node, &cd, &area), 0);
    EXPECT_NE(swmm_2d_get_triangle_coupling_row(eng_, 1, &tri, &node, &cd, &area), 0);
    EXPECT_NE(swmm_2d_get_triangle_coupling_row(eng_, 0, nullptr, &node, &cd, &area), 0);
}
