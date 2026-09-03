/**
 * @file ThreadInfo.hpp
 * @brief Process-wide hardware / OpenMP thread capability query.
 *
 * @details Single source of truth for "how many threads can this process
 *          usefully run", shared by SWMMEngine::start (global OMP team),
 *          DWSolver::setNumThreads (dynamic-wave Picard team),
 *          SurfaceRouter2D (2D marcher) and the C API
 *          swmm_get_thread_info(). See
 *          plans/THREAD_LIMITS_AND_OVERSUBSCRIPTION_PLAN_2026-09-03.md.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */
#ifndef OPENSWMM_CORE_THREADINFO_HPP
#define OPENSWMM_CORE_THREADINFO_HPP

#include <string>
#include <vector>

namespace openswmm {
namespace threadinfo {

/// Logical processors visible to the OS (SMT / hyper-threads included).
/// std::thread::hardware_concurrency(); 0 when unknown.
int logicalCpus();

/// omp_get_max_threads() as seen by this process — the OpenMP runtime's
/// default team size. Lower than logicalCpus() when OMP_NUM_THREADS,
/// OMP_THREAD_LIMIT or a CPU-affinity mask limits the process. 1 when
/// the engine is built without OpenMP.
int ompMaxThreads();

/// 1 when built with SWMM_USE_OPENMP, else 0.
int ompAvailable();

/// macOS: logical CPUs on the PERFORMANCE cluster (hw.perflevel0.logicalcpu);
/// 0 on Intel Macs, other platforms, or sysctl failure.
int perfCores();

/// Threads the Kokkos OpenMP 2D backend was initialised with in this
/// process; 0 until the plugin initialises (or when no plugin is loaded).
int kokkosOmpThreads();

/// Recorded by the engine when it hands a thread count to the Kokkos plugin.
void setKokkosOmpThreads(int n);

/**
 * @brief Resolve a [OPTIONS] THREADS request into a thread count.
 *
 * @details Rule (plan §2):
 *   - requested == 0 → auto: ompMaxThreads(), callers then apply their own
 *     heuristics (model-size gate, macOS P-core clamp).
 *   - requested  > 0 → honoured exactly. Warnings are appended when it
 *     exceeds logicalCpus() (oversubscription), exceeds ompMaxThreads()
 *     (environment / affinity lowered the runtime limit), or, on macOS,
 *     exceeds perfCores() (efficiency cores slow barrier-synchronised teams).
 *
 * @param requested  [OPTIONS] THREADS value.
 * @param what       Short label for warning text ("OpenMP team", "dynamic wave", "2D").
 * @param warnings   Sink for human-readable warnings; may be nullptr.
 * @return           Thread count to request from the runtime (>= 1).
 */
int resolveRequested(int requested, const char* what,
                     std::vector<std::string>* warnings);

/// True when @p threads exceeds the logical CPU count (the runtime would
/// oversubscribe). Used to decide whether an active spin-wait policy is safe.
bool isOversubscribed(int threads);

/// Minimum conduits of momentum work per dynamic-wave team thread before an
/// extra thread pays for its barriers (PERFORMANCE-ONLY; bit-identical).
constexpr int kMinConduitsPerThread = 100;

/**
 * @brief Dynamic-wave Picard team size for a THREADS request.
 *
 * @details resolveRequested(), then the model-size gate
 *          (`n_conduits / kMinConduitsPerThread`, applied to explicit values
 *          too — with a warning), then, for auto only, the macOS
 *          performance-core clamp (`perfCores() - 2`). Does NOT consult
 *          SWMM_DW_THREADS; the caller handles that override.
 */
int dwThreads(int requested, int n_conduits, std::vector<std::string>* warnings);

/**
 * @brief 2D CPU-marcher team size for a THREADS request.
 *
 * @details resolveRequested(), then `n_triangles < 4 * nt → 1` (warned when
 *          it reduces an explicit request).
 */
int twoDThreads(int requested, int n_triangles, std::vector<std::string>* warnings);

} // namespace threadinfo
} // namespace openswmm

#endif // OPENSWMM_CORE_THREADINFO_HPP
