// SPDX-License-Identifier: Apache-2.0
#include "SubsurfaceSolver.hpp"
#include "../../core/CompletedSourceArchive.hpp"
#include <cmath>
#include <stdexcept>

namespace openswmm::twoD {
namespace {
void intake(detail::CompletedSourceArchive& a,SurfaceIntakeReceipt& r) {
    auto& q=r.request;a(q.kind,q.source,q.unit,q.cell,q.area,q.pond,q.water_ceiling,q.candidate,
                       r.start,r.end,r.volume,r.mass);
}
void returned(detail::CompletedSourceArchive& a,SurfaceReturnReceipt& r) {
    a(r.kind);intake(a,r.intake);a(r.available_at,r.volume,r.mass);
}
void signal(detail::CompletedSourceArchive& a,GwSourceSignal& s) {
    auto times=s.timePoints(),values=s.ordinates();a(s.constant,times,values);
}
}
void SubsurfaceSolver::checkpoint(detail::CompletedSourceArchive& a,bool model) {
    if(std::any_of(richards_bed_owned_.begin(),richards_bed_owned_.end(),[](bool owned){return owned;}))
        throw std::runtime_error("Richards joint owner checkpoints require the R5 restart extension.");
    auto& s=state_;auto& t=tr_;auto& o=options_;
    if(model) {
        a(s.active,s.n_cells,s.m_layers);
    a.fixed(s.Ks,s.zs,s.theta_s,s.theta_r,s.alpha,s.psi_b,s.lambda,s.vg_n,s.vg_L,s.c_loss,s.soil_char,
            s.closure,s.area,s.z_bed);
        a(o.soil_char,o.closure,o.m_layers,o.capillary_diff,o.c_gw,o.c_col,o.force_closed_form,
          o.per_subcatch,o.dunne,o.gw_et,o.wilting_suction,o.wilting_suction_set,o.node_auto,o.link_seepage,o.authored);
        a(source_owned_,source_species_,cell_beds_,cell_sources_,cell_nface_,subcatch_outlet_node_,cell_subcatch_);
        a.sequence(node_beds_,[&](auto& b){a(b.node,b.cell,b.Kc,b.dC,b.area,b.locate,b.exchange,b.automatic);});
        a.sequence(sources_,[&](auto& r){a(r.name,r.scale,r.cells);signal(a,r.flow);
            a.sequence(r.terms,[&](auto& v){a(v.row,v.massRate,v.massScale);signal(a,v.signal);});});
        a(t.n_species,t.n_cells,t.n_pollut,t.n_msx,t.age_row,t.temp_row,t.dispersion_on);
    a.fixed(t.row_names,t.alpha_L,t.alpha_T,t.D_m,t.rho_b,t.kd,t.decay);
        return;
    }
    a.fixed(s.hg,s.hu,s.theta_sigma,s.q0_last,s.qnode_last,s.qlat_last,s.qdeep_last,s.qet_last,s.dunne_last,
            s.qplus_last,s.infil_capacity,s.infil_remaining,s.infil_refresh,s.infil_interval,s.wetting_front,
            s.reject_last,s.reject_cumulative,s.dunne_cumulative,s.et_pending,s.et_potential_cumulative,
            s.et_surface_cumulative,s.et_soil_cumulative,s.et_unused_cumulative,s.et_surface_last,
            s.et_potential_last,s.et_stress,s.et_refresh,s.qlink_last,s.dt_cell,s.tier,s.eacc_L,s.eacc_R,
            s.xacc_from_surface,s.xacc_to_surface,s.nacc,s.lacc);

    a(s.led_recharge,s.led_lateral,s.led_deep,s.led_node,s.led_reject,s.led_dunne,s.led_caprise,s.led_et,
      s.led_infil_in,s.led_link,s.led_subcatch_in,s.led_init_storage,s.led_final_storage,s.led_source_in,s.led_source_out);
    a.fixed(t.sat_mass,t.unsat_mass,t.sacc_L,t.sacc_R,t.xacc_from_surface,t.xacc_to_surface,t.nacc_mass,
            t.lacc_mass,t.node_out_mass,t.lost_deep,t.lost_node,t.lost_link,t.lost_dunne,t.lost_et,
            t.gained_infil,t.gained_node,t.gained_link,t.net_lateral,t.gained_source,t.lost_source,
            t.lost_reaction,t.init_mass,t.internal_recharge);

    a(t.dispersion_limiter_binds,source_firing_end_,gather_fraction_,cells_by_tier_,faces_by_tier_,face_tier_,tier_firings_,
      pending_surface_,pending_flag_,drain_cap_binds_,node_refunds_,accumulators_pending_);
    a.fixed(source_cell_end_,bed_exchange_cum_,bed_gathered_,node_drawn_gw_,node_exchange_vol_);
    a.sequence(source_receipts_,[&](auto& rows){a.sequence(rows,[&](auto& r){intake(a,r);});},true);
    a.sequence(source_returns_,[&](auto& r){returned(a,r);});
    if(a.reading()) {
        richards_clock_valid_=false;
        const auto fail=[](){throw std::runtime_error("Invalid completed-source aquifer state.");};
        const auto nonnegative=[&](double v){if(!std::isfinite(v)||v<0)fail();};
        const auto paired=[&](double v,const auto& mass){nonnegative(v);if(int(mass.size())!=t.n_species)fail();
            for(double m:mass){nonnegative(m);if(v==0 && m!=0)fail();}};
        for(const auto* values:{&s.hg,&s.hu,&s.theta_sigma,&s.q0_last,&s.qnode_last,&s.qlat_last,
            &s.qdeep_last,&s.qet_last,&s.dunne_last,&s.qplus_last,&s.infil_interval,&s.wetting_front,
            &s.reject_last,&s.reject_cumulative,&s.dunne_cumulative,&s.et_pending,&s.et_potential_cumulative,&s.et_surface_cumulative,
            &s.et_soil_cumulative,&s.et_unused_cumulative,&s.qlink_last,&s.eacc_L,&s.eacc_R,&s.xacc_from_surface,
            &s.xacc_to_surface,&s.nacc,&s.lacc})
            for(double v:*values)if(!std::isfinite(v))throw std::runtime_error("Nonfinite aquifer checkpoint state.");
        // NaN denotes an unpublished infiltration/ET diagnostic, including on untouched cells.
        for(const auto* values:{&s.infil_capacity,&s.infil_remaining,&s.infil_refresh,&s.et_surface_last,&s.et_potential_last,&s.et_refresh,&s.et_stress})
            for(double v:*values)if(std::isinf(v))fail();
        for(const auto* values:{&t.sat_mass,&t.unsat_mass,&t.sacc_L,&t.sacc_R,&t.xacc_from_surface,
            &t.xacc_to_surface,&t.nacc_mass,&t.lacc_mass,&t.node_out_mass,&t.lost_deep,&t.lost_node,
            &t.lost_link,&t.lost_dunne,&t.lost_et,&t.gained_infil,&t.gained_node,&t.gained_link,
            &t.net_lateral,&t.gained_source,&t.lost_source,&t.lost_reaction,&t.init_mass,
            &t.internal_recharge})
            for(double v:*values)if(!std::isfinite(v))throw std::runtime_error("Nonfinite t checkpoint state.");
        const auto receipt=[&](const auto& r){const auto& q=r.request;
            if(q.cell<0||q.cell>=s.n_cells||!source_owned_[q.cell]||
               !std::isfinite(r.start)||!std::isfinite(r.end)||r.start<0||r.end<=r.start||
               int(q.kind)<0||int(q.kind)>2)fail();
            for(double v:{q.area,q.pond,q.water_ceiling,q.candidate})nonnegative(v);
            if(q.kind==SurfaceDonorKind::MESH ? (q.source!="cell:"+std::to_string(q.cell)||q.unit!=-1) : q.source.empty())fail();
            paired(r.volume,r.mass);};
        for(int i=0;i<s.n_cells;++i) {
            nonnegative(s.hg[i]);nonnegative(s.hu[i]);if(s.hg[i]>s.zs[i])fail();nonnegative(source_cell_end_[i]);
            nonnegative(s.et_pending[i]);nonnegative(s.xacc_from_surface[i]);nonnegative(s.xacc_to_surface[i]);
            double volume=0;std::vector<double> mass(t.n_species,0);
            for(const auto& r:source_receipts_[i]){receipt(r);if(r.request.cell!=i)fail();volume+=r.volume;
                for(int p=0;p<t.n_species;++p)mass[p]+=r.mass[p];}
            if(source_owned_[i]) {
                if(std::abs(volume-s.xacc_from_surface[i])>1e-10*std::max(1.,volume)||s.xacc_to_surface[i]!=0)fail();
                for(int p=0;p<t.n_species;++p)if(std::abs(mass[p]-t.xacc_from_surface[t.idx(p,i)])>1e-10*std::max(1.,mass[p])||t.xacc_to_surface[t.idx(p,i)]!=0)fail();
            }
        }
        for(double m:t.sat_mass)nonnegative(m);for(double m:t.unsat_mass)nonnegative(m);
        for(const auto& r:source_returns_) {
            const int i=r.intake.request.cell;if(i<0||i>=s.n_cells||!source_owned_[i])fail();
            paired(r.volume,r.mass);nonnegative(r.available_at);
            if(int(r.kind)<0||int(r.kind)>2||r.available_at>source_cell_end_[i])fail();
            if(r.kind==SurfaceReturnKind::NUMERICAL_REFUND) {
                receipt(r.intake);if(r.available_at<r.intake.end||r.volume>r.intake.volume*(1+1e-10))fail();
                for(int p=0;p<t.n_species;++p)if(std::abs(r.mass[p]-(r.intake.volume>0?r.intake.mass[p]*r.volume/r.intake.volume:0))>1e-10*std::max(1.,r.mass[p]))fail();
            }
        }
        if(gather_fraction_!=1||cells_by_tier_.size()!=faces_by_tier_.size()||cells_by_tier_.size()!=tier_firings_.size()||
           face_tier_.size()!=s.eacc_L.size()||pending_flag_.size()!=std::size_t(s.n_cells))fail();
        if(cells_by_tier_.empty())fail();
        std::vector<int> cell_seen(s.n_cells,0),face_seen(s.eacc_L.size(),0);
        for(std::size_t k=0;k<cells_by_tier_.size();++k) {
            if(tier_firings_[k]<0)fail();
            for(int c:cells_by_tier_[k])if(c<0||c>=s.n_cells||++cell_seen[c]!=1||s.tier[c]!=k)fail();
            for(int f:faces_by_tier_[k])if(f<0||f>=int(s.eacc_L.size())||++face_seen[f]!=1||face_tier_[f]!=k)fail();
        }
        for(int count:cell_seen)if(count!=1)fail();for(int count:face_seen)if(count!=1)fail();
        for(int c:pending_surface_)if(c<0||c>=s.n_cells)fail();
    }
}
}
