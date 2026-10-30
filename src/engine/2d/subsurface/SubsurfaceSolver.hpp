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
 * @file      SubsurfaceSolver.hpp
 * @brief     G-steps 3, 6-12, 16 — the two-zone groundwater kernel: saturated FV
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
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP

#include "SigmaColumn.hpp"
#include "../solver/GwRechargeBackend.hpp"
#include "../../transport/components/ReactionModule/ReactionIntegrator.hpp"
#include "SurfaceExchange.hpp"
#include "../gw/GwSourceForcing.hpp"
#include "../gw/GwBoundaryForcing.hpp"
#include "SubsurfaceData.hpp"
#include "SubsurfaceTransportState.hpp"
#include "../coupling/RuntimeCoupling.hpp" // T7.1

#include <algorithm>
#include "../heat/GroundHeat.hpp"
#include <string>
#include <vector>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm {
struct NodeData;
struct SimulationContext;
} // namespace openswmm

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
    int n_pollut = 0;
    int n_msx = 0;
    int age_row = -1;
    int temp_row = -1;
    std::vector<std::string> names;
};

using openswmm::NodeData;

/**
 * @brief The two-zone kernel. Owned by SurfaceRouter2D; driven by the marcher's tier hooks.
 */
class SubsurfaceSolver {
public:
    /**
     * @brief Resolve the authored rows onto cells, seed state, size the accumulators.
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
     * @note Takes what it needs rather than a `SimulationContext`. That is not tidiness: it is
     * what lets the numerical gates build a mesh, a soil and a table in twenty lines and check a
     * conservation identity, with no engine, no project file and no units. A kernel that can
     * only be exercised through a full model is a kernel whose failures are found late.
     */
    std::string initialize(const MeshData& mesh, const InertialEdges& edges,
                           const SolverOptions2D& opts, const GwUnitFactors& uf, int n_nodes,
                           SubsurfaceConfig& cfg, std::vector<std::string>& warnings);

    /**
     * @brief One surface interval: reserve actual pond evaporation first. No forcing refresh
     * resets this budget.
     *
     * - potential_rate: Potential atmospheric loss rate m/s (definition: rate).
     * - actual_surface_volume: Already accepted surface evaporation m3 (definition: surface).
     */
    void bookSurfaceEt(int cell, double dt, double potential_rate, double actual_surface_volume);
    /**
     * @brief SI completed footprint volumes, already paid by their surface/LID owners. Only
     * soil_demand may reach this cell's extraction site; the rest expires.
     *
     * @details Book a completed finite atmospheric owner budget for later groundwater demand.
     * - owner_evaporation: Accepted surface/owner evaporation m3 (definition: evaporation).
     * - soil_demand: Remaining soil demand m3 (definition: soil).
     */
    void bookAreaEt(int cell, double dt, double potential, double owner_evaporation,
                    double soil_demand);

    /**
     * @brief Read whether the resolved groundwater kernel is active.
     * @return state_.active; does not validate prepared arrays, pending receipts or
     * transport/heat activation. Default/empty configurations remain inactive; no state change
     * occurs.
     */
    bool active() const noexcept { return state_.active; }
    /**
     * @brief Replace the owned optional pure raw-recharge evaluator descriptor.
     * @param[in] backend Owned callback/capability/diagnostic value moved into the solver. Empty
     * evaluate selects ordinary CPU computation. Captured plugin/library resources must remain
     * valid through every callback; this method performs no probing, ABI validation or
     * output-cache reset itself.
     * @note Caller serializes replacement with all firings. Existing batch/cell telemetry is
     * retained; validation/fallback of evaluated results occurs in prepareRechargeBatch().
     */
    void setRechargeBackend(GwRechargeBackend backend) { recharge_backend_ = std::move(backend); }
    /**
     * @brief Borrow the current optional raw-recharge extension descriptor.
     * @return Solver-owned const callback/capability/diagnostic reference. Stable only while the
     * solver and descriptor are unchanged; evaluation failure can disable its evaluator/update
     * diagnostic. No capability probe, callback or allocation occurs in this getter.
     */
    const GwRechargeBackend& rechargeBackend() const noexcept { return recharge_backend_; }
    /**
     * @brief Read successful validated raw-recharge extension batch evaluations.
     * @return Cumulative successful nonempty batches since initialize(), excluding
     * rejected/CPU-only batches. Getter does not invoke the extension or reset counters.
     */
    uint64_t rechargeBatches() const noexcept { return recharge_batches_; }
    /**
     * @brief Read the cumulative raw-recharge input rows accepted from successful extension
     * batches.
     * @return Processed cell-row count, including repeated cells across firings/batches, since
     * initialize(). It is not unique covered mesh cells and excludes rejected/CPU-fallback
     * batches.
     */
    uint64_t rechargeCells() const noexcept { return recharge_cells_; }
    /**
     * @brief Query the extension's recorded worker-team size when that callback exists.
     * @return threads() result, otherwise zero for no callback. No positivity/capability
     * validation or parallel execution is performed by this getter. Callback must not throw
     * because this method is noexcept; caller ensures serialized callback/library lifetime.
     */
    int rechargeThreads() const noexcept {
        return recharge_backend_.threads ? recharge_backend_.threads() : 0;
    }

    /**
     * @brief Mesh mode: 1 where a [2D_AQUIFER] row (`*`, TAG or CELL) covers the cell. An
     * uncovered cell has no aquifer: no storage, no lateral faces, and its infiltration stays
     * with the surface model. All 1 in PER_SUBCATCH mode.
     *
     * @return Borrowed solver-owned byte mask, one entry per resolved cell; a nonzero value
     * means an authored aquifer receiver covers it. Initialization/replacement invalidates
     * element views. Does not measure current water/activation or rebuild coverage; requires
     * stable solver access.
     */
    const std::vector<uint8_t>& coverage() const noexcept { return covered_; }
    /**
     * @brief Test resolved aquifer coverage with an index guard.
     * @param[in] cell Zero-based cell; negative/out-of-range returns false.
     * @return Stored coverage byte is nonzero. Does not test water depth, pending capacity or
     * validate model arrays. Read-only and allocation-free.
     */
    bool covers(int cell) const noexcept {
        return cell >= 0 && static_cast<std::size_t>(cell) < covered_.size() &&
               covered_[static_cast<std::size_t>(cell)] != 0;
    }
    /**
     * @brief Borrow mutable canonical SI hydraulic state and accumulators.
     * @return Solver-owned SubsurfaceState reference. Caller retains no ownership and must
     * preserve prepared shapes, identity, receipt phase and matched hydraulic/quality/thermal
     * invariants. Mutations require exclusive solver access and appropriate owner/cache
     * reconstruction before further firings. Initialization, restore or solver replacement can
     * replace contained vectors and invalidate views; this getter performs no validation,
     * allocation or synchronization.
     */
    SubsurfaceState& state() noexcept { return state_; }
    /**
     * @brief Borrow read-only canonical SI hydraulic state and accumulators.
     * @return Solver-owned const SubsurfaceState reference; owner must remain alive/stable
     * during access. Initialization, restore or replacement can replace contents/invalidate
     * element views. No copy, validation, allocation or receipt consumption occurs.
     */
    const SubsurfaceState& state() const noexcept { return state_; }

    // ---- T7.1: species / age / enthalpy -----------------------------------

