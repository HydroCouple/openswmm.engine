// SPDX-License-Identifier: Apache-2.0

/**
 * @file      SourceResultHdf5.cpp
 * @brief     HDF5 persistence of source-interval diagnostics.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "SourceResultHistory.hpp"
#include "../../io/Hdf5Mutex.hpp"
#include "EngineSourceResultHistory.hpp"
#ifdef OPENSWMM_HAS_HDF5_MODEL
#include "../../io/Hdf5ObjectCreation.hpp"
#include "../../io/hdf5/Hdf5Util.hpp"
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
#include "../../io/CheckpointHdf5.hpp"
#endif
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
using h5io::Handle;
constexpr std::size_t maxValues = 8 * 1024 * 1024, nameBytes = 512, maxNames = 65536;
const std::string artifactKind = "OpenSWMM internal source result history";
const std::string semantics =
    "accepted source intervals; frame 0 initial; receiver and queues at capture; cumulative ledgers; not hydraulic report time";
const std::string outfallSemantics =
    "water-only accepted hydraulic credits; separate completed hydraulic cursor; queued availability may exceed source end; pending snapshots not additive; no per-outfall or adoption events";
const std::map<std::string, std::string> metadata = {
    {"schema_version", "1"},
    {"temporal_meaning", semantics},
    {"sign_convention",
     "nonnegative amounts; signed residual; -1 unavailable identity/adoption/refresh"},
    {"donor_kinds", "0 mesh; 1 non-LID; 2 LID bottom"},
    {"return_kinds", "0 numerical refund; 1 physical Dunne; 2 column excess"},
    {"et_owners", "0 mesh; 1 pervious; 2 impervious; 3 LID"},
    {"refresh_convention", "-1 unpublished; otherwise elapsed seconds at receiver refresh"},
    {"mass_convention",
     "native concentration times m3; per-species mass units; no kg/lb conversion; no age/heat/MSX"},
    {"pool_convention",
     "type -1: IMPERV0/IMPERV1/PERV (fourth unused); otherwise LID surface/pavement/soil/storage; usage lookup maps type/unit"},
    {"footprint_convention",
     "reviewed uniform weather footprints; cell -1 outside mesh; owner follows et_owners"}};
struct QuietErrors {
    H5E_auto2_t function = nullptr;
    void* client = nullptr;
    /**
     * @brief Temporarily suppress the current HDF5 automatic error handler.
     * @note Saves callback/client data for the destructor to restore. HDF5 status returns are
     * ignored; caller holds the result HDF5 mutex and must not race independent error-handler
     * changes.
     */
    QuietErrors() {
        H5Eget_auto2(H5E_DEFAULT, &function, &client);
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    }
    /**
     * @brief Restore the saved HDF5 error handler without reporting restoration errors.
     * @note Caller still holds result HDF5 serialization; saved callback/client lifetime must
     * remain valid through this scope. No ownership of client data is assumed.
     */
    ~QuietErrors() { H5Eset_auto2(H5E_DEFAULT, function, client); }
};
/**
 * @brief Refuse a failed result-file condition by throwing a diagnostic.
 * @param[in] valid True returns normally; false throws.
 * @param[in] error Nonnull error text, copied into the exception.
 * @throws std::runtime_error When valid is false; no cleanup or state rollback is performed
 * here.
 */
void require(bool valid, const char* error) {
    if (!valid)
        throw std::runtime_error(error);
}
struct PublicationFile {
    hid_t id = -1;
    /**
     * @brief Exclusively create an owned source-history publication file.
     * @param[in] path New sibling staging path; existing files are not truncated.
     * @note id is negative on HDF5 creation failure and must be checked by the caller. The
     * destructor closes an owned ID but does not remove the filesystem entry. Caller holds HDF5
     * serialization.
     */
    explicit PublicationFile(const std::string& path)
        : id(io::createHdf5File(path.c_str(), H5F_ACC_EXCL, H5P_DEFAULT, H5P_DEFAULT)) {}
    /**
     * @brief Close a still-owned publication file ID, ignoring close errors.
     * @note Does not remove the staging path; the containing save operation owns filesystem
     * cleanup. Successful explicit close() first marks id invalid to prevent repeated ownership.
     */
    ~PublicationFile() {
        if (id >= 0)
            H5Fclose(id);
    }
    /**
     * @brief Explicitly close the owned publication file and invalidate id after success.
     * @throws std::runtime_error On H5Fclose failure; id remains owned for destructor cleanup.
     * @note Requires a valid open id and caller-held HDF5 serialization. No filesystem rename
     * occurs.
     */
    void close() {
        require(H5Fclose(id) >= 0, "Cannot close source result publication file.");
        id = -1;
    }
};
/**
 * @brief Create the schema's fixed 512-byte UTF-8, NUL-padded HDF5 string type.
 * @return Owned RAII datatype handle; closes when its owner is destroyed.
 * @throws std::runtime_error On HDF5 type creation/configuration failure.
 * @note Caller holds HDF5 serialization. Encoding flags do not validate UTF-8 bytes themselves.
 */
