/**
 * @file test_inp_writer_2d.cpp
 * @brief End-to-end tests for InpWriter [2D_*] section emission.
 *
 * @details Drives everything through the public C API: open a 2D .inp,
 *          (optionally) initialize, swmm_model_write, reopen the emitted
 *          file in a fresh engine, and compare the 2D model via the
 *          swmm_2d_* accessors. Covers inline round-trip, the
 *          `;; UNITS: SI (m)` header for post-initialize US-unit saves,
 *          external [2D_MESH_FILE] reference preservation, the no-2D
 *          no-op, and the GeoPackage write→reopen end-to-end.
 *
 *          Registered only when OPENSWMM_BUILD_2D=ON (the engine must
 *          carry the 2D module for the [2D_*] parse + initialize path).
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>

#include <cstdio>
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

const char* k1DBase = R"INP(
[OPTIONS]
FLOW_UNITS           {FLOW_UNITS}
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2026
START_TIME           00:00:00
END_DATE             01/01/2026
END_TIME             01:00:00
REPORT_STEP          00:05:00
ROUTING_STEP         5

[JUNCTIONS]
;;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded
J1      10    3         0          0         0

[OUTFALLS]
;;Name  Elev  Type  Gated
O1      9     FREE  NO

[CONDUITS]
;;Name  From  To  Length  Roughness  InOffset  OutOffset  InitFlow
C1      J1    O1  100     0.013      0         0          0

[XSECTIONS]
;;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR  1      0      0      0      1
)INP";

const char* k2DSections = R"INP(
[2D_OPTIONS]
MAX_TIMESTEP        5
DRY_DEPTH           0.002
COUPLING_CD         0.7
REPORT_2D           NO

[2D_VERTICES]
;;X     Y     Z
0       0     10
10      0     10.5
10      10    11
0       10    11.5

[2D_TRIANGLES]
;;V1  V2  V3  MANNINGS_N  INIT_DEPTH  TAG
0     1   2   0.03        0.25        wetcell
0     2   3   0.045

[2D_VERTEX_NODE_MAP]
0  J1  0.7  2.5

[2D_BOUNDARY_CONDITIONS]
;;TRI EDGE TYPE          PARAM_1
0     0    NORMAL_FLOW   0.01

[2D_EDGE_CONVEYANCE]
;;FROM TO  CONVEYANCE
0      2   0.5
)INP";

std::string replace(std::string s, const std::string& from, const std::string& to) {
    auto pos = s.find(from);
    if (pos != std::string::npos) s.replace(pos, from.size(), to);
    return s;
}

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

} // namespace

class InpWriter2DTest : public ::testing::Test {
protected:
    fs::path dir_;
    SWMM_Engine eng_a_ = nullptr;
    SWMM_Engine eng_b_ = nullptr;

    void SetUp() override {
        dir_ = fs::temp_directory_path() / "openswmm_inpwriter2d_test";
        fs::create_directories(dir_);
    }

    void TearDown() override {
        if (eng_a_) { swmm_engine_close(eng_a_); swmm_engine_destroy(eng_a_); }
        if (eng_b_) { swmm_engine_close(eng_b_); swmm_engine_destroy(eng_b_); }
        std::error_code ec;
        fs::remove_all(dir_, ec);
    }

    SWMM_Engine open_engine(const fs::path& inp) {
        SWMM_Engine e = swmm_engine_create();
        EXPECT_NE(e, nullptr);
        int rc = swmm_engine_open(e, inp.string().c_str(), "", "", nullptr);
        EXPECT_EQ(rc, 0) << "open failed for " << inp;
        return e;
    }

    // Compare the full 2D model definition of two initialized engines.
    void expect_same_2d_model(SWMM_Engine a, SWMM_Engine b) {
        int nva = -1, nvb = -1, nta = -1, ntb = -1;
        ASSERT_EQ(swmm_2d_vertex_count(a, &nva), 0);
        ASSERT_EQ(swmm_2d_vertex_count(b, &nvb), 0);
        ASSERT_EQ(swmm_2d_triangle_count(a, &nta), 0);
        ASSERT_EQ(swmm_2d_triangle_count(b, &ntb), 0);
        ASSERT_EQ(nva, nvb);
        ASSERT_EQ(nta, ntb);

        for (int i = 0; i < nva; ++i) {
            double xa, ya, za, xb, yb, zb;
            ASSERT_EQ(swmm_2d_vertex_get_xyz(a, i, &xa, &ya, &za), 0);
            ASSERT_EQ(swmm_2d_vertex_get_xyz(b, i, &xb, &yb, &zb), 0);
            EXPECT_NEAR(xa, xb, 1e-9) << "vx[" << i << "]";
            EXPECT_NEAR(ya, yb, 1e-9) << "vy[" << i << "]";
            EXPECT_NEAR(za, zb, 1e-9) << "vz[" << i << "]";
        }
        for (int t = 0; t < nta; ++t) {
            int va[3], vb[3], na[3], nb[3];
            double ma, mb;
            ASSERT_EQ(swmm_2d_triangle_get_vertices(a, t, &va[0], &va[1], &va[2]), 0);
            ASSERT_EQ(swmm_2d_triangle_get_vertices(b, t, &vb[0], &vb[1], &vb[2]), 0);
            ASSERT_EQ(swmm_2d_triangle_get_mannings(a, t, &ma), 0);
            ASSERT_EQ(swmm_2d_triangle_get_mannings(b, t, &mb), 0);
            double da = -1.0, db = -1.0;
            ASSERT_EQ(swmm_2d_triangle_get_init_depth(a, t, &da), 0);
            ASSERT_EQ(swmm_2d_triangle_get_init_depth(b, t, &db), 0);
            EXPECT_NEAR(da, db, 1e-12) << "init_depth tri " << t;
            ASSERT_EQ(swmm_2d_triangle_get_neighbours(a, t, &na[0], &na[1], &na[2]), 0);
            ASSERT_EQ(swmm_2d_triangle_get_neighbours(b, t, &nb[0], &nb[1], &nb[2]), 0);
            for (int k = 0; k < 3; ++k) {
                EXPECT_EQ(va[k], vb[k]) << "tri " << t << " v" << k;
                EXPECT_EQ(na[k], nb[k]) << "tri " << t << " nbr" << k;
            }
            EXPECT_NEAR(ma, mb, 1e-12) << "mannings tri " << t;
            for (int e = 0; e < 3; ++e) {
                double ka, kb;
                ASSERT_EQ(swmm_2d_get_edge_conveyance(a, t, e, &ka), 0);
                ASSERT_EQ(swmm_2d_get_edge_conveyance(b, t, e, &kb), 0);
                EXPECT_NEAR(ka, kb, 1e-12) << "conveyance " << t << "/" << e;
            }
        }
    }
};

// ---------------------------------------------------------------------------
// Inline round-trip: open → init → write → reopen → init → identical model
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, InlineRoundTrip) {
    const fs::path inp_a = dir_ / "a.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") + k2DSections);

    eng_a_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0);
    int active = 0;
    ASSERT_EQ(swmm_2d_is_active(eng_a_, &active), 0);
    ASSERT_EQ(active, 1);

    const fs::path inp_b = dir_ / "b.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);
    ASSERT_EQ(swmm_2d_is_active(eng_b_, &active), 0);
    ASSERT_EQ(active, 1);

    expect_same_2d_model(eng_a_, eng_b_);

    // Coupling survived the round-trip.
    int node_a = -2, node_b = -2;
    ASSERT_EQ(swmm_2d_vertex_get_coupled_node(eng_a_, 0, &node_a), 0);
    ASSERT_EQ(swmm_2d_vertex_get_coupled_node(eng_b_, 0, &node_b), 0);
    EXPECT_GE(node_a, 0);
    EXPECT_EQ(node_a, node_b);

    // The BC section survived too (textual check — the only BC C-API-free
    // signal is the emitted section itself).
    const std::string text = read_file(inp_b);
    EXPECT_NE(text.find("[2D_BOUNDARY_CONDITIONS]"), std::string::npos);
    EXPECT_NE(text.find("NORMAL_FLOW"), std::string::npos);
    EXPECT_NE(text.find("[2D_EDGE_CONVEYANCE]"), std::string::npos);
    // The INIT_DEPTH column is written (fixture triangle 0 has 0.25 m).
    EXPECT_NE(text.find("INIT_DEPTH"), std::string::npos);
}

// ---------------------------------------------------------------------------
// INIT_DEPTH seeds the solver state: total volume = depth * area at start,
// and the mass-balance ledger opens with it as initial storage (not error).
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, InitDepthSeedsVolume) {
    const fs::path inp = dir_ / "seed.inp";
    write_file(inp, replace(k1DBase, "{FLOW_UNITS}", "CMS") + k2DSections);

    eng_a_ = open_engine(inp);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0);
    int active = 0;
    ASSERT_EQ(swmm_2d_is_active(eng_a_, &active), 0);
    ASSERT_EQ(active, 1);

    double d0 = -1.0, d1 = -1.0;
    ASSERT_EQ(swmm_2d_triangle_get_init_depth(eng_a_, 0, &d0), 0);
    ASSERT_EQ(swmm_2d_triangle_get_init_depth(eng_a_, 1, &d1), 0);
    EXPECT_NEAR(d0, 0.25, 1e-12);
    EXPECT_NEAR(d1, 0.0, 1e-12);

    double a0 = 0.0, vol = -1.0;
    ASSERT_EQ(swmm_2d_triangle_get_area(eng_a_, 0, &a0), 0);
    ASSERT_EQ(swmm_2d_get_total_volume(eng_a_, &vol), 0);
    EXPECT_NEAR(vol, 0.25 * a0, 1e-9)
        << "initial volume must equal init_depth * area of the wet triangle";
}

// ---------------------------------------------------------------------------
// US-units project saved post-initialize: SI header prevents double-scaling
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, UnitsHeaderPreventsDoubleScaling) {
    const fs::path inp_a = dir_ / "us.inp";
    // Mesh coordinates are in feet (no UNITS header); initialize() scales
    // them to SI in place.
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CFS") + k2DSections);

    eng_a_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0);

    const fs::path inp_b = dir_ / "us_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    // The emitted mesh is in SI metres now — the units header must say so.
    const std::string text = read_file(inp_b);
    EXPECT_NE(text.find(";; UNITS: SI (m)"), std::string::npos);

    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);

    // Both engines hold the same (SI) coordinates — no double scaling.
    expect_same_2d_model(eng_a_, eng_b_);
}

// ---------------------------------------------------------------------------
// Pure-1D model emits no [2D_*] text at all
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, No2DModelEmitsNothing) {
    const fs::path inp_a = dir_ / "plain.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS"));

    eng_a_ = open_engine(inp_a);
    const fs::path inp_b = dir_ / "plain_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    EXPECT_EQ(read_file(inp_b).find("[2D_"), std::string::npos);
}

// ---------------------------------------------------------------------------
// External [2D_MESH_FILE] reference is preserved, geometry is not inlined
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, MeshFileReferencePreserved) {
    const fs::path mesh_2dm = dir_ / "mesh_ext.2dm";
    write_file(mesh_2dm, k2DSections); // .2dm uses the same section grammar

    const fs::path inp_a = dir_ / "ext.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") +
                          "\n[2D_MESH_FILE]\nFILE mesh_ext.2dm\n");

    eng_a_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0); // 2D accessors need active module
    int nv = -1;
    ASSERT_EQ(swmm_2d_vertex_count(eng_a_, &nv), 0);
    ASSERT_EQ(nv, 4);

    const fs::path inp_b = dir_ / "ext_out.inp"; // same dir → relative ref holds
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    const std::string text = read_file(inp_b);
    EXPECT_NE(text.find("[2D_MESH_FILE]"), std::string::npos);
    EXPECT_NE(text.find("mesh_ext.2dm"), std::string::npos);
    EXPECT_EQ(text.find("[2D_VERTICES]"), std::string::npos)
        << "external mode must not inline the mesh";

    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);
    int nvb = -1;
    ASSERT_EQ(swmm_2d_vertex_count(eng_b_, &nvb), 0);
    EXPECT_EQ(nvb, 4);
}

// ---------------------------------------------------------------------------
// Mesh-reference policy on Save-As to a DIFFERENT directory.
//
// Two regimes, and only one of them re-anchors:
//   * mesh loaded    → the sidecar travels with the .inp, so the reference stays
//                      a bare relative name and Save-As must NOT write back into
//                      the source folder.
//   * mesh NOT loaded → nothing is written, so the reference must be re-anchored
//                      or the saved model silently opens 1D-only.
// MeshFileReferencePreserved above writes to the same directory and so cannot
// distinguish any of this.
// ---------------------------------------------------------------------------

// Extract the FILE token from the [2D_MESH_FILE] block.
static std::string mesh_file_token(const std::string& text) {
    const auto at = text.find("[2D_MESH_FILE]");
    if (at == std::string::npos) return {};
    std::istringstream is(text.substr(at));
    std::string line;
    while (std::getline(is, line)) {
        if (line.rfind("FILE", 0) != 0) continue;
        const auto sp = line.find_first_not_of(" \t", 4);
        return (sp == std::string::npos) ? std::string{} : line.substr(sp);
    }
    return {};
}

TEST_F(InpWriter2DTest, MeshTravelsWithSaveAsAndLeavesSourceUntouched) {
    const fs::path src_dir = dir_ / "src";
    const fs::path dst_dir = dir_ / "saveas";
    fs::create_directories(src_dir);
    fs::create_directories(dst_dir);

    const fs::path mesh_2dm = src_dir / "mesh_ext.2dm";
    write_file(mesh_2dm, k2DSections);
    const std::string source_mesh_before = read_file(mesh_2dm);

    const fs::path inp_a = src_dir / "ext.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") +
                          "\n[2D_MESH_FILE]\nFILE mesh_ext.2dm\n");

    eng_a_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0);

    const fs::path inp_b = dst_dir / "ext_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    const std::string tok = mesh_file_token(read_file(inp_b));
    ASSERT_FALSE(tok.empty()) << "no FILE token in [2D_MESH_FILE]";
    EXPECT_FALSE(fs::path(tok).is_absolute())
        << "mesh reference leaked an absolute path: " << tok;
    EXPECT_EQ(tok.find(".."), std::string::npos)
        << "mesh travels with the .inp, so the reference must stay local: " << tok;

    // The sidecar came along, and the ORIGINAL was not overwritten.
    EXPECT_TRUE(fs::exists(dst_dir / "mesh_ext.2dm"))
        << "sidecar must be written next to the destination .inp";
    EXPECT_EQ(read_file(mesh_2dm), source_mesh_before)
        << "Save-As must not write back into the source directory";

    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);
    int nvb = -1;
    ASSERT_EQ(swmm_2d_vertex_count(eng_b_, &nvb), 0);
    EXPECT_EQ(nvb, 4) << "saved model lost its external mesh";
}

TEST_F(InpWriter2DTest, UnloadableMeshReferenceReanchoredOnSaveAs) {
    const fs::path src_dir = dir_ / "src_missing";
    const fs::path dst_dir = dir_ / "saveas_missing";
    fs::create_directories(src_dir);
    fs::create_directories(dst_dir);

    // Reference a .2dm that does not exist. A strict open rejects the model
    // outright; the editor path (lenient open — what the GUI uses) keeps it
    // loadable with no mesh in memory, so nothing will be written alongside
    // the destination. That is exactly the case the re-anchor exists for.
    const fs::path inp_a = src_dir / "ext.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") +
                          "\n[2D_MESH_FILE]\nFILE absent_mesh.2dm\n");

    eng_a_ = swmm_engine_create();
    ASSERT_NE(eng_a_, nullptr);
    swmm_engine_set_lenient_open(eng_a_, 1);
    ASSERT_EQ(swmm_engine_open(eng_a_, inp_a.string().c_str(), "", "", nullptr), 0);

    const fs::path inp_b = dst_dir / "ext_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    const std::string tok = mesh_file_token(read_file(inp_b));
    ASSERT_FALSE(tok.empty()) << "no FILE token in [2D_MESH_FILE]";
    EXPECT_EQ(tok.rfind("..", 0), 0u)
        << "dangling mesh reference was not re-anchored on Save-As: " << tok;
    EXPECT_NE(tok.find("absent_mesh.2dm"), std::string::npos);

    // The re-anchored token must name the ORIGINAL location, so dropping the
    // mesh back where it always was makes the saved model whole again.
    const fs::path resolved =
        fs::weakly_canonical(dst_dir / fs::path(tok));
    EXPECT_EQ(resolved, fs::weakly_canonical(src_dir / "absent_mesh.2dm"));
}

// ---------------------------------------------------------------------------
// 2D option keys set through swmm_options_set_ext reach the solver options
// and persist through swmm_model_write (GUI tab-6 wiring fix)
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, ExtOptions2DRouteToSolverAndPersist) {
    const fs::path inp_a = dir_ / "opt.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") + k2DSections);

    eng_a_ = open_engine(inp_a);

    // Set exactly like the GUI's 2D Surface Routing tab does.
    ASSERT_EQ(swmm_options_set_ext(eng_a_, "DRY_DEPTH", "0.005"), 0);
    ASSERT_EQ(swmm_options_set_ext(eng_a_, "THETA", "0.9"), 0);
    ASSERT_EQ(swmm_options_set_ext(eng_a_, "LTS_TIERS", "6"), 0);
    // Invalid values are rejected (parse2DOptionsLine validation).
    EXPECT_NE(swmm_options_set_ext(eng_a_, "DRY_DEPTH", "not_a_number"), 0);
    // Retired CVODE-stack keys are hard errors (D2, 2026-07-29).
    EXPECT_NE(swmm_options_set_ext(eng_a_, "LINEAR_SOLVER", "GMRES"), 0);
    EXPECT_NE(swmm_options_set_ext(eng_a_, "MAX_CVODE_STEPS", "500"), 0);
    EXPECT_NE(swmm_options_set_ext(eng_a_, "INTEGRATOR", "CVODE"), 0);

    // Read-back comes from the live SolverOptions2D, not a side store.
    char buf[64] = {};
    ASSERT_EQ(swmm_options_get_ext(eng_a_, "DRY_DEPTH", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "0.005");
    ASSERT_EQ(swmm_options_get_ext(eng_a_, "THETA", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "0.9");

    // The edits persist: the emitted [2D_OPTIONS] carries them...
    const fs::path inp_b = dir_ / "opt_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);
    const std::string text = read_file(inp_b);
    const auto dd = text.find("DRY_DEPTH");
    ASSERT_NE(dd, std::string::npos);
    const std::string dd_line = text.substr(dd, text.find('\n', dd) - dd);
    EXPECT_NE(dd_line.find("0.005"), std::string::npos)
        << "DRY_DEPTH line was: " << dd_line;
    // ...retired keys are never written back...
    EXPECT_EQ(text.find("LINEAR_SOLVER"), std::string::npos);
    EXPECT_EQ(text.find("MAX_CVODE_STEPS"), std::string::npos);

    // ...and survive a reload.
    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_options_get_ext(eng_b_, "DRY_DEPTH", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "0.005");
    ASSERT_EQ(swmm_options_get_ext(eng_b_, "LTS_TIERS", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "6");

    // Non-2D keys keep the generic ext_options behavior.
    ASSERT_EQ(swmm_options_set_ext(eng_a_, "MY_PLUGIN_KEY", "hello"), 0);
    ASSERT_EQ(swmm_options_get_ext(eng_a_, "MY_PLUGIN_KEY", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "hello");
}

// ---------------------------------------------------------------------------
// [2D_MESH_FILE] is settable through swmm_options_set_ext on a model whose
// engine holds NO in-memory mesh (the GUI "assign external .2dm" path). The
// reference must be emitted on save and must NOT clobber the existing .2dm,
// so a reopened model activates the 2D solver. Regression: assigning a mesh
// in the GUI silently reverted to 1D after the pre-run save dropped it.
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, MeshFileSettableViaExtApiWithoutInMemoryMesh) {
    const fs::path mesh_2dm = dir_ / "mesh_api.2dm";
    write_file(mesh_2dm, k2DSections);            // valid external mesh on disk
    const std::string mesh_before = read_file(mesh_2dm);

    // 1D-only model: the engine carries no 2D mesh in memory.
    const fs::path inp_a = dir_ / "api.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS"));
    eng_a_ = open_engine(inp_a);

    // Assign the external mesh exactly like the GUI's Set Active / generate.
    ASSERT_EQ(swmm_options_set_ext(eng_a_, "MESH_FILE", "mesh_api.2dm"), 0);
    char buf[128] = {};
    ASSERT_EQ(swmm_options_get_ext(eng_a_, "MESH_FILE", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "mesh_api.2dm");

    // Save: the reference is emitted even though no mesh is in memory, and
    // the geometry is NOT inlined.
    const fs::path inp_b = dir_ / "api_out.inp";  // same dir → relative ref holds
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);
    const std::string text = read_file(inp_b);
    EXPECT_NE(text.find("[2D_MESH_FILE]"), std::string::npos);
    EXPECT_NE(text.find("mesh_api.2dm"), std::string::npos);
    EXPECT_EQ(text.find("[2D_VERTICES]"), std::string::npos)
        << "must not inline geometry the engine does not hold";

    // The good .2dm sidecar was left untouched (never clobbered with empty).
    EXPECT_EQ(read_file(mesh_2dm), mesh_before);

    // Reopening the saved model activates the 2D solver via the reference.
    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);
    int active = 0;
    ASSERT_EQ(swmm_2d_is_active(eng_b_, &active), 0);
    EXPECT_EQ(active, 1);

    // Clearing the reference reverts to 1D (no [2D_MESH_FILE] emitted).
    ASSERT_EQ(swmm_options_set_ext(eng_a_, "MESH_FILE", ""), 0);
    ASSERT_EQ(swmm_options_get_ext(eng_a_, "MESH_FILE", buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "");
    const fs::path inp_c = dir_ / "api_clear.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_c.string().c_str()), 0);
    EXPECT_EQ(read_file(inp_c).find("[2D_MESH_FILE]"), std::string::npos);
}

// ---------------------------------------------------------------------------
// External-mesh mode: post-load API mutations persist — the save refreshes
// the .2dm sidecar next to the destination .inp
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, ExternalMeshMutationsPersistOnSave) {
    const fs::path mesh_2dm = dir_ / "mesh_mut.2dm";
    write_file(mesh_2dm, k2DSections);

    const fs::path inp_a = dir_ / "mut.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") +
                          "\n[2D_MESH_FILE]\nFILE mesh_mut.2dm\n");

    eng_a_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0);

    // Mutate the mesh after the external load.
    ASSERT_EQ(swmm_2d_set_vertex_z(eng_a_, 0, 99.0), 0);
    ASSERT_EQ(swmm_2d_set_edge_conveyance(eng_a_, 0, 0, 0.25), 0);

    // Save into a DIFFERENT directory: the sidecar must travel with the .inp.
    const fs::path out_dir = dir_ / "saved";
    fs::create_directories(out_dir);
    const fs::path inp_b = out_dir / "mut_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);
    EXPECT_TRUE(fs::exists(out_dir / "mesh_mut.2dm"))
        << "sidecar must be written next to the destination .inp";

    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);

    double x = 0, y = 0, z = 0;
    ASSERT_EQ(swmm_2d_vertex_get_xyz(eng_b_, 0, &x, &y, &z), 0);
    EXPECT_NEAR(z, 99.0, 1e-9) << "vertex-Z mutation must survive the save";

    double k = -1.0;
    ASSERT_EQ(swmm_2d_get_edge_conveyance(eng_b_, 0, 0, &k), 0);
    EXPECT_NEAR(k, 0.25, 1e-12) << "conveyance mutation must survive the save";
}

// ---------------------------------------------------------------------------
// Per-vertex coupling Cd/Area C API: parsed values readable in the OPENED
// state, setters mutate them, and edits persist through swmm_model_write
// (the GUI save path pushes these right before the write)
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, CouplingCdAreaApiRoundTrip) {
    const fs::path inp_a = dir_ / "cdarea.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") + k2DSections);

    // OPENED (not initialized) — the state the GUI keeps the engine in.
    eng_a_ = open_engine(inp_a);

    double cd = 0.0, area = 0.0;
    ASSERT_EQ(swmm_2d_get_vertex_coupling_cd(eng_a_, 0, &cd), 0);
    ASSERT_EQ(swmm_2d_get_vertex_coupling_area(eng_a_, 0, &area), 0);
    EXPECT_NEAR(cd, 0.7, 1e-12) << "parsed [2D_VERTEX_NODE_MAP] CD";
    EXPECT_NEAR(area, 2.5, 1e-12) << "parsed [2D_VERTEX_NODE_MAP] AREA";

    // Uncoupled vertex keeps the resize defaults.
    ASSERT_EQ(swmm_2d_get_vertex_coupling_cd(eng_a_, 1, &cd), 0);
    ASSERT_EQ(swmm_2d_get_vertex_coupling_area(eng_a_, 1, &area), 0);
    EXPECT_NEAR(cd, 0.65, 1e-12);
    EXPECT_NEAR(area, 1.0, 1e-12);

    // Invalid inputs are rejected.
    EXPECT_EQ(swmm_2d_set_vertex_coupling_cd(eng_a_, 0, 0.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_set_vertex_coupling_area(eng_a_, 0, -1.0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_2d_set_vertex_coupling_cd(eng_a_, 99, 0.5), SWMM_ERR_BADINDEX);

    // Mutate and read back.
    ASSERT_EQ(swmm_2d_set_vertex_coupling_cd(eng_a_, 0, 0.55), 0);
    ASSERT_EQ(swmm_2d_set_vertex_coupling_area(eng_a_, 0, 3.75), 0);
    ASSERT_EQ(swmm_2d_get_vertex_coupling_cd(eng_a_, 0, &cd), 0);
    ASSERT_EQ(swmm_2d_get_vertex_coupling_area(eng_a_, 0, &area), 0);
    EXPECT_NEAR(cd, 0.55, 1e-12);
    EXPECT_NEAR(area, 3.75, 1e-12);

    // Edits persist through the writer.
    const fs::path inp_b = dir_ / "cdarea_out.inp";
    ASSERT_EQ(swmm_model_write(eng_a_, inp_b.string().c_str()), 0);

    eng_b_ = open_engine(inp_b);
    ASSERT_EQ(swmm_2d_get_vertex_coupling_cd(eng_b_, 0, &cd), 0);
    ASSERT_EQ(swmm_2d_get_vertex_coupling_area(eng_b_, 0, &area), 0);
    EXPECT_NEAR(cd, 0.55, 1e-12) << "CD edit must survive the save";
    EXPECT_NEAR(area, 3.75, 1e-12) << "AREA edit must survive the save";
}

// ---------------------------------------------------------------------------
// GeoPackage end-to-end: open .inp → write .gpkg via plugin → reopen .gpkg
// ---------------------------------------------------------------------------

TEST_F(InpWriter2DTest, GeoPackageWriteReopenEndToEnd) {
    const fs::path inp_a = dir_ / "g.inp";
    write_file(inp_a, replace(k1DBase, "{FLOW_UNITS}", "CMS") + k2DSections);

    eng_a_ = open_engine(inp_a);
    ASSERT_EQ(swmm_engine_initialize(eng_a_), 0);

    const fs::path gpkg = dir_ / "g.gpkg";
    const char* kGpkgId = "org.hydrocouple.openswmm.plugins.geopackage";
    int rc = swmm_model_write_with_plugin(eng_a_, gpkg.string().c_str(), kGpkgId);
    if (rc != 0) {
        GTEST_SKIP() << "geopackage plugin unavailable in this build (rc="
                     << rc << ")";
    }

    eng_b_ = swmm_engine_create();
    ASSERT_NE(eng_b_, nullptr);
    ASSERT_EQ(swmm_engine_open(eng_b_, gpkg.string().c_str(), "", "", kGpkgId), 0);
    ASSERT_EQ(swmm_engine_initialize(eng_b_), 0);

    int active = 0;
    ASSERT_EQ(swmm_2d_is_active(eng_b_, &active), 0);
    ASSERT_EQ(active, 1);

    expect_same_2d_model(eng_a_, eng_b_);

    int node_a = -2, node_b = -2;
    ASSERT_EQ(swmm_2d_vertex_get_coupled_node(eng_a_, 0, &node_a), 0);
    ASSERT_EQ(swmm_2d_vertex_get_coupled_node(eng_b_, 0, &node_b), 0);
    EXPECT_GE(node_a, 0);
    EXPECT_EQ(node_a, node_b);
}
