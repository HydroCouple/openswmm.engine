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
 * @file      test_geopackage_external_content_reader.cpp
 * @brief     Slice IO-8 — round-trip: write Part D content via IO-7, re-open
 *        via the new reader hydration pass, confirm scratch files land
 *        and `SimulationContext` slots are bound to them.
 *
 * @details Test layout:
 *
 *            tests/unit/engine/data/io8_reader/&lt;case&gt;.gpkg
 *            tests/unit/engine/data/io8_reader/&lt;case&gt;_input.&lt;ext&gt;  (raw input)
 *            tests/unit/engine/data/io8_reader/&lt;case&gt;.scratch/...  (materialised)
 *
 *          All paths reviewable per CLAUDE.md §4.1. The scratch directory
 *          is created by the reader; we don't clean it up here so a
 *          reviewer can inspect.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>

#include "core/HotStartManager.hpp"
#include "core/SimulationContext.hpp"
#include "input/geopackage/ExternalContentReader.hpp"
#include "input/geopackage/ExternalContentWriter.hpp"
#include "input/geopackage/GeoPackageSchema.hpp"
#include "input/geopackage/GpkgUtils.hpp"

namespace fs = std::filesystem;
using openswmm::HotStartManager;
using openswmm::NodeType;
using openswmm::SimulationContext;
using openswmm::gpkg::GpkgError;
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

/**
 * @brief Locate/create the retained structured external-content reader artifacts.
 *
 * Searches at most eight parents from cwd for the test-data tree, then creates the target
 * regardless of search success. Filesystem errors propagate; existing artifacts remain and cwd is
 * unchanged.
 * @return Path ending tests/unit/engine/data/io8_reader beneath the located or final searched
 *     ancestor.
 */
fs::path testDir() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path())
            here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/io8_reader";
    fs::create_directories(dir);
    return dir;
}

/**
 * @brief Reset a controlled database/scratch artifact and create the maintained schema.
 *
 * Ignores remove/remove_all error codes, so failed deletion can leave old contents.
 * Open/schema/filesystem exceptions propagate; no transaction or rollback covers earlier persistent
 * work. Caller keeps stem scoped to expendable fixture artifacts.
 *
 * @param[in] stem Safe basename beneath testDir; both stem.gpkg and stem.scratch tree may be
 *     removed.
 * @return Owning SQLite connection after database/schema initialization.
 */
DbPtr freshDb(const std::string& stem) {
    auto path = testDir() / (stem + ".gpkg");
    std::error_code ec;
    fs::remove(path, ec);
    fs::remove_all(testDir() / (stem + ".scratch"), ec);
    auto db = open_database(path.string());
    create_schema(db.get());
    return db;
}

/**
 * @brief Form the reader-fixture GeoPackage path, creating its artifact directory.
 *
 * Directory creation/search follows testDir and can throw; no path sanitation occurs.
 *
 * @param[in] stem Safe database basename without extension.
 * @return Owned testDir()/stem.gpkg native string; database itself is not opened/created here.
 */
std::string dbPathFor(const std::string& stem) {
    return (testDir() / (stem + ".gpkg")).string();
}

/**
 * @brief Insert the simulation and object rows required by external-content foreign keys.
 *
 * Inserts simulation, J1/O1, L1, S1, G1 and TSS rows. exec exceptions can leave earlier inserts
 * committed; no transaction, escaping or independent ID validation is added here.
 *
 * @param[in,out] db Borrowed writable maintained-schema connection with no conflicting fixture
 *     rows.
 * @param[in] sim Trusted simulation id interpolated into SQL; must contain no unescaped single
 *     quotes.
 */
void plantMinimalModel(sqlite3* db, const std::string& sim) {
    exec(db, "INSERT INTO simulations (simulation_id, name, created_at, engine_version) "
             "VALUES ('" +
                 sim + "', 't', '2026-01-01T00:00:00Z', '6.0.0')");
    exec(db, "INSERT INTO nodes (simulation_id, node_id, node_type) "
             "VALUES ('" +
                 sim + "', 'J1', 'JUNCTION')");
    exec(db, "INSERT INTO nodes (simulation_id, node_id, node_type) "
             "VALUES ('" +
                 sim + "', 'O1', 'OUTFALL')");
    exec(db, "INSERT INTO links (simulation_id, link_id, link_type, from_node, to_node) "
             "VALUES ('" +
                 sim + "', 'L1', 'CONDUIT', 'J1', 'O1')");
    exec(db, "INSERT INTO subcatchments (simulation_id, subcatch_id) "
             "VALUES ('" +
                 sim + "', 'S1')");
    exec(db, "INSERT INTO rain_gages (simulation_id, gage_id) "
             "VALUES ('" +
                 sim + "', 'G1')");
    exec(db, "INSERT INTO pollutants (simulation_id, pollutant_id, units) "
             "VALUES ('" +
                 sim + "', 'TSS', 'MG/L')");
}

} // namespace

