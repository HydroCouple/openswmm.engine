/**
 * @file      test_options_parser.cpp
 * @brief     Unit tests for [OPTIONS] section parsing.
 *
 * @see src/engine/input/handlers/OptionsHandler.hpp
 * @see src/engine/core/SimulationOptions.hpp
 * @see Legacy reference: src/solver/input.c — readOption()
 * @ingroup engine_input
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>

#include <openswmm/legacy/engine/openswmm_solver.h>

#include "../../src/engine/input/handlers/OptionsHandler.hpp"
#include "../../src/engine/core/SimulationContext.hpp"

using openswmm::SimulationContext;
using openswmm::FlowUnits;
using openswmm::RoutingModel;
using openswmm::InfiltrationModel;

namespace {

// Helper: call handle_options with a list of raw option lines
/**
 * @brief Apply raw OPTIONS rows directly through the maintained handler.
 *
 * Delegates without a return status; callers inspect options/errors/warnings. Does not parse a
 * complete file, finalize a model or execute a simulation.
 *
 * @param[in,out] ctx Context whose option values and diagnostics may change.
 * @param[in] lines Owned-copy raw option rows.
 */
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
    parse(ctx, {"ROUTING_STEP  0:00:30"}); // 30 seconds
    EXPECT_DOUBLE_EQ(ctx.options.routing_step, 30.0);
}

TEST(OptionsParserTest, ReportStepHHMMSS) {
    SimulationContext ctx;
    parse(ctx, {"REPORT_STEP  0:15:00"}); // 15 minutes
    EXPECT_DOUBLE_EQ(ctx.options.report_step, 900.0);
}

TEST(OptionsParserTest, MinimumStep) {
    SimulationContext ctx;
    parse(ctx, {"MINIMUM_STEP  0.5"});
    EXPECT_DOUBLE_EQ(ctx.options.min_routing_step, 0.5);
}

// ============================================================================
// Date separators — legacy datetime_strToDate accepts '-' as well as '/'
// (`if (strchr(s,'-') || strchr(s,'/'))`, then `%d%c%d%c%d`), so a deck
// writing 1-1-2000 means the same instant as one writing 01/01/2000. The
// parser took '/' only and returned 0.0 for anything else, which is not a
// failure the caller can see: 1900-nodes got a start and an end of zero, ran a
// zero-length simulation, and wrote an .out with no periods while reporting
// success.
// ============================================================================

TEST(OptionsParserTest, DateAcceptsHyphenSeparator) {
    SimulationContext a, b;
    parse(a, {"START_DATE  01/01/2000", "END_DATE  01/02/2000"});
    parse(b, {"START_DATE  1-1-2000", "END_DATE  1-2-2000"});
    EXPECT_DOUBLE_EQ(b.options.start_date, a.options.start_date);
    EXPECT_DOUBLE_EQ(b.options.end_date, a.options.end_date);
    // and the run has a real duration, which is what the deck actually lost
    EXPECT_GT(b.options.end_date, b.options.start_date);
}

TEST(OptionsParserTest, DateRejectsAMissingSeparator) {
    SimulationContext ctx;
    parse(ctx, {"START_DATE  01012000"});
    EXPECT_DOUBLE_EQ(ctx.options.start_date, SimulationContext{}.options.start_date);
    ASSERT_EQ(ctx.errors.size(), 1u);
    EXPECT_NE(ctx.errors[0].find("ERROR 213"), std::string::npos);
}

// ============================================================================
// CRS option (R06)
// ============================================================================

