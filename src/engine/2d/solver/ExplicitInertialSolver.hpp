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
 * @file      ExplicitInertialSolver.hpp
 * @brief     Explicit local-inertial FV time-marcher for the 2D surface.
 *
 * @details Phase 1 of the 2026-07-29 2D reimplementation plan. A CFL-driven
 *          explicit marcher over the unique-face layout (InertialEdges):
 *          cell volume V is the conserved state (SurfaceStateData::volume,
 *          in place), one prognostic unit-width discharge q per interior
 *          face, semi-implicit friction, positivity limiter (V ≥ 0 always —
 *          no negative-volume debt machinery), and a flux-active cell set
 *          with lazy source-only integration for rain-on-grid thin films.
 *
 *          advance(t0, t1) ALWAYS reaches t1 (the step size is known a
 *          priori from the CFL bound), so none of the router's failed/frozen/
 *          partial-window machinery ever engages on this path. No linear
 *          solver, no Jacobian, no preconditioner.
 *
 *          Tiered local timestepping (LTS_TIERS > 1) lands in Phase 2; this
 *          phase steps the whole active set at the global CFL dt.
 *
 * @ingroup engine_2d
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
#define OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP

#include <array>
#include <functional>
#include <cstdint>
#include <string>
#include <vector>

#include "ISurfaceSolver.hpp"
#include "InertialEdges.hpp"
#include "MomentumPhysics2D.hpp"
#include "../gw/GwSourceForcing.hpp"
#include "../../transport/components/ReactionModule/ReactionIntegrator.hpp"
#include "../data/SolverOptions2D.hpp" // Momentum2D

namespace openswmm::detail {
class CompletedSourceArchive;
}
namespace openswmm {
class SWMMEngine;
struct SimulationContext;
struct Table;
}
namespace openswmm::runoff {
class SourceWaterDriver;
}
namespace openswmm::twoD {
struct Rk2CheckpointBinding;

class SubsurfaceSolver; // G1: the two-zone groundwater kernel, if authored
class CommonSourceStage;
class ChannelGroundState;
class SourceResultHistory;
class EngineSourceResultHistory;

class ExplicitInertialSolver final : public ISurfaceSolver {
public:
    /// Publish FULL_SWE velocity from accepted conserved momentum,
    /// using the internal negative-outflow display convention. No flux change.
    void publishMomentumVelocity();

    /**
     * @brief Prepare the CPU finite-volume marcher from authoritative surface volume and
     * resolved model.
     * @param[in,out] mesh Prepared SI triangle/quad mesh, complete
     * topology/stencils/material/initial- velocity arrays. Borrowed for the marcher lifetime;
     * geometry identity remains stable while running.
     * @param[in,out] state Matching canonical water/native/thermal owner, with live optional
     * boundary, node/coupling and groundwater bindings. Borrowed; seeded volume is authoritative
     * and head/depth are reconstructed. Allocates species boundary composition and ensures
     * boundary receipt rows.
     * @param[in,out] opts Resolved controls, borrowed. FULL_SWE reduces CFL>0.5 and order-two
     * LTS tiers>1 with stderr advisories; unsupported standalone second-order overland transport
     * uses first order.
     * @throws std::runtime_error Authored boundary quality targets a WALL/nonboundary edge or
     * invalid row.
     * @throws std::invalid_argument Surface/groundwater native row names are incomplete.
     * @note Resets source/channel/recorder/reaction/RK2-boundary bindings, builds unique edges,
     * side water/ native/momentum buffers, padded worker ledgers, active/tier/pin and boundary
     * lists, seeds momentum from initial SI velocity, resets clocks/counters/telemetry, then
     * marks initialized. Empty mesh returns without activation after some binding resets.
     * Requires consistent positive geometry/count products, finite controls and exclusive owner
     * access. Allocation or delegated failure leaves partial state; not an atomic reset and no
     * whole-engine initialization/time advance.
     */
    void initialize(MeshData& mesh, SurfaceStateData& state, SolverOptions2D& opts) override;
    /**
     * @brief Advance one accepted CPU surface/groundwater window to its target horizon.
     * @param[in] t_current Finite elapsed start s, consistent with current canonical owners;
     * common-source and coupled-RK2 modes require exact equality with their accepted completed
     * clocks.
     * @param[in] t_target Finite target s at least t_current, caller scheduled at applicable
     * forcing cuts.
     * @return t_target after success, or directly for ordinary uninitialized/nonforward calls.
     * Coupled RK2/common-source qualification may reject those invalid clock/profile states
     * before that no-op.
     * @throws std::invalid_argument Invalid completed-clock/common-stage input.
     * @throws std::runtime_error Unsupported coupled RK2 profile or delegated
     * physics/source/recording failure.
     * @note Invokes advanceImpl(): tiered finite-volume cycles/global equal tails, pending
     * settlement, lazy-source landing, paired groundwater/node work and final edge publication.
     * Coupled groundwater RK2 snapshots marcher/surface/ground and selected boundary
     * receipts/forcing arrays, restoring them on exceptions; arbitrary external
     * callback/recorder effects are not included in that snapshot. Other modes can retain
     * partial progress after failure. Requires initialized matching stable owners, serial outer
     * advancement and provisioned internal worker scratch; allocation exceptions propagate. Does
     * not update canonical 1D router state or engine calendar by itself.
     */
    double advance(double t_current, double t_target) override;
    /**
     * @brief Read the finest accepted source synchronization horizon.
     * @return t_last_sync_ in elapsed s while initialized, otherwise -1. Does not flush pending
     * lazy sources or advance a provider; stable owner access is required.
     */
    double completedSourceTime() const noexcept override {
        return initialized_ ? t_last_sync_ : -1.0;
    }
    /**
     * @brief Reconstruct externally edited volume after discarding stale surface momentum/side
     * packets.
     * @param[in] t0 Retained interface time, ignored by this implementation.
     * @throws std::runtime_error A common source stage is attached; generic rollback/edit is
     * unqualified.
     * @note Otherwise inactive returns. Zeros face/boundary and side water/SWE momentum packets,
     * zeroes prognostic SWE cell momentum, clears the pending flag and reconstructs head/depth.
     * Does not clear native side packets/producer ledgers, reset source clocks or force a tier
     * rebuild itself; caller coordinates the qualified external edit/restart protocol. Requires
     * exclusive matching prepared state.
     */
    void reinitialize(double t0) override;
    /**
     * @brief Reconstruct head/depth after authoritative external volume transfers without losing
     * momentum.
     * @param[in] t0 Retained interface time, ignored.
     * @throws std::runtime_error A common source stage is attached, because arbitrary external
     * edits are not qualified with its receipt clocks.
     * @note Inactive returns. Preserves face/cell momentum and pending side packets,
     * reconstructs current volume, then forces next-entry active/tier rebuild so newly wet cells
     * participate. Does not settle receipts, reset clocks or validate transfer mass balance;
     * caller owns paired canonical edits.
     */
    void resyncFromVolumes(double t0) override;
    /**
     * @brief External outfall transfers keep these cells and their halo on tier zero.
     *
     * @param[in] cells Valid prepared zero-based surface cells; duplicates are harmless.
     * @throws std::out_of_range Invalid cell through pin_t0_.at(); earlier valid pins remain
     * set.
     * @note Marks each cell pinned then forces next-entry tier/active rebuild. Halo activation
     * occurs during that rebuild, not here. Does not book water, clear pins, reconstruct state
     * or advance clocks; caller serializes configuration with all firings.
     */
    void pinExternalSourceCells(const std::vector<int>& cells);
    /**
     * @brief Internal qualification adapter; owned externally and attached at time zero.
     *
     * @param[in,out] stage Borrowed prepared stage matching this exact mesh/state/groundwater
     * identity. Must outlive marcher use, begin at completed time zero and satisfy qualified
     * heat ownership.
     * @throws std::invalid_argument Existing attachment, uninitialized/noninitial/second-order
     * state, mismatched owner identity or unsupported catchment/LID heat profile.
     * @note Stores pointer and pins managed cells; pin allocation/index failure can follow
     * attachment. Does not activate authored ownership or initialize/advance the external stage.
     * Exclusive setup only.
     */
    void setCommonSourceStage(CommonSourceStage& stage);
    /**
     * @brief Attach a borrowed active channel/soil finite thermal owner before first advance.
     * @note The stage reference and its external canonical dependencies must remain
     * alive/stable. Rejects uninitialized/nonzero-time/common-source modes, ignores inactive
     * stage, allocates shared source scratch and pins owned cells. Definition documents partial
     * attachment on allocation/index failure.
     */
    void setChannelGround(ChannelGroundState&);
    /**
     * @brief Capture initial state and every finest accepted source interval, after all due
     * groundwater firings/node sampling; externally owned recorder.
     *
     * @details Borrow and seed an empty source-history recorder at the initial common-stage
     * boundary.
     * @param[in,out] history External empty recorder, kept alive/stable through subsequent
     * captures. Initial append may allocate and mutates history on success; ownership remains
     * with caller.
     * @throws std::invalid_argument No common stage, any recorder already attached, noninitial
     * time, nonempty tables or recorder append refusal.
     * @note Attaches pointer only after successful initial append. Does not capture another
     * interval, advance physical owners, write files or implicitly enable result output.
     * Exclusive setup required.
     */
    void setSourceResultHistory(SourceResultHistory& history);
    /**
     * @brief Internal accepted CPU source boundary or standalone water-only RK2 component
     * checkpoint; not a whole-engine hotstart. Borrowed owners are guarded.
     *
     * - Argument path (in): Writable component checkpoint destination; exclusive writer
     * required.
     * @return Empty on success, otherwise qualification/serialization/publication diagnostic.
     * @note Requires an accepted qualified component boundary. Live joint coupling uses the
     * router checkpoint. No physics/calendar advance; definition details publication and
     * temporary-file failure.
     */
    std::string saveCheckpoint(const std::string& path) const;
    /**
     * @brief Restore a component checkpoint into fresh initialized matching exclusive owners.
     * - Argument path (in): Readable bounded checkpoint path.
     * @return Empty on success, otherwise freshness/model/decode/file diagnostic.
     * @note Uses private candidates and preserves borrowed owner identities. Full restore scope,
     * validation and callback side effects are documented on the byte overload; no full-engine
     * restore.
     */
    std::string loadCheckpoint(const std::string& path);
    /**
     * @brief Bind a saved exact source-history prefix after a marcher restore.
     *
     * @param[in,out] history Borrowed saved recorder prefix, whose exact accepted checkpoint
     * state must match the restored common source stage. Must outlive subsequent capture/refresh
     * use.
     * @throws std::invalid_argument Invalid checkpoint boundary, another attached recorder or
     * history resume mismatch. Append/identity allocation errors can also propagate.
     * @note Stores pointer only after successful resume. Does not replay physical source
     * intervals, replace canonical solver state or load a file. Call exclusively after qualified
     * marcher restore.
     */
    void resumeSourceResultHistory(SourceResultHistory& history);
    /**
     * @brief Configure the standalone coupled RK2 surface reaction owner at time zero.
     *
     * @details Configure initial qualified surface decay and compiled PIPE reaction ownership.
     * @return Empty on success, otherwise profile/layout/dependency diagnostic. Context
     * reference supplies matching prepared identities; active compiled ReactionData remains
     * borrowed and must stay stable/alive. No reaction is executed. Allocation may follow
     * partial binding; definition details supported variables, row units, staged validation and
     * counter initialization. Exclusive initial-time setup required.
     */
    std::string configureRk2Reactions(const SimulationContext&);
    /**
     * @brief Resolve owned cursor-free surface boundary forcing for the fresh coupled RK2 owner.
     * - Argument stage_scale (in): Finite positive input-stage to SI m factor.
     * - Argument flow_scale (in): Finite positive input flow to prepared boundary-flow units
     * factor.
     * @return Empty on acceptance, otherwise diagnostic. Context reference is borrowed during
     * resolution; time-series/rating payloads are copied. Definition details profile, staging
     * and initial evaluation. No physical advance, table cursor mutation or automatic runtime
     * override activation occurs.
     */
    std::string configureRk2BoundarySignals(SimulationContext&, double stage_scale,
                                            double flow_scale);
    /**
     * @brief Evaluate owned project boundary signals at the current coupled stage time/state.
     * @param[in] time Finite elapsed time s for immutable signal interpolation; rating curves
     * instead query current canonical volume-derived SI head divided by their authored
     * coordinate scale.
     * @throws std::runtime_error Changed table identity or nonfinite converted query/value.
     * Guarded slot/volume indexing can throw std::out_of_range.
     * @note Skips slots held by runtime_original overrides; writes only boundary head/flow
     * arrays. No signal cursor/calendar, boundary receipt or canonical water change. Evaluation
     * is sequential: later failure can follow earlier slot updates. Requires configured live
     * boundary/mesh/state and exclusive stage access; enclosing RK2 transaction saves/restores
     * relevant arrays on failure.
     */
    void resolveRk2BoundarySignals(double time);
    /**
     * @brief Read one row's cumulative qualified RK2 native reaction loss.
     * @param[in] row Zero-based native transport row; invalid returns zero.
     * @return Signed native concentration*m3 loss, including negative production where
     * configured. No reaction solve, reporting-ledger drain, allocation or mutation occurs.
     */
    double surfaceReactionLoss(int row) const noexcept {
        return row >= 0 && std::size_t(row) < rk_reacted_.size() ? rk_reacted_[row] : 0;
    }
    /**
     * @brief Detach source/checkpoint recorders, settle CPU surface packets and release marching
     * work lists.
     * @note Clears borrowed common/history/stream pointers and checkpoint flags, settles/flushes
     * remaining surface worker books when initialized, optionally overwrites telemetry CSV and
     * clears major momentum/ side/active/tier/boundary vectors plus active-fraction telemetry.
     * File-open failure is silently ignored. Marks uninitialized; does not destroy external
     * canonical mesh/state/groundwater/physical owners, advance leftover source time or write
     * their final result balances. Some scratch/bindings/counters remain until
     * replacement/destruction; run_stats() should be sampled before finalize. Requires exclusive
     * stable owners through settlement; unexpected delegated errors can leave partial cleanup.
     */
    void finalize() override;
    /**
     * @brief Refresh changed perimeter topology between completed advances.
     *
     * @note Inactive/missing-boundary returns. Settles pending surface packets, rebuilds
     * non-WALL and SWE wall lists, zeros new boundary window integrals, preserves prognostic q
     * for surviving slots, rebuilds prescribed/runtime native tuples, ensures receipt entries,
     * resolves qualified RK2 boundary signals at completed time and schedules an active/tier
     * rebuild. Wall output flux is zeroed; previous cell pins are retained rather than cleared.
     * Requires valid species/slot indices and runtime tuple shapes. Allocates/mutates
     * progressively and can throw after partial reconfiguration; call exclusively between
     * completed advances, not during a face/cell/source phase.
     */
    void refreshBoundaries();

