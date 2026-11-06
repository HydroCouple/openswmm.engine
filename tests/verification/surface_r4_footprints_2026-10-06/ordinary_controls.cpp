// Exact-parent versus candidate controls. Compile each with its own private headers.
#include "core/SimulationContext.hpp"
#include "hydrology/Runoff.hpp"
#include "hydrology/LID.hpp"
#include <iostream>
#include <iomanip>
using namespace openswmm;
SimulationContext model(){SimulationContext c;c.subcatches.resize(1);c.forcing.resize(0,0,1,0,0);c.subcatch_names.try_add("S");c.subcatches.area[0]=1;c.subcatches.width[0]=100;c.subcatches.slope[0]=.02;c.subcatches.n_perv[0]=.1;c.subcatches.n_imperv[0]=.01;c.subcatches.frac_imperv[0]=.4;c.options.ignore_snow_melt=true;return c;}
int main(){std::cout<<std::hexfloat;
 for(int method=0;method<6;++method){auto c=model();c.subcatches.infil_model[0]=method;c.subcatches.infil_p1[0]=method==4?75:3.5;c.subcatches.infil_p2[0]=.5;c.subcatches.infil_p3[0]=method<=1?4:method==4?7:.26;c.subcatches.infil_p4[0]=7;
  c.forcing.subcatch_rainfall_mode[0]=ForcingMode::OVERRIDE;runoff::RunoffSolver r;r.init(c);
  double start=0;for(int k=0;k<40;++k){double dt=k%3==0?.25:k%3==1?1:3; c.forcing.subcatch_rainfall_value[0]=k<15||k>30?43.2:0;c.climate_state.evap_rate=k<15?1e-5:.001;
   r.execute(c,dt,0,k<20?1:.3,1,k<20?9:10);int m;double state[6];r.infil_get_state(0,m,state);const auto&s=r.soa();
   std::cout<<"runoff "<<method<<' '<<k<<' '<<s.depth_perv[0]<<' '<<s.depth_imperv0[0]<<' '<<s.depth_imperv1[0]<<' '<<s.infil_vol[0]<<' '<<s.outflow_vol[0]<<' '<<c.subcatches.evap_loss[0];for(double x:state)std::cout<<' '<<x;std::cout<<'\n';start+=dt;
  }
 }
 for(const std::string type:{"BC","RG","GR","IT","PP","RB","VS","RD"}){auto c=model();c.lid_controls.names={"L"};c.lid_controls.lid_type={type};c.lid_controls.surface={{12,.1,.1,1,1}};c.lid_controls.soil={{12,.45,.3,.1,43.2,4,3.5}};c.lid_controls.pavement={{6,.2,0,43.2,0,0}};c.lid_controls.storage={{12,.4,4.32,0}};c.lid_controls.drain={{4.32,.5,0,0,0,0}};c.lid_controls.drainmat={{3,.5,.1}};c.lid_usage.subcatch_index={0};c.lid_usage.lid_index={0};c.lid_usage.number={2};c.lid_usage.area={100};c.lid_usage.width={10};c.lid_usage.init_sat={50};c.lid_usage.from_imperv={0};c.lid_usage.from_perv={0};c.lid_usage.to_perv={0};c.lid_usage.drain_to={""};lid::LIDSolver l;l.init(c);
  double start=0;for(int k=0;k<40;++k){double dt=k%3==0?.25:k%3==1?1:3;for(int t=0;t<l.numGroups();++t){auto& g=l.group(t);for(int u=0;u<g.count;++u){g.inflow[u]=k<15||k>30?.001:0;g.subcatch_rain[u]=g.inflow[u];}}
   l.execute(c,dt,0,k<15?1e-5:.001);const auto&g=l.group(l.usageOrder()[0].first);std::cout<<"lid "<<type<<' '<<k<<' '<<g.surf_depth[0]<<' '<<g.soil_moist[0]<<' '<<g.stor_depth[0]<<' '<<g.pave_depth[0]<<' '<<g.drain_flow[0]<<' '<<g.surface_runoff[0]<<' '<<g.wb_inflow[0]<<' '<<g.wb_evap[0]<<' '<<g.wb_infil[0]<<' '<<l.storedVolume()<<'\n';start+=dt;
  }
 }
}