// ---------------------------------------------------------------------------
// scratchDirFor: pure string transform
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, ScratchDirSiblingOfGpkg) {
    EXPECT_EQ(scratchDirFor("/tmp/foo.gpkg"), "/tmp/foo.scratch");
    EXPECT_EQ(scratchDirFor("/a/b/proj.gpkg"), "/a/b/proj.scratch");
    EXPECT_EQ(scratchDirFor("proj.gpkg"), "proj.scratch");
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
    EXPECT_NE(slot.original.find("<gpkg:input_timeseries:RAIN_X>"), std::string::npos);
    EXPECT_TRUE(fs::exists(slot.absolute)) << "scratch file should exist: " << slot.absolute;
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
    EXPECT_NE(rctx.options.temp_file.original.find("<gpkg:climate_data>"), std::string::npos);
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
    rctx.gage_names.add("G1"); // simulate Part A having already read the model
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));

    ASSERT_FALSE(rctx.gages.file_path.empty());
    EXPECT_FALSE(rctx.gages.file_path[0].absolute.empty());
    EXPECT_NE(rctx.gages.file_path[0].original.find("<gpkg:raingage_data:G1>"), std::string::npos);
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
    EXPECT_NE(rctx.files.inflows_path.original.find("<gpkg:routing_interface_node:INFLOWS:USE>"),
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
        v = 0;
        o.write(reinterpret_cast<const char*>(&v), 4); // nSubcatch
        v = 0;
        o.write(reinterpret_cast<const char*>(&v), 4); // nLanduses
        v = 2;
        o.write(reinterpret_cast<const char*>(&v), 4); // nNodes
        v = 1;
        o.write(reinterpret_cast<const char*>(&v), 4); // nLinks
        v = 1;
        o.write(reinterpret_cast<const char*>(&v), 4); // nPollut
        v = 0;
        o.write(reinterpret_cast<const char*>(&v), 4); // flowUnits
        float f;
        f = 0.5f;
        o.write(reinterpret_cast<const char*>(&f), 4); // J1 depth
        f = 0.1f;
        o.write(reinterpret_cast<const char*>(&f), 4); // J1 lat
        f = 12.5f;
        o.write(reinterpret_cast<const char*>(&f), 4); // J1 TSS
        f = 0.0f;
        o.write(reinterpret_cast<const char*>(&f), 4); // O1 depth
        f = 0.0f;
        o.write(reinterpret_cast<const char*>(&f), 4); // O1 lat
        f = 0.0f;
        o.write(reinterpret_cast<const char*>(&f), 4); // O1 TSS
        f = 1.75f;
        o.write(reinterpret_cast<const char*>(&f), 4); // L1 flow
        f = 0.4f;
        o.write(reinterpret_cast<const char*>(&f), 4); // L1 depth
        f = 1.0f;
        o.write(reinterpret_cast<const char*>(&f), 4); // L1 setting
        f = 5.0f;
        o.write(reinterpret_cast<const char*>(&f), 4); // L1 TSS
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
    EXPECT_NE(rctx.files.hotstart_use_path.original.find("<gpkg:hotstart_slots:use>"),
              std::string::npos);
    EXPECT_TRUE(fs::exists(rctx.files.hotstart_use_path.absolute));
    // The scratch HSF should have non-zero size since the slot was populated.
    EXPECT_GT(fs::file_size(rctx.files.hotstart_use_path.absolute), 30u);
}

// ---------------------------------------------------------------------------
// A legacy hot start of a model with subcatchments and a storage node, with a
// trailer after the links, comes back from the .gpkg byte for byte. Before,
// the writer refused the subcatchment section, misread every node after a
// storage node (its residence-time float) and dropped the trailer.
// ---------------------------------------------------------------------------

namespace {
/**
 * @brief Construct model shape for structured legacy-hotstart reader checks.
 *
 * Populates registries/type shape only; does not initialize a complete hydraulic/quality state,
 * connect the link or execute a run. Allocations may throw.
 *
 * @param[in] n_subcatch Nonnegative intended count of S1..Sn subcatchments supported by object
 *     allocation.
 * @return Owned allocated context with one land use, J1 storage/O1 outfall, one link and TSS.
 */
SimulationContext hotstartModel(int n_subcatch) {
    SimulationContext ctx;
    for (int i = 0; i < n_subcatch; ++i)
        ctx.subcatch_names.add("S" + std::to_string(i + 1));
    ctx.landuse_names.add("LU");
    ctx.node_names.add("J1");
    ctx.node_names.add("O1");
    ctx.link_names.add("L1");
    ctx.pollutant_names.add("TSS");
    ctx.allocate_objects();
    ctx.nodes.type[0] = NodeType::STORAGE;
    ctx.nodes.type[1] = NodeType::OUTFALL;
    return ctx;
}
} // namespace

