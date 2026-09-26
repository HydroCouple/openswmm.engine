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
 * @file LID.cpp
 * @brief LID control modules — batch-oriented, type-grouped.
 * @ingroup new_engine
 *
 * @author   Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license  Apache-2.0
 */

#include "LID.hpp"
#include "../core/SimulationContext.hpp"
#include "../core/UnitConversion.hpp"
#include <cmath>
#include <algorithm>
#include <array>

namespace openswmm {
namespace lid {

// static constexpr double MINFLOW = 2.3e-8;   // 0.001 in/hr in ft/sec

void LIDGroupSoA::resize(int n) {
    count = n;
    auto un = static_cast<std::size_t>(n);

    subcatch_idx.assign(un, -1);
    control_idx.assign(un, -1);
    area.assign(un, 0.0);
    from_imperv.assign(un, 0.0);
    from_perv.assign(un, 0.0);
    to_perv.assign(un, 0);
    drain_node.assign(un, -1);
    drain_subcatch.assign(un, -1);
    inflow.assign(un, 0.0);
    evap_rate_unit.assign(un, 0.0);

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
    unit_area.assign(un, 0.0);
    unit_width.assign(un, 0.0);
    dry_time.assign(un, 0.0);
    subcatch_rain.assign(un, 0.0);

    surf_depth.assign(un, 0.0);
    soil_moist.assign(un, 0.2);
    stor_depth.assign(un, 0.0);
    pave_depth.assign(un, 0.0);

    // drain_rmvl sized separately in model builder (needs n_pollutants)
    drain_rmvl.clear();

    in_surf.assign(un, 0.0);
    in_pave.assign(un, 0.0);
    in_soil.assign(un, 0.0);
    in_stor.assign(un, 0.0);

    f_old_surf.assign(un, 0.0);
    f_old_soil.assign(un, 0.0);
    f_old_stor.assign(un, 0.0);
    f_old_pave.assign(un, 0.0);

    soil_infil.assign(un, GreenAmptState{});
    old_drain_flow.assign(un, 0.0);
    is_wet.assign(un, 0);
    can_overflow.assign(un, 1);
    drainmat_alpha.assign(un, 0.0);
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

    // Unit-conversion factors for the model's flow units. LID_CONTROLS /
    // LID_USAGE parameters arrive in the user's display units (in|mm, in/hr|
    // mm/hr, ac|ha), but the whole LID solver runs in internal ft / ft-per-sec
    // / ft². Convert every layer parameter here, mirroring the legacy read*Data
    // routines in src/legacy/engine/lid.c — without this the underdrain and
    // conductivity terms are ~10^5-10^6× off (issue #102).
    const double ucfRainDepth = ucf::UCF(ucf::RAINDEPTH, ctx.options); // ft↔in|mm
    const double ucfRainfall  = ucf::UCF(ucf::RAINFALL, ctx.options);  // ft/s↔rate
    const double ucfLength    = ucf::UCF(ucf::LENGTH, ctx.options);    // ft↔ft|m
    const double ucfLength2   = ucfLength * ucfLength;                 // ft²↔ft²|m²

    // If no LID usage data, leave all groups empty
    int n_usage = ctx.lid_usage.count();
    ctx.lid_usage.resize_wb(n_usage);
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

        // Usage-level fields. area and full_width are scaled by the
        // replicate count so g.area is the TOTAL footprint of the usage row
        // (legacy lidUnit->area * lidUnit->number, lid.c:1688): every
        // consumer — inflow capture, outflow/drain coupling,
        // total_lid_area_ft2 — multiplies g.area as the footprint, while the
        // per-unit flux dynamics only ever use the full_width/area ratio,
        // which the common factor leaves unchanged (issue #131).
        //
        // They are also converted out of the deck's display units, like every
        // other parameter read above: [LID_USAGE] Area is ft²|m² and Width is
        // ft|m, while g.area / g.full_width are internal ft² / ft. Without the
        // division `total_lid_area_ft2` (summed from g.area in initTotals())
        // holds m² on an SI deck despite its name, and every consumer that
        // divides a CFS rate by it — the inflow capture below, the full-
        // coverage runon branch in SWMMEngine::stepRunoff — is off by 1/0.3048²
        // (issue #102). US decks are unaffected: Ucf[LENGTH][US] is 1.0.
        double n_units = static_cast<double>(ctx.lid_usage.number[uj]);
        g.subcatch_idx[us] = ctx.lid_usage.subcatch_index[uj];
        g.control_idx[us]  = li;
        g.area[us]         = ctx.lid_usage.area[uj]  * n_units / ucfLength2;
        g.full_width[us]   = ctx.lid_usage.width[uj] * n_units / ucfLength;
        // The swale kernel (legacy swaleFluxRates) is NOT invariant under the
        // n_units scaling: botWidth = topWidth - 2*slope*thickness and the
        // hydraulic radius use ONE unit's width and area (lidUnit->fullWidth,
        // lidUnit->area), so the kernel is handed the per-unit values.
        g.unit_area[us]    = ctx.lid_usage.area[uj]  / ucfLength2;
        g.unit_width[us]   = ctx.lid_usage.width[uj] / ucfLength;
        g.from_imperv[us]  = ctx.lid_usage.from_imperv[uj] / 100.0;  // % → fraction
        g.from_perv[us]    = (uj < ctx.lid_usage.from_perv.size())
                             ? ctx.lid_usage.from_perv[uj] / 100.0 : 0.0;
        g.to_perv[us]      = ctx.lid_usage.to_perv[uj];
        // legacy validateLidGroup: no pervious area to return to on a
        // (near-)fully impervious subcatchment.
        {
            const int sc0 = ctx.lid_usage.subcatch_index[uj];
            if (sc0 >= 0 && static_cast<std::size_t>(sc0) < ctx.subcatches.frac_imperv.size() &&
                ctx.subcatches.frac_imperv[static_cast<std::size_t>(sc0)] >= 0.999)
                g.to_perv[us] = 0;
        }

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
        // Legacy readSurfaceData: a zero storage height zeroes the vegetation
        // fraction; roughness and slope are kept as given (a zero of either
        // makes alpha 0 in validateLidProc, which is what lets the surface
        // overflow instantly instead of draining by Manning's equation).
        if (uli < ctx.lid_controls.surface.size()) {
            const auto& p = ctx.lid_controls.surface[uli];
            const double veg = (p[0] == 0.0) ? 0.0 : p[1];
            g.surf_store[us]      = p[0] / ucfRainDepth;              // in|mm → ft
            g.surf_void_frac[us]  = 1.0 - veg;
            g.surf_rough[us]      = p[2];
            g.surf_slope[us]      = p[3] / 100.0;                     // % → fraction
            g.surf_side_slope[us] = p[4];  // swale side slope (run/rise)
        }

        // SOIL layer: [0]=Thick, [1]=Poros, [2]=FC, [3]=WP, [4]=Ksat, [5]=Kslope, [6]=Suction
        if (uli < ctx.lid_controls.soil.size()) {
            const auto& p = ctx.lid_controls.soil[uli];
            g.soil_thick[us]    = p[0] / ucfRainDepth;   // in|mm  → ft
            g.soil_poros[us]    = p[1];
            g.soil_fc[us]       = p[2];
            g.soil_wp[us]       = p[3];
            g.soil_ksat[us]     = p[4] / ucfRainfall;    // in/hr|mm/hr → ft/sec
            g.soil_kslope[us]   = p[5];
            g.soil_suction[us]  = p[6] / ucfRainDepth;   // in|mm  → ft
        }

        // STORAGE layer: [0]=Thick, [1]=VoidRatio, [2]=Ksat, [3]=ClogFactor
        if (uli < ctx.lid_controls.storage.size()) {
            const auto& p = ctx.lid_controls.storage[uli];
            g.stor_thick[us] = p[0] / ucfRainDepth;              // in|mm → ft
            g.stor_void[us]  = (p[1] > 0.0) ? p[1] / (p[1] + 1.0) : 0.0; // ratio → fraction
            g.stor_ksat[us]  = p[2] / ucfRainfall;              // in/hr|mm/hr → ft/sec
            g.stor_clog[us]  = p[3];
        }

        // DRAIN layer: [0]=Coeff, [1]=Expon, [2]=Offset, [3]=Delay, [4]=hOpen, [5]=hClose
        if (uli < ctx.lid_controls.drain.size()) {
            const auto& p = ctx.lid_controls.drain[uli];
            g.drain_coeff[us]  = p[0];                    // user units; see getDrainRate
            g.drain_expon[us]  = p[1];                    // dimensionless
            g.drain_offset[us] = p[2] / ucfRainDepth;     // in|mm → ft
            g.drain_delay[us]  = p[3] * 3600.0;           // hours → seconds
            g.drain_hopen[us]  = p[4] / ucfRainDepth;     // in|mm → ft
            g.drain_hclose[us] = p[5] / ucfRainDepth;     // in|mm → ft
        }

        // PAVEMENT layer: [0]=Thick, [1]=VoidRatio, [2]=FracImperv, [3]=Ksat, [4]=ClogFactor, [5]=RegenDays
        if (uli < ctx.lid_controls.pavement.size()) {
            const auto& p = ctx.lid_controls.pavement[uli];
            g.pave_thick[us]       = p[0] / ucfRainDepth;               // in|mm → ft
            g.pave_void[us]        = (p[1] > 0.0) ? p[1] / (p[1] + 1.0) : 0.0; // ratio → fraction
            g.pave_imperv_frac[us] = p[2];
            g.pave_ksat[us]        = p[3] / ucfRainfall;               // in/hr|mm/hr → ft/sec
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
            g.drainmat_thick[us] = p[0] / ucfRainDepth;   // in|mm → ft
            g.drainmat_void[us]  = p[1];
            g.drainmat_rough[us] = p[2];
        }

        // Legacy validateLidProc (lid.c): the derived parameters, in its
        // operation order. PHI is legacy's 1.486.
        {
            constexpr double PHI = 1.486;
            if (g.type == LIDType::VEG_SWALE) {
                g.surf_alpha[us] = (g.surf_rough[us] * g.surf_slope[us] > 0.0 &&
                                    g.surf_store[us] != 0.0)
                    ? PHI * std::sqrt(g.surf_slope[us]) / g.surf_rough[us] : 0.0;
            } else {
                g.surf_alpha[us] = (g.surf_rough[us] > 0.0)
                    ? PHI / g.surf_rough[us] * std::sqrt(g.surf_slope[us]) : 0.0;
            }
            g.drainmat_alpha[us] = (g.drainmat_rough[us] > 0.0)
                ? PHI / g.drainmat_rough[us] * std::sqrt(g.surf_slope[us]) : 0.0;
            // Clogging factors become the treated volume (ft) at which the
            // layer is fully clogged: void volume times the user's factor.
            if (g.pave_thick[us] > 0.0)
                g.pave_clog_factor[us] *= g.pave_thick[us] * g.pave_void[us]
                                        * (1.0 - g.pave_imperv_frac[us]);
            if (g.stor_thick[us] > 0.0)
                g.stor_clog[us] *= g.stor_thick[us] * g.stor_void[us];
            else {
                g.stor_clog[us]     = 0.0;
                g.stor_void[us]     = 1.0;   // no storage layer
                g.drain_offset[us]  = 0.0;
            }
            bool can_overflow = true;
            switch (g.type) {
                case LIDType::ROOF_DISCON: can_overflow = false; break;
                case LIDType::INFIL_TRENCH: case LIDType::PERM_PAVEMENT:
                case LIDType::BIO_CELL: case LIDType::RAIN_GARDEN:
                case LIDType::GREEN_ROOF:
                    if (g.surf_alpha[us] > 0.0) can_overflow = false;
                    break;
                default: break;
            }
            g.can_overflow[us] = can_overflow ? 1 : 0;
            if (g.type == LIDType::RAIN_BARREL) {
                g.stor_void[us] = 1.0;
                g.stor_ksat[us] = 0.0;
            }
            if (g.type == LIDType::GREEN_ROOF) {
                // The drainage mat IS the storage layer.
                g.stor_thick[us] = g.drainmat_thick[us];
                g.stor_void[us]  = g.drainmat_void[us];
                g.stor_clog[us]  = 0.0;
                g.stor_ksat[us]  = 0.0;
            }
        }

        // Initial saturation (percent → fraction, legacy x[2]/100)
        double initSat = ctx.lid_usage.init_sat[uj] / 100.0;
        if (g.soil_thick[us] > 0.0) {
            g.soil_moist[us] = g.soil_wp[us]
                             + initSat * (g.soil_poros[us] - g.soil_wp[us]);
        }
        if (g.stor_thick[us] > 0.0) {
            g.stor_depth[us] = initSat * g.stor_thick[us];
        }

        // The unit's own Green-Ampt state for surface-to-soil infiltration
        // (legacy validateLidGroup lid.c:1173-1185 + lid_initState): the soil
        // layer's suction, conductivity and (porosity - wilt point) x
        // (1 - initSat) as the initial moisture deficit, handed through
        // grnampt_setParams in the deck's units exactly as legacy does. A
        // vegetative swale on a Green-Ampt subcatchment takes that
        // subcatchment's parameters instead. Ks stays 0 otherwise, and the
        // kernel then falls back to the native-soil rate.
        g.soil_infil[us] = GreenAmptState{};
        if (g.soil_thick[us] > 0.0) {
            infil::grnampt_init(g.soil_infil[us],
                                g.soil_suction[us] * ucfRainDepth,
                                g.soil_ksat[us] * ucfRainfall,
                                (g.soil_poros[us] - g.soil_wp[us]) * (1.0 - initSat),
                                ctx.options);
        }
        if (g.type == LIDType::VEG_SWALE) {
            const int sc = g.subcatch_idx[us];
            const auto usc = static_cast<std::size_t>(sc);
            if (sc >= 0 && usc < ctx.subcatches.infil_model.size() &&
                (ctx.subcatches.infil_model[usc] == 2 || ctx.subcatches.infil_model[usc] == 3)) {
                infil::grnampt_init(g.soil_infil[us],
                                    ctx.subcatches.infil_p1[usc], ctx.subcatches.infil_p2[usc],
                                    ctx.subcatches.infil_p3[usc], ctx.options);
            }
        }
        // legacy lid_initState: dryTime starts at DRY_DAYS
        g.dry_time[us] = ctx.options.dry_days * 86400.0;
        g.old_drain_flow[us] = 0.0;
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

    // Gap #60: accumulate total LID area (ft²) per subcatchment for snow plow exclusion.
    // Matches legacy Subcatch[i].lidArea used in snow.c Build 5.2.0.
    int n_sc = ctx.n_subcatches();
    if (n_sc > 0) {
        ctx.subcatches.total_lid_area_ft2.assign(static_cast<std::size_t>(n_sc), 0.0);
        for (auto& grp : groups_) {
            for (int u = 0; u < grp.count; ++u) {
                auto uu = static_cast<std::size_t>(u);
                int sc = grp.subcatch_idx[uu];
                if (sc >= 0 && sc < n_sc)
                    ctx.subcatches.total_lid_area_ft2[static_cast<std::size_t>(sc)] += grp.area[uu];
            }
        }
        // Legacy lid_validate() (lid.c:1234): snap the LID total to the full
        // subcatchment area when within 0.1%, so unit-conversion roundoff
        // cannot leave a sliver of runoff-generating area on a fully
        // LID-covered subcatchment (issue #131).
        double ucf_area = ucf::UCF(ucf::LANDAREA, ctx.options);
        for (int sc = 0; sc < n_sc; ++sc) {
            auto usc = static_cast<std::size_t>(sc);
            double full_ft2 = ctx.subcatches.area[usc] / ucf_area;
            if (ctx.subcatches.total_lid_area_ft2[usc] > 0.999 * full_ft2)
                ctx.subcatches.total_lid_area_ft2[usc] = full_ft2;
        }
    }
}

double LIDSolver::storedVolume() const {
    // Same per-unit water content the water-balance init uses above (and
    // legacy lid_getStoredVolume(), lid.c:1426): void-weighted layer depths
    // times the unit footprint.
    double total = 0.0;
    for (const auto& g : groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            total += (g.surf_depth[ui] * g.surf_void_frac[ui]
                      + g.soil_moist[ui] * g.soil_thick[ui]
                      + g.stor_depth[ui] * g.stor_void[ui]
                      + g.pave_depth[ui] * g.pave_void[ui]
                          * (1.0 - g.pave_imperv_frac[ui]))
                     * g.area[ui];
        }
    }
    return total;
}

// ============================================================================
// Legacy lidproc.c — the per-unit kernel, ported op for op
// ============================================================================
// Every LID type runs through legacy lidproc_getOutflow(): the layer moisture
// levels x[SURF, SOIL, STOR, PAVE] are advanced by modpuls_solve() with the
// type's flux-rate function (one explicit step for every type but the
// vegetative swale, which iterates with omega = 0.5), the results are
// clamped to the layer limits, any surface excess above the storage depth
// spills as overflow, and the unit's evaporation, exfiltration and drain
// flow are the kernel's shared rates after the step. The kernel below keeps
// legacy's file-scope shared variables as members and each legacy function
// as a method, so the arithmetic — including its operation order, its
// clamp order and every MIN/MAX — is legacy's. The SoA arrays hold the
// per-unit parameters and state; a unit is a view onto one slot.
//
// The earlier per-type batch kernels were schemes of their own (a different
// soil percolation law, no evaporation cascade, no Green-Ampt on the soil
// layer, a different drain hysteresis, no Manning surface outflow, ...) and
// every LID deck in the parity corpus failed; this is what closes them.
namespace {

constexpr int    L_SURF = 0, L_SOIL = 1, L_STOR = 2, L_PAVE = 3, L_MAX = 4;
constexpr double L_BIG     = 1.0e10;      // legacy BIG
constexpr double L_ZERO    = 1.0e-10;     // legacy ZERO
constexpr double L_STOPTOL = 0.00328;     // legacy STOPTOL (1 mm)
constexpr double L_MINFLOW = 2.3e-8;      // legacy MINFLOW (0.001 in/hr)

/// Per-unit parameter view (ft, ft/s; legacy TLidProc after validateLidProc).
struct LidProcView {
    LIDType type;
    double surf_thick, surf_void, surf_alpha, surf_side;
    bool   can_overflow;
    double pave_thick, pave_void, pave_imperv, pave_ksat, pave_clog,
           pave_regen_days, pave_regen_deg;
    double soil_thick, soil_poros, soil_fc, soil_wp, soil_ksat, soil_kslope;
    double stor_thick, stor_void, stor_ksat, stor_clog;
    double drain_coeff, drain_expon, drain_offset, drain_delay,
           drain_hopen, drain_hclose;
    double dm_thick, dm_alpha;
    double area, full_width;                  // legacy TLidUnit
    double ucf_rainfall, ucf_raindepth;
};

class LegacyLidKernel {
public:
    // legacy file-scope shared variables (lidproc.c)
    double Tstep = 0.0, EvapRate = 0.0, MaxNativeInfil = L_BIG;
    double SurfaceInflow = 0, SurfaceInfil = 0, SurfaceEvap = 0, SurfaceOutflow = 0;
    double PaveEvap = 0, PavePerc = 0, SoilEvap = 0, SoilPerc = 0;
    double StorageInflow = 0, StorageExfil = 0, StorageEvap = 0, StorageDrain = 0;
    double SurfaceVolume = 0, PaveVolume = 0, SoilVolume = 0, StorageVolume = 0;
    // the unit (legacy theLidUnit fields the kernel reads / writes)
    const LidProcView* P = nullptr;
    LIDGroupSoA* G = nullptr;
    std::size_t U = 0;
    double old_runoff_days = 0.0;

