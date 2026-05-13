/**
 * @file openswmm_output_impl.cpp
 * @brief C FFI wrapper for the OutputReader class.
 *
 * @details Maps the C API declared in openswmm_output.h to the C++
 *          OutputReader implementation. Each function casts the opaque
 *          SWMM_Output handle to an OutputReader pointer.
 *
 * @see openswmm_output.h  — public C API
 * @see OutputReader.hpp    — internal C++ reader
 * @ingroup engine_output
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  MIT License
 */

#include "../../../include/openswmm/engine/openswmm_output.h"
#include "OutputReader.hpp"

static inline openswmm::OutputReader* to_reader(SWMM_Output h) noexcept {
    return static_cast<openswmm::OutputReader*>(h);
}

#define CHECK_READER(h)  do { if (!(h)) return -1; } while(0)

extern "C" {

/* =========================================================================
 * Lifecycle
 * ========================================================================= */

SWMM_ENGINE_API SWMM_Output swmm_output_open(const char* path) {
    if (!path) return nullptr;
    auto* reader = new (std::nothrow) openswmm::OutputReader();
    if (!reader) return nullptr;
    if (!reader->open(path)) {
        delete reader;
        return nullptr;
    }
    return static_cast<SWMM_Output>(reader);
}

SWMM_ENGINE_API void swmm_output_close(SWMM_Output handle) {
    if (!handle) return;
    auto* reader = to_reader(handle);
    reader->close();
    delete reader;
}

/* =========================================================================
 * Metadata queries
 * ========================================================================= */

SWMM_ENGINE_API int swmm_output_get_version(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->version();
}

SWMM_ENGINE_API int swmm_output_get_flow_units(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->flow_units();
}

SWMM_ENGINE_API int swmm_output_get_subcatch_count(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->subcatch_count();
}

SWMM_ENGINE_API int swmm_output_get_node_count(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->node_count();
}

SWMM_ENGINE_API int swmm_output_get_link_count(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->link_count();
}

SWMM_ENGINE_API int swmm_output_get_pollut_count(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->pollut_count();
}

SWMM_ENGINE_API int swmm_output_get_period_count(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->period_count();
}

SWMM_ENGINE_API int swmm_output_get_start_date(SWMM_Output handle,
                                                 double* start_date) {
    CHECK_READER(handle);
    if (!start_date) return -1;
    *start_date = to_reader(handle)->start_date();
    return 0;
}

SWMM_ENGINE_API int swmm_output_get_report_step(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->report_step();
}

/* =========================================================================
 * Object ID retrieval
 * ========================================================================= */

SWMM_ENGINE_API const char* swmm_output_get_subcatch_id(SWMM_Output handle,
                                                          int index) {
    if (!handle) return nullptr;
    return to_reader(handle)->subcatch_id(index);
}

SWMM_ENGINE_API const char* swmm_output_get_node_id(SWMM_Output handle,
                                                      int index) {
    if (!handle) return nullptr;
    return to_reader(handle)->node_id(index);
}

SWMM_ENGINE_API const char* swmm_output_get_link_id(SWMM_Output handle,
                                                      int index) {
    if (!handle) return nullptr;
    return to_reader(handle)->link_id(index);
}

/* =========================================================================
 * Per-period result retrieval
 * ========================================================================= */

SWMM_ENGINE_API int swmm_output_get_subcatch_result(SWMM_Output handle,
                                                      int period,
                                                      int var,
                                                      float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_subcatch_result(period, var, values) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_node_result(SWMM_Output handle,
                                                  int period,
                                                  int var,
                                                  float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_node_result(period, var, values) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_link_result(SWMM_Output handle,
                                                  int period,
                                                  int var,
                                                  float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_link_result(period, var, values) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_system_result(SWMM_Output handle,
                                                    int period,
                                                    int var,
                                                    float* value) {
    CHECK_READER(handle);
    return to_reader(handle)->get_system_result(period, var, value) ? 0 : -1;
}

/* =========================================================================
 * Time series retrieval
 * ========================================================================= */

SWMM_ENGINE_API int swmm_output_get_subcatch_series(SWMM_Output handle,
                                                      int subcatch_idx,
                                                      int var,
                                                      int start_period,
                                                      int end_period,
                                                      float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_subcatch_series(subcatch_idx, var,
                                                   start_period, end_period,
                                                   values) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_node_series(SWMM_Output handle,
                                                  int node_idx,
                                                  int var,
                                                  int start_period,
                                                  int end_period,
                                                  float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_node_series(node_idx, var,
                                               start_period, end_period,
                                               values) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_link_series(SWMM_Output handle,
                                                  int link_idx,
                                                  int var,
                                                  int start_period,
                                                  int end_period,
                                                  float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_link_series(link_idx, var,
                                               start_period, end_period,
                                               values) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_system_series(SWMM_Output handle,
                                                    int var,
                                                    int start_period,
                                                    int end_period,
                                                    float* values) {
    CHECK_READER(handle);
    return to_reader(handle)->get_system_series(var, start_period, end_period,
                                                 values) ? 0 : -1;
}

/* =========================================================================
 * Per-object attribute retrieval
 * ========================================================================= */

SWMM_ENGINE_API int swmm_output_get_subcatch_attribute(SWMM_Output handle,
                                                        int subcatch_idx,
                                                        int period,
                                                        float* values,
                                                        int* count) {
    CHECK_READER(handle);
    return to_reader(handle)->get_subcatch_attribute(subcatch_idx, period,
                                                      values, count) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_node_attribute(SWMM_Output handle,
                                                     int node_idx,
                                                     int period,
                                                     float* values,
                                                     int* count) {
    CHECK_READER(handle);
    return to_reader(handle)->get_node_attribute(node_idx, period,
                                                  values, count) ? 0 : -1;
}

SWMM_ENGINE_API int swmm_output_get_link_attribute(SWMM_Output handle,
                                                     int link_idx,
                                                     int period,
                                                     float* values,
                                                     int* count) {
    CHECK_READER(handle);
    return to_reader(handle)->get_link_attribute(link_idx, period,
                                                  values, count) ? 0 : -1;
}

/* =========================================================================
 * Period time
 * ========================================================================= */

SWMM_ENGINE_API int swmm_output_get_period_time(SWMM_Output handle,
                                                  int period,
                                                  double* time) {
    CHECK_READER(handle);
    return to_reader(handle)->get_period_time(period, time) ? 0 : -1;
}

/* =========================================================================
 * Error reporting
 * ========================================================================= */

SWMM_ENGINE_API int swmm_output_get_error_code(SWMM_Output handle) {
    if (!handle) return -1;
    return to_reader(handle)->error_code();
}

} /* extern "C" */
