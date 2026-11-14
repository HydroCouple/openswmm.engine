// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin

/**
 * @file      SourceWaterDriver.hpp
 * @brief     Advancement and bookkeeping of hydrological water sources.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_SOURCE_WATER_DRIVER_HPP
#define OPENSWMM_SOURCE_WATER_DRIVER_HPP
#include "SourceClockGroups.hpp"
#include "LID.hpp"
#include "surface/SourceSnow.hpp"
#include <array>
#include <memory>

namespace openswmm::detail {
class CompletedSourceArchive;
}

namespace openswmm::runoff {

struct SourceWaterLedger {
    double initial_storage = 0.0, rain = 0.0, evaporation = 0.0, infiltration = 0.0;
    /// Completed destination split, ft3; sum equals infiltration.
    double spatial_infiltration = 0.0, outside_infiltration = 0.0;
    double aquifer_return = 0.0; // cumulative paired surface credits, ft3, never rainfall
    double outfall_return = 0.0; // cumulative hydraulic receipts, ft3, never rainfall
    double outlet = 0.0, captured = 0.0, runon = 0.0, pervious_return = 0.0;
};
struct SourceWaterDelivery {
    int source = -1, node = -1;
    double start = 0.0, end = 0.0, volume = 0.0; // ft3; node -1 = unresolved external outlet
    bool drain = false;
    std::vector<double> mass; // pollutant concentration units times m3, paired with volume
};
/// Private completed source pollutant accounting, concentration units times
/// m3 (no kg/lb conversion). For LIDs, layers are surface/pavement/soil/storage;
/// type -1 denotes non-LID IMPERV0/IMPERV1/PERV pools (fourth slot unused).
/// Initial source water is clean; network INIT_CONC is not a source seed.
struct SourcePollutantLedger {
    int source = -1, type = -1, unit = -1;
    std::vector<std::array<double, 4>> layers;
    std::vector<double> rain, spatial_infiltration, outside_infiltration, outlet, removed;
    std::vector<double> surface_mass, drain_mass;       // last completed forcing segment
    std::vector<double> impervious_mass, pervious_mass; // non-LID outlet pools before capture
    std::vector<double> aquifer_return;                 // cumulative paired surface credits
    std::vector<double> pending_routing,
        incoming; // internal subarea queue; cumulative received transfers
};
struct SourceSurfaceReturn {
    int source = -1, type = -1, unit = -1, pool = 2; // non-LID subarea or exact LID unit
    double volume = 0;        // ft3, consumed once at the next source segment
    std::vector<double> mass; // pollutant concentration units times m3
};
struct SourceOutfallReturn {
    int source = -1;
    double available = 0, volume = 0; // accepted hydraulic seconds, ft3
};
enum class SourceEtKind { PERVIOUS, IMPERVIOUS, LID };
struct SourceAtmosphericLedger {
    SourceEtKind kind = SourceEtKind::PERVIOUS;
    int source = -1, type = -1, unit = -1;
    double area = 0.0; // ft2, physical footprint (capture does not enlarge it)
    bool soil_eligible = false;
    double potential = 0.0, evaporation = 0.0; // cumulative completed ft3
};

/** @brief Owned completed-source water/snow and conservative-pollutant trial driver.
 * Context and kernel histories are copied; callbacks are borrowed synchronously.
 * One owner serializes initialization, crediting, trials, reads and checkpoints.
 * Stage advances private runoff/LID/snow state; commit adopts it with its group
 * clock. Spatial receivers/hydraulic routing must settle separately. Liquid source
 * and solid/liquid snow balances are distinct and paired, not double-counted rain.
 * Qualified pollutants are conservative source-pool mixtures with drain removal;
 * no land-use buildup/washoff, lumped groundwater, age/heat/reactions or production
 * hot-start initialization is supplied. Private accepted-boundary restart requires
 * a matching model and publication with paired source/surface/engine owners. */