    /**
     * @brief The transported tuple, sized from `TransportPolicy` at `initTransport` and inert
     * (`active() == false`) until then.
     *
     * @details Borrow mutable native species/age/temperature transport inventories and ledgers.
     * @return Solver-owned SubsurfaceTransportState reference. Caller retains no ownership and
     * must preserve prepared shapes, identity, receipt phase and matched
     * hydraulic/quality/thermal invariants. Mutations require exclusive solver access and
     * appropriate owner/cache reconstruction before further firings. Initialization, restore or
     * solver replacement can replace contained vectors and invalidate views; this getter
     * performs no validation, allocation or synchronization.
     */
    SubsurfaceTransportState& transport() noexcept { return tr_; }
    /**
     * @brief Borrow read-only native species/age/temperature transport inventories and ledgers.
     * @return Solver-owned const SubsurfaceTransportState reference; owner must remain
     * alive/stable during access. Initialization, restore or replacement can replace
     * contents/invalidate element views. No copy, validation, allocation or receipt consumption
     * occurs.
     */
    const SubsurfaceTransportState& transport() const noexcept { return tr_; }
    /**
     * @brief Borrow mutable prepared physical-layer thermal and porewater owner.
     * @return Solver-owned GroundHeatState reference. Caller retains no ownership and must
     * preserve prepared shapes, identity, receipt phase and matched hydraulic/quality/thermal
     * invariants. Mutations require exclusive solver access and appropriate owner/cache
     * reconstruction before further firings. Initialization, restore or solver replacement can
     * replace contained vectors and invalidate views; this getter performs no validation,
     * allocation or synchronization.
     */
    GroundHeatState& heat() noexcept { return heat_; }
    /**
     * @brief Borrow read-only prepared physical-layer thermal and porewater owner.
     * @return Solver-owned const GroundHeatState reference; owner must remain alive/stable
     * during access. Initialization, restore or replacement can replace contents/invalidate
     * element views. No copy, validation, allocation or receipt consumption occurs.
     */
    const GroundHeatState& heat() const noexcept { return heat_; }
    /**
     * @brief Read signed unconsumed lateral physical heat for a cell.
     * - Argument cell (in): Prepared cell index, called i in source; invalid/absent heat mapping
     * returns zero.
     * @return Pending inward J, without consuming packets or changing canonical energy. Stable
     * matching edge/physical owner arrays are required; no allocation.
     */
    double pendingHeat(int cell) const noexcept;
    /**
     * @brief Initialize canonical porous thermal layers and paired producer/lateral receipt
     * storage.
     * @return Empty on success/disabled mode, otherwise unsupported ownership/material/contact
     * diagnostic.
     * @note Context and groundwater-config reference arguments are borrowed for subsequent
     * forcing; keep them alive/stable. Requires initialized hydraulic/native rows. Clears prior
     * thermal state and can throw or fail after partial mutation; initPorewater() separately
     * initializes non-temperature physical inventories. Definition documents units, ordering and
     * detailed side effects.
     */
    std::string initHeat(SimulationContext&, const GwTransportData&);
    /**
     * @brief Initialize canonical native inventories on fixed physical porous thermal layers.
     * @return Empty for success/inapplicable mode, otherwise layout/mapping initialization
     * diagnostic or a caught std::exception message. Nonstandard exceptions are not caught.
     * @note Requires initialized positive-count physical thermal layers, matching
     * hydraulic/native row identity and SI retention data. Nonlayered mode projects existing
     * SAT/UNSAT native inventories; SIGMA constructs overlap mapping (also for temperature-only
     * geometry) and initial physical mass. Resets outgoing preset/age/contact bookkeeping,
     * refreshes derived transport and temperature initial inventory where applicable. Mutates
     * cell-by-cell and can fail after partial replacement; caller stages initialization. Does
     * not create hydraulic water, change engine clock or take external ownership.
     */
    std::string initPorewater();
    /**
     * @brief Rebuild fixed-depth porewater overlap/reservations from accepted SIGMA state.
     * - Argument cell (in): Valid prepared cell index, called i in the definition.
     * @return Empty on success/skip or mapping diagnostic. May allocate/throw after partial
     * canonical or derived publication; requires exclusive matching physical/SIGMA owners. Does
     * not advance water/time.
     */
    std::string refreshSigmaPorewater(int cell);
    /**
     * @brief Refresh one cell's physical/SIGMA outgoing reservation view without propagating
     * exceptions.
     * @param[in] cell Valid prepared thermal cell index, unchecked even on failure handling.
     * @note Calls refreshSigmaPorewater(); diagnostic or exception marks
     * heat_.cells[cell].failed. A reported diagnostic is copied into error where possible;
     * catch-all only sets failed. Partial refresh changes remain. Caller checks failure before
     * adoption and provides exclusive owner access.
     */
    void refreshSigmaPorewaterReservations(int cell) noexcept;
    /**
     * @brief Rebuild the signed soil-side contact receipt from the canonical layer increments
     * after the mesh/channel synchronization has settled.
     *
     * @return Empty after rebuilding native signed gained_contact per species from
     * physical-layer contact_mass (temperature excluded), or diagnostic for incompatible
     * shape/nonfinite increment/sum.
     * @note Inactive/absent ledger returns. Layered mode refreshes canonical/derived views
     * first, then zeros and accumulates the ledger. A failure can leave partial views or a
     * partial rebuilt ledger; refreshView() exceptions propagate. Does not consume physical
     * contact receipts, book water or advance clocks. Requires serial stable physical/native
     * owner access.
     */
    std::string refreshPorewaterContactLedger();
    /**
     * @brief Size the rows and seed both stores from `[GW_INITIAL_QUALITY]`. Separate from
     * `initialize` because the row layout needs the resolved `[GW_*]` sections, which the router
     * owns. The layout names follow TransportPolicy order.
     * @param pollut_decay the engine's `[POLLUTANTS]` Kdecay column (1/s), row-aligned with the
     * layout — what a `[GW_SORPTION]` row with no DECAY of its own falls back to.
     *
     * @param[in] rows Resolved row counts, names and age/temperature indices in transport policy
     * order. Counts, metadata and referenced species must be consistent; layout is copied, not
     * retained.
     * @param[in] gw Optional resolved SI initial quality, dispersion, sorption and groundwater
     * options. Null selects defaults. Borrowed only during this call; scoped layer overrides
     * require SIGMA cells and are rejected for PER_SUBCATCH. Authored decay overrides are per
     * day, converted to 1/s.
     * @param[in,out] warnings Reserved shared initialization diagnostics; this implementation
     * does not append to or clear this vector. Resolution warnings are produced by the upstream
     * resolver.
     * @note Clears existing transport, heat and related bookkeeping before testing activation.
     * Inactive hydraulics or no rows leaves transport/heat empty. Otherwise allocates stores and
     * node receipts, seeds native concentration times water volume, applies layered retention
     * where configured, and records initial inventory. Caller initializes physical
     * heat/porewater separately afterwards. Requires exclusive access and a prepared
     * hydraulic/mesh identity. Allocation or invalid layered configuration can throw after
     * partial clearing/allocation; this operation is not transactional.
     */
    void initTransport(const RowLayoutLite& rows, const GwTransportData* gw,
                       const std::vector<double>& pollut_decay, std::vector<std::string>& warnings);
    /**
     * @brief Water volume of one cell's saturated zone (m³) — the denominator for a saturated
     * concentration.
     *
     * @param[in] cell Valid prepared cell index in [0,state_.n_cells); bounds are unchecked.
     * @return hg*theta_s*area in m3, with saturated retained plus drainable water. Does not
     * subtract pending exports or reservations, clamp negative inputs, or validate finite state.
     */
    double satVolume(int cell) const noexcept {
        const auto u = static_cast<std::size_t>(cell);
        return state_.hg[u] * state_.theta_s[u] * state_.area[u];
    }
    /**
     * @brief A gaining conduit draws `vol` m³ of water from `cell`: book the mass it carries, at
     * the cell's dissolved saturated concentration, for the cell to debit at its next firing,
     * and return it (native units). The mass is capped at what the cell holds less what is
     * already pending, so a cell whose saturated zone has emptied cannot export residual mass at
     * a concentration divided by a vanishing volume.
     *
     * @param[in] cell Zero-based prepared transport cell; invalid returns zero before booking.
     * @param[in] species Transport row index; invalid/inactive transport returns zero.
     * @param[in] vol Finite positive accepted conduit-gain water volume m3; nonpositive/NaN
     * returns zero.
     * @return Booked dissolved native inventory (native_concentration*m3); ordinary unsigned
     * rows are capped against mobile saturated mass less queued exports. Signed rows can return
     * signed values. Zero can still accompany thermal volume/reservation bookkeeping in an
     * active temperature row.
     * @note Requires consistent hydraulic/transport/thermal/porewater arrays and serial producer
     * access. Adds outgoing mass to lout_mass for the later cell gather rather than subtracting
     * canonical mass now. Active temperature also books producer-time heat/volume, and physical
     * porewater reservations are updated. Does not book the water volume itself; caller pairs
     * that separately. General finite input/result validation is absent and noexcept
     * callbacks/helpers must not throw.
     */
    double bookLinkGain(int cell, int species, double vol) noexcept {
        if (!tr_.active() || !(vol > 0.0) || cell < 0 || cell >= tr_.n_cells || species < 0 ||
            species >= tr_.n_species)
            return 0.0;
        const double v = satVolume(cell);
        if (!(v > 0.0))
            return 0.0;
        const auto u = static_cast<std::size_t>(cell);
        const auto k = tr_.idx(species, cell);
        const double mob = tr_.dissolvedFraction(species, cell, state_.theta_s[u]);
        double m = vol * (tr_.sat_mass[k] / v) * mob;
        if (heat_.active() && species == tr_.temp_row) {
            heat_link_out_volume_[u] += vol;
            bookPresetHeatOut(cell, vol, m);
        }
        if (!tr_.signedRow(species)) {
            const double avail =
                std::max(0.0, tr_.sat_mass[k] * mob - pendingSaturatedMassExport(cell, species));
            m = std::min(m, avail);
        }
        if (m == 0.0)
            return 0.0;
        bookPresetPorewaterOut(cell, species, m);
        tr_.lout_mass[k] += m;
        if (tr_.n_layers > 0 && heat_.active())
            refreshSigmaPorewaterReservations(cell);
        return m;
    }
    /**
     * @brief …and of its unsaturated column store (m³), whatever closure holds it.
     *
     * @param[in] cell Valid prepared cell index; all hu/area arrays must cover it.
     * @return state_.hu[cell]*area[cell] in m3. Uses the maintained bulk/derived hu field
     * directly, including SIGMA's maintained view; it does not integrate theta_sigma or include
     * queued receipts. No index, finite-value or physical-bounds checks occur.
     */
    double unsatVolume(int cell) const noexcept {
        const auto u = static_cast<std::size_t>(cell);
        return state_.hu[u] * state_.area[u];
    }
    /**
     * @brief The surface hands mass down with its infiltration (T7.1 seam): the mass the water
     * it booked through `bookInfiltrationFromSurface` was carrying, gathered when the GW cell
     * fires.
     *
     * @param[in] cell Prepared receiver cell index; invalid indices are ignored.
     * @param[in] species Prepared transport row index; invalid indices or inactive transport are
     * ignored.
     * @param[in] mass Native row inventory (concentration*m3), signed for signed rows. Zero is
     * ignored; no finite-value or sign validation is performed. Caller supplies an amount
     * consistent with the water receipt and row identity.
     * @note Adds to xacc_from_surface only; canonical inventory is gathered later. Does not book
     * water or set the shared pending flag. Requires allocated matching arrays and exclusive
     * access to the addressed cell/row slot; independent slots may be produced separately.
     */
    void bookInfiltrationMass(int cell, int species, double mass) noexcept;
    /**
     * @brief Internal reviewed-source ownership. Must bind before pending exchange.
     *
     * @return True when any reviewed source_owned_ flag is set. An allocated all-false mask is
     * not considered bound. Does not inspect pending receipts or current aquifer activation; no
     * mutation.
     */
    bool sourceReceiptOwnershipBound() const {
        return std::any_of(source_owned_.begin(), source_owned_.end(), [](bool x) { return x; });
    }
    /**
     * @brief Bind exclusive reviewed-source receipt ownership to selected receiver cells.
     * @param[in] cells Zero-based receiver cells; duplicates and an empty list are accepted.
     * Every listed cell must be in range and have no pending surface water or native exchange
     * inventory.
     * @return Empty string after binding flags and copying transport row names; otherwise a
     * diagnostic before any flag is set when ownership already exists or a cell fails
     * validation.
     * @note Requires prepared ownership/receipt arrays and serial pre-exchange configuration.
     * Does not clear old queues or book water. Copying species metadata may allocate after flags
     * are set, so an allocation exception can leave ownership bound. No whole-call rollback is
     * provided.
     */
    std::string configureSourceReceipts(const std::vector<int>& cells);
    void retainSourceReceipt(const SurfaceIntakeReceipt&);
    /**
     * @brief Borrow queued reviewed-source intake provenance for one receiver cell.
     * @param[in] cell Valid source_receipts_ vector index.
     * @return Const solver-owned receipt vector; water m3, times s, native species inventory.
     * New retention/firing/restore may invalidate elements. No receipt is consumed or copied.
     * @throws std::out_of_range On an invalid cell; caller provides stable solver access.
     */
    const std::vector<SurfaceIntakeReceipt>& pendingSourceReceipts(int cell) const {
        return source_receipts_.at(cell);
    }
    /**
     * @brief Read the receiver's completed reviewed-source horizon.
     * @param[in] cell Valid source_cell_end_ index.
     * @return Elapsed seconds stored for that cell, independent of the global source/engine
     * clock.
     * @throws std::out_of_range On invalid cell. No queue drain, validation or synchronization
     * occurs.
     */
    double sourceCompletedTime(int cell) const { return source_cell_end_.at(cell); }
    /**
     * @brief Borrow queued source-attributed refunds and physical emergence.
     * @return Const solver-owned return vector; availability times s, water m3, native transport
     * amounts and donor provenance where relevant. Valid during stable solver access;
     * acknowledgment, firing, retention or restore can change/invalidate elements. No queue
     * consumption occurs.
     */
    const std::vector<SurfaceReturnReceipt>& sourceReturns() const { return source_returns_; }
    /**
     * @brief Erase source return receipts whose availability horizon has been adopted.
     * @param[in] completed_through Inclusive elapsed-time horizon s. Caller supplies a valid
     * completed consumer horizon; no finiteness/sign validation occurs. NaN removes none,
     * positive infinity all.
     * @note Caller first delivers every qualifying receipt, then acknowledges serially. Erases
     * matching queue entries and invalidates their views without booking or undoing water/native
     * inventory.
     */
    void acknowledgeSourceReturns(double completed_through);
    /**
     * @brief …and takes back what saturation excess and rejection carried up. Drained by the
     * surface cell in the same call that takes the water.
     *
     * @param[in] cell Receiver cell; invalid/inactive transport returns zero.
     * @param[in] species Transport row; invalid returns zero.
     * @return Entire queued xacc_to_surface native inventory, then resets that slot to zero.
     * Signed rows may return signed amounts. No partial gather fraction, bounds validation of
     * prepared arrays, water removal or finite-value check occurs. Pair once with the
     * corresponding surface water drain; independent slots may be drained separately, but
     * competing access to the same slot is unsafe.
     */
    double takeToSurfaceMass(int cell, int species) noexcept;
    /**
     * @brief Borrow the solver's resolved groundwater option copy.
     * @return Const solver-owned GwOptions, replaced on initialization/restore as applicable.
     * Numeric runtime options follow their documented SI conventions; this does not return
     * mutable authored rows or recompute automatic selections. No allocation or state change
     * occurs.
     */
    const GwOptions& options() const noexcept { return options_; }

    // ---- LTS hooks -------------------------------------------------------

    /**
     * @brief Per-cell stability step `min(Δt_g, Δt_u)`, written into `state_.dt_cell`. Called
     * once per rebuild; cheap enough to be unconditional (the plan's "they rarely bind").
     *
     * @details Recompute each active groundwater cell's explicit water/transport/thermal
     * stability duration.
     * @param[in] mesh Stable SI centroids for dispersion gradients, matching prepared hydraulic
     * cells.
     * @param[in] edges Matching saturated-flow CSR/face geometry; PER_SUBCATCH may have no
     * lateral faces.
     * @note Inactive returns. Writes dt_cell s from minimum saturated Darcy/boundary/node/deep
     * sink limit, closure-specific unsaturated relaxation/SIGMA bound, optional
     * skew/tensor-dispersion bound and thermal maximum/lateral limits. Uses current
     * moisture/table and previous q0_last for column motion. Does not assign tiers, fire
     * physics, settle receipts or advance clocks. Requires consistent parallel arrays, finite
     * resolved controls and exclusive state access; forcing-series ordinates are borrowed read
     * only.
     */
    void refreshDtCell(const MeshData& mesh, const InertialEdges& edges);
    /**
     * @brief G1-c (2026-09-19): the smallest per-cell stability step after `refreshDtCell` (s),
     * or 1e30 s when no stored value is smaller — the marcher folds it into the ladder's base
     * step so a groundwater cell whose own bound is finer than every active surface cell's does
     * not fire beyond it.
     *
     * @return Minimum stored dt_cell value below the initial 1e30 s sentinel; NaNs do not reduce
     * the minimum. An empty vector returns 1e30 s, not infinity. Caller refreshes dt_cell first;
     * this getter does not validate signs/finiteness, recompute stability or change any tier.
     */
    double minDtCell() const noexcept {
        double m = 1.0e30;
        for (const double v : state_.dt_cell)
            if (v < m)
                m = v;
        return m;
    }

