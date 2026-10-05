// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/subsurface/SurfaceExchange.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "core/SWMMEngine.hpp"
#include "core/UnitConversion.hpp"
#include "hydrology/Runoff.hpp"
#include "hydrology/LID.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <chrono>

using namespace openswmm;
using namespace openswmm::twoD;
namespace {
constexpr double ft3 = .3048 * .3048 * .3048;
struct Receiver {
    MeshData mesh; InertialEdges edges; SurfaceStateData surface;
    SolverOptions2D options; SubsurfaceConfig cfg; SubsurfaceSolver gw;
    void init(GwClosure closure = GwClosure::CLOSED_FORM, SoilChar law = SoilChar::RUSSO,
              int cells = 1, double table = 1, double ks = .1) {
        mesh.resize_vertices(cells * 3); mesh.resize_triangles(cells);
        for (int c = 0; c < cells; ++c) {
            mesh.vx[3*c]=4*c; mesh.vx[3*c+1]=4*c+2; mesh.vx[3*c+2]=4*c;
            mesh.vy[3*c]=0; mesh.vy[3*c+1]=0; mesh.vy[3*c+2]=2;
            for (int j=0;j<3;++j) mesh.vz[3*c+j]=10;
            mesh.set_triangle(c,3*c,3*c+1,3*c+2); mesh.tri_area[c]=2; mesh.tri_cz[c]=10;
            for (int j=0;j<3;++j) mesh.cell_nbr[MeshData::slot(c,j)]=-1;
        }
        edges.build(mesh); surface.resize(cells,cells*3);
        cfg.options.authored=true; cfg.options.closure=closure; cfg.options.soil_char=law;
        cfg.options.force_closed_form=true; GwAquiferRow row;row.Ks=ks;row.zs=2;row.hg0=table;
        cfg.rows.push_back(row);std::vector<std::string> warnings;
        ASSERT_EQ(gw.initialize(mesh,edges,options,{},0,cfg,warnings),"");
    }
};
SurfaceIntakeRequest request(std::string id, double candidate = .1, double water = 1,
                            int cell = 0, double area = .5) {
    return {SurfaceDonorKind::NON_LID,std::move(id),-1,cell,area,0,water,candidate};
}
std::vector<SurfaceIntakeActual> full(const SurfaceExchange& e) {
    std::vector<SurfaceIntakeActual> a;
    for (const auto& x:e.awards()) a.push_back({x.maximum,{}});
    return a;
}
struct Sources {
    SWMM_Engine handle=swmm_engine_create();
    ~Sources(){swmm_engine_close(handle);swmm_engine_destroy(handle);}
    SimulationContext& ctx(){return static_cast<SWMMEngine*>(handle)->context();}
    void open(bool lid=false) {
        const std::filesystem::path dir=OPENSWMM_R4_EXCHANGE_OUT;std::filesystem::create_directories(dir);
        const auto path=dir/(lid?"lid.inp":"nonlid.inp");std::ofstream f(path);f<<std::setprecision(17);
        f<<"[OPTIONS]\nFLOW_UNITS CFS\nFLOW_ROUTING DYNWAVE\nINFILTRATION GREEN_AMPT\nSTART_DATE 10/05/2026\nEND_DATE 10/05/2026\nEND_TIME 00:05:00\nWET_STEP 00:01:00\nROUTING_STEP 1\nREPORT_STEP 00:01:00\n"
          "[JUNCTIONS]\nJ1 0 3\n[OUTFALLS]\nO1 -1 FREE NO\n[CONDUITS]\nC1 J1 O1 30 .013 0 0\n[XSECTIONS]\nC1 CIRCULAR .5 0 0 0 1\n"
          "[RAINGAGES]\nRG INTENSITY 0:05 1 TIMESERIES rain\n[TIMESERIES]\nrain 0:00 0\n[SUBCATCHMENTS]\n";
        for(int i=1;i<=2;++i) f<<"S"<<i<<" RG J1 "<<1/(43560*.3048*.3048)<<" 0 1 0 0\n";
        f<<"[SUBAREAS]\nS1 .01 .1 0 0 0 OUTLET\nS2 .01 .1 0 0 0 OUTLET\n[INFILTRATION]\nS1 3.5 .5 .26\nS2 3.5 .5 .26\n";
        if(lid)f<<"[LID_CONTROLS]\nL1 IT\nL1 SURFACE 3 1 .1 0 0\nL1 STORAGE 12 .4 360 0\n[LID_USAGE]\nS1 L1 2 "<<.25/(.3048*.3048)<<" 1 50 0 0\n";
        f.close();ASSERT_EQ(swmm_engine_open(handle,path.string().c_str(),(dir/"source.rpt").string().c_str(),(dir/"source.out").string().c_str(),nullptr),SWMM_OK);
        ctx().climate_state.temperature=70;
        ctx().gages.rainfall[0]=.01*ucf::UCF(ucf::RAINFALL,ctx().options);
    }
};
}

