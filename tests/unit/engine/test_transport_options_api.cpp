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
 * @file test_transport_options_api.cpp
 * @brief Y0: the seven quality/transport option keys round-trip through
 *        `swmm_options_get/set` — the surface the GUI options page hydrates.
 *
 * @details Subplan Y0 (unblocking Y1). The keys existed in the `[OPTIONS]`
 *          PARSER since X1–X3b, but the C API's key dispatch never learned
 *          them: `swmm_options_set(e, "QUALITY_SOLVER", …)` returned
 *          SWMM_ERR_BADPARAM, so the GUI could not read or write a single
 *          one. The subplan's "G1g is unblocked today" claim had checked
 *          the parser, not this dispatch — the lesson-26 shape at the
 *          plan level rather than the code level.
 *
 *          The claims:
 *          1. Every key reads its documented DEFAULT.
 *          2. Every key round-trips set → get, in the exact string forms a
 *             dialog produces (combo tokens, spin formats).
 *          3. Enum keys REJECT unknown tokens (the FV precedent the GUI's
 *             hydration contract relies on to surface typos), and the
 *             parser's aliases (ARD/LARD) are accepted.
 *          4. **The setter reaches the ENGINE, not just a string table** —
 *             set through the API, then confirm the context enum/flags
 *             actually moved. A key that round-trips but changes nothing
 *             is the failure mode this whole round exists to prevent.
 *
 *          Scratch fixtures: none (no decks — pure API).
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include <gtest/gtest.h>

#include <string>

#include <openswmm/engine/openswmm_engine.h>

#include "core/SWMMEngine.hpp"

namespace {

openswmm::SWMMEngine& as_cpp_engine(SWMM_Engine engine) {
    return *static_cast<openswmm::SWMMEngine*>(engine);
}

std::string get_opt(SWMM_Engine e, const char* key) {
    char buf[256] = {0};
    if (swmm_options_get(e, key, buf, static_cast<int>(sizeof(buf))) != SWMM_OK)
        return "<ERR>";
    return std::string(buf);
}

/// Numeric-aware compare: the engine renders doubles as "0.000000".
bool opt_equals(const std::string& got, const char* expect) {
    try {
        const double g = std::stod(got);
        const double x = std::stod(expect);
        return std::abs(g - x) < 1e-9;
    } catch (...) {
        return got == expect;
    }
}

// ---------------------------------------------------------------------------
// Gate 1 — defaults.
// ---------------------------------------------------------------------------
TEST(TransportOptionsApiTest, KeysReadTheirDefaults) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    EXPECT_EQ(get_opt(e, "QUALITY_SOLVER"), "LEGACY");
    EXPECT_EQ(get_opt(e, "WATER_AGE"), "NO");
    EXPECT_EQ(get_opt(e, "HEAT_TRANSPORT"), "NO");
    EXPECT_TRUE(opt_equals(get_opt(e, "QUALITY_STEP"), "0"));
    EXPECT_TRUE(opt_equals(get_opt(e, "MAX_SEGMENTS_PER_LINK"), "100"));
    EXPECT_EQ(get_opt(e, "DISPERSION"), "OFF");
    EXPECT_TRUE(opt_equals(get_opt(e, "RWPT_SEED"), "0"));
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 2 — set → get in the dialog's string forms.
// ---------------------------------------------------------------------------
TEST(TransportOptionsApiTest, KeysRoundTripThroughTheApi) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const struct { const char* key; const char* set; const char* expect; }
    rows[] = {
        {"QUALITY_SOLVER",        "LAGRANGIAN",   "LAGRANGIAN"},
        {"QUALITY_SOLVER",        "EULERIAN_ARD", "EULERIAN_ARD"},
        {"WATER_AGE",             "YES",          "YES"},
        {"HEAT_TRANSPORT",        "YES",          "YES"},
        {"QUALITY_STEP",          "5.00",         "5"},
        {"MAX_SEGMENTS_PER_LINK", "50",           "50"},
        {"DISPERSION",            "RWPT",         "RWPT"},
        {"RWPT_SEED",             "7",            "7"},
        // ...and back off again: the OFF/NO directions must round-trip too
        // (a setter that only ever turns things ON passes a one-way table).
        {"WATER_AGE",             "NO",           "NO"},
        {"HEAT_TRANSPORT",        "NO",           "NO"},
        {"DISPERSION",            "OFF",          "OFF"},
        {"QUALITY_SOLVER",        "LEGACY",       "LEGACY"},
    };
    for (const auto& r : rows) {
        ASSERT_EQ(swmm_options_set(e, r.key, r.set), SWMM_OK) << r.key;
        EXPECT_TRUE(opt_equals(get_opt(e, r.key), r.expect))
            << r.key << ": got " << get_opt(e, r.key) << " want " << r.expect;
    }
    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 3 — enum keys reject junk; parser aliases are accepted.
// ---------------------------------------------------------------------------
TEST(TransportOptionsApiTest, EnumKeysRejectJunkAndAcceptAliases) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);

    EXPECT_NE(swmm_options_set(e, "QUALITY_SOLVER", "MAGIC"), SWMM_OK);
    EXPECT_NE(swmm_options_set(e, "DISPERSION", "FISCHER"), SWMM_OK);
    // A rejected set must leave the previous value standing.
    EXPECT_EQ(get_opt(e, "QUALITY_SOLVER"), "LEGACY");
    EXPECT_EQ(get_opt(e, "DISPERSION"), "OFF");

    // The [OPTIONS] parser's aliases work here too — a script or GUI that
    // learned them from a deck must not hit a different vocabulary.
    ASSERT_EQ(swmm_options_set(e, "QUALITY_SOLVER", "ARD"), SWMM_OK);
    EXPECT_EQ(get_opt(e, "QUALITY_SOLVER"), "EULERIAN_ARD");
    ASSERT_EQ(swmm_options_set(e, "QUALITY_SOLVER", "LARD"), SWMM_OK);
    EXPECT_EQ(get_opt(e, "QUALITY_SOLVER"), "LAGRANGIAN");

    // Clamps documented in the engine: cap floors at 2, step floors at 0.
    ASSERT_EQ(swmm_options_set(e, "MAX_SEGMENTS_PER_LINK", "1"), SWMM_OK);
    EXPECT_TRUE(opt_equals(get_opt(e, "MAX_SEGMENTS_PER_LINK"), "2"));
    ASSERT_EQ(swmm_options_set(e, "QUALITY_STEP", "-5"), SWMM_OK);
    EXPECT_TRUE(opt_equals(get_opt(e, "QUALITY_STEP"), "0"));
    // RWPT_SEED is deliberately unclamped (any int is a valid key).
    ASSERT_EQ(swmm_options_set(e, "RWPT_SEED", "-3"), SWMM_OK);
    EXPECT_TRUE(opt_equals(get_opt(e, "RWPT_SEED"), "-3"));

    swmm_engine_destroy(e);
}

