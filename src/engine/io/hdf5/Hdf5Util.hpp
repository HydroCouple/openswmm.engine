// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      Hdf5Util.hpp
 * @brief     RAII handles and columnar table helpers over the raw HDF5 C API.
 *
 * @details The model format is columnar: one group per object class, one
 *          equal-length 1-D dataset per attribute (see STRATEGY.md §3). Nearly
 *          every write is therefore "here is a vector of doubles / ints /
 *          strings, call it `length`, and say its units are feet". These
 *          helpers reduce that to one line so the writer reads as a schema
 *          rather than as HDF5 boilerplate.
 *
 *          Raw C HDF5 (`hdf5.h`, `hid_t`) deliberately — it is what
 *          Default2DOutputPlugin uses, and HDF5 is PRIVATE-linked with hidden
 *          symbols because a consumer linking its own copy (the Qt GUI does)
 *          otherwise dies in H5_term_library on foreign global state.
 *
 * @ingroup engine_hdf5
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

/**
 * @defgroup engine_hdf5 HDF5 model persistence
 * @brief HDF5 input, model output and shared dataset helpers. Contracts distinguish owned versus borrowed handles, array layouts and I/O failure behavior.
 */

#ifndef OPENSWMM_HDF5_UTIL_HPP
#define OPENSWMM_HDF5_UTIL_HPP

#include <hdf5.h>

#include <cstdint>
#include <string>
#include <vector>

namespace openswmm::h5io {

/**
 * @brief Owns an hid_t and closes it with the right H5*close on scope exit.
 *
 * HDF5 identifiers are not RAII and leak silently: a missed H5Dclose keeps the
 * whole file object alive, so H5Fclose "succeeds" while flushing nothing.
 */
class Handle {
public:
    enum class Kind { File, Group, Dataset, Space, Type, Attr, Plist };

    /**
     * @brief Construct an empty owner with identifier -1 and file close kind.
     */
    Handle() = default;
    /**
     * @brief Adopt an HDF5 identifier with its matching close operation.
     * @param[in] id Owned identifier, or a negative value for an empty owner.
     * @param[in] kind Close-function category matching id; not validated.
     * @note The caller must relinquish independent ownership of nonnegative id.
     * Identifiers must come from the engine's HDF5 library instance.
     */
    Handle(hid_t id, Kind kind) : id_(id), kind_(kind) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    /**
     * @brief Transfer identifier ownership from another handle.
     * @param[in,out] o Source owner; left empty with get() == -1.
     */
    Handle(Handle&& o) noexcept : id_(o.id_), kind_(o.kind_) { o.id_ = -1; }
    /**
     * @brief Close this handle and transfer ownership from another.
     * @param[in,out] o Source owner; left empty unless this is self-assignment.
     * @return Reference to this owner. Self-assignment preserves its identifier.
     * @note Close failures are ignored, as in close().
     */
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) {
            close();
            id_ = o.id_;
            kind_ = o.kind_;
            o.id_ = -1;
        }
        return *this;
    }
    /**
     * @brief Close any owned identifier using its stored HDF5 object kind.
     * @note Close errors are ignored and no exception is thrown.
     */
    ~Handle() { close(); }

    /**
     * @brief Inspect the owned identifier without transferring ownership.
     * @return Borrowed identifier, or -1 when empty. It must not be closed by
     * the caller and remains usable only until this owner closes or moves it.
     */
    hid_t get() const noexcept { return id_; }
    /**
     * @brief Test whether this owner stores a nonnegative identifier.
     * @return True when get() >= 0. Does not query HDF5 for identifier validity.
     */
    bool ok() const noexcept { return id_ >= 0; }
    /**
     * @brief Test whether this owner contains an identifier.
     * @return The result of ok(); ownership is unchanged.
     */
    explicit operator bool() const noexcept { return ok(); }

    /// Defined OUT OF LINE on purpose. HDF5 is PRIVATE-linked into the engine
    /// with hidden symbols, so a consumer that also linked its own copy would
    /// have two HDF5 instances with separate global state — the crash the
    /// CMake comments at src/engine/CMakeLists.txt warn about. Keeping every
    /// H5* call inside the engine means a caller needs these headers but not
    /// the library, and ids never cross between instances.
    /**
     * @brief Close any owned identifier and mark this owner empty.
     * @details Calls the matching H5*close within the engine's privately linked
     * HDF5 instance, preventing identifiers from crossing between independent
     * library instances. Repeated calls on an empty owner do nothing.
     * @note HDF5 close failures are ignored and the identifier is discarded anyway.
     */
    void close() noexcept;

