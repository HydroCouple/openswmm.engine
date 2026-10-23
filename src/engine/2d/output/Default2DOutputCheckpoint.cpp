// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      Default2DOutputCheckpoint.cpp
 * @brief     Checkpoint persistence for the default 2D output plugin.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifdef OPENSWMM_HAS_2D
#include "Default2DOutputPlugin.hpp"
#include "../subsurface/SourceResultHistory.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include "../../core/DateTime.hpp"
#include "../../core/FileIO.hpp"
#include "../../io/Hdf5Mutex.hpp"
#include "../../io/CheckpointHdf5.hpp"
#include "../../core/SimulationContext.hpp"
#include "../data/Report2DVars.hpp"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <limits>
#include <set>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
constexpr std::size_t maxBytes = 64 * 1024 * 1024;
/**
 * @brief Enforce a mesh-result checkpoint condition.
 * @param[in] ok Condition; true returns normally.
 * @param[in] message Nonnull exception diagnostic.
 * @throws std::runtime_error When ok is false; performs no rollback or HDF5 cleanup itself.
 */
void require(bool ok, const char* message) {
    if (!ok)
        throw std::runtime_error(message);
}
struct Handle {
    hid_t id;
    herr_t (*close)(hid_t);
    /**
     * @brief Close a nonnegative owned HDF5 identifier with its matching close callback.
     * @note The callback must be valid and nonthrowing; close errors are ignored. A negative
     * identifier is an empty handle. This aggregate must not be copied into multiple owners of
     * the same ID.
     */
    ~Handle() {
        if (id >= 0)
            close(id);
    }
};
/**
 * @brief Read a complete bounded mesh-result prefix through its existing native descriptor.
 * @param[in] path Existing stable path used to inspect file length, in [1,64 MiB].
 * @param[in] descriptor Borrowed valid seekable descriptor for that same open result file,
 * permitting reads. Ownership remains with the HDF5 writer; no descriptor is closed here.
 * @return Owned binary prefix from offset zero of the descriptor, using the path's measured
 * length. Content is not written and extra trailing growth is not checked by this helper.
 * @throws std::runtime_error On missing/oversized path or descriptor range/read/position-restore
 * failure. Allocation errors propagate.
 * @note checkpointReadOutput() temporarily changes the descriptor position and attempts to
 * restore it. Caller must exclude all competing descriptor I/O and file/path replacement during
 * size inspection and read. HDF5 serialization and a stable matching path/descriptor are
 * required.
 */
std::string readFile(const std::filesystem::path& path, int descriptor) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    require(!ec && size > 0 && size <= maxBytes,
            "Mesh result prefix is missing or exceeds its checkpoint limit.");
    std::string bytes(size, '\0');
    io::checkpointReadOutput(descriptor, 0, bytes.data(), bytes.size());
    return bytes;
}
/**
 * @brief Open a writable in-memory HDF5 file image without backing-store publication.
 * @param[in] bytes Stable valid HDF5 file image; borrowed while access properties are populated.
 * @return Owned nonnegative file ID. Caller closes it with H5Fclose; access-property IDs close
 * here.
 * @throws std::runtime_error On property/image setup or HDF5 open failure. Does not edit bytes
 * or publish a filesystem artifact; caller must hold the result HDF5 mutex.
 */
hid_t openImage(const std::string& bytes) {
    Handle props{H5Pcreate(H5P_FILE_ACCESS), H5Pclose};
    require(props.id >= 0 && H5Pset_fapl_core(props.id, 4096, false) >= 0 &&
                H5Pset_file_image(props.id, const_cast<char*>(bytes.data()), bytes.size()) >= 0,
            "Cannot prepare mesh schema image.");
    const auto file = H5Fopen("OpenSWMM mesh checkpoint schema", H5F_ACC_RDWR, props.id);
    require(file >= 0, "Cannot open mesh schema image.");
    return file;
}
/**
 * @brief Enumerate a bounded HDF5 group's immediate local hard-link names.
 * @param[in] file Borrowed open file/group ID.
 * @return Owned sorted unique names, with at most 256 links and name lengths below 256 bytes.
 * @throws std::runtime_error On inspection/read failure, bad name size or non-hard links. Does
 * not follow target objects here or modify the group. Caller supplies HDF5 serialization.
 */
std::set<std::string> links(hid_t file) {
    H5G_info_t info{};
    require(H5Gget_info(file, &info) >= 0 && info.nlinks <= 256, "Invalid mesh result schema.");
    std::set<std::string> names;
    for (hsize_t k = 0; k < info.nlinks; ++k) {
        const auto len =
            H5Lget_name_by_idx(file, ".", H5_INDEX_NAME, H5_ITER_INC, k, nullptr, 0, H5P_DEFAULT);
        require(len > 0 && len < 256, "Invalid mesh result name.");
        std::string name(len + 1, '\0');
        require(H5Lget_name_by_idx(file, ".", H5_INDEX_NAME, H5_ITER_INC, k, name.data(),
                                   name.size(), H5P_DEFAULT) == len,
                "Cannot read mesh result name.");
        name.resize(len);
        H5L_info_t link{};
        require(H5Lget_info(file, name.c_str(), &link, H5P_DEFAULT) >= 0 &&
                    link.type == H5L_TYPE_HARD,
                "Mesh result links must be local hard links.");
        names.insert(std::move(name));
    }
    return names;
}
/**
 * @brief Read a borrowed dataset's current or maximum extent vector.
 * @param[in] dataset Valid open HDF5 dataset ID; ownership remains with the caller.
 * @param[in] maximum True selects maximum extents, including H5S_UNLIMITED; false current
 * extents.
 * @return Owned dimension vector for rank 0..4; empty for a scalar dataspace.
 * @throws std::runtime_error On invalid rank or extent inspection. No dataset mutation occurs.
 */
