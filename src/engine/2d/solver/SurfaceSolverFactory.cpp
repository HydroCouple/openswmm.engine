/**
 * @file SurfaceSolverFactory.cpp
 * @brief Implementation of runtime 2D surface-solver backend selection.
 *
 * @see SurfaceSolverFactory.hpp
 * @ingroup engine_2d
 */

#ifdef OPENSWMM_HAS_2D

#include "SurfaceSolverFactory.hpp"
#include "ISurfaceSolver.hpp"
#include "CvodeSurfaceSolver.hpp"
#include "GpuPluginAbi.h"
#include "../data/SolverOptions2D.hpp"

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
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace openswmm::twoD {
namespace {

// ---------------------------------------------------------------------------
// Platform dynamic-loading helpers (mirror plugins/PluginFactory.cpp).
// ---------------------------------------------------------------------------

void* platform_load(const std::string& path) {
#if defined(_WIN32)
    return static_cast<void*>(::LoadLibraryA(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* platform_sym(void* handle, const char* sym) {
    if (!handle) return nullptr;
#if defined(_WIN32)
    return reinterpret_cast<void*>(::GetProcAddress(static_cast<HMODULE>(handle), sym));
#else
    return ::dlsym(handle, sym);
#endif
}

std::string this_library_dir() {
#if defined(_WIN32)
    HMODULE hm = nullptr;
    if (::GetModuleHandleExA(
            GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
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

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Candidate shared-library file names for a backend on this platform.
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

// Directories to search, in priority order.
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
        if (!tok.empty()) dirs.push_back(tok);
        if (pos == std::string::npos) break;
        start = pos + 1;
    }
    const std::string libdir = this_library_dir();
    if (!libdir.empty()) {
        dirs.push_back(libdir);
        dirs.push_back((fs::path(libdir) / "gpu").string());
    }
    return dirs;
}

// Process-lifetime handle cache: a plugin's code must stay mapped for as long
// as any solver it produced is alive, and reloading the same lib per model is
// wasteful, so handles are opened once and never closed.
void* load_cached(const std::string& path) {
    static std::mutex mtx;
    static std::map<std::string, void*> cache;
    std::lock_guard<std::mutex> lk(mtx);
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    void* h = platform_load(path);
    if (h) cache.emplace(path, h);
    return h;
}

using ProbeFn = int (*)(OpenSwmmGpuProbe*);
using MakeFn  = void* (*)(const OpenSwmmGpuProbe*);

// Try to load a specific backend plugin and build its solver. Returns null
// (without throwing) if the plugin is absent, broken, or reports no device.
std::unique_ptr<ISurfaceSolver> try_plugin(const std::string& backend,
                                           std::string* chosen) {
    for (const auto& dir : search_dirs()) {
        for (const auto& fname : plugin_filenames(backend)) {
            const fs::path path = fs::path(dir) / fname;
            std::error_code ec;
            if (!fs::exists(path, ec)) continue;

            void* h = load_cached(path.string());
            if (!h) continue;
            auto probe = reinterpret_cast<ProbeFn>(platform_sym(h, "openswmm_gpu_probe"));
            auto make  = reinterpret_cast<MakeFn>(platform_sym(h, "openswmm_make_gpu_surface_solver"));
            if (!probe || !make) continue;
            OpenSwmmGpuProbe info{};
            const int rc = probe(&info);
            if (info.abi_version != OPENSWMM_GPU_ABI_VERSION) continue;
            if (rc != 0 || info.device_count <= 0) {
                // No usable device behind this plugin — try the next candidate.
                continue;
            }
            void* raw = make(&info);
            if (!raw) continue;
            auto* solver = static_cast<ISurfaceSolver*>(raw);
            const std::string label = backend + " (" + info.device_name + ")";
            if (chosen) *chosen = label;
            // Announce the selected acceleration backend once, on successful
            // plugin load. This is the signal test_engine_2d_omp_default checks
            // for; it also gives operators a record of which 2D backend ran.
            std::fprintf(stderr, "[openswmm 2D] using GPU backend: %s\n",
                         label.c_str());
            return std::unique_ptr<ISurfaceSolver>(solver);
        }
    }
    return nullptr;
}

} // anonymous namespace

std::unique_ptr<ISurfaceSolver> makeSurfaceSolver(const SolverOptions2D& /*opts*/,
                                                  std::string* chosen,
                                                  int n_cells) {
    auto cpu = [&]() -> std::unique_ptr<ISurfaceSolver> {
        if (chosen) *chosen = "cpu (serial CVODE)";
        return std::make_unique<CvodeSurfaceSolver>();
    };

    const std::string mode = lower(env("OPENSWMM_2D_BACKEND"));

    if (mode.empty() || mode == "auto") {
        // Small-mesh gate: a Kokkos plugin (OpenMP/CUDA/HIP/SYCL) pays a
        // per-kernel launch overhead that dominates on small meshes, where the
        // serial CVODE solver is faster. Below a threshold, stay serial unless
        // the backend was requested explicitly above. Threshold is env-tunable
        // (OPENSWMM_2D_MIN_PARALLEL_CELLS); 0 cells = unknown ⇒ no gate.
        if (n_cells > 0) {
            long min_par = 20000;
            if (const char* s = std::getenv("OPENSWMM_2D_MIN_PARALLEL_CELLS"))
                min_par = std::atol(s);
            if (n_cells < min_par) return cpu();
        }
        // Prefer a GPU device, then the Kokkos-OpenMP CPU-parallel plugin, then
        // the serial solver. `omp` is auto-selected by design as of 2026-06-13
        // (docs/2D_GPU_PORTABLE_CVODE_STRATEGY.md §4.2, revised) — so OpenMP is
        // the default acceleration *wherever the plugin is installed*. The omp
        // plugin is NOT part of the base/portable build (it is opt-in, like the
        // device plugins), so a stock install finds no plugin here and falls
        // through to the serial CPU solver. The base build stays Kokkos-free.
        for (const char* b : {"cuda", "hip", "sycl", "omp"}) {
            if (auto s = try_plugin(b, chosen))
                return s;
        }
        return cpu();
    }

    if (mode == "cpu") return cpu();

    if (mode == "omp" || mode == "cuda" || mode == "hip" || mode == "sycl") {
        if (auto s = try_plugin(mode, chosen))
            return s;
        return cpu();
    }

    return cpu();
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
