/*
 *   test_output.cpp
 *
 *   Created: 11/2/2017
 *   Author: Michael E. Tryby
 *           US EPA - ORD/NRMRL
 *
 *   Unit testing for SWMM outputapi.
 *
 *   Ported from Boost.Test to GoogleTest on 2026-08-16 to align this branch
 *   with the openswmm.engine test harness (see
 *   plans/ENGINE_524_BUILD_MODERNIZATION_PLAN_2026-08-16.md, amendment 1).
 *   Test names, reference data and tolerances are carried over unchanged so the
 *   suite keeps asserting exactly what it did under Boost.Test.
 */

#include <gtest/gtest.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "swmm_output.h"

// NOTE: Reference data for the unit tests is currently tied to SWMM 5.1.7
#define DATA_PATH "./Example1.out"

// Checks for minimum number of correct decimal digits.
// Returns an AssertionResult so failures report the computed cdd, which the
// Boost predicate_result version did not.
static ::testing::AssertionResult check_cdd_float(const std::vector<float>& test,
                                                  const std::vector<float>& ref,
                                                  long cdd_tol) {
    if (test.size() != ref.size()) {
        return ::testing::AssertionFailure()
               << "size mismatch: test=" << test.size() << " ref=" << ref.size();
    }

    float tmp, min_cdd = 10.0f;

    std::vector<float>::const_iterator test_it;
    std::vector<float>::const_iterator ref_it;

    for (test_it = test.begin(), ref_it = ref.begin();
         (test_it < test.end()) && (ref_it < ref.end());
         ++test_it, ++ref_it)
    {
        if (*test_it != *ref_it) {
            // Compute log absolute error
            tmp = std::fabs(*test_it - *ref_it);
            if (tmp < 1.0e-7f)
                tmp = 1.0e-7f;

            else if (tmp > 2.0f)
                tmp = 1.0f;

            tmp = -std::log10(tmp);
            if (tmp < 0.0f)
                tmp = 0.0f;

            if (tmp < min_cdd)
                min_cdd = tmp;
        }
    }

    if (std::floor(min_cdd) >= static_cast<float>(cdd_tol))
        return ::testing::AssertionSuccess();

    return ::testing::AssertionFailure()
           << "correct decimal digits " << std::floor(min_cdd)
           << " < required " << cdd_tol;
}

// ---------------------------------------------------------------------------
// Suite: test_output_auto  (was BOOST_AUTO_TEST_SUITE(test_output_auto))
// ---------------------------------------------------------------------------

TEST(test_output_auto, InitTest) {
    SMO_Handle p_handle = NULL;

    int error = SMO_init(&p_handle);
    ASSERT_EQ(0, error);
    EXPECT_NE(nullptr, p_handle);

    SMO_close(&p_handle);
}

TEST(test_output_auto, CloseTest) {
    SMO_Handle p_handle = NULL;
    SMO_init(&p_handle);

    int error = SMO_close(&p_handle);
    ASSERT_EQ(0, error);
    EXPECT_EQ(nullptr, p_handle);
}

TEST(test_output_auto, InitOpenCloseTest) {
    std::string path     = std::string(DATA_PATH);
    SMO_Handle  p_handle = NULL;
    SMO_init(&p_handle);

    int error = SMO_open(p_handle, path.c_str());
    ASSERT_EQ(0, error);

    SMO_close(&p_handle);
}

// ---------------------------------------------------------------------------
// Suite: test_output_fixture  (was BOOST_FIXTURE_TEST_CASE(..., Fixture))
// ---------------------------------------------------------------------------

class Fixture : public ::testing::Test {
protected:
    void SetUp() override {
        std::string path = std::string(DATA_PATH);

        error = SMO_init(&p_handle);
        SMO_clearError(p_handle);
        error = SMO_open(p_handle, path.c_str());

        array     = NULL;
        array_dim = 0;
    }

    void TearDown() override {
        SMO_free((void**)&array);
        error = SMO_close(&p_handle);
    }

    std::string path;
    int         error = 0;
    SMO_Handle  p_handle = NULL;

    float* array = NULL;
    int    array_dim = 0;
};

TEST_F(Fixture, test_getVersion) {
    int version;

    error = SMO_getVersion(p_handle, &version);
    ASSERT_EQ(0, error);

    EXPECT_EQ(51000, version);
}

TEST_F(Fixture, test_getProjectSize) {
    int* i_array = NULL;

    error = SMO_getProjectSize(p_handle, &i_array, &array_dim);
    ASSERT_EQ(0, error);

    std::vector<int> test;
    test.assign(i_array, i_array + array_dim);

    // subcatchs, nodes, links, pollutants
    const int ref_dim            = 5;
    int       ref_array[ref_dim] = {8, 14, 13, 1, 2};

    std::vector<int> ref;
    ref.assign(ref_array, ref_array + ref_dim);

    EXPECT_EQ(ref, test);

    SMO_free((void**)&i_array);
}

