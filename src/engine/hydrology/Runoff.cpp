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
 * @file Runoff.cpp
 * @brief Subcatchment runoff — 3-subarea nonlinear reservoir model.
 *
 * @details Three subareas per subcatchment (matching legacy subcatch.c):
 *   - IMPERV0: Impervious with zero depression storage (PctZero fraction)
 *   - IMPERV1: Impervious with depression storage
 *   - PERV:    Pervious with depression storage and infiltration
 *
 *   Depth integration uses RK45 Cash-Karp adaptive ODE solver via
 *   ode::integrate() from OdeSolver.hpp, matching the legacy subcatch.c
 *   updatePondedDepth() + odesolve_integrate() approach exactly.
 *
 *   The legacy fills depression storage first (reducing the integration
 *   interval), then solves dd/dt = inflow - alpha*(d-Ds)^(5/3) implicitly.
 *
 * @note Legacy reference: src/legacy/engine/subcatch.c, odesolve.c
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "Runoff.hpp"
#include "Gage.hpp"
#include "../core/Constants.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../math/OdeSolver.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>
#include <stdexcept>

#if defined(SWMM_USE_OPENMP)
#include <omp.h>
#endif

namespace openswmm {
namespace runoff {

// ============================================================================
// RunoffSoA
// ============================================================================

void RunoffSoA::resize(int n) {
    n_subcatch = n;
    auto un = static_cast<std::size_t>(n);

    area.assign(un, 0.0);
    width.assign(un, 0.0);
    slope.assign(un, 0.0);
    imperv_pct.assign(un, 0.0);
    frac_imperv0.assign(un, 0.0);
    frac_imperv1.assign(un, 0.0);

    alpha_imperv.assign(un, 0.0);
    alpha_perv.assign(un, 0.0);
    ds_imperv.assign(un, 0.0);
    ds_perv.assign(un, 0.0);
    n_imperv.assign(un, 0.01);
    n_perv.assign(un, 0.1);

    depth_imperv0.assign(un, 0.0);
    depth_imperv1.assign(un, 0.0);
    depth_perv.assign(un, 0.0);

    old_runoff_imperv0.assign(un, 0.0);
    old_runoff_imperv1.assign(un, 0.0);
    old_runoff_perv.assign(un, 0.0);

    runoff.assign(un, 0.0);
    evap_loss.assign(un, 0.0);
    infil_loss.assign(un, 0.0);
    perv_evap_vol.assign(un, 0.0);
    actual_perv_evap_vol.assign(un, 0.0);
    infil_vol.assign(un, 0.0);
    spatial_infil_vol.assign(un,0.0);native_infil_rate.assign(un,0.0);
    subarea_runoff_rate.assign(un, 0.0);
    imperv_runoff_cfs.assign(un, 0.0);
    perv_runoff_cfs.assign(un, 0.0);
    outflow_vol.assign(un, 0.0);
}

void RunoffSoA::computeAlpha() {
    // Alpha = PHI * width * sqrt(slope) / (N * subarea_ft2)
    // Legacy: subcatch_getAlpha() in subcatch.c
    // Both IMPERV0 and IMPERV1 use the same alpha (same N, same combined area)
    for (int i = 0; i < n_subcatch; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double sq_slope = std::sqrt(slope[ui]);
        double fi = imperv_pct[ui];
        double fp = 1.0 - fi;
        if (area[ui] > 0.0) {
            double area_imperv = area[ui] * fi;
            double area_perv   = area[ui] * fp;
            // Match legacy subcatch.c:398-399 operand order EXACTLY:
            //   PHI * width / area * sqrt(slope) / N
            // i.e. divide by area BEFORE multiplying sqrt(slope), and divide by N
            // as a separate final step — do NOT fold (N*area) into one denominator
            // (that reassociation differs by ~1 ULP and biases every pow() output).
            alpha_imperv[ui] = (n_imperv[ui] > 0.0 && area_imperv > 0.0)
                ? PHI * width[ui] / area_imperv * sq_slope / n_imperv[ui] : 0.0;
            alpha_perv[ui] = (n_perv[ui] > 0.0 && area_perv > 0.0)
                ? PHI * width[ui] / area_perv * sq_slope / n_perv[ui] : 0.0;
        }
    }
}

// ============================================================================
// Ponded depth update via RK45 ODE solver
// Matches legacy subcatch.c updatePondedDepth() + odesolve_integrate()
// ============================================================================

void RunoffSolver::updatePondedDepth(double& depth, double inflow,
                                      double alpha, double dStore, double dt,
                                      double& t_runoff) {
    double tx = dt;

    // --- Check if not enough inflow to fill depression storage ---
    // Matches legacy subcatch.c line 1046
    if (depth + inflow * tx <= dStore) {
        depth += inflow * tx;
    } else {
        // --- Fill depression storage first, reduce remaining time ---
        // Matches legacy subcatch.c lines 1054-1059
        double dx = dStore - depth;
        if (dx > 0.0 && inflow > 0.0) {
            tx -= dx / inflow;
            depth = dStore;
        }

        // --- Integrate depth via RK45 ODE solver over remaining time ---
        // Matches legacy subcatch.c lines 1063-1067
        // ODE: dd/dt = inflow - alpha * max(0, d - Ds)^(5/3)
        if (alpha > 0.0 && tx > 0.0) {
            double captured_inflow = inflow;
            double captured_alpha  = alpha;
            double captured_dStore = dStore;

            ode::integrate(&depth, 1, 0.0, tx, ODETOL, tx,
                [captured_inflow, captured_alpha, captured_dStore]
                (double /*t*/, const double* d, double* dddt) {
                    double rx = *d - captured_dStore;
                    double outflow = (rx > 0.0)
                        ? captured_alpha * std::pow(rx, MEXP) : 0.0;
                    *dddt = captured_inflow - outflow;
                });
        } else {
            tx = std::max(tx, 0.0);
            depth += inflow * tx;
        }
    }

    // --- Clamp to non-negative ---
    // Matches legacy subcatch.c line 1077
    depth = std::max(depth, 0.0);

    // --- legacy `*dt = tx`: the time the depth spent above storage
    t_runoff = tx;
}

// ============================================================================
// Init
// ============================================================================

double RunoffSolver::pendingRoutingVolume(const SimulationContext& ctx, int source) const {
    const auto i = static_cast<std::size_t>(source);
    const auto& s = soa_;
    if (s.imperv_pct[i] == 0.0 || s.imperv_pct[i] == 1.0) return 0.0;
    double rate = 0.0;
    if (ctx.subcatches.subarea_routing[i] == 2)
        rate = s.old_runoff_imperv0[i] * s.frac_imperv0[i] +
               s.old_runoff_imperv1[i] * s.frac_imperv1[i];
    else if (ctx.subcatches.subarea_routing[i] == 1 && s.frac_imperv1[i] > 0.0)
        rate = s.old_runoff_perv[i] * (1.0 - s.imperv_pct[i]);
    return rate * ctx.subcatches.pct_routed[i] * s.area[i] * completed_step_seconds_[i];
}

void RunoffSolver::init(SimulationContext& ctx, const std::vector<std::pair<int, double>>& spatial_areas) {
    int n = ctx.n_subcatches();
    std::vector<double> external_area(static_cast<std::size_t>(n), -1);
    for (const auto& [i, area] : spatial_areas) {
        if (i < 0 || i >= n || !std::isfinite(area) || area < 0 || external_area[i] >= 0)
            throw std::invalid_argument("Invalid or duplicate reviewed non-LID source area.");
        external_area[i] = area;
    }
    soa_.resize(n);
    completed_step_seconds_.assign(static_cast<std::size_t>(n), 0.0);
    spatial_full_area_ft2_.clear();
    if (!spatial_areas.empty()) spatial_full_area_ft2_.assign(static_cast<std::size_t>(n), -1);

    double ucf_area  = ucf::UCF(ucf::LANDAREA,  ctx.options);
    double ucf_depth = ucf::UCF(ucf::RAINDEPTH, ctx.options);

    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        // Gap #23: subareas must use non-LID area, matching legacy subcatch_validate()
        // which sets nonLidArea = area - lidArea before computing alpha and subarea sizes.
        double full_area_ft2 = ctx.subcatches.area[ui] / ucf_area;
        double lid_area_ft2  = ctx.subcatches.total_lid_area_ft2[ui]; // already in ft²
        soa_.area[ui]       = std::max(0.0, full_area_ft2 - lid_area_ft2);
        if (external_area[ui] >= 0) {
            soa_.area[ui] = external_area[ui] / (.3048 * .3048);
            spatial_full_area_ft2_[ui] = soa_.area[ui] + lid_area_ft2;
        }
        soa_.width[ui]      = ctx.subcatches.width[ui];
        soa_.slope[ui]      = ctx.subcatches.slope[ui];
        soa_.imperv_pct[ui] = ctx.subcatches.frac_imperv[ui];
        // PARITY subcatch.c:268-270 — multiply by the AUTHORED percent,
        // divide by 100 afterwards. See RunoffSoA::frac_imperv0.
        const double fi_ = ctx.subcatches.frac_imperv[ui];
        const double pz_ = ctx.subcatches.pct_zero[ui];
        soa_.frac_imperv0[ui] = fi_ * pz_ / 100.0;
        soa_.frac_imperv1[ui] = fi_ * (1.0 - pz_ / 100.0);
        soa_.n_imperv[ui]   = ctx.subcatches.n_imperv[ui];
        soa_.n_perv[ui]     = ctx.subcatches.n_perv[ui];
        soa_.ds_imperv[ui]  = ctx.subcatches.ds_imperv[ui] / ucf_depth;
        soa_.ds_perv[ui]    = ctx.subcatches.ds_perv[ui]   / ucf_depth;
    }
    soa_.computeAlpha();

