// SPDX-License-Identifier: Apache-2.0
//
// Copyright 2026 Caleb Buahin
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/**
 * @file      KokkosPerfCounters.hpp
 * @brief     OPENSWMM_PERF=1 launch/fence/deep_copy counters for the Kokkos
 *        surface solver, riding the in-process Kokkos::Tools callbacks.
 *
 * @details Phase A2 of plans/CPU_GPU_PERF_REVIEW_PLAN_2026-09-02.md. The
 *          counters answer three questions the wall clock cannot: how many
 *          kernel launches an advance() costs, how long the host blocks in
 *          fences, and how many bytes cross host<->device per advance. They
 *          hook Kokkos::Tools::Experimental::set_*_callback rather than the
 *          ~25 dispatch sites so every launch — including implicit fences and
 *          the deep_copies inside devCopy/devRefresh — is counted without a
 *          line of code in the hot path.
 *
 *          Registration is skipped when an external tool is loaded
 *          (KOKKOS_TOOLS_LIBS / KOKKOS_PROFILE_LIBRARY): Kokkos holds ONE
 *          callback set, and stealing it would blind nsys/kernel-logger,
 *          which attribute by the same kernel labels (plan Phase A3). Use one
 *          or the other per run.
 *
 *          Per-kernel `s=` is HOST-SIDE wall between the begin/end dispatch
 *          callbacks: on the OpenMP backend that is the kernel's execution
 *          time (dispatch blocks); on CUDA/HIP it is launch overhead only —
 *          device time lives in the fence/nsys numbers.
 *
 *          Counters are plain (non-atomic) longs: the engine drives the
 *          solver from one thread, and Kokkos dispatch callbacks fire on the
 *          calling thread. Zero cost when OPENSWMM_PERF is unset (nothing is
 *          registered at all).
 *
 * @ingroup engine_2d_gpu
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_GPU_KOKKOS_PERF_COUNTERS_HPP
#define OPENSWMM_ENGINE_2D_GPU_KOKKOS_PERF_COUNTERS_HPP

#include <Kokkos_Core.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "../../core/PerfTimers.hpp"

namespace openswmm::twoD::gpu::kperf {

struct KernelEntry {
    std::string label;
    long n = 0;       ///< launches
    double sec = 0.0; ///< host-side dispatch wall (== execution on OMP)
};

struct Counters {
    long n_advance = 0;
    long n_launch = 0; ///< parallel_for + reduce + scan
    long n_fence = 0;
    double sec_fence = 0.0;
    long n_deep_copy = 0;
    unsigned long long bytes_h2d = 0, bytes_d2h = 0, bytes_same = 0;
    std::vector<KernelEntry> kernels; ///< indexed by kernel id
    std::map<std::string, std::uint64_t> label_id;
};

// Each solver owns its counters. Only dispatches in its active call scope
// are attributed to it; calls from unrelated Kokkos clients are ignored.
inline thread_local Counters* active = nullptr;
class Scope {
public:
    /**
     * @brief Attribute this thread's nested Kokkos dispatches to one solver counter set.
     * @param[in,out] counters Borrowed per-solver counters; must outlive this scope and any
     * callbacks dispatched within it.
     * @post Saves the previous thread-local active pointer and installs counters. Supports
     * nested scopes on one thread; a counter object must not be used concurrently on multiple
     * threads. No allocation or runtime registration.
     */
    explicit Scope(Counters& counters) : previous_(active) { active = &counters; }
    /**
     * @brief Restore this thread's previous counter attribution on scope exit.
     * @post Restores the saved borrowed pointer; owns/deletes no Counters and invokes no Kokkos
     * fence. Every in-scope callback pair must have completed before destruction.
     */
    ~Scope() { active = previous_; }
    Scope(const Scope&) = delete;

private:
    Counters* previous_;
};

