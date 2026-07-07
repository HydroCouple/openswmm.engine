/**
 * @file test_inp_writer_relative_paths.cpp
 * @brief Slice IO-4 verification — InpWriter always emits external-file
 *        paths relative to the destination directory.
 *
 * @details Covers the contract documented in
 *          openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.2 (engine write
 *          lifecycle). Cases:
 *            - Absolute slot, write to sibling dir → emits relative.
 *            - Parent-traversal needed → emits "../...".
 *            - Cross-volume → falls back to absolute + warning surfaced.
 *            - WRITE_ABSOLUTE_PATHS=true → opt-out, absolute everywhere.
 *            - Programmatic model with only `.original` (no resolver pass)
 *              → relative tokens pass through; absolute tokens get rebased.
 *
 *          Per the project CLAUDE.md §4.1 transparent-IO directive, every
 *          .inp produced by this test is written under
 *          tests/unit/engine/data/io4_roundtrip/ so a reviewer can open
 *          the file directly and compare against expectations. Files are
 *          overwritten on each run; no temporary directory is used.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/core/InpWriter.hpp"
#include "../../src/engine/input/PostParseResolver.hpp"

namespace fs = std::filesystem;
using openswmm::FileMode;
using openswmm::SimulationContext;
using openswmm::TableType;
using openswmm::input::resolve_external_file_slots;

namespace {

// Reviewable test-output directory (CLAUDE.md §4.1). Resolved at runtime
// relative to the test binary's CWD, then anchored to the source tree's
// tests/unit/engine/data/io4_roundtrip/ folder.
fs::path testDataDir() {
    fs::path here = fs::current_path();
    // Walk up looking for the openswmm.engine root marker.
    for (int i = 0; i < 8 && !fs::exists(here / "tests/unit/engine/data"); ++i) {
        if (here.has_parent_path()) here = here.parent_path();
    }
    fs::path dir = here / "tests/unit/engine/data/io4_roundtrip";
    fs::create_directories(dir);
    return dir;
}

std::string readFile(const fs::path& p) {
    std::ifstream in(p);
    std::stringstream ss; ss << in.rdbuf();
    return ss.str();
}

// Build a context whose external-file slots all have `.absolute` set (as
// if the resolver pass had run) to a known anchor `/proj/sub`. The
// destination directory is chosen per-test to exercise the rebase math.
SimulationContext makeResolvedContext() {
    SimulationContext ctx;
    // On Windows a rootless POSIX-absolute anchor like "/proj/sub" lives on a
    // different "volume" than the drive-lettered output directory, so the
    // rebase degrades to absolute and the relative-form assertions fail.
    // Anchor the synthetic paths to the destination's root (e.g. "C:") so the
    // contract holds on every platform. On POSIX root_name() is empty, leaving
    // the paths unchanged.
    const std::string root = fs::current_path().root_name().string();
    ctx.inp_file_path = root + "/proj/sub/model.inp";
    ctx.files.rainfall_mode = FileMode::USE;
    ctx.files.rainfall_path = std::string("rain.dat");
    ctx.files.runoff_mode   = FileMode::SAVE;
    ctx.files.runoff_path   = std::string("./out/runoff.bin");
    ctx.files.rdii_mode     = FileMode::USE;
    ctx.files.rdii_path     = std::string("../shared/rdii.dat");
    ctx.files.inflows_path  = std::string(root + "/abs/inflows.dat");
    ctx.files.outflows_path = std::string("outflows.dat");
    ctx.files.hotstart_use_path = std::string("../in/restart.hsf");

    openswmm::HotstartSaveEntry e;
    e.path = std::string("hot/save_0.hsf");
    ctx.files.hotstart_saves.push_back(e);

    // Run the resolver to populate `.absolute` so the writer rebases off
    // those values (matches the open→save round trip).
    resolve_external_file_slots(ctx, root + "/proj/sub");
    return ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// FILES block — happy path: rebase against destination directory
// ---------------------------------------------------------------------------

TEST(InpWriterRelativePaths, FilesBlockRebasedReviewable) {
    auto ctx = makeResolvedContext();
    const fs::path dst = testDataDir() / "files_rebased.inp";
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, dst.string(),
                                                  &warnings), 0);

    const std::string content = readFile(dst);
    const auto files_begin = content.find("[FILES]");
    ASSERT_NE(files_begin, std::string::npos);

    // Restrict the line scan to the [FILES] block — otherwise needles like
    // "RAINFALL"/"RDII" would match the [OPTIONS] IGNORE_RAINFALL / IGNORE_RDII
    // lines that appear earlier in the file.
    const auto files_end = content.find("\n[", files_begin + 1);
    const std::string files_block = content.substr(
        files_begin,
        files_end == std::string::npos ? std::string::npos : files_end - files_begin);

    // dst_dir is `tests/unit/engine/data/io4_roundtrip` (an absolute
    // path on the test machine). Every `.absolute` was set against
    // `/proj/sub`. The relative form for `/proj/sub/rain.dat` against
    // dst_dir is the relative form between them — a many-`..` token.
    // Assertion: every token is *relative* (no leading `/`).
    //
    // We can't predict the exact `..` count because dst_dir is
    // machine-dependent. We assert structural properties instead:
    auto line_for = [&](const char* needle) -> std::string {
        std::istringstream is(files_block);
        std::string line;
        while (std::getline(is, line))
            if (line.find(needle) != std::string::npos) return line;
        return {};
    };

    const std::string rain = line_for("RAINFALL");
    const std::string rdii = line_for("RDII");
    ASSERT_FALSE(rain.empty());
    ASSERT_FALSE(rdii.empty());
    // Extract the quoted token so we can test its leading form. The rebased
    // relative path legitimately *ends* in ".../proj/sub/rain.dat", so a bare
    // substring search for "/proj/sub" would false-positive; instead assert
    // the token is relative (begins with "..", hence no leading '/').
    auto quoted = [](const std::string& line) -> std::string {
        auto a = line.find('"');
        auto b = (a == std::string::npos) ? std::string::npos : line.find('"', a + 1);
        return (b == std::string::npos) ? std::string() : line.substr(a + 1, b - a - 1);
    };
    const std::string rain_tok = quoted(rain);
    const std::string rdii_tok = quoted(rdii);
    ASSERT_FALSE(rain_tok.empty());
    ASSERT_FALSE(rdii_tok.empty());
    // Token must be rebased to a relative form (not a bare absolute path).
    EXPECT_NE(rain_tok[0], '/') << "rainfall token leaked an absolute path: " << rain_tok;
    EXPECT_NE(rdii_tok[0], '/') << "rdii token leaked an absolute path: " << rdii_tok;
    EXPECT_EQ(rain_tok.rfind("..", 0), 0u) << "rainfall not rebased: " << rain_tok;
    EXPECT_EQ(rdii_tok.rfind("..", 0), 0u) << "rdii not rebased: " << rdii_tok;
    // And SHOULD reference the expected file name.
    EXPECT_NE(rain_tok.find("rain.dat"), std::string::npos);
    EXPECT_NE(rdii_tok.find("rdii.dat"), std::string::npos);
}

// ---------------------------------------------------------------------------
// Cross-volume slot falls back to absolute and surfaces a warning
// ---------------------------------------------------------------------------

TEST(InpWriterRelativePaths, CrossVolumeFallbackProducesWarning) {
    SimulationContext ctx;
    ctx.files.rainfall_mode = FileMode::USE;

    const fs::path dst = testDataDir() / "cross_volume.inp";

    // Pick a Windows drive letter guaranteed DIFFERENT from the destination's
    // own volume, so the cross-volume fallback fires on every platform and CI
    // runner. The original test hardcoded "D:" as the foreign drive, but the
    // Windows CI workspace itself lives on D:\ — there the slot was the *same*
    // volume, the writer correctly relativised it, and the cross-volume branch
    // never ran (no warning, token rebased). On POSIX the destination has no
    // root_name, so any drive letter is cross-volume; on Windows we avoid
    // colliding with dst's real drive.
    const std::string dst_root = dst.root_name().string();  // "" on POSIX, "D:" on Win
    const char foreign_drive =
        (!dst_root.empty() && (dst_root[0] == 'Z' || dst_root[0] == 'z')) ? 'Y' : 'Z';
    const std::string foreign_token =
        std::string(1, foreign_drive) + ":/data/rain.dat";
    ctx.files.rainfall_path.original = foreign_token;
    ctx.files.rainfall_path.absolute = foreign_token;

    // The writer takes one `path` parameter which is BOTH the file we open()
    // AND the anchor for the rebase. The slot is on a foreign volume relative
    // to that anchor, so the rebase is impossible: the writer must emit the
    // absolute form verbatim and surface a warning.
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, dst.string(),
                                                  &warnings), 0);
    const std::string content = readFile(dst);
    // The foreign-volume token *should still appear* — cross-volume rebase is
    // impossible against the destination anchor, so the writer emits the
    // absolute form.
    EXPECT_NE(content.find(foreign_token), std::string::npos)
        << "cross-volume token was not preserved";
    // A warning should have been surfaced.
    EXPECT_FALSE(warnings.empty());
    bool found_warn = false;
    for (const auto& w : warnings) {
        if (w.find("volume") != std::string::npos
            || w.find("relative") != std::string::npos) {
            found_warn = true; break;
        }
    }
    EXPECT_TRUE(found_warn) << "expected a volume/relative-form warning";
}

// ---------------------------------------------------------------------------
// WRITE_ABSOLUTE_PATHS opt-out bypasses rebase
// ---------------------------------------------------------------------------

TEST(InpWriterRelativePaths, WriteAbsolutePathsOptOutEmitsAbsoluteVerbatim) {
    auto ctx = makeResolvedContext();
    ctx.options.write_absolute_paths = true;

    const fs::path dst = testDataDir() / "write_absolute_opt_out.inp";
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, dst.string(),
                                                  &warnings), 0);
    const std::string content = readFile(dst);

    // The opt-out emits the absolute (resolved) form. rainfall_path was
    // /proj/sub/rain.dat after resolution; it should appear verbatim.
    EXPECT_NE(content.find("/proj/sub/rain.dat"), std::string::npos);
    EXPECT_NE(content.find("/proj/shared/rdii.dat"), std::string::npos);
    EXPECT_NE(content.find("/proj/in/restart.hsf"), std::string::npos);
    // The [OPTIONS] block should record the opt-out so a re-open re-applies it.
    EXPECT_NE(content.find("WRITE_ABSOLUTE_PATHS"), std::string::npos);
    EXPECT_NE(content.find("YES"), std::string::npos);
    // No warnings for routine opt-out.
    EXPECT_TRUE(warnings.empty());
}

// ---------------------------------------------------------------------------
// Programmatic model: no resolver pass, only `.original` populated
// ---------------------------------------------------------------------------

TEST(InpWriterRelativePaths, ProgrammaticRelativeTokensPassThrough) {
    // A user builds the model in memory and assigns "rain.dat" as a
    // relative token. Without a resolver pass, .absolute is empty.
    // The writer must emit the token unchanged — it has no anchor to
    // rebase against.
    SimulationContext ctx;
    ctx.files.rainfall_mode = FileMode::USE;
    ctx.files.rainfall_path = std::string("rain.dat");  // op= clears .absolute

    const fs::path dst = testDataDir() / "programmatic_relative.inp";
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, dst.string(),
                                                  &warnings), 0);
    const std::string content = readFile(dst);
    EXPECT_NE(content.find("RAINFALL"),  std::string::npos);
    EXPECT_NE(content.find("\"rain.dat\""), std::string::npos);
    EXPECT_TRUE(warnings.empty());
}

TEST(InpWriterRelativePaths, ProgrammaticAbsoluteTokensRebased) {
    // The user assigns "/some/abs/rain.dat" with no resolver run.
    // The writer sees .original is absolute and rebases it against
    // dst_dir.
    SimulationContext ctx;
    ctx.files.rainfall_mode = FileMode::USE;
    ctx.files.rainfall_path = std::string("/some/abs/rain.dat");

    const fs::path dst = testDataDir() / "programmatic_absolute.inp";
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, dst.string(),
                                                  &warnings), 0);
    const std::string content = readFile(dst);
    // dst_dir is io4_roundtrip/ on the local machine. The relative form
    // back to /some/abs/rain.dat traverses many `..` levels — assert
    // structurally rather than literally.
    EXPECT_NE(content.find("rain.dat"), std::string::npos);
    // Should not be the unmodified absolute form (unless we hit the depth
    // cap, in which case we'd see a warning).
    if (warnings.empty()) {
        EXPECT_EQ(content.find("\"/some/abs/rain.dat\""), std::string::npos)
            << "expected rebase, but absolute leaked through";
    }
}

// ---------------------------------------------------------------------------
// Defaults: WRITE_ABSOLUTE_PATHS not emitted when false
// ---------------------------------------------------------------------------

TEST(InpWriterRelativePaths, OptionOmittedWhenDefault) {
    auto ctx = makeResolvedContext();
    ASSERT_FALSE(ctx.options.write_absolute_paths);

    const fs::path dst = testDataDir() / "option_default_omitted.inp";
    std::vector<std::string> warnings;
    ASSERT_EQ(openswmm::inp_writer::writeInpFile(ctx, dst.string(),
                                                  &warnings), 0);
    const std::string content = readFile(dst);
    EXPECT_EQ(content.find("WRITE_ABSOLUTE_PATHS"), std::string::npos);
}
