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
 * @file HeatLid.cpp
 * @brief Phase H5b body — temperature through the LID layer stack.
 *
 * @see plans/transport/HEAT_TRANSPORT_PLAN.md §6 H5b, §6.1 D-H5b/D-H5c
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "HeatLid.hpp"

#include <cmath>
#include <cstddef>
#include <vector>

#include "../../../core/SimulationContext.hpp"
#include "../../../hydrology/LID.hpp"
#include "../HeatFluxModules/HeatFluxes.hpp"
#include "../HeatFluxModules/SolarRadiation.hpp"
#include "../HeatFluxModules/SurfaceExchange.hpp"
#include "../LidLayerCommon.hpp"

namespace openswmm::transport {

namespace {

constexpr int    kTemp    = static_cast<int>(LidSpecies::TEMPERATURE);
constexpr double kTinyVol = 1.0e-12;  ///< ft of water per unit area
constexpr double kFtToM   = 0.3048;

/// What a layer holding no water reports (plan D-H5c) — the same policy the
/// watershed uses, for the same reason: A4's "no water, no age" zero is a
/// sentinel, and 0 °C is a real temperature.
double dryTemperature(const SimulationContext& ctx, double held,
                      double t_air_c) noexcept {
    switch (ctx.heat_config.dry_temp_policy) {
        case DryTempPolicy::AIR:     return t_air_c;
        case DryTempPolicy::DEFAULT: return HeatConfigData::kDefaultTemp;
        case DryTempPolicy::HOLD:
        default:                     return held;
    }
}

/// Physical thickness of each layer, metres. Distinct from the WATER each
/// holds (`layerVolumes`): conduction crosses the whole layer, matrix
/// included, while advection only carries the water.
void layerThickness(const lid::LIDGroupSoA& g, std::size_t ui,
                    double (&d)[kNL]) {
    d[kSurf] = g.surf_store[ui] * kFtToM;
    d[kPave] = g.pave_thick[ui] * kFtToM;
    d[kSoil] = g.soil_thick[ui] * kFtToM;
    d[kStor] = g.stor_thick[ui] * kFtToM;
}

/// Volumetric heat capacity of a layer, J/m³/K — water and matrix together.
///
/// The SURFACE layer is ponded water over a (mostly) impervious face, so it
/// is water alone. The buried layers are a matrix with water in the voids,
/// so both contribute; that is what makes their thermal mass large enough
/// for conduction to be slow, and it is why a thin surface film is the only
/// stiff member of the stack.
double heatCapacity(const ConductionConfig& c, int layer, double water_m,
                    double thick_m, double rho_w, double cp_w) noexcept {
    const double water = rho_w * cp_w * water_m;
    if (layer == kSurf) return (thick_m > 0.0) ? water / thick_m : 0.0;
    const double solid_m = (thick_m > water_m) ? thick_m - water_m : 0.0;
    const double total   = water + c.sed_density * c.sed_specific_heat *
                                       solid_m;
    return (thick_m > 0.0) ? total / thick_m : 0.0;
}

/// Effective thermal conductivity of a layer, W/m/K — volume-weighted
/// between water and matrix. The simplest defensible mixing model; a
/// geometric or Johansen mixture would be a second parameter set with no
/// reference value in HydroCouple to gate it against.
double conductivity(const ConductionConfig& c, int layer, double water_m,
                    double thick_m) noexcept {
    if (layer == kSurf) return c.water_conductivity;
    if (!(thick_m > 0.0)) return c.water_conductivity;
    const double theta = std::min(1.0, water_m / thick_m);
    return theta * c.water_conductivity + (1.0 - theta) * c.sed_conductivity;
}

/// Solve a tridiagonal system in place (Thomas). `n <= kNL`.
void solveTridiagonal(int n, double (&a)[kNL], double (&b)[kNL],
                      double (&c)[kNL], double (&d)[kNL],
                      double (&x)[kNL]) noexcept {
    double cp[kNL], dp[kNL];
    if (!(std::fabs(b[0]) > 0.0)) { for (int i = 0; i < n; ++i) x[i] = d[i]; return; }
    cp[0] = c[0] / b[0];
    dp[0] = d[0] / b[0];
    for (int i = 1; i < n; ++i) {
        const double m = b[i] - a[i] * cp[i - 1];
        if (!(std::fabs(m) > 0.0)) { for (int k = 0; k < n; ++k) x[k] = d[k]; return; }
        cp[i] = c[i] / m;
        dp[i] = (d[i] - a[i] * dp[i - 1]) / m;
    }
    x[n - 1] = dp[n - 1];
    for (int i = n - 2; i >= 0; --i) x[i] = dp[i] - cp[i] * x[i + 1];
}

}  // namespace

double lidLayerHeatCapacity(const SimulationContext& ctx,
                            const lid::LIDGroupSoA& g, std::size_t unit,
                            int layer) noexcept {
    if (layer < 0 || layer >= kNL) return 0.0;
    double v[kNL], thick[kNL];
    layerVolumes(g, unit, v);
    layerThickness(g, unit, thick);
    if (!(thick[layer] > 0.0)) return 0.0;
    return heatCapacity(ctx.heat_config.conduction, layer,
                        v[layer] * kFtToM, thick[layer],
                        ctx.options.water_density,
                        ctx.options.water_specific_heat) * thick[layer];
}

void initLidLayerTemperature(SimulationContext& ctx,
                             const lid::LIDSolver& solver) {
    if (!ctx.options.heat_transport) return;
    auto offsets = buildOffsets(solver);
    const int units = offsets.empty() ? 0 : offsets.back();
    if (units <= 0) return;

    auto& st = ctx.lid_layer_state;
    st.ensureSized(units, static_cast<int>(LidSpecies::COUNT_), offsets);

    const double t_init = ctx.heat_config.global_temp[
        static_cast<int>(HeatSource::INITIAL_STATE)];
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
                // EVERY layer is seeded, wet or dry — unlike the age row,
                // which leaves a dry layer at 0 because 0 is age's "nothing
                // here". A dry layer at 0 °C would be a freezing layer.
                st.value[st.layer_index(flat, static_cast<LidLayer>(k),
                                        kTemp)] = t_init;
            }
        }
    }
}

