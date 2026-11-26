/*
 * test_warning_error_parity.cpp
 *
 * Verifies that the refactored engine surfaces the warnings/errors that legacy
 * EPA SWMM flags, instead of silently skipping them. Covers:
 *   - Fatal ERROR 209 for an undefined object reference (subcatchment gage /
 *     outlet given as '*') — legacy halts the open; the refactored engine must
 *     too, and the error must reach the .rpt.
 *   - Unknown/skipped input section — warning must reach the .rpt (was
 *     callback-only before).
 *   - Unknown [OPTIONS] keyword — warning must reach the .rpt.
 *   - Runtime WARNING 05/06/07/08 (min slope, dry/routing step clamps, elevation
 *     drop exceeds length) recorded during open.
 *
 * Fixtures live in tests/unit/engine/data/ (WORKING_DIRECTORY of the test) so
 * they are reviewable and committed, not written to a temp dir.
 */

/**
 * @file      test_warning_error_parity.cpp
 * @brief     Tests for warning error parity.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace {

// Open a model and, if the open succeeds, run the full lifecycle so the report
// plugin writes the .rpt (warnings recorded at open time reach the report via
// prepare()/write_summary()). If the open fails, the engine writes the error
// report itself. Returns the open() code; captures the produced .rpt text.
/**
 * @brief Open and optionally run a warning/error fixture, then read its report.
 *
 * Creates and closes a handle but does not destroy it, matching the current test helper lifetime.
 * Steps without a guard if setup succeeds, ignores end/report/close statuses and retains artifacts.
 * No separate report-read status or caller access to the created handle is provided.
 *
 * @param[in] inp Existing input filename.
 * @param[in] rpt Report destination, potentially overwritten.
 * @param[out] rpt_text Replaced with report text; empty/partial on read failure.
 * @return Original swmm_engine_open status only; later initialize/start/step/report failures do not
 *     replace it.
 */
int open_model(const std::string& inp, const std::string& rpt, std::string& rpt_text) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr, nullptr);
    if (rc == SWMM_OK) {
        if (swmm_engine_initialize(e) == SWMM_OK && swmm_engine_start(e, 0) == SWMM_OK) {
            double elapsed = 0.0;
            while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
            }
            swmm_engine_end(e);
            swmm_engine_report(e);
        }
    }
    swmm_engine_close(e);
    swmm_engine_destroy(e);

    std::ifstream f(rpt);
    std::stringstream ss;
    ss << f.rdbuf();
    rpt_text = ss.str();
    return rc;
}

/**
 * @brief Check a case-sensitive text fragment without modifying either string.
 *
 * @param[in] hay Borrowed text to search.
 * @param[in] needle Borrowed substring; an empty substring always matches.
 * @return True when std::string::find locates needle, false otherwise.
 */
bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

} // namespace

// A subcatchment whose gage and outlet are '*' references no defined object.
// Legacy raises a fatal ERROR 209; the refactored engine must fail the open and
// write the error to the .rpt.
TEST(WarningErrorParity, UndefinedOutletIsFatal209) {
    std::string rpt_text;
    const int rc =
        open_model("warnerr_undefined_outlet.inp", "warnerr_undefined_outlet.rpt", rpt_text);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_TRUE(contains(rpt_text, "ERROR 209")) << rpt_text;
}

// An [OPTIONS] keyword value legacy does not know refuses the deck with
// ERROR 205 (legacy project_readOption). FLOW_UNITS FURLONGS used to open and
// run as CFS.
TEST(WarningErrorParity, UnknownFlowUnitsIsFatal205) {
    std::string rpt_text;
    const int rc = open_model("warnerr_bad_flow_units.inp", "warnerr_bad_flow_units.rpt", rpt_text);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_TRUE(contains(rpt_text, "ERROR 205")) << rpt_text;
    EXPECT_TRUE(contains(rpt_text, "FURLONGS")) << rpt_text;
}

