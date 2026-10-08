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
 * @file TimestepController.hpp
 * @brief Explicit next-timestep computation for the new engine.
 *
 * @details The TimestepController implements the explicit timestep formula:
 *
 * @code
 * dt_next = min(time_to_next_report, dt_cfl, dt_controls, dt_rdii)
 * @endcode
 *
 * ### Report clock (legacy parity)
 *
 * Report scheduling runs on the legacy-parity millisecond clock:
 * `ctx.elapsed_ms` mirrors legacy `NewRoutingTime` (accumulated as
 * `+= 1000.0 * dt`, routing.c:309) and `ctx.next_report_ms` mirrors legacy
 * `ReportTime` (seeded `1000*ReportStep`, advanced by `1000*ReportStep` per
 * period, swmm5.c:721/1044). A report period is due when
 * `elapsed_ms >= next_report_ms` (swmm5.c:1019). Periods dated before
 * REPORT_START are skipped by the writer but the grid still advances
 * (output.c:481).
 *
 * ### Key design decision (R18) — resolved for parity
 *
 * The original refactored design shortened dt so the clock lands exactly on
 * report boundaries (no interpolation). Legacy instead free-steps and
 * interpolates results to the report instant (output.c:650/682). That clamp
 * was REMOVED by the A2 reporting-parity work: compute_next never aligns to
 * the report grid, and postOutputSnapshot performs the legacy-identical
 * interpolation. The only step alignments that remain are legacy-real: the
 * total-duration clamp (swmm5.c:975-980, 1 ms floor) and the RULE_STEP
 * control grid (routing.c:196-204, via ctx.dt_controls_remaining set by
 * SWMMEngine::step from the absolute rule clock).
 *
 * ### Integration with IO thread (Phase 5)
 *
 * The main simulation loop uses TimestepController as follows:
 * @code{.cpp}
 * while (!done) {
 *     double dt_cfl = dynwave.compute_cfl_step(ctx);
 *     double dt_next = TimestepController::compute_next(ctx, dt_cfl);
 *
 *     hydrology::step(ctx, dt_next);
 *     hydraulics::step(ctx, dt_next);
 *     quality::step(ctx, dt_next);
 *
 *     TimestepController::advance(ctx, dt_next);
 *
 *     if (TimestepController::output_due(ctx)) {
 *         io_thread.post(SimulationSnapshot(ctx));
 *         TimestepController::reset_output_timer(ctx);
 *     }
 * }
 * @endcode
 *
 * @see Legacy reference: src/solver/routing.c — routing_execute()
 * @see tests/unit/test_timestep_controller.cpp
 * @ingroup engine_hydraulics
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TIMESTEP_CONTROLLER_HPP
#define OPENSWMM_ENGINE_TIMESTEP_CONTROLLER_HPP

#include <algorithm>
#include <cmath>

namespace openswmm {
    struct SimulationContext;
}

namespace openswmm::hydraulics {

/**
 * @brief Static utility class for explicit timestep computation.
 *
 * @details All methods are pure static functions that operate on a
 *          SimulationContext reference. This class has no state.
 *
 * @ingroup engine_hydraulics
 */
class TimestepController {
public:
    TimestepController() = delete;

    // -----------------------------------------------------------------------
    // Core timestep computation
    // -----------------------------------------------------------------------

    /**
     * @brief Compute the next simulation timestep.
     *
     * @details Result is the minimum of:
     *          - time to the next report instant (from ctx.next_report_ms;
     *            clamp scheduled for removal by the A2 reporting-parity work)
     *          - `dt_cfl` (CFL-limited hydraulic step from DynamicWave)
     *          - `ctx.dt_controls_remaining` (next control rule event)
     *          - `ctx.options.routing_step` (user-configured max step)
     *          - NOT less than `ctx.options.min_routing_step` (minimum floor)
     *
     * @param ctx     Simulation context (reads elapsed_ms/next_report_ms, options).
     * @param dt_cfl  CFL-limited timestep from the DynamicWave solver (seconds).
     *                Pass `ctx.options.routing_step` if not using dynamic wave.
     * @returns       dt_next in seconds, guaranteed > 0.
     *
     * @post Return value <= dt_cfl.
     * @post Return value >= ctx.options.min_routing_step.
     */
    static double compute_next(const SimulationContext& ctx, double dt_cfl) noexcept;

    // -----------------------------------------------------------------------
    // Timer management
    // -----------------------------------------------------------------------

    /**
     * @brief Advance internal timers by dt_taken seconds.
     *
     * @details Updates:
     *          - `ctx.current_time += dt_taken` (seconds)
     *          - `ctx.current_date` (decompose-recompose from current_time)
     *          - `ctx.old_elapsed_ms = ctx.elapsed_ms;
     *             ctx.elapsed_ms += 1000.0 * dt_taken` (legacy ms clock)
     *          - `ctx.dt_controls_remaining -= dt_taken`
     *
     * @param ctx      Simulation context (mutated).
     * @param dt_taken Timestep just completed, in seconds.
     */
    static void advance(SimulationContext& ctx, double dt_taken) noexcept;

    /**
     * @brief Advance the report grid to the next period.
     *
     * @details Legacy swmm5.c:1044 — `ReportTime += 1000 * ReportStep`.
     *          Call after handling a due report period (written or skipped
     *          by the REPORT_START gate).
     *
     * @param ctx  Simulation context (next_report_ms advanced one period).
     */
    static void reset_output_timer(SimulationContext& ctx) noexcept;

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    /**
     * @brief Returns true when the clock has reached the next report instant.
     *
     * @details Legacy swmm5.c:1019 — `NewRoutingTime >= ReportTime`, on the
     *          ms-accumulated clock (no epsilon; both sides are exact sums
     *          of the same ms quantities, as in legacy).
     *
     * @param ctx  Simulation context.
     * @returns    true if elapsed_ms >= next_report_ms.
     */
    static bool output_due(const SimulationContext& ctx) noexcept;

    /**
     * @brief Returns true when the simulation has reached or passed end_time.
     * @param ctx  Simulation context.
     */
    static bool simulation_complete(const SimulationContext& ctx) noexcept;

    // -----------------------------------------------------------------------
    // Constants
    // -----------------------------------------------------------------------


    /** @brief Seconds per day (exact). */
    static constexpr double SEC_PER_DAY = 86400.0;
};

} /* namespace openswmm::hydraulics */

#endif /* OPENSWMM_ENGINE_TIMESTEP_CONTROLLER_HPP */
