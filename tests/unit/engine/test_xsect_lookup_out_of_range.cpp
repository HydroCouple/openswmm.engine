// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_xsect_lookup_out_of_range.cpp
 * @brief Table lookups with an out-of-range or NaN ratio (review CRASH-07, issue #174).
 *
 * XSectKernels.hpp located a table segment with i = (int)(alpha / delta) and
 * checked only the upper bound; a NaN or a very large ratio made the
 * conversion undefined behaviour (INT_MIN on x86) and the table was then read
 * at that index. The critical-depth search likewise converted a NaN estimate.
 * Decks from CRASH-07_test6.c of HydroCouple/swmm-code-review (e13c013), each
 * one conduit: CIRCULAR 1e-30 ft (depth ratio ~1e26), CIRCULAR nan, and a
 * "nan" initial flow. Correct behaviour is no crash and no undefined
 * behaviour; a run may still end with an input or run-time error (whether
 * "nan" should be rejected as input is the review's IO-01), so only survival
 * is asserted here. Under the sanitizers the unfixed engine stops with a
 * conversion error and a SEGV.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace {

int runDeck(const std::string& stem) {
    const std::string inp = "xsect_review/CRASH-07_" + stem + ".inp";
    const std::string rpt = "xsect_review/_CRASH-07_" + stem + ".rpt";
    const std::string out = "xsect_review/_CRASH-07_" + stem + ".out";
    SWMM_Engine e = swmm_engine_create();
    int err = swmm_engine_open(e, inp.c_str(), rpt.c_str(), out.c_str(), nullptr);
    if (!err) err = swmm_engine_initialize(e);
    if (!err) err = swmm_engine_start(e, 1);
    double t = 0.0;
    while (!err) {
        err = swmm_engine_step(e, &t);
        if (t <= 0.0) break;
    }
    if (!err) err = swmm_engine_end(e);
    if (!err) err = swmm_engine_report(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
    return err;
}

}  // namespace

TEST(XsectLookupOutOfRange, TinyDiameterRunsToCompletionOrAnError) {
    const int err = runDeck("tiny-diameter");
    RecordProperty("return_code", err);
}

TEST(XsectLookupOutOfRange, NanDiameterRunsToCompletionOrAnError) {
    const int err = runDeck("nan-diameter");
    RecordProperty("return_code", err);
}

TEST(XsectLookupOutOfRange, NanInitialFlowRunsToCompletionOrAnError) {
    const int err = runDeck("nan-initflow");
    RecordProperty("return_code", err);
}