    /**
     * @brief Assign tiers from `dt_cell` against the marcher's `dt0` and tier count, and rebuild
     * the per-tier cell/face lists. **G-A:** no surface quantity enters this.
     *
     * @param[in] dt0 Finite positive base duration s. Nonpositive selects tier zero; no general
     * finite validation occurs. Prepared dt_cell values govern floor(log2(dt_cell/dt0)) where
     * usable.
     * @param[in] n_tiers Requested tier count, raised to at least one. Must fit stored uint8_t
     * cell/face tiers and caller scheduler shifts; this method does not impose an upper cap.
     * @note Inactive returns. Replaces cell/face tier lists, resets tier firing counters,
     * assigns covered cells to bounded binary tiers and covered-covered faces to the finer
     * endpoint tier. Leaves cross- cadence receipts in their stable CSR slots; does not fire or
     * consume them. Allocations can fail after partial list replacement. Requires exclusive
     * access and consistent prepared topology/coverage.
     */
    void assignTiers(double dt0, int n_tiers);

    /**
     * @brief Minimum tier count the ladder needs so it reaches the slowest GW cell (`K ≥ 1 +
     * ⌈log2(Δt_gw_max/dt0)⌉`, G-A point 3). The marcher grows its ladder to this before
     * assigning.
     *
     * @param[in] dt0 Finite positive candidate base step s; inactive/nonpositive returns one.
     * @return max(1,ceil(log2(max(dt_cell)/dt0))+1), enough binary tiers to cover the largest
     * stored limit. Nonfinite maximum or maximum<=dt0 returns one. No maximum cap is applied;
     * representability of the log ratio as int is a caller precondition. Does not refresh
     * limits, assign tiers or mutate state.
     */
    int requiredTiers(double dt0) const noexcept;

    /**
     * @brief Lateral Darcy on the faces of one tier; books ±ΔV into `eacc_L/R`.
     *
     * @param[in] tier Selected stored face tier; inactive or absent tier returns after any
     * required physical/SIGMA view refresh. Negative/out-of-range tier is handled by
     * tierHasFaces().
     * @param[in] dt Finite positive accepted firing duration s; required by caller, not
     * validated here.
     * @note Books signed L/R water m3 at donor-thickness Darcy rate with closure-consistent
     * unreserved water caps. Native dissolved upwind inventory and optional tensor dispersion
     * use paired side ledgers; signed temperature avoids a nonnegative-mass cap and porous heat
     * excludes duplicate molecular flux. Also produces lateral heat, physical porewater/thermal
     * outgoing reservations and limiter diagnostics. Canonical cell water/species are gathered
     * later. Reviewed LID budgets/mapping may throw after earlier face reservations/bookings; no
     * whole-list rollback. Requires stable geometry/material/arrays and serial producer access.
     * A thermal owner failure may be flagged instead of thrown; caller checks it.
     */
    void fireGwFaces(int tier, double dt);
    /**
     * @brief Gather + closure + node/deep/ET/Dunne for the cells of one tier. accepted_end is
     * the common marcher's exact completed endpoint; owned cells start at their last completed
     * firing, without rounded clock gaps.
     *
     * @details Fire all cells in one due groundwater tier at their accepted ownership horizon.
     * @param[in] tier Stored tier index; inactive/absent tier returns after updating
     * source_firing_end_.
     * @param[in] dt Finite positive ordinary firing duration s.
     * @param[in,out] surf Matching surface state/recipient, mutated by accepted cell
     * water/species/heat.
     * @param[in] time Finite ordinary start s.
     * @param[in] accepted_end Negative selects time+dt, otherwise exact completed end s for
     * reviewed source/LID cells using their own previous completion. Must advance every selected
     * owner.
     * @throws std::runtime_error Richards-owned beds lack enabled joint LID receipt acceptance,
     * or a cell/forcing/thermal preflight rejects later work.
     * @note Invokes fireGwCellList and increments tier telemetry only after success. Earlier
     * subintervals can remain mutated on failure; caller stages the larger transaction. Requires
     * exclusive prepared solver/coupled-owner access; does not directly advance canonical 1D
     * nodes.
     */
    void fireGwCells(int tier, double dt, SurfaceStateData& surf, double time = 0.0,
                     double accepted_end = -1.0);
    /**
     * @brief Borrow mutable copied runtime source prescriptions, receipts and derived lookups.
     * @return Solver-owned RuntimeSources reference. Caller retains no ownership and must
     * preserve prepared shapes, identity, receipt phase and matched hydraulic/quality/thermal
     * invariants. Mutations require exclusive solver access and appropriate owner/cache
     * reconstruction before further firings. Initialization, restore or solver replacement can
     * replace contained vectors and invalidate views; this getter performs no validation,
     * allocation or synchronization.
     */
    RuntimeSources& runtimeSources() noexcept { return runtime_sources_; }
    /**
     * @brief Borrow read-only copied runtime source prescriptions, receipts and derived lookups.
     * @return Solver-owned const RuntimeSources reference; owner must remain alive/stable during
     * access. Initialization, restore or replacement can replace contents/invalidate element
     * views. No copy, validation, allocation or receipt consumption occurs.
     */
    const RuntimeSources& runtimeSources() const noexcept { return runtime_sources_; }
    /**
     * @brief Replace validated SI exterior boundaries and rebuild their per-cell
     * lookup/stability.
     * @note The by-value vector is moved into the solver; all row cell indices must be valid
     * prepared IDs. Prior boundary receipts are replaced. Can allocate/throw after partial
     * replacement; requires exclusive access and no pending reconfiguration-sensitive exchange.
     * Definition details its argument.
     */
    void setBoundaries(std::vector<GwResolvedBoundary>);
    /**
     * @brief Borrow resolved exterior groundwater hydraulic/quality boundary descriptors and
     * receipts.
     * @return Solver-owned const vector, in configured order. Geometry is SI, signal time
     * elapsed s, water ledgers m3 and transport inventory native units. setBoundaries(),
     * initialize/restore and firings can replace/change elements; no copy or forcing integration
     * occurs in this getter.
     */
    const std::vector<GwResolvedBoundary>& boundaries() const noexcept { return boundaries_; }
    // One forward-Euler evaluation within the owned shared RK2 transaction.
    // Prescribed forcing is integrated without additional water substeps.
    /**
     * @brief Run a tier-zero groundwater Euler evaluation within the shared RK2 transaction.
     * - Argument dt (in): Finite positive stage duration s.
     * - Argument time (in): Finite elapsed stage start s.
     * @note The surface reference is the writable matching staged owner. Requires prevalidated
     * RK2 profile and enclosing rollback/staging; exceptions may follow partial
     * water/species/heat changes. Definition documents source proposal preflight and completion
     * telemetry.
     */
    void fireGwRk2Cells(double dt, SurfaceStateData&, double time);
    /**
     * @brief Advance independent ground conduction with RK2 at a held thermal forcing sample
     * time.
     * @param[in] seconds Finite positive solve interval s; inactive/nonpositive returns,
     * positive infinity throws. NaN is skipped by the nonpositive test rather than explicitly
     * rejected.
     * @param[in] sample_time Finite elapsed forcing horizon s, interpolated without changing
     * table cursors.
     * @note Samples heat forcing once, conducts every covered unowned physical column and
     * refreshes its derived native views. Surface-owned columns are skipped by conduct().
     * Refusal throws after earlier cells/caches may have changed; enclosing coupled transaction
     * stages rollback. Allocates thermal work.
     */
    void advanceRk2Heat(double seconds, double sample_time);
    /**
     * @brief Replace resolved project aquifer sources and rebuild per-cell weighted lookup.
     * @param[in] sources Owned resolved SI source definitions, moved into solver. Invalid cell
     * entries are skipped, while source signal/species shapes and weights must already be
     * validated.
     * @note Clears runtime_sources_ including callbacks/receipts, replaces sources_ and
     * cell_sources_. Does not clear pending physical water/native packets, recalculate tiers or
     * initialize runtime driver bindings. Allocations may leave a partial mapping; reconfigure
     * serially before new source work.
     */
    void setSources(std::vector<GwResolvedSource> sources);
    /**
     * @brief Compact drained surface-return bookkeeping before scheduler transitions. Pending
     * edge/node water remains owned until the next paired cell firing.
     *
     * @param[in,out] surf Retained interface argument, unused; canonical surface state is
     * unchanged.
     * @note Compacts the pending surface list and clears accumulators_pending_ only. It
     * deliberately does not turn pending water into table height or consume water/native/heat
     * packets: the paired moving unsaturated/saturated update belongs to fireCell().
     * Continuity/checkpoints must include outstanding accumulators. Requires serial bookkeeping;
     * no allocation or physical firing occurs.
     */
    void settle(SurfaceStateData& surf);
    /**
     * @brief Rebuild pending-surface cell flags/list from positive xacc_to_surface water.
     * @note Clears prior list and flags, then marks each prepared cell whose queued outward
     * volume is positive. Requires pending_flag_ and xacc_to_surface sized for state_.n_cells.
     * Does not drain water or alter canonical storage. Serial rebuild invalidates list element
     * views; markPendingSurface() is noexcept but its vector push may allocate, so allocation
     * failure there terminates the process.
     */
    void restorePendingSurface() {
        pending_surface_.clear();
        std::fill(pending_flag_.begin(), pending_flag_.end(), 0);
        for (int i = 0; i < state_.n_cells; ++i)
            if (state_.xacc_to_surface[i] > 0.0)
                markPendingSurface(i);
    }

    /**
     * @brief True when the tier's cell list is non-empty.
     *
     * @details Test whether one stored tier has a nonempty firing list.
     * @param[in] tier Zero-based tier index; negative/out-of-range returns false.
     * @return Nonempty cells_by_tier_ entry. Does not recompute tiers, validate indices inside
     * the list or imply the tier is currently due; allocation-free and read-only.
     */
    bool tierHasCells(int tier) const noexcept {
        return tier >= 0 && tier < static_cast<int>(cells_by_tier_.size()) &&
               !cells_by_tier_[static_cast<std::size_t>(tier)].empty();
    }
    /**
     * @brief Test whether one stored tier has a nonempty firing list.
     * @param[in] tier Zero-based tier index; negative/out-of-range returns false.
     * @return Nonempty faces_by_tier_ entry. Does not recompute tiers, validate indices inside
     * the list or imply the tier is currently due; allocation-free and read-only.
     */
    bool tierHasFaces(int tier) const noexcept {
        return tier >= 0 && tier < static_cast<int>(faces_by_tier_.size()) &&
               !faces_by_tier_[static_cast<std::size_t>(tier)].empty();
    }
    /**
     * @brief Read the number of stored groundwater cell-tier lists.
     * @return cells_by_tier_.size() converted to int; prepared tier counts must fit int. Does
     * not validate the matching face lists or initialize/retier the solver.
     */
    int tierCount() const noexcept { return static_cast<int>(cells_by_tier_.size()); }
    /**
     * @brief Per-tier firing counts — the G-A telemetry (`swmm_gw2d_get_tier_histogram`).
     *
     * @return Borrowed solver-owned per-tier firing counters in tier order, reset/replaced by
     * owner initialization/tier management. Not wall-clock time or face-count telemetry. Stable
     * read access requires no concurrent firing or retiering; no copy/allocation/reset occurs
     * here.
     */
    const std::vector<long>& tierFirings() const noexcept { return tier_firings_; }

    // ---- surface ↔ subsurface --------------------------------------------