TEST(OptionsParserTest, CRSStoredInOptionsAndSpatial) {
    SimulationContext ctx;
    parse(ctx, {"CRS  EPSG:4326"});
    EXPECT_EQ(ctx.options.crs, "EPSG:4326");
    EXPECT_EQ(ctx.spatial.crs, "EPSG:4326");
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

// ============================================================================
// Unsteady friction (issue #156)
// ============================================================================

TEST(OptionsParserTest, UnsteadyFrictionDefaults) {
    SimulationContext ctx;
    parse(ctx, {});
    EXPECT_EQ(ctx.options.unsteady_friction, 0); // NONE
    EXPECT_DOUBLE_EQ(ctx.options.uf_k3, 0.015);
}

TEST(OptionsParserTest, UnsteadyFrictionVitkovsky) {
    SimulationContext ctx;
    parse(ctx, {"UNSTEADY_FRICTION  VITKOVSKY", "UF_K3  0.02"});
    EXPECT_EQ(ctx.options.unsteady_friction, 1);
    EXPECT_DOUBLE_EQ(ctx.options.uf_k3, 0.02);
}

TEST(OptionsParserTest, UnsteadyFrictionNoneExplicit) {
    SimulationContext ctx;
    parse(ctx, {"UNSTEADY_FRICTION  none"});
    EXPECT_EQ(ctx.options.unsteady_friction, 0);
}

TEST(OptionsParserTest, UnsteadyFrictionBadValueLeavesDefault) {
    SimulationContext ctx;
    parse(ctx, {"UNSTEADY_FRICTION  BOGUS"});
    EXPECT_EQ(ctx.options.unsteady_friction, 0);
}

// ============================================================================
// Keyword values legacy refuses (ERROR 205)
//
// Legacy project_readOption resolves these options with findmatch over a
// keyword list and refuses the deck with ERR_KEYWORD when nothing matches.
// v6 kept the default instead, so `FLOW_UNITS FURLONGS` opened and ran as CFS.
// ============================================================================

namespace {

/**
 * @brief Count ERROR 205 diagnostics that name a value.
 *
 * @param[in] ctx Context whose errors are read.
 * @param[in] value Borrowed option value expected in the message.
 * @return Number of ctx.errors entries containing both "ERROR 205" and value.
 */
int keywordErrors(const SimulationContext& ctx, const std::string& value) {
    int n = 0;
    for (const auto& e : ctx.errors)
        if (e.find("ERROR 205") != std::string::npos && e.find(value) != std::string::npos)
            ++n;
    return n;
}

} // namespace

TEST(OptionsParserTest, FlowUnitsUnknownValueRefusesTheDeck) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS  FURLONGS"});
    ASSERT_EQ(ctx.errors.size(), 1u);
    EXPECT_EQ(keywordErrors(ctx, "FURLONGS"), 1) << ctx.errors[0];
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::CFS); // the default, untouched
    EXPECT_EQ(ctx.options.ext_options.count("FLOW_UNITS"), 0u);
}

TEST(OptionsParserTest, UnknownKeywordValuesRefuseTheDeck) {
    const char* const rows[] = {
        "INFILTRATION  BOGUS",       "FLOW_ROUTING  BOGUS",        "LINK_OFFSETS  BOGUS",
        "FORCE_MAIN_EQUATION  BOGUS", "INERTIAL_DAMPING  BOGUS",   "NORMAL_FLOW_LIMITED  BOGUS",
        "SURCHARGE_METHOD  BOGUS",   "ALLOW_PONDING  BOGUS",       "SKIP_STEADY_STATE  BOGUS",
        "IGNORE_RAINFALL  BOGUS",    "IGNORE_SNOWMELT  BOGUS",     "IGNORE_GROUNDWATER  BOGUS",
        "IGNORE_RDII  BOGUS",        "IGNORE_ROUTING  BOGUS",      "IGNORE_QUALITY  BOGUS"};
    for (const char* row : rows) {
        SimulationContext ctx;
        parse(ctx, {row});
        EXPECT_EQ(ctx.errors.size(), 1u) << row;
        EXPECT_EQ(keywordErrors(ctx, "BOGUS"), 1) << row;
    }
}

TEST(OptionsParserTest, RefusedValueKeepsThePriorSetting) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS  LPS", "FLOW_UNITS  FURLONGS", "INFILTRATION  GREEN_AMPT",
                "INFILTRATION  BOGUS", "ALLOW_PONDING  YES", "ALLOW_PONDING  MAYBE"});
    EXPECT_EQ(ctx.errors.size(), 3u);
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::LPS);
    EXPECT_EQ(ctx.options.infiltration, InfiltrationModel::GREEN_AMPT);
    EXPECT_TRUE(ctx.options.allow_ponding);
}

