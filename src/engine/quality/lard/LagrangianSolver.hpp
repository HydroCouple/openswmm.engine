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
 * @file LagrangianSolver.hpp
 * @brief LARD (Lagrangian ARD) quality engine — X2: LTD transport core.
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
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
#define OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "../../core/SimulationContext.hpp"
#include "../../transport/InitialQualitySeeds.hpp"
#include "../NegativeSources.hpp"
#include "../QualityRouting.hpp"
#include "RwptDispersion.hpp"
#include "SegmentStore.hpp"

namespace openswmm {
namespace lard {

constexpr double kTinyFlow = 1.0e-8;  ///< cfs; below this a link moves nothing

/**
 * @brief The segment store's species-row layout, computed in ONE place.
 *
 * @details H7a. Three call sites (`step`, `substep`, `init`) each used to
 *          recompute `np + (age ? 1 : 0)` independently, and the age row was
 *          identified by the predicate `s >= np`. That predicate is only
 *          correct while age is the **one and only** reserved row: the
 *          moment a second one exists (temperature, H7b) `s >= np` captures
 *          both, and temperature would be silently aged, sourced from
 *          `node_age_vol_in` and published to `water_age_state` — a defect
 *          that produces plausible numbers rather than a crash.
 *
 *          So the row identity becomes an explicit INDEX rather than a
 *          threshold, mirroring what the ARD engine already does
 *          (`ArdEngine.cpp` `age_row_` / `temp_row_`). LARD is the engine
 *          that kept the threshold because it only ever had one.
 *
 *          **This struct is deliberately introduced while `temp_row` is
 *          always -1**, so the change is provably inert: with one reserved
 *          row, `s == age_row` and `s >= np` select exactly the same row.
 *          The corpus and the existing water-age gates are the proof. H7b
 *          then adds the temperature row on top of a layout that is already
 *          trusted, rather than changing the indexing and the physics in one
 *          step where a bit-identity check can no longer separate them.
 */
struct SpeciesRowLayout {
    int np       = 0;   ///< pollutant rows occupy [0, np)
    int age_row  = -1;  ///< water-age row index, or -1 when absent
    int temp_row = -1;  ///< temperature row index, or -1 (H7b)
    int ns       = 0;   ///< total rows the store carries
};

/// The single source of truth for the row layout. Reserved rows are appended
/// after the pollutants in a fixed order (age, then temperature) so an index
/// means the same thing everywhere.
inline SpeciesRowLayout rowLayout(const SimulationContext& ctx) {
    SpeciesRowLayout L;
    L.np = ctx.n_pollutants();
    L.ns = L.np;
    if (ctx.options.water_age) L.age_row = L.ns++;
    if (ctx.options.heat_transport) L.temp_row = L.ns++;   // H7b
    return L;
}

class LagrangianSolver {
public:
    /**
     * @brief One routing step of LTD transport. Lazily initializes on the
     *        first call (needs router-set volumes, the ARD precedent).
     */
    void step(SimulationContext& ctx, double dt_routing) {
        // X4/H7b: the age and temperature rows ride the segments after
        // the pollutants — age published to water_age_state (seconds),
        // temperature to heat_state (degC) — rather than the np-strided
        // conc arrays.
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const int ns = L.ns;
        if (ns <= 0) return;
        if (!initialized_) init(ctx);

        const int nl = ctx.n_links();
        auto& links = ctx.links;

        // ---- 0. Flow reversal (§4.4) — once per routing step: the flow
        //      solution is constant within it. -----------------------------
        bool topo_dirty = false;
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            const double q = links.flow[ul];
            if (std::abs(q) <= kTinyFlow) continue;
            const std::int8_t sign = (q >= 0.0) ? 1 : -1;
            if (sign != flow_sign_[ul]) {
                if (links.type[ul] == LinkType::CONDUIT) store_.reverse(l);
                flow_sign_[ul] = sign;
                topo_dirty = true;
            }
        }
        if (topo_dirty || topo_.empty()) computeTopoOrder(ctx);

