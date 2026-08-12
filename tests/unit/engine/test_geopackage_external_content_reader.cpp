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
 * @file test_geopackage_external_content_reader.cpp
 * @brief Slice IO-8 — round-trip: write Part D content via IO-7, re-open
 *        via the new reader hydration pass, confirm scratch files land
 *        and `SimulationContext` slots are bound to them.
 *
 * @details Test layout:
 *
 *            tests/unit/engine/data/io8_reader/<case>.gpkg
 *            tests/unit/engine/data/io8_reader/<case>_input.<ext>  (raw input)
 *            tests/unit/engine/data/io8_reader/<case>.scratch/...  (materialised)
 *
 *          All paths reviewable per CLAUDE.md §4.1. The scratch directory
 *          is created by the reader; we don't clean it up here so a
 *          reviewer can inspect.
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

#include "core/SimulationContext.hpp"
#include "input/geopackage/ExternalContentReader.hpp"
#include "input/geopackage/ExternalContentWriter.hpp"
#include "input/geopackage/GeoPackageSchema.hpp"
#include "input/geopackage/GpkgUtils.hpp"

namespace fs = std::filesystem;
using openswmm::SimulationContext;
using openswmm::TableType;
using openswmm::gpkg::open_database;
using openswmm::gpkg::create_schema;
using openswmm::gpkg::exec;
using openswmm::gpkg::prepare;
using openswmm::gpkg::bind_text;
using openswmm::gpkg::DbPtr;
using openswmm::gpkg::read_external_content;
using openswmm::gpkg::write_external_content;
using openswmm::gpkg::scratchDirFor;

namespace {

fs::path testDir() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path()) here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/io8_reader";
    fs::create_directories(dir);
    return dir;
}

DbPtr freshDb(const std::string& stem) {
    auto path = testDir() / (stem + ".gpkg");
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove_all(testDir() / (stem + ".scratch"), ec);
    auto db = open_database(path.string());
    create_schema(db.get());
    return db;
}

std::string dbPathFor(const std::string& stem) {
    return (testDir() / (stem + ".gpkg")).string();
}

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

} // namespace

// ---------------------------------------------------------------------------
// scratchDirFor: pure string transform
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, ScratchDirSiblingOfGpkg) {
    EXPECT_EQ(scratchDirFor("/tmp/foo.gpkg"),       "/tmp/foo.scratch");
    EXPECT_EQ(scratchDirFor("/a/b/proj.gpkg"),      "/a/b/proj.scratch");
    EXPECT_EQ(scratchDirFor("proj.gpkg"),           "proj.scratch");
}

// ---------------------------------------------------------------------------
// Round-trip writer → reader: timeseries
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, TimeseriesRoundTripMaterialisesScratchFile) {
    const std::string sim = "sim1";
    const std::string stem = "timeseries";
    const fs::path src = testDir() / "timeseries_input.dat";
    {
        std::ofstream o(src);
        o << "01/01/2026 00:00 0.10\n";
        o << "01/01/2026 01:00 0.05\n";
    }
    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);

    // Write phase.
    {
        SimulationContext wctx;
        int t = wctx.tables.add("RAIN_X", TableType::TIMESERIES);
        wctx.tables[t].file_path = src.string();
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }

    // Read phase — empty ctx with a matching name registry so the
    // reader knows which Table index to bind.
    SimulationContext rctx;
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));

    int t = rctx.find_timeseries("RAIN_X");
    ASSERT_GE(t, 0) << "reader did not register RAIN_X as a timeseries";
    const auto& slot = rctx.tables[t].file_path;
    EXPECT_FALSE(slot.absolute.empty());
    EXPECT_NE(slot.original.find("<gpkg:input_timeseries:RAIN_X>"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(slot.absolute))
        << "scratch file should exist: " << slot.absolute;
}

// ---------------------------------------------------------------------------
// Climate
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, ClimateRoundTripMaterialisesScratch) {
    const std::string sim = "sim1";
    const std::string stem = "climate";
    const fs::path src = testDir() / "climate_input.csv";
    {
        std::ofstream o(src);
        o << "date,tmin,tmax,evap,wind,sky,humidity\n";
        o << "2026-01-01,32.0,52.0,0.10,5.0,0.5,55.0\n";
    }
    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);
    {
        SimulationContext wctx;
        wctx.options.temp_file = src.string();
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }

    SimulationContext rctx;
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));

    EXPECT_FALSE(rctx.options.temp_file.absolute.empty());
    EXPECT_NE(rctx.options.temp_file.original.find("<gpkg:climate_data>"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(rctx.options.temp_file.absolute));
}