// Every legacy word, and this engine's own spellings, still parse cleanly.
TEST(OptionsParserTest, EveryAcceptedSpellingStillParses) {
    const char* const rows[] = {
        "FLOW_UNITS CFS", "FLOW_UNITS gpm", "FLOW_UNITS MGD", "FLOW_UNITS CMS", "FLOW_UNITS LPS",
        "FLOW_UNITS MLD", "INFILTRATION HORTON", "INFILTRATION MODIFIED_HORTON",
        "INFILTRATION MOD_HORTON", "INFILTRATION GREEN_AMPT", "INFILTRATION MODIFIED_GREEN_AMPT",
        "INFILTRATION MOD_GREEN_AMPT", "INFILTRATION CURVE_NUMBER", "FLOW_ROUTING NONE",
        "FLOW_ROUTING STEADY", "FLOW_ROUTING KINWAVE", "FLOW_ROUTING XKINWAVE",
        "FLOW_ROUTING DYNWAVE", "FLOW_ROUTING NF", "FLOW_ROUTING KW", "FLOW_ROUTING EKW",
        "FLOW_ROUTING DW", "FLOW_ROUTING STEADYFLOW", "FLOW_ROUTING KINEMATIC_WAVE",
        "FLOW_ROUTING DYNAMIC_WAVE", "FLOW_ROUTING FV", "FLOW_ROUTING FINITE_VOLUME",
        "FLOW_ROUTING NO_ROUTING", "LINK_OFFSETS DEPTH", "LINK_OFFSETS ELEVATION",
        "FORCE_MAIN_EQUATION H-W", "FORCE_MAIN_EQUATION HW", "FORCE_MAIN_EQUATION HAZEN-WILLIAMS",
        "FORCE_MAIN_EQUATION D-W", "FORCE_MAIN_EQUATION DW", "FORCE_MAIN_EQUATION DARCY-WEISBACH",
        "INERTIAL_DAMPING NONE", "INERTIAL_DAMPING PARTIAL", "INERTIAL_DAMPING FULL",
        "NORMAL_FLOW_LIMITED SLOPE", "NORMAL_FLOW_LIMITED FROUDE", "NORMAL_FLOW_LIMITED BOTH",
        "NORMAL_FLOW_LIMITED NONE", "NORMAL_FLOW_LIMITED NEITHER", "SURCHARGE_METHOD EXTRAN",
        "SURCHARGE_METHOD SLOT", "SURCHARGE_METHOD DYNAMIC_SLOT", "SURCHARGE_METHOD TPA",
        "ALLOW_PONDING YES", "ALLOW_PONDING NO", "ALLOW_PONDING TRUE", "ALLOW_PONDING FALSE",
        "ALLOW_PONDING 1", "ALLOW_PONDING 0", "SKIP_STEADY_STATE no", "IGNORE_QUALITY yes"};
    for (const char* row : rows) {
        SimulationContext ctx;
        parse(ctx, {row});
        EXPECT_TRUE(ctx.errors.empty()) << row << ": " << (ctx.errors.empty() ? "" : ctx.errors[0]);
    }
}

// legacy findmatch accepts a keyword followed by anything; read those as
// legacy reads them (FLOW_UNITS LPSX used to run as CFS).
TEST(OptionsParserTest, LegacyPrefixFormsReadAsLegacyReadsThem) {
    SimulationContext ctx;
    parse(ctx, {"FLOW_UNITS  LPSX", "INFILTRATION  CURVE_NUMBERS", "LINK_OFFSETS  ELEVATIONS",
                "SURCHARGE_METHOD  SLOTTED", "ALLOW_PONDING  YESS"});
    EXPECT_TRUE(ctx.errors.empty());
    EXPECT_EQ(ctx.options.flow_units, FlowUnits::LPS);
    EXPECT_EQ(ctx.options.infiltration, InfiltrationModel::CURVE_NUMBER);
    EXPECT_EQ(ctx.options.link_offsets, 1);
    EXPECT_EQ(ctx.options.surcharge_method, 1);
    EXPECT_TRUE(ctx.options.allow_ponding);
}

// Legacy NormalFlowWords ends with NONE (index 3); it used to be ignored and
// run as BOTH.
TEST(OptionsParserTest, NormalFlowLimitedNoneIsNeither) {
    SimulationContext ctx;
    parse(ctx, {"NORMAL_FLOW_LIMITED  NONE"});
    EXPECT_TRUE(ctx.errors.empty());
    EXPECT_EQ(ctx.options.normal_flow_ltd, 3);
}

// This engine's own keys keep their lenient handling.
TEST(OptionsParserTest, EngineOwnKeyBadValueIsNotAnError) {
    SimulationContext ctx;
    parse(ctx, {"UNSTEADY_FRICTION  BOGUS", "QUALITY_SOLVER  BOGUS"});
    EXPECT_TRUE(ctx.errors.empty());
}


