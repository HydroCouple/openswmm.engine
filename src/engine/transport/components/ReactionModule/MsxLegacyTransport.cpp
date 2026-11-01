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
 * @file      MsxLegacyTransport.cpp
 * @brief     R4b body. Formula provenance is `routeLegacyHeat` — see the header.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "MsxLegacyTransport.hpp"
#include "ReactionSources.hpp"

#include <algorithm>
#include <cmath>
#include <vector>

#include "ReactionLegacyBinding.hpp"
#include "../../../core/SimulationContext.hpp"
#include "../../../quality/QualityRouting.hpp"
#include "../../../quality/MsxLidReactions.hpp"
#include "../../../hydrology/LidNode.hpp"

namespace openswmm::transport {

namespace {
/// The quality path's own thresholds — one litre and one millimetre, the
/// numbers legacy qualrout.c routes on. This mirror previously carried a
/// local 1e-10, so it dispatched differently from the family it mirrors.
using quality::ZERO_DEPTH;
using quality::LEGACY_ZERO;

/// Routing-thread scratch (the BindingScratch pattern). Strided per species:
/// the node/link snapshots and the per-node mass-rate accumulator.
struct MsxScratch {
    std::vector<double> node_old, link_old, mass_in;
    /**
     * @brief Size the shared legacy native-species transport scratch for model dimensions.
     * @param[in] nn Nonnegative node count.
     * @param[in] nl Nonnegative link count.
     * @param[in] ns Species stride, products fit size_t and allocation limits.
     * @note Node mismatch zeroes old-node/mass-rate scratch, link mismatch zeroes old-link
     * scratch. Allocation may partially reset. No model/clock mutation; process-static workspace
     * is not concurrent.
     */
    void ensure(std::size_t nn, std::size_t nl, std::size_t ns) {
        if (node_old.size() != nn * ns) {
            node_old.assign(nn * ns, 0.0);
            mass_in.assign(nn * ns, 0.0);
        }
        if (link_old.size() != nl * ns)
            link_old.assign(nl * ns, 0.0);
    }
};
/**
 * @brief Borrow process-wide legacy native-species routing scratch.
 * @return Static nonowned MsxScratch reference with process lifetime; mutable use
 * unsynchronized. Caller serializes routeLegacyMsx calls across all contexts.
 */
MsxScratch& scratch() {
    static MsxScratch s;
    return s;
}
} // namespace

/**
 * @brief Initialize native network species continuity ledger when absent or row shape changes.
 * @param[in,out] ctx Prepared old native water volumes, reaction initial state and native LID
 * held inventories; ensures MSX concentration state then arms/resets matching ledger as needed.
 * @note Initial BULK inventory is node concentration*old ft3 plus held native LID mass, and link
 * concentration*old ft3. WALL rows excluded. Already armed matching ledger preserved.
 * Concentration*ft3 convention, no28.317/user-mass conversion. Requires covered species
 * flags/volumes/seeds; allocation can partially reset. Exclusive initial accepted-routing phase,
 * no water/clock advance.
 */
void armMsxRoutingLedger(SimulationContext& ctx) {
    auto& rx = ctx.reactions;
    ensureMsxState(ctx);
    const int nn = ctx.n_nodes(), nl = ctx.n_links();
    const auto uns = std::size_t(rx.n_species());
    // Review B14 (2026-10-15): arm the species routing ledger on the first
    // routed step, opening it with the stored mass exactly as the pollutant
    // ledger's initMassBalance does (concentration × volume, nodes + links).
    auto& led = rx.msx_ledger;
    if (!led.armed || led.init.size() != uns) {
        led.arm(uns);
        if(ctx.state_accessors.get_fv_retained_species_mass) {
            const auto held=ctx.state_accessors.get_fv_retained_species_mass();
            for(std::size_t s=0;s<uns&&s<held.size();++s)if(!rx.species_is_wall[s]) {
                led.init[s]+=held[s];led.final_dry[s]=held[s];
            }
        }
        for (int i = 0; i < nn; ++i)
            for (std::size_t s = 0; s < uns; ++s)
                if (!rx.species_is_wall[s])
                    led.init[s] += rx.msx_node_conc[static_cast<std::size_t>(i) * uns + s] *
                                       ctx.nodes.old_volume[static_cast<std::size_t>(i)] +
                                   msxlid::heldMass(ctx, i, int(s));
        for (int j = 0; j < nl; ++j)
            for (std::size_t s = 0; s < uns; ++s)
                if (!rx.species_is_wall[s])
                    led.init[s] += rx.msx_link_conc[static_cast<std::size_t>(j) * uns + s] *
                                   ctx.links.old_volume[static_cast<std::size_t>(j)];
    }
}

/**
 * @brief Transport native BULK species through legacy node/link complete-mix routing and LID
 * ports.
 * @param[in,out] ctx Prepared old/current native hydraulics, matching reaction stores and
 * assembled qual_vol_in ft3/external native concentration*ft3/s; updates native node/link
 * concentrations, LID captures, source boosters and network inflow/seep/dry continuity ledger.
 * @param[in] dt Positive finite accepted routing interval s; nonpositive/no species returns.
 * @note Ensures initial state/arms ledger, snapshots old species then routes absolute-flow
 * downstream rates, subtracting separately captured native LID mass. Node mobile water/history,
 * storage evaporation and exfiltration corrections, backflow/dry tests follow shared rules. WALL
 * rows remain local and are neither advected nor booked; kinetics is a separate stage. Link
 * STEADY uses geometric node1, others shared mixing inflow/dry tests; RATE extraction floors0
 * without an added-mass continuity entry here. Not guarded by configured/compiled/IGNORE_QUALITY
 * flags; caller owns dispatch policy. No hydraulic water/engine clock advance. Static scratch
 * prohibits concurrent models; allocation and booster/dependency exceptions can leave partial
 * stores/ledgers. Exactly-once accepted phase required.
 */
void routeLegacyMsx(SimulationContext& ctx, double dt) {
    auto& rx = ctx.reactions;
    const int nsp = rx.n_species();
    if (nsp <= 0 || dt <= 0.0)
        return;

    const int nn = ctx.n_nodes();
    const int nl = ctx.n_links();
    const auto uns = static_cast<std::size_t>(nsp);

    // R4's seeding path — sizes the arrays and applies GLOBAL +
    // [REACTION_QUALITY] fills exactly once, whichever dispatch touches the
    // state first this run.
    ensureMsxState(ctx);
    if (rx.msx_node_conc.size() < static_cast<std::size_t>(nn) * uns ||
        rx.msx_link_conc.size() < static_cast<std::size_t>(nl) * uns)
        return;

    auto& sc = scratch();
    sc.ensure(static_cast<std::size_t>(nn), static_cast<std::size_t>(nl), uns);

    armMsxRoutingLedger(ctx);
    auto& led = rx.msx_ledger;

    // ---- 1. Old-state snapshots (post-reaction: the react stages ran
    //         earlier in execute(), so transport moves this step's reacted
    //         concentrations — the Lie split the header documents). --------
    std::copy(rx.msx_node_conc.begin(),
              rx.msx_node_conc.begin() +
                  static_cast<std::ptrdiff_t>(static_cast<std::size_t>(nn) * uns),
              sc.node_old.begin());
    std::copy(rx.msx_link_conc.begin(),
              rx.msx_link_conc.begin() +
                  static_cast<std::ptrdiff_t>(static_cast<std::size_t>(nl) * uns),
              sc.link_old.begin());

    // ---- 2. Mass accumulation (accumulateLinkLoads mirror, RATE
    //         convention q·c_old into the downstream node). ----------------
    std::fill(sc.mass_in.begin(), sc.mass_in.end(), 0.0);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        const double q = std::fabs(ctx.links.flow[uj]);
        if (q <= 0.0)
            continue;
        const int dn = (ctx.links.flow[uj] >= 0.0) ? ctx.links.node2[uj] : ctx.links.node1[uj];
        if (dn < 0 || dn >= nn)
            continue;
        const auto udn = static_cast<std::size_t>(dn);
        const int source = ctx.links.flow[uj] >= 0 ? ctx.links.node1[uj] : ctx.links.node2[uj];
        for (std::size_t s = 0; s < uns; ++s)
            if (!rx.species_is_wall[s]) {
                double concentration = sc.link_old[uj * uns + s];
                if (quality::linkTakesUpstreamValue(ctx, j) &&
                    (lidnode::active(ctx, source) || lidnode::active(ctx, dn)))
                    concentration =
                        msxlid::outlet(ctx, source, j, int(s), sc.node_old[source * uns + s]);
                const double rate = q * concentration;
                sc.mass_in[udn * uns + s] +=
                    rate - msxlid::receive(ctx, dn, j, q * dt, int(s), rate, dt);
            }
    }

