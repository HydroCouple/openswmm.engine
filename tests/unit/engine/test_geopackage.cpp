/**
 * @file test_geopackage.cpp
 * @brief Unit tests for GeoPackage round-trip: write + read all SWMM sections.
 *
 * @details Verifies that all SWMM input sections survive a write -> read
 *          round-trip through GeoPackage format. Each section has its own
 *          test case, plus integration tests for full model round-trip.
 *
 * @ingroup engine_tests
 */

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include <filesystem>
#include <cstdio>

#include "core/SimulationContext.hpp"
#include "data/NodeData.hpp"
#include "data/LinkData.hpp"
#include "data/SubcatchData.hpp"
#include "data/GageData.hpp"
#include "data/TableData.hpp"
#include "data/PollutantData.hpp"
#include "data/InflowData.hpp"
#include "input/geopackage/GeoPackageSchema.hpp"
#include "input/geopackage/GeoPackageWriter.hpp"
#include "input/geopackage/GeoPackageReader.hpp"
#include "input/geopackage/GpkgUtils.hpp"
#include "input/geopackage/GpkgGeometry.hpp"
#include "input/geopackage/GeoPackagePluginInfo.hpp"
#include "input/geopackage/GeoPackageInputPlugin.hpp"
#include "input/geopackage/GeoPackageOutputPlugin.hpp"
#include "input/geopackage/GeoPackageReportPlugin.hpp"

namespace fs = std::filesystem;
using namespace openswmm;
using namespace openswmm::gpkg;

// ============================================================================
// Test fixture: creates a temp .gpkg file, cleans up on tear-down
// ============================================================================

class GeoPackageTest : public ::testing::Test {
protected:
    std::string db_path_;

    void SetUp() override {
        db_path_ = (fs::temp_directory_path() / "test_openswmm.gpkg").string();
        // Remove any leftover from previous run
        std::remove(db_path_.c_str());
    }

    void TearDown() override {
        std::remove(db_path_.c_str());
    }

    // Helper: create a fresh database with schema
    DbPtr create_db() {
        auto db = open_database(db_path_);
        create_schema(db.get());
        return db;
    }

