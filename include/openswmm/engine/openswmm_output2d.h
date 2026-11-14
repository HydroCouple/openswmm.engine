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

/**
 * @brief Query whether saved 2D HDF5 result reading was compiled into this engine.
 * @return 1 when available, or 0 when the backend is absent.
 * @note No file or reader handle is required. This query leaves the thread's
 *       previous reader diagnostic unchanged.
 */
SWMM_ENGINE_API int swmm_output2d_available(void);

/**
 * @brief Open and validate an existing 2D HDF5 result file for read-only access.
 * @param[in] path Non-null, non-empty, null-terminated filesystem path to a saved
 *                 result containing mesh coordinates, connectivity, and time datasets.
 * @param[out] output Required pointer receiving the newly owned reader handle.
 *                    Set to NULL before opening and left NULL on failure.
 * @retval SWMM_OK The file was opened and its required mesh/time layout was validated.
 * @retval SWMM_ERR_BADPARAM An argument or required dataset shape/type is invalid.
 * @retval SWMM_ERR_BADINDEX A required HDF5 object is absent.
 * @retval SWMM_ERR_IO The file cannot be opened or an HDF5 operation fails.
 * @retval SWMM_ERR_NOMEM Allocation fails while opening or validating the reader.
 * @retval SWMM_ERR_PLUGIN This build has no 2D HDF5 result reader.
 * @retval SWMM_ERR_INTERNAL An otherwise unclassified exception was caught.
 * @post On success, the caller must close the handle exactly once with
 *       swmm_output2d_close(). An older handle already stored in output is not closed.
 * @note Read a closed result file or an immutable published copy. Opening does not
 *       establish a concurrent-writer protocol or convert stored scientific units.
 */
SWMM_ENGINE_API int swmm_output2d_open(const char* path, SWMM_Output2D* output);

/**
 * @brief Release a reader and its owned HDF5 file.
 * @param[in] output Handle returned by swmm_output2d_open(), or NULL for a no-op.
 * @pre A non-null handle must still be live and must not be in use by another call.
 * @post A non-null handle is invalid and must not be used or closed again.
 * @note This function leaves the last reader diagnostic unchanged. Backend-disabled
 *       builds perform no action; arbitrary or already-closed handles are not validated.
 */
SWMM_ENGINE_API void swmm_output2d_close(SWMM_Output2D output);

/**
 * @brief Access the calling thread's last saved-result reader diagnostic.
 * @return Borrowed, null-terminated text, empty after a successful checked reader
 *         operation. Copy it before another operation replaces that thread's error.
 * @note The caller must not modify or free the text. Reading this diagnostic, testing
 *       backend availability, and closing a reader do not clear it. It is independent
 *       of an engine simulation handle's error state.
 */
SWMM_ENGINE_API const char* swmm_output2d_error(void);

/**
 * @brief Enumerate readable numeric and string datasets in a saved result.
 * @param[in] output Live reader handle.
 * @param[out] text Destination for null-terminated, newline-separated absolute HDF5
 *                  paths, or NULL with capacity zero to query the required size.
 * @param[in] capacity Available destination bytes, including the terminating NUL.
 * @param[out] required Required pointer receiving the complete byte count including NUL.
 * @return SWMM_OK on success; BADHANDLE for a null reader, BADPARAM for an invalid
 *         or short destination, or the IO/allocation/backend errors described by open().
 * @details Paths are sorted lexicographically. Only hard links are followed; external
 *          and symbolic links are skipped. Excessive group nesting is rejected.
 *          A short buffer updates required without writing partial text. Even a size
 *          query enumerates the datasets and can fail during HDF5 access.
 */
SWMM_ENGINE_API int swmm_output2d_datasets(SWMM_Output2D output, char* text, size_t capacity,
                                           size_t* required);

/**
 * @brief Read a dataset's rank, extents, storage category, and appendable axes.
 * @param[in] output Live reader handle.
 * @param[in] dataset Non-null, non-empty HDF5 path identifying a dataset in this file.
 * @param[out] rank Required pointer receiving the rank in [0,8]; zero denotes a scalar.
 * @param[out] dimensions Required storage for at least eight uint64_t entries. Only
 *                        the first rank entries are written; each is an element count.
 * @param[out] integer Required pointer receiving 1 for integer, 0 for floating-point,
 *                     or -1 for string storage. Other storage classes are rejected.
 * @param[out] unlimited_axes Required pointer receiving a bit mask whose bit i is set
 *                            when axis i has an unlimited maximum extent.
 * @return SWMM_OK on success; BADHANDLE, BADPARAM, BADINDEX, or a backend/IO/allocation
 *         error on failure. Output values must be disregarded after any failure.
 * @note Output pointers remain required for rank-zero scalars. Extents use the native
 *       dataset axis order and do not imply a particular physical unit or time axis.
 */
SWMM_ENGINE_API int swmm_output2d_shape(SWMM_Output2D output, const char* dataset, int* rank,
                                        uint64_t* dimensions, int* integer,
                                        uint64_t* unlimited_axes);

/**
 * @brief Enumerate metadata attribute names on an HDF5 object.
 * @param[in] output Live reader handle.
 * @param[in] object Non-null, non-empty dataset/group path, or "/" for the root object.
 * @param[out] text Null-terminated, newline-separated names; NULL/zero queries size.
 * @param[in] capacity Available bytes in text, including its NUL terminator.
 * @param[out] required Required pointer receiving the complete byte count including NUL.
 * @return SWMM_OK, or BADHANDLE/BADPARAM/BADINDEX and backend/IO/allocation errors.
 * @note Names are enumerated in HDF5 name-index order. A too-small buffer updates
 *       required but is not partially written; an object with no attributes needs one byte.
 */