TEST(SurfaceExchange, CompletedContiguousIntervalsOnlyAndNoReplay) {
    Receiver r;r.init();SurfaceExchange e;
    EXPECT_FALSE(e.plan(r.gw,0,60,1,{request("a")}).empty());
    EXPECT_DOUBLE_EQ(r.gw.state().xacc_from_surface[0],0);
    ASSERT_EQ(e.plan(r.gw,0,1,1,{request("a")}),"");
    EXPECT_FALSE(e.plan(r.gw,0,1,1,{}).empty());
    e.cancel();EXPECT_DOUBLE_EQ(e.completedEnd(),0);
    ASSERT_EQ(e.plan(r.gw,0,1,1,{request("a")}),"");ASSERT_EQ(e.commit(r.gw,full(e)),"");
    EXPECT_FALSE(e.commit(r.gw,full(e)).empty());
    EXPECT_FALSE(e.plan(r.gw,0,1,1,{}).empty());EXPECT_FALSE(e.plan(r.gw,2,3,3,{}).empty());
    ASSERT_EQ(e.plan(r.gw,1,2,2,{}),"");ASSERT_EQ(e.commit(r.gw,{}),"");
    EXPECT_DOUBLE_EQ(e.completedEnd(),2);
}

TEST(SurfaceExchange, DonorHeadCapacityIsReadOnlyAcrossClosuresAndSoils) {
    for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})
    for(auto law:{SoilChar::RUSSO,SoilChar::GARDNER,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}) {
        Receiver r;r.init(cl,law);auto front=r.gw.state().wetting_front;auto pending=r.gw.state().xacc_from_surface;
        const auto dry=r.gw.sourceInfiltrationCapacity(0,0,1);
        EXPECT_GT(r.gw.sourceInfiltrationCapacity(0,2,1),dry);
        EXPECT_EQ(front,r.gw.state().wetting_front);EXPECT_EQ(pending,r.gw.state().xacc_from_surface);
        r.surface.depth[0]=.2;r.gw.publishInfiltration(r.surface,1,0);
        EXPECT_DOUBLE_EQ(r.surface.infil_rate[0],std::min(r.gw.sourceInfiltrationCapacity(0,.2,1),r.gw.infiltrationHeadroom(0)/2));
    }
}

TEST(SurfaceExchange, SharedHeadroomProportionalDryAndPermutationInvariant) {
    Receiver r;r.init();const double room=r.gw.infiltrationHeadroom(0);
    r.gw.bookLinkSeepage(0,room-.01);
    auto a=request("nonlid",1),b=request("lid",3),c=request("mesh",0);
    a.pond=b.pond=10;a.water_ceiling=b.water_ceiling=10;
    b.kind=SurfaceDonorKind::LID_BOTTOM;b.unit=0;c.kind=SurfaceDonorKind::MESH;
    // Interface candidates are also capacity-limited; identical heads/areas
    // produce equal capped demands despite unequal unconstrained candidates.
    SurfaceExchange e,f;ASSERT_EQ(e.plan(r.gw,0,1,1,{a,b,c}),"");
    ASSERT_EQ(f.plan(r.gw,0,1,1,{c,b,a}),"");
    ASSERT_EQ(e.awards().size(),3);double sum=0;
    for(int j=0;j<3;++j){EXPECT_DOUBLE_EQ(e.awards()[j].maximum,f.awards()[j].maximum);sum+=e.awards()[j].maximum;}
    EXPECT_NEAR(sum,.01,1e-14);EXPECT_DOUBLE_EQ(e.awards()[0].maximum,0);
    EXPECT_NEAR(e.awards()[1].maximum,e.awards()[2].maximum,1e-14);
    EXPECT_DOUBLE_EQ(r.gw.state().xacc_from_surface[0],0);
}

