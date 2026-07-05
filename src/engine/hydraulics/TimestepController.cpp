/**
 * @file TimestepController.cpp
 * @brief Explicit next-timestep computation — implementation.
 *
 * @see TimestepController.hpp for interface documentation and design rationale.
 * @ingroup engine_hydraulics
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
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
    //         (matching legacy swmm5.c:975-979: nextRoutingTime > RoutingDuration
    //          → routingStep = (RoutingDuration - NewRoutingTime)/1000, min 1 ms).
    //         Uses the legacy-parity ms clock (ctx.elapsed_ms == NewRoutingTime)
    //         and the legacy-exact TotalDuration (no +0.5 rounding; see
    //         SimulationOptions::totalDurationMs()).
    const double total_msec = opt.totalDurationMs();
    if (ctx.elapsed_ms + 1000.0 * dt > total_msec) {
        dt = (total_msec - ctx.elapsed_ms) / 1000.0;
        dt = std::max(dt, 0.001);  // legacy floor: 1 msec
    }

    // Step 3: Align to control rule event boundary
    //         (matching legacy: if nextRoutingTime >= nextRuleTime, shorten)
    if (ctx.dt_controls_remaining > 0.0 && ctx.dt_controls_remaining < dt) {
        dt = ctx.dt_controls_remaining;
    }

    // (Parity) No report-boundary alignment: legacy free-steps and
    // interpolates results to the report instant (output.c:650/682);
    // postOutputSnapshot performs the identical interpolation.

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

    // Legacy-parity ms clock (routing.c:308-309):
    //   OldRoutingTime = NewRoutingTime;
    //   NewRoutingTime = NewRoutingTime + 1000.0 * routingStep;
    ctx.old_elapsed_ms = ctx.elapsed_ms;
    ctx.elapsed_ms     = ctx.elapsed_ms + 1000.0 * dt_taken;

    if (ctx.dt_controls_remaining > 0.0) {
        ctx.dt_controls_remaining -= dt_taken;
        if (ctx.dt_controls_remaining < 0.0) ctx.dt_controls_remaining = 0.0;
    }
}

// ============================================================================
// reset_output_timer
// ============================================================================

void TimestepController::reset_output_timer(SimulationContext& ctx) noexcept {
    // Legacy swmm5.c:1044 — ReportTime = ReportTime + 1000 * ReportStep.
    // The grid is anchored at SIM start and advances even for periods that
    // fall before REPORT_START (which are simply not written).
    ctx.next_report_ms = ctx.next_report_ms + 1000.0 * ctx.options.report_step;
}

// ============================================================================
// output_due
// ============================================================================

bool TimestepController::output_due(const SimulationContext& ctx) noexcept {
    // Legacy swmm5.c:1019 — if (NewRoutingTime >= ReportTime) save.
    return ctx.elapsed_ms >= ctx.next_report_ms;
}

// ============================================================================
// simulation_complete
// ============================================================================

bool TimestepController::simulation_complete(const SimulationContext& ctx) noexcept {
    // Match legacy swmm5.c: TotalDuration = floor((EndDate-StartDate)*SECperDAY
    //                                       + (EndTime-StartTime)*SECperDAY) * 1000
    // then: NewRoutingTime >= RoutingDuration (in milliseconds).
    //
    // PARITY: use the legacy-exact TotalDuration stored at parse time
    // (SimulationOptions::totalDurationMs()) — legacy FLOORS the separate
    // date+time second counts (no +0.5 rounding).
    return ctx.elapsed_ms >= ctx.options.totalDurationMs();
}

} /* namespace openswmm::hydraulics */
