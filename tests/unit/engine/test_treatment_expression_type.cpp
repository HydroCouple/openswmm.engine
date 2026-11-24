/**
 * @file test_treatment_expression_type.cpp
 * @brief A pollutant name in a treatment expression reads the concentration
 *        the expression's own type calls for, in both engines.
 *
 * SWMM 5 Reference Manual Vol. III, section 5.4.2: a pollutant name means the
 * node's concentration before treatment in a concentration (C =) expression
 * and the inflow concentration in a removal (R =) expression. Both engines
 * chose by the named pollutant's own equation instead (legacy read a pollutant
 * with no equation as a removal, v6 as a concentration), which differs where a
 * node holds water. Benchmarks 193-h-h-si-units-elements treats TSS at a
 * storage unit with C = TSS * (1 - EXP(-11*Temperature)), Temperature untreated.
 *
 * Each storage unit below starts with 4 ft of water holding no B, so while it
 * fills B's node concentration stays far below its inflow concentration.
 *   S1: C = B, B untreated       -> A equals B's node concentration
 *   S2: C = B, B has R = 0       -> A equals B's node concentration
 *   S3: R = B / 1000, B has C = B -> R = 500 / 1000, A = 0.5 * 100 inflow
 *   S4: R = B / 1000, B untreated -> the same 50
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <string>

#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_nodes.h>
#include <openswmm/legacy/engine/openswmm_solver.h>

namespace fs = std::filesystem;

namespace {

const char* kOutDir =
    "treatment_expression_type_out"; // *_out/ is the gitignored scratch convention

const char* kDeck = R"([OPTIONS]
FLOW_UNITS           CFS
FLOW_ROUTING         KINWAVE
START_DATE           01/01/2020
START_TIME           00:00:00
END_DATE             01/01/2020
END_TIME             01:00:00
REPORT_STEP          00:05:00
ROUTING_STEP         00:00:30

[OUTFALLS]
O1  90  FREE
O2  90  FREE
O3  90  FREE
O4  90  FREE

[STORAGE]
S1  100  10  4  FUNCTIONAL  1000  0  0  0  0
S2  100  10  4  FUNCTIONAL  1000  0  0  0  0
S3  100  10  4  FUNCTIONAL  1000  0  0  0  0
S4  100  10  4  FUNCTIONAL  1000  0  0  0  0

[CONDUITS]
C1  S1  O1  400  0.013  0  0  0  0
C2  S2  O2  400  0.013  0  0  0  0
C3  S3  O3  400  0.013  0  0  0  0
C4  S4  O4  400  0.013  0  0  0  0

[XSECTIONS]
C1  CIRCULAR  1  0  0  0  1
C2  CIRCULAR  1  0  0  0  1
C3  CIRCULAR  1  0  0  0  1
C4  CIRCULAR  1  0  0  0  1

[POLLUTANTS]
A  MG/L  0  0  0  0  NO  *  0  0  1000
B  MG/L  0  0  0  0  NO  *  0  0  0

[TREATMENT]
S1  A  C = B
S2  B  R = 0
S2  A  C = B
S3  B  C = B
S3  A  R = B / 1000
S4  A  R = B / 1000

[INFLOWS]
S1  FLOW  ""  FLOW    1.0  1.0  2
S1  A     ""  CONCEN  1.0  1.0  1000
S1  B     ""  CONCEN  1.0  1.0  100
S2  FLOW  ""  FLOW    1.0  1.0  2
S2  A     ""  CONCEN  1.0  1.0  1000
S2  B     ""  CONCEN  1.0  1.0  100
S3  FLOW  ""  FLOW    1.0  1.0  2
S3  A     ""  CONCEN  1.0  1.0  100
S3  B     ""  CONCEN  1.0  1.0  500
S4  FLOW  ""  FLOW    1.0  1.0  2
S4  A     ""  CONCEN  1.0  1.0  100
S4  B     ""  CONCEN  1.0  1.0  500
)";

bool near(double a, double b) {
    return std::fabs(a - b) <= 1e-9 * std::max(1.0, std::fabs(b));
}

} // namespace

TEST(TreatmentExpressionType, PollutantNamesReadWhatTheExpressionTypeCallsFor) {
    fs::create_directories(kOutDir);
    const std::string inp = (fs::path(kOutDir) / "treatment_expression_type.inp").string();
    std::ofstream(inp) << kDeck;

    SWMM_Engine e = swmm_engine_create();
    ASSERT_EQ(swmm_engine_open(e, inp.c_str(), (inp + ".v6.rpt").c_str(), (inp + ".v6.out").c_str(),
                               nullptr),
              0)
        << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_initialize(e), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_engine_start(e, 0), 0) << swmm_get_last_error_msg(e);
    ASSERT_EQ(swmm_open(inp.c_str(), (inp + ".legacy.rpt").c_str(), ""), 0);
    ASSERT_EQ(swmm_start(0), 0);

    const char* names[] = {"S1", "S2", "S3", "S4"};
    int v6_idx[4], legacy_idx[4];
    for (int k = 0; k < 4; ++k) {
        v6_idx[k] = swmm_node_index(e, names[k]);
        legacy_idx[k] = swmm_getIndex(swmm_NODE, names[k]);
        ASSERT_GE(v6_idx[k], 0);
        ASSERT_GE(legacy_idx[k], 0);
    }

    double v6_elapsed = 0.0, legacy_elapsed = 0.0;
    for (int step = 1; step <= 20; ++step) {
        ASSERT_EQ(swmm_engine_step(e, &v6_elapsed), 0);
        ASSERT_EQ(swmm_step(&legacy_elapsed), 0);
        for (int k = 0; k < 4; ++k) {
            double a_v6 = 0.0, b_v6 = 0.0;
            ASSERT_EQ(swmm_node_get_quality(e, v6_idx[k], 0, &a_v6), 0);
            ASSERT_EQ(swmm_node_get_quality(e, v6_idx[k], 1, &b_v6), 0);
            const double a_legacy = swmm_getValueExpanded(
                swmm_NODE, swmm_NODE_POLLUTANT_CONCENTRATION, legacy_idx[k], 0, 0);
            const double b_legacy = swmm_getValueExpanded(
                swmm_NODE, swmm_NODE_POLLUTANT_CONCENTRATION, legacy_idx[k], 0, 1);
            if (k < 2) {
                // C = B: B's concentration at the node before treatment.
                EXPECT_TRUE(near(a_v6, b_v6))
                    << names[k] << " v6 step " << step << ": A " << a_v6 << " vs node B " << b_v6;
                EXPECT_TRUE(near(a_legacy, b_legacy))
                    << names[k] << " legacy step " << step << ": A " << a_legacy << " vs node B "
                    << b_legacy;
            } else {
                // R = B / 1000: B's inflow concentration, 500.
                EXPECT_TRUE(near(a_v6, 50.0)) << names[k] << " v6 step " << step << ": A " << a_v6;
                EXPECT_TRUE(near(a_legacy, 50.0))
                    << names[k] << " legacy step " << step << ": A " << a_legacy;
            }
            if (step == 1) {
                // The node is still mostly clean water: its B is far below the inflow's.
                EXPECT_LT(b_v6, 0.1 * (k < 2 ? 100.0 : 500.0)) << names[k];
            }
        }
    }

    swmm_end();
    swmm_close();
    swmm_engine_end(e);
    swmm_engine_close(e);
    swmm_engine_destroy(e);
}
