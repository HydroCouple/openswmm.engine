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
 * @file ArdEngine.hpp
 * @brief Solver-agnostic Eulerian ARD transport engine (phase E1).
 *
 * @details Selected by `[OPTIONS] QUALITY_SOLVER EULERIAN_ARD`
 *          (plans/transport/EULERIAN_ARD_TRANSPORT_PLAN.md rev. 2). Runs the
 *          promoted FV species kernels (transport/fvkernels) on the
 *          NetworkMeshBuilder cell mesh under ANY hydraulic routing model:
 *          link/node hydraulics are projected onto the cells each routing
 *          step with continuity-consistent face fluxes (§3.2 of the plan),
 *          species advect with sign-of-flux upwinding + MUSCL/QUICKEST
 *          reconstruction + Zalesak FCT, junctions mix as CSTRs fed by the
 *          same face fluxes, and results publish back into the legacy
 *          links.conc / nodes.conc arrays so reporting is unchanged.
 *
 *          E1 scope (enforced; see the validation handoff): conservative
 *          transport only — kdecay/treatment/reactions arrive with the
 *          shared reaction module (E4); dispersion arrives with E3; direct
 *          consumption of the FV solver's own cell state (instead of the
 *          projection) arrives with E2. A model that requests EULERIAN_ARD
 *          with kdecay or treatment configured gets a warning that those are
 *          not yet applied under this engine.
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_ARD_ENGINE_HPP
#define OPENSWMM_ENGINE_TRANSPORT_ARD_ENGINE_HPP

#include <string>
#include <vector>

#include "../../../hydraulics/fv/FvKernels.hpp"
#include "../../../hydraulics/fv/NetworkMeshData.hpp"

namespace openswmm {
struct SimulationContext;
}

namespace openswmm::transport {

class ArdEngine {
public:
    /**
     * @brief Build the transport mesh and size all state; call once after the
     *        model is fully resolved (Router::init done, pollutants known).
     * @return false if the mesh could not be built — the caller must fall
     *         back to the legacy quality path and surface the warnings.
     */
    bool init(SimulationContext& ctx);

    /**
     * @brief One transport step over the routing step dt.
     *
     * @details Pre-condition: the external quality loads for this step have
     *          been assembled into nodes.qual_mass_in / qual_vol_in (the
     *          QualitySolver loader stage). Post-condition: links.conc,
     *          links.conc_old, nodes.conc, nodes.conc_old updated.
     */
    void step(SimulationContext& ctx, double dt);

    bool initialized() const noexcept { return initialized_; }
    const std::vector<std::string>& warnings() const noexcept { return warnings_; }

    /// Total species mass currently held (cells + node stores), one entry per
    /// species — the conservation ledger the unit gates check.
    std::vector<double> totalMass(const SimulationContext& ctx) const;

private:
    // Continuity-consistent face fluxes from the link/node fields of the
    // current routing step (plan §3.2): within each conduit the face flux
    // ramps linearly so every cell receives an equal share of the conduit's
    // volume change, and the two end faces carry the flux the node exchange
    // actually saw. Splice faces (virtual junctions) take the mean of the
    // two conduits' end values.
    void projectHydraulics(SimulationContext& ctx, double dt);

    // One advection substep of length dt_sub: kernel reconstruction + FCT on
    // interior faces, donor-upwinded node boundary fluxes, cell mass/area
    // update, node CSTR update (external loads prorated by dt_sub / dt_step).
    void substep(SimulationContext& ctx, double dt_sub, double load_frac);

    void publish(SimulationContext& ctx);

    fv::NetworkMeshData  mesh_;
    fv::NetworkStateData state_;

    // Projected per-face volumetric flux for the current routing step and the
    // per-cell start-of-step areas it is consistent with.
    std::vector<double> face_q_;

    // Node stores: species mass [node * ns + s] and water volume the mass
    // sits in (tracked from the same fluxes, so node concentration is
    // self-consistent between substeps).
    std::vector<double> node_mass_;
    std::vector<double> node_vol_;

    // Kernel scratch (sized once in init; see SpeciesKernelView).
    std::vector<double> f_mass_, f_sstar_, f_phi_l_, f_phi_r_, f_phi_flux_,
        cell_slope_, lo_flux_, anti_flux_, td_, anew_, rplus_, rminus_, cell_u_;
    std::vector<fv::kernels::FaceState> f_state_l_, f_state_r_;
    std::vector<fv::kernels::FaceFlux>  f_flux_;
    std::vector<char> cell_active_;
    std::vector<int>  active_faces_;

    std::vector<std::string> warnings_;
    bool initialized_ = false;
};

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_ARD_ENGINE_HPP
