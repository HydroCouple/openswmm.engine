// SPDX-License-Identifier: Apache-2.0

/**
 * @file      TraceSidecar.cpp
 * @brief     Persistence and discovery of flow trace companion data.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "FlowTracer.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <mutex>
#if defined(OPENSWMM_HAS_HDF5_MODEL)
#include <hdf5.h>
#endif
#if defined(_WIN32)
#define NOMINMAX
#include <windows.h>
#endif

namespace openswmm::trace {
#if defined(OPENSWMM_HAS_HDF5_MODEL)
namespace {
std::mutex cacheMutex;
struct H5Handle {
    hid_t id = -1;
    herr_t (*close)(hid_t) = nullptr;
    /** @brief Pair an HDF5 handle with the matching release function.
     * @param v Owned handle, or negative failed-open sentinel.
     * @param c Required close function when v>=0. Treat this helper as unique ownership
     * and do not copy it; its implicit copy operations are not disabled in code. */
    H5Handle(hid_t v, herr_t (*c)(hid_t)) : id(v), close(c) {}
    /** @brief Close a nonnegative owned handle, ignoring the close return status. */
    ~H5Handle() {
        if (id >= 0)
            close(id);
    }
    /** @brief Borrow the raw HDF5 identifier without transferring ownership.
     * @return Stored id, possibly negative after an HDF5 failure. */
    operator hid_t() const { return id; }
};
/** @brief Create and write one rank-one IEEE little-endian double dataset.
 * @param f Borrowed writable HDF5 file handle.
 * @param name Nonnull dataset name, required not to collide with an existing dataset.
 * @param a Borrowed doubles, converted from native representation by HDF5.
 * @return True after dataset creation and complete write, including an empty array;
 * false for dataset/write failure. Does not flush or remove a partially created dataset. */