std::vector<hsize_t> shape(hid_t dataset, bool maximum = false) {
    Handle space{H5Dget_space(dataset), H5Sclose};
    const int rank = H5Sget_simple_extent_ndims(space.id);
    require(rank >= 0 && rank <= 4, "Invalid mesh dataset rank.");
    std::vector<hsize_t> dims(rank), max(rank);
    require(H5Sget_simple_extent_dims(space.id, dims.data(), max.data()) >= 0,
            "Cannot inspect mesh dimensions.");
    return maximum ? max : dims;
}
/**
 * @brief Read bounded native-type bytes for exact static dataset/attribute comparison.
 * @param[in] object Borrowed open dataset or attribute ID, as selected by attribute.
 * @param[in] attribute True selects an attribute; false a dataset.
 * @return Owned representation: raw stored-type bytes for numeric/fixed attribute strings, or
 * length-prefixed NUL-separated values for variable-length string datasets. Temporary HDF5 vlen
 * strings are reclaimed before return. Result/value size is bounded by 64 MiB.
 * @throws std::runtime_error On unsupported type, invalid size or HDF5 read failure. Variable
 * strings are accepted only for datasets; fixed dataset strings are not handled by this helper.
 * @note No object mutation; caller holds HDF5 serialization. Allocation exceptions propagate.
 */
std::string raw(hid_t object, bool attribute) {
    Handle type{attribute ? H5Aget_type(object) : H5Dget_type(object), H5Tclose};
    Handle space{attribute ? H5Aget_space(object) : H5Dget_space(object), H5Sclose};
    const auto n = H5Sget_simple_extent_npoints(space.id);
    const auto width = H5Tget_size(type.id);
    const auto kind = H5Tget_class(type.id);
    if (!attribute && kind == H5T_STRING && H5Tis_variable_str(type.id) > 0) {
        require(n >= 0 && std::size_t(n) <= maxBytes / sizeof(char*),
                "Oversized mesh string result mapping.");
        std::vector<char*> values(static_cast<std::size_t>(n), nullptr);
        struct ReclaimStrings {
            hid_t type;
            hid_t space;
            void* values;
            /**
             * @brief Reclaim HDF5-owned variable-length string elements after a dataset read.
             * @note Borrows its type, dataspace and vector storage; all must outlive this
             * cleanup scope. HDF5 reclaim errors are ignored and no C++ container ownership is
             * transferred.
             */
            ~ReclaimStrings() { H5Dvlen_reclaim(type, space, H5P_DEFAULT, values); }
        } reclaim{type.id, space.id, values.data()};
        require(H5Dread(object, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, values.data()) >= 0,
                "Cannot read mesh string result mapping.");
        std::string result;
        for (const auto* value : values) {
            const std::size_t length = value ? std::strlen(value) : 0;
            const auto prefix = std::to_string(length);
            require(result.size() <= maxBytes && prefix.size() < maxBytes - result.size() &&
                        length < maxBytes - result.size() - prefix.size(),
                    "Oversized mesh string result value.");
            result += prefix;
            result.push_back(':');
            if (length)
                result.append(value, length);
            result.push_back('\0');
        }
        return result;
    }
    require(n >= 0 && width > 0 && std::size_t(n) <= maxBytes / width &&
                (kind == H5T_FLOAT || kind == H5T_INTEGER ||
                 (attribute && kind == H5T_STRING && !H5Tis_variable_str(type.id))),
            "Unsupported or oversized mesh result value.");
    std::string value(std::size_t(n) * width, '\0');
    require((attribute
                 ? H5Aread(object, type.id, value.data())
                 : H5Dread(object, type.id, H5S_ALL, H5S_ALL, H5P_DEFAULT, value.data())) >= 0,
            "Cannot read mesh result value.");
    return value;
}
/**
 * @brief Compare exact attribute names, HDF5 types and bounded value bytes.
 * @param[in] expected Borrowed open reference dataset/group/file.
 * @param[in] actual Borrowed open candidate object, required to have the same attribute count.
 * @throws std::runtime_error On differing names/types/values or inspection failure; no object is
 * changed. Comparison uses raw() representations, not physical tolerance or unit conversion.
 * @note Caller holds HDF5 serialization; temporary names/values may allocate.
 */
void attributes(hid_t expected, hid_t actual) {
    const int count = H5Aget_num_attrs(expected);
    require(count >= 0 && count == H5Aget_num_attrs(actual), "Mesh result attributes differ.");
    for (int k = 0; k < count; ++k) {
        Handle a{
            H5Aopen_by_idx(expected, ".", H5_INDEX_NAME, H5_ITER_INC, k, H5P_DEFAULT, H5P_DEFAULT),
            H5Aclose};
        const auto len = H5Aget_name(a.id, 0, nullptr);
        require(len > 0 && len < 256, "Invalid mesh attribute name.");
        std::string name(len + 1, '\0');
        H5Aget_name(a.id, name.size(), name.data());
        name.resize(len);
        Handle b{H5Aopen(actual, name.c_str(), H5P_DEFAULT), H5Aclose};
        Handle at{H5Aget_type(a.id), H5Tclose}, bt{b.id >= 0 ? H5Aget_type(b.id) : -1, H5Tclose};
        require(b.id >= 0 && H5Tequal(at.id, bt.id) > 0 && raw(a.id, true) == raw(b.id, true),
                "Mesh result attribute identity differs.");
    }
}
/**
 * @brief Compare dataset creation layout, chunks and filter pipelines for restart compatibility.
 * @param[in] a Borrowed expected dataset-creation property list, not a dataset ID.
 * @param[in] b Borrowed actual dataset-creation property list.
 * @return True when layouts match, actual storage is neither virtual nor external, and chunk
 * shapes/filter identifiers/flags/client data match. False on these mismatches or inspected
 * filter errors. Filters allow up to 32 client values; no HDF5 data or property list is
 * modified.
 */
