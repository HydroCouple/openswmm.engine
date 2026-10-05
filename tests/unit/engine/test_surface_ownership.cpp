// SPDX-License-Identifier: Apache-2.0
#include <gtest/gtest.h>
#include "core/SWMMEngine.hpp"
#include "2d/subsurface/SurfaceOwnership.hpp"
#include "2d/subsurface/FootprintGeometry.hpp"
#include "input/geopackage/GeoPackageWriter.hpp"
#include <openswmm/engine/openswmm_surface_ownership.h>
#include <openswmm/engine/openswmm_model.h>
#include <openswmm/engine/openswmm_2d.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
using namespace openswmm;using namespace openswmm::twoD;
namespace {
const std::filesystem::path dir=OPENSWMM_R4_OUT;
std::string deck(){return
 "[OPTIONS]\nFLOW_UNITS CMS\nFLOW_ROUTING DYNWAVE\nINFILTRATION GREEN_AMPT\nSTART_DATE 10/05/2026\nEND_DATE 10/05/2026\nEND_TIME 00:05:00\nWET_STEP 00:01:00\nROUTING_STEP 1\nREPORT_STEP 00:01:00\n"
 "[JUNCTIONS]\nJ1 0 3\n[OUTFALLS]\nO1 -1 FREE NO\n[CONDUITS]\nC1 J1 O1 30 .013 0 0\n[XSECTIONS]\nC1 CIRCULAR .5 0 0 0 1\n"
 "[RAINGAGES]\nRG INTENSITY 0:05 1 TIMESERIES rain\n[TIMESERIES]\nrain 0:00 0\n[SUBCATCHMENTS]\nS1 RG J1 .1 40 20 .5 0\n[SUBAREAS]\nS1 .01 .1 .05 .05 25 OUTLET\n[INFILTRATION]\nS1 3.5 .5 .26\n"
 "[POLYGONS]\nS1 10 0\nS1 30 0\nS1 30 50\nS1 10 50\n"
 "[2D_OPTIONS]\nGROUNDWATER AUTO\nREPORT_2D NO\nMAX_TIMESTEP 1\n[2D_VERTICES]\n0 0 0\n20 0 0\n20 50 0\n0 50 0\n[2D_QUADS]\n0 1 2 3 .03\n[2D_AQUIFER_OPTIONS]\nMODE MESH\nNODE_ENROLMENT ROWS\n";}
struct Model {
 SWMM_Engine h=swmm_engine_create();~Model(){swmm_engine_close(h);swmm_engine_destroy(h);}
 SWMMEngine& e(){return *static_cast<SWMMEngine*>(h);}
 bool open(const std::string& name,const std::string& text=deck()){
  std::filesystem::create_directories(dir);const auto path=dir/(name+".inp");std::ofstream f(path);f<<text;f.close();
  return swmm_engine_open(h,path.string().c_str(),(dir/(name+".rpt")).string().c_str(),(dir/(name+".out")).string().c_str(),nullptr)==SWMM_OK;
 }
 SurfaceOwnershipPreview preview(std::vector<SurfaceOwnerRecord> rows={{"S1"}}){auto& r=e().surfaceRouter2D();return resolveSurfaceOwnership(e().context(),r.mesh(),r.options(),r.aquiferConfig(),rows);}
 void lid(){auto& c=e().context();c.lid_controls.names={"L1"};c.lid_controls.storage.resize(1);c.lid_controls.storage[0]={1,.4,1,0};c.lid_usage.subcatch_index.resize(1);c.lid_usage.lid_index.resize(1);c.lid_usage.number.resize(1);c.lid_usage.area.resize(1);c.lid_usage.from_imperv.resize(1);c.lid_usage.from_perv.resize(1);c.lid_usage.drain_to.resize(1);c.lid_usage.subcatch_index[0]=0;c.lid_usage.lid_index[0]=0;c.lid_usage.number[0]=2;c.lid_usage.area[0]=100;}
 int apply(const std::string& token,std::vector<int> rows={0}){char d[2048];return swmm_surface_owner_replace(h,rows.data(),int(rows.size()),token.c_str(),d,sizeof d);}
};
}
TEST(SurfaceOwnership, ReconcilesFootprintLidFirstAndExactHalfOutside){
 Model m;ASSERT_TRUE(m.open("half"));m.lid();const auto p=m.preview();ASSERT_TRUE(p.valid())<<(p.errors.empty()?"":p.errors[0]);ASSERT_EQ(p.objects.size(),1);ASSERT_EQ(p.shares.size(),1);
 const auto& o=p.objects[0];EXPECT_DOUBLE_EQ(o.declared_area,1000);EXPECT_DOUBLE_EQ(o.polygon_area,1000);EXPECT_DOUBLE_EQ(o.lid_area,200);EXPECT_DOUBLE_EQ(o.pervious_area,480);EXPECT_DOUBLE_EQ(o.impervious_area,320);EXPECT_DOUBLE_EQ(o.inside_area,500);EXPECT_DOUBLE_EQ(o.outside_area,500);
 const auto& s=p.shares[0];EXPECT_EQ(s.cell,0);EXPECT_DOUBLE_EQ(s.weather_area,500);EXPECT_DOUBLE_EQ(s.pervious_area,240);EXPECT_DOUBLE_EQ(s.impervious_area,160);EXPECT_DOUBLE_EQ(s.lid_area,100);EXPECT_DOUBLE_EQ(s.native_lid_area,100);EXPECT_DOUBLE_EQ(p.mesh_weather_area[0],500);
 m.e().context().lid_controls.storage[0][2]=0;const auto sealed=m.preview();EXPECT_DOUBLE_EQ(sealed.shares[0].native_lid_area,0);EXPECT_DOUBLE_EQ(sealed.shares[0].weather_area,500);
}
TEST(SurfaceOwnership, UsAndSiGeometryResolveTheSamePhysicalArea){
 Model m;ASSERT_TRUE(m.open("units"));m.lid();const auto si=m.preview();auto& c=m.e().context();c.options.flow_units=FlowUnits::CFS;c.subcatches.area[0]=1000/(43560*.3048*.3048);c.lid_usage.area[0]=100/(.3048*.3048);
 for(auto& x:c.spatial.subcatch_polygon_x[0])x/=.3048;for(auto& y:c.spatial.subcatch_polygon_y[0])y/=.3048;
 auto& r=m.e().surfaceRouter2D();for(auto& x:r.mesh().vx)x/=.3048;for(auto& y:r.mesh().vy)y/=.3048;
 const auto us=m.preview();ASSERT_TRUE(us.valid());EXPECT_NEAR(us.shares[0].weather_area,si.shares[0].weather_area,1e-10);EXPECT_NEAR(us.shares[0].pervious_area,240,1e-10);
 r.options().mesh_units_si=true;for(auto& x:r.mesh().vx)x*=.3048;for(auto& y:r.mesh().vy)y*=.3048;const auto mixed=m.preview();ASSERT_TRUE(mixed.valid());EXPECT_NEAR(mixed.mesh_weather_area[0],500,1e-10);
}
TEST(SurfaceOwnership, RejectsMismatchMissingGeometryAndOverbookedLid){
 Model m;ASSERT_TRUE(m.open("invalid"));auto& c=m.e().context();c.subcatches.area[0]=.2;EXPECT_FALSE(m.preview().valid());c.subcatches.area[0]=.1;m.lid();c.lid_usage.area[0]=600;EXPECT_FALSE(m.preview().valid());c.lid_usage.area[0]=100;c.spatial.subcatch_polygon_x[0].clear();c.spatial.subcatch_polygon_y[0].clear();EXPECT_FALSE(m.preview().valid());
}
TEST(SurfaceOwnership, ConcaveDisconnectedClipClosingVertexAndLargeCoordinates){
 Model m;ASSERT_TRUE(m.open("concave"));auto& c=m.e().context();c.spatial.subcatch_polygon_x[0]={10,30,30,20,20,10,10};c.spatial.subcatch_polygon_y[0]={0,0,10,10,50,50,0};c.subcatches.area[0]=.06;
 auto p=m.preview();ASSERT_TRUE(p.valid());EXPECT_NEAR(p.objects[0].inside_area,500,1e-9);EXPECT_NEAR(p.objects[0].outside_area,100,1e-9);
 for(auto& x:c.spatial.subcatch_polygon_x[0])x+=1e9;for(auto& y:c.spatial.subcatch_polygon_y[0])y+=1e9;auto& mesh=m.e().surfaceRouter2D().mesh();for(auto& x:mesh.vx)x+=1e9;for(auto& y:mesh.vy)y+=1e9;auto shifted=m.preview();ASSERT_TRUE(shifted.valid());EXPECT_DOUBLE_EQ(shifted.shares[0].weather_area,p.shares[0].weather_area);
 footprint::Ring u{{0,0},{10,0},{10,10},{7,10},{7,3},{3,3},{3,10},{0,10}};std::vector<footprint::Ring> tris;ASSERT_EQ(footprint::triangulate(u,tris),"");double a=0;for(const auto& t:tris)a+=footprint::intersectionArea(t,{{0,5},{10,5},{10,10},{0,10}});EXPECT_NEAR(a,30,1e-12);
}
TEST(SurfaceOwnership, RejectsSelfIntersectionAndInvalidCellWithoutSkipping){
 Model m;ASSERT_TRUE(m.open("self_cross"));auto& c=m.e().context();c.spatial.subcatch_polygon_x[0]={10,30,10,30};c.spatial.subcatch_polygon_y[0]={0,50,50,0};EXPECT_FALSE(m.preview().valid());
 c.spatial.subcatch_polygon_x[0]={10,30,30,10};c.spatial.subcatch_polygon_y[0]={0,0,50,50};m.e().surfaceRouter2D().mesh().set_quad(0,0,1,2,99);EXPECT_FALSE(m.preview().valid());
}
TEST(SurfaceOwnership, ConflictingPeersBlockEvenOutsideEditScope){
 auto text=deck();text.insert(text.find("[SUBAREAS]"),"S2 RG J1 .1 0 20 .5 0\n");text.insert(text.find("[INFILTRATION]"),"S2 .01 .1 .05 .05 25 OUTLET\n");text.insert(text.find("[POLYGONS]"),"S2 3.5 .5 .26\n");text.insert(text.find("[2D_OPTIONS]"),"S2 0 0\nS2 20 0\nS2 20 50\nS2 0 50\n");
 Model m;ASSERT_TRUE(m.open("peers",text));auto p=m.preview();EXPECT_FALSE(p.valid());EXPECT_FALSE(m.preview({{"S1"},{"S2"}}).valid());
 auto& c=m.e().context();c.spatial.subcatch_polygon_x[0]={20,40,40,20};auto touching=m.preview({{"S1"},{"S2"}});EXPECT_TRUE(touching.valid())<<(touching.errors.empty()?"":touching.errors[0]);
 c.spatial.subcatch_polygon_x[0]={10,30,30,10};c.spatial.subcatch_polygon_x[1]={0,10,10,0};c.subcatches.area[1]=.05;EXPECT_FALSE(m.preview().valid());{auto both=m.preview({{"S1"},{"S2"}});EXPECT_TRUE(both.valid())<<(both.errors.empty()?"":both.errors[0]);}
}
TEST(SurfaceOwnership, StaleTokenAtomicityAbsenceAndLumpedWeather){
 Model m;ASSERT_TRUE(m.open("stale"));auto p=m.preview();int count=-1;ASSERT_EQ(swmm_surface_owner_get(m.h,nullptr,0,&count),SWMM_OK);EXPECT_EQ(count,0);
 m.e().context().subcatches.frac_imperv[0]=.3;EXPECT_EQ(m.apply(p.token),SWMM_ERR_BADPARAM);EXPECT_TRUE(m.e().surfaceRouter2D().aquiferConfig().surface_owners.empty());p=m.preview();ASSERT_EQ(m.apply(p.token),SWMM_OK);EXPECT_EQ(m.e().surfaceRouter2D().aquiferConfig().surface_owners.size(),1);
 EXPECT_EQ(m.apply(p.token),SWMM_ERR_BADPARAM);p=m.preview();ASSERT_EQ(m.apply(p.token,{}),SWMM_OK);EXPECT_TRUE(m.e().surfaceRouter2D().aquiferConfig().surface_owners.empty());
 m.e().context().subcatches.gw_aquifer[0]=0;const auto lumped=m.preview();EXPECT_TRUE(lumped.objects[0].lumped);EXPECT_DOUBLE_EQ(lumped.mesh_weather_area[0],500);
}
TEST(SurfaceOwnership, PublicPreviewDoesNotMutateAndCanCancel){
 Model m;ASSERT_TRUE(m.open("api"));double x[4],y[4],z[4];ASSERT_EQ(swmm_2d_vertex_get_xyz_bulk(m.h,x,y,z),SWMM_OK);EXPECT_DOUBLE_EQ(x[1],20);int on=0,sn=0,cn=0,valid=0;char token[17],diag[1024];int rows[]={0};
 ASSERT_EQ(swmm_surface_owner_preview(m.h,rows,1,nullptr,0,&on,nullptr,0,&sn,nullptr,0,&cn,&valid,token,17,diag,1024,nullptr,nullptr),SWMM_OK);EXPECT_TRUE(valid);EXPECT_EQ(on,1);EXPECT_EQ(sn,1);EXPECT_EQ(cn,1);EXPECT_TRUE(m.e().surfaceRouter2D().aquiferConfig().surface_owners.empty());
 SWMM_SurfaceOwnerObject o{};SWMM_SurfaceOwnerShare s{};double a=0;ASSERT_EQ(swmm_surface_owner_preview(m.h,rows,1,&o,1,&on,&s,1,&sn,&a,1,&cn,&valid,token,17,diag,1024,nullptr,nullptr),SWMM_OK);EXPECT_DOUBLE_EQ(a,500);EXPECT_STREQ(o.name,"S1");
 EXPECT_NE(swmm_surface_owner_preview(m.h,rows,1,nullptr,0,&on,nullptr,0,&sn,nullptr,0,&cn,&valid,token,17,diag,1024,[](int,int,void*){return 0;},nullptr),SWMM_OK);EXPECT_FALSE(valid);EXPECT_TRUE(m.e().surfaceRouter2D().aquiferConfig().surface_owners.empty());
}
TEST(SurfaceOwnership, InpAndGeoPackagePreserveAuthoredReviewAndAquifer){
 Model m;ASSERT_TRUE(m.open("persist"));auto& cfg=m.e().surfaceRouter2D().aquiferConfig();GwAquiferRow r;r.Ks=.123456789012345;r.zs=2;cfg.rows.push_back(r);ASSERT_EQ(m.apply(m.preview().token),SWMM_OK);
 const auto inp=dir/"reviewed.inp";ASSERT_EQ(swmm_model_write(m.h,inp.string().c_str()),SWMM_OK);
 Model reload;ASSERT_EQ(swmm_engine_open(reload.h,inp.string().c_str(),(dir/"reviewed.rpt").string().c_str(),(dir/"reviewed.out").string().c_str(),nullptr),SWMM_OK);EXPECT_EQ(reload.e().surfaceRouter2D().aquiferConfig().surface_owners,cfg.surface_owners);
 const auto gpkg=dir/"reviewed.gpkg";for(const auto& suffix:{"","-wal","-shm"})std::filesystem::remove(gpkg.string()+suffix);ASSERT_EQ(swmm_model_write_with_plugin(m.h,gpkg.string().c_str(),"org.hydrocouple.openswmm.plugins.geopackage"),0);
 Model g;const int opened=swmm_engine_open(g.h,gpkg.string().c_str(),(dir/"gpkg.rpt").string().c_str(),(dir/"gpkg.out").string().c_str(),"org.hydrocouple.openswmm.plugins.geopackage");ASSERT_EQ(opened,SWMM_OK)<<g.e().context().error_message;const auto& gc=g.e().surfaceRouter2D().aquiferConfig();EXPECT_EQ(gc.surface_owners,cfg.surface_owners);ASSERT_EQ(gc.rows.size(),1);EXPECT_DOUBLE_EQ(gc.rows[0].Ks,r.Ks);{auto p=g.preview();EXPECT_TRUE(p.valid())<<(p.errors.empty()?"":p.errors[0]);}
 EXPECT_NE(swmm_engine_initialize(g.h),SWMM_OK);EXPECT_TRUE(g.e().context().error_message.find("completed-interval")!=std::string::npos);
}
