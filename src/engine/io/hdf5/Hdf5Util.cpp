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
 * @file      Hdf5Util.cpp
 * @brief     Implementation of the RAII handles and columnar helpers.
 * @ingroup engine_hdf5
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Hdf5Util.hpp"
#include "../Hdf5ObjectCreation.hpp"

#include <cstring>
#include <vector>

namespace openswmm::h5io {
namespace {

/// Chunking is required for a compressed or extendible dataset, and pointless
/// for a tiny one. HDF5 rejects a zero-sized chunk, so an empty column is
/// written contiguous and uncompressed.
/**
 * @brief Build optional chunking and deflate properties for a column.
 * @param[in] n Row count; zero requests default contiguous storage.
 * @param[in] compression Deflate level 1..9; <= 0 disables compression.
 * @return Owned property list, or an empty handle for default storage or
 * allocation failure. Chunks contain min(n, 1024) rows.
 * @note H5Pset_chunk/H5Pset_deflate errors are not checked by this helper.
 */
Handle make_dcpl(hsize_t n, int compression) {
    if (n == 0 || compression <= 0)
        return {};
    Handle plist(H5Pcreate(H5P_DATASET_CREATE), Handle::Kind::Plist);
    if (!plist)
        return {};
    hsize_t chunk = n < 1024 ? n : 1024;
    H5Pset_chunk(plist.get(), 1, &chunk);
    H5Pset_deflate(plist.get(), static_cast<unsigned>(compression));
    return plist;
}

/// Attributes attach to an OBJECT, and a file id is not one. H5Aexists accepts
/// a file id on some builds and refuses it on others (vcpkg's 1.14.4 refuses
/// it on read with "can't set object access arguments"), so resolve a file id
/// to its root group once and use that everywhere.
struct AttrTarget {
    Handle owned; // holds "/" only when `loc` was a file
    hid_t id = -1;

    /**
     * @brief Resolve a file identifier to its root group for attribute access.
     * @param[in] loc Borrowed file or object identifier from this HDF5 instance.
     * @post id holds a borrowed object id, or the root group owned by this
     * adapter. Failed root opening sets a negative id. loc is not closed.
     */
    explicit AttrTarget(hid_t loc) {
        if (H5Iget_type(loc) == H5I_FILE) {
            owned = Handle(H5Gopen2(loc, "/", H5P_DEFAULT), Handle::Kind::Group);
            id = owned.get();
        } else {
            id = loc;
        }
    }
    /**
     * @brief Check whether attribute-target resolution produced an identifier.
     * @return True when id >= 0; no HDF5 validity query is performed.
     */
    bool ok() const { return id >= 0; }
};

/**
 * @brief Attempt to annotate a dataset with optional units and description.
 * @param[in] ds Borrowed dataset identifier.
 * @param[in] units Units text; empty omits the attribute.
 * @param[in] description Description text; empty omits the attribute.
 * @note Attribute write errors are ignored; no input is retained.
 */
void tag_column(hid_t ds, const std::string& units, const std::string& description) {
    if (!units.empty())
        write_attr(ds, "units", units);
    if (!description.empty())
        write_attr(ds, "description", description);
}

/// Create a 1-D dataset of `type` holding `n` elements.
/**
 * @brief Create a one-dimensional dataset and optional compression properties.
 * @param[in] group Borrowed destination group identifier.
 * @param[in] name New dataset name; no replacement is attempted.
 * @param[in] type Borrowed datatype identifier stored by the dataset.
 * @param[in] n Element count; zero creates an explicit empty dataset.
 * @param[in] compression Deflate level 1..9; <= 0 requests uncompressed storage.
 * @return Owned dataset handle, or an empty handle on dataspace/create failure.
 * Property setup failures can fall back to defaults or be reported by HDF5.
 */
Handle make_dataset(hid_t group, const std::string& name, hid_t type, hsize_t n, int compression) {
    Handle space(H5Screate_simple(1, &n, nullptr), Handle::Kind::Space);
    if (!space)
        return {};
    Handle dcpl = make_dcpl(n, compression);
    return Handle(H5Dcreate2(group, name.c_str(), type, space.get(), H5P_DEFAULT,
                             dcpl ? dcpl.get() : H5P_DEFAULT, H5P_DEFAULT),
                  Handle::Kind::Dataset);
}

} // namespace

// Contract: openswmm::h5io::Handle::close.
void Handle::close() noexcept {
    if (id_ < 0)
        return;
    switch (kind_) {
    case Kind::File:
        H5Fclose(id_);
        break;
    case Kind::Group:
        H5Gclose(id_);
        break;
    case Kind::Dataset:
        H5Dclose(id_);
        break;
    case Kind::Space:
        H5Sclose(id_);
        break;
    case Kind::Type:
        H5Tclose(id_);
        break;
    case Kind::Attr:
        H5Aclose(id_);
        break;
    case Kind::Plist:
        H5Pclose(id_);
        break;
    }
    id_ = -1;
}

// Contract: openswmm::h5io::open_file_readonly.
Handle open_file_readonly(const std::string& path) {
    return Handle(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), Handle::Kind::File);
}