bool storage(hid_t a, hid_t b) {
    const auto layout = H5Pget_layout(a);
    if (layout != H5Pget_layout(b) || layout == H5D_VIRTUAL || H5Pget_external_count(b) != 0)
        return false;
    if (layout == H5D_CHUNKED) {
        hsize_t ac[4]{}, bc[4]{};
        const auto rank = H5Pget_chunk(a, 4, ac);
        if (rank < 1 || rank != H5Pget_chunk(b, 4, bc) || !std::equal(ac, ac + rank, bc))
            return false;
    }
    const auto count = H5Pget_nfilters(a);
    if (count < 0 || count != H5Pget_nfilters(b))
        return false;
    for (int k = 0; k < count; ++k) {
        unsigned af = 0, bf = 0, av[32]{}, bv[32]{};
        size_t an = 32, bn = 32;
        const auto filter = H5Pget_filter2(a, k, &af, &an, av, 0, nullptr, nullptr);
        if (filter < 0 || filter != H5Pget_filter2(b, k, &bf, &bn, bv, 0, nullptr, nullptr) ||
            af != bf || an > 32 || an != bn || !std::equal(av, av + an, bv))
            return false;
    }
    return true;
}

// Rectangular hyperslabs bound series reads even when one row is very wide.
/**
 * @brief Validate and optionally visit numeric dataset values through bounded hyperslab reads.
 * @param[in] dataset Borrowed open numeric HDF5 dataset, rank 0..4, with stable extents/content.
 * @param[in] visit Optional synchronous callback invoked for each validated value in block
 * traversal order; never retained. It may receive an allowed missing NaN, and its exceptions
 * propagate.
 * @param[in] missing Optional flattened non-report-coordinate mask. A nonzero present entry
 * permits NaN for that coordinate; infinity is always invalid. Missing/short mask entries
 * require finiteness.
 * @throws std::runtime_error On shape/read/selection errors or disallowed nonfinite values.
 * @note Reads at most 65536 numbers per rectangular tile and at most 256 rows of an unlimited
 * leading reporting axis. Empty dimensions return without visits. Earlier callback effects
 * survive later failure; dataset contents are unchanged. Caller holds HDF5 serialization.
 */
void visitNumbers(hid_t dataset, const std::function<void(double)>& visit = {},
                  const std::vector<unsigned char>& missing = {}) {
    auto dims = shape(dataset);
    if (std::any_of(dims.begin(), dims.end(), [](auto n) { return n == 0; }))
        return;
    std::vector<hsize_t> at(dims.size(), 0), tile(dims.size(), 1);
    hsize_t capacity = 65536;
    for (int d = int(dims.size()) - 1; d >= 0; --d) {
        tile[d] = std::min(dims[d], capacity);
        capacity = std::max<hsize_t>(1, capacity / tile[d]);
    }
    // HDF5 also allocates workspace per selected chunk. Bound the reporting
    // axis as well as numeric values when histories use one chunk per report.
    const auto maximum = shape(dataset, true);
    const bool series = !maximum.empty() && maximum[0] == H5S_UNLIMITED;
    if (series)
        tile[0] = std::min<hsize_t>(tile[0], 256);
    Handle space{H5Dget_space(dataset), H5Sclose};
    bool done = false;
    while (!done) {
        auto count = tile;
        hsize_t n = 1;
        for (std::size_t d = 0; d < dims.size(); ++d) {
            count[d] = std::min(tile[d], dims[d] - at[d]);
            n *= count[d];
        }
        if (!dims.empty())
            require(H5Sselect_hyperslab(space.id, H5S_SELECT_SET, at.data(), nullptr, count.data(),
                                        nullptr) >= 0,
                    "Cannot select mesh checkpoint block.");
        Handle memory{H5Screate_simple(1, &n, nullptr), H5Sclose};
        std::vector<double> values(n);
        require(H5Dread(dataset, H5T_NATIVE_DOUBLE, memory.id, space.id, H5P_DEFAULT,
                        values.data()) >= 0,
                "Cannot read mesh checkpoint block.");
        for (std::size_t j = 0; j < values.size(); ++j) {
            const double v = values[j];
            std::size_t remainder = j, cell = 0;
            std::vector<hsize_t> coordinate(dims.size());
            for (std::size_t d = dims.size(); d-- > 0;) {
                coordinate[d] = at[d] + remainder % count[d];
                remainder /= count[d];
            }
            const std::size_t begin = series ? 1 : 0;
            for (std::size_t d = begin; d < dims.size(); ++d)
                cell = cell * static_cast<std::size_t>(dims[d]) +
                       static_cast<std::size_t>(coordinate[d]);
            const bool unavailable = cell < missing.size() && missing[cell];
            require(std::isfinite(v) || (unavailable && std::isnan(v)),
                    "Nonfinite mesh checkpoint result.");
            if (visit)
                visit(v);
        }
        if (dims.empty())
            break;
        for (int d = int(dims.size()) - 1; d >= 0; --d) {
            at[d] += tile[d];
            if (at[d] < dims[d])
                break;
            at[d] = 0;
            if (d == 0)
                done = true;
        }
    }
}
/**
 * @brief Copy a borrowed HDF5 object's nonempty path name.
 * @param[in] id Valid named HDF5 object, normally an open dataset.
 * @return Owned path string shorter than 256 bytes; no ID ownership transfer.
 * @throws std::runtime_error When initial name inspection fails or violates the name bound.
 */
std::string datasetName(hid_t id) {
    const auto len = H5Iget_name(id, nullptr, 0);
    require(len > 0 && len < 256, "Invalid mesh dataset handle.");
    std::string name(len + 1, '\0');
    H5Iget_name(id, name.data(), name.size());
    name.resize(len);
    return name;
}
/**
 * @brief Verify an embedded source-history tree uses local supported HDF5 objects.
 * @param[in] group Borrowed group/file ID with local hard links.
 * @param[in] depth Current nesting depth, normally zero; must remain below 16.
 * @throws std::runtime_error On excessive nesting, nonlocal links, unsupported object kinds or
 * datasets with external/virtual storage. Uses links()'s 256-link/name bounds per group.
 * @note No data values/schema are validated here. Hard-link cycles eventually hit the depth
 * bound; no group mutation occurs and callers hold HDF5 serialization.
 */