private:
    hid_t id_ = -1;
    Kind kind_ = Kind::File;
};

/// A variable-length UTF-8 string type, for name columns.
/**
 * @brief Allocate a variable-length UTF-8 HDF5 string datatype.
 * @return Owned datatype handle, or an empty handle if H5Tcopy fails.
 * @note Subsequent string-size/character-set configuration errors are not
 * checked separately. All HDF5 calls use the engine's library instance.
 */
Handle make_string_type();

/// Open an existing model file read-only. Callers use this rather than
/// H5Fopen so that every HDF5 call stays inside the engine's instance.
/**
 * @brief Open an existing HDF5 file for read-only access.
 * @param[in] path Borrowed filename passed to H5Fopen; must identify a readable file.
 * @return Owned file handle, or an empty handle on open failure.
 */
Handle open_file_readonly(const std::string& path);

/// A column's `units` attribute, or "" when it has none.
/**
 * @brief Read a dataset's units attribute.
 * @param[in] group Borrowed file/group identifier from this HDF5 instance.
 * @param[in] column Dataset name relative to group.
 * @return Owned units string, or an empty string if the link, dataset or
 * attribute cannot be read. An explicitly empty units value also returns empty.
 */
std::string column_units(hid_t group, const std::string& column);

/// Create (or open) a group, creating intermediate groups as needed.
/**
 * @brief Open a group or create it with any missing parent groups.
 * @param[in] loc Borrowed file/group identifier from this HDF5 instance.
 * @param[in] name Group path relative to loc; must denote a group if it exists.
 * @return Owned group handle, or an empty handle on open/create failure.
 * @note Existing group contents are preserved.
 */
Handle make_group(hid_t loc, const std::string& name);
/**
 * @brief Open an existing HDF5 group.
 * @param[in] loc Borrowed file/group identifier from this HDF5 instance.
 * @param[in] name Group path relative to loc.
 * @return Owned group handle, or an empty handle on failure. No groups are created.
 */
Handle open_group(hid_t loc, const std::string& name);

/// True when `loc` contains `name`.
/**
 * @brief Test a path's links while checking each nonempty ancestor component.
 * @param[in] loc Borrowed file/group identifier from this HDF5 instance.
 * @param[in] name Link path; empty components and leading slashes are skipped.
 * @return True when every nonempty accumulated component exists; false for
 * an empty path, a missing ancestor or an HDF5 lookup error.
 * @note Existence does not imply that the target is a group or dataset.
 */
bool has_link(hid_t loc, const std::string& name);

// ---------------------------------------------------------------------------
// Attributes — scalars on a group or on the file root.
// ---------------------------------------------------------------------------
/**
 * @brief Create or replace a scalar std::string attribute.
 * @param[in] loc Borrowed object or file identifier; a file targets its root group.
 * @param[in] name Attribute name, borrowed for this call.
 * @param[in] value Value to store; numeric units are caller-defined.
 * @return True if the attribute was created and written; false on failure.
 * @note An existing attribute is deleted before replacement, so failure may
 * leave it absent. No rollback is performed and no inputs are retained.
 */
bool write_attr(hid_t loc, const std::string& name, const std::string& value);
/**
 * @brief Create or replace a scalar double attribute.
 * @param[in] loc Borrowed object or file identifier; a file targets its root group.
 * @param[in] name Attribute name, borrowed for this call.
 * @param[in] value Value to store; numeric units are caller-defined.
 * @return True if the attribute was created and written; false on failure.
 * @note An existing attribute is deleted before replacement, so failure may
 * leave it absent. No rollback is performed and no inputs are retained.
 */
