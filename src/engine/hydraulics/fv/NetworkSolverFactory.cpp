/**
 * @file      NetworkSolverFactory.cpp
 * @brief     Implementation of runtime FV backend selection.
 *
 * @details Deliberately structured to read alongside
 *          2d/solver/SurfaceSolverFactory.cpp — same search path rules, same
 *          handle cache, same ABI check, same gate semantics — so the two
 *          plugin loaders can be reviewed as one design.
 *
 * @see NetworkSolverFactory.hpp
 * @ingroup engine_fv
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "NetworkSolverFactory.hpp"

#include "ExplicitFvSolver.hpp"
#include "FvDeviceMarcher.hpp"
#include "FvOptions.hpp"
#include "INetworkSolver.hpp"
#include "../../2d/solver/GpuPluginAbi.h"
#include "../../core/ThreadInfo.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace openswmm::fv {
namespace {

/**
 * @brief Load a native GPU module without unloading an existing module.
 *
 * POSIX resolves symbols immediately and locally (RTLD_NOW|RTLD_LOCAL). Successful handles must
 * remain mapped while any plugin solver exists; this loader never unloads them.
 *
 * @param[in] path Native narrow-character module filename; passed to LoadLibraryA or dlopen as
 *     supplied.
 * @return Native module handle, or nullptr if loading fails.
 */
void* platform_load(const std::string& path) {
#if defined(_WIN32)
    return static_cast<void*>(::LoadLibraryA(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

/**
 * @brief Resolve an exported function in a loaded module.
 *
 * @param[in] handle Native module handle, or nullptr.
 * @param[in] sym Non-null NUL-terminated exported symbol name.
 * @return Untyped symbol address, or nullptr for a null handle or lookup failure. No ownership is
 *     transferred.
 */
void* platform_sym(void* handle, const char* sym) {
    if (!handle)
        return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), sym));
#else
    return ::dlsym(handle, sym);
#endif
}

/**
 * @brief Find the directory containing the engine module that implements this loader.
 *
 * Windows uses narrow-character APIs and a fixed MAX_PATH buffer; this helper does not provide
 * long-path/UTF-8 adaptation or diagnose truncation. Filesystem/string allocation exceptions can
 * propagate.
 * @return Native parent directory string, or empty if platform address/module lookup fails.
 */
std::string this_library_dir() {
#if defined(_WIN32)
    HMODULE hm = nullptr;
    if (::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCSTR>(&platform_load), &hm)) {
        char buf[MAX_PATH] = {};
        if (::GetModuleFileNameA(hm, buf, MAX_PATH) > 0)
            return fs::path(buf).parent_path().string();
    }
    return {};
#else
    Dl_info info;
    if (::dladdr(reinterpret_cast<void*>(&platform_load), &info) && info.dli_fname)
        return fs::path(info.dli_fname).parent_path().string();
    return {};
#endif
}

/**
 * @brief Lowercase a copied backend name using the current C locale.
 *
 * @param[in] s Backend text copied into the function and modified locally.
 * @return Lowercased owned string; unsigned-character conversion avoids negative-char tolower
 *     input.
 */
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/**
 * @brief Copy the current value of one environment variable.
 *
 * @param[in] name Non-null NUL-terminated environment-variable name.
 * @return Owned current value, or an empty string if unset; no environment pointer is retained.
 */
std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

/**
 * @brief List platform filename candidates for one GPU backend.
 *
 * @param[in] backend Backend suffix such as cuda, hip, sycl or omp; inserted verbatim without
 *     sanitization.
 * @return Owned candidate names: openswmm_gpu_*.dll on Windows, lib*.dylib then lib*.so on macOS,
 *     lib*.so elsewhere.
 */
std::vector<std::string> plugin_filenames(const std::string& backend) {
    const std::string base = "openswmm_gpu_" + backend;
#if defined(_WIN32)
    return {base + ".dll"};
#elif defined(__APPLE__)
    return {"lib" + base + ".dylib", "lib" + base + ".so"};
#else
    return {"lib" + base + ".so"};
#endif
}

