// SPDX-License-Identifier: Apache-2.0
/**
 * @file test_xsect_error_names_link.cpp
 * @brief Invalid cross-section geometry names its conduit (review IO-13, issue #175).
 *
 * LinksHandler.cpp (zero Geom1) and PostParseResolver.cpp (any geometry
 * setParams() refuses) reported ERROR 211 "invalid number" with an empty
 * token, so the message said neither which conduit nor what was wrong. Check
 * from IO-13_test6.c of HydroCouple/swmm-code-review (e13c013): a deck with
 * three conduits each refused for a different reason (FILLED_CIRCULAR with
 * sediment deeper than the pipe, CIRCULAR 0, HORIZ_ELLIPSE size code 99) is
 * rejected, and each conduit's name appears as a whole word in a report line
 * that starts with ERROR.
 *
 * @note Working directory is tests/unit/engine/data (set by CMakeLists).
 */

#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include <openswmm/engine/openswmm_engine.h>

namespace {

bool namesWholeWord(const std::string& text, const std::string& id) {
    for (std::size_t p = text.find(id); p != std::string::npos; p = text.find(id, p + id.size())) {
        const bool before = p == 0 || !std::isalnum(static_cast<unsigned char>(text[p - 1]));
        const std::size_t q = p + id.size();
        const bool after = q >= text.size() || (!std::isalnum(static_cast<unsigned char>(text[q])) && text[q] != '_');
        if (before && after) return true;
    }
    return false;
}

}  // namespace

TEST(XsectErrorNamesLink, EveryRefusedConduitIsNamedInItsErrorLine) {
    EXPECT_NE(swmm_engine_run("xsect_review/IO-13_bad-xsections.inp", "xsect_review/_IO-13.rpt",
                              "xsect_review/_IO-13.out", nullptr),
              SWMM_OK)
        << "a deck whose every conduit has invalid geometry must be rejected";

    std::vector<std::string> errors;
    std::ifstream rpt("xsect_review/_IO-13.rpt");
    for (std::string line; std::getline(rpt, line);) {
        const auto p = line.find("ERROR");
        if (p != std::string::npos) errors.push_back(line.substr(p));
    }
    ASSERT_FALSE(errors.empty()) << "no ERROR line in the report";
    for (const char* id : {"C1", "C2", "C3"}) {
        bool named = false;
        for (const auto& e : errors) named = named || namesWholeWord(e, id);
        EXPECT_TRUE(named) << id << " is not named by any ERROR line; first is: " << errors.front();
    }
}
