#include <iostream>
#include <vector>
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/mesh/VertexReconstruction.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/coupling/NodeCoupling.hpp"
using namespace openswmm::twoD;
int main(){MeshData m;m.resize_vertices(5);m.vx={0,1,0,-1,0};m.vy={0,0,1,0,-1};m.resize_triangles(2);m.set_triangle(0,0,1,2);m.set_triangle(1,0,3,4);buildMeshTopology(m);buildVertexStencils(m);SurfaceStateData s;s.resize(2,5);s.volume={.1,1.};SolverOptions2D o;o.num_threads=1;o.lts_tiers=1;o.max_timestep=1;ExplicitInertialSolver solver;solver.initialize(m,s,o);CouplingPoint cp{};cp.vertex_idx=0;cp.cell_idx=0;cp.is_outfall=true;std::vector<CouplingPoint> cps{cp};injectAccumulatedExchange(cps,m,s,std::vector<double>{-1.1},1,1);solver.advance(0,1);double v=s.volume[0]+s.volume[1], applied=-(s.coupling_applied[0]+s.coupling_applied[1]);std::cout.precision(17);std::cout<<"initial=1.1 requested_withdrawal=1.1 actual_withdrawal="<<applied<<" final_storage="<<v<<" reported_residual="<<-v<<'\n';}
