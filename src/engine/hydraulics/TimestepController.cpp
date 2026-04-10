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
    const double sim_remaining =
        (opt.end_date - ctx.current_date) * SEC_PER_DAY;
    if (sim_remaining > 0.0 && dt > sim_remaining) {
        dt = std::max(sim_remaining, 0.001);  // legacy floor: 1 msec
    }

    // Step 3: Align to control rule event boundary
    //         (matching legacy: if nextRoutingTime >= nextRuleTime, shorten)
    if (ctx.dt_controls_remaining > 0.0 && ctx.dt_controls_remaining < dt) {
        dt = ctx.dt_controls_remaining;
    }

    // Step 4: Align to output boundary if it falls within this step.
    //         Legacy does NOT do this — it overshoots and saves at the
    //         overshot time. We align when possible (remaining >= min_step)
    //         to avoid interpolation; otherwise let the step overshoot.
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
    return ctx.current_date >= ctx.options.end_date - OUTPUT_EPSILON / SEC_PER_DAY;
}

} /* namespace openswmm::hydraulics */