Handle stringType() {
    Handle t(H5Tcopy(H5T_C_S1), Handle::Kind::Type);
    require(t.ok() && H5Tset_size(t.get(), nameBytes) >= 0 &&
                H5Tset_cset(t.get(), H5T_CSET_UTF8) >= 0 &&
                H5Tset_strpad(t.get(), H5T_STR_NULLPAD) >= 0,
            "Cannot create source result string type.");
    return t;
}
/**
 * @brief Create and write one fixed-width schema string attribute.
 * @param[in] loc Borrowed open HDF5 object to modify; attribute must not already exist.
 * @param[in] name Nonnull attribute name.
 * @param[in] value Bytes shorter than 512, copied into a zero-filled fixed string; embedded NUL
 * and UTF-8 validity are not checked here.
 * @throws std::runtime_error On excessive length or HDF5 create/write error. A failure can leave
 * a created attribute on the staging object; caller owns transaction cleanup and HDF5
 * serialization.
 */
void attribute(hid_t loc, const char* name, const std::string& value) {
    auto type = stringType();
    Handle space(H5Screate(H5S_SCALAR), Handle::Kind::Space);
    Handle a(H5Acreate2(loc, name, type.get(), space.get(), H5P_DEFAULT, H5P_DEFAULT),
             Handle::Kind::Attr);
    char bytes[nameBytes] = {};
    require(value.size() < nameBytes, "Source result attribute too long.");
    std::memcpy(bytes, value.data(), value.size());
    require(a.ok() && H5Awrite(a.get(), type.get(), bytes) >= 0,
            "Cannot write source result attribute.");
}
/**
 * @brief Read and validate a fixed-width scalar schema string attribute.
 * @param[in] loc Borrowed open HDF5 object.
 * @param[in] name Nonnull existing attribute name.
 * @return Owned bytes before the first NUL, from a scalar fixed UTF-8 string type of width 512.
 * @throws std::runtime_error On missing attribute, wrong shape/type/width/encoding, missing
 * termination or read failure. No object mutation; caller holds HDF5 serialization.
 */
std::string attribute(hid_t loc, const char* name) {
    Handle a(H5Aopen(loc, name, H5P_DEFAULT), Handle::Kind::Attr);
    require(a.ok(), "Missing source result attribute.");
    Handle type(H5Aget_type(a.get()), Handle::Kind::Type);
    Handle space(H5Aget_space(a.get()), Handle::Kind::Space);
    require(type.ok() && space.ok() && H5Sget_simple_extent_type(space.get()) == H5S_SCALAR &&
                H5Tget_class(type.get()) == H5T_STRING && H5Tis_variable_str(type.get()) == 0 &&
                H5Tget_size(type.get()) == nameBytes && H5Tget_cset(type.get()) == H5T_CSET_UTF8,
            "Invalid source result attribute shape or type.");
    char bytes[nameBytes] = {};
    require(H5Aread(a.get(), type.get(), bytes) >= 0 && std::memchr(bytes, 0, nameBytes),
            "Cannot read source result attribute.");
    return std::string(bytes);
}
/**
 * @brief Read a one-dimensional result column length with the numeric value-count bound.
 * @param[in] dataset Borrowed open HDF5 dataset.
 * @return Number of entries, at most 8*1024*1024; zero is valid.
 * @throws std::runtime_error On invalid dataspace/rank/extent inspection or excessive length. No
 * data is read or modified; dataset type is validated by the caller.
 */
std::size_t length(hid_t dataset) {
    Handle space(H5Dget_space(dataset), Handle::Kind::Space);
    hsize_t dims[1] = {};
    require(space.ok() && H5Sget_simple_extent_ndims(space.get()) == 1 &&
                H5Sget_simple_extent_dims(space.get(), dims, nullptr) >= 0 && dims[0] <= maxValues,
            "Invalid source result column rank or size.");
    return std::size_t(dims[0]);
}
/**
 * @brief Create a fixed-width UTF-8 name-vector dataset on a staging group.
 * @param[in] group Borrowed open group; destination dataset must be absent.
 * @param[in] name Nonnull dataset name.
 * @param[in] values At most 65536 names, each shorter than 512 bytes. Empty vector is allowed;
 * NUL/uniqueness/semantic validation belongs to history.validate() before this helper.
 * @throws std::runtime_error On size or HDF5 creation/write failure. Allocation errors
 * propagate.
 * @note Copies 512-byte NUL-padded entries. Failure may leave a partially created staging
 * dataset; caller owns publication rollback and HDF5 serialization.
 */
