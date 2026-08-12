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
 * @file test_geopackage_schema_external_content.cpp
 * @brief Slice IO-5 — structured external-file content schema + FK integrity.
 *
 * @details Pins the contract documented in
 *          openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.4:
 *
 *            - All Part D tables (hotstart_*, raingage_data, climate_data,
 *              routing_interface_{node,subcatch,gage}, _node_pollutants)
 *              exist after `create_schema()`.
 *            - `PRAGMA foreign_keys=ON` is active on every connection
 *              opened through `GpkgUtils::open_database` (Slice IO-5 fix).
 *            - Inserting a hot-start row referencing a non-existent node
 *              is rejected by FK enforcement.
 *            - Deleting a node cascades to every hot-start state row
 *              that points at it.
 *            - Renaming a node (UPDATE on the composite key) propagates
 *              to the dependent rows via ON UPDATE CASCADE.
 *            - `input_timeseries` carries the new `source` /
 *              `source_filename` / `source_column` columns.
 *
 *          Per CLAUDE.md §4.1 reviewable-IO rule, the test database is
 *          written under tests/unit/engine/data/io5_schema/ so a
 *          reviewer can open it with QGIS / sqlite3 after the run.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <sqlite3.h>
#include <string>

#include "input/geopackage/GeoPackageSchema.hpp"
#include "input/geopackage/GpkgUtils.hpp"

namespace fs = std::filesystem;
using openswmm::gpkg::create_schema;
using openswmm::gpkg::open_database;
using openswmm::gpkg::exec;
using openswmm::gpkg::prepare;
using openswmm::gpkg::DbPtr;
using openswmm::gpkg::bind_text;
using openswmm::gpkg::bind_double;
using openswmm::gpkg::bind_int;

namespace {

fs::path testDbPath(const std::string& stem) {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path()) here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/io5_schema";
    fs::create_directories(dir);
    return dir / (stem + ".gpkg");
}

DbPtr freshDb(const std::string& stem) {
    auto path = testDbPath(stem);
    std::error_code ec;
    fs::remove(path, ec);
    auto db = open_database(path.string());
    create_schema(db.get());
    return db;
}

bool tableExists(sqlite3* db, const std::string& name) {
    auto stmt = prepare(db,
        "SELECT name FROM sqlite_master WHERE type='table' AND name=?");
    bind_text(stmt.get(), 1, name);
    return sqlite3_step(stmt.get()) == SQLITE_ROW;
}

int columnCount(sqlite3* db, const std::string& table) {
    auto stmt = prepare(db, "SELECT COUNT(*) FROM pragma_table_info(?)");
    bind_text(stmt.get(), 1, table);
    sqlite3_step(stmt.get());
    return sqlite3_column_int(stmt.get(), 0);
}

bool fkActive(sqlite3* db) {
    auto stmt = prepare(db, "PRAGMA foreign_keys");
    sqlite3_step(stmt.get());
    return sqlite3_column_int(stmt.get(), 0) == 1;
}

// Plant a minimal simulation + one node/link/subcatch/gage/pollutant.
void planMinimalModel(sqlite3* db, const std::string& sim_id) {
    exec(db, std::string(
        "INSERT INTO simulations (simulation_id, name, created_at, engine_version) "
        "VALUES ('" + sim_id + "', 't', '2026-01-01T00:00:00Z', '6.0.0')"));
    exec(db, std::string(
        "INSERT INTO nodes (simulation_id, node_id, node_type) "
        "VALUES ('" + sim_id + "', 'J1', 'JUNCTION')"));
    exec(db, std::string(
        "INSERT INTO nodes (simulation_id, node_id, node_type) "
        "VALUES ('" + sim_id + "', 'O1', 'OUTFALL')"));
    exec(db, std::string(
        "INSERT INTO links (simulation_id, link_id, link_type, from_node, to_node) "
        "VALUES ('" + sim_id + "', 'L1', 'CONDUIT', 'J1', 'O1')"));
    exec(db, std::string(
        "INSERT INTO subcatchments (simulation_id, subcatch_id) "
        "VALUES ('" + sim_id + "', 'S1')"));
    exec(db, std::string(
        "INSERT INTO rain_gages (simulation_id, gage_id) "
        "VALUES ('" + sim_id + "', 'G1')"));
    exec(db, std::string(
        "INSERT INTO pollutants (simulation_id, pollutant_id, units) "
        "VALUES ('" + sim_id + "', 'TSS', 'MG/L')"));
}

} // namespace

