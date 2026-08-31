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
 * @file ReactionLegacyBinding.cpp
 * @brief Phase R4 body — LEGACY quality engine reaction binding.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ReactionLegacyBinding.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "ReactionIntegrator.hpp"

namespace openswmm::transport {

namespace {

/// One workspace + block per thread of execution; the binding runs on the
/// routing thread only, so a function-local static suffices. Re-inited when
/// the species count changes (model reopen).
struct BindingScratch {
    RxWorkspace ws;
    std::vector<double> block;
    int sized_for = -1;
    void ensure(const ReactionData& rx) {
        if (sized_for != rx.n_species()) {
            ws.init(rx);
            block.assign(static_cast<std::size_t>(rx.n_species()), 0.0);
            sized_for = rx.n_species();
        }
    }
};

BindingScratch& scratch() {
    static BindingScratch s;
    return s;
}

/// Lazily size + seed the MSX element state; warn once about the R4b
/// transport limitation when any RATE MSX species exists in either scope.
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
            if (s >= ns) continue;
            auto& arr = rx.init_elem_is_link[k] ? rx.msx_link_conc
                                                : rx.msx_node_conc;
            if (e * ns + s < arr.size()) arr[e * ns + s] =
                rx.init_elem_value[k];
        }
    }
    if (!rx.warned_msx_not_transported) {
        bool any_rate = false;
        for (int s = 0; s < rx.n_species(); ++s) {
            const auto us = static_cast<std::size_t>(s);
            if (rx.pipe_form[us] == ReactionExprForm::RATE ||
                rx.tank_form[us] == ReactionExprForm::RATE)
                any_rate = true;
        }
        if (any_rate) {
            rx.warned_msx_not_transported = true;
            ctx.warnings.push_back(
                "Reactions under QUALITY_SOLVER LEGACY: RATE species react "
                "per element but are not yet transported between elements "
                "(arrives with plan phase R4b) — EQUIL/FORMULA species and "
                "pollutant decay are fully supported.");
        }
    }
}

/// Exact exponential pollutant decay: c *= exp(-k*dt), clamp at 0. The
/// closed form of first-order decay — replaces the legacy linearized
/// (1 - k*dt) when reactions are active.
void decayPollutantsExact(SimulationContext& ctx, double dt,
                          std::vector<double>& conc, int n_elems,
                          const std::vector<double>& volume,
                          const std::vector<NodeType>* node_types) {
    const int np = ctx.n_pollutants();
    for (int p = 0; p < np; ++p) {
        const double k = ctx.pollutants.k_decay[static_cast<std::size_t>(p)];
        if (k == 0.0) continue;
        const double f = std::exp(-k * dt);
        double removed = 0.0;  // KD1: book the decayed mass
        for (int i = 0; i < n_elems; ++i) {
            const auto idx = static_cast<std::size_t>(i) *
                             static_cast<std::size_t>(np) +
                             static_cast<std::size_t>(p);
            if (idx >= conc.size()) continue;
            const auto ui = static_cast<std::size_t>(i);
            const double v = (ui < volume.size()) ? volume[ui] : 0.0;
            // KD1 legacy parity: a non-storage node holding no volume
            // does not decay (qualrout.c findNodeQual decays nothing).
            if (node_types && ui < node_types->size() &&
                (*node_types)[ui] != NodeType::STORAGE &&
                v <= 0.0353147)  // ZERO_VOLUME (QualityRouting.hpp)
                continue;
            removed += conc[idx] * (1.0 - f) * v;
            conc[idx] *= f;
            if (conc[idx] < 0.0) conc[idx] = 0.0;
        }
        if (static_cast<std::size_t>(p) <
            ctx.mass_balance.qual_routing_reacted.size())
            ctx.mass_balance.qual_routing_reacted[
                static_cast<std::size_t>(p)] += removed;
    }
}

void reactElements(SimulationContext& ctx, double dt, bool tank,
                   std::vector<double>& msx_conc,
                   const std::vector<double>& pollutant_conc, int n_elems,
                   const char* what) {
    auto& rx = ctx.reactions;
    const int ns = rx.n_species();
    if (ns == 0) return;
    auto& sc = scratch();
    sc.ensure(rx);

    const int np = ctx.n_pollutants();
    double hydvar[static_cast<int>(RxHydVar::COUNT_)] = {};

    for (int e = 0; e < n_elems; ++e) {
        const auto ue = static_cast<std::size_t>(e);
        const auto base = ue * static_cast<std::size_t>(ns);

        // HRT for nodes (treatment-variable parity); zero elsewhere in R4 —
        // full hydraulic-variable population is the ARD binding's job (R6).
        hydvar[static_cast<int>(RxHydVar::HRT)] =
            (tank && ue < ctx.nodes.hrt.size()) ? ctx.nodes.hrt[ue] : 0.0;

        for (int s = 0; s < ns; ++s)
            sc.block[static_cast<std::size_t>(s)] =
                msx_conc[base + static_cast<std::size_t>(s)];

        const double* pollut =
            (np > 0) ? &pollutant_conc[ue * static_cast<std::size_t>(np)]
                     : nullptr;

        const auto rep = ReactionIntegrator::step(
            rx, tank, dt, sc.block.data(), hydvar, sc.ws, pollut);

        if (rep.ok) {
            for (int s = 0; s < ns; ++s)
                msx_conc[base + static_cast<std::size_t>(s)] =
                    sc.block[static_cast<std::size_t>(s)];
        } else if (!rx.warned_react_failure) {
            rx.warned_react_failure = true;
            ctx.warnings.push_back(
                std::string("Reaction step failed at ") + what + " index " +
                std::to_string(e) + " (element state left unchanged): " +
                rep.error);
        }
    }
}

}  // namespace

bool legacyReactionsActive(const SimulationContext& ctx) {
    return ctx.reactions.configured && ctx.reactions.compiled;
}

void reactLegacyNodes(SimulationContext& ctx, double dt) {
    if (!legacyReactionsActive(ctx) || dt <= 0.0) return;
    ensureMsxState(ctx);
    decayPollutantsExact(ctx, dt, ctx.nodes.conc, ctx.n_nodes(),
                         ctx.nodes.volume, &ctx.nodes.type);
    reactElements(ctx, dt, /*tank=*/true, ctx.reactions.msx_node_conc,
                  ctx.nodes.conc, ctx.n_nodes(), "node");
}

void reactLegacyLinks(SimulationContext& ctx, double dt) {
    if (!legacyReactionsActive(ctx) || dt <= 0.0) return;
    ensureMsxState(ctx);
    decayPollutantsExact(ctx, dt, ctx.links.conc, ctx.n_links(),
                         ctx.links.volume, nullptr);
    reactElements(ctx, dt, /*tank=*/false, ctx.reactions.msx_link_conc,
                  ctx.links.conc, ctx.n_links(), "link");
}

void warnIfLegacyBindingBypassed(SimulationContext& ctx) {
    if (!legacyReactionsActive(ctx)) return;
    if (ctx.options.ignore_quality) {
        ctx.warnings.push_back(
            "A reactions component is configured but IGNORE_QUALITY is YES — "
            "the quality stage does not run, so no species react and no "
            "pollutant decays this simulation.");
    }
    // EULERIAN_ARD is no longer a bypass: the ARD engine runs its own
    // reaction binding as of E4/R6 (ReactionArdBinding), with MSX species
    // transported on the mesh. The R4-era warning is retired.
}

}  // namespace openswmm::transport