void names(hid_t group, const char* name, const std::vector<std::string>& values) {
    require(values.size() <= maxNames, "Too many source result names.");
    auto type = stringType();
    hsize_t dim = values.size();
    Handle space(H5Screate_simple(1, &dim, nullptr), Handle::Kind::Space);
    Handle d(io::createHdf5Dataset(group, name, type.get(), space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT),
             Handle::Kind::Dataset);
    std::vector<char> bytes(values.size() * nameBytes, 0);
    for (std::size_t i = 0; i < values.size(); ++i) {
        require(values[i].size() < nameBytes, "Source result name too long.");
        std::memcpy(bytes.data() + i * nameBytes, values[i].data(), values[i].size());
    }
    require(d.ok() && (values.empty() || H5Dwrite(d.get(), type.get(), H5S_ALL, H5S_ALL,
                                                  H5P_DEFAULT, bytes.data()) >= 0),
            "Cannot write source result names.");
}
/**
 * @brief Read a bounded fixed-width name-vector dataset.
 * @param[in] group Borrowed open parent group.
 * @param[in] name Nonnull existing dataset name.
 * @return Owned vector of at most 65536 NUL-terminated strings from rank-one fixed 512-byte
 * UTF-8 HDF5 entries; returned strings stop at their first NUL. Empty vector is valid.
 * @throws std::runtime_error On missing/wrong-type/oversized/unterminated data or read failure.
 * No group mutation; name identity/uniqueness is checked later by history.validate().
 */
std::vector<std::string> names(hid_t group, const char* name) {
    Handle d(H5Dopen2(group, name, H5P_DEFAULT), Handle::Kind::Dataset);
    require(d.ok(), "Missing source result names.");
    const auto count = length(d.get());
    Handle type(H5Dget_type(d.get()), Handle::Kind::Type);
    require(count <= maxNames && type.ok() && H5Tget_class(type.get()) == H5T_STRING &&
                H5Tis_variable_str(type.get()) == 0 && H5Tget_size(type.get()) == nameBytes &&
                H5Tget_cset(type.get()) == H5T_CSET_UTF8,
            "Invalid source result name type or count.");
    std::vector<char> bytes(count * nameBytes, 0);
    require(count == 0 ||
                H5Dread(d.get(), type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()) >= 0,
            "Cannot read source result names.");
    std::vector<std::string> out;
    out.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const auto* p = bytes.data() + i * nameBytes;
        require(std::memchr(p, 0, nameBytes) != nullptr, "Unterminated source result name.");
        out.emplace_back(p);
    }
    return out;
}
/**
 * @brief Create a columnar diagnostic table with exact field names and unit attributes.
 * @param[in] root Borrowed open staging group; table name must not exist.
 * @param[in] name Child table-group name.
 * @param[in] t Validated table with matching fields/units/columns extents. Each column is stored
 * as a rank-one IEEE little-endian float64 dataset, preserving native diagnostic units.
 * @throws std::runtime_error On HDF5 group/dataset/value/attribute failure. Allocation may
 * throw.
 * @note No input mutation. Failures may leave partial staging groups; caller owns transaction
 * cleanup. No per-value or equal-row validation is repeated here.
 */
void writeTable(hid_t root, const std::string& name, const SourceResultTable& t) {
    auto group = h5io::make_group(root, name);
    require(group.ok(), "Cannot create source result table.");
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        hsize_t dim = t.columns[c].size();
        Handle space(H5Screate_simple(1, &dim, nullptr), Handle::Kind::Space);
        Handle d(io::createHdf5Dataset(group.get(), t.fields[c].c_str(), H5T_IEEE_F64LE,
                                       space.get(), H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                 Handle::Kind::Dataset);
        require(d.ok() && (dim == 0 || H5Dwrite(d.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                                                H5P_DEFAULT, t.columns[c].data()) >= 0),
                "Cannot write source result column.");
        attribute(d.get(), "units", t.units[c]);
    }
}
/**
 * @brief Fill an expected table schema from bounded, typed HDF5 columns.
 * @param[in] root Borrowed open parent group.
 * @param[in] name Existing table-group name.
 * @param[in,out] t Prepared fields/units/column schema; columns resize and decode progressively.
 * @param[in,out] total Aggregate numeric values already read. Increments before each column
 * read; all decoded tables together must fit 8*1024*1024 values.
 * @throws std::runtime_error On missing columns, rank/length/type/unit mismatch, aggregate
 * overflow or read error. Allocation errors propagate; t/total may be partially changed on
 * failure.
 * @note Caller uses a private candidate. Numeric finiteness/semantic validation occurs
 * afterward.
 */
