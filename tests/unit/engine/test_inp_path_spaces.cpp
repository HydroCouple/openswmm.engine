// SPDX-License-Identifier: Apache-2.0
// File references must retain their exact name after saving and reopening.

/**
 * @file      test_inp_path_spaces.cpp
 * @brief     Tests for inp path spaces.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_gw_transport.h>
#include "core/SWMMEngine.hpp"
#include <filesystem>
#include <fstream>
#include <cstdlib>
#include <string>

namespace {
namespace fs = std::filesystem;
const char* base = R"([OPTIONS]
FLOW_UNITS CMS
FLOW_ROUTING DYNWAVE
START_DATE 01/01/2020
END_DATE 01/01/2020
END_TIME 01:00:00
REPORT_STEP 00:05:00
ROUTING_STEP 5
[JUNCTIONS]
J1 10 5 0 0 0
[OUTFALLS]
O1 0 FREE NO
[CONDUITS]
C1 J1 O1 400 0.01 0 0 0 0
[XSECTIONS]
C1 CIRCULAR 1 0 0 0 1
[POLLUTANTS]
TSS MG/L 0 0 0 0 NO * 0 0 0
)";
const char* mesh = "[2D_VERTICES]\n0 0 0\n10 0 0\n0 10 0\n"
                   "[2D_TRIANGLES]\n0 1 2 0.035 0\n";

class InpPathSpacesTest : public ::testing::TestWithParam<std::string> {
protected:
    fs::path dir;
    SWMM_Engine engine = nullptr, reopened = nullptr;
    /**
     * @brief Create a per-case artifact directory and an unopened engine for path tests.
     *
     * Uses absolute OPENSWMM_PATH_TEST_OUTPUT or file_path_spaces_output plus suite/case names,
     * retaining prior files. Filesystem errors propagate; creates only engine, leaving reopened null
     * until a case sets it. Fatal GoogleTest assertions return from this hook on failure; any created
     * handle remains owned by the fixture for teardown.
     */
    void SetUp() override {
        const char* root = std::getenv("OPENSWMM_PATH_TEST_OUTPUT");
        const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
        dir = fs::absolute(root ? fs::path(root) : fs::path("file_path_spaces_output")) /
              info->test_suite_name() / info->name();
        fs::create_directories(dir);
        engine = swmm_engine_create();
        ASSERT_NE(engine, nullptr);
    }
    /**
     * @brief Destroy both owned file-path test engines while retaining artifacts.
     *
     * Destroys non-null reopened and engine without separate close calls/status checks; paths are not
     * deleted by this hook.
     */
    void TearDown() override {
        if (reopened)
            swmm_engine_destroy(reopened);
        if (engine)
            swmm_engine_destroy(engine);
    }
    /**
     * @brief Create artifact parents and write a path-with-spaces fixture file.
     *
     * Uses throwing parent-directory creation and a fatal assertion on stream.good after insertion;
     * final close failure is not checked. Fatal assertion returns from this helper only; no file/body
     * storage is retained.
     *
     * @param[in] name Safe relative path beneath fixture dir; existing file is truncated.
     * @param[in] text Complete text to insert verbatim.
     */
    void file(const fs::path& name, const std::string& text) {
        fs::create_directories((dir / name).parent_path());
        std::ofstream out(dir / name);
        out << text;
        ASSERT_TRUE(out.good());
    }
    /**
     * @brief Write/open the base source deck plus raw extra sections.
     *
     * Writes Source Model.inp, opens into existing created engine with Source Report.rpt and no binary
     * output. Fatal open assertion returns from this helper with the handle retained for teardown; no
     * initialize/start occurs. A failure in the nested file helper does not itself stop this function.
     *
     * @param[in] extra Additional input text appended to base with caller-supplied section/newline
     *     delimiters.
     */
    void open(const std::string& extra) {
        file("Source Model.inp", std::string(base) + extra);
        ASSERT_EQ(swmm_engine_open(engine, (dir / "Source Model.inp").string().c_str(),
                                   (dir / "Source Report.rpt").string().c_str(), nullptr, nullptr),
                  0)
            << swmm_get_last_error_msg(engine);
    }
    /**
     * @brief Save the opened source model and open an independently owned saved copy.
     *
     * Requires readable/writable source engine and reopened=null. Writes Saved Model.inp, creates
     * reopened and opens Saved Report.rpt without binary output. Fatal assertions return with partial
     * artifacts/handles for teardown; repeated successful use without destruction would overwrite
     * ownership. Does not initialize either engine.
     */
    void saveAndReopen() {
        const std::string saved = (dir / "Saved Model.inp").string();
        ASSERT_EQ(swmm_model_write(engine, saved.c_str()), 0);
        reopened = swmm_engine_create();
        ASSERT_NE(reopened, nullptr);
        ASSERT_EQ(swmm_engine_open(reopened, saved.c_str(),
                                   (dir / "Saved Report.rpt").string().c_str(), nullptr, nullptr),
                  0)
            << swmm_get_last_error_msg(reopened);
    }
    /**
     * @brief Read the originally authored path spelling for one model file role.
     *
     * Also supplies an absolute-path buffer but discards it. Records nonfatal status expectation; no
     * allocation retry or separate truncation/error result occurs.
     *
     * @param[in] e Live fixture engine with readable file-path metadata.
     * @param[in] role Supported singleton file role accepted by swmm_file_path_get; object-specific
     *     roles are not supplied an object id.
     * @return Owned original spelling from a4096-byte zeroed buffer; empty can mean empty path or
     *     failed retrieval.
     */
    std::string path(SWMM_Engine e, SWMM_FilePathRole role) {
        char absolute[4096] = {}, original[4096] = {};
        EXPECT_EQ(swmm_file_path_get(e, role, nullptr, absolute, sizeof(absolute), original,
                                     sizeof(original)),
                  0);
        return original;
    }
    /**
     * @brief Probe whether the fixture exposes its2D mesh-file path role.
     *
     * Uses existing engine and null object id; does not prove a mesh exists/initialized independently
     * of API role/capacity behavior, and does not mutate paths.
     * @return True only when swmm_file_path_get returns0 with both8-byte output buffers; false for any
     *     error, including insufficient capacity.
     */
    bool has2D() {
        char absolute[8] = {}, original[8] = {};
        return swmm_file_path_get(engine, SWMM_FILE_MESH_2D, nullptr, absolute, sizeof(absolute),
                                  original, sizeof(original)) == 0;
    }
};

