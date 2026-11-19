// SPDX-License-Identifier: Apache-2.0
/**
 * @file      test_geopackage_run_lifecycle.cpp
 * @brief     GeoPackage project saves and runs through the public C API, as the GUI and the CLI
 *            reach them: no test plants a parent row first.
 *
 * Outputs are written under tests/output/geopackage_run_lifecycle in the source tree so a reviewer
 * can open them after a run.
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_geopackage.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

constexpr const char* kGeoPackagePlugin = "org.hydrocouple.openswmm.plugins.geopackage";

/** @brief Fresh, reviewable folder for one test case; earlier contents are removed. */
fs::path caseDir(const std::string& name) {
    const fs::path dir = fs::path(__FILE__).parent_path().parent_path().parent_path() / "output" /
                         "geopackage_run_lifecycle" / name;
    std::error_code ec;
    fs::remove_all(dir, ec);
    fs::create_directories(dir);
    return dir;
}

/**
 * @brief Write the two-node dynamic-wave model with extra [OPTIONS] and [FILES] lines.
 * @return Absolute path of the written input file.
 */
fs::path writeModel(const fs::path& dir, const std::string& options, const std::string& files) {
    const fs::path inp = dir / "model.inp";
    std::ofstream o(inp);
    o << "[TITLE]\ngeopackage run lifecycle\n\n"
         "[OPTIONS]\nFLOW_UNITS CFS\nINFILTRATION HORTON\nFLOW_ROUTING DYNWAVE\n"
         "START_DATE 01/01/2020\nSTART_TIME 00:00:00\nREPORT_START_DATE 01/01/2020\n"
         "REPORT_START_TIME 00:00:00\nEND_DATE 01/01/2020\nEND_TIME 01:00:00\n"
         "REPORT_STEP 00:05:00\nWET_STEP 00:05:00\nDRY_STEP 01:00:00\nROUTING_STEP 20\n"
      << options << "\n\n[FILES]\n"
      << files
      << "\n\n[JUNCTIONS]\nJ1 10 5 0 0 0\n\n[OUTFALLS]\nO1 0 FREE\n\n"
         "[CONDUITS]\nC1 J1 O1 400 0.013 0 0 0 0\n\n[XSECTIONS]\nC1 CIRCULAR 2 0 0 0 1\n\n"
         "[INFLOWS]\nJ1 FLOW \"\" FLOW 1.0 1.0 2.0\n\n[REPORT]\nNODES ALL\nLINKS ALL\n\n"
         "[COORDINATES]\nJ1 0 0\nO1 400 0\n";
    return inp;
}

/** @brief Run a model to completion; returns the first nonzero lifecycle code. */
int runModel(const fs::path& inp, std::string* error = nullptr) {
    SWMM_Engine e = swmm_engine_create();
    const std::string rpt = (inp.parent_path() / "model.rpt").string();
    const std::string out = (inp.parent_path() / "model.out").string();
    int rc = swmm_engine_open(e, inp.string().c_str(), rpt.c_str(), out.c_str(), nullptr);
    if (rc == SWMM_OK)
        rc = swmm_engine_initialize(e);
    if (rc == SWMM_OK)
        rc = swmm_engine_start(e, 1);
    double elapsed = 0.0;
    while (rc == SWMM_OK) {
        rc = swmm_engine_step(e, &elapsed);
        if (rc != SWMM_OK || elapsed <= 0.0)
            break;
    }
    if (rc == SWMM_OK)
        rc = swmm_engine_end(e);
    if (rc != SWMM_OK && error) {
        const char* msg = swmm_get_last_error_msg(e);
        *error = msg ? msg : "";
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return rc;
}

/** @brief Read one integer from a read-only connection to @p db. */
int scalar(const fs::path& db, const std::string& sql) {
    sqlite3* h = nullptr;
    if (sqlite3_open_v2(db.string().c_str(), &h, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK) {
        sqlite3_close(h);
        return -1;
    }
    sqlite3_stmt* s = nullptr;
    int value = -1;
    if (sqlite3_prepare_v2(h, sql.c_str(), -1, &s, nullptr) == SQLITE_OK &&
        sqlite3_step(s) == SQLITE_ROW)
        value = sqlite3_column_int(s, 0);
    sqlite3_finalize(s);
    sqlite3_close(h);
    return value;
}

std::string saveRow(const fs::path& dir) {
    return "SAVE HOTSTART \"" + (dir / "late.hsf").string() + "\" NATIVE";
}

} // namespace

// A project saved to a new GeoPackage keeps its scheduled hot start row. The
// slot's parent row did not exist, so the commit failed with a foreign-key error.
TEST(GeoPackageRunLifecycle, ProjectSaveWithAPendingSaveRowCommits) {
    const fs::path dir = caseDir("project_save_pending_row");
    const fs::path inp = writeModel(dir, "", saveRow(dir));
    const fs::path gpkg = dir / "project.gpkg";

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), nullptr, nullptr, nullptr), SWMM_OK);
    const int rc = swmm_model_write_with_plugin(e, gpkg.string().c_str(), kGeoPackagePlugin);
    const char* why = swmm_get_last_error_msg(e);
    EXPECT_EQ(rc, SWMM_OK) << (why ? why : "");
    // Saving again over the same file replaces the row instead of colliding with it.
    EXPECT_EQ(swmm_model_write_with_plugin(e, gpkg.string().c_str(), kGeoPackagePlugin), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    EXPECT_EQ(scalar(gpkg, "SELECT count(*) FROM hotstart_slots WHERE direction='SAVE' AND "
                           "status='pending' AND save_format='NATIVE'"),
              1);
}

