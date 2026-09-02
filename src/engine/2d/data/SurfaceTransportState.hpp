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
 * @file SurfaceTransportState.hpp
 * @brief Overland transport S1 — per-cell species MASS on the 2D surface.
 *
 * @details Species are carried as **mass**, not concentration (D-2DT1). The
 *          2D solver's integrated state is `SurfaceStateData::volume`, with
 *          depth and head *reconstructed* from it each step; a concentration
 *          state would have to be reconstructed against a volume that itself
 *          moves, and the two would disagree at every wet/dry transition.
 *          Mass is what the flux form conserves, and it makes drying
 *          trivial: a cell that empties keeps what it held.
 *
 *          Layout is SPECIES-MAJOR, `[s * n_cells + c]`, matching
 *          `ArdEngine`'s `cell_phi` so the two engines' kernels read alike.
 *
 * @par What S1 carries, and what it does not
 *      S1 rows are the `[POLLUTANTS]` species only, `[0, np)`. Water age,
 *      temperature and MSX rows arrive with S4 and take the same row
 *      convention the 1D engines use (pollutants, MSX, age, temperature).
 *      `n_species` is therefore whatever the router sizes it to, and no code
 *      in the marcher knows what a row MEANS — it moves mass.
 *
 * @par Sources in S1
 *      Water arriving through rainfall, a boundary inflow, or the 1D→2D
 *      coupling arrives at **zero concentration** in S1. Water LEAVING
 *      through infiltration, a boundary outflow, or the 2D→1D drain leaves
 *      **at the cell's concentration** and its mass is booked to a ledger,
 *      so nothing is destroyed silently. Evaporation removes volume and no
 *      mass — the concentration rises — which is the up-concentration the
 *      1D path gets wrong (KD1's open residual) and the 2D path gets right
 *      from the start. Source concentrations (rain concentration, boundary
 *      species, the coupling tuple) are S2/S3; the ledger rows below are
 *      what those phases will consume.
 *
 * @see plans/transport/OVERLAND_TRANSPORT_HEAT_MSX_PLAN_2026-09-01.md
 * @ingroup engine_2d
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#ifndef OPENSWMM_ENGINE_2D_SURFACE_TRANSPORT_STATE_HPP
#define OPENSWMM_ENGINE_2D_SURFACE_TRANSPORT_STATE_HPP

#include <cstddef>
#include <vector>

namespace openswmm::twoD {

struct SurfaceTransportState {
    int n_species = 0;   ///< rows carried; 0 ⇒ transport is off everywhere
    int n_cells   = 0;

    /// [s * n_cells + c] species mass in the cell's water, in the species'
    /// own mass units (concentration units × m³, so a report divides by the
    /// SI cell volume and needs no further conversion).
    std::vector<double> cell_mass;

    // ---- Ledgers (per species), m³·conc, cumulative over the run. ----------
    // These are what S3's coupling tuple and the continuity table will read;
    // in S1 they are the record that mass left the surface and where.
    std::vector<double> lost_infiltration;   ///< left through the bed
    std::vector<double> lost_boundary;       ///< left through an open edge
    std::vector<double> lost_coupling;       ///< left through the 1D↔2D exchange
    /// [k * n_species + s] species mass drained through coupling point k this
    /// advance (m³·conc, + = 2D→1D). Reset with `exch_` per advance; S3's
    /// tuple reads it into the node's `qual_mass_in`.
    std::vector<double> exch_mass;

    // ---- S2 source concentrations (species units) -------------------------
    /// [s] concentration carried by rainfall — `[POLLUTANTS]` rain
    /// concentration, set by the router. Empty ⇒ rain arrives clean (S1).
    std::vector<double> rain_conc;
    /// [bc_slot * n_species + s] concentration carried by INFLOW through a
    /// non-WALL boundary edge, from `[2D_BOUNDARY_QUALITY]`. Sized by the
    /// solver to its own `bc_cell_` list at initialize; empty ⇒ inflow
    /// arrives clean. Outflow always leaves at the cell's concentration.
    std::vector<double> bc_conc;
    /// Raw `[2D_BOUNDARY_QUALITY]` rows, resolved by the solver against its
    /// boundary-edge list: (flat mesh edge slot tri*3+e, species, conc).
    struct BoundaryQualityRow { int slot = -1; int species = -1; double conc = 0.0; };
    std::vector<BoundaryQualityRow> bc_quality_rows;

    // ---- S2 dispersion limiter telemetry ----------------------------------
    /// Times the explicit dispersive exchange on a face was capped to keep the
    /// pair from crossing (the max-principle limiter). Nonzero means the
    /// dispersion is under-resolved at the marcher's dt on that face — a
    /// modelling signal, not an error, and the reason it is counted rather
    /// than hidden.
    long dispersion_limiter_binds = 0;

    /// Source ledgers (per species, m³·conc, cumulative) so the continuity
    /// statement can close: what came in through rain and boundaries.
    std::vector<double> gained_rainfall;
    std::vector<double> gained_boundary;

    bool active() const noexcept { return n_species > 0 && n_cells > 0; }

    void resize(int n_spec, int n_tri, int n_coupling_points) {
        n_species = (n_spec > 0) ? n_spec : 0;
        n_cells   = (n_tri  > 0) ? n_tri  : 0;
        const auto ns = static_cast<std::size_t>(n_species);
        cell_mass.assign(ns * static_cast<std::size_t>(n_cells), 0.0);
        lost_infiltration.assign(ns, 0.0);
        lost_boundary.assign(ns, 0.0);
        lost_coupling.assign(ns, 0.0);
        exch_mass.assign(ns * static_cast<std::size_t>(
                                  n_coupling_points > 0 ? n_coupling_points
                                                        : 0),
                         0.0);
        rain_conc.clear();          // S2: router fills when pollutants carry one
        bc_conc.clear();            // S2: solver sizes at initialize
        bc_quality_rows.clear();
        dispersion_limiter_binds = 0;
        gained_rainfall.assign(ns, 0.0);
        gained_boundary.assign(ns, 0.0);
    }

    void clear() { *this = SurfaceTransportState{}; }

    std::size_t idx(int s, int c) const noexcept {
        return static_cast<std::size_t>(s) * static_cast<std::size_t>(n_cells) +
               static_cast<std::size_t>(c);
    }

    /// Reported concentration — derived, never stored. `dry_volume_m3` is the
    /// hydraulics' own dry threshold expressed as a volume; below it the
    /// cell holds its mass and reports 0 rather than dividing by a vanishing
    /// volume (the 2D analogue of the 1D dryness guard).
    double concentration(int s, int c, double volume_m3,
                         double dry_volume_m3) const noexcept {
        if (!(volume_m3 > dry_volume_m3)) return 0.0;
        return cell_mass[idx(s, c)] / volume_m3;
    }

    /// Total mass of one species over the surface + every ledger — the
    /// quantity the S1 conservation gate asserts is invariant.
    double totalIncludingLedgers(int s) const noexcept {
        double m = 0.0;
        const auto base = static_cast<std::size_t>(s) *
                          static_cast<std::size_t>(n_cells);
        for (int c = 0; c < n_cells; ++c)
            m += cell_mass[base + static_cast<std::size_t>(c)];
        const auto us = static_cast<std::size_t>(s);
        // Sources are SUBTRACTED so the quantity is "what was there at t=0":
        // surface + everything that left − everything that arrived.
        return m + lost_infiltration[us] + lost_boundary[us] +
               lost_coupling[us] -
               (us < gained_rainfall.size() ? gained_rainfall[us] : 0.0) -
               (us < gained_boundary.size() ? gained_boundary[us] : 0.0);
    }
};

}  // namespace openswmm::twoD

#endif  // OPENSWMM_ENGINE_2D_SURFACE_TRANSPORT_STATE_HPP
