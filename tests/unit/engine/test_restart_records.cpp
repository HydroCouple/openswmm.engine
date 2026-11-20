// SPDX-License-Identifier: Apache-2.0
/**
 * @file      test_restart_records.cpp
 * @brief     [RESTART]: recording restart states into GeoPackage and HDF5 files and starting a
 *            run from one, through the public C API.
 *
 * Each restore runs in a fresh engine that never saw the recording run. Comparisons use double
 * state read through the API, never the float32 binary output. Outputs are written under
 * tests/output/restart_records in the source tree.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_checkpoint.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_links.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_restart.h>

namespace fs = std::filesystem;

namespace {

fs::path caseDir(const std::string& name) {
    const fs::path dir = fs::path(__FILE__).parent_path().parent_path().parent_path() / "output" /
                         "restart_records" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/** Three-node dynamic-wave network with a time-varying inflow, so state changes over time. */
fs::path writeModel(const fs::path& dir, const std::string& name, const std::string& start,
                    const std::string& files, const std::string& restart,
                    const std::string& end = "01:00:00") {
    const fs::path inp = dir / (name + ".inp");
    std::ofstream o(inp);
    o << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2020\n"
         "START_TIME "
      << start
      << "\nREPORT_START_DATE 01/01/2020\nREPORT_START_TIME 00:00:00\n"
         "END_DATE 01/01/2020\nEND_TIME "
      << end
      << "\nREPORT_STEP 00:01:00\nROUTING_STEP 15\n\n"
         "[FILES]\n"
      << files << "\n\n[RESTART]\n"
      << restart
      << "\n\n[JUNCTIONS]\nJ1 10 6 0 0 0\nJ2 8 6 0 0 0\n\n[OUTFALLS]\nO1 5 FREE\n\n"
         "[CONDUITS]\nC1 J1 J2 400 0.013 0 0 0 0\nC2 J2 O1 400 0.013 0 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0 1\nC2 CIRCULAR 1.5 0 0 0 1\n\n"
         "[TIMESERIES]\nTS1 0:00 0\nTS1 0:20 3\nTS1 0:40 1\nTS1 1:00 0\n\n"
         "[INFLOWS]\nJ1 FLOW TS1\n";
    return inp;
}

struct Run {
    int rc = SWMM_OK;
    std::string message;
    std::vector<double> state; ///< node depths, then link flows, after the last step
};

/** Run to the end. Without @p results the run writes no binary output, as the CLI without one. */
Run run(const fs::path& inp, bool results = true) {
    Run r;
    SWMM_Engine e = swmm_engine_create();
    const fs::path stem = inp.parent_path() / inp.stem();
    const std::string out = stem.string() + ".out";
    r.rc = swmm_engine_open(e, inp.string().c_str(), (stem.string() + ".rpt").c_str(),
                            results ? out.c_str() : nullptr, nullptr);
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_initialize(e);
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_start(e, results ? 1 : 0);
    double elapsed = 0.0;
    while (r.rc == SWMM_OK) {
        r.rc = swmm_engine_step(e, &elapsed);
        if (r.rc != SWMM_OK || elapsed <= 0.0)
            break;
    }
    if (r.rc == SWMM_OK) {
        for (int i = 0; i < swmm_node_count(e); ++i) {
            double v = 0.0;
            swmm_node_get_depth(e, i, &v);
            r.state.push_back(v);
        }
        for (int j = 0; j < swmm_link_count(e); ++j) {
            double v = 0.0;
            swmm_link_get_flow(e, j, &v);
            r.state.push_back(v);
        }
        r.rc = swmm_engine_end(e);
    }
    if (const char* m = swmm_get_last_error_msg(e))
        r.message = m;
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return r;
}

std::string inspect(const fs::path& p) {
    size_t need = 0;
    if (swmm_restart_inspect_json(p.string().c_str(), nullptr, 0, &need) != SWMM_OK)
        return std::string("ERROR: ") + swmm_restart_last_error();
    std::string json(need, '\0');
    if (swmm_restart_inspect_json(p.string().c_str(), json.data(), need, &need) != SWMM_OK)
        return std::string("ERROR: ") + swmm_restart_last_error();
    json.resize(need - 1);
    return json;
}

/** Value of the first `"key":"..."` after @p from in @p json. */
std::string field(const std::string& json, const std::string& key, std::size_t from = 0) {
    const std::string tag = "\"" + key + "\":\"";
    const auto at = json.find(tag, from);
    if (at == std::string::npos)
        return {};
    const auto start = at + tag.size();
    return json.substr(start, json.find('"', start) - start);
}

int count(const std::string& json, const std::string& text) {
    int n = 0;
    for (auto at = json.find(text); at != std::string::npos; at = json.find(text, at + 1))
        ++n;
    return n;
}

std::string bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