    /**
     * @brief The surface books infiltration it delivered to cell `i` (m³) at its own (finer)
     * cadence; the GW cell gathers it when it fires.
     *
     * @param[in] cell Prepared covered cell; inactive/uncovered returns without booking.
     * @param[in] vol_m3 Signed pending intake volume m3, added directly without capacity, sign
     * or finite validation. Caller first establishes a physically accepted amount and paired
     * composition.
     * @note Changes only this cell's xacc_from_surface. No shared flag, wetting front, native
     * mass or canonical storage is changed. Independent cells can be booked concurrently;
     * competing producers must serialize access to the same slot.
     */
    void bookInfiltrationFromSurface(int cell, double vol_m3) noexcept;
    /**
     * @brief Publish the aquifer's held rate; publication never restores spent storage.
     *
     * @details Refresh published intake capacity and available volume for every groundwater
     * cell.
     * @param[in,out] surf Surface state receiving infil_rate where that array covers a cell.
     * Missing surface depths are treated as zero ponding; the remaining groundwater arrays must
     * be prepared.
     * @param[in] interval Positive publication duration s. Inactive solver or nonpositive/NaN
     * duration returns without work; caller supplies a finite value for meaningful volume/rate
     * conversion.
     * @param[in] time Elapsed refresh timestamp s, stored without validation.
     * @note Updates per-cell infil_remaining, infil_capacity, infil_refresh and infil_interval.
     * Does not accept water or advance either solver. Caller serializes publication with all
     * producers.
     */
    void publishInfiltration(SurfaceStateData& surf, double interval, double time);
    /**
     * @brief Reserve and book once, BEFORE the sender removes water (SI m3). Producers for one
     * cell run serially, as for the existing accumulators.
     *
     * @details Reserve a valid surface intake against current groundwater receiver headroom.
     * @param[in] cell Prepared covered receiver cell; inactive/uncovered returns zero.
     * @param[in] requested Finite positive requested water volume m3; invalid returns zero.
     * @param[in] prescribed True bypasses the conductivity/published-capacity admission gate.
     * False requires both Ks and a finite positive published infil_capacity; the published rate
     * is a gate, not an additional volume-over-time limit in this method.
     * @return min(requested,infiltrationHeadroom(cell)), m3, added to xacc_from_surface and
     * wetting_front as depth. Also refreshes infil_remaining; no canonical storage, native mass
     * or receipt is booked.
     * @note Requires positive prepared area and exclusive access to this cell. Caller pairs the
     * accepted water with species/provenance and updates surface storage; no whole coupled
     * transaction occurs.
     */
    double acceptSurfaceInfiltration(int cell, double requested, bool prescribed = false) noexcept;
    /**
     * @brief Compute current unreserved unsaturated receiver capacity.
     * @param[in] cell Zero-based receiver cell; inactive or uncovered cells return zero.
     * @return Nonnegative unreserved receiver capacity m3. Uses closure-specific unsaturated
     * storage; SIGMA also limits admission to the top layer's current deficit. Subtracts gross
     * pending surface, link, lateral and LID/node inflows without cancelling outgoing promises
     * against incoming water.
     * @note Requires stable prepared cell, edge and bed arrays. Read-only, allocation-free and
     * does not anticipate drainage, future provider receipts or refresh published infiltration
     * rates.
     */
    double infiltrationHeadroom(int cell) const noexcept;
    /**
     * @brief Read-only soil/interface rate (m/s) for the actual donor pond head. Group headroom
     * is allocated separately; no front or held rate advances.
     *
     * - cell: Covered receiver cell, called i in the definition.
     * - pond_m: Finite nonnegative ponding depth m, called pond in the definition.
     * - interval_s: Finite positive intake duration s, called interval in the definition.
     * @return Nonnegative closure-dependent intake rate m/s, before receiver
     * headroom/reservations. Zero on inactive/uncovered/invalid input or nonfinite result; no
     * mutation occurs.
     */
    double sourceInfiltrationCapacity(int cell, double pond_m, double interval_s) const noexcept;
    /**
     * @brief G-X3 (2026-09-19): the router books a conduit's seepage volume (m³, SI, its
     * length-weighted share for this cell) at the routing cadence; the GW cell gathers it at its
     * firing as a saturated-zone inflow.
     *
     * @param[in] cell Prepared covered cell; inactive/uncovered returns without booking.
     * @param[in] vol_m3 Signed water volume m3, positive conduit-to-aquifer. No sign,
     * finite-value, availability or headroom validation is performed; caller has accepted and
     * paired the exchange.
     * @note Adds lacc, reserves gross layered contact_link_out for negative volumes, and marks
     * shared accumulators pending. Canonical stores/mass are gathered later. Requires serial
     * producer access, including the shared pending flag; not a thread-safe cell-only producer
     * interface.
     */
    void bookLinkSeepage(int cell, double vol_m3) noexcept;
    /**
     * @brief Accepted leaking reach, with a linear bed elevation over its clipped length. Splits
     * water and producer quality at actual SIGMA interfaces.
     *
     * @param[in] cell Prepared covered receiver; inactive/uncovered returns without work.
     * @param[in] volume Positive accepted conduit intake volume m3; nonpositive/NaN returns
     * immediately, positive infinity is rejected by validation.
     * @param[in] concentration Borrowed native concentrations in exact transport row order. Size
     * must match n_species; all values finite and unsigned rows nonnegative. Not retained.
     * @param[in] elevation_begin Finite SI elevation m of one end of the conduit contact
     * segment.
     * @param[in] elevation_end Finite SI elevation m of the other end; equal elevations are
     * allowed.
     * @throws std::invalid_argument Invalid positive receipt volume, composition or endpoint.
     * @note Splits the accepted volume at SIGMA layer boundaries, books total link water, then
     * places paired mass/water into layer or saturated contact ledgers. Does not debit the
     * conduit or change canonical groundwater storage. Temporary cuts may allocate before
     * booking. Requires prepared geometry/ledger shapes and serial producer access; receipts are
     * not rolled back by this helper.
     */
    void bookLinkContact(int cell, double volume, const std::vector<double>& concentration,
                         double elevation_begin, double elevation_end);
    /**
     * @brief Locate a conduit contact in the current unsaturated SIGMA layers.
     * @param[in] cell Covered prepared cell; invalid/uncovered or nonlayered returns -1.
     * @param[in] elevation Finite contact elevation m in the groundwater vertical datum.
     * @return Zero-based unsaturated layer, clamped to [0,n_layers-1], or -1 for elevation
     * at/below the water table, no positive unsaturated length, or invalid input. Elevations
     * above the ground select the top layer. No ledger mutation or allocation occurs.
     */
    int contactLayer(int cell, double elevation) const noexcept;
    /**
     * @brief Volume (m³) the subsurface owes the surface twin — Dunne and exfiltration — drained
     * by the surface at its firing.
     *
     * @param[in] cell Prepared receiver cell; inactive/invalid returns zero.
     * @return Entire pending outward water volume m3, then zeros xacc_to_surface and clears its
     * pending flag. Does not erase from the shared pending list, modify canonical water or drain
     * native mass.
     * @note Independent cells may drain concurrently. Pair once with native mass delivery, then
     * compact the pending list serially; repeated draining returns zero until another producer
     * books water.
     */
    double takeToSurface(int cell) noexcept;
    /**
     * @brief Cells with pending surface-bound water, so the marcher can pin them active (a dry
     * surface cell above an exfiltrating column must route). Call `compactPending()` first —
     * `takeToSurface` clears flags but does not erase from this list, because it runs inside the
     * parallel cell loop.
     *
     * @return Borrowed solver-owned cell list, potentially containing already drained entries
     * until compactPending(). List mutation/restore/initialization invalidates element views.
     * Water receipts are not consumed by this query; caller serializes list rebuild with
     * firings.
     */
    const std::vector<int>& pendingSurfaceCells() const noexcept { return pending_surface_; }
    /**
     * @brief Drop drained cells from `pendingSurfaceCells()`. Serial; call from the marcher's
     * rebuild, not from a firing.
     *
     * @details Remove pending-surface list entries whose cell flags were cleared by drains.
     * @note Serial list maintenance after parallel per-cell takeToSurface() calls. Does not
     * book, consume or recompute water; flagged entries remain. Requires valid prepared cell
     * indices/flags and no concurrent list/flag mutation. Erasure invalidates list element
     * views; no allocation.
     */
    void compactPending() noexcept;

    /**
     * @brief T7.4: the node's own species row, `[node * n_species + s]`, published by the router
     * each batch — what a RECHARGING node's water carries down its bed. Null (or a zero row
     * count) means the seam moves water only, which is what every pre-T7.4 deck did.
     *
     * @details Copy current canonical 1D composition into a stable groundwater producer tuple
     * buffer.
     * @param[in] ctx Prepared matching node pollutant/MSX/age/heat state. MSX is matched by
     * exact row name; missing optional backing entries yield zero. Age remains seconds and
     * temperature degC.
     * @note Replaces node_concentrations_ in node-major [node*n_species+row] order, then binds
     * the solver's borrowed node_row_conc_ to this owned buffer. Invalidates prior buffer views.
     * Allocates and can throw before rebinding; caller must not use a stale prior pointer after
     * a failed replacement. Does not advance nodes, book water/native mass or change external
     * context; requires serial producer setup.
     */
    void publishNodeConcentrations(const SimulationContext& ctx);
    /**
     * @brief Borrow node-major producer composition for subsequent node/aquifer exchanges.
     * @param[in] rows Optional contiguous buffer [node*n_species+species] covering every
     * referenced 1D node. Values use each row's native concentration, age or temperature units.
     * Null disables pairing.
     * @param[in] n_species Published row stride/count; zero disables pairing, otherwise must
     * match the prepared transport identity. Negative values and buffer extents are not
     * validated.
     * @note Stores the pointer without copying/ownership. Caller keeps data alive/unchanged
     * until replaced or all consuming node samples complete. No receipts, water or species are
     * booked here.
     */
    void setNodeRowConc(const double* rows, int n_species) noexcept {
        node_row_conc_ = rows;
        node_row_ns_ = n_species;
    }
    /**
     * @brief T7.4: mass the aquifer owes each 1D node this batch (`[s * n_nodes + node]`, + out
     * of the aquifer), flushed by the router into the node's coupling queues alongside the
     * volume.
     *
     * @details Borrow per-batch node transport export in species-major [species*n_nodes+node]
     * order.
     * @return Solver-owned const vector in native_concentration*m3, signed positive out of the
     * aquifer. No copy, reset or receipt consumption occurs; initialization/restore/owner
     * bookkeeping can change contents and invalidate element views. Caller keeps the solver
     * stable and uses the matching node/bed/transport identity.
     */
    const std::vector<double>& nodeExchangeMass() const noexcept { return tr_.node_out_mass; }
    /**
     * @brief T7.4: mass a leaking conduit delivered into one cell, at the conduit's own
     * concentration — the species twin of `bookLinkSeepage`.
     *
     * @param[in] cell Prepared receiver cell index; invalid indices are ignored.
     * @param[in] species Prepared transport row index; invalid indices or inactive transport are
     * ignored.
     * @param[in] mass Native row inventory (concentration*m3), signed for signed rows. Zero is
     * ignored; no finite-value or sign validation is performed. Caller supplies an amount
     * consistent with the water receipt and row identity.
     * @note Adds to lacc_mass only; canonical inventory is gathered later. Does not book water
     * or set the shared pending flag. Requires allocated matching arrays and exclusive access to
     * the addressed cell/row slot; independent slots may be produced separately.
     */
    void bookLinkSeepageMass(int cell, int species, double mass) noexcept;

    /**
     * @brief Node ↔ aquifer exchange, evaluated at tier-0 cadence against the batch-frozen 1D
     * heads and booked into `nacc` (G-B row 3). The GW cell gathers its share when IT fires —
     * the node is never pinned to the aquifer's tier and the aquifer is never pinned to tier 0.
     * @param nodes 1D node data, or nullptr when nothing is coupled.
     *
     * @param[in] dt Finite positive sampling duration s, required by the caller; no duration
     * validation is performed here. Caps use max(dt,kTiny), whereas accepted volume uses Q*dt
     * directly.
     * @param[in] end Elapsed sample end time s for mobile-LID diagnostics; a negative value
     * selects the receiver cell's completed time. Does not advance the cell or node clock.
     * @note Requires stable, prepared node parallel arrays and solver bed/cell/transport shapes.
     * Node heads/held volume/inflow use the 1D engine units and are converted to SI; producer
     * concentration is borrowed from setNodeRowConc(). Positive water is aquifer-to-node.
     * Reserves gross donor water, receiver capacity and node held-water shares, books nacc and
     * paired incoming native inventory, and updates diagnostics. Canonical aquifer storage and
     * 1D node state are changed by later gathers. Inactive hydraulics, absent beds or null nodes
     * returns without work. Beds owned by Richards are skipped. Caller serializes samples and
     * resets only the appropriate batch ledgers between advances. A rejected reviewed node
     * budget throws std::runtime_error; prior beds' bookings remain applied.
     */
    void sampleNodeExchange(const NodeData* nodes, double dt, double end = -1);

