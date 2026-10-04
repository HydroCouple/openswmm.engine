// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include "2d/solver/SweKernels.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
using namespace openswmm::twoD;
namespace openswmm::twoD {
// Inspect the scheduling invariant without making debug state a public API.
struct ExplicitInertialSolverTestAccess {
    static void rebuild(ExplicitInertialSolver& s) { s.syncAndRebuild(0); }
    static void refresh(ExplicitInertialSolver& s) { s.refreshDt0(); }
    static int tier(const ExplicitInertialSolver& s,int i) {return s.tier_[i];}
    static double step(const ExplicitInertialSolver& s,int i) {return s.dt0_*(1<<s.tier_[i]);}
    static double length(const ExplicitInertialSolver& s,int i) {return s.edges_.cell_lchar[i];}
};
}
namespace {
MeshData grid(int n,bool quad=false) {
    MeshData m;m.resize_vertices((n+1)*(n+1));
    for(int y=0;y<=n;++y)for(int x=0;x<=n;++x){int v=y*(n+1)+x;m.vx[v]=x;m.vy[v]=y;}
    m.resize_triangles((quad?1:2)*n*n);int c=0;
    for(int y=0;y<n;++y)for(int x=0;x<n;++x){int a=y*(n+1)+x,b=a+1,d=a+n+1,e=d+1;if(quad)m.set_quad(c++,a,b,e,d);else{m.set_triangle(c++,a,b,e);m.set_triangle(c++,a,e,d);}}
    buildMeshTopology(m);return m;
}
double sum(const std::vector<double>& x){return std::accumulate(x.begin(),x.end(),0.);}
double infiltration(const MeshData&m,const SurfaceStateData&s){double v=0;for(int i=0;i<m.n_cells();++i)v+=s.infil_applied[i]*m.tri_area[i];return v;}
SurfaceStateData state(const MeshData&m,double h){SurfaceStateData s;s.resize(m.n_cells(),m.n_vertices());for(int i=0;i<m.n_cells();++i)s.volume[i]=h*m.tri_area[i];return s;}
SolverOptions2D options(int threads=1,int tiers=1){SolverOptions2D o;o.num_threads=threads;o.lts_tiers=tiers;o.max_timestep=1;o.cfl_number=.4;return o;}
}
TEST(Cpu2DCorrectness,FrozenCoarseTierRespectsRefreshedCfl) {
    auto m=grid(16);auto s=state(m,.0041);auto o=options(4,4);
    for(int i=0;i<m.n_cells();++i)if(m.tri_cx[i]<4)s.volume[i]=.3*m.tri_area[i];
    ExplicitInertialSolver solver;solver.initialize(m,s,o);using A=ExplicitInertialSolverTestAccess;A::rebuild(solver);
    int coarse=-1;for(int i=0;i<m.n_cells();++i)if(A::tier(solver,i)>0){coarse=i;break;}
    ASSERT_GE(coarse,0);s.depth[coarse]=.2;
    const double bound=inertial::cellCflDt(o.cfl_number,A::length(solver,coarse),.2,0);
    A::refresh(solver);EXPECT_LE(A::step(solver,coarse),bound*(1+1e-14));
}
TEST(Cpu2DCorrectness,MacroLandingPublishesAllConservedState) {
    for(int threads:{1,4}){
        auto m=grid(32);auto s=state(m,0);auto o=options(threads,4);s.transport.resize(1,m.n_cells(),0);
        for(int i=0;i<m.n_cells();++i){s.volume[i]=m.tri_cx[i]<16?.2*m.tri_area[i]:0;s.transport.cell_mass[i]=3*s.volume[i];}
        const double v=sum(s.volume),mass=sum(s.transport.cell_mass);ExplicitInertialSolver solver;solver.initialize(m,s,o);
        // Former exact macro-cycle boundary: storage excluded 3.55% in-flight water.
        double t=.93282250995272253;solver.advance(0,t);
        EXPECT_NEAR(sum(s.volume),v,1e-9);EXPECT_NEAR(sum(s.transport.cell_mass),mass,1e-9);
        solver.advance(t,2*t);EXPECT_NEAR(sum(s.volume),v,1e-9);EXPECT_NEAR(sum(s.transport.cell_mass),mass,1e-9);
        solver.finalize();EXPECT_NEAR(sum(s.volume),v,1e-9);
    }
}
TEST(Cpu2DCorrectness,FlowAndHeldSinksShareAvailableWater) {
    for(bool quad:{false,true})for(int tiers:{1,4})for(int mode:{0,1,2}){
        auto m=grid(16,quad);auto s=state(m,0);auto o=options(4,tiers);o.momentum=static_cast<Momentum2D>(mode);
        for(int i=0;i<m.n_cells();++i){double h=m.tri_cx[i]<8?.2:0;s.volume[i]=h*m.tri_area[i];s.infil_rate[i]=h;}
        double v=sum(s.volume);ExplicitInertialSolver solver;solver.initialize(m,s,o);solver.advance(0,1);
        EXPECT_NEAR(sum(s.volume)+infiltration(m,s),v,1e-9);EXPECT_GE(*std::min_element(s.volume.begin(),s.volume.end()),0);
    }
}
TEST(Cpu2DCorrectness,CompetingSinksBookOnlyAppliedAmounts) {
    for(double h:{.002,.1}){
        auto m=grid(1,true);auto s=state(m,h);auto o=options();s.infil_rate[0]=h*2;s.evap_rate[0]=h*2;s.coupling_flux[0]=-h*2;
        s.transport.resize(3,1,0);s.transport.age_row=1;s.transport.temp_row=2;s.transport.cell_mass={2*h,3*h,-5*h};
        ExplicitInertialSolver solver;solver.initialize(m,s,o);solver.advance(0,1);
        EXPECT_NEAR(sum(s.volume)+infiltration(m,s)+s.evap_loss_total-s.coupling_applied[0],h,1e-13);
        EXPECT_NEAR(infiltration(m,s),h/3,1e-13);EXPECT_NEAR(s.evap_loss_total,h/3,1e-13);
        // Evaporation leaves solute behind, but carries age and signed temperature.
        EXPECT_NEAR(s.transport.cell_mass[0],2*h/3,1e-13);EXPECT_NEAR(s.transport.cell_mass[1],0,1e-13);EXPECT_NEAR(s.transport.cell_mass[2],0,1e-13);
        EXPECT_NEAR(s.transport.lost_infiltration[0],2*h/3,1e-13);
    }
}
TEST(Cpu2DCorrectness,BalancedInactiveWaterStillTransportsRainMass) {
    auto m=grid(1,true);auto s=state(m,.002);auto o=options();s.rainfall[0]=s.infil_rate[0]=.0001;s.transport.resize(1,1,0);s.transport.rain_conc={2};
    ExplicitInertialSolver solver;solver.initialize(m,s,o);solver.advance(0,1);
    EXPECT_NEAR(s.volume[0],.002,1e-14);EXPECT_GT(s.transport.cell_mass[0],0);
    EXPECT_NEAR(s.transport.cell_mass[0]+s.transport.lost_infiltration[0],.0002,1e-14);
}
TEST(Cpu2DCorrectness,ParallelSpeciesAndRunoffLedgersClose) {
    std::vector<double> reference;
    for(int threads:{1,2,4,8})for(int repeat=0;repeat<3;++repeat){
        auto m=grid(32);auto s=state(m,.1);auto o=options(threads,4);s.transport.resize(2,m.n_cells(),0);s.transport.rain_conc={2,7};s.transport.cell_runoff_vol.assign(m.n_cells(),0);
        for(int i=0;i<m.n_cells();++i){s.volume[i]*=1+.2*std::sin(m.tri_cx[i]);s.rainfall[i]=.0001;s.infil_rate[i]=.0002;for(int r=0;r<2;++r)s.transport.cell_mass[s.transport.idx(r,i)]=(r+1)*s.volume[i];}
        double v=sum(s.volume);ExplicitInertialSolver solver;solver.initialize(m,s,o);solver.advance(0,2);
        for(int r=0;r<2;++r){double mass=0;for(int i=0;i<m.n_cells();++i)mass+=s.transport.cell_mass[s.transport.idx(r,i)];EXPECT_NEAR(mass+s.transport.lost_infiltration[r]-(r+1)*v-s.transport.gained_rainfall[r],0,1e-9);}
        EXPECT_NEAR(sum(s.transport.cell_runoff_vol),0,1e-9);
        if(reference.empty())reference=s.transport.cell_runoff_vol;else EXPECT_EQ(s.transport.cell_runoff_vol,reference);
    }
}
TEST(Cpu2DCorrectness,ParallelInfiltrationTransfersCellLocalSpeciesToGroundwater) {
    for(int threads:{1,4,8}){
        auto m=grid(24);auto s=state(m,.1);auto o=options(threads,4);InertialEdges ed;ed.build(m);
        SubsurfaceSolver gw;SubsurfaceConfig cfg;GwAquiferRow row;row.hg0=.5;row.Ks=1e-6;cfg.rows.push_back(row);std::vector<std::string>warnings;
        ASSERT_TRUE(gw.initialize(m,ed,o,GwUnitFactors{},0,cfg,warnings).empty());RowLayoutLite layout;layout.n_species=layout.n_pollut=1;layout.names={"Tracer"};gw.initTransport(layout,nullptr,{},warnings);
        s.transport.resize(1,m.n_cells(),0);for(int i=0;i<m.n_cells();++i){s.infil_rate[i]=.001;s.transport.cell_mass[i]=(1+i%5)*s.volume[i];}
        const double initial=sum(s.transport.cell_mass);ExplicitInertialSolver solver;solver.setSubsurface(&gw);solver.initialize(m,s,o);solver.advance(0,1);
        EXPECT_NEAR(sum(s.transport.cell_mass)+gw.transport().storage(0),initial,1e-9);
        EXPECT_NEAR(s.transport.lost_infiltration[0],gw.transport().storage(0),1e-9);
    }
}
TEST(Cpu2DCorrectness,Rk2EvaporationLedgerMatchesItsAveragedState) {
    auto m=grid(2,true);auto s=state(m,.1);auto o=options();o.momentum=Momentum2D::FULL_SWE;o.reconstruction_order=2;
    std::fill(s.evap_rate.begin(),s.evap_rate.end(),.01);double initial=sum(s.volume);ExplicitInertialSolver solver;solver.initialize(m,s,o);solver.advance(0,1);
    EXPECT_NEAR(sum(s.volume)+s.evap_loss_total,initial,1e-12);
}