std::string recordRows(const fs::path& file, const char* format, bool continuation) {
    return "RECORD_FILE \"" + file.string() + "\"\nRECORD_FORMAT " + format +
           "\nRECORD_AT 01/01/2020 00:30:00\nRECORD_AT END\n" +
           (continuation ? "RECORD_CONTINUATION YES\n" : "");
}

std::string sourceRows(const fs::path& file, const std::string& run, const char* record,
                       const char* mode) {
    return "SOURCE_FILE \"" + file.string() + "\"\nSOURCE_RUN " + run + "\nSOURCE_RECORD " +
           record + "\nMODE " + mode + "\n";
}

} // namespace

// The section survives the input file, a GeoPackage project and an HDF5 model, and its
// paths are rebased when the model is written to another folder.
TEST(RestartRecords, SectionRoundTripsThroughInputGeoPackageAndHdf5) {
    const fs::path dir = caseDir("section_round_trip");
    const fs::path inp =
        writeModel(dir, "model", "00:00:00", "",
                   sourceRows(dir / "in.gpkg", "run_a", "rec_000002", "CONTINUE") +
                       recordRows(dir / "states.h5", "HDF5", true) + "RECORD_EVERY 00:10:00\n");
    fs::create_directories(dir / "copy");
    const std::vector<std::pair<fs::path, const char*>> targets = {
        {dir / "copy" / "model.inp", nullptr},
        {dir / "copy" / "model.gpkg", "org.hydrocouple.openswmm.plugins.geopackage"},
        {dir / "copy" / "model.h5", "org.hydrocouple.openswmm.plugins.hdf5"}};
    {
        SWMM_Engine e = swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), nullptr, nullptr, nullptr), SWMM_OK);
        for (const auto& [path, plugin] : targets)
            ASSERT_EQ(swmm_model_write_with_plugin(e, path.string().c_str(), plugin), SWMM_OK)
                << path << ": " << swmm_get_last_error_msg(e);
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }
    for (const auto& [path, plugin] : targets) {
        SWMM_Engine e = swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(e, path.string().c_str(), nullptr, nullptr, plugin), SWMM_OK)
            << path << ": " << swmm_get_last_error_msg(e);
        char buf[1024];
        auto get = [&](const char* key) {
            buf[0] = '\0';
            swmm_restart_get(e, key, buf, sizeof buf);
            return std::string(buf);
        };
        EXPECT_EQ(fs::path(get("SOURCE_FILE")).filename(), "in.gpkg") << path;
        EXPECT_EQ(get("SOURCE_RUN"), "run_a") << path;
        EXPECT_EQ(get("SOURCE_RECORD"), "rec_000002") << path;
        EXPECT_EQ(get("MODE"), "CONTINUE") << path;
        EXPECT_EQ(fs::path(get("RECORD_FILE")).filename(), "states.h5") << path;
        EXPECT_EQ(get("RECORD_FORMAT"), "HDF5") << path;
        EXPECT_EQ(get("RECORD_EVERY"), "600") << path;
        EXPECT_EQ(get("RECORD_CONTINUATION"), "YES") << path;
        int n = 0;
        ASSERT_EQ(swmm_restart_record_at_count(e, &n), SWMM_OK);
        ASSERT_EQ(n, 2) << path;
        double at = -1.0;
        swmm_restart_record_at_get(e, 1, &at);
        EXPECT_EQ(at, 0.0) << path; // END
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }
    // Written from the copy folder, the paths point back at the original folder.
    const std::string text = bytes(dir / "copy" / "model.inp");
    EXPECT_NE(text.find("../in.gpkg"), std::string::npos) << text;
    EXPECT_NE(text.find("../states.h5"), std::string::npos) << text;
}

TEST(RestartRecords, ConflictingSettingsAreRefusedBeforeTheRun) {
    const fs::path dir = caseDir("conflicts");
    const fs::path hsf = dir / "warm.hsf";
    std::ofstream(hsf, std::ios::binary).write("OPENSWMM_HS_V1\0", 16);
    struct Case {
        const char* name;
        std::string files, restart, expected;
    };
    const std::vector<Case> cases = {
        {"both_sources", "USE HOTSTART \"" + hsf.string() + "\"",
         sourceRows(dir / "in.gpkg", "r", "LATEST", "INITIAL_CONDITIONS"), "USE HOTSTART"},
        {"no_record_id", "", "SOURCE_FILE \"" + (dir / "in.gpkg").string() + "\"", "SOURCE_RECORD"},
        {"no_format", "", "RECORD_FILE \"" + (dir / "s.gpkg").string() + "\"", "RECORD_FORMAT"},
        {"no_record_file", "", "RECORD_AT END", "RECORD_FILE"},
        {"same_file", "",
         sourceRows(dir / "x.gpkg", "r", "LATEST", "INITIAL_CONDITIONS") + "RECORD_FILE \"" +
             (dir / "x.gpkg").string() + "\"\nRECORD_FORMAT GEOPACKAGE",
         "different files"},
        {"missing_source", "",
         sourceRows(dir / "nowhere.gpkg", "r", "LATEST", "INITIAL_CONDITIONS"), "does not exist"},
        {"hotstart_as_source", "", sourceRows(hsf, "r", "LATEST", "INITIAL_CONDITIONS"),
         "[FILES] USE HOTSTART"}};
    for (const auto& c : cases) {
        const auto r = run(writeModel(dir, c.name, "00:00:00", c.files, c.restart));
        EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << c.name << ": " << r.message;
        EXPECT_NE(r.message.find(c.expected), std::string::npos) << c.name << ": " << r.message;
    }
}