    /**
     * @brief Read base substeps executed by the most recent accepted advance.
     * @return Stored last_steps_, zero after initialization; not cumulative firings across all
     * tiers. No synchronization, reset or solver work occurs; caller reads stable owner
     * telemetry.
     */
    long last_num_steps() const noexcept override { return last_steps_; }
    /**
     * @brief Read the most recent accepted base or tail duration.
     * @return Stored last_dt_ s, initially zero. Does not recompute stability or imply every
     * tier fired at that duration; no mutation/synchronization occurs.
     */
    double last_step_size() const noexcept override { return last_dt_; }
    /**
     * @brief Snapshot cumulative CPU marcher work and rebuild-sampled active/tier statistics.
     * @return Value containing total base substeps, cumulative face work, last duration s,
     * sample-weighted active min/max/mean and accumulated tier occupancy. Not
     * elapsed-time-weighted wet area or a new stability solve. Read before finalize resets
     * active samples/tier lists; no allocation or mutation. Requires stable serialized access to
     * host counters.
     */
    RunStats run_stats() const noexcept override;
    /**
     * @brief Read the marcher's stored lifecycle activation flag.
     * @return True after initialization and before finalize. Does not validate borrowed
     * geometry/context, current checkpoint phase or external source lifetime. Allocation-free
     * and read-only.
     */
    bool is_initialized() const noexcept override { return initialized_; }
    /**
     * @brief Borrow the last advance's integrated live junction exchange.
     * @return Solver-owned m3 per non-outfall coupling point, positive surface-to-node.
     * Reset/updated by advances; initialization/restore/replacement invalidates views. Does not
     * consume receipts or deliver canonical 1D water/native mass; stable serialized owner access
     * is required.
     */
    const std::vector<double>& last_coupling_exchange() const noexcept override { return exch_; }