class SourceWaterDriver {
public:
    /** @brief Construct an uninitialized driver without source state or IO.
     * Most data accessors require successful initialize(); clocks() alone can be read. */
    SourceWaterDriver();
    /** @brief Release private accepted/trial histories without committing a pending trial.
     * Borrowed state/kernel/ledger references become invalid. Caller ends concurrent
     * access and owns any external callback/receiver rollback or persistence. */
    ~SourceWaterDriver();
    /** @brief Move owned accepted/trial histories and source clocks without copying water.
     * Transfers pending-group bookkeeping; caller releases prior borrows and does not
     * use moved-from data accessors until assigning a valid driver. No synchronization
     * or external receiver settlement is performed. */
    SourceWaterDriver(SourceWaterDriver&&) noexcept;
    /** @brief Replace this driver's histories by move, discarding any prior private trial.
     * @return This driver. Caller ends all prior borrows and paired transactions first;
     * no source/receiver commit or file output occurs. Moved-from state is not a usable
     * initialized model; noexcept relies on member ownership/container moves. */
    SourceWaterDriver& operator=(SourceWaterDriver&&) noexcept;
    /** @brief Copy committed water, kernels, ledgers and clocks for an independent trial.
     * @return Owned snapshot; LID state uses waterTrial() to avoid sharing report writers.
     * @throws std::logic_error Missing accepted state or an active water trial. Caller
     * must also ensure source clocks are fully committed, including after an exception.
     * Context value copies can retain external handle/callback references; do not use
     * those to publish through a trial. Copy/allocation failures propagate; original
     * accepted water is retained. Use the copy for multi-group/receiver coordination. */
    SourceWaterDriver waterTrial() const;
    /** @brief Stream private native model or accepted runtime source histories.
     * @param[in,out] archive Matching CompletedSourceArchive reader/writer at the paired
     * schema position; same-platform bytes, no standalone portable file format here.
     * @param model True streams model/forcing/membership/footprint/configuration identity;
     * false streams accepted water/snow/kernel/transfer/ledger/delivery state and optional
     * hydraulic-return profile. Requires initialized state and no active water trial.
     * @throws std::runtime_error Pending clocks, kernel/shape/profile, snow-clock, finite
     * water/mass tuple, atmospheric or conservation mismatch; archive errors also propagate.
     * Read mutates this object progressively and may fail after a prefix; restore an
     * isolated waterTrial/fresh matching model, finish containing archive validation,
     * then publish all paired owners together. A write may emit a prefix before a later
     * validation failure. Does not activate production restart or settle external receivers. */
    void checkpoint(detail::CompletedSourceArchive& archive,
                    bool model); // internal accepted-boundary IO
    /** @brief Synchronous LID bottom-capacity callback; caller-owned, never retained.
     * Receives LID type/group and unit indices plus start/end elapsed seconds;
     * returns a finite nonnegative accepted infiltration ceiling ft/s. It can be
     * called for each weather subdivision. External side effects are not rolled
     * back with private source trials. */
    using BottomCeiling = std::function<double(int type, int unit, double start, double end)>;
    /** @brief Synchronous transform of a private resolved forcing-segment copy.
     * Caller supplies completed-interval rates, preserves positive ordered start/
     * end/cohort indices and finite nonnegative rain/PET/factors. References must
     * not escape the callback. Does not change immutable native clock records;
     * caller-owned side effects are outside source rollback. */
    using ForcingTransform = std::function<void(SourceForcingInterval&)>;
    /** @brief Build private source kernels/ledgers from a simulation-start model snapshot.
     * @param context Compatible fully initialized model at current_time==elapsed_ms==0,
     * with aligned geometry/forcing/quality/name arrays and resolved runoff/LID outlets.
     * Selected sources cannot use lumped groundwater or preexisting runon/pervious-return
     * histories. Nonempty selection rejects water age, heat and reactions species;
     * snow-enabled nonempty groups require water-only even without assigned packs.
     * Conservative pollutants otherwise require finite nonnegative rain concentration, no decay/
     * co-pollutants/snow-only species or land uses. Snow delegates its own qualification.
     * @param seeds Valid subcatchment indices selecting entire weakly connected source
     * graphs through SourceClockGroups. Empty selection is allowed.
     * @param non_lid_areas Optional reviewed (source index,m2) overrides forwarded to
     * RunoffSolver::init instead of rounded project-area conversion.
     * @param initial_lids Optional borrowed matching initialized LID water snapshot;
     * group/usage order and area/source/control mappings must match. Copies water-only
     * history without retaining the pointer/report writers. Null builds fresh LID state.
     * @param hydraulic_returns Explicit accepted water-only outfall-return profile;
     * rejects active source pollutants and requires compatible positive receiving footprints.
     * @return Empty after publishing private state/clocks, otherwise refusal or caught
     * kernel-setup standard exception. Prior uninitialized driver remains uninitialized
     * on normal failure. Reinitialization/hot-start import is refused. Allocation/copy
     * failures before the local setup catch can propagate. Original context is not
     * mutated; no source interval/receiver is advanced. All source pools start with
     * clean solute, not network INIT_CONC; defaults inside fractions to one. */
    std::string initialize(const SimulationContext& context, const std::vector<int>& seeds,
                           const std::vector<std::pair<int, double>>& non_lid_areas = {},
                           const lid::LIDSolver* initial_lids = nullptr,
                           bool hydraulic_returns = false);
    /** @brief Bind uniform inside fractions once before any source advancement.
     * @param inside_fractions One finite [0,1] value per original subcatchment, including
     * unowned rows. Represents reviewed physical footprint shares, not a water debit.
     * @return Empty after copying/marking partitioned, otherwise shape/state/range refusal.
     * Requires initialized committed state, not already partitioned and all group clocks
     * zero without pending forcing. Allocation can throw; no source water is advanced.
     * Inside native-bottom LID areas later require an explicit spatial bottom ceiling. */
    std::string configureSpatialCoverage(const std::vector<double>& inside_fractions);
    /** @brief Borrow accepted per-source inside physical-area fractions.
     * @return Owned vector in original subcatchment order, initially all one; no trial
     * selector. Requires initialized state (logic_error otherwise). Init/commit/checkpoint/
     * move/destruction can invalidate elements. Serialize borrows with mutation. */
    const std::vector<double>& spatialFractions() const;
    /** @brief Validate and queue paired aquifer-return water/mass before source trials.
     * @param start Finite exact completed clock for each referenced source group.
     * @param returns Borrowed batch with owned source, originating non-LID pool (type=unit=-1,
     * pool in [0,2]) or exact LID type/unit and positive physical footprint. volume is
     * finite nonnegative ft3; mass has one finite nonnegative concentration*m3 value per
     * source pollutant, all zero for zero water. Entries are not deduplicated: caller
     * credits accepted receipts exactly once.
     * @return Empty after queue/ledger additions, otherwise refusal before mutation.
     * Requires initialized committed state and no pending referenced group interval.
     * All entries are validated first, but append/allocation failures during publication
     * can leave an earlier credited prefix; this method does not provide exception-level
     * atomicity or cumulative overflow checks. Use an isolated snapshot for a transaction.
     * Returns remain pending until the next selected source segment, never as rainfall. */
    std::string creditSurfaceReturns(double start, const std::vector<SourceSurfaceReturn>& returns);
    /** @brief Queue one explicitly accepted hydraulic-return interval exactly once.
     * @param start Finite elapsed seconds exactly equal outfallReturnEnd().
     * @param end Finite later accepted seconds within total simulation duration tolerance;
     * no source group may be pending or physically ahead of this endpoint.
     * @param volumes One finite nonnegative ft3 per original source; nonzero values require
     * an owned source with positive full non-LID+LID footprint. Unowned entries are zero.
     * @return Empty after preparing the whole queue, adding cumulative ledgers and
     * advancing return frontier; otherwise refusal without accepted-state mutation.
     * Requires initialized committed hydraulic-return profile and no water trial. Queue
     * copy/append exceptions propagate before publication. Consumed only from end onward
     * when each source group fires; does not immediately modify its physical water. */
    std::string creditOutfallReturns(double start, double end, const std::vector<double>& volumes);
    /** @brief Read whether explicit water-only hydraulic receipts were enabled.
     * @return Stored initialization flag; logic_error without accepted state. Does not
     * validate a specific receipt, trial or source/receiving clock. */
    bool acceptsOutfallReturns() const;
    /** @brief Read the last credited accepted hydraulic-return endpoint.
     * @return Elapsed seconds, zero initially; logic_error without accepted state.
     * Distinct from independently advancing source-group clocks and receiving bridge time. */
    double outfallReturnEnd() const;
    /** @brief Borrow accepted hydraulic receipts waiting for their source clock.
     * @return Queue of original source, availability seconds and ft3. No trial view;
     * requires accepted state. Credit/commit/checkpoint/move may invalidate elements.
     * Borrow only under serialized access and copy for a stable receiver snapshot. */
    const std::vector<SourceOutfallReturn>& pendingOutfallReturns() const;
    /** @brief Advance one source group into a private completed-interval trial.
     * @param group Valid initialized source-group index, no active water/pending clock trial.
     * @param end Finite requested elapsed-second endpoint after that group's completed_end.
     * @param completed_through Finite completed-forcing frontier at least end; bounded by
     * simulation interval through SourceClockGroups::stage(). Not a prediction horizon.
     * @param boundary Optional borrowed synchronous non-LID infiltration callback per
     * RunoffSolver::InfiltrationBoundary: source, ponded feet, seconds and output ft/s;
     * return true to use its reviewed capacity. Must not publish trial water externally.
     * @param bottom_ceiling Optional borrowed LID callback returning capacity ft/s for
     * type/unit/start/end. Required for selected inside native-bottom LIDs when partitioned.
     * @param forcing Optional borrowed transform on each private weather subdivision;
     * preserve interval/cohort/index validity and meaningful finite rates. Callback
     * references cannot escape; external effects are not part of source rollback.
     * @return Empty with a viewable unsettled trial, otherwise refusal/standard-kernel
     * exception text. Splits forcing at outfall availability and held snow boundaries,
     * advances runoff/LID/snow mixtures, then checks group water and conservative mass
     * balance at 1e-10 scaled tolerance. Caught standard failures cancel the water/clock
     * trial, retaining accepted water; nonstandard callback exceptions can leave a trial
     * for explicit cancel. Copy allocation occurs after clock staging but outside the
     * catch: failure can leave pending clock state without pending_group_, requiring
     * discard/restoration of the driver. Success requires commit after paired receiver
     * settlement or cancel before retry. Does not settle a live aquifer/engine itself. */
    std::string stage(int group, double end, double completed_through,
                      const RunoffSolver::InfiltrationBoundary* boundary = nullptr,
                      const BottomCeiling* bottom_ceiling = nullptr,
                      const ForcingTransform* forcing = nullptr);
    /** @brief Adopt staged private water and its source-group endpoint together.
     * @return Empty on success, otherwise refusal for no trial or failed clock commit.
     * Clock commits before nonthrowing state swap; old accepted state/trial is discarded,
     * invalidating borrowed kernel/ledger references. Caller first settles matching
     * receiving ownership. No live engine/surface receiver or report writer is advanced. */
    std::string commit();
    /** @brief Drop an active water trial and its tracked pending group forcing.
     * Retains accepted water/histories and does not undo caller callback/receiver effects.
     * Safe without a tracked trial; cannot repair an untracked clock left by a copy
     * exception before pending_group_ assignment in stage(). Invalidates trial borrows. */
    void cancel();
    /** @brief Borrow source membership, accepted endpoints and staged forcing clocks.
     * @return Owned clocks valid until destruction/move/replacement; stage/commit/cancel/
     * checkpoint can mutate contents or invalidate elements. Readable before initialize,
     * when empty. No direct mutable clock access or receiver settlement is supplied. */
    const SourceClockGroups& clocks() const { return clocks_; }
    /** @brief Borrow trial or accepted snow state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Accepted/trial SourceSnow solid/liquid pack owner and its separate ft3 balance.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const surface::SourceSnow& snow(bool trial = false) const;
    /** @brief Borrow trial or accepted context state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Private copied SimulationContext, including scratch source arrays; external handles in the copy remain borrowed.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const SimulationContext& context(bool trial = false) const;
    /** @brief Borrow trial or accepted runoff state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Private RunoffSolver subarea water/kernel histories in native feet/seconds.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const RunoffSolver& runoff(bool trial = false) const;
    /** @brief Borrow trial or accepted lids state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Private LIDSolver physical layer water and completed budgets; report writers are not part of water trials.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const lid::LIDSolver& lids(bool trial = false) const;
    /** @brief Borrow trial or accepted ledgers state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Per-original-source cumulative liquid water accounts in ft3, independent of the snow water ledger.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const std::vector<SourceWaterLedger>& ledgers(bool trial = false) const;
    /** @brief Borrow trial or accepted deliveries state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Append-order completed external outlet records: original interval seconds, ft3 and paired concentration*m3 amounts.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const std::vector<SourceWaterDelivery>& deliveries(bool trial = false) const;
    /** @brief Borrow trial or accepted atmosphere state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Physical footprint/eligibility rows with cumulative potential and actual atmospheric ft3.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const std::vector<SourceAtmosphericLedger>& atmosphere(bool trial = false) const;
    /** @brief Borrow ordered active source-pollutant identifiers from accepted state.
     * @return Names defining every source mass-vector stride, empty for water-only or
     * ignore-quality profile. logic_error without accepted state; commit/checkpoint/
     * move/destruction can invalidate references. Does not include network age/heat/MSX. */
    const std::vector<std::string>& pollutantNames() const;
    /** @brief Borrow trial or accepted pollutants state without copying it.
     * @param trial True selects only an active water trial; false accepted state.
     * @return Per-source/type/unit conservative pool amounts and completed transfers in concentration*m3.
     * @throws std::logic_error Requested state absent. No fallback from trial to accepted.
     * Reference/elements are owned by the selected state and can be invalidated by
     * commit/cancel/checkpoint/move/destruction. Serialize reads with mutation and copy
     * snapshots needed across state publication; never modify through a cast. */
    const std::vector<SourcePollutantLedger>& pollutants(bool trial = false) const;
    /** @brief Read pollutantBalanceResidual for one selected source-clock group.
     * @param group Zero-based group index, bounds checked through groups().at().
     * @param pollutant Zero-based source mass-vector index, checked with names.at().
     * @param trial True selects active private trial, false accepted state.
     * @return Cumulative rain+aquifer-return input minus infiltration, outlet, removal, stored pools and pending mass, in concentration*m3.
     * @throws std::logic_error Requested state absent; std::out_of_range invalid group
     * or pollutant index.
     * No normalization, tolerance test, finite-value repair or state mutation occurs.
     * Callers decide whether the residual is acceptable; trial reads do not commit it. */
    double pollutantBalanceResidual(int group, int pollutant, bool trial = false) const;
    /** @brief Read pendingPollutantMass for one selected source-clock group.
     * @param group Zero-based group index, bounds checked through groups().at().
     * @param pollutant Zero-based source mass-vector index, checked with names.at().
     * @param trial True selects active private trial, false accepted state.
     * @return Queued runon/pervious-return/internal-routing/aquifer-return mass in concentration*m3; excludes completed external deliveries.
     * @throws std::logic_error Requested state absent; std::out_of_range invalid group
     * or pollutant index.
     * No normalization, tolerance test, finite-value repair or state mutation occurs.
     * Callers decide whether the residual is acceptable; trial reads do not commit it. */
    double pendingPollutantMass(int group, int pollutant, bool trial = false) const;
    /** @brief Read pendingVolume for one selected source-clock group.
     * @param group Zero-based group index, bounds checked through groups().at().
     * @param trial True selects active private trial, false accepted state.
     * @return Queued runon, pervious return, internal subarea routing, aquifer and hydraulic-return ft3; excludes stored and external delivered water.
     * @throws std::logic_error Requested state absent; std::out_of_range invalid group
     * index.
     * No normalization, tolerance test, finite-value repair or state mutation occurs.
     * Callers decide whether the residual is acceptable; trial reads do not commit it. */
    double pendingVolume(int group, bool trial = false) const;
    /** @brief Sum accepted physical runoff/LID stored water for one source.
     * @param source Original subcatchment index; bounds checked against accepted ledger.
     * @return ft3 in liquid surface/LID stores, excluding solid/liquid snow, pending
     * runon/returns and external deliveries. logic_error before init, out_of_range for
     * invalid source. Does not require selected ownership or mutate state. */
    double storedVolume(int source) const;
    /** @brief Read balanceResidual for one selected source-clock group.
     * @param group Zero-based group index, bounds checked through groups().at().
     * @param trial True selects active private trial, false accepted state.
     * @return Initial liquid storage+rain+paired aquifer/hydraulic returns minus evaporation, infiltration, outlets, stored and pending water, in ft3. Snow has its own paired balance.
     * @throws std::logic_error Requested state absent; std::out_of_range invalid group
     * index.
     * No normalization, tolerance test, finite-value repair or state mutation occurs.
     * Callers decide whether the residual is acceptable; trial reads do not commit it. */
    double balanceResidual(int group, bool trial = false) const;

private:
    struct State;
    std::unique_ptr<State> state_, trial_;
    SourceClockGroups clocks_;
    int pending_group_ = -1;
    /** @brief Select a borrowed accepted or active private water state.
     * @param trial True selects trial_ exactly; false accepted state_. No fallback.
     * @return Owned state reference, invalidated by commit/cancel/move/destruction or
     * replacement. Caller serializes access.
     * @throws std::logic_error Selected state absent. */
    const State& view(bool trial) const;
    /** @brief Sum one private state's non-LID ponded and LID physical water.
     * @param state Compatible initialized state with aligned runoff/LID arrays.
     * @param source Valid original subcatchment index, unchecked here.
     * @return ft3 in weighted runoff subarea depths plus completedUnitStorage for matching
     * LIDs. Excludes snow and all pending/delivered water; no numeric validation/mutation. */
    double storage(const State& state, int source) const;
    /** @brief Mutate one private state over a resolved forcing segment without public commit.
     * @param[in,out] state Owned trial with compatible kernels, transfers and ledgers.
     * @param interval Positive finite elapsed-second interval, nonempty unique selected
     * source cohort with valid indices/month, finite nonnegative internal ft/s weather
     * and qualified factors. May come from a caller transform; structural validity is
     * not comprehensively rechecked here.
     * @param boundary Optional synchronous non-LID capacity callback forwarded to runoff.
     * @param ceiling Optional synchronous LID bottom capacity ft/s; required for selected
     * partitioned native-bottom inside area. Not retained.
     * Advances held snow, releases eligible outfall returns, consumes prior runon/paired
     * surface returns once, integrates runoff/LID stores, conservative layered mixtures
     * and atmospheric budgets, then queues connected runoff or completed external outlet
     * volumes. Capture shares cannot exceed non-LID outflow. Native water/area remain
     * ft/ft3/ft2; pollutant amounts are concentration*m3. Evaporation retains dry solute;
     * drain removal alone removes configured fractions. Throws on budget/layout/numeric
     * refusal after partial private mutation; stage owns discard/rollback. No live engine
     * publication, source-clock commit or report IO occurs here. */
    void advance(State& state, const SourceForcingInterval& interval,
                 const RunoffSolver::InfiltrationBoundary* boundary, const BottomCeiling* ceiling);
};
}
#endif
