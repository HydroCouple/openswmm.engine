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
 * @file test_geopackage_external_content_writer.cpp
 * @brief Slice IO-7 — verify `write_external_content` fans out the
 *        in-memory external-file slots into the Part D content tables.
 *
 * @details Each case:
 *            1. Lays down a fresh GeoPackage at
 *               tests/unit/engine/data/io7_writer/<case>.gpkg.
 *            2. Plants a minimal model in the Part A tables (just the
 *               parent rows needed for FK satisfaction).
 *            3. Writes a real external file (timeseries / climate /
 *               raingage / hot-start / routing interface) under
 *               tests/unit/engine/data/io7_writer/<case>_input.<ext>
 *               so a reviewer can open it.
 *            4. Builds a SimulationContext with the external-file slot
 *               pointing at that real file.
 *            5. Calls `write_external_content` and queries the Part D
 *               tables, asserting row counts + selected values.
 *
 *          USE-direction missing-file and SAVE-pending behaviours are
 *          covered by dedicated tests.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>

#include "core/DateTime.hpp"
#include "core/SimulationContext.hpp"
#include "data/NodeData.hpp"
#include "data/LinkData.hpp"
#include "data/SubcatchData.hpp"
#include "data/GageData.hpp"
#include "data/TableData.hpp"
#include "data/PollutantData.hpp"
#include "input/geopackage/ExternalContentWriter.hpp"
#include "input/geopackage/GeoPackageSchema.hpp"
#include "input/geopackage/GpkgUtils.hpp"

namespace fs = std::filesystem;
using openswmm::SimulationContext;
using openswmm::FileMode;
using openswmm::TableType;
using openswmm::gpkg::open_database;
using openswmm::gpkg::create_schema;
using openswmm::gpkg::exec;
using openswmm::gpkg::prepare;
using openswmm::gpkg::bind_text;
using openswmm::gpkg::write_external_content;
using openswmm::gpkg::DbPtr;

namespace {

fs::path testDir() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path()) here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/io7_writer";
    fs::create_directories(dir);
    return dir;
}

DbPtr freshDb(const std::string& stem) {
    auto path = testDir() / (stem + ".gpkg");
    std::error_code ec;
    fs::remove(path, ec);
    auto db = open_database(path.string());
    create_schema(db.get());
    return db;
}

// Plant a single-row model providing FK targets for content rows.
void plantMinimalModel(sqlite3* db, const std::string& sim) {
    exec(db, "INSERT INTO simulations (simulation_id, name, created_at, engine_version) "
              "VALUES ('" + sim + "', 't', '2026-01-01T00:00:00Z', '6.0.0')");
    exec(db, "INSERT INTO nodes (simulation_id, node_id, node_type) "
              "VALUES ('" + sim + "', 'J1', 'JUNCTION')");
    exec(db, "INSERT INTO nodes (simulation_id, node_id, node_type) "
              "VALUES ('" + sim + "', 'O1', 'OUTFALL')");
    exec(db, "INSERT INTO links (simulation_id, link_id, link_type, from_node, to_node) "
              "VALUES ('" + sim + "', 'L1', 'CONDUIT', 'J1', 'O1')");
    exec(db, "INSERT INTO subcatchments (simulation_id, subcatch_id) "
              "VALUES ('" + sim + "', 'S1')");
    exec(db, "INSERT INTO rain_gages (simulation_id, gage_id) "
              "VALUES ('" + sim + "', 'G1')");
    exec(db, "INSERT INTO pollutants (simulation_id, pollutant_id, units) "
              "VALUES ('" + sim + "', 'TSS', 'MG/L')");
}

int countWhere(sqlite3* db, const std::string& sql) {
    auto stmt = prepare(db, sql);
    sqlite3_step(stmt.get());
    return sqlite3_column_int(stmt.get(), 0);
}

} // namespace

