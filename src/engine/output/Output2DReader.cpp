// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      Output2DReader.cpp
 * @brief     Independent read-only access to saved 2D HDF5 result datasets.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "Output2DReader.hpp"
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
#include <algorithm>
#include <cstring>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace openswmm {
namespace {
struct H5Handle {
    hid_t id;
    herr_t (*close)(hid_t);
    H5Handle(hid_t value, herr_t (*closer)(hid_t)) : id(value), close(closer) {
        if (id < 0)
            throw std::runtime_error("Cannot open HDF5 object or dataspace");
    }
    ~H5Handle() { close(id); }
    H5Handle(const H5Handle&) = delete;
    H5Handle& operator=(const H5Handle&) = delete;
};
void require(bool condition, const char* message) {
    if (!condition)
        throw std::invalid_argument(message);
}
H5Handle object(hid_t file, const char* path) {
    require(path && *path, "An HDF5 object path is required");
    hid_t id;
    H5E_BEGIN_TRY {
        id = H5Oopen(file, path, H5P_DEFAULT);
    }
    H5E_END_TRY;
    if (id < 0)
        throw std::out_of_range(std::string("HDF5 object not found: ") + path);
    return H5Handle(id, H5Oclose);
}
void check(herr_t status) {
    if (status < 0)
        throw std::runtime_error("HDF5 result read failed");
}
std::string joined(const std::vector<std::string>& items) {
    std::string result;
    for (const auto& item : items) {
        if (!result.empty())
            result += '\n';
        result += item;
    }
    return result;
}
void collect(hid_t group, const std::string& prefix, std::vector<std::string>& names,
             int depth = 0) {
    require(depth < 16, "Result groups exceed maximum nesting depth");
    H5G_info_t info{};
    check(H5Gget_info(group, &info));
    for (hsize_t i = 0; i < info.nlinks; ++i) {
        auto length =
            H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i, nullptr, 0, H5P_DEFAULT);
        require(length >= 0, "Cannot enumerate result datasets");
        std::string name(static_cast<size_t>(length) + 1, '\0');
        check(static_cast<herr_t>(H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i,
                                                     name.data(), name.size(), H5P_DEFAULT)));
        name.resize(static_cast<size_t>(length));
        // Do not follow external or symbolic links out of a saved result.
        H5L_info_t link{};
        check(H5Lget_info(group, name.c_str(), &link, H5P_DEFAULT));
        if (link.type != H5L_TYPE_HARD)
            continue;
        auto child = object(group, name.c_str());
        auto path = prefix + "/" + name;
        if (H5Iget_type(child.id) == H5I_GROUP)
            collect(child.id, path, names, depth + 1);
        if (H5Iget_type(child.id) == H5I_DATASET) {
            H5Handle type(H5Dget_type(child.id), H5Tclose);
            auto kind = H5Tget_class(type.id);
            if (kind == H5T_INTEGER || kind == H5T_FLOAT || kind == H5T_STRING)
                names.push_back(path);
        }
    }
}
}

Output2DReader::~Output2DReader() {
    if (file_ >= 0)
        H5Fclose(file_);
}

void Output2DReader::open(const char* path) {
    require(path && *path, "A result file path is required");
    H5E_BEGIN_TRY {
        file_ = H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT);
    }
    H5E_END_TRY;
    if (file_ < 0)
        throw std::runtime_error(std::string("Cannot open 2D result: ") + path);
    // Distinguish results from an HDF5 model input or unrelated HDF5 file.
    for (const char* required : {"/Mesh2_node_x", "/Mesh2_node_y", "/Mesh2_face_nodes", "/time"})
        object(file_, required);
    uint64_t dims[8]{}, unlimited;
    int rank, integer;
    shape("/time", rank, dims, integer, unlimited);
    require(rank == 1 && integer >= 0, "Result time coordinate must be numeric and one-dimensional");
    shape("/Mesh2_node_x", rank, dims, integer, unlimited);
    require(rank == 1 && integer >= 0, "Mesh X coordinates must be a numeric vector");
    const auto vertices = dims[0];
    shape("/Mesh2_node_y", rank, dims, integer, unlimited);
    require(rank == 1 && integer >= 0 && dims[0] == vertices,
            "Mesh X/Y coordinate dimensions differ");
    shape("/Mesh2_face_nodes", rank, dims, integer, unlimited);
    require(rank == 2 && integer == 1 && (dims[1] == 3 || dims[1] == 4),
            "Mesh connectivity must have three or four integer corners per cell");
}

