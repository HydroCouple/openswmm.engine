/**
 * @file test_geopackage_mesh2d.cpp
 * @brief Unit tests for GeoPackage 2D mesh persistence (Part E).
 *
 * @details Round-trips the 2D model definition — mesh geometry, triangle
 *          connectivity, boundary conditions, edge conveyance, 1D-2D
 *          coupling maps, and SolverOptions2D — through write_model /
 *          read_model, plus FK-integrity and transaction-rollback cases.
 *
 *          The fixture owns the 2D data structs directly and wires
 *          ctx.twod_io at them (the same thing SWMMEngine::wire2DModelIO
 *          does with SurfaceRouter2D's members), so these tests exercise
 *          the persistence layer without an engine and run in both 2D and
 *          non-2D builds.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <cstdio>
#include <string>
#include <vector>

#include "core/SimulationContext.hpp"
#include "data/NodeData.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/data/BoundaryData.hpp"
#include "2d/data/PendingRows2D.hpp"
#include "input/geopackage/GeoPackageSchema.hpp"
#include "input/geopackage/GeoPackageWriter.hpp"
#include "input/geopackage/GeoPackageReader.hpp"
#include "input/geopackage/GpkgUtils.hpp"

namespace fs = std::filesystem;
using namespace openswmm;
using namespace openswmm::gpkg;

namespace {
constexpr const char* kSimId = "run_1";
}

class GeoPackageMesh2DTest : public ::testing::Test {
protected:
    std::string db_path_;

    // Write-side 2D model storage (stand-in for SurfaceRouter2D's members).
    twoD::MeshData                              mesh_;
    twoD::SolverOptions2D                       opts_;
    twoD::BoundaryData                          boundary_;
    std::vector<twoD::PendingBoundaryRow>       pending_bc_;
    std::vector<twoD::PendingEdgeConveyanceRow> pending_ec_;

    // Read-side storage for the round-trip comparison.
    twoD::MeshData                              mesh_in_;
    twoD::SolverOptions2D                       opts_in_;
    twoD::BoundaryData                          boundary_in_;
    std::vector<twoD::PendingBoundaryRow>       pending_bc_in_;
    std::vector<twoD::PendingEdgeConveyanceRow> pending_ec_in_;

    void SetUp() override {
        // Unique file per test, plus sidecar cleanup. A single shared path let
        // one test's leftover rows/locks bleed into the next on Windows (where
        // a connection torn down mid-write keeps the .gpkg/-wal file locked):
        // the rollback test then observed a previous test's rows, and the test
        // after it failed to open with "database is locked". Per-test isolation
        // removes that coupling; on POSIX it is simply harmless.
        const ::testing::TestInfo* info =
            ::testing::UnitTest::GetInstance()->current_test_info();
        db_path_ = (fs::temp_directory_path() /
                    ("test_openswmm_mesh2d_" + std::string(info->name()) + ".gpkg"))
                       .string();
        remove_db_files();
    }

    void TearDown() override {
        remove_db_files();
    }

    // std::remove on the .gpkg alone leaves the WAL sidecars behind; clear all
    // three so a stale -wal can never resurrect a prior test's contents.
    void remove_db_files() {
        std::remove(db_path_.c_str());
        std::remove((db_path_ + "-wal").c_str());
        std::remove((db_path_ + "-shm").c_str());
    }

    DbPtr create_db() {
        auto db = open_database(db_path_);
        create_schema(db.get());
        return db;
    }

    // Minimal 1D context: three named nodes (write_nodes uses safe accessors,
    // so name entries alone are enough to materialise FK-able node rows).
    SimulationContext build_ctx_out() {
        SimulationContext ctx{};
        ctx.options.flow_units = FlowUnits::CMS;
        ctx.node_names.add("J1");
        ctx.node_names.add("J3");
        ctx.node_names.add("O1");
        ctx.twod_io.mesh       = &mesh_;
        ctx.twod_io.options    = &opts_;
        ctx.twod_io.boundary   = &boundary_;
        ctx.twod_io.pending_bc = &pending_bc_;
        ctx.twod_io.pending_ec = &pending_ec_;
        return ctx;
    }

    SimulationContext build_ctx_in() {
        SimulationContext ctx{};
        ctx.twod_io.mesh       = &mesh_in_;
        ctx.twod_io.options    = &opts_in_;
        ctx.twod_io.boundary   = &boundary_in_;
        ctx.twod_io.pending_bc = &pending_bc_in_;
        ctx.twod_io.pending_ec = &pending_ec_in_;
        return ctx;
    }

    // Unit square split along the (0, 2) diagonal:
    //   3 --- 2
    //   | t1 /|
    //   |  /  |
    //   |/ t0 |
    //   0 --- 1
    void makeUnitSquareMesh() {
        mesh_.resize_vertices(4);
        mesh_.vx = {0.0, 10.0, 10.0, 0.0};
        mesh_.vy = {0.0, 0.0, 10.0, 10.0};
        mesh_.vz = {10.0, 10.5, 11.0, 11.5};
        mesh_.vtag[0] = "VT0";

        mesh_.resize_triangles(2);
        mesh_.tri_v0 = {0, 0};
        mesh_.tri_v1 = {1, 2};
        mesh_.tri_v2 = {2, 3};
        mesh_.mannings_n = {0.03, 0.045};
        mesh_.tri_init_depth = {0.25, 0.0};
        mesh_.tri_tag[1] = "T1";
    }

    int count_rows(sqlite3* db, const std::string& table) {
        auto stmt = prepare(db, "SELECT COUNT(*) FROM " + table);
        if (sqlite3_step(stmt.get()) != SQLITE_ROW) return -1;
        return column_int(stmt.get(), 0);
    }
};

// ---------------------------------------------------------------------------
// Mesh geometry + connectivity round-trip
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, MeshRoundTripMinimal) {
    makeUnitSquareMesh();
    auto ctx = build_ctx_out();

    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
    }

    auto ctx_in = build_ctx_in();
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }

    ASSERT_EQ(mesh_in_.n_vertices(), 4);
    ASSERT_EQ(mesh_in_.n_triangles(), 2);
    for (int i = 0; i < 4; ++i) {
        EXPECT_DOUBLE_EQ(mesh_in_.vx[i], mesh_.vx[i]) << "vx[" << i << "]";
        EXPECT_DOUBLE_EQ(mesh_in_.vy[i], mesh_.vy[i]) << "vy[" << i << "]";
        EXPECT_DOUBLE_EQ(mesh_in_.vz[i], mesh_.vz[i]) << "vz[" << i << "]";
        EXPECT_EQ(mesh_in_.vtag[i], mesh_.vtag[i])    << "vtag[" << i << "]";
    }
    for (int t = 0; t < 2; ++t) {
        EXPECT_EQ(mesh_in_.tri_v0[t], mesh_.tri_v0[t]);
        EXPECT_EQ(mesh_in_.tri_v1[t], mesh_.tri_v1[t]);
        EXPECT_EQ(mesh_in_.tri_v2[t], mesh_.tri_v2[t]);
        EXPECT_DOUBLE_EQ(mesh_in_.mannings_n[t], mesh_.mannings_n[t]);
        EXPECT_DOUBLE_EQ(mesh_in_.tri_init_depth[t], mesh_.tri_init_depth[t]);
        EXPECT_EQ(mesh_in_.tri_tag[t], mesh_.tri_tag[t]);
    }
    // Derived topology is NOT persisted — it stays at the resize defaults
    // until SurfaceRouter2D::initialize() rebuilds it.
    EXPECT_EQ(mesh_in_.tri_nbr0[0], -1);
}

// ---------------------------------------------------------------------------
// SolverOptions2D round-trip via the options key-value table
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, OptionsRoundTrip2D) {
    makeUnitSquareMesh();

    opts_.max_timestep      = 5.5;
    opts_.dry_depth         = 0.002;
    opts_.limiter_epsilon   = 2.0e-7;   // would be destroyed by %.6f formatting
    opts_.flux_dh_eps       = 7.0e-12;  // ditto — g17 formatting must preserve it
    opts_.coupling_cd       = 0.71;
    opts_.report_2d         = false;
    // Closure keys.
    opts_.cell_closure        = twoD::CellClosure2D::VFR;
    opts_.face_reconstruction = twoD::FaceDepth2D::VFR_FACE;
    opts_.vfr_min_wet_frac    = 0.025;
    // Explicit-marcher keys (D2: the marcher is the only 2D integrator).
    opts_.theta             = 0.65;
    opts_.cfl_number        = 0.45;
    opts_.h_move            = 0.007;
    opts_.lts_tiers         = 6;
    opts_.froude_max        = 2.25;
    opts_.coupling_area_auto = true;
    opts_.output_file       = "results/run.h5";
    opts_.mesh_units_si     = true;
    opts_.mesh_file         = "meshes/site.2dm";  // provenance only

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
    }

    auto ctx_in = build_ctx_in();
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }

    EXPECT_DOUBLE_EQ(opts_in_.max_timestep,    5.5);
    EXPECT_DOUBLE_EQ(opts_in_.dry_depth,       0.002);
    EXPECT_DOUBLE_EQ(opts_in_.limiter_epsilon, 2.0e-7);
    EXPECT_DOUBLE_EQ(opts_in_.flux_dh_eps,     7.0e-12);
    EXPECT_DOUBLE_EQ(opts_in_.coupling_cd,     0.71);
    EXPECT_FALSE(opts_in_.report_2d);
    EXPECT_EQ(opts_in_.cell_closure,        twoD::CellClosure2D::VFR);
    EXPECT_EQ(opts_in_.face_reconstruction, twoD::FaceDepth2D::VFR_FACE);
    EXPECT_DOUBLE_EQ(opts_in_.vfr_min_wet_frac, 0.025);
    EXPECT_DOUBLE_EQ(opts_in_.theta,        0.65);
    EXPECT_DOUBLE_EQ(opts_in_.cfl_number,   0.45);
    EXPECT_DOUBLE_EQ(opts_in_.h_move,       0.007);
    EXPECT_EQ(opts_in_.lts_tiers,           6);
    EXPECT_DOUBLE_EQ(opts_in_.froude_max,   2.25);
    EXPECT_TRUE(opts_in_.coupling_area_auto);
    // 2D results always live in the HDF5 file — the path must round-trip so
    // SWMMEngine::open re-creates the Default2DOutputPlugin.
    EXPECT_EQ(opts_in_.output_file, "results/run.h5");
    EXPECT_TRUE(opts_in_.mesh_units_si);
    // 2D_MESH_FILE_SOURCE is provenance only — restoring it would trigger a
    // second external-mesh load on top of the gpkg-read mesh.
    EXPECT_TRUE(opts_in_.mesh_file.empty());
    // Runtime-only flag never persists.
    EXPECT_FALSE(opts_in_.mesh_scaled_to_si);
}

// ---------------------------------------------------------------------------
// Boundary conditions + edge conveyance round-trip (authored pending rows)
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, BCAndConveyanceRoundTrip) {
    makeUnitSquareMesh();

    pending_bc_.push_back({0, 0, 1, 0.01, "", ""});          // NORMAL_FLOW slope
    pending_bc_.push_back({0, 1, 2, 12.5, "", ""});          // SPECIFIED_STAGE head
    pending_bc_.push_back({0, 2, 2, 0.0, "TideTS", "GrpA"}); // TS_STAGE + group
    pending_bc_.push_back({1, 0, 3, 0.25, "", ""});          // SPECIFIED_FLOW
    pending_bc_.push_back({1, 1, 3, 0.0, "FlowTS", ""});     // TS_FLOW
    pending_bc_.push_back({1, 2, 4, 0.0, "RC1", "GrpB"});    // RATING_CURVE + group

    pending_ec_.push_back({0, 2, 0.5});   // interior diagonal edge
    pending_ec_.push_back({3, 0, 0.75});  // boundary edge, reversed order

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
    }

    auto ctx_in = build_ctx_in();
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }

    ASSERT_EQ(pending_bc_in_.size(), pending_bc_.size());
    // Reader orders by (tri_idx, edge) — same order the rows were authored.
    for (size_t i = 0; i < pending_bc_.size(); ++i) {
        EXPECT_EQ(pending_bc_in_[i].tri,     pending_bc_[i].tri)     << "row " << i;
        EXPECT_EQ(pending_bc_in_[i].edge,    pending_bc_[i].edge)    << "row " << i;
        EXPECT_EQ(pending_bc_in_[i].bc_type, pending_bc_[i].bc_type) << "row " << i;
        EXPECT_EQ(pending_bc_in_[i].name,    pending_bc_[i].name)    << "row " << i;
        EXPECT_EQ(pending_bc_in_[i].group,   pending_bc_[i].group)   << "row " << i;
        if (pending_bc_[i].name.empty()) {
            EXPECT_DOUBLE_EQ(pending_bc_in_[i].param1, pending_bc_[i].param1)
                << "row " << i;
        }
    }

    ASSERT_EQ(pending_ec_in_.size(), 2u);
    // Pairs are normalised v_from < v_to and ordered by (v_from, v_to).
    EXPECT_EQ(pending_ec_in_[0].v_from, 0);
    EXPECT_EQ(pending_ec_in_[0].v_to,   2);
    EXPECT_DOUBLE_EQ(pending_ec_in_[0].conveyance, 0.5);
    EXPECT_EQ(pending_ec_in_[1].v_from, 0);
    EXPECT_EQ(pending_ec_in_[1].v_to,   3);
    EXPECT_DOUBLE_EQ(pending_ec_in_[1].conveyance, 0.75);
}

// ---------------------------------------------------------------------------
// Conveyance fallback: no pending rows — reconstruct from mesh slots,
// de-duplicating the interior-edge mirror.
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, ConveyanceFallbackFromMeshSlots) {
    makeUnitSquareMesh();
    // Interior edge (0,2): slot t0/e1 (opposite v1 → connects v2,v0) and
    // slot t1/e2 (opposite v3... local edge e connects v[(e+1)%3], v[(e+2)%3]).
    // t0 = (0,1,2): e2 connects v0,v1; e0 connects v1,v2; e1 connects v2,v0.
    // t1 = (0,2,3): e2 connects v0,v2 — the mirror of t0/e1.
    mesh_.edge_conveyance[0 * 3 + 1] = 0.4;
    mesh_.edge_conveyance[1 * 3 + 2] = 0.4;

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
        EXPECT_EQ(count_rows(db.get(), "mesh_2d_edge_conveyance"), 1);
    }

    auto ctx_in = build_ctx_in();
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }
    ASSERT_EQ(pending_ec_in_.size(), 1u);
    EXPECT_EQ(pending_ec_in_[0].v_from, 0);
    EXPECT_EQ(pending_ec_in_[0].v_to,   2);
    EXPECT_DOUBLE_EQ(pending_ec_in_[0].conveyance, 0.4);
}

// ---------------------------------------------------------------------------
// 1D-2D coupling maps round-trip (names + cd + area, FK to nodes)
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, CouplingMapsRoundTrip) {
    makeUnitSquareMesh();
    mesh_.vert_coupled_node_name[0] = "J1";
    mesh_.vert_coupling_cd[0]       = 0.7;
    mesh_.vert_coupling_area[0]     = 2.5;
    mesh_.tri_coupled_node_name[1]  = "J3";
    mesh_.tri_coupling_cd[1]        = 0.55;
    mesh_.tri_coupling_area[1]      = 4.0;

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
    }

    auto ctx_in = build_ctx_in();
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }

    // Names restored (indices stay -1 — resolution is initialize()'s job,
    // identical to the inp path).
    EXPECT_EQ(mesh_in_.vert_coupled_node_name[0], "J1");
    EXPECT_DOUBLE_EQ(mesh_in_.vert_coupling_cd[0],   0.7);
    EXPECT_DOUBLE_EQ(mesh_in_.vert_coupling_area[0], 2.5);
    EXPECT_EQ(mesh_in_.vert_coupled_node[0], -1);
    EXPECT_TRUE(mesh_in_.vert_coupled_node_name[1].empty());

    EXPECT_EQ(mesh_in_.tri_coupled_node_name[1], "J3");
    EXPECT_DOUBLE_EQ(mesh_in_.tri_coupling_cd[1],   0.55);
    EXPECT_DOUBLE_EQ(mesh_in_.tri_coupling_area[1], 4.0);
    EXPECT_EQ(mesh_in_.tri_coupled_node[1], -1);
    EXPECT_TRUE(mesh_in_.tri_coupled_node_name[0].empty());
}

// ---------------------------------------------------------------------------
// Post-initialize save: mesh in SI is un-scaled back to authored units
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, PostInitUnscaleWrite) {
    makeUnitSquareMesh();
    // Simulate the state after SurfaceRouter2D::initialize() on a US-units
    // project: coordinates were scaled ft→m in place.
    const double ft_to_m = 0.3048;
    for (auto& v : mesh_.vx) v *= ft_to_m;
    for (auto& v : mesh_.vy) v *= ft_to_m;
    for (auto& v : mesh_.vz) v *= ft_to_m;
    mesh_.vert_coupled_node_name[0] = "J1";
    mesh_.vert_coupling_area[0]     = 2.5 * ft_to_m * ft_to_m;
    opts_.mesh_scaled_to_si = true;
    opts_.len_1d_to_2d      = ft_to_m;
    opts_.len_2d_to_1d      = 1.0 / ft_to_m;

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
    }

    auto ctx_in = build_ctx_in();
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }

    // Authored (feet) values restored to ~1 ulp; initialize() will re-apply
    // the ft→m scaling on the next run because mesh_units_si stays false.
    EXPECT_NEAR(mesh_in_.vx[1], 10.0, 1e-12);
    EXPECT_NEAR(mesh_in_.vy[2], 10.0, 1e-12);
    EXPECT_NEAR(mesh_in_.vz[3], 11.5, 1e-12);
    EXPECT_NEAR(mesh_in_.vert_coupling_area[0], 2.5, 1e-12);
    EXPECT_FALSE(opts_in_.mesh_units_si);
    EXPECT_FALSE(opts_in_.mesh_scaled_to_si);
}

// ---------------------------------------------------------------------------
// No-2D no-op behavior
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, Inactive2DNoOp) {
    // Null twod_io (non-2D engine build, or detached context): write_model
    // succeeds and produces no 2D rows or option keys.
    SimulationContext ctx{};
    ctx.node_names.add("J1");
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
        EXPECT_EQ(count_rows(db.get(), "mesh_2d_vertices"), 0);
        EXPECT_EQ(count_rows(db.get(), "mesh_2d_triangles"), 0);
        auto stmt = prepare(db.get(),
            "SELECT COUNT(*) FROM options WHERE key LIKE '2D_%'");
        ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
        EXPECT_EQ(column_int(stmt.get(), 0), 0);
    }
}

TEST_F(GeoPackageMesh2DTest, MeshBearingFileIntoNull2DContextWarns) {
    makeUnitSquareMesh();
    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
    }

    // Read into a context with no 2D wiring — must not crash, must warn.
    SimulationContext ctx_in{};
    {
        auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
        ASSERT_EQ(read_model(db.get(), ctx_in, kSimId, ""), 0);
    }
    bool warned = false;
    for (const auto& w : ctx_in.warnings)
        if (w.find("2D mesh") != std::string::npos) warned = true;
    EXPECT_TRUE(warned);
}

// ---------------------------------------------------------------------------
// FK integrity: coupling to a missing node aborts and rolls back atomically
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, UnknownCouplingNodeRollsBackWholeWrite) {
    makeUnitSquareMesh();
    mesh_.vert_coupled_node_name[0] = "NO_SUCH_NODE";

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        EXPECT_THROW(write_model(db.get(), ctx, kSimId), GpkgError);

        // The Transaction guard rolled the ENTIRE model write back — not
        // just the offending row: no nodes, no vertices, no options.
        EXPECT_EQ(count_rows(db.get(), "nodes"), 0);
        EXPECT_EQ(count_rows(db.get(), "mesh_2d_vertices"), 0);
        EXPECT_EQ(count_rows(db.get(), "options"), 0);
    }
}

// ---------------------------------------------------------------------------
// Cascade integrity: deleting a node cascades through the coupling table
// ---------------------------------------------------------------------------

TEST_F(GeoPackageMesh2DTest, NodeDeleteCascadesCoupling) {
    makeUnitSquareMesh();
    mesh_.vert_coupled_node_name[0] = "J1";

    auto ctx = build_ctx_out();
    {
        auto db = create_db();
        write_model(db.get(), ctx, kSimId);
        ASSERT_EQ(count_rows(db.get(), "mesh_2d_vertex_coupling"), 1);

        exec(db.get(), "DELETE FROM nodes WHERE node_id = 'J1'");
        EXPECT_EQ(count_rows(db.get(), "mesh_2d_vertex_coupling"), 0);
        // The mesh itself is untouched.
        EXPECT_EQ(count_rows(db.get(), "mesh_2d_vertices"), 4);
    }
}