TEST(SurfaceExchange, ProportionsUseWaterLimitedCandidatesRatherThanUnboundedCapacity) {
    Receiver r;r.init();const double room=r.gw.infiltrationHeadroom(0);r.gw.bookLinkSeepage(0,room-.001);
    auto a=request("a",.001,.001),b=request("b",.003,.003);SurfaceExchange e;
    ASSERT_EQ(e.plan(r.gw,0,1,1,{a,b}),"");
    EXPECT_NEAR(e.awards()[0].maximum,.00025,1e-14);EXPECT_NEAR(e.awards()[1].maximum,.00075,1e-14);
}

TEST(SurfaceExchange, OneDonorWaterCeilingAcrossCellsAndNoDenialRedistribution) {
    Receiver r;r.init(GwClosure::CLOSED_FORM,SoilChar::RUSSO,2);
    const double room=r.gw.infiltrationHeadroom(0);r.gw.bookLinkSeepage(0,room);
    auto a=request("a",1,.02,0),b=request("a",1,.02,1);a.pond=b.pond=10;
    SurfaceExchange e;ASSERT_EQ(e.plan(r.gw,0,1,1,{a,b}),"");
    EXPECT_DOUBLE_EQ(e.awards()[0].maximum,0);
    // Storage denial in cell 0 does not move its share of the donor's water
    // ceiling to cell 1. The soil interface and physical headroom are distinct.
    EXPECT_NEAR(e.awards()[1].maximum,.01,1e-14);
    ASSERT_EQ(e.commit(r.gw,full(e)),"");EXPECT_NEAR(r.gw.state().xacc_from_surface[1],.01,1e-14);
    Receiver both;both.init(GwClosure::CLOSED_FORM,SoilChar::RUSSO,2);SurfaceExchange x;
    ASSERT_EQ(x.plan(both.gw,0,1,1,{a,b}),"");
    EXPECT_NEAR(x.awards()[0].maximum,.01,1e-14);EXPECT_NEAR(x.awards()[1].maximum,.01,1e-14);
}

TEST(SurfaceExchange, SettlementRevalidatesAtomicallyAndExpiresUnusedAwards) {
    Receiver r;r.init();SurfaceExchange e;ASSERT_EQ(e.plan(r.gw,0,1,1,{request("a"),request("b")}),"");
    auto actual=full(e);const double room=r.gw.infiltrationHeadroom(0);r.gw.bookLinkSeepage(0,room);
    const auto front=r.gw.state().wetting_front;
    EXPECT_FALSE(e.commit(r.gw,actual).empty());EXPECT_DOUBLE_EQ(r.gw.state().xacc_from_surface[0],0);EXPECT_EQ(front,r.gw.state().wetting_front);
    e.cancel();r.gw.state().lacc[0]=0;ASSERT_EQ(e.plan(r.gw,0,1,1,{request("a"),request("b")}),"");
    actual=full(e);actual[0].volume=0;actual[1].volume*=.5;
    ASSERT_EQ(e.commit(r.gw,actual),"");EXPECT_DOUBLE_EQ(r.gw.state().xacc_from_surface[0],actual[1].volume);
    r.gw.publishInfiltration(r.surface,1,1);EXPECT_NEAR(r.gw.infiltrationHeadroom(0),room-actual[1].volume,1e-14);
    EXPECT_EQ(e.receipts()[0].request.source,"a");EXPECT_DOUBLE_EQ(e.receipts()[0].volume,0);
}

TEST(SurfaceExchange, InvalidInputsAndChangedConductivityBookNothing) {
    Receiver r;r.init();SurfaceExchange e;
    EXPECT_FALSE(e.plan(r.gw,0,1,1,{request("a",1,1,0,3)}).empty());
    EXPECT_FALSE(e.plan(r.gw,0,1,1,{request("a"),request("a")}).empty());
    auto x=request("a");x.pond=std::numeric_limits<double>::quiet_NaN();EXPECT_FALSE(e.plan(r.gw,0,1,1,{x}).empty());
    ASSERT_EQ(e.plan(r.gw,0,1,1,{request("a")}),"");auto a=full(e);a[0].volume=2;
    EXPECT_FALSE(e.commit(r.gw,a).empty());a=full(e);r.gw.state().Ks[0]=0;
    EXPECT_FALSE(e.commit(r.gw,a).empty());EXPECT_DOUBLE_EQ(r.gw.state().xacc_from_surface[0],0);
}