    // ---- legacy getSurfaceOutflowRate
    double getSurfaceOutflowRate(double depth) const {
        double delta = depth - P->surf_thick;
        if (delta < 0.0) return 0.0;
        double outflow = P->surf_alpha * std::pow(delta, 5.0 / 3.0) *
                         P->full_width / P->area;
        outflow = std::min(outflow, delta / Tstep);
        return outflow;
    }

    // ---- legacy getSurfaceOverflowRate (clamps the depth it is handed)
    double getSurfaceOverflowRate(double* surfaceDepth) const {
        double delta = *surfaceDepth - P->surf_thick;
        if (delta <= 0.0) return 0.0;
        *surfaceDepth = P->surf_thick;
        return delta * P->surf_void / Tstep;
    }

    // ---- legacy getPavementPermRate
    double getPavementPermRate() {
        double permReduction = 0.0;
        double clogFactor = P->pave_clog;
        double regenDays  = P->pave_regen_days;
        if (clogFactor > 0.0) {
            if (regenDays > 0.0) {
                if (old_runoff_days >= G->next_regen_day[U]) {
                    G->vol_treated[U] *= (1.0 - P->pave_regen_deg);
                    G->next_regen_day[U] += regenDays;
                }
            }
            permReduction = G->vol_treated[U] / clogFactor;
            permReduction = std::min(permReduction, 1.0);
        }
        return P->pave_ksat * (1.0 - permReduction);
    }

