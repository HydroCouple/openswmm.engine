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
    static double speed(const ExplicitInertialSolver& s,int i) {
        const double h = s.state_->depth[i];
        return h > s.opts_->dry_depth ? std::hypot(s.qcx_[i],s.qcy_[i])/h : 0.0;
    }
    static void fireSwe(ExplicitInertialSolver& s,double dt) {s.fireFacesSwe(s.active_faces_,dt,true);}
    static void rebuild(ExplicitInertialSolver& s) { s.syncAndRebuild(0); }
    static void refresh(ExplicitInertialSolver& s) { s.refreshDt0(); }
    static int tier(const ExplicitInertialSolver& s,int i) {return s.tier_[i];}
    static double step(const ExplicitInertialSolver& s,int i) {return s.dt0_*(1<<s.tier_[i]);}
    static double length(const ExplicitInertialSolver& s,int i) {return s.edges_.cell_lchar[i];}
    static void rebuildAt(ExplicitInertialSolver& s,double t) {s.syncAndRebuild(t);}
    static const InertialEdges& edges(const ExplicitInertialSolver& s) {return s.edges_;}
    static const std::vector<int>& faces(const ExplicitInertialSolver& s) {return s.active_faces_;}
    static bool active(const ExplicitInertialSolver& s,int i) {return s.cell_active_[i]!=0;}
    static double discharge(const ExplicitInertialSolver& s,int e) {return s.q_[e];}
    static void markActiveFaces(ExplicitInertialSolver& s) {
        for(int e:s.active_faces_)s.q_[e]=e+1.;
    }
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
    for(bool quad:{false,true})for(int order:{1,2})for(auto closure:{CellClosure2D::FLAT,CellClosure2D::VFR}){
        auto m=grid(16,quad);
        for(int i=0;i<m.n_vertices();++i){double x=m.vx[i]-8;m.vz[i]=std::abs(x)<2?.2-.05*x*x:0;}
        buildMeshTopology(m);auto s=state(m,0);auto o=options(4,1);o.momentum=Momentum2D::FULL_SWE;o.reconstruction_order=order;o.cell_closure=closure;o.dry_depth=1e-7;o.h_move=1e-6;
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

TEST(Cpu2DCorrectness,InactiveRainLandingSeedsFlowAndClosesSpeciesBudget) {
    for(bool quad:{false,true})for(int mode:{0,1,2})for(int threads:{1,4}){
        auto m=grid(16,quad);auto s=state(m,0);auto o=options(threads,4);
        o.momentum=static_cast<Momentum2D>(mode);
        s.transport.resize(1,m.n_cells(),0);s.transport.rain_conc={3};
        ExplicitInertialSolver solver;solver.initialize(m,s,o);
        solver.advance(0,2);EXPECT_EQ(solver.run_stats().nrhs,0);
        double rain_rate=0;
        for(int i=0;i<m.n_cells();++i)if(m.tri_cx[i]<1){
            s.rainfall[i]=.01;rain_rate+=.01*m.tri_area[i];
        }
        // Land lazy sources directly at a rebuild, the path fused with seed
        // selection. Newly wetted cells must be eligible in this same rebuild.
        ExplicitInertialSolverTestAccess::rebuildAt(solver,3);
        EXPECT_NEAR(sum(s.volume),rain_rate,1e-12);
        EXPECT_FALSE(ExplicitInertialSolverTestAccess::faces(solver).empty());
        for(int i=0;i<m.n_cells();++i)if(m.tri_cx[i]<1)
            EXPECT_NEAR(s.depth[i],.01,1e-12);
        solver.advance(3,4);EXPECT_GT(solver.run_stats().nrhs,0);
        std::fill(s.rainfall.begin(),s.rainfall.end(),0);
        std::fill(s.infil_rate.begin(),s.infil_rate.end(),.02);
        for(int t=4;t<9;++t){
            solver.advance(t,t+1);
            EXPECT_NEAR(sum(s.volume)+infiltration(m,s),2*rain_rate,1e-10);
            EXPECT_NEAR(sum(s.transport.cell_mass)+s.transport.lost_infiltration[0],6*rain_rate,1e-10);
            EXPECT_GE(*std::min_element(s.volume.begin(),s.volume.end()),0);
        }
        EXPECT_LT(sum(s.volume),rain_rate);
    }
}

TEST(Cpu2DCorrectness,SparseRebuildRetiresMomentumAndOrdersFaces) {
    using A=ExplicitInertialSolverTestAccess;
    for(bool quad:{false,true})for(int mode:{0,1,2}){
        auto m=grid(16,quad);auto s=state(m,.002);auto o=options(4,4);
        o.momentum=static_cast<Momentum2D>(mode);
        std::fill(m.tri_init_u.begin(),m.tri_init_u.end(),.25);
        ExplicitInertialSolver solver;solver.initialize(m,s,o);A::rebuild(solver);
        // Optional initial velocity may seed q before any face list exists.
        // A first rebuild must clear that momentum on inactive faces too.
        EXPECT_TRUE(A::faces(solver).empty());
        for(int e=0;e<A::edges(solver).ne;++e)EXPECT_EQ(A::discharge(solver,e),0);
        for(int i=0;i<m.n_cells();++i)s.volume[i]=.1*m.tri_area[i];
        solver.resyncFromVolumes(0);A::rebuild(solver);A::markActiveFaces(solver);
        // Retain a small wet patch, then dry everything, then rewet elsewhere.
        // resync keeps face momentum: rebuild must retire it as cells dry.
        for(int stage=0;stage<3;++stage){
            for(int i=0;i<m.n_cells();++i){
                const bool wet=stage==0 ? m.tri_cx[i]<1 : stage==2 && m.tri_cx[i]>15;
                s.volume[i]=wet?.1*m.tri_area[i]:0;
            }
            solver.resyncFromVolumes(0);A::rebuild(solver);
            const auto& ed=A::edges(solver);std::vector<int> expected;
            for(int e=0;e<ed.ne;++e){
                if(A::active(solver,ed.cL[e])&&A::active(solver,ed.cR[e]))expected.push_back(e);
                else EXPECT_EQ(A::discharge(solver,e),0);
                if(stage>0)EXPECT_EQ(A::discharge(solver,e),0);
            }
            EXPECT_EQ(A::faces(solver),expected);
            if(stage==1)EXPECT_TRUE(expected.empty());else EXPECT_FALSE(expected.empty());
        }
    }
}

TEST(Cpu2DCorrectness,ManySpeciesKeepIndependentBudgetsOnMixedWetDryMesh) {
    // Alternating quads and split squares give different CSR row lengths.
    // Distinct tracer values, including a signed row, expose cross-row mixing.
    constexpr int n = 8;
    MeshData m;
    m.resize_vertices((n + 1) * (n + 1));
    for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
        const int v = y * (n + 1) + x;
        m.vx[v] = x; m.vy[v] = y;
    }
    m.resize_triangles(3 * n * n / 2);
    int cell = 0;
    for (int y = 0; y < n; ++y) for (int x = 0; x < n; ++x) {
        const int a = y * (n + 1) + x, b = a + 1, d = a + n + 1, e = d + 1;
        if ((x + y) % 2 == 0) m.set_quad(cell++, a, b, e, d);
        else { m.set_triangle(cell++, a, b, e); m.set_triangle(cell++, a, e, d); }
    }
    buildMeshTopology(m);
    for (int ns : {1, 7, 8, 9, 17}) for (int threads : {1, 4}) for (int mode : {0, 1, 2}) {
        SCOPED_TRACE(::testing::Message() << "species=" << ns << " threads=" << threads << " mode=" << mode);
        auto s = state(m, 0);
        auto o = options(threads, 4);
        o.momentum = static_cast<Momentum2D>(mode);
        s.transport.resize(ns, m.n_cells(), 0);
        s.transport.temp_row = ns - 1;
        s.transport.rain_conc.resize(ns);
        for (int r = 0; r < ns; ++r) s.transport.rain_conc[r] = r == ns - 1 ? -4.0 : .7 * (r + 1);
        for (int i = 0; i < m.n_cells(); ++i) {
            s.volume[i] = m.tri_cx[i] < n / 2 ? .2 * m.tri_area[i] : 0;
            s.rainfall[i] = .005; s.infil_rate[i] = .001;
            for (int r = 0; r < ns; ++r)
                s.transport.cell_mass[s.transport.idx(r, i)] = s.transport.rain_conc[r] * s.volume[i];
        }
        const double initial_volume = sum(s.volume);
        ExplicitInertialSolver solver;
        solver.initialize(m, s, o);
        double time = 0;
        for (double next : {.2, .6, 1.4, 2.0}) {
            solver.advance(time, next);
            // Explicit resync also settles any outstanding per-face bookings.
            solver.resyncFromVolumes(next);
            EXPECT_NEAR(sum(s.volume) + infiltration(m, s), initial_volume + n * n * .005 * next, 1e-10);
            for (int r = 0; r < ns; ++r) {
                double mass = 0;
                const double c = s.transport.rain_conc[r];
                for (int i = 0; i < m.n_cells(); ++i) {
                    const double mi = s.transport.cell_mass[s.transport.idx(r, i)];
                    mass += mi;
                    EXPECT_NEAR(mi, c * s.volume[i], 1e-11);
                }
                EXPECT_NEAR(mass + s.transport.lost_infiltration[r] - s.transport.gained_rainfall[r], c * initial_volume, 1e-9);
            }
            time = next;
        }
    }
}

