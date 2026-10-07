// SPDX-License-Identifier: Apache-2.0
// Copyright 2026 Caleb Buahin
#include <gtest/gtest.h>
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/infil/Infil2D.hpp"
#include "core/SimulationOptions.hpp"
#include <cmath>
#include <cstring>
using namespace openswmm;
using namespace openswmm::twoD;
namespace {
struct Rig {
 MeshData mesh; InertialEdges edges; SurfaceStateData surf; SolverOptions2D opts;
 SubsurfaceConfig cfg; SubsurfaceSolver gw;
 void initialize(GwClosure closure, SoilChar law, double table=1.0, double ks=1e-5, double thickness=2.0) {
  mesh.resize_vertices(3);mesh.vx={0,2,0};mesh.vy={0,0,2};mesh.vz={10,10,10};
  mesh.resize_triangles(1);mesh.set_triangle(0,0,1,2);mesh.tri_area[0]=2;mesh.tri_cz[0]=10;
  for(int k=0;k<3;++k)mesh.cell_nbr[MeshData::slot(0,k)]=-1;
  edges.build(mesh);surf.resize(1,3);surf.depth[0]=.1;surf.volume[0]=.2;
  cfg.options.authored=true;cfg.options.closure=closure;cfg.options.force_closed_form=true;cfg.options.soil_char=law;
  GwAquiferRow row;row.Ks=ks;row.zs=thickness;row.hg0=table;cfg.rows.push_back(row);
  std::vector<std::string>warnings;ASSERT_EQ(gw.initialize(mesh,edges,opts,{},0,cfg,warnings),"");
 }
};
}
TEST(AquiferReceiving, SaturatedAndSealedPublishValidZero) {
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}) {
  Rig r;r.initialize(cl,SoilChar::RUSSO,2.0,0);r.gw.publishInfiltration(r.surf,60,0);
  EXPECT_DOUBLE_EQ(r.surf.infil_rate[0],0);EXPECT_DOUBLE_EQ(r.gw.acceptSurfaceInfiltration(0,100),0);
  EXPECT_DOUBLE_EQ(r.gw.state().led_reject,0);EXPECT_DOUBLE_EQ(r.surf.volume[0],.2);
 }
}
TEST(AquiferReceiving, PendingReservationsCannotBeSpentTwiceOrResetByRefresh) {
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})
 for(auto law:{SoilChar::RUSSO,SoilChar::GARDNER,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}) {
  SCOPED_TRACE(std::to_string(int(cl))+" / "+std::to_string(int(law)));
  Rig r;r.initialize(cl,law);r.gw.publishInfiltration(r.surf,60,0);
  const double allowance=r.gw.infiltrationHeadroom(0);ASSERT_GT(allowance,0);
  const double a=r.gw.acceptSurfaceInfiltration(0,allowance*.6);
  r.gw.publishInfiltration(r.surf,60,1);
  const double b=r.gw.acceptSurfaceInfiltration(0,allowance);
  EXPECT_NEAR(a+b,allowance,1e-14);EXPECT_DOUBLE_EQ(r.gw.acceptSurfaceInfiltration(0,1),0);
  r.gw.publishInfiltration(r.surf,60,2);EXPECT_DOUBLE_EQ(r.surf.infil_rate[0],0);
  EXPECT_NEAR(r.gw.state().xacc_from_surface[0],allowance,1e-14);
 }
}
TEST(AquiferReceiving, PendingPositiveLinkReceiptReducesAllowance) {
 Rig r;r.initialize(GwClosure::CLOSED_FORM,SoilChar::RUSSO);
 const double before=r.gw.infiltrationHeadroom(0);r.gw.bookLinkSeepage(0,before*.5);
 EXPECT_NEAR(r.gw.infiltrationHeadroom(0),before*.5,1e-14);
}
TEST(AquiferReceiving, AcceptedDrainageCreatesCapacityAfterFiring) {
 Rig r;r.initialize(GwClosure::ENSLAVED,SoilChar::RUSSO,2);
 r.gw.state().c_loss[0]=1e-3;r.gw.publishInfiltration(r.surf,60,0);
 EXPECT_DOUBLE_EQ(r.surf.infil_rate[0],0);
 r.gw.fireGwCells(0,1,r.surf,0);
 EXPECT_GT(r.surf.infil_rate[0],0);EXPECT_DOUBLE_EQ(r.gw.state().infil_refresh[0],1);
 EXPECT_NEAR(r.gw.state().continuityResidual(),0,1e-12);
}
TEST(AquiferReceiving, AcceptedDeliveryClosesEachSoilAndClosureWithoutRejection) {
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})
 for(auto law:{SoilChar::RUSSO,SoilChar::GARDNER,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}) {
  SCOPED_TRACE(std::to_string(int(cl))+" / "+std::to_string(int(law)));
  Rig r;r.initialize(cl,law);r.gw.publishInfiltration(r.surf,1,0);
  const double initial=r.gw.state().storage();double accepted=0;
  for(int step=0;step<20;++step) {
   accepted+=r.gw.acceptSurfaceInfiltration(0,r.surf.infil_rate[0]*r.mesh.tri_area[0]);
   r.gw.fireGwCells(0,1,r.surf,step);
  }
  EXPECT_NEAR(r.gw.state().storage()-initial,accepted-r.gw.state().led_dunne-r.gw.state().led_reject,1e-12);
  EXPECT_NEAR(r.gw.state().continuityResidual(),0,1e-12);EXPECT_NEAR(r.gw.state().led_reject,0,1e-12);
 }
}
TEST(AquiferOwnership, MixedDefaultsSkipCoveredCellsAndExplicitConflictIsAtomic) {
 MeshData mesh;mesh.resize_triangles(3);mesh.tri_tag={"soil","soil","paved"};
 Infil2D bank;Infil2DRow row;row.has_method=true;row.method=InfilModel::CONSTANT;row.p[0]=2;
 bank.defaults().push_back({"*",row});bank.defaults().push_back({"soil",row});
 bank.setAquiferOwners({1,0,1});SimulationOptions opts;std::string error;
 ASSERT_TRUE(bank.resolve(mesh,opts,error));ASSERT_TRUE(bank.active());
 EXPECT_EQ(bank.bank().owner(0),surface::InfilBank::Owner::EXTERNAL);
 EXPECT_EQ(bank.ownershipMessages()[0],"2D infiltration default '*': 1 applied, 2 skipped (aquifer-owned)");
 EXPECT_EQ(bank.ownershipMessages()[1],"2D infiltration default 'soil': 1 applied, 1 skipped (aquifer-owned)");
 bank.overrides().push_back({0,row});EXPECT_FALSE(bank.resolve(mesh,opts,error));
 EXPECT_NE(error.find("cell 1"),std::string::npos);EXPECT_TRUE(bank.resolvedRows()[1].has_method);
}
TEST(AquiferOwnership, ExternalOnlyBankHasNoOrdinaryKernelState) {
 MeshData mesh;mesh.resize_triangles(1);Infil2D bank;bank.setAquifer2DAvailable(true);
 SimulationOptions opts;std::string error;ASSERT_TRUE(bank.resolve(mesh,opts,error));ASSERT_TRUE(bank.active());
 SurfaceStateData surf;surf.resize(1,0);surf.infil_rate[0]=.25;
 bank.updateRates(mesh,surf,60);EXPECT_DOUBLE_EQ(surf.infil_rate[0],.25);
 int model;double state[6];bank.bank().pack(0,model,state);bank.bank().unpack(0,model,state);
}

