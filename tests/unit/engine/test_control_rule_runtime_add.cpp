/**
 * @file test_control_rule_runtime_add.cpp
 * @brief Rules added via `swmm_control_add_rule` after the model is
 *        initialized must actually take effect.
 *
 * @details Control rules are compiled once, inside `swmm_engine_initialize()`
 *          → `SWMMEngine::initHydraulics()`. Before this fixture existed,
 *          `swmm_control_add_rule` only appended to
 *          `ctx.control_rules.rule_text`, so a rule added after
 *          initialization returned `SWMM_OK`, incremented
 *          `swmm_control_count`, and then did nothing at all for the rest of
 *          the run. These tests pin the repaired contract:
 *
 *            (a) a rule added post-initialize is compiled into the live
 *                engine and drives link settings on the next step,
 *            (b) every action in a multi-action `THEN` block is applied, not
 *                just the first,
 *            (c) unparseable text is rejected rather than silently stored,
 *            (d) re-initializing does not duplicate already-compiled rules.
 *
 *          Model artifacts are written under `control_runtime_add/` relative
 *          to the test working directory so a failing run leaves inspectable
 *          .inp / .rpt / .out files behind (CLAUDE.md §4.1).
 *
 * @see src/engine/core/openswmm_controls_impl.cpp::swmm_control_add_rule
 * @see src/engine/core/SWMMEngine.cpp::initHydraulics (rule compile site)
 * @ingroup engine_controls
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_controls.h>
#include <openswmm/engine/openswmm_links.h>

namespace fs = std::filesystem;

namespace {

const fs::path kArtifactDir = "control_runtime_add";

/// Four orifices discharging to a common junction, no controls in the .inp —
/// every rule under test is added through the C API instead.
const char* kModel = R"INP([TITLE]
Fixture for post-initialize control-rule addition.

[OPTIONS]
FLOW_UNITS           CFS
INFILTRATION         HORTON
FLOW_ROUTING         DYNWAVE
START_DATE           01/01/2024
START_TIME           00:00:00
END_DATE             01/01/2024
END_TIME             06:00:00
REPORT_STEP          00:05:00
ROUTING_STEP         00:00:15

[JUNCTIONS]
J1       10.0  10.0  0.0  0.0  0.0
J2       10.0  10.0  0.0  0.0  0.0
J3       10.0  10.0  0.0  0.0  0.0
J4       10.0  10.0  0.0  0.0  0.0
J9        5.0  10.0  0.0  0.0  0.0

[OUTFALLS]
OF1      0.0   FREE       NO

[CONDUITS]
C1       J9    OF1   400   0.01   5.0  0.0  0

[ORIFICES]
OR1      J1    J9   SIDE   0.0   0.65  NO   0
OR2      J2    J9   SIDE   0.0   0.65  NO   0
OR3      J3    J9   SIDE   0.0   0.65  NO   0
OR4      J4    J9   SIDE   0.0   0.65  NO   0

[XSECTIONS]
C1      CIRCULAR   3.0   0   0   0   1
OR1     CIRCULAR   1.0   0   0   0   1
OR2     CIRCULAR   1.0   0   0   0   1
OR3     CIRCULAR   1.0   0   0   0   1
OR4     CIRCULAR   1.0   0   0   0   1

[REPORT]
INPUT      NO
CONTROLS   NO
NODES ALL
LINKS ALL
)INP";

/// Link indices follow [CONDUITS] then [ORIFICES] declaration order.
constexpr int kOR1 = 1, kOR2 = 2, kOR3 = 3, kOR4 = 4;

double setting_of(SWMM_Engine engine, int idx) {
    double v = -1.0;
    EXPECT_EQ(swmm_link_get_control_setting(engine, idx, &v), SWMM_OK);
    return v;
}

}  // namespace

class ControlRuleRuntimeAddTest : public ::testing::Test {
protected:
    SWMM_Engine engine = nullptr;

    void SetUp() override {
        fs::create_directories(kArtifactDir);
        const fs::path inp = kArtifactDir / "model.inp";
        {
            std::ofstream f(inp);
            ASSERT_TRUE(f) << "cannot write " << inp;
            f << kModel;
        }

        engine = swmm_engine_new();
        ASSERT_NE(engine, nullptr);
        ASSERT_EQ(swmm_engine_open(engine,
                                   inp.string().c_str(),
                                   (kArtifactDir / "model.rpt").string().c_str(),
                                   (kArtifactDir / "model.out").string().c_str(),
                                   nullptr),
                  SWMM_OK);
        ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
    }

    void TearDown() override { swmm_engine_destroy(engine); }

    /// Run a few steps so the control engine gets a chance to evaluate.
    void step_a_few() {
        ASSERT_EQ(swmm_engine_start(engine, 0), SWMM_OK);
        double elapsed = 0.0;
        for (int i = 0; i < 5; ++i) {
            ASSERT_EQ(swmm_engine_step(engine, &elapsed), SWMM_OK);
        }
    }
};

// ============================================================================
// The regression: added post-initialize, therefore never compiled.
// ============================================================================

TEST_F(ControlRuleRuntimeAddTest, RuleAddedAfterInitializeTakesEffect) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R_ONE\n"
        "IF SIMULATION TIME >= 0\n"
        "THEN ORIFICE OR1 SETTING = 0.11"), SWMM_OK);

    step_a_few();

    EXPECT_DOUBLE_EQ(setting_of(engine, kOR1), 0.11);
}

// ============================================================================
// Every action in the THEN block must fire — the user-reported shape.
// ============================================================================

TEST_F(ControlRuleRuntimeAddTest, AllChainedActionsAreApplied) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R_MULTI\n"
        "IF SIMULATION TIME >= 0\n"
        "THEN ORIFICE OR1 SETTING = 0.11\n"
        "AND ORIFICE OR2 SETTING = 0.22\n"
        "AND ORIFICE OR3 SETTING = 0.33\n"
        "AND ORIFICE OR4 SETTING = 0.44"), SWMM_OK);

    step_a_few();

    EXPECT_DOUBLE_EQ(setting_of(engine, kOR1), 0.11);
    EXPECT_DOUBLE_EQ(setting_of(engine, kOR2), 0.22);
    EXPECT_DOUBLE_EQ(setting_of(engine, kOR3), 0.33);
    EXPECT_DOUBLE_EQ(setting_of(engine, kOR4), 0.44);
}

// ============================================================================
// Post-initialize the parser runs, so bad text must be refused, not stored.
// ============================================================================

TEST_F(ControlRuleRuntimeAddTest, RejectsUnresolvedLinkAfterInitialize) {
    ASSERT_EQ(swmm_control_count(engine), 0);

    EXPECT_EQ(swmm_control_add_rule(engine,
        "RULE R_GHOST\n"
        "IF SIMULATION TIME >= 0\n"
        "THEN ORIFICE OR_NOPE SETTING = 1"), SWMM_ERR_BADPARAM);

    // Rejected text must not land in the store.
    EXPECT_EQ(swmm_control_count(engine), 0);

    // ...and the failure must be explainable, naming line and cause.
    const std::string msg = swmm_get_last_error_msg(engine);
    EXPECT_NE(msg.find("line 3"), std::string::npos) << msg;
    EXPECT_NE(msg.find("OR_NOPE"), std::string::npos) << msg;
}

TEST_F(ControlRuleRuntimeAddTest, RejectsTextContainingNoRule) {
    EXPECT_EQ(swmm_control_add_rule(engine, "   \n\n"), SWMM_ERR_BADPARAM);
    EXPECT_EQ(swmm_control_count(engine), 0);
}

// ============================================================================
// A mid-block rejection must not leave half the block compiled.
// ============================================================================

TEST_F(ControlRuleRuntimeAddTest, PartiallyBadBlockLeavesNoRulesBehind) {
    // First rule is fine; the second references a link that does not exist.
    EXPECT_EQ(swmm_control_add_rule(engine,
        "RULE R_GOOD\n"
        "IF SIMULATION TIME >= 0\n"
        "THEN ORIFICE OR1 SETTING = 0.11\n"
        "RULE R_BAD\n"
        "IF SIMULATION TIME >= 0\n"
        "THEN ORIFICE OR_NOPE SETTING = 1"), SWMM_ERR_BADPARAM);

    EXPECT_EQ(swmm_control_count(engine), 0);

    step_a_few();

    // R_GOOD must not have leaked into the live engine.
    EXPECT_DOUBLE_EQ(setting_of(engine, kOR1), 1.0);
}

// ============================================================================
// Re-initializing recompiles from the stored text.
//
// add_rule compiles into the same ControlEngine that initialize() populates,
// so initialize() clears the compiled set first (SWMMEngine::initHydraulics)
// to avoid stacking a second copy of every rule. The compiled-rule count is
// not reachable through the C API — `swmm_control_count` reports stored text —
// so this pins the observable half: re-initializing is safe and the rule still
// drives the link. Duplication itself is guarded by ControlEngine::clearRules.
// ============================================================================

TEST_F(ControlRuleRuntimeAddTest, RuleSurvivesReinitialize) {
    ASSERT_EQ(swmm_control_add_rule(engine,
        "RULE R_ONE\n"
        "IF SIMULATION TIME >= 0\n"
        "THEN ORIFICE OR1 SETTING = 0.11"), SWMM_OK);
    ASSERT_EQ(swmm_control_count(engine), 1);

    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
    EXPECT_EQ(swmm_control_count(engine), 1);

    step_a_few();
    EXPECT_DOUBLE_EQ(setting_of(engine, kOR1), 0.11);
}
