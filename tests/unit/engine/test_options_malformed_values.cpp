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
 * @file test_options_malformed_values.cpp
 * @brief A malformed option value returns an error — it does not kill the
 *        process.
 *
 * @details Found while validating Y0 and recorded there as pre-existing and
 *          TU-wide: thirty of `swmm_options_set`'s branches parsed with raw
 *          `std::stod` / `std::stoi`, which THROW. An exception crossing
 *          `extern "C"` terminates the process. Measured then:
 *          `FV_CFL = "abc"` aborted, while `ROUTING_STEP = "xyz"` survived
 *          because that one branch already carried a local try/catch.
 *
 *          Reachable in production, which is why it earned its own round:
 *          the MCP server's `model_set_option` tool passes arbitrary
 *          LLM-authored text straight into this dispatch, and a GUI line
 *          edit is transiently empty while a user types.
 *
 *          The fix is one guard around the whole dispatch rather than
 *          thirty local ones — a per-site fix can miss a site, and a future
 *          branch would be born unguarded. **These gates are therefore
 *          written to be exhaustive over the numeric keys**, not
 *          representative: every key that parses a number gets a row, so a
 *          regression on any single branch is visible.
 *
 *          Note on how these gates fail: if the guard is removed, the
 *          process ABORTS rather than reporting a failed expectation. A
 *          crashed test binary is the observation — ctest reports the
 *          death, and that is the intended signal.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>

namespace {

std::string get_opt(SWMM_Engine e, const char* key) {
    char buf[256] = {0};
    if (swmm_options_get(e, key, buf, static_cast<int>(sizeof(buf))) != SWMM_OK)
        return "<ERR>";
    return std::string(buf);
}

/// Every option key whose setter parses a number. Exhaustive on purpose
/// (see the file header) — grep the dispatch for std::stod/std::stoi when
/// adding a key and add a row here too.
const char* const kNumericKeys[] = {
    // Timesteps / dates
    "ROUTING_STEP", "MINIMUM_STEP", "DRY_DAYS", "DRY_STEP", "WET_STEP",
    "REPORT_STEP", "RULE_STEP", "LENGTHENING_STEP",
    // Solver
    "MAX_TRIALS", "HEAD_TOLERANCE", "SYS_FLOW_TOL", "LAT_FLOW_TOL",
    "VARIABLE_STEP", "THREADS",
    // Finite volume
    "FV_CELL_LENGTH", "FV_MIN_CELLS", "FV_CFL", "FV_ORDER",
    "FV_SLOT_CELERITY", "FV_DISPERSION",
    "FV_MIN_PARALLEL_CELLS", "FV_LTS_MAX_TIERS", "FV_CFL_CENSUS_INTERVAL",
    // Quality & transport (Y0)
    "QUALITY_STEP", "MAX_SEGMENTS_PER_LINK", "RWPT_SEED",
};

/// Values every numeric key must refuse. VALIDATION FINDING (H1 round):
/// the delivered list applied "99999999999999999999999" to EVERY key —
/// but that string is 1e23, a perfectly well-formed double, so the gate
/// over-claimed for stod/time keys. It moved to the int-only list below.
/// "1e999999" stays here: stod throws out_of_range, the strict int
/// parses refuse the trailing "e999999", and the strict time parse
/// refuses it as neither seconds nor H:M[:S].
const char* const kMalformed[] = {
    "abc",        // std::invalid_argument
    "",           // empty — a line edit mid-typing
    " ",          // whitespace only
    "--",         // punctuation only
    "1e999999",   // overflow (stod) / trailing junk (strict stoi & time)
};

/// Integer-parsed keys (strict stoi/stol in the dispatch) and the values
/// only THEY must refuse: a genuine stoi overflow, and the partial-parse
/// family std::stoi silently truncated before H1 ("1.5" became 1,
/// "1e999999" became 1 — a caller's typo turned into a different number).
const char* const kIntKeys[] = {
    "MAX_TRIALS", "THREADS", "FV_MIN_CELLS", "FV_ORDER",
    "FV_MIN_PARALLEL_CELLS", "FV_LTS_MAX_TIERS", "FV_CFL_CENSUS_INTERVAL",
    "MAX_SEGMENTS_PER_LINK", "RWPT_SEED",
};
const char* const kIntMalformed[] = {
    "99999999999999999999999",  // std::out_of_range on stoi/stol
    "1.5",                      // partial parse — silently truncated before H1
};

/// Time-typed keys (seconds or H:M[:S] — parse_time_seconds' grammar).
/// Their parser never throws: before H1, EVERY string in kMalformed was
/// silently stored as 0.0, and "1e999999" became 3600 s through the
/// H[:M] fallthrough. The strict wrapper refuses; the clock form must
/// keep working (gate 3).
const char* const kTimeKeys[] = {
    "ROUTING_STEP", "REPORT_STEP", "MINIMUM_STEP",
    "DRY_STEP", "WET_STEP", "RULE_STEP",
};

}  // namespace