#include "core/SWMMEngine.hpp"
#include "core/HotStartManager.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include <openswmm/engine/openswmm_engine.h>
#include <openswmm/engine/openswmm_infil2d.h>
#include <openswmm/engine/openswmm_gw2d.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
namespace {
struct Model {
 SWMM_Engine handle=swmm_engine_create();
 ~Model(){swmm_engine_close(handle);swmm_engine_destroy(handle);}
 SWMMEngine& engine(){return *static_cast<SWMMEngine*>(handle);}
 bool open(const std::string& name,const std::string& extra="",bool us=false,const std::string& closure="CLOSED_FORM",const std::string& law="RUSSO"){
  const std::filesystem::path dir=OPENSWMM_R2_OUT;std::filesystem::create_directories(dir);
  const auto path=dir/(name+".inp");std::ofstream f(path);f << std::setprecision(17);
  f<<"[OPTIONS]\nFLOW_UNITS "<<(us?"CFS":"CMS")<<"\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/05/2026\nEND_DATE 10/05/2026\nEND_TIME 00:05:00\nWET_STEP 00:01:00\nROUTING_STEP 1\nREPORT_STEP 00:01:00\n"
    "[JUNCTIONS]\nJ1 0 3\n[OUTFALLS]\nO1 -1 FREE NO\n[CONDUITS]\nC1 J1 O1 30 .013 0 0\n[XSECTIONS]\nC1 CIRCULAR .5 0 0 0 1\n"
    "[2D_OPTIONS]\nOUTPUT_FILE " << (dir/(name+".2d.h5")).string() << "\nINTEGRATOR EXPLICIT\nMAX_TIMESTEP 1\nLTS_TIERS 2\nREPORT_2D YES\nREPORT_2D_VARIABLES ALL\n"<<extra<<"\n"
    "[2D_VERTICES]\n0 0 0\n" << (us?10/.3048:10.) << " 0 0\n0 " << (us?10/.3048:10.) << " 0\n[2D_TRIANGLES]\n0 1 2 .03 " << (us?.1/.3048:.1) << "\n[2D_AQUIFER_OPTIONS]\nCLOSURE " << closure << "\nSOIL_CHAR " << law << "\nFORCE_CLOSED_FORM YES\nNODE_ENROLMENT ROWS\n[2D_AQUIFER]\n* "
    <<(us?36./25.4:36.)<<" "<<(us?2/.3048:2.)<<" .45 .1 "<<(us?2*.3048:2.)<<" HG0 "<<(us?1/.3048:1.)<<" PSI_B "<<(us?.2/.3048:.2)<<"\n";
  f.close();return swmm_engine_open(handle,path.string().c_str(),(dir/(name+".rpt")).string().c_str(),(dir/(name+".out")).string().c_str(),nullptr)==SWMM_OK;
 }
 bool initialize(){return swmm_engine_initialize(handle)==SWMM_OK;}
};
}
TEST(AquiferReceiving, RestartRetainsPendingReservationAndHeldCapacity) {
 Model original,resumed;ASSERT_TRUE(original.open("restart_original"));ASSERT_TRUE(resumed.open("restart_resumed"));
 ASSERT_TRUE(original.initialize());ASSERT_TRUE(resumed.initialize());
 auto& a=original.engine().surfaceRouter2D();auto& b=resumed.engine().surfaceRouter2D();
 RowLayoutLite rows;rows.n_species=1;rows.n_pollut=1;rows.names={"Tracer"};std::vector<std::string> warnings;
 a.subsurface().initTransport(rows,nullptr,{},warnings);b.subsurface().initTransport(rows,nullptr,{},warnings);
 const double take=a.subsurface().acceptSurfaceInfiltration(0,.01);ASSERT_GT(take,0);
 a.subsurface().bookInfiltrationMass(0,0,3.0);
 a.state().volume[0]-=take;a.state().infil_applied[0]+=take/a.mesh().tri_area[0];
 const auto file=(std::filesystem::path(OPENSWMM_R2_OUT)/"pending.hsf").string();
 std::unique_ptr<HotStartFile> saved(HotStartManager::save(original.engine().context(),file));ASSERT_TRUE(saved);EXPECT_EQ(saved->header.version,13u);
 std::unique_ptr<HotStartFile> loaded(HotStartManager::open(file));ASSERT_TRUE(loaded);
 auto invalid=*loaded;invalid.gw_interface[21].push_back(1.0);
 const auto nodes_before=resumed.engine().context().nodes.depth;
 if(!invalid.nodes.empty())invalid.nodes[0].depth=999;
 EXPECT_NE(HotStartManager::apply(invalid,resumed.engine().context()),0);
 EXPECT_EQ(resumed.engine().context().nodes.depth,nodes_before);
 invalid=*loaded;invalid.gw_species[0]="DifferentTracer";
 EXPECT_NE(HotStartManager::apply(invalid,resumed.engine().context()),0);
 EXPECT_EQ(resumed.engine().context().nodes.depth,nodes_before);
 EXPECT_EQ(HotStartManager::apply(*loaded,resumed.engine().context()),0);
 EXPECT_EQ(a.subsurface().state().xacc_from_surface,b.subsurface().state().xacc_from_surface);
 EXPECT_EQ(a.subsurface().state().infil_remaining,b.subsurface().state().infil_remaining);
 EXPECT_EQ(a.subsurface().state().wetting_front,b.subsurface().state().wetting_front);
 EXPECT_EQ(a.subsurface().transport().xacc_from_surface,b.subsurface().transport().xacc_from_surface);
 EXPECT_EQ(a.state().volume,b.state().volume);EXPECT_EQ(a.state().infil_applied,b.state().infil_applied);
 for(int step=0;step<20;++step){
  EXPECT_DOUBLE_EQ(a.subsurface().acceptSurfaceInfiltration(0,.002),b.subsurface().acceptSurfaceInfiltration(0,.002));
  a.subsurface().fireGwCells(0,1,a.state(),step);b.subsurface().fireGwCells(0,1,b.state(),step);
  EXPECT_EQ(a.subsurface().state().hg,b.subsurface().state().hg);EXPECT_EQ(a.subsurface().state().hu,b.subsurface().state().hu);
  EXPECT_DOUBLE_EQ(a.subsurface().state().led_infil_in,b.subsurface().state().led_infil_in);
  EXPECT_EQ(a.subsurface().transport().sat_mass,b.subsurface().transport().sat_mass);
  EXPECT_EQ(a.subsurface().transport().unsat_mass,b.subsurface().transport().unsat_mass);
 }
 EXPECT_NEAR(a.subsurface().state().continuityResidual(),0,1e-10);
}
TEST(AquiferOwnership, BulkPreviewAndAtomicAuthoredReplacementPreserveOrderingAndPresence) {
 Model model;ASSERT_TRUE(model.open("authored_rows"));
 SWMM_Infil2DAuthoredRow rows[3]{};
 for(auto& row:rows){row.cell=-1;std::strcpy(row.tag,"*");row.row.has_method=1;row.row.method=SWMM_INFIL2D_CONSTANT;row.row.p[0]=2;}
 rows[0].dest_explicit=0;rows[1].dest_explicit=1;rows[1].row.p[0]=4;rows[2].cell=0;
 ASSERT_EQ(swmm_infil2d_replace_authored_rows(model.handle,rows,3),SWMM_OK);
 int owner,source,conflict,written;
 ASSERT_EQ(swmm_infil2d_get_ownership_bulk(model.handle,&owner,&source,&conflict,1,&written),SWMM_OK);
 EXPECT_EQ(owner,2);EXPECT_EQ(source,0);EXPECT_EQ(conflict,1);EXPECT_EQ(written,1);
 SWMM_Infil2DAuthoredRow found[3]{};ASSERT_EQ(swmm_infil2d_get_authored_rows(model.handle,found,3,&written),SWMM_OK);
 EXPECT_EQ(found[0].dest_explicit,0);EXPECT_EQ(found[1].dest_explicit,1);EXPECT_EQ(found[1].row.p[0],4);
 rows[2].cell=100;EXPECT_EQ(swmm_infil2d_replace_authored_rows(model.handle,rows,3),SWMM_ERR_BADINDEX);
 ASSERT_EQ(swmm_infil2d_get_authored_rows(model.handle,found,3,&written),SWMM_OK);EXPECT_EQ(found[2].cell,0);
 EXPECT_FALSE(model.initialize());
}
TEST(AquiferReceiving, ZeroColumnRemainsEmptyThroughFirings) {
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})
 for(auto law:{SoilChar::RUSSO,SoilChar::GARDNER,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}) {
  SCOPED_TRACE(std::to_string(int(cl))+" / "+std::to_string(int(law)));
  Rig r;r.initialize(cl,law,2,1e-5);r.gw.publishInfiltration(r.surf,60,0);
  const double initial=r.gw.state().storage();
  for(int s=0;s<10;++s)r.gw.fireGwCells(0,1,r.surf,s);
  EXPECT_DOUBLE_EQ(r.gw.state().hu[0],0);EXPECT_DOUBLE_EQ(r.gw.state().storage(),initial);
  EXPECT_DOUBLE_EQ(r.gw.state().led_reject,0);EXPECT_DOUBLE_EQ(r.gw.state().led_dunne,0);
 }
}
TEST(AquiferReceiving, ProjectUnitsProduceTheSamePhysicalCapacity) {
 for(const std::string closure:{"CLOSED_FORM","ENSLAVED","SIGMA"})
 for(const std::string law:{"RUSSO","GARDNER","BROOKS_COREY","VAN_GENUCHTEN"}) {
 SCOPED_TRACE(closure+" / "+law);
 Model si,us;ASSERT_TRUE(si.open("units_si_"+closure+"_"+law,"",false,closure,law));ASSERT_TRUE(us.open("units_us_"+closure+"_"+law,"",true,closure,law));
 ASSERT_TRUE(si.initialize());ASSERT_TRUE(us.initialize());
 auto& a=si.engine().surfaceRouter2D();auto& b=us.engine().surfaceRouter2D();
 EXPECT_NEAR(a.mesh().tri_area[0],b.mesh().tri_area[0],1e-12);
 EXPECT_NEAR(a.state().depth[0],b.state().depth[0],1e-14);
 EXPECT_NEAR(a.state().infil_rate[0],b.state().infil_rate[0],1e-14);
 EXPECT_NEAR(a.subsurface().infiltrationHeadroom(0),b.subsurface().infiltrationHeadroom(0),1e-12);
 for(int step=0;step<100;++step) {
  const double qa=a.subsurface().acceptSurfaceInfiltration(0,a.state().infil_rate[0]*a.mesh().tri_area[0]);
  const double qb=b.subsurface().acceptSurfaceInfiltration(0,b.state().infil_rate[0]*b.mesh().tri_area[0]);
  EXPECT_NEAR(qa,qb,1e-12);
  a.subsurface().fireGwCells(0,1,a.state(),step);b.subsurface().fireGwCells(0,1,b.state(),step);
  EXPECT_NEAR(a.subsurface().state().storage(),b.subsurface().state().storage(),1e-12);
 }
 EXPECT_NEAR(a.subsurface().state().continuityResidual(),0,1e-10);
 EXPECT_NEAR(b.subsurface().state().continuityResidual(),0,1e-10);
 }
}