TEST_P(InpPathSpacesTest, MeshReferenceSurvivesSaveWithDecoy) {
    if (!has2D())
        GTEST_SKIP() << "engine built without 2D";
    const std::string name = "Mesh  Folder/" + GetParam() + ".2dm";
    file(name, mesh);
    // A truncated/collapsed path must not accidentally pass by finding a file.
    file("Mesh Folder/My Model.2dm", "[2D_VERTICES]\n0 0 99\n10 0 99\n0 10 99\n"
                                     "[2D_TRIANGLES]\n0 1 2 0.035 0\n");
    ASSERT_NO_FATAL_FAILURE(open("[2D_MESH_FILE]\nFILE \"" + name + "\"\n"));
    EXPECT_EQ(path(engine, SWMM_FILE_MESH_2D), name);
    ASSERT_NO_FATAL_FAILURE(saveAndReopen());
    EXPECT_EQ(path(reopened, SWMM_FILE_MESH_2D), name);
}

TEST_P(InpPathSpacesTest, OutputReferenceSurvivesSave) {
    if (!has2D())
        GTEST_SKIP() << "engine built without 2D";
    const std::string name = "Results  Folder/" + GetParam() + ".h5";
    ASSERT_NO_FATAL_FAILURE(
        open("[2D_OPTIONS]\nREPORT_2D NO\nOUTPUT_FILE \"" + name + "\"\n" + mesh));
    EXPECT_EQ(path(engine, SWMM_FILE_OUTPUT_2D), name);
    ASSERT_NO_FATAL_FAILURE(saveAndReopen());
    EXPECT_EQ(path(reopened, SWMM_FILE_OUTPUT_2D), name);
}

TEST_P(InpPathSpacesTest, InitialQualityReferenceSurvivesSave) {
    const std::string name = "Quality  Folder/" + GetParam() + ".csv";
    file(name, "scope,element,constituent,value\nNODE,J1,TSS,2\n");
    ASSERT_NO_FATAL_FAILURE(open("[INITIAL_QUALITY]\nFILE \"" + name + "\"\n"));
    ASSERT_EQ(static_cast<openswmm::SWMMEngine*>(engine)->context().initial_quality.count(), 1);
    ASSERT_NO_FATAL_FAILURE(saveAndReopen());
    const auto& quality = static_cast<openswmm::SWMMEngine*>(reopened)->context().initial_quality;
    EXPECT_EQ(quality.file, name);
    ASSERT_EQ(quality.count(), 1);
    EXPECT_DOUBLE_EQ(quality.value[0], 2);
}