    /**
     * @brief C3: cumulative spill volume (m³) the coupled nodes could not supply from their
     * ponded storage. Should stay 0 on a well-posed deck; a growing value means the orifice is
     * sized to move more water than the nodes hold above the rim.
     *
     * @return Cumulative requested node spill m3 unavailable from ponded native storage. Read
     * only; zero does not by itself prove every water/quality balance. Does not reserve, refund
     * or deliver water.
     */
    double spillDeficit() const noexcept { return spill_deficit_; }

private:
    friend struct ExplicitInertialSolverTestAccess;
    friend class SurfaceRouter2D;
    friend class openswmm::SWMMEngine;
    /**
     * @brief Encode a qualified completed CPU/source or coupled RK2 component boundary.
     * - Argument bytes (out): Replaced only on complete success; retained on diagnostic failure.
     * @return Empty on success, otherwise qualification/archive/size diagnostic.
     * @note Copies mutable owners; no physical/calendar advance. Exclusive consistent borrowed
     * views are required. Definition specifies format/platform guards and component limits.
     */
    std::string checkpointBytes(std::string& bytes) const;
    /**
     * @brief Validate the supported complete accepted coupled global RK2 checkpoint profile.
     * @return Empty on qualification, otherwise owner/clock/forcing/native/thermal/boundary
     * refusal.
     * @note Read-only with possible allocation/checked lookup exceptions; requires initialized
     * stable matching owners and does not settle or repair them. Definition details each
     * qualification.
     */
    std::string rk2CheckpointProfile() const;
    /**
     * @brief Serialize immutable current RK2 model and held forcing identity.
     * @return Binary identity string; may throw on allocation/archive/invalid prepared views.
     * @note Read-only and not a mutable-state checkpoint. Requires qualified stable exclusive
     * owners.
     */
    std::string rk2CheckpointModel() const;
    /**
     * @brief Serialize RK2 model identity with optional borrowed managed prescription/weather
     * overrides.
     * @return Binary immutable identity; may throw during copy/allocation/archive. The unnamed
     * nullable binding pointer supplies stable matching optional
     * source/forcing/boundary/weather/heat candidates, with null fields using live owners. It is
     * borrowed for this call only; no adoption or physics advance.
     * @note Requires prior profile validation and exclusive stable prepared identities.
     * Definition specifies encoded mesh/options/native/thermal/snow and immutable boundary model
     * coverage.
     */
    std::string rk2CheckpointModel(const Rk2CheckpointBinding*) const;
    /**
     * @brief Serialize the qualified coupled RK2 boundary into the output string reference.
     * @return Empty on success, otherwise profile/archive/size diagnostic. The unnamed writable
     * string argument is replaced only after complete successful encoding, retained on
     * diagnostic failure.
     * @note Copies candidates; no physical/calendar mutation. Requires exclusive matching views.
     */
    std::string rk2CheckpointBytes(std::string&) const;
    /**
     * @brief Restore a bounded RK2 envelope into fresh matching exclusive component owners.
     * @return Empty on successful staged install, otherwise profile/freshness/model/decode
     * diagnostic. The unnamed string is an owned encoded payload consumed locally. This overload
     * supplies no managed binding or hooks and requests installation, not inspection. Original
     * borrowed owner identities are preserved; unrelated engine/calendar/output state is
     * excluded.
     */
    std::string loadRk2CheckpointBytes(std::string);
    /**
     * @brief Validate and optionally install a coupled RK2 checkpoint using private owner
     * candidates.
     * - Argument inspect (in): True for validation-only return; false installs after final hook.
     * @return Empty on success/inspection, otherwise profile/freshness/decode/model/receipt
     * diagnostic.
     * @note Unnamed arguments, in order: owned bounded version-4 byte string; optional borrowed
     * managed model binding; optional pointer to a nonempty callback receiving const staged
     * marcher/surface/GW views valid only during the call; optional pointer to a nonempty final
     * pre-install callback. Null hooks are skipped. Callback effects are external and not rolled
     * back. Requires fresh qualified initialized exclusive owners and stable prepared
     * identities. Canonical owners are adopted only after all archive, identity, physical and
     * completed-clock checks; definition specifies exact scope. Does not restore unrelated
     * engine/calendar/output state.
     */
    std::string loadRk2CheckpointBytes(
        std::string, const Rk2CheckpointBinding*,
        const std::function<void(const ExplicitInertialSolver&, const SurfaceStateData&,
                                 const SubsurfaceSolver&)>*,
        const std::function<void()>*, bool inspect);
    /**
     * @brief Stage a first-order source/CPU restore with optional router receipt guards and
     * callbacks.
     * - Argument bytes (in): Owned bounded envelope.
     * - Argument router_boundary (in): Optional ten-element completed clock/water/provider
     * receipt guard; definition specifies slot meanings and SI units.
     * - Argument pending_outfalls (in): Optional per-cell pending outfall m3 guard, used with
     * router_boundary.
     * - Argument validate_source (in): Optional pointer to nonempty staged-source validation
     * callback.
     * - Argument before_install (in): Optional pointer to nonempty final pre-install callback;
     * external effects are not rolled back.
     * - Argument boundary_slots (in): Optional exact ordered exterior boundary slot guard.
     * - Argument inspect_source_end (out): Optional completed time s; nonnull selects
     * validation-only return.
     * @return Empty on success/inspection, otherwise profile/freshness/decode/model/receipt
     * diagnostic.
     * @note Requires fresh exclusive prepared owners. Reads private copies and installs
     * nonthrowing moves only after all checks. Standalone RK2 dispatch rejects every optional
     * router/source argument. No whole-engine calendar/output restoration; definition supplies
     * full slot and lifetime contracts.
     */
    std::string loadCheckpointBytes(
        std::string bytes, const std::array<double, 10>* router_boundary = nullptr,
        const std::vector<double>* pending_outfalls = nullptr,
        const std::function<void(const runoff::SourceWaterDriver&)>* validate_source = nullptr,
        const std::function<void()>* before_install = nullptr,
        const std::vector<int>* boundary_slots = nullptr, double* inspect_source_end = nullptr);
    /**
     * @brief Refresh the attached source history's checkpoint match after final face
     * publication.
     * @throws std::runtime_error Recorder refuses the common-stage match, prefixed
     * [2D_SOURCE_RESULTS].
     * @note No recorder is a no-op. Updates only checkpoint matching metadata, without adding
     * another source interval or firing physics. Requires live attached history/common stage and
     * exclusive accepted boundary access; recorder effects are not rolled back here.
     */
    void refreshSourceResultBoundary();
    double advanceImpl(double current, double target);
    /**
     * @brief Validate the current finite surface/porous thermal ownership required by coupled
     * RK2.
     * @return Empty for disabled heat or a qualified matched GROUND_COLUMN profile; descriptive
     * diagnostic for incompatible coverage/row/owner identity, channel/LID footprint, failed
     * state, latent donor or ledger shape/finiteness.
     * @note Requires live prepared mesh/state/groundwater/options before dereferencing. Checks
     * full aquifer coverage, shared canonical physical owner and temperature identity, active
     * thermal ledgers and nonnegative completed per-cell thermal time. Some checked vector
     * access may throw. Read-only; no repair, allocation of physical state, forcing sample or
     * canonical mutation.
     */
    std::string rk2ThermalProfileError() const;
    /**
     * @brief Apply the split thermal operator to qualified surface and physical ground owners.
     * - Argument seconds (in): Finite positive duration s.
     * - Argument sample_time (in): Finite elapsed held forcing sample s, called time in source.
     * @note Publishes degC*m3/J receipts and derived temperature views, not hydraulic water. Can
     * throw after partial owner work; requires validated finite-heat profile and enclosing
     * coupled transaction.
     */
    void advanceRk2Thermal(double seconds, double sample_time);
    /**
     * @brief Advance non-temperature finite surface/physical-porewater exchange with
     * conservative SSP blending.
     * @param[in] seconds Finite positive accepted contact interval s.
     * @note Disabled heat or temperature-only tuple returns. Saves surface/ground/porewater
     * beginnings, runs two beginning-donor Euler contacts scaled by current wet fraction,
     * refreshes canonical SIGMA views between stages, then blends non-temperature mass/contact
     * receipts and rebuilds derived native views/ground contact ledger. No water movement,
     * temperature solve or time-calendar update. Allocates and throws on contact/blend/mapping
     * refusal after possible earlier staged changes; requires matching qualified physical owners
     * and exclusive enclosing RK2 rollback.
     */
    void advanceRk2BedContact(double seconds);
    /**
     * @brief Advance an owned snow source interval on private weather and publish accepted
     * liquid water.
     * @param[in] seconds Finite positive accepted snow duration s.
     * @param[in] start Finite elapsed interval start s, consistent with the snow completed
     * horizon.
     * @note Inactive snow returns. Copies snow plus its borrowed weather context, advances to
     * start+seconds with held rainfall/catch factors, preflights each owned cell's finite
     * nonnegative new liquid m3, then installs snow/volume, uses accepted exposure and
     * reconstructs all heads/depths. No native engine weather calendar is changed and no
     * ordinary rain term is replayed for snow-owned cells. Refusal throws std::runtime_error
     * before adopting the copied snow/water; allocations/reconstruction can throw and enclosing
     * RK2 owner handles broader staging. Requires exclusive matching snow/mesh/state.
     */
    void advanceRk2Snow(double seconds, double start);
    /**
     * @brief Apply exact pollutant decay and scoped BULK kinetics to wet surface inventories.
     * @param[in] seconds Finite positive reaction duration s, validated by owner; empty loss
     * state or nonpositive/NaN interval returns. No independent positive-infinity guard is
     * performed here.
     * @note Stages full native cell_mass and cumulative loss, skips cells not above
     * dry_depth*area, then uses full decay alone or half decay/full subcycled kinetics/half
     * decay. Inputs are native concentration, DT and degC; age/temperature are not decayed as
     * pollutants. Rejects nonfinite/negative pollutant/MSX results and nonfinite signed loss.
     * Adopts staged arrays only after every wet cell succeeds; workspace scratch can change on
     * refusal. No water, physical heat, engine report ledger or clock mutation. Requires live
     * compatible reaction/config/state and serial shared workspace; allocations may throw.
     */
    void advanceRk2SurfaceReactions(double seconds);
    /**
     * @brief Deliver the unreported native pollutant reaction delta to the engine balance.
     * @note The context reference is writable and matching, with sufficient quality reaction
     * ledger extent. Stages finite converted totals then advances reported counters; throws on
     * mismatch/overflow. No physical reaction replay or water/calendar mutation; exclusive
     * report ownership required.
     */
    void syncRk2ReactionReport(SimulationContext&);
    const ReactionData* rk_reactions_ = nullptr; // immutable model config
    transport::RxWorkspace rk_reaction_workspace_;
    std::vector<int> rk_reaction_rows_;
    std::vector<double> rk_decay_, rk_reacted_, rk_reacted_reported_;
    std::string rk_reaction_identity_;