// An unrecognized section must produce a warning in the .rpt but still open.
TEST(WarningErrorParity, UnknownSectionWarnsInReport) {
    std::string rpt_text;
    const int rc =
        open_model("warnerr_unknown_section.inp", "warnerr_unknown_section.rpt", rpt_text);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(contains(rpt_text, "Unknown section")) << rpt_text;
    EXPECT_TRUE(contains(rpt_text, "BOGUS_SECTION")) << rpt_text;
}

// The clean base model opens with no ERROR/WARNING lines (guards against the new
// diagnostics firing spuriously).
TEST(WarningErrorParity, CleanModelHasNoDiagnostics) {
    std::string rpt_text;
    const int rc = open_model("warnerr_base.inp", "warnerr_base.rpt", rpt_text);
    EXPECT_EQ(rc, 0);
    EXPECT_FALSE(contains(rpt_text, "ERROR ")) << rpt_text;
    EXPECT_FALSE(contains(rpt_text, "WARNING ")) << rpt_text;
}

// Runtime step/geometry warnings 05/06/07/08 must be recorded during open.
TEST(WarningErrorParity, RuntimeWarningsRecorded) {
    std::string rpt_text;
    const int rc =
        open_model("warnerr_runtime_warnings.inp", "warnerr_runtime_warnings.rpt", rpt_text);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(contains(rpt_text, "WARNING 05")) << rpt_text; // min slope used
    EXPECT_TRUE(contains(rpt_text, "WARNING 06")) << rpt_text; // dry step increased
    EXPECT_TRUE(contains(rpt_text, "WARNING 07")) << rpt_text; // routing step reduced
    EXPECT_TRUE(contains(rpt_text, "WARNING 08")) << rpt_text; // elev drop exceeds length
}

// ===========================================================================
// Lenient (permissive) open — swmm_engine_set_lenient_open + the error/warning
// accumulation accessors (swmm_get_error_count/_at, swmm_get_warning_count/_at).
//
// Contract under test: a lenient open of the SAME model that fails a strict
// open (undefined gage/outlet '*' -> ERROR 209) must instead reach OPENED with
// every parsed object intact and editable, while the post-parse validation
// errors stay queryable. Mirrors the GUI editor load path in
// openswmm.gui/src/layers/swmmmodellayer.cpp (set_lenient_open before open;
// error/warning read-back in adoptOpenEngine). No lifecycle is run — a broken
// model cannot initialize/step, which is exactly why the GUI loads it for edit
// only. Fixtures are the SAME committed files under data/ used above.
// ===========================================================================

#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

namespace {

// Lenient-open the fixture WITHOUT running the lifecycle. Empty rpt/out paths
// mirror the editor load (no report written). Caller owns the engine.
/**
 * @brief Create/open the fixture in lenient post-parse mode.
 *
 * Records create/open expectations, ignores lenient-mode setter status and passes empty
 * report/output paths. Caller destroys the handle. Does not initialize/start and does not make
 * fatal syntax/file errors recoverable.
 *
 * @param[in] inp Non-null existing input filename.
 * @return Caller-owned handle, potentially null/partially opened after nonfatal failure.
 */
SWMM_Engine open_lenient(const char* inp) {
    SWMM_Engine e = swmm_engine_create();
    EXPECT_NE(e, nullptr);
    swmm_engine_set_lenient_open(e, 1);
    const int rc = swmm_engine_open(e, inp, "", "", nullptr);
    EXPECT_EQ(rc, SWMM_OK) << "lenient open must not fail on post-parse errors";
    return e;
}

} // namespace

// A lenient open of the broken model reaches OPENED with all parsed objects
// intact (2 nodes J1/O1, 1 link C1) and leaves the single last-error slot
// untouched (that slot is only set when open() actually fails).
TEST(LenientOpen, BrokenModelReachesOpenedWithObjectsIntact) {
    SWMM_Engine e = open_lenient("warnerr_undefined_outlet.inp");
    int state = -1;
    EXPECT_EQ(swmm_engine_get_state(e, &state), SWMM_OK);
    EXPECT_EQ(state, SWMM_STATE_OPENED);
    EXPECT_EQ(swmm_get_last_error(e), SWMM_OK);
    EXPECT_EQ(swmm_node_count(e), 2);
    EXPECT_EQ(swmm_link_count(e), 1);
    swmm_engine_destroy(e);
}

