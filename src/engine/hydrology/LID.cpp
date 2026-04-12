/**
 * @file LID.cpp
 * @brief LID control modules — batch-oriented, type-grouped.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 HydroCouple. All rights reserved.
 * @license  MIT License
 */

#include "LID.hpp"
#include "../core/SimulationContext.hpp"
#include <cmath>
#include <algorithm>
#include <array>

namespace openswmm {
namespace lid {

// static constexpr double MINFLOW = 2.3e-8;   // 0.001 in/hr in ft/sec

// ============================================================================
// Shared helpers for drain rate and storage exfiltration
// ============================================================================

/// Compute drain outflow rate with hysteresis (matching legacy getStorageDrainRate).
/// Updates drain_open state for next timestep.
static double getDrainRate(double head, double coeff, double expon,
                           double offset, double hOpen, double hClose,
                           int& drain_open_state) {
    double h = head - offset;
    if (h <= 0.0) { drain_open_state = 0; return 0.0; }

    // Hysteresis: drain opens when head >= hOpen, closes when head < hClose
    if (hOpen > 0.0 || hClose > 0.0) {
        if (drain_open_state) {
            if (h < hClose) { drain_open_state = 0; return 0.0; }
        } else {
            if (h < hOpen) return 0.0;
            drain_open_state = 1;
        }
    }

    if (coeff <= 0.0) return 0.0;
    return coeff * std::pow(h, expon);
}

/// Compute storage exfiltration rate with clogging reduction.
static double getStorageExfil(double kSat, double clogFactor,
                               double cumInflow) {
    if (kSat <= 0.0) return 0.0;
    double rate = kSat;
    if (clogFactor > 0.0 && cumInflow > 0.0) {
        double reduction = cumInflow / clogFactor;
        reduction = std::min(reduction, 1.0);
        rate *= (1.0 - reduction);
    }
    return rate;
}

void LIDGroupSoA::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);

    subcatch_idx.assign(un, -1);
    area.assign(un, 0.0);
    from_imperv.assign(un, 0.0);
    from_perv.assign(un, 0.0);
    to_perv.assign(un, 0);
    drain_node.assign(un, -1);
    drain_subcatch.assign(un, -1);
    inflow.assign(un, 0.0);

    surf_store.assign(un, 0.0);
    surf_rough.assign(un, 0.01);
    surf_slope.assign(un, 0.01);

    soil_thick.assign(un, 0.0);
    soil_poros.assign(un, 0.4);
    soil_fc.assign(un, 0.2);
    soil_wp.assign(un, 0.1);
    soil_ksat.assign(un, 0.0);
    soil_kslope.assign(un, 0.0);
    soil_suction.assign(un, 0.0);

    stor_thick.assign(un, 0.0);
    stor_void.assign(un, 0.5);
    stor_ksat.assign(un, 0.0);
    stor_clog.assign(un, 0.0);
    stor_covered.assign(un, 0);

    drain_coeff.assign(un, 0.0);
    drain_expon.assign(un, 0.5);
    drain_offset.assign(un, 0.0);
    drain_delay.assign(un, 0.0);
    drain_hopen.assign(un, 0.0);
    drain_hclose.assign(un, 0.0);
    drain_open.assign(un, 1);

    pave_thick.assign(un, 0.0);
    pave_void.assign(un, 0.15);
    pave_imperv_frac.assign(un, 0.0);
    pave_ksat.assign(un, 0.0);
    pave_clog_factor.assign(un, 0.0);
    pave_regen_days.assign(un, 0.0);
    pave_regen_deg.assign(un, 0.0);
    next_regen_day.assign(un, 0.0);

    drainmat_thick.assign(un, 0.0);
    drainmat_void.assign(un, 0.5);
    drainmat_rough.assign(un, 0.1);

    surf_void_frac.assign(un, 1.0);
    surf_alpha.assign(un, 0.0);
    surf_side_slope.assign(un, 0.0);
    full_width.assign(un, 0.0);
    dry_time.assign(un, 0.0);

    surf_depth.assign(un, 0.0);
    soil_moist.assign(un, 0.2);
    stor_depth.assign(un, 0.0);
    pave_depth.assign(un, 0.0);

    // drain_rmvl sized separately in model builder (needs n_pollutants)
    drain_rmvl.clear();

    f_old_surf.assign(un, 0.0);
    f_old_soil.assign(un, 0.0);
    f_old_stor.assign(un, 0.0);
    f_old_pave.assign(un, 0.0);

    surface_runoff.assign(un, 0.0);
    drain_flow.assign(un, 0.0);
    evap_loss.assign(un, 0.0);
    infil_loss.assign(un, 0.0);

    wb_inflow.assign(un, 0.0);
    wb_evap.assign(un, 0.0);
    wb_infil.assign(un, 0.0);
    wb_surf_flow.assign(un, 0.0);
    wb_drain_flow.assign(un, 0.0);
    wb_init_vol.assign(un, 0.0);
    wb_final_vol.assign(un, 0.0);
    vol_treated.assign(un, 0.0);
}

/// Map type code string to LIDType enum index (0-7).
static int typeCodeToIndex(const std::string& code) {
    if (code == "BC") return 0;
    if (code == "RG") return 1;
    if (code == "GR") return 2;
    if (code == "IT") return 3;
    if (code == "PP") return 4;
    if (code == "RB") return 5;
    if (code == "VS") return 6;
    if (code == "RD") return 7;
    return -1;
}