    // ---- 3. Node mixing (mixAtNodes mirror, storage evap factor). External
    //         loads: U2 (2026-09-07) — [INFLOWS] rows naming a species
    //         arrive through msx_ext_mass_in (a RATE, the qual_mass_in
    //         shape) and join the link mass here; empty when no such row
    //         exists, so species-only-from-reactions decks are unchanged.
    //         The clamp is TWO-SIDED like the heat mirror's: mixing cannot
    //         exceed the larger of the held and incoming concentrations,
    //         nor go below zero. --------------------------------------------
    const bool has_ext = rx.msx_ext_mass_in.size() >= static_cast<std::size_t>(nn) * uns;
    for (int i = 0; i < nn; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double v_old = msxlid::mobileWater(ctx, i, ctx.nodes.old_volume[ui]);
        const double v_in = ctx.nodes.qual_vol_in[ui];
        const double q_in = (dt > 0.0) ? v_in / dt : 0.0;
        const bool zero_bf = ctx.options.outfall_backflow_zero &&
                             ctx.nodes.type[ui] == NodeType::OUTFALL && v_in <= 0.0;
        // The quality path's dispatch and dry-node rule, asked rather than
        // re-derived — a species IS a concentration, so both carry over
        // exactly as they do for a pollutant.
        const bool reactor = quality::nodeIsReactor(ctx, i);
        const bool dry = quality::nodeIsDry(ctx, i, q_in) && !lidnode::active(ctx, i);
        // Review B1 (2026-10-15): a species concentrates with the water it
        // is dissolved in, exactly as a pollutant does (mixAtNodes).
        const double f_evap = reactor ? quality::storageEvapFactor(ctx, i) : 1.0;
        // The storage unit's exfiltration VOLUME this step, booked at the
        // stored concentration before evaporation — mixAtNodes' seepage row.
        double v_exfil = 0.0;
        if (reactor) {
            const int srow = ctx.node_subtypes.storage_row(i);
            const auto& st = ctx.node_subtypes.storages;
            if (srow >= 0 && static_cast<std::size_t>(srow) < st.exfil_loss.size())
                v_exfil = st.exfil_loss[static_cast<std::size_t>(srow)];
            if (srow >= 0 && rx.lid_enabled(st.lid[srow].control))
                v_exfil = std::max(0., v_exfil - st.lid_state[srow].richards_bottom_loss);
        }
        for (std::size_t s = 0; s < uns; ++s) {
            if (rx.species_is_wall[s])
                continue; // attached state never mixes with water
            const double c_old = sc.node_old[ui * uns + s];
            const double ext_rate = has_ext ? rx.msx_ext_mass_in[ui * uns + s] : 0.0;
            const double mass = (sc.mass_in[ui * uns + s] + ext_rate) * dt;
            led.inflow[s] += ext_rate * dt;

            if (!reactor) {
                // findNodeQual: no storage volume, so the concentration is
                // the inflow's.
                // The gate is legacy's `qNode > ZERO` on the RATE, the same
                // one the reactor half below uses — not "v_in is non-zero",
                // which divides residual mass by residual volume.
                double c;
                if (q_in > LEGACY_ZERO)
                    c = mass / v_in;
                else if (zero_bf)
                    c = 0.0;
                else if (ctx.nodes.depth[ui] > ZERO_DEPTH)
                    c = c_old;
                else
                    c = 0.0;
                if (q_in > LEGACY_ZERO)
                    c += reactionBoosterMass(ctx, i, int(s), std::max(c, 0.) * v_in, v_in, v_in) /
                         v_in;
                rx.msx_node_conc[ui * uns + s] = std::max(c, 0.0);
                continue;
            }

            led.seepage[s] += v_exfil * c_old;
            const double c1 = c_old * f_evap;
            double c_new;
            if (q_in <= LEGACY_ZERO) {
                c_new = zero_bf ? 0.0 : c1;
            } else {
                const double c_in = mass / v_in;
                const double c_max = std::max(c1, c_in);
                c_new = (c1 * v_old + mass) / (v_old + v_in);
                c_new = std::min(c_new, c_max);
            }
            const double mixed = v_old + v_in;
            if (!dry && mixed > 1e-12)
                c_new +=
                    reactionBoosterMass(ctx, i, int(s), std::max(c_new, 0.) * mixed, mixed, v_in) /
                    mixed;
            if (dry) {
                led.final_dry[s] += c_new * ctx.nodes.volume[ui];
                c_new = 0.0;
            }
            rx.msx_node_conc[ui * uns + s] = std::max(c_new, 0.0);
        }
    }