    // ---- legacy getSoilPercRate
    double getSoilPercRate(double theta) const {
        if (theta <= P->soil_fc) return 0.0;
        double delta = P->soil_poros - theta;
        return P->soil_ksat * std::exp(-delta * P->soil_kslope);
    }

    // ---- legacy getStorageExfilRate
    double getStorageExfilRate() const {
        double infil = 0.0;
        double clogFactor = 0.0;
        if (P->stor_ksat == 0.0) return 0.0;
        if (MaxNativeInfil == 0.0) return 0.0;
        clogFactor = P->stor_clog;
        if (clogFactor > 0.0) {
            clogFactor = G->wb_inflow[U] / clogFactor;
            clogFactor = std::min(clogFactor, 1.0);
        }
        infil = P->stor_ksat * (1.0 - clogFactor);
        return std::min(infil, MaxNativeInfil);
    }

    // ---- legacy getStorageDrainRate
    double getStorageDrainRate(double storageDepth, double soilTheta,
                               double paveDepth, double surfaceDepth) const {
        double head = storageDepth;
        double outflow = 0.0;
        const double paveThickness    = P->pave_thick;
        const double soilThickness    = P->soil_thick;
        const double soilPorosity     = P->soil_poros;
        const double soilFieldCap     = P->soil_fc;
        const double storageThickness = P->stor_thick;
        if (storageDepth >= storageThickness) {
            if (soilThickness > 0.0) {
                if (soilTheta > soilFieldCap) {
                    head += (soilTheta - soilFieldCap) /
                            (soilPorosity - soilFieldCap) * soilThickness;
                    if (soilTheta >= soilPorosity) {
                        if (paveThickness > 0.0) {
                            head += paveDepth;
                            if (paveDepth >= paveThickness) head += surfaceDepth;
                        }
                        else head += surfaceDepth;
                    }
                }
            }
            else if (paveThickness > 0.0) {
                head += paveDepth;
                if (paveDepth >= paveThickness) head += surfaceDepth;
            }
        }
        if (G->old_drain_flow[U] == 0.0 && head <= P->drain_hopen) return 0.0;
        if (G->old_drain_flow[U] > 0.0 && head <= P->drain_hclose) return 0.0;
        head -= P->drain_offset;
        if (head > L_ZERO) {
            head *= P->ucf_raindepth;
            outflow = P->drain_coeff * std::pow(head, P->drain_expon);
            // (a user-supplied drain control curve is not carried by the
            //  control store; legacy would multiply by its lookup here)
            outflow /= P->ucf_rainfall;
        }
        return outflow;
    }

