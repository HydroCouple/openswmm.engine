/*
 *   test_solver_xsect_error_names_link.cpp
 *
 *   Invalid cross-section geometry names its conduit (review IO-13, issue #175).
 *
 *   link.c reported geometry xsect_setParams() refuses as ERROR 211 "invalid
 *   number" with an empty token, so the message said neither which conduit
 *   nor what was wrong. Check from IO-13_test.c of HydroCouple/swmm-code-review
 *   (e13c013): a deck with three conduits each refused for a different reason
 *   (FILLED_CIRCULAR with sediment deeper than the pipe, CIRCULAR 0,
 *   HORIZ_ELLIPSE size code 99) is rejected, and each conduit's name appears
 *   as a whole word in a report line that starts with ERROR. The echoed input
 *   line below the message is not counted.
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test under tests/unit/engine/data/xsect_review.
 */

#include <gtest/gtest.h>

#include <cctype>
#include <fstream>
#include <string>
#include <vector>

#include "openswmm_solver.h"

#define XR "../../../engine/data/xsect_review/"

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

TEST(SolverXsectErrorNamesLink, EveryRefusedConduitIsNamedInItsErrorLine) {
    EXPECT_NE(swmm_open(XR "IO-13_bad-xsections.inp", XR "_IO-13_legacy.rpt", XR "_IO-13_legacy.out"), 0)
        << "a deck whose every conduit has invalid geometry must be rejected";
    swmm_close();

    std::vector<std::string> errors;
    std::ifstream rpt(XR "_IO-13_legacy.rpt");
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
