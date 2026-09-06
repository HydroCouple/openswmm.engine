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
 * @file test_geopackage_formats.cpp
 * @brief Slice IO-6 — round-trip parse/materialise for every external-file
 *        format the GeoPackage plugin consumes/emits.
 *
 * @details Each test writes a known-input file under
 *          tests/unit/engine/data/io6_formats/ (CLAUDE.md §4.1 reviewable
 *          IO), parses it, materialises the parsed rows back to a
 *          sibling file, and re-parses the sibling. The round-trip is
 *          required to preserve the row shape for every format.
 *
 *          Five formats are covered: timeseries SWMM-native text,
 *          climate user CSV, raingage SWMM "Standard" columnar text,
 *          routing-interface SWMM5 text, and hot-start HSF v4 binary
 *          (routing portion only — see HotstartFormat.hpp).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/DateTime.hpp"
#include "input/geopackage/formats/FormatTypes.hpp"
#include "input/geopackage/formats/TimeseriesFormat.hpp"
#include "input/geopackage/formats/ClimateFormat.hpp"
#include "input/geopackage/formats/RaingageFormat.hpp"
#include "input/geopackage/formats/RoutingInterfaceFormat.hpp"
#include "input/geopackage/formats/HotstartFormat.hpp"

namespace fs = std::filesystem;
using namespace openswmm::gpkg::formats;
namespace dt = openswmm::datetime;

namespace {

fs::path testDataDir() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path()) here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/io6_formats";
    fs::create_directories(dir);
    return dir;
}

bool nearly_equal(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps * (std::fabs(a) + std::fabs(b) + 1.0);
}

} // namespace

// ---------------------------------------------------------------------------
// Timeseries (SWMM-native text)
// ---------------------------------------------------------------------------

TEST(GpkgFormats_Timeseries, RoundTripSimple) {
    const fs::path src = testDataDir() / "timeseries_in.dat";
    const fs::path dst = testDataDir() / "timeseries_out.dat";
    {
        std::ofstream out(src);
        out << ";; sample SWMM-native timeseries\n";
        out << "01/01/2026 00:00 0.10\n";
        out << "01/01/2026 01:00 0.05\n";
        out << "01/01/2026 02:00 0.00\n";
    }
    std::vector<TimeseriesRow> rows;
    ASSERT_TRUE(parseTimeseriesText(src.string(), rows).ok);
    ASSERT_EQ(rows.size(), 3u);

    EXPECT_NEAR(rows[0].value, 0.10, 1e-9);
    EXPECT_NEAR(rows[1].value, 0.05, 1e-9);
    EXPECT_NEAR(rows[2].value, 0.00, 1e-9);
    int y, mo, d, h, mi, s;
    dt::decodeDate(rows[0].timestamp_oa, y, mo, d);
    dt::decodeTime(rows[0].timestamp_oa, h, mi, s);
    EXPECT_EQ(y, 2026); EXPECT_EQ(mo, 1); EXPECT_EQ(d, 1);
    EXPECT_EQ(h, 0);    EXPECT_EQ(mi, 0); EXPECT_EQ(s, 0);

    ASSERT_TRUE(writeTimeseriesText(dst.string(), rows).ok);

    std::vector<TimeseriesRow> rows2;
    ASSERT_TRUE(parseTimeseriesText(dst.string(), rows2).ok);
    ASSERT_EQ(rows.size(), rows2.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_NEAR(rows[i].timestamp_oa, rows2[i].timestamp_oa, 1e-9);
        EXPECT_NEAR(rows[i].value,        rows2[i].value,        1e-9);
    }
}

// ---------------------------------------------------------------------------
// Climate (user CSV)
// ---------------------------------------------------------------------------

TEST(GpkgFormats_Climate, RoundTripCanonicalHeader) {
    const fs::path src = testDataDir() / "climate_in.csv";
    const fs::path dst = testDataDir() / "climate_out.csv";
    {
        std::ofstream out(src);
        out << "date,tmin,tmax,evap,wind,sky,humidity\n";
        out << "2026-01-01,32.0,52.0,0.10,5.0,0.5,55.0\n";
        out << "2026-01-02,30.0,48.0,0.08,4.0,0.4,60.0\n";
    }
    std::vector<ClimateRow> rows;
    ASSERT_TRUE(parseClimateCsv(src.string(), rows).ok);
    ASSERT_EQ(rows.size(), 2u);
    EXPECT_EQ(rows[0].record_date, "2026-01-01");
    EXPECT_NEAR(rows[0].tmin, 32.0, 1e-9);
    EXPECT_NEAR(rows[1].evaporation, 0.08, 1e-9);

    ASSERT_TRUE(writeClimateCsv(dst.string(), rows).ok);
    std::vector<ClimateRow> rows2;
    ASSERT_TRUE(parseClimateCsv(dst.string(), rows2).ok);
    ASSERT_EQ(rows.size(), rows2.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].record_date, rows2[i].record_date);
        EXPECT_NEAR(rows[i].tmin, rows2[i].tmin, 1e-4);
        EXPECT_NEAR(rows[i].tmax, rows2[i].tmax, 1e-4);
    }
}