    // ---- 4. Link update (updateLinkQuality mirror; k = 0; seepage and
    //         evaporation as for a pollutant). ------------------------------
    const bool is_steady = (ctx.options.routing_model == RoutingModel::STEADY);
    for (int j = 0; j < nl; ++j) {
        const auto uj = static_cast<std::size_t>(j);
        // Steady flow draws on node1 unconditionally (findSFLinkQual).
        const int up =
            is_steady ? ctx.links.node1[uj]
                      : ((ctx.links.flow[uj] >= 0.0) ? ctx.links.node1[uj] : ctx.links.node2[uj]);
        if (up < 0 || up >= nn)
            continue;
        const auto uup = static_cast<std::size_t>(up);

        // A non-conduit or DUMMY link holds no water: upstream outright.
        const bool passthrough = quality::linkTakesUpstreamValue(ctx, j);
        const double v_old = ctx.links.old_volume[uj];
        const double q_in = passthrough ? 0.0 : quality::conduitMixingInflow(ctx, j, dt);
        const bool dry = !is_steady && !passthrough && quality::linkIsDry(ctx, j);
        // Review B14: the conduit's seepage and evaporation, exactly as
        // updateLinkQuality treats a pollutant — seepage booked at the
        // stored concentration, the store concentrated by fEvap.
        double q_seep = 0.0, f_evap = 1.0;
        if (!passthrough) {
            const int cr = ctx.link_subtypes.conduit_row(j);
            if (cr >= 0) {
                const auto ucr = static_cast<std::size_t>(cr);
                const auto& CD = ctx.link_subtypes.conduits;
                const double barrels = static_cast<double>(CD.barrels[ucr]);
                q_seep = CD.seep_loss_rate[ucr] * barrels;
                const double v_evap = CD.evap_loss_rate[ucr] * barrels * dt;
                if (v_evap > 0.0 && v_old > quality::ZERO_VOLUME)
                    f_evap += v_evap / v_old;
            }
        }

        for (std::size_t s = 0; s < uns; ++s) {
            if (rx.species_is_wall[s])
                continue; // attached state never mixes with water
            const double c_old = sc.link_old[uj * uns + s];
            const double c_up = msxlid::outlet(ctx, up, j, int(s), rx.msx_node_conc[uup * uns + s]);
            double c_new;
            if (passthrough) {
                c_new = c_up;
            } else if (is_steady) {
                led.seepage[s] += q_seep * c_up * dt;
                c_new = c_up * f_evap;
            } else {
                led.seepage[s] += q_seep * c_old * dt;
                const double c1 = c_old * f_evap;
                if (q_in <= LEGACY_ZERO) {
                    c_new = c1;
                } else {
                    const double v_in = q_in * dt;
                    const double c_max = std::max(c1, c_up);
                    c_new = (c1 * v_old + c_up * v_in) / (v_old + v_in);
                    c_new = std::min(c_new, c_max);
                }
            }
            if (dry) {
                led.final_dry[s] += c_new * ctx.links.volume[uj];
                c_new = 0.0;
            }
            rx.msx_link_conc[uj * uns + s] = std::max(c_new, 0.0);
        }
    }
}

