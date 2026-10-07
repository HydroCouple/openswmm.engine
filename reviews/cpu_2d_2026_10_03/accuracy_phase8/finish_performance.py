from pathlib import Path
import json,subprocess,random,resource,os
p=Path(__file__).resolve().parent;c=json.loads((p/'performance_commands.json').read_text())[-1];c=[x.replace(str(p/'build_selected/ExplicitInertialSolver.o'),str(p/'build_fastshore/ExplicitInertialSolver.o')).replace(str(p/'build_selected/performance'),str(p/'build_fastshore/performance'))for x in c];subprocess.run(c,check=True);(p/'fastshore_performance_command.json').write_text(json.dumps(c,indent=2))
rows=[];rng=random.Random(83479)
for case,quad in [('radial',1),('planar',1),('planar',0)]:
 for repeat in range(-1,12):
  vs=['baseline','fastshore'];rng.shuffle(vs)
  for v in vs:
   start=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,'64','1','2','1',str(quad),'3'],capture_output=True,text=True,check=True,timeout=180);end=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout);row.update(cpu_seconds=end.ru_utime-start.ru_utime+end.ru_stime-start.ru_stime,variant=v,repeat=repeat,load_average=os.getloadavg())
   if repeat>=0:rows.append(row);(p/'final_performance.json').write_text(json.dumps(rows,indent=2))
  print(case,quad,repeat,flush=True)
print('PASS',len(rows),'timed runs',flush=True)
