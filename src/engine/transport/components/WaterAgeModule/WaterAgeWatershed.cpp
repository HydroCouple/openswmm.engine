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
 * @file WaterAgeWatershed.cpp
 * @brief Phase A3 body — subcatchment surface water age.
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "WaterAgeWatershed.hpp"

#include "../WatershedCommon.hpp"

#include <algorithm>
#include <cmath>

#include "../../../core/SimulationContext.hpp"
#include "../../../hydrology/Runoff.hpp"

namespace openswmm::transport {

namespace {
constexpr int kNSub = static_cast<int>(SubArea::COUNT_);
constexpr double kTinyVol = 1.0e-12;  ///< ft³
}  // namespace

void addRunonAge(SimulationContext& ctx, int donor_sc, int receiver_sc,
                 double q) {
    if (!ctx.options.water_age) return;
    auto& ws = ctx.water_age_state;
    const auto ud = static_cast<std::size_t>(donor_sc);
    const auto ur = static_cast<std::size_t>(receiver_sc);
    if (ud >= ws.subcatch_runoff_age.size() ||
        ur >= ws.subcatch_runon_age_vol_in.size())
        return;
    // The RATE convention the loaders use everywhere: q · age, integrated by
    // the consumer. Run-on arrives at the age the donor's runoff left at.
    ws.subcatch_runon_age_vol_in[ur] += q * ws.subcatch_runoff_age[ud];
}

void routeSubcatchmentAge(SimulationContext& ctx,
                          const runoff::RunoffSoA& soa, double dt) {
    if (!ctx.options.water_age || dt <= 0.0) return;

    auto& ws = ctx.water_age_state;
    const int nsc = ctx.n_subcatches();
    if (nsc <= 0) return;
    const auto want = static_cast<std::size_t>(nsc) *
                      static_cast<std::size_t>(kNSub);
    if (ws.subarea_age.size() != want)
        ws.resize(ctx.n_nodes(), ctx.n_links(), nsc);

    // Age of water arriving from the sky. RAINFALL is the pathway the
    // wet-weather loader already uses for washoff, so the same configured
    // source age governs both — one table, one meaning.
    const double a_rain = ctx.water_age_config.global_age[
        static_cast<int>(WaterAgeSource::RAINFALL)];

    for (int i = 0; i < nsc && i < static_cast<int>(soa.area.size()); ++i) {
        const auto ui = static_cast<std::size_t>(i);
        const double area = soa.area[ui];
        const double fi   = soa.imperv_pct[ui];
        const double fp   = 1.0 - fi;
        const double f0   = fi * soa.imperv0_pct[ui];
        const double f1   = fi * (1.0 - soa.imperv0_pct[ui]);

        // Area-weighted subarea volumes, the same expression the runoff
        // mass balance uses (SWMMEngine.cpp:3664-3666).
        const double frac[kNSub] = {f0, f1, fp};
        const double depth[kNSub] = {soa.depth_imperv0[ui],
                                     soa.depth_imperv1[ui],
                                     soa.depth_perv[ui]};

        // Run-on arrives spread over the whole subcatchment as extra
        // precipitation (Runoff.cpp:331-333), so it mixes into every
        // subarea in proportion to that subarea's share of the inflow.
        const double runon_rate =
            (ui < ws.subcatch_runon_age_vol_in.size() &&
             ui < ctx.subcatches.runon_inflow.size())
                ? ctx.subcatches.runon_inflow[ui]
                : 0.0;
        const double runon_age =
            (runon_rate > 0.0 && ui < ws.subcatch_runon_age_vol_in.size())
                ? ws.subcatch_runon_age_vol_in[ui] / runon_rate
                : 0.0;

        // Run-on spreads over the whole subcatchment, so its depth rate is
        // the same for every subarea. The PRECIPITATION rate is not — see
        // below.
        const double runon_depth_rate = (area > 0.0) ? runon_rate / area : 0.0;

        double out_num = 0.0, out_den = 0.0;
        for (int k = 0; k < kNSub; ++k) {
            const auto idx = ui * static_cast<std::size_t>(kNSub) +
                             static_cast<std::size_t>(k);
            const double v_new = depth[k] * frac[k] * area;
            const double v_old = ws.subarea_vol_prev[idx];

            // S1: the water that ACTUALLY reached THIS subarea. On a bare
            // subcatchment that is the gage rate; under a snowpack it is
            // `imelt + rain·(1 − asc)`, which differs BETWEEN subareas — the
            // impervious value is an area-weighted blend over plowable and
            // non-plowable fractions, the pervious one is not. This read used
            // to sit outside the loop as a single scalar, which was wrong
            // even where the total was right.
            const double rain_rate = arrivingPrecipRate(ctx, ui, k);
            const double in_rate = rain_rate + runon_depth_rate;

            // Arriving water is precipitation and run-on MIXED, flow-
            // weighted. Rain routinely outweighs run-on by orders of
            // magnitude — on the gate deck's cascade it is 10.08 cfs against
            // 0.027 — so treating the arrival as pure run-on whenever any
            // run-on exists would hand the whole inflow the donor's age.
            const double a_in = (in_rate > 0.0)
                ? (rain_rate * a_rain + runon_depth_rate * runon_age) / in_rate
                : a_rain;

            // 1. Age what was already there.
            double a = ws.subarea_age[idx] + dt;

            // 2. Mix in what arrived — the GROSS inflow, not the net gain.
            //    A subarea shedding as fast as it fills has v_new == v_old
            //    and so no net gain at all, which is the ordinary state of
            //    an impervious surface during a storm: a net-gain mixing
            //    volume never admits the rain pouring through it, and the
            //    age degenerates into the time since the surface first
            //    wetted. Measured on a 100% impervious, zero-depression
            //    deck under sustained rain: 0.88592 h against a residence
            //    time V/Q of 0.14062 h. Complete-mix is the right model
            //    here — outflow leaves at the subarea's own age, so only
            //    the INFLOW is needed, and the solver already publishes it;
            //    the same deck then reads 0.14022 h.
            const double v_in = in_rate * frac[k] * area * dt;
            if (v_in > kTinyVol) {
                const double denom = v_old + v_in;
                a = (denom > kTinyVol) ? (a * v_old + a_in * v_in) / denom
                                       : a_in;
            }
            ws.subarea_age[idx]      = std::max(a, 0.0);
            ws.subarea_vol_prev[idx] = v_new;

            // 3. Runoff leaves at the volume-weighted mean of the subareas
            //    that hold water. A subarea with nothing on it contributes
            //    no water and therefore no age — weighting by area alone
            //    would let a dry pervious surface drag the runoff age.
            if (v_new > kTinyVol) {
                out_num += a * v_new;
                out_den += v_new;
            }
        }
        if (ui < ws.subcatch_runoff_age.size())
            ws.subcatch_runoff_age[ui] =
                (out_den > kTinyVol) ? out_num / out_den : a_rain;
    }

    // The run-on accumulator is a per-step rate, like node_age_vol_in:
    // zero it once consumed so the next assembly starts clean.
    std::fill(ws.subcatch_runon_age_vol_in.begin(),
              ws.subcatch_runon_age_vol_in.end(), 0.0);
}

}  // namespace openswmm::transport
