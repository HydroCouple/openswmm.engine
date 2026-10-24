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
 * @file      SurfaceRouter2D.hpp
 * @brief     Top-level orchestrator for the optional 2D surface routing module.
 *
 * @details Manages the full 2D workflow within the engine lifecycle:
 *          - Mesh topology construction (after parse)
 *          - Explicit-marcher solver initialization
 *          - Windowless co-advance coupling (sync batches), rainfall update
 *          - Statistics and finalization
 *
 *          Integrates with SWMMEngine via lifecycle hooks:
 *          initialize() → step() → finalize()
 *
 * @see TWO_DIMENSIONAL_SURFACE_ROUTING_IMPLEMENTATION_STRATEGY.md §4.1, §8.3
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP

#include "data/MeshData.hpp"
#include "data/SurfaceStateData.hpp"
#include "data/SolverOptions2D.hpp"
#include "data/BoundaryData.hpp"
#include "data/PendingRows2D.hpp"
#include "coupling/NodeCoupling.hpp"
#include "coupling/OutfallExchange.hpp"
#include "gw/GwTransportData.hpp"
#include "quality/SurfaceQuality2D.hpp"    // S7
#include "subsurface/SubsurfaceSolver.hpp" // G1: the two-zone GW kernel
#include "heat/ChannelGround.hpp"
#include "infil/Infil2D.hpp"
#include "mesh/RainfallInterpolator.hpp"

#include <memory>
#include <functional>
#include <array>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#ifdef OPENSWMM_HAS_2D
#include "solver/ISurfaceSolver.hpp"
#endif

namespace openswmm::runoff {
class SourceWaterDriver;
}
namespace openswmm {
struct SimulationContext;
struct HeatState;
namespace climate {
struct ClimateState;
}
class Router;
class SWMMEngine;
}

namespace openswmm::twoD {
class CommonSourceStage;
class SourceResultHistory;

/**
 * @brief Top-level orchestrator for the 2D surface routing module.
 */
class SurfaceRouter2D {
public:
    /**
     * @brief Create an inactive authoring router with empty owned physical/source state.
     * @note No solver/context attachment or physics advance. initialize() resolves a run before
     * routing; member allocation follows ordinary C++ construction.
     */
    SurfaceRouter2D() = default;
    /**
     * @brief Destroy owned router members and release solver/plugin resources.
     * @note Does not call finalize(ctx) or flush pending physics. Caller finalizes while
     * borrowed contexts/source/history owners remain alive. All router/member views become
     * invalid.
     */
    ~SurfaceRouter2D() = default;

    // Non-copyable, movable
    SurfaceRouter2D(const SurfaceRouter2D&) = delete;
    SurfaceRouter2D& operator=(const SurfaceRouter2D&) = delete;
    /**
     * @brief Move owned router members using their ordinary default move operations.
     * @note Source object remains destructible with moved-from containers. Raw
     * borrowed/self-member pointers are copied without automatic rebinding; move only an
     * unattached inactive authoring router, or reinitialize/rebind before routing. Retained
     * member/solver views can become invalid.
     */
    SurfaceRouter2D(SurfaceRouter2D&&) = default;
    /**
     * @brief Replace owned router members by default move assignment.
     * @return This router reference.
     * @note Finalize/detach the previous run before replacement. Raw pointers are copied without
     * self-member rebinding; move only inactive unattached authoring state or
     * reinitialize/rebind before use. Source remains destructible; all prior destination
     * member/solver views may be invalidated.
     */
    SurfaceRouter2D& operator=(SurfaceRouter2D&&) = default;

    /**
     * @brief Initialize the 2D module after input parsing is complete.
     *
     * Builds mesh topology, vertex stencils, resolves coupling names, and initializes the
     * explicit 2D solver.
     *
     * - Argument ctx: Simulation context (must have mesh_2d populated from parsing).
     *
     * @details Prepare authored mesh, coupling, physical source owners and selected solver for a
     * new run.
     * - Argument ctx (in,out): Fully parsed matching engine context with prepared
     * node/link/gage/tables, quality/reaction/heat configuration; borrowed context/node/thermal
     * views must outlive routing.
     * @note Requires serial lifecycle access and no concurrent retained mutable mesh/state
     * views. Clears source/checkpoint bindings and channel contacts, refuses unqualified surface
     * recharge ownership, and disables routing for missing mesh or IGNORE_2D. Converts authored
     * geometry and boundary units to SI, builds/validates topology and stencils, resolves named
     * coupling and pond footprints, selects CPU/plugin eligibility and initializes native rows,
     * groundwater/heat/snow/quality/infiltration. 1D remains internally ft/ft3, even for metric
     * display projects. Surface/GW native inventories use concentration*m3, age s*m3,
     * temperature degC*m3. Authored age concentrations convert hours to s.
     * @details Seeds initial water/native/bed storage, source rate capacities, report/render
     * fields and water ledgers. May alter options from environment, coupling/pond/exfiltration
     * ownership in ctx, quality queues and warnings. Does not advance the engine calendar.
     * Channel contacts are separately bound after 1D heat initialization.
     * Profile/name/layout/ownership errors throw std::runtime_error; allocation and delegated
     * validation can throw after partial setup. This is not a transactional reinitialization or
     * a safe way to preserve an evolved model's inventories.
     */
    void initialize(SimulationContext& ctx);
    /**
     * @brief Bind selected finite channel contacts after 1D heat state is seeded.
     *
     * @details Resolve requested finite conduit/physical-ground contacts after native 1D heat
     * initialization.
     * - Argument ctx (in,out): Matching prepared heat/node/link context; retained thermal views
     * must stay alive.
     * @note No requested mesh contact returns. Requires active mesh, compatible porous aquifer
     * heat/native owners and CPU solver. Converts authored map coordinates to m, initializes the
     * shared finite contact owner and binds it into CPU marcher. Refusal throws
     * std::runtime_error; initialization/allocation can leave partially prepared contact state.
     * No water/thermal advance; exclusive lifecycle access.
     */
    void initializeChannelGround(SimulationContext& ctx);
    /**
     * @brief Borrow the shared finite channel/physical-soil thermal contact owner and cumulative
     * J/native receipts.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const ChannelGroundState& channelGround() const noexcept { return channel_ground_; }
    /**
     * @brief Borrow the shared finite channel/physical-soil thermal contact owner and cumulative
     * J/native receipts.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    ChannelGroundState& channelGround() noexcept { return channel_ground_; }
    /**
     * @brief Internal qualification hook: externally owned stage, all cells managed, wall
     * boundaries, no live 1D exchanges. Does not activate authored ownership.
     *
     * @details Attach a borrowed all-cell source stage to a fresh first-order CPU router.
     * - Argument ctx (in,out): Exact matching initial context without live network coupling.
     * - Argument stage (in,out): Prepared matching mesh/surface/subsurface owner spanning all
     * cells; borrowed for later routing/checkpoint and must outlive attachment with stable
     * identities.
     * @note Delegates attachment validation/setup with no hydraulic joint owner. Does not
     * activate authored recharge ownership or advance physics. Throws std::invalid_argument on
     * profile/phase refusal and may allocate after partial preparation; exclusive fresh
     * lifecycle access required.
     */
    void setCommonSourceStage(SimulationContext& ctx, CommonSourceStage& stage);
    /**
     * @brief Internal water-only joint DW/source checkpoint, including non-outfall surface
     * exchange and pending node delivery. Attach after both routers and LID initialization; the
     * hydraulic router must track accepted steps. External providers, node beds and
     * whole-engine/output state remain excluded.
     *
     * @details Attach matching first-order source and tracked native hydraulic owners for joint
     * checkpoints.
     * - Argument ctx (in,out): Exact prepared initial context shared by both routers and LID
     * setup.
     * - Argument stage (in,out): Borrowed matching all-cell common-source stage, stable/alive
     * through routing.
     * - Argument hydraulic (in,out): Borrowed initialized tracked hydraulic owner sharing ctx;
     * must remain alive.
     * @note Delegates joint ownership, quality/thermal, LID, boundary and coupling
     * qualification. Disallows unsupported external head ramps/conduit owners. No physics
     * advance; refusal throws, allocation or delegated setup may leave partial preparation. Call
     * serially after native routers/LID initialize.
     */
    void setJointSourceStage(SimulationContext& ctx, CommonSourceStage& stage, Router& hydraulic);
    /**
     * @brief Attach a fresh borrowed accepted-source history recorder through the CPU source
     * marcher.
     * - Argument history (in,out): Prepared compatible recorder; borrowed and must outlive
     * attachment/use.
     * @note Requires common stage and disallows directly attaching to an engine-owned joint
     * checkpoint session. Delegates recorder freshness/identity/boundary validation and throws
     * on refusal. No physics or calendar advance; exclusive source/history/lifecycle access, no
     * duplicate recorder ownership.
     */
    void setSourceResultHistory(SourceResultHistory& history);
    /**
     * @brief Resume a saved accepted-source recorder prefix at a completed router boundary.
     * - Argument history (in,out): Compatible borrowed saved recorder prefix with exact
     * checkpoint match; must remain stable/alive through recording.
     * @note Requires attached common stage and ready boundary, then delegates exact prefix/phase
     * checks to CPU marcher. Throws on refusal; does not replay physical intervals or advance
     * engine calendar. Exclusive router/source/recorder access required.
     */
    void resumeSourceResultHistory(SourceResultHistory& history);
    /**
     * @brief Internal router/CPU/source boundary; not a full-project hotstart.
     *
     * @details Serialize and publish a qualified source/router or joint component boundary.
     * - Argument ctx (in): Exact attached context, held stable/exclusive through
     * qualification/serialization.
     * - Argument path (in): Writable destination with existing parent; existing record must
     * match profile magic.
     * @return Empty on success, otherwise owner, qualification, archive, publication or caught
     * standard exception diagnostic. Engine-owned managed RK2/joint sessions require engine
     * checkpoint entry.
     * @note Does not flush pending physics or advance clocks. Component scope includes qualified
     * router, CPU/source, optional channel/native hydraulic state; whole-engine/output state is
     * separate. Publisher uses exclusive temporary+rename and may leave temporary files on
     * exceptional filesystem failure.
     */
    std::string saveCheckpoint(const SimulationContext& ctx, const std::string& path) const;
    /**
     * @brief Read and stage a router component checkpoint into fresh matching attached owners.
     * - Argument ctx (in,out): Exact fresh attached context; successful install updates
     * checkpointed clocks, gage rainfall/evaporation, mass balance and qualified native/source
     * owners.
     * - Argument path (in): Readable nonempty bounded checkpoint file.
     * @return Empty on success, otherwise freshness/profile/model/file/decode diagnostic.
     * Engine-owned managed RK2 or joint sessions must use the engine checkpoint route.
     * @note Delegates private staged load; no general whole-engine/output hotstart. Requires
     * exclusive live prepared owners and no evolution since attachment; does not physically
     * replay saved intervals.
     */
    std::string loadCheckpoint(SimulationContext& ctx, const std::string& path);