// The record format is chosen, never inferred, and an existing file of another kind is left alone.
TEST(RestartRecords, RecordingLeavesAFileOfAnotherKindUnchanged) {
    const fs::path dir = caseDir("foreign_destination");
    const fs::path h5 = dir / "results.h5";
    { // an HDF5 file with other content
        const fs::path model = writeModel(dir, "seed", "00:00:00", "", "");
        SWMM_Engine e = swmm_engine_create();
        ASSERT_EQ(swmm_engine_open(e, model.string().c_str(), nullptr, nullptr, nullptr), SWMM_OK);
        ASSERT_EQ(swmm_model_write_with_plugin(e, h5.string().c_str(),
                                               "org.hydrocouple.openswmm.plugins.hdf5"),
                  SWMM_OK);
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }
    const fs::path text = dir / "notes.gpkg";
    std::ofstream(text) << "not a database";
    for (const auto& [target, format] :
         {std::pair<fs::path, const char*>{h5, "HDF5"}, {h5, "GEOPACKAGE"}, {text, "GEOPACKAGE"}}) {
        const std::string before = bytes(target);
        const auto r = run(writeModel(dir, "rec", "00:00:00", "",
                                      "RECORD_FILE \"" + target.string() + "\"\nRECORD_FORMAT " +
                                          format + "\nRECORD_AT END"));
        EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << target << " " << format << ": " << r.message;
        EXPECT_EQ(bytes(target), before) << target << " " << format;
    }
}

// A run records its states into either container with immutable IDs, and an inspection
// lists them without changing the file.
TEST(RestartRecords, RunRecordsIntoGeoPackageAndHdf5) {
    const fs::path dir = caseDir("record");
    for (const auto& [name, format] :
         {std::pair<const char*, const char*>{"states.gpkg", "GEOPACKAGE"},
          {"states.h5", "HDF5"}}) {
        const fs::path file = dir / name;
        const auto r =
            run(writeModel(dir, format, "00:00:00", "", recordRows(file, format, false)));
        ASSERT_EQ(r.rc, SWMM_OK) << format << ": " << r.message;
        const std::string before = bytes(file);
        const std::string json = inspect(file);
        EXPECT_EQ(bytes(file), before) << "inspection changed " << name;
        // Nor did it leave SQLite companion files beside a closed GeoPackage.
        EXPECT_FALSE(fs::exists(file.string() + "-wal")) << name;
        EXPECT_FALSE(fs::exists(file.string() + "-shm")) << name;
        EXPECT_EQ(field(json, "backend"), format) << json;
        EXPECT_EQ(count(json, "\"run_id\":\"run_"), 1) << json;
        EXPECT_EQ(count(json, "\"status\":\"complete\""), 2) << json;
        EXPECT_NE(json.find("\"record_id\":\"rec_000001\""), std::string::npos) << json;
        EXPECT_NE(json.find("\"record_id\":\"rec_000002\""), std::string::npos) << json;
        // The 00:30 record is the first accepted step at or after 00:30; END has no request.
        EXPECT_NE(json.find("\"requested_datetime\":43831.020833333336,"), std::string::npos)
            << json;
        EXPECT_NE(json.find("\"requested_datetime\":0,"), std::string::npos) << json;
        const auto second = json.find("\"record_id\":\"rec_000002\"");
        EXPECT_NE(json.find("\"elapsed_seconds\":3600,", second), std::string::npos) << json;
        EXPECT_NE(json.find("\"continuation\":false"), std::string::npos) << json;
    }
}

// Initial conditions from a record are exactly what a native hot start file saved at the
// same step gives, from either container, recording into either container.
TEST(RestartRecords, InitialConditionsMatchANativeHotStartFile) {
    const fs::path dir = caseDir("initial_conditions");
    const fs::path hsf = dir / "mid.hsf";
    const std::string save = "SAVE HOTSTART \"" + hsf.string() + "\" 01/01/2020 00:30:00 NATIVE";
    const fs::path gpkg = dir / "a.gpkg";
    const fs::path h5 = dir / "a.h5";
    ASSERT_EQ(
        run(writeModel(dir, "a_gpkg", "00:00:00", save, recordRows(gpkg, "GEOPACKAGE", false))).rc,
        SWMM_OK);
    ASSERT_EQ(run(writeModel(dir, "a_h5", "00:00:00", "", recordRows(h5, "HDF5", false))).rc,
              SWMM_OK);
    const std::string gpkg_run = field(inspect(gpkg), "run_id");
    const std::string h5_run = field(inspect(h5), "run_id");
    ASSERT_FALSE(gpkg_run.empty());
    ASSERT_FALSE(h5_run.empty());

    // The target starts at 00:30 from its own clock and runs two minutes, short enough that
    // its starting state still shows: a cold start must end elsewhere.
    const auto reference = run(writeModel(dir, "c_hsf", "00:30:00",
                                          "USE HOTSTART \"" + hsf.string() + "\"", "", "00:32:00"));
    ASSERT_EQ(reference.rc, SWMM_OK) << reference.message;
    const auto cold = run(writeModel(dir, "c_cold", "00:30:00", "", "", "00:32:00"));
    ASSERT_EQ(cold.rc, SWMM_OK) << cold.message;
    ASSERT_NE(reference.state, cold.state) << "the comparison cannot see the starting state";
    struct Case {
        const char* name;
        fs::path source;
        std::string run_id;
        fs::path destination;
        const char* format;
    };
    for (const auto& c : {Case{"g_to_g", gpkg, gpkg_run, dir / "b1.gpkg", "GEOPACKAGE"},
                          Case{"g_to_h", gpkg, gpkg_run, dir / "b2.h5", "HDF5"},
                          Case{"h_to_g", h5, h5_run, dir / "b3.gpkg", "GEOPACKAGE"},
                          Case{"h_to_h", h5, h5_run, dir / "b4.h5", "HDF5"}}) {
        const auto r = run(writeModel(
            dir, c.name, "00:30:00", "",
            sourceRows(c.source, c.run_id, "rec_000001", "INITIAL_CONDITIONS") + "RECORD_FILE \"" +
                c.destination.string() + "\"\nRECORD_FORMAT " + c.format + "\nRECORD_AT END\n",
            "00:32:00"));
        ASSERT_EQ(r.rc, SWMM_OK) << c.name << ": " << r.message;
        EXPECT_EQ(r.state, reference.state) << c.name;
        const std::string json = inspect(c.destination);
        EXPECT_EQ(field(json, "parent_run_id"), c.run_id) << json;
        EXPECT_EQ(field(json, "parent_record_id"), "rec_000001") << json;
        EXPECT_EQ(field(json, "restart_mode"), "INITIAL_CONDITIONS") << json;
        EXPECT_EQ(count(json, "\"status\":\"complete\""), 1) << json;
    }
    // The source files are never changed by a restore.
    EXPECT_EQ(field(inspect(gpkg), "run_id"), gpkg_run);
}

// LATEST resolves once, at start, to the run's last complete record.
TEST(RestartRecords, LatestResolvesToTheLastCompleteRecord) {
    const fs::path dir = caseDir("latest");
    const fs::path gpkg = dir / "a.gpkg";
    ASSERT_EQ(run(writeModel(dir, "a", "00:00:00", "", recordRows(gpkg, "GEOPACKAGE", false))).rc,
              SWMM_OK);
    const fs::path out = dir / "b.h5";
    const auto r = run(writeModel(dir, "b", "00:30:00", "",
                                  "SOURCE_FILE \"" + gpkg.string() +
                                      "\"\nSOURCE_RECORD LATEST\nRECORD_FILE \"" + out.string() +
                                      "\"\nRECORD_FORMAT HDF5\n"));
    ASSERT_EQ(r.rc, SWMM_OK) << r.message;
    EXPECT_EQ(field(inspect(out), "parent_record_id"), "rec_000002");
}

// Continuing from a record reproduces the uninterrupted run exactly, from either container.
TEST(RestartRecords, ContinueReproducesTheUninterruptedRun) {
    const fs::path dir = caseDir("continue");
    for (const auto& [name, format] :
         {std::pair<const char*, const char*>{"a.gpkg", "GEOPACKAGE"}, {"a.h5", "HDF5"}}) {
        const fs::path file = dir / name;
        const auto whole = run(writeModel(dir, std::string("a_") + format, "00:00:00", "",
                                          recordRows(file, format, true)),
                               false);
        ASSERT_EQ(whole.rc, SWMM_OK) << format << ": " << whole.message;
        const std::string json = inspect(file);
        EXPECT_EQ(count(json, "\"continuation\":true"), 2) << json;
        const auto resumed =
            run(writeModel(dir, std::string("b_") + format, "00:00:00", "",
                           sourceRows(file, field(json, "run_id"), "rec_000001", "CONTINUE")),
                false);
        ASSERT_EQ(resumed.rc, SWMM_OK) << format << ": " << resumed.message;
        EXPECT_EQ(resumed.state, whole.state) << format;
    }
}