/**
 * @brief Build the ordered native GPU module search directories.
 *
 * Splits the environment value on semicolon for Windows or colon elsewhere, discards empty
 * components, and preserves duplicates/order. No current-directory entry, normalization, existence
 * check or recursive traversal is added. Allocation/filesystem conversion exceptions propagate.
 * @return Owned directories from OPENSWMM_GPU_PLUGIN_PATH, then the engine module directory and its
 *     gpu child.
 */
std::vector<std::string> search_dirs() {
    std::vector<std::string> dirs;
#if defined(_WIN32)
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::string envpath = env("OPENSWMM_GPU_PLUGIN_PATH");
    std::size_t start = 0;
    while (start <= envpath.size()) {
        std::size_t pos = envpath.find(sep, start);
        std::string tok = envpath.substr(start, pos - start);
        if (!tok.empty())
            dirs.push_back(tok);
        if (pos == std::string::npos)
            break;
        start = pos + 1;
    }
    const std::string libdir = this_library_dir();
    if (!libdir.empty()) {
        dirs.push_back(libdir);
        dirs.push_back((fs::path(libdir) / "gpu").string());
    }
    return dirs;
}

/**
 * @brief Load a module once per exact path string and keep it mapped for process lifetime.
 *
 * A process-static mutex serializes cache lookup and loading. Failed loads are not cached;
 * successful modules are never unloaded, including rejected ABI/device plugins. Native load
 * diagnostics are not surfaced; map/string/mutex exceptions may propagate.
 *
 * @param[in] path Native module path used verbatim as a case-sensitive cache key.
 * @return Borrowed cached/new native handle, or nullptr on native loading failure.
 */
void* load_cached(const std::string& path) {
    static std::mutex mtx;
    static std::map<std::string, void*> cache;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = cache.find(path);
    if (it != cache.end())
        return it->second;
    void* h = platform_load(path);
    if (h)
        cache.emplace(path, h);
    return h;
}

using ProbeFn = int (*)(OpenSwmmGpuProbe*);
using MakeFn = void* (*)(const OpenSwmmGpuProbe*);
using DestroyFn = void (*)(void*);
using ContractFn = const char* (*)();

/// One usable plugin module for a backend, or why there is none.
struct Located {
    void* handle = nullptr;
    OpenSwmmGpuProbe info{};
    std::string reason; ///< set when handle is null
};

/**
 * @brief Find the first plugin module that can serve a backend, without creating a solver.
 *
 * Applies, in order: the module exists and loads; it was built from this engine's headers and
 * toolchain (the build contract); it exports the probe and the factory symbols the solver needs;
 * its ABI version and vendor match; its probe reports a usable device. Module handles stay cached
 * for the process lifetime. The reason of the candidate that got furthest is kept, so a plugin
 * that is present but unusable is not reported as missing.
 *
 * @param[in] backend "omp", "cuda", "hip" or "sycl".
 * @param[in] surface True to require the 2D surface solver factory, false for the 1D network
 *     marcher factory.
 * @return The module handle and its probe record, or a null handle and the reason.
 */
