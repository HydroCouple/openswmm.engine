/*
 *   test_gage_scalefactor.cpp
 *
 *   Created: 2026
 *   Converted from Boost.Test to Google Test: 2026
 *
 *   Unit tests for the rain gage scale factor feature.
 *
 *   Model: data/gage_scalefactor.inp uses a single rain gage RG1 with
 *          INTENSITY format, TIMESERIES TS1, and scale factor = 2.0.
 *          data/gage_noscalefactor.inp is identical but omits the scale
 *          factor (defaults to 1.0).
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <string>

#include "openswmm_solver.h"

#define DATA_PATH_SCALE    "./data/gage_scalefactor.inp"
#define DATA_PATH_NOSCALE  "./data/gage_noscalefactor.inp"

namespace {

// Run a SWMM simulation end-to-end and return the final error code.
int runSimulation(const char* inpFile, const char* rptFile, const char* outFile) {
    int err;
    double elapsedTime;

    err = swmm_open(inpFile, rptFile, outFile);
    if (err) return err;

    err = swmm_start(1);
    if (err) { swmm_close(); return err; }

    do {
        err = swmm_step(&elapsedTime);
        if (err) { swmm_end(); swmm_close(); return err; }
    } while (elapsedTime > 0.0);

    err = swmm_end();
    if (err) { swmm_close(); return err; }

    swmm_close();
    return 0;
}

// Step a simulation and return the first non-zero rainfall reported by gage 0.
double captureFirstRainfall(const char* inpFile, const char* rptFile, const char* outFile) {
    double elapsedTime;
    double rainfall = 0.0;

    EXPECT_EQ(swmm_open(inpFile, rptFile, outFile), 0);
    EXPECT_EQ(swmm_start(1), 0);

    do {
        EXPECT_EQ(swmm_step(&elapsedTime), 0);
        if (elapsedTime > 0.0 && rainfall == 0.0) {
            double r = swmm_getValue(swmm_GAGE_RAINFALL, 0);
            if (r > 0.0) rainfall = r;
        }
    } while (elapsedTime > 0.0);

    swmm_end();
    swmm_close();
    return rainfall;
}

} // namespace

// Fixture cleans up generated report/output files after each test.
class GageScaleFactor : public ::testing::Test {
protected:
    void TearDown() override {
        for (const char* f : {
                 "noscale_test.rpt", "noscale_test.out",
                 "scale_test.rpt",   "scale_test.out",
                 "noscale_rf.rpt",   "noscale_rf.out",
                 "scale_rf.rpt",     "scale_rf.out",
             }) {
            std::remove(f);
        }
    }
};

// A model that omits the scale factor token must still run (backward compatibility).
TEST_F(GageScaleFactor, NoScaleFactorBackwardCompat) {
    int err = runSimulation(DATA_PATH_NOSCALE,
                            "noscale_test.rpt",
                            "noscale_test.out");
    EXPECT_EQ(err, 0);
}

// A model that supplies a scale factor of 2.0 must run successfully.
TEST_F(GageScaleFactor, WithScaleFactorRuns) {
    int err = runSimulation(DATA_PATH_SCALE,
                            "scale_test.rpt",
                            "scale_test.out");
    EXPECT_EQ(err, 0);
}

// A scale factor of 2.0 must double the reported rainfall intensity.
TEST_F(GageScaleFactor, ScaleFactorDoublesRainfall) {
    double rainfall_noscale = captureFirstRainfall(
        DATA_PATH_NOSCALE, "noscale_rf.rpt", "noscale_rf.out");
    double rainfall_scale = captureFirstRainfall(
        DATA_PATH_SCALE, "scale_rf.rpt", "scale_rf.out");

    ASSERT_GT(rainfall_noscale, 0.0);
    ASSERT_GT(rainfall_scale,   0.0);

    // BOOST_CHECK_CLOSE used 0.01% tolerance — translate to absolute.
    EXPECT_NEAR(rainfall_scale,
                2.0 * rainfall_noscale,
                std::abs(2.0 * rainfall_noscale) * 0.0001);
}
