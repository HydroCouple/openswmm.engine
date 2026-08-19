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
 * @file WaterAgeLid.hpp
 * @brief Phase A4 — water age through the LID layer stack.
 *
 * @details Each layer of each LID unit is a complete-mix tank: what it holds
 *          ages by `dt`, what arrives mixes in by volume, and what leaves —
 *          percolation to the layer below, evaporation, exfiltration, the
 *          underdrain — leaves at the layer's own age. **Only the inflow is
 *          needed**, which is the whole reason this phase does not repeat
 *          A3's defect.
 *
 * @par Where the inflow rates come from
 *      Every `batch*Flux` routine already computed each layer's inflow as a
 *      local (`soil_infil`, `soil_perc`, `pavePerc`, `storageInflow`, ...);
 *      A4 publishes them as `LIDGroupSoA::in_surf/in_pave/in_soil/in_stor`,
 *      written after every clamp. They are **not** `f_old_*`: `f_old_surf`
 *      is the Modified Puls net `dx/dt` of the surface layer and
 *      `f_old_soil/stor/pave` are allocated and never written. A net rate of
 *      change is precisely what made A3 report elapsed time rather than age,
 *      so the survey claim that those four fields were inter-layer fluxes
 *      does not hold and is not what this uses.
 *
 * @par Which layer feeds which
 *      Resolved from an explicit per-TYPE table rather than inferred, because
 *      the stacks genuinely differ and the solver itself is written one
 *      routine per type. The single conditional is permeable pavement, whose
 *      storage receives from the soil when there is one and from the pavement
 *      when there is not — mirroring the solver's own `storageInflow_local`.
 *
 * @par Not in this phase
 *      Hotstart persistence. LID layer depths are not in the hot start file
 *      (no `surf_depth`/`soil_moist`/`stor_depth`/`pave_depth` anywhere in
 *      `HotStartManager.cpp`), so a restored age would be a mean over a
 *      volume that was not restored — A3's reasoning, verified rather than
 *      inherited.
 *
 *      Surface overflow returns to the subcatchment, and its age is the
 *      surface layer's, but it is not fed back into A3's subarea ages here.
 *
 * @see plans/transport/WATER_AGE_TRACKING_PLAN.md §7 A4
 * @see plans/transport/A4_IMPLEMENTATION_BRIEF_2026-08-17.md
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_WATERAGELID_HPP
#define OPENSWMM_ENGINE_TRANSPORT_WATERAGELID_HPP

namespace openswmm {
struct SimulationContext;
namespace lid { class LIDSolver; }
}  // namespace openswmm

namespace openswmm::transport {

/// Size the per-layer block against the model's LID units and seed the
/// layers that already hold water at t = 0. Idempotent; safe to call once
/// the LID manager has built its groups.
void initLidLayerAge(SimulationContext& ctx, const lid::LIDSolver& solver);

/// Flow-weighted age of the water arriving at each LID unit this step,
/// stored into `ctx.lid_layer_state.inflow_value`. Called where
/// `LIDGroupSoA::inflow` is assembled, because that is the only place the
/// contributing rates (rain, captured impervious and pervious runoff,
/// whole-subcatchment run-on) exist together.
void setLidInflowAge(SimulationContext& ctx, int type_index, int unit,
                     int subcatch, double rain_rate, double q_imperv,
                     double q_perv, double q_runon, double lid_area);

/// Advance the per-layer ages one runoff step, after `LIDSolver::execute`.
void routeLidLayerAge(SimulationContext& ctx, const lid::LIDSolver& solver,
                      double dt);

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_WATERAGELID_HPP
