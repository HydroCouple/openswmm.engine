/**
 * @file test_soft_rain_grid_parser.cpp
 * @brief Unit tests for [SOFT_RAINFALL_GRID] parsing (SR-2b).
 *
 * @ingroup engine_uncertainty
 */

#include <gtest/gtest.h>

#include "uncertainty/QualityUncertaintyHandler.hpp"
#include "uncertainty/UncertaintyConfig.hpp"

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

} // anonymous namespace