// A record without an engine checkpoint cannot continue a run.
TEST(RestartRecords, ContinueNeedsAContinuationRecord) {
    const fs::path dir = caseDir("continue_refused");
    const fs::path gpkg = dir / "a.gpkg";
    ASSERT_EQ(run(writeModel(dir, "a", "00:00:00", "", recordRows(gpkg, "GEOPACKAGE", false))).rc,
              SWMM_OK);
    const auto r =
        run(writeModel(dir, "b", "00:00:00", "",
                       sourceRows(gpkg, field(inspect(gpkg), "run_id"), "rec_000001", "CONTINUE")));
    EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << r.message;
    EXPECT_NE(r.message.find("RECORD_CONTINUATION"), std::string::npos) << r.message;
}

// With binary results, continuation records carry the output written so far, so a continued
// run writes the same binary file as the uninterrupted run, byte for byte.
TEST(RestartRecords, ContinuationWithBinaryResultsReproducesTheOutputFile) {
    const fs::path dir = caseDir("continuation_with_results");
    for (const auto& [name, format] :
         {std::pair<const char*, const char*>{"a.gpkg", "GEOPACKAGE"}, {"a.h5", "HDF5"}}) {
        const fs::path file = dir / name;
        const std::string a = std::string("a_") + format, b = std::string("b_") + format;
        const auto whole = run(writeModel(dir, a, "00:00:00", "", recordRows(file, format, true)));
        ASSERT_EQ(whole.rc, SWMM_OK) << format << ": " << whole.message;
        const std::string json = inspect(file);
        EXPECT_EQ(count(json, "\"binary_output\":true"), 2) << json;
        const auto resumed =
            run(writeModel(dir, b, "00:00:00", "",
                           sourceRows(file, field(json, "run_id"), "rec_000001", "CONTINUE")));
        ASSERT_EQ(resumed.rc, SWMM_OK) << format << ": " << resumed.message;
        EXPECT_EQ(resumed.state, whole.state) << format;
        const std::string a_out = bytes(dir / (a + ".out")), b_out = bytes(dir / (b + ".out"));
        ASSERT_FALSE(a_out.empty());
        EXPECT_EQ(b_out, a_out) << format << ": the continued run's binary output differs";
    }
}

// The output written so far goes into the checkpoint only for a new output file, which is
// then created exclusively; an existing file is refused before the run, by name.
TEST(RestartRecords, ContinuationNeedsANewBinaryOutputFile) {
    const fs::path dir = caseDir("continuation_existing_output");
    const fs::path inp =
        writeModel(dir, "a", "00:00:00", "", recordRows(dir / "a.gpkg", "GEOPACKAGE", true));
    std::ofstream(dir / "a.out") << "earlier results";
    const auto r = run(inp);
    EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << r.message;
    EXPECT_NE(r.message.find("a.out"), std::string::npos) << r.message;
    EXPECT_EQ(bytes(dir / "a.out"), "earlier results");
}

// A continued run writes binary results exactly when its record carries them.
TEST(RestartRecords, ContinuedRunMatchesItsRecordsBinaryResults) {
    const fs::path dir = caseDir("continuation_output_mismatch");
    const fs::path with = dir / "with.gpkg", without = dir / "without.gpkg";
    ASSERT_EQ(run(writeModel(dir, "a1", "00:00:00", "", recordRows(with, "GEOPACKAGE", true))).rc,
              SWMM_OK);
    ASSERT_EQ(
        run(writeModel(dir, "a2", "00:00:00", "", recordRows(without, "GEOPACKAGE", true)), false)
            .rc,
        SWMM_OK);
    const auto r1 =
        run(writeModel(dir, "b1", "00:00:00", "",
                       sourceRows(with, field(inspect(with), "run_id"), "rec_000001", "CONTINUE")),
            false);
    EXPECT_EQ(r1.rc, SWMM_ERR_HOTSTART) << r1.message;
    EXPECT_NE(r1.message.find("saves binary results"), std::string::npos) << r1.message;
    const auto r2 = run(writeModel(
        dir, "b2", "00:00:00", "",
        sourceRows(without, field(inspect(without), "run_id"), "rec_000001", "CONTINUE")));
    EXPECT_EQ(r2.rc, SWMM_ERR_HOTSTART) << r2.message;
    EXPECT_NE(r2.message.find("saves none"), std::string::npos) << r2.message;
}

// The engine says when a run restores or records its checkpoint, so a caller can give it a
// new output file.
TEST(RestartRecords, EngineReportsWhenARunUsesTheCheckpoint) {
    const fs::path dir = caseDir("uses_checkpoint");
    for (const auto& [rows, expected] :
         {std::pair<std::string, int>{recordRows(dir / "c.gpkg", "GEOPACKAGE", true), 1},
          {recordRows(dir / "n.gpkg", "GEOPACKAGE", false), 0},
          {"", 0}}) {
        SWMM_Engine e = swmm_engine_create();
        const fs::path inp =
            writeModel(dir, "m" + std::to_string(expected) + std::to_string(rows.size()),
                       "00:00:00", "", rows);
        ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), nullptr, nullptr, nullptr), SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
        int uses = -1;
        EXPECT_EQ(swmm_restart_uses_checkpoint(e, &uses), SWMM_OK);
        EXPECT_EQ(uses, expected) << rows;
        swmm_engine_close(e);
        swmm_engine_destroy(e);
    }
}

