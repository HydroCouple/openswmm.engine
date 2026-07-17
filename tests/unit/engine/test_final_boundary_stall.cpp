/**
 * @file test_final_boundary_stall.cpp
 * @brief Test for PR 11 - Engine: final-boundary time-stepping stall fix
 *
 * This test verifies that the engine no longer stalls making sub-nanosecond 
 * time progress at the final report boundary with adaptive routing active.
 */

#include "gtest/gtest.h"
#include "openswmm/engine/openswmm_engine.h"
#include <cmath>
#include <limits>
#include <string>
#include <fstream>
#include <iostream>

namespace {

// Test fixture for final boundary stall regression test
class FinalBoundaryStallTest : public ::testing::Test {
protected:
    SWMM_Engine engine_;

    void SetUp() override {
        engine_ = swmm_engine_create();
        ASSERT_TRUE(engine_ != nullptr);
    }

    void TearDown() override {
        if (engine_) {
            swmm_engine_destroy(engine_);
            engine_ = nullptr;
        }
    }
    
    // Simple function to find a test input file
    std::string findTestInputFile() {
        // Try common locations for test files
        std::string paths[] = {
            "data/site_drainage_model.inp",
            "../data/site_drainage_model.inp",
            "tests/unit/engine/data/site_drainage_model.inp",
            "../../tests/unit/engine/data/site_drainage_model.inp"
        };
        
        for (const auto& path : paths) {
            FILE* file = fopen(path.c_str(), "r");
            if (file) {
                fclose(file);
                return path;
            }
        }
        return ""; // File not found
    }
    