TEST_F(Fixture, test_getUnits) {
    int* i_array = NULL;

    error = SMO_getUnits(p_handle, &i_array, &array_dim);
    ASSERT_EQ(0, error);

    std::vector<int> test;
    test.assign(i_array, i_array + array_dim);

    // unit system, flow units, pollut units
    const int ref_dim            = 4;
    const int ref_array[ref_dim] = {SMO_US, SMO_CFS, SMO_MG, SMO_UG};

    std::vector<int> ref;
    ref.assign(ref_array, ref_array + ref_dim);

    EXPECT_EQ(ref, test);

    SMO_free((void**)&i_array);
}

TEST_F(Fixture, test_getFlowUnits) {
    int units = -1;

    error = SMO_getFlowUnits(p_handle, &units);
    ASSERT_EQ(0, error);
    EXPECT_EQ(0, units);
}

TEST_F(Fixture, test_getPollutantUnits) {
    int* i_array = NULL;

    error = SMO_getPollutantUnits(p_handle, &i_array, &array_dim);
    ASSERT_EQ(0, error);

    std::vector<int> test;
    test.assign(i_array, i_array + array_dim);

    const int ref_dim            = 2;
    int       ref_array[ref_dim] = {0, 1};

    std::vector<int> ref;
    ref.assign(ref_array, ref_array + ref_dim);

    EXPECT_EQ(ref, test);

    SMO_free((void**)&i_array);
    EXPECT_EQ(nullptr, i_array);
}

TEST_F(Fixture, test_getStartDate) {
    double date = -1;

    error = SMO_getStartDate(p_handle, &date);
    ASSERT_EQ(0, error);

    EXPECT_DOUBLE_EQ(35796., date);
}

TEST_F(Fixture, test_getTimes) {
    int time = -1;

    error = SMO_getTimes(p_handle, SMO_reportStep, &time);
    ASSERT_EQ(0, error);

    EXPECT_EQ(3600, time);

    error = SMO_getTimes(p_handle, SMO_numPeriods, &time);
    ASSERT_EQ(0, error);

    EXPECT_EQ(36, time);
}

TEST_F(Fixture, test_getElementName) {
    char* c_array = NULL;
    int   index   = 1;

    error = SMO_getElementName(p_handle, SMO_node, index, &c_array, &array_dim);
    ASSERT_EQ(0, error);

    std::string test(c_array);
    std::string ref("10");
    EXPECT_EQ(ref, test);

    SMO_free((void**)&c_array);
}

TEST_F(Fixture, test_getSubcatchSeries) {
    error = SMO_getSubcatchSeries(p_handle, 1, SMO_runoff_rate, 0, 10, &array,
                                  &array_dim);
    ASSERT_EQ(0, error);

    const int ref_dim            = 10;
    float     ref_array[ref_dim] = {
        0.0f, 1.2438242f, 2.5639679f, 4.524055f, 2.5115132f, 0.69808137f,
        0.040894926f, 0.011605669f, 0.00509294f, 0.0027438672f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + ref_dim);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

TEST_F(Fixture, test_getSystemSeries) {
    error = SMO_getSystemSeries(p_handle, SMO_runoff_flow, 0, 10, &array,
                                &array_dim);
    ASSERT_EQ(0, error);

    const int ref_dim            = 10;
    float     ref_array[ref_dim] = {
        0.0f, 6.216825f, 13.030855f, 24.252975f, 14.172027f, 4.1949716f,
        0.322329f, 0.056010f, 0.024938f, 0.012474f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + ref_dim);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

TEST_F(Fixture, test_getSubcatchResult) {
    error = SMO_getSubcatchResult(p_handle, 1, 1, &array, &array_dim);
    ASSERT_EQ(0, error);

    const int ref_dim            = 10;
    float     ref_array[ref_dim] = {
        0.5f, 0.0f, 0.0f, 0.125f, 1.2438242f,
        0.0f, 0.0f, 0.0f, 33.481991f, 6.6963983f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + ref_dim);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

TEST_F(Fixture, test_getNodeResult) {
    error = SMO_getNodeResult(p_handle, 2, 2, &array, &array_dim);
    ASSERT_EQ(0, error);

    const int ref_dim        = 8;
    float ref_array[ref_dim] = {
        0.296234f, 995.296204f, 0.0f, 1.302650f, 1.302650f, 0.0f,
        15.361463f, 3.072293f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + ref_dim);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

TEST_F(Fixture, test_getLinkResult) {
    error = SMO_getLinkResult(p_handle, 3, 3, &array, &array_dim);
    ASSERT_EQ(0, error);

    const int ref_dim        = 7;
    float ref_array[ref_dim] = {
        4.631762f, 1.0f, 5.8973422f, 314.15927f, 1.0f, 19.070757f,
        3.8141515f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + ref_dim);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

TEST_F(Fixture, test_getSystemResult) {
    error = SMO_getSystemResult(p_handle, 4, 4, &array, &array_dim);
    ASSERT_EQ(0, error);

    const int ref_dim            = 14;
    float     ref_array[ref_dim] = {
        70.0f, 0.1f, 0.0f, 0.19042271f, 14.172027f, 0.0f, 0.0f, 0.0f,
        0.0f, 14.172027f, 0.55517411f, 13.622702f, 2913.0793f, 0.0f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + ref_dim);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}
