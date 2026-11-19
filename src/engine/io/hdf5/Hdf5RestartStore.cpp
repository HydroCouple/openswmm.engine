// SPDX-License-Identifier: Apache-2.0
/**
 * @file      Hdf5RestartStore.cpp
 * @brief     Restart records in an HDF5 file.
 */
#include "Hdf5RestartStore.hpp"

#include "Hdf5Util.hpp"
#include "core/FileIO.hpp"
#include "core/InpWriter.hpp"
#include "core/restart/Sha256.hpp"
#include "io/Hdf5Mutex.hpp"
#include "io/Hdf5ObjectCreation.hpp"

#include <algorithm>
#include <filesystem>
#include <mutex>

namespace openswmm::h5io {
namespace {

constexpr const char* kSchema = "1.0";
constexpr hsize_t kChunkBytes = 1024u * 1024u;

/** Keeps HDF5 from printing its error stack while a failure is reported as text instead. */
class QuietErrors {
public:
    QuietErrors() {
        H5Eget_auto2(H5E_DEFAULT, &func_, &data_);
        H5Eset_auto2(H5E_DEFAULT, nullptr, nullptr);
    }
    ~QuietErrors() { H5Eset_auto2(H5E_DEFAULT, func_, data_); }
    QuietErrors(const QuietErrors&) = delete;
    QuietErrors& operator=(const QuietErrors&) = delete;

private:
    H5E_auto2_t func_ = nullptr;
    void* data_ = nullptr;
};

std::string attrText(hid_t loc, const char* name) {
    std::string v;
    return read_attr(loc, name, v) ? v : std::string{};
}
double attrNumber(hid_t loc, const char* name) {
    double v = 0.0;
    return read_attr(loc, name, v) ? v : 0.0;
}
int attrInt(hid_t loc, const char* name) {
    int v = 0;
    return read_attr(loc, name, v) ? v : 0;
}

/** Names of the links directly in @p group, in name order. */
std::vector<std::string> children(hid_t group) {
    std::vector<std::string> names;
    H5G_info_t info{};
    if (H5Gget_info(group, &info) < 0)
        return names;
    for (hsize_t i = 0; i < info.nlinks; ++i) {
        const ssize_t n =
            H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i, nullptr, 0, H5P_DEFAULT);
        if (n <= 0)
            continue;
        std::string name(static_cast<std::size_t>(n) + 1, '\0');
        H5Lget_name_by_idx(group, ".", H5_INDEX_NAME, H5_ITER_INC, i, name.data(), name.size(),
                           H5P_DEFAULT);
        name.resize(static_cast<std::size_t>(n));
        names.push_back(std::move(name));
    }
    return names;
}

bool writeText(hid_t loc, const std::string& name, const std::string& text) {
    Handle type(H5Tcopy(H5T_C_S1), Handle::Kind::Type);
    if (!type)
        return false;
    H5Tset_size(type.get(), text.empty() ? 1 : text.size());
    H5Tset_cset(type.get(), H5T_CSET_UTF8);
    Handle space(H5Screate(H5S_SCALAR), Handle::Kind::Space);
    Handle ds(io::createHdf5Dataset(loc, name.c_str(), type.get(), space.get(), H5P_DEFAULT,
                                    H5P_DEFAULT, H5P_DEFAULT),
              Handle::Kind::Dataset);
    return ds && H5Dwrite(ds.get(), type.get(), H5S_ALL, H5S_ALL, H5P_DEFAULT,
                          text.empty() ? "" : text.data()) >= 0;
}

class Hdf5Store final : public restart::Store {
public:
    Hdf5Store(Handle file, std::string path) : file_(std::move(file)), path_(std::move(path)) {}

