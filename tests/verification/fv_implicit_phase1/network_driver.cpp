// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "wet_cases.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>
#include <chrono>
#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

using namespace fvnetwork;
namespace fs=std::filesystem;
namespace {
void mesh(const fs::path& path,const NetworkMeshData& m,bool wet_case=false) {
    std::ofstream out(path); out<<std::setprecision(17);
    const auto write=[&](const char* name,const auto& v){
        out<<name; for (const auto x:v) out<<','<<+x; out<<'\n';
    };
#define V(name) write(#name,m.name)
    V(cell_geom); V(cell_conduit); V(cell_dx); V(cell_zb); V(cell_dzdx);
    V(cell_face0); V(cell_face1); V(cell_side0); V(cell_side1);
    V(face_cl); V(face_cr); V(face_node); V(face_geom); V(face_zb); V(face_dx);
    V(face_dir_l); V(face_dir_r); V(face_virtual); V(face_vj_node); V(face_gate); V(face_culvert);
    V(conduit_cell_begin); V(conduit_cell_count); V(conduit_link); V(conduit_section);
    V(conduit_roughness); V(conduit_rough_factor); V(conduit_loss_inlet); V(conduit_loss_outlet);
    V(chain_ptr); V(chain_cells); V(chain_dir); V(cell_chain); V(cell_chain_pos);
    V(node_kind); V(node_invert); V(node_area); V(node_can_pond); V(node_vol_off);
    if (wet_case) { V(node_sur_depth); }
    if (std::find(m.node_kind.begin(),m.node_kind.end(),kNodeStorage)!=m.node_kind.end()) {
        V(node_full_depth); V(node_vol_dmax); V(node_vol_atop); V(node_vol_tbl);
    }
    V(node_face_ptr); V(node_face_idx); V(node_face_sign); V(node_face_zb); V(node_vj_face);
#undef V
    for (std::size_t i=0;i<m.geom.size();++i) {
        const auto& g=m.geom[i];
        out<<"section,"<<i<<','<<g.xs.type<<','<<g.y_full<<','<<g.a_crown<<','<<g.t_slot<<','
           <<g.i1_crown<<','<<g.barrels<<','<<g.barrel_scale<<','<<g.r_full<<'\n';
    }
    std::ofstream closure(path.string()+".closures",std::ios::binary);
    for (const auto& g:m.geom) {
        const auto& c=g.closure_tbl;
        // Serialize all semantic members; raw POD bytes contain indeterminate
        // alignment padding and cannot fingerprint separate process runs.
        const double scalar[]={c.y_full,c.dh,c.inv_dh,c.a_crown,c.t_slot,c.i1_crown,c.r_full,
            c.inv_a_crown_n,c.c2,c.c1,c.p0,c.p1};
        closure.write(reinterpret_cast<const char*>(scalar),sizeof(scalar));
        closure.write(reinterpret_cast<const char*>(&c.kind),sizeof(c.kind));
        for (const auto* table:{c.A,c.M,c.I1,c.R})
            closure.write(reinterpret_cast<const char*>(table),sizeof(c.A));
        closure.write(reinterpret_cast<const char*>(c.j_of_a),sizeof(c.j_of_a));
    }
}
double inventory(const S::Problem& p) {
    double volume=0;
    for (int i=0;i<p.mesh->n_cells();++i) volume+=p.mesh->cell_dx[i]*p.old_area[i];
    for (double v:p.old_node_volume) volume+=v;
    return volume;
}
}
int main(int argc,char** argv) {
    if (argc!=9 && argc!=10) return 1;
    const std::string name=argv[1]; const int cells=std::stoi(argv[2]);
    const double speed=std::stod(argv[3]), cap=std::stod(argv[4]), end=std::stod(argv[5]);
    const fs::path out=argv[6]; const int restart=std::stoi(argv[7]); const bool moving=std::stoi(argv[8])!=0;
    const std::string mode=argc==10 ? argv[9] : "full";
    const bool reuse=mode.size()>6 && mode.compare(mode.size()-6,6,"_reuse")==0;
    const auto base_mode=reuse ? mode.substr(0,mode.size()-6) : mode;
    const bool block_mode=base_mode.size()>6 && base_mode.compare(base_mode.size()-6,6,"_block")==0;
    const auto method=block_mode ? base_mode.substr(0,base_mode.size()-6) : base_mode;
    if (method!="full" && method!="condensed" && method!="reduced" && method!="fallback" && method!="tree" && method!="tree_reduced") return 1;
    if (cells<4 || !(speed>0) || !(cap>0) || !(end>0)) return 1;
    fs::create_directories(out);
    const bool storage_case=name.starts_with("storage_");
    const bool wet_case=name.starts_with("wet_");
    auto ch=wet_case ? makeWet(name,cells,speed) : storage_case ? makeStorage(name,cells,speed) : make(name,cells,speed);
    auto p=wet_case ? wetProblem(ch,name,moving) : storage_case ? storageProblem(ch,name,moving) : problem(ch,name,moving);
    const bool volume_records=storage_case || wet_case;
    const double initial=inventory(p);
    std::ofstream state(out/"states.csv"),nodes(out/"nodes.csv"),trace(out/"trace.csv"),receipts(out/"faces.csv");
    for (auto* stream:{&state,&nodes,&trace,&receipts}) *stream<<std::setprecision(17);
    state<<"step,time_s,cell,area_ft2,discharge_cfs,depth_ft,bed_ft,dx_ft\n";
    nodes<<"step,time_s,node,head_ft,fixed,junction_residual_cfs\n";
    receipts<<"step,dt_s,face,mass_cfs,momentum,correction_left,correction_right\n";
    trace<<"step,time_s,dt_s,volume_ft3,external_out_ft3,source_ft3,balance_ft3,scaled_residual,newton,gmres,junction_cfs\n";
    std::ofstream storage;
    if (volume_records) {
        storage.open(out/"storage.csv"); storage<<std::setprecision(17);
        storage<<"step,time_s,node,volume_ft3,residual_ft3,lateral_cfs\n";
        std::ofstream sources(out/"sources.csv"); sources<<std::setprecision(17)<<"kind,index,rate_cfs\n";
        for (int i=0;i<ch.n;++i) sources<<"cell,"<<i<<','<<p.cell_lateral[i]<<'\n';
        for (int j=0;j<ch.mesh.n_nodes();++j) sources<<"node,"<<j<<','<<p.node_lateral[j]<<'\n';
    }
    auto states=[&](int step,double time,const S::Trial& x,const S::Evaluation* e){
        for (int i=0;i<ch.n;++i) state<<step<<','<<time<<','<<i<<','<<p.old_area[i]<<','<<x.discharge[i]<<','
            <<x.depth[i]<<','<<ch.mesh.cell_zb[i]<<','<<ch.mesh.cell_dx[i]<<'\n';
        for (int j=0;j<ch.mesh.n_nodes();++j) nodes<<step<<','<<time<<','<<j<<','<<x.head[j]<<','
            <<std::isfinite(p.fixed_head[j])<<','<<(e ? e->junction[j] : 0)<<'\n';
        if (volume_records) for (int j=0;j<ch.mesh.n_nodes();++j) if (ch.mesh.node_kind[j]==kNodeStorage)
            storage<<step<<','<<time<<','<<j<<','<<p.old_node_volume[j]<<','<<(e ? e->storage[j] : 0)<<','<<p.node_lateral[j]<<'\n';
    };
    mesh(out/"mesh_before.csv",ch.mesh,wet_case);
    states(0,0,trial(p),nullptr);
    S solver; S::Controls controls; controls.linear.restart=restart;
    if (method!="full") controls.method=S::LinearMethod::CondensedThenGmres;
    if (method=="reduced" || method=="tree_reduced") controls.condensation.reduced_gmres_only=true;
    if (method=="tree" || method=="tree_reduced") controls.condensation.tree_interface=true;
    if (method=="fallback") controls.condensation.workspace_limit_bytes=1;
    if (block_mode) controls.preconditioner=S::Preconditioner::BlockIlu0;
    controls.reuse_linear_workspace=reuse;
    std::ofstream linear_log(out/"linear.csv"); linear_log<<std::setprecision(17);
    linear_log<<"step,correction,accepted_step,condensation_status,fallback,eliminated,retained,schur_entries,buffer_bound_bytes,block_pivot_ratio,interface_pivot_ratio,failed_cell,failed_interface_pivot,dense,reduced_gmres,full_true_residual,full_tolerance,reduced_true_residual,gmres_true_residual,gmres_tolerance,gmres_iterations,gmres_status,linear_converged,shifted_pivots,reduced_iterations,reduced_restarts,reduced_status,tree,tree_eliminated,core_scalars,core_entries,unsafe_leaf_pivots,leaf_pivot_ratio,core_true_residual,block_ilu,ilu_blocks,ilu_entries,ilu_failed_block,ilu_min_pivot_ratio,ilu_largest_shift,reduced_block_ilu,reduced_ilu_blocks,reduced_ilu_entries,reduced_ilu_failed_block,reduced_ilu_min_pivot_ratio,reduced_ilu_largest_shift,workspace_enabled,symbolic_reused,symbolic_rebuilt,cache_released,retained_workspace_bytes,krylov_reused,krylov_growths,reduced_workspace_enabled,reduced_symbolic_reused,reduced_symbolic_rebuilt,reduced_cache_released,reduced_retained_workspace_bytes,reduced_krylov_reused,reduced_krylov_growths\n";
    int condensed=0,fallbacks=0,dense_solves=0,reduced_solves=0,max_retained=0,max_eliminated=0;
    std::size_t max_buffers=0,max_schur=0; double solver_wall=0.0;
    double time=0,external=0,source=0,maxbalance=0,maxscaled=0,maxjunction=0,maxq=0,maxhead=0;
    int steps=0,newton=0,gmres=0,shifted=0; bool completed=true; S::Result last;
    const auto initial_area=p.old_area,initial_q=p.old_discharge;
    while (time<end-1.e-13*end) {
        const double dt=std::min(cap,end-time);
        S::Endpoint endpoint;
        const auto started=std::chrono::steady_clock::now();
        last=solver.solve(p,dt,controls,endpoint);
        solver_wall+=std::chrono::duration<double>(std::chrono::steady_clock::now()-started).count();
        for (std::size_t correction=0;correction<last.linear_history.size();++correction) {
            const auto& line=last.linear_history[correction]; const auto& c=line.condensation;
            const bool accepted=c.status==S::CondensationStatus::Converged;
            const double true_r=accepted ? c.full_true_residual : line.gmres.true_residual;
            const double tolerance=accepted ? c.full_tolerance : line.gmres.tolerance;
            const auto buffers=std::max(c.peak_workspace_bytes,line.preconditioner_bytes+line.gmres.workspace_bytes);
            condensed+=accepted; fallbacks+=line.fallback; dense_solves+=c.used_dense; reduced_solves+=c.used_reduced_gmres;
            max_buffers=std::max(max_buffers,buffers); max_schur=std::max(max_schur,c.schur_entries);
            max_retained=std::max(max_retained,c.retained); max_eliminated=std::max(max_eliminated,c.eliminated);
            linear_log<<steps+1<<','<<correction<<','<<last.converged()<<','<<static_cast<int>(c.status)<<','<<line.fallback<<','
                <<c.eliminated<<','<<c.retained<<','<<c.schur_entries<<','<<buffers<<','<<c.minimum_block_pivot_ratio<<','
                <<c.minimum_interface_pivot_ratio<<','<<c.failed_cell<<','<<c.failed_interface_pivot<<','<<c.used_dense<<','
                <<c.used_reduced_gmres<<','<<true_r<<','<<tolerance<<','<<c.reduced_true_residual<<','<<line.gmres.true_residual<<','
                <<line.gmres.tolerance<<','<<line.gmres.iterations<<','<<static_cast<int>(line.gmres.status)<<','<<line.converged()<<','<<line.shifted_pivots<<','<<c.reduced.iterations<<','<<c.reduced.restarts<<','<<static_cast<int>(c.reduced.status)<<','
                <<c.used_tree<<','<<c.tree_eliminated<<','<<c.core_scalars<<','<<c.core_entries<<','<<c.unsafe_leaf_pivots<<','<<c.minimum_leaf_pivot_ratio<<','<<c.core_true_residual<<',';
            for (const auto* ilu:{&line.block_ilu,&c.reduced_block_ilu}) {
                linear_log<<ilu->used<<','<<ilu->blocks<<','<<ilu->entries<<','<<ilu->failed_block<<','<<ilu->minimum_pivot_ratio<<','<<ilu->largest_shift;
                if (ilu==&line.block_ilu) linear_log<<',';
            }
            for (const auto* w:{&line.workspace,&c.reduced_workspace}) {
                const auto& krylov=w==&line.workspace ? line.gmres : c.reduced;
                linear_log<<','<<w->enabled<<','<<w->symbolic_reused<<','<<w->symbolic_rebuilt<<','<<w->cache_released<<','
                    <<w->retained_bytes<<','<<krylov.reused_workspace<<','<<krylov.workspace_growths;
            }
            linear_log<<'\n';
        }
        newton+=last.newton_iterations; gmres+=last.gmres_iterations; shifted+=last.last_linear.shifted_pivots;
        if (!last.converged()) { completed=false; break; }
        S::Evaluation e;
        if (!S::residual(p,endpoint,dt,e)) return 3;
        for (int f=0;f<ch.mesh.n_faces();++f) {
            const auto& r=endpoint.faces[f]; const int j=ch.mesh.face_node[f];
            if (j>=0 && std::isfinite(p.fixed_head[j])) external+=dt*(ch.mesh.face_cl[f]>=0 ? 1 : -1)*r.flux.mass;
            receipts<<steps+1<<','<<dt<<','<<f<<','<<r.flux.mass<<','<<r.flux.mom<<','<<r.correction_left<<','<<r.correction_right<<'\n';
        }
        source+=dt*(std::accumulate(p.cell_lateral.begin(),p.cell_lateral.end(),0.0)+std::accumulate(p.node_lateral.begin(),p.node_lateral.end(),0.0));
        p.old_area=endpoint.area; p.old_discharge=endpoint.discharge; p.initial_head=endpoint.head; p.initial_depth=endpoint.depth;
        if (volume_records) p.old_node_volume=endpoint.node_volume;
        time+=dt; ++steps;
        const double balance=inventory(p)-initial+external-source;
        maxbalance=std::max(maxbalance,std::fabs(balance)/initial); maxscaled=std::max(maxscaled,last.scaled_residual);
        maxjunction=std::max(maxjunction,last.max_junction_residual);
        for (int i=0;i<ch.n;++i) {
            maxq=std::max(maxq,std::fabs(endpoint.discharge[i]));
            maxhead=std::max(maxhead,std::fabs(ch.mesh.cell_zb[i]+endpoint.depth[i]-6));
        }
        trace<<steps<<','<<time<<','<<dt<<','<<inventory(p)<<','<<external<<','<<source<<','<<balance<<','
            <<last.scaled_residual<<','<<last.newton_iterations<<','<<last.gmres_iterations<<','<<last.max_junction_residual<<'\n';
        states(steps,time,endpoint,&e);
    }
    mesh(out/"mesh_after.csv",ch.mesh,wet_case);
    std::size_t rss=0;
#if defined(__unix__) || defined(__APPLE__)
    rusage usage{};
    if (getrusage(RUSAGE_SELF,&usage)==0) {
#if defined(__APPLE__)
        rss=static_cast<std::size_t>(usage.ru_maxrss);
#else
        rss=1024*static_cast<std::size_t>(usage.ru_maxrss);
#endif
    }
#endif
    std::ofstream summary(out/"summary.json"); summary<<std::setprecision(17);
    summary<<"{\n\"case\":\""<<name<<"\",\n\"cells_per_conduit\":"<<cells<<",\n\"cells\":"<<ch.n
        <<",\n\"celerity_ft_s\":"<<speed<<",\n\"dt_cap_s\":"<<cap<<",\n\"restart\":"<<restart
        <<",\n\"moving\":"<<(moving ? "true" : "false")<<",\n\"completed\":"<<(completed ? "true" : "false")
        <<",\n\"reached_s\":"<<time<<",\n\"steps\":"<<steps<<",\n\"newton\":"<<newton<<",\n\"gmres\":"<<gmres
        <<",\n\"shifted_pivots_last_correction_sum\":"<<shifted<<",\n\"status\":"<<static_cast<int>(last.status)
        <<",\n\"last_linear_status\":"<<static_cast<int>(last.last_linear.gmres.status)
        <<",\n\"max_relative_balance\":"<<maxbalance<<",\n\"max_scaled_residual\":"<<maxscaled
        <<",\n\"max_junction_cfs\":"<<maxjunction<<",\n\"max_discharge_cfs\":"<<maxq<<",\n\"max_head_departure_ft\":"<<maxhead
        <<",\n\"mode\":\""<<mode<<"\",\n\"condensed_solves\":"<<condensed<<",\n\"full_fallbacks\":"<<fallbacks
        <<",\n\"dense_solves\":"<<dense_solves<<",\n\"reduced_gmres_solves\":"<<reduced_solves
        <<",\n\"maximum_retained\":"<<max_retained<<",\n\"maximum_eliminated\":"<<max_eliminated
        <<",\n\"maximum_schur_entries\":"<<max_schur<<",\n\"maximum_linear_buffer_bound_bytes\":"<<max_buffers
        <<",\n\"solver_wall_s\":"<<solver_wall<<",\n\"peak_rss_bytes\":"<<rss<<"\n}\n";
    std::cout<<name<<" reached "<<time<<" status "<<static_cast<int>(last.status)<<" scaled "<<last.scaled_residual<<'\n';
    return completed ? 0 : 2;
}
