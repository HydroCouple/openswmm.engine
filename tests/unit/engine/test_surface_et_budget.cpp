// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "2d/data/MeshData.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/solver/InertialEdges.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/subsurface/SubsurfaceSections.hpp"
#include "core/SWMMEngine.hpp"
#include "core/HotStartManager.hpp"
#include "2d/SurfaceRouter2D.hpp"
#include <openswmm/engine/openswmm_gw2d.h>
#include <openswmm/engine/openswmm_model.h>
#include <cmath>
#include <filesystem>
#include <fstream>
using namespace openswmm;using namespace openswmm::twoD;
namespace {
struct Rig {
 MeshData mesh;InertialEdges edges;SurfaceStateData surf;SolverOptions2D opts;SubsurfaceConfig cfg;SubsurfaceSolver gw;
 void initialize(GwClosure cl,SoilChar law,const std::string& et="BOTH",double wilting=150.,GwUnitFactors units={}) {
  mesh.resize_vertices(3);mesh.vx={0,2,0};mesh.vy={0,0,2};mesh.vz={10,10,10};mesh.resize_triangles(1);mesh.set_triangle(0,0,1,2);mesh.tri_area[0]=2;mesh.tri_cz[0]=10;for(int k=0;k<3;++k)mesh.cell_nbr[MeshData::slot(0,k)]=-1;edges.build(mesh);surf.resize(1,3);
  cfg.options.closure=cl;cfg.options.force_closed_form=true;cfg.options.soil_char=law;cfg.options.gw_et=et;cfg.options.wilting_suction_set=true;cfg.options.wilting_suction=wilting;
  GwAquiferRow row;row.Ks=0;row.zs=2/units.length;row.hg0=1/units.length;row.alpha=2/units.inv_len;row.psi_b=.2/units.length;cfg.rows={row};std::vector<std::string> w;ASSERT_EQ(gw.initialize(mesh,edges,opts,units,0,cfg,w),"");gw.assignTiers(1,1);
 }
 void fire(double dt=1,double t=0){gw.fireGwCells(0,dt,surf,t);}
};
void balance(const SubsurfaceState& s){EXPECT_NEAR(s.et_potential_cumulative[0],s.et_surface_cumulative[0]+s.et_soil_cumulative[0]+s.et_unused_cumulative[0]+s.et_pending[0],1e-13);EXPECT_LE(s.et_surface_cumulative[0]+s.et_soil_cumulative[0],s.et_potential_cumulative[0]+1e-13);}
}
TEST(SurfaceEt, PondedSurfaceConsumesOneWholeDemandBeforeAllClosures){
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}){
  Rig r;r.initialize(cl,law);auto& s=r.gw.state();const double initial=s.storage();
  for(int n=0;n<20;++n)r.gw.bookSurfaceEt(0,.5,1e-5,1e-5); // A=2, whole demand removed by surface
  r.fire(10);EXPECT_DOUBLE_EQ(s.led_et,0);EXPECT_NEAR(s.storage(),initial,1e-12);balance(s);
 }
}
TEST(SurfaceEt, AreaBudgetsExpireIneligibleDemandAndUseOneExtractionStress){
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}){
  Rig area,reference;area.initialize(cl,law,"BOUNDARY_ET",5);reference.initialize(cl,law,"BOUNDARY_ET",5);
  auto& s=area.gw.state();const double initial=s.storage();
  // Two thirds of the residual belong to sealed/impervious areas, and expire.
  area.gw.bookAreaEt(0,10,2e-4,5e-5,5e-5);balance(s);
  EXPECT_NEAR(s.et_unused_cumulative[0],1e-4,1e-15);EXPECT_NEAR(s.et_pending[0],5e-5,1e-15);
  reference.gw.bookSurfaceEt(0,10,2.5e-6,0);area.fire(10);reference.fire(10);
  EXPECT_NEAR(s.led_et,reference.gw.state().led_et,1e-13);EXPECT_NEAR(initial-s.storage(),s.led_et,1e-12);
  EXPECT_NEAR(s.et_refresh[0],10,1e-15);EXPECT_NEAR(s.continuityResidual(),0,1e-12);balance(s);
 }
}
TEST(SurfaceEt, InvalidAreaBudgetCannotPartiallyBookDemand){
 Rig r;r.initialize(GwClosure::CLOSED_FORM,SoilChar::GARDNER);auto& s=r.gw.state();
 for(const auto& amounts:{std::array<double,3>{1,2,0},{1,0,2},{-1,0,0},{1,-1,0},{1,0,-1}}){
  EXPECT_THROW(r.gw.bookAreaEt(0,1,amounts[0],amounts[1],amounts[2]),std::invalid_argument);
  EXPECT_DOUBLE_EQ(s.et_pending[0],0);EXPECT_DOUBLE_EQ(s.et_potential_cumulative[0],0);EXPECT_TRUE(std::isnan(s.et_refresh[0]));
 }
}
TEST(SurfaceEt, DryAndPartialPondDemandUsesExactlyOneStressAtExtraction){
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}){
  SCOPED_TRACE(std::to_string(int(cl))+"/"+std::to_string(int(law)));Rig r;r.initialize(cl,law,"BOUNDARY_ET",5);auto& s=r.gw.state();auto p=soil::Params{};p.law=law;
  const double L=s.zs[0]-s.hg[0];const double theta=cl==GwClosure::SIGMA?s.theta_sigma[0]:s.hu[0]/L;const double se=(theta-s.theta_r[0])/(s.theta_s[0]-s.theta_r[0]);const double psi=cl==GwClosure::ENSLAVED?L:soil::suctionAtSaturation(p,std::clamp(se,1e-6,1.));const double stress=soil::feddesStress(psi,5);
  const double initial=s.storage();r.gw.bookSurfaceEt(0,10,1e-5,4e-5);const double budget=1.6e-4;r.fire(10);
  EXPECT_NEAR(s.et_stress[0],stress,1e-12);EXPECT_NEAR(s.led_et,budget*stress,1e-12);EXPECT_NEAR(initial-s.storage(),s.led_et,1e-12);EXPECT_NEAR(s.continuityResidual(),0,1e-12);balance(s);
 }
}
TEST(SurfaceEt, CadenceAndRateRefreshCannotResetOrBorrowDemand){
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA}){
  Rig r;r.initialize(cl,SoilChar::GARDNER);auto& s=r.gw.state();r.fire();EXPECT_DOUBLE_EQ(s.led_et,0);
  double time=0;
  for(int cycle=0;cycle<12;++cycle){for(int k=0;k<7;++k){const double rate=k%2?2e-5:1e-5;const double surface=k%3?rate*.25:0;r.gw.bookSurfaceEt(0,.2,rate,surface);r.gw.publishInfiltration(r.surf,3,time);time+=.2;balance(s);}r.fire(1.4,time-1.4);balance(s);const double spent=s.led_et;r.fire(.1,time);EXPECT_DOUBLE_EQ(s.led_et,spent);}
  EXPECT_NEAR(s.continuityResidual(),0,1e-12);
 }
}
TEST(SurfaceEt, StressAndDonorCapsStopWithdrawalAtWilting){
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}){
  Rig r;r.initialize(cl,law,"BOTH",1e-6);r.gw.bookSurfaceEt(0,100,1,0);r.fire(100);EXPECT_DOUBLE_EQ(r.gw.state().led_et,0);balance(r.gw.state());
 }
 Rig r;r.initialize(GwClosure::SIGMA,SoilChar::GARDNER);auto& s=r.gw.state();for(double& theta:s.theta_sigma)theta=s.theta_r[0];s.hu[0]=s.theta_r[0];r.gw.bookSurfaceEt(0,1,1,0);r.fire();EXPECT_DOUBLE_EQ(s.led_et,0);balance(s);
}
TEST(SurfaceEt, USAndSIWiltingSuctionAreTheSamePhysicalParameter){
 for(auto cl:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED,GwClosure::SIGMA})for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}){
  Rig si,us;si.initialize(cl,law);GwUnitFactors units;units.length=.3048;units.inv_len=1/.3048;us.initialize(cl,law,"BOTH",150/.3048,units);EXPECT_NEAR(us.gw.options().wilting_suction,150,1e-12);
  for(int j=0;j<40;++j){for(Rig* r:{&si,&us}){r->gw.bookSurfaceEt(0,1,1e-5,0);r->fire(1,j);}EXPECT_NEAR(si.gw.state().led_et,us.gw.state().led_et,1e-12);EXPECT_NEAR(si.gw.state().storage(),us.gw.state().storage(),1e-12);}
 }
}
TEST(SurfaceEt, AutomaticDefaultsAndExplicitOptOutsRemainDistinct){
 GwOptions o;EXPECT_EQ(o.gw_et,"AUTO");EXPECT_EQ(o.link_seepage,GwLinkMode::DEFAULT);EXPECT_FALSE(o.wilting_suction_set);
 EXPECT_TRUE(parseAquiferOptionsLine({"GW_ET","NONE"},o).empty());EXPECT_EQ(o.gw_et,"NONE");EXPECT_TRUE(parseAquiferOptionsLine({"LINK_SEEPAGE","AUTO"},o).empty());EXPECT_EQ(o.link_seepage,GwLinkMode::AUTO);
 EXPECT_TRUE(parseAquiferOptionsLine({"LINK_SEEPAGE","DEFAULT"},o).empty());EXPECT_EQ(o.link_seepage,GwLinkMode::DEFAULT);
 for(const char* v:{"0","-1","nan","inf"})EXPECT_FALSE(parseAquiferOptionsLine({"WILTING_SUCTION",v},o).empty());
 Rig r;r.initialize(GwClosure::CLOSED_FORM,SoilChar::GARDNER,"AUTO");EXPECT_EQ(r.gw.options().gw_et,"BOTH");EXPECT_EQ(r.gw.options().link_seepage,GwLinkMode::TWO_WAY);
 Rig none;none.initialize(GwClosure::CLOSED_FORM,SoilChar::GARDNER,"NONE");none.gw.bookSurfaceEt(0,1,1,0);none.fire();EXPECT_DOUBLE_EQ(none.gw.state().led_et,0);balance(none.gw.state());
}
TEST(SurfaceEt, EquilibriumEtCannotSpendAnAlreadyCommittedNodeWithdrawal){
 Rig r;GwNodeBed bed;bed.node=0;bed.cell=0;bed.area=1;r.cfg.node_beds.push_back(bed);r.initialize(GwClosure::ENSLAVED,SoilChar::GARDNER);
 auto& s=r.gw.state();soil::Params p;p.law=SoilChar::GARDNER;const double donor=(s.theta_s[0]*s.hg[0]+s.hu[0]-soil::equilibriumStorage(p,s.zs[0]))*s.area[0];
 const double committed=donor*.4;s.nacc[0]=committed;const double before=s.storage();r.gw.bookSurfaceEt(0,1,10,0);r.fire();
 EXPECT_NEAR(s.led_node,committed,1e-13);EXPECT_NEAR(s.led_et,donor-committed,1e-13);EXPECT_NEAR(before-s.storage(),s.led_et+s.led_node,1e-13);EXPECT_NEAR(s.continuityResidual(),0,1e-13);balance(s);
}
TEST(SurfaceEt, ProcessOptionsValidateAtomicallyAndPreserveExplicitDefaults){
 auto handle=swmm_engine_new();ASSERT_TRUE(handle);ASSERT_EQ(swmm_gw2d_process_options_set(handle,"NONE","ONE_WAY","AUTO",1),SWMM_OK);
 EXPECT_EQ(swmm_gw2d_process_options_set(handle,"BOTH","TWO_WAY","0",1),SWMM_ERR_BADPARAM);char b[80]{};ASSERT_EQ(swmm_gw2d_option_get(handle,"GW_ET",b,sizeof b),SWMM_OK);EXPECT_STREQ(b,"NONE");ASSERT_EQ(swmm_gw2d_option_get(handle,"LINK_SEEPAGE",b,sizeof b),SWMM_OK);EXPECT_STREQ(b,"ONE_WAY");swmm_engine_destroy(handle);
}

