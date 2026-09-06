/**
 * @file NetworkSolverFactory.cpp
 * @brief Implementation of runtime FV backend selection.
 *
 * @details Deliberately structured to read alongside
 *          2d/solver/SurfaceSolverFactory.cpp — same search path rules, same
 *          handle cache, same ABI check, same gate semantics — so the two
 *          plugin loaders can be reviewed as one design.
 *
 * @see NetworkSolverFactory.hpp
 * @ingroup engine_fv
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
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace fs = std::filesystem;

namespace openswmm::fv {
namespace {

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

/// Process-lifetime handle cache: a plugin's code must stay mapped for as long
/// as any solver it produced is alive.
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

/// Try one backend. Returns null (without throwing) if the plugin is absent,
/// broken, reports no device, carries a stale ABI, or — the case that matters
/// for a single-binary plugin — exports the 2D surface factory but not the 1D
/// network one.
std::unique_ptr<INetworkSolver> try_plugin(const std::string& backend,
                                           std::string* chosen) {
    for (const auto& dir : search_dirs()) {
        for (const auto& fname : plugin_filenames(backend)) {
            const fs::path path = fs::path(dir) / fname;
            std::error_code ec;
            if (!fs::exists(path, ec)) continue;

            void* h = load_cached(path.string());
            if (!h) continue;
            auto probe = reinterpret_cast<ProbeFn>(platform_sym(h, "openswmm_gpu_probe"));
            auto make  = reinterpret_cast<MakeFn>(
                platform_sym(h, "openswmm_make_gpu_network_solver"));
            if (!probe || !make) continue;
            OpenSwmmGpuProbe info{};
            const int rc = probe(&info);
            if (info.abi_version != OPENSWMM_GPU_ABI_VERSION) continue;
            if (rc != 0 || info.device_count <= 0) continue;
            void* raw = make(&info);
            if (!raw) continue;
            auto* solver = static_cast<INetworkSolver*>(raw);
            const std::string label = backend + " (" + info.device_name + ")";
            if (chosen) *chosen = label;
            std::fprintf(stderr, "[openswmm FV] using GPU backend: %s\n",
                         label.c_str());
            return std::unique_ptr<INetworkSolver>(solver);
        }
    }
    return nullptr;
}

const char* backend_name(Backend b) {
    switch (b) {
        case Backend::CPU:  return "cpu";
        case Backend::OMP:  return "omp";
        case Backend::CUDA: return "cuda";
        case Backend::HIP:  return "hip";
        case Backend::SYCL: return "sycl";
        case Backend::AUTO:
        default:            return "auto";
    }
}

} // namespace

std::unique_ptr<INetworkSolver> makeNetworkSolver(const FvOptions& opts,
                                                  std::string* chosen,
                                                  int n_cells) {
    auto cpu = [&]() -> std::unique_ptr<INetworkSolver> {
        if (chosen) *chosen = "cpu (explicit finite volume)";
        return std::make_unique<ExplicitFvSolver>();
    };

    std::string mode = lower(env("OPENSWMM_FV_BACKEND"));
    if (mode.empty()) mode = backend_name(opts.backend);
    if (mode == "cpu") return cpu();

    // The implicit pressurized head update (FV_PRESSURIZED_IMPLICIT, slot
    // program R2a) lives in the CPU reference solver only — a device plugin
    // would silently run the pressurized subset explicitly at the edited
    // (advective) census bound, which is exactly the instability the pass
    // exists to remove. Honour the option over the backend request.
    if (opts.pressurized_implicit) return cpu();

    // Small-problem gate. A Kokkos plugin pays a per-kernel launch overhead on
    // EVERY substep, and the built-in solver is itself OpenMP-threaded, so the
    // plugin only pays off once per-cell work amortizes the launches. Carried
    // over from the 2D module, whose auto crossover was measured far above its
    // original 20k default (plan §2 guardrails) — expect the same recalibration
    // here once the Phase 5 benchmarks land. An explicit backend request
    // bypasses the gate.
    const bool gated = (mode == "auto") && [&] {
        if (n_cells <= 0) return false;
        long min_par = opts.min_parallel_cells;
        if (const char* s = std::getenv("OPENSWMM_FV_MIN_PARALLEL_CELLS"))
            min_par = std::atol(s);
        return static_cast<long>(n_cells) < min_par;
    }();

    if (!gated) {
        if (mode == "auto") {
            for (const char* b : {"cuda", "hip", "sycl", "omp"})
                if (auto s = try_plugin(b, chosen)) return s;
        } else if (mode == "omp" || mode == "cuda" || mode == "hip" ||
                   mode == "sycl") {
            if (auto s = try_plugin(mode, chosen)) return s;
        }
    }
    return cpu();
}

} // namespace openswmm::fv
