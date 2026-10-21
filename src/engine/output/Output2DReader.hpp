// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      Output2DReader.hpp
 * @brief     Independent read-only access to saved 2D HDF5 result datasets.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
#include <hdf5.h>
#include <cstdint>
#include <string>
#include <vector>

namespace openswmm {
/** Independent HDF5 result reader. Callers serialize access through
 * io::resultHdf5Mutex(), shared with the engine's HDF5 output writer. */
class Output2DReader {
public:
    ~Output2DReader();
    Output2DReader() = default;
    Output2DReader(const Output2DReader&) = delete;
    Output2DReader& operator=(const Output2DReader&) = delete;
    void open(const char* path);
    std::string datasets() const;
    void shape(const char* path, int& rank, uint64_t* dimensions, int& integer,
               uint64_t& unlimited_axes) const;
    std::string attributes(const char* path) const;
    std::string attribute(const char* path, const char* name) const;
    std::string strings(const char* path) const;
    void read(const char* path, const uint64_t* start, const uint64_t* count,
              const uint64_t* stride, void* values, size_t capacity, bool integer) const;

private:
    hid_t file_ = -1;
};
}
#endif
