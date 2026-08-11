/**
 * @file test_postparse_resolver.cpp
 * @brief Slice IO-3 verification — every FilePathPair slot has its
 *        `.absolute` populated against the .inp directory after the
 *        post-parse resolver runs.
 *
 * @details Pins the contract documented in
 *          openswmm.gui/docs/IO_PORTABILITY_PLAN.md §3.2 (engine read
 *          lifecycle). Covers:
 *
 *            - Relative tokens resolved against the .inp anchor.
 *            - Absolute tokens preserved unchanged.
 *            - Parent-traversal (`../shared/...`) tokens flattened.
 *            - Empty slots produce empty `.absolute`.
 *            - Timeseries `path:column` form: `.absolute` keeps the
 *              column suffix so the loader strips it the same way it
 *              strips it from `.original` (the convention from
 *              PathResolver / IO_PORTABILITY_PLAN §3.4.2).
 *            - Empty anchor leaves `.original` untouched and yields a
 *              lexically-normalised `.absolute`.
 *
 *          Per the project's CLAUDE.md §4.1 directive these tests never
 *          touch the filesystem — they exercise the resolver as a pure
 *          string transformation against in-memory state.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include <gtest/gtest.h>

#include "../../src/engine/core/SimulationContext.hpp"
#include "../../src/engine/data/TableData.hpp"
#include "../../src/engine/input/PostParseResolver.hpp"

using openswmm::SimulationContext;
using openswmm::TableType;
using openswmm::FileMode;
using openswmm::input::resolve_external_file_slots;

namespace {

constexpr const char* kAnchor = "/proj/sub";

SimulationContext makeContextWithSlots() {
    SimulationContext ctx;
    ctx.inp_file_path = "/proj/sub/model.inp";

    // [FILES] slots — mix of relative, absolute, parent-traversal.
    ctx.files.rainfall_mode = FileMode::USE;
    ctx.files.rainfall_path = std::string("rain.dat");                  // sibling
    ctx.files.runoff_mode   = FileMode::SAVE;
    ctx.files.runoff_path   = std::string("./out/runoff.bin");          // subdir
    ctx.files.rdii_mode     = FileMode::USE;
    ctx.files.rdii_path     = std::string("../shared/rdii.dat");        // parent
    ctx.files.inflows_path  = std::string("/abs/inflows.dat");          // absolute
    ctx.files.outflows_path = std::string("outflows.dat");

    openswmm::HotstartSaveEntry e;
    e.path = std::string("hot/save_0.hsf");
    e.datetime = 0.0;
    ctx.files.hotstart_saves.push_back(e);
    ctx.files.hotstart_use_path = std::string("../in/restart.hsf");

    // Raingage data file — vector slot.
    ctx.gages.file_path.emplace_back(std::string("gage_data/g1.dat"));
    ctx.gages.file_path.emplace_back(std::string());   // gage with no file
    ctx.gages.file_path.emplace_back(std::string("/abs/g3.dat"));

    // Climate file.
    ctx.options.temp_file = std::string("climate/temp.dat");

    // Timeseries — file-backed series, including the `:column` form.
    // ctx.tables is the name authority (kind-aware find_timeseries).
    int t1 = ctx.tables.add("TS_PLAIN", TableType::TIMESERIES);
    ctx.tables[t1].file_path = std::string("rain.csv");
    int t2 = ctx.tables.add("TS_COL", TableType::TIMESERIES);
    ctx.tables[t2].file_path = std::string("rainfall.csv:East_Gage");
    int t3 = ctx.tables.add("TS_INLINE", TableType::TIMESERIES);  // empty
    (void)t3;

    return ctx;
}

} // namespace

// ---------------------------------------------------------------------------
// FilesSpec slots
// ---------------------------------------------------------------------------

TEST(PostParseResolverIO3, RelativeRainfallAnchoredAgainstInpDir) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.files.rainfall_path.original, "rain.dat");
    EXPECT_EQ(ctx.files.rainfall_path.absolute, "/proj/sub/rain.dat");
}

TEST(PostParseResolverIO3, SubdirRunoffResolved) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.files.runoff_path.absolute, "/proj/sub/out/runoff.bin");
}

TEST(PostParseResolverIO3, ParentTraversalRdiiCollapsed) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.files.rdii_path.absolute, "/proj/shared/rdii.dat");
}

TEST(PostParseResolverIO3, AbsoluteInflowsPreserved) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.files.inflows_path.absolute, "/abs/inflows.dat");
    EXPECT_EQ(ctx.files.inflows_path.original, "/abs/inflows.dat");
}

TEST(PostParseResolverIO3, HotstartUsePathTraversalResolved) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.files.hotstart_use_path.absolute, "/proj/in/restart.hsf");
}

TEST(PostParseResolverIO3, HotstartSaveSlotResolved) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    ASSERT_EQ(ctx.files.hotstart_saves.size(), 1u);
    EXPECT_EQ(ctx.files.hotstart_saves.front().path.absolute,
              "/proj/sub/hot/save_0.hsf");
}

// ---------------------------------------------------------------------------
// Gage / climate / timeseries slots
// ---------------------------------------------------------------------------

TEST(PostParseResolverIO3, GageDataFileVectorResolvedPerEntry) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.gages.file_path[0].absolute, "/proj/sub/gage_data/g1.dat");
    EXPECT_EQ(ctx.gages.file_path[1].absolute, "");   // empty stays empty
    EXPECT_EQ(ctx.gages.file_path[2].absolute, "/abs/g3.dat");
}

TEST(PostParseResolverIO3, ClimateTempFileResolved) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.options.temp_file.absolute, "/proj/sub/climate/temp.dat");
}

TEST(PostParseResolverIO3, TimeseriesPlainFileResolved) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    int idx = ctx.find_timeseries("TS_PLAIN");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(ctx.tables[idx].file_path.absolute, "/proj/sub/rain.csv");
}

TEST(PostParseResolverIO3, TimeseriesColumnSuffixPreservedInAbsolute) {
    // The `:column` decorator stays attached to `.absolute` because the
    // resolver treats the whole token as opaque; the loader strips the
    // decorator the same way it strips it from `.original`.
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    int idx = ctx.find_timeseries("TS_COL");
    ASSERT_GE(idx, 0);
    EXPECT_EQ(ctx.tables[idx].file_path.original,
              "rainfall.csv:East_Gage");
    EXPECT_EQ(ctx.tables[idx].file_path.absolute,
              "/proj/sub/rainfall.csv:East_Gage");
}

TEST(PostParseResolverIO3, InlineTimeseriesUnchanged) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    int idx = ctx.find_timeseries("TS_INLINE");
    ASSERT_GE(idx, 0);
    EXPECT_TRUE(ctx.tables[idx].file_path.original.empty());
    EXPECT_TRUE(ctx.tables[idx].file_path.absolute.empty());
}

// ---------------------------------------------------------------------------
// Edge cases
// ---------------------------------------------------------------------------

TEST(PostParseResolverIO3, EmptyAnchorLeavesAbsoluteAsNormalisedToken) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, "");
    // With empty anchor, resolveRelative returns the token lexically
    // normalised; "./out/runoff.bin" becomes "out/runoff.bin".
    EXPECT_EQ(ctx.files.runoff_path.original, "./out/runoff.bin");
    EXPECT_NE(ctx.files.runoff_path.absolute.find("out"),
              std::string::npos);
    EXPECT_NE(ctx.files.runoff_path.absolute.find("runoff.bin"),
              std::string::npos);
}

TEST(PostParseResolverIO3, ReresolvingClearsThenRepopulates) {
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);

    // Clear the token, re-run; .absolute must be cleared too.
    ctx.files.rainfall_path = std::string{};
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_TRUE(ctx.files.rainfall_path.absolute.empty());

    // Set a new token, re-run; .absolute reflects the new value.
    ctx.files.rainfall_path = std::string("new_rain.dat");
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_EQ(ctx.files.rainfall_path.absolute, "/proj/sub/new_rain.dat");
}

TEST(PostParseResolverIO3, AssignToOriginalClearsCachedAbsolute) {
    // FilePathPair::operator=(string) is documented to clear .absolute
    // because the original token just changed. This pins that contract.
    auto ctx = makeContextWithSlots();
    resolve_external_file_slots(ctx, kAnchor);
    EXPECT_FALSE(ctx.files.rainfall_path.absolute.empty());

    ctx.files.rainfall_path = std::string("changed.dat");
    EXPECT_TRUE(ctx.files.rainfall_path.absolute.empty());
}