namespace {
struct Model {
 SWMM_Engine h=swmm_engine_create();~Model(){swmm_engine_close(h);swmm_engine_destroy(h);}SWMMEngine& engine(){return *static_cast<SWMMEngine*>(h);}
 bool open(const std::string& name,const std::string& closure,double pond){
  const auto dir=std::filesystem::path(OPENSWMM_R3_OUT);std::filesystem::create_directories(dir);const auto path=dir/(name+".inp");std::ofstream f(path);
  f<<"[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nSTART_DATE 10/05/2026\nEND_DATE 10/05/2026\nEND_TIME 00:05:00\nWET_STEP 00:01:00\nROUTING_STEP 1\nREPORT_STEP 00:01:00\n[JUNCTIONS]\nJ1 0 3\n[OUTFALLS]\nO1 -1 FREE NO\n[CONDUITS]\nC1 J1 O1 30 .013 0 0\n[XSECTIONS]\nC1 CIRCULAR .5 0 0 0 1\n[EVAPORATION]\nCONSTANT 10\n[2D_OPTIONS]\nEVAPORATION CLIMATE\nINFILTRATION NO\nMAX_TIMESTEP 1\nLTS_TIERS 3\nREPORT_2D YES\nREPORT_2D_VARIABLES ALL\nOUTPUT_FILE "<<(dir/(name+".2d.h5")).string()<<"\n[2D_VERTICES]\n0 0 0\n10 0 0\n0 10 0\n[2D_TRIANGLES]\n0 1 2 .03 "<<pond<<"\n[2D_AQUIFER_OPTIONS]\nCLOSURE "<<closure<<"\nFORCE_CLOSED_FORM YES\nNODE_ENROLMENT ROWS\n[2D_AQUIFER]\n* 1e-12 2 .45 .1 2 HG0 1\n";f.close();return swmm_engine_open(h,path.string().c_str(),(dir/(name+".rpt")).string().c_str(),(dir/(name+".out")).string().c_str(),nullptr)==SWMM_OK;
 }
};
}
TEST(SurfaceEt, ActualModelIntegratesOneAreaDemandAndWritesTheBudget){
 for(const std::string cl:{"CLOSED_FORM","SIGMA","ENSLAVED"})for(double pond:{0.,.1}){
  Model m;ASSERT_TRUE(m.open("et_"+cl+(pond>0?"_ponded":"_dry"),cl,pond));ASSERT_EQ(swmm_engine_initialize(m.h),SWMM_OK);auto& a=m.engine().surfaceRouter2D();const double initial=a.subsurface().state().storage();ASSERT_EQ(swmm_engine_start(m.h,1),SWMM_OK);double elapsed=0;
  do{ASSERT_EQ(swmm_engine_step(m.h,&elapsed),SWMM_OK);}while(elapsed>0);const auto& s=a.subsurface().state();const double independent=10./1000/86400*300*50;
  EXPECT_NEAR(s.et_potential_cumulative[0],independent,1e-12);EXPECT_NEAR(s.et_surface_cumulative[0],a.state().evap_loss_total,1e-12);balance(s);EXPECT_NEAR(initial-s.storage(),s.led_et,1e-10);EXPECT_NEAR(s.continuityResidual(),0,1e-10);
  if(pond>0){EXPECT_NEAR(s.et_surface_cumulative[0],independent,1e-12);EXPECT_DOUBLE_EQ(s.led_et,0);}else{EXPECT_GT(s.led_et,0);EXPECT_DOUBLE_EQ(s.et_surface_cumulative[0],0);}
  ASSERT_EQ(swmm_engine_end(m.h),SWMM_OK);
 }
}
TEST(SurfaceEt, RestartPreservesUnspentPastDemandWithoutReplay){
 Model a,b;ASSERT_TRUE(a.open("restart_original","SIGMA",0));ASSERT_TRUE(b.open("restart_resumed","SIGMA",0));ASSERT_EQ(swmm_engine_initialize(a.h),SWMM_OK);ASSERT_EQ(swmm_engine_initialize(b.h),SWMM_OK);auto& x=a.engine().surfaceRouter2D().subsurface();auto& y=b.engine().surfaceRouter2D().subsurface();x.bookSurfaceEt(0,7,1e-5,.001);
 const auto path=(std::filesystem::path(OPENSWMM_R3_OUT)/"pending_et.hsf").string();std::unique_ptr<HotStartFile> save(HotStartManager::save(a.engine().context(),path));ASSERT_TRUE(save);EXPECT_EQ(save->header.version,13u);std::unique_ptr<HotStartFile> loaded(HotStartManager::open(path));ASSERT_TRUE(loaded);
 auto malformed=*loaded;malformed.gw_interface.back().push_back(1);EXPECT_NE(HotStartManager::apply(malformed,b.engine().context()),0);EXPECT_DOUBLE_EQ(y.state().et_pending[0],0);ASSERT_EQ(HotStartManager::apply(*loaded,b.engine().context()),0);EXPECT_EQ(x.state().et_pending,y.state().et_pending);
 x.assignTiers(1,1);y.assignTiers(1,1);for(int j=0;j<10;++j){for(auto* m:{&a,&b}){auto& router=m->engine().surfaceRouter2D();router.subsurface().fireGwCells(0,1,router.state(),j);}EXPECT_EQ(x.state().hg,y.state().hg);EXPECT_EQ(x.state().hu,y.state().hu);EXPECT_EQ(x.state().et_soil_cumulative,y.state().et_soil_cumulative);EXPECT_EQ(x.state().et_unused_cumulative,y.state().et_unused_cumulative);balance(x.state());}const double spent=x.state().led_et;x.fireGwCells(0,1,a.engine().surfaceRouter2D().state(),10);EXPECT_DOUBLE_EQ(x.state().led_et,spent);
}
