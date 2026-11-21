// SPDX-License-Identifier: Apache-2.0
// Phase36: exact C-API row identity and scale of large groundwater assignments.

/**
 * @file      test_gw_authoring_index.cpp
 * @brief     Tests for gw authoring index.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include <openswmm/engine/openswmm_model.h>
#include "2d/gw/GwTransportData.hpp"
#include <chrono>
#include <iostream>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
namespace {
namespace fs = std::filesystem;
/**
 * @brief Select the retained large-groundwater-authoring artifact directory.
 *
 * Only returns a path; does not create it, canonicalize it or cache process environment. Caller
 * supplies directory creation and exclusive environment access.
 * @return OPENSWMM_GW_AUTHORING_OUTPUT when present, otherwise
 *     ../../output/phase36_engine_authoring relative to this source file.
 */
fs::path output() {
    if (const char* p = std::getenv("OPENSWMM_GW_AUTHORING_OUTPUT"))
        return p;
    return fs::path(__FILE__).parent_path() / "../../output/phase36_engine_authoring";
}
struct Engine {
    SWMM_Engine e = swmm_engine_new();
    /**
     * @brief Close/destroy the editable native engine owned by the authoring fixture.
     *
     * Null e is a no-op; ignores close status and consumes a successful handle. Do not copy the wrapper
     * into a second owner or use borrowed context storage after destruction.
     */
    ~Engine() {
        if (e) {
            swmm_engine_close(e);
            swmm_engine_destroy(e);
        }
    }
};
struct Source {
    std::string name, tag, series;
    int scope = -1, cell = -1;
    double flow = 0, scale = 0;
};
/**
 * @brief Copy one authored groundwater source row and its scale through the C API.
 *
 * Uses256-byte zero-initialized text buffers and nonfatal API success expectations; long
 * values/errors can yield incomplete/default fields, with no separate validity flag. Does not
 * mutate row order or prescription values.
 *
 * @param[in] e Live engine with source authoring rows.
 * @param[in] i Valid zero-based source-row position.
 * @return Owned source name/tag/series, scope/cell/flow and scale values, retaining API authored
 *     units.
 */
Source source(SWMM_Engine e, int i) {
    Source r;
    char name[256]{}, tag[256]{}, series[256]{};
    EXPECT_EQ(swmm_gw_source_get(e, i, name, sizeof name, &r.scope, tag, sizeof tag, &r.cell,
                                 &r.flow, series, sizeof series),
              SWMM_OK);
    EXPECT_EQ(swmm_gw_source_scale_get(e, i, &r.scale), SWMM_OK);
    r.name = name;
    r.tag = tag;
    r.series = series;
    return r;
}
struct Quality {
    int scope = -1, cell = -1, zone = -1, layer = -1;
    std::string tag, species;
    double value = 0;
};
/**
 * @brief Copy one authored groundwater initial-quality row through the C API.
 *
 * Uses256-byte text buffers and a nonfatal success expectation. API failure/truncation can leave
 * default/partial fields; no separate status is returned and no row is mutated.
 *
 * @param[in] e Live engine with groundwater initial-quality authoring rows.
 * @param[in] i Valid zero-based initial-quality row position.
 * @return Owned scope/cell/zone/layer/tag/species/value snapshot in API-authored row units.
 */
Quality quality(SWMM_Engine e, int i) {
    Quality r;
    char tag[256]{}, species[256]{};
    EXPECT_EQ(swmm_gw_init_quality_get(e, i, &r.scope, tag, sizeof tag, &r.cell, &r.zone, &r.layer,
                                       species, sizeof species, &r.value),
              SWMM_OK);
    r.tag = tag;
    r.species = species;
    return r;
}
/**
 * @brief Construct the one-quad CMS groundwater authoring deck with supplied extra rows.
 *
 * Constructs text only; source selectors/series/quality overrides are not validated or executed.
 *
 * @param[in] rows Raw additional section/row text appended after TRANSPORT_POLLUTANTS YES; caller
 *     supplies headers and delimiters.
 * @return Owned five-minute deck with TSS, a4 m2 quad, closed-form aquifer and minimal J1/O1
 *     conduit network.
 */