void localHistory(hid_t group, int depth = 0) {
    require(depth < 16, "Mesh source result group nesting exceeds its limit.");
    for (const auto& name : links(group)) {
        Handle object{H5Oopen(group, name.c_str(), H5P_DEFAULT), H5Oclose};
        require(object.id >= 0, "Cannot inspect mesh source result object.");
        if (H5Iget_type(object.id) == H5I_GROUP)
            localHistory(object.id, depth + 1);
        else {
            require(H5Iget_type(object.id) == H5I_DATASET,
                    "Unsupported mesh source result object.");
            Handle props{H5Dget_create_plist(object.id), H5Pclose};
            require(H5Pget_external_count(props.id) == 0 && H5Pget_layout(props.id) != H5D_VIRTUAL,
                    "Mesh source results must be stored in their checkpoint file.");
        }
    }
}
}

struct Default2DOutputPlugin::PreparedCheckpoint {
    std::filesystem::path staging;
    hid_t file = H5I_INVALID_HID;
    std::vector<std::pair<hid_t*, hid_t>> datasets;
    hsize_t steps = 0, beds = 0, layers = 0;
    hsize_t gw_species = 0, gw_transport_layers = 0, gw_boundaries = 0;
    double next_due = -1;
    bool groundwater = false;
    bool heat = false;
    hsize_t heat_layers = 0, heat_lid_layers = 0;
    std::shared_ptr<const SourceResultHistory> history;
    std::shared_ptr<io::OwnedCheckpointFile> file_copy;
    /**
     * @brief Release unadopted dataset/file handles and owned staging artifacts.
     * @note Acquires the recursive result HDF5 mutex, closes still-owned IDs, and best-effort
     * removes a directly owned staging file. A file_copy object owns its own cleanup instead.
     * Transferred IDs must already have been invalidated by adoptCheckpoint(); close/removal
     * errors are ignored.
     */
    ~PreparedCheckpoint() {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        for (auto& [target, id] : datasets)
            if (id >= 0)
                H5Dclose(id);
        if (file >= 0)
            H5Fclose(file);
        if (!file_copy && !staging.empty()) {
            std::error_code ec;
            std::filesystem::remove(staging, ec);
        }
    }
};

/**
 * @brief Settle and copy the current bounded mesh-result HDF5 file image.
 * @return Owned file bytes, nonempty and at most 64 MiB.
 * @throws std::runtime_error On a previous checkpoint write failure, closed file, flush error or
 * readFile() refusal. Flush is deliberately performed twice to settle HDF5 EOF metadata.
 * @note Caller holds the result HDF5 mutex. May flush file metadata despite being const; it does
 * not advance reporting counters, but requires a stable current output path and no outside
 * writer.
 */
std::string Default2DOutputPlugin::fileBytes() const {
    // HDF5 can reclaim an unused allocation tail after its first flush.
    // Settle that EOF/superblock update before capturing bytes, so another
    // read-only checkpoint does not invalidate the exact fresh-header guard.
    require(!checkpoint_write_failed_ && file_id_ >= 0 &&
                H5Fflush(file_id_, H5F_SCOPE_GLOBAL) >= 0 &&
                H5Fflush(file_id_, H5F_SCOPE_GLOBAL) >= 0,
            "Cannot flush mesh results.");
    return readFile(io::utf8_path(h5_path_), io::checkpointHdf5Descriptor(file_id_));
}

/**
 * @brief Validate a candidate mesh-result prefix against a fresh writer's schema and report
 * grid.
 * @param[in] file Borrowed open candidate HDF5 file, stable throughout validation.
 * @param[in] ctx Prepared simulation context with positive report_step s and calendar serial
 * dates.
 * @param[in] next_report_ms Next hydraulic reporting grid position in elapsed milliseconds.
 * @param[in] snap Stable restored/current simulation projection supplying GW/snow/heat identity,
 * missing-value masks, counts and model geometry.
 * @param[in] steps Expected number of accepted mesh report records.
 * @param[in] next_due Expected mesh cadence cursor in SWMM date-serial days, -1 before first
 * due.
 * @param[in] groundwater Expected GW dataset presence, matching active snapshot/report
 * selection.
 * @throws std::runtime_error On unavailable fresh reference header, surface species/buildup
 * output, schema/type/storage/attributes/static identity/shape/value mismatch or
 * report-count/date/cursor inconsistency. Temporary reference HDF5 creation and allocation may
 * also fail.
 * @note Ignores optional source_history at the root for later separate validation. Numeric data
 * must be finite except supported unavailable diagnostics represented by masked NaN. Checks
 * actual report dates with the engine's one-millisecond convention and rounded mesh reporting
 * stride. Caller holds the HDF5 mutex; candidate file and live writer counters are not changed.
 */
