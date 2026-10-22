/**
 * @file      SurfaceSolverFactory.cpp
 * @brief     Implementation of runtime 2D surface-solver backend selection.
 *
 * @details The explicit local-inertial LTS marcher is the only 2D integrator
 *          (D2 retirement, 2026-07-29). Selection is purely serial-vs-plugin:
 *          prefer the Kokkos marcher plugin (openswmm_make_gpu_explicit_solver
 *          — OpenMP threads on the host build, device-resident on
 *          CUDA/HIP/SYCL), fall back to the serial ExplicitInertialSolver.
 *          Respects [2D_OPTIONS] BACKEND (AUTO|CPU|OMP|CUDA|HIP|SYCL), the
 *          OPENSWMM_2D_BACKEND environment override, and the small-mesh
 *          launch-overhead gates. The marcher kernels implement both
 *          cell closures, so no CELL_CLOSURE restriction applies.
 *
 * @see SurfaceSolverFactory.hpp
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifdef OPENSWMM_HAS_2D

#include "SurfaceSolverFactory.hpp"
#include "ISurfaceSolver.hpp"
#include "ExplicitInertialSolver.hpp"
#include "GpuPluginAbi.h"
#include "PluginSurfaceSolver.hpp"
#include <cstring>
#include <stdexcept>
#include "../data/SolverOptions2D.hpp"
#include "../../core/ThreadInfo.hpp"

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

namespace openswmm::twoD {
namespace {

// ---------------------------------------------------------------------------
// Platform dynamic-loading helpers (mirror plugins/PluginFactory.cpp).
// ---------------------------------------------------------------------------

/**
 * @brief Open one platform shared library for runtime plugin discovery.
 * @param[in] path Borrowed filesystem path; Windows uses LoadLibraryA, POSIX uses immediate
 * local-symbol dlopen.
 * @return Owned OS module handle on success or NULL on loader failure. Successful handles are
 * retained by load_cached() for process lifetime; this helper does not close them.
 * @note Loading may run library initialization code. No path string is retained by this wrapper;
 * OS error diagnostics are not translated here.
 */
void* platform_load(const std::string& path) {
#if defined(_WIN32)
    return static_cast<void*>(::LoadLibraryA(path.c_str()));
#else
    return ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

/**
 * @brief Resolve one exact exported plugin symbol.
 * @param[in] handle Borrowed loaded OS module handle; NULL returns NULL.
 * @param[in] sym Required NUL-terminated export name.
 * @return Borrowed code/data address or NULL when unavailable. Valid only while the containing
 * module remains loaded; caller casts to the matched ABI type. No ownership transfer or model
 * mutation.
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
 * @brief Locate the directory containing this engine/factory module.
 * @return Newly owned directory path, or empty when platform module lookup fails.
 * @note Does not load/unload the module. Filesystem/string conversions may allocate/throw;
 * Windows uses its bounded MAX_PATH module-name buffer.
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

// The controlling thread and Kokkos runtime live until process exit. Keep
// the thread's code mapped even if a dynamic API client releases its handle.
/**
 * @brief Pin this module and acquire the resident plugin controlling thread.
 * @return Shared handle to GpuExecutionOwner::shared(), owning no simulation model.
 * @throws std::runtime_error When the containing module cannot be identified/retained.
 * @note Once-only module pin and owner initialization are synchronized by C++ static
 * initialization. POSIX keeps an extra dlopen reference; Windows pins the module. Owner/thread
 * creation and allocation may throw; code remains resident through process exit.
 */
std::shared_ptr<GpuExecutionOwner> execution_owner() {
    static const bool pinned = [] {
#if defined(_WIN32)
        HMODULE module = nullptr;
        if (!::GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                      GET_MODULE_HANDLE_EX_FLAG_PIN,
                                  reinterpret_cast<LPCSTR>(&execution_owner), &module))
            throw std::runtime_error("Cannot retain accelerator execution module");
#else
        Dl_info info{};
        if (::dladdr(reinterpret_cast<void*>(&execution_owner), &info) && info.dli_fname) {
            // No dlclose: retain one module reference. An implementation in
            // the main executable is already resident and may reject dlopen.
            void* retained = ::dlopen(info.dli_fname, RTLD_NOW | RTLD_LOCAL);
            if (!retained) {
                const std::string path(info.dli_fname);
                if (path.find(".so") != std::string::npos ||
                    path.find(".dylib") != std::string::npos)
                    throw std::runtime_error("Cannot retain accelerator execution module");
            }
        } else {
            throw std::runtime_error("Cannot identify accelerator execution module");
        }
#endif
        return true;
    }();
    (void)pinned;
    return GpuExecutionOwner::shared();
}

/**
 * @brief Lowercase a copied backend policy token byte by byte.
 * @param[in] s Owned input token, transformed in place using std::tolower on unsigned bytes.
 * @return Newly owned lowercase token; no trimming/Unicode normalization or shared-state
 * mutation.
 */
std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/**
 * @brief Copy a process environment variable into owned string storage.
 * @param[in] name Required NUL-terminated environment key.
 * @return Owned variable contents, or empty for absent/empty values.
 * @note getenv storage is not retained; string allocation may throw. Caller avoids concurrent
 * environment mutation during backend selection.
 */
std::string env(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : std::string();
}

// Candidate shared-library file names for a backend on this platform.
/**
 * @brief Construct platform library-name candidates for one backend.
 * @param[in] backend Borrowed lower-case backend token, normally omp/cuda/hip/sycl; no semantic
 * validation here.
 * @return Owned filename vector: DLL on Windows, dylib then so on macOS, so elsewhere.
 * @note Does not access the filesystem or load a library; allocations may throw.
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

// Directories to search, in priority order.
/**
 * @brief Assemble prioritized plugin search directories.
 * @return Owned list from nonempty OPENSWMM_GPU_PLUGIN_PATH components, then engine directory
 * and its gpu subdirectory when known. Windows separates with semicolons, POSIX with colons;
 * duplicates are preserved.
 * @note Does not verify directory existence or load plugins. Environment/path/string allocation
 * may throw; no borrowed getenv storage is retained.
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

// Process-lifetime handle cache: a plugin's code must stay mapped for as long
// as any solver it produced is alive, and reloading the same lib per model is
// wasteful, so handles are opened once and never closed.
/**
 * @brief Open a plugin once per exact path and keep its module resident.
 * @param[in] path Borrowed exact module path, copied into the cache only after successful
 * loading.
 * @return Borrowed process-lifetime module handle or NULL on loader failure; never close the
 * returned handle.
 * @note A static mutex serializes cache lookup/load/insertion. Failures are not cached and can
 * be retried. Map/string allocation may throw; path canonicalization and plugin compatibility
 * checks are separate.
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
using HostThreadsFn = int (*)(void);

/**
 * @brief Verify the loaded plugin's generated C++ build fingerprint.
 * @param[in] handle Borrowed loaded module handle.
 * @param[in] path Borrowed display path used in rejection diagnostics.
 * @return True only for an exported non-NULL exact current build contract, including its
 * terminator; false otherwise. Without a core build-contract macro all plugins are refused.
 * @note Invokes only the fingerprint export before any probe/factory. Rejection writes stderr.
 * The caller invokes this on the execution owner; plugin exceptions are handled by the caller.
 */
bool compatible_plugin(void* handle, const std::string& path) {
    using Contract = const char* (*)();
    auto query = reinterpret_cast<Contract>(platform_sym(handle, "openswmm_gpu_build_contract"));
#ifdef OPENSWMM_GPU_BUILD_CONTRACT
    const char* id = query ? query() : nullptr;
    if (id &&
        std::strncmp(id, OPENSWMM_GPU_BUILD_CONTRACT, sizeof(OPENSWMM_GPU_BUILD_CONTRACT)) == 0)
        return true;
#endif
    std::fprintf(stderr,
                 "[openswmm 2D] rejected incompatible plugin: %s (rebuild with this engine)\n",
                 path.c_str());
    return false;
}

// Try to load a specific backend plugin and build its marcher solver. Returns
// null (without throwing) if the plugin is absent, broken, reports no device,
// carries a stale ABI, or does not export the explicit-marcher factory.
//
// requested_threads: raw [OPTIONS] THREADS (0 = auto). Host (OpenMP) plugins
// take it as OpenSwmmGpuProbe::requested_threads (ABI v5).
/**
 * @brief Search, validate, construct and wrap one requested plugin backend.
 * @param[in] backend Borrowed validated lower-case backend token.
 * @param[out] chosen Optional owned-string destination for the selected label; written after
 * successful raw construction and possibly before a later adapter allocation fails.
 * @param[in] requested_threads Model host thread preference, zero for automatic performance-core
 * selection; environment/plugin initialization may override it.
 * @return Owned core PluginSurfaceSolver adapter, or NULL when all candidates are
 * absent/incompatible/unusable.
 * @pre Runs on execution_owner()'s worker thread. Successful module handles stay resident;
 * plugin-allocated solver destruction is guarded before subsequent allocations.
 * @note Allocation/foreign plugin exceptions can propagate to try_plugin(), which catches them.
 * Emits selection/failure/thread-mismatch diagnostics and records the effective Kokkos host
 * count; no model arrays are initialized here.
 */
std::unique_ptr<ISurfaceSolver> try_plugin_on_owner(const std::string& backend, std::string* chosen,
                                                    int requested_threads) {
    for (const auto& dir : search_dirs()) {
        for (const auto& fname : plugin_filenames(backend)) {
            const fs::path path = fs::path(dir) / fname;
            std::error_code ec;
            if (!fs::exists(path, ec))
                continue;

            void* h = load_cached(path.string());
            if (!h)
                continue;
            if (!compatible_plugin(h, path.string()))
                continue;
            auto destroy = reinterpret_cast<PluginSurfaceSolver::Destroy>(
                platform_sym(h, "openswmm_destroy_gpu_explicit_solver"));
            if (!destroy)
                continue;
            auto probe = reinterpret_cast<ProbeFn>(platform_sym(h, "openswmm_gpu_probe"));
            auto make =
                reinterpret_cast<MakeFn>(platform_sym(h, "openswmm_make_gpu_explicit_solver"));
            if (!probe || !make)
                continue;
            OpenSwmmGpuProbe info{};
            const int rc = probe(&info);
            if (info.abi_version != OPENSWMM_GPU_ABI_VERSION ||
                !std::memchr(info.device_name, '\0', sizeof(info.device_name)))
                continue;
            const int expected_vendor = backend == "omp"    ? OPENSWMM_GPU_VENDOR_OPENMP
                                        : backend == "cuda" ? OPENSWMM_GPU_VENDOR_CUDA
                                        : backend == "hip"  ? OPENSWMM_GPU_VENDOR_HIP
                                                            : OPENSWMM_GPU_VENDOR_SYCL;
            if (info.vendor != expected_vendor)
                continue;
            if (rc != 0 || info.device_count <= 0) {
                // No usable device behind this plugin — try the next candidate.
                continue;
            }

            // Host-backend thread count. Auto (0) on Apple Silicon defaults to
            // the performance-core count — efficiency cores drag the fenced
            // kernel loops (plan §5.1); elsewhere 0 leaves the Kokkos default.
            const bool host_backend = info.vendor == OPENSWMM_GPU_VENDOR_OPENMP;
            int host_req = requested_threads;
            if (host_backend && host_req == 0)
                host_req = threadinfo::perfCores();
            if (host_backend)
                info.requested_threads = host_req;

            void* raw = make(&info);
            if (!raw) {
                using Error = const char* (*)();
                auto error = reinterpret_cast<Error>(platform_sym(h, "openswmm_gpu_last_error"));
                const char* diagnostic = error ? error() : nullptr;
                std::fprintf(stderr, "[openswmm 2D] plugin construction failed: %s: %s\n",
                             path.string().c_str(), diagnostic ? diagnostic : "no diagnostic");
                continue;
            }
            auto* solver = static_cast<ISurfaceSolver*>(raw);
            // Install plugin-owned destruction before any subsequent allocation.
            std::unique_ptr<ISurfaceSolver, PluginSurfaceSolver::Destroy> guard(solver, destroy);
            const std::string label = backend + " (" + info.device_name + ")";
            if (chosen)
                *chosen = label;

            // Kokkos initialises once per process: record what the host
            // backend actually runs with and warn when this model asked for
            // something else (only a restart can change it).
            if (host_backend) {
                auto host_threads =
                    reinterpret_cast<HostThreadsFn>(platform_sym(h, "openswmm_gpu_host_threads"));
                const int in_use = host_threads ? host_threads() : 0;
                if (in_use > 0) {
                    threadinfo::setKokkosOmpThreads(in_use);
                    if (host_req > 0 && in_use != host_req)
                        std::fprintf(stderr,
                                     "[openswmm 2D] Kokkos OpenMP backend already initialised "
                                     "with %d threads in this process; THREADS = %d cannot "
                                     "take effect until the application restarts.\n",
                                     in_use, host_req);
                }
            }
            // Announce the selected acceleration backend once, on successful
            // plugin load. This is the signal the omp gate tests check for; it
            // also gives operators a record of which 2D backend ran.
            std::fprintf(stderr, "[openswmm 2D] using GPU backend: %s\n", label.c_str());
            auto adapter =
                std::make_unique<PluginSurfaceSolver>(solver, destroy, execution_owner());
            guard.release();
            return adapter;
        }
    }
    return nullptr;
}