    /**
     * @brief Per-node exchange volume this advance (m³ SI, + out of the aquifer into the pipe).
     * The router books it into `nodes.coupling_volume` through the same `flow_2d_to_1d` factor
     * the surface exchange uses.
     *
     * @details Borrow per-batch water exchange per 1D node.
     * @return Solver-owned const vector in m3, signed positive into the 1D node. No copy, reset
     * or receipt consumption occurs; initialization/restore/owner bookkeeping can change
     * contents and invalidate element views. Caller keeps the solver stable and uses the
     * matching node/bed/transport identity.
     */
    const std::vector<double>& nodeExchangeVolumes() const noexcept { return node_exchange_vol_; }
    /**
     * @brief Zero the per-node ledger at the start of a routing batch.
     *
     * @details Clear the per-batch node water exchange and held-node-water draw reservations.
     * @note Zeros node_exchange_vol_ and node_drawn_gw_ only. Does not clear transport mass, bed
     * accumulators, cumulative diagnostics or canonical storage. Call serially at the batch
     * boundary, before samples that must share the same node draw budget; no allocation occurs.
     */
    void resetNodeExchangeVolumes() noexcept;
    /**
     * @brief G-O: the `[2D_AQUIFER_NODE]` beds as resolved (cell index filled), in the authored
     * order — parallel to SurfaceRouter2D::aquiferNodeNames().
     *
     * @details Borrow resolved authored node/aquifer beds.
     * @return Solver-owned const vector in SI conductivity/thickness/area and zero-based
     * node/cell indices. No copy, reset or receipt consumption occurs;
     * initialization/restore/owner bookkeeping can change contents and invalidate element views.
     * Caller keeps the solver stable and uses the matching node/bed/transport identity.
     */
    const std::vector<GwNodeBed>& nodeBeds() const noexcept { return node_beds_; }
    /**
     * @brief G-O: cumulative exchange per bed (m³, + out of the aquifer into the pipe) since the
     * start — the results-file series behind the GUI's GwExchange plot. Never reset.
     *
     * @details Borrow cumulative signed exchange per resolved node bed.
     * @return Solver-owned const vector in m3, positive out of aquifer. No copy, reset or
     * receipt consumption occurs; initialization/restore/owner bookkeeping can change contents
     * and invalidate element views. Caller keeps the solver stable and uses the matching
     * node/bed/transport identity.
     */
    const std::vector<double>& bedExchangeCumulative() const noexcept { return bed_exchange_cum_; }

    // ---- G-W1b: what a gate needs to prove its own deck is an instrument --
    //
    // A gate that measures a cap must first establish that the cap BOUND on
    // the deck it is measuring, or it can pass with the defect present. The
    // ratio between the two specific yields does not establish that — a deck
    // scoring 56.8× was measured bit-identical with the bug — because what
    // makes the cap bind is `h_g · Sy · A` being small in absolute terms.
    // Only the clamp knows. These three say so directly, with no threshold
    // to tune and no geometry that can make them lie.

    /**
     * @brief Firings on which the aquifer→node drain cap actually clamped `Q`. Zero means the
     * deck cannot see anything this cap does.
     *
     * @return Stored count of node-drain cap binding events since owner initialization. This is
     * direct diagnostic telemetry, not a tolerance-derived estimate; no state or counter reset
     * occurs.
     */
    long drainCapBinds() const noexcept { return drain_cap_binds_; }
    /**
     * @brief Firings on which a column went short and the node's share of the overdraw was
     * refunded — into the aquifer's books alone, after the router had already paid the node. The
     * defect's signature: 124 before G-W1, 0 after.
     *
     * @return Stored count of node-share shortfall refunds since initialization. Reads
     * historical solver diagnostic state only; does not prove all current transfers/balances are
     * valid.
     */
    long nodeRefunds() const noexcept { return node_refunds_; }
    /**
     * @brief The specific yield the column ACTUALLY uses for `cell`, from the one definition.
     * Exposed so a test never re-derives it: re-deriving only the CLOSED_FORM branch (as the
     * first version of the G-W1b guard did) silently computes a number the solver never uses
     * under SIGMA or ENSLAVED, and agrees only for as long as every fixture is closure A.
     *
     * @param[in] cell Prepared hydraulic cell; negative/out-of-range hg index returns zero.
     * Remaining parallel cell/material arrays must have matching extents and valid closure
     * parameters.
     * @return Closure-specific dimensionless specific yield with its positive kSyFloor lower
     * bound, using max(zs-hg,0) as unsaturated length. No receipt reservations are subtracted
     * and no storage, clock or diagnostic changes occur.
     */
    double specificYieldOf(int cell) const noexcept;
    double unreservedDrainableWater(int cell) const noexcept;
    double unreservedDrainableWater(int cell, double specific_yield) const noexcept;
    double pendingSaturatedMassExport(int cell, int species) const noexcept;
    // Internal R5 trial budget in SI, before accepting a new node receipt.
    // Separate ceilings; incoming pending water never finances a withdrawal.
    // Call after atmospheric demand/other accepted channels have been booked.
    struct NodeWaterBudget {
        double head = 0, donor = 0, receiver = 0;
    };
    /**
     * @brief Read a validated finite node-trial water budget for one aquifer cell.
     * - Argument cell (in): Prepared active cell ID.
     * - Argument interval (in): Finite positive trial duration s.
     * @return Empty after replacing the borrowed NodeWaterBudget argument (head m,
     * donor/receiver m3), otherwise diagnostic with output unchanged. Definition documents
     * supported physics/reservations; requires stable prepared ownership and does not book water
     * or advance clocks.
     */
    std::string nodeWaterBudget(int cell, double interval, NodeWaterBudget&) const;
    /**
     * @brief Read one groundwater cell's accepted firing horizon.
     * @param[in] cell Valid cell_completed_ index.
     * @return Elapsed completed time s, independent of pending producer/source clocks.
     * @throws std::out_of_range On invalid cell; no synchronization or firing occurs.
     */
    double cellCompletedTime(int cell) const { return cell_completed_.at(cell); }
    /**
     * @brief Borrow cumulative accepted LID-to-aquifer water per resolved bed.
     * @return Solver-owned const vector in m3, gross nonnegative down. No copy, reset or receipt
     * consumption occurs; initialization/restore/owner bookkeeping can change contents and
     * invalidate element views. Caller keeps the solver stable and uses the matching
     * node/bed/transport identity.
     */
    const std::vector<double>& richardsBedDownCumulative() const noexcept {
        return richards_down_cum_;
    }
    /**
     * @brief Borrow cumulative accepted aquifer-to-LID water per resolved bed.
     * @return Solver-owned const vector in m3, gross nonnegative up. No copy, reset or receipt
     * consumption occurs; initialization/restore/owner bookkeeping can change contents and
     * invalidate element views. Caller keeps the solver stable and uses the matching
     * node/bed/transport identity.
     */
    const std::vector<double>& richardsBedUpCumulative() const noexcept { return richards_up_cum_; }
    // Hydraulic columns book completed intervals; groundwater consumes them
    // only at its own firing. Gross reservations cannot cancel each other.
    struct PendingLidBed {
        double start = 0, end = 0, down = 0, up = 0, down_j = 0, up_j = 0;
        bool heat_applied = false;
    };
    /**
     * @brief Bind reviewed Richards and mobile-LID water/optional-temperature receipt ownership.
     * @param[in] beds Zero-based resolved groundwater bed indices owned by persistent Richards
     * LIDs.
     * @param[in] mobile Zero-based bed indices retaining ordinary mobile-node hydraulic
     * sampling. Lists must be disjoint/duplicate-free, exchange-enabled, resolved and at initial
     * completed time zero, with no pending bed nacc. Empty both returns success without enabling
     * ownership.
     * @return Empty after setting ownership flags/enabled capability; otherwise
     * initialization/profile, bed-identity or finite one-second node-budget diagnostic, before
     * flags are changed.
     * @note Requires valid initial groundwater clock and water-only or temperature-only rows;
     * rejects pollutants/MSX/age and already enabled ownership. Uses allocation for duplicate
     * checking/list staging, which may throw. Does not create receipts, book water/heat or
     * advance clocks; exclusive setup access.
     */
    std::string configureLidReceipts(const std::vector<int>& beds,
                                     const std::vector<int>& mobile = {});
    /**
     * @brief Adopt a completed reviewed Richards water/heat receipt interval.
     * - Argument start (in): Finite nonnegative start s, matching each bed's previous booked
     * horizon.
     * - Argument end (in): Finite end s, greater than start.
     * @note The receipt-vector argument is borrowed and copied; node/bed/cell identity, gross m3
     * and J must be paired and within frozen shared budgets. Returns empty on acceptance or
     * diagnostic before ordinary validation adoption. Explicit heat refusal restores staged
     * thermal reservations; allocation exceptions can leave partial publication. Definition
     * documents phase/ownership and detailed effects.
     */
    std::string retainLidReceipts(double start, double end, const std::vector<RichardsBedReceipt>&);
    /**
     * @brief Publish persistent LID physical energy for the already accepted paired hydraulic
     * interval.
     * @return Empty on success/inactive heat, otherwise clock/owner/receipt/transfer diagnostic.
     * The context reference is the writable matching persistent LID owner and
     * producer-temperature source.
     * @note Stages every LID before publication, then marks corresponding queue heat receipts
     * adopted. Does not conduct physical layers or advance groundwater water; allocates and may
     * throw. Definition documents native-to-SI conversion, donor order and receipt phase
     * requirements.
     */
    std::string completeLidThermalInterval(SimulationContext&);
    /**
     * @brief Read whether reviewed LID/aquifer receipt ownership was enabled.
     * @return Stored capability flag. Does not inspect a specific pending bed, validate a
     * checkpoint or book any exchange; initially false until the owner configures the profile.
     */
    bool lidReceiptsEnabled() const noexcept { return lid_receipts_enabled_; }
    /**
     * @brief Sum gross pending LID-to-aquifer water across one cell's beds.
     * @param[in] cell Valid prepared cell_beds_ index, unchecked; bed lookup/receipt shapes must
     * match.
     * @return Sampled-node pending_down plus queued reviewed down m3 over all mapped beds.
     * Retains gross directions independently rather than netting returns. Read-only/no
     * allocation; caller serializes queue producers and must not interpret this as additional
     * unreserved canonical water.
     */
    double pendingLidDown(int cell) const noexcept;
    /**
     * @brief Sum gross pending aquifer-to-LID water across one cell's beds.
     * @param[in] cell Valid prepared cell_beds_ index, unchecked; bed lookup/receipt shapes must
     * match.
     * @return Sampled-node pending_up plus queued reviewed up m3 over all mapped beds. Retains
     * gross directions independently rather than netting returns. Read-only/no allocation;
     * caller serializes queue producers and must not interpret this as additional unreserved
     * canonical water.
     */
    double pendingLidUp(int cell) const noexcept;
    /**
     * @brief Read signed water stored in reviewed LID receipt queues awaiting groundwater
     * adoption.
     * @return Sum(down-up) m3 over queued receipts only. Excludes separate node_pending_down/up
     * ledgers; positive is LID-to-aquifer and negative aquifer-to-LID. Does not consume queues
     * or include canonical soil/LID water. Requires stable queues; allocation-free ordinary
     * arithmetic, no validation.
     */
    double pendingLidWater() const noexcept;
    /**
     * @brief Borrow per-bed queued accepted LID/aquifer interval receipts.
     * @return Const solver-owned nested vectors, indexed by resolved bed, with elapsed start/end
     * s, gross down/up m3, paired heat J and heat-applied flags. Firings/retention/restore can
     * invalidate entries; no receipt is consumed and opposite directions remain separate gross
     * reservations.
     */
    const std::vector<std::vector<PendingLidBed>>& pendingLidReceipts() const {
        return lid_receipts_;
    }
    struct LidBedDiagnostic {
        double start = 0, end = 0, groundwater_time = 0, lid_head = 0, groundwater_head = 0;
        double requested_down = 0, requested_up = 0, reserved_down = 0, reserved_up = 0,
               accepted_down = 0, accepted_up = 0;
    };
    /**
     * @brief Borrow the latest accepted LID/aquifer request/reservation/settlement samples per
     * bed.
     * @return Solver-owned diagnostic vector: clocks elapsed s, heads m and directional water
     * m3. A sample with end<=start is unavailable. No copy/history append/physical exchange
     * occurs; read only while owner state is stable.
     */
    const std::vector<LidBedDiagnostic>& lidBedDiagnostics() const { return lid_bed_diagnostics_; }
    /**
     * @brief Sum gross unconsumed LID-to-aquifer water for one bed.
     * @param[in] bed Valid prepared index into node_pending_down_ and lid_receipts_, unchecked.
     * @return Pending sampled-node down reservation plus every queued reviewed receipt's down
     * volume, m3. No cancellation against the opposite direction, queue drain or canonical
     * mutation occurs. Requires stable producer queues; allocation-free ordinary summation may
     * propagate nonfinite state.
     */
    double pendingBedDown(int bed) const noexcept;
    /**
     * @brief Sum gross unconsumed aquifer-to-LID water for one bed.
     * @param[in] bed Valid prepared index into node_pending_up_ and lid_receipts_, unchecked.
     * @return Pending sampled-node up reservation plus every queued reviewed receipt's up
     * volume, m3. No cancellation against the opposite direction, queue drain or canonical
     * mutation occurs. Requires stable producer queues; allocation-free ordinary summation may
     * propagate nonfinite state.
     */
    double pendingBedUp(int bed) const noexcept;
    /**
     * @brief Replace the multiplier applied to one resolved bed's subsequent node exchange
     * conductance.
     * @param[in] bed Valid lid_bed_factor_ index.
     * @param[in] factor Intended finite nonnegative dimensionless conductance factor; no
     * finite/range check or clipping is performed here.
     * @throws std::out_of_range On invalid bed. Changes no already accepted water/receipt and
     * performs no ownership activation; caller serializes the update with node sampling.
     */
    void setLidBedClogging(int bed, double factor) { lid_bed_factor_.at(bed) = factor; }

