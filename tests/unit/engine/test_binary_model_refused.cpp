// SPDX-License-Identifier: Apache-2.0
// The built-in .inp reader names the input plugin of an HDF5 or GeoPackage model.

/**
 * @file      test_binary_model_refused.cpp
 * @brief     Opening an HDF5 or GeoPackage model without its input plugin fails at once.
 *
 * An HDF5 model stores its whole .inp text inside the file, so the .inp reader
 * used to parse that text and fail only when the last section ran into binary
 * bytes, with a message that blamed that section. The reader now checks the
 * file signature and names the plugin to use.
 *
 * Files go to OPENSWMM_BINARY_MODEL_TEST_OUTPUT, or binary_model_refused_output
 * under the working directory.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <openswmm/engine/openswmm_engine.h>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
namespace fs = std::filesystem;

fs::path outputDir() {
    const char* root = std::getenv("OPENSWMM_BINARY_MODEL_TEST_OUTPUT");
    fs::path dir = root && *root ? fs::path(root) : fs::path("binary_model_refused_output");
    fs::create_directories(dir);
    return dir;
}

/** A file with `signature` first, then model text, as an HDF5 model carries it. */
fs::path writeModel(const std::string& name, const std::string& signature) {
    const fs::path path = outputDir() / name;
    std::ofstream out(path, std::ios::binary);
    out << signature << "\n[OPTIONS]\nFLOW_UNITS CMS\n[JUNCTIONS]\nJ1 0 1\n";
    return path;
}

/** Open `path` with the built-in reader; returns the code and the last error message. */
std::pair<int, std::string> openWithoutPlugin(const fs::path& path) {
    SWMM_Engine engine = swmm_engine_create();
    const std::string inp = path.string();
    const std::string rpt = fs::path(path).replace_extension(".rpt").string();
    const int rc = swmm_engine_open(engine, inp.c_str(), rpt.c_str(), "", nullptr);
    const char* message = swmm_get_last_error_msg(engine);
    std::pair<int, std::string> result{rc, message ? message : ""};
    swmm_engine_destroy(engine);
    return result;
}
} // namespace

TEST(BinaryModelRefused, Hdf5ModelNamesItsPlugin) {
    const auto [rc, message] =
        openWithoutPlugin(writeModel("model.h5", std::string("\x89HDF\r\n\x1a\n", 8)));
    EXPECT_EQ(rc, SWMM_ERR_INPFILE);
    EXPECT_NE(message.find("is an HDF5 file"), std::string::npos) << message;
    EXPECT_NE(message.find("org.hydrocouple.openswmm.plugins.hdf5"), std::string::npos) << message;
}

TEST(BinaryModelRefused, GeoPackageModelNamesItsPlugin) {
    const auto [rc, message] =
        openWithoutPlugin(writeModel("model.gpkg", std::string("SQLite format 3\0", 16)));
    EXPECT_EQ(rc, SWMM_ERR_INPFILE);
    EXPECT_NE(message.find("GeoPackage"), std::string::npos) << message;
    EXPECT_NE(message.find("org.hydrocouple.openswmm.plugins.geopackage"), std::string::npos)
        << message;
}

TEST(BinaryModelRefused, TextThatOnlyMentionsASignatureIsParsed) {
    // "SQLite format 3" without its terminating NUL is text, not a database header.
    const auto [rc, message] =
        openWithoutPlugin(writeModel("mentions.inp", "SQLite format 3 notes"));
    EXPECT_EQ(message.find("open it with the input plugin"), std::string::npos) << message;
    (void)rc;
}
