/*
 *   test_solver_xsect_lookup_out_of_range.cpp
 *
 *   Table lookups with an out-of-range or NaN ratio (review CRASH-07, issue #174).
 *
 *   lookup() in xsect.c located a table segment with i = (int)(x / delta) and
 *   checked only the upper bound; a NaN or a very large ratio made the
 *   conversion undefined behaviour (INT_MIN on x86) and the table was read at
 *   that index: a segmentation fault. tabular_getdSdA() and getYcritEnum()
 *   had the same conversion. Decks from CRASH-07_test.c of
 *   HydroCouple/swmm-code-review (e13c013), each one conduit: CIRCULAR 1e-30 ft
 *   (depth ratio ~1e26), CIRCULAR nan, and a "nan" initial flow. Correct
 *   behaviour is no crash and no undefined behaviour; a run may still end with
 *   an input or run-time error (whether "nan" should be rejected as input is
 *   the review's IO-01), so only survival is asserted. Under the sanitizers
 *   the unfixed engine stops with a conversion error and a SEGV.
 *
 *   Working directory is tests/unit/legacy/engine/data; the decks are shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <string>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

namespace {

int runDeck(const std::string& stem) {
    const std::string inp = XR "CRASH-07_" + stem + ".inp";
    const std::string rpt = XR "_CRASH-07_" + stem + "_legacy.rpt";
    const std::string out = XR "_CRASH-07_" + stem + "_legacy.out";
    int err = swmm_open(inp.c_str(), rpt.c_str(), out.c_str());
    if (!err) err = swmm_start(1);
    double t = 0.0;
    while (!err) {
        err = swmm_step(&t);
        if (t <= 0.0) break;
    }
    if (!err) err = swmm_end();
    if (!err) err = swmm_report();
    swmm_close();
    return err;
}

}  // namespace

TEST(SolverXsectLookupOutOfRange, TinyDiameterRunsToCompletionOrAnError) {
    RecordProperty("return_code", runDeck("tiny-diameter"));
}

TEST(SolverXsectLookupOutOfRange, NanDiameterRunsToCompletionOrAnError) {
    RecordProperty("return_code", runDeck("nan-diameter"));
}

TEST(SolverXsectLookupOutOfRange, NanInitialFlowRunsToCompletionOrAnError) {
    RecordProperty("return_code", runDeck("nan-initflow"));
}