Located locate_plugin(const std::string& backend, bool surface) {
    Located out;
    out.reason = "the " + backend + " plugin is not installed";
    const int expected_vendor = backend == "omp"    ? OPENSWMM_GPU_VENDOR_OPENMP
                                : backend == "cuda" ? OPENSWMM_GPU_VENDOR_CUDA
                                : backend == "hip"  ? OPENSWMM_GPU_VENDOR_HIP
                                                    : OPENSWMM_GPU_VENDOR_SYCL;
    for (const auto& dir : search_dirs()) {
        for (const auto& fname : plugin_filenames(backend)) {
            const fs::path path = fs::path(dir) / fname;
            std::error_code ec;
            if (!fs::exists(path, ec))
                continue;

            void* h = load_cached(path.string());
            if (!h) {
                out.reason = "the " + backend + " plugin could not be loaded (" + path.string() +
                             "); a library it depends on may be missing";
                continue;
            }
            bool same_build = false;
#ifdef OPENSWMM_GPU_BUILD_CONTRACT
            if (auto contract =
                    reinterpret_cast<ContractFn>(platform_sym(h, "openswmm_gpu_build_contract"))) {
                const char* id = contract();
                same_build = id && std::strncmp(id, OPENSWMM_GPU_BUILD_CONTRACT,
                                                sizeof(OPENSWMM_GPU_BUILD_CONTRACT)) == 0;
            }
#endif
            if (!same_build) {
                out.reason = "the " + backend + " plugin was built for a different engine (" +
                             path.string() + ")";
                continue;
            }
            auto probe = reinterpret_cast<ProbeFn>(platform_sym(h, "openswmm_gpu_probe"));
            const bool factories = surface
                                       ? (platform_sym(h, "openswmm_make_gpu_explicit_solver") &&
                                          platform_sym(h, "openswmm_destroy_gpu_explicit_solver"))
                                       : (platform_sym(h, "openswmm_make_gpu_network_marcher") &&
                                          platform_sym(h, "openswmm_destroy_gpu_network_marcher"));
            if (!probe || !factories) {
                out.reason = "the " + backend + " plugin does not provide this solver";
                continue;
            }
            OpenSwmmGpuProbe info{};
            const int rc = probe(&info);
            if (info.abi_version != OPENSWMM_GPU_ABI_VERSION ||
                !std::memchr(info.device_name, '\0', sizeof(info.device_name)) ||
                info.vendor != expected_vendor) {
                out.reason = "the " + backend + " plugin was built for a different engine (" +
                             path.string() + ")";
                continue;
            }
            if (rc != 0 || info.device_count <= 0) {
                out.reason = info.device_name[0]
                                 ? "no usable device: " + std::string(info.device_name)
                                 : "no usable device";
                continue;
            }
            out.handle = h;
            out.info = info;
            out.reason.clear();
            return out;
        }
    }
    return out;
}

/**
 * @brief Create the network substep marcher of the first usable plugin for a backend.
 *
 * A host (OpenMP) plugin is told the thread count to initialize with; an automatic request on a
 * machine with performance cores uses those, as the 2D factory does. Module handles remain cached
 * for the process lifetime, so the returned deleter stays valid.
 *
 * @param[in] backend Recognized backend suffix used for filenames and the selection label.
 * @param[in] requested_threads Raw [OPTIONS] THREADS (0 = automatic).
 * @param[out] label Optional; receives "<backend> (<device name>)" on success.
 * @param[out] reason Optional; receives why no marcher was created.
 * @return Shared marcher released through its plugin, or null.
 */
std::shared_ptr<IFvDeviceMarcher> try_plugin(const std::string& backend, int requested_threads,
                                             std::string* label, std::string* reason) {
    Located found = locate_plugin(backend == "metal" ? "omp" : backend, false);
    if (!found.handle) {
        if (reason)
            *reason = found.reason;
        return nullptr;
    }
    auto make = reinterpret_cast<MakeFn>(
        platform_sym(found.handle, backend == "metal" ? "openswmm_make_metal_network_marcher"
                                                      : "openswmm_make_gpu_network_marcher"));
    auto destroy = reinterpret_cast<DestroyFn>(
        platform_sym(found.handle, "openswmm_destroy_gpu_network_marcher"));
    if (found.info.vendor == OPENSWMM_GPU_VENDOR_OPENMP)
        found.info.requested_threads =
            requested_threads > 0 ? requested_threads : threadinfo::perfCores();
    if (!make) {
        if (reason)
            *reason = "installed plugin lacks the Metal network worker";
        return nullptr;
    }
    void* raw = make(&found.info);
    if (!raw) {
        if (reason) {
            using Error = const char* (*)();
            auto error =
                reinterpret_cast<Error>(platform_sym(found.handle, "openswmm_gpu_last_error"));
            const char* diagnostic = error ? error() : nullptr;
            *reason = diagnostic && *diagnostic
                          ? diagnostic
                          : "the " + backend + " plugin could not create the solver";
        }
        return nullptr;
    }
    if (label)
        *label = backend + " (" + found.info.device_name + ")";
    return std::shared_ptr<IFvDeviceMarcher>(
        static_cast<IFvDeviceMarcher*>(raw),
        [destroy](IFvDeviceMarcher* m) { destroy(static_cast<void*>(m)); });
}