    /**
     * @brief Storage now, for the continuity ledger.
     *
     * @return state_.storage() in m3: physical saturated plus maintained unsaturated storage,
     * excluding side/pending accumulators. Uses prepared state extents without new validation;
     * for queued holdings/ledger phase distinctions use the corresponding SubsurfaceState
     * helpers.
     */
    double storage() const noexcept { return state_.storage(); }
    /**
     * @brief Serialize qualified groundwater identity or completed mutable owner state.
     * - Argument model (in): True writes model identity; false writes/reads mutable state
     * against an externally matched initialized model. The archive reference is borrowed for
     * this call.
     * @note Physical water/native/heat reservations, source/LID provenance and clocks are
     * preserved and validated on restore. Reads can throw after partial mutation: stage a
     * private copy and finish/ validate the enclosing archive before adoption. Definition
     * details profiles, allowed NaNs and borrowed owner lifetime; no physical firing, file write
     * or engine calendar update occurs.
     */
    void checkpoint(detail::CompletedSourceArchive&, bool model); // internal accepted-boundary IO
    /**
     * @brief Serialize qualified groundwater identity or completed mutable owner state.
     * - Argument model (in): True writes model identity; false writes/reads mutable state
     * against an externally matched initialized model. The archive reference is borrowed for
     * this call.
     * - Argument held_air (in): Optional native degF air-temperature override for qualified RK2
     * AIR identity; null reads current context air. Not retained.
     * @note Physical water/native/heat reservations, source/LID provenance and clocks are
     * preserved and validated on restore. Reads can throw after partial mutation: stage a
     * private copy and finish/ validate the enclosing archive before adoption. Definition
     * details profiles, allowed NaNs and borrowed owner lifetime; no physical firing, file write
     * or engine calendar update occurs.
     */
    void checkpoint(detail::CompletedSourceArchive&, bool model, const double* held_air);
    /**
     * @brief Resolve scoped subsurface expression laws into per-cell saturated/unsaturated
     * reactors.
     * @param[in,out] ctx Prepared compiled reaction system, borrowed through reactions_ on
     * success; compiled token/term/species tables must remain alive and stable throughout solver
     * use.
     * @return Empty for success/no laws, otherwise
     * unsupported/uncompiled/missing-row/layer/scope or duplicate first-order-decay ownership
     * diagnostic.
     * @note Clears old reactor binding/identity before validation, allocates reactor
     * forms/spans, applies GLOBAL<TAG<CELL and baseline-before-layer overrides, prepares
     * workspace and serialized identity. Does not execute reactions or alter native mass.
     * Failures can leave partial inactive reactor storage; allocations can throw. Requires valid
     * prepared hydraulic/native model and exclusive initialization.
     */
    std::string initReactions(SimulationContext& ctx);
    /// Coupled SSP-RK2: spatial CLOSED_FORM/ENSLAVED/SIGMA water only, authored wells and HEAD/FLOW edges.
    std::string rk2ProfileError() const;
    /**
     * @brief Form the conservative half-beginning/half-two-Euler-state SSP groundwater endpoint.
     * @param[in] initial Immutable beginning-stage solver with identical prepared model, arrays,
     * row and boundary/source identities. Must not alias this mutable solver.
     * @param[in] end Finite accepted elapsed endpoint s, strictly after enabled source cells'
     * initial times.
     * @note Blends conserved hydraulic/native inventories, reservations and receipts; ENSLAVED
     * inverts blended total equilibrium water, SIGMA blends layer water across changing column
     * length then normalizes only endpoint roundoff. Rebuilds derived layer views and
     * clocks/receipts consistently. Validates profile/shapes/admissibility but mutates
     * progressively and may throw after partial blending; caller stages the complete coupled
     * transaction. Allocations can propagate. Does not average all configuration or borrowed
     * bindings, nor advance canonical 1D owners by itself.
     */
    void blendRk2Water(const SubsurfaceSolver& initial, double end);
    /**
     * @brief Publish the current coupled stage's bounded instantaneous intake capacity.
     * - Argument dt (in): Finite positive stage duration s.
     * - Argument time (in): Finite elapsed refresh time s.
     * @note The surface reference receives matching per-cell m/s rates. Requires validated RK2
     * profile, prepared arrays and exclusive staged access; can throw after partial publication.
     * Zero-front startup uses a finite integrated rate. Canonical water/native inventories are
     * unchanged.
     */
    void publishRk2Infiltration(SurfaceStateData&, double dt, double time);
    /**
     * @brief Test whether any scoped reactor or positive first-order native decay is configured.
     * @return True for nonempty reactors_ or any tr_.decay rate>0. Does not validate a live
     * reaction binding, active transport or finite/sign validity of other rates. Read-only and
     * allocation-free.
     */
    bool hasRk2Reactions() const noexcept;
    /**
     * @brief Apply symmetric reaction/decay composition to every covered coupled groundwater
     * cell.
     * @param[in] seconds Finite positive reaction interval s. Inapplicable/nonpositive/NaN
     * returns; positive nonfinite interval throws.
     * @note Without scoped reactors applies full exact exponential decay. Otherwise applies half
     * decay, full reactor solve and half decay so a decaying precursor couples symmetrically to
     * RATE laws. Age/temperature do not decay; physical outgoing native reservations are
     * excluded. Updates canonical mass, reaction ledgers and physical/SIGMA views. Refusal can
     * follow earlier rows/cells; caller stages the coupled transaction. Requires matching stable
     * prepared owners and exclusive shared workspace.
     */
    void advanceRk2Reactions(double seconds);
    /**
     * @brief Test whether current transport ownership requires its extended restart payload.
     * @return True for layered transport, nonempty age-generation ledger, hydraulic boundaries,
     * reaction reactors, or active transport with any reviewed source-owned cell. False is not a
     * complete checkpoint/profile eligibility check. No state is archived, validated or mutated.
     */
    bool needsTransportRestart() const noexcept {
        return tr_.n_layers > 0 || !tr_.generated_age.empty() || !boundaries_.empty() ||
               !reactors_.empty() ||
               (tr_.active() && std::any_of(source_owned_.begin(), source_owned_.end(),
                                            [](bool owned) { return owned; }));
    }
    /**
     * @brief Capture the extended groundwater continuation needed beyond the ordinary hotstart
     * payload.
     * @return Owned binary envelope containing profile, exact model identity and mutable
     * completed state; empty when needsTransportRestart() is false. Not a portable
     * human-readable result/history format.
     * @note Copies this solver privately, serializes model/state and includes pending
     * native/physical/ source/LID receipt ownership. Requires a stable completed phase and live
     * external bindings. Does not write files or mutate the live solver;
     * allocation/unsupported-state/archive exceptions propagate. Borrowed
     * callbacks/context/geometry are not independently made durable by these bytes.
     */
    std::string transportRestartState() const;
    /**
     * @brief Validate or adopt an extended groundwater hotstart against this prepared model
     * identity.
     * @param[in] bytes Binary envelope from transportRestartState(), borrowed only for the call.
     * Empty is accepted only when this model needs no extended transport/source-clock restart
     * profile.
     * @param[in] apply False performs full private read/validation only; true move-adopts the
     * validated private solver and rebinds its owned node concentration pointer when present.
     * @return Empty on accepted compatible state, otherwise missing/unknown/mismatched
     * model/profile or caught std::exception diagnostic. Reported validation errors leave this
     * live solver unchanged.
     * @note Requires initialized matching mesh, species, units, layers, forcing/reaction/thermal
     * identity and completed owner phase. Verifies complete envelope/payload consumption before
     * adoption; no file I/O, 1D owner restore or engine calendar change occurs. External
     * bindings must stay alive and access serialized. Nonstandard exceptions are not caught;
     * enclosing engine restore coordinates its other canonical owners and publication rather
     * than treating this as a standalone whole-model restart.
     */
    std::string restoreTransportRestart(const std::string& bytes, bool apply);

private:
    double infiltrationStorageDeficit(int cell) const noexcept;
    double infiltrationHeadroom(int cell, double deficit) const noexcept;
    friend class CommonSourceStage;
    friend class ExplicitInertialSolver;
    friend RichardsBedAcceptance acceptRichardsBedsAtFiring(SubsurfaceSolver&, SurfaceStateData&,
                                                            double, double,
                                                            const std::vector<RichardsBedOwner>&);
    friend RichardsBedAcceptance acceptRichardsBedsForTier(SubsurfaceSolver&, SurfaceStateData&,
                                                           int, double, double,
                                                           const std::vector<RichardsBedOwner>&);
    friend RichardsBedAcceptance acceptRichardsBedsAcrossSteps(SubsurfaceSolver&, SurfaceStateData&,
                                                               double, double,
                                                               const std::vector<double>&,
                                                               const std::vector<RichardsBedOwner>&,
                                                               const RichardsBedStepper*);
    friend RichardsBedAcceptance acceptRichardsBedsForTierAcrossSteps(
        SubsurfaceSolver&, SurfaceStateData&, int, double, double, const std::vector<double>&,
        const std::vector<RichardsBedOwner>&, const RichardsBedStepper*);
    /**
     * @brief Resolve authored aquifer scopes into SI cell material/geometry and coverage arrays.
     * @param[in] mesh Matching SI areas, ground elevations and optional tags. Missing
     * area/elevation entries use 1 m2/0 m fallback; scope TAG requires an existing matching tag.
     * @param[in] uf Authored-to-SI rate/length/inverse-length conversion factors.
     * @param[in,out] cfg Authored row collection, read only despite mutable reference; retains
     * user units.
     * @param[in,out] warnings Appends residual>=saturated-content clamp diagnostics, retaining
     * prior text.
     * @note Requires state_.n_cells and all writable arrays prepared. Applies GLOBAL then TAG
     * then CELL, last matching row per scope wins; converts dimensional fields once, optionally
     * caps hg0 to depth, sets basal elevation and clamps invalid theta_r to theta_s/2.
     * PER_SUBCATCH starts fully covered. Mutates progressively and may allocate/throw after
     * partial writes; caller serializes initialization.
     */
    void resolveRows(const MeshData& mesh, const GwUnitFactors& uf, SubsurfaceConfig& cfg,
                     std::vector<std::string>& warnings);
    /**
     * @brief Resolve automatic cell closure once from the initial dimensionless retention-length
     * group.
     * @param[in,out] warnings Appends one summary counting ENSLAVED/CLOSED_FORM/SIGMA cells.
     * @note AUTO selects ENSLAVED for alphaL<1, CLOSED_FORM for alphaL<5 or force_closed_form,
     * otherwise SIGMA. Explicit selectors are retained. Requires valid prepared material/storage
     * arrays; changes closure bytes only plus diagnostics, without initializing column moisture
     * or water. Allocation can throw after closure mutation; caller owns exclusive
     * initialization access.
     */
    void resolveClosures(std::vector<std::string>& warnings);
    /**
     * @brief Copy one prepared cell's SI retention/conductivity parameter tuple.
     * @param[in] i Valid cell index into every material parallel vector, unchecked.
     * @return Owned soil::Params with conductivity m/s, alpha 1/m, suction m and dimensionless
     * moisture/ shape coefficients. Does not resolve scope, convert authored values, allocate or
     * mutate state.
     */
    soil::Params paramsOf(int i) const noexcept;
    /**
     * @brief One cell's firing — the §gw_split sequence.
     *
     * @param[in] i Valid covered hydraulic cell index with matching material, bed, edge, source,
     * native/physical, pending and completion arrays. Bounds are unchecked; all borrowed owner
     * bindings must be live and prepared for the same model identity.
     * @param[in] dt Finite positive accepted firing duration s. Reviewed-source mode explicitly
     * checks it, while ordinary mode assumes caller validation. Stability limits and
     * chronological forcing cuts must already be honored by the scheduler.
     * @param[in,out] surf Matching surface owner. PER_SUBCATCH reads exposed evaporation
     * directly; spatial mode uses previously booked ET demand. Writes published infiltration
     * rate where allocated. Ordinary emergence is queued in groundwater for later surface
     * adoption rather than debiting surface here.
     * @param[in] time Finite nonnegative elapsed start s, consistent with the cell's accepted
     * horizon; reviewed sources forbid replay and require receipt ends not beyond
     * source_firing_end_.
     * @details This is the paired saturated/unsaturated ownership boundary. Before a
     * reviewed-source gather, verifies exact native row identity and approximate water/native
     * equality of the provenance queue with pending intake. Adopts only completed thermally
     * paired LID receipts, then consumes the current gather_fraction_ of edge/node/surface/link
     * packets and preserves future gross LID claims. Rates use SI m/s or m3/s; all accepted
     * transfers and receipts use m3. Resolves boundary/prescribed/ runtime source proposals,
     * producer composition and independent heat, jointly limits saturated withdrawals, and
     * records requested versus applied provider/boundary receipts.
     * @details CLOSED_FORM advances bulk unsaturated water with a closure-consistent
     * moving-table slab; ENSLAVED inverts monotone total equilibrium water with a bounded solve;
     * SIGMA settles the table against accepted bounded column transfers from the same initial
     * layer state, including RK2 ALE starting velocity. Caps recharge/capillary
     * rise/ET/deep/source withdrawals against appropriate donor budgets. Tracks deep/node
     * shortfall refunds, rejected top intake and physical Dunne emergence; when DUNNE is
     * disabled its return is not booked. Maintains residual-water constraints and reported
     * closure/ET/recharge rates without silently replacing a conserved water update by a bare
     * table clamp.
     * @details Passes the final accepted hydraulic receipt to native/physical transport,
     * including moving grain/water handover. Reviewed intake refunds keep the original donor
     * composition and availability horizon separate from physical/column emergence; ordinary
     * returns enter surface water/native queues. On normal completion updates table height,
     * cell/source clocks, drying wetting-front memory and any published intake allowance. Does
     * not directly advance canonical 1D node/LID hydraulic storage or engine calendar; those
     * transfers must already be paired by their owner protocol.
     * @throws std::runtime_error Unpaired/replayed/future source or incomplete LID heat receipt,
     * invalid proposal, failed SIGMA conservation solve or delegated transport/reaction refusal.
     * Allocation and other delegated errors can also propagate. Physical helpers may instead
     * record thermal failed flags.
     * @note This method mutates queues, ledgers and stores progressively. A late exception can
     * follow receipt consumption, source booking, native/thermal updates or successful earlier
     * substeps; it does not roll itself back. Callers requiring atomic multi-owner/RK2
     * acceptance operate on staged owners and check thermal failure as well as exceptions.
     * Requires serial firing because bed scratch, provider diagnostics, partial-gather fraction
     * and reaction workspace are shared.
     */
    void fireCell(int i, double dt, SurfaceStateData& surf, double time);
    void fireGwCellList(const std::vector<int>&, double dt, SurfaceStateData&, double time,
                        double accepted_end);
    std::vector<bool> source_owned_;
    std::vector<std::string> source_species_;
    std::vector<double> source_cell_end_;
    std::vector<std::vector<SurfaceIntakeReceipt>> source_receipts_;
    std::vector<SurfaceReturnReceipt> source_returns_;
    double source_firing_end_ = 0.0;
    double source_completed_end_ =
        0.0; // transient R5 readiness; old restores cannot activate Richards ownership
    double gather_fraction_ = 1.0;
    /**
     * @brief Consume this firing's proportional share of one accumulated scalar receipt.
     * @param[in,out] value Pending water/mass/energy scalar, in its owning accumulator's units.
     * @return Original value*gather_fraction_; subtracts that exact amount from value. Caller
     * prepares finite values and normally a fraction in [0,1], with exclusive access to this
     * scalar. No bounds, finite-value validation, allocation or whole-receipt synchronization
     * occurs.
     */
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
        double node_in = 0;  ///< gross node inflow in the layered profile
        double link_out = 0; ///< gross outgoing conduit water, separate from net link
        const sigma::ColumnTransfers* column = nullptr;
        double boundary_in = 0, boundary_out = 0;
        const std::vector<GwBoundaryPulse>* boundaries = nullptr;
        double hg_final = 0;
        double runtime_dt = 0.0, requested_source_out = 0.0;
        double v_sat0 = 0.0, v_uns0 = 0.0; ///< water volumes BEFORE the firing
        double lateral = 0.0;              ///< gathered, + into the cell
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
        double grid_volume =
            0.0;               ///< swept soil volume; beginning-state tangent in smooth RK stages
        double lid_in_j = 0.0; ///< accepted LID sensible heat entering this cell
        double et = 0.0;       ///< out of the column
        double deep = 0.0;
        double dt = 0.0;    ///< T7.2: the firing's own step (s), for decay
        double dunne = 0.0; ///< saturation excess, out of the saturated zone
        double paired_refund = 0.0;
        const std::vector<double>* refund_mass = nullptr;
        std::vector<double>* dunne_mass = nullptr;
        std::vector<double>* excess_mass = nullptr;
        double reject = 0.0; ///< column rejection, out of the unsaturated zone
    };
    std::vector<std::string> transport_units_;
    struct GroundReactor {
        std::vector<ReactionExprForm> forms;
        std::vector<RxExprSpan> expressions;
    };
    const ReactionData* reactions_ = nullptr;
    transport::RxWorkspace reaction_workspace_;
    std::vector<GroundReactor> reactors_;
    std::string reaction_identity_;
    /**
     * @brief Advance one cell's configured groundwater reaction inventories.
     * @param[in] cell Valid prepared covered cell with matching reactor/native/physical arrays;
     * unchecked.
     * @param[in] dt Finite positive reaction duration s, passed to the reaction integrator.
     * @param[in] hg Accepted saturated table height m above aquifer bed, used to locate phase
     * geometry.
     * @throws std::runtime_error Invalid moisture/reservation, missing physical owner, reactor
     * refusal or inadmissible resulting inventory. Allocations can also throw.
     * @note No bound reaction system returns. Nonlayered physical heat delegates to
     * reactPorewaterCell; otherwise stages all saturated/unsaturated or SIGMA zones before
     * publishing native stores. Books signed lost_reaction (production can make it negative),
     * refreshes derived views as applicable, and does not move hydraulic water or energy.
     * Workspace can mutate during rejected solves; derived publication may fail after canonical
     * reaction commit. Requires exclusive cell and shared workspace access; enclosing
     * multi-cell/RK2 transaction supplies broader rollback.
     */
    void reactCell(int cell, double dt, double hg);

    /**
     * @brief Move the tuple along @p f. Called at the END of `fireCell`, when every volume is
     * final.
     *
     * @details Apply one accepted hydraulic cell firing to all native transport rows and paired
     * outputs.
     * @param[in] i Valid prepared cell with matching native, hydraulic and optional physical
     * owners.
     * @param[in] f Accepted CellFlux containing SI m3 transfers/stores, s duration and live
     * optional row-sized source/refund/boundary input/output tuples. Caller retains payload
     * lifetime throughout.
     * @note Delegates SIGMA rows to fireLayerSpecies and qualified fixed physical
     * porewater/temperature to their canonical owners. Bulk rows gather paired arrivals,
     * transport dissolved mobile inventory, carry sorbed inventory across the moving table,
     * honor producer-prepaid link exports, cap independent sinks and book
     * node/provider/boundary/deep/ET/emergence ledgers. Unsigned rows use donor limits; signed
     * temperature retains signed degC*m3. RK2 uses beginning-state donor budgets and separates
     * reaction splitting; ordinary mode applies decay/scoped reaction after transport. Rejected
     * reviewed intake retains producer composition through refund tuples. Mutates
     * rows/packets/ledgers progressively and may throw after earlier work; physical helpers can
     * flag a failure instead. Caller stages acceptance, validates compatible extents and
     * serializes shared workspace/producer state. Does not update hydraulic table itself or
     * canonical 1D nodes directly.
     */
    void fireCellSpecies(int i, const CellFlux& f);
    void fireLayerSpecies(int i, const CellFlux& f);
    /**
     * @brief Advance one cell's canonical physical-layer reaction inventories.
     * @param[in] cell Valid prepared covered cell with matching reactor/native/physical arrays;
     * unchecked.
     * @param[in] dt Finite positive reaction duration s, passed to the reaction integrator.
     * @param[in] hg Accepted saturated table height m above aquifer bed, used to locate phase
     * geometry.
     * @throws std::runtime_error Invalid moisture/reservation, missing physical owner, reactor
     * refusal or inadmissible resulting inventory. Allocations can also throw.
     * @note Stages every physical layer/phase before replacing canonical porewater mass,
     * excludes promised outgoing inventory and uses active capacity/temperature. RK2 RATE-only
     * mixed phases share rate contexts; other forms integrate by phase. Books signed
     * lost_reaction (production can make it negative), refreshes derived views as applicable,
     * and does not move hydraulic water or energy. Workspace can mutate during rejected solves;
     * derived publication may fail after canonical reaction commit. Requires exclusive cell and
     * shared workspace access; enclosing multi-cell/RK2 transaction supplies broader rollback.
     */
    void reactPorewaterCell(int cell, double dt, double hg);
    /**
     * @brief Apply accepted hydraulic/temperature-volume receipts to canonical physical soil
     * energy.
     * @param[in] i Valid prepared physical/hydraulic cell, with active native temperature row;
     * unchecked.
     * @param[in] f Accepted CellFlux with matching SI water/elapsed-second transfers, final
     * moisture, optional native degC*m3 source/refund/boundary tuples and writable emergence
     * results. Payloads must remain valid and match the same hydraulic receipt used by
     * porewater.
     * @note Empty/failed column returns. Gathers full lateral heat and partial paired
     * native/preset receipts, projects final moisture, advects producer-time energy with
     * implicit or RK2 Euler budgets, settles reserved capacity, books
     * native/node/provider/boundary and J ledgers, optional soil latent heat and non-RK2 joint
     * LID conduction, then refreshes SIGMA/native views. Temperature is canonical physical
     * energy rather than independently transported bulk mass. All exceptions become thermal
     * failed/error where possible. Packet consumption/energy/ledgers can change before a late
     * failure; caller checks flags and stages broader acceptance. Allocates inside the guarded
     * path; requires exclusive stable physical/node/LID/snow/native owners. No engine clock
     * advance.
     */
    void fireGroundHeatSpecies(int i, const CellFlux& f) noexcept;
    /**
     * @brief Project accepted hydraulic water changes into physical thermal-layer m3 increments.
     * - Argument cell (in): Valid prepared physical/hydraulic cell, called i in source.
     * @return Empty after replacing the output vector argument, otherwise projection diagnostic.
     * The CellFlux reference must carry the accepted matching SIGMA/bulk water receipt; read
     * only. Does not change canonical state, may allocate/throw, and retains output on reported
     * refusal.
     */
    std::string groundWaterIncrement(int cell, const CellFlux&, std::vector<double>&) const;
    void fireGroundPorewaterSpecies(int i, const CellFlux& f) noexcept;
    /**
     * @brief Gather this cell's lateral side accumulators (m³) and zero them.
     *
     * @param[in] i Cell index into a valid matching edge CSR. Empty lateral storage or missing
     * CSR cell endpoint returns zero; a negative cast/wrap is not generally guarded.
     * @return Signed incoming water m3 consumed from only this cell's side of incident edges
     * using the current gather_fraction_. Retains unused fractions for later chronological
     * source subintervals.
     * @note Requires live edges_ when storage exists. Does not mutate canonical water or
     * neighbor-side receipts, book native mass or clear all accumulators. Exclusive access to
     * consumed slots is required.
     */
    double gatherLateral(int i) noexcept;
    /**
     * @brief T7.1: the same gather for one species' mass.
     *
     * @param[in] i Cell index into matching CSR; absent storage/edges or missing cell endpoint
     * returns zero.
     * @param[in] s Valid native species row, unchecked against species-major side storage.
     * @return Signed incoming native inventory consumed through gather_fraction_ on incident
     * cell-side slots. No canonical inventory/water mutation or neighbor-side consumption.
     * Requires stable prepared extents and exclusive affected-slot access; allocation-free.
     */
    double gatherLateralMass(int i, int s) noexcept;
    /// T7.2: does a `* / TAG / CELL` scoped row cover this cell?
    bool scopeCoversCell(int i, GwScope scope, const std::string& tag, int cell) const noexcept;
    /// T7.4: gather one species' share of this cell's node-seam arrivals.
    double gatherNodeMass(int i, int s) noexcept;
    /// T7.2: the `[POLLUTANTS]` decay constant for a transport row (1/s).
    double pollutantDecay(int s, const std::vector<double>& pollut_decay) const noexcept;
    /**
     * @brief Gather this cell's booked node exchange (m³, + out of the aquifer).
     *
     * @param[in] i Valid prepared receiver cell index with consistent node/heat/bed ledgers.
     * @return Signed outward aquifer-to-node water m3 consumed through gather_fraction_ from
     * every bed attached to the cell. Clears the previous gather's bed_gathered_ slots then
     * records this gather's outgoing weights, matching layered contact outflow when active.
     * @note Also gathers paired pending LID direction/heat volumes. Does not change canonical
     * node/soil water or move native species itself. Requires serial gathers because shared
     * scratch tracks the preceding cell; array indices and producer invariants are unchecked. No
     * allocation.
     */
    double gatherNode(int i) noexcept;

    /// G-W1: specific yield for cell `u` under closure `cl` — the ONE
    /// definition. Anything capping a withdrawal against the cell's drainable
    /// water must cap against the same number the column will spend it
    /// against, or the cap does not bind and the column goes short. Optionally
    /// reports the bottom/mean unsaturated content the derivation used.
    double specificYield(std::size_t u, const soil::Params& p, double ts, double L0, GwClosure cl,
                         double* theta_bot_out) const noexcept;
    void markPendingSurface(int i) noexcept;

    void publishCellInfiltration(int, SurfaceStateData&, double interval, double time);
    std::vector<std::vector<std::size_t>> cell_beds_;
    RuntimeSources runtime_sources_;
    std::vector<GwResolvedBoundary> boundaries_;
    std::vector<std::vector<std::size_t>> cell_boundaries_;
    std::vector<GwResolvedSource> sources_;
    std::vector<std::vector<std::pair<std::size_t, double>>> cell_sources_;
    SubsurfaceState state_;
    SubsurfaceTransportState tr_; ///< T7.1
    GroundHeatState heat_;
    std::vector<double> heat_link_out_volume_;
    std::vector<double>
        heat_preset_out_j_; // [cell*thermal_layers+layer], accepted producer-time exports
    std::vector<double>
        heat_preset_out_water_; // paired physical layer volumes, including source-interval splits
    std::vector<double> heat_node_out_volume_,
        heat_node_gathered_out_;               // gross accepted exports per hydraulic bed
    std::vector<double> porewater_preset_out_; // [species*n_cells*layers + cell*layers + layer]
    /**
     * @brief Debit accepted producer-time saturated sensible energy before later cell gather.
     * - Argument cell (in): Valid prepared cell, called i in source; inactive/invalid returns.
     * - Argument volume (in): Positive finite accepted outgoing m3.
     * - Argument temperature_volume (in): Signed native degC*m3 packet, matching that water.
     * @note Preflights finite receipts/remaining physical capacity, then debits J and reserves
     * water capacity. Refusal flags thermal failure; caller checks it. Does not book hydraulic
     * water itself.
     */
    void bookPresetHeatOut(int cell, double volume, double temperature_volume) noexcept;
    /**
     * @brief Conduct a groundwater-owned column jointly with its persistent Richards LID stores.
     * - Argument cell (in): Prepared physical cell index, called i in source.
     * - Argument seconds (in): Finite positive interval s, called dt in source.
     * @return Empty on success/skip, otherwise diagnostic. Surface-owned columns defer to their
     * shared mesh/channel solve. Allocates/stages finite stores; exceptions and external snow
     * refusal may occur. Requires exclusive stable coupled owners; does not book hydraulic water
     * or advance engine time.
     */
    std::string conductGroundWithLids(int cell, double seconds);
    /**
     * @brief Reserve accepted native export against its producing physical saturated layers.
     * - Argument cell (in): Valid prepared cell, called i in source.
     * - Argument species (in): Valid non-temperature native row, called r in source.
     * - Argument mass (in): Positive finite native inventory (concentration*m3), called target
     * in source.
     * @note No-op for nonphysical/SIGMA/temperature/nonpositive mode; invalid donor flags
     * thermal failure. Does not book water or debit canonical inventory. Requires serial
     * compatible producer state.
     */
    void bookPresetPorewaterOut(int cell, int species, double mass) noexcept;
    /**
     * @brief Read unreserved dissolved native inventory in a physical saturated zone.
     * - Argument cell (in): Prepared cell index.
     * - Argument species (in): Valid non-temperature native row.
     * @return Native inventory (concentration*m3), or quiet NaN for invalid/unavailable physical
     * state. Reads matching physical water/retention/reservations without booking or consuming
     * them.
     */
    double saturatedPorewaterMobileMass(int cell, int species) const noexcept;
    struct ThermalFaceSlice {
        int left = 0, right = 0;
        double low = 0, high = 0;
    };
    struct ThermalFace {
        double distance_left = 0, distance_right = 0;
        std::vector<ThermalFaceSlice> slices;
    };
    std::vector<ThermalFace> heat_faces_;
    std::vector<double> heat_pending_left_, heat_pending_right_, heat_dt_;
    /**
     * @brief Build physical saturated face-overlap contacts and conservative explicit thermal
     * limits.
     * @return Empty for success/disabled conduction, otherwise invalid positive normal-distance
     * diagnostic.
     * @note Requires live initialized heat config, physical layer count, matching mesh/edge
     * geometry and covered-cell columns. Sizes per-edge/layer pending J, builds elevation
     * overlap slices and bounds heat_dt_ s using solid capacity, largest phase conductivity and
     * min(0.5,c_diff). Does not change canonical energy or hydraulic state. Does not clear
     * preexisting slice vectors before push when called repeatedly; intended once after
     * initHeat() clears them. Allocates and can fail after partial mapping replacement; caller
     * serializes initialization and discards rejected staged state.
     */
    std::string initLateralHeat();
    /**
     * @brief Book one face's accepted saturated conductive layer packets.
     * - Argument edge (in): Valid prepared edge/physical heat-face index, called e in source.
     * - Argument seconds (in): Finite positive firing duration s, called dt in source.
     * @note Uses stable prepared geometry/active temperatures and adds equal-opposite J.
     * Nonfinite work throws after possible earlier slice bookings; caller stages acceptance and
     * honors thermal stability.
     */
    void fireLateralHeatFace(int edge, double seconds);
    /**
     * @brief Gather complete pending lateral physical energy packets into one canonical column.
     * - Argument cell (in): Valid prepared physical/CSR cell, called i in source.
     * @note Adds J to layer energy/lateral ledger and zeros each cell-side packet. Does not
     * apply partial gather fraction, refresh transport or book water. Requires exclusive valid
     * prepared arrays.
     */
    void gatherLateralHeat(int cell) noexcept;
    GwOptions options_;
    std::vector<GwNodeBed> node_beds_;
    std::vector<double> bed_exchange_cum_; ///< G-O: per bed (m³), cumulative
    // Internal R5 owner clocks and directional receipts; execution/checkpoint
    // activation remains guarded until the routing/restart adapter is qualified.
    std::vector<double> cell_completed_, richards_down_cum_, richards_up_cum_;
    std::vector<bool> richards_bed_owned_;
    bool lid_receipts_enabled_ = false;
    std::vector<std::vector<PendingLidBed>> lid_receipts_;
    std::vector<double> lid_booked_end_;
    std::vector<bool> mobile_lid_bed_;
    std::vector<double> lid_bed_factor_;
    std::vector<double> node_pending_down_, node_pending_up_;
    std::vector<LidBedDiagnostic> lid_bed_diagnostics_;
    bool richards_clock_valid_ = true; // old checkpoints do not restore the joint owner clocks
    /// T7.4: the per-bed volumes the CURRENT gather took — the weights that
    /// split this firing's outgoing mass across the cell's beds. Written by
    /// `gatherNode`, read by `fireCellSpecies` in the same firing, so it can
    /// never be stale (the bug the first cut had).
    std::vector<double> bed_gathered_;
    /// G-X1: water taken FROM each 1D node by the recharge direction within
    /// the current routing batch (m³, 2D units), reset with
    /// `node_exchange_vol_` — the frozen node volume is a batch budget, not a
    /// per-substep one, exactly as the surface's `node_drawn_` ledger.
    std::vector<double> node_drawn_gw_;
    /// T7.4: the router's published node species rows (not owned).
    std::vector<double> node_concentrations_;
    const double* node_row_conc_ = nullptr;
    int node_row_ns_ = 0;

    const MeshData* mesh_ = nullptr;
    const InertialEdges* edges_ = nullptr;
    const SolverOptions2D* opts_ = nullptr;

    GwRechargeBackend recharge_backend_;
    std::vector<OpenSwmmGwRechargeInputV1> recharge_inputs_;
    std::vector<double> recharge_values_;
    std::vector<int> recharge_slots_;
    uint64_t recharge_batches_ = 0, recharge_cells_ = 0;
    void prepareRechargeBatch(const std::vector<int>&);
    double rawRecharge(int cell, const soil::Params&, double length) const noexcept;

    std::vector<std::vector<int>> cells_by_tier_;
    std::vector<std::vector<int>> faces_by_tier_;
    std::vector<uint8_t> face_tier_;
    std::vector<long> tier_firings_;
    std::vector<int> pending_surface_;
    std::vector<uint8_t> pending_flag_;
    std::vector<int> cell_nface_;           ///< incident GW faces per cell
    std::vector<double> node_exchange_vol_; ///< per 1D node (m³)
    long drain_cap_binds_ = 0;              ///< G-W1b
    long node_refunds_ = 0;                 ///< G-W1b
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
    /// Derived scratch owner; reconstructed from bed_gathered_ on checkpoint restore.
    int bed_gathered_cell_ = -1;
    bool et_boundary_ = false, et_caprise_ = false;
    std::vector<int> reaction_rows_;

    // Nonphysical, pointer-free work storage. Snapshots do not copy its capacity;
    // assignment keeps the destination's workspace. Every used slot is reset or
    // overwritten by its next firing; no checkpoint record includes these values.
    struct CellScratch {
        std::vector<double> receipt_mass;
        std::vector<double> refund_mass, dunne_mass, excess_mass;
        CellScratch() = default;
        CellScratch(const CellScratch&) noexcept {}
        CellScratch& operator=(const CellScratch&) noexcept { return *this; }
        CellScratch(CellScratch&&) noexcept = default;
        CellScratch& operator=(CellScratch&&) noexcept = default;
    } cell_scratch_;
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SUBSURFACE_SOLVER_HPP