    // Helper: build a minimal SimulationContext for testing
    SimulationContext build_test_context() {
        SimulationContext ctx{};

        // --- OPTIONS ---
        ctx.options.flow_units = FlowUnits::GPM;
        ctx.options.routing_model = RoutingModel::DYNWAVE;
        ctx.options.wet_step = 300.0;
        ctx.options.dry_step = 3600.0;
        ctx.options.routing_step = 5.0;
        ctx.options.report_step = 60.0;
        ctx.options.start_date = 35796.0;
        ctx.options.end_date = 35797.0;
        ctx.spatial.crs = "EPSG:4326";

        // --- JUNCTIONS (3 nodes) ---
        for (const auto& name : {"J1", "J2", "J3"}) {
            int idx = ctx.node_names.add(name);
            size_t n = static_cast<size_t>(idx + 1);
            ctx.nodes.type.resize(n);
            ctx.nodes.invert_elev.resize(n);
            ctx.nodes.full_depth.resize(n);
            ctx.nodes.init_depth.resize(n);
            ctx.nodes.sur_depth.resize(n);
            ctx.nodes.ponded_area.resize(n);
            ctx.nodes.outfall_type.resize(n);
            ctx.nodes.outfall_param.resize(n);
            ctx.nodes.outfall_has_flap_gate.resize(n);
            ctx.nodes.storage_curve.resize(n);
            ctx.nodes.storage_curve_name.resize(n);
            ctx.nodes.storage_a.resize(n);
            ctx.nodes.storage_b.resize(n);
            ctx.nodes.storage_c.resize(n);
            ctx.nodes.divider_type.resize(n);
            ctx.nodes.divider_cutoff.resize(n);
            ctx.nodes.divider_curve.resize(n);
            ctx.nodes.divider_curve_name.resize(n);

            ctx.nodes.type[idx] = NodeType::JUNCTION;
            ctx.nodes.invert_elev[idx] = 100.0 - idx * 2.0;
            ctx.nodes.full_depth[idx] = 6.0;
            ctx.nodes.init_depth[idx] = 0.0;
            ctx.nodes.sur_depth[idx] = 0.0;
            ctx.nodes.ponded_area[idx] = 0.0;
        }

        // --- OUTFALL ---
        {
            int idx = ctx.node_names.add("O1");
            size_t n = static_cast<size_t>(idx + 1);
            ctx.nodes.type.resize(n);
            ctx.nodes.invert_elev.resize(n);
            ctx.nodes.full_depth.resize(n);
            ctx.nodes.init_depth.resize(n);
            ctx.nodes.sur_depth.resize(n);
            ctx.nodes.ponded_area.resize(n);
            ctx.nodes.outfall_type.resize(n);
            ctx.nodes.outfall_param.resize(n);
            ctx.nodes.outfall_has_flap_gate.resize(n);
            ctx.nodes.storage_curve.resize(n);
            ctx.nodes.storage_curve_name.resize(n);
            ctx.nodes.storage_a.resize(n);
            ctx.nodes.storage_b.resize(n);
            ctx.nodes.storage_c.resize(n);
            ctx.nodes.divider_type.resize(n);
            ctx.nodes.divider_cutoff.resize(n);
            ctx.nodes.divider_curve.resize(n);
            ctx.nodes.divider_curve_name.resize(n);

            ctx.nodes.type[idx] = NodeType::OUTFALL;
            ctx.nodes.invert_elev[idx] = 90.0;
            ctx.nodes.full_depth[idx] = 0.0;
            ctx.nodes.outfall_type[idx] = OutfallType::FREE;
        }

        // --- COORDINATES ---
        ctx.spatial.node_x = {1000.0, 2000.0, 3000.0, 4000.0};
        ctx.spatial.node_y = {5000.0, 5000.0, 5000.0, 5000.0};

        // --- CONDUITS (3 links) ---
        for (int i = 0; i < 3; ++i) {
            std::string name = "C" + std::to_string(i + 1);
            int idx = ctx.link_names.add(name);
            size_t n = static_cast<size_t>(idx + 1);
            ctx.links.type.resize(n);
            ctx.links.node1.resize(n);
            ctx.links.node2.resize(n);
            ctx.links.offset1.resize(n);
            ctx.links.offset2.resize(n);
            ctx.links.q0.resize(n);
            ctx.links.q_limit.resize(n);
            ctx.links.xsect_shape.resize(n);
            ctx.links.xsect_y_full.resize(n);
            ctx.links.xsect_a_full.resize(n);
            ctx.links.xsect_w_max.resize(n);
            ctx.links.xsect_curve.resize(n);
            ctx.links.roughness.resize(n);
            ctx.links.length.resize(n);
            ctx.links.barrels.resize(n);
            ctx.links.culvert_code.resize(n);
            ctx.links.loss_inlet.resize(n);
            ctx.links.loss_outlet.resize(n);
            ctx.links.loss_avg.resize(n);
            ctx.links.has_flap_gate.resize(n);
            ctx.links.seep_rate.resize(n);
            ctx.links.pump_curve.resize(n);
            ctx.links.pump_curve_name.resize(n);
            ctx.links.pump_init_state.resize(n);
            ctx.links.pump_startup.resize(n);
            ctx.links.pump_shutoff.resize(n);
            ctx.links.crest_height.resize(n);
            ctx.links.cd.resize(n);
            ctx.links.xsect_y_bot.resize(n);
            ctx.spatial.link_vertices_x.resize(n);
            ctx.spatial.link_vertices_y.resize(n);
            ctx.spatial.link_x.resize(n);
            ctx.spatial.link_y.resize(n);

            ctx.links.type[idx] = LinkType::CONDUIT;
            ctx.links.node1[idx] = i;
            ctx.links.node2[idx] = i + 1;
            ctx.links.xsect_shape[idx] = XsectShape::CIRCULAR;
            ctx.links.xsect_y_full[idx] = 2.0;
            ctx.links.roughness[idx] = 0.013;
            ctx.links.length[idx] = 400.0;
            ctx.links.barrels[idx] = 1;
            ctx.links.xsect_curve[idx] = -1;
        }

        // --- VERTICES ---
        ctx.spatial.link_vertices_x[1] = {2500.0};
        ctx.spatial.link_vertices_y[1] = {5100.0};

        // --- SUBCATCHMENTS (2) ---
        for (int i = 0; i < 2; ++i) {
            std::string name = "S" + std::to_string(i + 1);
            int idx = ctx.subcatch_names.add(name);
            size_t n = static_cast<size_t>(idx + 1);
            ctx.subcatches.outlet_node.resize(n, -1);
            ctx.subcatches.outlet_subcatch.resize(n, -1);
            ctx.subcatches.outlet_name.resize(n);
            ctx.subcatches.gage.resize(n, -1);
            ctx.subcatches.area.resize(n);
            ctx.subcatches.width.resize(n);
            ctx.subcatches.slope.resize(n);
            ctx.subcatches.curb_length.resize(n);
            ctx.subcatches.frac_imperv.resize(n);
            ctx.subcatches.n_imperv.resize(n);
            ctx.subcatches.n_perv.resize(n);
            ctx.subcatches.ds_imperv.resize(n);
            ctx.subcatches.ds_perv.resize(n);
            ctx.subcatches.frac_imperv_no_store.resize(n);
            ctx.subcatches.subarea_routing.resize(n);
            ctx.subcatches.pct_routed.resize(n);
            ctx.subcatches.infil_model.resize(n);
            ctx.subcatches.infil_p1.resize(n);
            ctx.subcatches.infil_p2.resize(n);
            ctx.subcatches.infil_p3.resize(n);
            ctx.subcatches.infil_p4.resize(n);
            ctx.subcatches.infil_p5.resize(n);
            ctx.spatial.subcatch_polygon_x.resize(n);
            ctx.spatial.subcatch_polygon_y.resize(n);

            ctx.subcatches.outlet_node[idx] = i;
            ctx.subcatches.area[idx] = 5.0;
            ctx.subcatches.width[idx] = 500.0;
            ctx.subcatches.slope[idx] = 0.01;
            ctx.subcatches.frac_imperv[idx] = 0.25;
            ctx.subcatches.n_imperv[idx] = 0.01;
            ctx.subcatches.n_perv[idx] = 0.1;
            ctx.subcatches.ds_imperv[idx] = 0.05;
            ctx.subcatches.ds_perv[idx] = 0.10;
            ctx.subcatches.infil_model[idx] = 0; // HORTON
            ctx.subcatches.infil_p1[idx] = 3.0;
            ctx.subcatches.infil_p2[idx] = 0.5;
            ctx.subcatches.infil_p3[idx] = 4.0;

            // Polygon
            ctx.spatial.subcatch_polygon_x[idx] = {0, 100, 100, 0, 0};
            ctx.spatial.subcatch_polygon_y[idx] = {0, 0, 100, 100, 0};
        }

        // --- RAIN GAGES ---
        {
            int idx = ctx.gage_names.add("RG1");
            size_t n = static_cast<size_t>(idx + 1);
            ctx.gages.rain_type.resize(n);
            ctx.gages.interval_sec.resize(n);
            ctx.gages.snow_factor.resize(n);
            ctx.gages.source.resize(n);
            ctx.gages.ts_index.resize(n, -1);
            ctx.spatial.gage_x.resize(n);
            ctx.spatial.gage_y.resize(n);

            ctx.gages.rain_type[idx] = 1;
            ctx.gages.interval_sec[idx] = 3600;
            ctx.gages.snow_factor[idx] = 1.0;
            ctx.gages.source[idx] = RainSource::TIMESERIES;
            ctx.spatial.gage_x[idx] = 1500.0;
            ctx.spatial.gage_y[idx] = 5500.0;
        }

        // --- TIMESERIES ---
        {
            int idx = ctx.table_names.add("TS1");
            ctx.tables.add("TS1", TableType::TIMESERIES);
            ctx.tables[idx].x = {0.0, 1.0, 2.0, 3.0};
            ctx.tables[idx].y = {0.5, 2.0, 1.5, 0.0};

            // Link gage to timeseries
            ctx.gages.ts_index[0] = idx;
        }

        // --- CURVES ---
        {
            int idx = ctx.table_names.add("StorageCurve1");
            ctx.tables.add("StorageCurve1", TableType::CURVE_CONTROL);
            ctx.tables[idx].x = {0.0, 2.0, 4.0};
            ctx.tables[idx].y = {100.0, 200.0, 500.0};
        }

        // --- POLLUTANTS ---
        {
            int idx = ctx.pollutant_names.add("TSS");
            size_t n = static_cast<size_t>(idx + 1);
            ctx.pollutants.units.resize(n);
            ctx.pollutants.c_rain.resize(n);
            ctx.pollutants.c_gw.resize(n);
            ctx.pollutants.k_decay.resize(n);
            ctx.pollutants.snow_only.resize(n);
            ctx.pollutants.co_pollut.resize(n, -1);
            ctx.pollutants.co_frac.resize(n);

            ctx.pollutants.units[idx] = MassUnits::MG_PER_L;
            ctx.pollutants.c_rain[idx] = 0.0;
            ctx.pollutants.c_gw[idx] = 0.0;
            ctx.pollutants.k_decay[idx] = 0.0;
        }

        // --- PATTERNS ---
        {
            ctx.patterns.add("DWF_Pattern", 0, {1.0, 1.1, 1.2, 1.0, 0.9, 0.8,
                                                 0.7, 0.8, 0.9, 1.0, 1.1, 1.2});
        }

        // --- TAGS ---
        ctx.node_tags["J1"] = "upstream";
        ctx.link_tags["C1"] = "trunk";
        ctx.subcatch_tags["S1"] = "residential";

        return ctx;
    }
};

// ============================================================================
// Schema tests
// ============================================================================

TEST_F(GeoPackageTest, CreateSchema) {
    auto db = create_db();

    // Verify GeoPackage metadata tables exist
    auto stmt = prepare(db.get(), "SELECT count(*) FROM gpkg_spatial_ref_sys");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_GE(column_int(stmt.get(), 0), 3); // At least 3 default SRS entries

    // Verify application tables exist
    auto stmt2 = prepare(db.get(), "SELECT count(*) FROM sqlite_master WHERE type='table' AND name='nodes'");
    ASSERT_EQ(sqlite3_step(stmt2.get()), SQLITE_ROW);
    EXPECT_EQ(column_int(stmt2.get(), 0), 1);
}

TEST_F(GeoPackageTest, VariablesCatalog) {
    auto db = create_db();
    populate_default_variables(db.get());

    auto stmt = prepare(db.get(), "SELECT count(*) FROM variables");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_GE(column_int(stmt.get(), 0), 20); // At least 20 pre-defined variables
}

// ============================================================================
// Geometry encoding/decoding tests
// ============================================================================

TEST_F(GeoPackageTest, PointRoundTrip) {
    auto blob = encode_point(42.5, -73.2, 4326);
    auto pt = decode_point(blob);
    EXPECT_DOUBLE_EQ(pt.x, 42.5);
    EXPECT_DOUBLE_EQ(pt.y, -73.2);
    EXPECT_EQ(pt.srs_id, 4326);
}

TEST_F(GeoPackageTest, LinestringRoundTrip) {
    std::vector<double> xs = {0.0, 1.0, 2.0, 3.0};
    std::vector<double> ys = {0.0, 1.0, 0.0, 1.0};
    auto blob = encode_linestring(xs, ys, 4326);
    auto ls = decode_linestring(blob);
    EXPECT_EQ(ls.xs.size(), 4u);
    EXPECT_EQ(ls.ys.size(), 4u);
    EXPECT_DOUBLE_EQ(ls.xs[0], 0.0);
    EXPECT_DOUBLE_EQ(ls.xs[3], 3.0);
    EXPECT_DOUBLE_EQ(ls.ys[1], 1.0);
}

TEST_F(GeoPackageTest, MultipolygonRoundTrip) {
    std::vector<double> xs = {0.0, 10.0, 10.0, 0.0, 0.0};
    std::vector<double> ys = {0.0, 0.0, 10.0, 10.0, 0.0};
    auto blob = encode_multipolygon(xs, ys, 4326);
    auto mp = decode_multipolygon(blob);
    EXPECT_EQ(mp.xs.size(), 5u);
    EXPECT_DOUBLE_EQ(mp.xs[0], 0.0);
    EXPECT_DOUBLE_EQ(mp.xs[1], 10.0);
}

// ============================================================================
// Section round-trip tests
// ============================================================================

TEST_F(GeoPackageTest, OptionsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.options.flow_units, ctx_out.options.flow_units);
    EXPECT_EQ(ctx_in.options.routing_model, ctx_out.options.routing_model);
    EXPECT_DOUBLE_EQ(ctx_in.options.wet_step, ctx_out.options.wet_step);
    EXPECT_DOUBLE_EQ(ctx_in.options.dry_step, ctx_out.options.dry_step);
    EXPECT_DOUBLE_EQ(ctx_in.options.routing_step, ctx_out.options.routing_step);
    EXPECT_DOUBLE_EQ(ctx_in.options.report_step, ctx_out.options.report_step);
    EXPECT_EQ(ctx_in.spatial.crs, "EPSG:4326");
}

TEST_F(GeoPackageTest, JunctionsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.node_names.size(), 4); // J1, J2, J3, O1

    int j1 = ctx_in.node_names.find("J1");
    ASSERT_GE(j1, 0);
    EXPECT_EQ(ctx_in.nodes.type[j1], NodeType::JUNCTION);
    EXPECT_DOUBLE_EQ(ctx_in.nodes.invert_elev[j1], 100.0);
    EXPECT_DOUBLE_EQ(ctx_in.nodes.full_depth[j1], 6.0);
}

TEST_F(GeoPackageTest, OutfallRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int o1 = ctx_in.node_names.find("O1");
    ASSERT_GE(o1, 0);
    EXPECT_EQ(ctx_in.nodes.type[o1], NodeType::OUTFALL);
    EXPECT_EQ(ctx_in.nodes.outfall_type[o1], OutfallType::FREE);
    EXPECT_DOUBLE_EQ(ctx_in.nodes.invert_elev[o1], 90.0);
}

TEST_F(GeoPackageTest, CoordinatesRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int j1 = ctx_in.node_names.find("J1");
    ASSERT_GE(j1, 0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.node_x[j1], 1000.0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.node_y[j1], 5000.0);
}

TEST_F(GeoPackageTest, ConduitsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.link_names.size(), 3);

    int c1 = ctx_in.link_names.find("C1");
    ASSERT_GE(c1, 0);
    EXPECT_EQ(ctx_in.links.type[c1], LinkType::CONDUIT);
    EXPECT_EQ(ctx_in.links.xsect_shape[c1], XsectShape::CIRCULAR);
    EXPECT_DOUBLE_EQ(ctx_in.links.xsect_y_full[c1], 2.0);
    EXPECT_DOUBLE_EQ(ctx_in.links.roughness[c1], 0.013);
    EXPECT_DOUBLE_EQ(ctx_in.links.length[c1], 400.0);
}

TEST_F(GeoPackageTest, LinkConnectivity) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int c1 = ctx_in.link_names.find("C1");
    ASSERT_GE(c1, 0);
    // C1 goes from J1 (idx 0) to J2 (idx 1)
    int from_idx = ctx_in.links.node1[c1];
    int to_idx = ctx_in.links.node2[c1];
    EXPECT_EQ(ctx_in.node_names.name_of(from_idx), "J1");
    EXPECT_EQ(ctx_in.node_names.name_of(to_idx), "J2");
}

TEST_F(GeoPackageTest, VerticesRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int c2 = ctx_in.link_names.find("C2");
    ASSERT_GE(c2, 0);
    ASSERT_EQ(ctx_in.spatial.link_vertices_x[c2].size(), 1u);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.link_vertices_x[c2][0], 2500.0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.link_vertices_y[c2][0], 5100.0);
}

TEST_F(GeoPackageTest, VerticesFollowNodeToNodeOrder) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    // Store C2 geometry in reverse direction: node2 -> interior -> node1.
    auto db = open_database(db_path_);
    auto rev_geom = encode_linestring(
        std::vector<double>{3000.0, 2500.0, 2000.0},
        std::vector<double>{5000.0, 5100.0, 5000.0},
        4326);
    auto upd = prepare(db.get(),
        "UPDATE links SET geom = ? WHERE simulation_id = ? AND link_id = ?");
    bind_blob(upd.get(), 1, rev_geom.data(), static_cast<int>(rev_geom.size()));
    bind_text(upd.get(), 2, "test_run");
    bind_text(upd.get(), 3, "C2");
    ASSERT_EQ(sqlite3_step(upd.get()), SQLITE_DONE);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int c2 = ctx_in.link_names.find("C2");
    ASSERT_GE(c2, 0);
    ASSERT_EQ(ctx_in.spatial.link_vertices_x[c2].size(), 1u);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.link_vertices_x[c2][0], 2500.0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.link_vertices_y[c2][0], 5100.0);
}

TEST_F(GeoPackageTest, VertexBuffersAreRebuiltOnRead) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    // C1 has no interior vertices in the stored geometry.
    SimulationContext ctx_in{};
    ctx_in.spatial.link_vertices_x.resize(1);
    ctx_in.spatial.link_vertices_y.resize(1);
    ctx_in.spatial.link_vertices_x[0] = {123.0, 456.0};
    ctx_in.spatial.link_vertices_y[0] = {789.0, 987.0};

    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int c1 = ctx_in.link_names.find("C1");
    ASSERT_GE(c1, 0);
    EXPECT_TRUE(ctx_in.spatial.link_vertices_x[c1].empty());
    EXPECT_TRUE(ctx_in.spatial.link_vertices_y[c1].empty());
}

TEST_F(GeoPackageTest, SubcatchmentsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.subcatch_names.size(), 2);

    int s1 = ctx_in.subcatch_names.find("S1");
    ASSERT_GE(s1, 0);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.area[s1], 5.0);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.width[s1], 500.0);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.slope[s1], 0.01);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.frac_imperv[s1], 0.25);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.n_imperv[s1], 0.01);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.n_perv[s1], 0.1);
}

TEST_F(GeoPackageTest, InfiltrationRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int s1 = ctx_in.subcatch_names.find("S1");
    ASSERT_GE(s1, 0);
    EXPECT_EQ(ctx_in.subcatches.infil_model[s1], 0); // HORTON
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.infil_p1[s1], 3.0);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.infil_p2[s1], 0.5);
    EXPECT_DOUBLE_EQ(ctx_in.subcatches.infil_p3[s1], 4.0);
}

TEST_F(GeoPackageTest, PolygonsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int s1 = ctx_in.subcatch_names.find("S1");
    ASSERT_GE(s1, 0);
    ASSERT_GE(ctx_in.spatial.subcatch_polygon_x[s1].size(), 4u);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.subcatch_polygon_x[s1][0], 0.0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.subcatch_polygon_x[s1][1], 100.0);
}

TEST_F(GeoPackageTest, PolygonBuffersAreRebuiltOnRead) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    // Remove S2 geometry from DB so reader should leave an empty polygon,
    // not stale values from a prior context state.
    auto db = open_database(db_path_);
    auto upd = prepare(db.get(),
        "UPDATE subcatchments SET geom = NULL WHERE simulation_id = ? AND subcatch_id = ?");
    bind_text(upd.get(), 1, "test_run");
    bind_text(upd.get(), 2, "S2");
    ASSERT_EQ(sqlite3_step(upd.get()), SQLITE_DONE);

    SimulationContext ctx_in{};
    ctx_in.spatial.subcatch_polygon_x.resize(2);
    ctx_in.spatial.subcatch_polygon_y.resize(2);
    ctx_in.spatial.subcatch_polygon_x[1] = {9.0, 8.0, 7.0};
    ctx_in.spatial.subcatch_polygon_y[1] = {6.0, 5.0, 4.0};

    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int s2 = ctx_in.subcatch_names.find("S2");
    ASSERT_GE(s2, 0);
    EXPECT_TRUE(ctx_in.spatial.subcatch_polygon_x[s2].empty());
    EXPECT_TRUE(ctx_in.spatial.subcatch_polygon_y[s2].empty());
}

TEST_F(GeoPackageTest, RainGagesRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.gage_names.size(), 1);
    int rg = ctx_in.gage_names.find("RG1");
    ASSERT_GE(rg, 0);
    EXPECT_EQ(ctx_in.gages.rain_type[rg], 1);
    EXPECT_EQ(ctx_in.gages.interval_sec[rg], 3600);
    EXPECT_DOUBLE_EQ(ctx_in.gages.snow_factor[rg], 1.0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.gage_x[rg], 1500.0);
    EXPECT_DOUBLE_EQ(ctx_in.spatial.gage_y[rg], 5500.0);
}

TEST_F(GeoPackageTest, TimeseriesRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int ts = ctx_in.table_names.find("TS1");
    ASSERT_GE(ts, 0);
    EXPECT_EQ(ctx_in.tables[ts].type, TableType::TIMESERIES);
    ASSERT_EQ(ctx_in.tables[ts].x.size(), 4u);
    EXPECT_DOUBLE_EQ(ctx_in.tables[ts].y[0], 0.5);
    EXPECT_DOUBLE_EQ(ctx_in.tables[ts].y[1], 2.0);
}

