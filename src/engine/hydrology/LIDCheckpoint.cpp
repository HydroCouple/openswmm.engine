// SPDX-License-Identifier: Apache-2.0
#include "LID.hpp"
#include "../core/CompletedSourceArchive.hpp"
namespace openswmm::lid {
void LIDSolver::checkpoint(detail::CompletedSourceArchive& a,bool model) {
    if(model)a(usage_order_);
    a.sequence(groups_,[&](auto& g) {
        if(model) {
            a(g.type,g.count,g.ucf_raindepth,g.ucf_rainfall,g.n_pollutants);
    a(g.subcatch_idx,g.control_idx,g.area,g.from_imperv,g.from_perv,g.to_perv);
    a(g.drain_node,g.drain_subcatch,g.rpt_path,g.surf_store,g.surf_rough,g.surf_slope);
    a(g.soil_thick,g.soil_poros,g.soil_fc,g.soil_wp,g.soil_ksat,g.soil_kslope);
    a(g.soil_suction,g.stor_thick,g.stor_void,g.stor_ksat,g.stor_clog,g.stor_covered);
    a(g.drain_coeff,g.drain_expon,g.drain_offset,g.drain_delay,g.drain_hopen,g.drain_hclose);
    a(g.pave_thick,g.pave_void,g.pave_imperv_frac,g.pave_ksat,g.pave_clog_factor,g.pave_regen_days);
    a(g.pave_regen_deg,g.drainmat_thick,g.drainmat_void,g.drainmat_rough,g.surf_void_frac,g.surf_alpha);
    a(g.surf_side_slope,g.full_width,g.unit_area,g.unit_width,g.can_overflow,g.drainmat_alpha);
    a(g.drain_rmvl);
        } else {
            a.fixed(g.inflow,g.evap_rate_unit,g.subcatch_rain,g.rpt_surf_infil,g.rpt_pave_perc,g.rpt_soil_perc);
            a.fixed(g.drain_open,g.next_regen_day,g.dry_time,g.surf_depth,g.soil_moist,g.stor_depth);
            a.fixed(g.pave_depth,g.old_drain_flow,g.is_wet,g.surface_runoff,g.drain_flow,g.evap_loss);
            a.fixed(g.infil_loss,g.in_surf,g.in_pave,g.in_soil,g.in_stor,g.evap_surf);
            a.fixed(g.evap_pave,g.evap_soil,g.evap_stor,g.f_old_surf,g.f_old_soil,g.f_old_stor);
            a.fixed(g.f_old_pave,g.wb_inflow,g.wb_evap,g.spatial_infil_loss,g.wb_spatial_infil,g.wb_infil);
            a.fixed(g.wb_surf_flow,g.wb_drain_flow,g.wb_init_vol,g.wb_final_vol,g.vol_treated);
            a.sequence(g.soil_infil,[&](auto& h){a(h.S,h.Ks,h.IMDmax,h.IMD,h.F,h.Fu,h.Fumax,h.Lu,h.T,h.saturated);},true);
        }
    },true);
    if(!model){a(old_runoff_sec_,recovery_factor_);
            a.fixed(native_infil_,max_native_infil_,infil_factor_);}
}
}
