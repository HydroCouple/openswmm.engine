// SPDX-License-Identifier: Apache-2.0

/**
 * @file      UebHeld.hpp
 * @brief     Held forcing and state for Utah Energy Balance snow integration.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "UebLumped.hpp"

namespace openswmm::surface::ueb {
// A snow solve can span many runoff/marcher steps. The end state is private
// until consumed: readers and checkpoints see only the elapsed share of the
// water/energy balance. Forcing must remain constant over the prepared window;
// drivers end a window at precipitation changes and at SNOW_STEP boundaries.
struct HeldState {
    LumpedState accepted, begin, end;
    LumpedFluxes flux;
    double duration = 0, elapsed = 0;
    /** @brief Test whether the prepared forcing window has unconsumed duration.
     * @return elapsed<duration exactly, without validating finite/nonnegative clock
     * invariants. Does not expose predicted end water as accepted or mutate history. */
    bool pending() const noexcept { return elapsed < duration; }
};
/** @brief Predict one full constant-forcing UEB window while retaining accepted inventory.
 * @param[in,out] state Consistent accepted lumped state with no pending window.
 * Stores begin/end/full-window flux, duration and zero elapsed on success; accepted
 * water/energy remains unchanged until consumed.
 * @param parameters Qualified UEB physical/numerical parameters.
 * @param site Qualified UEB site configuration.
 * @param forcing Held weather/precipitation over positive seconds in UEB point units;
 * drivers must close a window at forcing or SNOW_STEP changes.
 * @param curve Qualified depletion fractions, checked by advanceLumped().
 * @return Empty after preparation, otherwise pending-window/lumped-solve diagnostic.
 * Normal refusal retains accepted state and previous prediction. Copy/allocation
 * exceptions during preparation can leave prediction bookkeeping partly replaced;
 * caller owns private-state rollback. Serialize prepare/consume/close and snapshots. */
std::string prepareHeld(HeldState& state, const Parameters& parameters, const Site& site,
                        const Forcing& forcing, const std::array<double, 10>& curve);
/** @brief Publish only the requested elapsed share of an already solved snow window.
 * @param[in,out] state Consistent prepared window and current accepted partial state;
 * no concurrent forcing/model edits. Updates accepted inventory and elapsed only
 * on normal success, retaining private full predicted end/flux.
 * @param seconds Finite positive consumption no greater than remaining duration
 * except 1e-5-second calendar roundoff, which snaps exactly to the full endpoint.
 * @param curve Same qualified depletion curve used at preparation, checked again.
 * @param[out] output Assigned whole-element integrated depth/energy share on success;
 * retained on normal refusal. point fluxes remain the full prepared solve, not a
 * scaled new solve. Point steps/retries/diagnostics publish only on first consumption.
 * @return Empty on success, otherwise timing/curve/cover/conservation diagnostic.
 * Intermediate basin water/energy and selected point history fields interpolate by
 * window progress; coverage is recomputed and thermal history is truncated to 24 hours.
 * Last consumption adopts exact predicted end. Geometry energy is separately derived
 * from accepted-to-next representation; net physical energy/water scale by consumed
 * share. Allocation exceptions before publication propagate. No new UEB solve,
 * source-clock advancement or external receiver booking is performed here. */
std::string consumeHeld(HeldState& state, double seconds, const std::array<double, 10>& curve,
                        LumpedFluxes& output);
// Discard only the unused prediction, preserving the elapsed inventory. Used
// when a runtime forcing edit ends the old held interval early.
/** @brief Discard the remaining prediction's active clock while keeping elapsed inventory.
 * @param[in,out] state Sets duration=elapsed=0 only; accepted state is retained.
 * Begin/end/full flux data remain stored but are no longer pending, and a later
 * prepare replaces them. No water rollback, physical flux booking or allocation. */
void closeHeld(HeldState& state) noexcept;
} // namespace openswmm::surface::ueb