    // ---- legacy getDrainMatOutflow
    double getDrainMatOutflow(double depth) const {
        double result = SoilPerc;
        if (P->dm_alpha > 0.0) {
            result = P->dm_alpha * std::pow(depth, 5.0 / 3.0) *
                     P->full_width / P->area * P->stor_void;
        }
        return result;
    }

    // ---- legacy getEvapRates
    void getEvapRates(double surfaceVol, double paveVol, double soilVol,
                      double storageVol, double pervFrac) {
        double availEvap = EvapRate;
        SurfaceEvap = std::min(availEvap, surfaceVol / Tstep);
        SurfaceEvap = std::max(0.0, SurfaceEvap);
        availEvap = std::max(0.0, (availEvap - SurfaceEvap));
        availEvap *= pervFrac;
        if (SurfaceInfil > 0.0) {
            PaveEvap = 0.0;
            SoilEvap = 0.0;
            StorageEvap = 0.0;
        } else {
            PaveEvap = std::min(availEvap, paveVol / Tstep);
            availEvap = std::max(0.0, (availEvap - PaveEvap));
            SoilEvap = std::min(availEvap, soilVol / Tstep);
            availEvap = std::max(0.0, (availEvap - SoilEvap));
            StorageEvap = std::min(availEvap, storageVol / Tstep);
        }
    }

    // ---- legacy roofFluxRates
    void roofFluxRates(const double x[], double f[]) {
        double surfaceDepth = x[L_SURF];
        getEvapRates(surfaceDepth, 0.0, 0.0, 0.0, 1.0);
        SurfaceVolume = surfaceDepth;
        SurfaceInfil = 0.0;
        if (P->surf_alpha > 0.0)
            SurfaceOutflow = getSurfaceOutflowRate(surfaceDepth);
        else getSurfaceOverflowRate(&surfaceDepth);
        StorageDrain = std::min(P->drain_coeff / P->ucf_rainfall, SurfaceOutflow);
        SurfaceOutflow -= StorageDrain;
        f[L_SURF] = (SurfaceInflow - SurfaceEvap - StorageDrain - SurfaceOutflow);
    }

    // ---- legacy greenRoofFluxRates
    void greenRoofFluxRates(const double x[], double f[]) {
        double availVolume, maxRate;
        const double soilThickness    = P->soil_thick;
        const double storageThickness = P->stor_thick;
        const double soilPorosity     = P->soil_poros;
        const double storageVoidFrac  = P->stor_void;
        const double soilFieldCap     = P->soil_fc;
        const double soilWiltPoint    = P->soil_wp;
        const double surfaceDepth = x[L_SURF];
        const double soilTheta    = x[L_SOIL];
        const double storageDepth = x[L_STOR];
        SurfaceVolume = surfaceDepth * P->surf_void;
        SoilVolume = soilTheta * soilThickness;
        StorageVolume = storageDepth * storageVoidFrac;
        availVolume = SoilVolume - soilWiltPoint * soilThickness;
        getEvapRates(SurfaceVolume, 0.0, availVolume, StorageVolume, 1.0);
        if (soilTheta >= soilPorosity) StorageEvap = 0.0;
        SoilPerc = getSoilPercRate(soilTheta);
        availVolume = (soilTheta - soilFieldCap) * soilThickness;
        maxRate = std::max(availVolume, 0.0) / Tstep - SoilEvap;
        SoilPerc = std::min(SoilPerc, maxRate);
        SoilPerc = std::max(SoilPerc, 0.0);
        StorageExfil = 0.0;
        StorageDrain = getDrainMatOutflow(storageDepth);
        if (soilTheta >= soilPorosity && storageDepth >= storageThickness) {
            maxRate = std::min(SoilPerc, StorageDrain);
            SoilPerc = maxRate;
            StorageDrain = maxRate;
            SurfaceInfil = std::min(SurfaceInfil, maxRate);
        } else {
            maxRate = storageDepth * storageVoidFrac / Tstep - StorageEvap;
            if (storageDepth >= storageThickness) maxRate += SoilPerc;
            maxRate = std::max(maxRate, 0.0);
            StorageDrain = std::min(StorageDrain, maxRate);
            maxRate = (storageThickness - storageDepth) * storageVoidFrac / Tstep +
                      StorageDrain + StorageEvap;
            SoilPerc = std::min(SoilPerc, maxRate);
            maxRate = (soilPorosity - soilTheta) * soilThickness / Tstep +
                      SoilPerc + SoilEvap;
            SurfaceInfil = std::min(SurfaceInfil, maxRate);
        }
        SurfaceOutflow = getSurfaceOutflowRate(surfaceDepth);
        f[L_SURF] = (SurfaceInflow - SurfaceEvap - SurfaceInfil - SurfaceOutflow) /
                    P->surf_void;
        f[L_SOIL] = (SurfaceInfil - SoilEvap - SoilPerc) / P->soil_thick;
        f[L_STOR] = (SoilPerc - StorageEvap - StorageDrain) / P->stor_void;
    }

    // ---- legacy biocellFluxRates
    void biocellFluxRates(const double x[], double f[]) {
        double availVolume, maxRate;
        const double soilThickness    = P->soil_thick;
        const double soilPorosity     = P->soil_poros;
        const double soilFieldCap     = P->soil_fc;
        const double soilWiltPoint    = P->soil_wp;
        const double storageThickness = P->stor_thick;
        const double storageVoidFrac  = P->stor_void;
        const double surfaceDepth = x[L_SURF];
        const double soilTheta    = x[L_SOIL];
        const double storageDepth = x[L_STOR];
        SurfaceVolume = surfaceDepth * P->surf_void;
        SoilVolume    = soilTheta * soilThickness;
        StorageVolume = storageDepth * storageVoidFrac;
        availVolume = SoilVolume - soilWiltPoint * soilThickness;
        getEvapRates(SurfaceVolume, 0.0, availVolume, StorageVolume, 1.0);
        if (soilTheta >= soilPorosity) StorageEvap = 0.0;
        SoilPerc = getSoilPercRate(soilTheta);
        availVolume = (soilTheta - soilFieldCap) * soilThickness;
        maxRate = std::max(availVolume, 0.0) / Tstep - SoilEvap;
        SoilPerc = std::min(SoilPerc, maxRate);
        SoilPerc = std::max(SoilPerc, 0.0);
        StorageExfil = getStorageExfilRate();
        StorageDrain = 0.0;
        if (P->drain_coeff > 0.0) {
            StorageDrain = getStorageDrainRate(storageDepth, soilTheta, 0.0,
                                               surfaceDepth);
        }
        if (storageThickness == 0.0) {
            StorageEvap = 0.0;
            maxRate = std::min(SoilPerc, StorageExfil);
            SoilPerc = maxRate;
            StorageExfil = maxRate;
            maxRate = (soilPorosity - soilTheta) * soilThickness / Tstep +
                      SoilPerc + SoilEvap;
            SurfaceInfil = std::min(SurfaceInfil, maxRate);
        } else {
            if (soilTheta >= soilPorosity && storageDepth >= storageThickness) {
                maxRate = StorageExfil + StorageDrain;
                if (SoilPerc < maxRate) {
                    maxRate = SoilPerc;
                    if (maxRate > StorageExfil) StorageDrain = maxRate - StorageExfil;
                    else {
                        StorageExfil = maxRate;
                        StorageDrain = 0.0;
                    }
                }
                else SoilPerc = maxRate;
                SurfaceInfil = std::min(SurfaceInfil, maxRate);
            } else {
                maxRate = SoilPerc - StorageEvap + storageDepth * storageVoidFrac / Tstep;
                StorageExfil = std::min(StorageExfil, maxRate);
                StorageExfil = std::max(StorageExfil, 0.0);
                if (StorageDrain > 0.0) {
                    maxRate = -StorageExfil - StorageEvap;
                    if (storageDepth >= storageThickness) maxRate += SoilPerc;
                    if (P->drain_offset <= storageDepth) {
                        maxRate += (storageDepth - P->drain_offset) *
                                   storageVoidFrac / Tstep;
                    }
                    maxRate = std::max(maxRate, 0.0);
                    StorageDrain = std::min(StorageDrain, maxRate);
                }
                maxRate = StorageExfil + StorageDrain + StorageEvap +
                          (storageThickness - storageDepth) *
                          storageVoidFrac / Tstep;
                SoilPerc = std::min(SoilPerc, maxRate);
                maxRate = (soilPorosity - soilTheta) * soilThickness / Tstep +
                          SoilPerc + SoilEvap;
                SurfaceInfil = std::min(SurfaceInfil, maxRate);
            }
        }
        SurfaceOutflow = getSurfaceOutflowRate(surfaceDepth);
        f[L_SURF] = (SurfaceInflow - SurfaceEvap - SurfaceInfil - SurfaceOutflow) /
                    P->surf_void;
        f[L_SOIL] = (SurfaceInfil - SoilEvap - SoilPerc) / P->soil_thick;
        if (storageThickness == 0.0) f[L_STOR] = 0.0;
        else f[L_STOR] = (SoilPerc - StorageEvap - StorageExfil - StorageDrain) /
                         P->stor_void;
    }