TEST(Cpu2DCorrectness,ManySpeciesTransfersConserveSurfaceAndGroundwaterMass) {
    for (int ns : {1, 9, 17}) for (int threads : {1, 4}) for (bool quad : {false, true}) {
        SCOPED_TRACE(::testing::Message() << "species=" << ns << " threads=" << threads << " quad=" << quad);
        auto m = grid(8, quad); auto s = state(m, .1); auto o = options(threads, 4);
        InertialEdges ed; ed.build(m);
        SubsurfaceSolver gw; SubsurfaceConfig cfg; GwAquiferRow aquifer;
        aquifer.hg0 = .5; aquifer.Ks = 1e-6; cfg.rows.push_back(aquifer);
        std::vector<std::string> warnings;
        ASSERT_TRUE(gw.initialize(m, ed, o, GwUnitFactors{}, 0, cfg, warnings).empty());
        RowLayoutLite layout; layout.n_species = layout.n_pollut = ns;
        for (int r = 0; r < ns; ++r) layout.names.push_back("Tracer" + std::to_string(r));
        gw.initTransport(layout, nullptr, {}, warnings);
        s.transport.resize(ns, m.n_cells(), 0); std::vector<double> initial(ns, 0.0);
        for (int i = 0; i < m.n_cells(); ++i) {
            s.infil_rate[i] = .001;
            for (int r = 0; r < ns; ++r) {
                const double mass = (1 + r + i % 5) * s.volume[i];
                s.transport.cell_mass[s.transport.idx(r, i)] = mass; initial[r] += mass;
            }
        }
        ExplicitInertialSolver solver; solver.setSubsurface(&gw); solver.initialize(m, s, o);
        for (int t = 0; t < 4; ++t) {
            solver.advance(t, t + 1);
            for (int r = 0; r < ns; ++r) {
                double surface = 0;
                for (int i = 0; i < m.n_cells(); ++i) surface += s.transport.cell_mass[s.transport.idx(r, i)];
                EXPECT_NEAR(surface + gw.transport().storage(r), initial[r], 1e-8);
                EXPECT_NEAR(s.transport.lost_infiltration[r], gw.transport().storage(r), 1e-8);
            }
        }
    }
}

