/**
 * @file      helper.h
 * @brief     Tests for helper.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_tests Engine tests
 * @brief Maintained test helpers, fixtures and validation entry points.
 * @ingroup new_engine
 */

/**
 * @defgroup engine_unit_tests Engine unit tests
 * @brief Unit fixtures and direct engine contract checks.
 * @ingroup engine_tests
 */

/**
 * @defgroup engine_regression Engine regression tests
 * @brief Cross-engine comparisons and numerical regression gates.
 * @ingroup engine_tests
 */

#pragma once
#include <gtest/gtest.h>
#include "openswmm_output.h"

#include <vector>
#include <cmath>

/**
 * @brief Compare parallel vectors with an asymmetric mixed absolute/relative tolerance.
 *
 * NaN differences do not satisfy the greater-than test, so this helper can accept NaN/nonfinite
 * pairs; callers needing finite comparisons must check them separately.
 *
 * @param[in] v1 First borrowed vector.
 * @param[in] v2 Reference vector, whose absolute magnitude scales relative tolerance.
 * @param[in] rtol Expected nonnegative relative tolerance; not validated.
 * @param[in] atol Expected nonnegative absolute tolerance; not validated.
 * @return False on unequal sizes or an element with abs(v1-v2) > atol + rtol*abs(v2); true
 *     otherwise, including empty vectors.
 */
inline bool all_close(const std::vector<double>& v1, const std::vector<double>& v2,
                      double rtol = 1.0E-05, double atol = 1.0E-08) {
    if (v1.size() != v2.size())
        return false;

    for (size_t i = 0; i < v1.size(); i++) {
        double diff = std::abs(v1[i] - v2[i]);
        if (diff > (atol + rtol * std::abs(v2[i])))
            return false;
    }

    return true;
};

/*!
* \brief A structure to hold the SWMM output file.
* \note This structure is used to hold the SWMM output file.
* The structure is used to open the SWMM output file and close the SWMM output file.
* The structure is used to hold the error code and the handle to the SWMM output file.
* The structure is used to open the SWMM output file and close the SWMM output file.
*/
struct SWMMOutputFile {
    /**
     * @brief Allocate and open one legacy output-reader handle for a test.
     *
     * Stores SMO_open status in m_errorCode. The SMO_init result is overwritten after clearing error;
     * allocation failure is not safely handled. Owns m_handle until destruction. This struct does not
     * disable default copying; callers must avoid copying it because copies would close the same handle
     * twice.
     *
     * @param[in] outputFilepath Borrowed filename, used synchronously during construction.
     */
    SWMMOutputFile(const std::string& outputFilepath) : m_errorCode(0), m_handle(nullptr) {
        m_errorCode = SMO_init(&m_handle);
        SMO_clearError(m_handle);
        m_errorCode = SMO_open(m_handle, outputFilepath.c_str());
    }

    /**
     * @brief Close and release the reader handle owned by this test wrapper.
     *
     * Ignores SMO_close return status and invalidates aliases to the owned handle; returned result
     * arrays remain separately caller-owned.
     */
    ~SWMMOutputFile() { SMO_close(&m_handle); }

    /*!
	* \brief The error code for the SWMM output file.
	*/
    int m_errorCode;

    /*!
	* \brief The handle to the SWMM output file.
	*/
    SMO_Handle m_handle;
};