/**
 * @brief Discover/construct one backend synchronously on the controlling thread.
 * @param[in] backend Borrowed validated lower-case backend token.
 * @param[out] chosen Optional label destination, forwarded only for the duration of this call;
 * may be changed before an exceptional late construction refusal.
 * @param[in] threads Requested model host thread count, zero automatic.
 * @return Owned adapter on success, NULL after discovery failure or any caught exception; emits
 * stderr diagnostics for exceptional failures.
 * @note Borrows all inputs synchronously through GpuExecutionOwner::invoke(). Does not
 * initialize model state or unload modules.
 */
std::unique_ptr<ISurfaceSolver> try_plugin(const std::string& backend, std::string* chosen,
                                           int threads) {
    try {
        return execution_owner()->invoke(
            [&] { return try_plugin_on_owner(backend, chosen, threads); });
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[openswmm 2D] plugin %s failed before initialization: %s\n",
                     backend.c_str(), e.what());
    } catch (...) {
        std::fprintf(stderr, "[openswmm 2D] plugin %s failed before initialization\n",
                     backend.c_str());
    }
    return nullptr;
}

// [2D_OPTIONS] BACKEND value as the lower-case policy token the env var uses.
/**
 * @brief Format the model backend enum as a policy token.
 * @param[in] b Backend2D enum.
 * @return Owned lower-case cpu/omp/cuda/hip/sycl token; AUTO and unknown values map to auto. No
 * model/environment changes.
 */
std::string backend_name(Backend2D b) {
    switch (b) {
    case Backend2D::CPU:
        return "cpu";
    case Backend2D::OMP:
        return "omp";
    case Backend2D::CUDA:
        return "cuda";
    case Backend2D::HIP:
        return "hip";
    case Backend2D::SYCL:
        return "sycl";
    case Backend2D::AUTO:
        break;
    }
    return "auto";
}

} // anonymous namespace

