from pathlib import Path
import subprocess,json,os,random,resource
p=Path(__file__).resolve().parent;rows=[];rng=random.Random(97483)
jobs=[('same_mesh','radial',64,64,2,1,3,5),('same_mesh','planar',64,64,2,1,3,5),('same_mesh','planar',64,64,2,0,3,5),('same_mesh','planar',64,64,1,1,3,5),('same_mesh','ritter',256,256,2,0,2,5)]+[('accuracy_cost',c,128,96,2,q,3,3)for c in ['radial','planar']for q in [0,1]]
for tag,case,nb,ns,order,quad,duration,repeats in jobs:
 for repeat in range(-1,repeats):
  vs=['baseline','selected'];rng.shuffle(vs)
  for v in vs:
   n=nb if v=='baseline'else ns;start=resource.getrusage(resource.RUSAGE_CHILDREN);r=subprocess.run([str(p/f'build_{v}/performance'),case,str(n),'1',str(order),'1',str(quad),str(duration)],capture_output=True,text=True,check=True,timeout=300);end=resource.getrusage(resource.RUSAGE_CHILDREN);row=json.loads(r.stdout);row.update(cpu_seconds=end.ru_utime-start.ru_utime+end.ru_stime-start.ru_stime,variant=v,repeat=repeat,comparison=tag,load_average=os.getloadavg())
   if repeat>=0:rows.append(row);(p/'performance_results.json').write_text(json.dumps(rows,indent=2))
  print(tag,case,quad,order,repeat,flush=True)
print('PASS',len(rows),'timed runs',flush=True)