void setLidInflowTemperature(SimulationContext& ctx, int type_index, int unit,
                             int subcatch, double rain_rate, double q_imperv,
                             double q_perv, double q_runon, double lid_area) {
    auto& st = ctx.lid_layer_state;
    if (!ctx.options.heat_transport || !st.active()) return;
    const auto ut = static_cast<std::size_t>(type_index);
    if (ut + 1 >= st.group_offset.size()) return;
    const int flat = st.group_offset[ut] + unit;
    if (flat < 0 || flat >= st.n_units) return;

    const double q_rain = rain_rate * lid_area;
    const double t_rain = ctx.heat_config.global_temp[
        static_cast<int>(HeatSource::RAINFALL)];

    // Everything captured from the surface around the unit arrives at the
    // temperature H5a publishes for that subcatchment's runoff — the same
    // choice A4 made for age, and for the same reason: reconstructing a mean
    // from the raw subarea rows looks more precise and is wrong, because a
    // subarea can be shedding while its end-of-step stored volume is zero.
    const auto& hs = ctx.heat_state;
    const double t_surface =
        (subcatch >= 0 &&
         static_cast<std::size_t>(subcatch) < hs.subcatch_runoff_temp.size())
            ? hs.subcatch_runoff_temp[static_cast<std::size_t>(subcatch)]
            : t_rain;

    const double q_surface = q_imperv + q_perv + q_runon;
    const double q_tot = q_rain + q_surface;
    const double temp = (q_tot > 0.0)
        ? (q_rain * t_rain + q_surface * t_surface) / q_tot
        : t_rain;
    st.inflow_value[static_cast<std::size_t>(flat) *
                        static_cast<std::size_t>(st.n_species) +
                    static_cast<std::size_t>(kTemp)] = temp;
}

