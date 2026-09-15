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
 * @file SubsurfaceSolver.hpp
 * @brief G-steps 3, 6-12, 16 — the two-zone groundwater kernel: saturated FV
 *        update, lateral Darcy, closure dispatch, node exchange, Dunne
 *        transfer, ET, and the LTS tier hooks the surface marcher calls.
 *
 * @section gw_lts How this joins the marcher (§3, guarantees G-A and G-B)
 *
 * The subsurface does **not** get a loop of its own. `ExplicitInertialSolver`
 * calls four hooks, mirroring what it already does for the surface:
 *
 * | Hook | When | What |
 * |---|---|---|
 * | `assignTiers` | every `syncAndRebuild` | Δt_g / Δt_u per cell → tier lists |
 * | `fireGwFaces(tier, dt)` | face phase of a due tier | lateral Darcy → ±ΔV side accumulators |
 * | `fireGwCells(tier, dt)` | cell phase of a due tier | gather, closure, node/deep/ET/Dunne |
 * | `settle()` | before any re-tier | flush every accumulator |
 *
 * **G-A** — a GW cell's tier comes only from `min(Δt_g, Δt_u)`. The surface's
 * pin-to-tier-0 rule for coupling cells is deliberately NOT applied here: a
 * GW cell with node exchange or infiltration keeps its own tier and the
 * exchange accumulates until it fires. Only the surface twin is ever pinned.
 *
 * **G-B** — every new flux channel books ±Δ into a side accumulator at the
 * producer's cadence and is gathered by the owner at its own firing, exactly
 * like `facc_L_`/`facc_R_`. Cross-tier conservation is inherited, not
 * re-proved.
 *
 * @section gw_split Operator split within one cell firing
 *
 * The order matters and is the plan's (§2.3 "apply saturated Δh_g first,
 * then the column sees the new L"):
 *
 * 1. `q₀_phys` — the physical Darcy flux across the table, from the current
 *    column state (closure B) or the closed form (closure A).
 * 2. Saturated update with `q₀_phys`, lateral gather, node exchange, deep
 *    loss → new `h_g`, clamped to `[0, z_s]`.
 * 3. `L̇` from the CLAMPED `h_g`, so clamping cannot break conservation.
 * 4. Column sweep with that `L̇`; its `f_bot` carries the handover, and its
 *    `overflow_to_sat` / `deficit_from_sat` are applied to `h_g` in this same
 *    firing.
 * 5. Dunne: `h_g > z_s − ε` moves the excess to the surface twin.
 *
 * @note **Specific yield.** Step 2 divides by `θ_s − θ_bot`, not `θ_s`. See
 *       `SigmaColumn.hpp`'s note: with the handover made explicit, `θ_s`
 *       does not conserve and `θ_s − θ_bot` is the textbook specific yield.
 *
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP

#include "SigmaColumn.hpp"
#include "SubsurfaceData.hpp"

#include <string>
#include <vector>

namespace openswmm {
struct NodeData;
}  // namespace openswmm

namespace openswmm::twoD {

struct MeshData;
struct InertialEdges;
struct SurfaceStateData;
struct SolverOptions2D;

using openswmm::NodeData;

/**
 * @brief The two-zone kernel. Owned by SurfaceRouter2D; driven by the
 *        marcher's tier hooks.
 */
class SubsurfaceSolver {
public:
    /**
     * @brief Resolve the authored rows onto cells, seed state, size the
     *        accumulators.
     *
     * @param mesh     cell areas, centroid elevations and tags
     * @param edges    the marcher's unique interior-edge topology, shared
     * @param opts     the 2D solver options (LTS ladder, unit factors)
     * @param uf       project-unit → SI multipliers for the authored rows
     * @param n_nodes  1D node count, to size the node-exchange ledger
     * @param cfg      the authored rows — read, never modified
     * @param warnings advisory diagnostics appended here
     * @returns a fatal diagnostic; empty on success.
     *
     * @note Takes what it needs rather than a `SimulationContext`. That is
     *       not tidiness: it is what lets the numerical gates build a mesh,
     *       a soil and a table in twenty lines and check a conservation
     *       identity, with no engine, no project file and no units. A kernel
     *       that can only be exercised through a full model is a kernel
     *       whose failures are found late.
     */
    std::string initialize(const MeshData& mesh, const InertialEdges& edges,
                           const SolverOptions2D& opts,
                           const GwUnitFactors& uf, int n_nodes,
                           SubsurfaceConfig& cfg,
                           std::vector<std::string>& warnings);

    bool active() const noexcept { return state_.active; }
    SubsurfaceState&       state()       noexcept { return state_; }
    const SubsurfaceState& state() const noexcept { return state_; }
    const GwOptions&       options() const noexcept { return options_; }

    // ---- LTS hooks -------------------------------------------------------

    /// Per-cell stability step `min(Δt_g, Δt_u)`, written into
    /// `state_.dt_cell`. Called once per rebuild; cheap enough to be
    /// unconditional (the plan's "they rarely bind").
    void refreshDtCell(const MeshData& mesh, const InertialEdges& edges);

    /// Assign tiers from `dt_cell` against the marcher's `dt0` and tier
    /// count, and rebuild the per-tier cell/face lists. **G-A:** no surface
    /// quantity enters this.
    void assignTiers(double dt0, int n_tiers);