    std::string catalog(restart::Catalog& out) override {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        QuietErrors quiet;
        out = {};
        out.backend = restart::Backend::HDF5;
        if (!has_link(file_.get(), "restart/runs"))
            return {};
        Handle runs = open_group(file_.get(), "restart/runs");
        if (!runs)
            return "cannot open /restart/runs in '" + path_ + "'";
        for (const auto& id : children(runs.get())) {
            Handle g = open_group(runs.get(), id);
            if (!g)
                continue;
            restart::RunInfo run;
            run.run_id = id;
            run.created_at = attrText(g.get(), "created_at");
            run.engine_version = attrText(g.get(), "engine_version");
            run.parent_run_id = attrText(g.get(), "parent_run_id");
            run.parent_record_id = attrText(g.get(), "parent_record_id");
            run.restart_mode = attrText(g.get(), "restart_mode");
            run.restart_source = attrText(g.get(), "restart_source");
            std::vector<restart::RecordInfo> records;
            if (has_link(g.get(), "records")) {
                Handle recs = open_group(g.get(), "records");
                for (const auto& rid : children(recs.get())) {
                    Handle ds(H5Dopen2(recs.get(), rid.c_str(), H5P_DEFAULT),
                              Handle::Kind::Dataset);
                    if (!ds)
                        continue;
                    restart::RecordInfo r;
                    r.record_id = rid;
                    r.ordinal = attrInt(ds.get(), "ordinal");
                    r.created_at = attrText(ds.get(), "created_at");
                    r.model_datetime = attrNumber(ds.get(), "model_datetime");
                    r.elapsed_seconds = attrNumber(ds.get(), "elapsed_seconds");
                    r.requested_datetime = attrNumber(ds.get(), "requested_datetime");
                    r.hsf_version = attrInt(ds.get(), "hsf_version");
                    r.continuation = attrInt(ds.get(), "continuation") != 0;
                    r.binary_output = attrInt(ds.get(), "binary_output") != 0;
                    Handle space(H5Dget_space(ds.get()), Handle::Kind::Space);
                    hsize_t n = 0;
                    if (space && H5Sget_simple_extent_ndims(space.get()) == 1)
                        H5Sget_simple_extent_dims(space.get(), &n, nullptr);
                    r.byte_count = n;
                    r.sha256 = attrText(ds.get(), "sha256");
                    r.platform = attrText(ds.get(), "platform");
                    r.engine_version = attrText(ds.get(), "engine_version");
                    r.status = attrText(ds.get(), "status");
                    if (r.status.empty())
                        r.status = "incomplete";
                    records.push_back(std::move(r));
                }
            }
            std::sort(records.begin(), records.end(),
                      [](const auto& a, const auto& b) { return a.ordinal < b.ordinal; });
            out.runs.push_back({std::move(run), std::move(records)});
        }
        std::stable_sort(out.runs.begin(), out.runs.end(), [](const auto& a, const auto& b) {
            return a.first.created_at < b.first.created_at;
        });
        return {};
    }

    std::string read(const std::string& run_id, const std::string& record_id,
                     std::string& bytes) override {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        QuietErrors quiet;
        bytes.clear();
        const std::string where = "restart/runs/" + run_id + "/records/" + record_id;
        if (!has_link(file_.get(), where))
            return "'" + path_ + "' has no record " + record_id + " in run " + run_id;
        Handle ds(H5Dopen2(file_.get(), where.c_str(), H5P_DEFAULT), Handle::Kind::Dataset);
        if (!ds)
            return "cannot open record " + record_id + " of run " + run_id;
        if (attrText(ds.get(), "status") != "complete")
            return "record " + record_id + " of run " + run_id + " is not complete";
        Handle space(H5Dget_space(ds.get()), Handle::Kind::Space);
        hsize_t n = 0;
        if (!space || H5Sget_simple_extent_ndims(space.get()) != 1 ||
            H5Sget_simple_extent_dims(space.get(), &n, nullptr) < 0)
            return "record " + record_id + " has an unexpected shape";
        bytes.resize(static_cast<std::size_t>(n));
        if (n > 0 &&
            H5Dread(ds.get(), H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()) < 0) {
            bytes.clear();
            return "cannot read record " + record_id + " of run " + run_id;
        }
        if (restart::sha256Hex(bytes) != attrText(ds.get(), "sha256")) {
            bytes.clear();
            return "record " + record_id + " of run " + run_id +
                   " does not match its SHA-256 digest";
        }
        return {};
    }

    std::string beginRun(const SimulationContext& ctx, const restart::RunInfo& run) override {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        QuietErrors quiet;
        Handle root = make_group(file_.get(), "restart");
        if (!root || !write_attr(root.get(), "openswmm_restart_schema", std::string(kSchema)))
            return "cannot create /restart in '" + path_ + "'";
        const std::string where = "restart/runs/" + run.run_id;
        if (has_link(file_.get(), where))
            return "run " + run.run_id + " is already recorded in '" + path_ + "'";
        Handle g = make_group(file_.get(), where);
        if (!g)
            return "cannot create " + where + " in '" + path_ + "'";
        bool ok = write_attr(g.get(), "created_at", run.created_at) &&
                  write_attr(g.get(), "engine_version", run.engine_version) &&
                  write_attr(g.get(), "status", std::string("running"));
        for (const auto& [name, value] :
             {std::pair<const char*, const std::string*>{"parent_run_id", &run.parent_run_id},
              {"parent_record_id", &run.parent_record_id},
              {"restart_mode", &run.restart_mode},
              {"restart_source", &run.restart_source}})
            if (!value->empty())
                ok = ok && write_attr(g.get(), name, *value);
        ok = ok && writeText(g.get(), "model_input", inp_writer::modelText(ctx, path_));
        Handle recs = make_group(g.get(), "records");
        if (!ok || !recs || H5Fflush(file_.get(), H5F_SCOPE_GLOBAL) < 0)
            return "cannot record run " + run.run_id + " in '" + path_ + "'";
        return {};
    }

