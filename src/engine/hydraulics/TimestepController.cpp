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

    // User-configured maximum routing step
    double dt = opt.routing_step;

    // CFL-limited hydraulic step (from DynamicWave or caller-supplied value)
    dt = std::min(dt, dt_cfl);

    // Time to next output boundary
    if (ctx.dt_output_remaining > 0.0) {
        dt = std::min(dt, ctx.dt_output_remaining);
    }

    // Time to next control rule event
    if (ctx.dt_controls_remaining > 0.0) {
        dt = std::min(dt, ctx.dt_controls_remaining);
    }

    // Time remaining in the entire simulation
    const double sim_remaining =
        (opt.end_date - ctx.current_date) * SEC_PER_DAY;
    if (sim_remaining > 0.0) {
        dt = std::min(dt, sim_remaining);
    }

    // Enforce minimum floor
    dt = std::max(dt, opt.min_routing_step);

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
    ctx.current_date  = ctx.options.start_date + ctx.current_time / SEC_PER_DAY;

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
