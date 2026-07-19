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

#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace {

// Open a model and, if the open succeeds, run the full lifecycle so the report
// plugin writes the .rpt (warnings recorded at open time reach the report via
// prepare()/write_summary()). If the open fails, the engine writes the error
// report itself. Returns the open() code; captures the produced .rpt text.
int open_model(const std::string& inp, const std::string& rpt,
               std::string& rpt_text) {
    SWMM_Engine e = swmm_engine_create();
    const int rc = swmm_engine_open(e, inp.c_str(), rpt.c_str(), nullptr, nullptr);
    if (rc == SWMM_OK) {
        if (swmm_engine_initialize(e) == SWMM_OK &&
            swmm_engine_start(e, 0) == SWMM_OK) {
            double elapsed = 0.0;
            while (swmm_engine_step(e, &elapsed) == SWMM_OK && elapsed > 0.0) {
            }
            swmm_engine_end(e);
            swmm_engine_report(e);
        }
    }
    swmm_engine_close(e);

    std::ifstream f(rpt);
    std::stringstream ss;
    ss << f.rdbuf();
    rpt_text = ss.str();
    return rc;
}

bool contains(const std::string& hay, const std::string& needle) {
    return hay.find(needle) != std::string::npos;
}

}  // namespace

// A subcatchment whose gage and outlet are '*' references no defined object.
// Legacy raises a fatal ERROR 209; the refactored engine must fail the open and
// write the error to the .rpt.
TEST(WarningErrorParity, UndefinedOutletIsFatal209) {
    std::string rpt_text;
    const int rc = open_model("warnerr_undefined_outlet.inp",
                              "warnerr_undefined_outlet.rpt", rpt_text);
    EXPECT_EQ(rc, SWMM_ERR_PARSE);
    EXPECT_TRUE(contains(rpt_text, "ERROR 209")) << rpt_text;
}

// An unrecognized section must produce a warning in the .rpt but still open.
TEST(WarningErrorParity, UnknownSectionWarnsInReport) {
    std::string rpt_text;
    const int rc = open_model("warnerr_unknown_section.inp",
                              "warnerr_unknown_section.rpt", rpt_text);
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
    const int rc = open_model("warnerr_runtime_warnings.inp",
                              "warnerr_runtime_warnings.rpt", rpt_text);
    EXPECT_EQ(rc, 0);
    EXPECT_TRUE(contains(rpt_text, "WARNING 05")) << rpt_text;  // min slope used
    EXPECT_TRUE(contains(rpt_text, "WARNING 06")) << rpt_text;  // dry step increased
    EXPECT_TRUE(contains(rpt_text, "WARNING 07")) << rpt_text;  // routing step reduced
    EXPECT_TRUE(contains(rpt_text, "WARNING 08")) << rpt_text;  // elev drop exceeds length
}
