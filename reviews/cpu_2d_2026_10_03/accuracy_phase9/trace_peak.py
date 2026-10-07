from pathlib import Path
import json,subprocess,os
p=Path(__file__).resolve().parent;s=(p/'harness.cpp').read_text();s=s.replace('maxspeed=std::max(maxspeed,std::hypot(u,v));','''maxspeed=std::max(maxspeed,std::hypot(u,v));
   static double record=5;
   if (std::hypot(u,v)>record) {
     record=std::hypot(u,v);
     std::fprintf(stderr,"PEAK t=%.17g cell=%d x=%.17g y=%.17g h=%.17g u=%.17g v=%.17g href=%.17g\\n",t,i,x,y,h,u,v,r.h);
   }''');f=p/'peak.cpp';f.write_text(s)
cmds=json.loads((p/'build_baseline/commands.json').read_text());out=p/'build_connected_joint';saved=[]
for c in cmds[-2:]:
 c=[str(f)if x==str(p/'harness.cpp')else str(out/'peak.o')if x==str(p/'build_baseline/harness.o')else str(out/'ExplicitInertialSolver.o')if x==str(p/'build_baseline/ExplicitInertialSolver.o')else str(out/'peak')if x==str(p/'build_baseline/review')else x.replace(str(p/'src_baseline'),str(p/'src_connected_joint'))for x in c];subprocess.run(c,check=True);saved.append(c)
r=subprocess.run([str(out/'peak'),'planar','32','1','2','1','2','3'],env={**os.environ,'REVIEW_SKEW':'.15'},text=True,capture_output=True,check=True);(p/'peak.log').write_text(r.stdout+r.stderr);(p/'peak_commands.json').write_text(json.dumps(saved,indent=2));print(r.stderr)