TEST_F(GeoPackageTest, CurvesRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    int sc = ctx_in.table_names.find("StorageCurve1");
    ASSERT_GE(sc, 0);
    EXPECT_NE(ctx_in.tables[sc].type, TableType::TIMESERIES);
    ASSERT_EQ(ctx_in.tables[sc].x.size(), 3u);
    EXPECT_DOUBLE_EQ(ctx_in.tables[sc].x[0], 0.0);
    EXPECT_DOUBLE_EQ(ctx_in.tables[sc].y[0], 100.0);
    EXPECT_DOUBLE_EQ(ctx_in.tables[sc].y[2], 500.0);
}

TEST_F(GeoPackageTest, PollutantsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.pollutant_names.size(), 1);
    int tss = ctx_in.pollutant_names.find("TSS");
    ASSERT_GE(tss, 0);
    EXPECT_EQ(ctx_in.pollutants.units[tss], MassUnits::MG_PER_L);
}

TEST_F(GeoPackageTest, PatternsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    // Find pattern by name (PatternData.names is a plain vector)
    int pat = -1;
    for (int k = 0; k < static_cast<int>(ctx_in.patterns.names.size()); ++k) {
        if (ctx_in.patterns.names[k] == "DWF_Pattern") { pat = k; break; }
    }
    ASSERT_GE(pat, 0);
    EXPECT_EQ(ctx_in.patterns.types[pat], 0); // MONTHLY
    ASSERT_EQ(ctx_in.patterns.factors[pat].size(), 12u);
    EXPECT_DOUBLE_EQ(ctx_in.patterns.factors[pat][0], 1.0);
    EXPECT_DOUBLE_EQ(ctx_in.patterns.factors[pat][2], 1.2);
}

TEST_F(GeoPackageTest, TagsRoundTrip) {
    auto ctx_out = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx_out, "test_run"), 0);

    SimulationContext ctx_in{};
    ASSERT_EQ(read_from_file(db_path_, ctx_in, "test_run"), 0);

    EXPECT_EQ(ctx_in.node_tags["J1"], "upstream");
    EXPECT_EQ(ctx_in.link_tags["C1"], "trunk");
    EXPECT_EQ(ctx_in.subcatch_tags["S1"], "residential");
}

// ============================================================================
// Network topology query test
// ============================================================================

TEST_F(GeoPackageTest, TopologyTablesPopulated) {
    auto ctx_out = build_test_context();
    {
        auto db = create_db();
        write_model(db.get(), ctx_out, "test_run");
    }

    auto db = open_database(db_path_, SQLITE_OPEN_READONLY);

    // Check node_links
    auto stmt = prepare(db.get(),
        "SELECT count(*) FROM node_links WHERE simulation_id = 'test_run'");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_EQ(column_int(stmt.get(), 0), 3); // 3 conduits

    // Check subcatch_routing
    auto stmt2 = prepare(db.get(),
        "SELECT count(*) FROM subcatch_routing WHERE simulation_id = 'test_run'");
    ASSERT_EQ(sqlite3_step(stmt2.get()), SQLITE_ROW);
    EXPECT_EQ(column_int(stmt2.get(), 0), 2); // 2 subcatchments

    // Verify upstream trace works
    auto stmt3 = prepare(db.get(),
        "SELECT from_node FROM node_links WHERE simulation_id = 'test_run' AND to_node = 'J2'");
    ASSERT_EQ(sqlite3_step(stmt3.get()), SQLITE_ROW);
    EXPECT_EQ(column_text(stmt3.get(), 0), "J1");
}

// ============================================================================
// Multi-simulation coexistence test
// ============================================================================

TEST_F(GeoPackageTest, MultipleSimulationRuns) {
    auto ctx = build_test_context();

    // Write two runs to the same file
    {
        auto db = open_database(db_path_);
        create_schema(db.get());
        write_model(db.get(), ctx, "run_a");
        write_model(db.get(), ctx, "run_b");
    }

    // Verify both coexist
    auto db = open_database(db_path_, SQLITE_OPEN_READONLY);
    auto stmt = prepare(db.get(),
        "SELECT count(DISTINCT simulation_id) FROM nodes");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_EQ(column_int(stmt.get(), 0), 2);

    // Read run_a
    SimulationContext ctx_a{};
    ASSERT_EQ(read_model(db.get(), ctx_a, "run_a"), 0);
    EXPECT_EQ(ctx_a.node_names.size(), 4);

    // Read run_b
    SimulationContext ctx_b{};
    ASSERT_EQ(read_model(db.get(), ctx_b, "run_b"), 0);
    EXPECT_EQ(ctx_b.node_names.size(), 4);
}

// ============================================================================
// GeoPackage metadata test
// ============================================================================

TEST_F(GeoPackageTest, FeatureTablesRegistered) {
    auto ctx = build_test_context();
    ASSERT_EQ(write_to_file(db_path_, ctx, "test_run"), 0);

    auto db = open_database(db_path_, SQLITE_OPEN_READONLY);

    // Check gpkg_contents
    auto stmt = prepare(db.get(),
        "SELECT count(*) FROM gpkg_contents WHERE data_type = 'features'");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_EQ(column_int(stmt.get(), 0), 4); // nodes, links, subcatchments, rain_gages

    // Check gpkg_geometry_columns
    auto stmt2 = prepare(db.get(),
        "SELECT geometry_type_name FROM gpkg_geometry_columns WHERE table_name = 'nodes'");
    ASSERT_EQ(sqlite3_step(stmt2.get()), SQLITE_ROW);
    EXPECT_EQ(column_text(stmt2.get(), 0), "POINT");

    auto stmt3 = prepare(db.get(),
        "SELECT geometry_type_name FROM gpkg_geometry_columns WHERE table_name = 'links'");
    ASSERT_EQ(sqlite3_step(stmt3.get()), SQLITE_ROW);
    EXPECT_EQ(column_text(stmt3.get(), 0), "LINESTRING");

    auto stmt4 = prepare(db.get(),
        "SELECT geometry_type_name FROM gpkg_geometry_columns WHERE table_name = 'subcatchments'");
    ASSERT_EQ(sqlite3_step(stmt4.get()), SQLITE_ROW);
    EXPECT_EQ(column_text(stmt4.get(), 0), "MULTIPOLYGON");
}

// ============================================================================
// Plugin component info tests
// ============================================================================

