/*
 *   test_solver_storage_crown_warning.cpp
 *
 *   A link opening above a storage node's maximum depth is WARNING 14
 *   (issue #6, USEPA #207).
 *
 *   link_validate raises a node's full depth to the top of each connecting
 *   link, and node_validate reports that as WARNING 02, but a storage node
 *   without a surcharge depth keeps its own full depth, so the same weir that
 *   warns at a junction said nothing at a storage node. The deck is the one
 *   from USEPA #207 plus three storage nodes: one with a surcharge depth (it is
 *   raised, WARNING 02), one fed by a conduit whose crown is above it (WARNING
 *   14) and one whose weir tops out exactly at its maximum depth (no warning).
 *
 *   Working directory is tests/unit/legacy/engine/data; the deck is shared
 *   with the 6.0.0 test WarningErrorParity.LinkAboveStorageMaxDepthWarns14.
 */

#include <gtest/gtest.h>

#include <fstream>
#include <sstream>
#include <string>

#include "openswmm_solver.h"

#define ED "../../../engine/data/"

TEST(SolverStorageCrownWarning, LinkAboveStorageMaxDepthIsWarning14) {
    ASSERT_EQ(swmm_open(ED "warnerr_storage_crown.inp", ED "_storage_crown_legacy.rpt",
                        ED "_storage_crown_legacy.out"),
              0);
    swmm_close();

    std::ifstream rpt(ED "_storage_crown_legacy.rpt");
    std::stringstream ss;
    ss << rpt.rdbuf();
    const std::string text = ss.str();
    auto has = [&](const std::string& s) { return text.find(s) != std::string::npos; };

    const std::string w02 = "WARNING 02: maximum depth increased for Node ";
    const std::string w14 = "WARNING 14: link opening extends above maximum depth of Storage Node ";
    for (const char* id : {"W2Chamber", "SurChamber"}) {
        EXPECT_TRUE(has(w02 + id)) << id << "\n" << text;
        EXPECT_FALSE(has(w14 + id)) << id << "\n" << text;
    }
    for (const char* id : {"WeirChamber", "LowPond"}) {
        EXPECT_TRUE(has(w14 + id)) << id << "\n" << text;
        EXPECT_FALSE(has(w02 + id)) << id << "\n" << text;
    }
    EXPECT_FALSE(has(w02 + "OkChamber")) << text;
    EXPECT_FALSE(has(w14 + "OkChamber")) << text;
}