    // Recompute η/depth from state volumes for the whole mesh.
    /**
     * @brief Reconstruct SI surface head/depth from authoritative volume for every mesh cell.
     * @note Uses selected FLAT/VFR geometry in a static OpenMP cell partition. Requires
     * initialized matching mesh/options/state arrays and positive geometry. No
     * water/native/momentum/receipt change, clock advance or render-gradient reconstruction;
     * caller excludes competing state writers.
     */
    void reconstructAll();
    // Flush every pending face accumulator into its cell (CSR gather over ALL
    // cells) and refresh the closure of touched cells. MUST run before any
    // tier/active-set reassignment: a cell deactivated or re-tiered with a
    // pending side-accumulator strands flux whose counterpart the other side
    // already applied — the backstop then realizes it as created volume.
    /**
     * @brief Gather all pending CPU surface water/native/momentum side packets before reindexing
     * work.
     * @note First compacts groundwater bookkeeping through settle()/compactPending();
     * groundwater water remains pending for its paired cell firing. If surface flag is set,
     * clears it, gathers each own-side packet through CSR, updates canonical water/nonnegative
     * native rows/SWE momentum, runoff accounting and touched head/depth, then zeros consumed
     * side slots. No held source or ordinary boundary/node exchange is fired. Requires
     * consistent prepared arrays, exclusive outer access and safe disjoint cell-parallel writes.
     * No whole-operation rollback or general finite-state validation.
     */
    void settleAccumulators();
    // Apply lazily-accumulated sources (rain/held coupling) to INACTIVE cells
    // over [t_last_sync_, t], rebuild the active cell/edge lists, and assign
    // the LTS tiers (dt0_ = finest active CFL requirement).
    /**
     * @brief Settle CPU side packets and rebuild hysteretic active cells, stable face lists and
     * binary tiers.
     * @param[in] t Finite accepted elapsed synchronization horizon s, no earlier than
     * lazy-source clock.
     * @note Settles all surface side water/native/momentum, lands inactive held sources, then
     * seeds live depth/coupling/pinned/thermal/RK2-owner cells and groundwater owed surface
     * returns. Grows prepared halos, records frontier breach state, computes surface
     * mode-specific CFL/diffusion/thermal and independent groundwater stability, chooses shared
     * dt0 and assigns each owner's own cells/faces. Boundary/concentrated sources pin the
     * surface only; dry-front halo tiers propagate in fixed edge order. Zeros momentum of faces
     * with inactive sides and rebuilds ascending union lists. Updates rebuild-sampled active
     * fractions/tier occupancy and optional telemetry, not elapsed-time averages. Requires
     * compatible stable geometry, positive finite controls, live source/physical owners and
     * exclusive outer scheduling; internal cell workers own disjoint state/scratch. Can allocate
     * or fail after source settlement/partial list replacement; no rollback. Does not fire new
     * accepted face cycles.
     */
    void syncAndRebuild(double t);
    // Tighten-only dt0_ refresh from CURRENT depths/speeds between rebuilds
    // (dt0_ may only grow at syncAndRebuild, which reassigns the tiers).
    /**
     * @brief Tighten the shared base step from current active surface state without changing
     * tiers.
     * @note No active cells returns. Recomputes wet-cell mode-specific CFL/diffusion duration
     * using current depth/speed and divides by 2^stored_tier, min-reduces across workers and
     * only decreases dt0_. DIFFUSIVE_WAVE refreshes active slope scratch first. Growth,
     * groundwater limit refresh and tier reassignment wait for syncAndRebuild(). Requires stable
     * initialized positive geometry/controls and exclusive outer owner access; no physical
     * firing, receipt settlement or allocation by the reduction.
     */
    void refreshDt0();
    // Fire one tier's faces over their Δt: inertial update + Froude cap +
    // face-cadence positivity share, booking ±ΔM into both side accumulators.
    // global_step = every active cell fires at this same Δt (the window tail),
    // so a face's exporter republishes its volume every time this face does
    // and the positivity share is NOT divided down by a refire ratio.
    /**
     * @brief Dispatch an accepted interior-face pass to the configured momentum law.
     * @param[in] faces Stable valid unique face IDs from prepared tier/active lists, unchecked.
     * @param[in] dt_f Finite positive face duration s, already within accepted stability
     * constraints.
     * @param[in] global_step True treats every exporter as republished at this duration; false
     * uses prepared face/cell tier refire ratios in donor positivity budgets.
     * @note FULL_SWE, DIFFUSIVE_WAVE and default LOCAL_INERTIAL share coupled water/native side
     * receipts but their own momentum law. Requires exclusive outer scheduler and disjoint
     * per-face worker writes; does not gather cells, update owner clocks or roll back delegated
     * failures.
     */
    void fireFaces(const std::vector<int>& faces, double dt_f, bool global_step = false);
    // Fire one tier's cells over their Δt: gather + clear own-side face
    // accumulators, apply sources, refresh closure + Perot vector; tier-0
    // firings also evaluate the availability-clamped boundary edges.
    // tier0 = this firing carries the tier-0 cadence work (boundary edges and
    // the live junction exchange), which fires once per finest substep.
    /**
     * @brief Fire selected CPU cells with managed source ownership and compile-time fused row
     * specialization.
     * @param[in] cells Stable unique valid prepared cell IDs belonging to the selected due
     * tier/active list.
     * @param[in] dt_c Finite positive accepted cell interval s.
     * @param[in] tier0 True owns finest boundary/live junction/common-source/channel-contact
     * work; false fires only this selected cell-tier gather/source/momentum pass.
     * @note At tier zero pre-solves shared channel heat and lands managed common-stage face
     * water before advancing that source stage to source_phase_end_. Dispatches FULL_SWE/other
     * cell kernels and fused native-row processing when at least eight rows amortize setup.
     * Clears channel ready/mixed flags only after normal completion. Mutates canonical
     * surface/physical/provider/recording state progressively; exceptions may follow earlier
     * gathers/contact effects. Requires exclusive prepared outer ownership, valid worker scratch
     * and enclosing staged acceptance where atomicity is required.
     */
    void fireCells(const std::vector<int>& cells, double dt_c, bool tier0);
    /// Closure-specialised body of fireCells (kSwe = FULL_SWE momentum update).
    template <bool kSwe, bool kFuse>
    /**
     * @tparam kSwe True applies prognostic SWE momentum/wall pressure/friction, false optional
     * Perot reconstruction for other momentum modes.
     * @tparam kFuse True fuses accepted held-source row processing with blockwise native side
     * gather; caller selects only a supported matching runtime profile with sufficient native
     * rows.
     * @param[in] cells Unique valid prepared cells, unchecked; each worker owns its cell and
     * incident own-side water/native/momentum slots.
     * @param[in] dt_c Finite positive accepted firing duration s, consistent with
     * source_phase_end_.
     * @param[in] tier0 True enables the serial finest boundary/live-junction exchange passes and
     * advances within-window head-ramp time. Boundary cells must be pinned active on tier zero.
     * @details Gathers own-side m3/native/m4/s packets, preserving beginning-state RK2 donor
     * water until source debits are budgeted, processes held/runtime sources and closure,
     * detects halo-front breach, updates Perot or SWE wall-balanced/friction momentum, then
     * folds worker ledgers. Finest boundary pass evaluates the selected hydraulic law,
     * availability/stage-equality caps, paired native/J receipts, prognostic q and equal scaling
     * of requested SWE momentum. Finest junction pass samples live surface heads against frozen
     * 1D heads, caps drains by cell water and gross spill by remaining ponded node storage,
     * publishes integrated point/native/spill receipts and per-cell water before reconstruction.
     * @note Does not mutate canonical 1D node water directly; router adopts point receipts
     * later. No whole-pass rollback or complete finite/shape validation. Some RK2 boundary paths
     * throw after earlier cells/boundaries changed; noexcept species helpers can terminate on
     * unexpected dependencies. Requires stable geometry/forcing/bindings, no duplicate cells,
     * serial outer scheduling and provisioned worker team/scratch. Temporary boundary-donor
     * arrays may allocate. Caller checks thermal failures and stages qualified coupled
     * acceptance, including external recorder/provider effects where applicable.
     */
    void fireCellsImpl(const std::vector<int>& cells, double dt_c, bool tier0);
    // One halving-order macro cycle of nsub base substeps: tier k fires every
    // 2^k substeps.
    /**
     * @brief Advance binary local-time face/cell owners through a complete accepted macro
     * sequence.
     * @param[in] dt0 Finite positive accepted base duration s.
     * @param[in] nsub Nonnegative base-substep count aligned with due tier horizons, normally
     * 2^(K-1).
     * @param[in] time Finite elapsed start s for source/cell completion clocks.
     * @note Faces fire at each interval start, cells gather at the corresponding endpoint.
     * Groundwater uses its own stability-assigned tiers on the same ladder, fires after matching
     * surface cells, then batch-frozen node exchange samples at tier-zero cadence. Captures
     * accepted source results after all due firings/samples and increments base-step counters.
     * Optional invariant mode emits diagnostics without repairing state. Does not rebuild/choose
     * dt0, finalize host reporting or roll back a late physics/recording exception. Requires
     * exclusive initialized prepared owners and accepted forcing cuts.
     */
    void runMacroCycle(double dt0, int nsub, double time);

