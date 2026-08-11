/**
 * @file test_control_rule_validate_api.cpp
 * @brief Unit tests for `swmm_control_validate_rule` (engine gap BR-02).
 *
 * @details The validator runs the live engine's control-rule parser against
 *          the live SimulationContext for name resolution, but does **not**
 *          mutate the engine's rule list or PID state. These tests pin:
 *
 *            (a) parser accept/reject parity with `swmm_control_add_rule`
 *                followed by simulation-init parsing,
 *            (b) state invariance — rule count and per-rule text are
 *                unchanged across `validate`, regardless of accept/reject,
 *            (c) error-message capture into the caller-supplied buffer.
 *
 *          Line-precise error reporting is not yet plumbed through the
 *          parser (line_out is -1 on reject); a future iteration will
 *          carry a 1-based line index. Tests below assert -1 for now.
 *
 * @see src/engine/core/openswmm_controls_impl.cpp::swmm_control_validate_rule
 * @ingroup engine_controls
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_controls.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/engine/openswmm_links.h>

// ============================================================================
// Fixture — bare engine with a couple of nodes + a link so name resolution
// can succeed. The parser needs the SimulationContext's name tables
// populated; we don't run the simulation.
// ============================================================================

class ControlRuleValidateTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);

        // Seed two nodes + a pump so NODE / PUMP references resolve.
        // Node type 0 = Junction (matches openswmm_nodes.h default ctor path);
        // link type for "pump" varies across the API surface — using 4
        // (Pump) per the SWMM convention. If this needs to change to match
        // the engine's link-type enum, the validator behaviour is the same
        // either way — the test asserts accept/reject, not the type code.
        ASSERT_EQ(swmm_node_add(engine, "J1", 0), SWMM_OK);
        ASSERT_EQ(swmm_node_add(engine, "J2", 0), SWMM_OK);
        ASSERT_EQ(swmm_link_add(engine, "P1", 4), SWMM_OK);
    }

    void TearDown() override { swmm_engine_destroy(engine); }
};

// ============================================================================
// Happy path — well-formed rule against known objects → SWMM_OK + empty err.
// ============================================================================

TEST_F(ControlRuleValidateTest, AcceptsWellFormedRule) {
    char errbuf[128] = {'X'};   // pre-fill so we can verify truncation/clear
    int  line = 99;             // pre-fill so we can verify -1 on success
    const char* rule =
        "RULE R_OK\n"
        "IF NODE J1 DEPTH > 5\n"
        "THEN PUMP P1 STATUS = ON";

    EXPECT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_OK);
    EXPECT_STREQ(errbuf, "");
    EXPECT_EQ(line, -1);
}

// ============================================================================
// Reject cases — each exercises a different parser failure mode.
// ============================================================================

TEST_F(ControlRuleValidateTest, RejectsEmptyText) {
    char errbuf[128] = {};
    int  line = 0;
    EXPECT_EQ(swmm_control_validate_rule(engine, "", errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
}

TEST_F(ControlRuleValidateTest, RejectsRuleWithNoName) {
    char errbuf[128] = {};
    int  line = 0;
    // "RULE" alone — no name token.
    EXPECT_EQ(swmm_control_validate_rule(engine, "RULE\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON",
                                          errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
    // The buffer should hold a non-empty message on reject.
    EXPECT_GT(std::strlen(errbuf), 0u);
    EXPECT_EQ(line, 1);
}

TEST_F(ControlRuleValidateTest, RejectsUnresolvedNodeName) {
    char errbuf[128] = {};
    int  line = 0;
    // J_DOES_NOT_EXIST is not in ctx.node_names — parser returns -1.
    const char* rule =
        "RULE R_BadNode\n"
        "IF NODE J_DOES_NOT_EXIST DEPTH > 5\n"
        "THEN PUMP P1 STATUS = ON";
    EXPECT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
}

TEST_F(ControlRuleValidateTest, RejectsUnresolvedLinkInAction) {
    char errbuf[128] = {};
    int  line = 0;
    // PUMP P_GHOST does not exist.
    const char* rule =
        "RULE R_BadLink\n"
        "IF NODE J1 DEPTH > 5\n"
        "THEN PUMP P_GHOST STATUS = ON";
    EXPECT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
}

TEST_F(ControlRuleValidateTest, RejectsMissingActionEquals) {
    char errbuf[128] = {};
    int  line = 0;
    // "STATUS ON" instead of "STATUS = ON" — missing '='.
    const char* rule =
        "RULE R_NoEq\n"
        "IF NODE J1 DEPTH > 5\n"
        "THEN PUMP P1 STATUS ON";
    EXPECT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
}

// ============================================================================
// State invariance — engine rule list must not change across validate calls,
// regardless of accept/reject. This is the non-mutation contract callers
// (specifically the GUI editor) rely on.
// ============================================================================

TEST_F(ControlRuleValidateTest, DoesNotMutateEngineOnAccept) {
    // Pre-seed one stored rule via the existing add API.
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE Preexisting\nIF NODE J2 DEPTH > 2\nTHEN PUMP P1 STATUS = ON"),
        SWMM_OK);
    ASSERT_EQ(swmm_control_count(engine), 1);

    char errbuf[128] = {};
    int  line = 0;
    ASSERT_EQ(swmm_control_validate_rule(engine,
        "RULE Transient\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON",
        errbuf, sizeof(errbuf), &line), SWMM_OK);

    // Rule list must still hold exactly the one Preexisting rule.
    EXPECT_EQ(swmm_control_count(engine), 1);
    char buf[64] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Preexisting");
}

TEST_F(ControlRuleValidateTest, DoesNotMutateEngineOnReject) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE Preexisting\nIF NODE J2 DEPTH > 2\nTHEN PUMP P1 STATUS = ON"),
        SWMM_OK);
    ASSERT_EQ(swmm_control_count(engine), 1);

    char errbuf[128] = {};
    int  line = 0;
    ASSERT_EQ(swmm_control_validate_rule(engine,
        "RULE Garbage\nIF SOMETHING TOTALLY WRONG",
        errbuf, sizeof(errbuf), &line), SWMM_ERR_BADPARAM);

    EXPECT_EQ(swmm_control_count(engine), 1);
    char buf[64] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Preexisting");
}

// ============================================================================
// Error-buffer hygiene
// ============================================================================

TEST_F(ControlRuleValidateTest, NullErrbufIsTolerated) {
    int line = 0;
    EXPECT_EQ(swmm_control_validate_rule(engine, "RULE\n",
                                          nullptr, 0, &line),
              SWMM_ERR_BADPARAM);
    // Should not crash — line still gets a value.
    EXPECT_EQ(line, 1);
}

// ============================================================================
// Line-precise diagnostics — the rejection must name the offending line and
// say why, not just "the parser said no". `line` is 1-based over the text as
// submitted, blank lines included.
// ============================================================================

TEST_F(ControlRuleValidateTest, ReportsLineAndReasonForUnknownLink) {
    char errbuf[256] = {};
    int  line = 0;
    const char* rule =
        "RULE R_BadLink\n"        // 1
        "IF NODE J1 DEPTH > 5\n"  // 2
        "THEN PUMP P1 STATUS = ON\n"
        "AND PUMP P_GHOST STATUS = OFF";  // 4 — the bad one
    ASSERT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(line, 4);
    EXPECT_NE(std::string(errbuf).find("P_GHOST"), std::string::npos)
        << "message should name the unresolved link; got: " << errbuf;
}

TEST_F(ControlRuleValidateTest, LineNumberCountsBlankLines) {
    // Blank lines are skipped by the parser but must still be counted, so the
    // reported number indexes the caller's text directly.
    char errbuf[256] = {};
    int  line = 0;
    const char* rule =
        "RULE R_Blank\n"          // 1
        "\n"                      // 2
        "IF NODE J1 DEPTH > 5\n"  // 3
        "\n"                      // 4
        "THEN PUMP P1 STATUS = MAYBE";  // 5
    ASSERT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(line, 5);
}

TEST_F(ControlRuleValidateTest, ReportsReasonForBadRelationalOperator) {
    char errbuf[256] = {};
    int  line = 0;
    const char* rule =
        "RULE R_BadOp\n"
        "IF NODE J1 DEPTH => 5\n"   // "=>" is not an operator; ">=" is
        "THEN PUMP P1 STATUS = ON";
    ASSERT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(line, 2);
    EXPECT_NE(std::string(errbuf).find("relational operator"), std::string::npos)
        << "got: " << errbuf;
}

TEST_F(ControlRuleValidateTest, EmptyTextHasNoOffendingLine) {
    // Whitespace-only input parses cleanly but yields no rule, so there is no
    // single line to blame — line_out stays -1 while the call still fails.
    char errbuf[128] = {};
    int  line = 0;
    EXPECT_EQ(swmm_control_validate_rule(engine, "   \n\n", errbuf, sizeof(errbuf), &line),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(line, -1);
    EXPECT_GT(std::strlen(errbuf), 0u);
}

// ============================================================================
// Multi-action THEN blocks — `AND`-chained actions are unbounded. Pinned
// because a user reported (incorrectly) that a 7-action rule hit a limit.
// ============================================================================

TEST_F(ControlRuleValidateTest, AcceptsManyChainedActions) {
    std::string rule = "RULE R_Many\nIF SIMULATION TIME >= 0\n"
                       "THEN PUMP P1 STATUS = ON";
    for (int i = 0; i < 32; ++i) rule += "\nAND PUMP P1 SETTING = 0.5";

    char errbuf[256] = {};
    int  line = 0;
    EXPECT_EQ(swmm_control_validate_rule(engine, rule.c_str(),
                                          errbuf, sizeof(errbuf), &line),
              SWMM_OK) << errbuf;
}

TEST_F(ControlRuleValidateTest, NullLineOutIsTolerated) {
    char errbuf[64] = {};
    EXPECT_EQ(swmm_control_validate_rule(engine,
        "RULE R\nIF NODE J1 DEPTH > 5\nTHEN PUMP P1 STATUS = ON",
        errbuf, sizeof(errbuf), nullptr),
              SWMM_OK);
}

TEST_F(ControlRuleValidateTest, ErrbufTruncatesWithoutOverflow) {
    // 8-byte buffer is shorter than the canned reject message — the impl
    // must truncate without writing past the end + still null-terminate.
    char errbuf[8] = {'\xAB','\xAB','\xAB','\xAB','\xAB','\xAB','\xAB','\xAB'};
    EXPECT_EQ(swmm_control_validate_rule(engine, "RULE\n",
                                          errbuf, sizeof(errbuf), nullptr),
              SWMM_ERR_BADPARAM);
    EXPECT_EQ(errbuf[sizeof(errbuf) - 1], '\0');
    EXPECT_GT(std::strlen(errbuf), 0u);
    EXPECT_LT(std::strlen(errbuf), sizeof(errbuf));
}

// ============================================================================
// Refactor regression — swmm_control_add_rule must still accept identical
// inputs after BR-02 ships. (The new entry point shares the live engine's
// ControlEngine parser via a throwaway instance; the existing add path is
// untouched. This test pins the contract.)
// ============================================================================

TEST_F(ControlRuleValidateTest, AddRuleStillStoresValidatedText) {
    const char* rule =
        "RULE Persist\n"
        "IF NODE J1 DEPTH > 5\n"
        "THEN PUMP P1 STATUS = ON";

    char errbuf[128] = {};
    int  line = 0;
    ASSERT_EQ(swmm_control_validate_rule(engine, rule, errbuf, sizeof(errbuf), &line),
              SWMM_OK);
    ASSERT_EQ(swmm_control_add_rule(engine, rule), SWMM_OK);

    EXPECT_EQ(swmm_control_count(engine), 1);
    char buf[64] = {};
    ASSERT_EQ(swmm_control_get_id(engine, 0, buf, sizeof(buf)), SWMM_OK);
    EXPECT_STREQ(buf, "Persist");
}
