/**
 * @file test_soft_rain_grid_parser.cpp
 * @brief Unit tests for [SOFT_RAINFALL_GRID] parsing (SR-2b).
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include "uncertainty/QualityUncertaintyHandler.hpp"
#include "uncertainty/UncertaintyConfig.hpp"

using openswmm::uncertainty::Coherence;
using openswmm::uncertainty::GridMapping;
using openswmm::uncertainty::GridTarget;
using openswmm::uncertainty::UncertaintyConfig;
using openswmm::uncertainty::parseSoftRainfallGridLine;

namespace {

TEST(SoftRainGridParser, Parses2DCentroidForceLocation) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "FORCE_LOCATION"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].target, GridTarget::TWO_D);
    EXPECT_EQ(cfg.grid_sources[0].mapping, GridMapping::CENTROID);
    EXPECT_TRUE(cfg.grid_sources[0].force_location);
    EXPECT_EQ(cfg.grid_sources[0].file_path, "radar_jul.h5");
    EXPECT_TRUE(cfg.grid_sources[0].nodes_file.empty());
}

TEST(SoftRainGridParser, ParsesRunoffAreaMean) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "RUNOFF", "radar_jul.h5", "AREA_MEAN"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].target, GridTarget::RUNOFF);
    EXPECT_EQ(cfg.grid_sources[0].mapping, GridMapping::AREA_MEAN);
    EXPECT_FALSE(cfg.grid_sources[0].force_location);
}

TEST(SoftRainGridParser, ParsesInflowsNodesOption) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "INFLOWS", "radar_jul.h5", "CENTROID", "NODES", "node_list.txt"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].target, GridTarget::INFLOWS);
    EXPECT_EQ(cfg.grid_sources[0].nodes_file, "node_list.txt");
}

TEST(SoftRainGridParser, RejectsUnknownTarget) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "FOO", "radar_jul.h5", "CENTROID"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Unknown Target"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsUnknownMapping) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "NEAREST"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Unknown Mapping"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsNodesWithoutFile) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "INFLOWS", "radar_jul.h5", "CENTROID", "NODES"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("requires a file path"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsUnknownOption) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "BOGUS"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("Unknown option"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

// ---------------------------------------------------------------------------
// CL-1a: COHERENCE option parsing (design §6)
// ---------------------------------------------------------------------------

TEST(SoftRainGridParser, AbsentCoherenceIsComonotoneDefault) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].coherence, Coherence::FULL);
    EXPECT_DOUBLE_EQ(cfg.grid_sources[0].corr_len, 0.0);
}

TEST(SoftRainGridParser, ParsesCoherenceFull) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "FULL"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].coherence, Coherence::FULL);
    EXPECT_DOUBLE_EQ(cfg.grid_sources[0].corr_len, 0.0);
}

TEST(SoftRainGridParser, ParsesCoherenceCorrLen) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "CORR_LEN", "500"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].coherence, Coherence::CORR_LEN);
    EXPECT_DOUBLE_EQ(cfg.grid_sources[0].corr_len, 500.0);
}

TEST(SoftRainGridParser, CorrLenWorksAlongsideOtherOptions) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "INFLOWS", "radar_jul.h5", "BILINEAR", "FORCE_LOCATION",
        "NODES", "nlist.txt", "COHERENCE", "CORR_LEN", "250"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_TRUE(err.empty()) << err;
    ASSERT_EQ(cfg.grid_sources.size(), 1u);
    EXPECT_EQ(cfg.grid_sources[0].coherence, Coherence::CORR_LEN);
    EXPECT_DOUBLE_EQ(cfg.grid_sources[0].corr_len, 250.0);
    EXPECT_TRUE(cfg.grid_sources[0].force_location);
    EXPECT_EQ(cfg.grid_sources[0].nodes_file, "nlist.txt");
}

TEST(SoftRainGridParser, RejectsCorrLenNegative) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "CORR_LEN", "-5"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("CORR_LEN must be a positive"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsCorrLenNonNumeric) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "CORR_LEN", "abc"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("numeric"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsCorrLenZero) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "CORR_LEN", "0"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("CORR_LEN must be a positive"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsCorrLenWithoutValue) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "CORR_LEN"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("CORR_LEN requires"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsCoherenceBogusMode) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE", "BOGUS"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("not yet supported"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

TEST(SoftRainGridParser, RejectsCoherenceWithoutMode) {
    UncertaintyConfig cfg;
    const std::vector<std::string> tokens = {
        "2D", "radar_jul.h5", "CENTROID", "COHERENCE"
    };

    std::string err = parseSoftRainfallGridLine(tokens, cfg);
    EXPECT_FALSE(err.empty());
    EXPECT_NE(err.find("COHERENCE requires a mode"), std::string::npos);
    EXPECT_TRUE(cfg.grid_sources.empty());
}

} // anonymous namespace
