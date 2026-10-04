from pathlib import Path
import shutil,subprocess,json,hashlib
p=Path(__file__).resolve().parent;root=p.parents[2]
assert not(p/'src_baseline').exists()
for d in ['src','include']:shutil.copytree(root/d,p/'src_baseline'/d)
(p/'.gitignore').write_text('/src_*/\n/build_*/\n/engine_common/\n/engine_runs/\n/fields/\n/*.tmp\n')
(p/'baseline_test.cpp').write_bytes((root/'tests/unit/engine/test_2d_cpu_correctness.cpp').read_bytes())
(p/'baseline_commit.txt').write_bytes(subprocess.check_output(['git','rev-parse','HEAD']))
(p/'baseline.diff').write_bytes(subprocess.check_output(['git','diff','HEAD','--','src','include','tests/unit/engine/test_2d_cpu_correctness.cpp']))
(p/'baseline_manifest.json').write_text(json.dumps({str(f.relative_to(p/'src_baseline')):hashlib.sha256(f.read_bytes()).hexdigest()for f in (p/'src_baseline').rglob('*')if f.is_file()},indent=2))
# Diagnostics access only in isolated review snapshots; never alter production API.
h=p/'src_baseline/src/engine/2d/solver/ExplicitInertialSolver.hpp';s=h.read_text();s=s.replace('private:', 'public:\n    const std::vector<double>& reviewQx()const{return qcx_;}\n    const std::vector<double>& reviewQy()const{return qcy_;}\nprivate:',1);h.write_text(s)
s=(p.parent/'fixes/analytical.cpp').read_text();s=s.replace('#include <string>','#include <string>\n#include <sys/resource.h>')
s=s.replace(' SurfaceStateData s;', ''' if(const char* v=std::getenv("REVIEW_CFL"))o.cfl_number=std::atof(v);
 if(const char* v=std::getenv("REVIEW_DRY"))o.dry_depth=std::atof(v);
 if(const char* v=std::getenv("REVIEW_HMOVE"))o.h_move=std::atof(v);
 if(const char* v=std::getenv("REVIEW_DT"))o.max_timestep=std::atof(v);
 if(const char* v=std::getenv("REVIEW_BETA"))o.exchange_beta=std::atof(v);
 SurfaceStateData s;''')
s=s.replace(' auto start=std::chrono', ''' FILE* history=nullptr;if(const char* path=std::getenv("REVIEW_HISTORY")){history=std::fopen(path,"w");if(!history)return 5;}
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
  }
  if(history)std::fprintf(history,"%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g,%.17g\\n",t,volume,energy,exact_energy,cx/volume,cy/volume,r2/volume,px,py,l1/ref,shore,std::sqrt(velerr/ref),maxspeed);
 };
 if(history)std::fprintf(history,"t,volume,energy,exact_energy,cx,cy,r2,px,py,relative_l1,shore_area,velocity_l2,max_speed\\n");
 diagnostic(0);
 auto start=std::chrono''')
s=s.replace('(bowl?2*pi/omega/16:.25)','(bowl?2*pi/omega/64:.25)').replace('t=next;frame(t);++frame_index;','t=next;frame(t);diagnostic(t);++frame_index;').replace(' if(animation)std::fclose(animation);',' if(animation)std::fclose(animation);\n if(history)std::fclose(history);')
(p/'harness.cpp').write_text(s)
shutil.copyfile(p.parent/'cpu_phase5/build.py',p/'build.py')
print('Fresh phase-7 snapshot and energy/momentum/shoreline harness prepared')
