#include "2d/solver/InertialEdges.hpp"
#include "2d/data/SolverOptions2D.hpp"
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/subsurface/SubsurfaceSolver.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include <iostream>
#include <iomanip>
using namespace openswmm::twoD;
int main(){
 for(auto law:{SoilChar::GARDNER,SoilChar::RUSSO,SoilChar::BROOKS_COREY,SoilChar::VAN_GENUCHTEN})for(double dt:{10.,1.,.1})for(bool demand:{false,true}){
 MeshData mesh;mesh.resize_vertices(3);mesh.vx={0,4,0};mesh.vy={0,0,2};mesh.vz={10,10,10};mesh.resize_triangles(1);mesh.set_triangle(0,0,1,2);mesh.mannings_n[0]=.03;buildMeshTopology(mesh);
 InertialEdges edges;edges.build(mesh);SurfaceStateData surf;surf.resize(1,mesh.n_edge_slots());SolverOptions2D options;SubsurfaceConfig cfg;cfg.options.authored=true;cfg.options.force_closed_form=true;cfg.options.gw_et="BOUNDARY_ET";cfg.options.closure=GwClosure::SIGMA;cfg.options.soil_char=law;
 GwAquiferRow row;row.Ks=.01;row.zs=2;row.hg0=1;cfg.rows.push_back(row);SubsurfaceSolver gw;std::vector<std::string> warnings;
 auto error=gw.initialize(mesh,edges,options,{},0,cfg,warnings);if(!error.empty())return 1;
 gw.assignTiers(dt,1);if(demand)gw.bookSurfaceEt(0,dt,4.0476e-6,0);const double initial=gw.state().storage();gw.fireGwCells(0,dt,surf,dt);
 std::cout<<std::setprecision(17)<<"{\"law\":"<<int(law)<<",\"dt\":"<<dt<<",\"demand\":"<<(demand?"true":"false")<<",\"initial\":"<<initial<<",\"storage\":"<<gw.state().storage()<<",\"soil_et\":"<<gw.state().led_et<<",\"residual\":"<<gw.state().continuityResidual()<<"}\n";
 }
}
