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
#include "SubsurfaceTransportState.hpp"   // T7.1

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
struct GwTransportData;
/// T7.2: declared in GwTransportData.hpp; the solver only needs the tag.
enum class GwScope : int8_t;

/// T7.1: the row layout `initTransport` needs, without the solver having to
/// include `TransportPolicy` (and with it the whole SimulationContext) — the
/// router resolves the policy and hands the answer over.
struct RowLayoutLite {
    int n_species = 0;
    int n_pollut  = 0;
    int n_msx     = 0;
    int age_row   = -1;
    int temp_row  = -1;
    std::vector<std::string> names;
};

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

    // ---- T7.1: species / age / enthalpy -----------------------------------

    /// The transported tuple, sized from `TransportPolicy` at `initTransport`
    /// and inert (`active() == false`) until then.
    SubsurfaceTransportState&       transport()       noexcept { return tr_; }
    const SubsurfaceTransportState& transport() const noexcept { return tr_; }
    /// Size the rows and seed both stores from `[GW_INITIAL_QUALITY]`.
    /// Separate from `initialize` because the row layout needs the resolved
    /// `[GW_*]` sections, which the router owns. @p names is the row layout's
    /// name list (TransportPolicy order).
    /// @param pollut_decay the engine's `[POLLUTANTS]` Kdecay column (1/s),
    ///        row-aligned with the layout — what a `[GW_SORPTION]` row with
    ///        no DECAY of its own falls back to.
    void initTransport(const RowLayoutLite& rows, const GwTransportData* gw,
                       const std::vector<double>& pollut_decay,
                       std::vector<std::string>& warnings);
    /// Water volume of one cell's saturated zone (m³) — the denominator for
    /// a saturated concentration.
    double satVolume(int cell) const noexcept {
        const auto u = static_cast<std::size_t>(cell);
        return state_.hg[u] * state_.theta_s[u] * state_.area[u];
    }
    /// …and of its unsaturated column store (m³), whatever closure holds it.
    double unsatVolume(int cell) const noexcept {
        const auto u = static_cast<std::size_t>(cell);
        return state_.hu[u] * state_.area[u];
    }
    /// The surface hands mass down with its infiltration (T7.1 seam): the
    /// mass the water it booked through `bookInfiltrationFromSurface` was
    /// carrying, gathered when the GW cell fires.
    void bookInfiltrationMass(int cell, int species, double mass) noexcept;
    /// …and takes back what saturation excess and rejection carried up.
    /// Drained by the surface cell in the same call that takes the water.
    double takeToSurfaceMass(int cell, int species) noexcept;
    const GwOptions&       options() const noexcept { return options_; }

    // ---- LTS hooks -------------------------------------------------------

    /// Per-cell stability step `min(Δt_g, Δt_u)`, written into
    /// `state_.dt_cell`. Called once per rebuild; cheap enough to be
    /// unconditional (the plan's "they rarely bind").
    void refreshDtCell(const MeshData& mesh, const InertialEdges& edges);
    /// G1-c (2026-09-19): the smallest per-cell stability step after
    /// `refreshDtCell` (s), or +inf when nothing bounds it — the marcher folds
    /// it into the ladder's base step so a groundwater cell whose own bound is
    /// finer than every active surface cell's does not fire beyond it.
    double minDtCell() const noexcept {
        double m = 1.0e30;
        for (const double v : state_.dt_cell) if (v < m) m = v;
        return m;
    }

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
    /// G-X3 (2026-09-19): the router books a conduit's seepage volume (m³,
    /// SI, its length-weighted share for this cell) at the routing cadence;
    /// the GW cell gathers it at its firing as a saturated-zone inflow.
    void bookLinkSeepage(int cell, double vol_m3) noexcept;
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

    /// T7.4: the node's own species row, `[node * n_species + s]`, published
    /// by the router each batch — what a RECHARGING node's water carries
    /// down its bed. Null (or a zero row count) means the seam moves water
    /// only, which is what every pre-T7.4 deck did.
    void setNodeRowConc(const double* rows, int n_species) noexcept {
        node_row_conc_ = rows;
        node_row_ns_   = n_species;
    }
    /// T7.4: mass the aquifer owes each 1D node this batch
    /// (`[s * n_nodes + node]`, + out of the aquifer), flushed by the router
    /// into the node's coupling queues alongside the volume.
    const std::vector<double>& nodeExchangeMass() const noexcept {
        return tr_.node_out_mass;
    }
    /// T7.4: mass a leaking conduit delivered into one cell, at the
    /// conduit's own concentration — the species twin of `bookLinkSeepage`.
    void bookLinkSeepageMass(int cell, int species, double mass) noexcept;
    /// T7.4: zero the per-bed withdrawal weights, with the volume ledger.
    void clearBedDrawn() noexcept;

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
    /// G-O: the `[2D_AQUIFER_NODE]` beds as resolved (cell index filled),
    /// in the authored order — parallel to SurfaceRouter2D::aquiferNodeNames().
    const std::vector<GwNodeBed>& nodeBeds() const noexcept { return node_beds_; }
    /// G-O: cumulative exchange per bed (m³, + out of the aquifer into the
    /// pipe) since the start — the results-file series behind the GUI's
    /// GwExchange plot. Never reset.
    const std::vector<double>& bedExchangeCumulative() const noexcept {
        return bed_exchange_cum_;
    }

    /// Storage now, for the continuity ledger.
    double storage() const noexcept { return state_.storage(); }

