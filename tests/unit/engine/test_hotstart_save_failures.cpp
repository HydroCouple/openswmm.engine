// SPDX-License-Identifier: Apache-2.0
/**
 * @file      test_hotstart_save_failures.cpp
 * @brief     A scheduled hot start save that cannot be written fails the run.
 *
 * Outputs are written under tests/output/hotstart_save_failures in the source tree.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

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
