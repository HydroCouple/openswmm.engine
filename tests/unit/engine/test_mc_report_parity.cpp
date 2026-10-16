// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {
std::string readReport(const std::string& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::vector<std::string> section(const std::string& report, const std::string& title) {
    const auto start = report.find(title);
    if (start == std::string::npos) return {};
    auto body = report.find('\n', start);
    body = report.find('\n', body + 1); // skip the closing star line
    auto end = report.find("\n  ***", body);
    const auto footer = report.find("\n  Analysis begun on:", body);
    if (footer < end) end = footer;
    std::istringstream lines(report.substr(body, end - body));
    std::vector<std::string> result;
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string normalized, word;
        while (words >> word) normalized += word + " ";
        if (!normalized.empty()) result.push_back(normalized);
    }
    return result;
}

void compareSections(const std::string& base, const std::vector<std::string>& titles) {
    ASSERT_EQ(swmm_run((base + ".inp").c_str(),
                      (base + ".report_legacy.rpt").c_str(),
                      (base + ".report_legacy.out").c_str()), 0);
    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(swmm_engine_open(engine, (base + ".inp").c_str(),
                              (base + ".report_new.rpt").c_str(),
                              (base + ".report_new.out").c_str(), nullptr), SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine, 1), SWMM_OK);
    double elapsed = 0.0;
    do { ASSERT_EQ(swmm_engine_step(engine, &elapsed), SWMM_OK); }
    while (elapsed > 0.0);
    ASSERT_EQ(swmm_engine_end(engine), SWMM_OK);
    ASSERT_EQ(swmm_engine_report(engine), SWMM_OK);
    swmm_engine_destroy(engine);
    const auto legacy = readReport(base + ".report_legacy.rpt");
    const auto current = readReport(base + ".report_new.rpt");
    for (const auto& title : titles) {
        const auto expected = section(legacy, title);
        ASSERT_FALSE(expected.empty()) << title;
        EXPECT_EQ(expected, section(current, title)) << title;
    }
}
}

TEST(MCReportParity, RunonPumpFloodStorageAndFlowClassSummariesMatchLegacy) {
    compareSections("mc_storm_parity/report_statistics", {
        "Subcatchment Summary", "Subcatchment Runoff Summary", "Link Flow Summary",
        "Pumping Summary", "Node Flooding Summary", "Storage Volume Summary",
        "Flow Classification Summary"});
}

TEST(MCReportParity, GroundwaterTableUsesAbsoluteElevationAndLegacyPrecision) {
    compareSections("mc_storm_parity/groundwater_month_horton", {"Groundwater Summary"});
}
