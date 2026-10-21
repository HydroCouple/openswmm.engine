/* SPDX-License-Identifier: Apache-2.0 */
/* Copyright 2026 Caleb Buahin */
/**
 * @file      openswmm_output2d.h
 * @brief     Independent, read-only access to saved 2D HDF5 results.
 *
 * Dataset names are absolute HDF5 paths. Discovery includes static geometry,
 * report fields, envelopes and diagnostic groups, retaining their native axes
 * and units. No solver or Qt runtime is needed. Read only closed files or an
 * immutable published copy: this API does not promise concurrent writer access.
 *
 * All calls return SWMM_OK or an SWMM_ERR_* code, except available/error/close.
 * Text queries return the required capacity INCLUDING the terminating NUL;
 * pass NULL/0 first to size a buffer. Too-small buffers return BADPARAM without
 * truncating. Returned error text is thread-local and valid until the next call
 * on that thread. Handles own their files and must be closed exactly once.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_OUTPUT2D_H
#define OPENSWMM_OUTPUT2D_H
#include "openswmm_engine_export.h"
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef void* SWMM_Output2D;

/** Whether this engine was built with the 2D HDF5 result backend. */
SWMM_ENGINE_API int swmm_output2d_available(void);
SWMM_ENGINE_API int swmm_output2d_open(const char* path, SWMM_Output2D* output);
SWMM_ENGINE_API void swmm_output2d_close(SWMM_Output2D output);
SWMM_ENGINE_API const char* swmm_output2d_error(void);
/** Newline-separated absolute paths of numeric/string datasets, sorted by path. */
SWMM_ENGINE_API int swmm_output2d_datasets(SWMM_Output2D output, char* text, size_t capacity,
                                           size_t* required);
/** Shape of a dataset. rank <= 8; integer is 1 for integral storage,
 * 0 for floating storage, -1 for string storage.
 * dimensions must have room for eight uint64_t values. Scalars have rank 0.
 * unlimited_axes is a bitmask: bit i marks an appendable dimension i. */
SWMM_ENGINE_API int swmm_output2d_shape(SWMM_Output2D output, const char* dataset, int* rank,
                                        uint64_t* dimensions, int* integer,
                                        uint64_t* unlimited_axes);
/** Newline-separated attribute names on a dataset, group, or root ("/"). */
SWMM_ENGINE_API int swmm_output2d_attributes(SWMM_Output2D output, const char* object, char* text,
                                             size_t capacity, size_t* required);
/** String or numeric attribute; numeric vectors are comma-separated. */
SWMM_ENGINE_API int swmm_output2d_attribute(SWMM_Output2D output, const char* object,
                                            const char* name, char* text, size_t capacity,
                                            size_t* required);
/** Read a small one-dimensional string identity table as newline-separated
 * text. Embedded newlines are rejected. Limited to 65536 entries / 64 MiB. */
SWMM_ENGINE_API int swmm_output2d_strings(SWMM_Output2D output, const char* dataset, char* text,
                                          size_t capacity, size_t* required);
/** Bounded hyperslab read in native axis order, converted to double/int64.
 * start/count/stride contain rank entries; stride must be positive. Empty
 * selections succeed. Scalars use NULL axes and capacity >= 1. capacity is
 * the number of elements, not bytes. Shape, selection and capacity are checked
 * before writing to the caller's buffer. int64 reads require integer storage.
 */
SWMM_ENGINE_API int swmm_output2d_read_double(SWMM_Output2D output, const char* dataset,
                                              const uint64_t* start, const uint64_t* count,
                                              const uint64_t* stride, double* values,
                                              size_t capacity);
SWMM_ENGINE_API int swmm_output2d_read_int64(SWMM_Output2D output, const char* dataset,
                                             const uint64_t* start, const uint64_t* count,
                                             const uint64_t* stride, int64_t* values,
                                             size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