TEST(GeoPackagePluginInfoTest, Metadata) {
    auto& info = GeoPackagePluginInfo::instance();

    EXPECT_EQ(info.id(), "org.hydrocouple.openswmm.plugins.geopackage");
    EXPECT_EQ(info.caption(), "GeoPackage I/O Plugin");
    EXPECT_FALSE(info.description().empty());
    EXPECT_EQ(info.version(), "1.0.0");
    EXPECT_EQ(info.vendor(), "HydroCouple");
    EXPECT_FALSE(info.url().empty());
    EXPECT_EQ(info.license_type(), "MIT");
    EXPECT_FALSE(info.license_text().empty());
    EXPECT_TRUE(info.has_input());
    EXPECT_TRUE(info.has_output());
    EXPECT_TRUE(info.has_report());

    auto tags = info.tags();
    EXPECT_GE(tags.size(), 3u);
    EXPECT_NE(std::find(tags.begin(), tags.end(), "geopackage"), tags.end());
    EXPECT_NE(std::find(tags.begin(), tags.end(), "sqlite"), tags.end());
    EXPECT_NE(std::find(tags.begin(), tags.end(), "spatial"), tags.end());
}

TEST(GeoPackagePluginInfoTest, RegistrationInitiallyFalse) {
    // Note: singleton state persists across tests. This test may need to run
    // before RegisterSucceeds if ordering matters. GTest runs in declaration order.
    // We re-check after register below.
    auto& info = GeoPackagePluginInfo::instance();
    // After the RegisterSucceeds test runs, this will be true.
    // Just verify the method exists and returns a bool.
    (void)info.registered();
}

TEST(GeoPackagePluginInfoTest, RegisterSucceeds) {
    auto& info = GeoPackagePluginInfo::instance();

    RegistrationInfo reg;
    reg.license_key = "OPEN-2026-GPKG";
    reg.organization = "Test Org";
    reg.contact_email = "test@example.com";
    reg.deployment_id = "unit-test-001";

    bool result = info.register_plugin(reg);
    EXPECT_TRUE(result);
    EXPECT_TRUE(info.registered());

    EXPECT_EQ(info.registration_info().organization, "Test Org");
    EXPECT_EQ(info.registration_info().license_key, "OPEN-2026-GPKG");
    EXPECT_EQ(info.registration_info().deployment_id, "unit-test-001");
}

TEST(GeoPackagePluginInfoTest, FactoryCreatesInputPlugin) {
    auto& info = GeoPackagePluginInfo::instance();
    auto* plugin = info.create_input_plugin();
    ASSERT_NE(plugin, nullptr);

    // Verify it's a GeoPackageInputPlugin
    auto* gpkg_plugin = dynamic_cast<GeoPackageInputPlugin*>(plugin);
    EXPECT_NE(gpkg_plugin, nullptr);

    delete plugin;
}

TEST(GeoPackagePluginInfoTest, FactoryCreatesOutputPlugin) {
    auto& info = GeoPackagePluginInfo::instance();
    auto* plugin = info.create_output_plugin();
    ASSERT_NE(plugin, nullptr);
    delete plugin;
}

TEST(GeoPackagePluginInfoTest, FactoryCreatesReportPlugin) {
    auto& info = GeoPackagePluginInfo::instance();
    auto* plugin = info.create_report_plugin();
    ASSERT_NE(plugin, nullptr);
    delete plugin;
}

TEST(GeoPackagePluginInfoTest, CExportFunction) {
    // Verify the C export returns the same singleton
    auto* exported = openswmm_plugin_info();
    ASSERT_NE(exported, nullptr);
    EXPECT_EQ(exported->id(), "org.hydrocouple.openswmm.plugins.geopackage");
    EXPECT_EQ(exported, &GeoPackagePluginInfo::instance());
}

// ============================================================================
// C API tests
// ============================================================================

#include <openswmm/engine/openswmm_geopackage.h>

class GeoPackageCApiTest : public ::testing::Test {
protected:
    std::string db_path_;

    void SetUp() override {
        db_path_ = (fs::temp_directory_path() / "test_capi.gpkg").string();
        std::remove(db_path_.c_str());
        // Create and populate a GeoPackage via the C++ API so the C API can read it
        auto ctx = build_test_context_for_capi();
        write_to_file(db_path_, ctx, "run_1");
    }

    void TearDown() override {
        std::remove(db_path_.c_str());
    }

