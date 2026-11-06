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
 * @file      test_inp_writer_saveas_paths.cpp
 * @brief     Save-As re-anchoring for every path-bearing section, end to end.
 *
 * @details test_inp_writer_relative_paths.cpp builds a SimulationContext by
 *          hand and covers `[FILES]` only. This file drives the REAL pipeline
 *          the GUI drives — write a deck, `swmm_engine_open` it (which runs
 *          resolve_external_file_slots), then `swmm_model_write` into a
 *          different directory — and asserts every external reference still
 *          points at the file it named before the move.
 *
 *          The layout is deliberately adversarial: the data files live in a
 *          SIBLING directory of the source `.inp`, and the destination is a
 *          THIRD directory. A token that is merely copied through, or rebased
 *          against the wrong anchor, cannot survive that.
 *
 *          Per CLAUDE.md §4.1 every artefact is written under
 *          tests/unit/engine/data/saveas_paths/ so a reviewer can open the
 *          decks directly. Files are overwritten on each run.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_model.h>

namespace fs = std::filesystem;

namespace {

// Reviewable output root (CLAUDE.md §4.1), located by walking up to the
// engine source tree the same way test_inp_writer_relative_paths.cpp does.
/**
 * @brief Locate and create reviewable Save-As path evidence beneath the source tree.
 *
 * Walks upward at most eight times seeking tests/unit/engine/data, then creates the evidence
 * directory even if the marker was never found. Filesystem exceptions propagate; does not change
 * current directory.
 * @return Owned tests/unit/engine/data/saveas_paths path below the discovered ancestor.
 */
fs::path testDataDir() {
    fs::path here = fs::current_path();
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path())
            here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/saveas_paths";
    fs::create_directories(dir);
    return dir;
}

/**
 * @brief Write fixture content to a caller-selected file.
 *
 * Creates parent directories with the throwing filesystem overload before opening. Uses ordinary
 * text output mode. Open failure is not reported by this helper. Subsequent write/close failures
 * are not checked; the local stream closes at scope exit and no path/content storage is retained.
 * Allocation/filesystem exceptions can propagate.
 *
 * @param[in] p Borrowed destination path; an existing file is truncated/overwritten.
 * @param[in] text Borrowed content copied verbatim through the output stream; no newline is
 *     appended.
 */
void write_file(const fs::path& p, const std::string& text) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p);
    f << text;
}

/**
 * @brief Collect an entire fixture file into owned text.
 *
 * Uses ordinary text mode and its platform newline rules. The local stream closes on return; no
 * model state is touched and no borrowed buffer is returned. Default stream exception settings
 * apply; allocation exceptions may propagate.
 *
 * @param[in] p Borrowed input filename used for this synchronous open/read.
 * @return Owned string collected from the stream; empty can mean an empty file or failure to open.
 *     Partial read failures have no separate status.
 */
