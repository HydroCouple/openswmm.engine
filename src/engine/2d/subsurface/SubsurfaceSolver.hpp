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
 * 1. Gather completed lateral, node, link, source, infiltration and ET water.
 * 2. Compute physical Darcy recharge and a specific-yield table predictor.
 * 3. CLOSED_FORM and ENSLAVED retain their bulk/equilibrium updates. SIGMA
 *    trials the existing explicit column sweep from one original snapshot
 *    while bracketing the table on `[0, z_s]`. Saturated storage change must
 *    equal external saturated exchanges plus actual bottom transfer,
 *    compression surplus and residual support from that same sweep.
 * 4. Commit one settled column/table pair. Only water outside the physical
 *    bracket becomes saturation excess or an unavailable-withdrawal refund.
 * 5. Pair the accepted water with species and completed atmospheric ledgers.
 *
 * @note **Specific yield.** The predictor uses `θ_s − θ_bot`. SIGMA's final
 *       consistency equation uses saturated storage `θ_s·h_g` and the
 *       actual bottom flux including handover; it never treats the
 *       predictor or a floored storage derivative as completed water.
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
#include "SurfaceExchange.hpp"
#include "../gw/GwSourceForcing.hpp"
#include "SubsurfaceData.hpp"
#include "SubsurfaceTransportState.hpp"
#include "../coupling/RuntimeCoupling.hpp"   // T7.1

#include <algorithm>
#include "../heat/GroundHeat.hpp"
#include <string>
#include <vector>

namespace openswmm::detail { class CompletedSourceArchive; }

namespace openswmm {
struct NodeData;
struct SimulationContext;
}  // namespace openswmm

namespace openswmm::twoD {
struct RichardsBedStepper;

struct MeshData;
struct InertialEdges;
struct SurfaceStateData;
struct SolverOptions2D;
struct RichardsBedOwner;
struct RichardsBedAcceptance;
struct RichardsBedReceipt;
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

    /// One surface interval: reserve actual pond evaporation first. No forcing refresh resets this budget.
    void bookSurfaceEt(int cell,double dt,double potential_rate,double actual_surface_volume);
    /// SI completed footprint volumes, already paid by their surface/LID owners.
    /// Only soil_demand may reach this cell's extraction site; the rest expires.
    void bookAreaEt(int cell,double dt,double potential,double owner_evaporation,double soil_demand);

    bool active() const noexcept { return state_.active; }
    /// Mesh mode: 1 where a [2D_AQUIFER] row (`*`, TAG or CELL) covers the
    /// cell. An uncovered cell has no aquifer: no storage, no lateral faces,
    /// and its infiltration stays with the surface model. All 1 in
    /// PER_SUBCATCH mode.
    const std::vector<uint8_t>& coverage() const noexcept { return covered_; }
    bool covers(int cell) const noexcept {
        return cell >= 0 && static_cast<std::size_t>(cell) < covered_.size() &&
               covered_[static_cast<std::size_t>(cell)] != 0;
    }
    SubsurfaceState&       state()       noexcept { return state_; }
    const SubsurfaceState& state() const noexcept { return state_; }

    // ---- T7.1: species / age / enthalpy -----------------------------------

    /// The transported tuple, sized from `TransportPolicy` at `initTransport`
    /// and inert (`active() == false`) until then.
    SubsurfaceTransportState&       transport()       noexcept { return tr_; }
    const SubsurfaceTransportState& transport() const noexcept { return tr_; }
    GroundHeatState& heat() noexcept {return heat_;}
    const GroundHeatState& heat() const noexcept {return heat_;}
    double pendingHeat(int cell) const noexcept;
    std::string initHeat(SimulationContext&,const GwTransportData&);
    std::string initPorewater();
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
    /// A gaining conduit draws `vol` m³ of water from `cell`: book the mass
    /// it carries, at the cell's dissolved saturated concentration, for the
    /// cell to debit at its next firing, and return it (native units). The
    /// mass is capped at what the cell holds less what is already pending, so
    /// a cell whose saturated zone has emptied cannot export residual mass
    /// at a concentration divided by a vanishing volume.
    double bookLinkGain(int cell, int species, double vol) noexcept {
        if (!tr_.active() || !(vol > 0.0) || cell < 0 || cell >= tr_.n_cells ||
            species < 0 || species >= tr_.n_species) return 0.0;
        const double v = satVolume(cell);
        if (!(v > 0.0)) return 0.0;
        const auto u = static_cast<std::size_t>(cell);
        const auto k = tr_.idx(species, cell);
        const double mob = tr_.dissolvedFraction(species, cell, state_.theta_s[u]);
        double m = vol * (tr_.sat_mass[k] / v) * mob;
        if(heat_.active() && species==tr_.temp_row){heat_link_out_volume_[u]+=vol;bookPresetHeatOut(cell,vol,m);}
        if (!tr_.signedRow(species)) {
            const double avail = std::max(0.0, tr_.sat_mass[k] * mob - tr_.lout_mass[k]);
            m = std::min(m, avail);
        }
        if (m == 0.0) return 0.0;
        bookPresetPorewaterOut(cell,species,m);
        tr_.lout_mass[k] += m;
        return m;
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
    /// Internal reviewed-source ownership. Must bind before pending exchange.
    bool sourceReceiptOwnershipBound() const {
        return std::any_of(source_owned_.begin(),source_owned_.end(),[](bool x){return x;});
    }
    std::string configureSourceReceipts(const std::vector<int>& cells);
    void retainSourceReceipt(const SurfaceIntakeReceipt&);
    const std::vector<SurfaceIntakeReceipt>& pendingSourceReceipts(int cell) const { return source_receipts_.at(cell); }
    double sourceCompletedTime(int cell) const { return source_cell_end_.at(cell); }
    const std::vector<SurfaceReturnReceipt>& sourceReturns() const { return source_returns_; }
    void acknowledgeSourceReturns(double completed_through);
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
    /// accepted_end is the common marcher's exact completed endpoint; owned
    /// cells start at their last completed firing, without rounded clock gaps.
    void fireGwCells(int tier, double dt, SurfaceStateData& surf, double time = 0.0,
                     double accepted_end = -1.0);
    RuntimeSources& runtimeSources() noexcept { return runtime_sources_; }
    const RuntimeSources& runtimeSources() const noexcept { return runtime_sources_; }
    void setSources(std::vector<GwResolvedSource> sources);
    /// Flush every pending accumulator into its owner. Mandatory before any
    /// re-tier or active-set change (the stranded-flux hazard).
    void settle(SurfaceStateData& surf);
    void restorePendingSurface() {
        pending_surface_.clear(); std::fill(pending_flag_.begin(), pending_flag_.end(), 0);
        for (int i = 0; i < state_.n_cells; ++i) if (state_.xacc_to_surface[i] > 0.0) markPendingSurface(i);
    }

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
    /// Publish the aquifer's held rate; publication never restores spent storage.
    void publishInfiltration(SurfaceStateData& surf, double interval, double time);
    /// Reserve and book once, BEFORE the sender removes water (SI m3).
    /// Producers for one cell run serially, as for the existing accumulators.
    double acceptSurfaceInfiltration(int cell, double requested, bool prescribed=false) noexcept;
    double infiltrationHeadroom(int cell) const noexcept;
    /// Read-only soil/interface rate (m/s) for the actual donor pond head.
    /// Group headroom is allocated separately; no front or held rate advances.
    double sourceInfiltrationCapacity(int cell, double pond_m, double interval_s) const noexcept;
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

    /// Node ↔ aquifer exchange, evaluated at tier-0 cadence against the
    /// batch-frozen 1D heads and booked into `nacc` (G-B row 3). The GW cell
    /// gathers its share when IT fires — the node is never pinned to the
    /// aquifer's tier and the aquifer is never pinned to tier 0.
    /// @param nodes 1D node data, or nullptr when nothing is coupled.
    void sampleNodeExchange(const NodeData* nodes, double dt, double end=-1);

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

    // ---- G-W1b: what a gate needs to prove its own deck is an instrument --
    //
    // A gate that measures a cap must first establish that the cap BOUND on
    // the deck it is measuring, or it can pass with the defect present. The
    // ratio between the two specific yields does not establish that — a deck
    // scoring 56.8× was measured bit-identical with the bug — because what
    // makes the cap bind is `h_g · Sy · A` being small in absolute terms.
    // Only the clamp knows. These three say so directly, with no threshold
    // to tune and no geometry that can make them lie.

    /// Firings on which the aquifer→node drain cap actually clamped `Q`.
    /// Zero means the deck cannot see anything this cap does.
    long drainCapBinds() const noexcept { return drain_cap_binds_; }
    /// Firings on which a column went short and the node's share of the
    /// overdraw was refunded — into the aquifer's books alone, after the
    /// router had already paid the node. The defect's signature: 124 before
    /// G-W1, 0 after.
    long nodeRefunds() const noexcept { return node_refunds_; }
    /// The specific yield the column ACTUALLY uses for `cell`, from the one
    /// definition. Exposed so a test never re-derives it: re-deriving only
    /// the CLOSED_FORM branch (as the first version of the G-W1b guard did)
    /// silently computes a number the solver never uses under SIGMA or
    /// ENSLAVED, and agrees only for as long as every fixture is closure A.
    double specificYieldOf(int cell) const noexcept;
    // Internal R5 trial budget in SI, before accepting a new node receipt.
    // Separate ceilings; incoming pending water never finances a withdrawal.
    // Call after atmospheric demand/other accepted channels have been booked.
    struct NodeWaterBudget { double head=0, donor=0, receiver=0; };
    std::string nodeWaterBudget(int cell, double interval, NodeWaterBudget&) const;
    double cellCompletedTime(int cell) const { return cell_completed_.at(cell); }
    const std::vector<double>& richardsBedDownCumulative() const noexcept { return richards_down_cum_; }
    const std::vector<double>& richardsBedUpCumulative() const noexcept { return richards_up_cum_; }
    // Hydraulic columns book completed intervals; groundwater consumes them
    // only at its own firing. Gross reservations cannot cancel each other.
    struct PendingLidBed { double start=0,end=0,down=0,up=0; };
    std::string configureLidReceipts(const std::vector<int>& beds,const std::vector<int>& mobile={});
    std::string retainLidReceipts(double start,double end,const std::vector<RichardsBedReceipt>&);
    bool lidReceiptsEnabled() const noexcept { return lid_receipts_enabled_; }
    double pendingLidDown(int cell) const noexcept;
    double pendingLidUp(int cell) const noexcept;
    double pendingLidWater() const noexcept;
    const std::vector<std::vector<PendingLidBed>>& pendingLidReceipts() const { return lid_receipts_; }
    struct LidBedDiagnostic {
        double start=0,end=0,groundwater_time=0,lid_head=0,groundwater_head=0;
        double requested_down=0,requested_up=0,reserved_down=0,reserved_up=0,accepted_down=0,accepted_up=0;
    };
    const std::vector<LidBedDiagnostic>& lidBedDiagnostics() const { return lid_bed_diagnostics_; }
    double pendingBedDown(int bed) const noexcept;
    double pendingBedUp(int bed) const noexcept;
    void setLidBedClogging(int bed,double factor) { lid_bed_factor_.at(bed)=factor; }

    /// Storage now, for the continuity ledger.
    double storage() const noexcept { return state_.storage(); }
    void checkpoint(detail::CompletedSourceArchive&,bool model); // internal accepted-boundary IO

private:
    friend class CommonSourceStage;
    friend RichardsBedAcceptance acceptRichardsBedsAtFiring(SubsurfaceSolver&, SurfaceStateData&,
        double, double, const std::vector<RichardsBedOwner>&);
    friend RichardsBedAcceptance acceptRichardsBedsForTier(SubsurfaceSolver&, SurfaceStateData&,
        int, double, double, const std::vector<RichardsBedOwner>&);
    friend RichardsBedAcceptance acceptRichardsBedsAcrossSteps(SubsurfaceSolver&, SurfaceStateData&,
        double,double,const std::vector<double>&,const std::vector<RichardsBedOwner>&,const RichardsBedStepper*);
    friend RichardsBedAcceptance acceptRichardsBedsForTierAcrossSteps(SubsurfaceSolver&, SurfaceStateData&,
        int,double,double,const std::vector<double>&,const std::vector<RichardsBedOwner>&,const RichardsBedStepper*);
    void resolveRows(const MeshData& mesh, const GwUnitFactors& uf,
                     SubsurfaceConfig& cfg, std::vector<std::string>& warnings);
    void resolveClosures(std::vector<std::string>& warnings);
    soil::Params paramsOf(int i) const noexcept;
    /// One cell's firing — the §gw_split sequence.
    void fireCell(int i, double dt, SurfaceStateData& surf, double time);
    void fireGwCellList(const std::vector<int>&, double dt, SurfaceStateData&, double time, double accepted_end);
    std::vector<bool> source_owned_;
    std::vector<std::string> source_species_;
    std::vector<double> source_cell_end_;
    std::vector<std::vector<SurfaceIntakeReceipt>> source_receipts_;
    std::vector<SurfaceReturnReceipt> source_returns_;
    double source_firing_end_ = 0.0;
    double source_completed_end_ = 0.0; // transient R5 readiness; old restores cannot activate Richards ownership
    double gather_fraction_ = 1.0;
    double consumePending(double& value) noexcept {
        const double take = value * gather_fraction_;
        value -= take;
        return take;
    }

    /// T7.1: every water volume one cell's firing moved, so the species pass
    /// can ride exactly the same numbers. Volumes are m³ and signed as the
    /// kernel books them: `+ node_out` LEAVES the aquifer, `link` is signed
    /// (G-X4: `+` a leaking conduit filling the cell, `−` a gaining one
    /// drawing from it), `+ recharge` is unsaturated → saturated.
    struct CellFlux {
        double hg_final=0;
        double runtime_dt=0.0, requested_source_out=0.0;
        double v_sat0 = 0.0, v_uns0 = 0.0;   ///< water volumes BEFORE the firing
        double lateral = 0.0;                ///< gathered, + into the cell
        double node_out = 0.0;
        double infil_in = 0.0;
        double link = 0.0;
        double source_in = 0.0, source_out = 0.0;
        const std::vector<double>* source_mass = nullptr;
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
        double paired_refund = 0.0;
        const std::vector<double>* refund_mass = nullptr;
        std::vector<double>* dunne_mass = nullptr;
        std::vector<double>* excess_mass = nullptr;
        double reject = 0.0;                 ///< column rejection, out of the unsaturated zone
    };
    /// Move the tuple along @p f. Called at the END of `fireCell`, when every
    /// volume is final.
    void fireCellSpecies(int i, const CellFlux& f) noexcept;
    void fireGroundHeatSpecies(int i,const CellFlux& f) noexcept;
    void fireGroundPorewaterSpecies(int i,const CellFlux& f) noexcept;
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

    /// G-W1: specific yield for cell `u` under closure `cl` — the ONE
    /// definition. Anything capping a withdrawal against the cell's drainable
    /// water must cap against the same number the column will spend it
    /// against, or the cap does not bind and the column goes short. Optionally
    /// reports the bottom/mean unsaturated content the derivation used.
    double specificYield(std::size_t u, const soil::Params& p, double ts,
                         double L0, GwClosure cl,
                         double* theta_bot_out) const noexcept;
    void markPendingSurface(int i) noexcept;

    void publishCellInfiltration(int, SurfaceStateData&, double interval, double time);
    std::vector<std::vector<std::size_t>> cell_beds_;
    RuntimeSources runtime_sources_;
    std::vector<GwResolvedSource> sources_;
    std::vector<std::vector<std::pair<std::size_t,double>>> cell_sources_;
    SubsurfaceState  state_;
    SubsurfaceTransportState tr_;   ///< T7.1
    GroundHeatState heat_;
    std::vector<double> heat_link_out_volume_;
    std::vector<double> heat_preset_out_j_; // [cell*thermal_layers+layer], accepted producer-time exports
    std::vector<double> heat_preset_out_water_; // paired physical layer volumes, including source-interval splits
    std::vector<double> heat_node_out_volume_,heat_node_gathered_out_; // gross accepted exports per hydraulic bed
    std::vector<double> porewater_preset_out_; // [species*n_cells*layers + cell*layers + layer]
    void bookPresetHeatOut(int cell,double volume,double temperature_volume) noexcept;
    void bookPresetPorewaterOut(int cell,int species,double mass) noexcept;
    struct ThermalFaceSlice {int left=0,right=0;double low=0,high=0;};
    struct ThermalFace {double distance_left=0,distance_right=0;std::vector<ThermalFaceSlice> slices;};
    std::vector<ThermalFace> heat_faces_;
    std::vector<double> heat_pending_left_,heat_pending_right_,heat_dt_;
    std::string initLateralHeat();
    void fireLateralHeatFace(int edge,double seconds);
    void gatherLateralHeat(int cell) noexcept;
    GwOptions        options_;
    std::vector<GwNodeBed> node_beds_;
    std::vector<double>    bed_exchange_cum_;   ///< G-O: per bed (m³), cumulative
    // Internal R5 owner clocks and directional receipts; execution/checkpoint
    // activation remains guarded until the routing/restart adapter is qualified.
    std::vector<double> cell_completed_, richards_down_cum_, richards_up_cum_;
    std::vector<bool> richards_bed_owned_;
    bool lid_receipts_enabled_=false;
    std::vector<std::vector<PendingLidBed>> lid_receipts_;
    std::vector<double> lid_booked_end_;
    std::vector<bool> mobile_lid_bed_;
    std::vector<double> lid_bed_factor_;
    std::vector<double> node_pending_down_,node_pending_up_;
    std::vector<LidBedDiagnostic> lid_bed_diagnostics_;
    bool richards_clock_valid_ = true; // old checkpoints do not restore the joint owner clocks
    /// T7.4: the per-bed volumes the CURRENT gather took — the weights that
    /// split this firing's outgoing mass across the cell's beds. Written by
    /// `gatherNode`, read by `fireCellSpecies` in the same firing, so it can
    /// never be stale (the bug the first cut had).
    std::vector<double>    bed_gathered_;
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
    long                          drain_cap_binds_ = 0;  ///< G-W1b
    long                          node_refunds_    = 0;  ///< G-W1b
    bool accumulators_pending_ = false;

    /// PER_SUBCATCH: cell i ↔ subcatchment i, and the outlet node it
    /// exchanges with. Empty in mesh mode.
    std::vector<int> subcatch_outlet_node_;
    /// Mesh mode: cell → containing subcatchment for the legacy arbitration
    /// (centroid-in-polygon, draft decision 13). −1 = not covered.
    std::vector<int> cell_subcatch_;
    /// See coverage(). Rebuilt from the rows by every initialize(), so a
    /// restart reproduces it without a checkpoint field.
    std::vector<uint8_t> covered_;
};

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP
