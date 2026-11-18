/**
 * @file      ThreadInfo.hpp
 * @brief     Process-wide hardware / OpenMP thread capability query.
 *
 * @details Single source of truth for "how many threads can this process
 *          usefully run", shared by SWMMEngine::start (global OMP team),
 *          DWSolver::setNumThreads (dynamic-wave Picard team),
 *          SurfaceRouter2D (2D marcher) and the C API
 *          swmm_get_thread_info(). See
 *          plans/THREAD_LIMITS_AND_OVERSUBSCRIPTION_PLAN_2026-09-03.md.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_CORE_THREADINFO_HPP
#define OPENSWMM_CORE_THREADINFO_HPP

#include <string>
#include <vector>

namespace openswmm {
namespace threadinfo {

/**
 * @brief Query OS-visible logical CPU concurrency.
 * @return std::thread::hardware_concurrency() converted to int; zero if unavailable.
 */
int logicalCpus();

/**
 * @brief Query the current calling-context OpenMP maximum.
 *
 * Does not create a team or guarantee that the runtime will provide this many workers.
 * @return Positive omp_get_max_threads() result, normalized to at least one; one without
 *     SWMM_USE_OPENMP.
 */
int ompMaxThreads();

/**
 * @brief Report whether engine OpenMP support was compiled.
 * @return One with SWMM_USE_OPENMP, zero otherwise; no runtime/plugin qualification.
 */
int ompAvailable();

/**
 * @brief Query macOS performance-cluster logical CPU count.
 * @return Positive hw.perflevel0.logicalcpu value, or zero for sysctl failure/unsupported platform.
 */
int perfCores();

/**
 * @brief Read host thread reservation from the current environment.
 *
 * Value is read on every call; trailing nonnumeric bytes accepted by atoi are not separately
 * rejected. Caller avoids environment mutation races and out-of-int-range tokens.
 * @return Positive atoi prefix of OPENSWMM_HOST_RESERVED_THREADS, or zero when
 *     absent/empty/nonpositive.
 */
int hostReservedThreads();

/**
 * @brief Read the engine-recorded Kokkos OpenMP team hint.
 *
 * Does not query or initialize the plugin/runtime.
 * @return Relaxed atomic stored hint; zero until set or after a nonpositive reset.
 */
int kokkosOmpThreads();

/**
 * @brief Store the engine-recorded Kokkos OpenMP team hint.
 *
 * Uses relaxed atomic storage; provides no synchronization for plugin initialization or other
 * engine state.
 *
 * @param[in] n Team count; nonpositive values store zero.
 */
void setKokkosOmpThreads(int n);

/**
 * @brief Resolve a thread request and optionally append oversubscription diagnostics.
 *
 * For positive requests, warns when request+host reservation exceeds known logical CPUs; otherwise
 * warns above the current OpenMP maximum. On macOS can additionally warn above performance-cluster
 * size. No actual runtime team is changed. Caller keeps request+reservation representable as int.
 *
 * @param[in] requested Requested team size; <=0 resolves from OpenMP maximum. Exactly zero
 *     additionally enables automatic core-clamp heuristics.
 * @param[in] what Borrowed non-null NUL-terminated activity label used in formatted warnings.
 * @param[in,out] warnings Optional caller-owned sink; new messages append without clearing existing
 *     entries.
 * @return OpenMP maximum for requested<=0; otherwise the positive request is returned unchanged.
 */
int resolveRequested(int requested, const char* what, std::vector<std::string>* warnings);

/**
 * @brief Compare a team plus host reservation with known logical concurrency.
 *
 * @param[in] threads Team count; caller keeps threads+hostReservedThreads() representable as int.
 * @return True only when logical CPU count is known and the sum exceeds it; false when hardware
 *     count is unknown.
 */
bool isOversubscribed(int threads);

/// Minimum conduits of momentum work per dynamic-wave team thread before an
/// extra thread pays for its barriers (PERFORMANCE-ONLY; bit-identical).
///
/// Measured 2026-11-07 with the OpenMP runtime's default (passive) waiting —
/// the engine no longer forces active spinning (dce68091) — on an M1 Max with
/// ordinary desktop load, wall seconds at 1 / 2 / 4 / 8 threads
/// (tests/output/fv_bellinge_failure_20261102/perf/threads/):
///
///     conduits   1      2      4      8
///      1,015    16.3   29.1   65.2  116.5   every extra thread costs
///      2,100    28.8   28.5   35.2   58.4
///      4,146    38.2   27.9   24.3   36.3   first gain
///      6,402    22.2   14.4   11.6   13.6
///     12,840    17.3   11.1    8.0    7.9
///     24,721    27.4   17.5   11.7   10.8
///     41,678    26.5   19.5   12.7   11.0
///
/// One thread per 2,000 conduits follows that table: 1 below 4,000, 2 at
/// 4,000, 4 at 8,000, 8 from 16,000. The former 100 dates from the forced
/// active-wait era (T=4 was 1.74x on 1,015 conduits then); with passive
/// waiting the same model ran 7x slower at 8 threads than on one.
constexpr int kMinConduitsPerThread = 2000;

/**
 * @brief Resolve a dynamic-wave team request with model-size and automatic core caps.
 *
 * Applies model-size cap to explicit and automatic requests, warning only for explicit reductions.
 * Exactly requested=0 additionally caps to performance cores minus two on macOS when that count
 * exceeds two. Negative requests use OpenMP maximum without that auto-only clamp.
 *
 * @param[in] requested Requested team size; <=0 resolves from OpenMP maximum. Exactly zero
 *     additionally enables automatic core-clamp heuristics.
 * @param[in] n_conduits Conduit count used in floor(count/100), normalized to a minimum one-worker
 *     cap.
 * @param[in,out] warnings Optional caller-owned sink; new messages append without clearing existing
 *     entries.
 * @return Positive recommended team size; does not set OpenMP or honor SWMM_DW_THREADS directly.
 */
int dwThreads(int requested, int n_conduits, std::vector<std::string>* warnings);

/**
 * @brief Resolve a 2D marcher team request using core and triangle-count heuristics.
 *
 * Exactly requested=0 applies the macOS performance-core-minus-two cap before the mesh test.
 * Positive requests reduced by the mesh test append a diagnostic. Negative requests skip the
 * auto-only core cap.
 *
 * @param[in] requested Requested team size; <=0 resolves from OpenMP maximum. Exactly zero
 *     additionally enables automatic core-clamp heuristics.
 * @param[in] n_triangles Triangle count; recommends one worker when n_triangles<4*resolved team.
 *     Caller keeps that product representable as int.
 * @param[in,out] warnings Optional caller-owned sink; new messages append without clearing existing
 *     entries.
 * @return Positive recommended team size; does not create or alter a runtime team.
 */
int twoDThreads(int requested, int n_triangles, std::vector<std::string>* warnings);

} // namespace threadinfo
} // namespace openswmm

#endif // OPENSWMM_CORE_THREADINFO_HPP