// ---------------------------------------------------------------------------
// Timeseries FILE → input_timeseries with provenance columns
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, TimeseriesFilePopulatesProvenance) {
    const std::string sim = "sim1";
    const fs::path src = testDir() / "ts_input.dat";
    {
        std::ofstream o(src);
        o << "01/01/2026 00:00 0.10\n";
        o << "01/01/2026 01:00 0.05\n";
    }

    auto db = freshDb("timeseries");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    int t = ctx.tables.add("RAIN_X", TableType::TIMESERIES);
    ctx.tables[t].file_path = src.string();   // pre-resolved (.original) only

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM input_timeseries "
        "WHERE simulation_id='sim1' AND series_id='RAIN_X'"), 2);

    auto stmt = prepare(db.get(),
        "SELECT source, source_filename, source_column "
        "FROM input_timeseries WHERE series_id='RAIN_X' LIMIT 1");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)),
                 "imported_from_file");
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 1)),
                 "ts_input.dat");
    EXPECT_EQ(sqlite3_column_type(stmt.get(), 2), SQLITE_NULL);
}

TEST(GpkgExternalContentWriter, TimeseriesWithColumnSuffixCapturesColumn) {
    const std::string sim = "sim1";
    const fs::path src = testDir() / "ts_input_col.dat";
    {
        std::ofstream o(src);
        o << "01/01/2026 00:00 0.10\n";
    }
    auto db = freshDb("timeseries_col");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    int t = ctx.tables.add("RAIN_X", TableType::TIMESERIES);
    ctx.tables[t].file_path = src.string() + ":East_Gage";

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    auto stmt = prepare(db.get(),
        "SELECT source_column FROM input_timeseries "
        "WHERE series_id='RAIN_X' LIMIT 1");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_STREQ(reinterpret_cast<const char*>(sqlite3_column_text(stmt.get(), 0)),
                 "East_Gage");
}

// ---------------------------------------------------------------------------
// Climate FILE → climate_data
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, ClimateFilePopulatesRows) {
    const std::string sim = "sim1";
    const fs::path src = testDir() / "climate_input.csv";
    {
        std::ofstream o(src);
        o << "date,tmin,tmax,evap,wind,sky,humidity\n";
        o << "2026-01-01,32.0,52.0,0.10,5.0,0.5,55.0\n";
        o << "2026-01-02,30.0,48.0,0.08,4.0,0.4,60.0\n";
    }
    auto db = freshDb("climate");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    ctx.options.temp_file = src.string();

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM climate_data WHERE simulation_id='sim1'"), 2);
}

// ---------------------------------------------------------------------------
// Raingage FILE → raingage_data
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, RaingageFilePopulatesRows) {
    const std::string sim = "sim1";
    const fs::path src = testDir() / "raingage_input.dat";
    {
        std::ofstream o(src);
        o << "STA01 2026 01 01 06 00  0.10\n";
        o << "STA01 2026 01 01 07 00  0.05\n";
    }
    auto db = freshDb("raingage");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    ctx.gage_names.add("G1");
    ctx.gages.file_path.emplace_back(src.string());

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM raingage_data "
        "WHERE simulation_id='sim1' AND gage_id='G1'"), 2);
}

// ---------------------------------------------------------------------------
// [FILES] USE INFLOWS → routing_interface_node + _node_pollutants
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, RoutingInflowsFilePopulatesNodeAndPollutantRows) {
    const std::string sim = "sim1";
    const fs::path src = testDir() / "inflows_input.txt";
    {
        std::ofstream o(src);
        o << "SWMM5 Interface File\n";
        o << "test\n";
        o << "60   - reporting time step in sec\n";
        o << "2    - number of constituents as listed below:\n";
        o << "FLOW CFS\n";
        o << "TSS MG/L\n";
        o << "1    - number of nodes as listed below:\n";
        o << "J1\n";
        o << "Node             Year Mon Day Hr  Min Sec FLOW       TSS\n";
        o << "J1               2026 01  01  00  00  00  1.25       12.5\n";
        o << "J1               2026 01  01  00  01  00  0.95       10.0\n";
    }
    auto db = freshDb("inflows");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    ctx.files.inflows_path = src.string();    // legacy: USE only

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM routing_interface_node "
        "WHERE simulation_id='sim1' AND role='INFLOWS' AND direction='USE'"), 2);
    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM routing_interface_node_pollutants "
        "WHERE simulation_id='sim1' AND role='INFLOWS' "
        " AND pollutant_id='TSS'"), 2);
}

