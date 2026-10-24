// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LidNodeRichardsState.hpp
 * @brief     Persistent Richards-equation state associated with LID nodes.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#pragma once
#include "LidNode.hpp"

namespace openswmm::lidnode::detail {
// Shared geometry/material conversion and private publication draft. All node
// geometry stays in canonical feet; kernel columns and reports are SI.
/**
 * @brief Convert one initialized native node stack to an owned SI Richards column.
 *
 * Uses exact ft-to-SI constants, applies nonnegative monthly conductivity factor, splits one ET
 * demand between surface or porous cells, and selects CURRENT explicit-bottom free drainage with
 * clogging when configured. Other bottom modes retain the default boundary until their external
 * owner qualifies it. No live water/forcing is consumed; copying can throw.
 *
 * @param[in] ctx Borrowed resolved context; no mutation or pointer retention.
 * @param[in] row Valid initialized Richards storage row with positive volumes/areas and coherent
 *     authored layers.
 * @param[in] dt Positive native seconds used for discrete lateral capture.
 * @param[in] evaporation Finite potential evaporation in native ft/s.
 * @param[in] continuous True supplies positive lateral inflow as an SI rate; false pre-adds its dt
 *     volume to surface water.
 * @return Owned column with geometry/pressure in metres, water in m3, Ks in m/s and seconds
 *     unchanged.
 */
richards::Column column(const SimulationContext& ctx, int row, double dt, double evaporation,
                        bool continuous = false);
struct RichardsNodeDraft {
    int row = -1, node = -1;
    LidNodeState state;
    double volume = 0, depth = 0, head = 0, full_volume = 0, lat_flow = 0;
};
/**
 * @brief Prepare an owned native publication from an accepted SI column/report.
 *
 * Copies quality-water baselines, remaps surface transfer index zero to mobile index -1, converts
 * volume receipts, refreshes native Richards state and derives surface hydraulic capacity/head.
 * Non-Mesh capture is positive lateral*dt; report bottom loss is clamped nonnegative. Throws on
 * conversion/pressure/allocation failure before publication.
 *
 * @param[in] ctx Borrowed resolved context; no mutation or pointer retention.
 * @param[in] row Valid initialized matching storage row.
 * @param[in] col Accepted SI column with water aligned to live cells.
 * @param[in] report Successful matching kernel report; transfer/ET/bottom volumes are SI m3.
 * @param[in] dt Positive seconds converting captured volume to temporarily removed lateral rate.
 * @param[in] completed Accepted simulation end time in seconds.
 * @param[in] mesh True uses report.inflow as continuous capture, marks pending Mesh finish and
 *     leaves bottom exchange to its paired owner.
 * @return Owned draft with native ft3 water/receipts, ft pressure/head/depth and copied runtime
 *     state; live context unchanged.
 */
RichardsNodeDraft draft(const SimulationContext& ctx, int row, const richards::Column& col,
                        const richards::Report& report, double dt, double completed,
                        bool mesh = false);
/**
 * @brief Publish one fully prepared native Richards state without throwing.
 *
 * Requires valid row/node indices and exclusive publication. Swaps LidNodeState and assigns
 * lateral/mobile/full/rpt-full volume and depth/head together. Performs no validation, allocation
 * or kernel solve; caller builds all relevant drafts before entering publication.
 *
 * @param[in,out] ctx Borrowed initialized native node-LID context with coherent
 *     storage/cell/material and link/node joins; serial lifecycle ownership is required.
 * @param[in,out] d Owned validated draft for this exact context row/node; its state is swapped with
 *     the prior live state.
 */
void publish(SimulationContext& ctx, RichardsNodeDraft&& d) noexcept;
/**
 * @brief Prepare a Mesh Richards state incorporating accepted hydraulic porous-port water.
 *
 * Rejects shape mismatch, nonfinite delta or nonzero surface port delta (surface water belongs to
 * the mobile owner). Does not verify interval/cohort itself, reset port deltas or publish the copy;
 * caller performs those lifecycle checks before finish. Throws without changing live state.
 *
 * @param[in] ctx Borrowed resolved context; no mutation or pointer retention.
 * @param[in] row Valid accepted Mesh storage row with matching cells/water/port arrays and valid
 *     materials.
 * @return Owned copied state with each porous port_delta added and pressure/moisture/inventory
 *     refreshed.
 */
LidNodeState routed(const SimulationContext& ctx, int row);
/**
 * @brief Require an exact pending accepted Mesh owner interval.
 *
 * Throws runtime_error unless Richards is enabled, pending finish is true, stored start/end exactly
 * match and end>start. Does not check richards_mesh_routed or perform epsilon matching;
 * port/routing callers add those distinct checks. No state changes.
 *
 * @param[in] s Initialized Richards state carrying pending-finish/start/completed receipts.
 * @param[in] start Finite nonnegative expected start seconds.
 * @param[in] end Finite later expected end seconds, with finite difference.
 */
void checkMeshInterval(const LidNodeState& s, double start, double end);
}