    infil_bank_.init(n);
    infil_factor_used_.assign(static_cast<std::size_t>(n), 1.0);
    for (int i = 0; i < n; ++i) {
        const auto ui = static_cast<std::size_t>(i);
        // Preserve legacy subcatchment zero-drying-time normalization.
        const double dry_time = ctx.subcatches.infil_p4[ui] == 0.0
            ? constants::TINY : ctx.subcatches.infil_p4[ui];
        const double p[5] = {ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p2[ui],
            ctx.subcatches.infil_p3[ui], dry_time, ctx.subcatches.infil_p5[ui]};
        infil_bank_.setMethod(i, static_cast<InfilModel>(ctx.subcatches.infil_model[ui]), p, ctx.options);
    }
}

// ============================================================================
// Execute — one runoff timestep for ALL subcatchments
// ============================================================================

// Legacy infil_getInfil (infil.c): dispatch on the subcatchment's model with
// the [ADJUSTMENTS] / pattern factor and the evaporation recovery factor
// applied the way each model applies them.
double RunoffSolver::infilGetInfil(SimulationContext& ctx, int i, double precip,
                                   double runon, double depth, double dt,
                                   double local_infil, double recovery_factor) {
    (void)ctx;
    return infil_bank_.rateFeet(i, precip, runon, depth, dt, {local_infil, recovery_factor});
}

// Legacy findNativeInfil (lid.c) for a subcatchment whose non-LID area is
// absent or fully impervious: the native soil's infiltration rate for the
// subcatchment's own rainfall + runon and pervious ponded depth — a call
// that advances the subcatchment's infiltration state, as legacy's does.
double RunoffSolver::nativeInfilFullLid(SimulationContext& ctx, int i, double dt,
                                        double recovery_factor) {
    auto ui = static_cast<std::size_t>(i);
    const double rain = ctx.subcatches.rainfall[ui];
    const double runon = ctx.subcatches.runon_rate[ui];   // legacy Subcatch.runon
    const double depth = soa_.depth_perv[ui];
    return infilGetInfil(ctx, i, rain, runon, depth, dt,
                         infil_factor_used_[ui], recovery_factor);
}

void RunoffSolver::execute(SimulationContext& ctx, double dt, double evap_rate_in,
                           double infil_factor, double recovery_factor, int month,
                           const InfiltrationBoundary* boundary,
                           const std::vector<RunoffSourceForcing>* source_forcing,
                           const std::vector<double>* spatial_fractions) {
    int n = soa_.n_subcatch;
    if (source_forcing) {
        if (!std::isfinite(dt) || dt <= 0.0 || ctx.n_subcatches() != n)
            throw std::invalid_argument("Completed source solve requires a positive interval and matching source count.");
        int previous = -1;
        for (const auto& s : *source_forcing) {
            if (s.subcatch <= previous || s.subcatch >= n ||
                !std::isfinite(s.rain) || s.rain < 0.0 || !std::isfinite(s.pet) || s.pet < 0.0)
                throw std::invalid_argument("Completed source rates require ordered unique sources and finite nonnegative rates.");
            previous = s.subcatch;
        }
    }
    if (spatial_fractions) {
        if (!source_forcing || spatial_fractions->size()!=static_cast<std::size_t>(n))
            throw std::invalid_argument("Partial spatial intake requires completed forcing and matching fractions.");
        for (double f:*spatial_fractions) if(!std::isfinite(f)||f<0||f>1)
            throw std::invalid_argument("Invalid partial spatial intake fraction.");
    }
    if (n == 0) return;

    const int count = source_forcing ? static_cast<int>(source_forcing->size()) : n;
    const auto source = [&](int slot) { return source_forcing ? (*source_forcing)[slot].subcatch : slot; };

    auto un = static_cast<std::size_t>(n);
    precip_.resize(un);
    evap_rate_.resize(un);
    infil_rate_.resize(un);
    const double ucf_landarea = ucf::UCF(ucf::LANDAREA, ctx.options);

    // ----- Step 1: Rainfall → net precip (ft/sec) -----
    // Matches legacy getNetPrecip(): all subareas get same precipitation rate.
    //
    // The rain/snow split runs for EVERY subcatchment, not just those with a
    // snow pack — legacy gage_getPrecip() is called unconditionally from
    // getNetPrecip() (subcatch.c:772), and for a snowpack-less subcatchment
    // netPrecip is then rainfall + snowfall (subcatch.c:793). Since snowfall
    // carries the gage snow catch factor, omitting the split here made
    // snowpack-less subcatchments diverge from legacy whenever SCF != 1.0.
    //
    // splitPrecip() also applies the subcatchment rain/snow scale factors.
    // Any subcatchment rainfall forcing resolves on top (OVERRIDE replaces the
    // gage value, ADD augments it) so it cannot be clobbered by the gage
    // re-read — same pattern as the PET forcing below.
    for (int slot = 0; slot < count; ++slot) {
        const int i = source(slot);
        auto ui = static_cast<std::size_t>(i);
        if (source_forcing) {
            precip_[ui] = (*source_forcing)[slot].rain;
            ctx.subcatches.rainfall[ui] = precip_[ui];
            continue;
        }
        gage::PrecipSplit p = gage::splitPrecip(ctx, ui);  // ft/sec
        double rain = p.rainfall;
        // The forcing channel speaks user units; the x*UCF/UCF round trip
        // is not an identity in floating point, so it runs only when a
        // forcing is actually set (legacy has no such channel).
        if (ui < ctx.forcing.subcatch_rainfall_mode.size() &&
            ctx.forcing.subcatch_rainfall_mode[ui] != ForcingMode::NONE) {
            double rain_inhr = p.rainfall * ucf::UCF(ucf::RAINFALL, ctx.options);
            rain_inhr = ctx.forcing.effective_rainfall(ui, rain_inhr);
            rain = rain_inhr / ucf::UCF(ucf::RAINFALL, ctx.options);
        }
        double snow = ctx.forcing.effective_snowfall(ui, p.snowfall);
        precip_[ui] = rain + snow;
        ctx.subcatches.rainfall[ui] = precip_[ui];  // ft/sec (internal units)
    }

    // ----- Step 2: Evaporation rate -----
    // Wire climate module evaporation: use the global evap rate computed
    // by climate::updateDailyClimate(). Matches legacy:
    //   evapRate = (dryOnly && rainfall > 0) ? 0 : Evap.rate
    // Any prescribed PET forcing then resolves per subcatchment: an OVERRIDE
    // rate is used as-is (bypasses DRY_ONLY); ADD augments the climate rate.
    for (int slot = 0; slot < count; ++slot) {
        const int i = source(slot);
        auto ui = static_cast<std::size_t>(i);
        if (source_forcing) {
            evap_rate_[ui] = (*source_forcing)[slot].pet;
            continue;
        }
        bool is_dry_only = ctx.options.evap_dry_only;
        double rain = precip_[ui] * ucf::UCF(ucf::RAINFALL, ctx.options);
        double broadcast = (is_dry_only && rain > 0.0) ? 0.0 : evap_rate_in;
        evap_rate_[ui] = ctx.forcing.effective_evap_rate(ui, broadcast);
    }

    // ----- Step 3: Per-subcatchment subarea processing -----
    // Matches legacy subcatch_getRunoff() → getSubareaRunoff() chain exactly.
    // Infiltration and evaporation are computed INSIDE the per-subarea loop
    // to replicate the legacy's loss-limiting and inflow-subtraction order.
    for (int slot = 0; slot < count; ++slot) {
        const int i = source(slot);
        auto ui = static_cast<std::size_t>(i);
        double fi = soa_.imperv_pct[ui];
        double fp = 1.0 - fi;
        double f0 = soa_.frac_imperv0[ui];
        double f1 = soa_.frac_imperv1[ui];
        double total_area = soa_.area[ui];  // ft²

        double precip  = precip_[ui];     // ft/sec (rainfall + snowmelt)
        double evapRate = evap_rate_[ui]; // ft/sec (global evap rate)

        double alpha_i = soa_.alpha_imperv[ui];
        double alpha_p = soa_.alpha_perv[ui];

        // Mass balance accumulators (matching legacy Vevap, Vinfil, Voutflow)
        double Vevap    = 0.0;  // Total evaporation volume (ft³)
        double Vpevap   = 0.0;  // Pervious-subarea evaporation volume (ft³), legacy Vpevap
        double actual_perv_evap = 0.0;
        double Vspatial = 0.0;
        soa_.native_infil_rate[ui]=0;
        double Vinfil   = 0.0;  // Total infiltration volume (ft³)
        double Voutflow = 0.0;  // Total runoff volume (ft³)

        // Helper: process one subarea following legacy getSubareaRunoff() exactly.
        // Args: depth, alpha, dStore, subareaFrac, isPervious, runon
        auto processSubarea = [&](double& depth, double alpha, double dStore,
                                  double frac, bool isPervious,
                                  double runon_in, double subarea_n) -> double {
            if (frac <= 0.0) return 0.0;
            double subarea_area = total_area * frac;
            if (spatial_fractions && subarea_area <= 0.0) return 0.0;
            const double initial_depth = depth;

            // Step 3.1: Available surface moisture (legacy line 923)
            double surfMoisture = depth / dt;

            // Step 3.2: Limit evaporation to available moisture (legacy line 924)
            double surfEvap = std::min(surfMoisture, evapRate);

            // Step 3.3: Infiltration — pervious only (legacy line 927)
            // Called INSIDE the per-subarea loop with subarea inflow as runon.
            // For RouteTo=OUTLET models, subarea->inflow = 0 (no inter-subarea runon).
            double infil = 0.0;
            const double available = std::max(0.0, surfMoisture + precip + runon_in - surfEvap);
            const double spatial_fraction=spatial_fractions ? (*spatial_fractions)[ui] : 1.0;
            const bool external = isPervious && spatial_fraction>0 && boundary && (*boundary)(i, depth, available, infil);
            if (isPervious && spatial_fractions && spatial_fraction > 0 && !external)
                throw std::runtime_error("Completed inside pervious area requires a spatial infiltration boundary.");
            double spatial_rate=0.0;
            if (external) {
                if (!std::isfinite(infil) || infil < 0.0 || ctx.subcatches.gw_aquifer[ui] >= 0)
                    throw std::invalid_argument("External source intake requires a finite nonnegative rate and no lumped aquifer.");
                infil = std::min(infil, available);
                spatial_rate=infil;
            }
            if (isPervious && (!external || spatial_fraction<1)) {
                // Legacy: infil_getInfil(j, tStep, precip, subarea->inflow, depth)
                //   → horton_getInfil(state, tStep, precip + runon, depth)
                // subarea->inflow here is the runon from other subareas (0 for OUTLET routing).
                double runon = runon_in;  // runon from inter-subarea routing
                // Per-subcatchment INFIL pattern override
                // (matching legacy infil_setInfilFactor per subcatchment)
                double local_infil = infil_factor;
                if (month >= 0 && ui < ctx.subcatch_infil_pattern.size()) {
                    int pi = ctx.subcatch_infil_pattern[ui];
                    if (pi >= 0 && static_cast<std::size_t>(pi) < ctx.patterns.factors.size()) {
                        const auto& facs = ctx.patterns.factors[static_cast<std::size_t>(pi)];
                        auto umon = static_cast<std::size_t>(month);
                        if (umon < facs.size())
                            local_infil = facs[umon];
                    }
                }

                infil_factor_used_[ui] = local_infil;
                const double native = infilGetInfil(ctx, i, precip, runon,
                    spatial_fractions ? std::max(0.0,depth-surfEvap*dt) : depth, dt,
                    local_infil, recovery_factor);
                soa_.native_infil_rate[ui]=spatial_fractions ? std::min(native,available) : native;
                infil=external ? spatial_fraction*spatial_rate+(1-spatial_fraction)*soa_.native_infil_rate[ui]
                               : soa_.native_infil_rate[ui];
            }

            // Gap #40: limit pervious infiltration by GW upper zone capacity.
            // Matching legacy subcatch.c getSubareaInfil():
            //   infil = MIN(infil, GW->maxInfilVol / tStep)
            if (isPervious && !external && dt > 0.0) {
                double max_iv = ctx.subcatches.gw_max_infil_vol[ui];
                if (max_iv < 1.0e30)
                    infil = std::min(infil, max_iv / dt);
            }

            // Step 3.4: Accumulate inflow and moisture (legacy lines 930-931)
            double inflow = precip + runon_in;  // precip + inter-subarea runon
            surfMoisture += inflow;

            // Step 3.5: Update mass balance volumes (legacy lines 934-937)
            Vevap  += surfEvap * subarea_area * dt;
            if (isPervious) actual_perv_evap += surfEvap * subarea_area * dt;
            // legacy subcatch.c:984 `if (i == PERV) Vpevap += Vevap;` — the
            // WHOLE subcatchment's surface evaporation so far (the pervious
            // subarea is processed last, after both impervious ones), not
            // the pervious share. gwater_getGroundwater subtracts it from
            // MaxEvap = Evap.rate * FracPerv, so an impervious cover
            // evaporating at the full rate leaves the aquifer no evaporation
            // (runoff41-sw5: legacy's soil moisture holds at field capacity).
            if (isPervious) Vpevap += Vevap;
            Vinfil += infil * subarea_area * dt;
            if(external) Vspatial+=spatial_rate*spatial_fraction*subarea_area*dt;

            // Step 3.6: Loss check shortcut (legacy lines 945-948)
            // If evaporation + infiltration >= total available moisture,
            // all water is consumed — no runoff, depth goes to zero.
            double runoff_rate = 0.0;
            double t_runoff = dt;   // legacy tRunoff
            if (surfEvap + infil >= surfMoisture) {
                depth = 0.0;
            } else {
                // Step 3.7: Subtract losses from inflow before ODE. Legacy is
                // `subarea->inflow -= surfEvap + infil` (subcatch.c:1002) — the
                // two losses are summed FIRST and the sum subtracted once.
                // Written as `inflow - surfEvap - infil` the subtraction
                // rounds twice and lands one ULP away whenever the pervious
                // infiltration is small next to the evaporation; that ULP
                // seeds the whole network (user3's CCECROY at runoff step
                // 11641 of 43200, then every link flow from report period 107
                // on). Impervious subareas have infil == 0.0 and are
                // unaffected either way.
                double net_inflow = inflow - (surfEvap + infil);
                // Step 3.8: Integrate ponded depth (legacy updatePondedDepth:
                // with alpha 0 the depth simply accumulates), and keep the
                // time it spent above storage for the no-routing case.
                updatePondedDepth(depth, net_inflow, alpha, dStore, dt, t_runoff);
            }

            // Step 3.9: legacy findSubareaRunoff, keyed on Manning's N — NOT
            // on alpha: a subarea with N > 0 on a ZERO-slope subcatchment has
            // alpha 0 and legacy's runoff is Alpha*pow(...) = 0, the water
            // ponds; only N == 0 drains the excess instantly, over tRunoff.
            // Keyed on alpha, the flat S27 of 185-h-h-elements-si-units
            // (%Slope 0) drained instantly where legacy reports no runoff.
            {
                const double xDepth = depth - dStore;
                if (xDepth > 1.0e-10) {          // legacy ZERO
                    if (subarea_n > 0.0) {
                        runoff_rate = alpha * std::pow(xDepth, MEXP);
                    } else {
                        runoff_rate = xDepth / t_runoff;
                        depth = dStore;
                    }
                }
            }

            // A completed source interval transfers its actual discharged
            // volume. The legacy endpoint-rate rectangle is retained when
            // no explicit source batch is supplied. For the coupled path,
            // the reservoir's integrated storage change supplies the mean
            // outflow, also used by subsequent capture/subarea bookkeeping.
            if (source_forcing)
                runoff_rate = std::max(0.0, (initial_depth - depth) / dt +
                    (inflow - (surfEvap + infil)));

            // Step 3.10: Accumulate outlet volume (legacy line 964)
            // fOutlet is the fraction of runoff that goes directly to outlet.
            // For TO_OUTLET routing (default), fOutlet = 1.0.
            // For inter-subarea routing, fOutlet = 1 - pct_routed for the
            // routed subarea, and 1.0 for the receiving subarea.
            double fOutlet = 1.0;
            int route_mode = ctx.subcatches.subarea_routing[ui];
            double pct = ctx.subcatches.pct_routed[ui];
            // legacy subcatch_readSubareaParams (subcatch.c:282-284): a 0% or
            // 100% impervious subcatchment has no second subarea to route to,
            // so its routing is forced TO_OUTLET and every fOutlet stays 1.
            if (soa_.imperv_pct[ui] == 0.0 || soa_.imperv_pct[ui] == 1.0)
                route_mode = 0;
            if (route_mode == 2 && !isPervious) {
                // IMPERV → PERV: impervious fOutlet = 1 - pct_routed
                fOutlet = 1.0 - pct;
            } else if (route_mode == 1 && isPervious) {
                // PERV → IMPERV: pervious fOutlet = 1 - pct_routed
                fOutlet = 1.0 - pct;
            }
            Voutflow += fOutlet * runoff_rate * subarea_area * dt;

            return runoff_rate;
        };

        // --- Subarea inflows (legacy subcatch_getRunon), in its order ---
        // Every subarea's inflow starts with the subcatchment's run-on from
        // upstream subcatchments / outfalls / LID drains (subcatch_addRunonFlow
        // adds the same depth rate to all three; runon_rate is that sum,
        // formed in legacy's order by SWMMEngine::assembleRunon), then the
        // inter-subarea transfer of the PREVIOUS step's runoff, then the LID
        // return flow onto the pervious subarea. The run-on is NOT rainfall:
        // it reaches the infiltration kernel as `runon` (Horton / Green-Ampt
        // add it to the rate, the curve number adds it to the ponded depth —
        // infil_getInfil), and the snow-modified per-subarea precipitation
        // below replaces only the precipitation. Folded into `precip` it
        // was infiltrated as rain on a curve-number deck whose subcatchments
        // drain onto each other (185-h-h-elements-si-units S20 -> S21) and
        // vanished on a snow deck.
        // route_mode: 0=TO_OUTLET, 1=TO_IMPERV (perv→imperv), 2=TO_PERV (imperv→perv)
        int route_mode = ctx.subcatches.subarea_routing[ui];
        double pct = ctx.subcatches.pct_routed[ui];
        const double f_outlet = 1.0 - pct;   // legacy subArea.fOutlet
        // A changed interval duration must deliver the previous interval's
        // routed VOLUME, not hold its rate across the new duration.
        const double history_scale = source_forcing ? completed_step_seconds_[ui] / dt : 1.0;
        const double runon_sub = (total_area > 0.0) ? ctx.subcatches.runon_rate[ui] : 0.0;
        double runon_imperv0 = runon_sub;
        double runon_imperv1 = runon_sub;
        double runon_perv    = runon_sub;

        if (route_mode == 2 && fp > 0.0) {
            // legacy Case 1, imperv --> perv: area-weighted outflow of both
            // impervious subareas, (1 - fOutlet) of it, over the pervious area
            double q1 = soa_.old_runoff_imperv0[ui] * f0;
            double q2 = soa_.old_runoff_imperv1[ui] * f1;
            double q  = q1 + q2;
            runon_perv += q * (1.0 - f_outlet) / fp * history_scale;
        }
        else if (route_mode == 1 && f1 > 0.0) {
            // legacy Case 2, perv --> imperv (needs an IMPERV1 area)
            double q = soa_.old_runoff_perv[ui];
            runon_imperv1 += q * (1.0 - f_outlet) * fp / f1 * history_scale;
        }

        // Gap #23: LID return flow to pervious area (legacy lid_getFlowToPerv
        // in subcatch_getRunon — one-step lag), after the transfer above.
        {
            double q_ret = ctx.subcatches.lid_return_to_perv_cfs[ui];
            if (q_ret > 0.0 && total_area > 0.0 && fp > 0.0) {
                double perv_area = total_area * fp;
                runon_perv += q_ret / perv_area;  // ft/sec over pervious area
            }
            ctx.subcatches.lid_return_to_perv_cfs[ui] = 0.0;  // consume
        }

        // Gap #20: When snow is active, use snow-modified net precip per subarea
        // (imelt + rainfall*(1-asc)) instead of raw rainfall.
        // precip is captured by reference in processSubarea, so setting it here
        // controls the inflow for each subarea call.
        double precip_imperv = precip;
        double precip_perv   = precip;
        // IGNORE_SNOWMELT: fall back to raw gage precip for both subareas
        // (legacy subcatch.c:784 `Subcatch[j].snowpack && !IgnoreSnowmelt`).
        // Pairs with the snow-block skip in SWMMEngine::stepRunoff so the now
        // stale snow_net_* arrays are never read.
        if (ctx.subcatches.snowpack[ui] >= 0 && !ctx.options.ignore_snow_melt) {
            double sni = ctx.subcatches.snow_net_imperv[ui];
            double snp = ctx.subcatches.snow_net_perv[ui];
            if (sni >= 0.0) precip_imperv = sni;
            if (snp >= 0.0) precip_perv   = snp;
        }

        // Process all 3 subareas (matching legacy loop: IMPERV0, IMPERV1, PERV)
        precip = precip_imperv;
        double runoff0  = processSubarea(soa_.depth_imperv0[ui], alpha_i, 0.0,
                                         f0, false, runon_imperv0, soa_.n_imperv[ui]);
        double runoff1  = processSubarea(soa_.depth_imperv1[ui], alpha_i,
                                         soa_.ds_imperv[ui], f1, false, runon_imperv1,
                                         soa_.n_imperv[ui]);
        // legacy adjustSubareaParams (subcatch.c:1168-1200): the PERVIOUS
        // subarea's depression storage and runoff coefficient carry this
        // month's [ADJUSTMENTS] DSTORE / N-PERV pattern, re-applied to the
        // STORED values every runoff step. Alpha is DIVIDED by the roughness
        // factor — not re-derived from n*f, which rounds differently — and a
        // factor <= 0 zeroes it outright. v6 instead scaled ctx.subcatches
        // .n_perv / .ds_perv once per step, which the runoff kernel never
        // read back (its alphas and storages are snapshotted in init()), so
        // both adjustments were inert.
        double alpha_p_adj  = alpha_p;
        double ds_perv_adj  = soa_.ds_perv[ui];
        if (month >= 0) {
            const auto umon = static_cast<std::size_t>(month);
            auto monthly = [&](const std::vector<int>& pat, double& out) -> bool {
                if (ui >= pat.size()) return false;
                const int pi = pat[ui];
                if (pi < 0) return false;
                const auto upi = static_cast<std::size_t>(pi);
                if (upi >= ctx.patterns.types.size() || ctx.patterns.types[upi] != 0)
                    return false;
                const auto& facs = ctx.patterns.factors[upi];
                if (umon >= facs.size()) return false;
                out = facs[umon];
                return true;
            };
            double f = 0.0;
            if (monthly(ctx.subcatch_d_store_pattern, f) && f >= 0.0)
                ds_perv_adj *= f;
            if (monthly(ctx.subcatch_n_perv_pattern, f))
                alpha_p_adj = (f <= 0.0) ? 0.0 : alpha_p_adj / f;
        }

        precip = precip_perv;
        double runoff_p = processSubarea(soa_.depth_perv[ui], alpha_p_adj,
                                         ds_perv_adj, fp, true, runon_perv,
                                         soa_.n_perv[ui]);
        precip = precip_[ui];   // restore for subsequent use


        // Save per-subarea runoff for next step's inter-subarea routing
        soa_.old_runoff_imperv0[ui] = runoff0;
        soa_.old_runoff_imperv1[ui] = runoff1;
        soa_.old_runoff_perv[ui]    = runoff_p;
        completed_step_seconds_[ui] = dt;

        // legacy subcatch_getRunoff's return value (subcatch.c:714-724,
        // 773): `runoff += subArea[i].runoff * area_i` over the three
        // subareas when nonLidArea > 0, then `/ Subcatch.area` — the whole
        // surface's runoff BEFORE the imperv->perv / perv->imperv transfer
        // and the LID exchange. runoff_execute keeps the WET step while it
        // is positive: a zero-storage impervious subarea routed to the
        // pervious one trickles for hours after the rain, and the outlet
        // runoff (newRunoff) is 0 the whole time (117-h-h-elements-si-units
        // took the 1 h DRY step there, so its curve-number ponding was
        // tested against MIN_TOTAL_DEPTH an hour late).
        {
            double sum = 0.0;
            if (total_area > 0.0) {
                sum += runoff0  * (total_area * f0);
                sum += runoff1  * (total_area * f1);
                sum += runoff_p * (total_area * fp);
            }
            const double full_area = !spatial_full_area_ft2_.empty() && spatial_full_area_ft2_[ui] >= 0
                ? spatial_full_area_ft2_[ui] : ctx.subcatches.area[ui] / ucf_landarea;
            soa_.subarea_runoff_rate[ui] = (full_area > 0.0) ? sum / full_area : 0.0;
        }

        // Gap #23: Store per-subarea runoff CFS for LID inflow computation.
        // Matches legacy qImperv/qPerv used in lid_getRunoff() lid.c line ~1669.
        //
        // Legacy getImpervAreaRunoff()/getPervAreaRunoff() scale by the same
        // fOutlet the outlet volume above uses, so only the share that actually
        // reaches the outlet is offered to the LID. `total_area` is already
        // max(0, area − lidArea), i.e. legacy's nonLidArea. Without the fOutlet
        // factor the LID inflow is overstated on any deck with inter-subarea
        // routing, and the −VlidIn subtraction in SWMMEngine::stepRunoff — which
        // removes exactly this captured share from the outlet runoff — could
        // then drive that runoff negative.
        // legacy getImpervAreaRunoff / getPervAreaRunoff op order (lid.c:
        // 1794-1846): sum the subareas' DEPTH rates weighted by their area
        // FRACTIONS, apply fOutlet to that sum, and multiply by the non-LID
        // area ONCE at the end. Multiplying each term by the area first
        // rounds differently (1-ULP q_imperv/q_perv, which VlidIn carries
        // into the runoff of every LID subcatchment).
        {
            double q_i = runoff0 * f0 + runoff1 * f1;
            if (route_mode == 2 && soa_.imperv_pct[ui] < 1.0) q_i *= 1.0 - pct;
            soa_.imperv_runoff_cfs[ui] = q_i * total_area;
            double q_p = runoff_p * fp;
            if (route_mode == 1 && soa_.imperv_pct[ui] > 0.0) q_p *= 1.0 - pct;
            soa_.perv_runoff_cfs[ui] = q_p * total_area;
        }

        // ----- Step 4: Compute loss rates and net runoff -----
        // Matches legacy lines 700-709.
        // evapLoss/infilLoss stored as area-averaged depth rates (ft/sec)
        // matching legacy: evapLoss = Vevap / tStep / area
        double evapLoss  = (total_area > 0.0) ? Vevap / dt / total_area : 0.0;
        double infilLoss = (total_area > 0.0) ? Vinfil / dt / total_area : 0.0;
        double newRunoff = Voutflow / dt;  // CFS

        soa_.runoff[ui] = newRunoff;
        soa_.outflow_vol[ui] = Voutflow;   // ft3, for the LID volume-domain fold

        // Write back to SimulationContext
        ctx.subcatches.runoff[ui]     = newRunoff;
        // legacy subcatch_getRunoff's RETURN (`runoff / area`, ft/sec) — the
        // rate the surface-quality paths wash off with. Mirrored onto the
        // context so the pollutant and reactions-species kernels share one
        // number; see SubcatchData::subarea_runoff_rate.
        ctx.subcatches.subarea_runoff_rate[ui] = soa_.subarea_runoff_rate[ui];
        // legacy subcatch_getDepth: the area-weighted mean depth of ponded
        // water over the NON-LID subareas, the store findPondedLoads mixes
        // wet deposition and run-on into. `ctx.subcatches.ponded_depth` was
        // declared and read in three places but WRITTEN BY NOBODY, so the
        // ponded pool was permanently empty: the whole findPondedLoads
        // mirror was inert, and a subcatchment shed its rainfall-borne load
        // the instant the rain stopped instead of draining it down the
        // recession. The subarea fractions are the same triple the runoff
        // mass balance and the age/heat watershed mirrors weight with.
        {
            const double fi = soa_.imperv_pct[ui];
            ctx.subcatches.ponded_depth[ui] =
                soa_.depth_imperv0[ui] * soa_.frac_imperv0[ui] +
                soa_.depth_imperv1[ui] * soa_.frac_imperv1[ui] +
                soa_.depth_perv[ui]    * (1.0 - fi);
        }
        ctx.subcatches.evap_loss[ui]  = evapLoss;
        ctx.subcatches.infil_loss[ui] = infilLoss;
        // The volumes themselves, for the groundwater step: legacy hands
        // gwater_getGroundwater Vpevap and Vinfil (+ the LID shares) and
        // divides by the FULL area then by tStep there.
        soa_.perv_evap_vol[ui] = Vpevap;
        soa_.actual_perv_evap_vol[ui] = actual_perv_evap;
        soa_.infil_vol[ui]     = Vinfil;
        soa_.spatial_infil_vol[ui]=Vspatial;

        // Accumulate per-subcatchment statistics (matching legacy stats_updateSubcatchStats)
        ctx.subcatches.stat_evap_vol[ui]  += Vevap;
        ctx.subcatches.stat_infil_vol[ui] += Vinfil;
        // Impervious runoff: IMPERV0 + IMPERV1 subarea contributions
        double area_i0 = total_area * f0;
        double area_i1 = total_area * f1;
        double area_pv = total_area * fp;
        ctx.subcatches.stat_imperv_vol[ui] += (runoff0 * area_i0 + runoff1 * area_i1) * dt;
        ctx.subcatches.stat_perv_vol[ui]   += runoff_p * area_pv * dt;

        // Runoff is stored in subcatches.runoff[i]; routing picks it up via
        // interpolateRunoffToNodes() → nodes.runoff_inflow[] → assembleLateralInflows().
        (void)0;
    }
}

// ============================================================================
// Hot start helpers — Gap #54
// ============================================================================

void RunoffSolver::infil_get_state(int i, int& model, double state[6]) const noexcept {
    infil_bank_.pack(i, model, state);
}
void RunoffSolver::infil_set_state(int i, int model, const double state[6]) noexcept {
    infil_bank_.unpack(i, model, state);
}

} // namespace runoff
} // namespace openswmm