// ---------------------------------------------------------------------------
// Gate 1 — no malformed value on any numeric key may terminate the process.
// ---------------------------------------------------------------------------
TEST(OptionsMalformedValuesTest, MalformedNumericsReturnErrorNotDeath) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    for (const char* key : kNumericKeys) {
        for (const char* bad : kMalformed) {
            // Reaching the next line at all is the claim: before the guard
            // this call aborted the process on the first key.
            const int rc = swmm_options_set(e, key, bad);
            EXPECT_NE(rc, SWMM_OK)
                << "key '" << key << "' accepted malformed value '" << bad
                << "' — a bad parse must be refused, not silently stored";
        }
    }
    // Int-parsed keys additionally refuse overflow and PARTIAL parses —
    // std::stoi's silent truncation was the second non-throwing family
    // the exception guard alone could not see.
    for (const char* key : kIntKeys) {
        for (const char* bad : kIntMalformed) {
            EXPECT_NE(swmm_options_set(e, key, bad), SWMM_OK)
                << "int key '" << key << "' accepted '" << bad << "'";
        }
    }

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — a refused set leaves the previous value standing.
// ---------------------------------------------------------------------------
TEST(OptionsMalformedValuesTest, RefusedSetPreservesThePreviousValue) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    // Establish known-good values, then attack each with junk.
    ASSERT_EQ(swmm_options_set(e, "FV_CFL", "0.75"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(e, "MAX_SEGMENTS_PER_LINK", "50"), SWMM_OK);
    ASSERT_EQ(swmm_options_set(e, "QUALITY_STEP", "5"), SWMM_OK);
    const std::string cfl0  = get_opt(e, "FV_CFL");
    const std::string segs0 = get_opt(e, "MAX_SEGMENTS_PER_LINK");
    const std::string qs0   = get_opt(e, "QUALITY_STEP");

    EXPECT_NE(swmm_options_set(e, "FV_CFL", "abc"), SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "MAX_SEGMENTS_PER_LINK", "xyz"), SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "QUALITY_STEP", ""), SWMM_OK);

    // The throwing branch assigned nothing, so the old value must survive.
    // A guard that swallowed the exception AFTER a partial write would show
    // up here as a moved value.
    EXPECT_EQ(get_opt(e, "FV_CFL"), cfl0);
    EXPECT_EQ(get_opt(e, "MAX_SEGMENTS_PER_LINK"), segs0);
    EXPECT_EQ(get_opt(e, "QUALITY_STEP"), qs0);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — the guard did not break the happy path (liveness).
// ---------------------------------------------------------------------------
TEST(OptionsMalformedValuesTest, WellFormedValuesStillRoundTrip) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    // If the guard were placed so that it swallowed successful control flow
    // — e.g. wrapping the `return SWMM_OK` — every set would still "work"
    // but nothing would persist. These rows are the liveness half.
    const struct { const char* key; const char* val; } good[] = {
        { "FV_CFL",                 "0.6"  },
        { "MAX_SEGMENTS_PER_LINK",  "64"   },
        { "QUALITY_STEP",           "2.5"  },
        { "RWPT_SEED",              "11"   },
        { "THREADS",                "3"    },
    };
    for (const auto& g : good) {
        ASSERT_EQ(swmm_options_set(e, g.key, g.val), SWMM_OK) << g.key;
        const std::string got = get_opt(e, g.key);
        EXPECT_NE(got, "<ERR>") << g.key;
        EXPECT_DOUBLE_EQ(std::stod(got), std::stod(g.val)) << g.key;
    }

    // The documented clock form survives the strict time parse — a
    // strictness fix that broke "0:00:30" would trade one defect for
    // another. And the lenient parser's artifacts ("5:" meant 5 hours,
    // junk meant 0.0) are refused, not reinterpreted.
    ASSERT_EQ(swmm_options_set(e, "ROUTING_STEP", "0:00:30"), SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(get_opt(e, "ROUTING_STEP")), 30.0);
    ASSERT_EQ(swmm_options_set(e, "REPORT_STEP", "1:30"), SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(get_opt(e, "REPORT_STEP")), 5400.0);
    EXPECT_NE(swmm_options_set(e, "ROUTING_STEP", "5:"), SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "ROUTING_STEP", "1:2:3:4"), SWMM_OK);
    EXPECT_DOUBLE_EQ(std::stod(get_opt(e, "ROUTING_STEP")), 30.0);

    // And an unknown key is still refused — the guard must not turn the
    // dispatch's final `else` into a success.
    EXPECT_NE(swmm_options_set(e, "NO_SUCH_KEY_AT_ALL", "1"), SWMM_OK);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — enum keys keep rejecting junk (the guard must not mask them).