private:
    void resolveRows(const MeshData& mesh, const GwUnitFactors& uf,
                     SubsurfaceConfig& cfg, std::vector<std::string>& warnings);
    void resolveClosures(std::vector<std::string>& warnings);
    soil::Params paramsOf(int i) const noexcept;
    /// One cell's firing — the §gw_split sequence.
    void fireCell(int i, double dt, SurfaceStateData& surf);

    /// T7.1: every water volume one cell's firing moved, so the species pass
    /// can ride exactly the same numbers. Volumes are m³ and signed as the
    /// kernel books them: `+ node_out` LEAVES the aquifer, `link` is signed
    /// (G-X4: `+` a leaking conduit filling the cell, `−` a gaining one
    /// drawing from it), `+ recharge` is unsaturated → saturated.
    struct CellFlux {
        double v_sat0 = 0.0, v_uns0 = 0.0;   ///< water volumes BEFORE the firing
        double lateral = 0.0;                ///< gathered, + into the cell
        double node_out = 0.0;
        double infil_in = 0.0;
        double link = 0.0;
        double recharge = 0.0;
        /// The MOVING TABLE's own swap between the two stores (m³, `+` into
        /// the unsaturated one, i.e. a falling table handing its drained
        /// slab up). Water crosses the boundary here without any flux
        /// driving it, so the mass has to cross with it or the two zones
        /// end the step holding each other's solute. Derived from the
        /// column's own balance, which makes it exact for every closure
        /// rather than only for closure A's explicit `θ_bot·ΔL`.
        double handover = 0.0;
        double et = 0.0;                     ///< out of the column
        double deep = 0.0;
        double dt = 0.0;                     ///< T7.2: the firing's own step (s), for decay
        double dunne = 0.0;                  ///< saturation excess, out of the saturated zone
        double reject = 0.0;                 ///< column rejection, out of the unsaturated zone
    };
    /// Move the tuple along @p f. Called at the END of `fireCell`, when every
    /// volume is final.
    void fireCellSpecies(int i, const CellFlux& f) noexcept;
    /// Gather this cell's lateral side accumulators (m³) and zero them.
    double gatherLateral(int i) noexcept;
    /// T7.1: the same gather for one species' mass.
    double gatherLateralMass(int i, int s) noexcept;
    /// T7.2: does a `* / TAG / CELL` scoped row cover this cell?
    bool scopeCoversCell(int i, GwScope scope, const std::string& tag,
                         int cell) const noexcept;
    /// T7.4: gather one species' share of this cell's node-seam arrivals.
    double gatherNodeMass(int i, int s) noexcept;
    /// T7.2: the `[POLLUTANTS]` decay constant for a transport row (1/s).
    double pollutantDecay(int s, const std::vector<double>& pollut_decay) const noexcept;
    /// Gather this cell's booked node exchange (m³, + out of the aquifer).
    double gatherNode(int i) noexcept;
    void markPendingSurface(int i) noexcept;

    SubsurfaceState  state_;
    SubsurfaceTransportState tr_;   ///< T7.1
    GwOptions        options_;
    std::vector<GwNodeBed> node_beds_;
    std::vector<double>    bed_exchange_cum_;   ///< G-O: per bed (m³), cumulative
    /// T7.4: volume each bed drew OUT of the aquifer since the last flush —
    /// the weights that split a cell's outgoing mass across its beds.
    std::vector<double>    bed_last_out_;
    /// G-X1: water taken FROM each 1D node by the recharge direction within
    /// the current routing batch (m³, 2D units), reset with
    /// `node_exchange_vol_` — the frozen node volume is a batch budget, not a
    /// per-substep one, exactly as the surface's `node_drawn_` ledger.
    std::vector<double>    node_drawn_gw_;
    /// T7.4: the router's published node species rows (not owned).
    const double* node_row_conc_ = nullptr;
    int           node_row_ns_   = 0;

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