std::unique_ptr<ISurfaceSolver> makeSurfaceSolver(const SolverOptions2D& opts, std::string* chosen,
                                                  int n_cells, bool plugin_capable) {
    auto serial_marcher = [&](const char* label =
                                  "cpu (explicit marcher)") -> std::unique_ptr<ISurfaceSolver> {
        if (chosen)
            *chosen = label;
        return std::make_unique<ExplicitInertialSolver>();
    };

    // Policy: the environment variable (operator/CI override) wins; otherwise
    // the model's own [2D_OPTIONS] BACKEND — same precedence as the FV
    // module's OPENSWMM_FV_BACKEND over FV_BACKEND.
    std::string mode = lower(env("OPENSWMM_2D_BACKEND"));
    if (mode.empty())
        mode = opts.backend == Backend2D::AUTO && opts.auto_backend_cpu
                   ? "cpu"
                   : backend_name(opts.backend);
    if (mode != "cpu" && mode != "auto" && mode != "omp" && mode != "cuda" && mode != "hip" &&
        mode != "sycl")
        throw std::invalid_argument("Invalid OPENSWMM_2D_BACKEND: " + mode);
    if (mode == "cpu")
        return serial_marcher();

    // R6 (2D_TRI_QUAD_MESH_PLAN / 2D_FULL_SWE plan): the Kokkos plugin
    // implements the triangle-only LOCAL_INERTIAL scheme. A mixed tri-quad
    // mesh or a non-LI momentum closure is refused LOUDLY and served by the
    // CPU marcher — never a silent physics substitution. S4b: a model with
    // transport rows (species / age / temperature) is refused the same way.
    if (!plugin_capable) {
        // Say so under AUTO as well: a model that silently loses the plugin
        // (a DIFFUSIVE_WAVE deck on a 100k-cell mesh, say) looks like a
        // performance regression to the modeller.
        std::fprintf(
            stderr,
            "[openswmm 2D] backend '%s': the GPU/Kokkos plugin serves only "
            "all-triangle meshes under MOMENTUM_EQUATION LOCAL_INERTIAL with "
            "no transport rows (species / age / temperature) or coupled groundwater/porous heat; "
            "using the CPU marcher for this model.\n",
            mode.empty() ? "auto" : mode.c_str());
        return serial_marcher("cpu (explicit marcher; plugin not applicable)");
    }

    // Explicit backend request bypasses every mesh-size gate — the operator
    // asked for it by name (OPENSWMM_2D_BACKEND / BACKEND = omp|cuda|hip|sycl).
    if (mode == "omp" || mode == "cuda" || mode == "hip" || mode == "sycl") {
        if (auto s = try_plugin(mode, chosen, opts.requested_threads))
            return s;
        std::fprintf(stderr,
                     "[openswmm 2D] backend '%s' requested but no usable plugin/device "
                     "found; falling back to the CPU marcher.\n",
                     mode.c_str());
        return serial_marcher();
    }

    // auto / empty: the two plugin classes have very different economics, so
    // they get separate mesh-size floors (env-tunable; n_cells<=0 unknown = no
    // gate).
    //   * DEVICE backends (cuda/hip/sycl) offload to a SEPARATE accelerator, so
    //     the launch/transfer overhead amortizes far earlier — a low floor.
    //     Previously they shared the omp floor below and so a discrete GPU sat
    //     idle for every sub-200k mesh (the reported "CUDA not utilized").
    //   * The OMP host plugin competes with the already-OMP-threaded serial
    //     marcher (measured 15-20x SLOWER at 25k cells on Bellinge), so it
    //     keeps a high floor.
    auto floor_for = [](const char* var, long dflt) -> long {
        if (const char* s = std::getenv(var))
            return std::atol(s);
        return dflt;
    };
    const long dev_floor = floor_for("OPENSWMM_2D_MIN_PARALLEL_CELLS_DEVICE", 10000);
    const long omp_floor = floor_for("OPENSWMM_2D_MIN_PARALLEL_CELLS", 50000);

    if (n_cells <= 0 || n_cells >= dev_floor)
        for (const char* b : {"cuda", "hip", "sycl"})
            if (auto s = try_plugin(b, chosen, opts.requested_threads))
                return s;
    if (n_cells <= 0 || n_cells >= omp_floor)
        if (auto s = try_plugin("omp", chosen, opts.requested_threads))
            return s;

    std::fprintf(stderr,
                 "[openswmm 2D] using CPU marcher (no GPU device selected; n_cells=%d, "
                 "device floor=%ld cells). Set [2D_OPTIONS] BACKEND CUDA (or "
                 "OPENSWMM_2D_BACKEND=cuda) to force the GPU, or lower "
                 "OPENSWMM_2D_MIN_PARALLEL_CELLS_DEVICE.\n",
                 n_cells, dev_floor);
    return serial_marcher();
}