    MeshData* mesh_ = nullptr;
    SurfaceStateData* state_ = nullptr;
    SolverOptions2D* opts_ = nullptr;

    void refreshConveyance(); ///< Synchronize runtime LI factors and reconstructed cell discharge.
    InertialEdges edges_;
    /// Per unique interior face (m²/s). LOCAL_INERTIAL: the prognostic face
    /// discharge per open width; transported flux is q * xi * conveyance.
    /// FULL_SWE / DIFFUSIVE_WAVE: the last evaluated face mass
    /// flux per unit width (diagnostic — published as edge_flux).
    std::vector<double> q_;
    /// LOCAL_INERTIAL: Perot cell discharge vector (θ < 1 / ADVECTION).
    /// FULL_SWE: the PROGNOSTIC cell-mean unit discharge (h·u, h·v), m²/s.
    std::vector<double> qcx_, qcy_;

    // -----------------------------------------------------------------------
    // Momentum closure (MOMENTUM_EQUATION, 2D_FULL_SWE plan §2.1). One
    // marcher, three face laws; everything below the face/cell kernels
    // (tiers, active sets, positivity, coupling, transport) is shared.
    // -----------------------------------------------------------------------
    Momentum2D mode_ = Momentum2D::LOCAL_INERTIAL;
    /// Mesh carries at least one quad (2D_PERF_REGRESSION_DIAGNOSIS §2 F1):
    /// resolved ONCE in initialize(). MeshData::n_quads() is an O(n_cells)
    /// scan and was being re-run at the top of every face-tier fire.
    bool has_quads_ = false;
    /// FULL_SWE: CSR of WALL boundary slots per cell (wall_ptr_[i] ..
    /// wall_ptr_[i+1]) so the mirror-flux booking touches only the perimeter
    /// cells that have one (F5); every other cell's row is empty.
    std::vector<int> wall_ptr_, wall_slot_;
    /// FULL_SWE momentum accumulators per face side (m⁴/s = m²·m²/s): the
    /// x/y momentum the face booked for cL / cR, gathered and cleared by the
    /// cell exactly like the two logical water sides (same writer, same cadence).
    // Momentum packets share face_accum_ slots 2..5 in FULL_SWE.
    /// Momentum the last boundaryFluxSwe() call booked into qcx_/qcy_, so its
    /// caller can rescale it when the availability clamp shrinks the mass.
    /// Scalar: the boundary loop is serial (perimeter-sized).
    double swe_bc_dqx_ = 0.0, swe_bc_dqy_ = 0.0;
    /// RECONSTRUCTION_ORDER 2 (FULL_SWE): limited reconstruction gradients of
    /// (eta, h, u, v) per cell, refreshed at every face pass, and the SSP-RK2
    /// stage buffers. Empty at order 1.
    bool second_order_ = false;
    bool rk_transport_stage_ = false;
    std::vector<double> rk_stage_mass_, rk_stage_volume_; // scratch beginning-state donor views
    /// gex_/gey_: eta gradients in [0, n), depth gradients in [n, 2*n).
    /// The second block supplies the face bed (eta - h) only for order 2.
    std::vector<double> gex_, gey_, gux_, guy_, gvx_, gvy_;
    std::vector<double> rk_v0_, rk_qx0_, rk_qy0_;
    /// Stage-0 copies of the per-advance ledgers (F6): members so the RK2
    /// step allocates nothing.
    std::vector<double> rk_inf0_, rk_bc0_, rk_ex0_, rk_drawn0_, rk_cpl0_;
    /**
     * @brief Rebuild limited active-cell MUSCL gradients of eta, depth and x/y velocity.
     * @note Requires FULL_SWE second-order-sized scratch and valid prepared SI
     * head/depth/momentum/CSR. Zeroes active gradients first; depths<=10*dry retain zero. Fully
     * wet stencils use conditioned inverse-distance fitting of eta/depth/velocity.
     * Dry sills exclude neighbors; shoreline velocity stays cell-centered. Bounds at connected
     * face midpoints and vertex wet-cell rings limit velocity; face positivity jointly scales
     * eta/depth. Vertex CSR is prepared in initialize(), before any checkpoint fingerprint.
     * Allocates per-vertex bounds outside the cell loop; static workers write disjoint cells.
     * Does not change water/momentum inventories, receipts, mesh identity or clocks.
     */
    void computeLimitedGradientsSwe();
    MomentumPhysics2D momentum_physics_;
    std::vector<int> wall_drag_slots_;
    double wall_drag_energy_ = 0; ///< Cumulative kinetic energy removed / rho (m5/s2).
    void applyMomentumPhysics(double seconds);
    double runMomentumStep(double limit, double time);

    /**
     * @brief One SSP-RK2 (Heun) step of length dt over the active lists: two forward-Euler
     * substeps averaged with the start state; the ledgers that accumulated over both stages are
     * halved to the trapezoidal value.
     *
     * @details Advance an accepted global surface SSP-RK2 interval, with qualified coupled
     * operator splitting.
     * - Argument dt (in): Positive finite scheduler-selected duration s, called interval in the
     * definition.
     * - Argument time (in): Finite elapsed start s, called start in the definition.
     * @note Uses depth-zero thermal retry. Requires initialized exclusive owners and prepared
     * active lists. Definition documents coupled profile restrictions, source/thermal/reaction
     * splitting, receipt blending and rollback. The standalone surface path has no coupled
     * snapshot guarantee. Does not advance the engine calendar or publish source history.
     */
    void runRk2Step(double dt, double time);
    /**
     * @brief Execute an RK2 interval or recursively halve a coupled latent-cooling refusal.
     * - Argument dt (in): Positive finite duration s, called interval in the definition.
     * - Argument time (in): Finite elapsed start s, called start in the definition.
     * - Argument thermal_depth (in): Recursive retry depth, normally zero; refuses further
     * halves at 12.
     * @note Requires matching initialized exclusive owners/active lists. Qualified coupled state
     * and borrowed boundary receipts are restored on failure; standalone surface state has no
     * such rollback. May throw on validation, allocation, physics or unrepresentable/refused
     * retry. Full definition contracts specify split ordering, receipt blending, forcing knots
     * and accepted-step accounting.
     */
    void runRk2Step(double dt, double time, unsigned thermal_depth);
    /**
     * @brief Test whether coupled RK2 may apply surface latent cooling.
     * @return True for groundwater+second-order+enabled surface heat and either native
     * evaporation enabled or any retained runtime forcing row on evaporation channel 2. Does not
     * require that row currently be active/nonzero. Prepared state/options pointers must be
     * live; no mutation/allocation.
     */
    bool surfaceLatentCooling() const noexcept;
    struct Rk2BoundarySignal {
        int slot = -1, table = -1;
        bool stage = false;
        double scale = 1;
        std::string name;
        GwSourceSignal signal;
        std::shared_ptr<const Table> curve{};
        double x_scale = 1;
    };
    std::vector<Rk2BoundarySignal> rk_boundary_signals_;
    double rk_boundary_anchor_ = 0;