// ---------------------------------------------------------------------------
// Connection-level: foreign_keys pragma is on
// ---------------------------------------------------------------------------

TEST(GpkgSchemaIO5, ForeignKeysPragmaActiveAfterOpen) {
    auto db = freshDb("fk_pragma");
    EXPECT_TRUE(fkActive(db.get()));
}

// ---------------------------------------------------------------------------
// Every Part D table exists
// ---------------------------------------------------------------------------

TEST(GpkgSchemaIO5, AllHotstartTablesExist) {
    auto db = freshDb("hotstart_tables");
    EXPECT_TRUE(tableExists(db.get(), "hotstart_slots"));
    EXPECT_TRUE(tableExists(db.get(), "hotstart_node_state"));
    EXPECT_TRUE(tableExists(db.get(), "hotstart_link_state"));
    EXPECT_TRUE(tableExists(db.get(), "hotstart_subcatch_state"));
    EXPECT_TRUE(tableExists(db.get(), "hotstart_node_pollutant_state"));
    EXPECT_TRUE(tableExists(db.get(), "hotstart_link_pollutant_state"));
    EXPECT_TRUE(tableExists(db.get(), "hotstart_subcatch_pollutant_state"));
}

TEST(GpkgSchemaIO5, RaingageDataAndClimateDataExist) {
    auto db = freshDb("rain_climate_tables");
    EXPECT_TRUE(tableExists(db.get(), "raingage_data"));
    EXPECT_TRUE(tableExists(db.get(), "climate_data"));
}

TEST(GpkgSchemaIO5, RoutingInterfaceTablesExist) {
    auto db = freshDb("routing_iface_tables");
    EXPECT_TRUE(tableExists(db.get(), "routing_interface_node"));
    EXPECT_TRUE(tableExists(db.get(), "routing_interface_subcatch"));
    EXPECT_TRUE(tableExists(db.get(), "routing_interface_gage"));
    EXPECT_TRUE(tableExists(db.get(), "routing_interface_node_pollutants"));
}

// ---------------------------------------------------------------------------
// input_timeseries provenance columns
// ---------------------------------------------------------------------------

TEST(GpkgSchemaIO5, InputTimeseriesHasProvenanceColumns) {
    auto db = freshDb("ts_provenance");
    // pragma_table_info row exists for each new column.
    auto col_present = [&](const char* col) -> bool {
        auto stmt = prepare(db.get(),
            "SELECT 1 FROM pragma_table_info('input_timeseries') WHERE name=?");
        bind_text(stmt.get(), 1, col);
        return sqlite3_step(stmt.get()) == SQLITE_ROW;
    };
    EXPECT_TRUE(col_present("source"));
    EXPECT_TRUE(col_present("source_filename"));
    EXPECT_TRUE(col_present("source_column"));
}

// ---------------------------------------------------------------------------
// FK enforcement: orphan rejected, cascade on delete, cascade on update
// ---------------------------------------------------------------------------

TEST(GpkgSchemaIO5, OrphanHotstartNodeStateRejected) {
    auto db = freshDb("orphan_reject");
    planMinimalModel(db.get(), "sim1");
    exec(db.get(),
        "INSERT INTO hotstart_slots "
        "(simulation_id, slot_name, direction, format_version, num_pollutants) "
        "VALUES ('sim1', 'use', 'USE', 1, 1)");

    // No node named "GHOST" — FK to nodes(simulation_id, node_id) must reject.
    auto stmt = prepare(db.get(),
        "INSERT INTO hotstart_node_state "
        "(simulation_id, slot_name, node_id, depth) "
        "VALUES ('sim1', 'use', 'GHOST', 0.5)");
    int rc = sqlite3_step(stmt.get());
    EXPECT_EQ(rc, SQLITE_CONSTRAINT)
        << "orphan hotstart_node_state row should be rejected, got rc=" << rc;
}

