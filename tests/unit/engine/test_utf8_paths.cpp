/**
 * @file test_utf8_paths.cpp
 * @brief Opening and writing a model whose PATH contains non-ASCII characters.
 *
 * @details Regression gate for issue #7: on Windows the narrow `char*` file
 *          APIs (`std::fopen`, the `const char*` `fstream` constructors)
 *          decode their argument in the process's active ANSI code page, not
 *          UTF-8, so a model under a path such as
 *
 *              …/模拟情景8/模拟情景[8].inp
 *
 *          failed to open with "InputReader: cannot open file". Paths reach the
 *          engine as UTF-8, so every open has to go through
 *          openswmm::io::utf8_path / fopen_utf8 (src/engine/core/FileIO.hpp).
 *
 *          This test is meaningful on POSIX too — it pins that UTF-8 bytes
 *          survive the round trip — but the failure it guards is Windows-only.
 *
 *          The fixture directory is created under tests/unit/engine/data/ and
 *          left in place for review (CLAUDE.md §4.1), never in a temp dir.
 *
 * @ingroup engine_io
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "core/FileIO.hpp"

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

// The reporter's own directory and file names, verbatim — including the
// square brackets, which are legal on NTFS and were incidental to the bug.
constexpr const char* kDirName  = "utf8_paths_out/模拟情景8";
constexpr const char* kFileStem = "模拟情景[8]";

std::string buildModel() {
    return
        "[TITLE]\nUTF-8 path regression (issue #7)\n\n"
        "[OPTIONS]\n"
        "FLOW_UNITS           CFS\n"
        "FLOW_ROUTING         KINWAVE\n"
        "START_DATE           01/01/2026\n"
        "START_TIME           00:00:00\n"
        "END_DATE             01/01/2026\n"
        "END_TIME             01:00:00\n"
        "REPORT_STEP          00:05:00\n"
        "ROUTING_STEP         30\n\n"
        "[JUNCTIONS]\n;;Name Elev MaxDepth\nJ1  10.0  5.0\n\n"
        "[OUTFALLS]\n;;Name Elev Type Gated\nO1   8.0  FREE  NO\n\n"
        "[CONDUITS]\n;;Name From To Length Rough InOff OutOff\n"
        "C1    J1   O1  400    0.01  0     0\n\n"
        "[XSECTIONS]\n;;Link Shape Geom1 Geom2 Geom3 Geom4 Barrels\n"
        "C1     CIRCULAR  1.0  0  0  0  1\n";
}

/// Directory holding the fixture, created fresh. Built through utf8_path so
/// the directory itself is created correctly on Windows.
fs::path outDir() {
    const fs::path d = openswmm::io::utf8_path(kDirName);
    std::error_code ec;
    fs::create_directories(d, ec);
    return d;
}

}  // namespace

// A model under a non-ASCII directory AND with a non-ASCII file name opens,
// runs and reports. Before the fix this failed at swmm_engine_open on Windows
// with error 2 (SWMM_ERR_INPFILE).
TEST(Utf8Paths, ModelUnderNonAsciiPathOpensAndRuns) {
    const fs::path dir = outDir();
    ASSERT_TRUE(fs::exists(dir)) << "could not create the non-ASCII fixture dir";

    const std::string inp = std::string(kDirName) + "/" + kFileStem + ".inp";
    const std::string rpt = std::string(kDirName) + "/" + kFileStem + ".rpt";
    const std::string out = std::string(kDirName) + "/" + kFileStem + ".out";

    {
        std::ofstream f(openswmm::io::utf8_path(inp));
        ASSERT_TRUE(f.is_open()) << "could not write the fixture model";
        f << buildModel();
    }
    ASSERT_TRUE(fs::exists(openswmm::io::utf8_path(inp)));

    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    // The reported failure: open returns 2 and the message is
    // "InputReader: cannot open file '<path>'".
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr),
              SWMM_OK)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), SWMM_OK) << swmm_get_last_error_msg(e);

    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(e, &elapsed), SWMM_OK)
            << swmm_get_last_error_msg(e);
    } while (elapsed > 0.0);

    EXPECT_EQ(swmm_engine_end(e), SWMM_OK);
    EXPECT_EQ(swmm_engine_close(e), SWMM_OK);
    swmm_engine_destroy(e);

    // The .rpt and .out sit beside the model, so they exercise the WRITE side
    // of the same bug — fixing only the reader would leave these empty/absent.
    EXPECT_TRUE(fs::exists(openswmm::io::utf8_path(rpt))) << "no .rpt written";
    EXPECT_GT(fs::file_size(openswmm::io::utf8_path(rpt)), 0u);
}

// utf8_path must not corrupt a path that is plain ASCII, and must round-trip
// the bytes it was given.
TEST(Utf8Paths, Utf8PathRoundTripsBytes) {
    const std::string ascii = "utf8_paths_out/plain_ascii.inp";
    EXPECT_EQ(openswmm::io::utf8_path(ascii).u8string(),
              std::u8string(reinterpret_cast<const char8_t*>(ascii.data()),
                            ascii.size()));

    const std::string cjk = std::string(kDirName) + "/" + kFileStem + ".inp";
    EXPECT_EQ(openswmm::io::utf8_path(cjk).u8string(),
              std::u8string(reinterpret_cast<const char8_t*>(cjk.data()),
                            cjk.size()));
}