    /// FRONT_REBUILD: frontier_[i] = 1 for an active cell with at least one
    /// inactive neighbour (the outer ring of the halo). When such a cell
    /// crosses the activation depth the front has used up the halo, and the
    /// next macro cycle rebuilds the active set instead of waiting for the
    /// cadence (front_breach_).
    bool front_rebuild_ = false;
    std::vector<uint8_t> frontier_;
    bool front_breach_ = false;
    double h_on_front_ = 0.0; ///< activation depth of the last rebuild
    /// DIFFUSIVE_WAVE: per-cell largest |surface slope| over its faces, from
    /// the last rebuild/refresh — the explicit diffusion step bound's slope.
    std::vector<double> dw_slope_;
    /**
     * @brief Face kernels of the three closures (fireFaces dispatches on mode_).
     *
     * @details Book an accepted interior face pass using LOCAL_INERTIAL semi-implicit
     * friction/momentum with optional Perot theta/advection.
     * @param[in] faces Stable unique valid prepared interior-edge IDs; unchecked and safe for
     * disjoint per-face OpenMP writes. Count/products must fit the configured loop/index types.
     * @param[in] dt_f Finite positive accepted face duration s.
     * @param[in] global_step True uses one exporter publication per firing; false divides
     * positivity share by 2^(exporter_tier-face_tier). Tier shifts must be valid and aligned to
     * scheduler cadence.
     * @note Uses prepared SI geometry and live cell state, limits outgoing m3 by exchange_beta
     * divided by triangle/quad face count and refire ratio, updates q_ m2/s, adds equal-opposite
     * facc m3, and books native advective/dispersion packets from the final accepted water
     * transfer. Dry or zero-conveyance faces reset q_ and skip exchange; ψ scales flux,
     * limiter volume and Perot discharge without changing CFL geometry. Applies Froude cap after the
     * inertial update. Updates cumulative face work and pending flag. Does not gather canonical
     * cell volume/native state, advance clocks, validate all finite inputs or roll back side
     * packets. Requires exclusive outer scheduling, matching per-face/native/momentum shapes and
     * stable mode/material/geometry.
     */
    void fireFacesInertial(const std::vector<int>& faces, double dt_f, bool global_step);
    /**
     * @brief Book an accepted interior face pass using FULL_SWE hydrostatic Riemann
     * mass/momentum with optional limited MUSCL reconstruction.
     * @param[in] faces Stable unique valid prepared interior-edge IDs; unchecked and safe for
     * disjoint per-face OpenMP writes. Count/products must fit the configured loop/index types.
     * @param[in] dt_f Finite positive accepted face duration s.
     * @param[in] global_step True uses one exporter publication per firing; false divides
     * positivity share by 2^(exporter_tier-face_tier). Tier shifts must be valid and aligned to
     * scheduler cadence.
     * @note Uses prepared SI geometry and live cell state, limits outgoing m3 by exchange_beta
     * divided by triangle/quad face count and refire ratio, updates q_ m2/s, adds equal-opposite
     * facc m3, and books native advective/dispersion packets from the final accepted water
     * transfer. Also books bed-corrected SWE momentum side packets m4/s, scaling advective
     * momentum with the accepted mass flux; dry-dry faces still allow the species dispersion
     * helper. Updates cumulative face work and pending flag. Does not gather canonical cell
     * volume/native state, advance clocks, validate all finite inputs or roll back side packets.
     * Requires exclusive outer scheduling, matching per-face/native/momentum shapes and stable
     * mode/material/geometry.
     */
    void fireFacesSwe(const std::vector<int>& faces, double dt_f, bool global_step);
    /**
     * @brief Book an accepted interior face pass using DIFFUSIVE_WAVE regularized Manning
     * discharge and face conveyance.
     * @param[in] faces Stable unique valid prepared interior-edge IDs; unchecked and safe for
     * disjoint per-face OpenMP writes. Count/products must fit the configured loop/index types.
     * @param[in] dt_f Finite positive accepted face duration s.
     * @param[in] global_step True uses one exporter publication per firing; false divides
     * positivity share by 2^(exporter_tier-face_tier). Tier shifts must be valid and aligned to
     * scheduler cadence.
     * @note Uses prepared SI geometry and live cell state, limits outgoing m3 by exchange_beta
     * divided by triangle/quad face count and refire ratio, updates q_ m2/s, adds equal-opposite
     * facc m3, and books native advective/dispersion packets from the final accepted water
     * transfer. Dry face depth resets q_ and skips transfer. Has no prognostic momentum
     * relaxation or Froude cap in this law. Updates cumulative face work and pending flag. Does
     * not gather canonical cell volume/native state, advance clocks, validate all finite inputs
     * or roll back side packets. Requires exclusive outer scheduling, matching
     * per-face/native/momentum shapes and stable mode/material/geometry.
     */
    void fireFacesDiffusive(const std::vector<int>& faces, double dt_f, bool global_step);
    /**
     * @brief Species advection + dispersion booked on face e for the mass transfer dM (m³,
     * positive cL→cR) at face depth hf — shared by every closure.
     *
     * @param[in] e Valid unique interior face ID, unchecked.
     * @param[in] a Left prepared cell index, matching edges_.cL[e].
     * @param[in] b Right prepared cell index, matching edges_.cR[e].
     * @param[in] dM Final accepted signed water m3, positive a-to-b, after all hydraulic caps.
     * @param[in] hf Finite nonnegative face flow depth m for dispersive wetted section.
     * @param[in] dt_f Finite positive face duration s.
     * @param[in] global_step True uses one donor refire, false stored cell/face tier ratio.
     * @note Inactive native rows returns. Books equal-opposite face-major [e*n_species+row]
     * native inventory using donorConc(), with beginning RK2 donor views when selected. Optional
     * isotropic D m2/s dispersion uses current published concentrations, bounded by
     * recipient-scaled pairwise equalization and unsigned donor positivity/refire share; signed
     * temperature uses equalization only. Atomically counts limiter bindings. Does not consume
     * canonical species, water or advance clocks. One worker owns each face; caller excludes
     * competing writes to the same side slots and supplies valid stable shapes/tiers/volumes. No
     * general finite validation, allocation or exception recovery.
     */
    void bookFaceSpecies(int e, int a, int b, double dM, double hf, double dt_f,
                         bool global_step) noexcept;
    /**
     * @brief FULL_SWE ghost-cell Riemann boundary flux for BC entry k on cell i: returns the
     * inflow-positive volumetric flux (m³/s) and adds the momentum change (with the bed-slope
     * correction) to the cell's q⃗.
     *
     * @param[in] k Valid compact boundary entry, mapping through bc_slot_ to an exterior mesh
     * slot.
     * @param[in] i Its prepared cell index, matching bc_cell_[k], with positive SI area.
     * @param[in] dt_c Finite positive accepted cell duration s used for requested momentum
     * booking.
     * @param[in] held Optional readable [eta m, depth m, qx m2/s, qy m2/s] beginning-state donor
     * tuple; null uses current surface/prognostic momentum. Not retained or modified.
     * @return Requested inflow-positive volumetric flux m3/s, before the caller's final
     * donor/stage caps. Tiny/nonpositive edge length returns zero. Normal flow uses signed
     * Manning slope; prescribed flux is imposed directly (including dry-bed critical
     * reconstruction), while stage uses characteristic hydrostatic ghost data and cannot
     * constrain supercritical outflow.
     * @note Immediately adds requested pressure-balanced momentum to qcx_/qcy_ and saves
     * swe_bc_dqx_/dqy_ so caller can undo the rejected fraction. Does not book water/native
     * receipts itself or clear those scratch increments on every early return; caller zeros them
     * before each serial perimeter call. Requires FULL_SWE prepared geometry/arrays/forcing and
     * finite physical parameters; no shape checks, allocation, canonical 1D mutation or local
     * rollback. Caller serializes shared momentum scratch.
     */
    double boundaryFluxSwe(std::size_t k, int i, double dt_c,
                           const std::array<double, 4>* held = nullptr);
    /**
     * @brief DIFFUSIVE_WAVE step bound helper: refresh dw_slope_ for the active cells.
     *
     * @details Rebuild the active-cell slope magnitude used by the explicit diffusive stability
     * bound.
     * @note Stores max(|headR-headL|*inv_dx_normal) over incident unique interior faces into
     * dw_slope_. Exterior wall contribution is absent/zero; inactive-cell entries retain their
     * prior value. Requires DIFFUSIVE_WAVE-sized scratch, valid prepared CSR and stable SI
     * heads. Uses disjoint cell workers; no water/momentum/native state, receipts or clocks are
     * changed, and no allocation occurs.
     */
    void refreshDiffusiveSlopes();
    std::vector<uint8_t> cell_active_;
    std::vector<int> active_cells_;
    /// Every face with both sides active, ascending — the union of the face
    /// tier lists, in the order the tier-0 list would have carried them after
    /// a collapse. Rebuilt with the tier lists; fired as one list by the
    /// window tail.
    std::vector<int> active_faces_;
    std::vector<uint8_t> pin_t0_; ///< cells pinned active + tier 0
                                  ///< (boundary + live coupling)

