/**
 * @file ThreadInfo.cpp
 * @brief Process-wide hardware / OpenMP thread capability query.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */
#include "ThreadInfo.hpp"

#include <atomic>
#include <cstdio>
#include <thread>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif

#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif

namespace openswmm {
namespace threadinfo {

namespace {
std::atomic<int> g_kokkos_omp_threads{0};
} // namespace

int logicalCpus() {
    const unsigned n = std::thread::hardware_concurrency();
    return static_cast<int>(n);
}

int ompMaxThreads() {
#if defined(SWMM_USE_OPENMP)
    const int n = omp_get_max_threads();
    return n > 0 ? n : 1;
#else
    return 1;
#endif
}

int ompAvailable() {
#if defined(SWMM_USE_OPENMP)
    return 1;
#else
    return 0;
#endif
}

int perfCores() {
#if defined(__APPLE__)
    int n = 0;
    std::size_t sz = sizeof(n);
    if (sysctlbyname("hw.perflevel0.logicalcpu", &n, &sz, nullptr, 0) != 0)
        return 0;
    return (n > 0) ? n : 0;
#else
    return 0;
#endif
}

int kokkosOmpThreads() {
    return g_kokkos_omp_threads.load(std::memory_order_relaxed);
}

void setKokkosOmpThreads(int n) {
    g_kokkos_omp_threads.store(n > 0 ? n : 0, std::memory_order_relaxed);
}

bool isOversubscribed(int threads) {
    const int logical = logicalCpus();
    return logical > 0 && threads > logical;
}

int resolveRequested(int requested, const char* what,
                     std::vector<std::string>* warnings) {
    const int omp_max = ompMaxThreads();
    if (requested <= 0) return omp_max;

    const int nt = requested;
    if (!warnings) return nt;

    char buf[512];
    const int logical = logicalCpus();
    if (logical > 0 && nt > logical) {
        std::snprintf(buf, sizeof buf,
            "THREADS = %d exceeds the %d logical processors on this machine "
            "(%s). The run will be oversubscribed and is likely to be slower; "
            "active spin-wait is disabled for this run.",
            nt, logical, what);
        warnings->emplace_back(buf);
    } else if (nt > omp_max) {
        std::snprintf(buf, sizeof buf,
            "THREADS = %d exceeds the OpenMP limit of %d in this process "
            "(OMP_NUM_THREADS / OMP_THREAD_LIMIT / CPU affinity) for the %s; "
            "requesting %d anyway.",
            nt, omp_max, what, nt);
        warnings->emplace_back(buf);
    }

#if defined(__APPLE__)
    const int pcores = perfCores();
    if (pcores > 0 && nt > pcores) {
        std::snprintf(buf, sizeof buf,
            "THREADS = %d exceeds the %d performance cores on this Mac (%s). "
            "Efficiency cores slow barrier-synchronised solvers (measured up "
            "to 3-4x slower). Use THREADS = 0 (auto) or <= %d.",
            nt, pcores, what, pcores);
        warnings->emplace_back(buf);
    }
#endif
    return nt;
}

int dwThreads(int requested, int n_conduits, std::vector<std::string>* warnings) {
    int nt = resolveRequested(requested, "dynamic wave", warnings);

    // Model-size gate: step the team down until each thread keeps
    // >= kMinConduitsPerThread conduits of momentum work. Applies to explicit
    // requests too — a 16-thread team on 40 conduits is never useful.
    if (nt > 1) {
        int cap = n_conduits / kMinConduitsPerThread;
        if (cap < 1) cap = 1;
        if (cap < nt) {
            if (requested > 0 && warnings) {
                char buf[256];
                std::snprintf(buf, sizeof buf,
                    "THREADS = %d reduced to %d for dynamic wave: the model has "
                    "only %d conduits (%d per thread minimum).",
                    requested, cap, n_conduits, kMinConduitsPerThread);
                warnings->emplace_back(buf);
            }
            nt = cap;
        }
    }

    // Auto only: Apple Silicon performance-core clamp. With active waiting a
    // team thread on an efficiency core turns every Picard barrier into a
    // straggler wait (measured T=8 on 8P+2E: 204 s vs 57 s at T=4). Leave two
    // P-cores for the OS / IO thread. Explicit requests bypass this and are
    // warned by resolveRequested() instead.
    if (requested == 0 && nt > 1) {
        const int pcores = perfCores();
        if (pcores > 2 && nt > pcores - 2) nt = pcores - 2;
    }
    return nt;
}

int twoDThreads(int requested, int n_triangles, std::vector<std::string>* warnings) {
    int nt = resolveRequested(requested, "2D surface", warnings);
    if (n_triangles < 4 * nt) {
        if (requested > 0 && nt > 1 && warnings) {
            char buf[256];
            std::snprintf(buf, sizeof buf,
                "THREADS = %d reduced to 1 for the 2D surface: the mesh has only "
                "%d triangles (need >= 4 per thread).",
                requested, n_triangles);
            warnings->emplace_back(buf);
        }
        nt = 1;
    }
    return nt;
}

} // namespace threadinfo
} // namespace openswmm
