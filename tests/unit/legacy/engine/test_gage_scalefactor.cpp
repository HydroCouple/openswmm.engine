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

// C API getter must return the value parsed from the INP file.
TEST_F(GageScaleFactor, CApiGetReturnsParsedValue) {
    ASSERT_EQ(swmm_open(DATA_PATH_SCALE, "capi_get.rpt", "capi_get.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);

    double sf = swmm_getValue(swmm_GAGE_SCALEFACTOR, 0);
    EXPECT_DOUBLE_EQ(sf, 2.0);

    swmm_end();
    swmm_close();

    std::remove("capi_get.rpt");
    std::remove("capi_get.out");
}

// C API getter must return the default 1.0 when the INP omits the token.
TEST_F(GageScaleFactor, CApiGetReturnsDefaultOneWhenUnset) {
    ASSERT_EQ(swmm_open(DATA_PATH_NOSCALE, "capi_get_default.rpt", "capi_get_default.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);

    double sf = swmm_getValue(swmm_GAGE_SCALEFACTOR, 0);
    EXPECT_DOUBLE_EQ(sf, 1.0);

    swmm_end();
    swmm_close();

    std::remove("capi_get_default.rpt");
    std::remove("capi_get_default.out");
}

// Setting via the C API on a no-scale-factor model must double rainfall in the
// same way that parsing scale_factor=2.0 from the INP does.
TEST_F(GageScaleFactor, CApiSetTakesEffect) {
    double rainfall_baseline = captureFirstRainfall(
        DATA_PATH_NOSCALE, "capi_set_base.rpt", "capi_set_base.out");
    ASSERT_GT(rainfall_baseline, 0.0);

    // Open the unscaled model, set scale_factor = 2.0 via API, then step.
    ASSERT_EQ(swmm_open(DATA_PATH_NOSCALE, "capi_set.rpt", "capi_set.out"), 0);
    ASSERT_EQ(swmm_setValue(swmm_GAGE_SCALEFACTOR, 0, 2.0), 0);
    ASSERT_EQ(swmm_start(1), 0);

    double rainfall_scaled = 0.0;
    double elapsed;
    do {
        ASSERT_EQ(swmm_step(&elapsed), 0);
        if (elapsed > 0.0 && rainfall_scaled == 0.0) {
            double r = swmm_getValue(swmm_GAGE_RAINFALL, 0);
            if (r > 0.0) rainfall_scaled = r;
        }
    } while (elapsed > 0.0);
    swmm_end();
    swmm_close();

    ASSERT_GT(rainfall_scaled, 0.0);
    EXPECT_NEAR(rainfall_scaled,
                2.0 * rainfall_baseline,
                std::abs(2.0 * rainfall_baseline) * 0.0001);

    std::remove("capi_set_base.rpt");
    std::remove("capi_set_base.out");
    std::remove("capi_set.rpt");
    std::remove("capi_set.out");
}

// Non-positive values must be rejected, leaving the stored factor unchanged.
TEST_F(GageScaleFactor, CApiSetRejectsNonPositive) {
    ASSERT_EQ(swmm_open(DATA_PATH_NOSCALE, "capi_reject.rpt", "capi_reject.out"), 0);
    ASSERT_EQ(swmm_start(1), 0);

    // Default for an unscaled gage is 1.0.
    EXPECT_DOUBLE_EQ(swmm_getValue(swmm_GAGE_SCALEFACTOR, 0), 1.0);

    EXPECT_NE(swmm_setValueExpanded(swmm_GAGE,
                                    swmm_GAGE_SCALEFACTOR,
                                    0, 0, 0, 0.0), 0);
    EXPECT_NE(swmm_setValueExpanded(swmm_GAGE,
                                    swmm_GAGE_SCALEFACTOR,
                                    0, 0, 0, -1.0), 0);

    // Value must be untouched.
    EXPECT_DOUBLE_EQ(swmm_getValue(swmm_GAGE_SCALEFACTOR, 0), 1.0);

    swmm_end();
    swmm_close();

    std::remove("capi_reject.rpt");
    std::remove("capi_reject.out");
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
