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
#include "FvOptions.hpp"
#include "INetworkSolver.hpp"
#include "../../2d/solver/GpuPluginAbi.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
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

/**
 * @brief Probe candidate modules and construct the first usable network solver for a backend.
 *
 * Requires openswmm_gpu_probe and openswmm_make_gpu_network_solver. A zero probe status, matching
 * ABI and positive device count are required. The returned object must support deletion through
 * INetworkSolver under the shared C++ ABI. Success prints a selection message to stderr; module
 * handles remain cached. This function does not catch allocation, filesystem-conversion, label or
 * plugin exceptions.
 *
 * @param[in] backend Recognized backend suffix used for filenames and the selection label.
 * @param[out] chosen Optional writable label; changed only after a plugin factory returns a
 *     non-null solver.
 * @return Owning solver pointer, or nullptr if every candidate fails the
 *     existence/load/export/ABI/device/factory checks.
 */
std::unique_ptr<INetworkSolver> try_plugin(const std::string& backend, std::string* chosen) {
    for (const auto& dir : search_dirs()) {
        for (const auto& fname : plugin_filenames(backend)) {
            const fs::path path = fs::path(dir) / fname;
            std::error_code ec;
            if (!fs::exists(path, ec))
                continue;

            void* h = load_cached(path.string());
            if (!h)
                continue;
            auto probe = reinterpret_cast<ProbeFn>(platform_sym(h, "openswmm_gpu_probe"));
            auto make =
                reinterpret_cast<MakeFn>(platform_sym(h, "openswmm_make_gpu_network_solver"));
            if (!probe || !make)
                continue;
            OpenSwmmGpuProbe info{};
            const int rc = probe(&info);
            if (info.abi_version != OPENSWMM_GPU_ABI_VERSION)
                continue;
            if (rc != 0 || info.device_count <= 0)
                continue;
            void* raw = make(&info);
            if (!raw)
                continue;
            auto* solver = static_cast<INetworkSolver*>(raw);
            const std::string label = backend + " (" + info.device_name + ")";
            if (chosen)
                *chosen = label;
            std::fprintf(stderr, "[openswmm FV] using GPU backend: %s\n", label.c_str());
            return std::unique_ptr<INetworkSolver>(solver);
        }
    }
    return nullptr;
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
    case Backend::AUTO:
    default:
        return "auto";
    }
}

} // namespace

std::unique_ptr<INetworkSolver> makeNetworkSolver(const FvOptions& opts, std::string* chosen,
                                                  int n_cells) {
    auto cpu = [&]() -> std::unique_ptr<INetworkSolver> {
        if (chosen)
            *chosen = "cpu (explicit finite volume)";
        return std::make_unique<ExplicitFvSolver>();
    };

    std::string mode = lower(env("OPENSWMM_FV_BACKEND"));
    if (mode.empty())
        mode = backend_name(opts.backend);
    if (mode == "cpu")
        return cpu();

    // The implicit pressurized head update (FV_PRESSURIZED_IMPLICIT, slot
    // program R2a) lives in the CPU reference solver only — a device plugin
    // would silently run the pressurized subset explicitly at the edited
    // (advective) census bound, which is exactly the instability the pass
    // exists to remove. Honour the option over the backend request.
    if (opts.pressurized_implicit)
        return cpu();

    // Small-problem gate. A Kokkos plugin pays a per-kernel launch overhead on
    // EVERY substep, and the built-in solver is itself OpenMP-threaded, so the
    // plugin only pays off once per-cell work amortizes the launches. Carried
    // over from the 2D module, whose auto crossover was measured far above its
    // original 20k default (plan §2 guardrails) — expect the same recalibration
    // here once the Phase 5 benchmarks land. An explicit backend request
    // bypasses the gate.
    const bool gated = (mode == "auto") && [&] {
        if (n_cells <= 0)
            return false;
        long min_par = opts.min_parallel_cells;
        if (const char* s = std::getenv("OPENSWMM_FV_MIN_PARALLEL_CELLS"))
            min_par = std::atol(s);
        return static_cast<long>(n_cells) < min_par;
    }();

    if (!gated) {
        if (mode == "auto") {
            for (const char* b : {"cuda", "hip", "sycl", "omp"})
                if (auto s = try_plugin(b, chosen))
                    return s;
        } else if (mode == "omp" || mode == "cuda" || mode == "hip" || mode == "sycl") {
            if (auto s = try_plugin(mode, chosen))
                return s;
        }
    }
    return cpu();
}

} // namespace openswmm::fv
