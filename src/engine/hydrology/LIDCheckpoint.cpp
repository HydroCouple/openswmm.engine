// SPDX-License-Identifier: Apache-2.0

/**
 * @file      LIDCheckpoint.cpp
 * @brief     Checkpoint persistence for low-impact development state.
 *
 * @author    Caleb Buahin <caleb.buahin@gmail.com>
 * @copyright Copyright (c) 2026 Caleb Buahin. All rights reserved.
 * @license   Apache-2.0
 */

#include "LID.hpp"
#include "../core/CompletedSourceArchive.hpp"
#include <cmath>
namespace openswmm::lid {
void LIDSolver::checkpoint(detail::CompletedSourceArchive& a, bool model, bool lazy_buffers) {
    if (model)
        a(usage_order_);
    a.sequence(
        groups_,
        [&](auto& g) {
            if (model) {
                a(g.type, g.count, g.ucf_raindepth, g.ucf_rainfall, g.n_pollutants);
                a(g.subcatch_idx, g.control_idx, g.area, g.from_imperv, g.from_perv, g.to_perv);
                a(g.drain_node, g.drain_subcatch, g.rpt_path, g.surf_store, g.surf_rough,
                  g.surf_slope);
                a(g.soil_thick, g.soil_poros, g.soil_fc, g.soil_wp, g.soil_ksat, g.soil_kslope);
                a(g.soil_suction, g.stor_thick, g.stor_void, g.stor_ksat, g.stor_clog,
                  g.stor_covered);
                a(g.drain_coeff, g.drain_expon, g.drain_offset, g.drain_delay, g.drain_hopen,
                  g.drain_hclose);
                a(g.pave_thick, g.pave_void, g.pave_imperv_frac, g.pave_ksat, g.pave_clog_factor,
                  g.pave_regen_days);
                a(g.pave_regen_deg, g.drainmat_thick, g.drainmat_void, g.drainmat_rough,
                  g.surf_void_frac, g.surf_alpha);
                a(g.surf_side_slope, g.full_width, g.unit_area, g.unit_width, g.can_overflow,
                  g.drainmat_alpha);
                a(g.drain_rmvl);
            } else {
                a.fixed(g.inflow, g.evap_rate_unit, g.subcatch_rain, g.rpt_surf_infil,
                        g.rpt_pave_perc, g.rpt_soil_perc);
                a.fixed(g.drain_open, g.next_regen_day, g.dry_time, g.surf_depth, g.soil_moist,
                        g.stor_depth);
                a.fixed(g.pave_depth, g.old_drain_flow, g.is_wet, g.surface_runoff, g.drain_flow,
                        g.evap_loss);
                a.fixed(g.infil_loss, g.in_surf, g.in_pave, g.in_soil, g.in_stor, g.evap_surf);
                a.fixed(g.evap_pave, g.evap_soil, g.evap_stor, g.f_old_surf, g.f_old_soil,
                        g.f_old_stor);
                a.fixed(g.f_old_pave, g.wb_inflow, g.wb_evap, g.spatial_infil_loss,
                        g.wb_spatial_infil, g.wb_infil);
                a.fixed(g.wb_surf_flow, g.wb_drain_flow, g.wb_init_vol, g.wb_final_vol,
                        g.vol_treated);
                a.sequence(
                    g.soil_infil,
                    [&](auto& h) {
                        a(h.S, h.Ks, h.IMDmax, h.IMD, h.F, h.Fu, h.Fumax, h.Lu, h.T, h.saturated);
                    },
                    true);
            }
        },
        true);
    if (!model) {
        a(old_runoff_sec_, recovery_factor_);
        // Engine sessions restore into fresh, still-unallocated runoff buffers.
        // Default component archives retain their fixed-shape read contract.
        if (lazy_buffers)
            a(native_infil_, max_native_infil_, infil_factor_);
        else
            a.fixed(native_infil_, max_native_infil_, infil_factor_);
    }
}
void LIDSolver::validateCheckpoint(const LIDSolver& model, std::size_t runoff_count) const {
    const auto require = [](bool ok) {
        if (!ok)
            throw std::runtime_error("Invalid subcatchment LID checkpoint/material.");
    };
    require(groups_.size() == model.groups_.size() && std::isfinite(old_runoff_sec_) &&
            old_runoff_sec_ >= 0 && std::isfinite(recovery_factor_) && recovery_factor_ >= 0 &&
            rpt_files_.empty());
    for (const auto* row : {&native_infil_, &max_native_infil_, &infil_factor_}) {
        require(row->empty() || row->size() == runoff_count);
        for (double v : *row)
            require(std::isfinite(v) && v >= 0);
    }
    for (std::size_t k = 0; k < groups_.size(); ++k) {
        const auto& g = groups_[k];
        const auto& m = model.groups_[k];
        require(g.count == m.count && g.soil_infil.size() == m.soil_infil.size());
        for (const auto* row :
             {&g.inflow,           &g.evap_rate_unit, &g.subcatch_rain,  &g.rpt_surf_infil,
              &g.rpt_pave_perc,    &g.rpt_soil_perc,  &g.next_regen_day, &g.dry_time,
              &g.surf_depth,       &g.soil_moist,     &g.stor_depth,     &g.pave_depth,
              &g.old_drain_flow,   &g.surface_runoff, &g.drain_flow,     &g.evap_loss,
              &g.infil_loss,       &g.in_surf,        &g.in_pave,        &g.in_soil,
              &g.in_stor,          &g.evap_surf,      &g.evap_pave,      &g.evap_soil,
              &g.evap_stor,        &g.f_old_surf,     &g.f_old_soil,     &g.f_old_stor,
              &g.f_old_pave,       &g.wb_inflow,      &g.wb_evap,        &g.spatial_infil_loss,
              &g.wb_spatial_infil, &g.wb_infil,       &g.wb_surf_flow,   &g.wb_drain_flow,
              &g.wb_init_vol,      &g.wb_final_vol,   &g.vol_treated})
            for (double v : *row)
                require(std::isfinite(v));
        for (int u = 0; u < g.count; ++u) {
            require(g.surf_depth[u] >= -1e-12 && g.stor_depth[u] >= -1e-12 &&
                    g.pave_depth[u] >= -1e-12 && g.soil_moist[u] >= -1e-12 &&
                    (g.soil_thick[u] <= 0 || g.soil_moist[u] <= g.soil_poros[u] + 1e-8) &&
                    g.dry_time[u] >= 0 && g.old_drain_flow[u] >= 0 && g.drain_open[u] >= 0 &&
                    g.drain_open[u] <= 1 && g.is_wet[u] <= 1);
            for (double v : {g.wb_inflow[u], g.wb_evap[u], g.wb_infil[u], g.wb_spatial_infil[u],
                             g.wb_surf_flow[u], g.wb_drain_flow[u], g.wb_init_vol[u],
                             g.wb_final_vol[u], g.vol_treated[u]})
                require(v >= -1e-12);
            require(g.wb_init_vol[u] == m.wb_init_vol[u]);
            const auto& h = g.soil_infil[u];
            const auto& b = m.soil_infil[u];
            require(h.S == b.S && h.Ks == b.Ks && h.IMDmax == b.IMDmax && h.Fumax == b.Fumax &&
                    h.Lu == b.Lu);
            for (double v : {h.S, h.Ks, h.IMDmax, h.IMD, h.F, h.Fu, h.Fumax, h.Lu, h.T})
                require(std::isfinite(v));
            require(h.IMD >= 0 && h.IMD <= h.IMDmax && h.F >= 0 && h.Fu >= 0);
        }
    }
}

}