void LIDSolver::init(SimulationContext& ctx) {
    static constexpr int N_LID_TYPES = 8;
    groups_.resize(N_LID_TYPES);

    // Assign type codes
    groups_[0].type = LIDType::BIO_CELL;
    groups_[1].type = LIDType::RAIN_GARDEN;
    groups_[2].type = LIDType::GREEN_ROOF;
    groups_[3].type = LIDType::INFIL_TRENCH;
    groups_[4].type = LIDType::PERM_PAVEMENT;
    groups_[5].type = LIDType::RAIN_BARREL;
    groups_[6].type = LIDType::VEG_SWALE;
    groups_[7].type = LIDType::ROOF_DISCON;

    // If no LID usage data, leave all groups empty
    int n_usage = ctx.lid_usage.count();
    if (n_usage == 0) {
        for (auto& g : groups_) g.resize(0);
        return;
    }

    // 1. Count units per type from usage entries
    std::array<int, 8> type_counts = {};
    for (int j = 0; j < n_usage; ++j) {
        auto uj = static_cast<std::size_t>(j);
        int li = ctx.lid_usage.lid_index[uj];
        if (li < 0 || li >= ctx.lid_controls.count()) continue;
        int ti = typeCodeToIndex(ctx.lid_controls.lid_type[static_cast<std::size_t>(li)]);
        if (ti < 0 || ti >= N_LID_TYPES) continue;
        type_counts[static_cast<size_t>(ti)]++;
    }

    // 2. Resize groups
    for (int t = 0; t < N_LID_TYPES; ++t)
        groups_[static_cast<size_t>(t)].resize(type_counts[static_cast<size_t>(t)]);

    // 3. Populate per-unit parameters from LidControlStore + LidUsageStore
    std::array<int, 8> type_cursor = {};  // next free slot in each group
    for (int j = 0; j < n_usage; ++j) {
        auto uj = static_cast<std::size_t>(j);
        int li = ctx.lid_usage.lid_index[uj];
        if (li < 0 || li >= ctx.lid_controls.count()) continue;
        auto uli = static_cast<std::size_t>(li);
        int ti = typeCodeToIndex(ctx.lid_controls.lid_type[uli]);
        if (ti < 0 || ti >= N_LID_TYPES) continue;

        auto& g = groups_[static_cast<size_t>(ti)];
        int slot = type_cursor[static_cast<size_t>(ti)]++;
        auto us = static_cast<std::size_t>(slot);

        // Usage-level fields
        g.subcatch_idx[us] = ctx.lid_usage.subcatch_index[uj];
        g.area[us]         = ctx.lid_usage.area[uj];
        g.full_width[us]   = ctx.lid_usage.width[uj];
        g.from_imperv[us]  = ctx.lid_usage.from_imperv[uj] / 100.0;  // % → fraction
        g.from_perv[us]    = (uj < ctx.lid_usage.from_perv.size())
                             ? ctx.lid_usage.from_perv[uj] / 100.0 : 0.0;
        g.to_perv[us]      = ctx.lid_usage.to_perv[uj];

        // Resolve drain-to target
        if (uj < ctx.lid_usage.drain_to.size() && !ctx.lid_usage.drain_to[uj].empty()) {
            const auto& dt_name = ctx.lid_usage.drain_to[uj];
            int ni = ctx.node_names.find(dt_name);
            if (ni >= 0) {
                g.drain_node[us] = ni;
            } else {
                int si = ctx.subcatch_names.find(dt_name);
                if (si >= 0) g.drain_subcatch[us] = si;
            }
        }

        // SURFACE layer: [0]=StorHt, [1]=VegVolFrac, [2]=Roughness, [3]=SurfSlope, [4]=SideSlope
        if (uli < ctx.lid_controls.surface.size()) {
            const auto& p = ctx.lid_controls.surface[uli];
            g.surf_store[us]      = p[0];
            g.surf_void_frac[us]  = (p[1] > 0.0) ? (1.0 - p[1]) : 1.0;
            g.surf_rough[us]      = (p[2] > 0.0) ? p[2] : 0.01;
            g.surf_slope[us]      = (p[3] > 0.0) ? p[3] : 0.01;
            g.surf_side_slope[us] = p[4];  // swale side slope (run/rise)
            g.surf_alpha[us] = 1.49 * std::sqrt(g.surf_slope[us]) / g.surf_rough[us];
        }

        // SOIL layer: [0]=Thick, [1]=Poros, [2]=FC, [3]=WP, [4]=Ksat, [5]=Kslope, [6]=Suction
        if (uli < ctx.lid_controls.soil.size()) {
            const auto& p = ctx.lid_controls.soil[uli];
            g.soil_thick[us]    = p[0];
            g.soil_poros[us]    = p[1];
            g.soil_fc[us]       = p[2];
            g.soil_wp[us]       = p[3];
            g.soil_ksat[us]     = p[4];
            g.soil_kslope[us]   = p[5];
            g.soil_suction[us]  = p[6];
        }

        // STORAGE layer: [0]=Thick, [1]=VoidRatio, [2]=Ksat, [3]=ClogFactor
        if (uli < ctx.lid_controls.storage.size()) {
            const auto& p = ctx.lid_controls.storage[uli];
            g.stor_thick[us] = p[0];
            g.stor_void[us]  = p[1];
            g.stor_ksat[us]  = p[2];
            g.stor_clog[us]  = p[3];
        }

        // DRAIN layer: [0]=Coeff, [1]=Expon, [2]=Offset, [3]=Delay, [4]=hOpen, [5]=hClose
        if (uli < ctx.lid_controls.drain.size()) {
            const auto& p = ctx.lid_controls.drain[uli];
            g.drain_coeff[us]  = p[0];
            g.drain_expon[us]  = p[1];
            g.drain_offset[us] = p[2];
            g.drain_delay[us]  = p[3];
            g.drain_hopen[us]  = p[4];
            g.drain_hclose[us] = p[5];
        }

        // PAVEMENT layer: [0]=Thick, [1]=VoidRatio, [2]=FracImperv, [3]=Ksat, [4]=ClogFactor, [5]=RegenDays
        if (uli < ctx.lid_controls.pavement.size()) {
            const auto& p = ctx.lid_controls.pavement[uli];
            g.pave_thick[us]       = p[0];
            g.pave_void[us]        = p[1];
            g.pave_imperv_frac[us] = p[2];
            g.pave_ksat[us]        = p[3];
            g.pave_clog_factor[us] = p[4];
            if (p[5] > 0.0) {
                g.pave_regen_days[us] = p[5];
                g.pave_regen_deg[us]  = (uli < ctx.lid_controls.pavement.size() && p[5] > 0.0)
                                        ? 0.0 : 0.0;  // degree parsed separately if available
                g.next_regen_day[us]  = p[5];  // first regen after p[5] days
            }
        }

        // DRAINMAT layer: [0]=Thick, [1]=VoidRatio, [2]=Roughness
        if (uli < ctx.lid_controls.drainmat.size()) {
            const auto& p = ctx.lid_controls.drainmat[uli];
            g.drainmat_thick[us] = p[0];
            g.drainmat_void[us]  = p[1];
            g.drainmat_rough[us] = p[2];
        }

        // Initial saturation
        double initSat = ctx.lid_usage.init_sat[uj];
        if (g.soil_thick[us] > 0.0) {
            g.soil_moist[us] = g.soil_wp[us]
                             + initSat * (g.soil_poros[us] - g.soil_wp[us]);
        }
        if (g.stor_thick[us] > 0.0) {
            g.stor_depth[us] = initSat * g.stor_thick[us];
        }
    }

    // 4. Initialize water balance initial volumes
    for (auto& g : groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            double initVol = g.surf_depth[ui] * g.surf_void_frac[ui]
                           + g.soil_moist[ui] * g.soil_thick[ui]
                           + g.stor_depth[ui] * g.stor_void[ui]
                           + g.pave_depth[ui] * g.pave_void[ui]
                               * (1.0 - g.pave_imperv_frac[ui]);
            g.wb_init_vol[ui] = initVol;
            g.wb_final_vol[ui] = initVol;
        }
    }

    // 5. Populate drain pollutant removal fractions
    int np = ctx.n_pollutants();
    if (np > 0) {
        // Second pass: map units to their LID control index for removals
        std::array<int, 8> cursor2 = {};
        for (int j = 0; j < n_usage; ++j) {
            auto uj = static_cast<std::size_t>(j);
            int li = ctx.lid_usage.lid_index[uj];
            if (li < 0 || li >= ctx.lid_controls.count()) continue;
            auto uli = static_cast<std::size_t>(li);
            int ti = typeCodeToIndex(ctx.lid_controls.lid_type[uli]);
            if (ti < 0 || ti >= N_LID_TYPES) continue;

            auto& g = groups_[static_cast<size_t>(ti)];
            int slot = cursor2[static_cast<size_t>(ti)]++;

            // Ensure drain_rmvl is sized
            if (g.n_pollutants != np) {
                g.n_pollutants = np;
                g.drain_rmvl.assign(
                    static_cast<size_t>(g.count * np), 0.0);
            }

            // Copy removal fractions from LidControlStore
            if (uli < ctx.lid_controls.removals.size()) {
                for (const auto& pr : ctx.lid_controls.removals[uli]) {
                    int pi = pr.first;
                    double frac = pr.second;
                    if (pi >= 0 && pi < np) {
                        g.drain_rmvl[static_cast<size_t>(slot * np + pi)] = frac;
                    }
                }
            }
        }
    }
}