    // Tiered LTS. tier_[i] = k means cell i updates every 2^k base substeps
    // with Δt = 2^k·dt0; face tier = min of its incident cells so a face
    // always integrates at the finer cadence. Every face firing books the
    // identical ±ΔM into the two logical water sides (single writer per face); each cell
    // applies + clears its own side at its own firing — conservation across
    // tier interfaces is exact by construction.
    /// Rebuild scratch, held as members so a rebuild allocates nothing:
    /// the seed flags (n_triangles) and the per-active-cell CFL step.
    std::vector<uint8_t> rebuild_seed_;
    std::vector<double> rebuild_dt_cell_;
    /// Per-thread minima for the rebuild's CFL reduction (min is exact and
    /// order-independent, so any partition gives the identical dt0).
    std::vector<double> rebuild_dt_partial_;

    std::vector<uint8_t> tier_;      ///< per-cell tier
    std::vector<uint8_t> face_tier_; ///< per unique face
    /// CPU-private face packets: [waterL,waterR] for LI/DW; FULL_SWE adds
    /// [momentumXL,momentumXR,momentumYL,momentumYR]. Each logical side
    /// retains its original writer, scalar arithmetic and gather/clear order.
    /// Checkpoints stream the original separate logical vectors without transposing state.
    std::vector<double> face_accum_;
    std::size_t face_accum_stride_ = 2; ///< derived from momentum mode at initialize()

    // S1 — species mass rides the SAME face accumulators, with each face
    // holding adjacent rows at [e * n_species + s]. Booked in fireFaces
    // immediately after the volume ΔM, from the FINAL qn1 (after the Froude
    // cap and the positivity share),
    // at the exporting cell's concentration read at that same substep;
    // gathered and cleared in fireCells alongside the volume side. That is
    // D-2DT2: the species flux inherits the volume flux's tier cadence rather
    // than reproducing it, so conservation across tier interfaces is the
    // marcher's own property and not a second one to prove. Empty unless
    // `state_->transport.active()`.
    std::vector<double> sacc_L_;
    std::vector<double> sacc_R_;
    /// S7: "transport rows are live" — set with the accumulators above. The
    /// sites used to test `!sacc_L_.empty()`, which is ALSO empty on a mesh
    /// with no interior edge (one cell), where every species sink and source
    /// then silently skipped while the rows were active.
    bool species_on_ = false;
    /**
     * @brief Exporter concentration for a species at a cell, read against the cell's CURRENT
     * published volume — the same volume the positivity share budgets against, so cumulative
     * species takes are bounded by the same β share as the water and mass cannot go negative
     * where volume cannot.
     *
     * @param[in] s Valid native row index, unchecked.
     * @param[in] cell Valid prepared cell index, unchecked.
     * @return Native donor concentration (inventory/m3): beginning-stage mass/volume during an
     * RK2 transport stage, otherwise current canonical mass/volume. Zero for nonpositive water,
     * without a dry-depth threshold; signed temperature may yield negative degC. Requires
     * matching stage/native shapes and stable access. Does not consume mass, reserve water,
     * allocate or publish a report value.
     */
    double donorConc(int s, int cell) const noexcept;
    /**
     * @brief Remove `dv_m3` of water from cell `i` at the cell's concentration and book it to
     * `ledger` — infiltration, boundary outflow, coupling drain. Evaporation deliberately does
     * NOT go through here: it removes volume and no mass, so the concentration rises (§2.3 of
     * the plan).
     *
     * @param[in] i Valid prepared cell whose published water still denotes its pre-sink volume.
     * @param[in] dv_m3 Positive finite accepted water m3 already capped by the caller;
     * nonpositive/NaN or inactive transport returns without work.
     * @param[in,out] ledger Destination selector: must be the actual transport.lost_infiltration
     * or lost_boundary vector by reference identity, otherwise loss is classified CouplingOut.
     * Writes go to the current worker's deferred ledger scratch; this reference is not directly
     * incremented here.
     * @param[in,out] per_point_ledger Optional writable n_species native inventory tuple,
     * incremented with accepted donor-composition amounts. No bounds check or ownership
     * retention.
     * @note Debits canonical native rows at donorConc(), caps unsigned amounts against current
     * mass, retains signed temperature and adds produced-runoff water unless infiltration. Does
     * not remove canonical water itself. Caller serializes each cell and supplies valid
     * per-thread scratch/team size; independent cells may run concurrently, followed by
     * flushSourceLedgers().
     */
    void sinkMassAtCellConc(int i, double dv_m3, std::vector<double>& ledger,
                            double* per_point_ledger = nullptr) noexcept;
    // Sources share one water budget after face transfers have landed. Gross
    // constituent transfers are evaluated even when net water change is zero.
    // Returns false only when no source bookkeeping or volume change occurred.
    bool applyCellSources(int cell, double dt, double* boundary_donor = nullptr);
    bool applyPlainCellSources(int cell, double dt);
    // Selected only during an advance; external rebuild/settlement uses the full path.
    bool plain_sources_ = false;
    bool applyRuntimeSources(int cell, double dt);
    struct CellSourceStep {
        double area_dt = 0, rain = 0, coupling = 0, available = 0, infil = 0, evap = 0, out = 0,
               final_volume = 0, requested_infil = 0;
        double* ledger = nullptr;
    };
    bool prepareCellSources(int cell, double dt, CellSourceStep& source);
    void mixCellSourceRow(int cell, int row, double& mass, const CellSourceStep&);
    /**
     * @brief Stage the finest source tuple and perform the shared channel/soil finite heat
     * solve.
     * - Argument seconds (in): Finite positive accepted interval s, called dt in source.
     * @note Requires live prepared attached channel/thermal owners and exclusive access. Gathers
     * managed surface water/temperature before contact, publishes degC*m3 and marks the tuple
     * mixed once. Throws on refusal after possible partial owner effects; definition details
     * callback and scratch lifecycle.
     */
    void prepareChannelGround(double seconds);
    void applyCellSourceRow(int cell, int row, double& mass, const CellSourceStep& source);
    enum class SourceLedger {
        Infiltration,
        CouplingOut,
        Rainfall,
        CouplingIn,
        Exfiltration,
        Boundary,
        ExternalIn,
        ExternalOut,
        BedExchange,
        Count
    };
    /**
     * @brief Add one native source amount into this worker's deferred ledger slice.
     * @param[in] ledger Valid SourceLedger enum strictly before Count, unchecked.
     * @param[in] species Valid native transport row, unchecked.
     * @param[in] mass Signed native inventory (concentration*m3); sign follows ledger/row
     * convention.
     * @note Adds to source_ledgers_[thread*stride+ledger*n_species+species]. Caller provisions
     * nonoverlapping worker scratch, valid counts and finite accepted amounts. Does not mutate
     * global transport ledgers, canonical species/water or flush other workers; no allocation or
     * synchronization.
     */
    void bookSourceLedger(SourceLedger ledger, int species, double mass) noexcept;
    /**
     * @brief Fold every worker's deferred native/evaporation source books in fixed worker order.
     * @note Adds each native scratch row to its selected transport ledger when allocated, then
     * zeros that scratch even if the ledger is absent. Adds per-worker accepted evaporation m3
     * to evap_loss_total and clears it. Requires a positive prepared stride, matching row
     * extents and completion of all parallel producers; no concurrent writes. No water/species
     * physics, allocation or clock update.
     */
    void flushSourceLedgers();
    /**
     * @brief Select the current source-ledger worker slice.
     * @return OpenMP team thread number inside a parallel region when supported, otherwise zero.
     * Does not allocate, query configured maximum capacity or synchronize. Caller provisions
     * scratch for every participating worker and avoids simultaneous unrelated teams sharing
     * this solver.
     */
    int sourceThread() const noexcept;
    // Each worker owns a cache-line-padded slice: six species ledgers and
    // applied evaporation volume. Fold only after the parallel region ends.
    std::vector<double> source_ledgers_;
    std::size_t source_ledger_stride_ = 0;
    ChannelGroundState* channel_ground_ = nullptr;
    std::vector<CellSourceStep> channel_sources_;
    std::vector<uint8_t> channel_source_ready_, channel_temperature_mixed_;
    /// False when every booked ΔM is known to have been consumed already —
    /// true after a GLOBAL substep, where every active face fired and then
    /// every active cell gathered both of its sides (faces touching an
    /// inactive cell carry q = 0 and were zeroed when it deactivated). Under
    /// per-routing-step coupling every substep is global, so without this the
    /// marcher walked the whole per-cell CSR once per substep to find nothing.
    bool accumulators_pending_ = false;
    std::vector<std::vector<int>> cells_by_tier_;
    std::vector<std::vector<int>> edges_by_tier_;
    double dt0_ = 0.0;         ///< base (tier-0) step from the rebuild
    std::vector<int> bc_cell_; ///< cells with a non-WALL boundary edge
    std::vector<int> bc_slot_; ///< matching flat mesh edge slot
    std::vector<int> gw_row_map_;
    std::vector<double> bc_accum_; ///< ∫F_applied dt per BC entry (m³),
                                   ///< inflow-positive, reset per advance
    std::vector<double> bc_q_;     ///< prognostic boundary-edge discharge
                                   ///< (m²/s, inflow-positive). For
                                   ///< SPECIFIED_STAGE it is integrated by
                                   ///< the SAME inertial momentum law as an
                                   ///< interior face (ghost at η_bc); for
                                   ///< the prescribed-flux types it records
                                   ///< the applied per-metre discharge so
                                   ///< the Perot reconstruction sees the
                                   ///< boundary momentum either way.

