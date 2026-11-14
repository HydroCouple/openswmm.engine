// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include "../../unit/engine/fv_test_support.hpp"
#include "hydraulics/fv/ImplicitReachSolver.hpp"

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <numeric>

using namespace fvtest;
using Coupled=ImplicitReachSolver;
namespace fs=std::filesystem;
namespace {
std::ofstream file(const fs::path& path) {
    std::ofstream out(path); out.exceptions(std::ios::badbit|std::ios::failbit);
    out<<std::setprecision(17); return out;
}
void meshFile(const NetworkMeshData& m,const fs::path& path) {
    auto out=file(path);
    // Canonical fingerprint for this flat, one-section fixture family only.
    auto array=[&](const char* name,const auto& values) {
        out<<name<<','<<values.size(); for (auto v:values) out<<','<<+v; out<<'\n';
    };
#define FIELD(name) array(#name,m.name)
    FIELD(cell_dx); FIELD(cell_zb); FIELD(cell_dzdx); FIELD(cell_geom); FIELD(cell_conduit);
    FIELD(cell_face0); FIELD(cell_face1); FIELD(cell_side0); FIELD(cell_side1);
    FIELD(face_cl); FIELD(face_cr); FIELD(face_node); FIELD(face_zb); FIELD(face_dx);
    FIELD(face_dir_l); FIELD(face_dir_r); FIELD(conduit_cell_begin); FIELD(conduit_cell_count);
    FIELD(conduit_rough_factor); FIELD(conduit_roughness); FIELD(face_gate); FIELD(face_culvert);
#undef FIELD
    const auto& g=m.geom[0]; const auto& c=g.closure_tbl;
    out<<"section,"<<g.xs.type<<','<<g.y_full<<','<<g.a_crown<<','<<g.t_slot<<','<<g.y_crown<<','<<g.barrels<<'\n';
    out<<"closure,"<<+c.kind<<','<<c.y_full<<','<<c.dh<<','<<c.a_crown<<','<<c.t_slot<<','<<c.r_full<<'\n';
    for (int i=0;i<=kClosurePanels;++i) out<<"panel,"<<i<<','<<c.A[i]<<','<<c.M[i]<<','<<c.I1[i]<<','<<c.R[i]<<'\n';
}
}
int main(int argc,char** argv) {
    try {
        if (argc!=8 && argc!=9) throw std::invalid_argument("MODE CASE CELLS CELERITY DT_CAP END OUTPUT [RESTART]");
        const std::string mode=argv[1], name=argv[2];
        const int cells=std::stoi(argv[3]);
        const double speed=std::stod(argv[4]), cap=std::stod(argv[5]);
        double end=std::stod(argv[6]); const fs::path out=argv[7]; fs::create_directories(out);
        const int restart=argc==9 ? std::stoi(argv[8]) : 40;
        if ((mode!="coupled" && mode!="gmres" && mode!="jacobi" && mode!="explicit") || (name!="wave" && name!="momentum" && name!="headloss") ||
            cells<4 || cells>8192 || !std::isfinite(speed) || !std::isfinite(cap) || !std::isfinite(end) ||
            !(speed>0 && cap>0 && end>0) || restart<=0) throw std::invalid_argument("unsupported configuration");
        constexpr double length=1200, baseline=6, amplitude=.001, sigma=60, pi=3.14159265358979323846;
        auto ch=makeNodedChannel(circular(3.0),cells,length/cells,[](double){return 0.0;},name=="headloss" ? .013 : 0.0,speed);
        ch.mesh.node_kind.assign(2,kNodeOutfall); seedLevel(ch,baseline);
        const auto& g=ch.mesh.geom[0]; const double a0=kernels::areaOfDepth(g,baseline);
        // The full HLL Saint-Venant operator uses conserved A in c^2=g*A/T,
        // unlike the legacy pressure predictor's capped conveyance area.
        const double c=kernels::celerity(a0,g.t_slot);
        if (name=="wave") end=300/c;
        const double manning=(1.486/.013)*(pi*9/4)*std::pow(.75,2.0/3.0)*std::sqrt(.5/length);
        const double fixed[]={baseline,name=="headloss" ? 5.5 : baseline};
        ch.state.node_head={fixed[0],fixed[1]};
        for (int i=0;i<cells;++i) {
            const double x=(i+.5)*ch.dx;
            if (name=="wave") {
                const double h=amplitude*std::exp(-.5*std::pow((x-300)/sigma,2));
                ch.state.cell_a[i]=kernels::areaOfDepth(g,baseline+h); ch.state.cell_q[i]=c*g.t_slot*h;
            } else if (name=="momentum") ch.state.cell_q[i]=std::cos(2*pi*i/12.0);
            else { ch.state.cell_a[i]=kernels::areaOfDepth(g,6-.5*x/length); ch.state.cell_q[i]=manning; }
        }
        auto opts=defaultOptions(); opts.lts=false; opts.pressurized_implicit=false;
        ExplicitFvSolver explicit_solver; explicit_solver.initialize(ch.mesh,ch.state,opts);
        Coupled implicit_solver;
        Coupled::Controls controls;
        if (mode=="gmres" || mode=="jacobi") controls.linear.method=Coupled::LinearMethod::GmresOnly;
        if (mode=="jacobi") controls.linear.preconditioner=Coupled::Preconditioner::BlockJacobi;
        controls.linear.gmres.restart=restart;
        Coupled::Problem p{&ch.mesh,{}, {},fixed[0],fixed[1],3.01,100};
        FvStepForcing forcing; forcing.node_fixed_head=fixed; forcing.n_nodes=2;
        const auto initial_q=ch.state.cell_q;
        const double initial_volume=totalVolume(ch);
        meshFile(ch.mesh,out/"mesh_before.csv");
        auto states=file(out/"states.csv"), trace=file(out/"trace.csv");
        states<<"time_s,cell,x_ft,head_ft,area_ft2,discharge_cfs\n";
        trace<<"time_s,volume_ft3,boundary_out_ft3,balance_ft3,scaled_residual,newton_iterations\n";
        double t=0,boundary=0,max_balance=0,max_residual=0,linear_residual=0;
        long steps=0,newton=0,backtracks=0;
        long gmres_iterations=0,gmres_solves=0,fallbacks=0;
        std::size_t workspace=0;
        Coupled::Result last_result;
        auto write=[&] { for (int i=0;i<cells;++i) states<<t<<','<<i<<','<<(i+.5)*ch.dx<<','<<ch.state.cell_h[i]<<','<<ch.state.cell_a[i]<<','<<ch.state.cell_q[i]<<'\n'; };
        write();
        bool completed=true; int failure_status=-1;
        while (t<end) {
            const double target=std::min(end,t+cap),dt=target-t;
            if (!(dt>0)) throw std::runtime_error("timestep cannot advance floating-point time");
            double scaled=0; int iterations=0;
            if (mode!="explicit") {
                p.old_area=ch.state.cell_a; p.old_discharge=ch.state.cell_q;
                Coupled::Endpoint endpoint; const auto result=implicit_solver.solve(p,dt,controls,endpoint);
                last_result=result;
                failure_status=static_cast<int>(result.status);
                newton+=result.newton_iterations; backtracks+=result.backtracks;
                gmres_iterations+=result.gmres_iterations; gmres_solves+=result.gmres_solves;
                fallbacks+=result.linear_fallbacks; workspace=std::max(workspace,result.max_gmres_workspace_bytes);
                if (!result.converged()) { completed=false; break; }
                boundary+=dt*(endpoint.faces.back().mass-endpoint.faces.front().mass);
                ch.state.cell_a=std::move(endpoint.area); ch.state.cell_q=std::move(endpoint.discharge); ch.state.cell_h=std::move(endpoint.depth);
                scaled=result.scaled_residual; iterations=result.newton_iterations;
                linear_residual=std::max(linear_residual,result.linear_relative_residual); ++steps;
            } else {
                if (explicit_solver.advance(t,target,forcing)!=target) { completed=false; break; }
                for (double v:explicit_solver.node_exchange()) boundary+=v;
                INetworkSolver::Divergence divergence;
                if (explicit_solver.divergence(divergence)) { completed=false; break; }
                steps+=explicit_solver.last_num_steps();
            }
            t=target;
            const double balance=totalVolume(ch)-initial_volume+boundary;
            max_balance=std::max(max_balance,std::fabs(balance)); max_residual=std::max(max_residual,scaled);
            trace<<t<<','<<totalVolume(ch)<<','<<boundary<<','<<balance<<','<<scaled<<','<<iterations<<'\n';
        }
        write(); meshFile(ch.mesh,out/"mesh_after.csv");
        double error=0,norm=0,peak=0,centroid=0,weight=0,change=0,qnorm=0;
        for (int i=0;i<cells;++i) {
            const double x=(i+.5)*ch.dx, h=ch.state.cell_h[i]-baseline;
            const double ref=amplitude*std::exp(-.5*std::pow((x-300-c*t)/sigma,2));
            error+=std::pow(h-ref,2); norm+=ref*ref; peak=std::max(peak,h);
            centroid+=x*std::max(0.0,h); weight+=std::max(0.0,h);
            if (i>=cells/4 && i<3*cells/4) { change+=std::pow(ch.state.cell_q[i]-initial_q[i],2); qnorm+=initial_q[i]*initial_q[i]; }
        }
        auto summary=file(out/"summary.json");
        summary<<"{\"mode\":\""<<mode<<"\",\"case\":\""<<name<<"\",\"cells\":"<<cells
            <<",\"celerity_ft_s\":"<<speed<<",\"effective_speed_ft_s\":"<<c<<",\"dt_cap_s\":"<<cap
            <<",\"reached_s\":"<<t<<",\"requested_end_s\":"<<end<<",\"completed\":"<<(completed ? "true" : "false")
            <<",\"last_status\":"<<failure_status<<",\"internal_steps\":"<<steps<<",\"newton_iterations\":"<<newton
            <<",\"backtracks\":"<<backtracks<<",\"max_scaled_residual\":"<<max_residual<<",\"max_linear_relative_residual\":"<<linear_residual
            <<",\"gmres_restart\":"<<restart<<",\"gmres_iterations\":"<<gmres_iterations<<",\"gmres_solves\":"<<gmres_solves
            <<",\"linear_fallbacks\":"<<fallbacks<<",\"max_gmres_workspace_bytes\":"<<workspace
            <<",\"last_gmres_status\":"<<static_cast<int>(last_result.last_linear.gmres.status)
            <<",\"last_gmres_true_residual\":"<<last_result.last_linear.gmres.true_residual
            <<",\"last_gmres_tolerance\":"<<last_result.last_linear.gmres.tolerance
            <<",\"last_nonlinear_scaled_residual\":"<<last_result.scaled_residual
            <<",\"max_relative_balance\":"<<max_balance/initial_volume;
        if (name=="wave") summary<<",\"wave_relative_rms\":"<<std::sqrt(error/norm)<<",\"peak_ratio\":"<<peak/amplitude
            <<",\"speed_relative_error\":"<<(t>0 && weight>0 ? std::fabs((centroid/weight-300)/t-c)/c : -1);
        if (name=="momentum") summary<<",\"relative_q_change\":"<<std::sqrt(change/qnorm);
        if (name=="headloss") summary<<",\"relative_manning_q_error\":"<<std::fabs(std::accumulate(ch.state.cell_q.begin(),ch.state.cell_q.end(),0.0)/cells-manning)/manning;
        summary<<"}\n";
        return completed ? 0 : 2;
    } catch (const std::exception& e) { std::cerr<<e.what()<<'\n'; return 1; }
}