// ============================================================================
// Batch bio-cell flux rates — VECTORISABLE
// ============================================================================

void LIDSolver::batchBioCellFlux(LIDGroupSoA& g, double rainfall,
                                  double evap_rate, double dt) {
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Surface inflow: per-unit inflow if set, otherwise rainfall
        double inflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;

        // Surface evaporation (limited by ponded depth)
        double surf_evap = std::min(evap_rate, g.surf_depth[ui] / dt);

        // Surface → soil infiltration (Green-Ampt style)
        double theta = g.soil_moist[ui];
        double delta = theta - g.soil_fc[ui];
        double soil_infil = (delta > 0.0)
            ? g.soil_ksat[ui] * std::exp(g.soil_kslope[ui] * delta)
            : g.soil_ksat[ui];
        soil_infil = std::min(soil_infil, g.surf_depth[ui] / dt + inflow - surf_evap);
        soil_infil = std::max(soil_infil, 0.0);

        // Soil → storage percolation
        double soil_perc = (theta > g.soil_fc[ui])
            ? g.soil_ksat[ui] * std::exp(g.soil_kslope[ui] * (theta - g.soil_fc[ui]))
            : 0.0;

        // Storage → native exfiltration (with clogging reduction)
        double exfil = getStorageExfil(g.stor_ksat[ui], g.stor_clog[ui],
                                        g.wb_inflow[ui]);

        // Storage → drain (with hysteresis)
        double drain = getDrainRate(g.stor_depth[ui], g.drain_coeff[ui],
                                     g.drain_expon[ui], g.drain_offset[ui],
                                     g.drain_hopen[ui], g.drain_hclose[ui],
                                     g.drain_open[ui]);

        // Surface overflow
        double overflow = 0.0;
        double new_surf = g.surf_depth[ui] + (inflow - surf_evap - soil_infil) * dt;
        if (new_surf > g.surf_store[ui]) {
            overflow = (new_surf - g.surf_store[ui]) / dt;
            new_surf = g.surf_store[ui];
        }
        new_surf = std::max(new_surf, 0.0);

        // Update states
        g.surf_depth[ui] = new_surf;

        // Soil moisture update
        double soil_thick = g.soil_thick[ui];
        if (soil_thick > 0.0) {
            g.soil_moist[ui] += (soil_infil - soil_perc) * dt / soil_thick;
            g.soil_moist[ui] = std::max(g.soil_moist[ui], g.soil_wp[ui]);
            g.soil_moist[ui] = std::min(g.soil_moist[ui], g.soil_poros[ui]);
        }

        // Storage depth update
        double stor_void = g.stor_void[ui];
        if (stor_void > 0.0) {
            g.stor_depth[ui] += (soil_perc - exfil - drain) * dt / stor_void;
            g.stor_depth[ui] = std::max(g.stor_depth[ui], 0.0);
            g.stor_depth[ui] = std::min(g.stor_depth[ui], g.stor_thick[ui]);
        }

        // Outputs
        g.surface_runoff[ui] = overflow;
        g.drain_flow[ui] = drain;
        g.evap_loss[ui] = surf_evap * dt;
        g.infil_loss[ui] = exfil * dt;

        // Water balance tracking
        double totalVolume = g.surf_depth[ui] * g.surf_void_frac[ui]
                           + g.soil_moist[ui] * g.soil_thick[ui]
                           + g.stor_depth[ui] * g.stor_void[ui];
        g.wb_inflow[ui]     += inflow * dt;
        g.wb_evap[ui]       += surf_evap * dt;
        g.wb_infil[ui]      += exfil * dt;
        g.wb_surf_flow[ui]  += overflow * dt;
        g.wb_drain_flow[ui] += drain * dt;
        g.wb_final_vol[ui]   = totalVolume;
        g.vol_treated[ui]   += inflow * dt;
    }
}

// ============================================================================
// Batch rain barrel — VECTORISABLE (simplest LID)
// ============================================================================