TEST(Cpu2DCorrectness,SpeciesBudgetsSurviveChangingRainCouplingAndEvaporation) {
    for (int ns : {3, 9, 17}) for (int threads : {1, 4}) for (int mode : {0, 1, 2}) {
        SCOPED_TRACE(::testing::Message() << "species=" << ns << " threads=" << threads << " mode=" << mode);
        auto m = grid(8); auto s = state(m, .05); auto o = options(threads, 4);
        o.momentum = static_cast<Momentum2D>(mode);
        auto& tr = s.transport; tr.resize(ns, m.n_cells(), 0);
        tr.age_row = ns - 2; tr.temp_row = ns - 1;
        tr.rain_conc.resize(ns); tr.coupling_src.assign(ns * m.n_cells(), 0.0);
        for (int r = 0; r < ns; ++r) tr.rain_conc[r] = r == tr.temp_row ? -4.0 : r + 1.0;
        for (int i = 0; i < m.n_cells(); ++i) for (int r = 0; r < ns; ++r)
            tr.cell_mass[tr.idx(r, i)] = tr.rain_conc[r] * s.volume[i];
        const double initial_volume = sum(s.volume); double rain_volume = 0;
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        for (int t = 0; t < 8; ++t) {
            const int phase = t % 4;
            for (int i = 0; i < m.n_cells(); ++i) {
                s.rainfall[i] = (phase == 0 || phase == 2) ? .004 : 0;
                s.infil_rate[i] = .001;
                s.evap_rate[i] = phase == 3 ? .002 : 0;
                s.coupling_flux[i] = phase == 1 ? (m.tri_cx[i] < 4 ? .002 : -.002) : 0;
                rain_volume += s.rainfall[i] * m.tri_area[i];
                for (int r = 0; r < ns; ++r)
                    tr.coupling_src[tr.idx(r, i)] = std::max(0.0, s.coupling_flux[i]) * tr.rain_conc[r];
            }
            solver.advance(t, t + 1);
            EXPECT_NEAR(sum(s.volume) + infiltration(m, s) + s.evap_loss_total - sum(s.coupling_applied),
                        initial_volume + rain_volume, 1e-9);
            for (int r = 0; r < ns; ++r) {
                double mass = 0;
                for (int i = 0; i < m.n_cells(); ++i) {
                    const double mi = tr.cell_mass[tr.idx(r, i)]; mass += mi;
                    if (r == tr.age_row || r == tr.temp_row)
                        EXPECT_NEAR(mi, tr.rain_conc[r] * s.volume[i], 1e-9);
                }
                if (r != tr.age_row && r != tr.temp_row)
                    EXPECT_NEAR(mass + tr.lost_infiltration[r] + tr.lost_coupling[r]
                        - tr.gained_rainfall[r] - tr.gained_coupling[r],
                        tr.rain_conc[r] * initial_volume, 1e-8);
            }
        }
    }
}

