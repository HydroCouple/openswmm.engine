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
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "Runoff.hpp"
#include "Gage.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include "../math/OdeSolver.hpp"
#include "../math/SIMD.hpp"

#include <cmath>
#include <algorithm>

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
    imperv0_pct.assign(un, 0.0);

    alpha_imperv.assign(un, 0.0);
    alpha_perv.assign(un, 0.0);
    ds_imperv.assign(un, 0.0);
    ds_perv.assign(un, 0.0);
    n_imperv.assign(un, 0.01);
    n_perv.assign(un, 0.1);

    depth_imperv0.assign(un, 0.0);
    depth_imperv1.assign(un, 0.0);
    depth_perv.assign(un, 0.0);

    runoff.assign(un, 0.0);
    evap_loss.assign(un, 0.0);
    infil_loss.assign(un, 0.0);
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
            alpha_imperv[ui] = (n_imperv[ui] > 0.0 && area_imperv > 0.0)
                ? PHI * width[ui] * sq_slope / (n_imperv[ui] * area_imperv) : 0.0;
            alpha_perv[ui] = (n_perv[ui] > 0.0 && area_perv > 0.0)
                ? PHI * width[ui] * sq_slope / (n_perv[ui] * area_perv) : 0.0;
        }
    }
}

// ============================================================================
// Ponded depth update via RK45 ODE solver
// Matches legacy subcatch.c updatePondedDepth() + odesolve_integrate()
// ============================================================================

void RunoffSolver::updatePondedDepth(double& depth, double inflow,
                                      double alpha, double dStore, double dt) {
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
            if (tx < 0.0) tx = 0.0;
            depth += inflow * tx;
        }
    }

    // --- Clamp to non-negative ---
    // Matches legacy subcatch.c line 1077
    if (depth < 0.0) depth = 0.0;
}

double RunoffSolver::getRunoffRate(double depth, double dStore, double alpha) {
    double excess = depth - dStore;
    if (excess > 0.0 && alpha > 0.0)
        return alpha * std::pow(excess, MEXP);
    return 0.0;
}

// ============================================================================
// Init
// ============================================================================

void RunoffSolver::init(SimulationContext& ctx) {
    int n = ctx.n_subcatches();
    soa_.resize(n);

    double ucf_area  = ucf::UCF(ucf::LANDAREA,  ctx.options);
    double ucf_depth = ucf::UCF(ucf::RAINDEPTH, ctx.options);

    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        soa_.area[ui]       = ctx.subcatches.area[ui] / ucf_area;
        soa_.width[ui]      = ctx.subcatches.width[ui];
        soa_.slope[ui]      = ctx.subcatches.slope[ui];
        soa_.imperv_pct[ui] = ctx.subcatches.frac_imperv[ui];
        soa_.imperv0_pct[ui]= ctx.subcatches.frac_imperv_no_store[ui];
        soa_.n_imperv[ui]   = ctx.subcatches.n_imperv[ui];
        soa_.n_perv[ui]     = ctx.subcatches.n_perv[ui];
        soa_.ds_imperv[ui]  = ctx.subcatches.ds_imperv[ui] / ucf_depth;
        soa_.ds_perv[ui]    = ctx.subcatches.ds_perv[ui]   / ucf_depth;
    }
    soa_.computeAlpha();

    horton_states_.resize(static_cast<std::size_t>(n));
    grnampt_states_.resize(static_cast<std::size_t>(n));
    curvenum_states_.resize(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        int im = ctx.subcatches.infil_model[ui];
        switch (im) {
            case 0: case 1:
                infil_model_ = InfilModel::HORTON;
                infil::horton_init(horton_states_[ui],
                    ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p2[ui],
                    ctx.subcatches.infil_p3[ui], ctx.subcatches.infil_p4[ui],
                    ctx.subcatches.infil_p5[ui], ctx.options);
                break;
            case 2: case 3:
                infil_model_ = InfilModel::GREEN_AMPT;
                infil::grnampt_init(grnampt_states_[ui],
                    ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p2[ui],
                    ctx.subcatches.infil_p3[ui], ctx.options);
                break;
            case 4:
                infil_model_ = InfilModel::CURVE_NUM;
                infil::curvenum_init(curvenum_states_[ui],
                    ctx.subcatches.infil_p1[ui], ctx.subcatches.infil_p4[ui]);
                break;
        }
    }
}