// ---------------------------------------------------------------------------
TEST(OptionsMalformedValuesTest, EnumKeysStillRejectUnknownTokens) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    // These return BADPARAM by an explicit `else return`, not by throwing.
    // Same answer as a bad numeric, reached a different way — if the guard
    // ever short-circuited the dispatch these would start passing.
    EXPECT_NE(swmm_options_set(e, "FLOW_UNITS",     "FURLONGS"), SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "FLOW_ROUTING",   "MAGIC"),    SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "QUALITY_SOLVER", "MAGIC"),    SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "DISPERSION",     "FISCHER"),  SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "FV_RIEMANN",     "ROE"),      SWMM_OK);

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 5 — the H1 §7 residue: the same TU's OTHER two dispatches
// (closeout P1.3). swmm_options_set was hardened in H1; the audit of the
// remaining `*_impl.cpp` parse sites found exactly three more, all in
// this TU, all inside local try/catch — so neither could ABORT, but both
// accepted the PARTIAL-PARSE family silently: HOTSTART_SAVE_DATETIME
// stored 1.5 for "1.5abc" (and 1.0 for "01/01/2026"), and a file-slot
// owner index of "0.5" resolved to slot 0 — a caller's typo turned into
// a different slot. The LID_REPORT owner shares the same edit and
// pattern but has no direct observer here: exercising it needs a LID
// deck this TU does not carry. Recorded, not claimed.
// ---------------------------------------------------------------------------
TEST(OptionsMalformedValuesTest, FilesAndPathSlotParsesAreStrict) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    // Establish hot-start save slot 0 with a known datetime, then attack.
    ASSERT_EQ(swmm_files_set(e, "HOTSTART_SAVE_PATH", "a.hsf"), SWMM_OK);
    ASSERT_EQ(swmm_files_set(e, "HOTSTART_SAVE_DATETIME", "46036.5"),
              SWMM_OK);

    // ("" is deliberately absent: for this key it is the documented
    // clear spelling, not a malformed value.)
    const char* const bad_datetime[] = {
        "abc", " ", "--",
        "1.5abc",      // partial parse — stored 1.5 before this gate
        "01/01/2026",  // date spelling — stored 1.0 before this gate
        "1e999999",    // overflow
    };
    for (const char* bad : bad_datetime)
        EXPECT_NE(swmm_files_set(e, "HOTSTART_SAVE_DATETIME", bad), SWMM_OK)
            << "HOTSTART_SAVE_DATETIME accepted '" << bad << "'";
    // Every refused set left the previous value standing.
    {
        char buf[64] = {0};
        ASSERT_EQ(swmm_files_get(e, "HOTSTART_SAVE_DATETIME", buf,
                                 static_cast<int>(sizeof(buf))),
                  SWMM_OK);
        EXPECT_DOUBLE_EQ(std::stod(buf), 46036.5);
    }

    // Owner indexes are decimal integers, strictly. "0.5" resolving to
    // slot 0 was a wrong-slot WRITE — the quieter sibling of the
    // wrong-value store, and with slot 0 present it returned SWMM_OK.
    EXPECT_NE(swmm_file_path_set(e, SWMM_FILE_HOTSTART_SAVE, "0.5",
                                 "b.hsf"),
              SWMM_OK);
    EXPECT_NE(swmm_file_path_set(e, SWMM_FILE_HOTSTART_SAVE, "0x0",
                                 "b.hsf"),
              SWMM_OK);
    char abs_buf[512] = {0};
    char orig_buf[512] = {0};
    EXPECT_NE(swmm_file_path_get(e, SWMM_FILE_HOTSTART_SAVE, "0.5",
                                 abs_buf, static_cast<int>(sizeof(abs_buf)),
                                 orig_buf,
                                 static_cast<int>(sizeof(orig_buf))),
              SWMM_OK);
    // Liveness: the plain index still resolves, and the attacked slot
    // kept its path.
    ASSERT_EQ(swmm_file_path_get(e, SWMM_FILE_HOTSTART_SAVE, "0",
                                 abs_buf, static_cast<int>(sizeof(abs_buf)),
                                 orig_buf,
                                 static_cast<int>(sizeof(orig_buf))),
              SWMM_OK);
    EXPECT_STREQ(orig_buf, "a.hsf");

    swmm_engine_destroy(e);
}