TEST(Cpu2DCorrectness,SecondOrderSlopeReconstructionCarriesPhysicalMassFlux) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true}) for (int threads : {1, 4}) {
        auto m = grid(8, quad);
        for (int i = 0; i < m.n_vertices(); ++i)
            m.vz[i] = .04 * m.vx[i] + .03 * m.vy[i];
        buildMeshTopology(m);
        auto s = state(m, .5); auto o = options(threads, 1);
        o.momentum = Momentum2D::FULL_SWE; o.reconstruction_order = 2;
        for (int i = 0; i < m.n_cells(); ++i) {
            m.tri_init_u[i] = .2; m.tri_init_v[i] = -.1; m.mannings_n[i] = 0;
        }
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        A::rebuild(solver); A::fireSwe(solver, 1e-4);
        const auto& ed = A::edges(solver); int checked = 0;
        for (int e : A::faces(solver)) {
            const int a = ed.cL[e], b = ed.cR[e];
            auto interior = [&](int c) {
                return m.tri_cx[c] > 2 && m.tri_cx[c] < 6 &&
                       m.tri_cy[c] > 2 && m.tri_cy[c] < 6;
            };
            if (!interior(a) || !interior(b)) continue;
            // Both reconstructed states have h=.5 and the same velocity.
            // A smooth bed slope must not lower the advected water depth.
            EXPECT_NEAR(A::discharge(solver, e), .5 * (.2 * ed.nx[e] - .1 * ed.ny[e]), 1e-13);
            ++checked;
        }
        EXPECT_GT(checked, 10);
    }
}

TEST(Cpu2DCorrectness,SecondOrderPlanarThackerRetainsRotationPhase) {
    constexpr double pi = 3.14159265358979323846;
    const double omega = std::sqrt(2 * inertial::kGravity * .1);
    const double period = 2 * pi / omega;
    for (bool quad : {false, true}) {
        auto m = grid(64, quad);
        for (int i = 0; i < m.n_vertices(); ++i) {
            m.vx[i] /= 16; m.vy[i] /= 16;
            const double x = m.vx[i] - 2, y = m.vy[i] - 2;
            m.vz[i] = .1 * (x * x + y * y - 1);
        }
        buildMeshTopology(m); auto s = state(m, 0); auto o = options(1, 1);
        o.momentum = Momentum2D::FULL_SWE; o.reconstruction_order = 2;
        o.max_timestep = .25; o.dry_depth = 1e-7; o.h_move = 1e-6;
        for (int i = 0; i < m.n_cells(); ++i) {
            const double x = m.tri_cx[i] - 2.5, y = m.tri_cy[i] - 2;
            s.volume[i] = std::max(0.0, .1 * (1 - x*x - y*y)) * m.tri_area[i];
            m.tri_init_v[i] = .5 * omega; m.mannings_n[i] = 0;
        }
        const double initial_volume = sum(s.volume);
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        double max_speed = 0;
        for (int k = 0; k < 192; ++k) {
            solver.advance(k * period / 64, (k + 1) * period / 64);
            for (int i = 0; i < m.n_cells(); ++i) {
                const double speed = ExplicitInertialSolverTestAccess::speed(solver, i);
                ASSERT_TRUE(std::isfinite(speed));
                max_speed = std::max(max_speed, speed);
            }
        }
        // The analytical velocity is uniform, with magnitude .5*omega.
        // Allow shoreline truncation error, but catch thin-film acceleration
        // even when total volume, centroid phase and depth error look good.
        EXPECT_LT(max_speed, 5 * .5 * omega) << "quad=" << quad;
        double x = 0, y = 0, depth_error = 0, reference_volume = 0;
        for (int i = 0; i < m.n_cells(); ++i) {
            x += s.volume[i] * (m.tri_cx[i] - 2);
            y += s.volume[i] * (m.tri_cy[i] - 2);
            EXPECT_GE(s.volume[i], 0);
            const double dx=m.tri_cx[i]-2.5, dy=m.tri_cy[i]-2;
            const double exact_h=std::max(0.0,.1*(1-dx*dx-dy*dy));
            depth_error += std::abs(s.depth[i]-exact_h)*m.tri_area[i];
            reference_volume += exact_h*m.tri_area[i];
        }
        EXPECT_NEAR(sum(s.volume), initial_volume, 1e-12);
        // Exact centroid returns to (+.5, 0). Allow spatial damping while
        // rejecting the former ~30-degree phase lag after three periods.
        EXPECT_GT(x / initial_volume, .3);
        EXPECT_LT(std::abs(std::atan2(y, x)), .15);
        EXPECT_LT(depth_error/reference_volume, .18);
    }
}