SWMM_ENGINE_API int swmm_output2d_attributes(SWMM_Output2D output, const char* object, char* text,
                                             size_t capacity, size_t* required);

/**
 * @brief Obtain a string representation of a supported metadata attribute.
 * @param[in] output Live reader handle.
 * @param[in] object Non-null, non-empty path of the owning dataset/group, or "/".
 * @param[in] name Non-null, non-empty attribute name on that object.
 * @param[out] text Destination for null-terminated attribute text, or NULL with zero
 *                  capacity to query size. Empty attribute text still requires its NUL.
 * @param[in] capacity Destination capacity in bytes including the NUL terminator.
 * @param[out] required Required pointer receiving the byte count including NUL.
 * @return SWMM_OK, or BADHANDLE/BADPARAM/BADINDEX and backend/IO/allocation errors.
 * @details Scalar fixed/variable strings and integer/floating-point arrays are supported.
 *          Numeric arrays are flattened into comma-separated values; floating-point
 *          values use 17 significant digits. Unsupported types or excessive sizes fail.
 *          Querying does not allocate a persistent caller-owned string. A short buffer
 *          updates required without copying a partial attribute value.
 */
SWMM_ENGINE_API int swmm_output2d_attribute(SWMM_Output2D output, const char* object,
                                            const char* name, char* text, size_t capacity,
                                            size_t* required);

/**
 * @brief Read a bounded one-dimensional string identity table.
 * @param[in] output Live reader handle.
 * @param[in] dataset Non-null, non-empty path of a rank-one string dataset.
 * @param[out] text Destination for one string per line, or NULL with zero capacity
 *                  to query size. Each entry is followed by a newline, including the last.
 * @param[in] capacity Destination capacity in bytes, including the final NUL terminator.
 * @param[out] required Required pointer receiving the complete byte count including NUL.
 * @return SWMM_OK, or BADHANDLE/BADPARAM/BADINDEX and backend/IO/allocation errors.
 * @details Fixed/variable strings are supported, up to 65,536 entries and the reader's
 *          64 MiB string-payload bound. Embedded CR or LF characters are rejected so
 *          each entry remains one line. A short destination is not partially written.
 * @note A size query still reads the complete table; it is not a cheap metadata query.
 */
SWMM_ENGINE_API int swmm_output2d_strings(SWMM_Output2D output, const char* dataset, char* text,
                                          size_t capacity, size_t* required);

/**
 * @brief Read a bounded numeric hyperslab, converting values to double.
 * @param[in] output Live reader handle.
 * @param[in] dataset Non-null, non-empty path of an integer or floating-point dataset.
 * @param[in] start Zero-based starting indices in native axis order, one per rank axis.
 *                  May be NULL for a scalar, whose rank is zero.
 * @param[in] count Number of selected elements along each axis; any zero makes the
 *                  selection empty. May be NULL only for a scalar.
 * @param[in] stride Strictly positive index increments, one per axis; NULL for a scalar.
 * @param[out] values Storage for the selected doubles in HDF5's native axis order;
 *                    may be NULL only for an empty selection. Caller retains ownership.
 * @param[in] capacity Available double elements, not bytes. A scalar requires at least one.
 * @return SWMM_OK, including for valid empty selections; BADINDEX for an out-of-bounds
 *         selection; BADPARAM for missing axes, zero strides, size overflow, unsupported
 *         types, or insufficient storage; otherwise a handle/backend/IO/allocation error.
 * @details Shape, selection bounds, and capacity are checked before HDF5 copies data.
 *          No physical-unit conversion occurs. Integer-to-double conversion can lose
 *          precision; use swmm_output2d_read_int64() to retain integer identity values.
 *          A failure during the HDF5 read itself does not promise an untouched buffer.
 */
SWMM_ENGINE_API int swmm_output2d_read_double(SWMM_Output2D output, const char* dataset,
                                              const uint64_t* start, const uint64_t* count,
                                              const uint64_t* stride, double* values,
                                              size_t capacity);

/**
 * @brief Read a bounded integer hyperslab as signed 64-bit values.
 * @param[in] output Live reader handle.
 * @param[in] dataset Non-null, non-empty path of an integer dataset; floating-point
 *                    and string storage are rejected rather than rounded or parsed.
 * @param[in] start Zero-based starting indices; one per axis, or NULL for a scalar.
 * @param[in] count Selected extents; one per axis, or NULL for a scalar. A zero extent
 *                  makes the selection empty while all other selection checks still apply.
 * @param[in] stride Strictly positive increments; one per axis, or NULL for a scalar.
 * @param[out] values Caller-owned int64_t storage; NULL is accepted only for an empty
 *                    selection. Values are converted by HDF5 to its native int64 type.
 * @param[in] capacity Available int64_t elements, not bytes; at least one for a scalar.
 * @return The same selection/handle/backend/IO error categories as read_double(), plus
 *         BADPARAM when the dataset does not have integer storage.
 * @note Native axis order and stored scientific units are preserved. The reader adds
 *       no range check beyond HDF5's conversion from the stored integer representation.
 */
SWMM_ENGINE_API int swmm_output2d_read_int64(SWMM_Output2D output, const char* dataset,
                                             const uint64_t* start, const uint64_t* count,
                                             const uint64_t* stride, int64_t* values,
                                             size_t capacity);

#ifdef __cplusplus
}
#endif
#endif