    std::string append(const std::string& run_id, const restart::RecordInfo& r,
                       const std::string& bytes) override {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        QuietErrors quiet;
        const std::string fail =
            "cannot store restart record " + r.record_id + " in '" + path_ + "'";
        Handle recs = open_group(file_.get(), "restart/runs/" + run_id + "/records");
        if (!recs || bytes.empty())
            return fail;
        const hsize_t n = bytes.size();
        Handle space(H5Screate_simple(1, &n, nullptr), Handle::Kind::Space);
        Handle dcpl(H5Pcreate(H5P_DATASET_CREATE), Handle::Kind::Plist);
        const hsize_t chunk = std::min(n, kChunkBytes);
        if (!space || !dcpl || H5Pset_chunk(dcpl.get(), 1, &chunk) < 0)
            return fail;
        Handle ds(io::createHdf5Dataset(recs.get(), r.record_id.c_str(), H5T_STD_U8LE, space.get(),
                                        H5P_DEFAULT, dcpl.get(), H5P_DEFAULT),
                  Handle::Kind::Dataset);
        if (!ds ||
            H5Dwrite(ds.get(), H5T_NATIVE_UCHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, bytes.data()) < 0)
            return fail;
        const bool ok = write_attr(ds.get(), "ordinal", r.ordinal) &&
                        write_attr(ds.get(), "created_at", r.created_at) &&
                        write_attr(ds.get(), "model_datetime", r.model_datetime) &&
                        write_attr(ds.get(), "elapsed_seconds", r.elapsed_seconds) &&
                        write_attr(ds.get(), "requested_datetime", r.requested_datetime) &&
                        write_attr(ds.get(), "hsf_version", r.hsf_version) &&
                        write_attr(ds.get(), "continuation", r.continuation ? 1 : 0) &&
                        write_attr(ds.get(), "binary_output", r.binary_output ? 1 : 0) &&
                        write_attr(ds.get(), "sha256", restart::sha256Hex(bytes)) &&
                        write_attr(ds.get(), "platform", r.platform) &&
                        write_attr(ds.get(), "engine_version", r.engine_version);
        // Bytes and attributes reach the file before the record says it is complete.
        if (!ok || H5Fflush(file_.get(), H5F_SCOPE_GLOBAL) < 0 ||
            !write_attr(ds.get(), "status", std::string("complete")) ||
            H5Fflush(file_.get(), H5F_SCOPE_GLOBAL) < 0)
            return fail;
        return {};
    }

    std::string finishRun(const std::string& run_id) override {
        std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
        QuietErrors quiet;
        Handle g = open_group(file_.get(), "restart/runs/" + run_id);
        if (!g || !write_attr(g.get(), "status", std::string("completed")) ||
            !write_attr(g.get(), "finished_at", restart::utcNow()) ||
            H5Fflush(file_.get(), H5F_SCOPE_GLOBAL) < 0)
            return "cannot finish run " + run_id + " in '" + path_ + "'";
        return {};
    }

private:
    Handle file_;
    std::string path_;
};

} // namespace

std::unique_ptr<restart::Store> openRestartStore(const std::string& path, bool write,
                                                 std::string& error) {
    std::lock_guard<std::recursive_mutex> lock(io::resultHdf5Mutex());
    QuietErrors quiet;
    const auto kind = restart::probe(path);
    if (!write) {
        Handle file(H5Fopen(path.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT), Handle::Kind::File);
        if (!file) {
            error = "cannot open '" + path + "' as an HDF5 file";
            return nullptr;
        }
        return std::make_unique<Hdf5Store>(std::move(file), path);
    }
    if (kind == restart::Backend::NONE) {
        Handle file(io::createHdf5File(path.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, H5P_DEFAULT),
                    Handle::Kind::File);
        if (!file) {
            error = "cannot create the HDF5 file '" + path + "'";
            return nullptr;
        }
        return std::make_unique<Hdf5Store>(std::move(file), path);
    }
    if (kind != restart::Backend::HDF5) {
        error = "'" + path + "' exists and is not an HDF5 file; it was left unchanged";
        return nullptr;
    }
    Handle file(H5Fopen(path.c_str(), H5F_ACC_RDWR, H5P_DEFAULT), Handle::Kind::File);
    if (!file) {
        error = "cannot open '" + path + "' for writing";
        return nullptr;
    }
    // Only a file that already keeps restart records, or holds nothing, may take more.
    H5G_info_t info{};
    if (!has_link(file.get(), "restart") &&
        (H5Gget_info(file.get(), &info) < 0 || info.nlinks > 0)) {
        error = "'" + path +
                "' holds other HDF5 content and no restart records; it was left unchanged. "
                "Record into a new file or one that already holds restart records.";
        return nullptr;
    }
    return std::make_unique<Hdf5Store>(std::move(file), path);
}

} // namespace openswmm::h5io