namespace detail {

using clock = std::chrono::steady_clock;

/// Dispatch begin/end pairs nest strictly on the calling thread, so a stack
/// of start times pairs them; the uint64_t Kokkos hands back carries the
/// kernel id assigned at begin.
inline thread_local std::vector<clock::time_point> dispatch_t0;
inline thread_local std::vector<clock::time_point> fence_t0;

/**
 * @brief Begin one paired host-dispatch timing record.
 * @param[in] name Required stable NUL-terminated kernel label supplied by Kokkos; copied on
 * first use.
 * @param[out] kID Required caller record, set to zero even without an active counter scope;
 * otherwise assigned the local kernel-label index.
 * @par Unnamed callback argument The middle uint32_t is Kokkos's device identifier; it is
 * ignored.
 * @post With an active scope, increments launch count and pushes a host-clock timestamp.
 * Requires strictly paired same-thread callbacks and exclusive counter access. First
 * labels/stack growth may allocate/throw; measures dispatch wall, not asynchronous device
 * execution.
 */
inline void begin_dispatch(const char* name, std::uint32_t /*devid*/, std::uint64_t* kID) {
    *kID = 0;
    if (!active)
        return;
    auto& g = *active;
    const auto it = g.label_id.find(name);
    std::uint64_t id;
    if (it == g.label_id.end()) {
        id = g.kernels.size();
        g.label_id.emplace(name, id);
        g.kernels.push_back({name, 0, 0.0});
    } else {
        id = it->second;
    }
    *kID = id;
    ++g.n_launch;
    dispatch_t0.push_back(clock::now());
}

/**
 * @brief Finish a paired host-side dispatch timing record.
 * @param[in] kID Kernel-label index supplied by begin_dispatch().
 * @post Pops the latest dispatch timestamp when an active scope/stack exists; valid indices
 * increment label count and elapsed seconds. Invalid indices still consume the timestamp. No-op
 * without active scope or a matching begin. No allocation; same-thread pairing required.
 */
inline void end_dispatch(std::uint64_t kID) {
    if (!active)
        return;
    auto& g = *active;
    if (dispatch_t0.empty())
        return;
    const double dt = std::chrono::duration<double>(clock::now() - dispatch_t0.back()).count();
    dispatch_t0.pop_back();
    if (kID < g.kernels.size()) {
        ++g.kernels[kID].n;
        g.kernels[kID].sec += dt;
    }
}

/**
 * @brief Start timing a Kokkos fence callback pair.
 * @param[out] handle Required output, always set to zero; pairing uses the thread-local stack.
 * @par Unnamed callback arguments The label string and uint32_t device identifier are ignored
 * and not retained.
 * @post Pushes a clock timestamp only within an active Scope; stack growth may allocate/throw.
 * Must pair on the same thread before scope changes.
 */
inline void begin_fence(const char* /*name*/, std::uint32_t /*devid*/, std::uint64_t* handle) {
    *handle = 0;
    if (!active)
        return;
    fence_t0.push_back(clock::now());
}

/**
 * @brief Accumulate elapsed host wall for a paired Kokkos fence.
 * @par Unnamed callback argument The uint64_t handle is ignored; the newest same-thread fence
 * timestamp is used.
 * @post Within an active scope and nonempty timestamp stack, adds elapsed seconds, pops the
 * timestamp and increments fence count. Otherwise a no-op. No allocation; no fence is issued by
 * this callback.
 */
inline void end_fence(std::uint64_t /*handle*/) {
    if (!active)
        return;
    auto& g = *active;
    if (fence_t0.empty())
        return;
    g.sec_fence += std::chrono::duration<double>(clock::now() - fence_t0.back()).count();
    fence_t0.pop_back();
    ++g.n_fence;
}

/**
 * @brief Count a Kokkos deep-copy dispatch and its logical direction/bytes.
 * @param[in] dst Destination memory-space handle; a name starting with Host is classified as
 * host.
 * @param[in] src Source memory-space handle, classified by the same prefix rule.
 * @param[in] size Bytes transferred, accumulated without overflow checks.
 * @par Unnamed callback arguments Destination/source label strings and raw pointers are ignored;
 * pointers are neither dereferenced nor retained.
 * @post Active counters increment deep-copy count and H2D, D2H or same-space bytes. No
 * fence/timing/allocation occurs. Same counter object requires serialized callback access.
 */
inline void begin_deep_copy(Kokkos_Profiling_SpaceHandle dst, const char*, const void*,
                            Kokkos_Profiling_SpaceHandle src, const char*, const void*,
                            std::uint64_t size) {
    if (!active)
        return;
    auto& g = *active;
    ++g.n_deep_copy;
    const bool dst_host = std::strncmp(dst.name, "Host", 4) == 0;
    const bool src_host = std::strncmp(src.name, "Host", 4) == 0;
    if (src_host && !dst_host)
        g.bytes_h2d += size;
    else if (dst_host && !src_host)
        g.bytes_d2h += size;
    else
        g.bytes_same += size; // OMP backend: all
}

} // namespace detail