/*
 * @brief Discover and bind a qualified stateless OpenMP recharge operator.
 * @param[in] n_cells Mesh-cell count, currently unused; no AUTO threshold applies.
 * @param[in] opts Borrowed validated solver options and requested/session backend policy.
 * @note Formal descriptor construction is described in the header. Callback exceptions during
 * evaluate are translated to nonzero failure; the threads callback can propagate owner/foreign
 * exceptions. Capability and evaluation work execute synchronously on the shared execution
 * owner.
 */
GwRechargeBackend makeGwRechargeBackend(const SolverOptions2D& opts, int n_cells) {
    GwRechargeBackend result;
    auto mode = lower(env("OPENSWMM_2D_BACKEND"));
    if (mode.empty())
        mode = opts.backend == Backend2D::AUTO && opts.auto_backend_cpu
                   ? "cpu"
                   : backend_name(opts.backend);
    if (mode == "cpu") {
        result.diagnostic = "CPU requested";
        return result;
    }
    if (mode != "omp" && mode != "auto" && !mode.empty()) {
        result.diagnostic = "Requested backend has no qualified groundwater recharge operator";
        return result;
    }
    // Surface mesh-size thresholds do not establish a benefit for this
    // separate packed operator. Keep AUTO scalar until integrated workloads
    // qualify a selection policy; explicit OMP remains available for testing.
    (void)n_cells;
    if (mode != "omp") {
        result.diagnostic =
            "Raw recharge requires explicit OMP; AUTO retains CPU pending integrated performance qualification";
        return result;
    }
    using Capabilities = int (*)(OpenSwmmGwCapabilitiesV1*);
    for (const auto& dir : search_dirs())
        for (const auto& name : plugin_filenames("omp")) {
            const auto path = (fs::path(dir) / name).string();
            auto owner = execution_owner();
            void* handle = nullptr;
            bool compatible = false;
            try {
                compatible = owner->invoke([&] {
                    handle = load_cached(path);
                    return handle && compatible_plugin(handle, path);
                });
            } catch (...) {
                continue;
            }
            if (!compatible)
                continue;
            auto query =
                reinterpret_cast<Capabilities>(platform_sym(handle, "openswmm_gw_capabilities_v1"));
            auto evaluate = reinterpret_cast<GwRechargeBackend::Evaluate>(
                platform_sym(handle, "openswmm_gw_recharge_v1"));
            auto threads = reinterpret_cast<GwRechargeBackend::Threads>(
                platform_sym(handle, "openswmm_gpu_host_threads"));
            if (!query || !evaluate || !threads)
                continue;
            OpenSwmmGwCapabilitiesV1 c{};
            c.struct_size = sizeof(c);
            int query_status = 1;
            try {
                query_status = owner->invoke([&] { return query(&c); });
            } catch (...) {
                continue;
            }
            if (query_status != 0 || c.struct_size != sizeof(c) ||
                c.version != OPENSWMM_GW_RECHARGE_ABI_VERSION ||
                c.vendor != OPENSWMM_GPU_VENDOR_OPENMP ||
                c.execution_kind != OPENSWMM_GW_EXECUTION_HOST_OPENMP ||
                c.operators != OPENSWMM_GW_OPERATOR_CLOSED_FORM_RECHARGE || c.closures != 1u ||
                c.soil_laws != 15u || c.host_owned != 15u || c.time_integrators != 3u ||
                c.canonical_host_restart != 1 || c.max_layers != 0 || c.species_classes != 0 ||
                c.label[127] != 0)
                continue;
            result.evaluate = [owner, evaluate](const OpenSwmmGwRechargeInputV1* in, int n,
                                                double* out, int count) {
                try {
                    return owner->invoke([&] { return evaluate(in, n, out, count); });
                } catch (...) {
                    return 1;
                }
            };
            result.threads = [owner, threads] { return owner->invoke([&] { return threads(); }); };
            result.capabilities = c;
            result.diagnostic =
                "Only CLOSED_FORM raw recharge is host-parallel; canonical stores and all other operators remain CPU-owned";
            return result;
        }
    result.diagnostic = "No compatible Kokkos OpenMP recharge extension found; CPU used";
    return result;
}

} // namespace openswmm::twoD

#endif // OPENSWMM_HAS_2D
