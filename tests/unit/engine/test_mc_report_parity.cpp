// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      test_mc_report_parity.cpp
 * @brief     Tests for mc report parity.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include <gtest/gtest.h>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace {
/**
 * @brief Read a report into owned text using platform text mode.
 *
 * Stream closes on return, and allocation exceptions propagate; no report parsing or engine
 * mutation.
 *
 * @param[in] path Report filename borrowed for this synchronous read.
 * @return Collected text; empty may mean empty file/open failure, with no separate partial-read
 *     status.
 */
std::string readReport(const std::string& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/**
 * @brief Extract normalized nonempty lines from a known report section layout.
 *
 * Assumes the expected two-line title layout exists; malformed found headings can cause substr to
 * throw. Collapses words to single spaces with a trailing space and drops blank lines; no numeric
 * tolerance or semantic report parser.
 *
 * @param[in] report Complete report text with LF line boundaries.
 * @param[in] title Literal title identifying a section followed by its closing star line.
 * @return Owned whitespace-normalized lines until the next star heading or analysis footer; empty
 *     when title absent.
 */
std::vector<std::string> section(const std::string& report, const std::string& title) {
    const auto start = report.find(title);
    if (start == std::string::npos)
        return {};
    auto body = report.find('\n', start);
    body = report.find('\n', body + 1); // skip the closing star line
    auto end = report.find("\n  ***", body);
    const auto footer = report.find("\n  Analysis begun on:", body);
    if (footer < end)
        end = footer;
    std::istringstream lines(report.substr(body, end - body));
    std::vector<std::string> result;
    for (std::string line; std::getline(lines, line);) {
        std::istringstream words(line);
        std::string normalized, word;
        while (words >> word)
            normalized += word + " ";
        if (!normalized.empty())
            result.push_back(normalized);
    }
    return result;
}

/**
 * @brief Run legacy/current models and compare selected normalized report sections.
 *
 * Requires exclusive global legacy-engine use. Runs legacy swmm_run then owns a new engine,
 * stepping without a guard and explicitly generating its report. Fatal assertions before
 * destruction can leave the new handle allocated. On success destroys it, reads both reports and
 * records exact line equality; files remain and unrelated sections are not compared.
 *
 * @param[in] base Controlled existing model prefix without .inp; overwrites .report_legacy and
 *     .report_new artifacts.
 * @param[in] titles Exact report titles whose legacy normalized bodies must be nonempty.
 */
void compareSections(const std::string& base, const std::vector<std::string>& titles) {
    ASSERT_EQ(swmm_run((base + ".inp").c_str(), (base + ".report_legacy.rpt").c_str(),
                       (base + ".report_legacy.out").c_str()),
              0);
    SWMM_Engine engine = swmm_engine_create();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(swmm_engine_open(engine, (base + ".inp").c_str(), (base + ".report_new.rpt").c_str(),
                               (base + ".report_new.out").c_str(), nullptr),
              SWMM_OK);
    ASSERT_EQ(swmm_engine_initialize(engine), SWMM_OK);
    ASSERT_EQ(swmm_engine_start(engine, 1), SWMM_OK);
    double elapsed = 0.0;
    do {
        ASSERT_EQ(swmm_engine_step(engine, &elapsed), SWMM_OK);
    } while (elapsed > 0.0);
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
    compareSections("mc_storm_parity/report_statistics",
                    {"Subcatchment Summary", "Subcatchment Runoff Summary", "Link Flow Summary",
                     "Pumping Summary", "Node Flooding Summary", "Storage Volume Summary",
                     "Flow Classification Summary"});
}

TEST(MCReportParity, GroundwaterTableUsesAbsoluteElevationAndLegacyPrecision) {
    compareSections("mc_storm_parity/groundwater_month_horton", {"Groundwater Summary"});
}
