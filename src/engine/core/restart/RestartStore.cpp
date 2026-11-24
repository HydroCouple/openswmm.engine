// SPDX-License-Identifier: Apache-2.0
/**
 * @file      RestartStore.cpp
 * @brief     Backend probe, store factory and catalog JSON for restart records.
 */
#include "RestartStore.hpp"

#include "../FileIO.hpp"

#if defined(OPENSWMM_HAS_GEOPACKAGE)
#include "../../input/geopackage/RestartGpkgStore.hpp"
#endif
#if defined(OPENSWMM_HAS_HDF5_MODEL)
#include "../../io/hdf5/Hdf5RestartStore.hpp"
#endif

#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace openswmm::restart {

Backend probe(const std::string& path) noexcept {
    try {
        std::error_code ec;
        const auto p = io::utf8_path(path);
        if (!std::filesystem::exists(p, ec))
            return Backend::NONE;
        if (!std::filesystem::is_regular_file(p, ec))
            return Backend::OTHER;
        std::ifstream file(p, std::ios::binary);
        char head[1024] = {};
        file.read(head, sizeof head);
        const auto n = static_cast<std::size_t>(file.gcount());
        if (n == 0)
            return Backend::NONE;
        if (n >= 14 && (std::memcmp(head, "OPENSWMM_HS_V1", 14) == 0 ||
                        std::memcmp(head, "SWMM5-HOTSTART", 14) == 0))
            return Backend::HOTSTART;
        if (n >= 16 && std::memcmp(head, "SQLite format 3\0", 16) == 0)
            return Backend::GEOPACKAGE;
        // The HDF5 signature sits at offset 0 or, after a user block, at 512 or 1024.
        static constexpr char kHdf5[8] = {'\x89', 'H', 'D', 'F', '\r', '\n', '\x1a', '\n'};
        if (n >= 8 && std::memcmp(head, kHdf5, 8) == 0)
            return Backend::HDF5;
        if (n >= 520 && std::memcmp(head + 512, kHdf5, 8) == 0)
            return Backend::HDF5;
        file.clear();
        file.seekg(1024);
        char later[8] = {};
        file.read(later, 8);
        if (file.gcount() == 8 && std::memcmp(later, kHdf5, 8) == 0)
            return Backend::HDF5;
        return Backend::OTHER;
    } catch (...) {
        return Backend::OTHER;
    }
}

const char* backendName(Backend backend) noexcept {
    switch (backend) {
    case Backend::NONE:
        return "NONE";
    case Backend::HOTSTART:
        return "HOTSTART";
    case Backend::GEOPACKAGE:
        return "GEOPACKAGE";
    case Backend::HDF5:
        return "HDF5";
    case Backend::OTHER:
        break;
    }
    return "OTHER";
}

std::unique_ptr<Store> openStore(const std::string& path, Backend backend, bool write,
                                 std::string& error) {
    error.clear();
    if (backend == Backend::GEOPACKAGE) {
#if defined(OPENSWMM_HAS_GEOPACKAGE)
        return gpkg::openRestartStore(path, write, error);
#else
        error = "this engine was built without GeoPackage support";
        return nullptr;
#endif
    }
    if (backend == Backend::HDF5) {
#if defined(OPENSWMM_HAS_HDF5_MODEL)
        return h5io::openRestartStore(path, write, error);
#else
        error = "this engine was built without HDF5 support";
        return nullptr;
#endif
    }
    error = "'" + path + "' is not a GeoPackage or HDF5 file";
    return nullptr;
}

namespace {

std::string quoted(const std::string& s) {
    std::string out = "\"";
    for (const unsigned char c : s) {
        switch (c) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out + "\"";
}

std::string number(double v) {
    char buf[40];
    std::snprintf(buf, sizeof buf, "%.17g", v);
    return buf;
}

} // namespace

std::string catalogJson(const std::string& path, const Catalog& catalog) {
    std::ostringstream o;
    o << "{\"path\":" << quoted(path) << ",\"backend\":" << quoted(backendName(catalog.backend))
      << ",\"runs\":[";
    for (std::size_t i = 0; i < catalog.runs.size(); ++i) {
        const auto& [run, records] = catalog.runs[i];
        if (i)
            o << ',';
        o << "{\"run_id\":" << quoted(run.run_id) << ",\"created_at\":" << quoted(run.created_at)
          << ",\"engine_version\":" << quoted(run.engine_version)
          << ",\"parent_run_id\":" << quoted(run.parent_run_id)
          << ",\"parent_record_id\":" << quoted(run.parent_record_id)
          << ",\"restart_mode\":" << quoted(run.restart_mode)
          << ",\"restart_source\":" << quoted(run.restart_source) << ",\"records\":[";
        for (std::size_t k = 0; k < records.size(); ++k) {
            const auto& r = records[k];
            if (k)
                o << ',';
            o << "{\"record_id\":" << quoted(r.record_id) << ",\"ordinal\":" << r.ordinal
              << ",\"created_at\":" << quoted(r.created_at)
              << ",\"model_datetime\":" << number(r.model_datetime)
              << ",\"elapsed_seconds\":" << number(r.elapsed_seconds)
              << ",\"requested_datetime\":" << number(r.requested_datetime)
              << ",\"hsf_version\":" << r.hsf_version
              << ",\"continuation\":" << (r.continuation ? "true" : "false")
              << ",\"binary_output\":" << (r.binary_output ? "true" : "false")
              << ",\"byte_count\":" << r.byte_count << ",\"sha256\":" << quoted(r.sha256)
              << ",\"platform\":" << quoted(r.platform)
              << ",\"engine_version\":" << quoted(r.engine_version)
              << ",\"status\":" << quoted(r.status) << '}';
        }
        o << "]}";
    }
    o << "]}";
    return o.str();
}

std::string platformTag() {
#if defined(_WIN32)
    const char* os = "windows";
#elif defined(__APPLE__)
    const char* os = "darwin";
#elif defined(__EMSCRIPTEN__)
    const char* os = "wasm";
#elif defined(__linux__)
    const char* os = "linux";
#else
    const char* os = "unknown";
#endif
#if defined(__aarch64__) || defined(_M_ARM64)
    const char* cpu = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    const char* cpu = "x86_64";
#elif defined(__wasm32__)
    const char* cpu = "wasm32";
#else
    const char* cpu = "other";
#endif
    const std::uint16_t one = 1;
    unsigned char first = 0;
    std::memcpy(&first, &one, 1);
    return std::string(os) + "-" + cpu + (first ? "-le" : "-be");
}

std::string utcNow() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm utc{};
#ifdef _WIN32
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char text[32];
    std::strftime(text, sizeof text, "%Y-%m-%dT%H:%M:%SZ", &utc);
    return text;
}

} // namespace openswmm::restart