// Contract: openswmm::h5io::column_units.
std::string column_units(hid_t group, const std::string& column) {
    if (H5Lexists(group, column.c_str(), H5P_DEFAULT) <= 0)
        return {};
    Handle ds(H5Dopen2(group, column.c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
    if (!ds)
        return {};
    std::string out;
    return read_attr(ds.get(), "units", out) ? out : std::string();
}

// Contract: openswmm::h5io::make_string_type.
Handle make_string_type() {
    Handle t(H5Tcopy(H5T_C_S1), Handle::Kind::Type);
    if (!t)
        return t;
    H5Tset_size(t.get(), H5T_VARIABLE);
    H5Tset_cset(t.get(), H5T_CSET_UTF8);
    return t;
}

// Contract: openswmm::h5io::make_group.
Handle make_group(hid_t loc, const std::string& name) {
    if (has_link(loc, name))
        return open_group(loc, name);
    // Intermediate groups on demand, so "nodes/junctions" needs one call.
    Handle lcpl(H5Pcreate(H5P_LINK_CREATE), Handle::Kind::Plist);
    if (lcpl)
        H5Pset_create_intermediate_group(lcpl.get(), 1);
    return Handle(io::createHdf5Group(loc, name.c_str(), lcpl ? lcpl.get() : H5P_DEFAULT,
                                      H5P_DEFAULT, H5P_DEFAULT),
                  Handle::Kind::Group);
}

// Contract: openswmm::h5io::open_group.
Handle open_group(hid_t loc, const std::string& name) {
    return Handle(H5Gopen2(loc, name.c_str(), H5P_DEFAULT), Handle::Kind::Group);
}

// Contract: openswmm::h5io::has_link.
bool has_link(hid_t loc, const std::string& name) {
    // H5Lexists only checks one level, so walk the path; a missing ancestor
    // must answer false rather than raise.
    std::size_t start = 0;
    std::string acc;
    while (start <= name.size()) {
        const std::size_t slash = name.find('/', start);
        const std::string part = name.substr(start, slash - start);
        if (!part.empty()) {
            acc += (acc.empty() ? "" : "/") + part;
            const htri_t e = H5Lexists(loc, acc.c_str(), H5P_DEFAULT);
            if (e <= 0)
                return false;
        }
        if (slash == std::string::npos)
            break;
        start = slash + 1;
    }
    return !acc.empty();
}

// ---------------------------------------------------------------------------
// Attributes
// ---------------------------------------------------------------------------

// Contract: openswmm::h5io::write_attr(hid_t, const std::string&, const std::string&).
bool write_attr(hid_t loc_in, const std::string& name, const std::string& value) {
    AttrTarget t(loc_in);
    if (!t.ok())
        return false;
    const hid_t loc = t.id;
    if (H5Aexists(loc, name.c_str()) > 0)
        H5Adelete(loc, name.c_str());
    Handle type(H5Tcopy(H5T_C_S1), Handle::Kind::Type);
    if (!type)
        return false;
    // A fixed-size attribute type keeps the value readable from h5dump and
    // MATLAB, both of which handle variable-length attributes poorly.
    H5Tset_size(type.get(), value.empty() ? 1 : value.size());
    H5Tset_cset(type.get(), H5T_CSET_UTF8);
    Handle space(H5Screate(H5S_SCALAR), Handle::Kind::Space);
    Handle attr(H5Acreate2(loc, name.c_str(), type.get(), space.get(), H5P_DEFAULT, H5P_DEFAULT),
                Handle::Kind::Attr);
    if (!attr)
        return false;
    return H5Awrite(attr.get(), type.get(), value.empty() ? "" : value.data()) >= 0;
}

// Contract: openswmm::h5io::write_attr(hid_t, const std::string&, double).
bool write_attr(hid_t loc_in, const std::string& name, double value) {
    AttrTarget t(loc_in);
    if (!t.ok())
        return false;
    const hid_t loc = t.id;
    if (H5Aexists(loc, name.c_str()) > 0)
        H5Adelete(loc, name.c_str());
    Handle space(H5Screate(H5S_SCALAR), Handle::Kind::Space);
    Handle attr(
        H5Acreate2(loc, name.c_str(), H5T_NATIVE_DOUBLE, space.get(), H5P_DEFAULT, H5P_DEFAULT),
        Handle::Kind::Attr);
    if (!attr)
        return false;
    return H5Awrite(attr.get(), H5T_NATIVE_DOUBLE, &value) >= 0;
}

// Contract: openswmm::h5io::write_attr(hid_t, const std::string&, int).
bool write_attr(hid_t loc_in, const std::string& name, int value) {
    AttrTarget t(loc_in);
    if (!t.ok())
        return false;
    const hid_t loc = t.id;
    if (H5Aexists(loc, name.c_str()) > 0)
        H5Adelete(loc, name.c_str());
    Handle space(H5Screate(H5S_SCALAR), Handle::Kind::Space);
    Handle attr(
        H5Acreate2(loc, name.c_str(), H5T_NATIVE_INT, space.get(), H5P_DEFAULT, H5P_DEFAULT),
        Handle::Kind::Attr);
    if (!attr)
        return false;
    return H5Awrite(attr.get(), H5T_NATIVE_INT, &value) >= 0;
}

// Contract: openswmm::h5io::read_attr(hid_t, const std::string&, std::string&).
bool read_attr(hid_t loc_in, const std::string& name, std::string& out) {
    AttrTarget t(loc_in);
    if (!t.ok())
        return false;
    const hid_t loc = t.id;
    if (H5Aexists(loc, name.c_str()) <= 0)
        return false;
    Handle attr(H5Aopen(loc, name.c_str(), H5P_DEFAULT), Handle::Kind::Attr);
    if (!attr)
        return false;
    Handle type(H5Aget_type(attr.get()), Handle::Kind::Type);
    if (!type)
        return false;
    if (H5Tis_variable_str(type.get()) > 0) {
        char* raw = nullptr;
        Handle mem = make_string_type();
        if (H5Aread(attr.get(), mem.get(), &raw) < 0)
            return false;
        out = raw ? raw : "";
        if (raw)
            H5free_memory(raw);
        return true;
    }
    const std::size_t n = H5Tget_size(type.get());
    std::vector<char> buf(n + 1, '\0');
    if (H5Aread(attr.get(), type.get(), buf.data()) < 0)
        return false;
    out.assign(buf.data(), strnlen(buf.data(), n));
    return true;
}

// Contract: openswmm::h5io::read_attr(hid_t, const std::string&, double&).
bool read_attr(hid_t loc_in, const std::string& name, double& out) {
    AttrTarget t(loc_in);
    if (!t.ok())
        return false;
    const hid_t loc = t.id;
    if (H5Aexists(loc, name.c_str()) <= 0)
        return false;
    Handle attr(H5Aopen(loc, name.c_str(), H5P_DEFAULT), Handle::Kind::Attr);
    if (!attr)
        return false;
    return H5Aread(attr.get(), H5T_NATIVE_DOUBLE, &out) >= 0;
}

// Contract: openswmm::h5io::read_attr(hid_t, const std::string&, int&).
bool read_attr(hid_t loc_in, const std::string& name, int& out) {
    AttrTarget t(loc_in);
    if (!t.ok())
        return false;
    const hid_t loc = t.id;
    if (H5Aexists(loc, name.c_str()) <= 0)
        return false;
    Handle attr(H5Aopen(loc, name.c_str(), H5P_DEFAULT), Handle::Kind::Attr);
    if (!attr)
        return false;
    return H5Aread(attr.get(), H5T_NATIVE_INT, &out) >= 0;
}

// ---------------------------------------------------------------------------
// Columns
// ---------------------------------------------------------------------------

// Contract: openswmm::h5io::write_column(hid_t, const std::string&, const std::vector<double>&, const std::string&, const std::string&, int).
bool write_column(hid_t group, const std::string& name, const std::vector<double>& values,
                  const std::string& units, const std::string& description, int compression) {
    Handle ds =
        make_dataset(group, name, H5T_IEEE_F64LE, static_cast<hsize_t>(values.size()), compression);
    if (!ds)
        return false;
    tag_column(ds.get(), units, description);
    if (values.empty())
        return true;
    return H5Dwrite(ds.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >= 0;
}

// Contract: openswmm::h5io::write_column(hid_t, const std::string&, const std::vector<int>&, const std::string&, const std::string&, int).
bool write_column(hid_t group, const std::string& name, const std::vector<int>& values,
                  const std::string& units, const std::string& description, int compression) {
    Handle ds =
        make_dataset(group, name, H5T_STD_I32LE, static_cast<hsize_t>(values.size()), compression);
    if (!ds)
        return false;
    tag_column(ds.get(), units, description);
    if (values.empty())
        return true;
    return H5Dwrite(ds.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >= 0;
}

// Contract: openswmm::h5io::write_column(hid_t, const std::string&, const std::vector<std::string>&, const std::string&, int).
bool write_column(hid_t group, const std::string& name, const std::vector<std::string>& values,
                  const std::string& description, int compression) {
    Handle type = make_string_type();
    if (!type)
        return false;
    Handle ds =
        make_dataset(group, name, type.get(), static_cast<hsize_t>(values.size()), compression);
    if (!ds)
        return false;
    tag_column(ds.get(), "", description);
    if (values.empty())
        return true;
    // HDF5 writes variable-length strings from an array of char*, so the
    // pointers must outlive the call — hence the temporary vector.
    std::vector<const char*> ptrs;
    ptrs.reserve(values.size());
    for (const auto& s : values)
        ptrs.push_back(s.c_str());
    return H5Dwrite(ds.get(), type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, ptrs.data()) >= 0;
}

namespace {
/// Element count of a 1-D dataset.
/**
 * @brief Query the first dimension of a one-dimensional dataset.
 * @param[in] ds Borrowed dataset identifier from this HDF5 instance.
 * @return First dimension length, or zero if a dataspace query fails.
 * @pre ds describes a rank-one dataset; rank is not validated by this helper.
 */
hsize_t dataset_length(hid_t ds) {
    Handle space(H5Dget_space(ds), Handle::Kind::Space);
    if (!space)
        return 0;
    hsize_t dims[1] = {0};
    if (H5Sget_simple_extent_dims(space.get(), dims, nullptr) < 0)
        return 0;
    return dims[0];
}
} // namespace

// Contract: openswmm::h5io::read_column(hid_t, const std::string&, std::vector<double>&).
bool read_column(hid_t group, const std::string& name, std::vector<double>& out) {
    if (H5Lexists(group, name.c_str(), H5P_DEFAULT) <= 0)
        return false;
    Handle ds(H5Dopen2(group, name.c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
    if (!ds)
        return false;
    out.assign(static_cast<std::size_t>(dataset_length(ds.get())), 0.0);
    if (out.empty())
        return true;
    return H5Dread(ds.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) >= 0;
}

// Contract: openswmm::h5io::read_column(hid_t, const std::string&, std::vector<int>&).
bool read_column(hid_t group, const std::string& name, std::vector<int>& out) {
    if (H5Lexists(group, name.c_str(), H5P_DEFAULT) <= 0)
        return false;
    Handle ds(H5Dopen2(group, name.c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
    if (!ds)
        return false;
    out.assign(static_cast<std::size_t>(dataset_length(ds.get())), 0);
    if (out.empty())
        return true;
    return H5Dread(ds.get(), H5T_NATIVE_INT, H5S_ALL, H5S_ALL, H5P_DEFAULT, out.data()) >= 0;
}

// Contract: openswmm::h5io::read_column(hid_t, const std::string&, std::vector<std::string>&).
bool read_column(hid_t group, const std::string& name, std::vector<std::string>& out) {
    if (H5Lexists(group, name.c_str(), H5P_DEFAULT) <= 0)
        return false;
    Handle ds(H5Dopen2(group, name.c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
    if (!ds)
        return false;
    const auto n = static_cast<std::size_t>(dataset_length(ds.get()));
    out.assign(n, std::string());
    if (n == 0)
        return true;
    Handle type = make_string_type();
    std::vector<char*> raw(n, nullptr);
    if (H5Dread(ds.get(), type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, raw.data()) < 0)
        return false;
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = raw[i] ? raw[i] : "";
    }
    // Variable-length reads allocate per element; H5Dvlen_reclaim is the only
    // sanctioned way to give them back.
    Handle space(H5Dget_space(ds.get()), Handle::Kind::Space);
    H5Dvlen_reclaim(type.get(), space.get(), H5P_DEFAULT, raw.data());
    return true;
}

// Contract: openswmm::h5io::column_length.
std::size_t column_length(hid_t group, const std::string& probe) {
    if (H5Lexists(group, probe.c_str(), H5P_DEFAULT) <= 0)
        return 0;
    Handle ds(H5Dopen2(group, probe.c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
    if (!ds)
        return 0;
    return static_cast<std::size_t>(dataset_length(ds.get()));
}

} // namespace openswmm::h5io
