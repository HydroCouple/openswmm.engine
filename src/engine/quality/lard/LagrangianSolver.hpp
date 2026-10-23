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
 * @file      LagrangianSolver.hpp
 * @brief     LARD (Lagrangian ARD) quality engine — X2: LTD transport core.
 *
 * @details Subplan X2 (`plans/transport/LARD_AGE_EXPEDITE_SUBPLAN_2026-08-23.md`;
 *          strategy `plans/LAGRANGIAN_QUALITY_STRATEGY.md` §2/§4, §16
 *          amendments D-L1/D-L2/D-L5 binding). X3a: `[OPTIONS]
 *          QUALITY_STEP` splits each routing step into equal transport
 *          substeps (strategy §4.2) — flows are frozen within the routing
 *          step, so refining dtq refines transport ALONE, which is what
 *          the dt-reference instrument leans on; `MAX_SEGMENTS_PER_LINK`
 *          sizes the slabs. Both keys warn when set under other engines.
 *
 *          Step orchestration (§4.2, trimmed to X2 scope):
 *            0. flow-reversal detection → ring reversal + topo invalidation
 *            1. DRAIN — every conduit sends |Q|·dt of back-end segment water
 *               (and its mass) to its downstream node's inflow ledger; a
 *               link whose new volume is below the remainder sheds the
 *               difference through its FRONT to the upstream ledger, so
 *               segment volume always sums exactly to `links.volume` and
 *               the volume change is booked, never rescaled away (the E2
 *               unbooked-resync family).
 *            2. MIX — nodes in flow-aware topological order (Kahn; cycles
 *               broken in index order, their residue carried in the ledger
 *               to the next step rather than dropped): CSTR over the node's
 *               own old volume — junctions, dividers, storages (CMSTR) and
 *               outfalls all reduce to the same formula. External loads
 *               join here from the SHARED loader seam (`qual_mass_in` rate ×
 *               dt, `qual_vol_in` volume — assembled by
 *               `QualitySolver::assembleExternalLoads`, the ARD precedent).
 *               Zero-volume links (pump/orifice/weir/outlet) pass the node's
 *               NEW concentration through in the same step — the reason the
 *               order is topological (§4.3, Davis et al.).
 *            3. RELEASE — each conduit gains a front segment at its upstream
 *               node's new concentration, sized so the slab total equals
 *               `links.volume` exactly; §4.5 merge tolerance collapses plug
 *               flow.
 *            4. DECAY — exact-exponential kdecay on segments and node
 *               stores (species-major stripes), booked to
 *               `qual_routing_reacted`.
 *            5. PUBLISH — `links.conc` = volume-weighted segment mean (so
 *               engine-side final-storage and outfall bookings are exact),
 *               then `conc_old = conc` (the ARD convention).
 *
 *          X4: water age rides the segments as species row `np` — exact
 *          aging (+dt on every live parcel and node store BEFORE
 *          transport, the routeLegacyAge convention), volume-weighted
 *          mixing through the same drain/mix/release phases, sources from
 *          `node_age_vol_in` (the D-UT10 parallel accumulator, all seven
 *          loader pathways), state published to `water_age_state`
 *          (seconds), no decay on the age row, and the dry-link state
 *          keeps aging (the A2b state/report separation).
 *
 *          X3b: `[OPTIONS] DISPERSION RWPT` activates resolved
 *          vertical-shear dispersion on the segments (RwptDispersion.hpp,
 *          D-X3b1: particles estimate inter-segment exchange, carry no
 *          mass themselves), keyed by the deterministic `RWPT_SEED`
 *          (D-L6). Runs on the substep's final field, conduits only.
 *
 *          Deliberately NOT here: reactions module binding (deferred L3),
 *          heat (H7 — does not advance under this dispatch and
 *          the open() warning says so), treatment
 *          interop (warned bypass), storage mixing models beyond CMSTR,
 *          the legacy evaporation up-concentration factor (recorded
 *          deviation — steady gates cannot see it; parity work owns it),
 *          D-NS1's clamp counter/warning (the max(0,·) floor is here, but
 *          its observer — a negative source reaching a node — only exists
 *          once X6 lands negative loads in the shared loaders).
 *
 *          Header-only for the same reason X1 was: the engine source glob
 *          lacks CONFIGURE_DEPENDS, and a patch-applied .cpp silently does
 *          not compile.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
#define OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstdint>
#include <vector>
#include "../../core/CompletedSourceArchive.hpp"

#include "../../core/SimulationContext.hpp"
#include "../../transport/InitialQualitySeeds.hpp"
#include "../NegativeSources.hpp"
#include "../QualityRouting.hpp"
#include "../../transport/components/ReactionModule/ReactionLegacyBinding.hpp"
#include "../../transport/components/ReactionModule/ReactionGeometry.hpp"
#include "../../transport/TransportPolicy.hpp" // E2
#include "RwptDispersion.hpp"
#include "SegmentStore.hpp"
#include "../../transport/components/ReactionModule/ReactionSources.hpp"
#include "../../core/UnitConversion.hpp"
#include "../../hydraulics/Node.hpp"
#include "../../transport/components/HeatFluxModules/BedExchange.hpp"
#include "../../transport/components/HeatFluxModules/HeatFluxes.hpp"
#include "../../transport/components/HeatFluxModules/HeatOverrides.hpp"
#include "../../transport/components/HeatFluxModules/SolarRadiation.hpp"
#include "../../transport/components/HeatFluxModules/SurfaceExchange.hpp"
#include "../QualityRouting.hpp"

