// SPDX-License-Identifier: Apache-2.0
/**
 * @file      test_hotstart_save_failures.cpp
 * @brief     A scheduled hot start save that cannot be written fails the run, and no hot start
 *            save overwrites a file that is not a hot start file.
 *
 * Outputs are written under tests/output/hotstart_save_failures in the source tree.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_hotstart.h>

namespace fs = std::filesystem;

namespace {

fs::path caseDir(const std::string& name) {
    const fs::path dir = fs::path(__FILE__).parent_path().parent_path().parent_path() / "output" /
                         "hotstart_save_failures" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

fs::path writeModel(const fs::path& dir, const std::string& files) {
    const fs::path inp = dir / "model.inp";
    std::ofstream o(inp);
    o << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2020\n"
         "START_TIME 00:00:00\nREPORT_START_DATE 01/01/2020\nREPORT_START_TIME 00:00:00\n"
         "END_DATE 01/01/2020\nEND_TIME 01:00:00\nREPORT_STEP 00:05:00\nROUTING_STEP 20\n\n"
         "[FILES]\n"
      << files
      << "\n\n[JUNCTIONS]\nJ1 10 5 0 0 0\n\n[OUTFALLS]\nO1 0 FREE\n\n"
         "[CONDUITS]\nC1 J1 O1 400 0.013 0 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 2 0 0 0 1\n\n"
         "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 2.0\n";
    return inp;
}

struct Outcome {
    int rc = SWMM_OK;
    std::string message;
};

/** @brief Run to completion and report the first failing lifecycle code and its message. */
Outcome runModel(const fs::path& inp) {
    Outcome r;
    SWMM_Engine e = swmm_engine_create();
    const std::string rpt = (inp.parent_path() / "model.rpt").string();
    const std::string out = (inp.parent_path() / "model.out").string();
    r.rc = swmm_engine_open(e, inp.string().c_str(), rpt.c_str(), out.c_str(), nullptr);
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_initialize(e);
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_start(e, 1);
    double elapsed = 0.0;
    while (r.rc == SWMM_OK) {
        r.rc = swmm_engine_step(e, &elapsed);
        if (r.rc != SWMM_OK || elapsed <= 0.0)
            break;
    }
    if (r.rc == SWMM_OK)
        r.rc = swmm_engine_end(e);
    if (const char* m = swmm_get_last_error_msg(e))
        r.message = m;
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return r;
}

std::string bytes(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), {}};
}

/** @brief A small SQLite file, as a GeoPackage starts. */
fs::path writeSqliteLike(const fs::path& p) {
    std::ofstream o(p, std::ios::binary);
    o.write("SQLite format 3\0", 16);
    o << std::string(84, '\x01') << "some user data";
    return p;
}

/** @brief A small file with the HDF5 signature. */
fs::path writeHdf5Like(const fs::path& p) {
    std::ofstream o(p, std::ios::binary);
    o.write("\x89HDF\r\n\x1a\n", 8);
    o << std::string(40, '\x02') << "results";
    return p;
}

} // namespace

// D3: a scheduled save that cannot be written is a run error, not a warning.
TEST(HotstartSaveFailures, DatedSaveThatCannotBeWrittenFailsTheRun) {
    for (const char* format : {"NATIVE", "LEGACY"}) {
        const fs::path dir = caseDir(std::string("dated_") + format);
        const fs::path target = dir / "no_such_folder" / "mid.hsf";
        const auto r = runModel(writeModel(dir, "SAVE HOTSTART \"" + target.string() +
                                                    "\" 01/01/2020 00:30:00 " + format));
        EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << format << ": " << r.message;
        EXPECT_NE(r.message.find("mid.hsf"), std::string::npos) << r.message;
    }
}

TEST(HotstartSaveFailures, EndOfRunSaveThatCannotBeWrittenFailsTheRun) {
    for (const char* format : {"NATIVE", "LEGACY"}) {
        const fs::path dir = caseDir(std::string("end_") + format);
        const fs::path target = dir / "no_such_folder" / "end.hsf";
        const auto r =
            runModel(writeModel(dir, "SAVE HOTSTART \"" + target.string() + "\" " + format));
        EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << format << ": " << r.message;
        EXPECT_NE(r.message.find("end.hsf"), std::string::npos) << r.message;
    }
}

// D4: a scheduled save never overwrites a file that is not a hot start file.
TEST(HotstartSaveFailures, ScheduledSaveLeavesAContainerUnchanged) {
    const fs::path dir = caseDir("scheduled_container");
    const fs::path gpkg = writeSqliteLike(dir / "results.gpkg");
    const std::string before = bytes(gpkg);
    const auto r = runModel(writeModel(dir, "SAVE HOTSTART \"" + gpkg.string() + "\" NATIVE"));
    EXPECT_EQ(r.rc, SWMM_ERR_HOTSTART) << r.message;
    EXPECT_EQ(bytes(gpkg), before);
}

// D4: the explicit save tests the target first. A hot start file or a new path is
// written; anything else is refused and left byte-identical.
TEST(HotstartSaveFailures, ExplicitSaveTestsTheTargetFirst) {
    const fs::path dir = caseDir("explicit");
    const fs::path inp = writeModel(dir, "");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), (dir / "model.rpt").string().c_str(),
                               (dir / "model.out").string().c_str(), nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(e, 1), SWMM_OK);
    double elapsed = 0.0;
    ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK);

    const fs::path gpkg = writeSqliteLike(dir / "results.gpkg");
    const fs::path h5 = writeHdf5Like(dir / "results.h5");
    const fs::path text = dir / "notes.txt";
    std::ofstream(text) << "keep me";
    for (const fs::path& p : {gpkg, h5, text}) {
        const std::string before = bytes(p);
        EXPECT_EQ(swmm_hotstart_save(e, p.string().c_str()), SWMM_ERR_HOTSTART) << p;
        EXPECT_EQ(bytes(p), before) << p;
        EXPECT_NE(std::string(swmm_hotstart_last_io_error()).find(p.filename().string()),
                  std::string::npos)
            << swmm_hotstart_last_io_error();
    }
    const fs::path fresh = dir / "fresh.hsf";
    EXPECT_EQ(swmm_hotstart_save(e, fresh.string().c_str()), SWMM_OK);
    EXPECT_EQ(swmm_hotstart_save(e, fresh.string().c_str()), SWMM_OK); // a hot start file again
    const fs::path named_gpkg = dir / "state.gpkg"; // a new path with any suffix stays HSF
    EXPECT_EQ(swmm_hotstart_save(e, named_gpkg.string().c_str()), SWMM_OK);

    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
