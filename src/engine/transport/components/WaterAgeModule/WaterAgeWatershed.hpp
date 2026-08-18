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
 * @file WaterAgeWatershed.hpp
 * @brief Phase A3 — water age on subcatchment surfaces.
 *
 * @details Each subcatchment carries THREE ages, one per ponded subarea
 *          (`IMPERV0`, `IMPERV1`, `PERV`), mirroring the RunoffSolver's own
 *          `depth_imperv0/1/perv`. Per-subarea rather than lumped is a user
 *          decision (2026-08-17): impervious water is systematically younger
 *          than pervious, and the depths are already separate, so lumping
 *          would discard information the state already has.
 *
 *          Per step, per subarea: **age +dt, then mix with what arrived**,
 *          volume-weighted — the same order the node/link mirror uses.
 *          Water leaving as runoff carries the volume-weighted mean of the
 *          contributing subareas, and THAT is what reaches the outlet node
 *          and what run-on hands to a downstream subcatchment.
 *
 * @par Why the mixing volume is the GROSS inflow
 *      A subarea shedding as fast as it fills has `v_new == v_old`, which
 *      is the ordinary state of an impervious surface during a storm. A
 *      mixing volume taken as the NET gain, `max(0, v_new − v_old)`, is
 *      therefore zero exactly when rain is pouring through the surface, and
 *      the age stops being a residence time and becomes the elapsed time
 *      since the surface first wetted. Measured on a 100 % impervious,
 *      zero-depression deck under sustained 2 in/h rain (V = 5104.5 ft³,
 *      Q = 10.08 cfs, so V/Q = 0.14062 h): a net-gain mixing volume reports
 *      **0.88592 h**, 6.3x too old.
 *
 *      Only the INFLOW is needed to avoid this. Complete-mix means outflow
 *      leaves at the subarea's own age, so the outflow never enters the
 *      update — and the inflow the solver applied is already published:
 *      `ctx.subcatches.rainfall` (ft/s, written by `Runoff.cpp:295`) plus
 *      run-on, which the solver spreads over the whole area as extra
 *      precipitation (`Runoff.cpp:331-333`). So
 *      `v_in = (rain + runon/area) · frac · area · dt`, and the same deck
 *      returns **0.14022 h** against the analytic 0.14062 h — 0.3 %.
 *
 * @par What this still approximates
 *      With a snowpack active the solver substitutes `snow_net_imperv/perv`
 *      for rainfall per subarea (`Runoff.cpp:543-548`), and inter-subarea
 *      routing (`RouteTo IMPERV`/`PERV`) moves water between subareas
 *      without appearing in either term. Both leave the arriving volume
 *      mis-stated; neither is exercised by a `RouteTo OUTLET`, snow-free
 *      deck. Runoff also leaves at the **stored-volume** weighted mean of
 *      the subareas rather than at their outflow-weighted mean, because
 *      per-subarea outflow is genuinely not published — a depression-storage
 *      subarea therefore counts toward the departing age in proportion to
 *      what it holds, not what it sheds.
 *
 * @par Not in this phase
 *      Hotstart persistence (user decision: defer). The subarea depths are
 *      not in the hotstart either, so an age restored over a volume that
 *      was not restored would be a mean of nothing.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §3, §7 A3
 * @see plans/transport/A3_SCOPING_2026-08-17.md
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_WATERSHED_HPP
#define OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_WATERSHED_HPP

namespace openswmm {
struct SimulationContext;
namespace runoff { struct RunoffSoA; }
}

namespace openswmm::transport {

/// One runoff-step update of the subarea ages and the published runoff age.
/// Call immediately AFTER the runoff solver has stepped, so the depths are
/// this step's. No-op when WATER_AGE is off.
void routeSubcatchmentAge(SimulationContext& ctx,
                          const runoff::RunoffSoA& soa, double dt);

/// Accumulate run-on age-volume from a donor subcatchment to its receiver.
/// Called from the run-on assembly beside the flow it mirrors.
void addRunonAge(SimulationContext& ctx, int donor_sc, int receiver_sc,
                 double q);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_WATER_AGE_WATERSHED_HPP