void Default2DOutputPlugin::validateCheckpoint(hid_t file, const SimulationContext& ctx,
                                               double next_report_ms,
                                               const SimulationSnapshot& snap, hsize_t steps,
                                               double next_due, bool groundwater) const {
    require(
        checkpoint_fresh_ && !checkpoint_header_.empty() && n_species_ == 0 && n_buildup_ == 0,
        "Mesh checkpoint requires its prepared writer without surface species or buildup output.");
    Default2DOutputPlugin reference("");
    reference.file_id_ = openImage(checkpoint_header_);
    reference.n_faces_ = n_faces_;
    reference.report_vars_ = report_vars_;
    reference.storage_type_ = storage_type_;
    reference.compression_ = compression_;
    reference.native_species_units_ = native_species_units_;
    require(groundwater == (steps > 0 && want(report2d::GROUNDWATER) && snap.gw2d_active),
            "Mesh checkpoint groundwater presence differs.");
    if (groundwater)
        reference.createGroundwaterDatasets(snap);
    if (steps > 0)
        reference.createSnowDatasets(snap);
    if (steps > 0 && reference.want(report2d::HEAT) && snap.thermal2d_active)
        reference.createThermalDatasets(snap);
    auto expected = links(reference.file_id_), actual = links(file);
    actual.erase("source_history");
    require(actual == expected, "Mesh checkpoint dataset selection differs.");
    attributes(reference.file_id_, file);
    for (const auto& name : expected) {
        Handle a{H5Dopen2(reference.file_id_, name.c_str(), H5P_DEFAULT), H5Dclose};
        Handle b{H5Dopen2(file, name.c_str(), H5P_DEFAULT), H5Dclose};
        require(a.id >= 0 && b.id >= 0, "Mesh checkpoint object is not a dataset.");
        Handle at{H5Dget_type(a.id), H5Tclose}, bt{H5Dget_type(b.id), H5Tclose};
        Handle ap{H5Dget_create_plist(a.id), H5Pclose}, bp{H5Dget_create_plist(b.id), H5Pclose};
        require(H5Tequal(at.id, bt.id) > 0 && storage(ap.id, bp.id),
                ("Mesh checkpoint dataset type or storage differs: " + name).c_str());
        attributes(a.id, b.id);
        auto dims = shape(a.id), maximum = shape(a.id, true);
        const bool series = !maximum.empty() && maximum[0] == H5S_UNLIMITED;
        if (series)
            dims[0] = steps;
        require(dims == shape(b.id) && maximum == shape(b.id, true),
                "Mesh checkpoint dataset shape differs.");
        std::vector<unsigned char> missing;
        for (std::size_t field = 0; field < meshSnowFields.size(); ++field)
            if (name == meshSnowFields[field].name) {
                require(dims.size() == 2 && dims[1] == snap.surface_snow[0].size(),
                        "Mesh snow checkpoint identity differs.");
                missing.resize(dims[1]);
                for (std::size_t cell = 0; cell < missing.size(); ++cell) {
                    const int model = int(snap.surface_snow[0][cell]);
                    missing[cell] =
                        field != 0 && (model < 0 || field == 10 ||
                                       (model == 0 && (field == 7 || field == 8 || field == 9 ||
                                                       field == 11 || field >= 19)));
                }
            }
        const auto markMissing = [&](const std::vector<double>& values) {
            if (values.empty())
                return;
            missing.resize(values.size());
            for (std::size_t i = 0; i < values.size(); ++i)
                missing[i] = std::isnan(values[i]);
        };
        if (name == "Mesh2_face_water_temperature")
            markMissing(snap.thermal2d_water_temperature);
        else if (name == "Mesh2_face_bed_temperature")
            markMissing(snap.thermal2d_bed_temperature);
        else if (name == "Mesh2_face_bed_initial_energy")
            markMissing(snap.thermal2d_bed_initial_energy);
        else if (name == "Mesh2_face_bed_porewater_volume")
            markMissing(snap.thermal2d_bed_pore_volume);
        else if (name == "Mesh2_face_bed_age_generation")
            markMissing(snap.thermal2d_bed_age_generation);
        else if (name == "Mesh2_face_bed_species_conc")
            markMissing(snap.thermal2d_bed_species_concentration);
        else if (name == "Mesh2_face_bed_species_initial_amount")
            markMissing(snap.thermal2d_bed_species_initial_amount);
        else if (name == "Mesh2_face_bed_species_exchange")
            markMissing(snap.thermal2d_bed_species_exchange);
        else if (name == "Mesh2_face_soil_temperature")
            markMissing(snap.thermal2d_ground_temperature);
        else if (name == "Mesh2_face_soil_energy")
            markMissing(snap.thermal2d_ground_energy);
        else if (name == "Mesh2_face_soil_heat_capacity")
            markMissing(snap.thermal2d_ground_capacity);
        else if (name == "Mesh2_face_soil_layer_center_depth")
            markMissing(snap.thermal2d_ground_layer_center_m);
        else if (name == "Mesh2_face_soil_layer_temperature")
            markMissing(snap.thermal2d_ground_layer_temperature);
        else if (name == "Mesh2_face_soil_layer_energy")
            markMissing(snap.thermal2d_ground_layer_energy);
        else if (name == "Mesh2_face_soil_layer_heat_capacity")
            markMissing(snap.thermal2d_ground_layer_capacity);
        else if (name == "ground_heat_budget")
            markMissing(snap.thermal2d_ground_budget);
        else if (name == "Mesh2_lid_layer_temperature")
            markMissing(snap.thermal2d_lid_layer_temperature);
        // These diagnostics are unavailable until their first applicable
        // refresh/firing, and may remain so when infiltration or ET is off.
        // Their historical availability cannot be inferred from the current
        // snapshot. NaN is the missing marker; infinity is still invalid.
        else if (name == "Mesh2_face_gw_infil_capacity" || name == "Mesh2_face_gw_infil_refresh" ||
                 name == "Mesh2_face_et_stress")
            missing.assign(n_faces_, 1);
        if (H5Tget_class(bt.id) == H5T_FLOAT || H5Tget_class(bt.id) == H5T_INTEGER)
            visitNumbers(b.id, {}, missing);
        else
            require(H5Tget_class(bt.id) == H5T_STRING && !series,
                    "Unsupported mesh checkpoint dataset type.");
        if (!series && name.find("Mesh2_face_max_") != 0)
            require(raw(a.id, false) == raw(b.id, false),
                    "Mesh checkpoint static geometry or aquifer identity differs.");
    }
    const double step_ms = 1000 * ctx.options.report_step,
                 grid = std::round(next_report_ms / step_ms);
    require(std::isfinite(grid) && grid >= 1 && grid <= std::numeric_limits<int>::max() &&
                grid * step_ms == next_report_ms,
            "Mesh checkpoint hydraulic reporting clock differs.");
    const int completed = static_cast<int>(grid) - 1;
    const auto date = [&](int k) {
        return datetime::addSeconds(ctx.options.start_date, (k * step_ms + 1) / 1000);
    };
    int first = 1, upper = completed + 1;
    while (first < upper) {
        const int mid = first + (upper - first) / 2;
        if (date(mid) < ctx.options.report_start)
            first = mid + 1;
        else
            upper = mid;
    }
    const double ratio =
        report_2d_step_days_ > 0 ? report_2d_step_days_ * 86400 / ctx.options.report_step : 1;
    require(std::isfinite(ratio) && ratio >= 1 && ratio <= std::numeric_limits<int>::max(),
            "Invalid mesh reporting interval.");
    const int stride = static_cast<int>(std::round(ratio));
    require(steps == (first > completed ? 0 : 1 + (completed - first) / stride),
            "Mesh report count differs.");
    Handle times{H5Dopen2(file, "time", H5P_DEFAULT), H5Dclose};
    double due = -1;
    std::size_t k = 0;
    visitNumbers(times.id, [&](double value) {
        require(value == date(first + static_cast<int>(k) * stride),
                "Mesh checkpoint report date differs.");
        ++k;
        if (report_2d_step_days_ > 0) {
            if (due < 0)
                due = value;
            while (due <= value + 1e-9)
                due += report_2d_step_days_;
        }
    });
    require(std::isfinite(next_due) && next_due == due, "Mesh checkpoint cadence cursor differs.");
}