    /// Minimum tier count the ladder needs so it reaches the slowest GW cell
    /// (`K ≥ 1 + ⌈log2(Δt_gw_max/dt0)⌉`, G-A point 3). The marcher grows its
    /// ladder to this before assigning.
    int requiredTiers(double dt0) const noexcept;

    /// Lateral Darcy on the faces of one tier; books ±ΔV into `eacc_L/R`.
    void fireGwFaces(int tier, double dt);
    /// Gather + closure + node/deep/ET/Dunne for the cells of one tier.
    void fireGwCells(int tier, double dt, SurfaceStateData& surf);
    /// Flush every pending accumulator into its owner. Mandatory before any
    /// re-tier or active-set change (the stranded-flux hazard).
    void settle(SurfaceStateData& surf);

    /// True when the tier's cell list is non-empty.
    bool tierHasCells(int tier) const noexcept {
        return tier >= 0 && tier < static_cast<int>(cells_by_tier_.size()) &&
               !cells_by_tier_[static_cast<std::size_t>(tier)].empty();
    }
    bool tierHasFaces(int tier) const noexcept {
        return tier >= 0 && tier < static_cast<int>(faces_by_tier_.size()) &&
               !faces_by_tier_[static_cast<std::size_t>(tier)].empty();
    }
    int tierCount() const noexcept {
        return static_cast<int>(cells_by_tier_.size());
    }
    /// Per-tier firing counts — the G-A telemetry (`swmm_gw2d_get_tier_histogram`).
    const std::vector<long>& tierFirings() const noexcept { return tier_firings_; }

    // ---- surface ↔ subsurface --------------------------------------------

    /// The surface books infiltration it delivered to cell `i` (m³) at its
    /// own (finer) cadence; the GW cell gathers it when it fires.
    void bookInfiltrationFromSurface(int cell, double vol_m3) noexcept;
    /// Volume (m³) the subsurface owes the surface twin — Dunne and
    /// exfiltration — drained by the surface at its firing.
    double takeToSurface(int cell) noexcept;
    /// Cells with pending surface-bound water, so the marcher can pin them
    /// active (a dry surface cell above an exfiltrating column must route).
    /// Call `compactPending()` first — `takeToSurface` clears flags but does
    /// not erase from this list, because it runs inside the parallel cell
    /// loop.
    const std::vector<int>& pendingSurfaceCells() const noexcept {
        return pending_surface_;
    }
    /// Drop drained cells from `pendingSurfaceCells()`. Serial; call from
    /// the marcher's rebuild, not from a firing.
    void compactPending() noexcept;

    /// Node ↔ aquifer exchange, evaluated at tier-0 cadence against the
    /// batch-frozen 1D heads and booked into `nacc` (G-B row 3). The GW cell
    /// gathers its share when IT fires — the node is never pinned to the
    /// aquifer's tier and the aquifer is never pinned to tier 0.
    /// @param nodes 1D node data, or nullptr when nothing is coupled.
    void sampleNodeExchange(const NodeData* nodes, double dt);

    /// Per-node exchange volume this advance (m³ SI, + out of the aquifer
    /// into the pipe). The router books it into `nodes.coupling_volume`
    /// through the same `flow_2d_to_1d` factor the surface exchange uses.
    const std::vector<double>& nodeExchangeVolumes() const noexcept {
        return node_exchange_vol_;
    }
    /// Zero the per-node ledger at the start of a routing batch.
    void resetNodeExchangeVolumes() noexcept;

    /// Storage now, for the continuity ledger.
    double storage() const noexcept { return state_.storage(); }

private:
    void resolveRows(const MeshData& mesh, const GwUnitFactors& uf,
                     SubsurfaceConfig& cfg, std::vector<std::string>& warnings);
    void resolveClosures(std::vector<std::string>& warnings);
    soil::Params paramsOf(int i) const noexcept;
    /// One cell's firing — the §gw_split sequence.
    void fireCell(int i, double dt, SurfaceStateData& surf);
    /// Gather this cell's lateral side accumulators (m³) and zero them.
    double gatherLateral(int i) noexcept;
    /// Gather this cell's booked node exchange (m³, + out of the aquifer).
    double gatherNode(int i) noexcept;
    void markPendingSurface(int i) noexcept;

    SubsurfaceState  state_;
    GwOptions        options_;
    std::vector<GwNodeBed> node_beds_;

    const MeshData*      mesh_  = nullptr;
    const InertialEdges* edges_ = nullptr;
    const SolverOptions2D* opts_ = nullptr;

    std::vector<std::vector<int>> cells_by_tier_;
    std::vector<std::vector<int>> faces_by_tier_;
    std::vector<uint8_t>          face_tier_;
    std::vector<long>             tier_firings_;
    std::vector<int>              pending_surface_;
    std::vector<uint8_t>          pending_flag_;
    std::vector<int>              cell_nface_;   ///< incident GW faces per cell
    std::vector<double>           node_exchange_vol_;  ///< per 1D node (m³)
    bool accumulators_pending_ = false;

    /// PER_SUBCATCH: cell i ↔ subcatchment i, and the outlet node it
    /// exchanges with. Empty in mesh mode.
    std::vector<int> subcatch_outlet_node_;
    /// Mesh mode: cell → containing subcatchment for the legacy arbitration
    /// (centroid-in-polygon, draft decision 13). −1 = not covered.
    std::vector<int> cell_subcatch_;
};

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP
