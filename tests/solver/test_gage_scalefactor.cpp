/*
 *   test_gage_scalefactor.cpp
 *
 *   Created: 2026
 *
 *   Unit testing for rain gage scale factor feature using Boost Test.
 */

#define BOOST_TEST_MODULE "gage_scalefactor"
#include <boost/test/included/unit_test.hpp>

#include <string>
#include <cstdio>

#include "swmm5.h"

#define DATA_PATH_SCALE "./data/gage_scalefactor.inp"
#define DATA_PATH_NOSCALE "./data/gage_noscalefactor.inp"

// Helper to run a SWMM simulation and check for errors
static int runSimulation(const char* inpFile, const char* rptFile, const char* outFile)
{
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

BOOST_AUTO_TEST_SUITE(gage_scalefactor_tests)

// Test that a model with no scale factor (backward compatible) runs successfully
BOOST_AUTO_TEST_CASE(no_scalefactor_backward_compat)
{
    int err = runSimulation(DATA_PATH_NOSCALE,
                            "noscale_test.rpt",
                            "noscale_test.out");
    BOOST_CHECK_EQUAL(err, 0);

    // Clean up
    std::remove("noscale_test.rpt");
    std::remove("noscale_test.out");
}

// Test that a model with a scale factor of 2.0 runs successfully
BOOST_AUTO_TEST_CASE(with_scalefactor_runs)
{
    int err = runSimulation(DATA_PATH_SCALE,
                            "scale_test.rpt",
                            "scale_test.out");
    BOOST_CHECK_EQUAL(err, 0);

    // Clean up
    std::remove("scale_test.rpt");
    std::remove("scale_test.out");
}

// Test that scale factor of 2.0 produces doubled rainfall intensity
BOOST_AUTO_TEST_CASE(scalefactor_doubles_rainfall)
{
    int err;
    double elapsedTime;
    double rainfall_noscale = 0.0;
    double rainfall_scale = 0.0;

    // Run without scale factor and capture rainfall at time step
    err = swmm_open(DATA_PATH_NOSCALE, "noscale_rf.rpt", "noscale_rf.out");
    BOOST_REQUIRE_EQUAL(err, 0);
    err = swmm_start(1);
    BOOST_REQUIRE_EQUAL(err, 0);

    // Advance to a point where there is rainfall (after 1 hour)
    do {
        err = swmm_step(&elapsedTime);
        BOOST_REQUIRE_EQUAL(err, 0);
        if (elapsedTime > 0.0)
        {
            double r = swmm_getValue(swmm_GAGE_RAINFALL, 0);
            if (r > 0.0 && rainfall_noscale == 0.0)
                rainfall_noscale = r;
        }
    } while (elapsedTime > 0.0);
    swmm_end();
    swmm_close();

    // Run with scale factor = 2.0 and capture rainfall at time step
    err = swmm_open(DATA_PATH_SCALE, "scale_rf.rpt", "scale_rf.out");
    BOOST_REQUIRE_EQUAL(err, 0);
    err = swmm_start(1);
    BOOST_REQUIRE_EQUAL(err, 0);

    do {
        err = swmm_step(&elapsedTime);
        BOOST_REQUIRE_EQUAL(err, 0);
        if (elapsedTime > 0.0)
        {
            double r = swmm_getValue(swmm_GAGE_RAINFALL, 0);
            if (r > 0.0 && rainfall_scale == 0.0)
                rainfall_scale = r;
        }
    } while (elapsedTime > 0.0);
    swmm_end();
    swmm_close();

    // The scaled rainfall should be 2x the unscaled
    BOOST_REQUIRE(rainfall_noscale > 0.0);
    BOOST_REQUIRE(rainfall_scale > 0.0);
    BOOST_CHECK_CLOSE(rainfall_scale, 2.0 * rainfall_noscale, 0.01);

    // Clean up
    std::remove("noscale_rf.rpt");
    std::remove("noscale_rf.out");
    std::remove("scale_rf.rpt");
    std::remove("scale_rf.out");
}

BOOST_AUTO_TEST_SUITE_END()