#ifdef OPENSWMM_HAS_2D
TEST_P(InpPathSpacesTest, GroundwaterQualityReferenceSurvivesSave) {
    if (!has2D())
        GTEST_SKIP() << "engine built without 2D";
    const std::string name = "Quality  Folder/" + GetParam() + ".csv";
    file(name, "scope,zone,species,value\n*,SAT,TSS,2\n");
    ASSERT_NO_FATAL_FAILURE(
        open(std::string(mesh) + "[GW_INITIAL_QUALITY]\nFILE \"" + name + "\"\n"));
    ASSERT_EQ(swmm_gw_init_quality_count(engine), 1);
    ASSERT_NO_FATAL_FAILURE(saveAndReopen());
    char filename[4096] = {};
    ASSERT_EQ(swmm_gw_init_quality_file_get(reopened, filename, sizeof(filename)), 0);
    EXPECT_EQ(std::string(filename), name);
    EXPECT_EQ(swmm_gw_init_quality_count(reopened), 1);
}

#endif

TEST_P(InpPathSpacesTest, ComponentConfigurationSurvivesSave) {
    const std::string name = "Config  Folder/" + GetParam() + ".rxn";
    file(name, "[REACTION_OPTIONS]\nSOLVER RK5\n[REACTION_SPECIES]\nBULK A MG\n");
    ASSERT_NO_FATAL_FAILURE(
        open("[PROCESS_COMPONENTS]\norg.hydrocouple.openswmm.reactions config=\"" + name + "\"\n"));
    ASSERT_NO_FATAL_FAILURE(saveAndReopen());
    const auto& specs =
        static_cast<openswmm::SWMMEngine*>(reopened)->context().process_component_specs;
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0].config_path, name);
}

TEST_P(InpPathSpacesTest, PluginLibraryAndArgumentsSurviveSave) {
    ASSERT_NO_FATAL_FAILURE(open(""));
    const std::string name = "Plugin  Folder/" + GetParam() + ".dylib";
    const char* args =
        "file=\"Results  Folder/plugin output.h5\" compress=9 \"bare  argument; v1\"";
    ASSERT_EQ(swmm_plugin_set(engine, name.c_str(), args), 0);
    char library[4096] = {}, editable_args[4096] = {};
    ASSERT_EQ(
        swmm_plugin_get(engine, 0, library, sizeof(library), editable_args, sizeof(editable_args)),
        0);
    EXPECT_EQ(std::string(library), name);
    EXPECT_STREQ(editable_args, args);
    // The GUI reads these fields and writes them back when applying edits.
    ASSERT_EQ(swmm_plugin_set(engine, library, editable_args), 0);
    ASSERT_NO_FATAL_FAILURE(saveAndReopen());
    const auto& specs = static_cast<openswmm::SWMMEngine*>(reopened)->context().plugin_specs;
    ASSERT_EQ(specs.size(), 1u);
    EXPECT_EQ(specs[0].path, name);
    ASSERT_EQ(specs[0].init_args.size(), 3u);
    EXPECT_EQ(specs[0].init_args[0], "file=\"Results  Folder/plugin output.h5\"");
    EXPECT_EQ(specs[0].init_args[1], "compress=9");
    EXPECT_EQ(specs[0].init_args[2], "bare  argument; v1");
}

TEST_F(InpPathSpacesTest, UnquotedMeshPathPreservesRepeatedSpaces) {
    if (!has2D())
        GTEST_SKIP() << "engine built without 2D";
    const std::string name = "Mesh  Folder/My  Model.2dm";
    file(name, mesh);
    ASSERT_NO_FATAL_FAILURE(open("[2D_MESH_FILE]\n  FILE\t" + name + " ; comment\n"));
    EXPECT_EQ(path(engine, SWMM_FILE_MESH_2D), name);
}

TEST_F(InpPathSpacesTest, SpacedCsvMeshReferencePreservesQuotedPath) {
    if (!has2D())
        GTEST_SKIP() << "engine built without 2D";
    const std::string name = "Mesh  Folder/My  Model.2dm";
    file(name, mesh);
    ASSERT_NO_FATAL_FAILURE(open("[2D_MESH_FILE]\nFILE , \"" + name + "\"\n"));
    EXPECT_EQ(path(engine, SWMM_FILE_MESH_2D), name);
}

INSTANTIATE_TEST_SUITE_P(PathNames, InpPathSpacesTest,
                         ::testing::Values("My Model", "My  Model", "My; Model"));
} // namespace