// The post-parse validation errors accumulated during the lenient open are
// enumerable; indices out of range (and negative) return "" rather than crash.
TEST(LenientOpen, AccumulatedErrorsAreQueryable) {
    SWMM_Engine e = open_lenient("warnerr_undefined_outlet.inp");
    const int n = swmm_get_error_count(e);
    EXPECT_GE(n, 1) << "the undefined gage/outlet reference must be recorded";
    bool any_msg = false;
    for (int i = 0; i < n; ++i) {
        const char* msg = swmm_get_error_at(e, i);
        ASSERT_NE(msg, nullptr);
        if (msg[0] != '\0')
            any_msg = true;
    }
    EXPECT_TRUE(any_msg) << "a recorded error must carry a message";
    EXPECT_STREQ(swmm_get_error_at(e, n), "");
    EXPECT_STREQ(swmm_get_error_at(e, -1), "");
    swmm_engine_destroy(e);
}

// The accumulators are cleared at the start of each open() via ctx_.reset():
// re-opening a clean model on the same handle must show zero errors. open() only
// proceeds from the CREATED or CLOSED state, so the handle must be closed between
// opens; close() does not touch the accumulators, so the zeroing is open()'s doing.
TEST(LenientOpen, ErrorsClearedOnNextOpen) {
    SWMM_Engine e = swmm_engine_create();
    swmm_engine_set_lenient_open(e, 1);
    ASSERT_EQ(swmm_engine_open(e, "warnerr_undefined_outlet.inp", "", "", nullptr), SWMM_OK);
    ASSERT_GE(swmm_get_error_count(e), 1);
    ASSERT_EQ(swmm_engine_close(e), SWMM_OK); // lifecycle: reopen requires CLOSED
    ASSERT_GE(swmm_get_error_count(e), 1);    // close() alone does not clear them
    ASSERT_EQ(swmm_engine_open(e, "warnerr_base.inp", "", "", nullptr), SWMM_OK);
    EXPECT_EQ(swmm_get_error_count(e), 0);
    swmm_engine_destroy(e);
}

// Strict (default) open of the same fixture still fails — but the errors are
// populated regardless of mode, so they remain queryable after the failure,
// and the single last-error slot IS set (unlike the lenient path).
TEST(LenientOpen, StrictOpenStillFailsButRecordsErrors) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, "warnerr_undefined_outlet.inp", "", "", nullptr);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_GE(swmm_get_error_count(e), 1);
    EXPECT_STRNE(swmm_get_last_error_msg(e), "");
    swmm_engine_destroy(e);
}

#ifdef OPENSWMM_HAS_2D
// A [2D_MESH_FILE] pointing at a file that does not exist must not make the
// model unopenable in an editor: the lenient open reaches OPENED with the 1D
// objects intact and the broken reference recorded as a queryable error, so
// the GUI can load the model, warn, and let the user repair the reference.
TEST(LenientOpen, MissingExternalMeshOpensWithErrorRecorded) {
    SWMM_Engine e = open_lenient("warnerr_missing_mesh_file.inp");
    int state = -1;
    EXPECT_EQ(swmm_engine_get_state(e, &state), SWMM_OK);
    EXPECT_EQ(state, SWMM_STATE_OPENED);
    EXPECT_EQ(swmm_node_count(e), 2);
    EXPECT_EQ(swmm_link_count(e), 1);
    const int n = swmm_get_error_count(e);
    ASSERT_GE(n, 1) << "the broken mesh reference must be recorded";
    bool mentions_mesh = false;
    for (int i = 0; i < n; ++i)
        if (contains(swmm_get_error_at(e, i), "2D_MESH_FILE"))
            mentions_mesh = true;
    EXPECT_TRUE(mentions_mesh);
    swmm_engine_destroy(e);
}

// Strict (run-path) open of the same fixture still fails — graceful mesh
// degradation is an editor-only concession.
TEST(LenientOpen, MissingExternalMeshStillFailsStrictOpen) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, "warnerr_missing_mesh_file.inp", "", "", nullptr);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_TRUE(contains(swmm_get_last_error_msg(e), "2D_MESH_FILE"));
    swmm_engine_destroy(e);
}
#endif // OPENSWMM_HAS_2D

