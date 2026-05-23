/**
 * @file test_controls.cpp
 * @brief Unit tests for the [CONTROLS] C API (DA-ENG-02 + existing surface).
 *
 * @see src/engine/core/openswmm_controls_impl.cpp
 * @ingroup engine_controls
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_controls.h>

// ============================================================================
// Fixture
// ============================================================================

class ControlsCApiTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;
    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
    }
    void TearDown() override { swmm_engine_destroy(engine); }
};

// ============================================================================
// DA-ENG-02 — swmm_control_get_id (rule-name extraction)
// ============================================================================

TEST_F(ControlsCApiTest, GetIdReturnsCanonicalName) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE PumpOnHigh\n"
        "IF NODE J1 DEPTH > 5\n"
        "THEN PUMP P1 STATUS = ON"), SWMM_OK);

    char buf[64] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "PumpOnHigh");
}

TEST_F(ControlsCApiTest, GetIdHandlesLowercaseKeyword) {
    // Legacy parsers accept lowercase / mixed-case "rule" / "Rule".
    ASSERT_EQ(swmm_control_add_rule(engine,
        "rule WeirBypass\nIF NODE J2 DEPTH < 1\nTHEN WEIR W1 SETTING = 0"),
        SWMM_OK);
    ASSERT_EQ(swmm_control_add_rule(engine,
        "Rule TankFill\nIF NODE T1 DEPTH < 2\nTHEN PUMP P2 STATUS = ON"),
        SWMM_OK);

    char buf[64] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "WeirBypass");
    ASSERT_EQ(swmm_control_get_id(engine, 1, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "TankFill");
}

TEST_F(ControlsCApiTest, GetIdSkipsLeadingWhitespace) {
    // A rule text saved with leading spaces / tabs / blank lines is still
    // parseable. The engine's tokenizer permits leading whitespace, so the
    // name extractor must mirror that contract.
    ASSERT_EQ(swmm_control_add_rule(engine,
        "  \t\nRULE OrificeClose\nIF NODE J3 DEPTH > 10\n"
        "THEN ORIFICE O1 SETTING = 0"), SWMM_OK);

    char buf[64] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "OrificeClose");
}

TEST_F(ControlsCApiTest, GetIdRejectsMalformedRuleText) {
    // No RULE keyword at all -> caller must fall back to a sentinel.
    ASSERT_EQ(swmm_control_add_rule(engine,
        "IF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"), SWMM_OK);
    // RULE keyword with no following name token.
    ASSERT_EQ(swmm_control_add_rule(engine, "RULE\n"), SWMM_OK);
    // RULE keyword as part of a longer word ("RULES") — must not match.
    ASSERT_EQ(swmm_control_add_rule(engine, "RULES are not RULE\n"), SWMM_OK);

    char buf[64] = {};
    EXPECT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_control_get_id(engine, 1, buf, sizeof(buf)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_control_get_id(engine, 2, buf, sizeof(buf)),
              SWMM_ERR_BADPARAM);
}

TEST_F(ControlsCApiTest, GetIdTruncatesOverlongName) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE A_Very_Long_Rule_Name_That_Exceeds_The_Buffer\n"
        "IF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"), SWMM_OK);

    char buf[8] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_EQ(std::string(buf), "A_Very_");           // 7 chars + NUL
    EXPECT_EQ(buf[sizeof(buf) - 1], '\0');
}

TEST_F(ControlsCApiTest, GetIdRejectsBadIndex) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"), SWMM_OK);

    char buf[16] = {};
    EXPECT_EQ(swmm_control_get_id(engine, -1, buf, sizeof(buf)),
              SWMM_ERR_BADINDEX);
    EXPECT_EQ(swmm_control_get_id(engine, 1, buf, sizeof(buf)),
              SWMM_ERR_BADINDEX);
}

TEST_F(ControlsCApiTest, GetIdNullArgsReturnBadParam) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"), SWMM_OK);

    char buf[16] = {};
    EXPECT_EQ(swmm_control_get_id(engine, 0, nullptr, sizeof(buf)),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_control_get_id(engine, 0, buf, 0), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_control_get_id(nullptr, 0, buf, sizeof(buf)),
              SWMM_ERR_BADHANDLE);
}

// ============================================================================
// Existing surface — sanity round-trips so the test file is self-contained.
// ============================================================================

TEST_F(ControlsCApiTest, AddAndCountRoundTrip) {
    EXPECT_EQ(swmm_control_count(engine), 0);
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R1\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON"), SWMM_OK);
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R2\nIF NODE J1 DEPTH < 1\nTHEN PUMP P1 STATUS = OFF"), SWMM_OK);
    EXPECT_EQ(swmm_control_count(engine), 2);

    ASSERT_EQ(swmm_control_clear_rules(engine), SWMM_OK);
    EXPECT_EQ(swmm_control_count(engine), 0);
}
