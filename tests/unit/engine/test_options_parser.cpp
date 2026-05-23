/**
 * @file test_options_parser.cpp
 * @brief Unit tests for [OPTIONS] section parsing.
 *
 * @see src/engine/input/handlers/OptionsHandler.hpp
 * @see src/engine/core/SimulationOptions.hpp
 * @see Legacy reference: src/solver/input.c — readOption()
 * @ingroup engine_input
 */

#include <gtest/gtest.h>

#include "../../src/engine/input/handlers/OptionsHandler.hpp"
#include "../../src/engine/core/SimulationContext.hpp"

using openswmm::SimulationContext;
using openswmm::FlowUnits;
using openswmm::RoutingModel;
using openswmm::InfiltrationModel;

namespace {

// Helper: call handle_options with a list of raw option lines
static void parse(SimulationContext& ctx, std::vector<std::string> lines) {
    openswmm::input::handle_options(ctx, lines);
}

// ============================================================================
// Flow units
// ============================================================================

TEST(OptionsParserTest, FlowUnitsCFS) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS  CFS"});
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::CFS);
}

TEST(OptionsParserTest, FlowUnitsCMS) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS  CMS"});
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::CMS);
}

TEST(OptionsParserTest, FlowUnitsLPS) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS  LPS"});
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::LPS);
}

// ============================================================================
// Routing model
// ============================================================================

TEST(OptionsParserTest, RoutingModelDynwave) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_ROUTING  DYNWAVE"});
    EXPECT_EQ(ctx.options.routing_model, RoutingModel::DYNWAVE);
}

TEST(OptionsParserTest, RoutingModelKinwave) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_ROUTING  KINWAVE"});
    EXPECT_EQ(ctx.options.routing_model, RoutingModel::KINWAVE);
}

// ============================================================================
// Timesteps
// ============================================================================

TEST(OptionsParserTest, RoutingStepSeconds) {
    SimulationContext ctx;
    parse(ctx, {"ROUTING_STEP  30"});
    EXPECT_DOUBLE_EQ(ctx.options.routing_step, 30.0);
}

TEST(OptionsParserTest, RoutingStepHHMMSS) {
    SimulationContext ctx;
    parse(ctx, {"ROUTING_STEP  0:00:30"});  // 30 seconds
    EXPECT_DOUBLE_EQ(ctx.options.routing_step, 30.0);
}

TEST(OptionsParserTest, ReportStepHHMMSS) {
    SimulationContext ctx;
    parse(ctx, {"REPORT_STEP  0:15:00"});   // 15 minutes
    EXPECT_DOUBLE_EQ(ctx.options.report_step, 900.0);
}

TEST(OptionsParserTest, MinimumStep) {
    SimulationContext ctx;
    parse(ctx, {"MINIMUM_STEP  0.5"});
    EXPECT_DOUBLE_EQ(ctx.options.min_routing_step, 0.5);
}

// ============================================================================
// CRS option (R06)
// ============================================================================

TEST(OptionsParserTest, CRSStoredInOptionsAndSpatial) {
    SimulationContext ctx;
    parse(ctx, {"CRS  EPSG:4326"});
    EXPECT_EQ(ctx.options.crs,   "EPSG:4326");
    EXPECT_EQ(ctx.spatial.crs,   "EPSG:4326");
    EXPECT_TRUE(ctx.spatial.is_geographic);
}

TEST(OptionsParserTest, CRSQuotedProjString) {
    SimulationContext ctx;
    parse(ctx, {R"(CRS  "+proj=utm +zone=33 +datum=WGS84")"});
    EXPECT_EQ(ctx.options.crs, "+proj=utm +zone=33 +datum=WGS84");
    EXPECT_FALSE(ctx.spatial.is_geographic);
}

// ============================================================================
// Extension options map (R05)
// ============================================================================

TEST(OptionsParserTest, UnknownKeyStoredInExtOptions) {
    SimulationContext ctx;
    parse(ctx, {"TURBULENCE_DAMP  0.85"});
    ASSERT_TRUE(ctx.options.ext_options.count("TURBULENCE_DAMP") > 0);
    EXPECT_EQ(ctx.options.ext_options["TURBULENCE_DAMP"], "0.85");
}

TEST(OptionsParserTest, UnknownKeySetsWarning) {
    SimulationContext ctx;
    parse(ctx, {"PLUGIN_TIMEOUT  30"});
    EXPECT_NE(ctx.warning_code, 0);
}

TEST(OptionsParserTest, MultipleUnknownKeysAllStored) {
    SimulationContext ctx;
    parse(ctx, {"KEY_A  1", "KEY_B  hello", "KEY_C  3.14"});
    EXPECT_EQ(ctx.options.ext_options["KEY_A"], "1");
    EXPECT_EQ(ctx.options.ext_options["KEY_B"], "hello");
    EXPECT_EQ(ctx.options.ext_options["KEY_C"], "3.14");
}

// ============================================================================
// Case insensitivity
// ============================================================================

TEST(OptionsParserTest, LowercaseKeyParsed) {
    SimulationContext ctx;
    parse(ctx, {"flow_units  CFS"});
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::CFS);
}

TEST(OptionsParserTest, MixedCaseKey) {
    SimulationContext ctx;
    parse(ctx, {"Flow_Units  LPS"});
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::LPS);
}

// ============================================================================
// Boolean flags
// ============================================================================

TEST(OptionsParserTest, AllowPondingYes) {
    SimulationContext ctx;
    parse(ctx, {"ALLOW_PONDING  YES"});
    EXPECT_TRUE(ctx.options.allow_ponding);
}

TEST(OptionsParserTest, AllowPondingNo) {
    SimulationContext ctx;
    parse(ctx, {"ALLOW_PONDING  NO"});
    EXPECT_FALSE(ctx.options.allow_ponding);
}

TEST(OptionsParserTest, IgnoreRainfallTrue) {
    SimulationContext ctx;
    parse(ctx, {"IGNORE_RAINFALL  YES"});
    EXPECT_TRUE(ctx.options.ignore_rainfall);
}

// ============================================================================
// Comma delimiter
// ============================================================================

TEST(OptionsParserTest, CommaDelimited) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS,CFS"});
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::CFS);
}

// ============================================================================
// Solver settings
// ============================================================================

TEST(OptionsParserTest, MaxTrials) {
    SimulationContext ctx;
    parse(ctx, {"MAX_TRIALS  8"});
    EXPECT_EQ(ctx.options.max_trials, 8);
}

TEST(OptionsParserTest, HeadTolerance) {
    SimulationContext ctx;
    parse(ctx, {"HEAD_TOLERANCE  0.001"});
    EXPECT_DOUBLE_EQ(ctx.options.head_tol, 0.001);
}

// ============================================================================
// Infiltration model
// ============================================================================

TEST(OptionsParserTest, InfiltrationHorton) {
    SimulationContext ctx;
    parse(ctx, {"INFILTRATION  HORTON"});
    EXPECT_EQ(ctx.options.infiltration, InfiltrationModel::HORTON);
}

TEST(OptionsParserTest, InfiltrationGreenAmpt) {
    SimulationContext ctx;
    parse(ctx, {"INFILTRATION  GREEN_AMPT"});
    EXPECT_EQ(ctx.options.infiltration, InfiltrationModel::GREEN_AMPT);
}

} /* anonymous namespace */