// ---------------------------------------------------------------------------
// Raingage
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, RaingageRoundTripMaterialisesScratch) {
    const std::string sim = "sim1";
    const std::string stem = "raingage";
    const fs::path src = testDir() / "raingage_input.dat";
    {
        std::ofstream o(src);
        o << "STA01 2026 01 01 06 00  0.10\n";
        o << "STA01 2026 01 01 07 00  0.05\n";
    }
    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);
    {
        SimulationContext wctx;
        wctx.gage_names.add("G1");
        wctx.gages.file_path.emplace_back(src.string());
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }

    SimulationContext rctx;
    rctx.gage_names.add("G1");   // simulate Part A having already read the model
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));

    ASSERT_FALSE(rctx.gages.file_path.empty());
    EXPECT_FALSE(rctx.gages.file_path[0].absolute.empty());
    EXPECT_NE(rctx.gages.file_path[0].original.find("<gpkg:raingage_data:G1>"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(rctx.gages.file_path[0].absolute));
}

// ---------------------------------------------------------------------------
// Routing INFLOWS
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, RoutingInflowsRoundTrip) {
    const std::string sim = "sim1";
    const std::string stem = "inflows";
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
    }
    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);
    {
        SimulationContext wctx;
        wctx.files.inflows_path = src.string();
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }

    SimulationContext rctx;
    rctx.node_names.add("J1");
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));

    EXPECT_FALSE(rctx.files.inflows_path.absolute.empty());
    EXPECT_NE(rctx.files.inflows_path.original.find(
                  "<gpkg:routing_interface_node:INFLOWS:USE>"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(rctx.files.inflows_path.absolute));
}

// ---------------------------------------------------------------------------
// Hot-start USE (populated) round-trip
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, HotstartUsePopulatedRoundTrip) {
    const std::string sim = "sim1";
    const std::string stem = "hotstart_use";
    const fs::path src = testDir() / "hotstart_input.hsf";
    // Minimal v4 HSF: 2 nodes, 1 link, 1 pollutant.
    {
        std::ofstream o(src, std::ios::binary);
        o.write("SWMM5-HOTSTART4", 15);
        int32_t v;
        v=0; o.write(reinterpret_cast<const char*>(&v), 4); // nSubcatch
        v=0; o.write(reinterpret_cast<const char*>(&v), 4); // nLanduses
        v=2; o.write(reinterpret_cast<const char*>(&v), 4); // nNodes
        v=1; o.write(reinterpret_cast<const char*>(&v), 4); // nLinks
        v=1; o.write(reinterpret_cast<const char*>(&v), 4); // nPollut
        v=0; o.write(reinterpret_cast<const char*>(&v), 4); // flowUnits
        float f;
        f=0.5f;  o.write(reinterpret_cast<const char*>(&f), 4); // J1 depth
        f=0.1f;  o.write(reinterpret_cast<const char*>(&f), 4); // J1 lat
        f=12.5f; o.write(reinterpret_cast<const char*>(&f), 4); // J1 TSS
        f=0.0f;  o.write(reinterpret_cast<const char*>(&f), 4); // O1 depth
        f=0.0f;  o.write(reinterpret_cast<const char*>(&f), 4); // O1 lat
        f=0.0f;  o.write(reinterpret_cast<const char*>(&f), 4); // O1 TSS
        f=1.75f; o.write(reinterpret_cast<const char*>(&f), 4); // L1 flow
        f=0.4f;  o.write(reinterpret_cast<const char*>(&f), 4); // L1 depth
        f=1.0f;  o.write(reinterpret_cast<const char*>(&f), 4); // L1 setting
        f=5.0f;  o.write(reinterpret_cast<const char*>(&f), 4); // L1 TSS
    }
    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);
    {
        SimulationContext wctx;
        wctx.node_names.add("J1");
        wctx.node_names.add("O1");
        wctx.link_names.add("L1");
        wctx.pollutant_names.add("TSS");
        wctx.files.hotstart_use_path = src.string();
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }

    // Re-read.
    SimulationContext rctx;
    rctx.node_names.add("J1");
    rctx.node_names.add("O1");
    rctx.link_names.add("L1");
    rctx.pollutant_names.add("TSS");
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));

    EXPECT_FALSE(rctx.files.hotstart_use_path.absolute.empty());
    EXPECT_NE(rctx.files.hotstart_use_path.original.find(
                  "<gpkg:hotstart_slots:use>"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(rctx.files.hotstart_use_path.absolute));
    // The scratch HSF should have non-zero size since the slot was populated.
    EXPECT_GT(fs::file_size(rctx.files.hotstart_use_path.absolute), 30u);
}

// ---------------------------------------------------------------------------
// Empty scratch_dir → no-op
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, EmptyScratchDirSkipsHydration) {
    const std::string sim = "sim1";
    auto db = freshDb("noop");
    plantMinimalModel(db.get(), sim);

    SimulationContext rctx;
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, ""));
    EXPECT_TRUE(rctx.options.temp_file.empty());
    EXPECT_TRUE(rctx.files.inflows_path.empty());
}
