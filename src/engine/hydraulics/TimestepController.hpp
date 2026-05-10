/**
 * @file TimestepController.hpp
 * @brief Explicit next-timestep computation for the new engine.
 *
 * @details The TimestepController implements the explicit timestep formula:
 *
 * @code
 * dt_next = min(dt_output_remaining, dt_cfl, dt_controls, dt_rdii)
 * @endcode
 *
 * ### Key design decision (R18)
 *
 * The legacy SWMM engine (src/solver/routing.c, routing_execute()) uses a
 * fixed routing step and then *interpolates* simulation state to hit output
 * time boundaries. This means the output values are not computed at exact
 * output times — they are interpolated.
 *
 * The new engine instead:
 * 1. Computes dt_next so the simulation clock lands exactly on the next
 *    output boundary.
 * 2. Takes the computed step (no interpolation needed).
 * 3. Posts the exact-time snapshot to the IO thread.
 *
 * This is more physically correct (no interpolation artifacts) and also
 * simplifies the output writing code (no interpolation logic needed).
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
 * @license  MIT License
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
     *          - `ctx.dt_output_remaining` (time to next output boundary)
     *          - `dt_cfl` (CFL-limited hydraulic step from DynamicWave)
     *          - `ctx.dt_controls_remaining` (next control rule event)
     *          - `ctx.options.routing_step` (user-configured max step)
     *          - NOT less than `ctx.options.min_routing_step` (minimum floor)
     *
     * @param ctx     Simulation context (reads dt_output_remaining, options).
     * @param dt_cfl  CFL-limited timestep from the DynamicWave solver (seconds).
     *                Pass `ctx.options.routing_step` if not using dynamic wave.
     * @returns       dt_next in seconds, guaranteed > 0.
     *
     * @pre  ctx.dt_output_remaining > 0.
     * @post Return value <= dt_cfl.
     * @post Return value <= ctx.dt_output_remaining.
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
     *          - `ctx.current_time += dt_taken / 86400.0` (decimal days)
     *          - `ctx.dt_output_remaining -= dt_taken`
     *          - `ctx.dt_controls_remaining -= dt_taken`
     *
     * @param ctx      Simulation context (mutated).
     * @param dt_taken Timestep just completed, in seconds.
     */
    static void advance(SimulationContext& ctx, double dt_taken) noexcept;

    /**
     * @brief Reset the output countdown timer to the full report step.
     *
     * @details Call this immediately after posting a snapshot to the IO thread.
     *
     * @param ctx  Simulation context (dt_output_remaining reset to report_step).
     */
    static void reset_output_timer(SimulationContext& ctx) noexcept;

    // -----------------------------------------------------------------------
    // State queries
    // -----------------------------------------------------------------------

    /**
     * @brief Returns true when simulation clock has reached an output boundary.
     *
     * @details A small epsilon (1e-6 seconds) is used for floating-point comparison
     *          to avoid missing an output due to floating-point rounding.
     *
     * @param ctx  Simulation context.
     * @returns    true if dt_output_remaining <= EPSILON.
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

    /** @brief Epsilon for output-time floating-point comparison (seconds). */
    static constexpr double OUTPUT_EPSILON = 1.0e-6;

    /** @brief Seconds per day (exact). */
    static constexpr double SEC_PER_DAY = 86400.0;
};

} /* namespace openswmm::hydraulics */

#endif /* OPENSWMM_ENGINE_TIMESTEP_CONTROLLER_HPP */