// ============================================================================
// A twin's cycles: each continues from the previous cycle's record into the same model with a
// later end and the rain that arrived since. Continuation records end a step at their time, so
// the cycles reproduce an uninterrupted run that records at the same times, bit for bit.
// ============================================================================

namespace {

/** Rain every five minutes over four hours, in in/h: two storms and a dry spell. */
constexpr double kRain[48] = {0.0, 0.4, 1.2, 2.4, 3.0, 2.0, 1.1, 0.6, 0.3, 0.1, 0.0, 0.0,
                              0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0,
                              0.8, 1.6, 3.2, 4.0, 2.5, 1.5, 0.9, 0.5, 0.2, 0.0, 0.0, 0.0,
                              0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.3, 0.6, 0.6, 0.3, 0.1, 0.0};

struct TwinModel {
    int end_minutes = 240;             ///< END_TIME after 01/01/2020 00:00.
    int rain_minutes = 240;            ///< Rain points strictly before this many minutes.
    int inflow_minutes = 240;          ///< Inflow series points up to this many minutes.
    double roughness = 0.013;          ///< Conduit C1's Manning n.
    double first_rain = 0.0;           ///< Value of the first rain point.
    std::string wet_step = "00:01:00"; ///< WET_STEP and DRY_STEP; steps that do not divide
    std::string dry_step = "00:05:00"; ///< an hour end at a record's time only when it ends them.
    std::string restart;               ///< [RESTART] rows.
};

std::string clock(int minutes) {
    char text[16];
    std::snprintf(text, sizeof text, "%02d:%02d:00", minutes / 60, minutes % 60);
    return text;
}

/** A subcatchment draining to a two-conduit dynamic-wave network, its gage on a rain series. */
fs::path writeTwinModel(const fs::path& dir, const std::string& name, const TwinModel& m) {
    const fs::path inp = dir / (name + ".inp");
    std::ofstream o(inp);
    o << "[OPTIONS]\nFLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n"
         "IGNORE_GROUNDWATER YES\nSTART_DATE 01/01/2020\nSTART_TIME 00:00:00\n"
         "REPORT_START_DATE 01/01/2020\nREPORT_START_TIME 00:00:00\nEND_DATE 01/01/2020\n"
         "END_TIME "
      << clock(m.end_minutes) << "\nREPORT_STEP 00:01:00\nWET_STEP " << m.wet_step << "\nDRY_STEP "
      << m.dry_step
      << "\nROUTING_STEP 15\n"
         "VARIABLE_STEP 0.75\n\n[RESTART]\n"
      << m.restart
      << "\n\n[RAINGAGES]\nRG INTENSITY 0:05 1.0 TIMESERIES RAIN\n\n"
         "[SUBCATCHMENTS]\nS1 RG J1 20 60 800 1.0 0\n\n"
         "[SUBAREAS]\nS1 0.012 0.15 0.05 0.2 25 OUTLET\n\n"
         "[INFILTRATION]\nS1 3.0 0.5 4 7 0\n\n"
         "[JUNCTIONS]\nJ1 10 6 0 0 0\nJ2 8 6 0 0 0\n\n[OUTFALLS]\nO1 5 FREE\n\n"
         "[CONDUITS]\nC1 J1 J2 400 "
      << m.roughness
      << " 0 0 0 0\nC2 J2 O1 400 0.013 0 0 0 0\n\n"
         "[XSECTIONS]\nC1 CIRCULAR 1.5 0 0 0 1\nC2 CIRCULAR 1.5 0 0 0 1\n\n"
         "[INFLOWS]\nJ2 FLOW BASE\n\n[TIMESERIES]\n";
    for (int i = 0; i < 48 && 5 * i < m.rain_minutes; ++i)
        o << "RAIN 01/01/2020 " << clock(5 * i) << " " << (i == 0 ? m.first_rain : kRain[i])
          << "\n";
    for (int minute = 0; minute <= m.inflow_minutes; minute += 30)
        o << "BASE 01/01/2020 " << clock(minute) << " " << 0.2 + 0.05 * (minute % 60 == 0) << "\n";
    return inp;
}

std::string continuationRows(const fs::path& file, const std::vector<int>& at_minutes) {
    std::string rows =
        "RECORD_FILE \"" + file.string() + "\"\nRECORD_FORMAT HDF5\nRECORD_CONTINUATION YES\n";
    for (const int minute : at_minutes)
        rows += minute < 0 ? std::string("RECORD_AT END\n")
                           : "RECORD_AT 01/01/2020 " + clock(minute) + "\n";
    return rows;
}

std::string continueFrom(const fs::path& file) {
    return "SOURCE_FILE \"" + file.string() + "\"\nSOURCE_RECORD LATEST\nMODE CONTINUE\n";
}

/** Cycles of @p hours each over four hours, every one continuing the one before. */
Run runCycles(const fs::path& dir, int hours, const TwinModel& steps) {
    Run last;
    fs::path previous;
    for (int k = 1; k * hours <= 4; ++k) {
        TwinModel m;
        m.wet_step = steps.wet_step;
        m.dry_step = steps.dry_step;
        m.end_minutes = m.rain_minutes = 60 * hours * k;
        const fs::path record = dir / ("cycle" + std::to_string(k) + ".h5");
        m.restart =
            (k > 1 ? continueFrom(previous) : std::string()) + continuationRows(record, {-1});
        last = run(writeTwinModel(dir, "cycle" + std::to_string(k), m));
        if (last.rc != SWMM_OK)
            return last;
        previous = record;
    }
    return last;
}

} // namespace