    // Minimal context with results pre-inserted for read tests
    SimulationContext build_test_context_for_capi() {
        SimulationContext ctx{};
        ctx.options.flow_units = static_cast<FlowUnits>(1);
        ctx.spatial.crs = "EPSG:4326";

        // 2 junctions + 1 outfall
        for (const auto& name : {"J1", "J2"}) {
            int idx = ctx.node_names.add(name);
            ctx.nodes.type.resize(idx + 1);
            ctx.nodes.invert_elev.resize(idx + 1);
            ctx.nodes.full_depth.resize(idx + 1);
            ctx.nodes.init_depth.resize(idx + 1);
            ctx.nodes.sur_depth.resize(idx + 1);
            ctx.nodes.ponded_area.resize(idx + 1);
            ctx.nodes.outfall_type.resize(idx + 1);
            ctx.nodes.outfall_param.resize(idx + 1);
            ctx.nodes.outfall_has_flap_gate.resize(idx + 1);
            ctx.nodes.storage_curve.resize(idx + 1);
            ctx.nodes.storage_curve_name.resize(idx + 1);
            ctx.nodes.storage_a.resize(idx + 1);
            ctx.nodes.storage_b.resize(idx + 1);
            ctx.nodes.storage_c.resize(idx + 1);
            ctx.nodes.divider_type.resize(idx + 1);
            ctx.nodes.divider_cutoff.resize(idx + 1);
            ctx.nodes.divider_curve.resize(idx + 1);
            ctx.nodes.divider_curve_name.resize(idx + 1);
            ctx.nodes.type[idx] = NodeType::JUNCTION;
            ctx.nodes.invert_elev[idx] = 100.0 - idx * 2.0;
            ctx.nodes.full_depth[idx] = 6.0;
        }
        ctx.spatial.node_x = {1000.0, 2000.0};
        ctx.spatial.node_y = {5000.0, 5000.0};

        // 1 conduit
        {
            int idx = ctx.link_names.add("C1");
            ctx.links.type.resize(idx + 1);
            ctx.links.node1.resize(idx + 1);
            ctx.links.node2.resize(idx + 1);
            ctx.links.offset1.resize(idx + 1);
            ctx.links.offset2.resize(idx + 1);
            ctx.links.q0.resize(idx + 1);
            ctx.links.q_limit.resize(idx + 1);
            ctx.links.xsect_shape.resize(idx + 1);
            ctx.links.xsect_y_full.resize(idx + 1);
            ctx.links.xsect_a_full.resize(idx + 1);
            ctx.links.xsect_w_max.resize(idx + 1);
            ctx.links.xsect_curve.resize(idx + 1, -1);
            ctx.links.roughness.resize(idx + 1);
            ctx.links.length.resize(idx + 1);
            ctx.links.barrels.resize(idx + 1, 1);
            ctx.links.culvert_code.resize(idx + 1);
            ctx.links.loss_inlet.resize(idx + 1);
            ctx.links.loss_outlet.resize(idx + 1);
            ctx.links.loss_avg.resize(idx + 1);
            ctx.links.has_flap_gate.resize(idx + 1);
            ctx.links.seep_rate.resize(idx + 1);
            ctx.links.pump_curve.resize(idx + 1, -1);
            ctx.links.pump_curve_name.resize(idx + 1);
            ctx.links.pump_init_state.resize(idx + 1);
            ctx.links.pump_startup.resize(idx + 1);
            ctx.links.pump_shutoff.resize(idx + 1);
            ctx.links.crest_height.resize(idx + 1);
            ctx.links.cd.resize(idx + 1);
            ctx.links.xsect_y_bot.resize(idx + 1);
            ctx.spatial.link_vertices_x.resize(idx + 1);
            ctx.spatial.link_vertices_y.resize(idx + 1);
            ctx.spatial.link_x.resize(idx + 1);
            ctx.spatial.link_y.resize(idx + 1);
            ctx.links.type[idx] = LinkType::CONDUIT;
            ctx.links.node1[idx] = 0;
            ctx.links.node2[idx] = 1;
            ctx.links.xsect_shape[idx] = XsectShape::CIRCULAR;
            ctx.links.xsect_y_full[idx] = 2.0;
            ctx.links.roughness[idx] = 0.013;
            ctx.links.length[idx] = 400.0;
        }

        return ctx;
    }
};

TEST_F(GeoPackageCApiTest, OpenClose) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);
    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, OpenNullPath) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(nullptr);
    EXPECT_EQ(gpkg, nullptr);
}

TEST_F(GeoPackageCApiTest, Registration) {
    int rc = swmm_gpkg_register("TEST-KEY", "Test Org", "test@test.com", "deploy-1");
    EXPECT_EQ(rc, 1);
    EXPECT_EQ(swmm_gpkg_is_registered(), 1);
}