void LIDSolver::batchBarrelFlux(LIDGroupSoA& g, double rainfall, double dt) {
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        // Covered barrel blocks direct rainfall (legacy: storage.covered)
        double unit_inflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;
        if (g.stor_covered[ui]) unit_inflow = 0.0;

        // Track dry time for drain delay
        if (unit_inflow > 0.0)
            g.dry_time[ui] = 0.0;
        else
            g.dry_time[ui] += dt;

        // Inflow fills storage
        double new_depth = g.stor_depth[ui] + unit_inflow * dt;

        // Overflow
        double overflow = 0.0;
        if (new_depth > g.stor_thick[ui]) {
            overflow = (new_depth - g.stor_thick[ui]) / dt;
            new_depth = g.stor_thick[ui];
        }

        // Drain — with delay and hysteresis
        double drain = 0.0;
        bool delay_ok = (g.drain_delay[ui] <= 0.0 || g.dry_time[ui] >= g.drain_delay[ui]);
        if (delay_ok) {
            drain = getDrainRate(new_depth, g.drain_coeff[ui], g.drain_expon[ui],
                                 g.drain_offset[ui], g.drain_hopen[ui],
                                 g.drain_hclose[ui], g.drain_open[ui]);
            drain = std::min(drain, new_depth / dt);
            new_depth -= drain * dt;
        }

        g.stor_depth[ui] = std::max(new_depth, 0.0);
        g.surface_runoff[ui] = overflow;
        g.drain_flow[ui] = drain;
        g.evap_loss[ui] = 0.0;
        g.infil_loss[ui] = 0.0;

        // Water balance tracking
        g.wb_inflow[ui]     += unit_inflow * dt;
        g.wb_evap[ui]       += 0.0;
        g.wb_infil[ui]      += 0.0;
        g.wb_surf_flow[ui]  += overflow * dt;
        g.wb_drain_flow[ui] += drain * dt;
        g.wb_final_vol[ui]   = g.stor_depth[ui];
        g.vol_treated[ui]   += unit_inflow * dt;
    }
}

// ============================================================================
// Batch vegetative swale — VECTORISABLE
// ============================================================================

void LIDSolver::batchSwaleFlux(LIDGroupSoA& g, double rainfall,
                                double evap_rate, double dt) {
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        double inflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;
        double surf_evap = std::min(evap_rate, g.surf_depth[ui] / dt);

        // Soil infiltration
        double infil = g.soil_ksat[ui];
        infil = std::min(infil, g.surf_depth[ui] / dt + inflow - surf_evap);
        infil = std::max(infil, 0.0);

        // Manning's surface outflow: Q = (1/n) * depth^(5/3) * sqrt(slope)
        double excess = g.surf_depth[ui] - g.surf_store[ui];
        double runoff = 0.0;
        if (excess > 0.0 && g.surf_rough[ui] > 0.0) {
            runoff = std::pow(excess, 5.0 / 3.0) * std::sqrt(g.surf_slope[ui])
                     / g.surf_rough[ui];
        }

        // Update surface depth
        double new_surf = g.surf_depth[ui] + (inflow - surf_evap - infil - runoff) * dt;
        g.surf_depth[ui] = std::max(new_surf, 0.0);

        g.surface_runoff[ui] = runoff;
        g.drain_flow[ui] = 0.0;
        g.evap_loss[ui] = surf_evap * dt;
        g.infil_loss[ui] = infil * dt;

        // Water balance tracking
        g.wb_inflow[ui]     += inflow * dt;
        g.wb_evap[ui]       += surf_evap * dt;
        g.wb_infil[ui]      += infil * dt;
        g.wb_surf_flow[ui]  += runoff * dt;
        g.wb_drain_flow[ui] += 0.0;
        g.wb_final_vol[ui]   = g.surf_depth[ui] * g.surf_void_frac[ui];
        g.vol_treated[ui]   += inflow * dt;
    }
}

// ============================================================================
// Batch swale with Modified Puls (omega=0.5, iterative)
// ============================================================================
// Legacy reference: modpuls_solve() in lidproc.c with VEG_SWALE omega=0.5.
// The swale Manning's equation is nonlinear — iteration is needed for
// the trapezoid-rule time weighting to converge.

static constexpr double STOPTOL = 0.00328;  // 1 mm in ft
static constexpr int    MAX_ITERATIONS = 20;

void LIDSolver::batchSwaleModPuls(LIDGroupSoA& g, double rainfall,
                                   double evap_rate, double dt) {
    constexpr double omega = 0.5;

    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        double inflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;

        // Save initial state
        double x_old = g.surf_depth[ui];
        double x_prev = x_old;
        double f_old = g.f_old_surf[ui];  // flux rate from previous timestep

        // Iterate Modified Puls
        double x = x_old;
        double f_new = 0.0;
        for (int iter = 0; iter < MAX_ITERATIONS; ++iter) {
            // Compute flux rates at current state
            double surf_evap = std::min(evap_rate, std::max(x, 0.0) / dt);
            double soil_infil = g.soil_ksat[ui];
            soil_infil = std::min(soil_infil, std::max(x, 0.0) / dt + inflow - surf_evap);
            soil_infil = std::max(soil_infil, 0.0);

            double excess = x - g.surf_store[ui];
            double runoff = 0.0;
            if (excess > 0.0 && g.surf_rough[ui] > 0.0) {
                runoff = std::pow(excess, 5.0 / 3.0) * std::sqrt(g.surf_slope[ui])
                         / g.surf_rough[ui];
            }

            // Net flux = dx/dt
            f_new = inflow - surf_evap - soil_infil - runoff;

            // Modified Puls update: x = x_old + (omega*f_old + (1-omega)*f_new) * dt
            x = x_old + (omega * f_old + (1.0 - omega) * f_new) * dt;
            x = std::max(x, 0.0);

            // Check convergence
            if (std::abs(x - x_prev) <= STOPTOL) break;
            x_prev = x;
        }

        // Save new flux rate for next timestep
        g.f_old_surf[ui] = f_new;
        g.surf_depth[ui] = x;

        // Compute final outputs at converged state for reporting
        double surf_evap = std::min(evap_rate, std::max(x, 0.0) / dt);
        double soil_infil = g.soil_ksat[ui];
        soil_infil = std::min(soil_infil, std::max(x, 0.0) / dt + inflow - surf_evap);
        soil_infil = std::max(soil_infil, 0.0);
        double excess = x - g.surf_store[ui];
        double runoff = 0.0;
        if (excess > 0.0 && g.surf_rough[ui] > 0.0) {
            runoff = std::pow(excess, 5.0 / 3.0) * std::sqrt(g.surf_slope[ui])
                     / g.surf_rough[ui];
        }

        g.surface_runoff[ui] = runoff;
        g.drain_flow[ui] = 0.0;
        g.evap_loss[ui] = surf_evap * dt;
        g.infil_loss[ui] = soil_infil * dt;

        // Water balance: use actual state change to back-compute fluxes
        // delta_vol = (x - x_old) * void_frac
        // inflow*dt = evap*dt + infil*dt + runoff*dt + delta_vol
        // => runoff*dt = inflow*dt - evap*dt - infil*dt - delta_vol
        double delta_vol = (x - x_old) * g.surf_void_frac[ui];
        double wb_runoff = inflow * dt - surf_evap * dt - soil_infil * dt - delta_vol;
        wb_runoff = std::max(wb_runoff, 0.0);

        g.wb_inflow[ui]     += inflow * dt;
        g.wb_evap[ui]       += surf_evap * dt;
        g.wb_infil[ui]      += soil_infil * dt;
        g.wb_surf_flow[ui]  += wb_runoff;
        g.wb_drain_flow[ui] += 0.0;
        g.wb_final_vol[ui]   = x * g.surf_void_frac[ui];
        g.vol_treated[ui]   += inflow * dt;
    }
}