TEST(SurfaceExchange, ActualDonorMassAndWaterStayPairedInPendingReceipts) {
    Receiver r;r.init();RowLayoutLite rows;rows.n_species=1;rows.n_pollut=1;rows.names={"Tracer"};
    std::vector<std::string> w;r.gw.initTransport(rows,nullptr,{},w);
    SurfaceExchange e;ASSERT_EQ(e.plan(r.gw,0,1,1,{request("a"),request("b")}),"");
    auto a=full(e);a[0].mass={2};a[1].mass={5};ASSERT_EQ(e.commit(r.gw,a),"");
    EXPECT_DOUBLE_EQ(r.gw.transport().xacc_from_surface[0],7);
    EXPECT_DOUBLE_EQ(r.gw.state().xacc_from_surface[0],a[0].volume+a[1].volume);
    EXPECT_EQ(e.receipts()[0].mass,std::vector<double>{2});EXPECT_EQ(e.receipts()[1].mass,std::vector<double>{5});
    EXPECT_DOUBLE_EQ(e.receipts()[0].start,0);EXPECT_DOUBLE_EQ(e.receipts()[0].end,1);
}

TEST(SurfaceExchange, ClosedSaturationAndActualFiringConserveAcrossClosures) {
    for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}) {
        Receiver saturated;saturated.init(cl,SoilChar::RUSSO,1,2);SurfaceExchange zero;
        ASSERT_EQ(zero.plan(saturated.gw,0,1,1,{request("a")}),"");EXPECT_DOUBLE_EQ(zero.awards()[0].maximum,0);
        ASSERT_EQ(zero.commit(saturated.gw,full(zero)),"");EXPECT_DOUBLE_EQ(saturated.gw.state().xacc_from_surface[0],0);
        Receiver r;r.init(cl,SoilChar::RUSSO,1,1,1e-5);SurfaceExchange e;const double initial=r.gw.state().storage();double accepted=0;
        for(int t=0;t<10;++t){ASSERT_EQ(e.plan(r.gw,t,t+1,t+1,{request("a"),request("b")}),"");auto a=full(e);for(auto& x:a)accepted+=x.volume;
            ASSERT_EQ(e.commit(r.gw,a),"");r.gw.fireGwCells(0,1,r.surface,t);}
        EXPECT_NEAR(r.gw.state().storage()-initial,accepted-r.gw.state().led_dunne-r.gw.state().led_reject-r.gw.state().led_deep,1e-12);
        EXPECT_NEAR(r.gw.state().continuityResidual(),0,1e-12);
    }
}

TEST(SurfaceExchange, TwoRealRunoffKernelsApplyAwardsBeforeWithdrawal) {
    Sources s;s.open();auto& ctx=s.ctx();runoff::RunoffSolver live;live.init(ctx,{{0,1},{1,1}});
    const auto source_before=ctx.subcatches;const auto ledger_before=ctx.mass_balance.runoff_infil;
    Receiver r;r.init();const double room=r.gw.infiltrationHeadroom(0);r.gw.bookLinkSeepage(0,room-.002);
    std::vector<SurfaceIntakeRequest> candidates;
    auto trial=live;
    runoff::RunoffSolver::InfiltrationBoundary propose=[&](int i,double pond,double water,double& rate){
        const double area=trial.soa().area[i]*.3048*.3048;
        rate=std::min(water,r.gw.sourceInfiltrationCapacity(0,pond*.3048,1)/.3048);
        candidates.push_back(request(ctx.subcatch_names.name_of(i),rate*area*.3048,water*area*.3048,0,area));return true;
    };
    trial.execute(ctx,1,0,1,1,-1,&propose);ctx.subcatches=source_before;
    EXPECT_DOUBLE_EQ(ctx.mass_balance.runoff_infil,ledger_before);EXPECT_DOUBLE_EQ(live.soa().depth_perv[0],0);
    SurfaceExchange exchange;ASSERT_EQ(exchange.plan(r.gw,0,1,1,candidates),"");
    auto bounded=live;
    runoff::RunoffSolver::InfiltrationBoundary limit=[&](int i,double,double,double& rate){
        rate=std::nextafter(exchange.awards()[i].maximum/(bounded.soa().area[i]*ft3),0.0);return true;
    };
    int method;double soil_before[6],soil_after[6];bounded.infil_get_state(0,method,soil_before);
    bounded.execute(ctx,1,0,1,1,-1,&limit);
    auto bounded_outputs=ctx.subcatches;ctx.subcatches=source_before;
    std::vector<SurfaceIntakeActual> actual;
    for(int i=0;i<2;++i){actual.push_back({bounded.soa().infil_vol[i]*ft3,{}});
        EXPECT_NEAR(actual[i].volume,exchange.awards()[i].maximum,1e-17);
        const double store=bounded.soa().depth_perv[i]*bounded.soa().area[i]*ft3;
        EXPECT_NEAR(store+actual[i].volume,.01*bounded.soa().area[i]*ft3,1e-17);EXPECT_GT(store,0);}
    ASSERT_EQ(exchange.commit(r.gw,actual),"");live=std::move(bounded);ctx.subcatches=std::move(bounded_outputs);
    EXPECT_NEAR(r.gw.state().xacc_from_surface[0],.002,1e-14);
    live.infil_get_state(0,method,soil_after);for(int j=0;j<6;++j)EXPECT_DOUBLE_EQ(soil_before[j],soil_after[j]);
}

