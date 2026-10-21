// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      openswmm_output2d_impl.cpp
 * @brief     Public C API implementation for saved 2D result access.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "openswmm/engine/openswmm_output2d.h"
#include "openswmm/engine/openswmm_engine.h"
#include "Output2DReader.hpp"
#include "../io/Hdf5Mutex.hpp"
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace {
thread_local std::string error;
template <class F> int invoke(SWMM_Output2D handle, F function) noexcept {
    error.clear();
    if (!handle) {
        error = "2D output reader is closed";
        return SWMM_ERR_BADHANDLE;
    }
    std::lock_guard<std::recursive_mutex> lock(openswmm::io::resultHdf5Mutex());
    try {
        function();
        return SWMM_OK;
    } catch (const std::out_of_range& e) {
        error = e.what();
        return SWMM_ERR_BADINDEX;
    } catch (const std::invalid_argument& e) {
        error = e.what();
        return SWMM_ERR_BADPARAM;
    } catch (const std::bad_alloc&) {
        error = "Cannot allocate result read buffer";
        return SWMM_ERR_NOMEM;
    } catch (const std::exception& e) {
        error = e.what();
        return SWMM_ERR_IO;
    } catch (...) {
        error = "Unexpected result reader failure";
        return SWMM_ERR_INTERNAL;
    }
}
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
openswmm::Output2DReader& reader(SWMM_Output2D handle) {
    return *static_cast<openswmm::Output2DReader*>(handle);
}
void copy(const std::string& value, char* buffer, size_t capacity, size_t* required) {
    if (!required)
        throw std::invalid_argument("Required-capacity pointer is null");
    *required = value.size() + 1;
    if (!buffer && capacity == 0)
        return;
    if (!buffer || capacity < *required)
        throw std::invalid_argument("Text buffer is too small");
    std::memcpy(buffer, value.c_str(), *required);
}
#else
int unavailable() {
    error = "Engine was built without 2D HDF5 result support";
    return SWMM_ERR_PLUGIN;
}
#endif
}
extern "C" {
int swmm_output2d_available(void) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return 1;
#else
    return 0;
#endif
}
const char* swmm_output2d_error(void) {
    return error.c_str();
}
int swmm_output2d_open(const char* path, SWMM_Output2D* output) {
    if (!output) {
        error = "Output handle pointer is null";
        return SWMM_ERR_BADPARAM;
    }
    *output = nullptr;
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output, [&] {
        auto owned = std::make_unique<openswmm::Output2DReader>();
        owned->open(path);
        *output = owned.release();
    });
#else
    return unavailable();
#endif
}
void swmm_output2d_close(SWMM_Output2D output) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    std::lock_guard<std::recursive_mutex> lock(openswmm::io::resultHdf5Mutex());
    delete static_cast<openswmm::Output2DReader*>(output);
#endif
}
int swmm_output2d_datasets(SWMM_Output2D output, char* text, size_t capacity, size_t* required) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output, [&] { copy(reader(output).datasets(), text, capacity, required); });
#else
    return unavailable();
#endif
}
int swmm_output2d_shape(SWMM_Output2D output, const char* path, int* rank, uint64_t* dimensions,
                        int* integer, uint64_t* unlimited_axes) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output, [&] {
        if (!rank || !dimensions || !integer || !unlimited_axes)
            throw std::invalid_argument("Shape pointers are required");
        reader(output).shape(path, *rank, dimensions, *integer, *unlimited_axes);
    });
#else
    return unavailable();
#endif
}
int swmm_output2d_attributes(SWMM_Output2D output, const char* path, char* text, size_t capacity,
                             size_t* required) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output, [&] { copy(reader(output).attributes(path), text, capacity, required); });
#else
    return unavailable();
#endif
}
int swmm_output2d_attribute(SWMM_Output2D output, const char* path, const char* name, char* text,
                            size_t capacity, size_t* required) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output,
                  [&] { copy(reader(output).attribute(path, name), text, capacity, required); });
#else
    return unavailable();
#endif
}
int swmm_output2d_read_double(SWMM_Output2D output, const char* path, const uint64_t* start,
                              const uint64_t* count, const uint64_t* stride, double* values,
                              size_t capacity) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(
        output, [&] { reader(output).read(path, start, count, stride, values, capacity, false); });
#else
    return unavailable();
#endif
}
int swmm_output2d_strings(SWMM_Output2D output, const char* path, char* text, size_t capacity,
                          size_t* required) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output, [&] { copy(reader(output).strings(path), text, capacity, required); });
#else
    return unavailable();
#endif
}
int swmm_output2d_read_int64(SWMM_Output2D output, const char* path, const uint64_t* start,
                             const uint64_t* count, const uint64_t* stride, int64_t* values,
                             size_t capacity) {
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
    return invoke(output,
                  [&] { reader(output).read(path, start, count, stride, values, capacity, true); });
#else
    return unavailable();
#endif
}
}
