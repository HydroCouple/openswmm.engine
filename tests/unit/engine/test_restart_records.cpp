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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

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
