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
 * @file Hdf5Util.hpp
 * @brief RAII handles and columnar table helpers over the raw HDF5 C API.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
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

    Handle() = default;
    Handle(hid_t id, Kind kind) : id_(id), kind_(kind) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&& o) noexcept : id_(o.id_), kind_(o.kind_) { o.id_ = -1; }
    Handle& operator=(Handle&& o) noexcept {
        if (this != &o) { close(); id_ = o.id_; kind_ = o.kind_; o.id_ = -1; }
        return *this;
    }
    ~Handle() { close(); }

    hid_t get() const noexcept { return id_; }
    bool  ok()  const noexcept { return id_ >= 0; }
    explicit operator bool() const noexcept { return ok(); }

    /// Defined OUT OF LINE on purpose. HDF5 is PRIVATE-linked into the engine
    /// with hidden symbols, so a consumer that also linked its own copy would
    /// have two HDF5 instances with separate global state — the crash the
    /// CMake comments at src/engine/CMakeLists.txt warn about. Keeping every
    /// H5* call inside the engine means a caller needs these headers but not
    /// the library, and ids never cross between instances.
    void close() noexcept;

private:
    hid_t id_ = -1;
    Kind  kind_ = Kind::File;
};

/// A variable-length UTF-8 string type, for name columns.
Handle make_string_type();

/// Open an existing model file read-only. Callers use this rather than
/// H5Fopen so that every HDF5 call stays inside the engine's instance.
Handle open_file_readonly(const std::string& path);

/// A column's `units` attribute, or "" when it has none.
std::string column_units(hid_t group, const std::string& column);

/// Create (or open) a group, creating intermediate groups as needed.
Handle make_group(hid_t loc, const std::string& name);
Handle open_group(hid_t loc, const std::string& name);

/// True when `loc` contains `name`.
bool has_link(hid_t loc, const std::string& name);

// ---------------------------------------------------------------------------
// Attributes — scalars on a group or on the file root.
// ---------------------------------------------------------------------------
bool write_attr(hid_t loc, const std::string& name, const std::string& value);
bool write_attr(hid_t loc, const std::string& name, double value);
bool write_attr(hid_t loc, const std::string& name, int value);

bool read_attr(hid_t loc, const std::string& name, std::string& out);
bool read_attr(hid_t loc, const std::string& name, double& out);
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
bool write_column(hid_t group, const std::string& name,
                  const std::vector<double>& values,
                  const std::string& units = "",
                  const std::string& description = "",
                  int compression = 4);

bool write_column(hid_t group, const std::string& name,
                  const std::vector<int>& values,
                  const std::string& units = "",
                  const std::string& description = "",
                  int compression = 4);

bool write_column(hid_t group, const std::string& name,
                  const std::vector<std::string>& values,
                  const std::string& description = "",
                  int compression = 4);

bool read_column(hid_t group, const std::string& name, std::vector<double>& out);
bool read_column(hid_t group, const std::string& name, std::vector<int>& out);
bool read_column(hid_t group, const std::string& name, std::vector<std::string>& out);

/// Row count of a group's columns — the length of `probe`, or 0 if absent.
std::size_t column_length(hid_t group, const std::string& probe);

}  // namespace openswmm::h5io

#endif  // OPENSWMM_HDF5_UTIL_HPP