// The accessors are null-handle safe (the GUI may query a handle whose open
// failed): counts are 0, messages are "", and the setter is a no-op.
TEST(LenientOpen, AccessorsAreNullHandleSafe) {
    EXPECT_EQ(swmm_get_error_count(nullptr), 0);
    EXPECT_EQ(swmm_get_warning_count(nullptr), 0);
    EXPECT_STREQ(swmm_get_error_at(nullptr, 0), "");
    EXPECT_STREQ(swmm_get_warning_at(nullptr, 0), "");
    swmm_engine_set_lenient_open(nullptr, 1); // must not crash
}

// ===========================================================================
// Post-open failure teardown — a run that fails AFTER open() must still
// surface its root cause. Two contracts:
//   (a) end()/report() called on the unconditional teardown path must NOT
//       clobber swmm_get_last_error_msg with wrong-state complaints.
//   (b) close() must flush the cause into the .rpt, which previously carried
//       only the "[Report interrupted]" footer with no reason.
// ===========================================================================

namespace {

/**
 * @brief Collect an entire fixture file into owned text.
 *
 * Uses ordinary text mode and its platform newline rules. The local stream closes on return; no
 * model state is touched and no borrowed buffer is returned. Default stream exception settings
 * apply; allocation exceptions may propagate.
 *
 * @param[in] path Borrowed input filename used for this synchronous open/read.
 * @return Owned string collected from the stream; empty can mean an empty file or failure to open.
 *     Partial read failures have no separate status.
 */
std::string read_file(const std::string& path) {
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

// USE HOTSTART names a missing file: open() succeeds, initialize() fails.
// This site records its message ONLY via set_error (never in ctx.errors), so
// it exercises the close()-time append too.
TEST(PostOpenFailure, InitializeFailureSurvivesTeardown) {
    std::remove("warnerr_bad_hotstart.rpt");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, "warnerr_bad_hotstart.inp", "warnerr_bad_hotstart.rpt", nullptr,
                               nullptr),
              SWMM_OK);
    const int irc = swmm_engine_initialize(e);
    ASSERT_NE(irc, SWMM_OK);
    const std::string cause = swmm_get_last_error_msg(e);
    EXPECT_TRUE(contains(cause, "USE HOTSTART")) << cause;

    swmm_engine_end(e);
    swmm_engine_report(e);
    EXPECT_EQ(swmm_get_last_error(e), irc);
    EXPECT_EQ(std::string(swmm_get_last_error_msg(e)), cause);

    swmm_engine_close(e);
    swmm_engine_destroy(e);

    const std::string rpt_text = read_file("warnerr_bad_hotstart.rpt");
    EXPECT_TRUE(contains(rpt_text, cause)) << rpt_text;
}

// USE INFLOWS names a missing routing interface file: open() and initialize()
// succeed, start() fails. The cause must survive teardown and reach the .rpt.
TEST(PostOpenFailure, StartFailureSurvivesTeardown) {
    std::remove("warnerr_bad_iface.rpt");
    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(
        swmm_engine_open(e, "warnerr_bad_iface.inp", "warnerr_bad_iface.rpt", nullptr, nullptr),
        SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(e), SWMM_OK);
    const int src = swmm_engine_start(e, 0);
    ASSERT_NE(src, SWMM_OK);
    const std::string cause = swmm_get_last_error_msg(e);
    EXPECT_TRUE(contains(cause, "routing interface")) << cause;

    swmm_engine_end(e);
    swmm_engine_report(e);
    EXPECT_EQ(swmm_get_last_error(e), src);
    EXPECT_EQ(std::string(swmm_get_last_error_msg(e)), cause);

    swmm_engine_close(e);
    swmm_engine_destroy(e);

    const std::string rpt_text = read_file("warnerr_bad_iface.rpt");
    EXPECT_TRUE(contains(rpt_text, cause)) << rpt_text;
}