        // ---- X3a: QUALITY_STEP substepping (strategy §4.2). Equal
        //      substeps; mass/age external loads are RATES and scale
        //      through dt, the per-routing-step external VOLUME
        //      (qual_vol_in) scales through frac. dtq absent or >= the
        //      routing step degenerates to one substep — bit-identical to
        //      the pre-X3a engine by construction.
        const double dtq = ctx.options.quality_step;
        const int nsub = (dtq > 0.0 && dtq < dt_routing)
                             ? static_cast<int>(std::ceil(dt_routing / dtq))
                             : 1;
        const double dt = dt_routing / static_cast<double>(nsub);
        const double frac = 1.0 / static_cast<double>(nsub);
        for (int sub = 0; sub < nsub; ++sub) substep(ctx, dt, frac);

        publish(ctx, dt_routing);
    }

    /**
     * @brief One LTD substep: AGE → DRAIN → MIX(+passthrough) → RELEASE →
     *        DECAY.
     *
     * @param frac  fraction of the per-routing-step external volume
     *              (`qual_vol_in`) this substep consumes.
     */
    void substep(SimulationContext& ctx, double dt, double frac) {
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const int ns = L.ns;
        // Derived from the layout rather than re-read from options, so the
        // "is there a reserved row" question has exactly one answer per call.
        const bool age  = (L.age_row >= 0);
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
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT) continue;
            const double q = std::abs(links.flow[ul]);
            const int dn = downstreamNode(ctx, l);
            const int up = upstreamNode(ctx, l);

            if (q > kTinyFlow && dn >= 0) {
                std::fill(scratch_.begin(), scratch_.end(), 0.0);
                const double drained = store_.drain_back(l, q * dt,
                                                         scratch_.data());
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
                const double shed = store_.drain_front(l, v_rem - v_new,
                                                       scratch_.data());
                addToLedger(up, shed, scratch_.data(), ns);
            }
        }

        // ---- 2. MIX in topo order, passthrough zero-volume links ----------
        for (const int n : topo_) {
            const auto un = static_cast<std::size_t>(n);
            const double v_old = nodes.old_volume[un];
            const double v_in = node_vol_in_[un];
            // OUTFALL_BACKFLOW_QUALITY ZERO: an outfall taking no volume
            // inflow this substep is a fresh boundary — its held state
            // (pollutant rows AND the age row) reads zero, so the RELEASE
            // seeding and zero-volume passthrough below draw clean water.
            const bool zero_bf =
                ctx.options.outfall_backflow_zero &&
                nodes.type[un] == NodeType::OUTFALL &&
                v_in + nodes.qual_vol_in[un] * frac <= 0.0;

            for (int s = 0; s < ns; ++s) {
                // H7a: identity by INDEX, not by threshold. `s >= np` was
                // correct only while age was the sole reserved row; it would
                // silently capture the temperature row too (H7b).
                const bool is_age  = (s == L.age_row);
                const bool is_temp = (s == L.temp_row);
                const auto li = un * static_cast<std::size_t>(ns) +
                                static_cast<std::size_t>(s);  // ledger index
                // State and external load per row. Pollutants: nodes.conc +
                // qual_mass_in (rate × dt — the mixAtNodes convention). Age:
                // water_age_state.node_age (already aged +dt this step) +
                // node_age_vol_in (age·ft³/s rate, the D-UT10 parallel
                // accumulator filled by all seven loader pathways).
                const auto ci = un * static_cast<std::size_t>(np) +
                                static_cast<std::size_t>(s);
                // Temperature mirrors age one row over: state in
                // heat_state.node_temp (degC), external load from
                // node_temp_vol_in (degC.ft3/s, the D-UT10 twin filled by
                // the same seven loaders) -- H1's convention, consumed here
                // instead of by routeLegacyHeat.
                const double st_old = is_age  ? ws.node_age[un]
                                    : is_temp ? hs.node_temp[un]
                                              : nodes.conc[ci];
                const double m_ext =
                    is_age  ? ws.node_age_vol_in[un] * dt
                  : is_temp ? hs.node_temp_vol_in[un] * dt
                            : nodes.qual_mass_in[ci] * dt;
                double m = st_old * v_old + node_mass_in_[li] + m_ext;
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
                    else
                        quality::bookNegativeSourceClamp(ctx, n, s, -m);
                    m = 0.0;
                }
                const double denom =
                    v_old + v_in + nodes.qual_vol_in[un] * frac;
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
                const double st_new =
                    zero_bf ? 0.0
                            : ((denom > 1.0e-12) ? m / denom : st_old);
                if (is_age)       ws.node_age[un] = st_new;
                else if (is_temp) hs.node_temp[un] = st_new;
                else              nodes.conc[ci] = st_new;
                node_mass_in_[li] = 0.0;  // consumed; cycle residue carries
            }
            node_vol_in_[un] = 0.0;