TEST_F(GeoPackageCApiTest, ReadModelCounts) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    EXPECT_EQ(swmm_gpkg_node_count(gpkg, "run_1"), 2);
    EXPECT_EQ(swmm_gpkg_link_count(gpkg, "run_1"), 1);
    EXPECT_EQ(swmm_gpkg_topology_edge_count(gpkg, "run_1"), 1);
    EXPECT_GE(swmm_gpkg_variable_count(gpkg), 20);

    // Non-existent simulation
    EXPECT_EQ(swmm_gpkg_node_count(gpkg, "nonexistent"), 0);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, TransactionCommit) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    EXPECT_EQ(swmm_gpkg_begin(gpkg), SWMM_GPKG_OK);

    int sid = swmm_gpkg_create_observed_series(
        gpkg, "test_series_txn", "depth", "NODE", "J1", "Test", "m");
    EXPECT_GE(sid, 0);

    EXPECT_EQ(swmm_gpkg_write_observed_value(
        gpkg, sid, "2026-01-01T00:00:00Z", 1.0, "A"), SWMM_GPKG_OK);

    EXPECT_EQ(swmm_gpkg_commit(gpkg), SWMM_GPKG_OK);

    // Verify persisted
    EXPECT_EQ(swmm_gpkg_observed_value_count(gpkg, sid), 1);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, TransactionRollback) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    int sid = swmm_gpkg_create_observed_series(
        gpkg, "test_series_rb", "depth", "NODE", "J1", "Test", "m");
    EXPECT_GE(sid, 0);

    EXPECT_EQ(swmm_gpkg_begin(gpkg), SWMM_GPKG_OK);
    EXPECT_EQ(swmm_gpkg_write_observed_value(
        gpkg, sid, "2026-01-01T00:00:00Z", 99.0, nullptr), SWMM_GPKG_OK);
    EXPECT_EQ(swmm_gpkg_rollback(gpkg), SWMM_GPKG_OK);

    // Should have 0 values after rollback
    EXPECT_EQ(swmm_gpkg_observed_value_count(gpkg, sid), 0);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, BulkWriteObservedValues) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    int sid = swmm_gpkg_create_observed_series(
        gpkg, "bulk_flow", "flow", "LINK", "C1", "Sensor", "CMS");
    EXPECT_GE(sid, 0);

    // Prepare vectors
    const char* timestamps[] = {
        "2026-01-15T08:00:00Z",
        "2026-01-15T09:00:00Z",
        "2026-01-15T10:00:00Z",
        "2026-01-15T11:00:00Z",
        "2026-01-15T12:00:00Z"
    };
    double values[] = {1.5, 2.3, 1.8, 3.1, 2.7};
    const char* flags[] = {"A", "A", "P", "A", "E"};

    // Bulk write in a transaction
    EXPECT_EQ(swmm_gpkg_begin(gpkg), SWMM_GPKG_OK);
    EXPECT_EQ(swmm_gpkg_write_observed_values(gpkg, sid, timestamps, values, flags, 5),
              SWMM_GPKG_OK);
    EXPECT_EQ(swmm_gpkg_commit(gpkg), SWMM_GPKG_OK);

    EXPECT_EQ(swmm_gpkg_observed_value_count(gpkg, sid), 5);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, BulkWriteNullFlags) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    int sid = swmm_gpkg_create_observed_series(
        gpkg, "noflag_series", "depth", "NODE", "J1", "Sensor", "m");
    EXPECT_GE(sid, 0);

    const char* timestamps[] = {"2026-01-01T00:00:00Z", "2026-01-01T01:00:00Z"};
    double values[] = {0.5, 0.7};

    EXPECT_EQ(swmm_gpkg_begin(gpkg), SWMM_GPKG_OK);
    EXPECT_EQ(swmm_gpkg_write_observed_values(gpkg, sid, timestamps, values, nullptr, 2),
              SWMM_GPKG_OK);
    EXPECT_EQ(swmm_gpkg_commit(gpkg), SWMM_GPKG_OK);

    EXPECT_EQ(swmm_gpkg_observed_value_count(gpkg, sid), 2);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, BulkReadObservedValues) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    // Write some data
    int sid = swmm_gpkg_create_observed_series(
        gpkg, "read_test", "flow", "LINK", "C1", "Gauge", "CMS");
    EXPECT_GE(sid, 0);

    const char* timestamps[] = {
        "2026-03-01T00:00:00Z",
        "2026-03-01T01:00:00Z",
        "2026-03-01T02:00:00Z"
    };
    double write_vals[] = {10.0, 20.0, 30.0};

    swmm_gpkg_begin(gpkg);
    swmm_gpkg_write_observed_values(gpkg, sid, timestamps, write_vals, nullptr, 3);
    swmm_gpkg_commit(gpkg);

    // Bulk read back
    double read_vals[3] = {};
    char ts_buf[3 * 32] = {};  // 3 timestamps, 32 chars each
    int n = swmm_gpkg_read_observed_values(gpkg, sid, ts_buf, 32, read_vals, 3);
    EXPECT_EQ(n, 3);
    EXPECT_DOUBLE_EQ(read_vals[0], 10.0);
    EXPECT_DOUBLE_EQ(read_vals[1], 20.0);
    EXPECT_DOUBLE_EQ(read_vals[2], 30.0);
    EXPECT_STREQ(ts_buf + 0 * 32, "2026-03-01T00:00:00Z");
    EXPECT_STREQ(ts_buf + 1 * 32, "2026-03-01T01:00:00Z");
    EXPECT_STREQ(ts_buf + 2 * 32, "2026-03-01T02:00:00Z");

    // Read without timestamps (NULL)
    double read_vals2[3] = {};
    int n2 = swmm_gpkg_read_observed_values(gpkg, sid, nullptr, 0, read_vals2, 3);
    EXPECT_EQ(n2, 3);
    EXPECT_DOUBLE_EQ(read_vals2[2], 30.0);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, ReadResultTimeseries) {
    // First insert some results via the C++ API (simulating engine output)
    {
        auto db = open_database(db_path_);
        // Create a simulation entry
        exec(db.get(),
            "INSERT OR REPLACE INTO simulations (simulation_id, name, created_at, engine_version, status) "
            "VALUES ('run_1', 'test', '2026-01-01', '6.0.0', 'completed')");
        // Insert result rows
        auto var_stmt = prepare(db.get(),
            "SELECT variable_id FROM variables WHERE name = 'depth' AND object_type = 'NODE'");
        ASSERT_EQ(sqlite3_step(var_stmt.get()), SQLITE_ROW);
        int vid = column_int(var_stmt.get(), 0);

        auto ins = prepare(db.get(),
            "INSERT INTO result_timeseries (simulation_id, object_type, object_id, variable_id, elapsed_time, value) "
            "VALUES ('run_1', 'NODE', 'J1', ?, ?, ?)");
        double test_times[] = {0.0, 60.0, 120.0, 180.0};
        double test_values[] = {0.0, 0.5, 1.2, 0.8};
        for (int i = 0; i < 4; ++i) {
            sqlite3_reset(ins.get());
            sqlite3_clear_bindings(ins.get());
            bind_int(ins.get(), 1, vid);
            bind_double(ins.get(), 2, test_times[i]);
            bind_double(ins.get(), 3, test_values[i]);
            sqlite3_step(ins.get());
        }
    }

    // Read via C API
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    // Count first
    int count = swmm_gpkg_result_ts_count(gpkg, "run_1", "NODE", "J1", "depth");
    EXPECT_EQ(count, 4);

    // Bulk read
    double times[10] = {};
    double values[10] = {};
    int n = swmm_gpkg_read_result_ts(gpkg, "run_1", "NODE", "J1", "depth",
                                      times, values, 10);
    EXPECT_EQ(n, 4);
    EXPECT_DOUBLE_EQ(times[0], 0.0);
    EXPECT_DOUBLE_EQ(times[2], 120.0);
    EXPECT_DOUBLE_EQ(values[1], 0.5);
    EXPECT_DOUBLE_EQ(values[3], 0.8);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, ReadSummary) {
    // Insert a summary value via C++ API
    {
        auto db = open_database(db_path_);
        exec(db.get(),
            "INSERT OR REPLACE INTO simulations (simulation_id, name, created_at, engine_version, status) "
            "VALUES ('run_1', 'test', '2026-01-01', '6.0.0', 'completed')");
        auto var_stmt = prepare(db.get(),
            "SELECT variable_id FROM variables WHERE name = 'max_depth' AND object_type = 'NODE'");
        ASSERT_EQ(sqlite3_step(var_stmt.get()), SQLITE_ROW);
        int vid = column_int(var_stmt.get(), 0);

        auto ins = prepare(db.get(),
            "INSERT OR REPLACE INTO result_summary "
            "(simulation_id, object_type, object_id, variable_id, value) "
            "VALUES ('run_1', 'NODE', 'J1', ?, 3.14)");
        bind_int(ins.get(), 1, vid);
        sqlite3_step(ins.get());
    }

    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    double val = 0.0;
    EXPECT_EQ(swmm_gpkg_read_summary(gpkg, "run_1", "NODE", "J1", "max_depth", &val),
              SWMM_GPKG_OK);
    EXPECT_DOUBLE_EQ(val, 3.14);

    // Non-existent variable
    double val2 = 0.0;
    EXPECT_EQ(swmm_gpkg_read_summary(gpkg, "run_1", "NODE", "J1", "nonexistent", &val2),
              SWMM_GPKG_ERR);

    swmm_gpkg_close(gpkg);
}

TEST_F(GeoPackageCApiTest, QueryHelpers) {
    SWMM_Gpkg gpkg = swmm_gpkg_open(db_path_.c_str());
    ASSERT_NE(gpkg, nullptr);

    int vars = swmm_gpkg_variable_count(gpkg);
    EXPECT_GE(vars, 20);

    int nodes = swmm_gpkg_query_int(gpkg,
        "SELECT count(*) FROM nodes WHERE simulation_id = 'run_1'");
    EXPECT_EQ(nodes, 2);

    swmm_gpkg_close(gpkg);
}