// ---------------------------------------------------------------------------
// Hot-start USE → populated slot + state rows
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, HotstartUsePopulatesSlotAndState) {
    const std::string sim = "sim1";
    const fs::path src = testDir() / "hotstart_input.hsf";
    // Write a v4 HSF with one node + one link + one pollutant placeholder.
    {
        std::ofstream o(src, std::ios::binary);
        const char stamp[] = "SWMM5-HOTSTART4";
        o.write(stamp, 15);
        int32_t v;
        v = 0;  o.write(reinterpret_cast<const char*>(&v), 4); // nSubcatch
        v = 0;  o.write(reinterpret_cast<const char*>(&v), 4); // nLanduses
        v = 2;  o.write(reinterpret_cast<const char*>(&v), 4); // nNodes
        v = 1;  o.write(reinterpret_cast<const char*>(&v), 4); // nLinks
        v = 1;  o.write(reinterpret_cast<const char*>(&v), 4); // nPollut
        v = 0;  o.write(reinterpret_cast<const char*>(&v), 4); // flowUnits = CFS
        float f;
        // Node J1: depth, lateral, pollut
        f = 0.5f;   o.write(reinterpret_cast<const char*>(&f), 4);
        f = 0.10f;  o.write(reinterpret_cast<const char*>(&f), 4);
        f = 12.5f;  o.write(reinterpret_cast<const char*>(&f), 4);
        // Node O1
        f = 0.0f;   o.write(reinterpret_cast<const char*>(&f), 4);
        f = 0.0f;   o.write(reinterpret_cast<const char*>(&f), 4);
        f = 0.0f;   o.write(reinterpret_cast<const char*>(&f), 4);
        // Link L1: flow, depth, setting, pollut
        f = 1.75f;  o.write(reinterpret_cast<const char*>(&f), 4);
        f = 0.40f;  o.write(reinterpret_cast<const char*>(&f), 4);
        f = 1.0f;   o.write(reinterpret_cast<const char*>(&f), 4);
        f = 5.0f;   o.write(reinterpret_cast<const char*>(&f), 4);
    }
    auto db = freshDb("hotstart_use");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    ctx.node_names.add("J1");
    ctx.node_names.add("O1");
    ctx.link_names.add("L1");
    ctx.pollutant_names.add("TSS");
    ctx.files.hotstart_use_path = src.string();

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM hotstart_slots "
        "WHERE simulation_id='sim1' AND slot_name='use' "
        " AND direction='USE' AND status='populated'"), 1);
    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM hotstart_node_state "
        "WHERE simulation_id='sim1' AND slot_name='use'"), 2);
    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM hotstart_link_state "
        "WHERE simulation_id='sim1' AND slot_name='use' "
        " AND link_id='L1'"), 1);
    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM hotstart_node_pollutant_state "
        "WHERE simulation_id='sim1' AND slot_name='use' "
        " AND node_id='J1' AND pollutant_id='TSS'"), 1);

    // Spot-check the actual value.
    auto stmt = prepare(db.get(),
        "SELECT depth FROM hotstart_node_state "
        "WHERE node_id='J1' AND slot_name='use'");
    ASSERT_EQ(sqlite3_step(stmt.get()), SQLITE_ROW);
    EXPECT_NEAR(sqlite3_column_double(stmt.get(), 0), 0.5, 1e-5);
}

// ---------------------------------------------------------------------------
// SAVE hotstart slot with missing file → status='pending', no state rows
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, HotstartSavePendingWhenFileMissing) {
    const std::string sim = "sim1";
    auto db = freshDb("hotstart_pending");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    openswmm::HotstartSaveEntry e;
    e.path = (testDir() / "does_not_exist.hsf").string();
    e.datetime = 46036.5;
    ctx.files.hotstart_saves.push_back(e);

    ASSERT_NO_THROW(write_external_content(db.get(), ctx, sim));

    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM hotstart_slots "
        "WHERE simulation_id='sim1' AND slot_name='save_0' "
        " AND direction='SAVE' AND status='pending'"), 1);
    EXPECT_EQ(countWhere(db.get(),
        "SELECT COUNT(*) FROM hotstart_node_state "
        "WHERE simulation_id='sim1' AND slot_name='save_0'"), 0);
}

// ---------------------------------------------------------------------------
// USE direction with missing file → GpkgError
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentWriter, UseDirectionMissingFileThrows) {
    const std::string sim = "sim1";
    auto db = freshDb("use_missing");
    plantMinimalModel(db.get(), sim);

    SimulationContext ctx;
    ctx.options.temp_file = (testDir() / "missing_climate.csv").string();
    EXPECT_THROW(write_external_content(db.get(), ctx, sim),
                 openswmm::gpkg::GpkgError);
}
