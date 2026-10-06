#include "2d/solver/InertialEdges.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <string>
using namespace openswmm::twoD;
struct Rig {
 MeshData mesh;InertialEdges edges;SurfaceStateData surface;SubsurfaceSolver gw;
 void init(GwClosure closure,SoilChar law,double ks=1e-4){
  mesh.resize_vertices(3);mesh.vx={0,4,0};mesh.vy={0,0,2};mesh.vz={10,10,10};
  mesh.resize_triangles(1);mesh.set_triangle(0,0,1,2);mesh.mannings_n[0]=.03;buildMeshTopology(mesh);
  edges.build(mesh);surface.resize(1,mesh.n_edge_slots());SolverOptions2D options;SubsurfaceConfig cfg;
  cfg.options.authored=true;cfg.options.force_closed_form=true;cfg.options.gw_et="BOUNDARY_ET";
  cfg.options.closure=closure;cfg.options.soil_char=law;
  GwAquiferRow row;row.Ks=ks;row.zs=2;row.hg0=1;cfg.rows={row};std::vector<std::string> warnings;
  const auto error=gw.initialize(mesh,edges,options,{},0,cfg,warnings);if(!error.empty())throw std::runtime_error(error);
 }
 void run(double dt,int count,bool forced){
  gw.assignTiers(dt,1);
  for(int k=0;k<count;++k){
   if(forced){gw.state().xacc_from_surface[0]+=.0004*dt;gw.bookSurfaceEt(0,dt,1e-5,0);gw.bookLinkSeepage(0,.0001*dt);}
   gw.fireGwCells(0,dt,surface,k*dt);
  }
 }
 void print(){const auto& s=gw.state();std::cout<<",\"hg\":"<<s.hg[0]<<",\"hu\":"<<s.hu[0]<<",\"storage\":"<<s.storage()<<",\"et\":"<<s.led_et<<",\"dunne\":"<<s.led_dunne<<",\"residual\":"<<s.continuityResidual()<<",\"layers\":[";
 for(std::size_t j=0;j<s.theta_sigma.size();++j){if(j)std::cout<<",";std::cout<<s.theta_sigma[j];}std::cout<<"]}"<<std::endl;}
};
int main(int argc,char**argv){
 const std::string mode=argc>1?argv[1]:"controls";std::cout<<std::setprecision(17);
 for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN}){
  if(mode=="controls")for(auto closure:{GwClosure::CLOSED_FORM,GwClosure::ENSLAVED})for(bool forced:{false,true}){
   Rig r;r.init(closure,law);r.run(.1,160,forced);std::cout<<"{\"mode\":\"controls\",\"law\":"<<int(law)<<",\"closure\":"<<int(closure)<<",\"forced\":"<<(forced?"true":"false");r.print();
  }
  if(mode=="refinement")for(double dt:{.2,.1,.05,.025,.00625}){
   Rig r;r.init(GwClosure::SIGMA,law);r.run(dt,int(16/dt),true);std::cout<<"{\"mode\":\"refinement\",\"law\":"<<int(law)<<",\"dt\":"<<dt;r.print();
  }
  if(mode=="cost")for(double ks:{1e-4,.01})for(int repeat=0;repeat<5;++repeat){
   Rig r;r.init(GwClosure::SIGMA,law,ks);const auto start=std::chrono::steady_clock::now();r.run(.1,5000,true);
   const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
   std::cout<<"{\"mode\":\"cost\",\"law\":"<<int(law)<<",\"ks\":"<<ks<<",\"repeat\":"<<repeat<<",\"steps\":5000,\"seconds\":"<<elapsed;r.print();
  }
 }
}