bool write_attr(hid_t loc, const std::string& name, double value);
/**
 * @brief Create or replace a scalar int attribute.
 * @param[in] loc Borrowed object or file identifier; a file targets its root group.
 * @param[in] name Attribute name, borrowed for this call.
 * @param[in] value Value to store; numeric units are caller-defined.
 * @return True if the attribute was created and written; false on failure.
 * @note An existing attribute is deleted before replacement, so failure may
 * leave it absent. No rollback is performed and no inputs are retained.
 */
bool write_attr(hid_t loc, const std::string& name, int value);

/**
 * @brief Read a scalar attribute as std::string.
 * @param[in] loc Borrowed object or file identifier; a file targets its root group.
 * @param[in] name Attribute name to find.
 * @param[out] out Caller-owned value assigned on success. On HDF5 read
 * failure its value is unspecified; a missing attribute leaves it unchanged.
 * @return True after a successful read; false for missing attributes or HDF5 errors.
 * @note Numeric conversion follows HDF5 rules; string storage is copied and
 * any variable-length HDF5 allocation is reclaimed by the helper.
 */
bool read_attr(hid_t loc, const std::string& name, std::string& out);
/**
 * @brief Read a scalar attribute as double.
 * @param[in] loc Borrowed object or file identifier; a file targets its root group.
 * @param[in] name Attribute name to find.
 * @param[out] out Caller-owned value assigned on success. On HDF5 read
 * failure its value is unspecified; a missing attribute leaves it unchanged.
 * @return True after a successful read; false for missing attributes or HDF5 errors.
 * @note Numeric conversion follows HDF5 rules; string storage is copied and
 * any variable-length HDF5 allocation is reclaimed by the helper.
 */
bool read_attr(hid_t loc, const std::string& name, double& out);
/**
 * @brief Read a scalar attribute as int.
 * @param[in] loc Borrowed object or file identifier; a file targets its root group.
 * @param[in] name Attribute name to find.
 * @param[out] out Caller-owned value assigned on success. On HDF5 read
 * failure its value is unspecified; a missing attribute leaves it unchanged.
 * @return True after a successful read; false for missing attributes or HDF5 errors.
 * @note Numeric conversion follows HDF5 rules; string storage is copied and
 * any variable-length HDF5 allocation is reclaimed by the helper.
 */
bool read_attr(hid_t loc, const std::string& name, int& out);

// ---------------------------------------------------------------------------
// Columns — equal-length 1-D datasets inside a group.
//
// `units` and `description` ride along as dataset attributes so the file
// explains itself without this repository (STRATEGY.md §2). Pass an empty
// string to omit either.
//
// An EMPTY column is still written, as a zero-length dataset: a group whose
// columns disagree on length is the one corruption a columnar reader cannot
// recover from, so shape is always explicit.
// ---------------------------------------------------------------------------
/**
 * @brief Create a one-dimensional double column with optional metadata.
 * @param[in] group Borrowed destination group from this HDF5 instance.
 * @param[in] name New dataset name; an existing link is not replaced.
 * @param[in] values Borrowed column data; empty creates an explicit zero-length dataset.
 * @param[in] units Optional units attribute; empty omits it.
 * @param[in] description Optional description attribute; empty omits it.
 * @param[in] compression Deflate level 1..9; <= 0 disables compression.
 * Empty columns are uncompressed; positive levels request chunks of up to 1024 rows.
 * @return True when dataset creation and the data write succeed; false otherwise.
 * Metadata and property-configuration errors are not reflected separately.
 * @note Values are copied synchronously and no inputs are retained. Failure
 * may leave a dataset or metadata behind; this helper does not roll back.
 */
bool write_column(hid_t group, const std::string& name, const std::vector<double>& values,
                  const std::string& units = "", const std::string& description = "",
                  int compression = 4);

/**
 * @brief Create a one-dimensional int column with optional metadata.
 * @param[in] group Borrowed destination group from this HDF5 instance.
 * @param[in] name New dataset name; an existing link is not replaced.
 * @param[in] values Borrowed column data; empty creates an explicit zero-length dataset.
 * @param[in] units Optional units attribute; empty omits it.
 * @param[in] description Optional description attribute; empty omits it.
 * @param[in] compression Deflate level 1..9; <= 0 disables compression.
 * Empty columns are uncompressed; positive levels request chunks of up to 1024 rows.
 * @return True when dataset creation and the data write succeed; false otherwise.
 * Metadata and property-configuration errors are not reflected separately.
 * @note Values are copied synchronously and no inputs are retained. Failure
 * may leave a dataset or metadata behind; this helper does not roll back.
 */