std::string read_file(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// First line of `text` containing `needle`, restricted to the [SECTION] block
// so that e.g. "RAINFALL" cannot match the [OPTIONS] IGNORE_RAINFALL line.
/**
 * @brief Find the first substring-matching line inside a raw section block.
 *
 * Uses the first header occurrence anywhere and ends at the next newline followed by [. Does not
 * parse full input grammar or exclude the header itself; caller supplies known writer output.
 *
 * @param[in] text Borrowed complete written input text.
 * @param[in] section Raw case-sensitive section-header substring to locate.
 * @param[in] needle Case-sensitive line substring; empty matches the header line.
 * @return Owned first matching line, or empty when section/needle is absent.
 */
std::string line_in_section(const std::string& text, const std::string& section,
                            const std::string& needle) {
    const auto begin = text.find(section);
    if (begin == std::string::npos)
        return {};
    const auto end = text.find("\n[", begin + 1);
    const std::string block =
        text.substr(begin, end == std::string::npos ? std::string::npos : end - begin);
    std::istringstream is(block);
    std::string line;
    while (std::getline(is, line))
        if (line.find(needle) != std::string::npos)
            return line;
    return {};
}

// The text between the first pair of double quotes on `line`.
/**
 * @brief Extract text between the first two literal double quotes.
 *
 * Does not interpret escape sequences or distinguish an empty quoted value from missing quotes.
 *
 * @param[in] line Borrowed row text.
 * @return Owned unescaped quoted content, or empty if either quote is absent.
 */
std::string quoted(const std::string& line) {
    const auto a = line.find('"');
    if (a == std::string::npos)
        return {};
    const auto b = line.find('"', a + 1);
    if (b == std::string::npos)
        return {};
    return line.substr(a + 1, b - a - 1);
}

const char* kRainRows = "STA01 2007 01 01 00 00 0.10\n"
                        "STA01 2007 01 01 01 00 0.20\n"
                        "STA01 2007 01 01 02 00 0.00\n";

const char* kSeriesRows = "01/01/2007 00:00 1.0\n"
                          "01/01/2007 01:00 2.0\n"
                          "01/01/2007 02:00 3.0\n";

// One gage on a sibling-directory rain file, one FILE-backed timeseries on a
// sibling-directory series file, plus the [FILES] interface slots.
/**
 * @brief Build the Save-As fixture referring to sibling-directory source files.
 *
 * Returns literal text only; setup separately creates referenced fixture files. Does not parse
 * paths or invoke the writer.
 * @return Owned CFS deck with ../data rain/series/interface paths and one subcatchment/conduit.
 */
std::string deck() {
    return R"INP([TITLE]
Save-As path re-anchoring probe

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2007
START_TIME           00:00:00
END_DATE             01/01/2007
END_TIME             03:00:00
REPORT_STEP          00:15:00
WET_STEP             00:15:00
DRY_STEP             00:15:00
ROUTING_STEP         0:00:30

[RAINGAGES]
;;Name  Format     Intvl  SCF   Source
RG1     INTENSITY  1:00   1.0   FILE "../data/rain.dat" STA01 IN

[TIMESERIES]
;;Name  Source
TS1     FILE "../data/series.dat"

[FILES]
USE INFLOWS "../data/inflows.txt"

[JUNCTIONS]
;;Name  Elev  MaxDepth  InitDepth  SurDepth  Aponded
J1      10    5         0          0         0

[OUTFALLS]
;;Name  Elev  Type  StageData  Gated
O1      0     FREE             NO

[CONDUITS]
;;Name  Node1  Node2  Length  Roughness  In-Off  Out-Off  InitFlow  MaxFlow
C1      J1     O1     400     0.01       0       0        0         0

[XSECTIONS]
;;Link  Shape     Geom1  Geom2  Geom3  Geom4  Barrels
C1      CIRCULAR  3.0    0      0      0      1

[SUBCATCHMENTS]
;;Name  Raingage  Outlet  Area  %Imperv  Width  %Slope  CurbLen
S1      RG1       J1      10    100      500    0.5     0

[SUBAREAS]
;;Subcat  N-Imperv  N-Perv  S-Imperv  S-Perv  PctZero  RouteTo
S1        0.01      0.1     0.0       0.0     100      OUTLET

[INFILTRATION]
;;Subcat  MaxRate  MinRate  Decay  DryTime  MaxInfil
S1        3.0      0.5      4      7        0
)INP";
}

class SaveAsPathsTest : public ::testing::Test {
protected:
    fs::path root_, src_dir_, data_dir_, dst_dir_;
    SWMM_Engine eng_ = nullptr;

    /**
     * @brief Create source/data/saved evidence folders and populate the Save-As fixture.
     *
     * Uses testDataDir, creates model/data/saved siblings, overwrites rain.dat/series.dat/empty
     * inflows.txt and source model.inp. Files remain for user review and can be reused by later cases;
     * stream helpers define IO assertion behavior.
     */
    void SetUp() override {
        root_ = testDataDir();
        src_dir_ = root_ / "model";
        data_dir_ = root_ / "data";
        dst_dir_ = root_ / "saved";
        fs::create_directories(src_dir_);
        fs::create_directories(data_dir_);
        fs::create_directories(dst_dir_);

        write_file(data_dir_ / "rain.dat", kRainRows);
        write_file(data_dir_ / "series.dat", kSeriesRows);
        write_file(data_dir_ / "inflows.txt", "");
        write_file(src_dir_ / "model.inp", deck());
    }

    /**
     * @brief Close and destroy a Save-As engine when allocated.
     *
     * Ignores close status and leaves the member value uncleared at fixture end. Retains all
     * source/saved evidence artifacts.
     */
    void TearDown() override {
        if (eng_) {
            swmm_engine_close(eng_);
            swmm_engine_destroy(eng_);
        }
    }

    // Open the source deck and save it into dst_dir_; returns the saved text.
    /**
     * @brief Open the source model and save into the destination sibling directory.
     *
     * Requires no previous live eng_ because allocation replaces its slot without release. Uses
     * nonfatal create/open/write assertions, retains engine until teardown, and does not initialize/run
     * the model.
     *
     * @param[in] out_name Raw filename/path appended below dst_dir_; matching output may be
     *     overwritten.
     * @return Owned saved text read through read_file, including possible old/partial text when writer
     *     status rejects.
     */
    std::string saveAs(const std::string& out_name) {
        eng_ = swmm_engine_create();
        EXPECT_NE(eng_, nullptr);
        const fs::path inp = src_dir_ / "model.inp";
        EXPECT_EQ(swmm_engine_open(eng_, inp.string().c_str(), "", "", nullptr), 0);
        const fs::path out = dst_dir_ / out_name;
        EXPECT_EQ(swmm_model_write(eng_, out.string().c_str()), 0);
        return read_file(out);
    }

