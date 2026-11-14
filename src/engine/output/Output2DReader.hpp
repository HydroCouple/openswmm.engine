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
/**
 * @brief Own one read-only HDF5 result file independently of a simulation engine.
 * @details Callers serialize every operation, including destruction, through
 *          io::resultHdf5Mutex(), shared with the engine's HDF5 output writer.
 *          Paths use HDF5 object naming; result values retain their stored units.
 *          This class throws C++ exceptions. The public swmm_output2d_* wrappers
 *          translate them into status codes and thread-local diagnostics.
 * @note Use closed files or immutable copies; this reader implements no live-writer
 *       synchronization protocol. Instances cannot be copied or assigned.
 */
class Output2DReader {
public:
    /**
     * @brief Close the owned HDF5 file, if opening acquired one.
     * @pre The caller holds the shared HDF5 mutex and no operation is using this instance.
     * @note HDF5 close errors are ignored; destruction provides no error status.
     */
    ~Output2DReader();

    /** @brief Construct an unopened reader without allocating an HDF5 file handle. */
    Output2DReader() = default;

    /** @brief Prohibit copying ownership of the HDF5 file handle. */
    Output2DReader(const Output2DReader&) = delete;

    /** @brief Prohibit replacing ownership through copy assignment. */
    Output2DReader& operator=(const Output2DReader&) = delete;

    /**
     * @brief Open a result file read-only and verify its required mesh/time datasets.
     * @param[in] path Non-null, non-empty, null-terminated filesystem path.
     * @pre This instance has not already acquired a file. The caller holds the HDF5 mutex.
     * @post Success leaves a file with numeric rank-one time/X/Y coordinates and rank-two
     *       integer connectivity with three or four corners per cell. X/Y extents agree.
     * @throws std::invalid_argument For an empty path or an unsupported dataset layout.
     * @throws std::out_of_range If a required mesh/time object is missing.
     * @throws std::runtime_error If opening or another HDF5 operation fails.
     * @note A failed validation may leave a file acquired; destroy the reader to close it.
     *       Calling open() twice is unsupported and does not close the first file.
     */
    void open(const char* path);

    /**
     * @brief List absolute paths of all supported numeric and string datasets.
     * @return Lexicographically sorted paths separated by newlines, with no trailing
     *         newline; an empty string when no supported dataset is found.
     * @pre The reader is open and the caller holds the shared HDF5 mutex.
     * @throws std::invalid_argument If recursive group traversal reaches its depth limit.
     * @throws std::runtime_error If enumeration or an HDF5 object operation fails.
     * @note Only hard links are followed. External and symbolic links are skipped.
     */
    std::string datasets() const;

    /**
     * @brief Inspect a numeric/string dataset without reading its element values.
     * @param[in] path Non-null, non-empty dataset path in the open result file.
     * @param[out] rank Dataset rank in [0,8]; zero for a scalar.
     * @param[out] dimensions Non-null array with eight writable uint64_t entries.
     *                        Only the first rank entries are assigned, in native axis order.
     * @param[out] integer Storage category: 1 for integer, 0 for floating-point, -1 for string.
     * @param[out] unlimited_axes Bit mask with bit i set for an appendable axis i.
     * @pre The reader is open, the caller holds the HDF5 mutex, and dimensions is writable.
     * @throws std::invalid_argument For an empty path, non-dataset object, unsupported
     *                               storage type, or rank outside the supported range.
     * @throws std::out_of_range If the object path cannot be opened.
     * @throws std::runtime_error On an HDF5 failure after opening the object.
     * @note Outputs can be partially assigned before a later validation failure.
     */
    void shape(const char* path, int& rank, uint64_t* dimensions, int& integer,
               uint64_t& unlimited_axes) const;

    /**
     * @brief List attribute names on a dataset, group, or root object.
     * @param[in] path Non-null, non-empty HDF5 object path; "/" selects the root.
     * @return Names in HDF5 name-index order, newline-separated without a final newline;
     *         empty when the object has no attributes.
     * @pre The reader is open and the shared HDF5 mutex is held by the caller.
     * @throws std::invalid_argument For an empty path or an invalid attribute enumeration.
     * @throws std::out_of_range If the object cannot be opened.
     * @throws std::runtime_error On another HDF5 failure.
     */
    std::string attributes(const char* path) const;

    /**
     * @brief Read a supported attribute as a scalar string or comma-separated numeric text.
     * @param[in] path Non-null, non-empty path of the attribute's owning HDF5 object.
     * @param[in] name Non-null, non-empty attribute name.
     * @return The string value, or flattened integer/floating values separated by commas;
     *         floating values use 17 significant digits. Stored units are not converted.
     * @pre The reader is open and the caller holds the shared HDF5 mutex.
     * @throws std::invalid_argument For invalid names, unsupported types, non-scalar string
     *                               attributes, or the reader's excessive-size guards.
     * @throws std::out_of_range If the object or attribute is absent.
     * @throws std::runtime_error If HDF5 cannot read or inspect the attribute.
     */
    std::string attribute(const char* path, const char* name) const;

    /**
     * @brief Read a small rank-one identity table stored as HDF5 strings.
     * @param[in] path Non-null, non-empty path of a one-dimensional string dataset.
     * @return Decoded strings in dataset order, each followed by '\n'; empty for zero entries.
     * @pre The reader is open and the caller holds the shared HDF5 mutex.
     * @throws std::invalid_argument For unsupported type/rank, more than 65,536 entries,
     *                               excessive string payload, or embedded CR/LF characters.
     * @throws std::out_of_range If the dataset is missing.
     * @throws std::runtime_error If HDF5 fails to inspect or read the table.
     * @note Variable/fixed-length strings are supported. Payload limits are 64 MiB;
     *       newline separators and the returned string's allocation add their own overhead.
     */
    std::string strings(const char* path) const;

    /**
     * @brief Copy a validated numeric hyperslab into caller-owned native numeric storage.
     * @param[in] path Non-null, non-empty integer or floating-point dataset path.
     * @param[in] start Zero-based start indices, one per axis; may be null for a scalar.
     * @param[in] count Selected extents, one per axis; may be null for a scalar. A zero
     *                  extent produces an empty selection after validation of every axis.
     * @param[in] stride Strictly positive axis increments; may be null for a scalar.
     * @param[out] values Writable double or int64_t array according to integer; null is
     *                    permitted only when the validated selection contains no elements.
     * @param[in] capacity Available destination elements, not bytes; at least one for a scalar.
     * @param[in] integer True requests native int64_t and requires integer storage;
     *                    false requests double and accepts either numeric storage class.
     * @pre The reader is open, output storage has the correct alignment/type, and the
     *      caller holds the shared HDF5 mutex.
     * @throws std::invalid_argument For unsupported storage, missing arrays, zero strides,
     *                               element-count overflow, or insufficient destination storage.
     * @throws std::out_of_range For a missing dataset or an out-of-bounds selection.
     * @throws std::runtime_error If HDF5 fails to construct or read the selection.
     * @note Validation precedes writing values; an HDF5 read failure itself does not
     *       guarantee rollback of the output buffer. HDF5 performs numeric conversion
     *       in native axis order, without physical-unit conversion or extra range checks.
     */
    void read(const char* path, const uint64_t* start, const uint64_t* count,
              const uint64_t* stride, void* values, size_t capacity, bool integer) const;

private:
    hid_t file_ = -1;
};
}
#endif