    // ---- legacy trenchFluxRates
    void trenchFluxRates(const double x[], double f[]) {
        double availVolume = 0.0;
        double maxRate = 0.0;
        const double storageThickness = P->stor_thick;
        const double storageVoidFrac  = P->stor_void;
        const double surfaceDepth = x[L_SURF];
        const double storageDepth = x[L_STOR];
        SurfaceVolume = surfaceDepth * P->surf_void;
        SoilVolume = 0.0;
        StorageVolume = storageDepth * storageVoidFrac;
        availVolume = (storageThickness - storageDepth) * storageVoidFrac;
        (void)availVolume;
        getEvapRates(SurfaceVolume, 0.0, 0.0, StorageVolume, 1.0);
        if (surfaceDepth > 0.0) StorageEvap = 0.0;
        StorageInflow = SurfaceInflow + SurfaceVolume / Tstep;
        StorageExfil = getStorageExfilRate();
        StorageDrain = 0.0;
        if (P->drain_coeff > 0.0) {
            StorageDrain = getStorageDrainRate(storageDepth, 0.0, 0.0, surfaceDepth);
        }
        maxRate = StorageInflow - StorageEvap + storageDepth * storageVoidFrac / Tstep;
        StorageExfil = std::min(StorageExfil, maxRate);
        StorageExfil = std::max(StorageExfil, 0.0);
        if (StorageDrain > 0.0) {
            maxRate = -StorageExfil - StorageEvap;
            if (storageDepth >= storageThickness) maxRate += StorageInflow;
            if (P->drain_offset <= storageDepth) {
                maxRate += (storageDepth - P->drain_offset) *
                           storageVoidFrac / Tstep;
            }
            maxRate = std::max(maxRate, 0.0);
            StorageDrain = std::min(StorageDrain, maxRate);
        }
        maxRate = (storageThickness - storageDepth) * storageVoidFrac / Tstep +
                  StorageExfil + StorageEvap + StorageDrain;
        StorageInflow = std::min(StorageInflow, maxRate);
        SurfaceInfil = StorageInflow;
        SurfaceOutflow = getSurfaceOutflowRate(surfaceDepth);
        f[L_SURF] = (SurfaceInflow - SurfaceEvap - StorageInflow - SurfaceOutflow) /
                    P->surf_void;
        f[L_STOR] = (StorageInflow - StorageEvap - StorageExfil - StorageDrain) /
                    P->stor_void;
        f[L_SOIL] = 0.0;
    }

    // ---- legacy pavementFluxRates
    void pavementFluxRates(const double x[], double f[]) {
        const double pervFrac = (1.0 - P->pave_imperv);
        double storageInflow;
        double availVolume;
        double maxRate;
        const double paveVoidFrac     = P->pave_void * pervFrac;
        const double paveThickness    = P->pave_thick;
        const double soilThickness    = P->soil_thick;
        const double soilPorosity     = P->soil_poros;
        const double soilFieldCap     = P->soil_fc;
        const double soilWiltPoint    = P->soil_wp;
        const double storageThickness = P->stor_thick;
        const double storageVoidFrac  = P->stor_void;
        const double surfaceDepth = x[L_SURF];
        const double paveDepth    = x[L_PAVE];
        const double soilTheta    = x[L_SOIL];
        const double storageDepth = x[L_STOR];
        SurfaceVolume = surfaceDepth * P->surf_void;
        PaveVolume = paveDepth * paveVoidFrac;
        SoilVolume = soilTheta * soilThickness;
        StorageVolume = storageDepth * storageVoidFrac;
        availVolume = SoilVolume - soilWiltPoint * soilThickness;
        getEvapRates(SurfaceVolume, PaveVolume, availVolume, StorageVolume,
                     pervFrac);
        if (paveDepth >= paveThickness ||
            (soilThickness > 0.0 && soilTheta >= soilPorosity)) StorageEvap = 0.0;
        SurfaceInfil = SurfaceInflow + (SurfaceVolume / Tstep);
        PavePerc = getPavementPermRate() * pervFrac;
        SurfaceInfil = std::min(SurfaceInfil, PavePerc);
        maxRate = PaveVolume / Tstep + SurfaceInfil - PaveEvap;
        maxRate = std::max(maxRate, 0.0);
        PavePerc = std::min(PavePerc, maxRate);
        if (soilThickness > 0.0) {
            SoilPerc = getSoilPercRate(soilTheta);
            availVolume = (soilTheta - soilFieldCap) * soilThickness;
            maxRate = std::max(availVolume, 0.0) / Tstep - SoilEvap;
            SoilPerc = std::min(SoilPerc, maxRate);
            SoilPerc = std::max(SoilPerc, 0.0);
        }
        else SoilPerc = PavePerc;
        StorageExfil = getStorageExfilRate();
        StorageDrain = 0.0;
        if (P->drain_coeff > 0.0) {
            StorageDrain = getStorageDrainRate(storageDepth, soilTheta, paveDepth,
                                               surfaceDepth);
        }
        if (soilThickness == 0.0 &&
            storageDepth >= storageThickness &&
            paveDepth >= paveThickness) {
            maxRate = StorageEvap + StorageDrain + StorageExfil;
            if (PavePerc > maxRate) PavePerc = maxRate;
            else {
                StorageExfil = std::min(StorageExfil, PavePerc);
                StorageDrain = PavePerc - StorageExfil;
            }
            SoilPerc = PavePerc;
            SurfaceInfil = std::min(SurfaceInfil, PavePerc);
        }
        else if (soilThickness > 0 &&
                 storageDepth >= storageThickness &&
                 soilTheta >= soilPorosity &&
                 paveDepth >= paveThickness) {
            maxRate = StorageExfil + StorageDrain;
            if (SoilPerc < maxRate) maxRate = SoilPerc;
            else maxRate = std::min(maxRate, PavePerc);
            if (maxRate > StorageExfil) StorageDrain = maxRate - StorageExfil;
            else {
                StorageExfil = maxRate;
                StorageDrain = 0.0;
            }
            SoilPerc = maxRate;
            PavePerc = maxRate;
            SurfaceInfil = std::min(SurfaceInfil, PavePerc);
        }
        else if (soilThickness > 0.0 &&
                 storageDepth >= storageThickness &&
                 soilTheta >= soilPorosity) {
            maxRate = StorageDrain + StorageExfil;
            if (SoilPerc > maxRate) SoilPerc = maxRate;
            else {
                StorageExfil = std::min(StorageExfil, SoilPerc);
                StorageDrain = SoilPerc - StorageExfil;
            }
            PavePerc = std::min(PavePerc, SoilPerc);
            availVolume = (paveThickness - paveDepth) * paveVoidFrac;
            maxRate = availVolume / Tstep + PavePerc + PaveEvap;
            SurfaceInfil = std::min(SurfaceInfil, maxRate);
        }
        else if (soilThickness > 0.0 &&
                 paveDepth >= paveThickness &&
                 soilTheta >= soilPorosity) {
            PavePerc = std::min(PavePerc, SoilPerc);
            SoilPerc = PavePerc;
            SurfaceInfil = std::min(SurfaceInfil, PavePerc);
            maxRate = std::max(StorageVolume / Tstep + SoilPerc - StorageEvap, 0.0);
            StorageExfil = std::min(StorageExfil, maxRate);
        }
        else {
            maxRate = SoilPerc - StorageEvap + StorageVolume / Tstep;
            maxRate = std::max(0.0, maxRate);
            StorageExfil = std::min(StorageExfil, maxRate);
            if (StorageDrain > 0.0) {
                maxRate = -StorageExfil - StorageEvap;
                if (storageDepth >= storageThickness) maxRate += SoilPerc;
                if (P->drain_offset <= storageDepth) {
                    maxRate += (storageDepth - P->drain_offset) *
                               storageVoidFrac / Tstep;
                }
                maxRate = std::max(maxRate, 0.0);
                StorageDrain = std::min(StorageDrain, maxRate);
            }
            availVolume = (storageThickness - storageDepth) * storageVoidFrac;
            maxRate = availVolume / Tstep + StorageEvap + StorageDrain + StorageExfil;
            maxRate = std::max(maxRate, 0.0);
            if (soilThickness > 0.0) {
                SoilPerc = std::min(SoilPerc, maxRate);
                maxRate = (soilPorosity - soilTheta) * soilThickness / Tstep +
                          SoilPerc;
            }
            PavePerc = std::min(PavePerc, maxRate);
            availVolume = (paveThickness - paveDepth) * paveVoidFrac;
            maxRate = availVolume / Tstep + PavePerc + PaveEvap;
            SurfaceInfil = std::min(SurfaceInfil, maxRate);
        }
        SurfaceOutflow = getSurfaceOutflowRate(surfaceDepth);
        f[L_SURF] = SurfaceInflow - SurfaceEvap - SurfaceInfil - SurfaceOutflow;
        f[L_PAVE] = (SurfaceInfil - PaveEvap - PavePerc) / paveVoidFrac;
        if (P->soil_thick > 0.0) {
            f[L_SOIL] = (PavePerc - SoilEvap - SoilPerc) / soilThickness;
            storageInflow = SoilPerc;
        } else {
            f[L_SOIL] = 0.0;
            storageInflow = PavePerc;
            SoilPerc = 0.0;
        }
        f[L_STOR] = (storageInflow - StorageEvap - StorageExfil - StorageDrain) /
                    storageVoidFrac;
    }