/**
 * @brief Flush pending source history and encode a validated in-memory mesh-result checkpoint.
 * @param[in] ctx Stable prepared simulation reporting context.
 * @param[in] next_report_ms Next hydraulic report position in elapsed milliseconds.
 * @param[in] snap Stable accepted snapshot used to validate GW/snow/heat identity and report
 * shape.
 * @return Owned binary archive containing version, mesh cadence, record count/cursor, GW
 * presence and complete HDF5 prefix bytes. Not portable across incompatible
 * model/schema/platform state.
 * @throws std::runtime_error On source-history/writer failure, prefix read/size or validation
 * refusal.
 * @note Acquires the recursive result HDF5 mutex. Pending history publication and HDF5 flush may
 * change output state before a later refusal; this is not a read-only transactional file
 * operation. Does not advance hydraulic physics or reporting counters; allocation exceptions
 * propagate.
 */
std::string Default2DOutputPlugin::checkpointBytes(const SimulationContext& ctx,
                                                   double next_report_ms,
                                                   const SimulationSnapshot& snap) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    require(writePendingSourceHistory() == 0 && state_ != PluginState::ERROR,
            "Cannot checkpoint failed mesh source history.");
    auto prefix = fileBytes();
    validateCheckpoint(file_id_, ctx, next_report_ms, snap, n_steps_, next_due_days_, gw_created_);
    detail::CompletedSourceArchive out;
    uint32_t version = 1;
    auto step = report_2d_step_days_;
    out(version, step, n_steps_, next_due_days_, gw_created_, prefix);
    return out.bytes();
}

/**
 * @brief Stage and validate an in-memory mesh-result restore without adopting live writer state.
 * @param[in] ctx Stable prepared simulation reporting context.
 * @param[in] next_report_ms Restored next hydraulic report position in elapsed milliseconds.
 * @param[in] snap Restored accepted snapshot used for schema/model/report validation.
 * @param[in] initial_history Optional required prefix identity for an embedded source_history;
 * when embedded history exists it must be water-only, extend this prefix and not exceed snap
 * time.
 * @param[in] bytes Owned encoded checkpoint; version/cadence/framing and <=64 MiB prefix
 * validated.
 * @return Shared owned plan with staged file/open datasets and optional history. Abandoning the
 * plan closes/removes its resources; this writer must outlive the plan's stored dataset target
 * pointers.
 * @throws std::runtime_error On nonfresh or already source-bound writer, invalid archive/schema/
 * file/history/prefix, or staging/open failure. Up to 32 exclusive sibling-name attempts are
 * made.
 * @note Acquires HDF5 and brief source-state locks. Requires quiescent writer/exclusive restore;
 * fileBytes() can flush the current fresh header, but publication/counter adoption is deferred.
 * Allocation errors propagate with plan-owned resource cleanup.
 */
std::shared_ptr<Default2DOutputPlugin::PreparedCheckpoint> Default2DOutputPlugin::prepareCheckpoint(
    const SimulationContext& ctx, double next_report_ms, const SimulationSnapshot& snap,
    const SourceResultHistory* initial_history, std::string bytes) {
    std::lock_guard<std::recursive_mutex> hdf_lock(io::resultHdf5Mutex());
    {
        std::lock_guard<std::mutex> lock(source_mutex_);
        require(source_pending_.empty() && !source_written_,
                "Mesh restore requires an unbound fresh source writer.");
    }
    require(n_steps_ == 0 && fileBytes() == checkpoint_header_,
            "Mesh restore requires its fresh result header.");
    detail::CompletedSourceArchive in(std::move(bytes));
    uint32_t version = 0;
    double step = 0;
    std::string prefix;
    auto result = std::make_shared<PreparedCheckpoint>();
    in(version, step, result->steps, result->next_due, result->groundwater, prefix);
    in.finish();
    require(version == 1 && step == report_2d_step_days_ && !prefix.empty() &&
                prefix.size() <= maxBytes,
            "Mesh checkpoint format or reporting interval differs.");
    const auto path = io::utf8_path(h5_path_);
    static std::atomic<unsigned long long> serial{0};
    FILE* stream = nullptr;
    for (int k = 0; k < 32 && !stream; ++k) {
        result->staging = path;
        result->staging += ".mesh-part-" + std::to_string(++serial);
        stream = io::fopen_path(result->staging, "wbx");
        if (!stream)
            result->staging.clear();
    }
    require(stream, "Cannot create staged mesh result prefix.");
    const bool wrote = std::fwrite(prefix.data(), 1, prefix.size(), stream) == prefix.size();
    const bool closed = std::fclose(stream) == 0;
    require(wrote && closed, "Cannot stage mesh result prefix.");
    result->file = io::openCheckpointHdf5(io::path_utf8(result->staging).c_str(), H5F_ACC_RDWR);
    require(result->file >= 0, "Cannot reopen staged mesh result prefix.");
    validateCheckpoint(result->file, ctx, next_report_ms, snap, result->steps, result->next_due,
                       result->groundwater);
    resolveCheckpointDatasets(*result, snap);
    if (H5Lexists(result->file, "source_history", H5P_DEFAULT) > 0) {
        Handle group{H5Gopen2(result->file, "source_history", H5P_DEFAULT), H5Gclose};
        require(group.id >= 0, "Invalid mesh source result group.");
        localHistory(group.id);
    }
    SourceResultHistory history;
    bool available = false;
    const auto error = SourceResultHistory::read(result->staging.string(), history, available);
    require(error.empty(), error.c_str());
    if (available) {
        require(initial_history && history.pollutantNames().empty() &&
                    history.tables().at("boundaries").columns[3].back() <= snap.sim_time,
                "Mesh source result ownership or coverage differs.");
        const auto differs = history.extends(*initial_history);
        require(differs.empty(), differs.c_str());
        result->history = std::make_shared<SourceResultHistory>(std::move(history));
    }
    return result;
}