TEST(AquiferReceiving, SealedUnsaturatedColumnCannotAcceptSurfaceDelivery) {
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}) {
  Rig r;r.initialize(cl,SoilChar::RUSSO,1,0);r.gw.publishInfiltration(r.surf,60,0);
  EXPECT_GT(r.gw.infiltrationHeadroom(0),0);EXPECT_DOUBLE_EQ(r.surf.infil_rate[0],0);
  EXPECT_DOUBLE_EQ(r.gw.acceptSurfaceInfiltration(0,100),0);
 }
}
TEST(AquiferReceiving, ExternalOnlyRunWritesCapacityAndOffIsUnavailable) {
 Model model;ASSERT_TRUE(model.open("external_only_results"));ASSERT_TRUE(model.initialize());
 double capacity=0;EXPECT_EQ(swmm_gw2d_get_cell(model.handle,0,SWMM_GW2D_VAR_INFIL_CAPACITY,&capacity),SWMM_OK);EXPECT_GT(capacity,0);
 ASSERT_EQ(swmm_engine_start(model.handle,1),SWMM_OK);double elapsed=0;
 while(swmm_engine_step(model.handle,&elapsed)==SWMM_OK&&elapsed>0){}
 auto& gw=model.engine().surfaceRouter2D().subsurface().state();
 EXPECT_GT(gw.led_infil_in,0);EXPECT_NEAR(gw.led_reject,0,1e-12);EXPECT_NEAR(gw.continuityResidual(),0,1e-10);
 EXPECT_LT(std::fabs(model.engine().context().mass_balance_2d.error()),1e-6);
 ASSERT_EQ(swmm_engine_end(model.handle),SWMM_OK);
 Model off;ASSERT_TRUE(off.open("off_results","INFILTRATION NO"));ASSERT_TRUE(off.initialize());
 EXPECT_EQ(swmm_gw2d_get_cell(off.handle,0,SWMM_GW2D_VAR_INFIL_CAPACITY,&capacity),SWMM_ERR_LIFECYCLE);
 ASSERT_EQ(swmm_engine_start(off.handle,1),SWMM_OK);
 while(swmm_engine_step(off.handle,&elapsed)==SWMM_OK&&elapsed>0){}
 ASSERT_EQ(swmm_engine_end(off.handle),SWMM_OK);
}