    // ---- legacy swaleFluxRates
    void swaleFluxRates(const double x[], double f[]) {
        double depth = x[L_SURF];
        depth = std::min(depth, P->surf_thick);
        double dStore = 0.0;
        double slope = P->surf_side;
        double topWidth = P->full_width;
        topWidth = std::max(topWidth, 0.5);
        double botWidth = topWidth - 2.0 * slope * P->surf_thick;
        if (botWidth < 0.5) {
            botWidth = 0.5;
            slope = 0.5 * (topWidth - 0.5) / P->surf_thick;
        }
        const double lidArea = P->area;
        const double length = lidArea / topWidth;
        const double surfWidth = botWidth + 2.0 * slope * depth;
        const double surfArea = length * surfWidth;
        double flowArea = (depth * (botWidth + slope * depth)) * P->surf_void;
        const double volume = length * flowArea;
        const double surfInflow = SurfaceInflow * lidArea;
        SurfaceEvap = EvapRate * surfArea;
        SurfaceEvap = std::min(SurfaceEvap, volume / Tstep);
        StorageExfil = SurfaceInfil * surfArea;
        const double xDepth = depth - dStore;
        if (xDepth <= L_ZERO) SurfaceOutflow = 0.0;
        else {
            flowArea -= (dStore * (botWidth + slope * dStore)) * P->surf_void;
            if (flowArea < L_ZERO) SurfaceOutflow = 0.0;
            else {
                botWidth = botWidth + 2.0 * dStore * slope;
                double hydRadius = botWidth + 2.0 * xDepth * std::sqrt(1.0 + slope * slope);
                hydRadius = flowArea / hydRadius;
                SurfaceOutflow = P->surf_alpha * flowArea *
                                 std::pow(hydRadius, 2. / 3.);
            }
        }
        double dVdT = surfInflow - SurfaceEvap - StorageExfil - SurfaceOutflow;
        if (depth == P->surf_thick && dVdT > 0.0) {
            SurfaceOutflow += dVdT;
            dVdT = 0.0;
        }
        SurfaceEvap /= lidArea;
        StorageExfil /= lidArea;
        SurfaceOutflow /= lidArea;
        f[L_SURF] = dVdT / surfArea;
        f[L_SOIL] = 0.0;
        f[L_STOR] = 0.0;
        SurfaceVolume = volume / lidArea;
        SoilVolume = 0.0;
        StorageVolume = 0.0;
    }

    // ---- legacy barrelFluxRates
    void barrelFluxRates(const double x[], double f[]) {
        const double storageDepth = x[L_STOR];
        double head;
        double maxValue;
        SurfaceVolume = 0.0;
        SoilVolume = 0.0;
        StorageVolume = storageDepth;
        SurfaceInfil = 0.0;
        SurfaceOutflow = 0.0;
        StorageDrain = 0.0;
        if (P->drain_delay == 0.0 || G->dry_time[U] >= P->drain_delay) {
            head = storageDepth - P->drain_offset;
            if (head > 0.0) {
                StorageDrain = getStorageDrainRate(storageDepth, 0.0, 0.0, 0.0);
                maxValue = (head / Tstep);
                StorageDrain = std::min(StorageDrain, maxValue);
            }
        }
        StorageInflow = SurfaceInflow;
        maxValue = (P->stor_thick - storageDepth) / Tstep + StorageDrain;
        StorageInflow = std::min(StorageInflow, maxValue);
        SurfaceInfil = StorageInflow;
        f[L_SURF] = SurfaceInflow - StorageInflow;
        f[L_STOR] = StorageInflow - StorageDrain;
        f[L_SOIL] = 0.0;
    }

    void fluxRates(const double x[], double f[]) {
        switch (P->type) {
            case LIDType::BIO_CELL:
            case LIDType::RAIN_GARDEN:   biocellFluxRates(x, f);   break;
            case LIDType::GREEN_ROOF:    greenRoofFluxRates(x, f); break;
            case LIDType::INFIL_TRENCH:  trenchFluxRates(x, f);    break;
            case LIDType::PERM_PAVEMENT: pavementFluxRates(x, f);  break;
            case LIDType::RAIN_BARREL:   barrelFluxRates(x, f);    break;
            case LIDType::ROOF_DISCON:   roofFluxRates(x, f);      break;
            case LIDType::VEG_SWALE:     swaleFluxRates(x, f);     break;
        }
    }

    // ---- legacy modpuls_solve
    int modpulsSolve(int n, double* x, double* xOld, double* xPrev,
                     const double* xMin, const double* xMax, const double* xTol,
                     const double* qOld, double* q, double dt, double omega) {
        int steps = 1;
        const int maxSteps = 20;
        for (int i = 0; i < n; i++) {
            xOld[i] = x[i];
            xPrev[i] = x[i];
        }
        while (steps < maxSteps) {
            int canStop = 1;
            fluxRates(x, q);
            for (int i = 0; i < n; i++) {
                x[i] = xOld[i] + (omega * qOld[i] + (1.0 - omega) * q[i]) * dt;
                x[i] = std::min(x[i], xMax[i]);
                x[i] = std::max(x[i], xMin[i]);
                if (omega > 0.0 &&
                    std::fabs(x[i] - xPrev[i]) > xTol[i]) canStop = 0;
                xPrev[i] = x[i];
            }
            if (canStop) return steps;
            steps++;
        }
        return 0;
    }

