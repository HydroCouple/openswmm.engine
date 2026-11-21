// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Caleb Buahin. All rights reserved.
#pragma once

#include "PerfTimers.hpp"
#include <array>
#include <atomic>
#include <mutex>
#include <optional>
#include <string_view>
#include <cstdint>
#include <unordered_set>
#include <vector>
#if defined(__APPLE__)
#include <dlfcn.h>
#endif

namespace openswmm::perf {

// Self times across the process models. Keep these outside solver/checkpoint
// state: trial copies and rollbacks must not copy or discard profiling evidence.
enum class ProcessPhase : std::size_t {
    surface_faces,
    surface_cells,
    surface_rebuild,
    surface_boundary,
    surface_exchange,
    surface_publish,
    surface_species,
    surface_species_cells,
    surface_reactions,
    gw_faces,
    gw_cells,
    gw_node_exchange,
    gw_dt,
    gw_infiltration,
    gw_species,
    gw_reactions,
    gw_sigma_map,
    heat_columns,
    surface_quality,
    runoff,
    groundwater,
    lid,
    lid_nodes,
    richards,
    mesh_snow,
    subcatch_snow,
    common_source,
    source_trial,
    outfall,
    source_results,
    engine_source_results,
    count
};
struct ProcessPhaseName {
    const char* group;
    const char* key;
};
inline constexpr std::array<ProcessPhaseName, std::size_t(ProcessPhase::count)> process_names{
    {{"2D", "faces"},
     {"2D", "cells"},
     {"2D", "rebuild"},
     {"2D", "boundary"},
     {"2D", "exchange"},
     {"2D", "publish"},
     {"2D", "species_faces"},
     {"2D", "species_cells"},
     {"2D", "reactions"},
     {"GW2D", "faces"},
     {"GW2D", "cells"},
     {"GW2D", "node_exchange"},
     {"GW2D", "refresh_dt"},
     {"GW2D", "infiltration"},
     {"GW2D", "species"},
     {"GW2D", "reactions"},
     {"GW2D", "sigma_map"},
     {"GW2D", "heat_columns"},
     {"2D", "quality"},
     {"HYD", "runoff"},
     {"HYD", "groundwater"},
     {"HYD", "lid"},
     {"HYD", "lid_nodes"},
     {"HYD", "richards"},
     {"HYD", "mesh_snow"},
     {"HYD", "subcatch_snow"},
     {"HYD", "common_source"},
     {"HYD", "source_trial"},
     {"HYD", "outfall"},
     {"HYD", "source_results"},
     {"HYD", "engine_source_results"}}};
inline std::array<std::atomic<double>, std::size_t(ProcessPhase::count)> process_seconds{};
inline std::array<std::atomic<unsigned long long>, std::size_t(ProcessPhase::count)>
    process_calls{};
inline std::array<std::atomic<unsigned long long>, std::size_t(ProcessPhase::count)>
    process_allocations{};
inline std::array<std::atomic<unsigned long long>, std::size_t(ProcessPhase::count)>
    process_allocated_bytes{};
// Process totals are qualified only for isolated engine lifetimes. An overlap
// invalidates the whole cohort, including the engine that remains open last.
inline std::atomic<bool> process_attribution_valid{true};
inline std::mutex process_models_mutex;
inline std::unordered_set<const void*> process_models;
inline bool process_tracking_failed = false; // protected by process_models_mutex

// Optional malloc interposer supplied by the benchmark tooling on macOS. The
// shipping engine never replaces malloc. Counts are inclusive, on the calling
// thread, and count successful allocation/reallocation requests, not live blocks.
using AllocationSnapshot = void (*)(std::uint64_t*, std::uint64_t*);
inline AllocationSnapshot allocation_snapshot() noexcept {
#if defined(__APPLE__)
    static const auto snapshot =
        reinterpret_cast<AllocationSnapshot>(dlsym(RTLD_DEFAULT, "openswmm_perf_malloc_snapshot"));
    return snapshot;
#else
    return nullptr;
#endif
}

enum class ProcessCounter : std::size_t {
    sigma_evaluations,
    sigma_firings,
    sigma_le_1,
    sigma_le_8,
    sigma_le_32,
    sigma_le_64,
    sigma_gt_64,
    sigma_map_refreshes,
    water_trial_copies,
    source_stage_copies,
    rk2_snapshots,
    mesh_snow_copies,
    count
};
inline constexpr std::array<const char*, std::size_t(ProcessCounter::count)> process_counter_names{
    {"sigma.evaluate", "sigma.firings", "sigma.hist.le1", "sigma.hist.2_8", "sigma.hist.9_32",
     "sigma.hist.33_64", "sigma.hist.gt64", "sigma_map.refresh", "copy.water_trial",
     "copy.source_stage", "copy.rk2_snapshot", "copy.mesh_snow"}};
inline std::array<std::atomic<unsigned long long>, std::size_t(ProcessCounter::count)>
    process_counts{};

inline void count(ProcessCounter counter, unsigned long long n = 1) noexcept {
    if (enabled() && process_attribution_valid.load(std::memory_order_relaxed))
        process_counts[std::size_t(counter)].fetch_add(n, std::memory_order_relaxed);
}
inline unsigned long long value(ProcessCounter counter) noexcept {
    return process_counts[std::size_t(counter)].load(std::memory_order_relaxed);
}

// Totals of scopes entered once per face or cell inside a worksharing loop. Each thread
// books into its own cache-line-aligned block, so the hot loop never writes a counter another
// thread writes. The blocks are folded into the process totals when a thread ends and before
// the totals are reported or cleared.
struct alignas(64) ThreadProcessTotals {
    std::array<std::atomic<double>, std::size_t(ProcessPhase::count)> seconds{};
    std::array<std::atomic<unsigned long long>, std::size_t(ProcessPhase::count)> calls{};
    bool registered = false;
    ThreadProcessTotals() noexcept;
    ~ThreadProcessTotals() noexcept;
    ThreadProcessTotals(const ThreadProcessTotals&) = delete;
    ThreadProcessTotals& operator=(const ThreadProcessTotals&) = delete;
    /// Move this block's totals into the process totals. The owning thread must not be inside
    /// a scope.
    void fold() noexcept {
        for (std::size_t i = 0; i < seconds.size(); ++i) {
            const double sec = seconds[i].exchange(0.0, std::memory_order_relaxed);
            const auto n = calls[i].exchange(0, std::memory_order_relaxed);
            if (n == 0)
                continue;
            auto& total = process_seconds[i];
            double old = total.load(std::memory_order_relaxed);
            while (!total.compare_exchange_weak(old, old + sec, std::memory_order_relaxed)) {
            }
            process_calls[i].fetch_add(n, std::memory_order_relaxed);
        }
    }
};
// The registry is never destroyed: a thread's block can be destroyed after the statics of this
// library at process exit, and must still find its mutex.
struct ThreadProcessRegistry {
    std::mutex mutex;
    std::vector<ThreadProcessTotals*> blocks;
};
inline ThreadProcessRegistry& thread_process_registry() {
    static auto* registry = new ThreadProcessRegistry;
    return *registry;
}
inline ThreadProcessTotals::ThreadProcessTotals() noexcept {
    try {
        auto& registry = thread_process_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        registry.blocks.push_back(this);
        registered = true;
    } catch (...) {
        // Unregistered blocks are not used: their scopes book into the process totals directly.
    }
}
inline ThreadProcessTotals::~ThreadProcessTotals() noexcept {
    if (!registered)
        return;
    auto& registry = thread_process_registry();
    std::lock_guard<std::mutex> lock(registry.mutex);
    fold();
    for (auto& block : registry.blocks)
        if (block == this) {
            block = registry.blocks.back();
            registry.blocks.pop_back();
            break;
        }
}
/// Fold every thread's per-item totals into the process totals. Call with no active scopes or
/// worksharing regions.
inline void fold_thread_process_totals() noexcept {
    try {
        auto& registry = thread_process_registry();
        std::lock_guard<std::mutex> lock(registry.mutex);
        for (auto* block : registry.blocks)
            block->fold();
    } catch (...) {
        // No registry could be made, so no block was ever registered.
    }
}

// GatedTimer preserves nested_wall across module boundaries. Each scope books
// into private storage before merging, including scopes inside OMP face loops.
// With profiling disabled there are no clock reads, allocations or atomic writes.
class ProcessTimer {
    double seconds_ = 0;
    ProcessPhase phase_;
    bool per_item_ = false;
    std::optional<GatedTimer> timer_;
    AllocationSnapshot allocations_ = nullptr;
    std::uint64_t allocations_begin_ = 0, bytes_begin_ = 0;

public:
    explicit ProcessTimer(ProcessPhase phase) noexcept : phase_(phase) {
        if (enabled() && process_attribution_valid.load(std::memory_order_relaxed)) {
            allocations_ = allocation_snapshot();
            if (allocations_)
                allocations_(&allocations_begin_, &bytes_begin_);
            timer_.emplace(seconds_);
        }
    }
    /// Tag for a scope entered once per face or cell inside a worksharing loop: its time and
    /// call are booked into the calling thread's own block (see ThreadProcessTotals).
    struct PerItem {};
    ProcessTimer(ProcessPhase phase, PerItem) noexcept : ProcessTimer(phase) { per_item_ = true; }
    void stop() noexcept {
        if (!timer_)
            return;
        timer_.reset();
        if (!process_attribution_valid.load(std::memory_order_relaxed))
            return;
        if (allocations_) {
            std::uint64_t calls = 0, bytes = 0;
            allocations_(&calls, &bytes);
            process_allocations[std::size_t(phase_)].fetch_add(calls - allocations_begin_,
                                                               std::memory_order_relaxed);
            process_allocated_bytes[std::size_t(phase_)].fetch_add(bytes - bytes_begin_,
                                                                   std::memory_order_relaxed);
        }
        if (per_item_) {
            thread_local ThreadProcessTotals mine;
            if (mine.registered) {
                // Single writer: a plain load and store, not a locked read-modify-write.
                auto& sec = mine.seconds[std::size_t(phase_)];
                auto& n = mine.calls[std::size_t(phase_)];
                sec.store(sec.load(std::memory_order_relaxed) + seconds_,
                          std::memory_order_relaxed);
                n.store(n.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
                return;
            }
        }
        auto& total = process_seconds[std::size_t(phase_)];
        double old = total.load(std::memory_order_relaxed);
        while (!total.compare_exchange_weak(old, old + seconds_, std::memory_order_relaxed)) {
        }
        process_calls[std::size_t(phase_)].fetch_add(1, std::memory_order_relaxed);
    }
    ~ProcessTimer() noexcept { stop(); }
    ProcessTimer(const ProcessTimer&) = delete;
    ProcessTimer& operator=(const ProcessTimer&) = delete;
};

// A histogram entry describes one SIGMA cell firing, including the final capture
// sweep. Failed firings are counted as attempted work as well.
struct SigmaEvaluations {
    unsigned long long evaluations = 0;
    void evaluate() noexcept {
        if (enabled())
            ++evaluations;
    }
    ~SigmaEvaluations() noexcept {
        if (!enabled())
            return;
        count(ProcessCounter::sigma_firings);
        count(ProcessCounter::sigma_evaluations, evaluations);
        count(evaluations <= 1    ? ProcessCounter::sigma_le_1
              : evaluations <= 8  ? ProcessCounter::sigma_le_8
              : evaluations <= 32 ? ProcessCounter::sigma_le_32
              : evaluations <= 64 ? ProcessCounter::sigma_le_64
                                  : ProcessCounter::sigma_gt_64);
    }
};

// Called between simulations, with no active scopes/worksharing regions.
inline void clear_process_totals() noexcept {
    fold_thread_process_totals();
    for (auto& seconds : process_seconds)
        seconds.store(0, std::memory_order_relaxed);
    for (auto& calls : process_calls)
        calls.store(0, std::memory_order_relaxed);
    for (auto& calls : process_allocations)
        calls.store(0, std::memory_order_relaxed);
    for (auto& bytes : process_allocated_bytes)
        bytes.store(0, std::memory_order_relaxed);
    for (auto& count : process_counts)
        count.store(0, std::memory_order_relaxed);
}
// Standalone kernel benchmarks/tests call this only between complete models.
inline void reset_process() noexcept {
    std::lock_guard<std::mutex> lock(process_models_mutex);
    if (!process_models.empty() || process_tracking_failed) {
        process_attribution_valid.store(false, std::memory_order_relaxed);
        return;
    }
    clear_process_totals();
    process_attribution_valid.store(true, std::memory_order_relaxed);
}
inline void begin_process_model(const void* model) noexcept {
    if (!enabled())
        return;
    try {
        std::lock_guard<std::mutex> lock(process_models_mutex);
        if (!process_models.insert(model).second)
            return;
        if (process_models.size() == 1 && !process_tracking_failed) {
            clear_process_totals();
            process_attribution_valid.store(true, std::memory_order_relaxed);
        } else {
            process_attribution_valid.store(false, std::memory_order_relaxed);
        }
    } catch (...) {
        // Optional instrumentation must not turn an allocation failure into
        // failure of the numerical run. No later run may bless these totals.
        std::lock_guard<std::mutex> lock(process_models_mutex);
        process_tracking_failed = true;
        process_attribution_valid.store(false, std::memory_order_relaxed);
        std::fprintf(stderr, "[PERF-PROCESS-INVALID] reason=tracking_failed\n");
    }
}
inline void dump_process() noexcept {
    if (!process_attribution_valid.load(std::memory_order_relaxed)) {
        std::fprintf(stderr, "[PERF-PROCESS-INVALID] reason=overlapping_engine_lifetimes\n");
        return;
    }
    fold_thread_process_totals();
    for (const char* group : {"2D", "GW2D", "HYD"}) {
        bool any = false;
        for (std::size_t i = 0; i < process_names.size(); ++i)
            if (std::string_view(process_names[i].group) == group && process_calls[i].load())
                any = true;
        if (!any)
            continue;
        std::fprintf(stderr, "[PERF-%s]", group);
        for (std::size_t i = 0; i < process_names.size(); ++i) {
            if (std::string_view(process_names[i].group) != group)
                continue;
            std::fprintf(stderr, " %s=%.9f n.%s=%llu", process_names[i].key,
                         process_seconds[i].load(), process_names[i].key, process_calls[i].load());
        }
        std::fprintf(stderr, "\n");
    }
    bool any = false;
    for (const auto& count : process_counts)
        if (count.load())
            any = true;
    if (any) {
        std::fprintf(stderr, "[PERF-PROCESS-COUNTS]");
        for (std::size_t i = 0; i < process_counts.size(); ++i)
            std::fprintf(stderr, " %s=%llu", process_counter_names[i], process_counts[i].load());
        std::fprintf(stderr, "\n");
    }
    if (allocation_snapshot()) {
        std::fprintf(stderr, "[PERF-ALLOC] inclusive=1 calling_thread=1");
        for (std::size_t i = 0; i < process_names.size(); ++i)
            if (process_calls[i].load())
                std::fprintf(stderr, " %s.%s.calls=%llu %s.%s.bytes=%llu", process_names[i].group,
                             process_names[i].key, process_allocations[i].load(),
                             process_names[i].group, process_names[i].key,
                             process_allocated_bytes[i].load());
        std::fprintf(stderr, "\n");
    }
}
inline void end_process_model(const void* model, bool report = true) noexcept {
    if (!enabled())
        return;
    std::lock_guard<std::mutex> lock(process_models_mutex);
    const auto found = process_models.find(model);
    if (found == process_models.end())
        return;
    if (report)
        dump_process();
    process_models.erase(found);
}

} // namespace openswmm::perf