/**
 * @brief Registers the counting callbacks once per process. No-op unless OPENSWMM_PERF is set;
 * defers to an external Kokkos tool if one is loaded.
 *
 * @note First-call initialization is process-wide and synchronized by C++ local-static
 * initialization. Skipping because performance is disabled or an external tool environment
 * variable exists is permanent for later calls. Registers callbacks globally but collects only
 * inside an active Scope; does not own counter objects or initialize Kokkos.
 */
inline void install() {
    static const bool done = [] {
        if (!perf::enabled())
            return false;
        if (std::getenv("KOKKOS_TOOLS_LIBS") || std::getenv("KOKKOS_PROFILE_LIBRARY")) {
            std::fprintf(stderr, "[PERF-2D-KOKKOS] external Kokkos tool loaded; in-process "
                                 "counters disabled for this run.\n");
            return false;
        }
        namespace KTE = Kokkos::Tools::Experimental;
        KTE::set_begin_parallel_for_callback(detail::begin_dispatch);
        KTE::set_end_parallel_for_callback(detail::end_dispatch);
        KTE::set_begin_parallel_reduce_callback(detail::begin_dispatch);
        KTE::set_end_parallel_reduce_callback(detail::end_dispatch);
        KTE::set_begin_parallel_scan_callback(detail::begin_dispatch);
        KTE::set_end_parallel_scan_callback(detail::end_dispatch);
        KTE::set_begin_fence_callback(detail::begin_fence);
        KTE::set_end_fence_callback(detail::end_fence);
        KTE::set_begin_deep_copy_callback(detail::begin_deep_copy);
        return true;
    }();
    (void)done;
}

/**
 * @brief Replace active solver counters with empty defaults.
 * @post No-op without an active Scope; otherwise clears all counts, labels and kernel entries.
 * Does not clear pairing timestamp stacks or remove global callbacks.
 * @pre No dispatch/fence timing pair may be open across reset. Requires exclusive active-counter
 * access; prior entry references are invalidated.
 */
inline void reset() {
    if (!active)
        return;
    auto& g = *active;
    g = Counters{};
}

/**
 * @brief Count one managed solver advance when profiling is enabled.
 * @post Increments the active n_advance counter only when both a Scope and perf::enabled()
 * exist. No allocation, synchronization or solver advancement.
 */
inline void count_advance() {
    if (!active)
        return;
    auto& g = *active;
    if (perf::enabled())
        ++g.n_advance;
}

/**
 * @brief One [PERF-2D-KOKKOS] summary line plus one line per kernel label, sorted by host-side
 * dispatch wall descending. Same key=value contract as [PERF-FV] / [PERF-LOAD]; tests/benchmarks
 * scripts can scrape it.
 *
 * @note No-op without active counters, disabled profiling, or zero advances. Allocates a
 * temporary pointer vector to sort labels and may throw; counters remain unchanged. Output goes
 * to stderr and can interleave with other threads; label seconds measure host dispatch wall, not
 * device kernel time. Requires stable exclusive access while reading counters.
 */
inline void dump() {
    if (!active)
        return;
    auto& g = *active;
    if (!perf::enabled() || g.n_advance == 0)
        return;
    const double adv = static_cast<double>(g.n_advance);
    std::fprintf(stderr,
                 "[PERF-2D-KOKKOS] advances=%ld launches=%ld launches_per_advance=%.1f "
                 "fences=%ld fence_s=%.4f deep_copies=%ld "
                 "h2d_bytes=%llu d2h_bytes=%llu samespace_bytes=%llu "
                 "h2d_bytes_per_advance=%.0f d2h_bytes_per_advance=%.0f\n",
                 g.n_advance, g.n_launch, g.n_launch / adv, g.n_fence, g.sec_fence, g.n_deep_copy,
                 g.bytes_h2d, g.bytes_d2h, g.bytes_same, g.bytes_h2d / adv, g.bytes_d2h / adv);
    std::vector<const KernelEntry*> order;
    order.reserve(g.kernels.size());
    for (const auto& k : g.kernels)
        order.push_back(&k);
    std::sort(order.begin(), order.end(),
              [](const KernelEntry* a, const KernelEntry* b) { return a->sec > b->sec; });
    for (const KernelEntry* k : order)
        std::fprintf(stderr, "[PERF-2D-KOKKOS-KERNEL] name=%s n=%ld s=%.4f per_advance=%.1f\n",
                     k->label.c_str(), k->n, k->sec, k->n / adv);
}

} // namespace openswmm::twoD::gpu::kperf

#endif // OPENSWMM_ENGINE_2D_GPU_KOKKOS_PERF_COUNTERS_HPP