    // ---- legacy lidproc_getOutflow: returns the surface outflow (ft/s per
    //      unit area) and the unit's evaporation, exfiltration and drain rates
    double getOutflow(double inflow, double evap, double infil, double maxInfil,
                      double tStep, double infil_factor, double recovery_factor,
                      double* lidEvap, double* lidInfil, double* lidDrain) {
        double x[L_MAX], xOld[L_MAX], xPrev[L_MAX], xMin[L_MAX], xMax[L_MAX];
        double fOld[L_MAX], f[L_MAX];
        double xTol[L_MAX] = {L_STOPTOL, L_STOPTOL, L_STOPTOL, L_STOPTOL};
        double omega = 0.0;

        Tstep = tStep;
        EvapRate = evap;
        MaxNativeInfil = maxInfil;
        x[L_SURF] = G->surf_depth[U];
        x[L_SOIL] = G->soil_moist[U];
        x[L_STOR] = G->stor_depth[U];
        x[L_PAVE] = G->pave_depth[U];

        SurfaceVolume  = 0.0;
        PaveVolume     = 0.0;
        SoilVolume     = 0.0;
        StorageVolume  = 0.0;
        SurfaceInflow  = inflow;
        SurfaceInfil   = 0.0;
        SurfaceEvap    = 0.0;
        SurfaceOutflow = 0.0;
        PaveEvap       = 0.0;
        PavePerc       = 0.0;
        SoilEvap       = 0.0;
        SoilPerc       = 0.0;
        StorageInflow  = 0.0;
        StorageExfil   = 0.0;
        StorageEvap    = 0.0;
        StorageDrain   = 0.0;
        fOld[L_SURF] = G->f_old_surf[U];
        fOld[L_SOIL] = G->f_old_soil[U];
        fOld[L_STOR] = G->f_old_stor[U];
        fOld[L_PAVE] = G->f_old_pave[U];
        for (int i = 0; i < L_MAX; i++) {
            f[i] = 0.0;
            xMin[i] = 0.0;
            xMax[i] = L_BIG;
        }

        // surface-to-soil infiltration: the unit's own Green-Ampt state, or
        // the native soil rate when the unit has no soil layer
        if (P->type == LIDType::PERM_PAVEMENT) SurfaceInfil = 0.0;
        else if (G->soil_infil[U].Ks > 0.0) {
            SurfaceInfil = infil::grnampt_getInfil(G->soil_infil[U], SurfaceInflow,
                                                   G->surf_depth[U], Tstep,
                                                   InfilModel::MOD_GREEN_AMPT,
                                                   infil_factor, recovery_factor);
        }
        else SurfaceInfil = infil;

        if (P->soil_thick > 0.0) {
            xMin[L_SOIL] = P->soil_wp;
            xMax[L_SOIL] = P->soil_poros;
        }
        if (P->pave_thick > 0.0) xMax[L_PAVE] = P->pave_thick;
        if (P->stor_thick > 0.0) xMax[L_STOR] = P->stor_thick;
        if (P->type == LIDType::GREEN_ROOF) xMax[L_STOR] = P->dm_thick;
        if (P->type == LIDType::VEG_SWALE) omega = 0.5;

        modpulsSolve(L_MAX, x, xOld, xPrev, xMin, xMax, xTol, fOld, f, tStep, omega);

        if (P->can_overflow || P->full_width == 0.0)
            SurfaceOutflow += getSurfaceOverflowRate(&x[L_SURF]);

        G->surf_depth[U] = x[L_SURF];
        G->pave_depth[U] = x[L_PAVE];
        G->soil_moist[U] = x[L_SOIL];
        G->stor_depth[U] = x[L_STOR];
        G->f_old_surf[U] = f[L_SURF];
        G->f_old_soil[U] = f[L_SOIL];
        G->f_old_stor[U] = f[L_STOR];
        G->f_old_pave[U] = f[L_PAVE];

        *lidEvap = SurfaceEvap + PaveEvap + SoilEvap + StorageEvap;
        *lidInfil = StorageExfil;
        *lidDrain = StorageDrain;
        return SurfaceOutflow;
    }
};

/// Fill a parameter view from SoA slot u.
inline LidProcView makeView(const LIDGroupSoA& g, std::size_t u) {
    LidProcView p;
    p.type = g.type;
    p.surf_thick = g.surf_store[u]; p.surf_void = g.surf_void_frac[u];
    p.surf_alpha = g.surf_alpha[u]; p.surf_side = g.surf_side_slope[u];
    p.can_overflow = g.can_overflow[u] != 0;
    p.pave_thick = g.pave_thick[u]; p.pave_void = g.pave_void[u];
    p.pave_imperv = g.pave_imperv_frac[u]; p.pave_ksat = g.pave_ksat[u];
    p.pave_clog = g.pave_clog_factor[u]; p.pave_regen_days = g.pave_regen_days[u];
    p.pave_regen_deg = g.pave_regen_deg[u];
    p.soil_thick = g.soil_thick[u]; p.soil_poros = g.soil_poros[u];
    p.soil_fc = g.soil_fc[u]; p.soil_wp = g.soil_wp[u];
    p.soil_ksat = g.soil_ksat[u]; p.soil_kslope = g.soil_kslope[u];
    p.stor_thick = g.stor_thick[u]; p.stor_void = g.stor_void[u];
    p.stor_ksat = g.stor_ksat[u]; p.stor_clog = g.stor_clog[u];
    p.drain_coeff = g.drain_coeff[u]; p.drain_expon = g.drain_expon[u];
    p.drain_offset = g.drain_offset[u]; p.drain_delay = g.drain_delay[u];
    p.drain_hopen = g.drain_hopen[u]; p.drain_hclose = g.drain_hclose[u];
    p.dm_thick = g.drainmat_thick[u]; p.dm_alpha = g.drainmat_alpha[u];
    // ONE unit's area / width (legacy lidUnit->area, ->fullWidth); a hand-built
    // group (unit tests) that only set the footprint is a single unit.
    p.area = g.unit_area[u] > 0.0 ? g.unit_area[u] : g.area[u];
    p.full_width = g.unit_area[u] > 0.0 ? g.unit_width[u] : g.full_width[u];
    p.ucf_rainfall = g.ucf_rainfall; p.ucf_raindepth = g.ucf_raindepth;
    return p;
}

/// Run one unit through the legacy kernel and book its results the way
/// legacy evalLidUnit + lidproc_saveResults do. `evap` is the potential
/// evaporation rate legacy's subcatch_getEvapRate returns for the parent
/// subcatchment, `native_infil` / `max_native_infil` its findNativeInfil
/// values, `inflow` the unit's surface inflow (ft/s).
inline void runUnitLegacy(LIDGroupSoA& g, std::size_t u, double inflow, double evap,
                          double native_infil, double max_native_infil, double dt,
                          double old_runoff_sec, double infil_factor,
                          double recovery_factor) {
    LidProcView view = makeView(g, u);
    LegacyLidKernel k;
    k.P = &view;
    k.G = &g;
    k.U = u;
    k.old_runoff_days = old_runoff_sec / 86400.0;

    double lidEvap = 0.0, lidInfil = 0.0, lidDrain = 0.0;
    double lidRunoff = k.getOutflow(inflow, evap, native_infil, max_native_infil, dt,
                                    infil_factor, recovery_factor,
                                    &lidEvap, &lidInfil, &lidDrain);

    // legacy lidproc_saveResults: water balance and the wet-LID flag
    const double totalEvap = k.SurfaceEvap + k.PaveEvap + k.SoilEvap + k.StorageEvap;
    const double totalVolume = k.SurfaceVolume + k.PaveVolume + k.SoilVolume + k.StorageVolume;
    g.vol_treated[u]   += k.SurfaceInflow * dt;
    g.wb_inflow[u]     += k.SurfaceInflow * dt;
    g.wb_evap[u]       += totalEvap * dt;
    g.wb_infil[u]      += k.StorageExfil * dt;
    g.wb_surf_flow[u]  += k.SurfaceOutflow * dt;
    g.wb_drain_flow[u] += k.StorageDrain * dt;
    g.wb_final_vol[u]   = totalVolume;
    const bool is_dry = (k.SurfaceInflow < L_MINFLOW && k.SurfaceOutflow < L_MINFLOW &&
                         k.StorageDrain < L_MINFLOW && k.StorageExfil < L_MINFLOW &&
                         totalEvap < L_MINFLOW);
    g.is_wet[u] = is_dry ? 0 : 1;

    // outputs (rates per unit area; the engine scales by the footprint)
    g.surface_runoff[u] = lidRunoff;
    g.drain_flow[u]     = lidDrain;
    g.evap_loss[u]      = lidEvap * dt;     // depth this step (ft)
    g.infil_loss[u]     = lidInfil * dt;    // depth this step (ft)

    // per-layer inflow rates after every clamp, for the transport tracks
    g.in_surf[u] = k.SurfaceInflow;
    g.in_pave[u] = (g.type == LIDType::PERM_PAVEMENT) ? k.SurfaceInfil : 0.0;
    g.in_soil[u] = (g.type == LIDType::PERM_PAVEMENT) ? k.PavePerc
                 : (g.soil_thick[u] > 0.0 ? k.SurfaceInfil : 0.0);
    g.in_stor[u] = (g.type == LIDType::RAIN_BARREL || g.type == LIDType::INFIL_TRENCH)
                   ? k.StorageInflow
                   : ((g.soil_thick[u] > 0.0 && g.stor_thick[u] > 0.0) ? k.SoilPerc
                      : (g.type == LIDType::PERM_PAVEMENT ? k.PavePerc : 0.0));
}

} // namespace

// ============================================================================
// Per-type entry points — thin wrappers over the legacy kernel (kept for the
// unit tests that drive a group directly: no engine context, so the native
// infiltration is 0, its ceiling unlimited and the factors 1).
// ============================================================================

namespace {
inline void runGroupStandalone(LIDGroupSoA& g, double rainfall,
                               const double* evap_rate, double dt) {
    constexpr double MIN_RUNOFF = 2.31481e-8;   // legacy consts.h (ft/s)
    for (int i = 0; i < g.count; ++i) {
        auto ui = static_cast<std::size_t>(i);
        double inflow = (g.inflow[ui] > 0.0) ? g.inflow[ui] : rainfall;
        if (g.type == LIDType::RAIN_BARREL && g.stor_covered[ui]) inflow = 0.0;
        const double evap = evap_rate ? evap_rate[ui] : 0.0;
        runUnitLegacy(g, ui, inflow, evap, 0.0, L_BIG, dt, 0.0, 1.0, 1.0);
        // legacy evalLidUnit: the dry clock for the rain-barrel drain delay
        if (g.subcatch_rain[ui] > MIN_RUNOFF) g.dry_time[ui] = 0.0;
        else                                  g.dry_time[ui] += dt;
        g.drain_open[ui] = (g.drain_flow[ui] > 0.0) ? 1 : 0;
        g.old_drain_flow[ui] = g.drain_flow[ui];
    }
}
} // namespace

void LIDSolver::batchBioCellFlux(LIDGroupSoA& g, double rainfall,
                                  const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}
void LIDSolver::batchBarrelFlux(LIDGroupSoA& g, double rainfall, double dt) {
    runGroupStandalone(g, rainfall, nullptr, dt);
}
void LIDSolver::batchInfilTrenchFlux(LIDGroupSoA& g, double rainfall,
                                      const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}
void LIDSolver::batchSwaleFlux(LIDGroupSoA& g, double rainfall,
                                const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}
void LIDSolver::batchSwaleModPuls(LIDGroupSoA& g, double rainfall,
                                   const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}
void LIDSolver::batchGreenRoofFlux(LIDGroupSoA& g, double rainfall,
                                    const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}
void LIDSolver::batchPavementFlux(LIDGroupSoA& g, double rainfall,
                                   const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}
void LIDSolver::batchRoofDisconFlux(LIDGroupSoA& g, double rainfall,
                                     const double* evap_rate, double dt) {
    runGroupStandalone(g, rainfall, evap_rate, dt);
}

// ============================================================================
// Stored-volume queries — feed the runoff-continuity storage term (#102 C)
// ============================================================================

double LIDSolver::totalStoredVolume() const {
    double vol = 0.0;
    for (const auto& g : groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            vol += g.wb_final_vol[ui] * g.area[ui];  // ft depth × ft² = ft³
        }
    }
    return vol;
}