// ============================================================================
// Batch green roof flux rates — VECTORISABLE
// ============================================================================
// Legacy reference: greenRoofFluxRates() in lidproc.c
// Layers: surface → soil → drainage mat (storage layer used as drain mat)
// Key difference from biocell: no exfiltration, drainage mat outflow via
// Manning equation through the mat, storage thickness = drainmat thickness.

void LIDSolver::batchGreenRoofFlux(LIDGroupSoA& g, double rainfall,
                                    double evap_rate, double dt) {
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        double surfaceDepth  = g.surf_depth[ui];
        double soilTheta     = g.soil_moist[ui];
        double storageDepth  = g.stor_depth[ui];

        double soilThickness    = g.soil_thick[ui];
        double soilPorosity     = g.soil_poros[ui];
        double soilFieldCap     = g.soil_fc[ui];
        double soilWiltPoint    = g.soil_wp[ui];
        double surfVoidFrac     = g.surf_void_frac[ui];

        // For green roof, storage layer represents the drainage mat
        double storageThickness = g.drainmat_thick[ui];
        double storageVoidFrac  = g.drainmat_void[ui];

        // --- convert moisture levels to volumes ---
        double surfaceVolume = surfaceDepth * surfVoidFrac;
        double soilVolume    = soilTheta * soilThickness;
        double storageVolume = storageDepth * storageVoidFrac;

        // --- inflow ---
        double surfaceInflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;

        // --- surface infiltration (Green-Ampt style) ---
        double surfaceInfil;
        if (soilThickness > 0.0 && g.soil_ksat[ui] > 0.0) {
            double delta = soilPorosity - soilTheta;
            surfaceInfil = g.soil_ksat[ui] * std::exp(-delta * g.soil_kslope[ui]);
        } else {
            surfaceInfil = 0.0;
        }

        // --- evaporation cascade (legacy getEvapRates with pervFrac=1.0) ---
        double availEvap = evap_rate;
        double surfaceEvap = std::min(availEvap, surfaceVolume / dt);
        surfaceEvap = std::max(0.0, surfaceEvap);
        availEvap = std::max(0.0, availEvap - surfaceEvap);

        double soilEvap = 0.0;
        double storageEvap = 0.0;
        if (surfaceInfil > 0.0) {
            // no subsurface evap if water is infiltrating
            soilEvap = 0.0;
            storageEvap = 0.0;
        } else {
            double availSoilVol = soilVolume - soilWiltPoint * soilThickness;
            soilEvap = std::min(availEvap, std::max(0.0, availSoilVol) / dt);
            availEvap = std::max(0.0, availEvap - soilEvap);
            storageEvap = std::min(availEvap, storageVolume / dt);
        }
        // no storage evap if soil saturated
        if (soilTheta >= soilPorosity) storageEvap = 0.0;

        // --- soil percolation rate ---
        double soilPerc = 0.0;
        if (soilTheta > soilFieldCap) {
            double delta = soilPorosity - soilTheta;
            soilPerc = g.soil_ksat[ui] * std::exp(-delta * g.soil_kslope[ui]);
        }
        // limit by available water above field capacity
        double availVolume = (soilTheta - soilFieldCap) * soilThickness;
        double maxRate = std::max(availVolume, 0.0) / dt - soilEvap;
        soilPerc = std::min(soilPerc, maxRate);
        soilPerc = std::max(soilPerc, 0.0);

        // --- drainage mat outflow (legacy getDrainMatOutflow) ---
        double storageExfil = 0.0; // green roof has no exfiltration
        double storageDrain = soilPerc; // default: pass all inflow
        // Manning equation through drainage mat if alpha > 0
        double drainmatAlpha = 0.0;
        if (g.drainmat_rough[ui] > 0.0 && g.surf_slope[ui] > 0.0) {
            drainmatAlpha = std::sqrt(g.surf_slope[ui]) / g.drainmat_rough[ui];
        }
        if (drainmatAlpha > 0.0 && g.full_width[ui] > 0.0 && g.area[ui] > 0.0) {
            storageDrain = drainmatAlpha * std::pow(storageDepth, 5.0 / 3.0)
                         * g.full_width[ui] / g.area[ui]
                         * storageVoidFrac;
        }

        // --- limit fluxes for full/not-full conditions ---
        if (soilTheta >= soilPorosity && storageDepth >= storageThickness) {
            // Unit is full: outflow from both layers equals limiting rate
            maxRate = std::min(soilPerc, storageDrain);
            soilPerc = maxRate;
            storageDrain = maxRate;
            surfaceInfil = std::min(surfaceInfil, maxRate);
        } else {
            // Unit not full
            // limit drainmat outflow by available storage volume
            maxRate = storageDepth * storageVoidFrac / dt - storageEvap;
            if (storageDepth >= storageThickness) maxRate += soilPerc;
            maxRate = std::max(maxRate, 0.0);
            storageDrain = std::min(storageDrain, maxRate);

            // limit soil perc by unused storage volume
            maxRate = (storageThickness - storageDepth) * storageVoidFrac / dt
                    + storageDrain + storageEvap;
            soilPerc = std::min(soilPerc, maxRate);

            // limit surface infil so soil porosity not exceeded
            maxRate = (soilPorosity - soilTheta) * soilThickness / dt
                    + soilPerc + soilEvap;
            surfaceInfil = std::min(surfaceInfil, maxRate);
        }

        // --- surface outflow (Manning's over surface storage) ---
        double surfaceOutflow = 0.0;
        double delta_surf = surfaceDepth - g.surf_store[ui];
        if (delta_surf > 0.0 && g.surf_alpha[ui] > 0.0
            && g.full_width[ui] > 0.0 && g.area[ui] > 0.0) {
            surfaceOutflow = g.surf_alpha[ui] * std::pow(delta_surf, 5.0 / 3.0)
                           * g.full_width[ui] / g.area[ui];
            surfaceOutflow = std::min(surfaceOutflow, delta_surf / dt);
        }

        // --- Euler integration of layer depths ---
        // Surface
        double f_surf = (surfaceInflow - surfaceEvap - surfaceInfil - surfaceOutflow)
                       / surfVoidFrac;
        double newSurf = surfaceDepth + f_surf * dt;
        newSurf = std::max(newSurf, 0.0);

        // Soil moisture
        double f_soil = 0.0;
        if (soilThickness > 0.0) {
            f_soil = (surfaceInfil - soilEvap - soilPerc) / soilThickness;
        }
        double newTheta = soilTheta + f_soil * dt;
        newTheta = std::max(newTheta, soilWiltPoint);
        newTheta = std::min(newTheta, soilPorosity);

        // Storage (drainage mat) depth
        double f_stor = 0.0;
        if (storageVoidFrac > 0.0) {
            f_stor = (soilPerc - storageEvap - storageDrain) / storageVoidFrac;
        }
        double newStor = storageDepth + f_stor * dt;
        newStor = std::max(newStor, 0.0);
        newStor = std::min(newStor, storageThickness);

        // --- update state ---
        g.surf_depth[ui] = newSurf;
        g.soil_moist[ui] = newTheta;
        g.stor_depth[ui] = newStor;

        // --- outputs ---
        g.surface_runoff[ui] = surfaceOutflow;
        g.drain_flow[ui] = storageDrain;
        double totalEvap = surfaceEvap + soilEvap + storageEvap;
        g.evap_loss[ui] = totalEvap * dt;
        g.infil_loss[ui] = storageExfil * dt; // always 0 for green roof

        // --- water balance ---
        double totalVolume = newSurf * surfVoidFrac
                           + newTheta * soilThickness
                           + newStor * storageVoidFrac;
        g.wb_inflow[ui]     += surfaceInflow * dt;
        g.wb_evap[ui]       += totalEvap * dt;
        g.wb_infil[ui]      += storageExfil * dt;
        g.wb_surf_flow[ui]  += surfaceOutflow * dt;
        g.wb_drain_flow[ui] += storageDrain * dt;
        g.wb_final_vol[ui]   = totalVolume;
        g.vol_treated[ui]   += surfaceInflow * dt;
    }
}

