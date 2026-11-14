// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      Hdf5Mutex.hpp
 * @brief     Shared synchronization for HDF5 operations.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <mutex>
namespace openswmm::io {
// Mesh output and its diagnostic artifacts share the engine's HDF5 instance.
// Recursive because a mesh publication may call the source-history writer.
/**
 * @brief Access the shared recursive lock for result-file HDF5 operations.
 * @return Borrowed process-lifetime mutex shared by mesh output and source
 * history writers. The caller owns each lock acquisition and must release it.
 * @note Recursion permits mesh publication to call the source-history writer.
 * The function returns the mutex without acquiring it.
 */
inline std::recursive_mutex& resultHdf5Mutex() {
    static std::recursive_mutex mutex;
    return mutex;
}
}