TEST(SurfaceExchange, NullBoundaryRetainsNativeTrajectoryAndActualPerviousEvapIsSeparate) {
    Sources s;s.open();auto& ctx=s.ctx();ctx.subcatches.frac_imperv[0]=.5;
    runoff::RunoffSolver a,b;a.init(ctx);b=a;const auto initial=ctx.subcatches;
    for(int t=0;t<10;++t){a.execute(ctx,1,.0001);auto advanced=ctx.subcatches;ctx.subcatches=initial;b.execute(ctx,1,.0001,1,1,-1,nullptr);
        EXPECT_EQ(a.soa().depth_perv,b.soa().depth_perv);EXPECT_EQ(a.soa().infil_vol,b.soa().infil_vol);EXPECT_EQ(a.soa().evap_loss,b.soa().evap_loss);ctx.subcatches=advanced;}
    runoff::RunoffSolver pond;pond.init(ctx);
    runoff::RunoffSolver::InfiltrationBoundary sealed=[](int,double,double,double& rate){rate=0;return true;};
    pond.execute(ctx,1,0,1,1,-1,&sealed);pond.execute(ctx,1,.0001,1,1,-1,&sealed);
    EXPECT_GT(pond.soa().perv_evap_vol[0],pond.soa().actual_perv_evap_vol[0]);
    EXPECT_NEAR(pond.soa().actual_perv_evap_vol[0],pond.soa().area[0]*.5*.0001,1e-15);
}

TEST(SurfaceExchange, RealStorageLidBottomConsumesOnlyItsBoundedAward) {
    Sources s;s.open(true);auto& ctx=s.ctx();lid::LIDSolver solver;solver.init(ctx);
    const int gi=static_cast<int>(lid::LIDType::INFIL_TRENCH);ASSERT_EQ(solver.group(gi).count,1);const auto original=solver.group(gi);
    solver.setNativeInfil({0,0},{1e30,1e30});solver.execute(ctx,1,0,0);
    const double candidate=solver.group(gi).infil_loss[0]*solver.group(gi).area[0]*ft3;
    ASSERT_GT(candidate,0);solver.group(gi)=original;
    Receiver r;r.init();auto req=request("S1",candidate,candidate,0,original.area[0]*.3048*.3048);req.kind=SurfaceDonorKind::LID_BOTTOM;req.unit=0;
    SurfaceExchange e;ASSERT_EQ(e.plan(r.gw,0,1,1,{req}),"");const double cap=e.awards()[0].maximum*.5;
    solver.setNativeInfil({0,0},{cap/(original.area[0]*ft3),1e30});solver.execute(ctx,1,0,0);
    const double actual=solver.group(gi).infil_loss[0]*original.area[0]*ft3;
    EXPECT_LE(actual,cap*(1+1e-12));EXPECT_GT(actual,0);
    auto bounded=solver.group(gi);solver.group(gi)=original;
    ASSERT_EQ(e.commit(r.gw,{{actual,{}}}),"");solver.group(gi)=std::move(bounded);
    EXPECT_NEAR(solver.group(gi).wb_infil[0]*original.area[0]*ft3,actual,1e-17);
    EXPECT_NEAR(r.gw.state().xacc_from_surface[0],actual,1e-17);
    EXPECT_NEAR((original.stor_depth[0]-solver.group(gi).stor_depth[0])*original.stor_void[0],solver.group(gi).infil_loss[0],1e-12);
}