std::string deck(const std::string& rows) {
    return R"INP([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2026
START_TIME 00:00:00
END_DATE 01/01/2026
END_TIME 00:05:00
REPORT_STEP 00:01:00
ROUTING_STEP 5
[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0
[JUNCTIONS]
J1 -2 4 0 0 0
[OUTFALLS]
O1 -2.5 FREE NO
[CONDUITS]
C1 J1 O1 30 0.013 0 0 0
[XSECTIONS]
C1 CIRCULAR 0.5 0 0 0 1
[2D_OPTIONS]
INTEGRATOR EXPLICIT
MAX_TIMESTEP 5
REPORT_2D NO
[2D_VERTICES]
0 0 0
2 0 0
2 2 0
0 2 0
[2D_QUADS]
0 1 2 3 0.03 0
[2D_AQUIFER_OPTIONS]
CLOSURE CLOSED_FORM
NODE_ENROLMENT ROWS
[2D_AQUIFER]
* 36 1 0.45 0.10 2 HG0 0.5
[GW_TRANSPORT_OPTIONS]
TRANSPORT_POLLUTANTS YES
)INP" + rows;
}
/**
 * @brief Create artifact parents and write a complete authoring-test deck.
 *
 * Throwing create_directories can propagate filesystem errors. Does not validate deck syntax, run a
 * model or retain input buffers.
 *
 * @param[in] p Destination path with a usable parent; existing file is truncated.
 * @param[in] text Complete text inserted verbatim.
 * @return Stream boolean state immediately after insertion; final close/flush failure is not
 *     included.
 */
bool write(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
    return bool(f);
}
/**
 * @brief Open an existing authoring deck with an appended report filename.
 *
 * Disables binary output by passing null, uses no progress callback and performs no
 * initialization/start/cleanup.
 *
 * @param[in,out] e Valid native engine in a lifecycle state accepting file open; caller retains
 *     ownership.
 * @param[in] p Readable input path; report destination is p.string()+.rpt.
 * @return Raw engine_open status; no success assertion is made.
 */