/**
 * @brief Convert accepted queued coupling inventory into native external mass rate for this
 * substep.
 * @param[in,out] ctx Reaction queue [node*species+m], external native rate array and held
 * coupling_delivery_remaining seconds; remaining duration is read, not decremented here.
 * @param[in] dt Positive finite accepted routing subinterval s; empty/no species/nonpositive/NaN
 * returns.
 * @note Queue shape mismatch returns untouched. External shape mismatch resets destination0.
 * When remaining>dt releases queued/remaining rate and subtracts rate*dt; otherwise drains all
 * queued/dt. Signed inventory supported, no finite clamp. Native concentration*ft3 units, no
 * litre conversion, physical water/clock/ledger mutation. Allocation can partially reset;
 * exclusive exactly-once receipt consumption.
 */
void drainMsxCouplingQueue(SimulationContext& ctx, double dt) {
    auto& rx = ctx.reactions;
    auto& q = rx.msx_coupling_queue;
    const int nsp = rx.n_species();
    if (q.empty() || nsp <= 0 || !(dt > 0.0))
        return;
    const auto want = static_cast<std::size_t>(ctx.n_nodes()) * static_cast<std::size_t>(nsp);
    if (q.size() != want)
        return;
    if (rx.msx_ext_mass_in.size() != want)
        rx.msx_ext_mass_in.assign(want, 0.0);
    // assembleLateralInflows' drain_queue rule, read at the same span: this
    // runs after msxsurf::deliver and before the span is advanced, so the
    // species mass leaves the queue at the rate the coupled volume does.
    const double remaining = ctx.coupling_delivery_remaining;
    for (std::size_t k = 0; k < want; ++k) {
        double& qm = q[k];
        if (qm == 0.0)
            continue;
        double rate;
        if (remaining > dt) {
            rate = qm / remaining;
            qm -= rate * dt;
        } else {
            rate = qm / dt;
            qm = 0.0;
        }
        rx.msx_ext_mass_in[k] += rate;
    }
}