    // Live junction exchange (windowless coupling): state_->node_coupling
    // points, evaluated at tier-0 cadence against live 2D heads and the
    // routing-step 1D heads. exch_[k] = ∫Q_k dt (m³, + = 2D→1D), reset per
    // advance; node_drawn_ caps a step's total spill at the node's stored
    // volume so fill-and-spill thrash is structurally impossible.
    std::vector<double> exch_;
    std::vector<double> node_drawn_; ///< spill drawn per node this advance (m³)
    /// C3: cumulative volume (m³) the orifice law asked a node for and the
    /// node's PONDED storage could not supply. Not an error — a spill that
    /// empties the ponded water is the physically right outcome — but it must
    /// be visible, because the 1D side's `y_new = max(y_new, 0)` floor would
    /// otherwise absorb an over-draw silently and create water. A validator
    /// watches this against zero on a well-posed deck.
    double spill_deficit_ = 0.0;

public:
    /**
     * @brief Experimental (OPENSWMM_2D_HEAD_RAMP): per-coupling-point 1D head trend slope (m/s,
     * 2D frame) extrapolated across the batch — the exchange evaluates against h_1d + slope·τ
     * instead of the held batch-start head. Empty = held heads (default).
     *
     * @param[in] slopes Owned per-coupling-point SI head trends m/s, moved into the solver.
     * Empty selects held heads; caller supplies correct point order/extent and finite values,
     * not validated here.
     * @note Experimental head extrapolation uses slope*within-window time, without mutating
     * canonical 1D heads. Requires exclusive configuration between completed windows; does not
     * rebuild topology or activate the experiment by itself.
     */
    void setExchangeHeadSlopes(std::vector<double> slopes) { exch_head_slope_ = std::move(slopes); }

private:
    std::vector<double> exch_head_slope_;
    double exch_tau_ = 0.0; ///< time into current advance (s)

    double t_last_sync_ = 0.0; ///< lazy-source clock
    CommonSourceStage* common_stage_ = nullptr;
    SourceResultHistory* source_history_ = nullptr;
    EngineSourceResultHistory* engine_source_history_ = nullptr;
    class EngineSourceResultStream* engine_source_stream_ = nullptr;
    bool checkpoint_ready_ = false;
    bool joint_checkpoint_ = false; // only SurfaceRouter2D may restore live 1D pairing
    /**
     * @brief Check accepted first-order source/CPU ownership, completed clocks and boundary
     * layout.
     * @return Empty on qualification, otherwise refusal text; allocation/checked lookup may
     * throw.
     * @note Read-only, requires initialized stable owners; does not settle packets or repair
     * state.
     */
    std::string checkpointBoundary() const;
    /**
     * @brief Archive and validate fixed CPU momentum, settled packets, masks, tiers and
     * statistics.
     * @param[in,out] archive Sequential archive, called a in the definition; reading mutates
     * this candidate progressively, so decode only into a private staged copy.
     * @note Requires matching prepared mesh/owner extents; throws on archive, shape, nonfinite
     * or unsettled state. Does not reconstruct momentum, advance physics or install external
     * owners.
     */
    void checkpointState(detail::CompletedSourceArchive& archive);
    /**
     * @brief Capture one accepted finest source interval into each attached recorder/stream.
     * @throws std::runtime_error Detailed stream or history capture refuses, with
     * source-specific prefix.
     * @note Detailed engine stream is invoked first, then engine history and local history. Each
     * requires a live matching common stage and completed owner phase. Earlier recorder/stream
     * effects remain if a later capture fails; no multi-recorder rollback. Does not fire physics
     * or advance calendar; result stream may perform its owned append I/O. Serial
     * accepted-boundary access is required.
     */
    void captureSourceResults();
    double source_phase_end_ = 0.0;
    int cycles_since_rebuild_ = 1000; /**
     * @brief < persists across advances (co-advance) Lazy-source landing without the O(nt)
     * rebuild (advance boundaries between rebuild cadences).
     *
     * @details Land held source effects on inactive cells without rebuilding active/tier lists.
     * @param[in] t Finite elapsed horizon s; nonpositive t-t_last_sync_ returns.
     * @note Applies source tuples over that gap only to cells not active, reconstructs changed
     * head/depth, folds worker ledgers then advances t_last_sync_. Does not fire
     * faces/groundwater or change tiers. Requires stable prepared source/physical owners and
     * exclusive outer scheduling; delegated failure can leave partial cells/ledgers before the
     * clock is updated. Supports disjoint internal cell workers.
     */
    void lazySourcesOnly(double t);
    long substeps_run_ = 0; ///< cumulative substeps (whole run)
    long face_passes_ = 0;  ///< cumulative face-kernel passes
    long last_steps_ = 0;   ///< substeps in the last advance()
    double last_dt_ = 0.0;
    bool initialized_ = false;

    // Active-fraction telemetry: (sim time, active cells) sampled at every
    // rebuild; dumped as CSV at finalize() when OPENSWMM_2D_MARCHER_TELEMETRY
    // names a file. The Phase-1 gate reads this to verify the thin-film budget
    // assumption on the Bellinge storm slice.
    std::vector<std::pair<double, int>> telemetry_; ///< kept ONLY when telemetry_path_ is set
    std::string telemetry_path_;
    /// Running active-fraction statistics for run_stats(). The samples above
    /// used to be kept for the whole run unconditionally, just to be folded
    /// into min/mean/max at the end (16 B per rebuild, ~10 MB per 2.6 M steps).
    double active_frac_min_ = 1.0e30;
    double active_frac_max_ = -1.0e30;
    double active_frac_sum_ = 0.0;
    long active_samples_ = 0;
    /// Cumulative rebuild-sampled cell count per LTS tier (report histogram).
    std::array<long, 8> tier_occupancy_{};

    /// G1: the two-zone groundwater kernel, or nullptr when no
    /// `[2D_AQUIFER]` was authored. Not owned — SurfaceRouter2D holds it.
    ///
    /// The marcher drives it through four hooks (SubsurfaceSolver.hpp
    /// §gw_lts) so the subsurface shares this ladder instead of running a
    /// loop of its own. The subsurface's tiers come from ITS OWN stability
    /// steps (guarantee G-A): a GW cell that exchanges with a node or
    /// receives infiltration is deliberately NOT pinned to tier 0 — only its
    /// surface twin is. Cross-cadence volume rides the same ±Δ side
    /// accumulators the surface faces use (G-B).
    SubsurfaceSolver* gw_ = nullptr;

public:
    /**
     * @brief Attach (or detach, with nullptr) the groundwater kernel. Must be called before the
     * first advance and after `initialize`.
     *
     * @param[in] gw Optional borrowed prepared groundwater owner, null detaches. Must outlive
     * all marcher firings/checkpoints, with matching cell/edge/row identity; callable after
     * initialize and before first advance. Stores pointer only; does not initialize, settle or
     * refresh row mapping.
     * @note Requires exclusive completed owner boundary. Replacing a running owner with pending
     * receipts is unsupported without the enclosing coordinated transaction.
     */
    void setSubsurface(SubsurfaceSolver* gw) noexcept { gw_ = gw; }
    /**
     * @brief Rebuild exact-name native surface-to-groundwater row mapping after owners are
     * prepared.
     * @throws std::invalid_argument Active surface/groundwater layout lacks exactly one name per
     * row.
     * @note Replaces gw_row_map_ with -1 then matches exact names; duplicate groundwater names
     * select its last matching index. Inactive/no groundwater leaves unmapped rows. Does not
     * move inventory, convert concentrations or validate unit equivalence; initialization
     * supplies compatible native units. Allocates and may fail after replacement; requires live
     * stable owners and exclusive mapping access.
     */
    void refreshSubsurfaceRows();

    /**
     * @brief The unique interior-edge topology this marcher built. Exposed so the groundwater
     * kernel can put its lateral Darcy flux on the SAME faces rather than build a second,
     * necessarily-consistent copy of the same graph. Valid only after `initialize`.
     *
     * @return Solver-owned const unique interior-edge geometry/CSR, valid after initialize.
     * Shares exact topology with groundwater; initialization/restore/replacement invalidates
     * views. No copy, rebuild, ownership transfer or physical mutation occurs. Requires stable
     * owner access.
     */
    const InertialEdges& inertialEdges() const noexcept { return edges_; }
};

} // namespace openswmm::twoD

#endif // OPENSWMM_ENGINE_2D_EXPLICIT_INERTIAL_SOLVER_HPP