/**
 * @brief Resolve staged dataset IDs and model counts before any live writer adoption.
 * @param[in,out] result Owned prepared plan with open staged file; appends pairs of live-member
 * ID addresses and newly owned staged IDs, and sets GW/thermal dimension metadata.
 * @param[in] snap Stable prepared restored snapshot for optional dataset identity.
 * @throws std::runtime_error On reference-image creation or staged dataset open failure.
 * @note This writer must outlive result and remain quiescent. Live target IDs/counters are not
 * changed; a partial plan owns all previously opened IDs for cleanup. Caller holds the HDF5
 * mutex.
 */
void Default2DOutputPlugin::resolveCheckpointDatasets(PreparedCheckpoint& result,
                                                      const SimulationSnapshot& snap) {
    // Resolve all IDs while failure still leaves the live engine and writer untouched.
    Default2DOutputPlugin reference("");
    reference.file_id_ = openImage(checkpoint_header_);
    reference.n_faces_ = n_faces_;
    reference.report_vars_ = report_vars_;
    reference.storage_type_ = storage_type_;
    reference.compression_ = compression_;
    reference.native_species_units_ = native_species_units_;
    if (result.groundwater)
        reference.createGroundwaterDatasets(snap);
    if (result.steps > 0)
        reference.createSnowDatasets(snap);
    if (result.steps > 0 && reference.want(report2d::HEAT) && snap.thermal2d_active)
        reference.createThermalDatasets(snap);
    result.heat = reference.heat_created_;
    result.heat_layers = reference.n_heat_layers_;
    result.heat_lid_layers = reference.n_heat_lid_layers_;
    const auto targets = checkpointDatasets(), gw = reference.checkpointDatasets();
    result.datasets.reserve(targets.size());
    for (std::size_t k = 0; k < targets.size(); ++k) {
        const hid_t known = *targets[k] >= 0 ? *targets[k] : *gw[k];
        if (known < 0)
            continue;
        const auto name = datasetName(known);
        const auto id = H5Dopen2(result.file, name.c_str(), H5P_DEFAULT);
        require(id >= 0, "Cannot reopen mesh checkpoint dataset.");
        result.datasets.emplace_back(targets[k], id);
    }
    result.beds = reference.n_gw_beds_;
    result.layers = reference.n_gw_layers_;
    result.gw_species = reference.n_gw_species_;
    result.gw_transport_layers = reference.n_gw_transport_layers_;
    result.gw_boundaries = reference.n_gw_boundaries_;
}

/**
 * @brief Copy a validated mesh report file and encode its owned external checkpoint descriptor.
 * @param[in] ctx Stable simulation reporting context.
 * @param[in] next Next hydraulic report position in elapsed milliseconds.
 * @param[in] snap Accepted snapshot for model/schema/report validation.
 * @param[in] base Base path used to create the checkpoint companion artifact.
 * @param[out] artifact Receives shared ownership of the copied companion once descriptor
 * encoding succeeds; retain it while the archive is needed. Prior value is replaced at that
 * publication point.
 * @return Owned archive of version, mesh cadence/count/cursor, GW presence and file descriptor.
 * @throws std::runtime_error On bound/embedded source history, prior write error,
 * file/flush/schema failure or companion-copy verification error. Allocation exceptions
 * propagate.
 * @note Acquires HDF5/source locks. Uses a separate source stream; no embedded history is
 * allowed. Flush can update the live result file; hydraulic state/report counters are not
 * advanced.
 */
std::string Default2DOutputPlugin::captureFileCheckpoint(
    const SimulationContext& ctx, double next, const SimulationSnapshot& snap,
    const std::filesystem::path& base, std::shared_ptr<io::OwnedCheckpointFile>& artifact) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    {
        std::lock_guard<std::mutex> source_lock(source_mutex_);
        require(source_pending_.empty() && !source_written_,
                "File checkpoint uses its separate full source stream.");
    }
    require(!checkpoint_write_failed_ && file_id_ >= 0 &&
                H5Lexists(file_id_, "source_history", H5P_DEFAULT) == 0 &&
                H5Fflush(file_id_, H5F_SCOPE_GLOBAL) >= 0,
            "Cannot settle mesh file checkpoint or embedded source history.");
    validateCheckpoint(file_id_, ctx, next, snap, n_steps_, next_due_days_, gw_created_);
    const auto path = io::utf8_path(h5_path_);
    auto copy = io::copyCheckpointFile(path, io::checkpointFileSize(path), base,
                                       io::checkpointHdf5Descriptor(file_id_));
    detail::CompletedSourceArchive a;
    uint32_t version = 1;
    auto step = report_2d_step_days_;
    auto descriptor = copy->file.encode();
    a(version, step, n_steps_, next_due_days_, gw_created_, descriptor);
    artifact = std::move(copy);
    return a.bytes();
}
/**
 * @brief Verify/copy an external mesh checkpoint companion and stage its open dataset handles.
 * @param[in] ctx Stable reporting context.
 * @param[in] next Restored next hydraulic report position, elapsed milliseconds.
 * @param[in] snap Restored accepted snapshot for schema/identity/report validation.
 * @param[in] bytes Owned file-descriptor checkpoint archive, exact supported version/cadence.
 * @return Shared plan owning a fingerprint-verified companion copy, staged HDF5 handles and
 * target member pointers. Writer must remain alive/quiescent until the plan is adopted or
 * abandoned.
 * @throws std::runtime_error On nonfresh/source-bound writer, invalid archive/fingerprint,
 * changing companion during copy, embedded history or schema/report mismatch. Allocation/I/O
 * errors propagate.
 * @note Acquires HDF5/source locks. No live counter/ID adoption or final-path publication
 * occurs; abandoning the plan releases copied resources. Fresh-header flushing may settle live
 * metadata.
 */