// ---------------------------------------------------------------------------
// Gate 4 — THE claim: the setter reaches engine state, not a string table.
// ---------------------------------------------------------------------------
TEST(TransportOptionsApiTest, SettersReachTheEngineState) {
    SWMM_Engine e = swmm_engine_create();
    ASSERT_NE(e, nullptr);
    const auto& opt = as_cpp_engine(e).context().options;

    ASSERT_EQ(swmm_options_set(e, "QUALITY_SOLVER", "LAGRANGIAN"), SWMM_OK);
    EXPECT_EQ(opt.quality_solver, openswmm::QualitySolverKind::LAGRANGIAN)
        << "the key round-trips through the API but never reached the "
           "engine — a surface bound to nothing (the defect Y0 exists for)";

    ASSERT_EQ(swmm_options_set(e, "WATER_AGE", "YES"), SWMM_OK);
    EXPECT_TRUE(opt.water_age);
    ASSERT_EQ(swmm_options_set(e, "HEAT_TRANSPORT", "YES"), SWMM_OK);
    EXPECT_TRUE(opt.heat_transport);
    ASSERT_EQ(swmm_options_set(e, "QUALITY_STEP", "2.5"), SWMM_OK);
    EXPECT_DOUBLE_EQ(opt.quality_step, 2.5);
    ASSERT_EQ(swmm_options_set(e, "MAX_SEGMENTS_PER_LINK", "42"), SWMM_OK);
    EXPECT_EQ(opt.max_segments_per_link, 42);
    ASSERT_EQ(swmm_options_set(e, "DISPERSION", "RWPT"), SWMM_OK);
    EXPECT_TRUE(opt.lard_rwpt);
    ASSERT_EQ(swmm_options_set(e, "RWPT_SEED", "9"), SWMM_OK);
    EXPECT_EQ(opt.rwpt_seed, 9);

    swmm_engine_destroy(e);
}

}  // namespace
