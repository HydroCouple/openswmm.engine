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
 * @file ReactionArdBinding.cpp
 * @brief Phase E4/R6 body — Eulerian ARD engine reaction stage.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "ReactionArdBinding.hpp"

#include <cmath>
#include <string>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "ReactionIntegrator.hpp"

namespace openswmm::transport {

namespace {

/// One workspace + gather blocks per thread of execution; the stage runs on
/// the routing thread only, so a function-local static suffices (the R4
/// BindingScratch pattern). Re-inited when the species count changes.
struct ArdScratch {
    RxWorkspace ws;
    std::vector<double> msx;     ///< [n_msx] gathered MSX block
    std::vector<double> pollut;  ///< [n_pollut] gathered pollutant block
    int sized_species = -1;
    int sized_pollut  = -1;
    void ensure(const ReactionData& rx, int np) {
        if (sized_species != rx.n_species()) {
            ws.init(rx);
            msx.assign(static_cast<std::size_t>(rx.n_species()), 0.0);
            sized_species = rx.n_species();
        }
        if (sized_pollut != np) {
            pollut.assign(static_cast<std::size_t>(np > 0 ? np : 1), 0.0);
            sized_pollut = np;
        }
    }
};

ArdScratch& scratch() {
    static ArdScratch s;
    return s;
}

void containFailure(SimulationContext& ctx, const char* what, int index,
                    const std::string& error) {
    if (ctx.reactions.warned_react_failure) return;
    ctx.reactions.warned_react_failure = true;
    ctx.warnings.push_back(std::string("Reaction step failed at ARD ") +
                           what + " index " + std::to_string(index) +
                           " (element state left unchanged): " + error);
}

}  // namespace

bool ardReactionsActive(const SimulationContext& ctx) {
    return ctx.reactions.configured && ctx.reactions.compiled;
}

bool ardHasWallSpecies(const SimulationContext& ctx) {
    for (const auto w : ctx.reactions.species_is_wall)
        if (w != 0) return true;
    return false;
}

void reactArdStage(SimulationContext& ctx, double dt, double* cell_phi,
                   const double* cell_a, const double* cell_dx, int n_cells,
                   double* node_mass, const double* node_vol, int n_nodes,
                   int n_pollut, int ns_total, double min_store_vol,
                   int temp_row) {
    if (dt <= 0.0) return;

    const auto unc = static_cast<std::size_t>(n_cells);
    const auto uns = static_cast<std::size_t>(ns_total);

    // ---- 1. Pollutant kdecay: exact exponential (closed form of the
    //         first-order implicit RATE; plan E4 "kdecay-as-RATE"). Applied
    //         to cell CONCENTRATIONS and store MASSES — both scale by the
    //         same factor, so store concentration follows. E5b: the removed
    //         mass books into the qual_routing_reacted ledger row (cells:
    //         a·dx·Δconc; stores: Δmass), closing the continuity gap the
    //         report would otherwise show as unexplained loss. ------------
    for (int p = 0; p < n_pollut; ++p) {
        const double k = ctx.pollutants.k_decay[static_cast<std::size_t>(p)];
        if (k == 0.0) continue;
        const double f = std::exp(-k * dt);
        const double loss_frac = 1.0 - f;
        double removed = 0.0;
        const auto row = static_cast<std::size_t>(p) * unc;
        for (int c = 0; c < n_cells; ++c) {
            const auto uc = row + static_cast<std::size_t>(c);
            const auto ucc = static_cast<std::size_t>(c);
            if (cell_phi[uc] > 0.0)
                removed += cell_a[ucc] * cell_dx[ucc] * cell_phi[uc] *
                           loss_frac;
            cell_phi[uc] *= f;
            if (cell_phi[uc] < 0.0) cell_phi[uc] = 0.0;
        }
        for (int nd = 0; nd < n_nodes; ++nd) {
            const auto ui =
                static_cast<std::size_t>(nd) * uns + static_cast<std::size_t>(p);
            if (node_mass[ui] > 0.0) removed += node_mass[ui] * loss_frac;
            node_mass[ui] *= f;
            if (node_mass[ui] < 0.0) node_mass[ui] = 0.0;
        }
        if (static_cast<std::size_t>(p) <
            ctx.mass_balance.qual_routing_reacted.size())
            ctx.mass_balance.qual_routing_reacted[static_cast<std::size_t>(p)] +=
                removed;
    }

    // ---- 2. MSX species: per-cell pipe scope, per-store tank scope. -----
    if (!ardReactionsActive(ctx)) return;
    auto& rx = ctx.reactions;
    const int nm = rx.n_species();
    // A1a: ns_total may exceed np + nm by the reserved __WATER_AGE__ row
    // (which reacts with nothing) — the MSX block still occupies rows
    // [np, np + nm). The old equality guard would have SILENTLY skipped
    // every reaction the moment the age row appeared (the lesson-14 shape:
    // a consistency check about one layout blocking another).
    if (nm == 0 || ns_total < n_pollut + nm) return;

    auto& sc = scratch();
    sc.ensure(rx, n_pollut);
    double hydvar[static_cast<int>(RxHydVar::COUNT_)] = {};

    // Cells (pipe scope). HRT is a node concept — zero here, matching R4's
    // link treatment; the fuller hydraulic-variable population is E5 work.
    for (int c = 0; c < n_cells; ++c) {
        const auto ucell = static_cast<std::size_t>(c);
        for (int m = 0; m < nm; ++m)
            sc.msx[static_cast<std::size_t>(m)] =
                cell_phi[static_cast<std::size_t>(n_pollut + m) * unc + ucell];
        for (int p = 0; p < n_pollut; ++p)
            sc.pollut[static_cast<std::size_t>(p)] =
                cell_phi[static_cast<std::size_t>(p) * unc + ucell];
        hydvar[static_cast<int>(RxHydVar::HRT)] = 0.0;
        // TEMP: the cell's own heat-transport temperature row when the mesh
        // carries one, else the [REACTION_OPTIONS] TEMPERATURE constant.
        hydvar[static_cast<int>(RxHydVar::TEMP)] =
            (temp_row >= 0)
                ? cell_phi[static_cast<std::size_t>(temp_row) * unc + ucell]
                : rx.default_temp_c;

        const auto rep = ReactionIntegrator::step(
            rx, /*tank=*/false, dt, sc.msx.data(), hydvar, sc.ws,
            (n_pollut > 0) ? sc.pollut.data() : nullptr);
        if (rep.ok) {
            for (int m = 0; m < nm; ++m)
                cell_phi[static_cast<std::size_t>(n_pollut + m) * unc + ucell] =
                    sc.msx[static_cast<std::size_t>(m)];
        } else {
            containFailure(ctx, "cell", c, rep.error);
        }
    }

    // Node stores (tank scope): integrate CONCENTRATIONS, write back MASS.
    for (int nd = 0; nd < n_nodes; ++nd) {
        const auto und = static_cast<std::size_t>(nd);
        const double vol = node_vol[und];
        if (vol <= min_store_vol) continue;  // no meaningful concentration
        for (int m = 0; m < nm; ++m)
            sc.msx[static_cast<std::size_t>(m)] =
                node_mass[und * uns + static_cast<std::size_t>(n_pollut + m)] /
                vol;
        for (int p = 0; p < n_pollut; ++p)
            sc.pollut[static_cast<std::size_t>(p)] =
                node_mass[und * uns + static_cast<std::size_t>(p)] / vol;
        hydvar[static_cast<int>(RxHydVar::HRT)] =
            (und < ctx.nodes.hrt.size()) ? ctx.nodes.hrt[und] : 0.0;
        // TEMP: node stores carry temperature-volume (degC·ft³) on the
        // temperature row, so concentration-form division recovers degC.
        hydvar[static_cast<int>(RxHydVar::TEMP)] =
            (temp_row >= 0)
                ? node_mass[und * uns + static_cast<std::size_t>(temp_row)] /
                      vol
                : rx.default_temp_c;

        const auto rep = ReactionIntegrator::step(
            rx, /*tank=*/true, dt, sc.msx.data(), hydvar, sc.ws,
            (n_pollut > 0) ? sc.pollut.data() : nullptr);
        if (rep.ok) {
            for (int m = 0; m < nm; ++m)
                node_mass[und * uns + static_cast<std::size_t>(n_pollut + m)] =
                    sc.msx[static_cast<std::size_t>(m)] * vol;
        } else {
            containFailure(ctx, "node store", nd, rep.error);
        }
    }
}

}  // namespace openswmm::transport
