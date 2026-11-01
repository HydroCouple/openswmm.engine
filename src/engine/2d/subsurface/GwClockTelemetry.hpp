// SPDX-License-Identifier: Apache-2.0
// Copyright (c) 2026 Caleb Buahin. All rights reserved.
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <vector>

namespace openswmm::twoD {

enum class GwFiringOrigin { direct, macro, tail, rk2 };

// Phase-0 diagnostics only: never consulted by the solver or serialized into
// checkpoints/hotstarts. Value semantics follow solver trial copies; these are
// retained-object observations, unlike ProcessPerf's attempted-work timers.
// A fresh solver starts a new observation period; restoring into an existing
// solver does not rewind these counters. RK2 counts Euler stages, not steps.
struct GwClockTelemetry {
    using Count = std::uint64_t;
    struct Cell {
        Count firings = 0;
        double dt_g = 1e30, dt_u = 1e30;
    };
    struct Closure {
        Count firings = 0, unbounded = 0, invalid_interval = 0;
        double elapsed = 0, max_elapsed = 0;
        // dt_stab / elapsed: <1, [1,2), [2,8), [8,32), [32,128),
        // [128,512), [512,2048), >=2048. Unbounded is separate.
        std::array<Count, 8> overfire{};
        // dt_g / dt_u: <1/16, [1/16,1/4), [1/4,1), [1,4), [4,16), >=16.
        std::array<Count, 6> zone_ratio{};
        Count g_unbounded = 0, u_unbounded = 0, both_unbounded = 0;
    };
    std::vector<Cell> cells;
    std::array<Closure, 3> closures{}; // CLOSED_FORM, ENSLAVED, SIGMA
    std::array<Count, 4> origins{};
    std::array<Count, 8> tier_batches{}; // shared ladder, cumulative across re-tiers
    Count rebuilds = 0;
    double start = std::numeric_limits<double>::infinity(), end = 0;

    void reset(int n, bool enabled) {
        *this = {};
        if (enabled)
            cells.resize(static_cast<std::size_t>(n));
    }
    bool active() const noexcept { return !cells.empty(); }
    static bool bounded(double dt) noexcept { return std::isfinite(dt) && dt < 1e29; }

    // Called on distinct cells by the existing parallel stability loop.
    void bounds(int cell, double dt_g, double dt_u) noexcept {
        if (!active()) return;
        cells[cell].dt_g = dt_g;
        cells[cell].dt_u = dt_u;
    }
    void rebuilt(const std::vector<uint8_t>& covered,
                 const std::vector<int8_t>& closure) noexcept {
        if (!active()) return;
        ++rebuilds;
        constexpr std::array<double, 5> edges{1./16, 1./4, 1, 4, 16};
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (!covered[i]) continue;
            auto& row = closures[closure[i]];
            const auto& cell = cells[i];
            const bool g = bounded(cell.dt_g), u = bounded(cell.dt_u);
            if (!g && !u) ++row.both_unbounded;
            else if (!g) ++row.g_unbounded;
            else if (!u) ++row.u_unbounded;
            else {
                const auto bin = std::upper_bound(edges.begin(), edges.end(), cell.dt_g / cell.dt_u);
                ++row.zone_ratio[bin - edges.begin()];
            }
        }
    }
    void fired(int cell, int closure, double time, double dt, double stable,
               GwFiringOrigin origin) noexcept {
        if (!active()) return;
        ++cells[cell].firings;
        auto& row = closures[closure];
        ++row.firings;
        ++origins[static_cast<std::size_t>(origin)];
        if (!(dt > 0) || !std::isfinite(dt) || !std::isfinite(time)) {
            ++row.invalid_interval;
            return;
        }
        start = std::min(start, time);
        end = std::max(end, time + dt);
        row.elapsed += dt;
        row.max_elapsed = std::max(row.max_elapsed, dt);
        if (!bounded(stable)) ++row.unbounded;
        else {
            constexpr std::array<double, 7> edges{1, 2, 8, 32, 128, 512, 2048};
            const auto bin = std::upper_bound(edges.begin(), edges.end(), stable / dt);
            ++row.overfire[bin - edges.begin()];
        }
    }
    void batch(int tier) noexcept {
        if (active() && tier >= 0 && tier < static_cast<int>(tier_batches.size()))
            ++tier_batches[tier];
    }
    void dump(const std::vector<uint8_t>& covered, bool per_cell = false) const noexcept {
        if (!active()) return;
        const double duration = std::max(0., end - start);
        Count covered_count = 0, total = 0, min_firings = std::numeric_limits<Count>::max(), max_firings = 0;
        for (std::size_t i = 0; i < cells.size(); ++i) {
            if (!covered[i]) continue;
            ++covered_count;
            total += cells[i].firings;
            min_firings = std::min(min_firings, cells[i].firings);
            max_firings = std::max(max_firings, cells[i].firings);
            if (per_cell)
                std::fprintf(stderr, "[GW-CLOCK-CELL] cell=%zu firings=%llu per_hour=%.17g\n", i,
                    static_cast<unsigned long long>(cells[i].firings),
                    duration > 0 ? cells[i].firings * 3600. / duration : 0.);
        }
        std::fprintf(stderr, "[GW-CLOCK] cells=%llu seconds=%.17g rebuilds=%llu firings=%llu"
                     " direct=%llu macro=%llu tail=%llu rk2=%llu min_per_hour=%.17g"
                     " mean_per_hour=%.17g max_per_hour=%.17g\n",
                     static_cast<unsigned long long>(covered_count), duration,
                     static_cast<unsigned long long>(rebuilds), static_cast<unsigned long long>(total),
                     static_cast<unsigned long long>(origins[0]), static_cast<unsigned long long>(origins[1]),
                     static_cast<unsigned long long>(origins[2]), static_cast<unsigned long long>(origins[3]),
                     duration > 0 && covered_count ? min_firings * 3600. / duration : 0.,
                     duration > 0 && covered_count ? total * 3600. / duration / covered_count : 0.,
                     duration > 0 ? max_firings * 3600. / duration : 0.);
        auto histogram = [](const auto& counts) {
            for (std::size_t i = 0; i < counts.size(); ++i)
                std::fprintf(stderr, "%s%llu", i ? "," : "", static_cast<unsigned long long>(counts[i]));
        };
        std::fprintf(stderr, "[GW-CLOCK-TIERS] batches=");
        histogram(tier_batches);
        std::fprintf(stderr, "\n");
        constexpr const char* names[]{"CLOSED_FORM", "ENSLAVED", "SIGMA"};
        for (std::size_t k = 0; k < closures.size(); ++k) {
            const auto& row = closures[k];
            std::fprintf(stderr, "[GW-CLOCK-CLOSURE] closure=%s firings=%llu mean_elapsed=%.17g"
                         " max_elapsed=%.17g unbounded=%llu invalid_interval=%llu overfire=",
                         names[k], static_cast<unsigned long long>(row.firings),
                         row.firings ? row.elapsed / row.firings : 0., row.max_elapsed,
                         static_cast<unsigned long long>(row.unbounded),
                         static_cast<unsigned long long>(row.invalid_interval));
            histogram(row.overfire);
            std::fprintf(stderr, " zone_ratio=");
            histogram(row.zone_ratio);
            std::fprintf(stderr, " g_unbounded=%llu u_unbounded=%llu both_unbounded=%llu\n",
                         static_cast<unsigned long long>(row.g_unbounded),
                         static_cast<unsigned long long>(row.u_unbounded),
                         static_cast<unsigned long long>(row.both_unbounded));
        }
    }
};

} // namespace openswmm::twoD