/**
 * @brief Add native BULK species inventory carried by accepted system discharge to continuity
 * ledger.
 * @param[in,out] ctx Armed native MSX ledger and stable node concentrations/species flags.
 * @param[in] node Native node ID; negative/uncovered returns.
 * @param[in] q Positive finite outflow ft3/s; nonpositive/NaN returns.
 * @param[in] flooding True books flooding row, false regular outflow.
 * @param[in] dt Matching accepted positive finite duration s, not independently checked.
 * @note Adds q*c*dt concentration*ft3, skips WALL. Requires flooding/outflow row consistency
 * (only outflow shape explicitly checked). No node inventory/water removal, queue consumption,
 * allocation or clock advance; caller owns exactly-once accepted flux ledger booking.
 */
void bookMsxSystemOutflow(SimulationContext& ctx, int node, double q, bool flooding, double dt) {
    auto& rx = ctx.reactions;
    auto& led = rx.msx_ledger;
    const int nsp = rx.n_species();
    if (!led.armed || nsp <= 0 || !(q > 0.0) || node < 0)
        return;
    const auto uns = static_cast<std::size_t>(nsp);
    const auto base = static_cast<std::size_t>(node) * uns;
    if (base + uns > rx.msx_node_conc.size() || led.outflow.size() != uns)
        return;
    auto& row = flooding ? led.flooding : led.outflow;
    for (std::size_t s = 0; s < uns; ++s)
        if (!rx.species_is_wall[s])
            row[s] += q * rx.msx_node_conc[base + s] * dt;
}

} // namespace openswmm::transport
