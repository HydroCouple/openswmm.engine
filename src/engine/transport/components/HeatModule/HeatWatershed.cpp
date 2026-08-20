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
 * @file HeatWatershed.cpp
 * @brief Phase H5a — temperature on subcatchment surfaces.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H5a, §6.1
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HeatWatershed.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

#include "../../../core/SimulationContext.hpp"
#include "../../../hydrology/Runoff.hpp"
#include "../HeatFluxModules/HeatFluxes.hpp"
#include "../HeatFluxModules/SurfaceExchange.hpp"

namespace openswmm::transport {

namespace {

constexpr int    kNSub    = HeatState::kNSubArea;
constexpr double kTinyVol = 1.0e-12;  ///< ft³

// `kMaxStepC` lived here: the refuse-above-5-°C bound H5a used to stop the
// forward-Euler divergence. D-H5d removed it with the divergence — a
// relaxation step cannot overshoot equilibrium, so there is nothing left to
// refuse, and keeping a threshold nothing can reach would be a constant that
// no gate could observe being wrong (lesson 39).

/// What a subarea holding no water reports (plan D-H5c).
///
/// A4's age rule — "no water, no age", write 0 — cannot be reused, because
/// 0 °C is a real temperature and a reader has no way to tell the sentinel
/// from a freezing puddle. The policy is the deck's choice.
double dryTemperature(const SimulationContext& ctx, double held,
                      double t_air_c) noexcept {
    switch (ctx.heat_config.dry_temp_policy) {
        case DryTempPolicy::AIR:     return t_air_c;
        case DryTempPolicy::DEFAULT: return HeatConfigData::kDefaultTemp;
        case DryTempPolicy::HOLD:
        default:                     return held;
    }
}

}  // namespace

void addRunonTemperature(SimulationContext& ctx, int donor_sc,
                         int receiver_sc, double q) {
    if (!ctx.options.heat_transport) return;
    auto& hs = ctx.heat_state;
    const auto ud = static_cast<std::size_t>(donor_sc);
    const auto ur = static_cast<std::size_t>(receiver_sc);
    if (ud >= hs.subcatch_runoff_temp.size() ||
        ur >= hs.subcatch_runon_temp_vol_in.size() ||
        ur >= hs.subcatch_runon_temp_rate.size())
        return;
    addRunonTemperatureAt(ctx, receiver_sc, q, hs.subcatch_runoff_temp[ud]);
}

void addRunonTemperatureAt(SimulationContext& ctx, int receiver_sc, double q,
                           double temp_c) {
    if (!ctx.options.heat_transport || !(q > 0.0)) return;
    auto& hs = ctx.heat_state;
    const auto ur = static_cast<std::size_t>(receiver_sc);
    if (ur >= hs.subcatch_runon_temp_vol_in.size() ||
        ur >= hs.subcatch_runon_temp_rate.size())
        return;
    // Numerator AND denominator, always together. Adding to one without the
    // other is the shape of A3's defect, and keeping them in a single
    // function is what makes that shape hard to write by accident.
    hs.subcatch_runon_temp_vol_in[ur] += q * temp_c;
    hs.subcatch_runon_temp_rate[ur]   += q;
}

void routeSubcatchmentTemperature(SimulationContext& ctx,
                                  const runoff::RunoffSoA& soa, double dt) {
    if (!ctx.options.heat_transport || dt <= 0.0) return;

    auto& hs  = ctx.heat_state;
    const int nsc = ctx.n_subcatches();
    if (nsc <= 0) return;
    // Seed from INITIAL_STATE, not kDefaultTemp: that is the source meaning
    // "water already in the model at t = 0", and it is exactly what
    // QualityRouting.cpp:164-166 seeds the node and link temperatures from.
    // A dry surface at t = 0 holds no water, but the first rain mixes
    // against this value, so seeding it differently from the network would
    // put a discontinuity at the outlet that no gate would name.
    if (!hs.watershedSized(nsc))
        hs.resizeWatershed(nsc, ctx.heat_config.global_temp[
            static_cast<int>(HeatSource::INITIAL_STATE)]);

    // Temperature of water arriving from the sky. RAINFALL is the pathway
    // the wet-weather loader already uses, so one table governs both.
    const double t_rain = ctx.heat_config.global_temp[
        static_cast<int>(HeatSource::RAINFALL)];

    // Met forcing. These are written in stepRunoff at SWMMEngine.cpp:1379
    // (air), :1424 (wind) and :1431 (humidity), all BEFORE runoff_.execute
    // at :1648 — so they are current here, which is the fact that made the
    // runoff-clock binding possible at all.
    // Air temperature is still read here — the DRY-element policy needs it
    // (D-H5c). The humidity, wind and wind-function coefficients that used
    // to be extracted alongside it moved inside the shared flux evaluators
    // with D-H5e, so this function no longer knows which parameters a flux
    // family reads.
    const double t_air = heat::airTempCelsius(ctx);
    const double rho   = ctx.options.water_density;
    const double cp    = ctx.options.water_specific_heat;

    const bool do_surface   = ctx.heat_config.surface_exchange;
    const bool do_radiative = ctx.heat_config.radiative_exchange;

    for (int i = 0; i < nsc && i < static_cast<int>(soa.area.size()); ++i) {
        const auto ui = static_cast<std::size_t>(i);

        // `soa.area` is ft2, LID-excluded. `ctx.subcatches.area` is the
        // deck's USER area units (acres here — Runoff.cpp:197 divides by
        // ucf_area before subtracting the LID footprint), so substituting it
        // is a 43560x error, not the footprint double-count it looks like.
        // See the header: the exchange area cancels in `relaxT`, so only the
        // run-on depth conversion below can see this.
        const double area = soa.area[ui];
        const double fi   = soa.imperv_pct[ui];
        const double fp   = 1.0 - fi;
        const double f0   = fi * soa.imperv0_pct[ui];
        const double f1   = fi * (1.0 - soa.imperv0_pct[ui]);

        const double frac[kNSub]  = {f0, f1, fp};
        const double depth[kNSub] = {soa.depth_imperv0[ui],
                                     soa.depth_imperv1[ui],
                                     soa.depth_perv[ui]};

        // Run-on spreads over the whole subcatchment as extra precipitation
        // (Runoff.cpp:331-333), so it mixes into every subarea in proportion
        // to that subarea's share of the inflow.
        const double runon_rate =
            (ui < ctx.subcatches.runon_inflow.size())
                ? ctx.subcatches.runon_inflow[ui]
                : 0.0;

        // Divide by the rate whose temperature is KNOWN, not by the total
        // run-on. A3 divided by the total while filling the numerator from
        // one of three contributors, and the arriving age came out younger
        // than anything entering the model. Since H5b all three contributors
        // — cascade, outfall return and LID underdrain — supply one, so the
        // two rates coincide today; the pair is what keeps a FOURTH
        // contributor from reintroducing the defect, and
        // `EveryRunonContributorKeepsTemperaturesInsideTheSources` asserts
        // they still match.
        const double known_rate =
            (ui < hs.subcatch_runon_temp_rate.size())
                ? hs.subcatch_runon_temp_rate[ui]
                : 0.0;
        const double runon_temp =
            (known_rate > kTinyVol &&
             ui < hs.subcatch_runon_temp_vol_in.size())
                ? hs.subcatch_runon_temp_vol_in[ui] / known_rate
                : t_rain;

        const double rain_rate = (ui < ctx.subcatches.rainfall.size())
                                     ? ctx.subcatches.rainfall[ui]
                                     : 0.0;
        const double runon_depth_rate = (area > 0.0) ? runon_rate / area : 0.0;
        const double in_rate = rain_rate + runon_depth_rate;

        // Arriving water is rain and run-on MIXED, flow-weighted. Rain
        // routinely outweighs run-on by orders of magnitude, so treating
        // the arrival as pure run-on wherever any exists would hand the
        // whole inflow the donor's temperature.
        const double t_in = (in_rate > 0.0)
            ? (rain_rate * t_rain + runon_depth_rate * runon_temp) / in_rate
            : t_rain;

        double out_num = 0.0, out_den = 0.0;
        for (int k = 0; k < kNSub; ++k) {
            const auto idx = ui * static_cast<std::size_t>(kNSub) +
                             static_cast<std::size_t>(k);
            if (idx >= hs.subarea_temp.size()) break;

            const double v_new = depth[k] * frac[k] * area;
            const double v_old = hs.subarea_vol_prev[idx];
            const double v_in  = in_rate * frac[k] * area * dt;
            const double a_ft2 = frac[k] * area;

            double t = hs.subarea_temp[idx];

            // A subarea with no water and none arriving has no thermal mass
            // to speak of. Applying a flux to it would divide by ~zero, and
            // A4's answer (write 0) is wrong for a temperature — so the
            // deck's policy decides, and the volume ledger still advances.
            if (v_old <= kTinyVol && v_in <= kTinyVol) {
                hs.subarea_temp[idx]     = dryTemperature(ctx, t, t_air);
                hs.subarea_vol_prev[idx] = v_new;
                continue;
            }

            // 1. The surface energy balance acts on the water that is
            //    ALREADY there, over that subarea's ponded area (D-H5a).
            //    Applying it to the post-mix volume instead would let a
            //    step's rain be heated before it had arrived.
            if (v_old > kTinyVol && (do_surface || do_radiative)) {
                // Net outward flux, via the shared composition (D-H5e).
                // This was a hand-rolled sum of the two modules — the fourth
                // such copy in the program, and copies of exactly this sum
                // are how the LEGACY node/link path ended up relaxing each
                // module separately toward a different equilibrium.
                const auto net_out = [&](double tw) {
                    return heat::netFluxOut(ctx, tw);
                };
                // Both flux families are signed POSITIVE OUT of the water, so
                // they add. There is exactly one sign flip in this program
                // and it lives inside `relaxT`; a second one here would
                // silently cancel it.
                //
                // D-H5d: SEMI-IMPLICIT. H5a shipped a forward-Euler step and
                // a refuse-above-5 C bound, because a ponded film has almost
                // no thermal mass per unit exchanging area: a 0.52 ft3 film
                // over 27226 ft2 moved 862 C in one 60 s step and the
                // sequence diverged 5 -> 182 -> -1.8e4 -> -3.9e9 -> inf ->
                // NaN. The bound stopped the divergence but made an
                // unresolved film carry its inflow temperature instead of
                // exchanging at all. Relaxation removes both problems: the
                // step cannot overshoot the equilibrium temperature however
                // thin the film, so there is nothing left to refuse.
                t += heat::relaxT(net_out(t), net_out(t + heat::kProbeC),
                                  heat::kProbeC, a_ft2 * heat::kSqFtToSqM,
                                  v_old * heat::kCuFtToCuM, dt, rho, cp);
            }

            // 2. What arrived mixes in by GROSS inflow volume — never the
            //    net gain. A subarea shedding as fast as it fills has
            //    v_new == v_old and so no net gain at all, which is the
            //    ordinary state of an impervious surface under rain; A3
            //    measured that mistake at 6.3x on the age it produced.
            if (v_in > kTinyVol) {
                const double denom = v_old + v_in;
                t = (denom > kTinyVol) ? (t * v_old + t_in * v_in) / denom
                                       : t_in;
            }

            // No aging term and NO ZERO FLOOR. `WaterAgeWatershed` clamps
            // with max(a, 0) because a negative age is meaningless; a
            // sub-zero temperature is ordinary, and clamping it would
            // silently manufacture energy on every frozen surface.
            hs.subarea_temp[idx]     = t;
            hs.subarea_vol_prev[idx] = v_new;

            // 3. Runoff leaves at the volume-weighted mean of the subareas
            //    HOLDING water. Weighting by area would let a dry pervious
            //    surface — whose temperature is a policy value, not a
            //    measurement — drag the runoff temperature.
            if (v_new > kTinyVol) {
                out_num += t * v_new;
                out_den += v_new;
            }
        }
        if (ui < hs.subcatch_runoff_temp.size())
            hs.subcatch_runoff_temp[ui] =
                (out_den > kTinyVol) ? out_num / out_den : t_rain;
    }

    // Per-step rate accumulator, like node_temp_vol_in: zero once consumed
    // so the next assembly starts clean.
    std::fill(hs.subcatch_runon_temp_vol_in.begin(),
              hs.subcatch_runon_temp_vol_in.end(), 0.0);
    std::fill(hs.subcatch_runon_temp_rate.begin(),
              hs.subcatch_runon_temp_rate.end(), 0.0);
}

}  // namespace openswmm::transport