namespace openswmm {
namespace lard {

constexpr double kTinyFlow = 1.0e-8; ///< cfs; below this a link moves nothing

/**
 * @brief The segment store's species-row layout, computed in ONE place.
 *
 * @details H7a. Three call sites (`step`, `substep`, `init`) each used to recompute `np + (age ?
 * 1 : 0)` independently, and the age row was identified by the predicate `s >= np`. That
 * predicate is only correct while age is the **one and only** reserved row: the moment a second
 * one exists (temperature, H7b) `s >= np` captures both, and temperature would be silently aged,
 * sourced from `node_age_vol_in` and published to `water_age_state` — a defect that produces
 * plausible numbers rather than a crash.
 *
 * So the row identity becomes an explicit INDEX rather than a threshold, mirroring what the ARD
 * engine already does (`ArdEngine.cpp` `age_row_` / `temp_row_`). LARD is the engine that kept
 * the threshold because it only ever had one.
 *
 * **This struct is deliberately introduced while `temp_row` is always -1**, so the change is
 * provably inert: with one reserved row, `s == age_row` and `s >= np` select exactly the same
 * row. The corpus and the existing water-age gates are the proof. H7b then adds the temperature
 * row on top of a layout that is already trusted, rather than changing the indexing and the
 * physics in one step where a bit-identity check can no longer separate them.
 */
struct SpeciesRowLayout {
    int np = 0;         ///< pollutant rows occupy [0, np)
    int age_row = -1;   ///< water-age row index, or -1 when absent
    int temp_row = -1;  ///< temperature row index, or -1 (H7b)
    int msx_first = -1; ///< first MSX species row, or -1 (L3);
                        ///< species s rides row msx_first + s
    int ns = 0;         ///< total rows the store carries
};

/**
 * @brief The single source of truth for the row layout. Reserved rows are appended after the
 * pollutants in a fixed order (age, then temperature) so an index means the same thing
 * everywhere.
 *
 * @details Resolve the carried network1D row order for the LARD owner.
 * @param[in] ctx Stable transport class policy and active legacy-reaction profile.
 * @return Pollutants first, optional age seconds, temperature degC, then enabled native MSX
 * species; absent row selectors=-1, ns is total. No state/clock change, allocation or shape
 * preparation.
 */
inline SpeciesRowLayout rowLayout(const SimulationContext& ctx) {
    // E2: the class ENABLES come from the one policy
    // (transport::network1DEnables — allocation-free, this runs per step).
    // Identical to the pre-E2 reads: LARD only runs with IGNORE_QUALITY NO,
    // where the policy's gate is a no-op. The row ORDER below is deliberately
    // LARD's own (pollutants, age, temperature, MSX) — see the struct note;
    // E2 unifies the decision, not the index arithmetic.
    const transport::ClassEnables en = transport::network1DEnables(ctx);
    SpeciesRowLayout L;
    L.np = en.n_pollut;
    L.ns = L.np;
    if (en.age)
        L.age_row = L.ns++;
    if (en.temperature)
        L.temp_row = L.ns++; // H7b
    // L3: MSX species ride the segments after the reserved rows —
    // stable across the run (the compiled species table is fixed at
    // open). n_msx == 0 leaves every index above IDENTICAL to H7b's
    // layout, which is the bit-inertness claim the corpus checks.
    if (transport::legacyReactionsActive(ctx)) {
        L.msx_first = L.ns;
        L.ns += en.n_msx;
    }
    return L;
}

class LagrangianSolver {
public:
    /**
     * @brief Export a validated completed-source snapshot of solver-owned
     * segment/particle/topology state.
     * @return Owning archive bytes, including initialized flag; excludes SimulationContext
     * inventories, clock, options and scratch. Retain matching completed context and
     * configuration separately.
     * @note Copies this owner, then archives/validates copy; allocation/archive/validation
     * exceptions propagate without original solver mutation. Caller establishes a stable
     * exclusive checkpoint barrier.
     */
    std::string checkpointState() const {
        auto copy = *this;
        detail::CompletedSourceArchive a;
        copy.checkpoint(a);
        return a.bytes();
    }
    /**
     * @brief Restore solver-owned completed-source state against a matching prepared context.
     * @param[in] bytes Complete archive from checkpointState with compatible
     * dimensions/options/layout.
     * @param[in,out] ctx Matching completed SimulationContext; ready snapshot calls init(ctx),
     * which may seed scalar/native context state before archive fields are read.
     * @throws std::runtime_error Archive/trailing-byte, topology, segment or particle validation
     * failure; allocation/dependency exceptions propagate. Partial owner/context changes are
     * retained on failure.
     * @note Archives do not contain context inventories/configuration. Exclusive lifecycle, not
     * transactional; false-ready snapshot skips init and checkpointFields uses this owner's
     * current initialized flag, so caller must restore into the intended matching initialization
     * phase.
     */
    void restoreCheckpointState(const std::string& bytes, SimulationContext& ctx) {
        detail::CompletedSourceArchive a(bytes);
        bool ready = false;
        a(ready);
        if (ready)
            init(ctx);
        checkpointFields(a);
        a.finish();
    }
    /**
     * @brief Read whether this segment owner completed initialization.
     * @return Initialized flag only; does not validate current context/layout or prepare state.
     * No allocation/mutation.
     */
    bool initialized() const noexcept { return initialized_; }
    /**
     * @brief Export one mean-temperature channel region per wet represented link.
     * @param[in] ctx Stable matching initialized row policy, native geometry and heat/contact
     * configuration.
     * @return Owning regions with link identity, region0/full-link interval, volume m3 and mean
     * degC plus resolved thermal geometry. Empty when uninitialized or temperature row absent;
     * zero-volume links skip.
     * @note Read-only owner; context remains stable through geometry queries. Allocations may
     * throw; returned records own values and need no owner lifetime, but later
     * state/configuration changes make them stale.
     */
    std::vector<ChannelThermalRegion> thermalRegions(const SimulationContext& ctx) const {
        std::vector<ChannelThermalRegion> out;
        const auto layout = rowLayout(ctx);
        if (!initialized_ || layout.temp_row < 0)
            return out;
        std::vector<double> mean(layout.ns);
        for (int link = 0; link < ctx.n_links(); ++link) {
            const double volume = store_.total_volume(link) * .028316846592;
            if (!(volume > 0))
                continue;
            store_.mean_conc(link, mean.data());
            out.push_back({link, 0, 0, 1, volume, mean[layout.temp_row]});
            transport::heat::fillChannelThermalGeometry(ctx, out.back());
        }
        return out;
    }
    /**
     * @brief Export mean carried-species tuples for each wet represented channel link.
     * @param[in] ctx Stable matching row layout/names and native completed hydraulic geometry.
     * @return Owning region0 records, volume m3 and row-order mean values: pollutant/native
     * declared units, age seconds, temperature degC. Empty when uninitialized OR temperature row
     * absent, even with species.
     * @note Requires configured names/extents for all rows. No state mutation, ledger or clock
     * advance; allocation can throw. Returned copied records are phase snapshots, not live
     * borrowed views.
     */
    std::vector<ChannelSpeciesRegion> speciesRegions(const SimulationContext& ctx) const {
        std::vector<ChannelSpeciesRegion> out;
        const auto layout = rowLayout(ctx);
        if (!initialized_ || layout.temp_row < 0)
            return out;
        std::vector<double> mean(layout.ns);
        for (int link = 0; link < ctx.n_links(); ++link) {
            const double volume = store_.total_volume(link) * .028316846592;
            if (!(volume > 0))
                continue;
            store_.mean_conc(link, mean.data());
            ChannelSpeciesRegion region;
            region.link = link;
            region.region = 0;
            region.volume = volume;
            for (int p = 0; p < layout.np; ++p)
                region.row_names.push_back(ctx.pollutant_names.name_of(p));
            if (layout.age_row >= 0)
                region.row_names.emplace_back("__WATER_AGE__");
            if (layout.temp_row >= 0)
                region.row_names.emplace_back("__TEMPERATURE__");
            if (layout.msx_first >= 0)
                for (int row = layout.msx_first; row < layout.ns; ++row)
                    region.row_names.push_back(
                        ctx.reactions.species_name[std::size_t(row - layout.msx_first)]);
            region.concentration = mean;
            out.push_back(std::move(region));
        }
        return out;
    }
    /**
     * @brief Apply caller concentration deltas to represented nonthermal channel rows then
     * republish.
     * @param[in,out] ctx Matching initialized row policy/public quality stores; publish(ctx,0)
     * refreshes link means and copies node/link pollutant current values into old histories.
     * @param[in] updates Owning stable region0 deltas, one total-row tuple per valid link;
     * temperature delta must be exactly0. Positive existing mean scales each segment
     * proportionally, otherwise sets target mean.
     * @return Empty on success, descriptive validation message for unavailable state, region/row
     * mismatch, nonzero thermal delta or nonfinite/negative candidate; validation precedes
     * mutation of all entries.
     * @note Repeated updates to same link validate independently against pre-update state, then
     * apply sequentially to changed state, so caller must qualify duplicate/aggregate deltas.
     * Allocation/publish can throw after earlier writes. No external mass ledger or
     * physical/clock booking; exclusive coupling phase.
     */
    std::string applySpeciesIncrements(SimulationContext& ctx,
                                       const std::vector<ChannelSpeciesIncrement>& updates) {
        const auto layout = rowLayout(ctx);
        if (!initialized_ || layout.temp_row < 0)
            return "LARD channel species state is unavailable.";
        for (const auto& u : updates) {
            if (u.link < 0 || u.link >= ctx.n_links() || u.region != 0 ||
                u.concentration_delta.size() != std::size_t(layout.ns))
                return "LARD species increment has an invalid channel region or row count.";
            if (u.concentration_delta[layout.temp_row] != 0)
                return "LARD species update cannot modify the temperature row.";
            std::vector<double> mean(layout.ns);
            store_.mean_conc(u.link, mean.data());
            for (int k = 0; k < store_.count(u.link); ++k)
                for (int row = 0; row < layout.ns; ++row) {
                    const double delta = u.concentration_delta[row];
                    const double target = mean[row] + delta;
                    const double next = mean[row] > 0
                                            ? store_.seg_conc(u.link, k, row) * target / mean[row]
                                            : target;
                    if (!std::isfinite(delta) || !std::isfinite(next) ||
                        (row != layout.temp_row && next < 0))
                        return "LARD species increment is nonfinite or makes a carried mass negative.";
                }
        }
        // A channel region is a whole reach in LARD. Apply the finite-contact
        // change proportionally to its current segment concentrations, which
        // preserves their pattern and keeps every segment nonnegative. The
        // scaling also makes an inverse rollback restore the original state.
        for (const auto& u : updates) {
            std::vector<double> mean(layout.ns);
            store_.mean_conc(u.link, mean.data());
            for (int k = 0; k < store_.count(u.link); ++k)
                for (int row = 0; row < layout.ns; ++row)
                    if (row != layout.temp_row) {
                        const double target = mean[row] + u.concentration_delta[row];
                        const double next =
                            mean[row] > 0 ? store_.seg_conc(u.link, k, row) * target / mean[row]
                                          : target;
                        store_.set_seg_conc(u.link, k, row, next);
                    }
        }
        publish(ctx, 0);
        return {};
    }
    /**
     * @brief Add caller channel temperature increments uniformly to represented segments then
     * republish.
     * @param[in,out] ctx Matching initialized thermal/public quality state; publish(ctx,0) also
     * refreshes pollutant old histories, not just temperature.
     * @param[in] updates Stable valid link/region0 records whose celsius field is a signed degC
     * increment.
     * @return Empty on success; message for unavailable owner, invalid identity or nonfinite
     * candidate. All entries are prevalidated independently before writes; repeated-link
     * aggregate overflow is not revalidated after each increment. Allocation/publish may throw
     * after writes; no rollback, ledger, water or clock mutation. Caller owns exclusive
     * completed coupling phase and qualified absolute temperatures.
     */
    std::string applyThermalIncrements(SimulationContext& ctx,
                                       const std::vector<ChannelThermalIncrement>& updates) {
        const auto layout = rowLayout(ctx);
        if (!initialized_ || layout.temp_row < 0)
            return "LARD channel thermal state is unavailable.";
        for (const auto& u : updates) {
            if (u.link < 0 || u.link >= ctx.n_links() || u.region != 0 || !std::isfinite(u.celsius))
                return "LARD thermal increment has an invalid channel region.";
            for (int k = 0; k < store_.count(u.link); ++k)
                if (!std::isfinite(store_.seg_conc(u.link, k, layout.temp_row) + u.celsius))
                    return "LARD thermal increment is nonfinite.";
        }
        for (const auto& u : updates)
            for (int k = 0; k < store_.count(u.link); ++k)
                store_.set_seg_conc(u.link, k, layout.temp_row,
                                    store_.seg_conc(u.link, k, layout.temp_row) + u.celsius);
        publish(ctx, 0);
        return {};
    }
    /**
     * @brief One routing step of LTD transport. Lazily initializes on the first call (needs
     * router-set volumes, the ARD precedent).
     *
     * @details Advance segment advection/mixing/kinetics with optional RWPT and final heat/bed
     * exchange.
     * @param[in,out] ctx Prepared completed hydraulics and freshly assembled external
     * quality/scalar rates; updates node/link family concentrations, histories, HRT,
     * reaction/negative-source and heat/bed state.
     * @param[in] dt_routing Positive finite accepted hydraulic interval s, not validated here.
     * @note No carried rows returns. Lazily initializes; reverses conduit segment order on
     * meaningful flow-sign flips, recomputes directed topology, subdivides by positive
     * quality_step, then applies heat/bed once and publishes. Caller keeps owner
     * dimensions/layout/options stable after init and assembles source rates once for whole
     * interval. Internal routines allocate/mutate progressively, exceptions retain partial work.
     * No hydraulic water/engine clock advance or concurrent owner use.
     */
    void step(SimulationContext& ctx, double dt_routing) {
        // X4/H7b: the age and temperature rows ride the segments after
        // the pollutants — age published to water_age_state (seconds),
        // temperature to heat_state (degC) — rather than the np-strided
        // conc arrays.
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const int ns = L.ns;
        if (ns <= 0)
            return;
        if (!initialized_)
            init(ctx);

        const int nl = ctx.n_links();
        auto& links = ctx.links;

        // ---- 0. Flow reversal (§4.4) — once per routing step: the flow
        //      solution is constant within it. -----------------------------
        bool topo_dirty = false;
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            const double q = links.flow[ul];
            if (std::abs(q) <= kTinyFlow)
                continue;
            const std::int8_t sign = (q >= 0.0) ? 1 : -1;
            if (sign != flow_sign_[ul]) {
                if (links.type[ul] == LinkType::CONDUIT)
                    store_.reverse(l);
                flow_sign_[ul] = sign;
                topo_dirty = true;
            }
        }
        if (topo_dirty || topo_.empty())
            computeTopoOrder(ctx);

