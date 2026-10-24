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
 * @file      ReactionLegacyBinding.cpp
 * @brief     Phase R4 body — LEGACY quality engine reaction binding.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "ReactionLegacyBinding.hpp"
#include "../../../quality/QualityRouting.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "ReactionIntegrator.hpp"
#include "ReactionGeometry.hpp"
#include "../../../hydrology/LidNode.hpp"

namespace openswmm::transport {

namespace {

/// One workspace + block per thread of execution; the binding runs on the
/// routing thread only, so a function-local static suffices. Re-inited when
/// the species count changes (model reopen).
struct BindingScratch {
    RxWorkspace ws;
    std::vector<double> block, added, coefficients, wall_scales;
    int sized_for = -1, sized_terms = -1;
    /**
     * @brief Resize legacy native-kinetics workspace when species or term count changes.
     * @param[in] rx Stable compiled dimensions; resets workspace/block/clamp scratch on
     * mismatch.
     * @note Does not detect changed laws with equal dimensions or reset coefficient/wall-scale
     * buffers. Allocation can partially reconfigure; process-static owner requires serial calls
     * across contexts.
     */
    void ensure(const ReactionData& rx) {
        if (sized_for != rx.n_species() || sized_terms != static_cast<int>(rx.term_name.size())) {
            sized_terms = static_cast<int>(rx.term_name.size());
            ws.init(rx);
            block.assign(static_cast<std::size_t>(rx.n_species()), 0.0);
            added.assign(static_cast<std::size_t>(rx.n_species()), 0.0);
            sized_for = rx.n_species();
        }
    }
};

/**
 * @brief Borrow the process-wide legacy/LARD reaction-binding workspace.
 * @return Static nonowned BindingScratch reference, process lifetime. No synchronization for
 * mutable use; serialize all reactElements/reactSpeciesBlock calls across models.
 */
BindingScratch& scratch() {
    static BindingScratch s;
    return s;
}

} // namespace

/**
 * @brief Lazily size + seed the MSX element state; warn once about the R4b transport limitation
 * when any RATE MSX species exists in either scope (LEGACY only — under LARD the species ride
 * the segments as of L3).
 *
 * @details Seed native node/link species state when node-shaped storage differs from current
 * model.
 * @param[in,out] ctx Stable species/global/element initial rows and model counts; mismatch
 * replaces node/link concentration vectors and applies covered element overrides after global
 * seeds.
 * @note Only node vector shape triggers reset; matching node shape with wrong link shape is not
 * repaired. Requires covered parallel seed arrays. No configured/compiled/quality flag check,
 * ledger, water or clock advance. Allocation may partially reset; exclusive initial lifecycle
 * before routing.
 */
void ensureMsxState(SimulationContext& ctx) {
    auto& rx = ctx.reactions;
    const auto ns = static_cast<std::size_t>(rx.n_species());
    const auto want_nodes = static_cast<std::size_t>(ctx.n_nodes()) * ns;
    const auto want_links = static_cast<std::size_t>(ctx.n_links()) * ns;
    if (rx.msx_node_conc.size() != want_nodes) {
        rx.msx_node_conc.assign(want_nodes, 0.0);
        rx.msx_link_conc.assign(want_links, 0.0);
        for (std::size_t e = 0; e < static_cast<std::size_t>(ctx.n_nodes()); ++e)
            for (std::size_t s = 0; s < ns; ++s)
                rx.msx_node_conc[e * ns + s] = rx.init_global[s];
        for (std::size_t e = 0; e < static_cast<std::size_t>(ctx.n_links()); ++e)
            for (std::size_t s = 0; s < ns; ++s)
                rx.msx_link_conc[e * ns + s] = rx.init_global[s];
        // E-B2: [REACTION_QUALITY] NODE/LINK rows override the GLOBAL fill.
        for (std::size_t k = 0; k < rx.init_elem_idx.size(); ++k) {
            const auto e = static_cast<std::size_t>(rx.init_elem_idx[k]);
            const auto s = static_cast<std::size_t>(rx.init_elem_species[k]);
            if (s >= ns)
                continue;
            auto& arr = rx.init_elem_is_link[k] ? rx.msx_link_conc : rx.msx_node_conc;
            if (e * ns + s < arr.size())
                arr[e * ns + s] = rx.init_elem_value[k];
        }
    }
    // R4b (2026-09-01): the not-transported warning is GONE, not narrowed —
    // routeLegacyMsx advects the element state on the CSTR mirror family
    // (QualityRouting.cpp end-of-execute), so RATE species now arrive
    // downstream under every engine.
}

/**
 * @brief Review A2: one warning per run when the reaction step had to clamp a negative
 * concentration. The LEGACY binding also books the mass the clamp added (msx_ledger.clamped);
 * the other engines report it here only.
 *
 * @details Append one context-wide diagnostic after a negative RATE result is floored.
 * @param[in,out] ctx Reaction warning latch and warnings list; latch set before string append.
 * @param[in] where Nonnull null-terminated element category, read synchronously/copied into
 * warning.
 * @note Does not clamp inventories or book mass itself. Once-latched returns; allocation failure
 * can leave latch set without warning. Exclusive context diagnostic access, no water/clock
 * change.
 */
void noteNegativeClamp(SimulationContext& ctx, const char* where) {
    auto& rx = ctx.reactions;
    if (rx.warned_negative_clamp)
        return;
    rx.warned_negative_clamp = true;
    ctx.warnings.push_back(std::string("Reaction step produced a negative concentration at ") +
                           where +
                           "; it was set to zero (mass added). The explicit solver is "
                           "too coarse for these kinetics: use [REACTION_OPTIONS] SOLVER ROS2, "
                           "COUPLING FULL or a shorter step.");
}

/**
 * @brief Floor negative RATE species only and optionally report each concentration addition.
 * @param[in] rx Stable n_species and selected pipe/tank form arrays.
 * @param[in] tank True selects tank forms, false pipe.
 * @param[in,out] block Writable n_species concentrations, finite expected; negative RATE
 * becomes0, EQUIL/FORMULA/NONE remain unchanged even if negative.
 * @param[out] added Optional n_species tuple replaced with added concentration per row (not
 * inventory).
 * @return Sum of concentration additions across rows, not a unit-homogeneous physical mass when
 * row units differ. No ledger/clock/warning, finite repair or allocation; caller supplies
 * nonaliasing output.
 */
double clampRateSpecies(const ReactionData& rx, bool tank, double* block, double* added) {
    // Review A2 (2026-10-15): an explicit step on stiff kinetics can
    // overshoot below zero; downstream mixing then clamped it silently and
    // created mass with no record. Clamp once, where it is produced, and
    // return what was added. Only RATE species — EQUIL and FORMULA species
    // may legitimately be signed. The integrator itself stays the pure
    // numerical method its gates pin.
    const auto& forms = tank ? rx.tank_form : rx.pipe_form;
    double total = 0.0;
    const int ns = rx.n_species();
    for (int s = 0; s < ns; ++s) {
        const auto us = static_cast<std::size_t>(s);
        double a = 0.0;
        if (us < forms.size() && forms[us] == ReactionExprForm::RATE && block[s] < 0.0) {
            a = -block[s];
            block[s] = 0.0;
        }
        if (added)
            added[s] = a;
        total += a;
    }
    return total;
}

namespace {

/**
 * @brief Exact exponential pollutant decay: c *= exp(-k*dt), clamp at 0. The closed form of
 * first-order decay — replaces the legacy linearized (1 - k*dt) when reactions are active.
 *
 * @details Apply exact exponential pollutant decay and book represented concentration-volume
 * loss.
 * @param[in,out] ctx Matching pollutant decay constants1/s and reacted ledger.
 * @param[in] dt Positive finite reaction seconds, caller qualifies.
 * @param[in,out] conc Element-major n_elems*np concentration array; uncovered tuples skip.
 * @param[in] n_elems Nonnegative element count.
 * @param[in] volume Covered native ft3 per element; missing volume uses0 but concentration still
 * decays.
 * @param[in] node_types Optional type array; covered nonstorage node with volume<=one litre
 * skips, null selects link behavior without that skip.
 * @note Multiplier exp(-k*dt), signed ledger loss supports growth k<0. Negative final
 * concentration floors0 without separate clamp ledger. No hydraulic/clock change or allocation;
 * exclusive once-only kinetics.
 */
void decayPollutantsExact(SimulationContext& ctx, double dt, std::vector<double>& conc, int n_elems,
                          const std::vector<double>& volume,
                          const std::vector<NodeType>* node_types) {
    const int np = ctx.n_pollutants();
    for (int p = 0; p < np; ++p) {
        const double k = ctx.pollutants.k_decay[static_cast<std::size_t>(p)];
        if (k == 0.0)
            continue;
        const double f = std::exp(-k * dt);
        double removed = 0.0; // KD1: book the decayed mass
        for (int i = 0; i < n_elems; ++i) {
            const auto idx = static_cast<std::size_t>(i) * static_cast<std::size_t>(np) +
                             static_cast<std::size_t>(p);
            if (idx >= conc.size())
                continue;
            const auto ui = static_cast<std::size_t>(i);
            const double v = (ui < volume.size()) ? volume[ui] : 0.0;
            // KD1 legacy parity: a non-storage node holding no volume
            // does not decay (qualrout.c findNodeQual decays nothing).
            if (node_types && ui < node_types->size() && (*node_types)[ui] != NodeType::STORAGE &&
                v <= 0.0353147) // ZERO_VOLUME (QualityRouting.hpp)
                continue;
            removed += conc[idx] * (1.0 - f) * v;
            conc[idx] *= f;
            if (conc[idx] < 0.0)
                conc[idx] = 0.0;
        }
        if (static_cast<std::size_t>(p) < ctx.mass_balance.qual_routing_reacted.size())
            ctx.mass_balance.qual_routing_reacted[static_cast<std::size_t>(p)] += removed;
    }
}

/**
 * @brief Integrate native species element-by-element with transactional per-element scratch
 * acceptance.
 * @param[in,out] ctx Compiled reaction system, node/link geometry/temp/HRT and MSX
 * reacted/clamped ledgers; can append once-only failure/clamp warning.
 * @param[in] dt Positive finite accepted reaction duration s.
 * @param[in] tank True node/tank scope, false link/pipe scope.
 * @param[in,out] msx_conc Covered n_elems*n_species element-major concentrations, writable
 * exclusively.
 * @param[in] pollutant_conc Covered n_elems*np current pollutant concentrations, stable during
 * integration.
 * @param[in] n_elems Native element count matching selected node/link families.
 * @param[in] what Nonnull null-terminated element label for warnings.
 * @note Node nonreactors/dry pools and separately opted native LID reactors skip; wall geometry
 * also requires wet represented volume and supplies AV plus active/reference kinetic scaling.
 * Per-element coefficient overrides resolved. Failed report preserves that element species;
 * accepted solve books BULK old-raw volume loss and RATE negative clamp addition before
 * publishing clipped tuple. Other elements continue. Warning/allocation exceptions can follow
 * ledger changes; no global rollback. Static binding workspace prohibits concurrent calls across
 * contexts. No hydraulic/clock advance.
 */
void reactElements(SimulationContext& ctx, double dt, bool tank, std::vector<double>& msx_conc,
                   const std::vector<double>& pollutant_conc, int n_elems, const char* what) {
    auto& rx = ctx.reactions;
    const int ns = rx.n_species();
    if (ns == 0)
        return;
    auto& sc = scratch();
    sc.ensure(rx);

    const int np = ctx.n_pollutants();
    double hydvar[static_cast<int>(RxHydVar::COUNT_)] = {};

    // TEMP source: the heat engines mirror element temperatures into
    // heat_state (node_temp / link_temp, degC) whichever router runs;
    // absent that (HEAT_TRANSPORT off), the [REACTION_OPTIONS]
    // TEMPERATURE constant applies.
    const auto& elem_temp = tank ? ctx.heat_state.node_temp : ctx.heat_state.link_temp;
    // Review B14: the routing ledger books what the reaction step does to
    // each element's mass, on the element's current volume.
    auto& led = rx.msx_ledger;
    const bool book = led.armed && led.reacted.size() == static_cast<std::size_t>(ns);
    const auto& volume = tank ? ctx.nodes.volume : ctx.links.volume;

    for (int e = 0; e < n_elems; ++e) {
        const auto ue = static_cast<std::size_t>(e);
        const auto base = ue * static_cast<std::size_t>(ns);
        // Review A5 (2026-10-15): a flow-through node holds no water for
        // quality (legacy findNodeQual: concentration is the inflow's, and
        // nothing reacts there — the pollutant path's KD1 gate). Reacting
        // its concentration in tank scope removed mass from water passing
        // through on a volume basis it does not have.
        if (tank && (!quality::nodeIsReactor(ctx, e) || ue >= volume.size() ||
                     volume[ue] <= quality::ZERO_VOLUME))
            continue;
        if (rx.wall_geometry && (ue >= volume.size() || volume[ue] <= quality::ZERO_VOLUME))
            continue;
        if (tank && lidnode::active(ctx, e)) {
            const int row = ctx.node_subtypes.storage_row(e);
            if (row >= 0 && rx.lid_enabled(ctx.node_subtypes.storages.lid[row].control))
                continue;
        }

        // HRT for nodes (treatment-variable parity); zero elsewhere in R4 —
        // full hydraulic-variable population is the ARD binding's job (R6).
        hydvar[static_cast<int>(RxHydVar::HRT)] =
            (tank && ue < ctx.nodes.hrt.size()) ? ctx.nodes.hrt[ue] : 0.0;
        hydvar[static_cast<int>(RxHydVar::TEMP)] =
            (ctx.options.heat_transport && ue < elem_temp.size()) ? elem_temp[ue]
                                                                  : rx.default_temp_c;
        hydvar[static_cast<int>(RxHydVar::AV)] =
            rx.wall_geometry ? reactionAreaVolume(ctx, !tank, e, volume[ue]) : 0.;
        if (rx.wall_geometry)
            reactionWallScales(ctx, !tank, e, volume[ue], sc.wall_scales);

        for (int s = 0; s < ns; ++s)
            sc.block[static_cast<std::size_t>(s)] = msx_conc[base + static_cast<std::size_t>(s)];

        const double* pollut =
            (np > 0) ? &pollutant_conc[ue * static_cast<std::size_t>(np)] : nullptr;

        const auto rep = ReactionIntegrator::step(
            rx, tank ? rx.tank_form : rx.pipe_form, tank ? rx.tank_expr : rx.pipe_expr, dt,
            sc.block.data(), hydvar, sc.ws, pollut,
            rx.wall_geometry ? sc.wall_scales.data() : nullptr,
            rx.coefficients(!tank, e, sc.coefficients));

        if (rep.ok) {
            const double v = (ue < volume.size()) ? volume[ue] : 0.0;
            // Net reacted = what the kinetics removed (before the clamp);
            // the clamp's addition is booked on its own row.
            if (book)
                for (int s = 0; s < ns; ++s) {
                    const auto us = static_cast<std::size_t>(s);
                    if (!rx.species_is_wall[us])
                        led.reacted[us] += (msx_conc[base + us] - sc.block[us]) * v;
                }
            if (clampRateSpecies(rx, tank, sc.block.data(), sc.added.data()) > 0.0) {
                noteNegativeClamp(ctx, what);
                if (book)
                    for (int s = 0; s < ns; ++s)
                        if (!rx.species_is_wall[static_cast<std::size_t>(s)])
                            led.clamped[static_cast<std::size_t>(s)] +=
                                sc.added[static_cast<std::size_t>(s)] * v;
            }
            for (int s = 0; s < ns; ++s)
                msx_conc[base + static_cast<std::size_t>(s)] =
                    sc.block[static_cast<std::size_t>(s)];
        } else if (!rx.warned_react_failure) {
            rx.warned_react_failure = true;
            ctx.warnings.push_back(std::string("Reaction step failed at ") + what + " index " +
                                   std::to_string(e) +
                                   " (element state left unchanged): " + rep.error);
        }
    }
}

} // namespace

/**
 * @brief React one LARD-owned tuple with held HRT/temperature and optional element geometry.
 * @param[in,out] ctx Stable compiled reaction data/coefficients and once-only diagnostic
 * flags/list.
 * @param[in] tank True tank laws, false pipe laws.
 * @param[in] dt Accepted positive finite reaction duration s, forwarded to integrator.
 * @param[in,out] species_block Writable n_species local tuple, retained unchanged on returned
 * solve failure; accepted RATE negatives floor0. Must not alias private static
 * workspace/pollutant input.
 * @param[in] pollut Optional stable np current concentrations required by referenced tokens.
 * @param[in] hrt_seconds Held reactor HRT seconds.
 * @param[in] temp_c Held degC; nonfinite selects reaction default.
 * @param[in] element Matching native node/link ID or negative for no element geometry/override
 * lookup; when wall geometry enabled nonnegative ID must be valid.
 * @note Unlike reactElements, does not book MSX reacted/clamped inventory or apply wall coverage
 * kinetic scaling. Copies through shared static scratch; failure warns once, no water/clock
 * mutation. Allocation/diagnostic exceptions propagate and all bindings across contexts require
 * serialization.
 */
void reactSpeciesBlock(SimulationContext& ctx, bool tank, double dt, double* species_block,
                       const double* pollut, double hrt_seconds, double temp_c, int element) {
    auto& rx = ctx.reactions;
    const int ns = rx.n_species();
    if (ns == 0)
        return;
    auto& sc = scratch();
    sc.ensure(rx);
    double hydvar[static_cast<int>(RxHydVar::COUNT_)] = {};
    hydvar[static_cast<int>(RxHydVar::HRT)] = hrt_seconds;
    hydvar[static_cast<int>(RxHydVar::TEMP)] = std::isfinite(temp_c) ? temp_c : rx.default_temp_c;
    if (rx.wall_geometry && element >= 0)
        hydvar[static_cast<int>(RxHydVar::AV)] = reactionAreaVolume(
            ctx, !tank, element, tank ? ctx.nodes.volume[element] : ctx.links.volume[element]);
    for (int s = 0; s < ns; ++s)
        sc.block[static_cast<std::size_t>(s)] = species_block[s];
    const auto rep = ReactionIntegrator::step(rx, tank, dt, sc.block.data(), hydvar, sc.ws, pollut,
                                              rx.coefficients(!tank, element, sc.coefficients));
    if (rep.ok) {
        if (clampRateSpecies(rx, tank, sc.block.data(), nullptr) > 0.0)
            noteNegativeClamp(ctx, "a LARD element");
        for (int s = 0; s < ns; ++s)
            species_block[s] = sc.block[static_cast<std::size_t>(s)];
    } else if (!rx.warned_react_failure) {
        rx.warned_react_failure = true;
        ctx.warnings.push_back(std::string("Reaction step failed on a LARD element "
                                           "(element state left unchanged): ") +
                               rep.error);
    }
}

/**
 * @brief Test configured and compiled native reaction readiness.
 * @param[in] ctx Stable reaction flags.
 * @return configured and compiled; no species-count/quality-option/shape validation or mutation.
 */
bool legacyReactionsActive(const SimulationContext& ctx) {
    return ctx.reactions.configured && ctx.reactions.compiled;
}

/**
 * @brief Run legacy-bound exact pollutant decay and native node kinetics once.
 * @param[in,out] ctx Configured/compiled reactions and matching prepared native
 * species/pollutant, hydraulic, thermal/geometry and mass-ledger arrays; seeds species only if
 * node-shaped state mismatches.
 * @param[in] dt Positive finite accepted routing duration s; inactive/nonpositive returns.
 * @note Exact pollutant decay runs before native laws; nodes skip tiny nonstorage pools, native
 * node reactions also skip separately reactive LID ownership. Failed native element solve
 * retains species and warns, pollutant decay remains applied. No water/clock advance,
 * cross-element rollback or concurrent static workspace access. Allocation/warning failures can
 * leave partial context changes.
 */
void reactLegacyNodes(SimulationContext& ctx, double dt) {
    if (!legacyReactionsActive(ctx) || dt <= 0.0)
        return;
    ensureMsxState(ctx);
    decayPollutantsExact(ctx, dt, ctx.nodes.conc, ctx.n_nodes(), ctx.nodes.volume, &ctx.nodes.type);
    reactElements(ctx, dt, /*tank=*/true, ctx.reactions.msx_node_conc, ctx.nodes.conc,
                  ctx.n_nodes(), "node");
}

/**
 * @brief Run legacy-bound exact pollutant decay and native link kinetics once.
 * @param[in,out] ctx Configured/compiled reactions and matching prepared native
 * species/pollutant, hydraulic, thermal/geometry and mass-ledger arrays; seeds species only if
 * node-shaped state mismatches.
 * @param[in] dt Positive finite accepted routing duration s; inactive/nonpositive returns.
 * @note Exact pollutant decay runs before native laws; nodes skip tiny nonstorage pools, native
 * node reactions also skip separately reactive LID ownership. Failed native element solve
 * retains species and warns, pollutant decay remains applied. No water/clock advance,
 * cross-element rollback or concurrent static workspace access. Allocation/warning failures can
 * leave partial context changes.
 */
void reactLegacyLinks(SimulationContext& ctx, double dt) {
    if (!legacyReactionsActive(ctx) || dt <= 0.0)
        return;
    ensureMsxState(ctx);
    decayPollutantsExact(ctx, dt, ctx.links.conc, ctx.n_links(), ctx.links.volume, nullptr);
    reactElements(ctx, dt, /*tank=*/false, ctx.reactions.msx_link_conc, ctx.links.conc,
                  ctx.n_links(), "link");
}

/**
 * @brief Warn when configured compiled reactions are bypassed by IGNORE_QUALITY.
 * @param[in,out] ctx Reaction readiness and option flags; appends explanatory warning each
 * qualifying call.
 * @note No once-only latch, state seeding or clock change. Warning string may allocate;
 * exclusive setup diagnostics.
 */
void warnIfLegacyBindingBypassed(SimulationContext& ctx) {
    if (!legacyReactionsActive(ctx))
        return;
    if (ctx.options.ignore_quality) {
        ctx.warnings.push_back("A reactions component is configured but IGNORE_QUALITY is YES — "
                               "the quality stage does not run, so no species react and no "
                               "pollutant decays this simulation.");
    }
    // EULERIAN_ARD is no longer a bypass: the ARD engine runs its own
    // reaction binding as of E4/R6 (ReactionArdBinding), with MSX species
    // transported on the mesh. The R4-era warning is retired.
}

} // namespace openswmm::transport