// ============================================================================
// Batch permeable pavement flux rates — VECTORISABLE
// ============================================================================
// Legacy reference: pavementFluxRates() in lidproc.c
// Layers: surface → pavement → (optional soil) → storage → drain/exfil

void LIDSolver::batchPavementFlux(LIDGroupSoA& g, double rainfall,
                                   double evap_rate, double dt) {
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        double surfaceDepth  = g.surf_depth[ui];
        double paveDepth     = g.pave_depth[ui];
        double soilTheta     = g.soil_moist[ui];
        double storageDepth  = g.stor_depth[ui];

        double pervFrac         = 1.0 - g.pave_imperv_frac[ui];
        double paveVoidFrac     = g.pave_void[ui] * pervFrac;
        double paveThickness    = g.pave_thick[ui];
        double soilThickness    = g.soil_thick[ui];
        double soilPorosity     = g.soil_poros[ui];
        double soilFieldCap     = g.soil_fc[ui];
        double soilWiltPoint    = g.soil_wp[ui];
        double storageThickness = g.stor_thick[ui];
        double storageVoidFrac  = g.stor_void[ui];
        double surfVoidFrac     = g.surf_void_frac[ui];

        // --- convert moisture levels to volumes ---
        double surfaceVolume = surfaceDepth * surfVoidFrac;
        double paveVolume    = paveDepth * paveVoidFrac;
        double soilVolume    = soilTheta * soilThickness;
        double storageVolume = storageDepth * storageVoidFrac;

        // --- inflow ---
        double surfaceInflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;

        // --- evaporation cascade (legacy getEvapRates with pervFrac) ---
        double availEvap = evap_rate;
        double surfaceEvap = std::min(availEvap, surfaceVolume / dt);
        surfaceEvap = std::max(0.0, surfaceEvap);
        availEvap = std::max(0.0, availEvap - surfaceEvap);
        availEvap *= pervFrac;

        double paveEvap = 0.0;
        double soilEvap = 0.0;
        double storageEvap = 0.0;

        // Surface infiltration into pavement (nominal)
        double surfaceInfil = surfaceInflow + surfaceVolume / dt;

        if (surfaceInfil > 0.0) {
            paveEvap = 0.0;
            soilEvap = 0.0;
            storageEvap = 0.0;
        } else {
            paveEvap = std::min(availEvap, paveVolume / dt);
            availEvap = std::max(0.0, availEvap - paveEvap);
            double availSoilVol = soilVolume - soilWiltPoint * soilThickness;
            soilEvap = std::min(availEvap, std::max(0.0, availSoilVol) / dt);
            availEvap = std::max(0.0, availEvap - soilEvap);
            storageEvap = std::min(availEvap, storageVolume / dt);
        }

        // no storage evap if soil or pavement layer saturated
        if (paveDepth >= paveThickness
            || (soilThickness > 0.0 && soilTheta >= soilPorosity)) {
            storageEvap = 0.0;
        }

        // --- pavement regeneration (reduce clogging at intervals) ---
        if (g.pave_regen_days[ui] > 0.0 && g.next_regen_day[ui] > 0.0) {
            // Decrement regen counter by dt (in days)
            g.next_regen_day[ui] -= dt / 86400.0;
            if (g.next_regen_day[ui] <= 0.0) {
                // Regenerate: reduce vol_treated by (1 - regen_degree)
                g.vol_treated[ui] *= (1.0 - g.pave_regen_deg[ui]);
                g.next_regen_day[ui] = g.pave_regen_days[ui];
            }
        }

        // --- pavement permeability (exponential clog model) ---
        double permReduction = 0.0;
        double clogFactor = g.pave_clog_factor[ui];
        if (clogFactor > 0.0) {
            permReduction = g.vol_treated[ui] / clogFactor;
            permReduction = std::min(permReduction, 1.0);
        }
        double pavePerc = g.pave_ksat[ui] * (1.0 - permReduction) * pervFrac;

        // surface infil can't exceed pavement permeability
        surfaceInfil = std::min(surfaceInfil, pavePerc);

        // limit pavement perc by available water
        double maxRate = paveVolume / dt + surfaceInfil - paveEvap;
        maxRate = std::max(maxRate, 0.0);
        pavePerc = std::min(pavePerc, maxRate);

        // --- soil percolation ---
        double soilPerc = 0.0;
        if (soilThickness > 0.0) {
            if (soilTheta > soilFieldCap) {
                double delta = soilPorosity - soilTheta;
                soilPerc = g.soil_ksat[ui] * std::exp(-delta * g.soil_kslope[ui]);
            }
            double availVolume = (soilTheta - soilFieldCap) * soilThickness;
            maxRate = std::max(availVolume, 0.0) / dt - soilEvap;
            soilPerc = std::min(soilPerc, maxRate);
            soilPerc = std::max(soilPerc, 0.0);
        } else {
            soilPerc = pavePerc;
        }

        // --- storage exfiltration (with clogging) ---
        double storageExfil = getStorageExfil(g.stor_ksat[ui], g.stor_clog[ui],
                                               g.wb_inflow[ui]);

        // --- underdrain flow (with hysteresis) ---
        double storageDrain = getDrainRate(storageDepth, g.drain_coeff[ui],
                                            g.drain_expon[ui], g.drain_offset[ui],
                                            g.drain_hopen[ui], g.drain_hclose[ui],
                                            g.drain_open[ui]);

        // --- adjacency saturation checks (from legacy pavementFluxRates) ---

        if (soilThickness == 0.0
            && storageDepth >= storageThickness
            && paveDepth >= paveThickness) {
            // No soil layer, pavement & storage full
            maxRate = storageEvap + storageDrain + storageExfil;
            if (pavePerc > maxRate) {
                pavePerc = maxRate;
            } else {
                storageExfil = std::min(storageExfil, pavePerc);
                storageDrain = pavePerc - storageExfil;
            }
            soilPerc = pavePerc;
            surfaceInfil = std::min(surfaceInfil, pavePerc);

        } else if (soilThickness > 0.0
                   && storageDepth >= storageThickness
                   && soilTheta >= soilPorosity
                   && paveDepth >= paveThickness) {
            // Pavement, soil & storage full
            maxRate = storageExfil + storageDrain;
            if (soilPerc < maxRate) maxRate = soilPerc;
            else maxRate = std::min(maxRate, pavePerc);
            if (maxRate > storageExfil) storageDrain = maxRate - storageExfil;
            else { storageExfil = maxRate; storageDrain = 0.0; }
            soilPerc = maxRate;
            pavePerc = maxRate;
            surfaceInfil = std::min(surfaceInfil, pavePerc);

        } else if (soilThickness > 0.0
                   && storageDepth >= storageThickness
                   && soilTheta >= soilPorosity) {
            // Storage & soil full
            maxRate = storageDrain + storageExfil;
            if (soilPerc > maxRate) soilPerc = maxRate;
            else {
                storageExfil = std::min(storageExfil, soilPerc);
                storageDrain = soilPerc - storageExfil;
            }
            pavePerc = std::min(pavePerc, soilPerc);
            double availVolume = (paveThickness - paveDepth) * paveVoidFrac;
            maxRate = availVolume / dt + pavePerc + paveEvap;
            surfaceInfil = std::min(surfaceInfil, maxRate);

        } else if (soilThickness > 0.0
                   && paveDepth >= paveThickness
                   && soilTheta >= soilPorosity) {
            // Soil and pavement full
            pavePerc = std::min(pavePerc, soilPerc);
            soilPerc = pavePerc;
            surfaceInfil = std::min(surfaceInfil, pavePerc);
            maxRate = std::max(storageVolume / dt + soilPerc - storageEvap, 0.0);
            storageExfil = std::min(storageExfil, maxRate);

        } else {
            // No adjoining layers full
            maxRate = soilPerc - storageEvap + storageVolume / dt;
            maxRate = std::max(0.0, maxRate);
            storageExfil = std::min(storageExfil, maxRate);

            if (storageDrain > 0.0) {
                maxRate = -storageExfil - storageEvap;
                if (storageDepth >= storageThickness) maxRate += soilPerc;
                if (g.drain_offset[ui] <= storageDepth) {
                    maxRate += (storageDepth - g.drain_offset[ui])
                             * storageVoidFrac / dt;
                }
                maxRate = std::max(maxRate, 0.0);
                storageDrain = std::min(storageDrain, maxRate);
            }

            // limit soil & pavement by unused storage volume
            double availVolume = (storageThickness - storageDepth) * storageVoidFrac;
            maxRate = availVolume / dt + storageEvap + storageDrain + storageExfil;
            maxRate = std::max(maxRate, 0.0);
            if (soilThickness > 0.0) {
                soilPerc = std::min(soilPerc, maxRate);
                maxRate = (soilPorosity - soilTheta) * soilThickness / dt + soilPerc;
            }
            pavePerc = std::min(pavePerc, maxRate);

            // limit surface infil by available pavement volume
            availVolume = (paveThickness - paveDepth) * paveVoidFrac;
            maxRate = availVolume / dt + pavePerc + paveEvap;
            surfaceInfil = std::min(surfaceInfil, maxRate);
        }

        // --- surface outflow ---
        double surfaceOutflow = 0.0;
        double delta_surf = surfaceDepth - g.surf_store[ui];
        if (delta_surf > 0.0 && g.surf_alpha[ui] > 0.0
            && g.full_width[ui] > 0.0 && g.area[ui] > 0.0) {
            surfaceOutflow = g.surf_alpha[ui] * std::pow(delta_surf, 5.0 / 3.0)
                           * g.full_width[ui] / g.area[ui];
            surfaceOutflow = std::min(surfaceOutflow, delta_surf / dt);
        }

        // --- Euler integration ---
        // Surface
        double f_surf = surfaceInflow - surfaceEvap - surfaceInfil - surfaceOutflow;
        double newSurf = surfaceDepth + f_surf * dt;
        newSurf = std::max(newSurf, 0.0);

        // Pavement
        double f_pave = 0.0;
        if (paveVoidFrac > 0.0) {
            f_pave = (surfaceInfil - paveEvap - pavePerc) / paveVoidFrac;
        }
        double newPave = paveDepth + f_pave * dt;
        newPave = std::max(newPave, 0.0);
        newPave = std::min(newPave, paveThickness);

        // Soil
        double f_soil = 0.0;
        double newTheta = soilTheta;
        double storageInflow_local = soilPerc;
        if (soilThickness > 0.0) {
            f_soil = (pavePerc - soilEvap - soilPerc) / soilThickness;
            newTheta = soilTheta + f_soil * dt;
            newTheta = std::max(newTheta, soilWiltPoint);
            newTheta = std::min(newTheta, soilPorosity);
        } else {
            storageInflow_local = pavePerc;
            soilPerc = 0.0;
        }

        // Storage
        double f_stor = 0.0;
        if (storageVoidFrac > 0.0) {
            f_stor = (storageInflow_local - storageEvap - storageExfil - storageDrain)
                   / storageVoidFrac;
        }
        double newStor = storageDepth + f_stor * dt;
        newStor = std::max(newStor, 0.0);
        newStor = std::min(newStor, storageThickness);

        // --- update state ---
        g.surf_depth[ui] = newSurf;
        g.pave_depth[ui] = newPave;
        g.soil_moist[ui] = newTheta;
        g.stor_depth[ui] = newStor;

        // --- outputs ---
        g.surface_runoff[ui] = surfaceOutflow;
        g.drain_flow[ui] = storageDrain;
        double totalEvap = surfaceEvap + paveEvap + soilEvap + storageEvap;
        g.evap_loss[ui] = totalEvap * dt;
        g.infil_loss[ui] = storageExfil * dt;

        // --- water balance ---
        double totalVolume = newSurf * surfVoidFrac
                           + newPave * paveVoidFrac
                           + newTheta * soilThickness
                           + newStor * storageVoidFrac;
        g.wb_inflow[ui]     += surfaceInflow * dt;
        g.wb_evap[ui]       += totalEvap * dt;
        g.wb_infil[ui]      += storageExfil * dt;
        g.wb_surf_flow[ui]  += surfaceOutflow * dt;
        g.wb_drain_flow[ui] += storageDrain * dt;
        g.wb_final_vol[ui]   = totalVolume;
        g.vol_treated[ui]   += surfaceInflow * dt;
    }
}