            // Zero-volume passthrough (§2.4): outgoing pump/orifice/weir/
            // outlet links deliver the node's NEW concentration downstream
            // within this step — the property the topo order exists for.
            for (const int l : node_out_links_[un]) {
                const auto ul = static_cast<std::size_t>(l);
                if (links.type[ul] == LinkType::CONDUIT) continue;
                const double q = std::abs(links.flow[ul]);
                if (q <= kTinyFlow) continue;
                const int dn = downstreamNode(ctx, l);
                if (dn < 0) continue;
                for (int p = 0; p < np; ++p) {
                    const auto ni = un * static_cast<std::size_t>(np) +
                                    static_cast<std::size_t>(p);
                    const auto lp = ul * static_cast<std::size_t>(np) +
                                    static_cast<std::size_t>(p);
                    scratch_[static_cast<std::size_t>(p)] = nodes.conc[ni];
                    links.conc[lp] = nodes.conc[ni];
                }
                if (age) {
                    scratch_[static_cast<std::size_t>(L.age_row)] =
                        ws.node_age[un];
                    ws.link_age[ul] = ws.node_age[un];
                }
                if (heat) {
                    scratch_[static_cast<std::size_t>(L.temp_row)] =
                        hs.node_temp[un];
                    hs.link_temp[ul] = hs.node_temp[un];
                }
                addToLedgerRate(dn, q * dt, scratch_.data(), ns);
            }
        }

        // ---- 3. RELEASE new front segments --------------------------------
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT) continue;
            const int up = upstreamNode(ctx, l);
            const double need = links.volume[ul] - store_.total_volume(l);
            release_vol_[ul] = (need > 0.0) ? need : 0.0;  // X3b: RWPT's V_in
            if (need <= 0.0 || up < 0) continue;
            const auto uu = static_cast<std::size_t>(up);
            for (int p = 0; p < np; ++p)
                scratch_[static_cast<std::size_t>(p)] =
                    nodes.conc[uu * static_cast<std::size_t>(np) +
                               static_cast<std::size_t>(p)];
            if (age)
                scratch_[static_cast<std::size_t>(L.age_row)] =
                    ws.node_age[uu];
            if (heat)
                scratch_[static_cast<std::size_t>(L.temp_row)] =
                    hs.node_temp[uu];
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
                if (links.type[ul] != LinkType::CONDUIT) continue;
                const double q = std::abs(links.flow[ul]);
                if (q <= kTinyFlow) continue;
                const int row = ctx.link_subtypes.conduit_row(l);
                if (row < 0) continue;
                const auto ur = static_cast<std::size_t>(row);
                const double len = cond.length[ur];
                const double vol = links.volume[ul];
                if (len <= 0.0 || vol <= 0.0) continue;
                const double a_flow = vol / len;
                const double ubar = q / a_flow;
                const double h = links.depth[ul];
                const bool circ =
                    links.xsect_shape[ul] == XsectShape::CIRCULAR;
                const double rh =
                    rwpt_hyd_radius(a_flow, h, links.xsect_geom1[ul], circ);
                rwpt_.disperse(ctx, store_, l, ubar, h, rh,
                               cond.roughness[ur], release_vol_[ul],
                               q * dt, dt, substep_counter_,
                               static_cast<std::uint64_t>(
                                   ctx.options.rwpt_seed),
                               scratch_);
            }
        }

        // ---- 4. DECAY (exact exponential; species-major stripes) ----------
        for (int p = 0; p < np; ++p) {
            const double k = ctx.pollutants.k_decay[static_cast<std::size_t>(p)];
            if (k == 0.0) continue;
            const double f = std::exp(-k * dt);
            double removed = store_.decay_species(p, f);
            for (int n = 0; n < nn; ++n) {
                const auto idx = static_cast<std::size_t>(n) *
                                     static_cast<std::size_t>(np) +
                                 static_cast<std::size_t>(p);
                const double v = nodes.volume[static_cast<std::size_t>(n)];
                removed += nodes.conc[idx] * (1.0 - f) * v;
                nodes.conc[idx] *= f;
            }
            if (static_cast<std::size_t>(p) <
                ctx.mass_balance.qual_routing_reacted.size())
                ctx.mass_balance.qual_routing_reacted[
                    static_cast<std::size_t>(p)] += removed;
        }
    }

    /// Per-routing-step publication: link means + the conc_old convention.
    /// `dt_routing` ages the held state of empty slabs (once per step).
    void publish(SimulationContext& ctx, double dt_routing) {
        // H7b: the fourth layout-aware site joins rowLayout() (H7a
        // converted step/substep/init and flagged this one).
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const bool age  = (L.age_row >= 0);
        const bool heat = (L.temp_row >= 0);
        const int nl = ctx.n_links();
        auto& nodes = ctx.nodes;
        auto& links = ctx.links;
        auto& ws = ctx.water_age_state;
        auto& hs = ctx.heat_state;

        // ---- 5. PUBLISH ---------------------------------------------------
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            if (links.type[ul] != LinkType::CONDUIT) continue;
            store_.mean_conc(l, scratch_.data());
            for (int p = 0; p < np; ++p)
                links.conc[ul * static_cast<std::size_t>(np) +
                           static_cast<std::size_t>(p)] =
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
                    ws.link_age[ul] =
                        scratch_[static_cast<std::size_t>(L.age_row)];
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
                    hs.link_temp[ul] =
                        scratch_[static_cast<std::size_t>(L.temp_row)];
            }
        }
        // conc_old bookkeeping matches the ARD/legacy convention.
        links.conc_old = links.conc;
        nodes.conc_old = nodes.conc;
    }