TEST(GpkgFormats_Climate, ParserHandlesReorderedHeader) {
    const fs::path src = testDataDir() / "climate_reordered.csv";
    {
        std::ofstream out(src);
        // Reordered columns; parser must use header position.
        out << "humidity,sky,wind,date,tmin,tmax,evap\n";
        out << "55.0,0.5,5.0,2026-01-01,32.0,52.0,0.10\n";
    }
    std::vector<ClimateRow> rows;
    ASSERT_TRUE(parseClimateCsv(src.string(), rows).ok);
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_EQ(rows[0].record_date, "2026-01-01");
    EXPECT_NEAR(rows[0].humidity, 55.0, 1e-9);
    EXPECT_NEAR(rows[0].sky_cover, 0.5, 1e-9);
}

// ---------------------------------------------------------------------------
// Raingage (SWMM Standard)
// ---------------------------------------------------------------------------

TEST(GpkgFormats_Raingage, RoundTripStd) {
    const fs::path src = testDataDir() / "raingage_in.dat";
    const fs::path dst = testDataDir() / "raingage_out.dat";
    {
        std::ofstream out(src);
        out << "; SWMM Standard format\n";
        out << "STA01 2026 01 01 06 00  0.10\n";
        out << "STA01 2026 01 01 07 00  0.05\n";
        out << "STA01 2026 01 01 08 00  0.00\n";
    }
    std::vector<RaingageRow> rows;
    ASSERT_TRUE(parseRaingageStd(src.string(), rows).ok);
    ASSERT_EQ(rows.size(), 3u);
    EXPECT_EQ(rows[0].station_id, "STA01");
    EXPECT_NEAR(rows[0].rainfall, 0.10, 1e-9);

    ASSERT_TRUE(writeRaingageStd(dst.string(), rows).ok);
    std::vector<RaingageRow> rows2;
    ASSERT_TRUE(parseRaingageStd(dst.string(), rows2).ok);
    ASSERT_EQ(rows.size(), rows2.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows[i].station_id, rows2[i].station_id);
        EXPECT_NEAR(rows[i].timestamp_oa, rows2[i].timestamp_oa, 1e-6);
        EXPECT_NEAR(rows[i].rainfall, rows2[i].rainfall, 1e-4);
    }
}

// ---------------------------------------------------------------------------
// Routing interface (SWMM5 text)
// ---------------------------------------------------------------------------

TEST(GpkgFormats_RoutingInterface, RoundTripWithPollutants) {
    const fs::path dst = testDataDir() / "routing_iface.txt";

    RoutingInterfaceMetadata meta;
    meta.title           = "IO-6b test interface";
    meta.report_step_sec = 60;
    meta.flow_units      = "CFS";
    meta.pollutant_ids   = {"TSS", "BOD"};
    meta.pollutant_units = {"MG/L", "MG/L"};
    meta.object_ids      = {"J1", "J2"};

    std::vector<RoutingInterfaceRow> rows;
    {
        RoutingInterfaceRow r;
        r.object_id = "J1";
        r.timestamp_oa = dt::encodeDate(2026, 1, 1) + dt::encodeTime(0, 0, 0);
        r.flow_value = 1.25;
        r.pollutant_values = {10.0, 5.0};
        rows.push_back(r);
        r.object_id = "J2";
        r.timestamp_oa = dt::encodeDate(2026, 1, 1) + dt::encodeTime(0, 1, 0);
        r.flow_value = 0.95;
        r.pollutant_values = {12.0, 6.5};
        rows.push_back(r);
    }

    ASSERT_TRUE(writeRoutingInterfaceText(dst.string(), meta, rows).ok);

    RoutingInterfaceMetadata meta2;
    std::vector<RoutingInterfaceRow> rows2;
    ASSERT_TRUE(parseRoutingInterfaceText(dst.string(), meta2, rows2).ok);

    EXPECT_EQ(meta2.title,           meta.title);
    EXPECT_EQ(meta2.report_step_sec, meta.report_step_sec);
    EXPECT_EQ(meta2.flow_units,      meta.flow_units);
    ASSERT_EQ(meta2.pollutant_ids,   meta.pollutant_ids);
    EXPECT_EQ(meta2.pollutant_units, meta.pollutant_units);
    ASSERT_EQ(meta2.object_ids,      meta.object_ids);

    ASSERT_EQ(rows2.size(), rows.size());
    for (std::size_t i = 0; i < rows.size(); ++i) {
        EXPECT_EQ(rows2[i].object_id, rows[i].object_id);
        EXPECT_NEAR(rows2[i].timestamp_oa, rows[i].timestamp_oa, 1e-6);
        EXPECT_NEAR(rows2[i].flow_value, rows[i].flow_value, 1e-4);
        ASSERT_EQ(rows2[i].pollutant_values.size(),
                  rows[i].pollutant_values.size());
        for (std::size_t j = 0; j < rows[i].pollutant_values.size(); ++j)
            EXPECT_NEAR(rows2[i].pollutant_values[j],
                        rows[i].pollutant_values[j], 1e-4);
    }
}

