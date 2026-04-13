/**
 * @file TimestepController.cpp
 * @brief Explicit next-timestep computation — implementation.
 *
 * @see TimestepController.hpp for interface documentation and design rationale.
 * @ingroup engine_hydraulics
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "TimestepController.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/DateTime.hpp"
#include <cmath>

#include <algorithm>
#include <cmath>

namespace openswmm::hydraulics {

// ============================================================================
// compute_next
// ============================================================================

double TimestepController::compute_next(
    const SimulationContext& ctx,
    double                   dt_cfl
) noexcept {
    const SimulationOptions& opt = ctx.options;
    const double min_step = opt.min_routing_step;

    // ================================================================
    // Matching legacy execRouting() + routing_getRoutingStep() exactly:
    //   1. Get CFL-limited step (clamped to MinRouteStep by DW solver)
    //   2. Adjust to not exceed simulation duration
    //   3. Adjust to land on control rule event boundary
    //   4. Save results when NewRoutingTime >= ReportTime (no alignment)
    // ================================================================

    // Step 1: CFL-limited step, clamped to user's fixed routing step
    double dt = std::min(opt.routing_step, dt_cfl);

    // Enforce minimum floor (from [OPTIONS] MINIMUM_STEP)
    dt = std::max(dt, min_step);

    // Step 2: Adjust so total duration is not exceeded
    //         (matching legacy: routingStep = (RoutingDuration - NewRoutingTime) / 1000)
    //         Use millisecond-based arithmetic to match legacy exactly.
    const double total_sec = std::floor(
        (opt.end_date - opt.start_date) * SEC_PER_DAY + 0.5);
    const double sim_remaining = total_sec - ctx.current_time;
    if (sim_remaining > 0.0 && dt > sim_remaining) {
        dt = std::max(sim_remaining, 0.001);  // legacy floor: 1 msec
    }

    // Step 3: Align to control rule event boundary
    //         (matching legacy: if nextRoutingTime >= nextRuleTime, shorten)
    if (ctx.dt_controls_remaining > 0.0 && ctx.dt_controls_remaining < dt) {
        dt = ctx.dt_controls_remaining;
    }

    // Step 4: Align to output boundary if it falls within this step.
    //         Shortens the step to land exactly on the report time, avoiding
    //         interpolation of output values. Only applied when the remaining
    //         time to the next output is at least min_step (prevents tiny steps).
    if (ctx.dt_output_remaining > 0.0 &&
        ctx.dt_output_remaining >= min_step &&
        ctx.dt_output_remaining < dt) {
        dt = ctx.dt_output_remaining;
    }

    // Final safety: never allow zero or negative
    if (dt <= 0.0) dt = min_step;

    return dt;
}

// ============================================================================
// advance
// ============================================================================

void TimestepController::advance(
    SimulationContext& ctx,
    double             dt_taken
) noexcept {
    ctx.current_time += dt_taken;
    // Use decompose-recompose arithmetic (matching legacy getDateTime)
    // to avoid floating-point divergence from simple division.
    ctx.current_date  = datetime::addSeconds(ctx.options.start_date, ctx.current_time);

    if (ctx.dt_output_remaining > 0.0) {
        ctx.dt_output_remaining -= dt_taken;
        if (ctx.dt_output_remaining < 0.0) ctx.dt_output_remaining = 0.0;
    }

    if (ctx.dt_controls_remaining > 0.0) {
        ctx.dt_controls_remaining -= dt_taken;
        if (ctx.dt_controls_remaining < 0.0) ctx.dt_controls_remaining = 0.0;
    }
}

// ============================================================================
// reset_output_timer
// ============================================================================

void TimestepController::reset_output_timer(SimulationContext& ctx) noexcept {
    ctx.dt_output_remaining = ctx.options.report_step;
}

// ============================================================================
// output_due
// ============================================================================

bool TimestepController::output_due(const SimulationContext& ctx) noexcept {
    return ctx.dt_output_remaining <= OUTPUT_EPSILON;
}

// ============================================================================
// simulation_complete
// ============================================================================

bool TimestepController::simulation_complete(const SimulationContext& ctx) noexcept {
    // Match legacy swmm5.c: TotalDuration = floor((EndDate-StartDate)*SECperDAY
    //                                       + (EndTime-StartTime)*SECperDAY) * 1000
    // then: NewRoutingTime >= RoutingDuration (in milliseconds).
    //
    // Use floor() to get an exact integer second count, then compare in
    // milliseconds to avoid floating-point drift in OADate arithmetic.
    double total_sec = std::floor(
        (ctx.options.end_date - ctx.options.start_date) * SEC_PER_DAY + 0.5);
    double current_msec = ctx.current_time * 1000.0;
    double total_msec   = total_sec * 1000.0;
    return current_msec >= total_msec;
}

} /* namespace openswmm::hydraulics */
