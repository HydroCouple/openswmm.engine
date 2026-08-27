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
 * @file WaterAgeLid.cpp
 * @brief Phase A4 body — water age through the LID layer stack.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "WaterAgeLid.hpp"

#include "../LidLayerCommon.hpp"

#include <algorithm>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../../../hydrology/LID.hpp"
#include "../../../hydrology/Runoff.hpp"

namespace openswmm::transport {

namespace {

constexpr int    kAge     = static_cast<int>(LidSpecies::AGE);
constexpr double kTinyVol = 1.0e-12;  ///< ft of water per unit area

// Donors/donorsFor, layerVolumes and buildOffsets moved to
// LidLayerCommon at H5b: temperature needs the same topology, and a
// second copy of an eight-case per-type stack table is exactly the
// duplication lesson 81 is about.

}  // namespace

void initLidLayerAge(SimulationContext& ctx, const lid::LIDSolver& solver) {
    if (!ctx.options.water_age) return;
    auto offsets = buildOffsets(solver);
    const int units = offsets.empty() ? 0 : offsets.back();
    if (units <= 0) return;

    auto& st = ctx.lid_layer_state;
    // H5b: ensureSized, not resize. The block is now shared with the heat
    // track, and whichever capability initialises first must not wipe the
    // other's row.
    st.ensureSized(units, static_cast<int>(LidSpecies::COUNT_), offsets);

    // Seed the volumes a unit already holds, so the first step mixes against
    // real water rather than against zero. INITIAL_STATE is the age of water
    // already in the model, the same source A1a and A3 seed from.
    const double a_init = ctx.water_age_config.global_age[
        static_cast<int>(WaterAgeSource::INITIAL_STATE)];
    for (int t = 0; t < solver.numGroups(); ++t) {
        const auto& g = solver.group(t);
        for (int u = 0; u < g.count; ++u) {
            const auto ui   = static_cast<std::size_t>(u);
            const int  flat = offsets[static_cast<std::size_t>(t)] + u;
            double v[kNL];
            layerVolumes(g, ui, v);
            for (int k = 0; k < kNL; ++k) {
                const auto idx = static_cast<std::size_t>(flat) * kNL +
                                 static_cast<std::size_t>(k);
                st.vol_prev[idx] = v[k];
                if (v[k] > kTinyVol)
                    st.value[st.layer_index(flat, static_cast<LidLayer>(k),
                                            kAge)] = a_init;
            }
        }
    }
}

void setLidInflowAge(SimulationContext& ctx, int type_index, int unit,
                     int subcatch, double rain_rate, double q_imperv,
                     double q_perv, double q_runon, double lid_area) {
    auto& st = ctx.lid_layer_state;
    if (!ctx.options.water_age || !st.active()) return;
    const auto ut = static_cast<std::size_t>(type_index);
    if (ut + 1 >= st.group_offset.size()) return;
    const int flat = st.group_offset[ut] + unit;
    if (flat < 0 || flat >= st.n_units) return;

    // The unit's inflow is assembled from four sources with known rates, all
    // in one place — so the age is a direct flow-weighted mean rather than
    // the scattered-donor accumulator A3's run-on needed. Rates here are
    // volumes per unit time over the LID footprint (ft3/s): `rain_rate` is a
    // depth rate, the rest are already CFS.
    const double q_rain = rain_rate * lid_area;

    const double a_rain = ctx.water_age_config.global_age[
        static_cast<int>(WaterAgeSource::RAINFALL)];

    // Everything the unit captures from the surface around it — impervious
    // runoff, pervious runoff, and whole-subcatchment run-on — is the same
    // water A3 publishes as `subcatch_runoff_age`, so that is what it
    // arrives at. Reconstructing an impervious mean from the raw
    // `subarea_age` rows instead looked more precise and was wrong: a
    // subarea can be shedding while its END-of-step stored volume is zero,
    // and the untouched zero of a dry row then drags the mix BELOW the
    // youngest thing entering the model. Measured on the composition deck:
    // 3.759 h of arriving water under 4 h rain, with no source younger than
    // 4 h anywhere. A3 already resolves exactly this weighting for the
    // outlet node, so using its answer keeps the LID and the node
    // consistent as well as correct.
    const auto& ws = ctx.water_age_state;
    const int   sc = subcatch;
    const double a_surface =
        (sc >= 0 && static_cast<std::size_t>(sc) < ws.subcatch_runoff_age.size() &&
         ws.subcatch_runoff_age[static_cast<std::size_t>(sc)] > 0.0)
            ? ws.subcatch_runoff_age[static_cast<std::size_t>(sc)]
            : a_rain;

    const double q_surface = q_imperv + q_perv + q_runon;
    const double q_tot = q_rain + q_surface;
    const double age = (q_tot > 0.0)
        ? (q_rain * a_rain + q_surface * a_surface) / q_tot
        : a_rain;
    st.inflow_value[static_cast<std::size_t>(flat) *
                        static_cast<std::size_t>(st.n_species) +
                    static_cast<std::size_t>(kAge)] = age;
}

void routeLidLayerAge(SimulationContext& ctx, const lid::LIDSolver& solver,
                      double dt) {
    auto& st = ctx.lid_layer_state;
    if (!ctx.options.water_age || !st.active() || !(dt > 0.0)) return;

    for (int t = 0; t < solver.numGroups(); ++t) {
        const auto& g = solver.group(t);
        if (g.count == 0) continue;
        const auto ut = static_cast<std::size_t>(t);
        if (ut + 1 >= st.group_offset.size()) continue;

        for (int u = 0; u < g.count; ++u) {
            const auto ui   = static_cast<std::size_t>(u);
            const int  flat = st.group_offset[ut] + u;
            if (flat < 0 || flat >= st.n_units) continue;
            const auto uf = static_cast<std::size_t>(flat);

            const Donors d = donorsFor(g.type, g.soil_thick[ui] > 0.0);
            const int donor[kNL] = {d.surface, d.pavement, d.soil, d.storage};
            const double in_rate[kNL] = {g.in_surf[ui], g.in_pave[ui],
                                         g.in_soil[ui], g.in_stor[ui]};
            double v_new[kNL];
            layerVolumes(g, ui, v_new);

            const double a_ext =
                st.inflow_value[uf * static_cast<std::size_t>(st.n_species) +
                                static_cast<std::size_t>(kAge)];

            // Read the donors' OLD ages before any layer is overwritten:
            // percolation carries the water the donor held at the start of
            // the step, not the value it ends with. Updating in place would
            // let a parcel fall through the whole stack in one step.
            double a_old[kNL];
            for (int k = 0; k < kNL; ++k)
                a_old[k] = st.value[st.layer_index(flat,
                                                   static_cast<LidLayer>(k),
                                                   kAge)];

            for (int k = 0; k < kNL; ++k) {
                const auto vidx = uf * kNL + static_cast<std::size_t>(k);
                const double v_old = st.vol_prev[vidx];
                const double v_in  = std::max(0.0, in_rate[k]) * dt;

                // A layer this TYPE does not have carries no age, whatever
                // the SoA happens to hold in its slot. A vegetative swale
                // has soil parameters — that is where its infiltration
                // conductivity lives — but no soil LAYER: water that
                // infiltrates leaves for native ground. Without this the
                // swale's soil row would sit on a nonzero `soil_moist`,
                // receive nothing, and climb to the elapsed run time.
                //
                // A layer that holds nothing AND receives nothing is the
                // same case for a different reason: no water, no age.
                if (donor[k] == kAbsent ||
                    (v_old <= kTinyVol && v_in <= kTinyVol)) {
                    st.value[st.layer_index(flat, static_cast<LidLayer>(k),
                                            kAge)] = 0.0;
                    st.vol_prev[vidx] = v_new[k];
                    continue;
                }

                // 1. What is held ages.
                double a = a_old[k] + dt;

                // 2. What arrived mixes in, by GROSS inflow volume. The
                //    solver publishes the rate, so this is exact — there is
                //    no need for the net change in storage, which is zero
                //    for a layer passing water straight through and was how
                //    A3 came to report elapsed time instead of age.
                if (v_in > kTinyVol) {
                    const double a_in =
                        (donor[k] == kExternal) ? a_ext
                        : (donor[k] >= 0)       ? a_old[donor[k]] + dt
                                                : a_ext;
                    const double den = v_old + v_in;
                    a = (den > kTinyVol) ? (a * v_old + a_in * v_in) / den
                                         : a_in;
                }

                // 3. What leaves does so at the layer's own age, so nothing
                //    more is needed — that is why the outflow rates the
                //    solver does not publish are not required.
                st.value[st.layer_index(flat, static_cast<LidLayer>(k),
                                        kAge)] = std::max(a, 0.0);
                st.vol_prev[vidx] = v_new[k];
            }

            // The underdrain draws from the storage layer (decision,
            // 2026-08-18) — `getDrainRate` reads `stor_depth`. Roof
            // disconnection has no storage layer: its "drain" is a routed
            // fraction of the roof outflow, so it leaves at the surface age.
            const bool from_surface = (g.type == lid::LIDType::ROOF_DISCON);
            const auto src = from_surface ? LidLayer::SURFACE
                                          : LidLayer::STORAGE;
            st.drain_value[uf * static_cast<std::size_t>(st.n_species) +
                           static_cast<std::size_t>(kAge)] =
                st.value[st.layer_index(flat, src, kAge)];
        }
    }
}

}  // namespace openswmm::transport