TEST(RestartRecords, TwinCyclesReproduceTheUninterruptedRun) {
    {
        SWMM_Engine e = swmm_engine_create();
        unsigned long flags = 0;
        ASSERT_EQ(swmm_checkpoint_capabilities(e, &flags), SWMM_OK);
        EXPECT_TRUE(flags & SWMM_CHECKPOINT_CONTINUE_EXTENDED_FORCING);
        swmm_engine_destroy(e);
    }
    // A 3.5 minute wet step (at most the gage's 5 minute recording interval, or the engine
    // shortens it) and a 7 minute dry step do not divide the hour: runoff steps end at the
    // records' times only because the records end them there.
    for (const auto& [hours, steps] : {std::pair<int, bool>{1, false}, {2, false}, {1, true}}) {
        const fs::path dir = caseDir("twin_cycles_" + std::to_string(hours) + "h" +
                                     (steps ? "_runoff_steps_unaligned" : ""));
        TwinModel whole;
        if (steps) {
            whole.wet_step = "00:03:30";
            whole.dry_step = "00:07:00";
        }
        std::vector<int> boundaries;
        for (int minute = 60 * hours; minute < 240; minute += 60 * hours)
            boundaries.push_back(minute);
        whole.restart = continuationRows(dir / "whole.h5", boundaries);
        const auto reference = run(writeTwinModel(dir, "whole", whole));
        ASSERT_EQ(reference.rc, SWMM_OK) << reference.message;
        const auto cycles = runCycles(dir, hours, whole);
        ASSERT_EQ(cycles.rc, SWMM_OK) << hours << " h cycles: " << cycles.message;
        EXPECT_EQ(cycles.state, reference.state) << hours << " h cycles";
        const std::string a = bytes(dir / "whole.out"),
                          b = bytes(dir / ("cycle" + std::to_string(4 / hours) + ".out"));
        ASSERT_FALSE(a.empty());
        EXPECT_EQ(b, a) << hours << " h cycles: the binary output differs";
    }
}

// A twin cycle sets its end through the API, as the Python Solver does: the day's serial plus
// the time as a fraction of a day. Some of those serials lie a rounding below their whole
// second, and the duration, floored to whole seconds, ended such a run one second early, so its
// record was a second before the cycle's end and the next cycle left the uninterrupted run.
TEST(RestartRecords, AnEndSetThroughTheApiEndsTheRunAtItsTime) {
    const fs::path dir = caseDir("end_through_the_api");
    const fs::path inp = writeTwinModel(dir, "model", TwinModel{});
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), (dir / "model.rpt").string().c_str(),
                               nullptr, nullptr),
              SWMM_OK);
    double start = 0.0;
    ASSERT_EQ(swmm_options_get_start_date(e, &start), SWMM_OK);
    int seconds = 0; // the first whole minute whose serial lies below its second
    for (int minute = 1; minute <= 240 && !seconds; ++minute)
        if ((start + 60.0 * minute / 86400.0 - start) * 86400.0 < 60.0 * minute)
            seconds = 60 * minute;
    ASSERT_GT(seconds, 0) << "no serial in the four hours lies below its second";
    ASSERT_EQ(swmm_options_set_end_date(e, start + seconds / 86400.0), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 0), SWMM_OK);
    double elapsed = 1.0, last = 0.0;
    int rc = SWMM_OK;
    while (elapsed > 0.0 && (rc = swmm_engine_step(e, &elapsed)) == SWMM_OK)
        ASSERT_EQ(swmm_engine_get_elapsed_seconds(e, &last), SWMM_OK);
    EXPECT_EQ(rc, SWMM_OK);
    EXPECT_EQ(last, double(seconds)) << "the run ended before the end set through the API";
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}