bool writeArray(hid_t f, const char* name, const std::vector<double>& a) {
    hsize_t count = a.size();
    H5Handle space(H5Screate_simple(1, &count, nullptr), H5Sclose);
    H5Handle ds(H5Dcreate2(f, name, H5T_IEEE_F64LE, space, H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
                H5Dclose);
    return ds.id >= 0 && (a.empty() || H5Dwrite(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL,
                                                H5P_DEFAULT, a.data()) >= 0);
}
/** @brief Read a rank-one HDF5 dataset with an exact expected element count.
 * @param f Borrowed readable file handle.
 * @param name Nonnull dataset name.
 * @param[out] a Resized only after rank/dimension validation; can be partially
 * overwritten on HDF5 read failure. Allocation exceptions propagate.
 * @param expected Required element count.
 * @return True for a complete converted-native-double read (or zero elements), false
 * for missing dataset/rank/count/read failure. Content finiteness is checked elsewhere. */
bool readArray(hid_t f, const char* name, std::vector<double>& a, size_t expected) {
    H5Handle ds(H5Dopen2(f, name, H5P_DEFAULT), H5Dclose);
    if (ds.id < 0)
        return false;
    H5Handle space(H5Dget_space(ds), H5Sclose);
    hsize_t size = 0;
    if (H5Sget_simple_extent_ndims(space) != 1 ||
        H5Sget_simple_extent_dims(space, &size, nullptr) < 0 || size != expected)
        return false;
    a.resize(expected);
    return a.empty() ||
           H5Dread(ds, H5T_NATIVE_DOUBLE, H5S_ALL, H5S_ALL, H5P_DEFAULT, a.data()) >= 0;
}
} // namespace
#endif
bool FlowTracer::readCache(const char* path, const std::string& key) {
#if defined(OPENSWMM_HAS_HDF5_MODEL)
    std::lock_guard<std::mutex> guard(cacheMutex);
    bool ok = false;
    H5E_BEGIN_TRY {
        H5Handle file(H5Fopen(path, H5F_ACC_RDONLY, H5P_DEFAULT), H5Fclose);
        if (file.id >= 0) {
            H5Handle ds(H5Dopen2(file, "cache_key", H5P_DEFAULT), H5Dclose);
            if (ds.id >= 0) {
                H5Handle space(H5Dget_space(ds), H5Sclose);
                hsize_t size = 0;
                if (H5Sget_simple_extent_ndims(space) == 1 &&
                    H5Sget_simple_extent_dims(space, &size, nullptr) >= 0 && size == key.size()) {
                    std::string got(size, '\0');
                    if (H5Dread(ds, H5T_NATIVE_CHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, got.data()) >=
                            0 &&
                        got == key) {
                        std::vector<double> meta, a, b;
                        if (readArray(file, "report_metadata", meta, 6) &&
                            readArray(file, "node_averages", a, nodes.size() * 7) &&
                            readArray(file, "link_averages", b, links.size() * 7)) {
                            info = {2,
                                    2,
                                    int(nodes.size()),
                                    int(links.size()),
                                    int(meta[0]),
                                    int(meta[1]),
                                    1,
                                    meta[2],
                                    meta[3],
                                    meta[4]};
                            na.assign(nodes.size(), {});
                            la.assign(links.size(), {});
                            for (size_t i = 0; i < nodes.size(); ++i) {
                                auto& v = na[i];
                                size_t j = i * 7;
                                v.volume_m3 = a[j];
                                v.inflow_m3s = a[j + 1];
                                v.lateral_in_m3s = a[j + 2];
                                v.withdrawal_m3s = a[j + 3];
                                v.overflow_m3s = a[j + 4];
                                v.first_volume_m3 = a[j + 5];
                                v.last_volume_m3 = a[j + 6];
                            }
                            for (size_t i = 0; i < links.size(); ++i) {
                                auto& v = la[i];
                                size_t j = i * 7;
                                v.net_flow_m3s = b[j];
                                v.absolute_flow_m3s = b[j + 1];
                                v.absolute_velocity_mps = b[j + 2];
                                v.forward_volume_m3 = b[j + 3];
                                v.reverse_volume_m3 = b[j + 4];
                                v.first_volume_m3 = b[j + 5];
                                v.last_volume_m3 = b[j + 6];
                            }
                            ok = info.periods > 0 && info.source_flow_units >= 0 &&
                                 info.source_flow_units <= 5 && info.duration_s >= 0 &&
                                 derive() == 0;
                        }
                    }
                }
            }
        }
    }
    H5E_END_TRY;
    if (!ok) {
        prepared = false;
        error.clear();
    }
    return ok;
#else
    (void)path;
    (void)key;
    return false;
#endif
}
bool FlowTracer::writeCache(const char* path, const std::string& key) {
#if defined(OPENSWMM_HAS_HDF5_MODEL)
    std::lock_guard<std::mutex> guard(cacheMutex);
    static std::atomic<unsigned> serial{0};
    const std::string staging =
        std::string(path) + ".part." +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "." +
        std::to_string(serial++);
    bool ok = false;
    {
        H5Handle file(H5Fcreate(staging.c_str(), H5F_ACC_EXCL, H5P_DEFAULT, H5P_DEFAULT), H5Fclose);
        if (file.id < 0)
            return false;
        hsize_t size = key.size();
        H5Handle space(H5Screate_simple(1, &size, nullptr), H5Sclose);
        H5Handle ds(H5Dcreate2(file, "cache_key", H5T_STD_I8LE, space, H5P_DEFAULT, H5P_DEFAULT,
                               H5P_DEFAULT),
                    H5Dclose);
        std::vector<double> a, b;
        for (const auto& v : na)
            a.insert(a.end(), {v.volume_m3, v.inflow_m3s, v.lateral_in_m3s, v.withdrawal_m3s,
                               v.overflow_m3s, v.first_volume_m3, v.last_volume_m3});
        for (const auto& v : la)
            b.insert(b.end(), {v.net_flow_m3s, v.absolute_flow_m3s, v.absolute_velocity_mps,
                               v.forward_volume_m3, v.reverse_volume_m3, v.first_volume_m3,
                               v.last_volume_m3});
        ok = ds.id >= 0 &&
             H5Dwrite(ds, H5T_NATIVE_CHAR, H5S_ALL, H5S_ALL, H5P_DEFAULT, key.data()) >= 0 &&
             writeArray(file, "report_metadata",
                        {double(info.periods), double(info.source_flow_units),
                         info.first_report_date, info.last_report_date, info.duration_s, 2}) &&
             writeArray(file, "node_averages", a) && writeArray(file, "link_averages", b) &&
             H5Fflush(file, H5F_SCOPE_GLOBAL) >= 0;
    }
    if (ok) {
#if defined(_WIN32)
        ok =
            MoveFileExW(std::filesystem::path(staging).c_str(), std::filesystem::path(path).c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        ok = std::rename(staging.c_str(), path) == 0;
#endif
    }
    if (!ok) {
        std::error_code ec;
        std::filesystem::remove(staging, ec);
    }
    return ok;
#else
    (void)path;
    (void)key;
    return false;
#endif
}
} // namespace openswmm::trace