// Each row is also opened by the real legacy engine. This protects its distinct
// getDouble / atoi / atof / datetime rules, including permissive accepted text.
TEST(OptionsParserTest, NumericAndDateErrorsMatchLegacy) {
    struct Case { const char* row; int error; };
    const Case cases[] = {
        {"SLOPE_WEIGHTING BOGUS", 205}, {"SLOPE_WEIGHTING YES", 0},
        {"COMPATIBILITY 7", 205}, {"COMPATIBILITY 5junk", 205}, {"COMPATIBILITY 5", 0},
        {"OUTFALL_BACKFLOW_QUALITY BOGUS", 205}, {"OUTFALL_BACKFLOW_QUALITY LASTX", 0},
        {"MINIMUM_STEP abc", 211}, {"MINIMUM_STEP 1junk", 211},
        {"MINIMUM_STEP -1", 211}, {"MINIMUM_STEP 0:00:01", 211},
        {"MINIMUM_STEP 0", 0}, {"MINIMUM_STEP +0.5", 0},
        {"VARIABLE_STEP abc", 211}, {"VARIABLE_STEP -0.1", 211},
        {"VARIABLE_STEP 2.01", 211}, {"VARIABLE_STEP 0", 0}, {"VARIABLE_STEP 2", 0},
        {"MIN_SURFAREA abc", 211}, {"MIN_SURFAREA -1", 211}, {"MIN_SURFAREA 0", 0},
        {"MIN_SLOPE abc", 211}, {"MIN_SLOPE -1", 211},
        {"MIN_SLOPE 100", 211}, {"MIN_SLOPE 99.99", 0},
        {"HEAD_TOLERANCE abc", 211}, {"HEAD_TOLERANCE 1junk", 211},
        {"HEAD_TOLERANCE -1", 0}, {"HEAD_TOLERANCE +0.01", 0},
        {"SYS_FLOW_TOL abc", 211}, {"LAT_FLOW_TOL abc", 211},
        {"SYS_FLOW_TOL -1", 0}, {"LAT_FLOW_TOL -1", 0},
        {"THREADS -1", 211}, {"MAX_TRIALS -1", 211},
        {"THREADS abc", 0}, {"THREADS 1junk", 0},
        {"MAX_TRIALS abc", 0}, {"MAX_TRIALS 2.5", 0},
        {"DRY_DAYS -1", 211}, {"DRY_DAYS abc", 0}, {"DRY_DAYS 2junk", 0},
        {"ROUTING_STEP abc", 211}, {"ROUTING_STEP 0", 211},
        {"ROUTING_STEP -1", 211}, {"ROUTING_STEP 0:00:05", 0},
        {"ROUTING_STEP 1junk", 0}, {"ROUTING_STEP 1:", 0},
        {"LENGTHENING_STEP abc", 211}, {"LENGTHENING_STEP -1", 0},
        {"LENGTHENING_STEP 0:00:05", 0},
        {"WET_STEP abc", 213}, {"DRY_STEP abc", 213},
        {"REPORT_STEP abc", 213}, {"RULE_STEP abc", 213},
        {"WET_STEP 0", 211}, {"DRY_STEP 0", 211},
        {"REPORT_STEP 0", 211}, {"RULE_STEP 0", 0},
        {"RULE_STEP -1", 0}, {"RULE_STEP -24", 211}, {"RULE_STEP 0:00:00.4", 0},
        {"REPORT_STEP 0.5", 0}, {"REPORT_STEP 0:00:30junk", 0},
        {"REPORT_STEP 0:61:00", 0}, {"REPORT_STEP 25:00:00", 0},
        {"START_DATE abc", 213}, {"END_DATE 02/30/2020", 213},
        {"REPORT_START_DATE 02/29/2021", 213}, {"START_DATE 01012020", 213},
        {"START_DATE 01/01/2020junk", 0}, {"START_DATE JAN-01-2020", 0},
        {"START_DATE 1-1-2020", 0}, {"REPORT_START_DATE 02/29/2020", 0},
        {"START_TIME abc", 213}, {"END_TIME abc", 213},
        {"REPORT_START_TIME abc", 213}, {"START_TIME -1:00", 213},
        {"START_TIME 0.5", 0}, {"START_TIME 0:61:00", 0},
        {"SWEEP_START abc", 213}, {"SWEEP_END 13/01", 213},
        {"SWEEP_START 02/29", 213}, {"SWEEP_END 12/31", 0},
        {"SWEEP_START FEB-28", 0},
    };
    const std::filesystem::path dir = "_options_legacy_parity";
    std::filesystem::create_directories(dir);
    std::ifstream base_file("warnerr_base.inp");
    ASSERT_TRUE(base_file.good());
    const std::string base((std::istreambuf_iterator<char>(base_file)), {});
    int index = 0;
    for (const auto& c : cases) {
        SCOPED_TRACE(c.row);
        const auto stem = dir / std::to_string(index++);
        const std::string inp = stem.string() + ".inp";
        const std::string rpt = stem.string() + ".rpt";
        const std::string out = stem.string() + ".out";
        { std::ofstream f(inp); f << base << "\n[OPTIONS]\n" << c.row << "\n"; }
        const int legacy_rc = swmm_open(inp.c_str(), rpt.c_str(), out.c_str());
        swmm_close();
        std::ifstream report_file(rpt);
        const std::string report((std::istreambuf_iterator<char>(report_file)), {});
        if (c.error) {
            EXPECT_NE(legacy_rc, 0);
            EXPECT_NE(report.find("ERROR " + std::to_string(c.error)), std::string::npos) << report;
        } else {
            // Some accepted options can fail later model validation (e.g. a
            // report start beyond its end). Only parsing is compared here.
            EXPECT_EQ(report.find("ERROR 205"), std::string::npos) << report;
            EXPECT_EQ(report.find("ERROR 211"), std::string::npos) << report;
            EXPECT_EQ(report.find("ERROR 213"), std::string::npos) << report;
        }
        SimulationContext ctx;
        parse(ctx, {c.row});
        if (c.error) {
            ASSERT_EQ(ctx.errors.size(), 1u);
            EXPECT_NE(ctx.errors[0].find("ERROR " + std::to_string(c.error)), std::string::npos);
        } else {
            EXPECT_TRUE(ctx.errors.empty());
        }
    }
}

