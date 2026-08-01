/**
 * @file test_soft_rain_parser.cpp
 * @brief Unit tests for [SOFT_RAINGAGES] parsing (SR-1a).
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include "core/SimulationContext.hpp"
#include "uncertainty/QualityUncertaintyHandler.hpp"
#include "uncertainty/SoftRainData.hpp"

using openswmm::SimulationContext;
using openswmm::uncertainty::DistType;
using openswmm::uncertainty::GageCoherence;
using openswmm::uncertainty::SoftSpreadKind;
using openswmm::uncertainty::parseSoftRaingagesLine;

namespace {

TEST(SoftRainParser, ParsesConstantCvLognormal) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(ctx, {"RG1", "LOGNORMAL", "CV", "0.25"});
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(ctx.soft_rain.count(), 1);
    EXPECT_TRUE(ctx.soft_rain.configured[0]);
    EXPECT_EQ(ctx.soft_rain.family[0], DistType::LOGNORMAL);
    EXPECT_EQ(ctx.soft_rain.spread_kind[0], SoftSpreadKind::CV);
    EXPECT_DOUBLE_EQ(ctx.soft_rain.spread_const[0], 0.25);
    EXPECT_EQ(ctx.soft_rain.spread_ts[0], -1);
}

TEST(SoftRainParser, ParsesTimeseriesSpread) {
    SimulationContext ctx;
    ctx.gage_names.add("RG2");
    ctx.table_names.add("sd_rg2");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(ctx, {"RG2", "NORMAL", "SD", "TIMESERIES", "sd_rg2"});
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(ctx.soft_rain.configured[0]);
    EXPECT_EQ(ctx.soft_rain.family[0], DistType::NORMAL);
    EXPECT_EQ(ctx.soft_rain.spread_kind[0], SoftSpreadKind::SD);
    EXPECT_EQ(ctx.soft_rain.spread_ts_name[0], "sd_rg2");
    EXPECT_EQ(ctx.soft_rain.spread_ts[0], 0);
}

TEST(SoftRainParser, RejectsUnknownGage) {
    SimulationContext ctx;
    std::string err = parseSoftRaingagesLine(ctx, {"RG404", "NORMAL", "SD", "0.1"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Unknown gage"), std::string::npos);
}

TEST(SoftRainParser, RejectsHalfrangeWithNonUniform) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(ctx, {"RG1", "NORMAL", "HALFRANGE", "0.1"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("HALFRANGE"), std::string::npos);
}

TEST(SoftRainParser, RejectsCvWithUniform) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(ctx, {"RG1", "UNIFORM", "CV", "0.1"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("SD/CV"), std::string::npos);
}

TEST(SoftRainParser, RejectsNegativeSpread) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(ctx, {"RG1", "NORMAL", "SD", "-0.1"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("non-negative"), std::string::npos);
}

TEST(SoftRainParser, UnlistedGageRemainsHardData) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.gage_names.add("RG2");
    ctx.soft_rain.grow_to(2);

    std::string err = parseSoftRaingagesLine(ctx, {"RG1", "NORMAL", "SD", "0.1"});
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_TRUE(ctx.soft_rain.configured[0]);
    EXPECT_FALSE(ctx.soft_rain.configured[1]);
}

// ---------------------------------------------------------------------------
// CL-1a: COHERENCE option parsing for [SOFT_RAINGAGES] (design §6)
// ---------------------------------------------------------------------------

TEST(SoftRainParser, AbsentCoherenceIsComonotoneDefault) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(ctx, {"RG1", "LOGNORMAL", "CV", "0.25"});
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(ctx.soft_rain.coherence[0], GageCoherence::FULL);
    EXPECT_DOUBLE_EQ(ctx.soft_rain.corr_len[0], 0.0);
}

TEST(SoftRainParser, ParsesCoherenceFull) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "FULL"});
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(ctx.soft_rain.coherence[0], GageCoherence::FULL);
    EXPECT_DOUBLE_EQ(ctx.soft_rain.corr_len[0], 0.0);
}

TEST(SoftRainParser, ParsesCoherenceCorrLen) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "CORR_LEN", "500"});
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(ctx.soft_rain.coherence[0], GageCoherence::CORR_LEN);
    EXPECT_DOUBLE_EQ(ctx.soft_rain.corr_len[0], 500.0);
}

TEST(SoftRainParser, ParsesCorrLenWithTimeseriesSpread) {
    SimulationContext ctx;
    ctx.gage_names.add("RG2");
    ctx.table_names.add("sd_rg2");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG2", "NORMAL", "SD", "TIMESERIES", "sd_rg2", "COHERENCE", "CORR_LEN", "300"});
    EXPECT_TRUE(err.empty()) << err;
    EXPECT_EQ(ctx.soft_rain.coherence[0], GageCoherence::CORR_LEN);
    EXPECT_DOUBLE_EQ(ctx.soft_rain.corr_len[0], 300.0);
    EXPECT_EQ(ctx.soft_rain.spread_ts_name[0], "sd_rg2");
}

TEST(SoftRainParser, RejectsCorrLenNegative) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "CORR_LEN", "-5"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("CORR_LEN must be a positive"), std::string::npos);
    EXPECT_EQ(ctx.soft_rain.coherence[0], GageCoherence::FULL);
    EXPECT_DOUBLE_EQ(ctx.soft_rain.corr_len[0], 0.0);
}

TEST(SoftRainParser, RejectsCorrLenNonNumeric) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "CORR_LEN", "abc"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("numeric"), std::string::npos);
}

TEST(SoftRainParser, RejectsCorrLenZero) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "CORR_LEN", "0"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("CORR_LEN must be a positive"), std::string::npos);
}

TEST(SoftRainParser, RejectsCorrLenWithoutValue) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "CORR_LEN"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("CORR_LEN requires"), std::string::npos);
}

TEST(SoftRainParser, RejectsCoherenceBogusMode) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE", "BOGUS"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not yet supported"), std::string::npos);
}

TEST(SoftRainParser, RejectsCoherenceWithoutMode) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "COHERENCE"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("COHERENCE requires a mode"), std::string::npos);
}

TEST(SoftRainParser, RejectsUnknownOptionAfterSpread) {
    SimulationContext ctx;
    ctx.gage_names.add("RG1");
    ctx.soft_rain.grow_to(1);

    std::string err = parseSoftRaingagesLine(
        ctx, {"RG1", "LOGNORMAL", "CV", "0.25", "BOGUS"});
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Unknown option"), std::string::npos);
}

} // anonymous namespace
