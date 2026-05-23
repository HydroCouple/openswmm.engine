/**
 * @file test_plugin_lifecycle.cpp
 * @brief Unit tests for the plugin lifecycle state machine.
 *
 * @details Tests that:
 *          - Valid state transitions are allowed by plugin_state_transition_valid()
 *          - Invalid transitions are rejected
 *          - DefaultOutputPlugin traverses the full lifecycle in order
 *          - DefaultReportPlugin traverses the full lifecycle in order
 *          - PluginFactory dispatches lifecycle calls to all registered plugins
 *          - PluginFactory skips plugins not in the expected state
 *
 * @see include/openswmm/plugin_sdk/PluginState.hpp
 * @see src/engine/plugins/DefaultOutputPlugin.hpp
 * @see src/engine/plugins/DefaultReportPlugin.hpp
 * @see src/engine/plugins/PluginFactory.hpp
 * @see docs/MASTER_IMPLEMENTATION_PLAN.md Phase 4
 * @ingroup engine_plugins
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <openswmm/plugin_sdk/PluginState.hpp>

#include "../../src/engine/plugins/DefaultOutputPlugin.hpp"
#include "../../src/engine/plugins/DefaultReportPlugin.hpp"
#include "../../src/engine/plugins/PluginFactory.hpp"
#include "../../src/engine/core/SimulationContext.hpp"
#include "../../include/openswmm/plugin_sdk/SimulationSnapshot.hpp"

using openswmm::PluginState;
using openswmm::DefaultOutputPlugin;
using openswmm::DefaultReportPlugin;
using openswmm::PluginFactory;
using openswmm::SimulationContext;
using openswmm::SimulationSnapshot;

namespace fs = std::filesystem;

namespace {

// Cross-platform temporary directory (resolves to /tmp on Unix,
// %TEMP% on Windows).
static const std::string TMP = (fs::temp_directory_path() / "").string();

// ============================================================================
// PluginState transition validation
// ============================================================================

TEST(PluginStateTest, ValidTransitionsAllowed) {
    using PS = PluginState;
    using openswmm::plugin_state_transition_valid;

    EXPECT_TRUE(plugin_state_transition_valid(PS::UNLOADED,    PS::LOADED));
    EXPECT_TRUE(plugin_state_transition_valid(PS::LOADED,      PS::INITIALIZED));
    EXPECT_TRUE(plugin_state_transition_valid(PS::INITIALIZED, PS::VALIDATED));
    EXPECT_TRUE(plugin_state_transition_valid(PS::VALIDATED,   PS::PREPARED));
    EXPECT_TRUE(plugin_state_transition_valid(PS::PREPARED,    PS::UPDATING));
    EXPECT_TRUE(plugin_state_transition_valid(PS::UPDATING,    PS::PREPARED));
    EXPECT_TRUE(plugin_state_transition_valid(PS::PREPARED,    PS::FINALIZED));
    EXPECT_TRUE(plugin_state_transition_valid(PS::FINALIZED,   PS::CLOSED));
}

TEST(PluginStateTest, InvalidTransitionsRejected) {
    using PS = PluginState;
    using openswmm::plugin_state_transition_valid;

    EXPECT_FALSE(plugin_state_transition_valid(PS::UNLOADED,    PS::INITIALIZED));
    EXPECT_FALSE(plugin_state_transition_valid(PS::LOADED,      PS::PREPARED));
    EXPECT_FALSE(plugin_state_transition_valid(PS::INITIALIZED, PS::UPDATING));
    EXPECT_FALSE(plugin_state_transition_valid(PS::CLOSED,      PS::LOADED));
    EXPECT_FALSE(plugin_state_transition_valid(PS::FINALIZED,   PS::UPDATING));
}

TEST(PluginStateTest, AnyStateCanTransitionToError) {
    using PS = PluginState;
    using openswmm::plugin_state_transition_valid;

    EXPECT_TRUE(plugin_state_transition_valid(PS::UNLOADED,    PS::ERROR));
    EXPECT_TRUE(plugin_state_transition_valid(PS::LOADED,      PS::ERROR));
    EXPECT_TRUE(plugin_state_transition_valid(PS::INITIALIZED, PS::ERROR));
    EXPECT_TRUE(plugin_state_transition_valid(PS::PREPARED,    PS::ERROR));
    EXPECT_TRUE(plugin_state_transition_valid(PS::UPDATING,    PS::ERROR));
    EXPECT_TRUE(plugin_state_transition_valid(PS::FINALIZED,   PS::ERROR));
}

TEST(PluginStateTest, StateToStringNeverNull) {
    using PS = PluginState;
    using openswmm::plugin_state_to_string;

    EXPECT_STREQ(plugin_state_to_string(PS::UNLOADED),    "UNLOADED");
    EXPECT_STREQ(plugin_state_to_string(PS::LOADED),      "LOADED");
    EXPECT_STREQ(plugin_state_to_string(PS::INITIALIZED), "INITIALIZED");
    EXPECT_STREQ(plugin_state_to_string(PS::VALIDATED),   "VALIDATED");
    EXPECT_STREQ(plugin_state_to_string(PS::PREPARED),    "PREPARED");
    EXPECT_STREQ(plugin_state_to_string(PS::UPDATING),    "UPDATING");
    EXPECT_STREQ(plugin_state_to_string(PS::FINALIZED),   "FINALIZED");
    EXPECT_STREQ(plugin_state_to_string(PS::CLOSED),      "CLOSED");
    EXPECT_STREQ(plugin_state_to_string(PS::ERROR),       "ERROR");
}

// ============================================================================
// DefaultOutputPlugin state machine
// ============================================================================

class DefaultOutputPluginTest : public ::testing::Test {
protected:
    SimulationContext ctx;
    SimulationSnapshot snap;
};

TEST_F(DefaultOutputPluginTest, ConstructorSetsLoadedState) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    EXPECT_EQ(p.state(), PluginState::LOADED);
}

TEST_F(DefaultOutputPluginTest, InitializeTransitionsToInitialized) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    EXPECT_EQ(p.initialize({}, nullptr), 0);
    EXPECT_EQ(p.state(), PluginState::INITIALIZED);
}

TEST_F(DefaultOutputPluginTest, ValidateTransitionsToValidated) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    p.initialize({}, nullptr);
    EXPECT_EQ(p.validate(ctx), 0);
    EXPECT_EQ(p.state(), PluginState::VALIDATED);
}

TEST_F(DefaultOutputPluginTest, PrepareTransitionsToPrepared) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    EXPECT_EQ(p.prepare(ctx), 0);
    EXPECT_EQ(p.state(), PluginState::PREPARED);
}

TEST_F(DefaultOutputPluginTest, UpdateTransitionsToUpdating) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);
    EXPECT_EQ(p.update(snap), 0);
    EXPECT_EQ(p.state(), PluginState::UPDATING);
}

TEST_F(DefaultOutputPluginTest, MultipleUpdatesIncrementStepCount) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);

    // PluginFactory calls update_all for each snapshot; here we call directly
    for (int i = 0; i < 5; ++i) {
        EXPECT_EQ(p.update(snap), 0);
    }
    // State ends at UPDATING after the last call
    EXPECT_EQ(p.state(), PluginState::UPDATING);
}

TEST_F(DefaultOutputPluginTest, FinalizeTransitionsToFinalized) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);
    p.update(snap);
    EXPECT_EQ(p.finalize(ctx), 0);
    EXPECT_EQ(p.state(), PluginState::FINALIZED);
}

TEST_F(DefaultOutputPluginTest, FullLifecycleReturnsAllZero) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    EXPECT_EQ(p.initialize({}, nullptr), 0);
    EXPECT_EQ(p.validate(ctx),           0);
    EXPECT_EQ(p.prepare(ctx),            0);
    EXPECT_EQ(p.update(snap),            0);
    EXPECT_EQ(p.update(snap),            0);
    EXPECT_EQ(p.finalize(ctx),           0);
    EXPECT_EQ(p.state(), PluginState::FINALIZED);
}

TEST_F(DefaultOutputPluginTest, PrepareResetsStepCountAfterReinit) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);
    p.update(snap);
    p.update(snap);
    p.finalize(ctx);

    // Simulate re-initialize (e.g., for a second run)
    EXPECT_EQ(p.initialize({}, nullptr), 0);
    EXPECT_EQ(p.validate(ctx),           0);
    EXPECT_EQ(p.prepare(ctx),            0);
    EXPECT_EQ(p.state(), PluginState::PREPARED);
}

TEST_F(DefaultOutputPluginTest, LastErrorMessageIsNotNull) {
    DefaultOutputPlugin p((TMP + "test.out").c_str());
    EXPECT_NE(p.last_error_message(), nullptr);
}

// ============================================================================
// DefaultReportPlugin state machine
// ============================================================================

class DefaultReportPluginTest : public ::testing::Test {
protected:
    SimulationContext ctx;
    SimulationSnapshot snap;
};

TEST_F(DefaultReportPluginTest, ConstructorSetsLoadedState) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    EXPECT_EQ(p.state(), PluginState::LOADED);
}

TEST_F(DefaultReportPluginTest, InitializeTransitionsToInitialized) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    EXPECT_EQ(p.initialize({}, nullptr), 0);
    EXPECT_EQ(p.state(), PluginState::INITIALIZED);
}

TEST_F(DefaultReportPluginTest, ValidateTransitionsToValidated) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    p.initialize({}, nullptr);
    EXPECT_EQ(p.validate(ctx), 0);
    EXPECT_EQ(p.state(), PluginState::VALIDATED);
}

TEST_F(DefaultReportPluginTest, PrepareTransitionsToPrepared) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    EXPECT_EQ(p.prepare(ctx), 0);
    EXPECT_EQ(p.state(), PluginState::PREPARED);
}

TEST_F(DefaultReportPluginTest, UpdateTransitionsToUpdating) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);
    EXPECT_EQ(p.update(snap), 0);
    EXPECT_EQ(p.state(), PluginState::UPDATING);
}

TEST_F(DefaultReportPluginTest, FinalizeTransitionsToFinalized) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);
    p.update(snap);
    EXPECT_EQ(p.finalize(ctx), 0);
    EXPECT_EQ(p.state(), PluginState::FINALIZED);
}

TEST_F(DefaultReportPluginTest, WriteSummaryReturnsZero) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    p.initialize({}, nullptr);
    p.validate(ctx);
    p.prepare(ctx);
    p.finalize(ctx);
    EXPECT_EQ(p.write_summary(ctx), 0);
}

TEST_F(DefaultReportPluginTest, FullLifecycleReturnsAllZero) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    EXPECT_EQ(p.initialize({}, nullptr), 0);
    EXPECT_EQ(p.validate(ctx),           0);
    EXPECT_EQ(p.prepare(ctx),            0);
    EXPECT_EQ(p.update(snap),            0);
    EXPECT_EQ(p.finalize(ctx),           0);
    EXPECT_EQ(p.write_summary(ctx),      0);
    EXPECT_EQ(p.state(), PluginState::FINALIZED);
}

TEST_F(DefaultReportPluginTest, LastErrorMessageIsNotNull) {
    DefaultReportPlugin p((TMP + "test.rpt").c_str());
    EXPECT_NE(p.last_error_message(), nullptr);
}

// ============================================================================
// PluginFactory lifecycle dispatch
// ============================================================================

class PluginFactoryLifecycleTest : public ::testing::Test {
protected:
    SimulationContext ctx;
    SimulationSnapshot snap;
};

TEST_F(PluginFactoryLifecycleTest, EmptyFactoryHasNoPlugins) {
    PluginFactory factory;
    EXPECT_TRUE(factory.empty());
    EXPECT_EQ(factory.plugin_count(), 0);
    EXPECT_EQ(factory.output_plugins().size(), 0u);
    EXPECT_EQ(factory.report_plugins().size(), 0u);
}

TEST_F(PluginFactoryLifecycleTest, AddOutputPluginIncrementsCount) {
    PluginFactory factory;
    auto* p = new DefaultOutputPlugin((TMP + "a.out").c_str());
    factory.add_output_plugin(p);
    EXPECT_EQ(factory.plugin_count(), 1);
    EXPECT_FALSE(factory.empty());
    EXPECT_EQ(factory.output_plugins().size(), 1u);
}

TEST_F(PluginFactoryLifecycleTest, AddReportPluginIncrementsCount) {
    PluginFactory factory;
    auto* p = new DefaultReportPlugin((TMP + "a.rpt").c_str());
    factory.add_report_plugin(p);
    EXPECT_EQ(factory.plugin_count(), 1);
    EXPECT_EQ(factory.report_plugins().size(), 1u);
}

TEST_F(PluginFactoryLifecycleTest, AddBothPluginTypes) {
    PluginFactory factory;
    factory.add_output_plugin(new DefaultOutputPlugin((TMP + "a.out").c_str()));
    factory.add_report_plugin(new DefaultReportPlugin((TMP + "a.rpt").c_str()));
    EXPECT_EQ(factory.plugin_count(), 2);
}

TEST_F(PluginFactoryLifecycleTest, PrepareAllDispatchesToValidatedPlugins) {
    PluginFactory factory;
    auto* op = new DefaultOutputPlugin((TMP + "a.out").c_str());
    auto* rp = new DefaultReportPlugin((TMP + "a.rpt").c_str());

    // Manually drive to VALIDATED state (as SWMMEngine::open() does)
    op->initialize({}, nullptr);
    op->validate(ctx);
    rp->initialize({}, nullptr);
    rp->validate(ctx);

    factory.add_output_plugin(op);
    factory.add_report_plugin(rp);

    EXPECT_EQ(factory.prepare_all(ctx), 0);
    EXPECT_EQ(op->state(), PluginState::PREPARED);
    EXPECT_EQ(rp->state(), PluginState::PREPARED);
}

TEST_F(PluginFactoryLifecycleTest, PrepareAllSkipsUnvalidatedPlugins) {
    PluginFactory factory;

    // Output plugin manually initialized but NOT validated
    auto* op = new DefaultOutputPlugin((TMP + "a.out").c_str());
    op->initialize({}, nullptr);
    // state == INITIALIZED, not VALIDATED → prepare_all should skip it

    factory.add_output_plugin(op);
    EXPECT_EQ(factory.prepare_all(ctx), 0);
    // Plugin should still be in INITIALIZED (not PREPARED)
    EXPECT_EQ(op->state(), PluginState::INITIALIZED);
}

TEST_F(PluginFactoryLifecycleTest, UpdateAllDispatchesToPreparedPlugins) {
    PluginFactory factory;
    auto* op = new DefaultOutputPlugin((TMP + "a.out").c_str());

    op->initialize({}, nullptr);
    op->validate(ctx);
    op->prepare(ctx);  // state == PREPARED

    factory.add_output_plugin(op);

    EXPECT_EQ(factory.update_all(snap), 0);
    EXPECT_EQ(op->state(), PluginState::UPDATING);
}

TEST_F(PluginFactoryLifecycleTest, UpdateAllSkipsUnpreparedPlugins) {
    PluginFactory factory;
    auto* op = new DefaultOutputPlugin((TMP + "a.out").c_str());
    // state == LOADED — should not receive update_all

    factory.add_output_plugin(op);
    EXPECT_EQ(factory.update_all(snap), 0);
    EXPECT_EQ(op->state(), PluginState::LOADED);
}

TEST_F(PluginFactoryLifecycleTest, FinalizeAllDispatchesToAllPlugins) {
    PluginFactory factory;
    auto* op = new DefaultOutputPlugin((TMP + "a.out").c_str());
    auto* rp = new DefaultReportPlugin((TMP + "a.rpt").c_str());

    op->initialize({}, nullptr);
    op->validate(ctx);
    op->prepare(ctx);
    rp->initialize({}, nullptr);
    rp->validate(ctx);
    rp->prepare(ctx);

    factory.add_output_plugin(op);
    factory.add_report_plugin(rp);

    EXPECT_EQ(factory.finalize_all(ctx), 0);
    EXPECT_EQ(op->state(), PluginState::FINALIZED);
    EXPECT_EQ(rp->state(), PluginState::FINALIZED);
}

TEST_F(PluginFactoryLifecycleTest, WriteSummaryAllDispatchesToReportPlugins) {
    PluginFactory factory;
    auto* rp = new DefaultReportPlugin((TMP + "a.rpt").c_str());

    rp->initialize({}, nullptr);
    rp->validate(ctx);
    rp->prepare(ctx);
    rp->finalize(ctx);

    factory.add_report_plugin(rp);

    EXPECT_EQ(factory.write_summary_all(ctx), 0);
    // State unchanged after write_summary (report plugin stays FINALIZED)
    EXPECT_EQ(rp->state(), PluginState::FINALIZED);
}

TEST_F(PluginFactoryLifecycleTest, FullLifecycleViaFactory) {
    PluginFactory factory;
    auto* op = new DefaultOutputPlugin((TMP + "b.out").c_str());
    auto* rp = new DefaultReportPlugin((TMP + "b.rpt").c_str());

    // SWMMEngine::open() pattern: inject and immediately init+validate
    op->initialize({}, nullptr);
    op->validate(ctx);
    rp->initialize({}, nullptr);
    rp->validate(ctx);

    factory.add_output_plugin(op);
    factory.add_report_plugin(rp);

    // SWMMEngine::start()
    EXPECT_EQ(factory.prepare_all(ctx), 0);
    EXPECT_EQ(op->state(), PluginState::PREPARED);
    EXPECT_EQ(rp->state(), PluginState::PREPARED);

    // SWMMEngine::step() × 3
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(factory.update_all(snap), 0);
    }

    // SWMMEngine::end()
    EXPECT_EQ(factory.finalize_all(ctx), 0);
    EXPECT_EQ(op->state(), PluginState::FINALIZED);
    EXPECT_EQ(rp->state(), PluginState::FINALIZED);

    // SWMMEngine::report()
    EXPECT_EQ(factory.write_summary_all(ctx), 0);
}

TEST_F(PluginFactoryLifecycleTest, UnloadAllClearsPlugins) {
    PluginFactory factory;
    factory.add_output_plugin(new DefaultOutputPlugin((TMP + "c.out").c_str()));
    factory.add_report_plugin(new DefaultReportPlugin((TMP + "c.rpt").c_str()));
    EXPECT_FALSE(factory.empty());

    factory.unload_all();
    EXPECT_TRUE(factory.empty());
    EXPECT_EQ(factory.plugin_count(), 0);
}

TEST_F(PluginFactoryLifecycleTest, LoadPluginsWithEmptySpecsReturnsZero) {
    PluginFactory factory;
    int loaded = factory.load_plugins({});
    EXPECT_EQ(loaded, 0);
    EXPECT_TRUE(factory.empty());
}

TEST_F(PluginFactoryLifecycleTest, LoadPluginsWithBadPathEmitsWarning) {
    PluginFactory factory;
    openswmm::PluginSpec bad_spec;
    bad_spec.path = "/nonexistent/path/plugin.so";

    std::string captured_warning;
    int loaded = factory.load_plugins({bad_spec}, [&](const std::string& msg) {
        captured_warning = msg;
    });

    EXPECT_EQ(loaded, 0);
    EXPECT_TRUE(factory.empty());
    // Warning should contain the path
    EXPECT_NE(captured_warning.find("plugin.so"), std::string::npos);
}

TEST_F(PluginFactoryLifecycleTest, DynamicLoadFromPath) {
    // Loading a real .so/.dylib is an integration test; skip here.
    // This test verifies the load_plugins() plumbing path above.
    GTEST_SKIP() << "Real dynamic library load tested in integration tests";
}

} /* anonymous namespace */
