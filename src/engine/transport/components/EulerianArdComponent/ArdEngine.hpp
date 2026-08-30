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
 *          Scope as of E4/R6 (see the validation handoffs): conservative
 *          transport with structures (pumps/orifices/weirs/outlets) as
 *          zero-volume passthrough of the donor node store, persistent
 *          user quality-mass-flux forcing, longitudinal dispersion
 *          (per-conduit user coefficients and/or Fischer et al. 1979 via
 *          the transport.ard component, E3) sequentially split after
 *          advection each substep, and REACTIONS (E4/R6, Lie-split per
 *          routing step): exact-exponential kdecay plus MSX species
 *          integration per cell (pipe scope) and node store (tank scope)
 *          through the shared reaction module. MSX species are TRANSPORTED
 *          on the mesh under this engine — the state carries pollutant
 *          rows first (np-aligned) then the MSX rows; the R4b
 *          element-local limitation is LEGACY-only. WALL species fall
 *          back to LEGACY with a warning.
 *
 *          Treatment interop, sources/BCs and the mass-balance ledger rows
 *          LANDED with E5a (`cbb9d321`) and E5b (`721ae60c`) — this comment
 *          listed all three as pending until 2026-08-25, five rounds after
 *          the fact, and a program-state audit read it as authoritative.
 *
 *          Genuinely still pending: direct consumption of the FV solver's
 *          own cell state instead of the projection (E2b); storage mixing
 *          models beyond CMSTR (E2b, shared with LARD); tidal reverse-flow
 *          boundary concentration (E2b, no scaffolding at all).
 *
 * @ingroup engine_transport
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_TRANSPORT_ARD_ENGINE_HPP
#define OPENSWMM_ENGINE_TRANSPORT_ARD_ENGINE_HPP

#include <fstream>
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

    /**
     * @brief E5b treatment interop: absorb treated node concentrations back
     *        into the node stores.
     *
     * @details Called by SWMMEngine AFTER QualitySolver::applyTreatment ran
     *          on the PUBLISHED nodes.conc. Only nodes with treatment
     *          defined are touched — untreated nodes keep their store mass
     *          bit-identical (a conc→mass→conc round trip is NOT exact in
     *          floating point, so a blanket absorb would break the
     *          no-treatment parity).
     */
    void absorbTreatedNodeConc(SimulationContext& ctx);

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
    // update, node CSTR update (external loads prorated by dt_sub / dt_step),
    // then the implicit dispersion solve over the updated cells (E3,
    // sequential Lie split: one full advection step, then one full
    // dispersion step — NOT Strang, which would halve the advection).
    void substep(SimulationContext& ctx, double dt_sub, double load_frac);

    // E3: refresh cell_disp_ from the routing step's hydraulics. Per-conduit
    // overrides always win; otherwise VALUE mode broadcasts the global
    // coefficient and FISCHER mode evaluates D = 0.011 v²B²/(Y·U*),
    // U* = √(g·Y·S) from the link fields (all internal ft units).
    void updateDispersion(SimulationContext& ctx);

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

    // E5a: transport boundaries/sources resolved onto the mesh at init.
    // Boundaries: the node's external inflow water carries the species at
    // bc_now_ (evaluated once per routing step: VALUE or timeseries).
    // Sources: internal-rate mass distributed over the conduit's wet cells.
    void updateTransportRows(SimulationContext& ctx);
    std::vector<int>    bc_node_, bc_srow_, bc_ts_;
    std::vector<double> bc_value_, bc_now_;
    std::vector<int>    src_crow_, src_srow_, src_ts_;
    std::vector<double> src_value_, src_len_, src_now_;

    // E3 dispersion state. disp_active_ gates the whole path: when false the
    // substep passes no per-cell array and dispersionSolve early-outs — the
    // pre-E3 behavior, bit-identical for existing ARD decks.
    bool disp_active_ = false;
    int  disp_mode_   = 0;      ///< ArdDispersionMode as int (OFF/FISCHER/VALUE)
    double disp_global_ft_ = 0.0;   ///< VALUE-mode coefficient, ft²/s
    std::vector<double> conduit_disp_ft_;  ///< per mesh-conduit override, ft²/s (<0 ⇒ none)
    std::vector<double> cell_disp_;        ///< per-cell D handed to the kernel

    // E5b: per-cell CSV sidecar ([TRANSPORT_OPTIONS] DETAILED_OUTPUT).
    // Written every routing step (documented decision — a detail feature
    // for short diagnostic runs; cadence control can come later if sizes
    // demand it). Columns: time_s, element, kind (L link cell / N node
    // store), cell index, species name, concentration.
    void writeDetailRows(SimulationContext& ctx);
    std::ofstream detail_out_;
    bool   detail_active_ = false;
    double detail_time_s_ = 0.0;

    /// A1a: state row of the reserved __WATER_AGE__ species (after
    /// pollutants and MSX); -1 when WATER_AGE is off.
    int age_row_ = -1;

    /// H4: per-cell surface heat exchange (plan §1's source term), the
    /// mesh twin of the LEGACY mirror's whole-link application.
    void applyHeatFluxes(SimulationContext& ctx, double dt);

    /// H4: state row of the reserved __TEMPERATURE__ species — the LAST
    /// row, after age, matching the REPORTED column order fixed in H1 so a
    /// deck that adds heat to an age model does not move the age column.
    /// -1 when HEAT_TRANSPORT is off.
    int temp_row_ = -1;

    std::vector<std::string> warnings_;
    bool initialized_ = false;
    bool warned_cfl_clamp_ = false;  ///< E2: once-per-run loud subcycle clamp
};

}  // namespace openswmm::transport

#endif  // OPENSWMM_ENGINE_TRANSPORT_ARD_ENGINE_HPP