    // A token is correctly re-anchored when resolving it against the
    // DESTINATION directory lands on the same file it named originally.
    /**
     * @brief Assert a written relative path resolves to the original source artifact.
     *
     * Fatally requires nonempty token, nonfatally rejects absolute paths, and compares
     * weakly_canonical(dst_dir_/token) with weakly_canonical(expected). Does not alter files or require
     * existence beyond canonical path rules; filesystem exceptions may propagate.
     *
     * @param[in] token Borrowed path token already extracted/unquoted from writer output.
     * @param[in] expected Borrowed expected source artifact path.
     */
    void expectResolvesTo(const std::string& token, const fs::path& expected) {
        ASSERT_FALSE(token.empty());
        EXPECT_FALSE(fs::path(token).is_absolute()) << "token leaked an absolute path: " << token;
        const fs::path resolved = fs::weakly_canonical(dst_dir_ / fs::path(token));
        EXPECT_EQ(resolved, fs::weakly_canonical(expected))
            << "token '" << token << "' does not resolve back to the source file";
    }
};

} // namespace

// ---------------------------------------------------------------------------
// Every external reference survives a move to a third directory
// ---------------------------------------------------------------------------

TEST_F(SaveAsPathsTest, RaingageFileReanchored) {
    const std::string text = saveAs("raingage.inp");
    const std::string tok = quoted(line_in_section(text, "[RAINGAGES]", "RG1"));
    expectResolvesTo(tok, data_dir_ / "rain.dat");
}

TEST_F(SaveAsPathsTest, TimeseriesFileReanchored) {
    const std::string text = saveAs("timeseries.inp");
    const std::string tok = quoted(line_in_section(text, "[TIMESERIES]", "TS1"));
    expectResolvesTo(tok, data_dir_ / "series.dat");
}

TEST_F(SaveAsPathsTest, FilesSectionReanchored) {
    const std::string text = saveAs("files.inp");
    const std::string tok = quoted(line_in_section(text, "[FILES]", "INFLOWS"));
    expectResolvesTo(tok, data_dir_ / "inflows.txt");
}

// The re-anchored tokens must be `..`-relative here: source and destination are
// siblings, so a token that came through unchanged would silently name a
// non-existent file inside the destination folder.
TEST_F(SaveAsPathsTest, ReanchoredTokensAreParentRelative) {
    const std::string text = saveAs("parent_relative.inp");
    for (const auto& [section, needle] :
         {std::pair<const char*, const char*>{"[RAINGAGES]", "RG1"},
          std::pair<const char*, const char*>{"[TIMESERIES]", "TS1"},
          std::pair<const char*, const char*>{"[FILES]", "INFLOWS"}}) {
        const std::string tok = quoted(line_in_section(text, section, needle));
        ASSERT_FALSE(tok.empty()) << section;
        EXPECT_EQ(tok.rfind("..", 0), 0u) << section << " token was not re-anchored: " << tok;
    }
}

// The saved deck must be loadable and still see its rainfall — the whole point.
TEST_F(SaveAsPathsTest, SavedDeckStillLoadsItsRainfall) {
    saveAs("reopen.inp");

    SWMM_Engine reopened = swmm_engine_create();
    ASSERT_NE(reopened, nullptr);
    const fs::path out = dst_dir_ / "reopen.inp";
    EXPECT_EQ(swmm_engine_open(reopened, out.string().c_str(), "", "", nullptr), 0)
        << "saved deck could not be reopened — a reference did not survive";
    swmm_engine_close(reopened);
    swmm_engine_destroy(reopened);
}

// ---------------------------------------------------------------------------
// WRITE_ABSOLUTE_PATHS is a real option, not an extension key
// ---------------------------------------------------------------------------

TEST_F(SaveAsPathsTest, WriteAbsolutePathsExtKeySetsTheOptionNotExtOptions) {
    eng_ = swmm_engine_create();
    ASSERT_NE(eng_, nullptr);
    const fs::path inp = src_dir_ / "model.inp";
    ASSERT_EQ(swmm_engine_open(eng_, inp.string().c_str(), "", "", nullptr), 0);

    ASSERT_EQ(swmm_options_set_ext(eng_, "WRITE_ABSOLUTE_PATHS", "YES"), 0);

    const fs::path out = dst_dir_ / "abs_optout.inp";
    ASSERT_EQ(swmm_model_write(eng_, out.string().c_str()), 0);
    const std::string text = read_file(out);

    // The flag must actually take effect on THIS save. Previously it landed in
    // ext_options, so the deck said YES while its own paths were written
    // relative — the next open then behaved differently from the save.
    const std::string tok = quoted(line_in_section(text, "[RAINGAGES]", "RG1"));
    ASSERT_FALSE(tok.empty());
    EXPECT_TRUE(fs::path(tok).is_absolute()) << "opt-out did not reach the writer; token: " << tok;
    EXPECT_NE(text.find("WRITE_ABSOLUTE_PATHS"), std::string::npos);

    // ...and exactly once — a stray ext_options copy would duplicate the line.
    const auto first = text.find("WRITE_ABSOLUTE_PATHS");
    EXPECT_EQ(text.find("WRITE_ABSOLUTE_PATHS", first + 1), std::string::npos)
        << "WRITE_ABSOLUTE_PATHS emitted twice (ext_options copy not erased)";
}