double LIDSolver::totalInitVolume() const {
    double vol = 0.0;
    for (const auto& g : groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            vol += g.wb_init_vol[ui] * g.area[ui];
        }
    }
    return vol;
}

double LIDSolver::totalInfilVolume() const {
    double vol = 0.0;
    for (const auto& g : groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            vol += g.wb_infil[ui] * g.area[ui];  // ft depth × ft² = ft³
        }
    }
    return vol;
}

double LIDSolver::totalEvapVolume() const {
    double vol = 0.0;
    for (const auto& g : groups_) {
        for (int i = 0; i < g.count; ++i) {
            auto ui = static_cast<std::size_t>(i);
            vol += g.wb_evap[ui] * g.area[ui];
        }
    }
    return vol;
}

// ============================================================================
// Execute — all LID types batch
// ============================================================================

void LIDSolver::execute(SimulationContext& ctx, double dt,
                        double rainfall, double evap_rate) {
    constexpr double MIN_RUNOFF = 2.31481e-8;   // legacy consts.h (ft/s)
    (void)rainfall;   // the engine sets each unit's inflow (rain + capture)
    for (auto& g : groups_) {
        if (g.count == 0) continue;
        for (int u = 0; u < g.count; ++u) {
            auto uu = static_cast<std::size_t>(u);
            // legacy lid_getRunoff: a unit without area (0 units or 0 ft2,
            // no-units-w-wo-rg-2subcatchments) is never evaluated — the
            // kernel divides by it.
            if (g.area[uu] <= 0.0) continue;
            int sc = g.subcatch_idx[uu];
            const auto usc = static_cast<std::size_t>(sc);
            // legacy subcatch_getEvapRate: an API-prescribed rate first, else
            // 0 under DRY_ONLY while it rains on the subcatchment, else the
            // climate rate.
            double e_dry = (ctx.options.evap_dry_only && g.subcatch_rain[uu] > 0.0)
                           ? 0.0 : evap_rate;
            const double evap = (sc >= 0)
                ? ctx.forcing.effective_evap_rate(usc, e_dry) : e_dry;
            g.evap_rate_unit[uu] = evap;
            const double native = (sc >= 0 && usc < native_infil_.size())
                                  ? native_infil_[usc] : 0.0;
            const double max_native = (sc >= 0 && usc < max_native_infil_.size())
                                      ? max_native_infil_[usc] : L_BIG;
            const double infil_factor = (sc >= 0 && usc < infil_factor_.size())
                                        ? infil_factor_[usc] : 1.0;
            runUnitLegacy(g, uu, g.inflow[uu], evap, native, max_native, dt,
                          old_runoff_sec_, infil_factor, recovery_factor_);
            // legacy evalLidUnit: the dry clock for the rain-barrel drain
            // delay, updated AFTER the unit ran (it reads the clock as it
            // stood before this step); reset by the parent subcatchment's
            // rainfall above MIN_RUNOFF.
            if (g.subcatch_rain[uu] > MIN_RUNOFF) g.dry_time[uu] = 0.0;
            else                                  g.dry_time[uu] += dt;
        }
    }
}

bool LIDSolver::anyWet() const {
    for (const auto& g : groups_)
        for (int u = 0; u < g.count; ++u)
            if (g.is_wet[static_cast<std::size_t>(u)]) return true;
    return false;
}

} // namespace lid
} // namespace openswmm