        // ---- X3a: QUALITY_STEP substepping (strategy §4.2). Equal
        //      substeps; mass/age external loads are RATES and scale
        //      through dt, the per-routing-step external VOLUME
        //      (qual_vol_in) scales through frac. dtq absent or >= the
        //      routing step degenerates to one substep — bit-identical to
        //      the pre-X3a engine by construction.
        const double dtq = ctx.options.quality_step;
        const int nsub =
            (dtq > 0.0 && dtq < dt_routing) ? static_cast<int>(std::ceil(dt_routing / dtq)) : 1;
        const double dt = dt_routing / static_cast<double>(nsub);
        const double frac = 1.0 / static_cast<double>(nsub);
        for (int sub = 0; sub < nsub; ++sub)
            substep(ctx, dt, frac);

        // H2/H3/H6b under LARD: surface + radiative fluxes and the bed
        // pair, once per routing step (the ARD slot), applied to the
        // segment field before publication.
        applyFluxesAndBed(ctx, dt_routing);

        publish(ctx, dt_routing);
    }

    /**
     * @brief One LTD substep: AGE → DRAIN → MIX(+passthrough) → RELEASE → DECAY.
     *
     * @param frac  fraction of the per-routing-step external volume (`qual_vol_in`) this substep
     * consumes.
     *
     * @details Route and react one LARD quality subinterval against held hydraulic state.
     * @param[in,out] ctx Prepared node/link quality, age/heat/native arrays and external-rate
     * assembly; receives node/segment mixing, pollutant decay, treatment/HRT/native kinetics and
     * diagnostics.
     * @param[in] dt Positive finite subinterval s.
     * - Additional argument frac: Positive hydraulic interval fraction used on already
     * integrated qual_vol_in; external mass/scalar rates use dt independently, so fractions
     * across routing step must sum1.
     * @note Requires init, matching row policy, fresh topology and receipts. Ages all
     * segments/nodes, drains conduit downstream/front excess, mixes nodes in order, forwards
     * nonconduit loads, refills conduits, optionally disperses particles then decays/reacts.
     * Pending cyclic graph receipts remain for next traversal. Not a standalone source assembler
     * or publisher; no water/clock advance. Shared scratch, allocation, warning and integration
     * can leave partial stores/ledgers on exception; exclusive once-per-subinterval chronology
     * and stable hydraulic/source owner required.
     */
    void substep(SimulationContext& ctx, double dt, double frac) {
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const int ns = L.ns;
        // Derived from the layout rather than re-read from options, so the
        // "is there a reserved row" question has exactly one answer per call.
        const bool age = (L.age_row >= 0);
        const bool heat = (L.temp_row >= 0);
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        auto& nodes = ctx.nodes;
        auto& links = ctx.links;
        auto& ws = ctx.water_age_state;
        auto& hs = ctx.heat_state;

        // ---- AGE (before transport): every parcel ages by exactly dt, and
        //      the aged value is this substep's "old" state — the plan §1
        //      convention routeLegacyAge follows (age then mix). ------------
        if (age) {
            store_.add_species(L.age_row, dt);
            for (int n = 0; n < nn; ++n)
                ws.node_age[static_cast<std::size_t>(n)] += dt;
        }

        // ---- 1. DRAIN (all conduits, before any node mixes) ---------------
        scratch_.assign(static_cast<std::size_t>(ns), 0.0);
        treat_cin_.assign(static_cast<std::size_t>(np > 0 ? np : 0), 0.0);
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT)
                continue;
            const double q = std::abs(links.flow[ul]);
            const int dn = downstreamNode(ctx, l);
            const int up = upstreamNode(ctx, l);

            if (q > kTinyFlow && dn >= 0) {
                std::fill(scratch_.begin(), scratch_.end(), 0.0);
                const double drained = store_.drain_back(l, q * dt, scratch_.data());
                addToLedger(dn, drained, scratch_.data(), ns);
            }
            // Volume reconciliation: the slab must sum to links.volume.
            // A shortfall is filled at RELEASE with upstream water; an
            // excess left after the outflow drain leaves through the FRONT
            // to the upstream ledger — booked, not rescaled (see header).
            const double v_new = links.volume[ul];
            const double v_rem = store_.total_volume(l);
            if (v_rem > v_new && up >= 0) {
                std::fill(scratch_.begin(), scratch_.end(), 0.0);
                const double shed = store_.drain_front(l, v_rem - v_new, scratch_.data());
                addToLedger(up, shed, scratch_.data(), ns);
            }
        }

