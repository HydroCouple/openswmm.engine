// Fresh SWASHES equations (Delestre et al., arXiv:1110.0288, sections 3.1, 4.1, 4.2).
// References use the engine's gravity so this measures numerical error, not a g mismatch.
#include "2d/mesh/MeshBuilder.hpp"
#include "2d/data/SurfaceStateData.hpp"
#include "2d/solver/ExplicitInertialSolver.hpp"
#include "2d/solver/InertialKernels.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numeric>
#include <string>
#include <sys/resource.h>
using namespace openswmm::twoD;
constexpr double g=9.80665, pi=3.14159265358979323846;
struct Reference {double h,u,v;};
int main(int argc,char**argv) {
 if(argc<8)return 2;
 std::string kind=argv[1];int n=std::atoi(argv[2]), tiers=std::atoi(argv[3]),order=std::atoi(argv[4]),threads=std::atoi(argv[5]),quad=std::atoi(argv[6]);
 double duration=std::atof(argv[7]);
 bool bowl=kind=="radial"||kind=="planar", lake=kind=="lake_wet"||kind=="lake_dry";
 double L=bowl?4:lake?25:32;
 // Shock/rarefaction tests are uniform across a four-cell-wide channel.
 int ny=bowl?n:4;double dx=L/n, Ly=ny*dx;
 double omega=std::sqrt((kind=="radial"?8:2)*g*.1);
 double end=bowl?duration*2*pi/omega:duration;
 double hs=.0,us=.0,cs=.0,shock=.0,c0=std::sqrt(g*.2);
 if(kind=="stoker"){
  double lo=.05,hi=.2;
  for(int j=0;j<100;++j){double h=(lo+hi)/2;double f=2*(c0-std::sqrt(g*h))-(h-.05)*std::sqrt(g*.5*(1/h+1/.05));if(f>0)lo=h;else hi=h;}
  hs=(lo+hi)/2;us=2*(c0-std::sqrt(g*hs));cs=std::sqrt(g*hs);shock=hs*us/(hs-.05);
 }
 auto bed=[&](double x,double y){if(bowl)return .1*((x-2)*(x-2)+(y-2)*(y-2)-1);if(lake&&x>8&&x<12)return .2-.05*(x-10)*(x-10);return 0.;};
 auto exact=[&](double x,double y,double t)->Reference{
  if(lake)return {std::max(0.,(kind=="lake_wet"?.5:.1)-bed(x,y)),0,0};
  if(bowl){x-=2;y-=2;
   if(kind=="planar")return {std::max(0.,.1*(1-std::pow(x-.5*std::cos(omega*t),2)-std::pow(y-.5*std::sin(omega*t),2))),-.5*omega*std::sin(omega*t),.5*omega*std::cos(omega*t)};
   double A=(1-.8*.8)/(1+.8*.8),den=1-A*std::cos(omega*t),B=std::sqrt(1-A*A)/den,vel=.5*omega*A*std::sin(omega*t)/den;
   return {std::max(0.,.1*(B-(x*x+y*y)*B*B)),vel*x,vel*y};
  }
  if(t==0)return {x<L/2?.2:kind=="stoker"?.05:0.,0,0};
  double xi=(x-L/2)/t;
  if(xi<-c0)return {.2,0,0};
  if(kind=="stoker"){
   if(xi<us-cs)return {std::pow(2*c0-xi,2)/(9*g),2*(c0+xi)/3,0};
   if(xi<shock)return {hs,us,0};return {.05,0,0};
  }
  if(xi>2*c0)return {0,0,0};return {std::pow(2*c0-xi,2)/(9*g),2*(c0+xi)/3,0};
 };
 const double skew=std::getenv("REVIEW_SKEW")?std::atof(std::getenv("REVIEW_SKEW")):0.0;
 MeshData m;m.resize_vertices((n+1)*(ny+1));
 for(int y=0;y<=ny;++y)for(int x=0;x<=n;++x){int i=y*(n+1)+x;const bool inner=x>0&&x<n&&y>0&&y<ny;
  m.vx[i]=x*dx+(inner?skew*dx*std::sin(.7*x+1.1*y):0);m.vy[i]=y*dx+(inner?skew*dx*std::cos(1.3*x-.4*y):0);m.vz[i]=bed(m.vx[i],m.vy[i]);}
 int nc=0;for(int y=0;y<ny;++y)for(int x=0;x<n;++x)nc+=(quad==1||(quad==2&&(x+y)%2))?1:2;
 m.resize_triangles(nc);int cell=0;
 for(int y=0;y<ny;++y)for(int x=0;x<n;++x){int a=y*(n+1)+x,b=a+1,c=a+n+1,d=c+1;if(quad==1||(quad==2&&(x+y)%2))m.set_quad(cell++,a,b,d,c);else{m.set_triangle(cell++,a,b,d);m.set_triangle(cell++,a,d,c);}}
 buildMeshTopology(m);if(!validateMesh(m).empty())return 3;
 SolverOptions2D o;o.num_threads=threads;o.momentum=Momentum2D::FULL_SWE;o.lts_tiers=tiers;o.reconstruction_order=order;o.cfl_number=.4;o.max_timestep=.25;o.dry_depth=1e-7;o.h_move=1e-6;
 if(const char* v=std::getenv("REVIEW_CFL"))o.cfl_number=std::atof(v);
 if(const char* v=std::getenv("REVIEW_DRY"))o.dry_depth=std::atof(v);
 if(const char* v=std::getenv("REVIEW_HMOVE"))o.h_move=std::atof(v);
 if(const char* v=std::getenv("REVIEW_DT"))o.max_timestep=std::atof(v);
 if(const char* v=std::getenv("REVIEW_BETA"))o.exchange_beta=std::atof(v);
 SurfaceStateData s;s.resize(m.n_cells(),m.n_vertices());
 for(int i=0;i<m.n_cells();++i){auto r=exact(m.tri_cx[i],m.tri_cy[i],0);m.mannings_n[i]=0;m.tri_init_u[i]=r.u;m.tri_init_v[i]=r.v;
  s.volume[i]=lake?inertial::cellVolumeFromEta(m,o,i,kind=="lake_wet"?.5:.1):r.h*m.tri_area[i];}
 auto initial=s.volume;double v0=std::accumulate(initial.begin(),initial.end(),0.);
 ExplicitInertialSolver solver;solver.initialize(m,s,o);
 FILE* history=nullptr;if(const char* path=std::getenv("REVIEW_HISTORY")){history=std::fopen(path,"w");if(!history)return 5;}
 auto diagnostic=[&](double t){
  double volume=0,energy=0,exact_energy=0,cx=0,cy=0,r2=0,px=0,py=0,l1=0,ref=0,shore=0,velerr=0,maxspeed=0;
  const auto& qx=solver.reviewQx();const auto& qy=solver.reviewQy();
  for(int i=0;i<m.n_cells();++i){
   const double a=m.tri_area[i],h=s.depth[i],x=m.tri_cx[i]-2,y=m.tri_cy[i]-2,z=m.tri_cz[i]+(bowl?.1:0);
   const auto r=exact(m.tri_cx[i],m.tri_cy[i],t);const double u=h>o.dry_depth?qx[i]/h:0,v=h>o.dry_depth?qy[i]/h:0;
   volume+=a*h;cx+=a*h*x;cy+=a*h*y;r2+=a*h*(x*x+y*y);px+=a*qx[i];py+=a*qy[i];
   energy+=a*(.5*h*(u*u+v*v)+g*(.5*h*h+h*z));exact_energy+=a*(.5*r.h*(r.u*r.u+r.v*r.v)+g*(.5*r.h*r.h+r.h*z));
   l1+=a*std::abs(h-r.h);ref+=a*r.h;velerr+=a*r.h*((u-r.u)*(u-r.u)+(v-r.v)*(v-r.v));
   if((h>1e-6)!=(r.h>1e-6))shore+=a;
   maxspeed=std::max(maxspeed,std::hypot(u,v));
   static double record=5;
   if (std::hypot(u,v)>record) {
     record=std::hypot(u,v);
     std::fprintf(stderr,"PEAK t=%.17g cell=%d x=%.17g y=%.17g h=%.17g u=%.17g v=%.17g href=%.17g\n",t,i,x,y,h,u,v,r.h);
   }
  }
  if(history)std::fprintf(history,"%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n",t,volume,energy,exact_energy,cx/volume,cy/volume,r2/volume,px,py,l1/ref,shore,std::sqrt(velerr/ref),maxspeed);
 };
 if(history)std::fprintf(history,"t,volume,energy,exact_energy,cx,cy,r2,px,py,relative_l1,shore_area,velocity_l2,max_speed\n");
 diagnostic(0);
 auto start=std::chrono::steady_clock::now();
 // Animation sampling is opt-in, independent of the convergence matrix.
 FILE* animation=nullptr;
 if(const char* path=std::getenv("ANALYTICAL_ANIMATION")){animation=std::fopen(path,"wb");if(!animation)return 4;}
 auto frame=[&](double t){if(!animation)return;std::fwrite(&t,sizeof(double),1,animation);std::fwrite(s.depth.data(),sizeof(double),s.depth.size(),animation);};
 frame(0);
 const double output_dt=animation?end/80:(bowl?2*pi/omega/64:.25);
 int frame_index=1;
 for(double t=0;t<end;){double next=std::min(end,frame_index*output_dt);solver.advance(t,next);t=next;frame(t);diagnostic(t);++frame_index;}
 if(animation)std::fclose(animation);
 if(history)std::fclose(history);
 double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
 double l1=0,l2=0,linf=0,ref=0,maxdrift=0;
 FILE* f=nullptr;if(const char* path=std::getenv("ANALYTICAL_FIELDS")){f=std::fopen(path,"w");std::fprintf(f,"x,y,area,bed,depth,exact\n");}
 for(int i=0;i<m.n_cells();++i){auto r=exact(m.tri_cx[i],m.tri_cy[i],end);double error=std::abs(s.depth[i]-r.h),a=m.tri_area[i];l1+=a*error;l2+=a*error*error;linf=std::max(linf,error);ref+=a*r.h;maxdrift=std::max(maxdrift,std::abs(s.volume[i]-initial[i])/a);if(f)std::fprintf(f,"%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\n",m.tri_cx[i],m.tri_cy[i],a,m.tri_cz[i],s.depth[i],r.h);}
 if(f)std::fclose(f);
 double volume=std::accumulate(s.volume.begin(),s.volume.end(),0.);
 std::printf("{\"case\":\"%s\",\"nx\":%d,\"ny\":%d,\"quad\":%d,\"tiers\":%d,\"order\":%d,\"threads\":%d,\"duration\":%.17g,\"end\":%.17g,\"cells\":%d,\"l1_m\":%.17g,\"relative_l1\":%.17g,\"l2_m\":%.17g,\"linf_m\":%.17g,\"volume_relative_error\":%.17g,\"min_depth\":%.17g,\"max_depth_change\":%.17g,\"seconds\":%.9g,\"steps\":%ld}\n",kind.c_str(),n,ny,quad,o.lts_tiers,order,threads,duration,end,m.n_cells(),l1/(L*Ly),l1/ref,std::sqrt(l2/(L*Ly)),linf,(volume-v0)/v0,*std::min_element(s.depth.begin(),s.depth.end()),maxdrift,elapsed,solver.run_stats().nsteps);
}