std::string Output2DReader::datasets() const {
    std::vector<std::string> names;
    collect(file_, "", names);
    std::sort(names.begin(), names.end());
    return joined(names);
}

void Output2DReader::shape(const char* path, int& rank, uint64_t* dimensions, int& integer,
                           uint64_t& unlimited_axes) const {
    auto dataset = object(file_, path);
    require(H5Iget_type(dataset.id) == H5I_DATASET, "Object is not a dataset");
    H5Handle space(H5Dget_space(dataset.id), H5Sclose);
    rank = H5Sget_simple_extent_ndims(space.id);
    require(rank >= 0 && rank <= 8, "Result rank must be between zero and eight");
    hsize_t dims[8]{}, maximum[8]{};
    check(H5Sget_simple_extent_dims(space.id, dims, maximum));
    unlimited_axes = 0;
    for (int i = 0; i < rank; ++i)
        dimensions[i] = dims[i];
    for (int i = 0; i < rank; ++i)
        if (maximum[i] == H5S_UNLIMITED)
            unlimited_axes |= uint64_t{1} << i;
    H5Handle type(H5Dget_type(dataset.id), H5Tclose);
    auto kind = H5Tget_class(type.id);
    require(kind == H5T_INTEGER || kind == H5T_FLOAT || kind == H5T_STRING,
            "Unsupported dataset type");
    integer = kind == H5T_STRING ? -1 : kind == H5T_INTEGER;
}

std::string Output2DReader::attributes(const char* path) const {
    auto obj = object(file_, path);
    int count = H5Aget_num_attrs(obj.id);
    require(count >= 0, "Cannot enumerate attributes");
    std::vector<std::string> names;
    for (int i = 0; i < count; ++i) {
        H5Handle attr(
            H5Aopen_by_idx(obj.id, ".", H5_INDEX_NAME, H5_ITER_INC, i, H5P_DEFAULT, H5P_DEFAULT),
            H5Aclose);
        auto length = H5Aget_name(attr.id, 0, nullptr);
        require(length >= 0, "Cannot read attribute name");
        std::string name(static_cast<size_t>(length) + 1, '\0');
        check(static_cast<herr_t>(H5Aget_name(attr.id, name.size(), name.data())));
        name.resize(static_cast<size_t>(length));
        names.push_back(name);
    }
    return joined(names);
}

std::string Output2DReader::attribute(const char* path, const char* name) const {
    require(name && *name, "An attribute name is required");
    auto obj = object(file_, path);
    if (H5Aexists(obj.id, name) <= 0)
        throw std::out_of_range("Result attribute not found");
    H5Handle attr(H5Aopen(obj.id, name, H5P_DEFAULT), H5Aclose);
    H5Handle type(H5Aget_type(attr.id), H5Tclose);
    H5Handle space(H5Aget_space(attr.id), H5Sclose);
    auto n = H5Sget_simple_extent_npoints(space.id);
    require(n >= 0 && n <= 1000000, "Invalid attribute size");
    auto kind = H5Tget_class(type.id);
    if (kind == H5T_STRING) {
        require(n == 1, "Only scalar string attributes are supported");
        if (H5Tis_variable_str(type.id) > 0) {
            char* value = nullptr;
            check(H5Aread(attr.id, type.id, &value));
            std::string result = value ? value : "";
            if (value)
                H5free_memory(value);
            return result;
        }
        auto size = H5Tget_size(type.id);
        require(size <= 1000000, "Invalid string attribute size");
        std::string value(size + 1, '\0');
        check(H5Aread(attr.id, type.id, value.data()));
        value.resize(value.find('\0'));
        return value;
    }
    std::ostringstream out;
    out << std::setprecision(17);
    if (kind == H5T_INTEGER) {
        std::vector<int64_t> values(static_cast<size_t>(n));
        check(H5Aread(attr.id, H5T_NATIVE_INT64, values.data()));
        for (size_t i = 0; i < values.size(); ++i) {
            if (i)
                out << ',';
            out << values[i];
        }
    } else {
        require(kind == H5T_FLOAT, "Unsupported result attribute type");
        std::vector<double> values(static_cast<size_t>(n));
        check(H5Aread(attr.id, H5T_NATIVE_DOUBLE, values.data()));
        for (size_t i = 0; i < values.size(); ++i) {
            if (i)
                out << ',';
            out << values[i];
        }
    }
    return out.str();
}