void readTable(hid_t root, const std::string& name, SourceResultTable& t, std::size_t& total) {
    auto group = h5io::open_group(root, name);
    require(group.ok(), "Missing source result table.");
    std::size_t rows = 0;
    for (std::size_t c = 0; c < t.columns.size(); ++c) {
        Handle d(H5Dopen2(group.get(), t.fields[c].c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
        require(d.ok(), "Missing source result column.");
        const auto count = length(d.get());
        if (c == 0)
            rows = count;
        Handle type(H5Dget_type(d.get()), Handle::Kind::Type);
        require(count == rows && count <= maxValues - total && type.ok() &&
                    H5Tequal(type.get(), H5T_IEEE_F64LE) > 0,
                "Source result column shape, type or size differs.");
        total += count;
        require(attribute(d.get(), "units") == t.units[c], "Source result column units differ.");
        t.columns[c].resize(count);
        require(count == 0 || H5Dread(d.get(), H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                                      t.columns[c].data()) >= 0,
                "Cannot read source result column.");
    }
}
/**
 * @brief Create the checkpoint-match binary byte dataset for exact continuation binding.
 * @param[in] group Borrowed staging group with no checkpoint_match dataset yet.
 * @param[in] bytes Owned elsewhere, borrowed binary checkpoint payload; copied as rank-one
 * uint8.
 * @throws std::runtime_error On HDF5 dataset creation/write failure. Empty/size bounds are not
 * validated here; reader requires a nonempty payload of at most 64 MiB. Caller owns staging
 * cleanup.
 */
void boundary(hid_t group, const std::string& bytes) {
    hsize_t dim = bytes.size();
    Handle space(H5Screate_simple(1, &dim, nullptr), Handle::Kind::Space);
    Handle d(io::createHdf5Dataset(group, "checkpoint_match", H5T_STD_U8LE, space.get(),
                                   H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
             Handle::Kind::Dataset);
    require(d.ok() && H5Dwrite(d.get(), H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT,
                               bytes.data()) >= 0,
            "Cannot write source checkpoint match.");
}
/**
 * @brief Read exact component/engine checkpoint-match bytes from a history group.
 * @param[in] group Borrowed open group with checkpoint_match dataset.
 * @return Owned nonempty binary string, at most 64 MiB, from a rank-one little-endian uint8
 * dataset.
 * @throws std::runtime_error On missing/mistyped/invalid-size data or read failure. No input
 * mutation; allocation errors propagate and caller provides HDF5 serialization.
 */
std::string boundary(hid_t group) {
    Handle d(H5Dopen2(group, "checkpoint_match", H5P_DEFAULT), Handle::Kind::Dataset);
    require(d.ok(), "Missing source checkpoint match.");
    Handle type(H5Dget_type(d.get()), Handle::Kind::Type);
    Handle space(H5Dget_space(d.get()), Handle::Kind::Space);
    hsize_t dim = 0;
    require(type.ok() && H5Tequal(type.get(), H5T_STD_U8LE) > 0 && space.ok() &&
                H5Sget_simple_extent_ndims(space.get()) == 1 &&
                H5Sget_simple_extent_dims(space.get(), &dim, nullptr) >= 0 && dim > 0 &&
                dim <= 64 * 1024 * 1024,
            "Invalid source checkpoint match shape/type/size.");
    std::string bytes(std::size_t(dim), '\0');
    require(H5Dread(d.get(), H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()) >= 0,
            "Cannot read source checkpoint match.");
    return bytes;
}

}
struct SourceResultFile {
    /**
     * @brief Write validated source-history schema, names, tables and optional checkpoint
     * anchor.
     * @param[in] group Borrowed empty staging group to populate.
     * @param[in] history Stable validated history. Schema version is 1 without anchor, 2 with
     * anchor, or 3 for water-only outfall-return capability; exact semantic metadata accompanies
     * it.
     * @throws std::runtime_error On HDF5 or bounded-string write failure; allocation errors
     * propagate.
     * @note Does not validate the history or publish links. Partial staged objects may survive
     * failure; caller owns rollback and holds HDF5 serialization.
     */
    static void writeGroup(hid_t group, const SourceResultHistory& history) {
        for (const auto& [key, value] : metadata)
            attribute(group, key.c_str(),
                      key == "schema_version" ? (history.outfall_returns_     ? "3"
                                                 : !history.boundary_.empty() ? "2"
                                                                              : "1")
                                              : value);
        if (history.outfall_returns_)
            attribute(group, "outfall_return_convention", outfallSemantics);
        names(group, "source_names", history.sources_);
        names(group, "pollutant_names", history.pollutants_);
        names(group, "pollutant_mass_units", history.mass_units_);
        for (const auto& [name, t] : history.tables_)
            writeTable(group, name, t);
        if (!history.boundary_.empty())
            boundary(group, history.boundary_);
    }
    // Publish the optional group inside the engine-owned open HDF5 file.
    // Other root datasets/time axes are untouched. This is process-failure
    // rollback, not crash/power-loss atomicity or the production streaming gate.
    /**
     * @brief Stage and publish a validated source_history group inside an existing engine result
     * file.
     * @param[in] history Stable source history to validate/write.
     * @param[in] file Borrowed valid open HDF5 file from this engine's linked HDF5 instance.
     * @return Empty on flushed publication; diagnostic on validation, unfinished staging,
     * invalid file or caught standard exception. No other root datasets/time axes are
     * intentionally edited.
     * @note Preserves previous history through link moves and attempts rollback on failure.
     * Rollback return codes are unchecked; this is best-effort process-error recovery, not
     * crash/power-loss atomicity. The file remains open and may retain incomplete links after an
     * HDF5 rollback failure. Caller holds the result HDF5 mutex; error printing is suppressed
     * only for this scope.
     */
    static std::string embed(const SourceResultHistory& history, hid_t file) {
        const char *staged = "__source_history_next", *old = "__source_history_previous";
        bool made = false, moved = false, published = false;
        QuietErrors quiet;
        try {
            const auto invalid = history.validate();
            if (!invalid.empty())
                return invalid;
            require(file >= 0 && H5Iis_valid(file) > 0, "Source output file is not open.");
            require(!h5io::has_link(file, staged) && !h5io::has_link(file, old),
                    "Source output has an unfinished publication.");
            auto group = h5io::make_group(file, staged);
            require(group.ok(), "Cannot stage source result group.");
            made = true;
            writeGroup(group.get(), history);
            group.close();
            require(H5Fflush(file, H5F_SCOPE_GLOBAL) >= 0, "Cannot flush staged source results.");
            if (h5io::has_link(file, "source_history")) {
                require(H5Lmove(file, "source_history", file, old, H5P_DEFAULT, H5P_DEFAULT) >= 0,
                        "Cannot retain previous source results.");
                moved = true;
            }
            require(H5Lmove(file, staged, file, "source_history", H5P_DEFAULT, H5P_DEFAULT) >= 0,
                    "Cannot publish source result group.");
            published = true;
            require(H5Fflush(file, H5F_SCOPE_GLOBAL) >= 0,
                    "Cannot flush published source results.");
            if (moved)
                require(H5Ldelete(file, old, H5P_DEFAULT) >= 0,
                        "Cannot release previous source result group.");
            return {};
        } catch (const std::exception& e) {
            if (published)
                H5Ldelete(file, "source_history", H5P_DEFAULT);
            if (moved)
                H5Lmove(file, old, file, "source_history", H5P_DEFAULT, H5P_DEFAULT);
            if (made && !published)
                H5Ldelete(file, staged, H5P_DEFAULT);
            H5Fflush(file, H5F_SCOPE_GLOBAL);
            return e.what();
        }
    }

    /**
     * @brief Validate and publish a standalone SourceResultHistory HDF5 artifact.
     * @param[in] history Stable recorder; full schema/receipt validation must pass before file
     * creation.
     * @param[in] path Destination whose parent exists. If a file exists, its artifact_kind must
     * match this standalone artifact; hydraulic/model files are refused.
     * @return Empty after sibling exclusive creation, complete write, HDF5 flush/close and
     * rename; otherwise diagnostic on validation/I/O or a caught standard exception. Same-path
     * writers require external coordination. Temporary names use source-results-part and
     * process-local atomic numbering.
     * @note A failed exclusive create never deletes another writer's file. Caught later failures
     * remove this call's temporary file best-effort. Filesystem rename is the publication point
     * and provides no power-loss guarantee. Caller holds HDF5 serialization; physics/history
     * inputs remain unchanged.
     */
    static std::string save(const SourceResultHistory& history, const std::string& path) {
        std::string part;
        try {
            const auto invalid = history.validate();
            if (!invalid.empty())
                return invalid;
            QuietErrors quiet;
            if (std::filesystem::exists(path)) {
                auto existing = h5io::open_file_readonly(path);
                require(existing.ok() && attribute(existing.get(), "artifact_kind") == artifactKind,
                        "Destination is not a standalone source result artifact.");
            }
            static std::atomic<unsigned long> serial{0};
            part = path + ".source-results-part-" + std::to_string(++serial);
            PublicationFile file(part);
            // Never delete someone else's publication file after an exclusive-create failure.
            if (file.id < 0) {
                part.clear();
                return "Cannot create source result publication file.";
            }
            attribute(file.id, "artifact_kind", artifactKind);
            auto group = h5io::make_group(file.id, "source_history");
            require(group.ok(), "Cannot create source result history group.");
            writeGroup(group.get(), history);
            require(H5Fflush(file.id, H5F_SCOPE_GLOBAL) >= 0, "Cannot flush source result file.");
            group.close();
            file.close();
            std::error_code error;
            std::filesystem::rename(part, path, error);
            require(!error, "Cannot publish source result file.");
            part.clear();
            return {};
        } catch (const std::exception& e) {
            if (!part.empty()) {
                std::error_code ignored;
                std::filesystem::remove(part, ignored);
            }
            return e.what();
        }
    }
    /**
     * @brief Decode and validate a source-history group, including supported semantic metadata.
     * @param[in] group Borrowed source_history or paired source_intervals HDF5 group.
     * @param[in,out] total Aggregate numeric-value budget shared with any containing history;
     * increases as columns are decoded and can remain increased on failure.
     * @return Owned validated saved recorder for schema version 1, 2 or 3. No live stage is
     * bound; version 1 lacks a continuation checkpoint anchor.
     * @throws std::runtime_error On unsupported version/metadata/capability or
     * name/table/anchor/receipt failure. Allocation errors propagate. Group is unchanged; caller
     * holds HDF5 serialization.
     */
    static SourceResultHistory readGroup(hid_t group, std::size_t& total) {
        const auto version = attribute(group, "schema_version");
        require(version == "1" || version == "2" || version == "3",
                "Unsupported source result schema.");
        for (const auto& [key, value] : metadata)
            if (key != "schema_version")
                require(attribute(group, key.c_str()) == value,
                        "Unsupported source result metadata or schema.");
        SourceResultHistory trial;
        trial.outfall_returns_ = version == "3";
        if (trial.outfall_returns_)
            require(attribute(group, "outfall_return_convention") == outfallSemantics,
                    "Unsupported outfall source result metadata.");
        trial.tables_ = SourceResultHistory::schema(trial.outfall_returns_);
        if (version != "1")
            trial.boundary_ = boundary(group);
        trial.sources_ = names(group, "source_names");
        trial.pollutants_ = names(group, "pollutant_names");
        trial.mass_units_ = names(group, "pollutant_mass_units");
        for (auto& [name, t] : trial.tables_)
            readTable(group, name, t, total);
        const auto invalid = trial.validate();
        require(invalid.empty(), invalid.c_str());
        return trial;
    }
    /**
     * @brief Read and validate a bounded optional source_history from an HDF5 file.
     * @param[in] path Existing nonempty file of at most 256 MiB, opened read-only.
     * @param[out] out Replaced only with a fully validated private SourceResultHistory
     * candidate; remains unchanged on errors or absent capability. A saved recorder is not
     * live-bound for continuation.
     * @param[out] available Set false immediately, true only after out is replaced.
     * @return Empty on valid read or absent optional group; diagnostic for
     * I/O/schema/metadata/identity/ receipt failure or a caught standard exception. Unsupported
     * files do not imply zero-valued data.
     * @note Caller holds HDF5 serialization. All numeric tables share an 8*1024*1024 value
     * budget and fixed name/anchor limits. Exact checkpoint resume/pairing is a separate
     * operation.
     */
    static std::string read(const std::string& path, SourceResultHistory& out, bool& available) {
        available = false;
        try {
            QuietErrors quiet;
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            require(!error && size > 0 && size <= 256 * 1024 * 1024,
                    "Source result file missing or exceeds size limit.");
#ifdef OPENSWMM_HAS_2D_HDF5_OUTPUT
            // Mesh validation also inspects a live checkpoint writer. Reuse
            // its VFD so HDF5 can share the existing locked file instance.
            Handle file(io::openCheckpointHdf5(path.c_str(), H5F_ACC_RDONLY), Handle::Kind::File);
#else
            auto file = h5io::open_file_readonly(path);
#endif
            require(file.ok(), "Cannot open source result HDF5 file.");
            require(!h5io::has_link(file.get(), "__source_history_next") &&
                        !h5io::has_link(file.get(), "__source_history_previous"),
                    "Source output contains an unfinished publication.");
            if (!h5io::has_link(file.get(), "source_history"))
                return {};
            auto group = h5io::open_group(file.get(), "source_history");
            require(group.ok(), "Invalid source result group.");
            std::size_t total = 0;
            auto trial = readGroup(group.get(), total);
            out = std::move(trial);
            available = true;
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    }
};
std::string SourceResultHistory::writeIntoOpenResult(std::int64_t file) const {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    return SourceResultFile::embed(*this, file);
}
std::string SourceResultHistory::save(const std::string& path) const {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    return SourceResultFile::save(*this, path);
}
std::string SourceResultHistory::read(const std::string& path, SourceResultHistory& out,
                                      bool& available) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    return SourceResultFile::read(path, out, available);
}

struct EngineSourceResultFile {
    static constexpr const char* kind = "OpenSWMM internal accepted engine source result history";
    static constexpr const char* semantics =
        "accepted engine steps; frame 0 initial; source clocks may repeat; cumulative water; queues are snapshots not additive events; whole-engine checkpoint match; no hydraulic output append";
    static constexpr const char* pairedSemantics =
        "immutable source intervals on their own axis; earlier source capture checkpoint inspected privately; continuation requires containing whole-engine checkpoint";
    /**
     * @brief Validate and publish a standalone EngineSourceResultHistory HDF5 artifact.
     * @param[in] history Stable recorder; full schema/receipt validation must pass before file
     * creation.
     * @param[in] path Destination whose parent exists. If a file exists, its artifact_kind must
     * match this standalone artifact; hydraulic/model files are refused.
     * @return Empty after sibling exclusive creation, complete write, HDF5 flush/close and
     * rename; otherwise diagnostic on validation/I/O or a caught standard exception. Same-path
     * writers require external coordination. Temporary names use engine-source-results-part and
     * process-local atomic numbering.
     * @note A failed exclusive create never deletes another writer's file. Caught later failures
     * remove this call's temporary file best-effort. Filesystem rename is the publication point
     * and provides no power-loss guarantee. Caller holds HDF5 serialization; physics/history
     * inputs remain unchanged.
     */
    static std::string save(const EngineSourceResultHistory& history, const std::string& path) {
        std::string part;
        try {
            const auto invalid = history.validate();
            if (!invalid.empty())
                return invalid;
            QuietErrors quiet;
            if (std::filesystem::exists(path)) {
                auto existing = h5io::open_file_readonly(path);
                require(existing.ok() && attribute(existing.get(), "artifact_kind") == kind,
                        "Destination is not a standalone engine source result artifact.");
            }
            static std::atomic<unsigned long> serial{0};
            part = path + ".engine-source-results-part-" + std::to_string(++serial);
            PublicationFile file(part);
            if (file.id < 0) {
                part.clear();
                return "Cannot create engine source result publication file.";
            }
            attribute(file.id, "artifact_kind", kind);
            auto group = h5io::make_group(file.id, "engine_source_history");
            require(group.ok(), "Cannot create engine source history group.");
            attribute(group.get(), "schema_version",
                      history.lid_beds_ ? (history.detailed_ ? "4" : "3")
                                        : (history.detailed_ ? "2" : "1"));
            attribute(group.get(), "temporal_meaning", semantics);
            attribute(group.get(), "outfall_returns", history.outfall_returns_ ? "1" : "0");
            names(group.get(), "source_names", history.sources_);
            names(group.get(), "node_names", history.nodes_);
            for (const auto& [name, t] : history.tables_)
                writeTable(group.get(), name, t);
            boundary(group.get(), history.boundary_);
            if (history.detailed_) {
                attribute(group.get(), "source_interval_convention", pairedSemantics);
                auto source = h5io::make_group(group.get(), "source_intervals");
                require(source.ok(), "Cannot create paired source interval group.");
                SourceResultFile::writeGroup(source.get(), history.source_intervals_);
            }
            require(H5Fflush(file.id, H5F_SCOPE_GLOBAL) >= 0,
                    "Cannot flush engine source result file.");
            group.close();
            file.close();
            std::error_code error;
            std::filesystem::rename(part, path, error);
            require(!error, "Cannot publish engine source result file.");
            part.clear();
            return {};
        } catch (const std::exception& e) {
            if (!part.empty()) {
                std::error_code ignored;
                std::filesystem::remove(part, ignored);
            }
            return e.what();
        }
    }
    /**
     * @brief Read and validate a bounded optional engine_source_history from an HDF5 file.
     * @param[in] path Existing nonempty file of at most 256 MiB, opened read-only.
     * @param[out] out Replaced only with a fully validated private EngineSourceResultHistory
     * candidate; remains unchanged on errors or absent capability. A saved recorder is not
     * live-bound for continuation.
     * @param[out] available Set false immediately, true only after out is replaced.
     * @return Empty on valid read or absent optional group; diagnostic for
     * I/O/schema/metadata/identity/ receipt failure or a caught standard exception. Unsupported
     * files do not imply zero-valued data.
     * @note Caller holds HDF5 serialization. All numeric tables share an 8*1024*1024 value
     * budget and fixed name/anchor limits. Exact checkpoint resume/pairing is a separate
     * operation.
     */
    static std::string read(const std::string& path, EngineSourceResultHistory& out,
                            bool& available) {
        available = false;
        try {
            QuietErrors quiet;
            std::error_code error;
            const auto size = std::filesystem::file_size(path, error);
            require(!error && size > 0 && size <= 256 * 1024 * 1024,
                    "Engine source result file missing or exceeds size limit.");
            auto file = h5io::open_file_readonly(path);
            require(file.ok(), "Cannot open engine source result HDF5 file.");
            if (!h5io::has_link(file.get(), "engine_source_history"))
                return {};
            auto group = h5io::open_group(file.get(), "engine_source_history");
            require(group.ok(), "Invalid engine source history group.");
            const auto version = attribute(group.get(), "schema_version");
            require((version == "1" || version == "2" || version == "3" || version == "4") &&
                        attribute(group.get(), "temporal_meaning") == semantics,
                    "Unsupported engine source result schema or metadata.");
            require((version == "2" || version == "4") ==
                        h5io::has_link(group.get(), "source_intervals"),
                    "Engine source interval capability differs from schema.");
            const auto returns = attribute(group.get(), "outfall_returns");
            require(returns == "0" || returns == "1", "Invalid engine source return capability.");
            EngineSourceResultHistory trial;
            trial.detailed_ = version == "2" || version == "4";
            trial.lid_beds_ = version == "3" || version == "4";
            trial.outfall_returns_ = returns == "1";
            trial.tables_ =
                EngineSourceResultHistory::schema(trial.outfall_returns_, trial.lid_beds_);
            trial.sources_ = names(group.get(), "source_names");
            trial.nodes_ = names(group.get(), "node_names");
            trial.boundary_ = boundary(group.get());
            std::size_t total = 0;
            for (auto& [name, t] : trial.tables_)
                readTable(group.get(), name, t, total);
            if (trial.detailed_) {
                require(attribute(group.get(), "source_interval_convention") == pairedSemantics,
                        "Unsupported paired source interval convention.");
                auto source = h5io::open_group(group.get(), "source_intervals");
                require(source.ok(), "Missing paired source interval group.");
                trial.source_intervals_ = SourceResultFile::readGroup(source.get(), total);
            }
            const auto& dates = trial.tables_.at("boundaries").columns[6];
            require(!dates.empty(), "Missing engine source start date.");
            trial.start_date_ = dates.front();
            const auto invalid = trial.validate();
            if (!invalid.empty())
                return invalid;
            out = std::move(trial);
            available = true;
            return {};
        } catch (const std::exception& e) {
            return e.what();
        }
    }
};
std::string EngineSourceResultHistory::save(const std::string& path) const {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    return EngineSourceResultFile::save(*this, path);
}
std::string EngineSourceResultHistory::read(const std::string& path, EngineSourceResultHistory& out,
                                            bool& available) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    return EngineSourceResultFile::read(path, out, available);
}
}
#else
namespace openswmm::twoD {
std::string SourceResultHistory::writeIntoOpenResult(std::int64_t) const {
    return "Source result HDF5 support unavailable in this build.";
}
std::string SourceResultHistory::save(const std::string&) const {
    return "Source result HDF5 support unavailable in this build.";
}
std::string SourceResultHistory::read(const std::string&, SourceResultHistory&, bool& available) {
    available = false;
    return "Source result HDF5 support unavailable in this build.";
}
std::string EngineSourceResultHistory::save(const std::string&) const {
    return "Engine source result HDF5 support unavailable in this build.";
}
std::string EngineSourceResultHistory::read(const std::string&, EngineSourceResultHistory&,
                                            bool& available) {
    available = false;
    return "Engine source result HDF5 support unavailable in this build.";
}
}
#endif