    // Create a simple test input file
    std::string createTestInputFile() {
        std::string inp_path = "/tmp/test_stall.inp";
        std::ofstream f(inp_path);
        f << ";; Test input file for final boundary stall test\n";
        f << "[TITLE]\n";
        f << "Test network for PR 11 stall fix\n";
        f << "\n[OPTIONS]\n";
        f << "FLOW_UNITS           CFS\n";
        f << "INFILTRATION         HORTON\n";
        f << "FLOW_ROUTING         DYNWAVE\n";
        f << "START_DATE           01/01/2026\n";
        f << "START_TIME           00:00:00\n";
        f << "REPORT_START_DATE    01/01/2026\n";
        f << "REPORT_START_TIME    00:00:00\n";
        f << "END_DATE             01/01/2026\n";
        f << "END_TIME             00:10:00\n";
        f << "SWEEP_START          01/01\n";
        f << "SWEEP_END            12/31\n";
        f << "DRY_DAYS             0\n";
        f << "REPORT_STEP          00:01:00\n";
        f << "WET_STEP             00:00:30\n";
        f << "DRY_STEP             00:01:00\n";
        f << "ROUTING_STEP         00:00:05\n";
        f << "ALLOW_PONDING        NO\n";
        f << "INERTIAL_DAMPING     PARTIAL\n";
        f << "VARIABLE_STEP        0.75\n";
        f << "LENGTHENING_STEP     0\n";
        f << "MIN_SURFAREA         0\n";
        f << "COMPATIBILITY        SWMM5\n";
        f << "SKIP_STEADY_STATE    NO\n";
        f << "TEMPERATURE          NO\n";
        f << "MAX_TRIALS           8\n";
        f << "HEAD_TOLERANCE       0.005\n";
        f << "SYS_FLOW_TOL         5\n";
        f << "LAT_FLOW_TOL         5\n";
        f << "MINIMUM_STEP         0.5\n";
        f << "THREADS              1\n";
        f << "\n[EVAPORATION]\n";
        f << ";;Data Source    Parameters\n";
        f << ";;-------------- ----------\n";
        f << "CONSTANT         0.0\n";
        f << "\n[RAINGAGES]\n";
        f << ";;Name           Format    Interval  SCF       Source\n";
        f << ";;----           ------    --------  ---       ------\n";
        f << "RG1              INTENSITY 0:05:00   1.0       TIMESERIES TS1\n";
        f << "\n[SUBCATCHMENTS]\n";
        f << ";;Name    Rain Gage   Outlet    Area    %Imperv   Width   %Slope   CurbLen  Snow\n";
        f << ";;----    ---------   ------    ----    -------   -----   ------   -------  ----\n";
        f << "S1        RG1         J1        10      50        500     0.5      0\n";
        f << "\n[SUBAREAS]\n";
        f << ";;Name    N-Imperv  N-Perv    S-Imperv  S-Perv    PctZero   RouteTo    PctRouted\n";
        f << ";;----    --------  ------    --------  ------    -------   -------    ---------\n";
        f << "S1        0.01      0.1       0.05      0.05      25        OUTLET     100\n";
        f << "\n[INFILTRATION]\n";
        f << ";;Subcatchment  MaxRate   MinRate   Decay   DryTime  MaxInfil\n";
        f << ";;------------  -------   -------   -----   -------  --------\n";
        f << "S1              3.0       0.5       4       7        0\n";
        f << "\n[JUNCTIONS]\n";
        f << ";;Name  Elev.  MaxDepth  InitDepth  SurDepth  Aponded\n";
        f << ";;----  -----  --------  ---------  --------  -------\n";
        f << "J1      100    0         0          0         0\n";
        f << "J2      95     0         0          0         0\n";
        f << "\n[OUTFALLS]\n";
        f << ";;Name  Elev.  Type        Stage Data\n";
        f << ";;----  -----  ----        ----------\n";
        f << "O1      90     FREE                        \n";
        f << "\n[LINKS]\n";
        f << ";;Name  From  To  Type  Length  Width  Roughness  InOffset  OutOffset\n";
        f << ";;----  ----  --  ----  ------  -----  ---------  --------  ---------\n";
        f << "C1      J1    J2  CONDUIT  1000    10     0.013      0         0\n";
        f << "C2      J2    O1  CONDUIT  1000    10     0.013      0         0\n";
        f << "\n[XSECTIONS]\n";
        f << ";;Link  Shape  Geom1  Geom2  Geom3  Geom4  Barrels  Culvert\n";
        f << ";;----  -----  -----  -----  -----  -----  -------  -------\n";
        f << "C1      CIRCULAR  3\n";
        f << "C2      CIRCULAR  3\n";
        f << "\n[TIMESERIES]\n";
        f << ";;Name  Date    Time  Value\n";
        f << ";;----  ----    ----  -----\n";
        f << "TS1     FILE  AND  UCF\n";
        f << "\n[REPORT]\n";
        f << "INPUT      NO\n";
        f << "CONTROLS   NO\n";
        f << "SUBCATCHMENTS ALL\n";
        f << "NODES ALL\n";
        f << "LINKS ALL\n";
        f << "CONTINUITY YES\n";
        f << "FLOWSTATS NO\n";
        f << "CONTROLS NO\n";
        f.close();
        return inp_path;
    }
};

// Test that the engine doesn't stall at the final boundary
TEST_F(FinalBoundaryStallTest, NoStallAtFinalBoundary) {
    std::cout << "Starting test..." << std::endl;
    // Open the engine with the PR 11 stall test fixture (END_TIME on a report
    // boundary, adaptive stepping via ROUTING_STEP=10s + MINIMUM_STEP=0.5s).
    int err = swmm_engine_open(engine_, 
                               "pr11_stall_test.inp", 
                               "/tmp/pr11_stall_test.rpt", 
                               "", 
                               nullptr);
    std::cout << "swmm_engine_open returned: " << err << std::endl;
    ASSERT_EQ(err, SWMM_OK) << "Failed to open engine: " << swmm_get_last_error_msg(engine_);
    
    // Initialize the engine
    err = swmm_engine_initialize(engine_);
    std::cout << "swmm_engine_initialize returned: " << err << std::endl;
    ASSERT_EQ(err, SWMM_OK);
    
    // Start the simulation
    err = swmm_engine_start(engine_, 0);
    std::cout << "swmm_engine_start returned: " << err << std::endl;
    ASSERT_EQ(err, SWMM_OK);
    
    // Run the simulation and check that it completes without stalling
    double elapsed = 1.0;
    int step_count = 0;
    const int max_steps = 30000;  // Should be enough for the 30-hour simulation with 5s steps
    
    // Run the simulation
    do {
        err = swmm_engine_step(engine_, &elapsed);
        std::cout << "swmm_engine_step returned: " << err << " at step: " << step_count << " elapsed: " << elapsed << std::endl;
        ASSERT_EQ(err, SWMM_OK) << "step failed at step " << step_count << ": " << swmm_get_last_error_msg(engine_);
        step_count++;

        // Safety limit — the model should complete in well under 30k steps
        ASSERT_LT(step_count, max_steps) << "Simulation did not terminate";
    } while (elapsed > 0.0);
    
    // Simulation should have run for a reasonable number of steps
    EXPECT_GT(step_count, 1) << "Too few steps — simulation may not have run";
    EXPECT_LT(step_count, 1000) << "Too many steps — possible stall";
    std::cout << "Simulation completed in " << step_count << " steps" << std::endl;

    // End the simulation
    err = swmm_engine_end(engine_);
    ASSERT_EQ(err, SWMM_OK);
    
    // Close the engine
    err = swmm_engine_close(engine_);
    ASSERT_EQ(err, SWMM_OK);
}

} // namespace