TEST(OptionsParserTest, LegacyPermissiveConversionsAndUnitsArePreserved) {
    SimulationContext ctx;
    parse(ctx, {"THREADS 2junk", "MAX_TRIALS 3.5", "DRY_DAYS 4junk",
                "REPORT_STEP 0.5", "WET_STEP 0:61:00", "DRY_STEP 25:00:00",
                "START_DATE JAN-01-2020", "START_TIME 0.5", "RULE_STEP 0",
                "LENGTHENING_STEP -10", "MIN_SLOPE 2", "SYS_FLOW_TOL 5", "LAT_FLOW_TOL 3"});
    ASSERT_TRUE(ctx.errors.empty());
    EXPECT_EQ(ctx.options.num_threads, 2);
    EXPECT_EQ(ctx.options.max_trials, 3);
    EXPECT_DOUBLE_EQ(ctx.options.dry_days, 4.0);
    EXPECT_DOUBLE_EQ(ctx.options.report_step, 1800.0);
    EXPECT_DOUBLE_EQ(ctx.options.wet_step, 3660.0);
    EXPECT_DOUBLE_EQ(ctx.options.dry_step, 90000.0);
    EXPECT_DOUBLE_EQ(ctx.options.start_date, 43831.0 + 0.5 / 24.0);
    EXPECT_DOUBLE_EQ(ctx.options.rule_step, 0.0);
    EXPECT_DOUBLE_EQ(ctx.options.lengthening_step, 0.0);
    EXPECT_DOUBLE_EQ(ctx.options.min_slope, 2.0); // stored in input percent, converted by resolver
    EXPECT_DOUBLE_EQ(ctx.options.sys_flow_tol, 0.05);
    EXPECT_DOUBLE_EQ(ctx.options.lat_flow_tol, 0.03);
}

TEST(OptionsParserTest, RefusedNumericAndDateValuesPreservePriorSettings) {
    SimulationContext ctx;
    parse(ctx, {"START_DATE 01/01/2020", "START_TIME 0.5", "VARIABLE_STEP 0.8",
                "START_DATE 02/30/2020", "START_TIME abc", "VARIABLE_STEP 3"});
    ASSERT_EQ(ctx.errors.size(), 3u);
    EXPECT_DOUBLE_EQ(ctx.options.start_date, 43831.0 + 0.5 / 24.0);
    EXPECT_DOUBLE_EQ(ctx.options.variable_step, 0.8);
}


TEST(OptionsParserTest, ExplicitFractionalStepClocksPreserveLegacyHoursAndRuleBehavior) {
    SimulationContext ctx;
    parse(ctx, {"REPORT_STEP 0:00:00.1", "WET_STEP 0:00:00.25", "DRY_STEP 0:01:30.125"});
    EXPECT_TRUE(ctx.errors.empty());
    EXPECT_DOUBLE_EQ(ctx.options.report_step,.1);
    EXPECT_DOUBLE_EQ(ctx.options.wet_step,.25);
    EXPECT_DOUBLE_EQ(ctx.options.dry_step,90.125);
    parse(ctx, {"REPORT_STEP 0.5", "RULE_STEP 0:00:00.4"});
    EXPECT_DOUBLE_EQ(ctx.options.report_step,1800);
    EXPECT_DOUBLE_EQ(ctx.options.rule_step,0);
    for(const char* value : {"0:00:-0.1", "0:00:0.0", "0:00:0.1junk", "0:00:1.1e309"}) {
        SimulationContext bad;
        parse(bad,{std::string("REPORT_STEP ")+value});
        EXPECT_FALSE(bad.errors.empty());
    }
}