// Issue #124. Land use coverages adding to more than 100 % are WARNING 110:
// each land use's buildup and washoff scale by its own coverage, so the excess
// loads the subcatchment more than once. Legacy accepts the deck silently, so
// the engine warns and runs it with the values as given. Rows for one
// subcatchment add up across lines (S3), and rounding to 100 stays quiet (S2).
TEST(WarningErrorParity, CoverageOver100PercentWarns110) {
    std::string rpt_text;
    const int rc =
        open_model("warnerr_coverage_over_100.inp", "warnerr_coverage_over_100.rpt", rpt_text);
    EXPECT_EQ(rc, 0) << rpt_text;
    EXPECT_FALSE(contains(rpt_text, "ERROR ")) << rpt_text;
    EXPECT_TRUE(contains(rpt_text,
                         "WARNING 110: land use coverages of Subcatchment S1 add to more than 100"))
        << rpt_text;
    EXPECT_TRUE(contains(rpt_text,
                         "WARNING 110: land use coverages of Subcatchment S3 add to more than 100"))
        << rpt_text;
    EXPECT_FALSE(contains(rpt_text, "Subcatchment S2 add to more")) << rpt_text;
}

namespace {
// A deck whose node rows use each section's shortest legal form: a junction
// with only an elevation, a FREE outfall, a TABULAR storage unit, an OVERFLOW
// divider. `extra` is appended verbatim to [extra_section].
std::string shortRowsDeck(const std::string& extra_section, const std::string& extra) {
    std::ostringstream s;
    auto section = [&](const char* name, const char* rows) {
        s << "[" << name << "]\n" << rows;
        if (extra_section == name)
            s << extra << "\n";
        s << "\n";
    };
    s << "[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 01/01/2020\n"
         "END_DATE 01/01/2020\nEND_TIME 01:00:00\nREPORT_STEP 00:05:00\nROUTING_STEP 5\n\n";
    section("JUNCTIONS", "J1  10\n");
    section("OUTFALLS", "O1  0  FREE\nO2  0  FREE\n");
    section("STORAGE", "ST  4  10  0  TABULAR  SC\n");
    section("DIVIDERS", "DV  6  C2  OVERFLOW\n");
    s << "[CONDUITS]\nC1  J1  DV  400  0.01  0  0\nC2  DV  O1  400  0.01  0  0\n"
         "C3  DV  ST  400  0.01  0  0\nC4  ST  O2  400  0.01  0  0\n\n"
         "[XSECTIONS]\nC1  CIRCULAR  1  0  0  0\nC2  CIRCULAR  1  0  0  0\n"
         "C3  CIRCULAR  1  0  0  0\nC4  CIRCULAR  1  0  0  0\n\n"
         "[CURVES]\nSC  STORAGE  0  100\nSC           10  100\n";
    return s.str();
}
} // namespace

// Legacy node.c reads every node row with a minimum item count and stops with
// ERROR 203 below it: junctions 2; outfalls 3, and 4 for FIXED, TIDAL and
// TIMESERIES; storage 6, and 8 for every shape but TABULAR; dividers 4, 5
// for TABULAR and CUTOFF, 7 for WEIR. The engine skipped such rows, or took
// them with zero parameters, without a word. Decks are written to
// short_rows/ beside the test data.
TEST(WarningErrorParity, ShortNodeRowsAre203) {
    std::filesystem::create_directories("short_rows");
    std::string rpt_text;
    const std::string base_inp = "short_rows/base.inp";
    std::ofstream(base_inp) << shortRowsDeck("", "");
    EXPECT_EQ(open_model(base_inp, "short_rows/base.rpt", rpt_text), 0) << rpt_text;
    EXPECT_FALSE(contains(rpt_text, "ERROR ")) << rpt_text;

    const struct {
        const char* tag;
        const char* section;
        const char* row;
    } cases[] = {
        {"junction_bare", "JUNCTIONS", "J9"},
        {"outfall_two_items", "OUTFALLS", "O9  0"},
        {"outfall_fixed_no_stage", "OUTFALLS", "O9  0  FIXED"},
        {"storage_four_items", "STORAGE", "S9  4  10  0"},
        {"storage_functional_short", "STORAGE", "S9  4  10  0  FUNCTIONAL  1000"},
        {"divider_cutoff_no_flow", "DIVIDERS", "D9  6  C2  CUTOFF"},
        {"divider_weir_short", "DIVIDERS", "D9  6  C2  WEIR  1  2"},
    };
    for (const auto& c : cases) {
        const std::string inp = std::string("short_rows/") + c.tag + ".inp";
        std::ofstream(inp) << shortRowsDeck(c.section, c.row);
        EXPECT_NE(open_model(inp, std::string("short_rows/") + c.tag + ".rpt", rpt_text), 0)
            << c.tag << " opened clean";
        EXPECT_TRUE(contains(rpt_text, "ERROR 203")) << c.tag << "\n" << rpt_text;
    }
}

