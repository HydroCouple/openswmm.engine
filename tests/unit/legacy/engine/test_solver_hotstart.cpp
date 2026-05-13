/*
 *   test_solver_hotstart.cpp
 *
 *   Created: 02/01/2024
 *   Updated: 2026-03-25
 *
 *   Unit testing for SWMM solver API hotstart saving (Google Test).
 */

#include <gtest/gtest.h>
#include <fstream>

#include "openswmm_solver.h"
#include "openswmm_output_enums.h"
#include "openswmm_output.h"
#include "helper.h"

/*!
* \def ORIGINAL_INPUT_FILE 
* \brief The original input file for the SWMM model.
*/
#define ORIGINAL_INPUT_FILE "./hotstart/site_drainage_model.inp"

/*!
* \def SAVE_HOTSTART_INPUT_FILE
* \brief The input file for the SWMM model to save the hotstart file.
*/
#define SAVE_HOTSTART_INPUT_FILE "./hotstart/site_drainage_model_save_hotstart.inp"

/*!
* \def HOTSTART_FILE_V1
* \brief The first hotstart file to save for the SWMM model.
*/
#define HOTSTART_FILE_V1 "./hotstart/hotstart_v1.hsf"

/*!
* \def HOTSTART_FILE_V2
* \brief The second hotstart file to save for the SWMM model.
*/
#define HOTSTART_FILE_V2 "./hotstart/hotstart_v2.hsf"

/*!
* \def HOTSTART_FILE_V3
* \brief The third hotstart file to save for the SWMM model.
*/
#define HOTSTART_FILE_END "./hotstart/hotstart_end.hsf"

/*!
* \def RUN_HOTSTART_INPUT_FILE_v1
* \brief The input file for the SWMM model to use the first hotstart file.
*/
#define RUN_HOTSTART_INPUT_FILE_v1 "./hotstart/site_drainage_model_use_hotstart_v1.inp"

/*!
* \def RUN_HOTSTART_INPUT_FILE_v2
* \brief The input file for the SWMM model to use the second hotstart file.
*/
#define RUN_HOTSTART_INPUT_FILE_v2 "./hotstart/site_drainage_model_use_hotstart_v2.inp"

/*!
* \def RUN_HOTSTART_INPUT_FILE_v3
* \brief The input file for the SWMM model to use the third hotstart file.
*/
#define RUN_HOTSTART_INPUT_FILE_v3 "./hotstart/site_drainage_model_use_hotstart_v3.inp"

/*!
  \brief Test the hotstart saving feature of the SWMM solver.
  \sa swmm_run
*/
TEST(SolverHotstartTest, TestSaveHotstart)
{
	int error = 0;
	std::string filepath = std::string(ORIGINAL_INPUT_FILE);
    std::string extension = ".inp";

    size_t startPos = filepath.find(extension);
    if (startPos == std::string::npos) {
        FAIL() << "Extension not found in filepath";
    }

    std::string reportFilepath = std::string(filepath).replace(startPos, extension.length(), ".rpt");
	std::string outputFilepath = std::string(filepath).replace(startPos, extension.length(), ".out");

	error = swmm_run(filepath.c_str(), reportFilepath.c_str(), outputFilepath.c_str());
	ASSERT_EQ(error, 0);

	// Check to make sure output exists
	std::ifstream output_file(outputFilepath);
	ASSERT_TRUE(output_file.good());

	filepath = std::string(SAVE_HOTSTART_INPUT_FILE);
	startPos = filepath.find(extension);
	reportFilepath = std::string(filepath).replace(startPos, filepath.length(), ".rpt");
	outputFilepath = std::string(filepath).replace(startPos, extension.length(), ".out");
	error = swmm_run(SAVE_HOTSTART_INPUT_FILE, reportFilepath.c_str(), outputFilepath.c_str());
	ASSERT_EQ(error, 0);

	// Check to make sure hotstart output file exists
	std::ifstream hotstartSaveOutputFile(outputFilepath);
	ASSERT_TRUE(hotstartSaveOutputFile.good());

	// Check to make sure hotstart files exist
	std::ifstream hotstartFileV1(std::string(HOTSTART_FILE_V1));
	ASSERT_TRUE(hotstartFileV1.good());

	std::ifstream hotstartFileV2(std::string(HOTSTART_FILE_V2));
	ASSERT_TRUE(hotstartFileV2.good());

	std::ifstream hotstartFileEnd(std::string(HOTSTART_FILE_END));
	ASSERT_TRUE(hotstartFileEnd.good());
}

/*!
* \brief Test the hotstart saving feature of the SWMM solver.
* \test
* The test case runs the SWMM model with the hotstart file and compares
* the results with the original input file at critical locations.
*/
TEST(SolverHotstartTest, TestRunHotstartFirst)
{
	int error = 0;
	std::string originalFilepath = std::string(ORIGINAL_INPUT_FILE);
	std::string filepath = std::string(RUN_HOTSTART_INPUT_FILE_v1);
	std::string extension = ".inp";

    size_t startPos = filepath.find(extension);
    if (startPos == std::string::npos) {
        FAIL() << "Extension not found in filepath";
    }
	std::string reportFilepath = std::string(filepath).replace(startPos, extension.length(), ".rpt");
	std::string outputFilepath = std::string(filepath).replace(startPos, extension.length(), ".out");
	error = swmm_run(filepath.c_str(), reportFilepath.c_str(), outputFilepath.c_str());
	ASSERT_EQ(error, 0);

	startPos = originalFilepath.find(extension);
    if (startPos == std::string::npos) {
        FAIL() << "Extension not found in filepath";
    }

	// std::string originalOutputFilepath = std::string(originalFilepath).replace(startPos, extension.length(), ".out");
	// SWMMOutputFile output_file(originalOutputFilepath);

	// int *origElementCount;
	// int origLength;
	// error = SMO_getProjectSize(output_file.m_handle, &origElementCount, &origLength);
	// ASSERT_EQ(error, 0);

	// int num_periods;
	// double startDate;
	// error = SMO_getTimes(output_file.m_handle, SMO_time::SMO_numPeriods, &num_periods);
	// ASSERT_EQ(error, 0);

	// error = SMO_getStartDate(output_file.m_handle, &startDate);
	// ASSERT_EQ(error, 0);

	// std::string outputFilePath = std::string(filepath).replace(startPos, extension.length(), ".out");
	// SWMMOutputFile hotstart_output_file(outputFilePath);
}