int open(SWMM_Engine e, const fs::path& p) {
    return swmm_engine_open(e, p.string().c_str(), (p.string() + ".rpt").c_str(), nullptr, nullptr);
}
}
TEST(GwAuthoringIndex, SourceUpsertPreservesPositionScaleAndTerms) {
    Engine e;
    ASSERT_TRUE(e.e);
    for (int i = 0; i < 32; ++i)
        ASSERT_EQ(
            swmm_gw_source_set(e.e, ("source_" + std::to_string(i)).c_str(), 2, "", i, i + .25, ""),
            SWMM_OK);
    ASSERT_EQ(swmm_gw_source_scale_set(e.e, 7, .125), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_species_set(e.e, 7, "TSS", "CONC", 23, ""), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_set(e.e, "source_7", 1, "corridor", -1, -2, "flow_series"), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_count(e.e), 32);
    const auto r = source(e.e, 7);
    EXPECT_EQ(r.name, "source_7");
    EXPECT_EQ(r.scope, 1);
    EXPECT_EQ(r.tag, "corridor");
    EXPECT_EQ(r.flow, -2);
    EXPECT_EQ(r.series, "flow_series");
    EXPECT_EQ(r.scale, .125);
    EXPECT_EQ(swmm_gw_source_species_count(e.e, 7), 1);
    ASSERT_EQ(swmm_gw_source_set(e.e, "SOURCE_7", 2, "", 1, 4, ""), SWMM_OK);
    EXPECT_EQ(swmm_gw_source_count(e.e), 33);
    EXPECT_EQ(source(e.e, 32).name, "SOURCE_7");
}
TEST(GwAuthoringIndex, SourceMiddleTailRemovalAndNameReuse) {
    Engine e;
    for (int i = 0; i < 6; ++i)
        ASSERT_EQ(swmm_gw_source_set(e.e, ("s" + std::to_string(i)).c_str(), 2, "", i, i, ""),
                  SWMM_OK);
    ASSERT_EQ(swmm_gw_source_remove(e.e, 2), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_set(e.e, "s4", 2, "", 44, 44, ""), SWMM_OK);
    EXPECT_EQ(source(e.e, 3).cell, 44);
    ASSERT_EQ(swmm_gw_source_remove(e.e, 4), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_set(e.e, "s5", 2, "", 55, 55, ""), SWMM_OK);
    EXPECT_EQ(source(e.e, 4).cell, 55);
    ASSERT_EQ(swmm_gw_source_set(e.e, "s2", 2, "", 22, 22, ""), SWMM_OK);
    EXPECT_EQ(source(e.e, 5).cell, 22);
    for (int i = 5; i >= 0; --i)
        ASSERT_EQ(swmm_gw_source_remove(e.e, i), SWMM_OK);
    EXPECT_EQ(swmm_gw_source_count(e.e), 0);
    ASSERT_EQ(swmm_gw_source_set(e.e, "s4", 2, "", 0, 9, ""), SWMM_OK);
    EXPECT_EQ(source(e.e, 0).flow, 9);
}
TEST(GwAuthoringIndex, QualityCompositeIdentityAndRemoval) {
    Engine e;
    for (int cell = 0; cell < 4; ++cell)
        for (int zone = 0; zone < 2; ++zone)
            ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", cell, zone, -1, "TSS", 10 * cell + zone),
                      SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 1, "corridor", -1, 0, -1, "TSS", 88), SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 0, "", -1, 0, -1, "TSS", 99), SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", 2, 1, -1, "TSS", 123), SWMM_OK);
    EXPECT_EQ(swmm_gw_init_quality_count(e.e), 10);
    EXPECT_EQ(quality(e.e, 5).value, 123);
    EXPECT_EQ(quality(e.e, 4).value, 20);
    ASSERT_EQ(swmm_gw_init_quality_remove(e.e, 1), SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", 2, 1, -1, "TSS", 321), SWMM_OK);
    EXPECT_EQ(quality(e.e, 4).value, 321);
    ASSERT_EQ(swmm_gw_init_quality_remove(e.e, 8), SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 0, "", -1, 0, -1, "TSS", 77), SWMM_OK);
    EXPECT_EQ(quality(e.e, 8).value, 77);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", 0, 1, -1, "TSS", 42), SWMM_OK);
    EXPECT_EQ(quality(e.e, 9).value, 42);
}
TEST(GwAuthoringIndex, LoadSaveCloseAndReopenRebuildIdentity) {
    const auto dir = output() / "reload";
    const auto first = dir / "first.inp", second = dir / "second.inp", saved = dir / "saved.inp";
    ASSERT_TRUE(write(
        first,
        deck(
            "[GW_INITIAL_QUALITY]\nCELL 1 SAT TSS 1\n[GW_SOURCES]\nWell XY 1 1 FLOW .001 SCALE .25 TSS CONC 10\n")));
    ASSERT_TRUE(write(
        second,
        deck("[GW_INITIAL_QUALITY]\nCELL 1 UNSAT TSS 2\n[GW_SOURCES]\nOther CELL 1 FLOW .002\n")));
    Engine e;
    ASSERT_EQ(swmm_engine_close(e.e), SWMM_OK);
    ASSERT_EQ(open(e.e, first), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_set(e.e, "Well", 2, "", 0, .003, ""), SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", 0, 0, -1, "TSS", 3), SWMM_OK);
    EXPECT_EQ(swmm_gw_source_count(e.e), 1);
    EXPECT_EQ(swmm_gw_init_quality_count(e.e), 1);
    EXPECT_EQ(source(e.e, 0).scale, .25);
    EXPECT_EQ(swmm_gw_source_species_count(e.e, 0), 1);
    ASSERT_EQ(swmm_model_write(e.e, saved.string().c_str()), SWMM_OK);
    ASSERT_EQ(swmm_engine_close(e.e), SWMM_OK);
    ASSERT_EQ(open(e.e, saved), SWMM_OK);
    EXPECT_EQ(source(e.e, 0).flow, .003);
    EXPECT_EQ(quality(e.e, 0).value, 3);
    ASSERT_EQ(swmm_gw_source_set(e.e, "Well", 2, "", 0, .004, ""), SWMM_OK);
    ASSERT_EQ(swmm_engine_close(e.e), SWMM_OK);
    ASSERT_EQ(open(e.e, second), SWMM_OK);
    ASSERT_EQ(swmm_gw_source_set(e.e, "Well", 2, "", 0, .005, ""), SWMM_OK);
    ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", 0, 0, -1, "TSS", 5), SWMM_OK);
    EXPECT_EQ(swmm_gw_source_count(e.e), 2);
    EXPECT_EQ(source(e.e, 0).name, "Other");
    EXPECT_EQ(source(e.e, 1).name, "Well");
    EXPECT_EQ(swmm_gw_init_quality_count(e.e), 2);
    EXPECT_EQ(quality(e.e, 0).zone, 1);
    EXPECT_EQ(quality(e.e, 1).zone, 0);
    const auto empty = dir / "no_gw_rows.inp";
    ASSERT_TRUE(write(empty, deck("")));
    ASSERT_EQ(swmm_engine_close(e.e), SWMM_OK);
    ASSERT_EQ(open(e.e, empty), SWMM_OK);
    EXPECT_EQ(swmm_gw_source_count(e.e), 0);
    EXPECT_EQ(swmm_gw_init_quality_count(e.e), 0);
    ASSERT_EQ(swmm_gw_source_set(e.e, "Well", 2, "", 0, .006, ""), SWMM_OK);
    EXPECT_EQ(source(e.e, 0).flow, .006);
}
TEST(GwAuthoringIndex, DerivedIndexesPreserveFirstMatchAndBulkReplacement) {
    using namespace openswmm::twoD;
    GwTransportData gw;
    GwSourceRow src;
    src.name = "well";
    gw.sources = {src, src};
    GwInitialQualityRow q;
    q.scope = GwScope::CELL;
    q.cell = 4;
    q.species = "TSS";
    gw.initial_quality = {q, q};
    EXPECT_EQ(gw.findSource("well"), 0u);
    EXPECT_EQ(gw.findInitialQuality(q), 0u);
    gw.removeSource(1);
    gw.removeInitialQuality(1); // deleting later duplicate must retain first
    EXPECT_EQ(gw.findSource("well"), 0u);
    EXPECT_EQ(gw.findInitialQuality(q), 0u);
    auto copy = gw;
    gw.clear();
    EXPECT_EQ(gw.findSource("well"), GwTransportData::noRow);
    EXPECT_EQ(gw.findInitialQuality(q), GwTransportData::noRow);
    EXPECT_EQ(copy.findSource("well"), 0u);
    EXPECT_EQ(copy.findInitialQuality(q), 0u);
    copy.sources[0].name = "replacement";
    copy.initial_quality[0].cell = 8;
    copy.invalidateAuthoringIndexes(); // bulk load may replace with the same row count
    EXPECT_EQ(copy.findSource("well"), GwTransportData::noRow);
    EXPECT_EQ(copy.findSource("replacement"), 0u);
    EXPECT_EQ(copy.findInitialQuality(q), GwTransportData::noRow);
    q.cell = 8;
    EXPECT_EQ(copy.findInitialQuality(q), 0u);
    auto moved = std::move(copy);
    EXPECT_EQ(moved.findSource("replacement"), 0u);
    EXPECT_EQ(moved.findInitialQuality(q), 0u);
}
TEST(GwAuthoringIndex, ScalingBenchmark) {
    const char* requested = std::getenv("OPENSWMM_GW_AUTHORING_BENCHMARK_SIZES");
    if (!requested || !*requested)
        GTEST_SKIP() << "Set explicit benchmark sizes to retain timing evidence.";
    fs::create_directories(output());
    std::ofstream csv(output() / "scaling.csv");
    ASSERT_TRUE(csv);
    csv << "rows,source_append_ms,source_update_ms,source_tail_remove_ms,quality_append_ms,quality_update_ms,quality_tail_remove_ms\n";
    std::istringstream list(requested);
    std::string token;
    while (std::getline(list, token, ',')) {
        int n = std::stoi(token);
        ASSERT_GT(n, 0);
        ASSERT_LE(n, 1000000);
        Engine e;
        std::vector<std::string> names;
        names.reserve(n);
        for (int i = 0; i < n; ++i)
            names.push_back("bench_" + std::to_string(i));
        using Clock = std::chrono::steady_clock;
        const auto timed = [&](auto fn) {
            auto start = Clock::now();
            fn();
            return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        };
        const auto sa = timed([&] {
            for (int i = 0; i < n; ++i)
                ASSERT_EQ(swmm_gw_source_set(e.e, names[i].c_str(), 2, "", i, .001, ""), SWMM_OK);
        });
        const auto su = timed([&] {
            for (int i = 0; i < n; ++i)
                ASSERT_EQ(swmm_gw_source_set(e.e, names[i].c_str(), 2, "", i, .002, ""), SWMM_OK);
        });
        EXPECT_EQ(swmm_gw_source_count(e.e), n);
        EXPECT_EQ(source(e.e, n - 1).flow, .002);
        const auto sr = timed([&] {
            for (int i = n - 1; i >= 0; --i)
                ASSERT_EQ(swmm_gw_source_remove(e.e, i), SWMM_OK);
        });
        EXPECT_EQ(swmm_gw_source_count(e.e), 0);
        const auto qa = timed([&] {
            for (int i = 0; i < n; ++i)
                ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", i, 0, -1, "TSS", 1), SWMM_OK);
        });
        const auto qu = timed([&] {
            for (int i = 0; i < n; ++i)
                ASSERT_EQ(swmm_gw_init_quality_set(e.e, 2, "", i, 0, -1, "TSS", 2), SWMM_OK);
        });
        EXPECT_EQ(swmm_gw_init_quality_count(e.e), n);
        EXPECT_EQ(quality(e.e, n - 1).value, 2);
        const auto qr = timed([&] {
            for (int i = n - 1; i >= 0; --i)
                ASSERT_EQ(swmm_gw_init_quality_remove(e.e, i), SWMM_OK);
        });
        EXPECT_EQ(swmm_gw_init_quality_count(e.e), 0);
        csv << n << ',' << sa << ',' << su << ',' << sr << ',' << qa << ',' << qu << ',' << qr
            << '\n';
        csv.flush();
        std::cout << "Groundwater authoring rows=" << n << " source append/update=" << sa << '/'
                  << su << "ms quality append/update=" << qa << '/' << qu << "ms\n";
    }
}