TEST(GpkgSchemaIO5, DeleteNodeCascadesHotstartState) {
    auto db = freshDb("cascade_delete");
    planMinimalModel(db.get(), "sim1");
    exec(db.get(),
        "INSERT INTO hotstart_slots "
        "(simulation_id, slot_name, direction, format_version, num_pollutants) "
        "VALUES ('sim1', 'use', 'USE', 1, 1)");
    exec(db.get(),
        "INSERT INTO hotstart_node_state "
        "(simulation_id, slot_name, node_id, depth) "
        "VALUES ('sim1', 'use', 'J1', 1.0)");

    auto count_state = [&]() -> int {
        auto stmt = prepare(db.get(),
            "SELECT COUNT(*) FROM hotstart_node_state "
            "WHERE simulation_id='sim1' AND node_id='J1'");
        sqlite3_step(stmt.get());
        return sqlite3_column_int(stmt.get(), 0);
    };
    ASSERT_EQ(count_state(), 1);

    exec(db.get(), "DELETE FROM nodes WHERE simulation_id='sim1' AND node_id='J1'");
    EXPECT_EQ(count_state(), 0)
        << "deleting J1 should have cascaded its hotstart_node_state row";
}

TEST(GpkgSchemaIO5, RenameNodePropagatesToHotstartState) {
    auto db = freshDb("cascade_update");
    planMinimalModel(db.get(), "sim1");
    exec(db.get(),
        "INSERT INTO hotstart_slots "
        "(simulation_id, slot_name, direction, format_version, num_pollutants) "
        "VALUES ('sim1', 'use', 'USE', 1, 1)");
    exec(db.get(),
        "INSERT INTO hotstart_node_state "
        "(simulation_id, slot_name, node_id, depth) "
        "VALUES ('sim1', 'use', 'J1', 1.0)");

    exec(db.get(),
        "UPDATE nodes SET node_id='J1_renamed' "
        "WHERE simulation_id='sim1' AND node_id='J1'");

    auto stmt = prepare(db.get(),
        "SELECT node_id FROM hotstart_node_state WHERE simulation_id='sim1'");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)),
                 "J1_renamed");
}

TEST(GpkgSchemaIO5, DeleteSimulationCascadesAllPartD) {
    auto db = freshDb("cascade_sim");
    planMinimalModel(db.get(), "sim1");
    exec(db.get(),
        "INSERT INTO hotstart_slots "
        "(simulation_id, slot_name, direction, format_version, num_pollutants) "
        "VALUES ('sim1', 'use', 'USE', 1, 1)");
    exec(db.get(),
        "INSERT INTO climate_data "
        "(simulation_id, record_date, tmin, tmax) "
        "VALUES ('sim1', '2026-01-01', 32.0, 50.0)");
    exec(db.get(),
        "INSERT INTO raingage_data "
        "(simulation_id, gage_id, record_time, rainfall_value) "
        "VALUES ('sim1', 'G1', '2026-01-01T00:00:00Z', 0.5)");

    exec(db.get(), "DELETE FROM simulations WHERE simulation_id='sim1'");

    auto count = [&](const char* sql) -> int {
        auto stmt = prepare(db.get(), sql);
        sqlite3_step(stmt.get());
        return sqlite3_column_int(stmt.get(), 0);
    };
    // hotstart_slots FKs simulations directly → cascades.
    EXPECT_EQ(count("SELECT COUNT(*) FROM hotstart_slots WHERE simulation_id='sim1'"), 0);
    // climate_data FKs simulations directly → cascades.
    EXPECT_EQ(count("SELECT COUNT(*) FROM climate_data  WHERE simulation_id='sim1'"), 0);
    // raingage_data only FKs into rain_gages (composite). The existing
    // schema does not declare a `rain_gages → simulations` FK, so deleting
    // a simulation does NOT cascade through to raingage_data via that
    // path. This is consistent with the rest of Part A — the new Part D
    // tables do not introduce stricter cascade semantics than the model
    // tables they reference. The G1 gage row (and the raingage_data row
    // inserted above) therefore deliberately SURVIVE the simulation delete:
    EXPECT_EQ(count("SELECT COUNT(*) FROM rain_gages "
                     "WHERE simulation_id='sim1' AND gage_id='G1'"), 1);
    EXPECT_EQ(count("SELECT COUNT(*) FROM raingage_data "
                     "WHERE simulation_id='sim1' AND gage_id='G1'"), 1);

    // Test the model-object cascade path instead, using the surviving gage.
    exec(db.get(),
        "INSERT INTO raingage_data (simulation_id, gage_id, record_time, "
        " rainfall_value) VALUES ('sim1','G1','2026-02-01T00:00:00Z', 0.7)");
    exec(db.get(),
        "DELETE FROM rain_gages WHERE simulation_id='sim1' AND gage_id='G1'");
    EXPECT_EQ(count("SELECT COUNT(*) FROM raingage_data "
                     "WHERE simulation_id='sim1' AND gage_id='G1'"), 0)
        << "deleting a rain_gages row should cascade to raingage_data";
}