        // ---- 2. MIX in topo order, passthrough zero-volume links ----------
        for (const int n : topo_) {
            const auto un = static_cast<std::size_t>(n);
            // The node's mixed STORE (node::storeVolume): the routers book
            // nodes.volume in legacy's convention, 0 for a junction below
            // its rim, but the store this solver mixes into is the water
            // the continuity equation attributes to the node.
            const double v_old =
                node::storeVolume(nodes, n, nodes.old_depth[un], nodes.old_volume[un]);
            const double v_in = node_vol_in_[un];
            // OUTFALL_BACKFLOW_QUALITY ZERO: an outfall taking no volume
            // inflow this substep is a fresh boundary — its held state
            // (pollutant rows AND the age row) reads zero, so the RELEASE
            // seeding and zero-volume passthrough below draw clean water.
            const bool zero_bf = ctx.options.outfall_backflow_zero &&
                                 nodes.type[un] == NodeType::OUTFALL &&
                                 v_in + nodes.qual_vol_in[un] * frac <= 0.0;

            for (int s = 0; s < ns; ++s) {
                // H7a: identity by INDEX, not by threshold. `s >= np` was
                // correct only while age was the sole reserved row; it would
                // silently capture the temperature row too (H7b).
                const bool is_age = (s == L.age_row);
                const bool is_temp = (s == L.temp_row);
                // L3: species rows — state lives in msx_node_conc
                // ([node*nsp + sp]); U2 gave them the [INFLOWS] pathway
                // (msx_ext_mass_in, same index).
                const bool is_msx = (L.msx_first >= 0 && s >= L.msx_first);
                const auto xi = is_msx ? un * static_cast<std::size_t>(ns - L.msx_first) +
                                             static_cast<std::size_t>(s - L.msx_first)
                                       : 0;
                const auto li =
                    un * static_cast<std::size_t>(ns) + static_cast<std::size_t>(s); // ledger index
                // State and external load per row. Pollutants: nodes.conc +
                // qual_mass_in (rate × dt — the mixAtNodes convention). Age:
                // water_age_state.node_age (already aged +dt this step) +
                // node_age_vol_in (age·ft³/s rate, the D-UT10 parallel
                // accumulator filled by all seven loader pathways).
                const auto ci = un * static_cast<std::size_t>(np) + static_cast<std::size_t>(s);
                // Temperature mirrors age one row over: state in
                // heat_state.node_temp (degC), external load from
                // node_temp_vol_in (degC.ft3/s, the D-UT10 twin filled by
                // the same seven loaders) -- H1's convention, consumed here
                // instead of by routeLegacyHeat.
                const double st_old = is_age    ? ws.node_age[un]
                                      : is_temp ? hs.node_temp[un]
                                      : is_msx  ? ctx.reactions.msx_node_conc[xi]
                                                : nodes.conc[ci];
                // U2: species rows take the [INFLOWS] species loads
                // (msx_ext_mass_in, a rate; empty when no such row).
                const double m_ext = is_age    ? ws.node_age_vol_in[un] * dt
                                     : is_temp ? hs.node_temp_vol_in[un] * dt
                                     : is_msx  ? ((xi < ctx.reactions.msx_ext_mass_in.size())
                                                      ? ctx.reactions.msx_ext_mass_in[xi] * dt
                                                      : 0.0)
                                               : nodes.qual_mass_in[ci] * dt;
                double m = st_old * v_old + node_mass_in_[li] + m_ext;
                // P2.3: stash the ARRIVING mass for the treatment cin below
                // (pollutant rows only; s == p for rows 0..np-1).
                if (!is_age && !is_temp && !is_msx && s < static_cast<int>(treat_cin_.size()))
                    treat_cin_[static_cast<std::size_t>(s)] = node_mass_in_[li] + m_ext;
                // D-NS1 (X6, now observable): extraction beyond the
                // store's mass clamps to available — counted, warned
                // once, and (pollutant rows) un-booked so the ledger
                // carries what actually left.
                // The non-negativity clamp does NOT apply to temperature:
                // degC water below zero is an ordinary state (the freezing
                // gate in heat_watershed exists to keep it so), where a
                // negative mass or age is a defect. Same reasoning as the
                // report boundary's deliberate no-mask on temperature.
                if (m < 0.0 && !is_temp) {
                    if (is_age)
                        quality::bookNegativeAgeClamp(ctx, n);
                    else if (!is_msx) // L3: species have no ledger row
                        quality::bookNegativeSourceClamp(ctx, n, s, -m);
                    m = 0.0;
                }
                const double denom = v_old + v_in + nodes.qual_vol_in[un] * frac;
                // ALWAYS divide by the full denominator. m/denom is a convex
                // combination of st_old and the arriving values, so it can
                // never exceed its inputs; the fallback this replaces
                // divided a mass that included st_old*v_old by a divisor
                // that EXCLUDED v_old, and at a nearly-dry junction that
                // quotient amplified step over step -- measured on a
                // receding-flow deck (inflow stops at 1 h): node
                // concentrations reached 2.7e30, 2.0e281, then inf, and the
                // final-storage row went NaN. Below 1e-12 ft^3 there is no
                // meaningful water and the store keeps its value.
                if (is_msx && !zero_bf)
                    m += transport::reactionBoosterMass(ctx, n, s - L.msx_first, m, denom,
                                                        v_in + nodes.qual_vol_in[un] * frac);
                const double st_new = zero_bf ? 0.0 : ((denom > 1.0e-12) ? m / denom : st_old);
                if (is_age)
                    ws.node_age[un] = st_new;
                else if (is_temp)
                    hs.node_temp[un] = st_new;
                else if (is_msx)
                    ctx.reactions.msx_node_conc[xi] = st_new;
                else
                    nodes.conc[ci] = st_new;
                node_mass_in_[li] = 0.0; // consumed; cycle residue carries
            }
            node_vol_in_[un] = 0.0;

            // P2.3: [TREATMENT] applies HERE — to the freshly mixed node
            // state, BEFORE the passthrough and RELEASE below draw it: the
            // LEGACY ordering (mix → treat → links), transplanted. The
            // handoff's end-of-step application outside the solver was a
            // no-op twice over: the evaluator's cin read the LEGACY
            // accumulators (external loads only — 0 at an interior node,
            // and an R-typed expression keeps c_node when cin reads 0),
            // and a junction's ~zero stored volume gave the late write no
            // weight in the next mix. Fed with THIS substep's inflow
            // figures instead; nodes with no treatment pay one flag test.
            if (np > 0 && dt > 0.0 && un < ctx.treatment.has_treatment.size() &&
                ctx.treatment.has_treatment[un]) {
                const double v_in_total = v_in + nodes.qual_vol_in[un] * frac;
                for (int p = 0; p < np; ++p)
                    treat_cin_[static_cast<std::size_t>(p)] =
                        (v_in_total > 1.0e-12)
                            ? std::max(0.0, treat_cin_[static_cast<std::size_t>(p)]) / v_in_total
                            : 0.0;
                quality::applyNodeTreatment(ctx, n, dt, v_in_total / dt, treat_cin_.data());
            }

            // Zero-volume passthrough (§2.4): outgoing pump/orifice/weir/
            // outlet links deliver the node's NEW concentration downstream
            // within this step — the property the topo order exists for.
            for (const int l : node_out_links_[un]) {
                const auto ul = static_cast<std::size_t>(l);
                if (links.type[ul] == LinkType::CONDUIT)
                    continue;
                const double q = std::abs(links.flow[ul]);
                if (q <= kTinyFlow)
                    continue;
                const int dn = downstreamNode(ctx, l);
                if (dn < 0)
                    continue;
                for (int p = 0; p < np; ++p) {
                    const auto ni = un * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                    const auto lp = ul * static_cast<std::size_t>(np) + static_cast<std::size_t>(p);
                    scratch_[static_cast<std::size_t>(p)] = nodes.conc[ni];
                    links.conc[lp] = nodes.conc[ni];
                }
                if (age) {
                    scratch_[static_cast<std::size_t>(L.age_row)] = ws.node_age[un];
                    ws.link_age[ul] = ws.node_age[un];
                }
                if (heat) {
                    scratch_[static_cast<std::size_t>(L.temp_row)] = hs.node_temp[un];
                    hs.link_temp[ul] = hs.node_temp[un];
                }
                if (L.msx_first >= 0) { // L3
                    const int nsp = ns - L.msx_first;
                    for (int sp = 0; sp < nsp; ++sp) {
                        const double c =
                            ctx.reactions.msx_node_conc[un * static_cast<std::size_t>(nsp) +
                                                        static_cast<std::size_t>(sp)];
                        scratch_[static_cast<std::size_t>(L.msx_first + sp)] = c;
                        ctx.reactions.msx_link_conc[ul * static_cast<std::size_t>(nsp) +
                                                    static_cast<std::size_t>(sp)] = c;
                    }
                }
                addToLedgerRate(dn, q * dt, scratch_.data(), ns);
            }
        }