TEST(AquiferReceiving, DeepBulkColumnApproachesAnalyticGreenAmptIntake) {
 Rig r;r.initialize(GwClosure::CLOSED_FORM,SoilChar::GARDNER,1,1e-5,1001);
 auto& st=r.gw.state();const double length=st.zs[0]-st.hg[0];st.hu[0]=.2*length;st.wetting_front[0]=.01;st.led_init_storage=st.storage();
 soil::Params p;p.law=SoilChar::GARDNER;
 const double delta=p.theta_s-.2;
 const double suction=-std::log((.2-p.theta_r)/(p.theta_s-p.theta_r))/p.alpha;
 const double B=(r.surf.depth[0]+suction)*delta,F0=st.wetting_front[0],dt=.01;
 for(int k=0;k<10000;++k){
  r.gw.publishInfiltration(r.surf,dt,k*dt);
  r.gw.acceptSurfaceInfiltration(0,r.surf.infil_rate[0]*st.area[0]*dt);
  r.gw.fireGwCells(0,dt,r.surf,k*dt);
 }
 const double F=st.wetting_front[0];
 // Integrated GA: (F-F0)-B*ln((F+B)/(F0+B)) = Ks*t.
 // A deep, nearly unchanged bulk moisture approaches this independent
 // analytic curve; finite depth still evolves through the column equation.
 const double integrated=F-F0-B*std::log((F+B)/(F0+B));
 EXPECT_NEAR(integrated,p.Ks*100,1e-3*p.Ks*100);
 EXPECT_NEAR(st.continuityResidual(),0,1e-9);EXPECT_NEAR(st.led_reject,0,1e-12);
}
TEST(AquiferReceiving, TopLayerWettingAndRisingTableReduceCapacity) {
 Rig sigma;sigma.initialize(GwClosure::SIGMA,SoilChar::RUSSO);sigma.gw.publishInfiltration(sigma.surf,60,0);
 const double dry=sigma.surf.infil_rate[0];sigma.gw.state().theta_sigma[0]=.449;sigma.gw.publishInfiltration(sigma.surf,60,1);
 EXPECT_LT(sigma.surf.infil_rate[0],dry);
 Rig bulk;bulk.initialize(GwClosure::CLOSED_FORM,SoilChar::RUSSO);bulk.gw.state().hu[0]=.2;bulk.gw.state().wetting_front[0]=.01;
 bulk.gw.publishInfiltration(bulk.surf,3600,0);const double low=bulk.surf.infil_rate[0];
 bulk.gw.state().hg[0]=1.9;bulk.gw.state().hu[0]=.02;bulk.gw.publishInfiltration(bulk.surf,3600,1);
 EXPECT_LT(bulk.surf.infil_rate[0],low);
}

TEST(AquiferReceiving, ZeroFrontHasFiniteAnalyticFirstIntervalIntake) {
 Rig r;r.initialize(GwClosure::CLOSED_FORM,SoilChar::GARDNER,1,1e-5,1001);
 auto& st=r.gw.state();const double length=st.zs[0]-st.hg[0];st.hu[0]=.2*length;
 const double delta=.45-.2,suction=-std::log((.2-.1)/(.45-.1))/2,B=(r.surf.depth[0]+suction)*delta;
 r.gw.publishInfiltration(r.surf,100,0);
 const double F=r.gw.acceptSurfaceInfiltration(0,r.surf.infil_rate[0]*st.area[0]*100)/st.area[0];
 EXPECT_TRUE(std::isfinite(F));EXPECT_GT(F,1e-5*100);EXPECT_LT(F,.1);
 EXPECT_NEAR(F-B*std::log1p(F/B),1e-5*100,1e-14);
 EXPECT_LT(F*st.area[0],.01*r.gw.infiltrationHeadroom(0));
}