std::shared_ptr<Default2DOutputPlugin::PreparedCheckpoint>
Default2DOutputPlugin::prepareFileCheckpoint(const SimulationContext& ctx, double next,
                                             const SimulationSnapshot& snap, std::string bytes) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    {
        std::lock_guard<std::mutex> source_lock(source_mutex_);
        require(source_pending_.empty() && !source_written_,
                "File restore requires its separate source stream.");
    }
    require(n_steps_ == 0 && fileBytes() == checkpoint_header_,
            "Mesh file restore requires its fresh header.");
    detail::CompletedSourceArchive a(std::move(bytes));
    uint32_t version;
    double step;
    std::string descriptor;
    auto result = std::make_shared<PreparedCheckpoint>();
    a(version, step, result->steps, result->next_due, result->groundwater, descriptor);
    a.finish();
    require(version == 1 && step == report_2d_step_days_,
            "Mesh file checkpoint version or cadence differs.");
    auto saved = io::CheckpointFile::decode(std::move(descriptor));
    saved.verify();
    result->file_copy = io::copyCheckpointFile(saved.path, saved.bytes, io::utf8_path(h5_path_));
    require(result->file_copy->file.fingerprint == saved.fingerprint,
            "Mesh companion changed during copying.");
    result->staging = result->file_copy->file.path;
    result->file = io::openCheckpointHdf5(io::path_utf8(result->staging).c_str(), H5F_ACC_RDWR);
    require(result->file >= 0 && H5Lexists(result->file, "source_history", H5P_DEFAULT) == 0,
            "Mesh file checkpoint contains unsupported embedded history.");
    validateCheckpoint(result->file, ctx, next, snap, result->steps, result->next_due,
                       result->groundwater);
    resolveCheckpointDatasets(*result, snap);
    return result;
}

/**
 * @brief Describe staged-to-live publication guarded by the fresh writer's native descriptor.
 * @param[in] prepared Stable valid plan whose staging file is ready for publication.
 * @return Owned output descriptor containing staging/destination paths, expected fresh-header
 * bytes and the borrowed native file descriptor. The live writer must remain open until
 * publication.
 * @throws std::runtime_error When file-access properties cannot be inspected or the writer has
 * no supported native file descriptor. Caller holds the result HDF5 mutex; no file is published.
 */
io::CheckpointOutputPublication::Output
Default2DOutputPlugin::checkpointPublication(const PreparedCheckpoint& prepared) const {
    const int descriptor = io::checkpointHdf5Descriptor(file_id_);
    require(descriptor >= 0,
            "Mesh checkpoint publication requires the native file descriptor driver.");
    return {prepared.staging, io::utf8_path(h5_path_), checkpoint_header_, descriptor};
}
/**
 * @brief Publish one prepared output file and adopt its staged handles/cursors.
 * @param[in,out] prepared Valid owned plan for this live fresh writer. On successful adoption
 * its file/dataset IDs and history transfer to the writer and staging ownership is disarmed.
 * @throws std::runtime_error On publication/guard failure before adoption; allocation/I/O errors
 * may propagate from publication preparation.
 * @note Acquires the result HDF5 mutex. The higher-level owner must coordinate this with
 * hydraulic state adoption; this single-output method does not itself restore a solver or source
 * driver.
 */
void Default2DOutputPlugin::installCheckpoint(PreparedCheckpoint& prepared) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    io::CheckpointOutputPublication publication({checkpointPublication(prepared)});
    publication.publish();
    adoptCheckpoint(prepared);
}
// Caller holds the result HDF5 mutex before publication and adoption.
/**
 * @brief Transfer already-published staged output handles and report metadata without throwing.
 * @param[in,out] prepared Valid plan for this writer whose staged artifact has successfully been
 * published. Transfers file/dataset IDs, history and GW/thermal counts; invalidates transferred
 * IDs, retains owned companion files, and clears staging cleanup ownership.
 * @note Caller must hold the result HDF5 mutex and exclude other writer/source operations.
 * Closes old IDs with unchecked HDF5 close results. No allocation, schema checking or file
 * publication is performed here; preflight must have resolved all target pointers while this
 * writer remained alive. Writer state becomes UPDATING for nonzero steps, otherwise PREPARED.
 */
void Default2DOutputPlugin::adoptCheckpoint(PreparedCheckpoint& prepared) noexcept {
    // All throwing checks and ID opens precede publication. The solver moves next.
    closeDatasets();
    H5Fclose(file_id_);
    file_id_ = prepared.file;
    prepared.file = H5I_INVALID_HID;
    for (auto& [target, id] : prepared.datasets) {
        *target = id;
        id = H5I_INVALID_HID;
    }
    if (prepared.file_copy)
        prepared.file_copy->retain();
    prepared.staging.clear();
    n_steps_ = prepared.steps;
    next_due_days_ = prepared.next_due;
    gw_created_ = prepared.groundwater;
    n_gw_beds_ = prepared.beds;
    n_gw_layers_ = prepared.layers;
    n_gw_species_ = prepared.gw_species;
    n_gw_transport_layers_ = prepared.gw_transport_layers;
    n_gw_boundaries_ = prepared.gw_boundaries;
    heat_created_ = prepared.heat;
    n_heat_layers_ = prepared.heat_layers;
    n_heat_lid_layers_ = prepared.heat_lid_layers;
    source_written_ = std::move(prepared.history);
    state_ = n_steps_ ? PluginState::UPDATING : PluginState::PREPARED;
}
} // namespace openswmm::twoD
#endif
