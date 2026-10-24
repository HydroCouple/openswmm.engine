// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LidNodeAquifer.hpp
 * @brief     Water exchange between LID nodes and aquifers.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include <vector>
namespace openswmm {
struct SimulationContext;
class Router;
namespace twoD {
class SubsurfaceSolver;
struct SurfaceStateData;
struct RichardsBedAcceptance;
}
namespace lidnode {
// Production column preparation at the single global hydraulic cadence.
// Accepted gross receipts wait for each receiving groundwater cell's clock.
/** @brief Prepare global-cadence Richards LID nodes and retain completed aquifer receipts.
 * @param[in,out] ctx Compatible initialized node/storage/LID geometry, held forcing
 * and empty prior port/capture trials at current_time. Uses internal feet/ft3 at the
 * node boundary and converts kernel columns/receipts to SI. Changes real accepted
 * node state and later ordinary mixed-row preparation state.
 * @param[in,out] gw Active matching aquifer with resolved unique node-bed mappings
 * and an enabled receipt owner; gross downward/upward receipts are reserved/queued
 * for each receiving cell's later clock, not immediately netted into its water.
 * @param[in,out] surface Compatible shared surface owner, passed to acceptance;
 * queued preparation itself does not advance surface/aquifer physical water.
 * @param dt Finite positive interval seconds from ctx.current_time.
 * @param evaporation Finite nonnegative potential ft/s for node evaporation.
 * @throws std::runtime_error Missing receipt owner or acceptance/preparation refusal.
 * With no Mesh Richards nodes delegates to ordinary prepareStep. Water-only routing
 * is required (no pollutants, water age or configured reactions); heat additionally
 * requires an active ground thermal owner. On successful preparation route once,
 * then finishStep once. A later prepareStepAfterMesh exception can follow accepted
 * node/receipt publication; this facade is not a rollback boundary. Build with
 * OPENSWMM_HAS_2D and serialize all related model/solver operations. */
void prepareMeshRichardsStep(SimulationContext& ctx, twoD::SubsurfaceSolver& gw,
                             twoD::SurfaceStateData& surface, double dt, double evaporation);
// Internal water-only real LID-state bridge at an explicit closed owner interval.
// Uses current node geometry, pressure limits, forcing and clogging; no new UI
// parameters. Successful preparation requires one finishStep after routing ports.
// Production cross-tier 1D scheduling and joint restart remain gated.
/** @brief Settle a closed owner interval and publish corresponding real Richards node state.
 * @param[in,out] ctx Compatible initialized water-only LID-node context; each selected
 * node is an initialized STORAGE Mesh column with completed clock equal start, no
 * pending finish/capture/mobile/port trial, finite qualified geometry and forcing.
 * @param[in,out] gw Matching active aquifer with one resolved exchange bed per node
 * and sufficient paired directional reservations; thermal owner required for heat.
 * @param[in,out] surface Compatible surface state settled by the groundwater firing.
 * @param start Finite nonnegative elapsed simulation seconds, exact prior owner boundary.
 * @param end Finite elapsed seconds strictly after start, with representable end-start.
 * @param evaporation Finite nonnegative potential ft/s held over this interval.
 * @param nodes Nonempty unique valid node indices; copied/sorted internally. Caller
 * supplies the complete cohort required by the lower-level firing acceptance.
 * @return Acceptance reports/receipts or nonempty error. Trial aquifer/surface copies
 * and all real-node publication drafts qualify before ordinary publication. Normal
 * refusal retains real water states; exceptions before publication can propagate.
 * Successful return marks node mesh_pending_finish: route accepted ports and finish
 * exactly once. Does not advance the global context clock or route links itself.
 * No qualified ponding/flooding, pollutant/age/reaction or joint restart path here;
 * caller serializes stable context/solver ownership and retained thermal references. */
twoD::RichardsBedAcceptance
acceptRichardsNodesAtFiring(SimulationContext& ctx, twoD::SubsurfaceSolver& gw,
                            twoD::SurfaceStateData& surface, double start, double end,
                            double evaporation, const std::vector<int>& nodes);
// Same atomic real-state publication, replacing one actual due GW tier firing
// including its ordinary cells. Caller supplies all due Richards node owners.
/** @brief Replace one due groundwater tier firing with a paired Richards-node settlement.
 * @param[in,out] ctx Compatible water-only initialized storage/LID context with selected
 * owners closed at start and no unresolved capture/port/finish trials.
 * @param[in,out] gw Active matching tiered aquifer, qualified bed reservations and
 * thermal owner when heat is enabled. Includes ordinary due cells in that tier.
 * @param[in,out] surface Matching surface state used by the accepted firing.
 * @param tier Nonnegative due tier index; negative returns an error before delegation.
 * @param start Finite nonnegative exact accepted elapsed-second boundary.
 * @param end Finite elapsed-second endpoint strictly after start.
 * @param evaporation Finite nonnegative held potential ft/s.
 * @param nodes Nonempty unique valid Mesh Richards STORAGE owners, complete for the
 * requested tier. Uses the same qualification/publication and required subsequent
 * routing/finish contract as acceptRichardsNodesAtFiring().
 * @return Reports/receipts or error; normal refusal retains real candidate water
 * states. Exceptions outside draft conversion may propagate. Does not itself route
 * links or advance the global context clock; serialize all related operations. */
twoD::RichardsBedAcceptance acceptRichardsNodesForTier(SimulationContext& ctx,
                                                       twoD::SubsurfaceSolver& gw,
                                                       twoD::SurfaceStateData& surface, int tier,
                                                       double start, double end, double evaporation,
                                                       const std::vector<int>& nodes);
// Internal atomic fixed-forcing conduit window. Native DW and mixed LID rows
// advance at step_ends; the due GW tier settles once. The caller supplies all
// Mesh nodes in the 1D network on that tier. Other GW tiers retain their clocks.
// No live source/face drivers, structure callbacks, ponding/flooding or restart.
/** @brief Trial and publish a fixed-forcing native dynamic-wave/Richards tier window.
 * @param[in,out] ctx Initialized compatible water-only context at current_time==start,
 * DYNWAVE routing, conduit-only links and no enabled ponding. All global Mesh owners
 * must be included. Native link/LID state and elapsed/date clocks publish on success.
 * Live 2D IO/accessor handles are cleared in the private trial then restored unchanged.
 * @param[in,out] router Initialized native dynamic-wave router outside a checkpoint
 * session; its private solver/group window state publishes after accepted context.
 * @param[in,out] gw Matching active aquifer with the selected due tier/bed/thermal owners.
 * @param[in,out] surface Matching surface state; other tiers retain their clocks.
 * @param tier Nonnegative actual due groundwater tier.
 * @param start Finite nonnegative exact current elapsed-second boundary.
 * @param end Finite endpoint seconds strictly later than start.
 * @param step_ends Ordered native routing substep endpoints spanning the closed
 * window, as qualified by acceptRichardsBedsForTierAcrossSteps().
 * @param evaporation Finite nonnegative potential ft/s held across all substeps.
 * @param nodes Nonempty unique complete cohort of Mesh Richards STORAGE node owners;
 * omitted global Mesh nodes are refused even if another tier would otherwise retain time.
 * @return Acceptance/receipts, or error for validation, convergence, nonfinite state,
 * flooding, lower-level refusal or caught standard exception. Trials use private
 * context/aquifer/surface/router copies; publish uses nonthrowing moves/swaps only
 * after success. Does not install live source/face drivers, structure callbacks or
 * restart support; caller supplies stable fixed forcing and serializes publication. */
twoD::RichardsBedAcceptance
routeRichardsNodesForTierWindow(SimulationContext& ctx, Router& router, twoD::SubsurfaceSolver& gw,
                                twoD::SurfaceStateData& surface, int tier, double start, double end,
                                const std::vector<double>& step_ends, double evaporation,
                                const std::vector<int>& nodes);
}
}