TEST(Cpu2DCorrectness,SecondOrderDistortedThackerKeepsShorelineVelocityBounded) {
    constexpr int n = 32;
    constexpr double dx = 4.0 / n, pi = 3.14159265358979323846;
    const double omega = std::sqrt(8 * inertial::kGravity * .1);
    const double period = 2 * pi / omega, amplitude = (1-.8*.8)/(1+.8*.8);
    const double root = std::sqrt(1-amplitude*amplitude);
    // Maximize the analytical radial speed over the wet disk and a period.
    const double c = (1-root)/amplitude;
    const double exact_max_speed = .5*omega*amplitude*
        std::sqrt((1-c*c)/((1-amplitude*c)*root));
    for (int shape : {0, 1, 2}) { // triangles, quadrilaterals, mixed cells
        SCOPED_TRACE(shape);
        MeshData m; m.resize_vertices((n+1)*(n+1));
        for (int y = 0; y <= n; ++y) for (int x = 0; x <= n; ++x) {
            const int i = y*(n+1)+x;
            const bool inner = x>0 && x<n && y>0 && y<n;
            m.vx[i] = x*dx + (inner ? .15*dx*std::sin(.7*x+1.1*y) : 0);
            m.vy[i] = y*dx + (inner ? .15*dx*std::cos(1.3*x-.4*y) : 0);
            const double xc = m.vx[i]-2, yc = m.vy[i]-2;
            m.vz[i] = .1*(xc*xc+yc*yc-1);
        }
        int nc = 0;
        for (int y=0; y<n; ++y) for (int x=0; x<n; ++x)
            nc += (shape==1 || (shape==2 && (x+y)%2)) ? 1 : 2;
        m.resize_triangles(nc); int cell=0;
        for (int y=0; y<n; ++y) for (int x=0; x<n; ++x) {
            const int a=y*(n+1)+x, b=a+1, c=a+n+1, d=c+1;
            if (shape==1 || (shape==2 && (x+y)%2)) m.set_quad(cell++,a,b,d,c);
            else {m.set_triangle(cell++,a,b,d); m.set_triangle(cell++,a,d,c);}
        }
        buildMeshTopology(m); ASSERT_TRUE(validateMesh(m).empty());
        auto s = state(m,0); auto o = options(1,1);
        o.momentum=Momentum2D::FULL_SWE; o.reconstruction_order=2;
        o.max_timestep=.25; o.dry_depth=1e-7; o.h_move=1e-6;
        const double scale = root/(1-amplitude);
        for (int i=0; i<m.n_cells(); ++i) {
            const double x=m.tri_cx[i]-2, y=m.tri_cy[i]-2;
            s.volume[i]=std::max(0.0,.1*(scale-(x*x+y*y)*scale*scale))*m.tri_area[i];
            m.mannings_n[i]=0;
        }
        const double initial_volume=sum(s.volume);
        ExplicitInertialSolver solver; solver.initialize(m,s,o);
        double max_speed=0;
        for (int k=0; k<192; ++k) {
            solver.advance(k*period/64,(k+1)*period/64);
            for (int i=0; i<m.n_cells(); ++i) {
                const double speed=ExplicitInertialSolverTestAccess::speed(solver,i);
                ASSERT_TRUE(std::isfinite(speed));
                ASSERT_GE(s.volume[i],0);
                max_speed=std::max(max_speed,speed);
            }
        }
        EXPECT_NEAR(sum(s.volume),initial_volume,1e-12);
        // Generous truncation allowance: the rejected shoreline fit reached
        // 18–52 m/s here despite good depth error and exact conservation.
        EXPECT_LT(max_speed,5*exact_max_speed);
    }
}