        // ---- 3. RELEASE new front segments --------------------------------
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT)
                continue;
            const int up = upstreamNode(ctx, l);
            const double need = links.volume[ul] - store_.total_volume(l);
            release_vol_[ul] = (need > 0.0) ? need : 0.0; // X3b: RWPT's V_in
            if (need <= 0.0 || up < 0)
                continue;
            const auto uu = static_cast<std::size_t>(up);
            for (int p = 0; p < np; ++p)
                scratch_[static_cast<std::size_t>(p)] =
                    nodes.conc[uu * static_cast<std::size_t>(np) + static_cast<std::size_t>(p)];
            if (age)
                scratch_[static_cast<std::size_t>(L.age_row)] = ws.node_age[uu];
            if (heat)
                scratch_[static_cast<std::size_t>(L.temp_row)] = hs.node_temp[uu];
            if (L.msx_first >= 0) { // L3
                const int nsp = ns - L.msx_first;
                for (int sp = 0; sp < nsp; ++sp)
                    scratch_[static_cast<std::size_t>(L.msx_first + sp)] =
                        ctx.reactions.msx_node_conc[uu * static_cast<std::size_t>(nsp) +
                                                    static_cast<std::size_t>(sp)];
            }
            store_.push_front(l, need, scratch_.data());
        }

        // ---- 3b. RWPT dispersion (X3b) — on the substep's FINAL segment
        //      field, per link, resolved vertical shear + walk. The age row
        //      disperses with the water like every other species — mixing
        //      moves age, physically. H7b: so does TEMPERATURE, at the same
        //      coefficient as a solute — the ARD engine's deliberate choice
        //      for its temperature row, adopted here for cross-engine
        //      consistency (decision 2026-08-30). ---------------------------
        if (ctx.options.lard_rwpt) {
            ++substep_counter_;
            const auto& cond = ctx.link_subtypes.conduits;
            for (int l = 0; l < nl; ++l) {
                const auto ul = static_cast<std::size_t>(l);
                if (links.type[ul] != LinkType::CONDUIT)
                    continue;
                const double q = std::abs(links.flow[ul]);
                if (q <= kTinyFlow)
                    continue;
                const int row = ctx.link_subtypes.conduit_row(l);
                if (row < 0)
                    continue;
                const auto ur = static_cast<std::size_t>(row);
                const double len = cond.length[ur];
                const double vol = links.volume[ul];
                if (len <= 0.0 || vol <= 0.0)
                    continue;
                const double a_flow = vol / len;
                const double ubar = q / a_flow;
                const double h = links.depth[ul];
                const bool circ = links.xsect_shape[ul] == XsectShape::CIRCULAR;
                const double rh = rwpt_hyd_radius(a_flow, h, links.xsect_geom1[ul], circ);
                rwpt_.disperse(ctx, store_, l, ubar, h, rh, cond.roughness[ur], release_vol_[ul],
                               q * dt, dt, substep_counter_,
                               static_cast<std::uint64_t>(ctx.options.rwpt_seed), scratch_);
            }
        }

        // ---- 4. DECAY (exact exponential; species-major stripes) ----------
        for (int p = 0; p < np; ++p) {
            const double k = ctx.pollutants.k_decay[static_cast<std::size_t>(p)];
            if (k == 0.0)
                continue;
            const double f = std::exp(-k * dt);
            double removed = store_.decay_species(p, f);
            for (int n = 0; n < nn; ++n) {
                const auto idx = static_cast<std::size_t>(n) * static_cast<std::size_t>(np) +
                                 static_cast<std::size_t>(p);
                const double v = node::storeVolume(nodes, n);
                removed += nodes.conc[idx] * (1.0 - f) * v;
                nodes.conc[idx] *= f;
            }
            if (static_cast<std::size_t>(p) < ctx.mass_balance.qual_routing_reacted.size())
                ctx.mass_balance.qual_routing_reacted[static_cast<std::size_t>(p)] += removed;
        }

        // ---- 4b. REACT (L3): MSX species integrate per SEGMENT (pipe
        //      scope, the segment's own pollutant rows as context) and
        //      per NODE store (tank scope, the node's HRT) — D-L1's
        //      gather/scatter through the shared integrator. Pollutant
        //      kdecay is NOT re-applied here: stage 4 owns it (the
        //      integrator handles only MSX kinetics), so there is no
        //      double-decay the way the legacy binding had to avoid.
        if (L.msx_first >= 0) {
            quality::updateUntreatedStorageHrt(ctx, dt); // review A6
            auto& rx = ctx.reactions;
            const int nsp = ns - L.msx_first;
            if (static_cast<int>(msx_block_.size()) < nsp)
                msx_block_.assign(static_cast<std::size_t>(nsp), 0.0);
            if (static_cast<int>(msx_poll_.size()) < np && np > 0)
                msx_poll_.assign(static_cast<std::size_t>(np), 0.0);
            for (int l = 0; l < nl; ++l) {
                const auto ul = static_cast<std::size_t>(l);
                if (links.type[ul] != LinkType::CONDUIT)
                    continue;
                const int cnt = store_.count(l);
                for (int i = 0; i < cnt; ++i) {
                    for (int sp = 0; sp < nsp; ++sp)
                        msx_block_[static_cast<std::size_t>(sp)] =
                            store_.seg_conc(l, i, L.msx_first + sp);
                    for (int p = 0; p < np; ++p)
                        msx_poll_[static_cast<std::size_t>(p)] = store_.seg_conc(l, i, p);
                    // TEMP: the segment's own temperature row when heat
                    // rides the store (H7b); NaN defers to the
                    // [REACTION_OPTIONS] TEMPERATURE constant.
                    const double seg_temp_c = (L.temp_row >= 0)
                                                  ? store_.seg_conc(l, i, L.temp_row)
                                                  : std::numeric_limits<double>::quiet_NaN();
                    transport::reactSpeciesBlock(ctx, /*tank=*/false, dt, msx_block_.data(),
                                                 np > 0 ? msx_poll_.data() : nullptr, 0.0,
                                                 seg_temp_c, l);
                    for (int sp = 0; sp < nsp; ++sp)
                        store_.set_seg_conc(l, i, L.msx_first + sp,
                                            msx_block_[static_cast<std::size_t>(sp)]);
                }
            }
            for (int n = 0; n < nn; ++n) {
                const auto un = static_cast<std::size_t>(n);
                if (!quality::nodeIsReactor(ctx, n) || nodes.volume[un] <= quality::ZERO_VOLUME)
                    continue;
                for (int sp = 0; sp < nsp; ++sp)
                    msx_block_[static_cast<std::size_t>(sp)] =
                        rx.msx_node_conc[un * static_cast<std::size_t>(nsp) +
                                         static_cast<std::size_t>(sp)];
                for (int p = 0; p < np; ++p)
                    msx_poll_[static_cast<std::size_t>(p)] =
                        nodes.conc[un * static_cast<std::size_t>(np) + static_cast<std::size_t>(p)];
                const double hrt = (un < nodes.hrt.size()) ? nodes.hrt[un] : 0.0;
                const double node_temp_c = (L.temp_row >= 0 && un < ctx.heat_state.node_temp.size())
                                               ? ctx.heat_state.node_temp[un]
                                               : std::numeric_limits<double>::quiet_NaN();
                transport::reactSpeciesBlock(ctx, /*tank=*/true, dt, msx_block_.data(),
                                             np > 0 ? msx_poll_.data() : nullptr, hrt, node_temp_c,
                                             n);
                for (int sp = 0; sp < nsp; ++sp)
                    rx.msx_node_conc[un * static_cast<std::size_t>(nsp) +
                                     static_cast<std::size_t>(sp)] =
                        msx_block_[static_cast<std::size_t>(sp)];
            }
        }
    }

    /**
     * @brief Per-routing-step publication: link means + the conc_old convention. `dt_routing`
     * ages the held state of empty slabs (once per step).
     *
     * @details Publish represented conduit means and copy pollutant current values into old
     * histories.
     * @param[in,out] ctx Matching row policy and covered link/node public pollutant/age/heat/MSX
     * arrays.
     * @param[in] dt_routing Elapsed accepted interval s added only to age of empty conduit
     * rings;0 for coupling republish avoids additional aging. No independent time validation.
     * @note Requires initialized store and scratch covering row count. Pollutants publish zero
     * means for empty rings; heat/MSX preserve old public values when empty. Nonconduit outputs
     * remain from node routing. Copies all link/node pollutant current arrays to old, can
     * allocate/throw after partial publication. No hydraulic/clock/global-ledger advance;
     * exclusive completed quality phase.
     */
    void publish(SimulationContext& ctx, double dt_routing) {
        // H7b: the fourth layout-aware site joins rowLayout() (H7a
        // converted step/substep/init and flagged this one).
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const bool age = (L.age_row >= 0);
        const bool heat = (L.temp_row >= 0);
        const int nl = ctx.n_links();
        auto& nodes = ctx.nodes;
        auto& links = ctx.links;
        auto& ws = ctx.water_age_state;
        auto& hs = ctx.heat_state;

        // ---- 5. PUBLISH ---------------------------------------------------
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT)
                continue;
            store_.mean_conc(l, scratch_.data());
            for (int p = 0; p < np; ++p)
                links.conc[ul * static_cast<std::size_t>(np) + static_cast<std::size_t>(p)] =
                    scratch_[static_cast<std::size_t>(p)];
            // Age: volume-weighted mean over the same segments, seconds.
            // The dry-element report mask (A2b) is at the report boundary,
            // engine-side — state keeps aging here regardless. An EMPTY
            // slab holds no parcels to average, so the link's held age
            // ages in place instead of resetting to 0 — the state/report
            // separation the dry-mask round established (a dry element's
            // STATE keeps aging; only the report masks it).
            if (age) {
                if (store_.count(l) > 0)
                    ws.link_age[ul] = scratch_[static_cast<std::size_t>(L.age_row)];
                else
                    ws.link_age[ul] += dt_routing;
            }
            // Temperature: volume-weighted mean over the same segments. An
            // EMPTY slab HOLDS its temperature — unlike age it does not
            // grow, and unlike the age report it is not masked when dry
            // (0 degC is an ordinary temperature; the no-mask call is
            // documented at the snapshot builder).
            if (heat) {
                if (store_.count(l) > 0)
                    hs.link_temp[ul] = scratch_[static_cast<std::size_t>(L.temp_row)];
            }
            // L3: species rows publish like temperature — an EMPTY
            // slab HOLDS its concentration.
            if (L.msx_first >= 0 && store_.count(l) > 0) {
                const int nsp = L.ns - L.msx_first;
                for (int sp = 0; sp < nsp; ++sp)
                    ctx.reactions.msx_link_conc[ul * static_cast<std::size_t>(nsp) +
                                                static_cast<std::size_t>(sp)] =
                        scratch_[static_cast<std::size_t>(L.msx_first + sp)];
            }
        }
        // conc_old bookkeeping matches the ARD/legacy convention.
        links.conc_old = links.conc;
        nodes.conc_old = nodes.conc;
    }