void routeLidLayerTemperature(SimulationContext& ctx,
                              const lid::LIDSolver& solver, double dt) {
    auto& st = ctx.lid_layer_state;
    if (!ctx.options.heat_transport || !st.active() || !(dt > 0.0)) return;

    const double t_air = heat::airTempCelsius(ctx);
    const double rho_w = ctx.options.water_density;
    const double cp_w  = ctx.options.water_specific_heat;
    const auto&  cc    = ctx.heat_config.conduction;
    const bool   do_cond = ctx.heat_config.layer_conduction;
    const bool   do_flux = ctx.heat_config.surface_exchange ||
                           ctx.heat_config.radiative_exchange;

    // H6a: runoff-clock binding, same reasoning as HeatWatershed
    // (SolarRadiation.hpp).
    heat::updateSolarForcing(ctx);

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
            double v_new[kNL], thick[kNL];
            layerVolumes(g, ui, v_new);
            layerThickness(g, ui, thick);

            const double t_ext =
                st.inflow_value[uf * static_cast<std::size_t>(st.n_species) +
                                static_cast<std::size_t>(kTemp)];

            double t_old[kNL];
            for (int k = 0; k < kNL; ++k)
                t_old[k] = st.value[st.layer_index(flat,
                                                   static_cast<LidLayer>(k),
                                                   kTemp)];

            // ---- 1. Advection: complete-mix on the published inflows.
            //         Donors are read from t_old, so a parcel cannot fall
            //         through the whole stack in one step (A4's finding).
            double tk[kNL];
            bool   live[kNL];      ///< holds or receives WATER → advection
            bool   mass[kNL];      ///< has THERMAL MASS → conduction, policy
            for (int k = 0; k < kNL; ++k) {
                const auto vidx = uf * kNL + static_cast<std::size_t>(k);
                const double v_old = st.vol_prev[vidx];
                const double v_in  = std::max(0.0, in_rate[k]) * dt;

                live[k] = (donor[k] != kAbsent) &&
                          (v_old > kTinyVol || v_in > kTinyVol);

                // "Holds water" and "has thermal mass" are NOT the same
                // predicate, and the first draft used one for both. A buried
                // layer is a matrix with water in its voids: dry, it still
                // has `rho_s*cp_s` — the larger term — and it still conducts.
                // A SURFACE layer is ponded water over a face, so dry it has
                // nothing.
                //
                // Conflating them cost two things. The drying soil layer
                // dropped out of the conduction system, leaving the wet
                // surface conducting DIRECTLY into the wet storage layer
                // across the gap that should insulate them. And it fell
                // under the D-H5c dry policy, which then RESET a layer whose
                // temperature is a real physical state governed by
                // conduction. D-H5c answers "what does an element with no
                // thermal mass report?" — a present soil matrix was never
                // one of those.
                mass[k] = (thick[k] > 0.0) &&
                          ((k == kSurf) ? live[k] : donor[k] != kAbsent);

                if (!live[k]) {
                    // No water to mix. If there is also no thermal mass, the
                    // deck's policy decides; otherwise the value is carried
                    // and the thermal solve below governs it.
                    tk[k] = mass[k] ? t_old[k]
                                    : dryTemperature(ctx, t_old[k], t_air);
                    st.vol_prev[vidx] = v_new[k];
                    continue;
                }

                double tt = t_old[k];
                if (v_in > kTinyVol) {
                    const double t_in =
                        (donor[k] == kExternal) ? t_ext
                        : (donor[k] >= 0)       ? t_old[donor[k]]
                                                : t_ext;
                    const double den = v_old + v_in;
                    tt = (den > kTinyVol)
                             ? (tt * v_old + t_in * v_in) / den
                             : t_in;
                }
                tk[k] = tt;                 // NO aging term, NO zero floor
                st.vol_prev[vidx] = v_new[k];
            }

            if (!do_cond && !do_flux) {
                for (int k = 0; k < kNL; ++k)
                    st.value[st.layer_index(flat, static_cast<LidLayer>(k),
                                            kTemp)] = tk[k];
            } else {
                // ---- 2. ONE coupled thermal solve over the column.
                //
                // Conduction couples adjacent layers, so it is a second
                // operator on the same state. Applying it as its own pass
                // after a per-layer relaxation is exactly the composition
                // defect D-H5e fixed between SurfaceExchange and
                // RadiativeExchange (lesson 80) — it would land on whichever
                // operator ran last. And applying it explicitly is stiff
                // where it matters: a 1e-4 m film against 0.3 m of soil
                // gives k*dt ~ 1.4 at a 60 s step, the regime that produced
                // H5a's NaN.
                //
                // Backward Euler on the coupled system is therefore the
                // scheme: unconditionally stable, and the inter-layer flux
                // is evaluated ONCE at the new time and applied equal and
                // opposite, so the column conserves energy exactly. Only the
                // atmospheric term enters or leaves.
                // Membership is `mass[k]`, not `live[k]` — see the comment
                // where it is computed. A layer conducts if it HAS thermal
                // mass, not if it holds water.
                int  idx[kNL], n = 0;
                for (int k = 0; k < kNL; ++k)
                    if (mass[k]) idx[n++] = k;

                if (n == 0) {
                    for (int k = 0; k < kNL; ++k)
                        st.value[st.layer_index(flat,
                                                static_cast<LidLayer>(k),
                                                kTemp)] = tk[k];
                    continue;
                }

                double cap[kNL], cond[kNL];
                for (int i = 0; i < n; ++i) {
                    const int k = idx[i];
                    const double water_m = v_new[k] * kFtToM;
                    cap[i]  = heatCapacity(cc, k, water_m, thick[k],
                                           rho_w, cp_w) * thick[k];
                    cond[i] = conductivity(cc, k, water_m, thick[k]);
                }

                double a[kNL] = {}, b[kNL] = {}, c[kNL] = {}, rhs[kNL] = {},
                       out[kNL] = {};
                for (int i = 0; i < n; ++i) {
                    b[i]   = (cap[i] > 0.0) ? cap[i] : 1.0;
                    rhs[i] = b[i] * tk[idx[i]];
                }

                if (do_cond) {
                    for (int i = 0; i + 1 < n; ++i) {
                        // Series resistance between layer centres: each half
                        // thickness over its own conductivity. Using a single
                        // averaged k over the whole gap would let a thin
                        // high-conductivity layer dominate a thick one.
                        const double r =
                            0.5 * thick[idx[i]] / std::max(cond[i], 1.0e-12) +
                            0.5 * thick[idx[i + 1]] /
                                std::max(cond[i + 1], 1.0e-12);
                        if (!(r > 0.0)) continue;
                        const double h = dt / r;   // J/m2/K over the step
                        b[i]     += h;  c[i]       = -h;
                        b[i + 1] += h;  a[i + 1]   = -h;
                    }
                }

                if (do_flux) {
                    // Atmospheric flux on the TOPMOST PRESENT layer only.
                    // A buried layer has no sky and no wind; the surface
                    // layer of a stack that has one is where the exchange
                    // happens, and for a rain barrel that layer is storage.
                    const int top = idx[0];
                    const double j0 = heat::netFluxOut(ctx, tk[top]);
                    const double j1 =
                        heat::netFluxOut(ctx, tk[top] + heat::kProbeC);
                    const double djdt = (j1 - j0) / heat::kProbeC;
                    // Linearize as J(T) = j0 + J'(T - T0), the same
                    // linearization relaxT performs, so the coupled solve
                    // and the single-element relaxation agree in the limit
                    // of one layer and no conduction.
                    if (djdt > 0.0) {
                        b[0]   += dt * djdt;
                        rhs[0] += dt * (djdt * tk[top] - j0);
                    } else {
                        rhs[0] -= dt * j0;
                    }
                }

                solveTridiagonal(n, a, b, c, rhs, out);
                for (int k = 0; k < kNL; ++k)
                    st.value[st.layer_index(flat, static_cast<LidLayer>(k),
                                            kTemp)] = tk[k];
                for (int i = 0; i < n; ++i)
                    if (std::isfinite(out[i]))
                        st.value[st.layer_index(
                            flat, static_cast<LidLayer>(idx[i]), kTemp)] =
                            out[i];
            }

            // The underdrain draws from the storage layer, matching A4's
            // decision and `getDrainRate`'s own read of `stor_depth`. Roof
            // disconnection has no storage: its drain is a routed fraction
            // of the roof outflow, so it leaves at the surface temperature.
            const bool from_surface = (g.type == lid::LIDType::ROOF_DISCON);
            const auto src = from_surface ? LidLayer::SURFACE
                                          : LidLayer::STORAGE;
            st.drain_value[uf * static_cast<std::size_t>(st.n_species) +
                           static_cast<std::size_t>(kTemp)] =
                st.value[st.layer_index(flat, src, kTemp)];
        }
    }
}

}  // namespace openswmm::transport