TEST(SurfaceExchange, ReviewedAreasAvoidLegacyRoundingInUsAndSiAndRejectBadOverrides) {
    Sources s;s.open();auto& ctx=s.ctx();runoff::RunoffSolver legacy,us,si;
    legacy.init(ctx);us.init(ctx,{{0,1}});
    EXPECT_NE(legacy.soa().area[0],us.soa().area[0]);
    EXPECT_DOUBLE_EQ(legacy.soa().area[1],us.soa().area[1]);
    EXPECT_NEAR(us.soa().area[0]*.3048*.3048,1,1e-15);
    ctx.options.flow_units=FlowUnits::CMS;ctx.subcatches.area[0]=.0001;
    si.init(ctx,{{0,1}});EXPECT_DOUBLE_EQ(us.soa().area[0],si.soa().area[0]);
    const auto before=si.soa().area;
    EXPECT_THROW(si.init(ctx,{{0,1},{0,1}}),std::invalid_argument);EXPECT_EQ(si.soa().area,before);
    EXPECT_THROW(si.init(ctx,{{2,1}}),std::invalid_argument);
    EXPECT_THROW(si.init(ctx,{{0,-1}}),std::invalid_argument);
}

TEST(SurfaceExchange, ExistingEngineRunoffClockActuallyLeadsTheFirstRoutingStep) {
    Sources s;s.open();auto* engine=static_cast<SWMMEngine*>(s.handle);
    ASSERT_EQ(swmm_engine_initialize(s.handle),SWMM_OK);
    ASSERT_EQ(swmm_engine_start(s.handle,0),SWMM_OK);
    const auto path=std::filesystem::path(OPENSWMM_R4_EXCHANGE_OUT)/"clock.rnf";
    ASSERT_EQ(engine->openRunoffIfaceWrite(path.string()),0);
    double elapsed=0;ASSERT_EQ(swmm_engine_step(s.handle,&elapsed),SWMM_OK);
    engine->closeRunoffIface();
    std::ifstream f(path,std::ios::binary);ASSERT_TRUE(f.good());f.seekg(28);
    float runoff_interval=0;f.read(reinterpret_cast<char*>(&runoff_interval),sizeof(runoff_interval));ASSERT_TRUE(f.good());
    EXPECT_GT(runoff_interval,engine->context().current_time);
    std::ofstream audit(std::filesystem::path(OPENSWMM_R4_EXCHANGE_OUT)/"clock.json");
    audit<<"{\"routing_completed_seconds\":"<<engine->context().current_time<<",\"runoff_evaluated_through_seconds\":"<<runoff_interval<<"}\n";
}

TEST(SurfaceExchange, SourceCadenceProbeRecordsWorkCountWithoutClaimingWholeModelCost) {
    Sources s;s.open();auto& ctx=s.ctx();runoff::RunoffSolver coarse,fine;
    runoff::RunoffSolver::InfiltrationBoundary sealed=[](int,double,double,double& rate){rate=0;return true;};
    const auto initial=ctx.subcatches;constexpr int repeats=100;
    auto run=[&](runoff::RunoffSolver& solver,int steps,double dt){
        const auto begin=std::chrono::steady_clock::now();
        for(int repeat=0;repeat<repeats;++repeat){ctx.subcatches=initial;solver.init(ctx,{{0,1},{1,1}});
            for(int step=0;step<steps;++step)solver.execute(ctx,dt,0,1,1,-1,&sealed);}
        return std::chrono::duration<double>(std::chrono::steady_clock::now()-begin).count();
    };
    const double slow=run(coarse,1,300),fast=run(fine,600,.5);
    EXPECT_NEAR(coarse.soa().depth_perv[0],fine.soa().depth_perv[0],1e-12);
    std::ofstream out(std::filesystem::path(OPENSWMM_R4_EXCHANGE_OUT)/"source-cadence.json");
    out<<std::setprecision(17)<<"{\"sources\":2,\"repeats\":"<<repeats<<",\"coarse_calls_per_repeat\":1,\"fine_calls_per_repeat\":600,\"coarse_seconds\":"<<slow<<",\"fine_seconds\":"<<fast<<"}\n";
}