    /**
     * @brief Advance the 2D surface routing by one SWMM routing step.
     *
     * Sequence:
     * 1. Update outfall boundary heads from 2D state (before 1D routing)
     * 2. After 1D routing: compute coupling exchange flows
     * 3. Update 2D rainfall from system gages
     * 4. Advance the explicit 2D solver by dt_swmm
     * 5. Transfer outfall discharges into 2D cells
     * 6. Update statistics
     *
     * - Argument ctx: Simulation context.
     * - Argument dt: SWMM routing timestep (seconds).
     * - Argument t: Current simulation time (seconds from start).
     *
     * @details Invoke the pre-outfall and post-routing surface hooks consecutively.
     * - Argument ctx (in,out): Matching prepared engine context with accepted hydraulic state
     * for post-routing.
     * - Argument dt (in): Positive finite routing duration s expected by prepared exchange
     * owners.
     * - Argument t (in): Elapsed routing interval start s, matching completed/pending source
     * clock.
     * @note Inactive router returns. This convenience wrapper does not run the 1D hydraulic
     * solve; SWMMEngine interleaves that solve between the separate hooks. Hook
     * validation/exceptions and partial-state effects apply; caller must enforce the intended
     * hydraulic chronology and exclusive context/router access. Does not itself advance the
     * engine calendar.
     */
    void step(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Pre-routing hook: update outfall boundaries from 2D surface heads.
     *
     * Must be called BEFORE the 1D routing step, after setOutfallDepths().
     *
     * - Argument ctx: Simulation context.
     *
     * @details Freeze outfall tailwater and donor reservations before the matching 1D routing
     * solve.
     * - Argument ctx (in,out): Matching live node/hydraulic state, with native outfall depth
     * setup completed.
     * - Argument dt (in): Routing interval duration s; default zero supplies no positive donor
     * interval and cannot qualify a live joint-outfall tracked interval.
     * @note Inactive router returns. Joint outfalls require a ready source boundary, unused
     * reservation, matching context/time and tracked hydraulic interval. Updates native
     * tailwater, captures prepared outfall budgets, and relaxes tracking ponds without changing
     * their held pond volume. Throws on joint chronology or delegated validation; partial
     * preparation is possible. Exclusive owners required.
     */
    void updateOutfallsPreRouting(SimulationContext& ctx, double dt = 0.0);

    /**
     * @brief Post-routing hook: compute coupling exchange and advance 2D solver.
     *
     * Must be called AFTER the 1D routing step.
     *
     * - Argument ctx: Simulation context.
     * - Argument dt: SWMM routing timestep (seconds).
     * - Argument t: Current simulation time (seconds from start).
     *
     * @details Consume accepted hydraulic exchange and schedule or execute the pending 2D batch.
     * - Argument ctx (in,out): Matching accepted native hydraulic/quality state after its
     * routing solve.
     * - Argument dt (in): Positive finite accepted routing duration s (routing_dt in the
     * definition).
     * - Argument t (in): Elapsed accepted interval start s, matching sim_time_+pending_dt_ for
     * common sources.
     * @note Inactive router returns. Common/joint profiles require ready boundary/context/clock
     * and matching pre-routing outfall reservation. Completes LID thermal interval, updates bed
     * clogging, publishes node native values and applies only accepted reserved outfall
     * transfers. Books conduit seepage/native mass into aquifer or gained quality into native
     * end-node receivers, then freezes next two-way budgets. Accumulates pending time and
     * co-advances when sync threshold (or LID heat) requires; source boundary becomes ready
     * after completion. No 1D solve/engine clock increment. Invalid chronology throws
     * std::invalid_argument; physics/owner/allocation can throw after partial receipts, queues
     * and pending-time changes. Do not retry as an idempotent operation; serialize owners.
     */
    void advancePostRouting(SimulationContext& ctx, double dt, double t);

    /**
     * @brief Prepare a RESET 2D forcing to apply on the next routing step.
     *
     * Flushes any already accumulated macro-window before the new one-shot is recorded, then
     * forces the next routing step to fire a 2D advance so the prescription applies to future
     * time only and is cleared after use.
     *
     * - Argument ctx (in,out): Matching active context used to close old pending time before
     * caller edits forcing.
     * @note Inactive router returns. This method only flushes existing time; it does not
     * record/reset any forcing or independently force the next batch. flushPendingBatch
     * exceptions/partial-state contract apply. Exclusive lifecycle/forcing edit access required.
     */
    void prepareOneShotForcing(SimulationContext& ctx);

    /**
     * @brief Flush the pending (partial) sync batch, if any.
     *
     * Closes the accumulated routing span with one co-advance so the 2D clock reaches the
     * current routing time. A no-op under default per-routing-step coupling (the batch closes
     * every step). Used before a report snapshot — so the report-time blend brackets the report
     * instant — and before a one-shot forcing.
     *
     * - Argument ctx (in,out): Matching live context; common-source mode requires the exact
     * attached identity and a completed ready source boundary.
     * @note Throws std::invalid_argument on common-source readiness mismatch. Inactive/no
     * pending batch returns. Clears pending duration before coAdvanceStep; delegated failure can
     * leave partial state and is not automatically replayable. Exclusive router/context access
     * required.
     */
    void flushPendingBatch(SimulationContext& ctx);

    /**
     * @brief Report-time interpolation weight for the 2D snapshot blend: f = (report_ms −
     * window_old_ms)/(window_new_ms − window_old_ms), clamped to [0,1]; returns 1.0 (pure
     * current state) when no batch has crossed a report instant yet. Unlike the 1D weight there
     * is no legacy bit pattern to reproduce, so the clamp simply absorbs ulp-level clock skew
     * between the router's and the engine's ms accumulators.
     *
     * - Argument report_ms (in): Requested elapsed report time in ms, normally finite.
     * @return Clamped interpolation fraction [0,1] or 1 for no valid old side/nonpositive window
     * span. NaN is not independently rejected and can propagate. Read-only, allocation-free; no
     * flush or refresh.
     */
    double reportWeight(double report_ms) const noexcept;

    /**
     * @brief Book the gage rates a runoff step starting at @p runoff_start_s (seconds since the
     * run start) takes, after the monthly adjustment. The 2D integrates these piecewise-constant
     * rates over each of its own windows, so a cell receives exactly the gage depth the
     * subcatchments do over the same interval.
     *
     * - Argument ctx (in): Matching post-monthly-adjustment native gage rainfall vector,
     * extent>=n_gages.
     * - Argument runoff_start_s (in): Finite nondecreasing elapsed runoff interval start s.
     * @note Integrates previous held rates to this boundary and installs current rates; stores
     * depth at a report instant crossed by the change. Resizes/resets all gage histories when
     * count changes. No water advance or gage cursor mutation; exactly once per accepted runoff
     * rate change and exclusive history access required. Does not reject backwards/nonfinite
     * input; allocation may partially reset.
     */
    void bookGageRates(const SimulationContext& ctx, double runoff_start_s);

    /**
     * @brief Per-cell rainfall (m/s) as the .out reports subcatchment rainfall: the source gage
     * rate at @p report_date, independent of runoff cursor, mapped through RAINFALL_MODE, with
     * the per-cell rainfall forcings applied.
     *
     * - Argument ctx (in): Prepared matching gage/time-series/report-unit configuration.
     * - Argument report_date (in): Absolute engine DateTime days, not elapsed seconds.
     * - Argument out (out): Resized per-cell rainfall m/s, mapped by rainfall policy then
     * composed with current per-cell replace/add forcing arrays; previous contents replaced.
     * @note Queries reporting series independently of runoff-held rate history. No canonical
     * water/rain cumulative/clock advance; may allocate and delegates series lookup. Stable
     * model/forcing and exclusive output buffer required; no generic finite/shape repair.
     */
    void reportRainfall(const SimulationContext& ctx, double report_date,
                        std::vector<double>& out) const;

    /**
     * @brief Rainfall volume (m³) integrated to the report boundary, excluding the part of a
     * routing/sync window after that boundary. Runtime totals remain available through
     * rainCumulative().
     *
     * @param[in] report_ms Requested engine report boundary in elapsed milliseconds; compared
     * exactly to stored report_rain_ms_, without tolerance or finite-value checks.
     * @return Borrowed report-boundary integrated m3 vector only on exact timestamp equality,
     * otherwise current runtime rain_cum_. Does not interpolate, integrate, flush or consume
     * source history. Router must remain stable; later rainfall/report bookkeeping can change
     * either vector/invalidate views.
     */
    const std::vector<double>& reportRainCumulative(double report_ms) const noexcept {
        return report_rain_ms_ == report_ms ? report_rain_cum_ : rain_cum_;
    }

    /**
     * @brief Cell depth (m) at the start of the sync batch that crossed the current report
     * instant (meaningful when reportWeight() < 1).
     *
     * @details Borrow surface depth at the old side of the last report-crossing sync batch.
     * @return Router-owned const reference in m per mesh cell. Valid only during stable router
     * access; initialization, restore, replacement or corresponding producer updates can change
     * contents/invalidate views. No copy, reset, queue consumption, ownership transfer or
     * synchronization occurs in this query.
     */
    const std::vector<double>& reportOldDepth() const noexcept { return report_old_depth_; }
    /**
     * @brief Cell head (m) at the start of that batch.
     *
     * @details Borrow surface head at the old side of the last report-crossing sync batch.
     * @return Router-owned const reference in absolute SI elevation m per cell. Valid only
     * during stable router access; initialization, restore, replacement or corresponding
     * producer updates can change contents/invalidate views. No copy, reset, queue consumption,
     * ownership transfer or synchronization occurs in this query.
     */
    const std::vector<double>& reportOldHead() const noexcept { return report_old_head_; }

    /**
     * @brief Finalize the 2D module at simulation end.
     *
     * Flushes any partial macro-step window (routing time accumulated since the last 2D advance)
     * so the 2D clock ends at the simulation end instead of up to one window short, then
     * releases the solver.
     *
     * - Argument ctx: Simulation context (needed for the flush advance).
     *
     * - Argument ctx (in,out): Matching live context for pending physics, solver statistics and
     * warning receipts.
     * @note Flushes remaining batch first; captures solver statistics and finalizes solver
     * internals, then records dispersion limiter advisories, clears source/checkpoint
     * attachments and marks inactive. Retains router mesh/state and solver object for
     * inspection; does not destroy the solver pointer. Delegated advance/finalize or warning
     * allocation can throw after partial cleanup. Must be called serially with borrowed owners
     * still alive, before their destruction.
     */
    void finalize(SimulationContext& ctx);

    /**
     * @brief Check if the 2D module is active.
     *
     * @return Stored router activation flag. Does not validate borrowed solver/context bindings,
     * inspect current wetness or complete a pending batch. Read-only and allocation-free;
     * serialize with lifecycle.
     */
    bool isActive() const noexcept { return active_; }
    /**
     * @brief Prepare the next finite Richards LID/mesh acceptance and conduit donor budgets.
     * - Argument dt (in): Positive finite interval duration s.
     * - Argument evaporation (in): Held native evaporation passed to the LID admission owner.
     * @note The unnamed writable context reference is the matching prepared native LID/hydraulic
     * owner. Delegates admission against mesh/GW state and republishes gain budgets when
     * required. May throw after partial preparation; requires exclusive interval ownership.
     * Definition details receipts.
     */
    void prepareLidStep(SimulationContext&, double dt, double evaporation);

    /**
     * @brief Thread-count advisories produced by initialize() (THREADS above the machine limits,
     * triangle gate reducing an explicit request). The engine forwards them to the report
     * warnings and the host warning callback.
     *
     * @details Borrow initialization thread-count advisories.
     * @return Router-owned const reference in owned diagnostic strings. Valid only during stable
     * router access; initialization, restore, replacement or corresponding producer updates can
     * change contents/invalidate views. No copy, reset, queue consumption, ownership transfer or
     * synchronization occurs in this query.
     */
    const std::vector<std::string>& threadWarnings() const noexcept { return thread_warnings_; }

    /**
     * @brief Windowless-coupling stabilizer: per coupled node, the exchange head sensitivity G =
     * Σ_points −∂Q/∂h_1d ≥ 0 converted to 1D units (ft³/s per ft), for the caller to add into
     * the dynamic-wave `sumdqdh` each Picard iteration. Appends (node_idx, G) pairs; cheap
     * (O(points)).
     *
     * @details Append positive live non-outfall coupling head derivatives in native hydraulic
     * units.
     * - Argument ctx (in): Matching live native node heads/depths and prepared coupling model.
     * - Argument out (in,out): Existing vector retained; appends (zero-based node index,G) per
     * positive point, where G is ft3/s per ft of native 1D head. Entries for shared nodes are
     * not aggregated here.
     * @note Inactive router returns. Evaluates current SI law then multiplies flow and length
     * conversion; no state/canonical water/calendar mutation. Allocation may leave an appended
     * prefix. Requires stable mesh/native views; caller sums point derivatives in its hydraulic
     * assembly.
     */
    void computeCouplingConductances(const SimulationContext& ctx,
                                     std::vector<std::pair<int, double>>& out) const;

    /**
     * @brief Make the parsed mesh editable without a full initialize().
     *
     * The GUI keeps the engine in OPENED (not INITIALIZED) state so 1D property edits stay
     * legal. In that state the 2D mesh is parsed but the [2D_BOUNDARY_CONDITIONS] /
     * [2D_EDGE_CONVEYANCE] rows still live in the pending-row buffers, which the serializer
     * prefers over live state. This drains those rows into BoundaryData / mesh edge slots (the
     * same drain initialize() performs) so per-edge API edits take effect and are written on
     * save. No-op once drained or when no mesh is loaded.
     */
    void prepareForEdit();

    /**
     * @brief Access mesh data (read-only).
     *
     * @details Borrow the owned mesh geometry/topology; runtime coordinates are SI after
     * initialization, authoring may precede conversion.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const MeshData& mesh() const noexcept { return mesh_; }

    /**
     * @brief Access mesh data (mutable, for input parsing).
     *
     * @details Borrow the owned mesh geometry/topology; runtime coordinates are SI after
     * initialization, authoring may precede conversion.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    MeshData& mesh() noexcept { return mesh_; }

    /**
     * @brief Access surface state (read-only).
     *
     * @details Borrow the owned canonical surface water/native/thermal state and pending
     * producer receipts.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const SurfaceStateData& state() const noexcept { return state_; }

    /**
     * @brief Access surface state (mutable, for forcing).
     *
     * @details Borrow the owned canonical surface water/native/thermal state and pending
     * producer receipts.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    SurfaceStateData& state() noexcept { return state_; }

    /**
     * @brief Signed native surface reaction loss for the qualified RK2 owner.
     *
     * @details Read a CPU RK2 surface native reaction loss without consuming its receipt.
     * - Argument row (in): Zero-based canonical native row; invalid rows return zero through CPU
     * owner.
     * @return Cumulative signed native inventory loss for supported CPU row, otherwise zero
     * (including plugin/no-2D build). No conversion, allocation, physical/clock mutation or
     * report delivery occurs.
     */
    double surfaceReactionLoss(int row) const noexcept;

    /**
     * @brief Access solver options (read-only).
     *
     * @details Borrow the owned resolved surface solver controls, SI for runtime geometry/rates.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const SolverOptions2D& options() const noexcept { return options_; }

    /**
     * @brief Access solver options (mutable).
     *
     * @details Borrow the owned resolved surface solver controls, SI for runtime geometry/rates.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    SolverOptions2D& options() noexcept { return options_; }

    /**
     * @brief Access per-edge boundary-condition data (read-only).
     *
     * @details Borrow the owned per-edge boundary descriptors/forcing/receipts, indexed by
     * MeshData::slot.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const BoundaryData& boundary() const noexcept { return boundary_; }

    /**
     * @brief Access per-edge boundary-condition data (mutable, for forcing/parsing).
     *
     * @details Borrow the owned per-edge boundary descriptors/forcing/receipts, indexed by
     * MeshData::slot.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    BoundaryData& boundary() noexcept { return boundary_; }

    /**
     * @brief Announce that a boundary slot's TYPE changed (the API BC-type setter), so the
     * compact non-WALL slot list the per-step boundary passes walk is rebuilt before it is next
     * used.
     *
     * @details Test active CPU support for the runtime prescription API route.
     * @return True for an active CPU marcher and either non-FULL_SWE or qualified coupled
     * second-order FULL_SWE with groundwater and no common stage, with no foreign joint owner.
     * This is a capability gate, not full current-boundary validation. No allocation, state
     * change or lifecycle synchronization.
     */
    bool runtimeCpuAvailable() const noexcept;
    /**
     * @brief Borrow mutable runtime prescriptions/receipts for the surface owner.
     * @return Router-owned state_.runtime_sources. Caller preserves validated row identity,
     * per-cell lookup, units and accepted phase, and serializes mutations with
     * routing/checkpointing. No callback, validation, cache rebuild or ownership activation
     * occurs; external provider resources remain borrowed.
     */
    RuntimeSources& runtimeSources() noexcept { return state_.runtime_sources; }
    /**
     * @brief Test whether an external common-source stage pointer is attached.
     * @return common_stage_!=nullptr, independent of its current eligibility/validity. Does not
     * dereference it, validate the owner lifetime or activate authored ownership. No allocation
     * or state change.
     */
    bool commonSourceAttached() const noexcept { return common_stage_ != nullptr; }
    /**
     * @brief Borrow the externally attached common-source stage, if any.
     * @return Nonowning const pointer, null when detached. Stage must outlive router/marcher
     * uses; attachments/restore/replacement can change the pointer. No stage advance, copy or
     * validation occurs.
     */
    const CommonSourceStage* commonSourceStage() const noexcept { return common_stage_; }
    /**
     * @brief Test qualification ownership for managed runtime water checkpointing.
     * @return True for attached standalone RK2 owner or common source with runtime water
     * enabled. Read-only, allocation-free; does not validate a complete boundary or save/restore
     * anything.
     */
    bool runtimeWaterCheckpointing() const noexcept;
    /**
     * @brief Test whether the attached common source owns runtime boundary checkpointing.
     * @return True only with common source and its runtime-boundary enable. No state validation,
     * mutation or allocation occurs; standalone RK2 uses its separate qualification route.
     */
    bool runtimeBoundaryCheckpointing() const noexcept;
    /**
     * @brief Joint checkpoints of a model with coupled outfalls carry each one's tailwater
     * record — a distinct checkpoint version.
     *
     * @return True when a joint hydraulic owner is attached and prepared outfall points are
     * nonempty. This qualifies the specific tailwater checkpoint extension, not whole-engine
     * restart validity. No reservation, file access, mutation or synchronization occurs.
     */
    bool outfallTailwaterCheckpointing() const noexcept {
        return joint_hydraulic_ && !outfall_exchange_.points_.empty();
    }
    /**
     * @brief Coupled-outfall delivery and tailwater records (report).
     *
     * @details Borrow coupled outfall reservations, accepted receipts and tailwater diagnostics.
     * @return Router-owned const reference in SI water m3 and native 1D tailwater length with
     * elapsed seconds. Valid only during stable router access; initialization, restore,
     * replacement or corresponding producer updates can change contents/invalidate views. No
     * copy, reset, queue consumption, ownership transfer or synchronization occurs in this
     * query.
     */
    const OutfallExchange& outfallExchange() const noexcept { return outfall_exchange_; }
    /**
     * @brief Validate copied runtime water proposals through the attached common-source owner.
     * - Argument time (in): Proposed finite elapsed boundary s, matching the prepared source
     * clock.
     * - Argument boundary (in): Optional borrowed proposed boundary; required by its active
     * profile.
     * @return Delegated diagnostic, or empty without validation when no common stage is
     * attached.
     * @note First three unnamed const references are proposed surface sources, groundwater
     * sources and held forcings, in that order, with matching cell/native layouts. No
     * adoption/physics advance. Read-only stable views and exclusive source lifecycle required;
     * allocation/delegated checks may throw.
     */
    std::string validateRuntimeWater(const RuntimeSources&, const RuntimeSources&,
                                     const RuntimeForcings&, double time,
                                     const BoundaryData* boundary = nullptr) const;
    /**
     * @brief Test whether any runtime surface/aquifer source, boundary override or forcing is
     * retained.
     * @return OR of stored active source/forcing states and nonempty boundary runtime_original
     * map. Does not expire stale prescriptions, validate callbacks or imply every source is
     * enabled. Read-only; requires stable matching owners, no allocation or physical update.
     */
    bool runtimeActive() const noexcept {
        return state_.runtime_sources.active() || subsurface_.runtimeSources().active() ||
               !boundary_.runtime_original.empty() || state_.runtime_forcings.active();
    }
    /**
     * @brief Read the earliest scheduled source/forcing transition after a given time.
     * @param[in] now Finite elapsed time s, in the same origin as runtime source/forcing
     * windows.
     * @return Minimum nextTime() across surface sources, aquifer sources and forcing; infinity
     * if none. Does not include unrelated reporting, project boundary-series or solver stability
     * cuts. No queue consumption or mutation; caller serializes scheduling state with runtime
     * edits.
     */
    double nextRuntimeTime(double now) const noexcept {
        return std::min({state_.runtime_sources.nextTime(now),
                         subsurface_.runtimeSources().nextTime(now),
                         state_.runtime_forcings.nextTime(now)});
    }
    /**
     * @brief Retire all runtime source and forcing entries at an accepted completed horizon.
     * @param[in] now Finite elapsed completed time s used by each owner expire() policy.
     * @note Applies surface sources, aquifer sources then forcings, including their prescribed
     * expiry/ receipt handling. Does not restore boundary runtime_original slots itself or
     * advance physical water. Requires exclusive validated owner access; exceptions can leave
     * earlier owners expired. No whole- router rollback is provided by this wrapper.
     */
    void expireRuntime(double now) {
        state_.runtime_sources.expire(now);
        subsurface_.runtimeSources().expire(now);
        state_.runtime_forcings.expire(now);
    }
    /**
     * @brief Mark the compact non-wall boundary index stale and refresh attached CPU boundary
     * work lists.
     * @note No time-series name reset or physics/clock advance. CPU refresh may allocate/throw
     * after the dirty flag is set and can alter pin/layout caches. Call serially after
     * compatible boundary edits; retained work-list views may become invalid.
     */
    void invalidateBoundaryIndex();
    /**
     * @brief Mark symbolic boundary names unresolved after authoring/name changes.
     * @note Sets boundary_names_resolved_=false only. Does not modify slots, force immediate
     * name resolution, rebuild compact solver topology or alter existing forcing. Caller
     * serializes edits.
     */
    void invalidateBoundaryNames() noexcept { boundary_names_resolved_ = false; }

    /**
     * @brief Bring the render fields (face velocity, vertex heads, output gradients, vertex render depths) up
     * to date with the current solver state if a batch has run since they were last computed.
     * The API getters that expose those fields call this so a reader arriving between reports is
     * never served a stale field; it is a no-op at a report instant, where the refresh already
     * ran.
     *
     * @note When render_dirty_ is true, mutates derived head/gradient/render-depth caches by
     * calling refreshRenderFields(); otherwise no-op. Requires live matching geometry/solver
     * state and exclusive cache access. May allocate/throw through reconstruction; does not
     * advance physical solver/source clocks.
     */
    void refreshRenderFieldsIfStale() {
        if (render_dirty_)
            refreshRenderFields();
    }

    /**
     * @brief Access the per-cell infiltration model (plan §5.5, track I).
     *
     * Mutable so SWMMEngine can publish it on `ctx.twod_io.infil` — the [2D_INFILTRATION*]
     * section handlers and the InpWriter reach it there. The router owns the object, resolves it
     * against the mesh in initialize(), and drives its INFIL_STEP cadence in coAdvanceStep().
     *
     * @details Borrow the owned per-cell infiltration authoring, held capacity and model memory.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    Infil2D& infil() noexcept { return infil_; }
    /**
     * @brief Borrow the owned per-cell infiltration authoring, held capacity and model memory.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const Infil2D& infil() const noexcept { return infil_; }

    /**
     * @brief U4 (2026-09-07): the `[GW_*]` subsurface-transport authoring rows. Reached by the
     * parser, the writer and the C API through SimulationContext::twod_io.gw; resolved owners
     * consume them during initialization.
     *
     * @details Borrow the owned groundwater quality/thermal/source authoring and resolved SI
     * transport metadata.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    GwTransportData& gwTransport() noexcept { return gw_; }
    /**
     * @brief Borrow the owned groundwater quality/thermal/source authoring and resolved SI
     * transport metadata.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const GwTransportData& gwTransport() const noexcept { return gw_; }
    /**
     * @brief S7: cell coverages / buildup / washoff / sweeping (authoring + state).
     *
     * @details Borrow the owned land-use coverage/buildup/washoff/sweeping authoring and runtime
     * ledgers.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    SurfaceQuality2D& surfaceQuality() noexcept { return surface_quality_; }
    /**
     * @brief Borrow the owned land-use coverage/buildup/washoff/sweeping authoring and runtime
     * ledgers.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const SurfaceQuality2D& surfaceQuality() const noexcept { return surface_quality_; }

    /**
     * @brief G1: the authored `[2D_AQUIFER*]` rows, in the user's own units. Reached by the
     * section handlers and the InpWriter through `SimulationContext::twod_io.aquifer`.
     *
     * @details Borrow the owned aquifer authoring rows, retained in project input units.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    SubsurfaceConfig& aquiferConfig() noexcept { return aquifer_cfg_; }
    /**
     * @brief Borrow the owned aquifer authoring rows, retained in project input units.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const SubsurfaceConfig& aquiferConfig() const noexcept { return aquifer_cfg_; }
    /**
     * @brief Borrow the owned node names parallel to aquiferConfig().node_beds authoring
     * identity.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    std::vector<std::string>& aquiferNodeNames() noexcept { return aquifer_node_names_; }
    /**
     * @brief Borrow the owned node names parallel to aquiferConfig().node_beds authoring
     * identity.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const std::vector<std::string>& aquiferNodeNames() const noexcept {
        return aquifer_node_names_;
    }
    /**
     * @brief G-X4: the `[2D_AQUIFER_LINKS]` row names, parallel to `aquiferConfig().link_rows` —
     * the writer's round-trip key.
     *
     * @details Borrow the owned conduit names parallel to aquiferConfig().link_rows authoring
     * identity.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    std::vector<std::string>& aquiferLinkNames() noexcept { return aquifer_link_names_; }
    /**
     * @brief Borrow the owned conduit names parallel to aquiferConfig().link_rows authoring
     * identity.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const std::vector<std::string>& aquiferLinkNames() const noexcept {
        return aquifer_link_names_;
    }
    /**
     * @brief The running kernel. `active()` is false until `[2D_AQUIFER]` resolves.
     *
     * @details Borrow the owned groundwater solver with SI hydraulic state, native rows and
     * paired physical/source/LID receipts.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    SubsurfaceSolver& subsurface() noexcept { return subsurface_; }
    /**
     * @brief Borrow the owned groundwater solver with SI hydraulic state, native rows and paired
     * physical/source/LID receipts.
     * @return Reference to this router's member, with mutability determined by the overload. No
     * copy, validation or ownership transfer occurs. Router must remain alive/stable;
     * initialization, restore, move/replacement and vector edits can invalidate contained views.
     * @note Mutable access requires exclusive ownership and preservation of prepared topology,
     * row shapes, unit convention, source clocks and receipt phase. Authoring edits may require
     * resolution/rebuild before routing. This getter performs no automatic cache/solver
     * synchronization or physical update.
     */
    const SubsurfaceSolver& subsurface() const noexcept { return subsurface_; }

    /**
     * @brief Ledger-consistent cumulative infiltrated depth per cell (m).
     *
     * @details Drains the SAME `SurfaceStateData::infil_applied` accumulator that
     * `MassBalance2D::infil_out` books — the depth the marcher actually removed, summed as it
     * was removed — so `sum(infilCumulative()[i] * tri_area[i]) == infil_out` holds by
     * construction. Prefer this over `Infil2D::cumulative()`, which is the unramped capacity the
     * kernels offered and therefore exceeds the applied loss on drying cells. Empty when no
     * `[2D_INFILTRATION*]` model resolved.
     */
    /**
     * @brief U3 (track I-b) — per-subcatchment 2D infiltration recharge pending delivery to the
     * legacy aquifer, in m³.
     *
     * @details Filled in accumulateMassBalance() for every cell whose resolved row says
     * SUBCATCH_AQUIFER and that a subcatchment contains. `drainSubcatchRecharge()` hands it to
     * the runoff step (which adds it to that subcatchment's infiltration rate for
     * GWSolver::execute) and zeroes it, so the accumulator is drained exactly once per runoff
     * step. Empty when the destination is not in use.
     *
     * @details Borrow unconsumed infiltration recharge awaiting legacy subcatchment delivery.
     * @return Router-owned const reference in m3 per original subcatchment. Valid only during
     * stable router access; initialization, restore, replacement or corresponding producer
     * updates can change contents/invalidate views. No copy, reset, queue consumption, ownership
     * transfer or synchronization occurs in this query.
     */
    const std::vector<double>& subcatchRecharge() const noexcept { return subcatch_recharge_; }
    /**
     * @brief Move the pending recharge into @p out (m³ per subcatchment) and zero the
     * accumulator. @p out is sized to the accumulator (empty when the destination is not in
     * use).
     *
     * @param[out] out Replaced by pending SI m3 per original subcatchment, then the owned
     * accumulator is zeroed. Must not alias subcatch_recharge_ itself. Empty destination mapping
     * yields empty output.
     * @note Copy allocation can throw before the accumulator is cleared. Call once on the
     * matching runoff handoff cadence; does not inject legacy groundwater itself or update
     * canonical hydraulic storage. Requires exclusive accumulator/consumer access to avoid
     * duplicate delivery.
     */
    void drainSubcatchRecharge(std::vector<double>& out) {
        out = subcatch_recharge_;
        std::fill(subcatch_recharge_.begin(), subcatch_recharge_.end(), 0.0);
    }
    /**
     * @brief Cell → containing subcatchment index (-1 = none). Empty unless the SUBCATCH_AQUIFER
     * destination resolved.
     *
     * @details Borrow resolved recharge destination lookup.
     * @return Router-owned const reference in zero-based containing subcatchment per cell, -1
     * for none. Valid only during stable router access; initialization, restore, replacement or
     * corresponding producer updates can change contents/invalidate views. No copy, reset, queue
     * consumption, ownership transfer or synchronization occurs in this query.
     */
    const std::vector<int>& cellSubcatchment() const noexcept { return cell_subcatch_; }

    /**
     * @brief Read elapsed duration since the last held infiltration model rate refresh.
     * @return Stored duration s on router/co-advance cadence, not total simulation time or
     * marcher substep count. No rate update, reset or allocation occurs; caller reads stable
     * state.
     */
    double infiltrationElapsed() const noexcept { return infil_elapsed_; }
    /**
     * @brief Replace router applied-infiltration reporting and held-model elapsed accounting.
     * @param[in] cumulative Matching per-cell actually applied depth m, copied verbatim with no
     * shape, sign or finite-value validation.
     * @param[in] elapsed Finite nonnegative held-rate elapsed duration s, assigned without
     * validation.
     * @note Does not restore Infil2D physical model memory, state_.infil_applied, mass balance
     * or groundwater receipts; caller coordinates the full qualified restore. Copy may throw
     * before elapsed assignment. Requires exclusive prepared owner access; element views may be
     * invalidated.
     */
    void restoreInfiltration(const std::vector<double>& cumulative, double elapsed) {
        infil_cum_applied_ = cumulative;
        infil_elapsed_ = elapsed;
    }
    /**
     * @brief Borrow actually applied cumulative infiltration depth.
     * @return Router-owned const reference in m per mesh cell, empty when no model resolved.
     * Valid only during stable router access; initialization, restore, replacement or
     * corresponding producer updates can change contents/invalidate views. No copy, reset, queue
     * consumption, ownership transfer or synchronization occurs in this query.
     */
    const std::vector<double>& infilCumulative() const noexcept { return infil_cum_applied_; }

    /*!
     * \brief Signed cumulative coupling exchange per cell (m³).
     *
     * \details Positive = water the 1D node delivered INTO this cell (spill,
     *          submerged-outfall discharge); negative = water the node
     *          abstracted FROM it (drain). Summing the vector gives
     *          `coupling_1d_to_2d_in − coupling_2d_to_1d_out`, which is the
     *          check that ties this series to the domain totals.
     */
    /**
     * @brief Borrow signed accepted cumulative native-node/surface exchange.
     * @return Router-owned const reference in m3 per cell, positive into surface. Valid only
     * during stable router access; initialization, restore, replacement or corresponding
     * producer updates can change contents/invalidate views. No copy, reset, queue consumption,
     * ownership transfer or synchronization occurs in this query.
     */
    const std::vector<double>& couplingCumulative() const noexcept { return coupling_cum_applied_; }

    /**
     * @brief Ledger-consistent cumulative rainfall volume per cell (m³).
     *
     * @details Booked in accumulateMassBalance() from the SAME per-cell rainfall term that feeds
     * `MassBalance2D::rainfall_in`, so `sum(rainCumulative()[i]) == rainfall_in` holds by
     * construction. Sized [n_triangles] from initialize(); zero-filled when no rain.
     *
     * @details Borrow accepted cumulative rainfall source volume.
     * @return Router-owned const reference in m3 per mesh cell. Valid only during stable router
     * access; initialization, restore, replacement or corresponding producer updates can change
     * contents/invalidate views. No copy, reset, queue consumption, ownership transfer or
     * synchronization occurs in this query.
     */
    const std::vector<double>& rainCumulative() const noexcept { return rain_cum_; }

    /**
     * @brief Static rainfall weights built at initialize() (diagnostic inspection).
     *
     * @details Borrow prepared static gage-to-cell weights and mapping.
     * @return Router-owned const reference in SI mesh identity and the configured rainfall
     * interpolation policy. Valid only during stable router access; initialization, restore,
     * replacement or corresponding producer updates can change contents/invalidate views. No
     * copy, reset, queue consumption, ownership transfer or synchronization occurs in this
     * query.
     */
    const RainfallInterpolator& rainfallInterpolator() const noexcept { return interp_; }

    /**
     * @brief Per-row buffer for `[2D_BOUNDARY_CONDITIONS]` parse output.
     *
     * V-E3. Populated by the input parser during reading (before the mesh is finalized), drained
     * into `boundary_` during `initialize()` after `boundary_.resize()` allocates the per-edge
     * slots. Retained after the drain so serialization (InpWriter / GeoPackage) can re-emit the
     * authored rows (group label, TS-vs-constant choice). Hoisted to data/PendingRows2D.hpp;
     * alias kept for call sites.
     */
    using PendingBoundaryRow = twoD::PendingBoundaryRow;
    using PendingInitialQualityRow = twoD::PendingInitialQualityRow;
    using PendingInitialBedQualityRow = twoD::PendingInitialBedQualityRow;
    using PendingBoundaryQualityRow = twoD::PendingBoundaryQualityRow;
    /**
     * @brief S1/S2: raw [2D_INITIAL_QUALITY] rows, resolved at initialize().
     *
     * @details Borrow retained surface initial native quality authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    std::vector<PendingInitialQualityRow>& pendingInitialQualityRows() noexcept {
        return pending_iq_rows_;
    }
    /**
     * @brief Borrow retained local-bed initial native quality authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    std::vector<PendingInitialBedQualityRow>& pendingInitialBedQualityRows() noexcept {
        return pending_ibq_rows_;
    }
    /**
     * @brief Borrow retained boundary native quality authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    std::vector<PendingBoundaryQualityRow>& pendingBoundaryQualityRows() noexcept {
        return pending_bq_rows_;
    }
    /**
     * @brief Borrow retained hydraulic boundary authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    std::vector<PendingBoundaryRow>& pendingBCRows() noexcept { return pending_bc_rows_; }
    /**
     * @brief Borrow retained hydraulic boundary authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    const std::vector<PendingBoundaryRow>& pendingBCRows() const noexcept {
        return pending_bc_rows_;
    }

    /**
     * @brief Per-row buffer for `[2D_EDGE_CONVEYANCE]` parse output (§11A).
     *
     * Populated by the input parser during reading (vertices known but mesh topology not yet
     * built), drained into `mesh_.edge_conveyance` during `initialize()` after
     * `buildMeshTopology` has populated the neighbour table. Mirrored to both slots of an
     * interior edge so antisymmetric FV flux integration stays mass-conservative. Retained after
     * the drain for faithful re-serialization.
     */
    using PendingEdgeConveyanceRow = twoD::PendingEdgeConveyanceRow;
    /**
     * @brief Borrow retained interior/exterior edge conveyance authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    std::vector<PendingEdgeConveyanceRow>& pendingEdgeConveyanceRows() noexcept {
        return pending_edge_conveyance_rows_;
    }
    /**
     * @brief Borrow retained interior/exterior edge conveyance authoring rows.
     * @return Router-owned row vector reference, mutable only in the mutable overload. Kept for
     * faithful input serialization and later resolution; changing it does not automatically
     * update prepared live mesh/solver fields. No copy/validation/ownership transfer occurs.
     * @note Caller preserves input unit/one-based token versus internal index conventions
     * defined by PendingRows2D and coordinates drain/resolution before use. Router/vector
     * mutations invalidate views; exclusive edit access is required.
     */
    const std::vector<PendingEdgeConveyanceRow>& pendingEdgeConveyanceRows() const noexcept {
        return pending_edge_conveyance_rows_;
    }

    /**
     * @brief Get total 2D surface volume (sum of depth * area).
     *
     * @details Sum canonical surface water volume over all prepared mesh cells.
     * @return Surface storage m3, in ascending cell order; no depth*area reconstruction or
     * finite repair.
     * @note Requires state.volume extent>=n_cells and stable exclusive/const-consistent model.
     * No physics, allocation or clock change; active flag is not checked.
     */
    double totalVolume() const;

    /**
     * @brief Get total exchange flow (sum of coupling flows, m³/s).
     *
     * @details Sum the current held surface coupling source-rate field over cell areas.
     * @return Sum coupling_flux[i]*area[i] in m3/s, positive into surface. This is not the
     * accepted per-point integrated drain receipt and does not consume any queue.
     * @note Requires matching prepared cell arrays; active flag is not checked. Read-only, no
     * validation, reconstruction, allocation, physical or clock change.
     */
    double totalExchangeFlow() const;
#ifdef OPENSWMM_HAS_2D
    /**
     * @brief Access explicit-marcher sub-step statistics.
     *
     * @details Read the last surface advance's base substep count.
     * @return Delegated last_num_steps() when the optional backend exists; zero otherwise or
     * when built without 2D. No solver advance/device synchronization or reset; access
     * serialized with lifecycle.
     */
    long lastSolverSteps() const { return solver_ ? solver_->last_num_steps() : 0; }
    /**
     * @brief Read the last accepted surface base/tail step duration.
     * @return Delegated last_step_size() in seconds when a backend exists, zero otherwise or
     * without 2D. No stability recomputation/device synchronization or reset; access serialized
     * with lifecycle.
     */
    double lastSolverStepSize() const { return solver_ ? solver_->last_step_size() : 0.0; }
    /**
     * @brief Cumulative marcher statistics (ISurfaceSolver::RunStats): substeps, face-kernel
     * evaluations, active-cell fractions and LTS tier occupancy. Zeros once the solver has been
     * finalised.
     *
     * @return Value snapshot delegated to the live optional backend, otherwise default zero
     * RunStats. Active fractions/tier occupancy follow backend sampling semantics; not
     * instantaneous wet area or elapsed-time-weighted statistics. Does not advance, reset or
     * synchronize a device explicitly.
     */
    ISurfaceSolver::RunStats runStats() const {
        return solver_ ? solver_->run_stats() : ISurfaceSolver::RunStats{};
    }
#else
    /**
     * @brief Read the last surface advance's base substep count.
     * @return Delegated last_num_steps() when the optional backend exists; zero otherwise or
     * when built without 2D. No solver advance/device synchronization or reset; access
     * serialized with lifecycle.
     */
    long lastSolverSteps() const { return 0; }
    /**
     * @brief Read the last accepted surface base/tail step duration.
     * @return Delegated last_step_size() in seconds when a backend exists, zero otherwise or
     * without 2D. No stability recomputation/device synchronization or reset; access serialized
     * with lifecycle.
     */
    double lastSolverStepSize() const { return 0.0; }
#endif
    /**
     * @brief Backend label chosen by SurfaceSolverFactory at initialize() ("cpu (explicit
     * marcher)", "omp (…)", …); empty before that.
     *
     * @details Borrow selected surface solver backend label.
     * @return Router-owned const reference in owned UTF-8 string, empty before backend
     * selection. Valid only during stable router access; initialization, restore, replacement or
     * corresponding producer updates can change contents/invalidate views. No copy, reset, queue
     * consumption, ownership transfer or synchronization occurs in this query.
     */
    const std::string& backendName() const noexcept { return backend_name_; }

private:
    friend class openswmm::SWMMEngine;
    struct RestartState;
    /**
     * @brief Encode the qualified completed router/CPU/source and optional joint/channel
     * boundary.
     * @return Empty on success, otherwise qualification/receipt/archive/size diagnostic. First
     * unnamed const context reference must be exact attached stable owner; second unnamed
     * writable string is replaced only on complete successful encoding and retained on
     * diagnostics.
     * @note Copies private state without flushing pending time or advancing physics. Exclusive
     * matching owners required; source definition details serialized scope, platform guards and
     * checksum.
     */
    std::string checkpointBytes(const SimulationContext&, std::string&) const;
    /**
     * @brief Validate, inspect or install a router checkpoint through staged matching component
     * owners.
     * - Argument engine_boundary (in): Optional exact [elapsed s,absolute DateTime days,elapsed
     * ms,old ms, next-report ms,native evaporation ft/s] guard.
     * - Argument engine_rainfall (in): Optional exact native per-gage rainfall guard.
     * - Argument engine_storage (in): Optional native hydraulic session storage guard at
     * positive saved time.
     * - Argument engine_routed (in): Hydraulic storage-validation routing mode.
     * - Argument validate_source (in): Optional pointer to nonempty staged-source callback;
     * forbidden for managed RK2, callback views last only during the call.
     * - Argument before_install (in): Optional pointer to nonempty post-validation/pre-install
     * callback; external effects are not rolled back.
     * - Argument inspect_source_end (out): Optional accepted numerical/source time s; nonnull
     * selects validation-only return and skips installation/final hook.
     * - Argument prepared_climate (in): Optional borrowed managed RK2 prepared weather identity,
     * call lifetime.
     * - Argument prepared_heat (in): Optional borrowed managed RK2 prepared heat identity, call
     * lifetime.
     * @return Empty on success/inspection, otherwise
     * qualification/freshness/envelope/model/receipt or caught standard exception diagnostic.
     * @note First unnamed writable context is the exact fresh attached owner (zero
     * completed/pending router/native time); second unnamed string is an owned bounded envelope
     * consumed locally. Decodes private candidates and validates cross-component clocks,
     * water/native/outfall/boundary receipts before nonthrowing adoption. Requires exclusive
     * stable prepared owners. Definition supplies exact scope and callback behavior; unrelated
     * whole-engine/output state is excluded.
     */
    std::string loadCheckpointBytes(
        SimulationContext&, std::string, const std::array<double, 6>* engine_boundary = nullptr,
        const std::vector<double>* engine_rainfall = nullptr,
        const double* engine_storage = nullptr, bool engine_routed = false,
        const std::function<void(const runoff::SourceWaterDriver&)>* validate_source = nullptr,
        const std::function<void()>* before_install = nullptr, double* inspect_source_end = nullptr,
        const climate::ClimateState* prepared_climate = nullptr,
        const HeatState* prepared_heat = nullptr);
    /**
     * @brief Bind initial borrowed all-cell source ownership with optional tracked hydraulic
     * pairing.
     * @note Unnamed arguments are exact writable prepared context, matching writable common
     * stage and nullable hydraulic owner pointer, respectively. All borrowed owners must remain
     * alive/stable. Requires fresh active first-order CPU, zero clocks/pending time, compatible
     * all-cell/native/LID/ boundary profile and no unsupported external donors. Refusal throws
     * std::invalid_argument; allocation/setup can leave partial preparatory ledger/view changes.
     * No physics/calendar advance; definition documents complete qualification and installed
     * identities. Exclusive lifecycle access.
     */
    void attachCommonSourceStage(SimulationContext&, CommonSourceStage&, Router*);
    /**
     * @brief Test whether the router has attached standalone second-order checkpoint ownership.
     * @return True for nonnull source context, reconstruction order 2 and no common stage.
     * @note Allocation-free flag/identity query; does not qualify current physics or receipt
     * boundary.
     */
    bool standaloneRk2Checkpointing() const noexcept;
    /**
     * @brief Bind the initial managed standalone RK2 checkpoint context/model.
     * @note Unnamed context reference is the exact prepared zero-time hydraulic session owner.
     * Requires no prior source/common attachment and zero pending/numerical time. Validates
     * coupled standalone profile before marking ready; refusal clears new context/model and
     * throws std::invalid_argument. Borrowed owners remain externally owned/alive/stable. No
     * physics advance; serial lifecycle only.
     */
    void attachStandaloneRk2Checkpoint(SimulationContext&);
    /**
     * @brief Validate the accepted managed standalone coupled RK2 router boundary.
     * @return Empty on qualification, otherwise owner/profile/model/replay-ID/clock/water
     * receipt refusal.
     * @note Unnamed const context is the exact attached owner with matching completed/pending
     * time and water ledgers. Read-only, may allocate/throw on checked views; does not flush,
     * settle or repair state. Requires stable exclusive prepared identities; definition
     * specifies supported scope.
     */
    std::string standaloneRk2Boundary(const SimulationContext&) const;
    /**
     * @brief Bind active mesh snow to its finite physical ground heat owner and install UEB
     * contact mask.
     * @note Inactive snow returns. For each snow-owned UEB cell, enables contact only with
     * configured conducting nonempty physical column; publishes borrowed snow pointers on ground
     * owners. Requires initialized matching mesh/heat/snow shapes and exclusive lifecycle
     * access. Allocation or mask refusal may throw after pointer changes; no hydraulic/thermal
     * advance or weather calendar mutation.
     */
    void configureSnowGroundContacts();
    /**
     * @brief Verify tracked native hydraulic completion for the next common-source surface
     * interval.
     * - Argument dt (in): Positive accepted duration s, at most configured routing step.
     * - Argument start (in): Elapsed accepted interval start s.
     * @note Unnamed const context is the exact tracked owner at start. Throws
     * std::invalid_argument for joint identity/finished-step/time/duration mismatch; no check
     * for absent joint/standalone RK2 route. Read-only, no physics/clock mutation; stable serial
     * lifecycle required.
     */
    void checkJointRoutingStep(const SimulationContext&, double dt, double start) const;
    Router* joint_hydraulic_ = nullptr; // externally owned, stable lifetime
    /**
     * @brief Resolve configured/process-cached surface batching duration in seconds.
     * @return Positive OPENSWMM_2D_SYNC_SPAN floored at routing step, else positive configured
     * sync clamped to [routing_step,60 s], else routing step. The process environment is sampled
     * once.
     * @note First unnamed const reference supplies valid positive native routing_step, second
     * supplies prepared coupling policy. Configured clamp requires routing_step<=60 s.
     * Read-only, no input repair, allocation or physics/calendar advance.
     */
    static double couplingSyncSpan(const SimulationContext&, const SolverOptions2D&);
    CommonSourceStage* common_stage_ = nullptr;
    bool checkpoint_owned_initialization_ = false; // scoped explicit native builder
    SimulationContext* source_context_ = nullptr;
    std::string source_router_model_;
    bool source_boundary_ready_ = false;
    /**
     * @brief Validate a completed attached common-source or managed RK2 router checkpoint
     * boundary.
     * - Argument ctx (in): Exact attached context with consistent source/router/native receipts
     * and immutable model.
     * @return Empty on qualification, otherwise unsupported live owner/profile,
     * boundary/quality/receipt, clock/model or provider-use diagnostic.
     * @note Standalone RK2 delegates. First-order requires all-cell matching common stage and
     * source ready flag; rejects uncarried tracking ponds, conduit/catchment/landuse/runtime
     * providers and unsettled outfall reservations. Checks native node-cache ownership, source
     * capacities, CPU profile, gross boundary/external water, completed clocks and
     * surface/source water totals. No repair/flush, serialization or physics advance;
     * allocation/checked lookup may throw under inconsistent layout.
     */
    std::string sourceBoundary(const SimulationContext& ctx) const;
    /**
     * @brief Encode immutable router report/weather, coupling policy and authored boundary
     * identity.
     * - Argument ctx (in): Stable prepared options, gage locations/names and boundary tables.
     * @return Binary model identity, extended by joint coupling structures and RK2 authored
     * boundary fallback when those profiles are used. No mutable state checkpoint by itself.
     * @note Serializes copies; source runtime overrides are cleared only on a private boundary
     * copy. Allocation/archive can throw. Requires matching stable initialized views; no
     * physical/calendar change.
     */
    std::string sourceRouterModel(const SimulationContext& ctx) const;
    /**
     * @brief Drain pending [2D_BOUNDARY_CONDITIONS] / [2D_EDGE_CONVEYANCE] rows into
     * BoundaryData / mesh edge slots and flip pending_rows_drained. Shared by initialize() and
     * prepareForEdit(); idempotent.
     *
     * @details Materialize authored boundary/conveyance rows into prepared live mesh edge slots.
     * @note No mesh returns. Resizes/defaults boundaries then applies valid authored rows,
     * skipping out-of-range boundary cell/local-edge entries. Builds canonical undirected edge
     * lookup and mirrors conveyance factors on all matching slots; unknown/off-mesh conveyance
     * vertices throw. Retains all pending rows for faithful input serialization and marks
     * drained only after success. Geometry must already be sized; units remain authored at this
     * stage, with initialization conversion separate. Allocation/validation may leave partial
     * arrays. Exclusive authoring/setup access; no physics advance.
     */
    void drainPendingRows();

    MeshData mesh_;
    SurfaceStateData state_;
    ChannelGroundState channel_ground_;
    SolverOptions2D options_;
    BoundaryData boundary_;
    std::vector<std::string> thread_warnings_; ///< See threadWarnings().
    std::string backend_name_;                 ///< See backendName().

    /// §5.5 track I — per-cell infiltration parameters, kernel state and the
    /// held rates published into state_.infil_rate. Inert (no allocation, no
    /// per-step work) until a [2D_INFILTRATION*] section resolves a model.
    Infil2D infil_;

    /// Sim time since the last Infil2D::updateRates call (D-I1 INFIL_STEP
    /// cadence). Advanced on the routing/co-advance cadence, NEVER per
    /// marcher substep.
    double infil_elapsed_ = 0.0;

    /// Per-cell APPLIED cumulative infiltrated depth (m) — see
    /// infilCumulative(). Filled in accumulateMassBalance() by draining the
    /// same SurfaceStateData::infil_applied accumulator that feeds
    /// MassBalance2D::infil_out; empty when no [2D_INFILTRATION*] model
    /// resolved.
    std::vector<double> infil_cum_applied_;

    /// C1: SIGNED cumulative coupling exchange volume per cell (m³).
    /// + = received from the 1D node, − = abstracted by it. Drained from
    /// SurfaceStateData::coupling_applied on the same once-per-routing-step
    /// pass as infil_cum_applied_. Always sized once the mesh resolves.
    std::vector<double> coupling_cum_applied_;

    /// U3 (track I-b): cell → containing subcatchment (-1 = none), resolved
    /// once at initialize() from the cell centroid and the [Polygons]; and
    /// the per-subcatchment recharge volume (m³) awaiting delivery.
    std::vector<int> cell_subcatch_;
    std::vector<double> subcatch_recharge_;

    /// U4: subsurface-transport authoring rows (inert until the kernel).
    GwTransportData gw_;
    SurfaceQuality2D surface_quality_; ///< S7

    /// G1: the `[2D_AQUIFER*]` authoring rows (project units, never
    /// converted in place) and the two-zone kernel that reads them into SI
    /// state. Both are inert — no allocation, no per-step work — until a
    /// `[2D_AQUIFER]` row resolves.
    SubsurfaceConfig aquifer_cfg_;
    std::vector<std::string> aquifer_node_names_;
    std::vector<std::string> aquifer_link_names_; ///< G-X4
    SubsurfaceSolver subsurface_;
    /// G-X3: the (conduit, cell, length share) table `LINK_SEEPAGE AUTO`
    /// resolved at initialize; empty when nothing seeps over the mesh.
    std::vector<GwLinkShare> aquifer_link_shares_;
    /// G-X4: the conduits in `aquifer_link_shares_`, each once, ascending —
    /// the publisher's loop, so it does not re-scan the share table.
    std::vector<int> aquifer_link_ids_;

    /// Per-cell cumulative rainfall volume (m³) — see rainCumulative().
    /// Filled in accumulateMassBalance() alongside MassBalance2D::rainfall_in.
    std::vector<double> rain_cum_;

    /// V-E3 — parse-time scratch for [2D_BOUNDARY_CONDITIONS] rows.
    std::vector<PendingBoundaryRow> pending_bc_rows_;
    std::vector<PendingInitialQualityRow> pending_iq_rows_; ///< S1/S2
    std::vector<PendingInitialBedQualityRow> pending_ibq_rows_;
    std::vector<PendingBoundaryQualityRow> pending_bq_rows_; ///< S2

    /// §11A — parse-time scratch for [2D_EDGE_CONVEYANCE] rows.
    /// Drained in initialize() into mesh_.edge_conveyance after
    /// buildMeshTopology populates the neighbour table.
    std::vector<PendingEdgeConveyanceRow> pending_edge_conveyance_rows_;

    std::vector<CouplingPoint> coupling_points_;
    OutfallExchange outfall_exchange_;
    bool outfall_state_changed_ = false;
    /// Non-outfall coupling points only — the live in-marcher exchange list.
    /// Stable storage that state_.node_coupling points at; built once in
    /// initialize().
    std::vector<CouplingPoint> node_coupling_points_;
    /// COUPLING_HEAD VERTEX / STENCIL_WET_MEAN sampling cells of the live
    /// points, each point's run at [head_begin, head_end).
    std::vector<int> coupling_head_cells_;
    /// COUPLING_POND_MODE TRACKING: per node, the cells (CSR over nodes) whose
    /// wet area its pond tracks, with each cell's median-dual share (m²), and
    /// the chamber area floor (1D ft²).
    std::vector<int> pond_track_ptr_;
    std::vector<int> pond_track_cell_;
    std::vector<double> pond_track_share_;
    std::vector<double> pond_chamber_ft2_;
    /// COUPLING_EQUALIZE: each coupled node's 1D surface area (m²) for the
    /// coming advance, published on the state.
    std::vector<double> node_pond_area_m2_;
    /**
     * @brief Relax tracking-mode coupled pond area toward wet footprint while preserving
     * above-rim volume.
     * - Argument ctx (in,out): Matching node depths/heads and per-node pond areas/bands in
     * native ft units.
     * - Argument dt (in): Positive routing duration s; nonpositive duration or no tracking CSR
     * returns.
     * @note Uses tau=positive configured relaxation or 5*dt, alpha=min(1,dt/tau), chamber-area
     * floor and cells at/above dry depth. Converts wet m2 shares to ft2, updates depth/head for
     * existing above-rim water, then installs area. No water exchange/clock change or general
     * finite/shape validation occurs. Requires prepared indices/arrays and exclusive
     * hydraulic/mesh state.
     */
    void updateTrackingPonds(SimulationContext& ctx, double dt);
    /**
     * @brief Publish per-node pond surface area and held coupling window for equalization caps.
     * - Argument ctx (in): Prepared native node depth/pond/storage configuration and
     * display-unit system.
     * - Argument window (in): Positive held exchange window s supplied by owning routing
     * advance.
     * @note Clears borrowed state area pointer first. With equalizing points, rebuilds per-node
     * m2 areas from current ft2 pond areas floored at native minimum surface area, then borrows
     * that member vector on surface state. No water/clock advance. Allocation may throw after
     * pointer clear; prepared point indices/layouts and exclusive state required. Later vector
     * rebuild/move invalidates views.
     */
    void publishEqualizeAreas(const SimulationContext& ctx, double window);
    /// S4: published 1D node row values `[node * n_species + s]` (pollutant
    /// conc, MSX conc, age, temperature), refreshed once per batch by
    /// publishNodeRows(); state_.node_row_conc points here.
    std::vector<double> node_row_conc_;
    /**
     * @brief Refresh the node-major native row cache borrowed by mesh exchange kernels.
     * - Argument ctx (in): Matching prepared native pollutant/MSX concentrations, age s and
     * temperature degC.
     * @note Returns for inactive transport or cache extent != n_species*n_nodes. Copies
     * available native values into [node*n_species+row], zero-filling missing source entries; no
     * conversion, physical inventory/clock mutation or destination allocation. Requires
     * canonical row ordering and exclusive cache access; does not validate concentration
     * admissibility.
     */
    void publishNodeRows(const SimulationContext& ctx);
    /**
     * @brief S4: aging of the age row and the per-cell reaction stage (pollutant kdecay + MSX
     * via reactArdStage), Lie-split after each advance.
     *
     * @details Apply post-advance native age and reactions, or report already accepted coupled
     * RK2 loss.
     * - Argument ctx (in,out): Matching prepared pollutant/reaction laws, scoped workspace and
     * native ledgers.
     * - Argument dt (in): Positive finite accepted duration s; inactive
     * transport/nonpositive/NaN returns.
     * @note Coupled global RK2 only synchronizes unreported reaction receipts because physics
     * was split inside RK2. Other modes add dt*volume to surface age inventory and age finite
     * local bed, then run native exact pollutant decay/BULK PIPE kinetics on cells with
     * volume>dry_depth*area. Converts volume to ft3 for native balance while concentrations
     * retain native values; age/temp rows do not react. No water/engine clock change. Allocates
     * scratch and may throw after age/earlier ledger changes; requires exclusive native
     * workspace/context and exactly-once accepted-step ownership.
     */
    void applyAgingAndReactions(SimulationContext& ctx, double dt);
    std::vector<double> react_phi_, react_a_, react_dx_; ///< scratch

    /// Sim time since the last statistics/continuity sample (30 s cadence).
    double co_refresh_elapsed_ = 0.0;
    /// Sim time since the last RENDER-field refresh (vertex heads, gradients,
    /// render depths). Those fields have exactly two consumers — the output
    /// snapshot and the bulk API getters — so they are refreshed on the
    /// REPORT_STEP grid, and on demand for an API reader that arrives between
    /// reports (render_dirty_).
    double co_render_elapsed_ = 0.0;
    bool render_dirty_ = false;
    /// Per-cell free-surface scratch for the render-depth reconstruction,
    /// held across refreshes so the pass allocates nothing.
    std::vector<double> render_eta_;
    /// Flat mesh edge slots (3*i+e) carrying a non-WALL boundary condition.
    /// The BC TYPE of a slot is fixed once the model is resolved, so the list
    /// is built once in initialize(); the per-step boundary ledger and the
    /// per-step TS/rating resolve walk it instead of all 3*n_triangles slots.
    std::vector<int> bc_nonwall_slots_;
    /// Set whenever a slot's BC TYPE changes (parse-time drain, API setter);
    /// bc_nonwall_slots_ is rebuilt on the next resolve.
    bool bc_nonwall_dirty_ = true;
    /// Per-node dedupe for the coupling term of the per-batch mass balance;
    /// a member so the batch does not build and destroy a hash set each time.
    std::unordered_set<int> mb_seen_nodes_;
    /// Sim time since the last rainfall/forcing refresh (gage-scale cadence).
    double co_forcing_elapsed_ = 0.0;
    bool co_forcing_first_ = true;
    /**
     * @brief One windowless routing-step co-advance: outfall inject + rainfall + BC resolve +
     * solver advance over exactly [t, t+dt] + ledger booking + mass balance + output refresh.
     * Replaces the whole window state machine on the marcher path.
     *
     * @details Execute one pending surface batch and consume its water/native exchange receipts
     * once.
     * - Argument ctx (in,out): Matching live engine context, prepared native
     * node/GW/heat/quality and report state.
     * - Argument dt (in): Positive finite accepted batch span s; nonpositive duration returns.
     * - Argument t (in): Elapsed routing sample/start time s used for held infiltration factors
     * and boundary lookups; numerical interval itself is [sim_time_,sim_time_+dt].
     * @note Requires active initialized solver, prepared source ownership, stable borrowed views
     * and exclusive router/context. Captures report/continuity beginnings when due; integrates
     * piecewise constant gage history into exact batch means, applies held/one-shot forcings,
     * snow and infiltration cadence, boundary lookup, thermal preparation and equalization area,
     * then advances solver to target. Drains plugin gross boundary packets into engine receipts,
     * refreshes ground/heat views and checks thermal failures before incrementing completed
     * surface time. Applies native aging/reaction once.
     * @details Converts accepted node water/native inventories from m3 to ft3 convention, pairs
     * opposite gross spill/drain native transfers, books surface ledgers before adding
     * groundwater node delivery, then hands delivery to queues, consumes accepted
     * infiltration/coupling/rain receipts and clears one-shot forcings. Updates
     * continuity/statistic and render fields at separate bounded cadences. Common-source
     * boundary/history match is requalified last. No engine calendar increment or 1D solve. This
     * large operation can throw after partial physical, clock, queue or report changes; it has
     * no router-wide rollback/replay contract. Call exactly once per pending interval and
     * serialize access.
     */
    void coAdvanceStep(SimulationContext& ctx, double dt, double t);
    /**
     * @brief G-X4: freeze the aquifer's side of the conduit exchange (table elevation relative
     * to the conduit invert, conductivity, path length and the gain budget) into the conduit
     * rows the two loss laws read.
     *
     * @details Freeze aquifer head, conductivity/path and finite gain budgets for the next
     * conduit solve.
     * - Argument ctx (in,out): Matching conduit subtype hydraulic coupling arrays, native node
     * inverts/offsets, seepage coefficients and routing-step configuration.
     * @note Inactive/no share/no coupling storage returns. Length-weights SI aquifer table head,
     * converts to native ft relative to conduit mid-invert, chooses authored/legacy conductivity
     * and path, and limits per-barrel native ft3/s gain by half-cell drainable water, pending
     * withdrawals, reservations, shared consumers and optional LID donor budgets. Does not
     * itself withdraw water/native mass or advance a clock. Throws on delegated budget refusal
     * after possible earlier conduit updates; requires prepared share indices/positive scales
     * and exclusive routing/GW model.
     */
    void publishLinkCoupling(SimulationContext& ctx);
    /**
     * @brief T7.4: one species' concentration in a conduit, in 1D units — what a leaking pipe's
     * water carries into the aquifer.
     *
     * @details Read a conduit's native concentration for one canonical aquifer transport row.
     * - Argument ctx (in): Matching native conduit pollutant/MSX concentration, age s and
     * temperature degC views.
     * - Argument link (in): Valid nonnegative zero-based link index in the resolved prepared
     * model.
     * - Argument s (in): Valid nonnegative row below tr.n_species.
     * - Argument tr (in): Stable groundwater canonical row names/kinds matching native species
     * identities.
     * @return Available pollutant/MSX concentration, age s or temperature degC; zero when
     * corresponding published native vector entry/kind is absent. No conversion, allocation or
     * mutation occurs.
     * @note Requires valid row/name indices; this helper does not fully bounds-check model
     * metadata.
     */
    double linkRowConc(const SimulationContext& ctx, int link, int s,
                       const SubsurfaceTransportState& tr) const;
    /**
     * @brief T7.4: hand the aquifer's node-seam mass to the 1D in both directions — out through
     * the node's coupling queues, in as the exfiltration loss the 1D quality balance never
     * booked.
     *
     * @details Deliver pending aquifer-to-node native inventories and book reverse pollutant/MSX
     * seepage loss.
     * - Argument ctx (in,out): Matching node native queues/concentrations, reaction mapping and
     * quality ledgers.
     * @note Active native aquifer with nonempty node_out_mass required; otherwise returns.
     * Converts concentration*m3 to concentration*ft3 (age/temperature likewise), appends by kind
     * to node queues, and books node-to-ground recharge native loss using accepted node water
     * receipts. Clears the full node_out_mass vector after delivery; does not clear node water,
     * which the caller drains separately. Missing destination extents are skipped, not grown
     * except MSX queue. Requires matching positive species/node strides and stable prepared IDs;
     * allocation may throw after partial delivery. Exactly once consumption and exclusive
     * context/GW access required; no hydraulic or calendar advance.
     */
    void flushAquiferNodeSpecies(SimulationContext& ctx);
    /**
     * @brief True when the batch about to run (span @p dt) is the one that closes the current
     * output-refresh window — decided BEFORE the advance so the continuity snapshot is only
     * taken on batches that will use it.
     *
     * @details Test whether the next batch reaches the bounded diagnostic sampling cadence.
     * - Argument ctx (in): Unused matching context, retained for the router interface.
     * - Argument dt (in): Positive finite next batch duration s.
     * @return co_refresh_elapsed_+dt >= max(dt,30 s). Read-only, no reset or finite-value
     * repair.
     * @note Reports peak/continuity/velocity sampling due; it does not advance or refresh any
     * field.
     */
    bool refreshDue(const SimulationContext& ctx, double dt) const;
    /**
     * @brief Recompute the render fields from the current state.
     *
     * @details Rebuild derived vertex heads, optional output gradients and render free-surface
     * depths.
     * @note Requires prepared consistent mesh/state/options and exclusive access. Resets render
     * elapsed and dirty markers, then updates attached CPU source-history boundary matching
     * metadata. Does not advance canonical water/momentum/native inventories or engine calendar.
     * Allocation/delegated reconstruction may throw after some derived fields changed; retained
     * render views may be invalidated.
     */
    void refreshRenderFields();

    bool active_ = false;
    double sim_time_ = 0.0;
    /// Routing time accumulated since the last co-advance sync batch.
    double pending_dt_ = 0.0;

    /// Report-time blend support (the 2D analogue of the 1D old/new report
    /// interpolation): cell depth/head captured at the START of the sync
    /// batch that crosses the next report instant, plus that batch's ms
    /// window [old, new] kept on the same `+= 1000*dt` recurrence as
    /// ctx.elapsed_ms. fillSurfaceSnapshot blends old→current with
    /// reportWeight() so the snapshot's depth/head are truthful at the
    /// report datestamp instead of lagging at the window end.
    std::vector<double> report_old_depth_;
    std::vector<double> report_old_head_;
    double report_window_old_ms_ = 0.0;
    double report_window_new_ms_ = 0.0;
    bool report_old_valid_ = false;

    /// OPENSWMM_2D_HEAD_RAMP experiment: per-coupling-point 1D head at the
    /// previous batch (2D metre frame) + that batch's span, for the
    /// batch-over-batch trend slope handed to the marcher.
    std::vector<double> ramp_prev_head_;
    double ramp_prev_span_ = 0.0;

    /// Previous cumulative boundary flux (Σ edge_bc_cum_flux, m³), for the
    /// per-step delta in the global mass balance.
    double prev_boundary_cum_ = 0.0;
    double prev_boundary_in_ = 0.0, prev_boundary_out_ = 0.0;

    /// Per-coupling-point outfall exchange volume (m³, + = 1D discharge onto
    /// the surface, − = withdrawal) accumulated per sync batch. Indexed like
    /// coupling_points_ (non-outfall slots stay 0).
    std::vector<double> window_outfall_accum_;
    /**
     * @brief Zero the consumed outfall ledger after each surface sync batch.
     *
     * @details Zero the batch outfall accumulator and reset its exchange window counters.
     * @note Does not clear canonical cumulative receipts, native queues, water or clocks.
     * Requires exclusive exchange owner; no allocation or independent chronology validation.
     */
    void resetWindowAccumulators();

    /// Simulation time of the most recent routing step (for the finalize flush).
    double last_t_ = 0.0;

    /// One-shot guard: resolve deferred boundary timeseries/curve NAMES to
    /// registry indices on the first advance (ctx.tables is populated by
    /// then), not at parse time.
    bool boundary_names_resolved_ = false;

    /// Mesh-file-units → SI factor for CONSTANT SPECIFIED_STAGE heads
    /// (0.3048 for US FLOW_UNITS with a non-SI mesh file, else 1.0). Constant
    /// heads live in the mesh sections and share the mesh's vertical datum
    /// and units, so they follow the mesh scaling exactly (including the
    /// `;; UNITS: SI (m)` header). Applied once after drainPendingRows().
    double bc_stage_scale_ = 1.0;

    /// Project-display-units → SI factor for TIME-SERIES / rating-curve
    /// stage values (0.3048 for US FLOW_UNITS, else 1.0). [TIMESERIES] and
    /// [CURVES] tables are 1D project data authored in display units
    /// regardless of how the mesh file is tagged, so this is driven by
    /// FLOW_UNITS alone (like bc_flow_scale_) — NOT by the mesh header.
    /// Applied at every lookup in resolveBoundaryValues(); also converts the
    /// SI head back to display units for rating-curve stage-axis queries.
    double bc_stage_ts_scale_ = 1.0;

    /// Display flow units → m³/s factor for SPECIFIED_FLOW / TS_FLOW /
    /// RATING_CURVE per-metre discharges (from FLOW_UNITS; 1.0 for CMS).
    /// Applied once to constant flows after drainPendingRows() and at every
    /// TS / rating-curve lookup.
    double bc_flow_scale_ = 1.0;
#ifdef OPENSWMM_HAS_2D
    /// Time integrator, chosen at runtime: the serial ExplicitInertialSolver,
    /// or the Kokkos marcher plugin when installed/eligible (constructed in
    /// initialize() via makeSurfaceSolver).
    std::unique_ptr<ISurfaceSolver> solver_;
#endif

    /**
     * @brief Update rainfall from the rain gages (natural-neighbour interpolation or the uniform
     * SYSTEM mean, per options_.rainfall_mode).
     *
     * @details Publish the current batch gage-mean rainfall field before external forcing
     * composition.
     * - Argument ctx (in,out): Matching gage rainfall/display-unit configuration; logically read
     * by this helper.
     * @note Uses integrated window rates only when their extent matches gage count, otherwise
     * current native gage rates. Copies n_gages values into cached last sample and delegates
     * cell mapping. Requires source extent>=n_gages; allocation can leave cache updated before
     * field mapping. No gage clock or water advance and no per-cell forcing composition here;
     * exclusive field access required.
     */
    void updateRainfall(SimulationContext& ctx);

    /**
     * @brief Map per-gage rates (user units, in/hr or mm/hr) onto the cells (m/s) per
     * options_.rainfall_mode.
     *
     * @details Map native display-unit gage samples linearly into SI cell rainfall samples.
     * - Argument ctx (in): Prepared gage count and flow-unit system matching interpolation
     * weights.
     * - Argument rates (in): At least n_gages input samples, ordinarily in/hr (US) or mm/hr
     * (SI). Integrated samples retain the same extra time factor, making this linear helper
     * useful for cumulative depth.
     * - Argument out (out): Resized n_cells, filled zero for no gages/NONE; otherwise spatial
     * interpolation or uniform mean converted by native rainfall-to-m/s factor. Existing
     * contents replaced.
     * @note Natural/nearest uses ready precomputed weights; unavailable spatial interpolation
     * falls back to mean of all gages. No finite/admissibility checks, forcing application,
     * water/clock mutation. Allocation may throw after resizing; input/output must not alias
     * because output is resized first.
     */
    void gageRatesToCells(const SimulationContext& ctx, const std::vector<double>& rates,
                          std::vector<double>& out) const;

    /// Static per-cell rainfall-interpolation weights. Built once in
    /// initialize() (gage positions are fixed for a run); applied each step in
    /// updateRainfall() for RainfallMode::NATURAL_NEIGHBOUR.
    RainfallInterpolator interp_;

    /// Gage rainfall (user units) the current cell field was built from. A
    /// change forces a refresh ahead of the 30 s cadence, so the field never
    /// lags a gage record by more than the step that crosses it.
    std::vector<double> rain_gage_last_;

    /// Gage rates booked by bookGageRates (user units), the time they took
    /// effect (s), the depth each gage delivered before then (rate x s), the
    /// same depth at the end of the last 2D window, and that window's mean
    /// rate — what updateRainfall maps onto the cells.
    std::vector<double> gage_rate_, gage_cum_, gage_window_depth_, gage_window_rate_;
    double gage_since_ = 0.0;
    // Preserve the integral at the next report boundary before the runoff
    // clock moves beyond it. Only one gage row and one cell row are retained.
    std::vector<double> gage_report_depth_, report_rain_cum_;
    double report_rain_ms_ = -1.0;

    /**
     * @brief Resolve per-step boundary driving values: evaluate SPECIFIED_STAGE / SPECIFIED_FLOW
     * timeseries at time @p t and RATING_CURVE from the boundary cell stage into edge_bc_head /
     * edge_bc_flow (which the flux kernels read). Resolves deferred timeseries/curve names to
     * registry indices once. No-op for WALL / NORMAL_FLOW and for constant SPECIFIED_* edges.
     *
     * @details Resolve compact non-wall boundary descriptors at a held elapsed routing sample.
     * - Argument ctx (in,out): Matching prepared time-series/curve table store; cursor lookup
     * can advance its mutable lookup cursor without changing the underlying series values or
     * engine clock.
     * - Argument t (in): Finite elapsed sample s, converted from start_date to absolute DateTime
     * days.
     * @note Rebuilds non-wall slot cache when dirty and resolves deferred names once (-2 to
     * table index, missing becomes -1). Coupled groundwater RK2 returns after name/cache work
     * because its marcher owns Euler-time immutable signal lookup. Otherwise updates stage m or
     * flow m3/s/m from table/display scales; rating curves sample lagged surface head in display
     * elevation units. Constants remain held, missing/out-of-range tables retain prior values.
     * No accepted water transfer. Requires consistent boundary/model shapes and exclusive
     * caches/table cursors; allocation/lookup can partially update.
     */
    void resolveBoundaryValues(SimulationContext& ctx, double t);

    /**
     * @brief Accumulate the global 2D mass-balance terms for one executed step into
     * ctx.mass_balance_2d (rainfall, coupling, outfall, boundary, infiltration, latest storage)
     * and the evaporation loss into state_.evap_loss_total. All terms in the 2D solver's SI
     * internal units (m³).
     *
     * @details Consume one completed surface batch's accepted receipts into SI water/report
     * ledgers.
     * - Argument ctx (in,out): Matching mass_balance_2d and current native coupling_volume
     * before queue handoff.
     * - Argument dt (in): Positive finite accepted batch duration s; exactly once per batch.
     * @note Ascending cell pass updates gross/raw weather rain cumulative, uses snow liquid or
     * accepted RK2 rain receipts for physical rainfall, drains applied infiltration depth m and
     * signed coupling m3, updates subcatchment recharge pending m3 and final surface storage.
     * Reads cumulative evaporation, managed/GW return and runtime source totals; adds only new
     * gross boundary receipts using prior cumulative markers and deduplicates native node
     * exchange water (ft3 converted to m3). Outfalls were already booked at transfer, not
     * replayed. No physics or clock advance, allocation only in node dedupe. Requires prepared
     * extents and exclusive once-only receipt ownership; no rollback/finite validation. Failure
     * after a consumed prefix is not safe to retry as a fresh batch.
     */
    void accumulateMassBalance(SimulationContext& ctx, double dt);
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_SURFACE_ROUTER_HPP