// The project model is not a run: listing runs still finds none after a save.
TEST(GeoPackageRunLifecycle, ProjectModelIsNotListedAsARun) {
    const fs::path dir = caseDir("project_model_not_a_run");
    const fs::path inp = writeModel(dir, "", saveRow(dir));
    const fs::path gpkg = dir / "project.gpkg";

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.string().c_str(), nullptr, nullptr, nullptr), SWMM_OK);
    ASSERT_EQ(swmm_model_write_with_plugin(e, gpkg.string().c_str(), kGeoPackagePlugin), SWMM_OK);
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    SWMM_Gpkg db = swmm_gpkg_open(gpkg.string().c_str());
    ASSERT_NE(db, nullptr);
    EXPECT_EQ(swmm_gpkg_simulation_count(db), 0);
    swmm_gpkg_close(db);
}

// A run whose results go to a GeoPackage, with a scheduled hot start row, starts,
// finishes and keeps the row. It failed at start with a foreign-key error.
TEST(GeoPackageRunLifecycle, RunWithAGeoPackageOutputAndASaveRowCompletes) {
    const fs::path dir = caseDir("run_with_save_row");
    const fs::path gpkg = dir / "results.gpkg";
    const fs::path inp = writeModel(dir,
                                    "IO_OUTPUT_WRITER " + std::string(kGeoPackagePlugin) +
                                        "\nIO_OUTPUT_PATH " + gpkg.string(),
                                    saveRow(dir));
    std::string why;
    ASSERT_EQ(runModel(inp, &why), SWMM_OK) << why;

    EXPECT_EQ(scalar(gpkg, "SELECT count(*) FROM hotstart_slots WHERE direction='SAVE'"), 1);
    EXPECT_GT(scalar(gpkg, "SELECT count(*) FROM result_timeseries"), 0);
    EXPECT_EQ(scalar(gpkg, "SELECT count(*) FROM simulations WHERE status='completed'"), 1);
}

// Results and report written to one GeoPackage share the run and keep the row.
TEST(GeoPackageRunLifecycle, SingleContainerRunWithASaveRowCompletes) {
    const fs::path dir = caseDir("single_container_with_save_row");
    const fs::path gpkg = dir / "all.gpkg";
    const fs::path inp = writeModel(
        dir,
        "IO_OUTPUT_WRITER " + std::string(kGeoPackagePlugin) + "\nIO_REPORT_WRITER " +
            kGeoPackagePlugin + "\nIO_SINGLE_CONTAINER YES\nIO_OUTPUT_PATH " + gpkg.string(),
        saveRow(dir));
    std::string why;
    ASSERT_EQ(runModel(inp, &why), SWMM_OK) << why;

    EXPECT_EQ(scalar(gpkg, "SELECT count(*) FROM hotstart_slots WHERE direction='SAVE'"), 1);
    EXPECT_EQ(scalar(gpkg, "SELECT count(*) FROM simulations"), 1);
}