// ---------------------------------------------------------------------------
// Hot-start (HSF v4 binary, routing portion only)
// ---------------------------------------------------------------------------

TEST(GpkgFormats_Hotstart, RoutingPortionRoundTrip) {
    const fs::path dst = testDataDir() / "hotstart.hsf";

    HotstartSnapshot snap;
    snap.header.file_stamp    = "SWMM5-HOTSTART4";
    snap.header.file_version  = 4;
    snap.header.num_subcatch  = 0;        // runoff state out of scope (Slice IO-6e)
    snap.header.num_landuses  = 0;
    snap.header.num_nodes     = 2;
    snap.header.num_links     = 1;
    snap.header.num_pollut    = 2;
    snap.header.flow_units    = 0;        // CFS

    {
        HotstartNodeState n;
        n.depth = 0.50; n.lateral_inflow = 0.10;
        n.concentration = {12.5, 3.2};
        snap.node_state.push_back(n);
        n.depth = 0.30; n.lateral_inflow = 0.05;
        n.concentration = {0.0,  4.1};
        snap.node_state.push_back(n);
    }
    {
        HotstartLinkState l;
        l.flow = 1.75; l.depth = 0.40; l.setting = 1.0;
        l.concentration = {6.6, 1.8};
        snap.link_state.push_back(l);
    }

    ASSERT_TRUE(writeHotstartHsf(dst.string(), snap).ok);

    HotstartSnapshot snap2;
    ASSERT_TRUE(parseHotstartHsf(dst.string(), snap2).ok);

    EXPECT_EQ(snap2.header.file_version,  4);
    EXPECT_EQ(snap2.header.num_nodes,     snap.header.num_nodes);
    EXPECT_EQ(snap2.header.num_links,     snap.header.num_links);
    EXPECT_EQ(snap2.header.num_pollut,    snap.header.num_pollut);
    EXPECT_EQ(snap2.header.flow_units,    snap.header.flow_units);

    ASSERT_EQ(snap2.node_state.size(), 2u);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_NEAR(snap2.node_state[i].depth,
                    snap.node_state[i].depth, 1e-5);
        EXPECT_NEAR(snap2.node_state[i].lateral_inflow,
                    snap.node_state[i].lateral_inflow, 1e-5);
        for (std::size_t j = 0; j < 2; ++j)
            EXPECT_NEAR(snap2.node_state[i].concentration[j],
                        snap.node_state[i].concentration[j], 1e-3);
    }
    ASSERT_EQ(snap2.link_state.size(), 1u);
    EXPECT_NEAR(snap2.link_state[0].flow,    snap.link_state[0].flow,    1e-5);
    EXPECT_NEAR(snap2.link_state[0].depth,   snap.link_state[0].depth,   1e-5);
    EXPECT_NEAR(snap2.link_state[0].setting, snap.link_state[0].setting, 1e-5);
    EXPECT_NEAR(snap2.link_state[0].target_setting,
                snap.link_state[0].setting, 1e-5);
}

TEST(GpkgFormats_Hotstart, RejectsTruncatedFile) {
    const fs::path dst = testDataDir() / "hotstart_trunc.hsf";
    {
        std::ofstream out(dst, std::ios::binary);
        out << "SWMM5-HOTSTART4";  // stamp only — no counts
    }
    HotstartSnapshot snap;
    auto r = parseHotstartHsf(dst.string(), snap);
    EXPECT_FALSE(r.ok);
    EXPECT_FALSE(r.error.empty());
}

TEST(GpkgFormats_Hotstart, RejectsHeaderMismatch) {
    // num_nodes=3 in header but only 2 entries in vector → write must fail.
    HotstartSnapshot snap;
    snap.header.num_nodes = 3;
    snap.header.num_links = 0;
    snap.header.num_pollut = 0;
    snap.node_state.resize(2);
    for (auto& n : snap.node_state) n.concentration.resize(0);
    auto r = writeHotstartHsf((testDataDir() / "mismatch.hsf").string(), snap);
    EXPECT_FALSE(r.ok);
}
