/*!
* \file test_output.cpp
* \brief Unit testing for SWMM output API using Google Test.
* \author Michael E. Tryby (US EPA - ORD/NRMRL)
* \date 11/2/2017
* \note Converted from Boost.Test to Google Test 2026-03-25.
*/

#include <gtest/gtest.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>

#include "openswmm_output.h"

using namespace std;

// NOTE: Reference data for the unit tests is currently tied to SWMM 5.1.7
#define DATA_PATH "./Example1.out"

/*!
* \brief Check for minimum number of correct decimal digits.
* \param test vector of test data
* \param ref vector of reference data
* \param cdd_tol minimum number of correct decimal digits
* \return true if minimum number of correct decimal digits is met
*/
bool check_cdd_float(const std::vector<float>& test,
    const std::vector<float>& ref, long cdd_tol) {

    float tmp, min_cdd = 10.0;

    // TODO: What if the vectors aren't the same length?

    std::vector<float>::const_iterator test_it;
    std::vector<float>::const_iterator ref_it;

    for (test_it = test.begin(), ref_it = ref.begin();
        (test_it < test.end()) && (ref_it < ref.end());
        ++test_it, ++ref_it)
    {
        if (*test_it != *ref_it) {
            // Compute log absolute error
            tmp = abs(*test_it - *ref_it);
            if (tmp < 1.0e-7f)
                tmp = 1.0e-7f;

            else if (tmp > 2.0f)
                tmp = 1.0f;

            tmp = -log10(tmp);
            if (tmp < 0.0f)
                tmp = 0.0f;

            if (tmp < min_cdd)
                min_cdd = tmp;
        }
    }
    return floor(min_cdd) >= cdd_tol;
}

/*!
* \brief Check for string equality.
* \param test string to test
* \param ref reference string
* \return true if strings are equal
*/
bool check_string(std::string test, std::string ref) {
    return ref.compare(test) == 0;
}

/*!
* \brief Test SWMM output API functions (basic open/close).
*/
TEST(OutputApiTest, InitTest) {
    SMO_Handle p_handle = NULL;

    int error = SMO_init(&p_handle);
    ASSERT_EQ(error, 0);
    EXPECT_NE(p_handle, nullptr);

    SMO_close(&p_handle);
}

/*!
* \brief Test the SMO_close function.
*/
TEST(OutputApiTest, CloseTest) {
    SMO_Handle p_handle = NULL;
    SMO_init(&p_handle);

    int error = SMO_close(&p_handle);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(p_handle, nullptr);
}

/*!
* \brief Test the SMO_open function on valid file and then test SMO_close.
*/
TEST(OutputApiTest, InitOpenCloseTest) {
    std::string path     = std::string(DATA_PATH);
    SMO_Handle  p_handle = NULL;
    SMO_init(&p_handle);

    int error = SMO_open(p_handle, path.c_str());
    ASSERT_EQ(error, 0);

    SMO_close(&p_handle);
}


/*!
* \brief Test fixture for SWMM output API functions.
*/
class OutputFixture : public ::testing::Test {
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
    int         error;
    SMO_Handle  p_handle;

    float* array;
    int    array_dim;
};

/*!
* \brief Test SWMM output API functions (fixture-based).
*/

/*!
* \brief Test the SMO_getVersion function.
*/
TEST_F(OutputFixture, test_getVersion) {
    int version;

    error = SMO_getVersion(p_handle, &version);
    ASSERT_EQ(error, 0);

    EXPECT_EQ(51000, version);
}