private:
    /**
     * @brief Archive initialized flag then validated persistent solver fields.
     * @param[in,out] a Compatible read/write completed-source archive; fixed extents already
     * configured.
     * @note Internal checkpoint helper; no SimulationContext serialization, rollback or clock
     * mutation. Archive/validation/allocation exceptions propagate; exclusive stable owner
     * required.
     */
    void checkpoint(detail::CompletedSourceArchive& a) {
        a(initialized_);
        checkpointFields(a);
    }
    /**
     * @brief Archive and validate segment, particle, topology and pending node receipt fields.
     * @param[in,out] a Matching completed-source read/write archive; no work when owner
     * uninitialized.
     * @throws std::runtime_error Invalid topology shape/permutation/link, flow sign outside-1..1
     * or nonfinite pending receipt; nested segment/particle/archive errors also propagate.
     * @note Read can partially mutate owner before failure. Does not validate adjacency
     * consistency, nonnegative receipt volumes or current hydraulic layout; dimensions/row
     * configuration are caller preconditions. Scratch/release volumes/context inventories not
     * serialized. Exclusive checkpoint barrier.
     */
    void checkpointFields(detail::CompletedSourceArchive& a) {
        if (!initialized_)
            return;
        store_.checkpoint(a);
        rwpt_.checkpoint(a);
        a(substep_counter_, topo_, node_out_links_);
        a.fixed(flow_sign_, node_mass_in_, node_vol_in_);
        const int nn = int(node_vol_in_.size()), nl = int(flow_sign_.size());
        if (topo_.size() != std::size_t(nn) || node_out_links_.size() != std::size_t(nn))
            throw std::runtime_error("Invalid LARD topology shape.");
        std::vector<bool> seen(nn, false);
        for (int n : topo_) {
            if (n < 0 || n >= nn || seen[n])
                throw std::runtime_error("Invalid LARD checkpoint topology.");
            seen[n] = true;
        }
        for (const auto& links : node_out_links_)
            for (int l : links)
                if (l < 0 || l >= nl)
                    throw std::runtime_error("Invalid LARD checkpoint link.");
        for (auto v : flow_sign_)
            if (v < -1 || v > 1)
                throw std::runtime_error("Invalid LARD flow direction.");
        for (const auto* v : {&node_mass_in_, &node_vol_in_})
            for (double x : *v)
                if (!std::isfinite(x))
                    throw std::runtime_error("Nonfinite LARD inflow receipt.");
    }
    /**
     * @brief Surface + radiative fluxes and the H6b bed pair, LARD spelling.
     *
     * @details Nodes relax exactly as `HeatFluxes.cpp` relaxes them (the node stores ARE
     * `heat_state.node_temp` under this engine). Links act on the SEGMENT field: the
     * pair/relaxation is solved against the volume-weighted link mean and the increment is
     * applied UNIFORMLY to every segment. Uniform-in-temperature is exact for the flux
     * linearization — each segment's surface share is proportional to its volume, so k =
     * A·J′/(ρ·cp·V) is the same for every segment — and it preserves the along-link profile,
     * which is the property this engine exists for. Solutes take the same uniform-delta spelling
     * from `exchangePair`. The bed itself stays per LINK (well-mixed along the conduit) — a
     * per-parcel bed would have to move with the water, and a bed does not move. Recorded
     * divergence.
     *
     * Until this method the LARD engine TRANSPORTED temperature but applied no flux module at
     * all, silently — a deck with SURFACE_EXCHANGE ON simply did not exchange, and no warning
     * said so. Found while closing H6b §4.
     *
     * @details Apply one accepted interval's channel/node atmospheric heat and channel bed
     * exchange.
     * @param[in,out] ctx Matching thermal rows, completed geometry, weather/overrides and
     * heat/bed stores; updates weather forcing, node/segment temperatures, shared bed
     * temperatures and solute concentrations.
     * @param[in] dt Positive finite accepted routing duration s; no enabled flux/bed or
     * nonpositive/NaN returns.
     * @note Atmospheric flux uses SI area/volume, water density/heat capacity; segment link mean
     * receives uniform degC increment. Bed coupling includes bulk bed heat/solute and prepared
     * contact overrides; mesh-contact links skip to separate owner. Bed solute shape change
     * zeros its concentrations. No hydraulic/clock/global mass booking. Requires exclusive
     * heat/segment ownership and prepared finite extents; allocations/dependencies may throw
     * after weather or earlier element changes.
     */
    void applyFluxesAndBed(SimulationContext& ctx, double dt) {
        const SpeciesRowLayout L = rowLayout(ctx);
        const bool heat_on = (L.temp_row >= 0);
        const bool bed_on = transport::heat::bedExchangeEnabled(ctx);
        const bool flux_on =
            heat_on && (ctx.heat_config.surface_exchange || ctx.heat_config.radiative_exchange);
        if ((!flux_on && !bed_on) || !(dt > 0.0))
            return;
        namespace th = transport::heat;

        if (flux_on)
            th::updateSolarForcing(ctx, dt);
        if (bed_on)
            th::seedBedTemperature(ctx);
        const double t_gr = bed_on ? th::groundTemperature(ctx) : 0.0;
        const double rho = ctx.options.water_density;
        const double cp = ctx.options.water_specific_heat;
        constexpr double kSqFt = 0.09290304;
        constexpr double kCuFt = 0.028316846592;

        auto& hs = ctx.heat_state;
        const int nl = ctx.n_links();
        const int nn = ctx.n_nodes();

        // ---- Nodes: storage surfaces only, HeatFluxes.cpp's convention. --
        if (flux_on) {
            const int unit_sys = ucf::getUnitSystem(static_cast<int>(ctx.options.flow_units));
            for (int n = 0; n < nn; ++n) {
                const auto un = static_cast<std::size_t>(n);
                if (un >= hs.node_temp.size())
                    break;
                const double vol = node::storeVolume(ctx.nodes, n);
                if (!(vol > 0.0))
                    continue;
                const double a = node::getSurfArea(ctx.nodes, n, ctx.nodes.depth[un], &ctx.tables,
                                                   unit_sys, &ctx.node_subtypes);
                if (!(a > 0.0))
                    continue;
                const double t = hs.node_temp[un];
                const HeatElement ne = HeatElement::node(n);
                hs.node_temp[un] +=
                    th::relaxT(th::netFluxOut(ctx, ne, t), th::netFluxOut(ctx, ne, t + th::kProbeC),
                               th::kProbeC, a * kSqFt, vol * kCuFt, dt, rho, cp);
            }
        }

        // ---- Links: segment field, uniform increments. -------------------
        // Bed rows: pollutants then MSX — every row before age/temp.
        const int n_bed = L.np + ((L.msx_first >= 0) ? (L.ns - L.msx_first) : 0);
        auto& bed = ctx.bed_state;
        if (bed_on && n_bed > 0 &&
            (bed.n_species != n_bed ||
             bed.link_conc.size() !=
                 static_cast<std::size_t>(nl) * static_cast<std::size_t>(n_bed))) {
            bed.link_conc.assign(static_cast<std::size_t>(nl) * static_cast<std::size_t>(n_bed),
                                 0.0);
            bed.n_species = n_bed;
        }

        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (ul < ctx.heat_state.mesh_contact_link.size() &&
                ctx.heat_state.mesh_contact_link[ul])
                continue;
            if (ctx.links.type[ul] != LinkType::CONDUIT)
                continue;
            const int cnt = store_.count(l);
            if (cnt <= 0)
                continue;
            const double vol_ft3 = store_.total_volume(l);
            if (!(vol_ft3 > 0.0))
                continue;

            store_.mean_conc(l, scratch_.data());
            // PE1: every parcel of a conduit shares the conduit's
            // attributes (D-PE1) — the bed and the shading belong to the
            // pipe, not to the water passing through it.
            const HeatElement le = HeatElement::link(l);

            // Heat: one pair/relaxation against the mean, uniform dT.
            if (heat_on) {
                const double t_mean = scratch_[static_cast<std::size_t>(L.temp_row)];
                const double surf_m2 = flux_on ? th::linkFreeSurfaceFt2(ctx, l) * kSqFt : 0.0;
                double d_tw = 0.0;
                bool bed_stepped = false;
                if (bed_on && ul < bed.link_temp.size()) {
                    // PE2: this link's own bed material and boundary.
                    const auto& sd = th::sedimentFor(ctx, le);
                    const th::BedCoupling g =
                        th::bedCouplingFromContact(ctx, sd, th::linkBedAreaM2(ctx, l), vol_ft3,
                                                   th::groundTempFor(ctx, sd, t_gr));
                    if (g.viable()) {
                        const th::PairStep ps = th::relaxPair(
                            g, t_mean, bed.link_temp[ul],
                            flux_on ? th::netFluxOut(ctx, le, t_mean) : 0.0,
                            flux_on ? th::netFluxOut(ctx, le, t_mean + th::kProbeC) : 0.0,
                            flux_on ? th::kProbeC : 0.0, surf_m2, dt);
                        d_tw = ps.dt_w;
                        bed.link_temp[ul] += ps.dt_b;
                        bed_stepped = true;
                    }
                }
                if (!bed_stepped && flux_on && surf_m2 > 0.0) {
                    d_tw = th::relaxT(th::netFluxOut(ctx, le, t_mean),
                                      th::netFluxOut(ctx, le, t_mean + th::kProbeC), th::kProbeC,
                                      surf_m2, vol_ft3 * kCuFt, dt, rho, cp);
                }
                if (d_tw != 0.0)
                    for (int i = 0; i < cnt; ++i)
                        store_.set_seg_conc(l, i, L.temp_row,
                                            store_.seg_conc(l, i, L.temp_row) + d_tw);
            }

            // Solutes: pair against the mean, uniform dc, per bed row.
            if (bed_on && n_bed > 0) {
                // PE2: same per-link config the heat pair above used, so the
                // two halves of one bed cannot disagree about its material.
                const auto& sd = th::sedimentFor(ctx, le);
                const double bed_m2 = th::linkBedAreaM2(ctx, l);
                const double vol_b = bed_m2 * sd.bed_thickness;
                const double q_exch = th::bedExchangeQ(sd, bed_m2);
                if (q_exch > 0.0 && vol_b > 0.0) {
                    for (int b = 0; b < n_bed; ++b) {
                        // Bed row b: pollutants 0..np-1 map directly, MSX
                        // rows at msx_first + (b - np).
                        const int row = (b < L.np) ? b : L.msx_first + (b - L.np);
                        const double c_mean = scratch_[static_cast<std::size_t>(row)];
                        double& cb = bed.link_conc[ul * static_cast<std::size_t>(n_bed) +
                                                   static_cast<std::size_t>(b)];
                        const th::SolutePairStep st =
                            th::exchangePair(c_mean, cb, vol_ft3 * kCuFt, vol_b, q_exch, dt);
                        if (st.dc_w != 0.0)
                            for (int i = 0; i < cnt; ++i)
                                store_.set_seg_conc(l, i, row,
                                                    store_.seg_conc(l, i, row) + st.dc_w);
                        cb += st.dc_b;
                    }
                }
            }
        }
    }

    /**
     * @brief Rebuild segment owner from current native hydraulic/quality inventories and row
     * policy.
     * @param[in,out] ctx Final stable dimensions/layout, native link/node quality/scalar values
     * and options; may resize/seed age/heat, apply initial overrides and ensure native MSX
     * state.
     * @note Replaces rings/receipts/scratch, resets flow signs, optionally resets particles,
     * seeds one wet segment per conduit from current public family values and computes topology.
     * Hotstart/legacy-seed flags control scalar seeding. Does not reset substep_counter_ here or
     * book mass/advance clock. Exclusive initial/reconfiguration phase; calling on evolved owner
     * discards parcel detail. Allocations can leave partial owner/context state; initialized
     * flag set only at final successful return.
     */
    void init(SimulationContext& ctx) {

        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const int ns = L.ns;
        // Derived from the layout rather than re-read from options, so the
        // "is there a reserved row" question has exactly one answer per call.
        const bool age = (L.age_row >= 0);
        const bool heat = (L.temp_row >= 0);
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        // X3a: slab capacity from [OPTIONS] MAX_SEGMENTS_PER_LINK, floored
        // at 2 (one segment to hold, one to receive).
        store_.resize(nl, ns, std::max(2, ctx.options.max_segments_per_link));
        flow_sign_.assign(static_cast<std::size_t>(nl), 1);
        node_mass_in_.assign(static_cast<std::size_t>(nn) * static_cast<std::size_t>(ns), 0.0);
        node_vol_in_.assign(static_cast<std::size_t>(nn), 0.0);
        scratch_.assign(static_cast<std::size_t>(ns), 0.0);
        treat_cin_.assign(static_cast<std::size_t>(np > 0 ? np : 0), 0.0);
        node_out_links_.assign(static_cast<std::size_t>(nn), {});
        release_vol_.assign(static_cast<std::size_t>(nl), 0.0);
        if (ctx.options.lard_rwpt)
            rwpt_.resize(nl);

        // L3: size + seed the shared MSX element state (GLOBAL fill +
        // [REACTION_QUALITY] overrides) with the SAME spelling the
        // LEGACY dispatch uses, then let the seeded link values seed
        // the segments below like every other row.
        if (L.msx_first >= 0)
            transport::ensureMsxState(ctx);

        // X4 age seeding, the ARD precedent: a hotstart-loaded state wins
        // (node_age/link_age already carry the restored values, A2a);
        // otherwise a configured INITIAL_STATE age fills the network.
        // Restored/seeded link ages then seed the segments below — the
        // same within-link profile collapse A2a recorded for ARD
        // (lesson 37): continuous, not bit-continuous.
        if (age) {
            auto& ws = ctx.water_age_state;
            if (ws.node_age.size() != static_cast<std::size_t>(nn))
                ws.resize(nn, nl, ctx.n_subcatches());
            if (!ws.hotstart_loaded) {
                const double a0 = ctx.water_age_config
                                      .global_age[static_cast<int>(WaterAgeSource::INITIAL_STATE)];
                if (a0 > 0.0) {
                    std::fill(ws.node_age.begin(), ws.node_age.end(), a0);
                    std::fill(ws.link_age.begin(), ws.link_age.end(), a0);
                }
            }
            // E-A3: [INITIAL_QUALITY] __WATER_AGE__ rows override the
            // global fill; the helper no-ops under hotstart (D-IQ7). The
            // per-link values then seed the segments below.
            transport::applyInitialAgeOverrides(ctx);
        }

        // H7b temperature seeding, the routeLegacyHeat convention: size the
        // state if the loaders have not already, fill with the configured
        // INITIAL_STATE once (legacy_seeded — shared with the LEGACY mirror
        // so a fallback path never re-seeds), then let per-element
        // [INITIAL_QUALITY] __TEMPERATURE__ rows override. The seeded link
        // temperatures seed the segments below, the same within-link
        // profile collapse the age row records (lesson 37): continuous,
        // not bit-continuous. Hotstart does NOT restore temperature — no
        // engine's does, the record has no field for it (owed; see the
        // H7b round record) — so a restarted run re-seeds from
        // INITIAL_STATE exactly as LEGACY and ARD do.
        if (heat) {
            auto& hstate = ctx.heat_state;
            const double t0 =
                ctx.heat_config.global_temp[static_cast<int>(HeatSource::INITIAL_STATE)];
            if (hstate.node_temp.size() != static_cast<std::size_t>(nn))
                hstate.resize(nn, nl, t0);
            if (!hstate.legacy_seeded) {
                std::fill(hstate.node_temp.begin(), hstate.node_temp.end(), t0);
                std::fill(hstate.link_temp.begin(), hstate.link_temp.end(), t0);
                transport::applyInitialTempOverrides(ctx);
                hstate.legacy_seeded = true;
            }
        }

        // Seed: one segment per conduit at the link's current volume and
        // (initQuality-seeded) concentration — a dry link seeds nothing.
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            store_.clear_link(l);
            if (ctx.links.type[ul] != LinkType::CONDUIT)
                continue;
            const double v = ctx.links.volume[ul];
            if (v <= 0.0)
                continue;
            for (int p = 0; p < np; ++p)
                scratch_[static_cast<std::size_t>(p)] =
                    ctx.links.conc[ul * static_cast<std::size_t>(np) + static_cast<std::size_t>(p)];
            if (age)
                scratch_[static_cast<std::size_t>(L.age_row)] = ctx.water_age_state.link_age[ul];
            if (heat)
                scratch_[static_cast<std::size_t>(L.temp_row)] = ctx.heat_state.link_temp[ul];
            if (L.msx_first >= 0) { // L3
                const int nsp = ns - L.msx_first;
                for (int sp = 0; sp < nsp; ++sp)
                    scratch_[static_cast<std::size_t>(L.msx_first + sp)] =
                        ctx.reactions.msx_link_conc[ul * static_cast<std::size_t>(nsp) +
                                                    static_cast<std::size_t>(sp)];
            }
            store_.push_front(l, v, scratch_.data());
            flow_sign_[ul] = (ctx.links.flow[ul] >= 0.0) ? 1 : -1;
        }
        computeTopoOrder(ctx);
        initialized_ = true;
    }

    /**
     * @brief Map a link endpoint through the owner's retained routing orientation.
     * @param[in] ctx Stable covered link endpoint arrays matching this owner.
     * @param[in] l Valid initialized nonnegative link ID, unchecked.
     * @return node1 for nonnegative retained flow_sign, else node2; may be negative if topology
     * uses a sentinel. No endpoint validation, allocation, physical/clock or owner change.
     */
    int upstreamNode(const SimulationContext& ctx, int l) const {
        const auto ul = static_cast<std::size_t>(l);
        return (flow_sign_[ul] >= 0) ? ctx.links.node1[ul] : ctx.links.node2[ul];
    }
    /**
     * @brief Map a link endpoint through the owner's retained routing orientation.
     * @param[in] ctx Stable covered link endpoint arrays matching this owner.
     * @param[in] l Valid initialized nonnegative link ID, unchecked.
     * @return node2 for nonnegative retained flow_sign, else node1; may be negative if topology
     * uses a sentinel. No endpoint validation, allocation, physical/clock or owner change.
     */
    int downstreamNode(const SimulationContext& ctx, int l) const {
        const auto ul = static_cast<std::size_t>(l);
        return (flow_sign_[ul] >= 0) ? ctx.links.node2[ul] : ctx.links.node1[ul];
    }

    /**
     * @brief Add a transferred water/inventory tuple to the private pending node accumulator.
     * @param[in] n Valid nonnegative node ID below initialized receipt count.
     * @param[in] vol Accepted transfer ft3, not flow rate; finite nonnegative expected but
     * unchecked.
     * @param[in] mass Already integrated species concentration*ft3 inventories, readable np
     * entries and nonaliasing destination.
     * @param[in] np Total carried row count/stride, including age/temperature/MSX, must equal
     * owner layout.
     * @note Adds vol to node water and mass[p] to each pending inventory without clearing. No
     * global ledger/clock/hydraulic mutation, allocation or bounds/finite check; caller owns
     * exclusive transfer phase.
     */
    void addToLedger(int n, double vol, const double* mass, int np) {
        const auto un = static_cast<std::size_t>(n);
        node_vol_in_[un] += vol;
        for (int p = 0; p < np; ++p)
            node_mass_in_[un * static_cast<std::size_t>(np) + static_cast<std::size_t>(p)] +=
                mass[static_cast<std::size_t>(p)];
    }
    /**
     * @brief Ledger add where `conc` (not mass) is supplied — passthrough links.
     *
     * @details Add a transferred water/inventory tuple to the private pending node accumulator.
     * @param[in] n Valid nonnegative node ID below initialized receipt count.
     * @param[in] vol Accepted transfer ft3, not flow rate; finite nonnegative expected but
     * unchecked.
     * @param[in] conc Species concentration values (despite Rate name, vol is integrated),
     * readable np entries and nonaliasing destination.
     * @param[in] np Total carried row count/stride, including age/temperature/MSX, must equal
     * owner layout.
     * @note Adds vol to node water and vol*conc[p] to each pending inventory without clearing.
     * No global ledger/clock/hydraulic mutation, allocation or bounds/finite check; caller owns
     * exclusive transfer phase.
     */
    void addToLedgerRate(int n, double vol, const double* conc, int np) {
        const auto un = static_cast<std::size_t>(n);
        node_vol_in_[un] += vol;
        for (int p = 0; p < np; ++p)
            node_mass_in_[un * static_cast<std::size_t>(np) + static_cast<std::size_t>(p)] +=
                vol * conc[static_cast<std::size_t>(p)];
    }

    /**
     * @brief Kahn over the node graph, edges from links with |Q| > kTinyFlow in current flow
     * direction (§4.3). Cycles: leftovers appended in index order — their same-step passthrough
     * mass carries in the ledger to the next step instead of being dropped.
     *
     * @details Rebuild directed node adjacency and traversal order using retained meaningful
     * flow signs.
     * @param[in] ctx Stable matching node/link counts, endpoint IDs and accepted flows.
     * @note Builds Kahn order over links with abs(flow)>1e-8ft3/s; adjacency includes
     * valid-endpoint links regardless of flow. Appends unresolved cyclic nodes in ascending ID
     * (no cycle error or iteration). Negative endpoints skip; positive endpoints must be within
     * node count, unchecked. Replaces internal adjacency/topology, no context mutation or
     * clock/ledger update; allocation may partially rebuild.
     */
    void computeTopoOrder(SimulationContext& ctx) {
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        std::vector<int> indeg(static_cast<std::size_t>(nn), 0);
        for (auto& v : node_out_links_)
            v.clear();
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            const int up = upstreamNode(ctx, l);
            const int dn = downstreamNode(ctx, l);
            if (up < 0 || dn < 0)
                continue;
            node_out_links_[static_cast<std::size_t>(up)].push_back(l);
            if (std::abs(ctx.links.flow[ul]) > kTinyFlow)
                indeg[static_cast<std::size_t>(dn)] += 1;
        }
        topo_.clear();
        topo_.reserve(static_cast<std::size_t>(nn));
        std::vector<int> q;
        for (int n = 0; n < nn; ++n)
            if (indeg[static_cast<std::size_t>(n)] == 0)
                q.push_back(n);
        std::vector<char> seen(static_cast<std::size_t>(nn), 0);
        std::size_t qi = 0;
        while (qi < q.size()) {
            const int n = q[qi++];
            if (seen[static_cast<std::size_t>(n)])
                continue;
            seen[static_cast<std::size_t>(n)] = 1;
            topo_.push_back(n);
            for (const int l : node_out_links_[static_cast<std::size_t>(n)]) {
                if (std::abs(ctx.links.flow[static_cast<std::size_t>(l)]) <= kTinyFlow)
                    continue;
                const int dn = downstreamNode(ctx, l);
                if (dn >= 0 && --indeg[static_cast<std::size_t>(dn)] == 0)
                    q.push_back(dn);
            }
        }
        for (int n = 0; n < nn; ++n) // cycle leftovers
            if (!seen[static_cast<std::size_t>(n)])
                topo_.push_back(n);
    }

    SegmentStore store_;
    RwptDispersion rwpt_;               ///< X3b particle field
    std::vector<double> release_vol_;   ///< per-substep V_in per link (X3b)
    std::uint64_t substep_counter_ = 0; ///< D-L6 RNG key component
    std::vector<int> topo_;
    std::vector<std::vector<int>> node_out_links_;
    std::vector<std::int8_t> flow_sign_;
    std::vector<double> node_mass_in_; ///< per-step ledger, mass
    std::vector<double> node_vol_in_;  ///< per-step ledger, volume
    std::vector<double> scratch_;      ///< np-sized work array
    /// P2.3: per-substep inflow MASS per pollutant at the node under MIX,
    /// becoming the treatment evaluator's cin — LARD's own inflow figures,
    /// because the LEGACY accumulators carry only external loads here.
    std::vector<double> treat_cin_;
    std::vector<double> msx_block_; ///< L3: per-element species block
    std::vector<double> msx_poll_;  ///< L3: pollutant context block
    bool initialized_ = false;
};

} // namespace lard
} // namespace openswmm

#endif // OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