// ============================================================================
// Batch roof disconnection flux rates — VECTORISABLE
// ============================================================================
// Legacy reference: roofFluxRates() in lidproc.c
// Simplest LID: rainfall → surface ponding → surface outflow split into
// drain (downspout) and overflow.

void LIDSolver::batchRoofDisconFlux(LIDGroupSoA& g, double rainfall,
                                     double evap_rate, double dt) {
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);

        double surfaceDepth = g.surf_depth[ui];

        // --- inflow ---
        double surfaceInflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;

        // --- evaporation (surface only, pervFrac = 1.0) ---
        double surfaceEvap = std::min(evap_rate, surfaceDepth / dt);
        surfaceEvap = std::max(0.0, surfaceEvap);

        // --- surface outflow ---
        double surfaceOutflow = 0.0;
        if (g.surf_alpha[ui] > 0.0 && g.full_width[ui] > 0.0
            && g.area[ui] > 0.0) {
            // Manning's equation outflow from surface
            double delta_surf = surfaceDepth - g.surf_store[ui];
            if (delta_surf > 0.0) {
                surfaceOutflow = g.surf_alpha[ui]
                               * std::pow(delta_surf, 5.0 / 3.0)
                               * g.full_width[ui] / g.area[ui];
                surfaceOutflow = std::min(surfaceOutflow, delta_surf / dt);
            }
        } else {
            // No Manning parameters: overflow = anything above surface storage
            double delta_surf = surfaceDepth - g.surf_store[ui];
            if (delta_surf > 0.0) {
                surfaceOutflow = delta_surf * g.surf_void_frac[ui] / dt;
            }
        }

        // --- drain (downspout): fraction of surface outflow up to drain_coeff ---
        // Legacy: StorageDrain = MIN(drain.coeff/UCF(RAINFALL), SurfaceOutflow)
        // In internal units drain_coeff is already in ft/s
        double storageDrain = std::min(g.drain_coeff[ui], surfaceOutflow);
        surfaceOutflow -= storageDrain;

        // --- Euler integration of surface depth ---
        double f_surf = surfaceInflow - surfaceEvap - storageDrain - surfaceOutflow;
        double newSurf = surfaceDepth + f_surf * dt;
        newSurf = std::max(newSurf, 0.0);

        // --- update state ---
        g.surf_depth[ui] = newSurf;

        // --- outputs ---
        g.surface_runoff[ui] = surfaceOutflow;
        g.drain_flow[ui] = storageDrain;
        g.evap_loss[ui] = surfaceEvap * dt;
        g.infil_loss[ui] = 0.0;

        // --- water balance ---
        double totalVolume = newSurf * g.surf_void_frac[ui];
        g.wb_inflow[ui]     += surfaceInflow * dt;
        g.wb_evap[ui]       += surfaceEvap * dt;
        g.wb_infil[ui]      += 0.0;
        g.wb_surf_flow[ui]  += surfaceOutflow * dt;
        g.wb_drain_flow[ui] += storageDrain * dt;
        g.wb_final_vol[ui]   = totalVolume;
        g.vol_treated[ui]   += surfaceInflow * dt;
    }
}

// ============================================================================
// Execute — all LID types batch
// ============================================================================

void LIDSolver::execute(SimulationContext& /*ctx*/, double dt,
                        double rainfall, double evap_rate) {
    for (auto& g : groups_) {
        if (g.count == 0) continue;

        switch (g.type) {
            case LIDType::BIO_CELL:
            case LIDType::RAIN_GARDEN:
            case LIDType::INFIL_TRENCH:
                batchBioCellFlux(g, rainfall, evap_rate, dt);
                break;

            case LIDType::RAIN_BARREL:
                batchBarrelFlux(g, rainfall, dt);
                break;

            case LIDType::VEG_SWALE:
                batchSwaleModPuls(g, rainfall, evap_rate, dt);
                break;

            case LIDType::GREEN_ROOF:
                batchGreenRoofFlux(g, rainfall, evap_rate, dt);
                break;

            case LIDType::PERM_PAVEMENT:
                batchPavementFlux(g, rainfall, evap_rate, dt);
                break;

            case LIDType::ROOF_DISCON:
                batchRoofDisconFlux(g, rainfall, evap_rate, dt);
                break;
        }
    }
}

} // namespace lid
} // namespace openswmm
