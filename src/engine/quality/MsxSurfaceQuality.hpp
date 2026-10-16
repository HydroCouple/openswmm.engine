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
 * @file MsxSurfaceQuality.hpp
 * @brief BW-MSX (2026-09-19) — buildup, washoff and street sweeping of the
 *        reactions component's (MSX) species on subcatchment surfaces.
 *
 * @details The pollutant surface-quality step (`SWMMEngine::stepSurfaceQuality`,
 *          the A7 sweeping block and `QualitySolver::addWetWeatherLoads`) is
 *          keyed by pollutant index throughout. This module runs the SAME
 *          arithmetic — buildup accrual through the inverse-days form
 *          (POW / EXP / SAT / EXT), washoff EXP / RC / EMC with legacy's unit
 *          pre-multiplies, the available-buildup cap, the "no buildup
 *          function" ledger branch, BMP removal, and the per-(subcatchment,
 *          land use) sweeping schedule — over a separate MSX-keyed store
 *          (`ReactionData::surface`, `MsxSurfaceData`), and delivers the
 *          washoff into `ReactionData::msx_ext_mass_in`, the rate the ARD
 *          engine, the legacy MSX dispatch and LARD already consume for
 *          external species loads.
 *
 *          What is deliberately NOT mirrored (pollutant-only mechanisms):
 *          co-pollutant fractions (D-A28), rain concentration / ponded-quality
 *          mixing and LID wet deposition (MSX species rain in clean, as on the
 *          2D surface), and any kinetics inside the dry store (D-A29).
 *
 *          Units: buildup is user mass (lbs / kg) per normalizer unit; the
 *          per-species factor `mcf` converts concentration-mass to user mass
 *          from the species' declared units (D-A27: `MG` → UCF(MASS),
 *          `UG` → UCF(MASS)/1000, anything else → 1).
 *
 *          Design: `plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md`
 *          §8.9.
 *
 * @ingroup engine_quality
 */

#ifndef OPENSWMM_ENGINE_QUALITY_MSX_SURFACE_QUALITY_HPP
#define OPENSWMM_ENGINE_QUALITY_MSX_SURFACE_QUALITY_HPP

#include <string>
#include <vector>

namespace openswmm {

struct SimulationContext;
struct SimulationOptions;

namespace msxsurf {

/// D-A27: concentration-mass → user-mass factor from a species' declared
/// units (`MG` → UCF(MASS), `UG` → UCF(MASS)/1000, anything else → 1). Shared
/// with the 2D cell store (S7).
double speciesUnitFactor(const std::string& units, const SimulationOptions& opts);

/// True once rows resolved to at least one MSX species with a land use.
bool active(const SimulationContext& ctx) noexcept;

/**
 * @brief Bind the by-name rows to (land use, species) — call after the
 *        reactions component has been applied and the name tables are final.
 * @return Warnings (unresolvable species / land use / subcatchment names,
 *         WALL species). Never fatal: a typo in a row is a warning here,
 *         exactly as the pollutant handlers silently skip theirs.
 */
std::vector<std::string> resolve(SimulationContext& ctx);

/// Re-derive `bu_max_days` after a parameter edit (C API).
void refreshDerived(SimulationContext& ctx) noexcept;

/**
 * @brief Size the state and seed initial buildup: `[LOADINGS]` row when
 *        given, else the land use's function at `[OPTIONS] DRY_DAYS`
 *        (legacy `landuse_getInitBuildup`).
 */
void initState(SimulationContext& ctx);

/**
 * @brief One runoff step: buildup accrual then washoff for every
 *        (subcatchment, land use, species); stores the washoff
 *        concentration per (subcatchment, species) for delivery.
 *        Runs the pollutant kernel (SurfaceSpeciesKernel) over the species
 *        store (Multispecies Phase 2).
 * @param dt_runoff        runoff step (s)
 * @param old_runoff_time  seconds since the start at the step's start
 */
void step(SimulationContext& ctx, double dt_runoff, double old_runoff_time);

/// Post-LID bookings for the species, after the LID units have run
/// (SWMMEngine::bookWashoffLoads' species half).
void bookPostLid(SimulationContext& ctx, double dt_runoff,
                 const std::vector<double>& lid_infil_vol);

/// LID underdrain species load from one unit (the A6b drain loop's species
/// half), and the per-runoff-step roll of those loads.
void addLidDrain(SimulationContext& ctx, int control, int sc, int node, double drain_cfs,
                 bool to_perv, bool runoff_load, double dt_runoff) noexcept;
void rollLidDrain(SimulationContext& ctx) noexcept;
/// A LID underdrain routed to another subcatchment: the species join that
/// subcatchment's run-on (next runoff step), as the water does.
void addLidDrainRunon(SimulationContext& ctx, int control, int sc, int receiver, double drain_cfs,
                      bool to_perv, bool runoff_load, double dt_runoff) noexcept;
void consumeLidDrainRunon(SimulationContext& ctx, int receiver) noexcept;

/// Set the store up for a species that only rains in (called by resolve and
/// initState).
void activateForRain(SimulationContext& ctx) noexcept;

/// Bind LID REMOVALS entries kept by name (called by resolve).
void resolveLidRemovals(SimulationContext& ctx, std::vector<std::string>& warnings);

/// Run-on, beside the pollutant lines in SWMMEngine::assembleRunon.
void clearRunon(SimulationContext& ctx) noexcept;
void addRunon(SimulationContext& ctx, int donor, int receiver, double q) noexcept;


/**
 * @brief Deliver this routing step's washoff into `msx_ext_mass_in`
 *        (+= washoff_conc × q̄ per outlet node, the `addWetWeatherLoads`
 *        trapezoid on old/new runoff). Call after the external-inflow
 *        assembly has (re)zeroed the array for the step.
 */
void deliver(SimulationContext& ctx, double dt_routing);

} // namespace msxsurf
} // namespace openswmm

#endif // OPENSWMM_ENGINE_QUALITY_MSX_SURFACE_QUALITY_HPP