TEST(Cpu2DCorrectness,ReconstructedBedPressureUsesTheCellToFaceDepthIntegral) {
    // For a linear profile, the bed-source integral is -g*mean(h)*delta(z).
    // Both sides agree at the face, so the hydrostatic correction is zero.
    swe::FaceFlux f; double lx,ly,rx,ry;
    const double h_cell=2, h_face=2.2, z_face=.1, eta_face=h_face+z_face;
    ASSERT_TRUE(swe::faceFluxReconBed(eta_face,0,0,0,h_cell,
        eta_face,0,0,z_face,h_face,1,0,1e-7,f,lx,ly,rx,ry,z_face,z_face));
    EXPECT_NEAR(f.mass,0,1e-14);
    EXPECT_NEAR(lx,-inertial::kGravity*.5*(h_cell+h_face)*z_face,1e-14);
    EXPECT_EQ(ly,0); EXPECT_NEAR(rx,0,1e-14); EXPECT_EQ(ry,0);
    // A bounded positive depth polynomial cannot produce a bed-force per
    // unit water volume that diverges as the cell dries.
    for(double h : {1e-4,1e-6,1e-8}) {
        ASSERT_TRUE(swe::faceFluxReconBed(.01+2*h,0,0,0,h,
            .1,0,0,.1,0,1,0,1e-10,f,lx,ly,rx,ry,.01,.1));
        EXPECT_EQ(f.mass,0); EXPECT_EQ(f.mx,0);
        EXPECT_LT(std::abs(lx)/h,.02*inertial::kGravity);
    }
}

TEST(Cpu2DCorrectness,SecondOrderShorelineKeepsConnectedWetFaceAccuracy) {
    using A = ExplicitInertialSolverTestAccess;
    for (bool quad : {false, true}) for (int threads : {1, 4}) {
        auto m = grid(8, quad);
        for (int i = 0; i < m.n_vertices(); ++i)
            m.vz[i] = .04 * m.vx[i] + .03 * m.vy[i];
        buildMeshTopology(m); auto s = state(m, 0); auto o = options(threads, 1);
        o.momentum = Momentum2D::FULL_SWE; o.reconstruction_order = 2;
        for (int i = 0; i < m.n_cells(); ++i) {
            s.volume[i] = m.tri_cx[i] < 4 ? .5 * m.tri_area[i] : 0;
            m.tri_init_u[i] = .2; m.tri_init_v[i] = -.1; m.mannings_n[i] = 0;
        }
        ExplicitInertialSolver solver; solver.initialize(m, s, o);
        A::rebuild(solver); A::fireSwe(solver, 1e-4);
        const auto& ed = A::edges(solver); int checked = 0;
        auto shore = [&](int c) {
            for (int p = ed.cell_ptr[c]; p < ed.cell_ptr[c+1]; ++p) {
                const int e = ed.cell_edge[p], j = ed.cL[e] == c ? ed.cR[e] : ed.cL[e];
                if (s.depth[j] == 0) return true;
            }
            return false;
        };
        auto interior = [&](int c) {
            return m.tri_cx[c] > 1 && m.tri_cx[c] < 7 &&
                   m.tri_cy[c] > 1 && m.tri_cy[c] < 7;
        };
        for (int e : A::faces(solver)) {
            const int a = ed.cL[e], b = ed.cR[e];
            if (!interior(a) || !interior(b) || s.depth[a] == 0 || s.depth[b] == 0) continue;
            if (!shore(a) && !shore(b)) continue;
            // The dry neighbor must not suppress a linear, fully wet face
            // reconstruction when two independent wet directions remain.
            EXPECT_NEAR(A::discharge(solver,e), .5*(.2*ed.nx[e]-.1*ed.ny[e]), 1e-13);
            ++checked;
        }
        EXPECT_GT(checked, 3);
    }
}