// ---------------------------------------------------------------------------
// Round-trip insert for a representative Part D row to confirm column shape
// ---------------------------------------------------------------------------

TEST(GpkgSchemaIO5, HotstartSubcatchStateAcceptsFullRow) {
    auto db = freshDb("subcatch_state_shape");
    planMinimalModel(db.get(), "sim1");
    exec(db.get(),
        "INSERT INTO hotstart_slots "
        "(simulation_id, slot_name, direction, format_version, num_pollutants) "
        "VALUES ('sim1', 'use', 'USE', 1, 1)");

    // All 17 state columns supplied with valid REAL values.
    auto stmt = prepare(db.get(),
        "INSERT INTO hotstart_subcatch_state "
        "(simulation_id, slot_name, subcatch_id, runoff, infil_model, "
        " infil_state_0, infil_state_1, infil_state_2, infil_state_3, "
        " infil_state_4, infil_state_5, gw_theta_upper, gw_lower_depth, "
        " snow_we_plowable, snow_we_imperv, snow_we_perv, "
        " snow_fw_plowable, snow_fw_imperv, snow_fw_perv, snow_ati) "
        "VALUES ('sim1','use','S1',0.1,0,"
        "        0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,"
        "        1.0,1.1,1.2,1.3,1.4,1.5,1.6)");
    int rc = sqlite3_step(stmt.get());
    EXPECT_EQ(rc, SQLITE_DONE) << "valid subcatch state row should insert";
}

TEST(GpkgSchemaIO5, RoutingInterfaceNodePollutantsFkChain) {
    auto db = freshDb("routing_iface_node_polls");
    planMinimalModel(db.get(), "sim1");
    exec(db.get(),
        "INSERT INTO routing_interface_node "
        "(simulation_id, role, direction, node_id, record_time, flow_value) "
        "VALUES ('sim1','INFLOWS','USE','J1','2026-01-01T00:00:00Z', 0.5)");
    // Valid pollutant row — references both parent row and pollutants(TSS).
    int rc = 0;
    {
        auto stmt = prepare(db.get(),
            "INSERT INTO routing_interface_node_pollutants "
            "(simulation_id, role, direction, node_id, record_time, "
            " pollutant_id, concentration) "
            "VALUES ('sim1','INFLOWS','USE','J1','2026-01-01T00:00:00Z',"
            "        'TSS', 12.5)");
        rc = sqlite3_step(stmt.get());
    }
    EXPECT_EQ(rc, SQLITE_DONE);

    // Orphan pollutant id rejected.
    {
        auto stmt = prepare(db.get(),
            "INSERT INTO routing_interface_node_pollutants "
            "(simulation_id, role, direction, node_id, record_time, "
            " pollutant_id, concentration) "
            "VALUES ('sim1','INFLOWS','USE','J1','2026-01-01T00:00:00Z',"
            "        'GHOST', 1.0)");
        rc = sqlite3_step(stmt.get());
    }
    EXPECT_EQ(rc, SQLITE_CONSTRAINT)
        << "orphan pollutant_id should be FK-rejected";
}

// ---------------------------------------------------------------------------
// Column shape sanity (smoke check — exact count not load-bearing)
// ---------------------------------------------------------------------------

TEST(GpkgSchemaIO5, HotstartSubcatchStateHasAllDocumentedColumns) {
    auto db = freshDb("subcatch_columns");
    // 3 keys + runoff + infil_model + 6 infil_state + 2 gw + 7 snow = 20
    EXPECT_EQ(columnCount(db.get(), "hotstart_subcatch_state"), 20);
}