// Legacy node.c stops with ERROR 205 on an outfall type, divider type or
// storage shape it does not know, before it counts the type's own items, so a
// six-item storage row with an unknown shape is 205, not 203. The engine ran
// such rows as FREE, with the default divider method, or as FUNCTIONAL. Decks
// are written to short_rows/ beside the test data.
TEST(WarningErrorParity, UnknownNodeTypeKeywordsAre205) {
    std::filesystem::create_directories("short_rows");
    std::string rpt_text;
    const struct {
        const char* tag;
        const char* section;
        const char* row;
        const char* word;
    } cases[] = {
        {"outfall_unknown_type", "OUTFALLS", "O9  0  OPEN", "OPEN"},
        {"outfall_unknown_type_four", "OUTFALLS", "O9  0  OPEN  5", "OPEN"},
        {"divider_unknown_type", "DIVIDERS", "D9  6  C2  SPILL", "SPILL"},
        {"storage_unknown_six", "STORAGE", "S9  4  10  0  BOWL  1000", "BOWL"},
        {"storage_unknown_eight", "STORAGE", "S9  4  10  0  BOWL  1000  0  0", "BOWL"},
    };
    for (const auto& c : cases) {
        const std::string inp = std::string("short_rows/") + c.tag + ".inp";
        std::ofstream(inp) << shortRowsDeck(c.section, c.row);
        EXPECT_NE(open_model(inp, std::string("short_rows/") + c.tag + ".rpt", rpt_text), 0)
            << c.tag << " opened clean";
        EXPECT_TRUE(contains(rpt_text, std::string("ERROR 205: invalid keyword ") + c.word))
            << c.tag << "\n"
            << rpt_text;
        EXPECT_FALSE(contains(rpt_text, "ERROR 203")) << c.tag << "\n" << rpt_text;
    }
}

// Issue #36 (USEPA #104). Legacy removes an inlet whose design its conduit's
// shape cannot host, with WARNING 12 (inlet_validate). The engine printed the
// warning as "ERROR 635", and the usage stayed behind in the Street Inlet
// Flow Summary with no design and all of its flow bypassed. The drop grate on
// the RECT_OPEN conduit CH2 stays.
TEST(WarningErrorParity, IncompatibleInletIsRemovedWithWarning12) {
    std::string rpt_text;
    EXPECT_EQ(open_model("warnerr_inlet_shape.inp", "warnerr_inlet_shape.rpt", rpt_text), 0)
        << rpt_text;
    EXPECT_TRUE(contains(rpt_text,
                         "\n  WARNING 12: inlet removed due to unsupported shape for Conduit CH\n"))
        << rpt_text;
    EXPECT_FALSE(contains(rpt_text, "ERROR")) << rpt_text;

    // The table runs from below its starred heading to the next section's.
    const auto at = rpt_text.find("Street Inlet Flow Summary");
    ASSERT_NE(at, std::string::npos) << rpt_text;
    const auto body = rpt_text.find('\n', rpt_text.find("*****", at));
    const std::string summary = rpt_text.substr(body, rpt_text.find("*****", body) - body);
    EXPECT_TRUE(contains(summary, "\n  CH2 ")) << summary;
    EXPECT_FALSE(contains(summary, "\n  CH ")) << summary;
}