private:
    void init(SimulationContext& ctx) {
        const SpeciesRowLayout L = rowLayout(ctx);
        const int np = L.np;
        const int ns = L.ns;
        // Derived from the layout rather than re-read from options, so the
        // "is there a reserved row" question has exactly one answer per call.
        const bool age  = (L.age_row >= 0);
        const bool heat = (L.temp_row >= 0);
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        // X3a: slab capacity from [OPTIONS] MAX_SEGMENTS_PER_LINK, floored
        // at 2 (one segment to hold, one to receive).
        store_.resize(nl, ns,
                      std::max(2, ctx.options.max_segments_per_link));
        flow_sign_.assign(static_cast<std::size_t>(nl), 1);
        node_mass_in_.assign(
            static_cast<std::size_t>(nn) * static_cast<std::size_t>(ns), 0.0);
        node_vol_in_.assign(static_cast<std::size_t>(nn), 0.0);
        scratch_.assign(static_cast<std::size_t>(ns), 0.0);
        node_out_links_.assign(static_cast<std::size_t>(nn), {});
        release_vol_.assign(static_cast<std::size_t>(nl), 0.0);
        if (ctx.options.lard_rwpt) rwpt_.resize(nl);

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
                const double a0 = ctx.water_age_config.global_age[
                    static_cast<int>(WaterAgeSource::INITIAL_STATE)];
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
            const double t0 = ctx.heat_config.global_temp[
                static_cast<int>(HeatSource::INITIAL_STATE)];
            if (hstate.node_temp.size() != static_cast<std::size_t>(nn))
                hstate.resize(nn, nl, t0);
            if (!hstate.legacy_seeded) {
                std::fill(hstate.node_temp.begin(), hstate.node_temp.end(),
                          t0);
                std::fill(hstate.link_temp.begin(), hstate.link_temp.end(),
                          t0);
                transport::applyInitialTempOverrides(ctx);
                hstate.legacy_seeded = true;
            }
        }

        // Seed: one segment per conduit at the link's current volume and
        // (initQuality-seeded) concentration — a dry link seeds nothing.
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            store_.clear_link(l);
            if (ctx.links.type[ul] != LinkType::CONDUIT) continue;
            const double v = ctx.links.volume[ul];
            if (v <= 0.0) continue;
            for (int p = 0; p < np; ++p)
                scratch_[static_cast<std::size_t>(p)] =
                    ctx.links.conc[ul * static_cast<std::size_t>(np) +
                                   static_cast<std::size_t>(p)];
            if (age)
                scratch_[static_cast<std::size_t>(L.age_row)] =
                    ctx.water_age_state.link_age[ul];
            if (heat)
                scratch_[static_cast<std::size_t>(L.temp_row)] =
                    ctx.heat_state.link_temp[ul];
            store_.push_front(l, v, scratch_.data());
            flow_sign_[ul] = (ctx.links.flow[ul] >= 0.0) ? 1 : -1;
        }
        computeTopoOrder(ctx);
        initialized_ = true;
    }

    int upstreamNode(const SimulationContext& ctx, int l) const {
        const auto ul = static_cast<std::size_t>(l);
        return (flow_sign_[ul] >= 0) ? ctx.links.node1[ul]
                                     : ctx.links.node2[ul];
    }
    int downstreamNode(const SimulationContext& ctx, int l) const {
        const auto ul = static_cast<std::size_t>(l);
        return (flow_sign_[ul] >= 0) ? ctx.links.node2[ul]
                                     : ctx.links.node1[ul];
    }

    void addToLedger(int n, double vol, const double* mass, int np) {
        const auto un = static_cast<std::size_t>(n);
        node_vol_in_[un] += vol;
        for (int p = 0; p < np; ++p)
            node_mass_in_[un * static_cast<std::size_t>(np) +
                          static_cast<std::size_t>(p)] +=
                mass[static_cast<std::size_t>(p)];
    }
    /// Ledger add where `conc` (not mass) is supplied — passthrough links.
    void addToLedgerRate(int n, double vol, const double* conc, int np) {
        const auto un = static_cast<std::size_t>(n);
        node_vol_in_[un] += vol;
        for (int p = 0; p < np; ++p)
            node_mass_in_[un * static_cast<std::size_t>(np) +
                          static_cast<std::size_t>(p)] +=
                vol * conc[static_cast<std::size_t>(p)];
    }

    /// Kahn over the node graph, edges from links with |Q| > kTinyFlow in
    /// current flow direction (§4.3). Cycles: leftovers appended in index
    /// order — their same-step passthrough mass carries in the ledger to
    /// the next step instead of being dropped.
    void computeTopoOrder(SimulationContext& ctx) {
        const int nn = ctx.n_nodes();
        const int nl = ctx.n_links();
        std::vector<int> indeg(static_cast<std::size_t>(nn), 0);
        for (auto& v : node_out_links_) v.clear();
        for (int l = 0; l < nl; ++l) {
            const auto ul = static_cast<std::size_t>(l);
            const int up = upstreamNode(ctx, l);
            const int dn = downstreamNode(ctx, l);
            if (up < 0 || dn < 0) continue;
            node_out_links_[static_cast<std::size_t>(up)].push_back(l);
            if (std::abs(ctx.links.flow[ul]) > kTinyFlow)
                indeg[static_cast<std::size_t>(dn)] += 1;
        }
        topo_.clear();
        topo_.reserve(static_cast<std::size_t>(nn));
        std::vector<int> q;
        for (int n = 0; n < nn; ++n)
            if (indeg[static_cast<std::size_t>(n)] == 0) q.push_back(n);
        std::vector<char> seen(static_cast<std::size_t>(nn), 0);
        std::size_t qi = 0;
        while (qi < q.size()) {
            const int n = q[qi++];
            if (seen[static_cast<std::size_t>(n)]) continue;
            seen[static_cast<std::size_t>(n)] = 1;
            topo_.push_back(n);
            for (const int l : node_out_links_[static_cast<std::size_t>(n)]) {
                if (std::abs(ctx.links.flow[static_cast<std::size_t>(l)]) <=
                    kTinyFlow)
                    continue;
                const int dn = downstreamNode(ctx, l);
                if (dn >= 0 && --indeg[static_cast<std::size_t>(dn)] == 0)
                    q.push_back(dn);
            }
        }
        for (int n = 0; n < nn; ++n)  // cycle leftovers
            if (!seen[static_cast<std::size_t>(n)]) topo_.push_back(n);
    }

    SegmentStore store_;
    RwptDispersion rwpt_;                ///< X3b particle field
    std::vector<double> release_vol_;    ///< per-substep V_in per link (X3b)
    std::uint64_t substep_counter_ = 0;  ///< D-L6 RNG key component
    std::vector<int> topo_;
    std::vector<std::vector<int>> node_out_links_;
    std::vector<std::int8_t> flow_sign_;
    std::vector<double> node_mass_in_;  ///< per-step ledger, mass
    std::vector<double> node_vol_in_;   ///< per-step ledger, volume
    std::vector<double> scratch_;       ///< np-sized work array
    bool initialized_ = false;
};

}  // namespace lard
}  // namespace openswmm

#endif  // OPENSWMM_QUALITY_LARD_LAGRANGIAN_SOLVER_HPP