/**
 * @brief Map an options backend enum to its lowercase factory request.
 *
 * @param[in] b Backend enum; unrecognized values use AUTO behavior.
 * @return Borrowed static literal cpu/omp/cuda/hip/sycl/auto; never null.
 */
const char* backend_name(Backend b) {
    switch (b) {
    case Backend::CPU:
        return "cpu";
    case Backend::OMP:
        return "omp";
    case Backend::CUDA:
        return "cuda";
    case Backend::HIP:
        return "hip";
    case Backend::SYCL:
        return "sycl";
    case Backend::METAL:
        return "metal";
    case Backend::AUTO:
    default:
        return "auto";
    }
}

} // namespace

BackendProbe probeBackend(const std::string& backend_in, bool surface) {
    const std::string backend = lower(backend_in);
    BackendProbe out;
    if (backend == "cpu" || backend == "auto") {
        out.usable = true;
        out.device_name = "CPU";
        return out;
    }
    if (backend == "metal") {
        auto found = locate_plugin("omp", surface);
        if (!found.handle) {
            out.reason = found.reason;
            return out;
        }
        using ProbeMetal = int (*)(char*, size_t);
        auto probe =
            reinterpret_cast<ProbeMetal>(platform_sym(found.handle, "openswmm_probe_metal_faces"));
        char name[256]{};
        if (!probe)
            out.reason = "engine plugin was built without Metal";
        else if (probe(name, sizeof(name)) != 0)
            out.reason = name;
        else {
            out.usable = true;
            out.device_name = name;
        }
        return out;
    }
    if (backend != "omp" && backend != "cuda" && backend != "hip" && backend != "sycl") {
        out.reason = "'" + backend_in + "' is not a backend name";
        return out;
    }
    const Located found = locate_plugin(backend, surface);
    out.usable = found.handle != nullptr;
    out.reason = found.reason;
    if (out.usable) {
        out.device_name = found.info.device_name;
        out.device_mem_bytes = found.info.device_mem_bytes;
    }
    return out;
}

std::unique_ptr<INetworkSolver> makeNetworkSolver(const FvOptions& opts, std::string* chosen,
                                                  int n_cells, int requested_threads) {
    (void)n_cells;
    auto solver = std::make_unique<ExplicitFvSolver>();
    if (chosen)
        *chosen = "cpu (explicit finite volume)";

    std::string mode = lower(env("OPENSWMM_FV_BACKEND"));
    if (mode.empty())
        mode = backend_name(opts.backend);

    // A device backend replaces the substep loop of the same solver object
    // (FvDeviceMarcher.hpp), and only on an explicit request. AUTO stays on
    // the CPU: the marcher steps globally and holds structure forcing over the
    // routing step, so switching to it changes results against the default
    // local-time-stepping path, and its crossover size has not been measured.
    if (mode != "metal" && mode != "omp" && mode != "cuda" && mode != "hip" && mode != "sycl")
        return solver;

    std::string label, reason;
    if (auto marcher = try_plugin(mode, requested_threads, &label, &reason)) {
        solver->attachDeviceMarcher(std::move(marcher));
        if (mode == "metal")
            label = "metal (Kokkos SYCL network faces)";
        if (chosen)
            *chosen = label + ", device substeps";
        std::fprintf(stderr, "[openswmm FV] using device backend: %s\n", label.c_str());
    } else {
        if (mode == "metal")
            throw std::runtime_error("Metal FV network request failed: " + reason);
        std::fprintf(stderr,
                     "[openswmm FV] backend '%s' requested but no usable plugin was found "
                     "(%s); using the CPU solver\n",
                     mode.c_str(), reason.c_str());
    }
    return solver;
}

} // namespace openswmm::fv