bool write_column(hid_t group, const std::string& name, const std::vector<int>& values,
                  const std::string& units = "", const std::string& description = "",
                  int compression = 4);

/**
 * @brief Create a one-dimensional std::string column with optional metadata.
 * @param[in] group Borrowed destination group from this HDF5 instance.
 * @param[in] name New dataset name; an existing link is not replaced.
 * @param[in] values Borrowed column data; empty creates an explicit zero-length dataset.
 * @param[in] description Optional description attribute; empty omits it.
 * @param[in] compression Deflate level 1..9; <= 0 disables compression.
 * Empty columns are uncompressed; positive levels request chunks of up to 1024 rows.
 * @return True when dataset creation and the data write succeed; false otherwise.
 * Metadata and property-configuration errors are not reflected separately.
 * @note Values are copied synchronously and no inputs are retained. Failure
 * may leave a dataset or metadata behind; this helper does not roll back.
 */
bool write_column(hid_t group, const std::string& name, const std::vector<std::string>& values,
                  const std::string& description = "", int compression = 4);

/**
 * @brief Read a one-dimensional column into a double vector.
 * @param[in] group Borrowed source group from this HDF5 instance.
 * @param[in] name Existing dataset name relative to group.
 * @param[out] out Caller-owned vector resized to the stored length and
 * filled on success. Missing/open failures leave it unchanged; later failures
 * may leave it resized or partly filled.
 * @return True after a successful read, including an empty column; false
 * for a missing link, failed open or failed data read.
 * @pre The dataset has rank one; shape/type validation is not performed here.
 * @note Numeric conversion follows HDF5 rules. String data is copied before
 * HDF5's temporary variable-length storage is reclaimed.
 */
bool read_column(hid_t group, const std::string& name, std::vector<double>& out);
/**
 * @brief Read a one-dimensional column into a int vector.
 * @param[in] group Borrowed source group from this HDF5 instance.
 * @param[in] name Existing dataset name relative to group.
 * @param[out] out Caller-owned vector resized to the stored length and
 * filled on success. Missing/open failures leave it unchanged; later failures
 * may leave it resized or partly filled.
 * @return True after a successful read, including an empty column; false
 * for a missing link, failed open or failed data read.
 * @pre The dataset has rank one; shape/type validation is not performed here.
 * @note Numeric conversion follows HDF5 rules. String data is copied before
 * HDF5's temporary variable-length storage is reclaimed.
 */
bool read_column(hid_t group, const std::string& name, std::vector<int>& out);
/**
 * @brief Read a one-dimensional column into a std::string vector.
 * @param[in] group Borrowed source group from this HDF5 instance.
 * @param[in] name Existing dataset name relative to group.
 * @param[out] out Caller-owned vector resized to the stored length and
 * filled on success. Missing/open failures leave it unchanged; later failures
 * may leave it resized or partly filled.
 * @return True after a successful read, including an empty column; false
 * for a missing link, failed open or failed data read.
 * @pre The dataset has rank one; shape/type validation is not performed here.
 * @note Numeric conversion follows HDF5 rules. String data is copied before
 * HDF5's temporary variable-length storage is reclaimed.
 */
bool read_column(hid_t group, const std::string& name, std::vector<std::string>& out);

/// Row count of a group's columns — the length of `probe`, or 0 if absent.
/**
 * @brief Inspect the number of rows in a one-dimensional probe column.
 * @param[in] group Borrowed source group from this HDF5 instance.
 * @param[in] probe Dataset name relative to group.
 * @return Row count; zero for an empty, missing or unopenable dataset,
 * or a dataspace-query failure.
 * @pre An existing probe dataset has rank one; rank is not validated here.
 */
std::size_t column_length(hid_t group, const std::string& probe);

} // namespace openswmm::h5io

#endif // OPENSWMM_HDF5_UTIL_HPP