void Output2DReader::read(const char* path, const uint64_t* start, const uint64_t* count,
                          const uint64_t* stride, void* values, size_t capacity,
                          bool integer) const {
    int rank, stored_integer;
    uint64_t dimensions[8]{}, unlimited;
    shape(path, rank, dimensions, stored_integer, unlimited);
    require(stored_integer >= 0, "Use strings() for string identity tables");
    require(!integer || stored_integer == 1, "Integer reads require integer storage");
    require(rank == 0 || (start && count && stride), "Selection axes are required");
    hsize_t offset[8]{}, extent[8]{}, step[8]{};
    size_t total = 1;
    for (int i = 0; i < rank; ++i) {
        require(stride[i] > 0, "Selection stride must be positive");
        if (start[i] > dimensions[i] ||
            (count[i] && (start[i] == dimensions[i] ||
                          (count[i] - 1) > (dimensions[i] - 1 - start[i]) / stride[i])))
            throw std::out_of_range("Result selection exceeds dataset shape");
        require(!count[i] || total <= std::numeric_limits<size_t>::max() / count[i],
                "Selection element count overflows size_t");
        total *= count[i];
        offset[i] = start[i];
        extent[i] = count[i];
        step[i] = stride[i];
    }
    require(total <= capacity && (!total || values), "Result buffer is too small or null");
    if (!total)
        return;
    auto dataset = object(file_, path);
    H5Handle file_space(H5Dget_space(dataset.id), H5Sclose);
    if (rank)
        check(H5Sselect_hyperslab(file_space.id, H5S_SELECT_SET, offset, step, extent, nullptr));
    H5Handle memory_space(rank ? H5Screate_simple(rank, extent, nullptr) : H5Screate(H5S_SCALAR),
                          H5Sclose);
    check(H5Dread(dataset.id, integer ? H5T_NATIVE_INT64 : H5T_NATIVE_DOUBLE, memory_space.id,
                  file_space.id, H5P_DEFAULT, values));
}

std::string Output2DReader::strings(const char* path) const {
    int rank, integer;
    uint64_t dims[8]{}, unlimited;
    shape(path, rank, dims, integer, unlimited);
    require(rank == 1 && integer == -1 && dims[0] <= 65536,
            "Expected a one-dimensional string identity table of at most 65536 entries");
    auto dataset = object(file_, path);
    H5Handle type(H5Dget_type(dataset.id), H5Tclose);
    std::vector<std::string> result;
    const auto n = static_cast<size_t>(dims[0]);
    if (H5Tis_variable_str(type.id) > 0) {
        std::vector<char*> raw(n, nullptr);
        if (n)
            check(H5Dread(dataset.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw.data()));
        try {
            size_t bytes = 0;
            for (auto value : raw) {
                bytes += value ? std::strlen(value) : 0;
                require(bytes <= 64 * 1024 * 1024, "Identity table exceeds 64 MiB");
                result.emplace_back(value ? value : "");
            }
        } catch (...) {
            for (auto value : raw)
                if (value)
                    H5free_memory(value);
            throw;
        }
        for (auto value : raw)
            if (value)
                H5free_memory(value);
    } else {
        const size_t width = H5Tget_size(type.id);
        require(width > 0 && width <= 64 * 1024 * 1024 / std::max<size_t>(1, n),
                "Identity table exceeds 64 MiB");
        std::vector<char> raw(n * width);
        if (n)
            check(H5Dread(dataset.id, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, raw.data()));
        for (size_t i = 0; i < n; ++i) {
            const char* begin = raw.data() + i * width;
            result.emplace_back(begin, std::find(begin, begin + width, '\0'));
        }
    }
    std::string text;
    for (const auto& value : result) {
        require(value.find_first_of("\r\n") == std::string::npos,
                "Newlines in result identities are unsupported");
        text += value + '\n';
    }
    return text;
}
}
#endif