TEST(GpkgExternalContentReader, HotstartWithSubcatchmentsStorageAndTrailerRoundTrips) {
    const std::string sim = "sim1";
    const std::string stem = "hotstart_runoff_section";
    const fs::path src = testDir() / "hotstart_runoff_section.hsf";
    const SimulationContext model = hotstartModel(1);
    // 1 subcatchment, no groundwater or snowpack, 1 pollutant, 1 land use:
    // 4 + 6 + 2*1 + 1*(1+1) doubles.
    ASSERT_EQ(HotStartManager::legacy_runoff_size(model, 1), 14u);
    {
        std::ofstream o(src, std::ios::binary);
        o.write("SWMM5-HOTSTART4", 15);
        for (int32_t v : {1, 1, 2, 1, 1, 0}) // nSub nLand nNodes nLinks nPollut flowUnits
            o.write(reinterpret_cast<const char*>(&v), 4);
        for (int k = 0; k < 14; ++k) {
            const double d = 0.5 + k;
            o.write(reinterpret_cast<const char*>(&d), 8);
        }
        // J1 (storage): depth, lateral inflow, residence time, TSS;
        // O1: depth, lateral inflow, TSS; L1: flow, depth, setting, TSS.
        for (float f : {0.5f, 0.1f, 3600.0f, 12.5f, 0.25f, 0.0f, 7.0f, 1.75f, 0.4f, 1.0f, 5.0f})
            o.write(reinterpret_cast<const char*>(&f), 4);
        o.write("OPENSWMM-HSMSX1\x02\x00\x00\x00trailer", 26);
    }
    std::string original;
    {
        std::ifstream f(src, std::ios::binary);
        original.assign(std::istreambuf_iterator<char>(f), {});
    }

    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);
    {
        SimulationContext other = hotstartModel(2); // the file does not fit it
        other.files.hotstart_use_path = src.string();
        EXPECT_THROW(write_external_content(db.get(), other, sim), GpkgError);
    }
    {
        SimulationContext wctx = hotstartModel(1);
        wctx.files.hotstart_use_path = src.string();
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }

    SimulationContext rctx = hotstartModel(1);
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));
    std::string rebuilt;
    {
        std::ifstream f(rctx.files.hotstart_use_path.absolute, std::ios::binary);
        rebuilt.assign(std::istreambuf_iterator<char>(f), {});
    }
    EXPECT_EQ(rebuilt.size(), original.size());
    EXPECT_TRUE(rebuilt == original) << "the rebuilt hot start differs from the embedded one";
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

// ---------------------------------------------------------------------------
// SAVE HOTSTART format keyword survives write → read; NULL reads as AUTO.
// ---------------------------------------------------------------------------

TEST(GpkgExternalContentReader, HotstartSaveFormatRoundTrip) {
    const std::string sim = "sim1";
    const std::string stem = "hotstart_save_format";
    auto db = freshDb(stem);
    plantMinimalModel(db.get(), sim);
    {
        SimulationContext wctx;
        openswmm::HotstartSaveEntry a, n, l;
        a.path = (testDir() / "rt_auto.hsf").string();
        n.path = (testDir() / "rt_native.hsf").string();
        n.format = openswmm::HotstartFormat::NATIVE;
        l.path = (testDir() / "rt_legacy.hsf").string();
        l.format = openswmm::HotstartFormat::LEGACY;
        l.datetime = 46036.5;
        for (const auto* e : {&a, &n, &l}) {
            std::error_code ec;
            fs::remove(e->path.str(), ec); // pending slots: no file on disk
        }
        wctx.files.hotstart_saves = {a, n, l};
        ASSERT_NO_THROW(write_external_content(db.get(), wctx, sim));
    }
    SimulationContext rctx;
    const std::string scratch = scratchDirFor(dbPathFor(stem));
    ASSERT_NO_THROW(read_external_content(db.get(), rctx, sim, scratch));
    ASSERT_EQ(rctx.files.hotstart_saves.size(), 3u);
    EXPECT_EQ(rctx.files.hotstart_saves[0].format, openswmm::HotstartFormat::AUTO);
    EXPECT_EQ(rctx.files.hotstart_saves[1].format, openswmm::HotstartFormat::NATIVE);
    EXPECT_EQ(rctx.files.hotstart_saves[2].format, openswmm::HotstartFormat::LEGACY);
    EXPECT_DOUBLE_EQ(rctx.files.hotstart_saves[2].datetime, 46036.5);
}