TEST(RestartRecords, TwinCycleRefusesAnyOtherChange) {
    const fs::path dir = caseDir("twin_cycle_refusals");
    // Cycle 1's inflow series stops at 00:30, before the record at 01:00: the values after
    // 00:30 would interpolate towards a later point, so extending it is refused. One that
    // already has its point at 01:00 may be extended after it.
    TwinModel first;
    first.end_minutes = first.rain_minutes = 60;
    first.inflow_minutes = 30;
    first.restart = continuationRows(dir / "first.h5", {-1});
    ASSERT_EQ(run(writeTwinModel(dir, "first", first)).rc, SWMM_OK);
    const auto next = [&](const std::string& name, TwinModel m) {
        m.restart = continueFrom(dir / "first.h5");
        return run(writeTwinModel(dir, name, m));
    };
    TwinModel ok;
    ok.end_minutes = ok.rain_minutes = 120;
    ok.inflow_minutes = 30;
    EXPECT_EQ(next("rain_arrived", ok).rc, SWMM_OK)
        << "only the rain after 01:00 and the end change";
    TwinModel pointed = first;
    pointed.inflow_minutes = 60;
    pointed.restart = continuationRows(dir / "pointed.h5", {-1});
    ASSERT_EQ(run(writeTwinModel(dir, "pointed", pointed)).rc, SWMM_OK);
    TwinModel extended = ok;
    extended.inflow_minutes = 120;
    extended.restart = continueFrom(dir / "pointed.h5");
    const auto after_its_point = run(writeTwinModel(dir, "inflow_after_its_point", extended));
    EXPECT_EQ(after_its_point.rc, SWMM_OK) << after_its_point.message;
    TwinModel rough = ok;
    rough.roughness = 0.015;
    TwinModel past = ok;
    past.first_rain = 0.5;
    TwinModel inflow = ok;
    inflow.inflow_minutes = 120;
    TwinModel earlier = ok;
    earlier.end_minutes = 30;
    for (const auto& [name, model] : {std::pair<const char*, TwinModel>{"roughness", rough},
                                      {"past_rain", past},
                                      {"inflow_series", inflow},
                                      {"earlier_end", earlier}}) {
        const auto r = next(name, model);
        EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << name << ": " << r.message;
        EXPECT_FALSE(r.message.empty()) << name;
    }
}

// A run with no output file writes no binary results whatever start() is asked, as the Python
// Solver asks by default, so its continuation records carry none and continue into such a run.
TEST(RestartRecords, ContinuationRecordsInRunsWithoutAnOutputFile) {
    const fs::path dir = caseDir("continuation_without_output_file");
    const auto stepped = [&](const fs::path& inp) {
        struct Run r; // gtest's Test::Run hides the type here
        SWMM_Engine e = swmm_engine_create();
        r.rc = swmm_engine_open(
            e, inp.string().c_str(),
            (inp.parent_path() / (inp.stem().string() + ".rpt")).string().c_str(), "", nullptr);
        if (r.rc == SWMM_OK)
            r.rc = swmm_engine_initialize(e);
        if (r.rc == SWMM_OK)
            r.rc = swmm_engine_start(e, 1);
        double elapsed = 0.0;
        while (r.rc == SWMM_OK && (r.rc = swmm_engine_step(e, &elapsed)) == SWMM_OK &&
               elapsed > 0.0) {
        }
        for (int i = 0; r.rc == SWMM_OK && i < swmm_node_count(e); ++i) {
            double v = 0.0;
            swmm_node_get_depth(e, i, &v);
            r.state.push_back(v);
        }
        if (r.rc == SWMM_OK)
            r.rc = swmm_engine_end(e);
        if (const char* m = swmm_get_last_error_msg(e))
            r.message = m;
        swmm_engine_close(e);
        swmm_engine_destroy(e);
        return r;
    };
    TwinModel whole;
    whole.restart = continuationRows(dir / "whole.h5", {120});
    const auto reference = stepped(writeTwinModel(dir, "whole", whole));
    ASSERT_EQ(reference.rc, SWMM_OK) << reference.message;
    TwinModel first;
    first.end_minutes = first.rain_minutes = 120;
    first.restart = continuationRows(dir / "first.h5", {-1});
    const auto a = stepped(writeTwinModel(dir, "first", first));
    ASSERT_EQ(a.rc, SWMM_OK) << a.message;
    const std::string json = inspect(dir / "first.h5");
    EXPECT_EQ(count(json, "\"binary_output\":false"), 1) << json;
    EXPECT_EQ(count(json, "\"continuation\":true"), 1) << json;
    TwinModel second;
    second.restart = continueFrom(dir / "first.h5");
    const auto b = stepped(writeTwinModel(dir, "second", second));
    ASSERT_EQ(b.rc, SWMM_OK) << b.message;
    EXPECT_EQ(b.state, reference.state);
}