// ============================================================================
// Execute — one runoff timestep for ALL subcatchments
// ============================================================================

void RunoffSolver::execute(SimulationContext& ctx, double dt, double evap_rate_in) {
    int n = soa_.n_subcatch;
    if (n == 0) return;

    auto un = static_cast<std::size_t>(n);
    precip_.resize(un);
    evap_rate_.resize(un);
    infil_rate_.resize(un);

    // ----- Step 1: Rainfall → net precip (ft/sec) -----
    // Matches legacy getNetPrecip(): all subareas get same precipitation rate.
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        int gi = ctx.subcatches.gage[ui];
        double rain_inhr = 0.0;
        if (gi >= 0 && gi < ctx.n_gages())
            rain_inhr = ctx.gages.rainfall[static_cast<std::size_t>(gi)];
        precip_[ui] = rain_inhr / ucf::UCF(ucf::RAINFALL, ctx.options);
        ctx.subcatches.rainfall[ui] = rain_inhr;
    }

    // ----- Step 2: Evaporation rate -----
    // Wire climate module evaporation: use the global evap rate computed
    // by climate::updateDailyClimate(). Matches legacy:
    //   evapRate = (dryOnly && rainfall > 0) ? 0 : Evap.rate
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        bool is_dry_only = ctx.options.evap_dry_only;
        double rain = precip_[ui] * ucf::UCF(ucf::RAINFALL, ctx.options);
        evap_rate_[ui] = (is_dry_only && rain > 0.0) ? 0.0 : evap_rate_in;
    }

    // ----- Step 3: Per-subcatchment subarea processing -----
    // Matches legacy subcatch_getRunoff() → getSubareaRunoff() chain exactly.
    // Infiltration and evaporation are computed INSIDE the per-subarea loop
    // to replicate the legacy's loss-limiting and inflow-subtraction order.
    for (int i = 0; i < n; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double fi = soa_.imperv_pct[ui];
        double fp = 1.0 - fi;
        double f0 = fi * soa_.imperv0_pct[ui];
        double f1 = fi * (1.0 - soa_.imperv0_pct[ui]);
        double total_area = soa_.area[ui];  // ft²

        double precip  = precip_[ui];     // ft/sec (rainfall + snowmelt)
        double evapRate = evap_rate_[ui]; // ft/sec (global evap rate)

        double alpha_i = soa_.alpha_imperv[ui];
        double alpha_p = soa_.alpha_perv[ui];

        // Mass balance accumulators (matching legacy Vevap, Vinfil, Voutflow)
        double Vevap    = 0.0;  // Total evaporation volume (ft³)
        double Vinfil   = 0.0;  // Total infiltration volume (ft³)
        double Voutflow = 0.0;  // Total runoff volume (ft³)

        // Helper: process one subarea following legacy getSubareaRunoff() exactly.
        // Args: depth, alpha, dStore, subareaFrac, isPervious
        auto processSubarea = [&](double& depth, double alpha, double dStore,
                                  double frac, bool isPervious) -> double {
            if (frac <= 0.0) return 0.0;
            double subarea_area = total_area * frac;

            // Step 3.1: Available surface moisture (legacy line 923)
            double surfMoisture = depth / dt;

            // Step 3.2: Limit evaporation to available moisture (legacy line 924)
            double surfEvap = std::min(surfMoisture, evapRate);

            // Step 3.3: Infiltration — pervious only (legacy line 927)
            // Called INSIDE the per-subarea loop with subarea inflow as runon.
            // For RouteTo=OUTLET models, subarea->inflow = 0 (no inter-subarea runon).
            double infil = 0.0;
            if (isPervious) {
                // Legacy: infil_getInfil(j, tStep, precip, subarea->inflow, depth)
                //   → horton_getInfil(state, tStep, precip + runon, depth)
                // subarea->inflow here is the runon from other subareas (0 for OUTLET routing).
                double runon = 0.0;  // runon from inter-subarea routing
                // Inter-subarea routing: if impervious routes to pervious,
                // the pervious subarea receives runon from impervious subareas.
                // This uses the PREVIOUS internal runoff values (set by getRunon)
                // which are stored in subarea inflow accumulators.
                // runon is added to inflow for infiltration calculation.
                // (runon accumulated below after all subareas are processed)
                switch (infil_model_) {
                    case InfilModel::HORTON:
                        infil = infil::horton_getInfil(horton_states_[ui],
                                    precip + runon, depth, dt);
                        break;
                    case InfilModel::GREEN_AMPT:
                        infil = infil::grnampt_getInfil(grnampt_states_[ui],
                                    precip + runon, depth, dt);
                        break;
                    case InfilModel::CURVE_NUM:
                        infil = infil::curvenum_getInfil(curvenum_states_[ui],
                                    precip + runon, depth, dt);
                        break;
                }
            }

            // Step 3.4: Accumulate inflow and moisture (legacy lines 930-931)
            double inflow = precip;  // + runon (when inter-subarea routing wired)
            surfMoisture += inflow;

            // Step 3.5: Update mass balance volumes (legacy lines 934-937)
            Vevap  += surfEvap * subarea_area * dt;
            Vinfil += infil * subarea_area * dt;

            // Step 3.6: Loss check shortcut (legacy lines 945-948)
            // If evaporation + infiltration >= total available moisture,
            // all water is consumed — no runoff, depth goes to zero.
            double runoff_rate = 0.0;
            if (surfEvap + infil >= surfMoisture) {
                depth = 0.0;
            } else {
                // Step 3.7: Subtract losses from inflow before ODE (legacy line 954)
                double net_inflow = inflow - surfEvap - infil;

                // Step 3.8: Integrate ponded depth via ODE (legacy line 955)
                updatePondedDepth(depth, net_inflow, alpha, dStore, dt);

                // Step 3.9: Compute runoff from final depth (legacy line 959)
                runoff_rate = getRunoffRate(depth, dStore, alpha);
            }

            // Step 3.10: Accumulate outlet volume (legacy line 964)
            // fOutlet is the fraction of runoff that goes directly to outlet.
            // For TO_OUTLET routing (default), fOutlet = 1.0.
            // For inter-subarea routing, fOutlet = 1 - pct_routed for the
            // routed subarea, and 1.0 for the receiving subarea.
            double fOutlet = 1.0;
            int route_mode = ctx.subcatches.subarea_routing[ui];
            double pct = ctx.subcatches.pct_routed[ui];
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

        // Process all 3 subareas (matching legacy loop: IMPERV0, IMPERV1, PERV)
        double runoff0  = processSubarea(soa_.depth_imperv0[ui], alpha_i, 0.0,
                                         f0, false);
        double runoff1  = processSubarea(soa_.depth_imperv1[ui], alpha_i,
                                         soa_.ds_imperv[ui], f1, false);
        double runoff_p = processSubarea(soa_.depth_perv[ui], alpha_p,
                                         soa_.ds_perv[ui], fp, true);


        // ----- Step 4: Compute loss rates and net runoff -----
        // Matches legacy lines 700-709.
        // evapLoss/infilLoss stored as area-averaged depth rates (ft/sec)
        // matching legacy: evapLoss = Vevap / tStep / area
        double evapLoss  = (total_area > 0.0) ? Vevap / dt / total_area : 0.0;
        double infilLoss = (total_area > 0.0) ? Vinfil / dt / total_area : 0.0;
        double newRunoff = Voutflow / dt;  // CFS

        soa_.runoff[ui] = newRunoff;

        // Write back to SimulationContext
        // Note: infil_loss stores area-averaged rate (over total area),
        // so the mass balance in SWMMEngine must multiply by full area
        // (not by f_perv again, since the averaging already accounts for it).
        ctx.subcatches.runoff[ui]     = newRunoff;
        ctx.subcatches.evap_loss[ui]  = evapLoss;
        ctx.subcatches.infil_loss[ui] = infilLoss;

        // Route runoff to outlet node
        int out_node = ctx.subcatches.outlet_node[ui];
        if (out_node >= 0 && out_node < ctx.n_nodes())
            ctx.nodes.lat_flow[static_cast<std::size_t>(out_node)] += newRunoff;
    }
}

} // namespace runoff
} // namespace openswmm