TEST(Cpu2DCorrectness,BlockedWetDryFaceRetainsHydrostaticPressure) {
    swe::FaceFlux f;double lx,ly,rx,ry;
    EXPECT_TRUE(swe::faceFlux(.1,.1,0,0,1,0,0,0,1,0,1e-7,f,lx,ly,rx,ry));
    EXPECT_EQ(f.mass,0);EXPECT_EQ(f.mx,0);EXPECT_NEAR(lx,-.5*inertial::kGravity*.01,1e-15);
    EXPECT_EQ(ly,0);EXPECT_EQ(rx,0);EXPECT_EQ(ry,0);
    EXPECT_TRUE(swe::faceFluxRecon(.1,0,0,0,.1,1,0,0,1,0,1,0,1e-7,f,lx,ly,rx,ry));
    EXPECT_EQ(f.mass,0);EXPECT_NEAR(lx,-.5*inertial::kGravity*.01,1e-15);
}
TEST(Cpu2DCorrectness,LakeAtRestAgainstAnEmergedBump) {
    for(bool quad:{false,true})for(int order:{1,2}){
        auto m=grid(16,quad);
        for(int i=0;i<m.n_vertices();++i){double x=m.vx[i]-8;m.vz[i]=std::abs(x)<2?.2-.05*x*x:0;}
        buildMeshTopology(m);auto s=state(m,0);auto o=options(4,1);o.momentum=Momentum2D::FULL_SWE;o.reconstruction_order=order;o.dry_depth=1e-7;o.h_move=1e-6;
        for(int i=0;i<m.n_cells();++i){m.mannings_n[i]=0;s.volume[i]=inertial::cellVolumeFromEta(m,o,i,.1);}
        auto initial=s.volume;ExplicitInertialSolver solver;solver.initialize(m,s,o);solver.advance(0,10);
        for(int i=0;i<m.n_cells();++i)EXPECT_NEAR(s.volume[i],initial[i],1e-10);
    }
}
TEST(Cpu2DCorrectness,SecondOrderThackerDoesNotAccelerateTrappedFilms) {
    auto m=grid(64);
    for(int i=0;i<m.n_vertices();++i){m.vx[i]/=16;m.vy[i]/=16;double x=m.vx[i]-2,y=m.vy[i]-2;m.vz[i]=.1*(x*x+y*y-1);}
    buildMeshTopology(m);auto s=state(m,0);auto o=options();o.momentum=Momentum2D::FULL_SWE;o.reconstruction_order=2;o.dry_depth=1e-7;o.h_move=1e-6;o.max_timestep=.25;
    const double omega=std::sqrt(2*inertial::kGravity*.1),period=2*3.14159265358979323846/omega;
    for(int i=0;i<m.n_cells();++i){double x=m.tri_cx[i]-2.5,y=m.tri_cy[i]-2;s.volume[i]=std::max(0.,.1*(1-x*x-y*y))*m.tri_area[i];m.mannings_n[i]=0;m.tri_init_v[i]=.5*omega;}
    const double initial=sum(s.volume);ExplicitInertialSolver solver;solver.initialize(m,s,o);
    for(double t=0;t<2;){double next=std::min(2.,t+period/16);solver.advance(t,next);t=next;}
    EXPECT_NEAR(sum(s.volume),initial,1e-12);
    // The old wet-cell-only fallback developed >300 m/s in a 1.7 µm film,
    // requiring >23,000 substeps just to reach t=2 on this mesh.
    EXPECT_LT(solver.run_stats().nsteps,10000);
    double err=0,ref=0;
    for(int i=0;i<m.n_cells();++i){double x=m.tri_cx[i]-2-.5*std::cos(2*omega),y=m.tri_cy[i]-2-.5*std::sin(2*omega);double h=std::max(0.,.1*(1-x*x-y*y));err+=std::abs(s.depth[i]-h)*m.tri_area[i];ref+=h*m.tri_area[i];}
    EXPECT_LT(err/ref,.3);
}