/*!
* \brief Test the SMO_getProjectSize function.
*/
TEST_F(OutputFixture, test_getProjectSize) {
    int* i_array = NULL;

    error = SMO_getProjectSize(p_handle, &i_array, &array_dim);
    ASSERT_EQ(error, 0);

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

/*!
* \brief Test the SMO_getUnits function.
*/
TEST_F(OutputFixture, test_getUnits) {
    int*      i_array            = NULL;

    error = SMO_getUnits(p_handle, &i_array, &array_dim);
    ASSERT_EQ(error, 0);

	std::vector<int> test;
    test.assign(i_array, i_array + array_dim);

	// unit system, flow units, pollut units
	const int        ref_dim            = 4;
    const int        ref_array[ref_dim] = {SMO_US, SMO_CFS, SMO_MG, SMO_UG};

	std::vector<int> ref;
    ref.assign(ref_array, ref_array + ref_dim);

    EXPECT_EQ(ref, test);

    SMO_free((void**)&i_array);
}

/*!
* \brief Test the SMO_getFlowUnits function.
*/
TEST_F(OutputFixture, test_getFlowUnits) {
    int units = -1;

    error = SMO_getFlowUnits(p_handle, &units);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(0, units);
}

/*!
* \brief Test the SMO_getPollutantUnits function.
*/
TEST_F(OutputFixture, test_getPollutantUnits) {
    int* i_array = NULL;

    error = SMO_getPollutantUnits(p_handle, &i_array, &array_dim);
    ASSERT_EQ(error, 0);

    std::vector<int> test;
    test.assign(i_array, i_array + array_dim);

    const int ref_dim            = 2;
    int       ref_array[ref_dim] = {0, 1};

    std::vector<int> ref;
    ref.assign(ref_array, ref_array + ref_dim);

    EXPECT_EQ(ref, test);

    SMO_free((void**)&i_array);
    EXPECT_EQ(i_array, nullptr);
}

/*!
* \brief Test the SMO_getStartDate function.
*/
TEST_F(OutputFixture, test_getStartDate) {
    double date = -1;

    error = SMO_getStartDate(p_handle, &date);
    ASSERT_EQ(error, 0);

    EXPECT_EQ(35796., date);
}

/*!
* \brief Test the SMO_getTimes function.
*/
TEST_F(OutputFixture, test_getTimes) {
    int time = -1;

    error = SMO_getTimes(p_handle, SMO_reportStep, &time);
    ASSERT_EQ(error, 0);

    EXPECT_EQ(3600, time);

    error = SMO_getTimes(p_handle, SMO_numPeriods, &time);
    ASSERT_EQ(error, 0);

    EXPECT_EQ(36, time);
}

/*!
* \brief Test the SMO_getElementName function.
*/
TEST_F(OutputFixture, test_getElementName) {
    char* c_array = NULL;
    int   index   = 1;

    error = SMO_getElementName(p_handle, SMO_node, index, &c_array, &array_dim);
    ASSERT_EQ(error, 0);

    std::string test(c_array);
    std::string ref("10");
    EXPECT_TRUE(check_string(test, ref));

    SMO_free((void**)&c_array);
}

/*!
* \brief Test the SMO_getSubcatchSeries function.
*/
TEST_F(OutputFixture, test_getSubcatchSeries) {
    error = SMO_getSubcatchSeries(p_handle, 1, SMO_runoff_rate, 0, 10, &array,
                                  &array_dim);
    ASSERT_EQ(error, 0);

    const int ref_dim            = 10;
    float     ref_array[ref_dim] = {
        0.0f, 1.2438242f, 2.5639679f, 4.524055f, 2.5115132f, 0.69808137f,
		0.040894926f, 0.011605669f, 0.00509294f, 0.0027438672f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + 10);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

/*!
* \brief Test the SMO_getSystemSeries function.
*/
TEST_F(OutputFixture, test_getSystemSeries) {
    error = SMO_getSystemSeries(p_handle, SMO_runoff_flow, 0, 10, &array,
                                  &array_dim);
    ASSERT_EQ(error, 0);

    const int ref_dim            = 10;
    float     ref_array[ref_dim] = {
        0.0f, 6.216825f, 13.030855f, 24.252975f, 14.172027f, 4.1949716f,
		0.322329f, 0.056010f, 0.024938f, 0.012474f};

    std::vector<float> ref_vec;
    ref_vec.assign(ref_array, ref_array + 10);

    std::vector<float> test_vec;
    test_vec.assign(array, array + array_dim);

    EXPECT_TRUE(check_cdd_float(test_vec, ref_vec, 3));
}

/*!
* \brief Test the SMO_getSubcatchResult function.
*/
TEST_F(OutputFixture, test_getSubcatchResult) {
    error = SMO_getSubcatchResult(p_handle, 1, 1, &array, &array_dim);
    ASSERT_EQ(error, 0);

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

/*!
* \brief Test the SMO_getNodeResult function.
*/
TEST_F(OutputFixture, test_getNodeResult) {
    error = SMO_getNodeResult(p_handle, 2, 2, &array, &array_dim);
    ASSERT_EQ(error, 0);

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

/*!
* \brief Test the SMO_getLinkResult function.
*/
TEST_F(OutputFixture, test_getLinkResult) {
    error = SMO_getLinkResult(p_handle, 3, 3, &array, &array_dim);
    ASSERT_EQ(error, 0);

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

/*!
* \brief Test the SMO_getSystemResult function.
*/
TEST_F(OutputFixture, test_getSystemResult) {
    error = SMO_getSystemResult(p_handle, 4, 4, &array, &array_dim);
    ASSERT_EQ(error, 0);

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

/*!
* \brief Test SMO_getElementName for subcatchment element type.
*/
TEST_F(OutputFixture, test_getElementName_Subcatch) {
    char* c_array = NULL;
    error = SMO_getElementName(p_handle, SMO_subcatch, 0, &c_array, &array_dim);
    ASSERT_EQ(error, 0);
    EXPECT_NE(c_array, nullptr);
    EXPECT_GT(std::string(c_array).size(), 0u);
    SMO_free((void**)&c_array);
}

/*!
* \brief Test SMO_getElementName for link element type.
*/
TEST_F(OutputFixture, test_getElementName_Link) {
    char* c_array = NULL;
    error = SMO_getElementName(p_handle, SMO_link, 0, &c_array, &array_dim);
    ASSERT_EQ(error, 0);
    EXPECT_NE(c_array, nullptr);
    EXPECT_GT(std::string(c_array).size(), 0u);
    SMO_free((void**)&c_array);
}

/*!
* \brief Test SMO_getNumVars returns expected variable counts.
* Counts derived from getResult array sizes:
*   subcatch=10 (8 vars + 2 pollutants), node=8 (6+2), link=7 (5+2), sys=14
*/
TEST_F(OutputFixture, test_getNumVars_Subcatch) {
    int count = -1;
    error = SMO_getNumVars(p_handle, SMO_subcatch, &count);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(count, 10);
}

TEST_F(OutputFixture, test_getNumVars_Node) {
    int count = -1;
    error = SMO_getNumVars(p_handle, SMO_node, &count);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(count, 8);
}

TEST_F(OutputFixture, test_getNumVars_Link) {
    int count = -1;
    error = SMO_getNumVars(p_handle, SMO_link, &count);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(count, 7);
}

TEST_F(OutputFixture, test_getNumVars_System) {
    int count = -1;
    error = SMO_getNumVars(p_handle, SMO_sys, &count);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(count, 14);
}

/*!
* \brief Test SMO_getVarCodes returns an array whose length matches SMO_getNumVars.
*/
TEST_F(OutputFixture, test_getVarCodes_Subcatch) {
    int* codes = NULL;
    error = SMO_getVarCodes(p_handle, SMO_subcatch, &codes, &array_dim);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(array_dim, 10);
    EXPECT_NE(codes, nullptr);
    SMO_free((void**)&codes);
}

TEST_F(OutputFixture, test_getVarCodes_Node) {
    int* codes = NULL;
    error = SMO_getVarCodes(p_handle, SMO_node, &codes, &array_dim);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(array_dim, 8);
    SMO_free((void**)&codes);
}

TEST_F(OutputFixture, test_getVarCodes_Link) {
    int* codes = NULL;
    error = SMO_getVarCodes(p_handle, SMO_link, &codes, &array_dim);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(array_dim, 7);
    SMO_free((void**)&codes);
}

/*!
* \brief Test SMO_getVarCode returns the code for variable index 0. For subcatchments,
* index 0 maps to SMO_rainfall_subcatch = 0.
*/
TEST_F(OutputFixture, test_getVarCode_SubcatchRainfall) {
    int varCode = -1;
    error = SMO_getVarCode(p_handle, SMO_subcatch, 0, &varCode);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(varCode, static_cast<int>(SMO_rainfall_subcatch));
}

/*!
* \brief Test SMO_getNumProperties returns a non-negative count without error.
*/
TEST_F(OutputFixture, test_getNumProperties_Subcatch) {
    int count = -1;
    error = SMO_getNumProperties(p_handle, SMO_subcatch, &count);
    ASSERT_EQ(error, 0);
    EXPECT_GE(count, 0);
}

TEST_F(OutputFixture, test_getNumProperties_Node) {
    int count = -1;
    error = SMO_getNumProperties(p_handle, SMO_node, &count);
    ASSERT_EQ(error, 0);
    EXPECT_GE(count, 0);
}

TEST_F(OutputFixture, test_getNumProperties_Link) {
    int count = -1;
    error = SMO_getNumProperties(p_handle, SMO_link, &count);
    ASSERT_EQ(error, 0);
    EXPECT_GE(count, 0);
}

/*!
* \brief Test SMO_getNodeSeries for invert depth.
* Cross-reference: test_getNodeResult shows node 2 at time 2 has depth 0.296234.
*/
TEST_F(OutputFixture, test_getNodeSeries_InvertDepth) {
    // Single period [2, 3) for node 2 — depth should match getNodeResult[0]
    error = SMO_getNodeSeries(p_handle, 2, SMO_invert_depth, 2, 3, &array, &array_dim);
    ASSERT_EQ(error, 0);
    ASSERT_EQ(array_dim, 1);
    EXPECT_NEAR(array[0], 0.296234f, 1e-3f);
}

/*!
* \brief Test SMO_getNodeSeries returns the correct number of periods.
*/
TEST_F(OutputFixture, test_getNodeSeries_MultiPeriod) {
    error = SMO_getNodeSeries(p_handle, 0, SMO_invert_depth, 0, 10, &array, &array_dim);
    ASSERT_EQ(error, 0);
    EXPECT_EQ(array_dim, 10);
}

/*!
* \brief Test SMO_getLinkSeries for flow rate.
* Cross-reference: test_getLinkResult shows link 3 at time 3 has flow 4.631762.
*/
TEST_F(OutputFixture, test_getLinkSeries_FlowRate) {
    error = SMO_getLinkSeries(p_handle, 3, SMO_flow_rate_link, 3, 4, &array, &array_dim);
    ASSERT_EQ(error, 0);
    ASSERT_EQ(array_dim, 1);
    EXPECT_NEAR(array[0], 4.631762f, 1e-2f);
}

/*!
* \brief Test SMO_getSubcatchAttribute returns array sized to number of subcatchments.
* Cross-reference: subcatch 1 runoff at period 1 = 1.2438242 (test_getSubcatchSeries).
*/
TEST_F(OutputFixture, test_getSubcatchAttribute_RunoffRate) {
    error = SMO_getSubcatchAttribute(p_handle, 1, SMO_runoff_rate, &array, &array_dim);
    ASSERT_EQ(error, 0);
    // array_dim == number of subcatchments (8)
    ASSERT_GE(array_dim, 2);
    EXPECT_NEAR(array[1], 1.2438242f, 5e-3f);
}

/*!
* \brief Test SMO_getNodeAttribute returns array sized to number of nodes.
* Cross-reference: node 2 invert depth at period 2 = 0.296234.
*/
TEST_F(OutputFixture, test_getNodeAttribute_InvertDepth) {
    error = SMO_getNodeAttribute(p_handle, 2, SMO_invert_depth, &array, &array_dim);
    ASSERT_EQ(error, 0);
    // array_dim == number of nodes (14)
    ASSERT_GE(array_dim, 3);
    EXPECT_NEAR(array[2], 0.296234f, 1e-3f);
}

/*!
* \brief Test SMO_getLinkAttribute returns array sized to number of links.
* Cross-reference: link 3 flow at period 3 = 4.631762.
*/
TEST_F(OutputFixture, test_getLinkAttribute_FlowRate) {
    error = SMO_getLinkAttribute(p_handle, 3, SMO_flow_rate_link, &array, &array_dim);
    ASSERT_EQ(error, 0);
    // array_dim == number of links (13)
    ASSERT_GE(array_dim, 4);
    EXPECT_NEAR(array[3], 4.631762f, 1e-2f);
}

/*!
* \brief Test SMO_getSystemAttribute at period 4 for runoff flow.
* Cross-reference: test_getSystemResult[4] = 14.172027 (SMO_runoff_flow is index 4).
*/
TEST_F(OutputFixture, test_getSystemAttribute_RunoffFlow) {
    error = SMO_getSystemAttribute(p_handle, 4, SMO_runoff_flow, &array, &array_dim);
    ASSERT_EQ(error, 0);
    ASSERT_GE(array_dim, 1);
    EXPECT_NEAR(array[0], 14.172027f, 5e-2f);
}

// ============================================================
// Error-condition tests (separate test suite, no shared fixture)
// ============================================================

/*!
* \brief Test that SMO_open on a non-existent file returns an error.
*/
TEST(OutputErrorTest, OpenNonExistentFile) {
    SMO_Handle p_handle = NULL;
    SMO_init(&p_handle);
    int err = SMO_open(p_handle, "./no_such_file.out");
    EXPECT_NE(err, 0);
    SMO_close(&p_handle);
}

/*!
* \brief Test SMO_checkError after a failed open returns non-zero with a message.
*/
TEST(OutputErrorTest, CheckErrorAfterFailedOpen) {
    SMO_Handle p_handle = NULL;
    SMO_init(&p_handle);
    SMO_open(p_handle, "./no_such_file.out");

    char* msg = NULL;
    int code = SMO_checkError(p_handle, &msg);
    EXPECT_NE(code, 0);
    // message pointer should have been populated
    EXPECT_NE(msg, nullptr);

    SMO_free((void**)&msg);
    SMO_close(&p_handle);
}

/*!
* \brief Test that SMO_close nullifies the handle.
*/
TEST(OutputErrorTest, CloseNullifiesHandle) {
    SMO_Handle p_handle = NULL;
    SMO_init(&p_handle);
    EXPECT_NE(p_handle, nullptr);
    SMO_close(&p_handle);
    EXPECT_EQ(p_handle, nullptr);
}

/*!
* \brief Test double-close does not crash (second close on null handle).
*/
TEST(OutputErrorTest, DoubleCloseIsSafe) {
    SMO_Handle p_handle = NULL;
    SMO_init(&p_handle);
    SMO_close(&p_handle);
    EXPECT_EQ(p_handle, nullptr);
    // Second close on null pointer should be benign
    int err = SMO_close(&p_handle);
    // Acceptable outcomes: no crash; error code may or may not be 0
    (void)err;
}

